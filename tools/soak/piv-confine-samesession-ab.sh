#!/usr/bin/env bash
# piv-confine-samesession-ab.sh — does cgroup v2 cpuset confinement move the engine?
#
# WHY THIS REPLACES THE EARLIER COMPARISONS
#
# Two attempts at this question have been confounded, in two different ways, and both are worth
# stating because they are the same failure mode twice:
#
#   * `docs/distro-slimming.md` section 7 confined the top-level slices, which confines the
#     *measurement process* too, so its confined arms ran the engine on the loaded housekeeping
#     cores ("sched_setaffinity(cpu 3): Invalid argument"). Corrected in section 7.0.
#   * my own first comparison put a confined arm (no reboot) against the isolation A/B's arms,
#     each of which ran about a minute after a reboot. Every A/B arm's first overrun is in its
#     first 240 blocks -- block 0, 99, 20 and 239 -- i.e. the post-boot storm, which the
#     confined arm did not have at all. That comparison measured the boot, not the confinement.
#
# So this runs the two conditions **back to back in one session, with no reboots**, in a balanced
# order, with nothing else deliberate running. Confinement is a runtime property, so unlike the
# boot-argument A/B it needs no reboot -- which is exactly why it can be compared cleanly.
#
# Both conditions pin the engine to cpu 3. The confined arms additionally run it inside an
# `audio.slice` cpuset partition, because confining the top-level slices to 0-1 would otherwise
# stop it reaching cpu 3, and pass the rlimits through the unit because a systemd service does
# not get pam_limits and would silently run SCHED_OTHER.
#
# Usage: piv-confine-samesession-ab.sh [seconds-per-arm]      (on piv; 1800 by default)

set -u
cd "$(dirname "$0")/../.." || exit 1
REPO=$PWD
SOAK=$REPO/build/namchain-soak
MODEL=$REPO/build/generated/A2-Full.nam
SECS="${1:-1800}"
HK=0-1

card() { arecord -l 2>/dev/null | awk '/SSL 2\+ Mk II/ { gsub(":", "", $2); print $2; exit }'; }

args=()
mk() {
  args=(--device "hw:$(card),0" --capture-channels 8 --playback-channels 6
        --quantum 64 --periods 6 --output-queue-periods 3 --output-queue-floor-periods 0
        --splice-fade-ms 1.333 --splice-verify --instances 4 --model "$MODEL"
        --input-channel 0 --output-channel 0 --self-oracle --oracle-window 5
        --rt-priority 80 --cpu 3 --seconds "$SECS")
}

confine() {  # confine on|off
  local want=$1 s
  for s in system.slice user.slice machine.slice init.scope; do
    if [[ $want == on ]]; then
      sudo systemctl set-property --runtime "$s" AllowedCPUs=$HK >/dev/null 2>&1
    else
      sudo systemctl revert --runtime "$s" >/dev/null 2>&1
    fi
  done
  printf '  confinement=%-3s system.slice=%-4s user.slice=%-4s  loadavg=%s\n' \
    "$want" "$(cat /sys/fs/cgroup/system.slice/cpuset.cpus.effective 2>/dev/null)" \
    "$(cat /sys/fs/cgroup/user.slice/cpuset.cpus.effective 2>/dev/null)" \
    "$(cut -d' ' -f1 /proc/loadavg)"
}

arm() {  # arm NAME on|off
  local name=$1 mode=$2
  confine "$mode"
  mk
  if [[ $mode == on ]]; then
    # The engine has to be started *inside* the partition, or the slice confinement stops it
    # reaching cpu 3 -- the bug that confounded the first attempt.
    sudo systemd-run --unit="nab-$name" --slice=audio.slice \
      -p AllowedCPUs=2-3 -p LimitRTPRIO=95 -p LimitMEMLOCK=infinity \
      -p Type=oneshot --uid=rik --wait --collect \
      env NAMCHAIN_ARM_LABEL="piv confine AB $name: confined, quiet" \
      "$REPO/tools/soak/arm-run.sh" "$REPO/bench-results/$name" "$SOAK" "${args[@]}" >/dev/null 2>&1
  else
    NAMCHAIN_ARM_LABEL="piv confine AB $name: unconfined, quiet" \
      "$REPO/tools/soak/arm-run.sh" "$REPO/bench-results/$name" "$SOAK" "${args[@]}" >/dev/null 2>&1
  fi
  local rc=$?
  printf '  %s rc=%d  pin: ' "$name" "$rc"
  grep -E '^scheduling:' "$REPO/bench-results/$name.txt" 2>/dev/null | sed 's/scheduling: *//'
}

# Balanced B A A B, and no reboots anywhere.
arm piv-cx-1-unconf off
arm piv-cx-2-conf   on
arm piv-cx-3-conf   on
arm piv-cx-4-unconf off
confine off
echo "=== done ==="
python3 "$REPO/tools/soak/rt-arm-summary.py" \
  "$REPO"/bench-results/piv-cx-1-unconf "$REPO"/bench-results/piv-cx-2-conf \
  "$REPO"/bench-results/piv-cx-3-conf "$REPO"/bench-results/piv-cx-4-unconf 2>/dev/null
