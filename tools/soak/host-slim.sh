#!/usr/bin/env bash
# host-slim.sh — the third layer: what to change in the *distribution* between the kernel
# (rtpi) and the engine (namchain). Nothing here is destructive: no package purge, no kernel
# rebuild, no sched_rt_runtime_us, no boot asset.
#
# Usage:
#   host-slim.sh --apply [--dry-run]   apply and verify; records what changed
#   host-slim.sh --check               report current state against the intended state
#   host-slim.sh --revert              undo exactly what --apply did (from the manifest)
#   host-slim.sh --diff                show intended vs actual, per item
#
# What it does, and why (full reasoning in docs/distro-slimming.md; measured evidence in
# bench-results/host-confine-screen.json and bench-results/tib-e5c-*):
#
#   1. cpuset confinement of the top-level slices, so housekeeping work cannot run on the
#      audio cores. `isolcpus=domain` removes cores from load balancing but NOT tasks that
#      are already affine to them (133 tasks on piv had cores 2-3 in their mask), and on
#      tib there is no isolcpus at all. The kernel calls cgroup v2 cpuset partitions the
#      recommended, runtime-tunable interface. Measured: on piv this collapses arch_timer
#      on the audio cores from ~475k/120s to 23/0 -- i.e. it is what makes nohz_full
#      actually hold -- and on tib with the real SSL it took driver xruns 746 -> 0.
#
#   2. Mask background services that wake up, touch memory or write disk and are irrelevant
#      to a headless audio node. Mask (not disable): disable still allows socket and
#      dependency activation. unattended-upgrades is also the hazard rtpi's README documents
#      (a dracut upgrade re-running flash-kernel and eating the rollback slot mid-test).
#
#   3. Keep logs off the disk (journald Storage=volatile) and cap them. Costs log
#      persistence across reboot; that is the trade and it is stated, not hidden.
#
#   4. vm.stat_interval, the housekeeping vmstat timer. Runtime-writable as root but not
#      persistent, so it goes in a oneshot unit.
#
#   5. /dev/cpu_dma_latency for the audio group. Ubuntu Studio ships exactly this udev rule;
#      neither board has it, so no application can use the PM-QoS interface today.
#
#   6. RT scheduling guards: RLIMIT_RTTIME as a runaway backstop. Deliberately NOT
#      sched_rt_runtime_us=-1 (AudioLinux sets it; it removes the only automatic escape from
#      a runaway SCHED_FIFO loop on a 4-core SBC).
#
# Board matters in two places, both explicit below: which services exist, and which CPUs the
# housekeeping side gets. Read the header of each block before changing it.

set -uo pipefail

STATE_DIR="${NAMSLIM_STATE:-/var/lib/namchain/host-slim}"
MANIFEST="$STATE_DIR/manifest"
JOURNALD_DROPIN="/etc/systemd/journald.conf.d/10-namchain-audio.conf"
VMSTAT_UNIT="/etc/systemd/system/namchain-host-slim-vmstat.service"
CDL_RULE="/etc/udev/rules.d/90-namchain-cpu-dma-latency.rules"
LIMITS_FILE="/etc/security/limits.d/90-namchain-rttime.conf"

# ---------------------------------------------------------------------------- board
board() {
  if [[ -r /proc/device-tree/compatible ]]; then
    if grep -qa 'rockchip,rk3288' /proc/device-tree/compatible; then echo tib; return; fi
    if grep -qa 'brcm,bcm2712' /proc/device-tree/compatible; then echo piv; return; fi
  fi
  case "$(hostname)" in
    tinkerboard|tib) echo tib ;;
    piv)             echo piv ;;
    *)               echo unknown ;;
  esac
}

# CPUs the housekeeping side is confined to. Only E5d-style measurement should change these.
#   piv 0-1 : cores 2-3 are isolated for the engine; the audio IRQ thread lives in the root
#             cgroup, which this does not touch, so its pin to cpu 2 survives.
#   tib 0-1 : no isolcpus exists on this board; 2-3 is reserved by convention for the engine.
housekeeping_cpus() { echo 0-1; }
audio_cpus()        { echo 2-3; }

# ---------------------------------------------------------------------------- helpers
say()  { printf '%s\n' "$*"; }
head2() { printf '\n== %s\n' "$*"; }
ok()   { printf '  ok    %s\n' "$*"; }
warn() { printf '  WARN  %s\n' "$*" >&2; }
skip() { printf '  skip  %s\n' "$*"; }
die()  { printf 'host-slim: %s\n' "$*" >&2; exit 2; }

need_root() {
  if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    if sudo -n true 2>/dev/null; then exec sudo -n "$0" "$@"; fi
    die "must run as root (or with passwordless sudo)"
  fi
}

run() {
  if [[ $DRY -eq 1 ]]; then printf '  DRY   %s\n' "$*"; return 0; fi
  "$@"
}

# record every irreversible-ish action so --revert is exact rather than a guess
manifest_add() {
  [[ $DRY -eq 1 ]] && return 0
  mkdir -p "$STATE_DIR"
  grep -qxF "$1" "$MANIFEST" 2>/dev/null || printf '%s\n' "$1" >> "$MANIFEST"
}

manifest_has() { [[ -r $MANIFEST ]] && grep -qxF "$1" "$MANIFEST"; }

masked_by_us() { [[ -r $MANIFEST ]] && grep -q "^mask " "$MANIFEST"; }

# ---------------------------------------------------------------------------- pieces
# 1. cpuset confinement of the top-level slices
do_confine() {
  head2 "cpuset confinement (housekeeping -> $(housekeeping_cpus))"
  local hk; hk=$(housekeeping_cpus) s
  for s in system.slice user.slice machine.slice; do
    if ! systemctl cat "$s" >/dev/null 2>&1; then skip "$s (not present)"; continue; fi
    run systemctl set-property --runtime "$s" AllowedCPUs="$hk"
    local eff; eff=$(cat "/sys/fs/cgroup/$s/cpuset.cpus.effective" 2>/dev/null)
    if [[ $DRY -eq 0 && $eff == "$hk" ]]; then ok "$s -> $eff"
    elif [[ $DRY -eq 0 ]]; then warn "$s read back '$eff', wanted '$hk'"; fi
  done
  # init.scope holds pid 1 and kernel threads; constraining it is what removes unbound work
  # from the audio cores entirely. Measured to be effective where the kernel permits it.
  if systemctl cat init.scope >/dev/null 2>&1; then
    run systemctl set-property --runtime init.scope AllowedCPUs="$hk"
    local e; e=$(cat /sys/fs/cgroup/init.scope/cpuset.cpus.effective 2>/dev/null)
    [[ $DRY -eq 0 ]] && ok "init.scope -> ${e:-<unchanged>}"
  fi
  manifest_add "confine runtime system.slice user.slice machine.slice init.scope"
  # Persistence. --runtime is deliberate for the first pass (survives until reboot, one
  # command to undo); writing the drop-ins makes it survive reboot. The audio IRQ thread is
  # in the root cgroup, which none of these touch, so its pin survives either way.
  local dir
  for s in system.slice user.slice; do
    dir="/etc/systemd/system/$s.d"
    run install -d -m 0755 "$dir"
    if [[ $DRY -eq 1 ]]; then printf '  DRY   write %s/10-namchain-cpuset.conf\n' "$dir"
    else printf '[Slice]\nAllowedCPUs=%s\n' "$hk" > "$dir/10-namchain-cpuset.conf"; fi
    manifest_add "file $dir/10-namchain-cpuset.conf"
    manifest_add "dir $dir"
  done
}

# 2. mask background services
service_list() {
  local b; b=$(board)
  # Common to both boards: anything that periodically wakes, scans, writes disk or indexes.
  cat <<'EOF'
apt-daily.service apt-daily.timer apt-daily-upgrade.service apt-daily-upgrade.timer
unattended-upgrades.service man-db.timer plocate-updatedb.timer mlocate.timer
fwupd.service fwupd-refresh.timer
EOF
  case $b in
    piv) cat <<'EOF'
cloud-init.service cloud-init-local.service cloud-final.service cloud-config.service
cloud-init-main.service cloud-init-network.service
snapd.service snapd.socket snapd.seeded.service snapd.apparmor.service
ModemManager.service bluetooth.service avahi-daemon.service avahi-daemon.socket cups.service cups.socket cups-browsed.service
NetworkManager-wait-online.service systemd-timesyncd.service
motd-news.service motd-news.timer ubuntu-advantage.service ua-reboot-cmds.service
apport.service whoopsie.service
rsyslog.service sysstat.service
udisks2.service multipathd.service open-iscsi.service
EOF
    ;;
    tib) cat <<'EOF'
armbian-hardware-monitor.service armbian-led-state.service
armbian-hardware-optimize.service
armbian-zram-config.service
systemd-networkd-wait-online.service
rsyslog.service cron.service
EOF
    ;;
  esac
}

# armbian-ramlog puts /var/log on a 50MB zram device. It is *not* masked by default because
# removing it relocates logs to the SD card (write traffic on tib's only block device).
# It is listed separately so the trade is visible instead of smuggled into a list.
ramlog_service() { board | grep -qx tib && echo armbian-ramlog.service; }

do_mask() {
  head2 "mask background services"
  local u existing=0 absent=0
  for u in $(service_list); do
    if ! systemctl cat "$u" >/dev/null 2>&1; then absent=$((absent+1)); continue; fi
    if [[ $(systemctl is-enabled "$u" 2>/dev/null) == masked ]]; then existing=$((existing+1)); continue; fi
    run systemctl mask "$u"
    manifest_add "mask $u"
    [[ $DRY -eq 0 ]] && ok "masked $u"
  done
  say "  ($existing already masked, $absent not present on this board)"
  local r; r=$(ramlog_service)
  if [[ -n $r && $(systemctl is-enabled "$r" 2>/dev/null) != masked ]]; then
    say "  NOT masking $r: it keeps /var/log in RAM. Masking it moves log writes to the"
    say "  SD card. Decide deliberately; see docs/distro-slimming.md section 4.3."
  fi
}

# 3. journald off the disk
do_journald() {
  head2 "journald: keep logs off the disk"
  run install -d -m 0755 "$(dirname "$JOURNALD_DROPIN")"
  if [[ $DRY -eq 1 ]]; then printf '  DRY   write %s\n' "$JOURNALD_DROPIN"
  else
    cat > "$JOURNALD_DROPIN" <<'EOF'
# namchain host-slim: logs in tmpfs so logging never touches the disk.
# Trade: logs do not survive reboot. That is the point on an appliance.
[Journal]
Storage=volatile
RuntimeMaxUse=64M
Audit=no
EOF
  fi
  manifest_add "file $JOURNALD_DROPIN"
  run systemctl restart systemd-journald
  local where; where=$(findmnt -no FSTYPE /run/log/journal 2>/dev/null || echo unknown)
  [[ $DRY -eq 0 ]] && ok "journal now on ${where:-?} (expect tmpfs)"
}

# 4. vmstat timer
do_vmstat() {
  head2 "vm.stat_interval (housekeeping vmstat timer)"
  local cur; cur=$(cat /proc/sys/vm/stat_interval 2>/dev/null)
  if [[ $DRY -eq 1 ]]; then printf '  DRY   write %s (now %s)\n' "$VMSTAT_UNIT" "$cur"; return; fi
  cat > "$VMSTAT_UNIT" <<'EOF'
[Unit]
Description=namchain host-slim: quiet the per-CPU vmstat timer
After=multi-user.target

[Service]
Type=oneshot
RemainAfterExit=yes
# Runtime-writable as root, but not persistent: this is what makes it survive reboot.
ExecStart=/bin/sh -c 'echo 10 > /proc/sys/vm/stat_interval'

[Install]
WantedBy=multi-user.target
EOF
  manifest_add "file $VMSTAT_UNIT"
  run systemctl daemon-reload
  run systemctl enable --now namchain-host-slim-vmstat.service
  ok "vm.stat_interval -> $(cat /proc/sys/vm/stat_interval) (was $cur)"
}

# 5. cpu_dma_latency for the audio group
do_cdl() {
  head2 "/dev/cpu_dma_latency for group audio"
  local grp; grp=$(getent group audio >/dev/null && echo audio || echo root)
  if [[ $DRY -eq 1 ]]; then printf '  DRY   write %s (group %s)\n' "$CDL_RULE" "$grp"; return; fi
  cat > "$CDL_RULE" <<EOF
# namchain host-slim: the PM-QoS interface that lets an application refuse deep C-states.
# Shipped as 0600 root:root on both boards; Ubuntu Studio grants it to group audio.
KERNEL=="cpu_dma_latency", GROUP="$grp", MODE="0660"
EOF
  manifest_add "file $CDL_RULE"
  run udevadm control --reload-rules
  run udevadm trigger --name-match=cpu_dma_latency 2>/dev/null || true
  ok "rule installed for group $grp: $(ls -l /dev/cpu_dma_latency 2>/dev/null)"
}

# 6. RT runaway backstop
do_limits() {
  head2 "RT runaway backstop (RLIMIT_RTTIME)"
  if [[ $DRY -eq 1 ]]; then printf '  DRY   write %s\n' "$LIMITS_FILE"; return; fi
  cat > "$LIMITS_FILE" <<'EOF'
# namchain host-slim: cap the CPU time a realtime task may consume without blocking.
# SIGXCPU on breach. This is a backstop against a runaway SCHED_FIFO loop, NOT a latency
# setting; 200 ms is ~150 periods at 1.333 ms and is far above any legitimate block.
@audio   -  rttime    200000
EOF
  manifest_add "file $LIMITS_FILE"
  ok "rttime 200000us for @audio"
  say "  (deliberately NOT sched_rt_runtime_us=-1: it removes the only automatic escape"
  say "   from a runaway realtime loop on a 4-core board. See section 5.6 of the doc.)"
}

# ---------------------------------------------------------------------------- check
check_one() {
  local label=$1 actual=$2 want=$3
  if [[ $actual == "$want" ]]; then printf '  ok    %-46s %s\n' "$label" "$actual"
  else printf '  FAIL  %-46s %s (want %s)\n' "$label" "${actual:-<empty>}" "$want"; fi
}

do_check() {
  local b hk; b=$(board); hk=$(housekeeping_cpus)
  head2 "board"
  say "  board=$b  housekeeping=$hk  audio=$(audio_cpus)"

  head2 "cpuset confinement"
  local s
  for s in system.slice user.slice; do
    check_one "$s cpuset.cpus.effective" "$(cat /sys/fs/cgroup/$s/cpuset.cpus.effective 2>/dev/null)" "$hk"
  done
  check_one "init.scope cpuset.cpus.effective" "$(cat /sys/fs/cgroup/init.scope/cpuset.cpus.effective 2>/dev/null)" "$hk"

  head2 "the intervention actually held (the manipulation check)"
  say "  /proc/cmdline isolcpus/nohz_full: $(tr ' ' '\n' < /proc/cmdline | grep -E 'isolcpus|nohz_full|rcu_nocbs' | tr '\n' ' ')"
  say "  arch_timer per cpu, 10s delta (audio cores should be ~0):"
  local before after i ncpu
  before=$(awk 'NR==1{print NF; exit}' /proc/interrupts)
  local -a t0 t1
  mapfile -t t0 < <(awk -v n="$before" '$NF=="arch_timer"{for(c=2;c<=n+1;c++) print $c}' /proc/interrupts)
  sleep 10
  mapfile -t t1 < <(awk -v n="$before" '$NF=="arch_timer"{for(c=2;c<=n+1;c++) print $c}' /proc/interrupts)
  for ((i=0; i<before; i++)); do
    printf '    cpu%d: %s\n' "$i" "$(( ${t1[$i]:-0} - ${t0[$i]:-0} ))"
  done

  head2 "masked services"
  local u n=0 bad=0 st
  for u in $(service_list); do
    systemctl cat "$u" >/dev/null 2>&1 || continue
    st=$(systemctl is-enabled "$u" 2>/dev/null)
    # `static` and `indirect` units are not driven by the enablement system at all, so
    # masking is a no-op for them and they are not a defect. `enabled` is.
    case $st in
      masked|static|indirect|alias|not-found|disabled) n=$((n+1)) ;;
      *) printf '  FAIL  %-46s %s\n' "$u" "$st"; bad=$((bad+1)) ;;
    esac
  done
  [[ $n -gt 0 ]] && ok "$n services masked/not-enabled"
  [[ $bad -eq 0 ]] && ok "no enabled background service left unmasked"

  head2 "files"
  for f in "$JOURNALD_DROPIN" "$VMSTAT_UNIT" "$CDL_RULE" "$LIMITS_FILE"; do
    [[ -r $f ]] && ok "$f" || printf '  FAIL  %-46s absent\n' "$f"
  done
  check_one "vm.stat_interval" "$(cat /proc/sys/vm/stat_interval 2>/dev/null)" "10"
  say "  /dev/cpu_dma_latency: $(ls -l /dev/cpu_dma_latency 2>/dev/null || echo absent)"
  say "  journal Storage: $(grep -h '^Storage=' /etc/systemd/journald.conf "$JOURNALD_DROPIN" 2>/dev/null | tail -1)"
}

do_diff() { do_check; say ""; say "(--diff is --check plus the intended values; see docs/distro-slimming.md)"; }

# ---------------------------------------------------------------------------- revert
do_revert() {
  local b hk; b=$(board); hk=$(housekeeping_cpus)
  head2 "revert"
  if [[ ! -r $MANIFEST ]]; then
    warn "no manifest at $MANIFEST; reverting only the fixed file set"
  fi
  local u
  while read -r kind what _; do
    case $kind in
      mask) run systemctl unmask "$what"; ok "unmasked $what" ;;
      file) run rm -f "$what";          ok "removed $what" ;;
      dir)  run rmdir "$what" 2>/dev/null || true ;;
      confine) : ;;  # handled below, wholesale
    esac
  done < <(cat "$MANIFEST" 2>/dev/null)
  # reload the transient cgroup properties
  local s
  for s in system.slice user.slice machine.slice init.scope; do
    systemctl cat "$s" >/dev/null 2>&1 && run systemctl revert --runtime "$s" 2>/dev/null
  done
  run systemctl daemon-reload
  run udevadm control --reload-rules
  run systemctl restart systemd-journald
  [[ $DRY -eq 0 ]] && ok "reverted; system.slice effective cpus now $(cat /sys/fs/cgroup/system.slice/cpuset.cpus.effective 2>/dev/null)"
  [[ $DRY -eq 0 ]] && say "  vm.stat_interval is $(cat /proc/sys/vm/stat_interval) (unit removed; reboot or write it back for 1)"
}

# ---------------------------------------------------------------------------- main
DRY=0
MODE=""
PASSTHRU=()
while [[ $# -gt 0 ]]; do
  case $1 in
    --apply)  MODE=apply;  PASSTHRU+=(--apply) ;;
    --check)  MODE=check;  PASSTHRU+=(--check) ;;
    --diff)   MODE=diff;   PASSTHRU+=(--diff) ;;
    --revert) MODE=revert; PASSTHRU+=(--revert) ;;
    --dry-run) DRY=1;      PASSTHRU+=(--dry-run) ;;
    -h|--help) sed -n '2,45p' "$0"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done
[[ -n $MODE ]] || die "one of --apply --check --diff --revert is required"

case $MODE in
  apply)
    need_root "${PASSTHRU[@]}"
    say "host-slim --apply on board $(board)$([[ $DRY -eq 1 ]] && echo ' (DRY RUN -- nothing will change)')"
    say "changes are recorded in $MANIFEST for --revert"
    [[ $DRY -eq 0 ]] && mkdir -p "$STATE_DIR" && : > "$MANIFEST"
    do_confine; do_mask; do_journald; do_vmstat; do_cdl; do_limits
    head2 "verify"
    do_check
    say ""
    if [[ $DRY -eq 1 ]]; then
      say "Dry run complete. Re-run without --dry-run to apply."
    else
      say "Applied. Nothing here touched a boot asset, a package or a kernel setting."
      say "To undo: $0 --revert    To re-check: $0 --check"
    fi
    ;;
  check)  do_check ;;
  diff)   do_diff ;;
  revert)
    need_root "${PASSTHRU[@]}"
    do_revert
    ;;
esac
