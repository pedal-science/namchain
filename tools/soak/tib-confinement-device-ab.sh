#!/usr/bin/env bash
# tib-confinement-device-ab.sh — re-run E5b/E5c properly: does confinement move the *device*
# path on tib, with and without a memory load?
#
# WHY IT NEEDS RE-RUNNING. `docs/distro-slimming.md` §7c and §7d are the two device-side
# confinement arms, and both confined the workload along with everything else: `AllowedCPUs=` on
# the top-level slices applies to the soak too, so its confined arms pushed the engine off cpu 3
# ("any of 2 cpus (not as requested: sched_setaffinity(cpu 3): Invalid argument)") and onto the
# loaded housekeeping cores. Their headline -- "confinement takes driver xruns 746 -> 0" -- is
# therefore not a confinement effect at all. See §7.0.
#
# What this runs instead is the same 2x2 with the engine held on the audio core throughout:
#
#   arm                 load   confinement
#   tib-dx-1-quiet-unc  none   off
#   tib-dx-2-vm-unc     vm     off
#   tib-dx-3-vm-conf    vm     on
#   tib-dx-4-quiet-conf none   on
#
# Balanced, in ONE session, with NO reboots -- confinement is a runtime property, so there is no
# excuse for the confound this replaces.
#
# The engine configuration is E5c's ("the piv shipping queue shape"), because E5b's quiet arm used
# `--output-queue-periods 0` and its report says plainly that the ring had no headroom by design:
# 769 blocks wrote into a full playback ring and the write tail reached 3.1 ms. Its author already
# wrote that "the quiet row must not be quoted as tib's device ceiling".
#
# Usage: tib-confinement-device-ab.sh [seconds-per-arm]     (on tib; 600 by default)

set -u
cd "$(dirname "$0")/../.." || exit 1
REPO=$PWD
SOAK=$REPO/bin-neon/namchain-soak
MODEL=$REPO/models/A2-Lite.nam
SECS="${1:-600}"
HK=0-1
AUDIO=2-3

card() { arecord -l 2>/dev/null | awk '/SSL 2\+ Mk II/ { gsub(":", "", $2); print $2; exit }'; }

args=()
mk() {  # mk CARD
  args=(--device "hw:$1,0" --capture-channels 8 --playback-channels 6
        --quantum 64 --periods 6 --output-queue-periods 4 --output-queue-floor-periods 2
        --instances 3 --model "$MODEL" --input-channel 0 --output-channel 0
        --self-oracle --oracle-window 5 --rt-priority 80 --cpu 3 --seconds "$SECS")
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
  echo "  confinement=$want"
}

arm() {  # arm NAME CONF ON|off WITH_LOAD yes|no
  local name=$1 mode=$2 withload=$3 lpid="" c
  c=$(card)
  if [[ -z $c ]]; then echo "  $name: no SSL 2+ present -- skipping"; return 1; fi
  confine "$mode"
  if [[ $withload == yes ]]; then
    taskset -c $HK stress-ng --vm 1 --vm-bytes 128M --vm-keep --timeout $((SECS + 90))s -q &
    lpid=$!
    sleep 5
  fi
  mk "$c"
  if [[ $mode == on ]]; then
    # Inside its own partition, or the slice confinement stops it reaching cpu 3.
    sudo -n systemd-run --unit="nab-$name" --slice=audio.slice \
      -p AllowedCPUs=$AUDIO -p LimitRTPRIO=95 -p LimitMEMLOCK=infinity \
      -p Type=oneshot --uid=rik --wait --collect \
      env EXPECT_NO_ISOLATION=1 NAMCHAIN_ARM_LABEL="tib device confine AB $name: $mode load=$withload" \
      "$REPO/tools/soak/arm-run.sh" "$REPO/bench-results/$name" "$SOAK" "${args[@]}" >/dev/null 2>&1
  else
    EXPECT_NO_ISOLATION=1 NAMCHAIN_ARM_LABEL="tib device confine AB $name: $mode load=$withload" \
      "$REPO/tools/soak/arm-run.sh" "$REPO/bench-results/$name" "$SOAK" "${args[@]}" >/dev/null 2>&1
  fi
  echo "  $name rc=$? card=$c"
  [[ -n $lpid ]] && { kill "$lpid" 2>/dev/null; wait "$lpid" 2>/dev/null; pkill -x stress-ng 2>/dev/null; sleep 3; }
  printf '  pin: '; grep -E '^scheduling:' "$REPO/bench-results/$name.txt" 2>/dev/null | sed 's/scheduling: *//'
}

echo "tib device confinement A/B: 4 arms x ${SECS}s, one session, no reboots"
arm tib-dx-1-quiet-unc off no
arm tib-dx-2-vm-unc    off yes
arm tib-dx-3-vm-conf   on  yes
arm tib-dx-4-quiet-conf on no
confine off
echo "=== done ==="
python3 "$REPO/tools/soak/rt-arm-summary.py" \
  "$REPO"/bench-results/tib-dx-1-quiet-unc "$REPO"/bench-results/tib-dx-2-vm-unc \
  "$REPO"/bench-results/tib-dx-3-vm-conf "$REPO"/bench-results/tib-dx-4-quiet-conf 2>/dev/null
