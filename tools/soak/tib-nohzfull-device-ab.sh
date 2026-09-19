#!/usr/bin/env bash
# tib-nohzfull-device-ab.sh — does the trimmed 7.2.6-tib1 kernel, with isolcpus=3 +
# nohz_full=3, move the DEVICE path on tib?
#
# WHY THIS IS TWO ARMS AND NOT FOUR
# The lever changed. `tib-confinement-device-ab.sh` (the tib-dx-* family) measured cgroup-v2
# cpuset confinement, which was tib's only isolation lever at the time, in a balanced 2x2.
# The new kernel reserves cpu 3 with `isolcpus=managed_irq,domain,3` and stops its tick with
# `nohz_full=3`, so slice confinement is redundant and the comparison that matters is against
# the UNCONFINED baseline arms: same engine, same load, same pins, only the kernel's own
# reservation differs.
#
#   tib-dx-1-quiet-unc   7.2.6       no isolation          no load   <- baseline
#   tib-nz-quiet         7.2.6-tib1  isolcpus+nohz_full    no load
#   tib-dx-2-vm-unc      7.2.6       no isolation          vm load   <- baseline
#   tib-nz-vm            7.2.6-tib1  isolcpus+nohz_full    vm load
#
# The engine configuration is tib-dx's exactly (E5c's "piv shipping queue shape"), including
# --periods 6 --output-queue-periods 4 --output-queue-floor-periods 2, so the kernel and the
# load are the only differences.
#
# This script deliberately changes NO host state and restores nothing: the whole point of the
# new kernel is that the reservation is a boot property rather than a runtime cgroup write.
# It does print the slice cpusets so a leftover confinement from an earlier session shows up
# as the confound it would be (it would stop the engine reaching cpu 3).
#
# Usage: tib-nohzfull-device-ab.sh [seconds-per-arm]        (on tib; 600 by default)

set -u
cd "$(dirname "$0")/../.." || exit 1
REPO=$PWD
SOAK=$REPO/bin-neon/namchain-soak
MODEL=$REPO/models/A2-Lite.nam
SECS="${1:-600}"
HK=0-1

# Card numbers move between boots (the SSL was card 2 on 2026-09-17 and card 1 on 2026-09-19),
# so resolve it by name every time rather than hard-coding the path in the evidence.
card() { arecord -l 2>/dev/null | awk '/SSL 2\+ Mk II/ { gsub(":", "", $2); print $2; exit }'; }

args=()
mk() {  # mk CARD
  args=(--device "hw:$1,0" --capture-channels 8 --playback-channels 6
        --quantum 64 --periods 6 --output-queue-periods 4 --output-queue-floor-periods 2
        --instances 3 --model "$MODEL" --input-channel 0 --output-channel 0
        --self-oracle --oracle-window 5 --rt-priority 80 --cpu 3 --seconds "$SECS")
}

arm() {  # arm NAME WITH_LOAD yes|no
  local name=$1 withload=$2 lpid="" c
  c=$(card)
  if [[ -z $c ]]; then echo "  $name: no SSL 2+ present -- skipping"; return 1; fi
  printf '  %s: user.slice=%-4s system.slice=%-4s (both must be 0-3, or the engine cannot reach cpu 3)\n' \
    "$name" "$(cat /sys/fs/cgroup/user.slice/cpuset.cpus.effective 2>/dev/null)" \
    "$(cat /sys/fs/cgroup/system.slice/cpuset.cpus.effective 2>/dev/null)"
  if [[ $withload == yes ]]; then
    # The same load E5 and tib-dx used. --vm-keep is deliberate: without it stress-ng frees and
    # re-maps its 128MB every cycle, which is a different and much heavier load.
    taskset -c $HK stress-ng --vm 1 --vm-bytes 128M --vm-keep --timeout $((SECS + 90))s -q &
    lpid=$!
    sleep 5
  fi
  mk "$c"
  # No EXPECT_NO_ISOLATION: the reservation is present, so the isolation check must prove it
  # (isolated=3, nohz_full=3, .tick_stopped=1) rather than excuse its absence.
  NAMCHAIN_ARM_LABEL="tib nohz_full AB $name: isolcpus+nohz_full, load=$withload" \
    "$REPO/tools/soak/arm-run.sh" "$REPO/bench-results/$name" "$SOAK" "${args[@]}" >/dev/null 2>&1
  echo "  $name rc=$? card=$c"
  [[ -n $lpid ]] && { kill "$lpid" 2>/dev/null; wait "$lpid" 2>/dev/null; pkill -x stress-ng 2>/dev/null; sleep 3; }
  printf '  pin:  '; grep -E '^scheduling:' "$REPO/bench-results/$name.txt" 2>/dev/null | sed 's/scheduling: *//'
  printf '  tick: '; grep -E '^ok: +cpu 3 \.tick_stopped|^FAIL: +cpu 3 \.tick_stopped' "$REPO/bench-results/$name.txt" 2>/dev/null | head -1
}

echo "tib NO_HZ_FULL device A/B: 2 arms x ${SECS}s, one session, no reboots, no confinement"
echo "kernel:   $(uname -r)"
echo "isolated: $(cat /sys/devices/system/cpu/isolated)"
echo "nohz_full: $(cat /sys/devices/system/cpu/nohz_full 2>/dev/null)"
arm tib-nz-quiet no
arm tib-nz-vm    yes
echo "=== done ==="
python3 "$REPO/tools/soak/rt-arm-summary.py" \
  "$REPO/bench-results/tib-dx-1-quiet-unc" "$REPO/bench-results/tib-nz-quiet" \
  "$REPO/bench-results/tib-dx-2-vm-unc"    "$REPO/bench-results/tib-nz-vm" 2>/dev/null
