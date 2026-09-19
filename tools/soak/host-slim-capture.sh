#!/usr/bin/env bash
# host-slim-capture.sh — record the applied state of one board as a reviewable transcript.
#
# WHY THIS EXISTS
#
# `host-slim.sh --check` is a live read: it answers "is the intended state in force right
# now?" and then the answer is gone. That is fine for a decision made in the moment and
# useless as evidence a week later, and it cannot distinguish "the configuration is applied"
# from "the configuration is applied *and* it came back after a reboot". The check output
# alone also does not say which revision of the tool produced the state, or what the
# persisted files actually contain.
#
# So this script copies the local `host-slim.sh` to the board (so the transcript proves
# which revision ran), then writes one file per board containing the check, the manifest,
# the persisted artifacts verbatim, and the unit-transience facts that determine whether
# each piece of confinement survives a reboot.
#
# It changes nothing on the board except an ephemeral /tmp copy of the tool, and it is safe
# to run while the board is busy.
#
# Usage:
#   host-slim-capture.sh piv            -> bench-results/host-slim-state-piv.txt
#   host-slim-capture.sh tib OUT.txt
#   host-slim-capture.sh --local        -> capture this machine (only useful on a board)
#
# Exit: 0 on success, 2 usage, 3 if a board could not be reached.

set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$here/../.." && pwd)
TOOL="$here/host-slim.sh"
[[ -r $TOOL ]] || { echo "cannot read $TOOL" >&2; exit 2; }

target=${1:-}
[[ -n $target ]] || { sed -n '2,25p' "$0"; exit 2; }

if [[ $target == --local ]]; then
  out=${2:-$repo/bench-results/host-slim-state-$(hostname).txt}
  REMOTE=0
  BOARD=$(hostname)
else
  out=${2:-$repo/bench-results/host-slim-state-$target.txt}
  REMOTE=1
  BOARD=$target
fi

mkdir -p -- "$(dirname -- "$out")"

# The remote payload. Kept as a single quoted heredoc so nothing expands locally; the board
# runs it under bash and we prepend its stdout to the transcript.
collect() {
  cat <<'PAYLOAD'
echo "## identity"
printf 'hostname        %s\n' "$(hostname)"
printf 'kernel          %s\n' "$(uname -r)"
printf 'machine         %s\n' "$(uname -m)"
printf 'uptime          %s (as of %s)\n' "$(uptime -p 2>/dev/null)" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf 'boot id         %s\n' "$(cat /proc/sys/kernel/random/boot_id)"
printf 'cmdline         %s\n' "$(cat /proc/cmdline)"

echo
echo "## tool under test"
printf 'deployed as     %s\n' "$TOOLPATH"
md5sum "$TOOLPATH" 2>/dev/null | sed 's/^/md5             /'

echo
echo "## manifest (/var/lib/namchain/host-slim/manifest)"
sudo -n cat /var/lib/namchain/host-slim/manifest 2>/dev/null || echo "(no manifest)"

echo
echo "## host-slim.sh --check"
sudo -n "$TOOLPATH" --check

echo
echo "## cgroup state (what each unit can actually run on)"
for u in system.slice user.slice machine.slice init.scope; do
  if [[ -d /sys/fs/cgroup/$u ]]; then
    printf '%-14s cpus.effective=%-8s transient=%-4s fragment=%s\n' \
      "$u" \
      "$(cat /sys/fs/cgroup/$u/cpuset.cpus.effective 2>/dev/null)" \
      "$(systemctl show $u -p Transient --value 2>/dev/null)" \
      "$(systemctl show $u -p FragmentPath --value 2>/dev/null)"
  else
    printf '%-14s (no cgroup directory -- skipped by host-slim)\n' "$u"
  fi
done
printf 'root -.slice   cpus.effective=%s\n' "$(cat /sys/fs/cgroup/cpuset.cpus.effective 2>/dev/null)"

echo
echo "## persisted artifacts (verbatim)"
for f in \
  /etc/systemd/system/system.slice.d/10-namchain-cpuset.conf \
  /etc/systemd/system/user.slice.d/10-namchain-cpuset.conf \
  /etc/systemd/system/namchain-host-slim-initconfine.service \
  /etc/systemd/system/namchain-host-slim-vmstat.service \
  /etc/systemd/journald.conf.d/10-namchain-audio.conf \
  /etc/udev/rules.d/90-namchain-cpu-dma-latency.rules \
  /etc/security/limits.d/90-namchain-rttime.conf ; do
  echo "--- $f"
  if sudo -n test -r "$f"; then sudo -n sed 's/^/    /' "$f"
  else echo "    ABSENT"; fi
done

echo
echo "## boot-time units"
for u in namchain-host-slim-initconfine.service namchain-host-slim-vmstat.service; do
  if systemctl cat "$u" >/dev/null 2>&1; then
    printf '%-46s enabled=%-9s active=%-8s result=%s\n' "$u" \
      "$(systemctl is-enabled "$u" 2>/dev/null)" \
      "$(systemctl is-active "$u" 2>/dev/null)" \
      "$(systemctl show "$u" -p Result --value 2>/dev/null)"
  else
    printf '%-46s not loaded\n' "$u"
  fi
done

echo
echo "## runtime knobs"
printf 'vm.stat_interval      %s\n' "$(cat /proc/sys/vm/stat_interval)"
printf 'sched_rt_runtime_us   %s\n' "$(cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null)"
printf 'cpu_dma_latency       %s\n' "$(ls -l /dev/cpu_dma_latency 2>/dev/null || echo absent)"
printf 'journal Storage       %s\n' "$(grep -h '^Storage=' /etc/systemd/journald.conf /etc/systemd/journald.conf.d/*.conf 2>/dev/null | tail -1)"
printf 'journal on            %s\n' "$(findmnt -no FSTYPE --target /run/log/journal 2>/dev/null || echo unknown)"
# journald volatile keeps logs under /run, which is tmpfs, but /run/log/journal is usually
# not its own mountpoint -- asking findmnt about that path directly answers "not mounted",
# which reads as a failure when the storage is in fact exactly where we want it.
printf 'throttled             %s\n' "$(grep -m1 . /sys/devices/system/cpu/cpu0/thermal_throttle/*_throttle_count 2>/dev/null | tr '\n' ' ' || echo n/a)"

echo
echo "## audio interrupt (matched by name, not number -- the number drifts across reboots)"
grep -iE 'xhci|ehci|dwc2|usb' /proc/interrupts | sed 's/^/    /' || true
# Name -> number comes from /proc/interrupts, because /proc/irq/N/ does not carry the name.
awk '$NF ~ /xhci|ehci|dwc2/ {gsub(":", "", $1); print $1, $NF}' /proc/interrupts \
| while read -r n name; do
    printf '    irq %-5s %-22s affinity_list=%s\n' "$n" "$name" \
      "$(cat /proc/irq/$n/smp_affinity_list 2>/dev/null)"
    ps -eo comm,rtprio,psr 2>/dev/null | awk -v p="irq/$n-" '$1 ~ p {printf "          thread %s rtprio=%s cpu=%s\n", $1, $2, $3}'
  done

echo
echo "## systemd failures (a name here is not automatically ours: check the timestamp first)"
systemctl --failed --no-legend --no-pager 2>/dev/null | sed 's/^/    /' || true
PAYLOAD
}

emit_header() {
  {
    echo "# host-slim applied-state transcript"
    echo "#"
    echo "# Generated by tools/soak/host-slim-capture.sh -- do not hand-edit."
    echo "# board: $BOARD"
    echo "# captured: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# local tool: $(md5sum "$TOOL" | awk '{print $1}')  ($TOOL)"
    if (( REMOTE )); then
      echo "# transport: ssh $BOARD; the tool was copied to /tmp and its md5 re-read there,"
      echo "#            so the transcript names the revision that produced the state."
    else
      echo "# transport: local"
    fi
    echo
  } > "$out"
}

if (( REMOTE )); then
  TOOLPATH=/tmp/namchain-host-slim.sh
  ssh -o BatchMode=yes -o ConnectTimeout=10 "$BOARD" \
      "cat > $TOOLPATH && chmod 0755 $TOOLPATH" < "$TOOL" || {
    echo "cannot copy the tool to $BOARD" >&2; exit 3; }
  emit_header
  { echo "## remote tool path"; echo "    $TOOLPATH"; echo; } >> "$out"
  { echo "TOOLPATH=$TOOLPATH"; collect; } | ssh -o BatchMode=yes -o ConnectTimeout=10 "$BOARD" bash -s >> "$out" || {
    echo "capture on $BOARD failed" >&2; exit 3; }
else
  TOOLPATH=$TOOL
  emit_header
  { echo "TOOLPATH=$TOOLPATH"; collect; } | bash -s >> "$out" || {
    echo "local capture failed" >&2; exit 3; }
fi

echo "wrote $out ($(wc -l < "$out") lines)"
