#!/usr/bin/env bash
# tib-confinement-ab.sh — re-run the deviceless confinement arm (E5) correctly.
#
# WHY IT NEEDS RE-RUNNING. docs/distro-slimming.md §7b compared a "confined" arm against an
# "unconfined" one, but `AllowedCPUs=` on the top-level slices confines the measurement process
# too, so the confined arm pushed the *soak* off cpu 3 and onto the loaded housekeeping cores --
# its own report says "any of 2 cpus (not as requested: sched_setaffinity(cpu 3): Invalid
# argument)". The two arms therefore differ in where the engine ran, not only in confinement.
# See §7.0.
#
# WHAT THIS DOES INSTEAD, and it is the same shape as the piv re-run:
#   * confine the top-level slices to 0-1, so all the *other* work is off the audio cores;
#   * run the engine in its own cpuset partition at 2-3, so it is the one thing allowed there and
#     `--cpu 3` succeeds;
#   * pass the rlimits through the unit, because a systemd service does not get pam_limits and
#     would otherwise silently run SCHED_OTHER;
#   * four arms in ONE session with NO reboots, balanced unconfined/confined/confined/unconfined.
#
# The load is the one E5 used -- `stress-ng --vm 1 --vm-bytes 128M --vm-keep` on cpus 0-1 -- and
# `--vm-keep` is deliberate: without it stress-ng frees and re-maps its 128 MB every cycle, which
# is a different and much heavier load (measured on piv: 2400 xruns against 562).
#
# Usage: tib-confinement-ab.sh [seconds-per-arm]      (on tib; 900 by default)

set -u
cd "$(dirname "$0")/../.." || exit 1
REPO=$PWD
SOAK=$REPO/bin-neon/namchain-soak
MODEL=$REPO/models/A2-Lite.nam
SECS="${1:-900}"
HK=0-1
AUDIO=2-3

args=()
mk() {
  args=(--backend null --quantum 64 --instances 3 --model "$MODEL" --input-offset 1
        --self-oracle --rt-priority 80 --cpu 3 --seconds "$SECS")
}

confine() {  # confine on|off
  local want=$1 s
  for s in system.slice user.slice machine.slice init.scope; do
    if [[ $want == on ]]; then
      sudo -n systemctl set-property --runtime "$s" AllowedCPUs=$HK >/dev/null 2>&1
    else
      sudo -n systemctl revert --runtime "$s" >/dev/null 2>&1
    fi
  done
  printf '  confinement=%-3s system.slice=%-4s user.slice=%-4s loadavg=%s\n' \
    "$want" "$(cat /sys/fs/cgroup/system.slice/cpuset.cpus.effective 2>/dev/null)" \
    "$(cat /sys/fs/cgroup/user.slice/cpuset.cpus.effective 2>/dev/null)" \
    "$(cut -d' ' -f1 /proc/loadavg)"
}

arm() {  # arm NAME on|off
  local name=$1 mode=$2 lpid=""
  confine "$mode"
  # The memory load is the same in every arm -- it is what makes the tail interesting.
  taskset -c $HK stress-ng --vm 1 --vm-bytes 128M --vm-keep --timeout $((SECS + 90))s -q &
  lpid=$!
  sleep 5
  mk
  if [[ $mode == on ]]; then
    # The engine must start *inside* the partition, or the slice confinement stops it reaching
    # cpu 3 -- which is the bug this script exists to correct.
    sudo -n systemd-run --unit="nab-$name" --slice=audio.slice \
      -p AllowedCPUs=$AUDIO -p LimitRTPRIO=95 -p LimitMEMLOCK=infinity \
      -p Type=oneshot --uid=rik --wait --collect \
      env EXPECT_NO_ISOLATION=1 NAMCHAIN_ARM_LABEL="tib confine AB $name: confined, vm load" \
      "$REPO/tools/soak/arm-run.sh" "$REPO/bench-results/$name" "$SOAK" "${args[@]}" >/dev/null 2>&1
  else
    EXPECT_NO_ISOLATION=1 NAMCHAIN_ARM_LABEL="tib confine AB $name: unconfined, vm load" \
      "$REPO/tools/soak/arm-run.sh" "$REPO/bench-results/$name" "$SOAK" "${args[@]}" >/dev/null 2>&1
  fi
  echo "  $name rc=$?"
  kill "$lpid" 2>/dev/null; wait "$lpid" 2>/dev/null; pkill -x stress-ng 2>/dev/null; sleep 3
  printf '  pin: '; grep -E '^scheduling:' "$REPO/bench-results/$name.txt" 2>/dev/null | sed 's/scheduling: *//'
}

echo "tib confinement A/B: 4 arms x ${SECS}s, no reboots, balanced"
arm tib-cx-1-unconf off
arm tib-cx-2-conf   on
arm tib-cx-3-conf   on
arm tib-cx-4-unconf off
confine off
echo "=== done ==="
python3 "$REPO/tools/soak/rt-arm-summary.py" \
  "$REPO"/bench-results/tib-cx-1-unconf "$REPO"/bench-results/tib-cx-2-conf \
  "$REPO"/bench-results/tib-cx-3-conf "$REPO"/bench-results/tib-cx-4-unconf 2>/dev/null
