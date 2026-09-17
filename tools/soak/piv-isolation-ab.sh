#!/usr/bin/env bash
# piv-isolation-ab.sh — compare the RT kernel WITH and WITHOUT the isolation boot arguments.
#
# WHAT THIS ANSWERS
#
# piv runs `7.0.0-1017-raspi-realtime` with
#   isolcpus=managed_irq,domain,2-3 nohz_full=2-3 rcu_nocbs=2-3 irqaffinity=0-1 skew_tick=1
# on its command line. Every measurement in the piv round was taken with those in force, so
# "the RT kernel removed the tail" cannot be separated from "the boot arguments removed it".
# This compares the two configurations on the **same kernel image** -- the only difference is
# the command line -- which is the one paired comparison that needs no stock kernel as a control.
#
# WHY IT HAS TO DRIVE ITSELF
#
# isolcpus/nohz_full/rcu_nocbs/skew_tick are boot-time parameters. There is no runtime
# equivalent, so each arm is a reboot, and a reboot ends whatever session is driving the
# experiment. Hence a systemd oneshot that runs one arm per boot and sets up the next.
#
# WHY IT IS WRITTEN TO BE BORING
#
# It edits the boot partition of the machine it runs on, so it is deliberately bounded and
# self-restoring:
#   * exactly four arms, balanced off / on / on / off, so a drift in the session cannot be
#     mistaken for the effect of the argument;
#   * the command line is restored to the isolation configuration on the last arm **and** on any
#     abort, so the machine always ends in the state it started in;
#   * a missing or `done` state file means "do nothing and do not reboot", so a stray boot of the
#     unit is harmless;
#   * the written command line is read back and verified before any reboot is issued.
#
# The change itself is low-risk: the "off" command line is byte-for-byte what
# /boot/firmware/old/cmdline.txt holds, which is the configuration this machine was validated on
# *before* the isolation arguments were promoted (old/state = good). Nothing else on the boot
# partition is touched.
#
# USAGE
#   sudo piv-isolation-ab.sh --install     arm the sequence and reboot into arm 0
#        piv-isolation-ab.sh --status      where the sequence is (no root needed)
#        piv-isolation-ab.sh --abort       stop after the arm in progress
#   sudo piv-isolation-ab.sh --uninstall   remove the unit and restore the isolation args
#        piv-isolation-ab.sh --collect     summarise the completed arms
#
# Arms write the usual .env/.heartbeat/.txt/.isolation evidence into <repo>/bench-results/, so a
# reboot cannot lose a completed arm, and the analysis is the same rt-arm-summary.py pass used
# everywhere else.

set -u

DIR=/var/lib/namchain-ab
STATE="$DIR/state"
ABORT="$DIR/abort"
UNIT=/etc/systemd/system/namchain-ab.service
SELF=/usr/local/sbin/piv-isolation-ab
CMDLINE=/boot/firmware/current/cmdline.txt
REPO="${NAMCHAIN_REPO:-/home/rik/code.local/namchain}"

# The boot arguments under test. Keep in step with AUDIO_TUNING_ARGS in rtpi's device.env.
ISO_KEYS='isolcpus|nohz_full|rcu_nocbs|irqaffinity|skew_tick'

ARMS=(off on on off)          # balanced: B A A B
ARM_SECONDS="${ARM_SECONDS:-1800}"

log() { echo "$(date -Is): $*"; }

# Find the interface's card rather than hard-coding it: card numbers are assigned by probe order
# and have moved on this machine across a kernel upgrade before.
card() {
  local c
  c=$(arecord -l 2>/dev/null | awk '/SSL 2\+ Mk II/ { gsub(":", "", $2); print $2; exit }')
  [[ -n $c ]] || c=2
  echo "$c"
}

# One arm, on the SSL through the device path. This is the piv shipping configuration as of
# 2026-09-16: q64, ring 6, cap 3, floor 0, 4 x A2-Full.
run_arm() {  # run_arm INDEX CONDITION
  local idx="$1" cond="$2" c
  c=$(card)
  local prefix="$REPO/bench-results/piv-ab-$idx-$cond"
  local label="piv isolation A/B arm $idx/3: isolation=$cond, 4xA2-Full q64 cap3, ${ARM_SECONDS}s"
  log "arm $idx: isolation=$cond  card=$c  -> $prefix"
  cd "$REPO" || return 1
  # Run as rik, not root: the evidence should be owned by the user, and rik has rtprio 95 from
  # /etc/security/limits.d/audio.conf. Root would also get SCHED_FIFO, via CAP_SYS_NICE, but the
  # point is to measure the configuration a user actually runs.
  sudo -u rik env NAMCHAIN_ARM_LABEL="$label" \
    "$REPO/tools/soak/arm-run.sh" "$prefix" \
    "$REPO/build/namchain-soak" \
      --device "hw:$c,0" --capture-channels 8 --playback-channels 6 \
      --quantum 64 --periods 6 \
      --output-queue-periods 3 --output-queue-floor-periods 0 \
      --splice-fade-ms 1.333 --splice-verify \
      --instances 4 --model "$REPO/build/generated/A2-Full.nam" \
      --input-channel 0 --output-channel 0 \
      --self-oracle --oracle-window 5 \
      --rt-priority 80 --cpu 3 --seconds "$ARM_SECONDS"
  local rc=$?
  log "arm $idx exit=$rc"
  return $rc
}

# Write the command line for the *next* boot: isolation args present or stripped.
set_cmdline() {  # set_cmdline on|off
  local want="$1" out got
  [[ -r $DIR/cmdline.isolation ]] || { log "no cmdline backup in $DIR"; return 1; }
  if [[ $want == on ]]; then
    out=$(cat "$DIR/cmdline.isolation")
  else
    out=$(tr ' ' '\n' < "$DIR/cmdline.isolation" | grep -vE "^($ISO_KEYS)=" | tr '\n' ' ')
    out="${out% }"
  fi
  printf '%s\n' "$out" > "$CMDLINE" || return 1
  sync
  # Read back and verify rather than assuming the write took: a silently wrong command line
  # would silently measure the wrong configuration, which is the failure this whole round has
  # been about avoiding.
  got=$(cat "$CMDLINE")
  [[ $got == "$out" ]] || { log "cmdline read-back mismatch -- refusing to proceed"; return 1; }
  if [[ $want == on ]]; then
    grep -qE "$ISO_KEYS=" <<<"$got" || { log "isolation args missing after write"; return 1; }
  else
    ! grep -qE "$ISO_KEYS=" <<<"$got" || { log "isolation args still present after write"; return 1; }
  fi
  log "next boot: isolation=$want ($(grep -cE "$ISO_KEYS=" <<<"$got") isolation args)"
}

# The boot-time entry point: run this boot's arm, then arrange the next.
driver() {
  mkdir -p "$DIR"
  if [[ ! -f $STATE ]]; then
    log "no state file -- not armed, doing nothing and not rebooting"
    exit 0
  fi
  local idx; idx=$(cat "$STATE")
  if [[ $idx == done ]]; then
    log "sequence complete; restoring the isolation command line"
    set_cmdline on || true
    exit 0
  fi

  local cond="${ARMS[$idx]}"
  log "running arm $idx (isolation=$cond)" | tee -a "$DIR/ab.log"
  run_arm "$idx" "$cond" 2>&1 | tee -a "$DIR/ab.log"

  local next=$(( idx + 1 ))
  if [[ -f $ABORT || $next -ge ${#ARMS[@]} ]]; then
    [[ -f $ABORT ]] && log "abort file present -- stopping after arm $idx"
    set_cmdline on
    echo done > "$STATE"
    # Leave the machine as it was found: not merely with the isolation command line *written*,
    # but actually running it. The last arm is an "off" arm, so without this the board would sit
    # unisolated until someone rebooted it -- a silent change to the state of a machine we do not
    # own. Disable the unit too, so future boots do nothing.
    systemctl disable namchain-ab.service 2>/dev/null || true
    log "sequence finished. Evidence: $REPO/bench-results/piv-ab-*"
    if [[ $cond != on ]]; then
      log "this boot is running isolation=$cond, so rebooting once more to apply the isolation"
      log "command line and leave the machine in the configuration it started in."
      sync
      systemctl reboot
    fi
    log "already running with isolation; nothing further to do."
    exit 0
  fi

  echo "$next" > "$STATE"
  if ! set_cmdline "${ARMS[$next]}"; then
    log "refusing to reboot: could not set the command line for arm $next"
    echo done > "$STATE"
    exit 1
  fi
  log "rebooting into arm $next (isolation=${ARMS[$next]})"
  sync
  systemctl reboot
}

install_unit() {
  [[ $(id -u) -eq 0 ]] || { echo "--install must run as root:  sudo $0 --install" >&2; exit 2; }
  mkdir -p "$DIR"
  if [[ ! -s $DIR/cmdline.isolation ]]; then
    cat "$CMDLINE" > "$DIR/cmdline.isolation"
  fi
  grep -qE "$ISO_KEYS=" "$DIR/cmdline.isolation" \
    || { echo "the saved command line has no isolation args -- refusing to arm" >&2; exit 2; }
  # Keep a copy of the plain variant next to it, so what will be written is inspectable before
  # anything reboots.
  tr ' ' '\n' < "$DIR/cmdline.isolation" | grep -vE "^($ISO_KEYS)=" | tr '\n' ' ' > "$DIR/cmdline.plain"
  echo >> "$DIR/cmdline.plain"
  [[ $(grep -cE "$ISO_KEYS=" "$DIR/cmdline.plain") -eq 0 ]] \
    || { echo "could not derive a plain command line -- refusing to arm" >&2; exit 2; }

  install -m 0755 "$0" "$SELF"
  cat > "$UNIT" <<UNIT
[Unit]
Description=namchain isolation A/B, one arm per boot
After=multi-user.target network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=$SELF --driver
RemainAfterExit=no
# The duration must be passed through the unit, not inherited: systemd starts a service with
# a clean environment, so `sudo env ARM_SECONDS=3600 ./piv-isolation-ab.sh --install` armed a
# sequence that silently ran the 1800 s default instead. That is how the first A/B ran half
# the length requested.
Environment=ARM_SECONDS=$ARM_SECONDS
# An arm plus the oracle afterwards; do not let systemd kill it mid-measurement.
TimeoutStartSec=infinity

[Install]
WantedBy=multi-user.target
UNIT
  systemctl daemon-reload
  systemctl enable namchain-ab.service
  echo 0 > "$STATE"
  rm -f "$ABORT"
  set_cmdline "${ARMS[0]}" || { echo "could not set the first command line" >&2; exit 1; }

  echo
  echo "armed: ${#ARMS[@]} arms of ${ARM_SECONDS}s, order ${ARMS[*]}"
  echo "  arm 0 isolation=off   $(cat "$DIR/cmdline.plain")"
  echo "  arm 1 isolation=on    $(cat "$DIR/cmdline.isolation")"
  echo "Stop after the arm in progress with:  $SELF --abort"
  echo "Rebooting into arm 0 now."
  sync
  systemctl reboot
}

case "${1:-}" in
  --driver)    driver ;;
  --install)   install_unit ;;
  --status)
    echo "state:        $(cat "$STATE" 2>/dev/null || echo '(not armed)')"
    [[ -f $ABORT ]] && echo "abort file:   present"
    echo "isolation now: $(grep -cE "$ISO_KEYS=" /proc/cmdline) args in /proc/cmdline"
    echo "armed duration: $(grep -ohE 'ARM_SECONDS=[0-9]+' "$UNIT" 2>/dev/null || echo '(unit not installed)')"
    echo "arms done:"
    ls -1 "$REPO"/bench-results/piv-ab-*.txt 2>/dev/null | sed 's/^/  /' || echo "  (none)"
    ;;
  --abort)     mkdir -p "$DIR" 2>/dev/null || sudo mkdir -p "$DIR"; touch "$ABORT" 2>/dev/null || sudo touch "$ABORT"; echo "abort set: stops after the arm in progress" ;;
  --uninstall)
    [[ $(id -u) -eq 0 ]] || { echo "--uninstall must run as root" >&2; exit 2; }
    systemctl disable --now namchain-ab.service 2>/dev/null
    rm -f "$UNIT"; systemctl daemon-reload
    set_cmdline on || true
    rm -f "$STATE" "$ABORT"
    echo "uninstalled; isolation command line restored."
    ;;
  --collect)
    cd "$REPO" || exit 1
    python3 tools/soak/rt-arm-summary.py bench-results/piv-ab-* 2>/dev/null || echo "(no arms yet)"
    ;;
  *) sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
