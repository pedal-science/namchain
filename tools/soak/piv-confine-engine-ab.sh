#!/usr/bin/env bash
# piv-confine-engine-ab.sh — does cgroup v2 cpuset confinement move the *engine's* tail?
#
# WHY THIS EXISTS, AND WHAT IT FIXES
#
# docs/distro-slimming.md §7 ran a confinement screen whose "confined" arms confined the
# workload along with everything else: `AllowedCPUs=` on the top-level slices applies to the
# measurement process too, so the engine was pushed off the audio cores and onto the loaded
# housekeeping cores. Its own reports say so -- "any of 2 cpus (not as requested:
# sched_setaffinity(cpu 3): Invalid argument)" -- which means those tables compare two
# *placements* rather than one placement with and without confinement. See §7.0 of that file.
#
# This script runs the comparison properly:
#
#   1. the top-level slices are held at 0-1, so every *other* task is off the audio cores;
#   2. the audio work gets its own cpuset partition (`AllowedCPUs=2-3` on audio.slice), so it is
#      the one thing allowed to use them, and `--cpu 3` therefore succeeds;
#   3. the rlimits are passed to the unit -- a systemd service does not get pam_limits, and
#      without `-p LimitRTPRIO=95` the identical invocation silently runs SCHED_OTHER.
#
# Four arms, so both questions are answered on one configuration:
#
#   A  unconfined, no load     (already measured as the piv isolation A/B's "on" arms: 15 and 4
#                               xruns, 555 and 300 late blocks, worst blocks 3728 and 2964 us)
#   B  confined,   no load     -> does confinement alone move the tail?
#   C  unconfined, vm load     -> the reference for D
#   D  confined,   vm load     -> does the engine survive a memory load once confined?
#
# The load is `stress-ng --vm 1 --vm-bytes 128M --vm-keep` pinned to cpus 0-1: the project's own
# measurements say *memory* traffic is what costs the engine, and that `--cpu` load does not.
#
# Usage: piv-confine-engine-ab.sh [--seconds N]   (on piv, root for the cgroup changes)

set -u
cd "$(dirname "$0")/../.." || exit 1
REPO=$PWD
SOAK=$REPO/build/namchain-soak
MODEL=$REPO/build/generated/A2-Full.nam
SECONDS_PER_ARM="${1:-1800}"
HK=0-1

card() { arecord -l 2>/dev/null | awk '/SSL 2\+ Mk II/ { gsub(":", "", $2); print $2; exit }'; }

SOAK_ARGS=()
mk_args() {  # mk_args CARD
  SOAK_ARGS=(--device "hw:$1,0" --capture-channels 8 --playback-channels 6
             --quantum 64 --periods 6 --output-queue-periods 3 --output-queue-floor-periods 0
             --splice-fade-ms 1.333 --splice-verify --instances 4 --model "$MODEL"
             --input-channel 0 --output-channel 0 --self-oracle --oracle-window 5
             --rt-priority 80 --cpu 3 --seconds "$SECONDS_PER_ARM")
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
  echo "  confinement=$want  system.slice=$(cat /sys/fs/cgroup/system.slice/cpuset.cpus.effective 2>/dev/null)" \
       "user.slice=$(cat /sys/fs/cgroup/user.slice/cpuset.cpus.effective 2>/dev/null)"
}

load_start() { taskset -c $HK stress-ng --vm 1 --vm-bytes 128M --vm-keep --timeout $((SECONDS_PER_ARM + 90))s -q & echo $!; }
load_stop()   { kill "$1" 2>/dev/null; wait "$1" 2>/dev/null; pkill -x stress-ng 2>/dev/null; sleep 2; }

run_unconfined() {  # run_unconfined NAME WITH_LOAD
  local name=$1 withload=$2 c; c=$(card)
  confine off
  local lpid=""
  [[ $withload == yes ]] && { lpid=$(load_start); sleep 5; }
  mk_args "$c"
  echo "=== $name (unconfined, load=$withload) engine card=$c, pinned cpu 3 ==="
  NAMCHAIN_ARM_LABEL="piv confine-ab $name: unconfined load=$withload" \
    ./tools/soak/arm-run.sh "bench-results/$name" "$SOAK" "${SOAK_ARGS[@]}" >/dev/null 2>&1
  echo "  rc=$?"
  [[ -n $lpid ]] && load_stop "$lpid"
}

run_confined() {  # run_confined NAME WITH_LOAD
  local name=$1 withload=$2 c; c=$(card)
  confine on
  local lpid=""
  [[ $withload == yes ]] && { lpid=$(load_start); sleep 5; }
  mk_args "$c"
  echo "=== $name (CONFINED, load=$withload) engine in audio.slice/2-3, card=$c ==="
  # The engine must be started inside its own partition, or the slice confinement stops it
  # reaching cpu 3 -- which is the bug this script exists to avoid.
  sudo systemd-run --unit="nab-$name" --slice=audio.slice \
    -p AllowedCPUs=2-3 -p LimitRTPRIO=95 -p LimitMEMLOCK=infinity \
    -p Type=oneshot --uid=rik --wait --collect \
    env NAMCHAIN_ARM_LABEL="piv confine-ab $name: confined load=$withload" \
    "$REPO/tools/soak/arm-run.sh" "$REPO/bench-results/$name" "$SOAK" "${SOAK_ARGS[@]}" >/dev/null 2>&1
  echo "  rc=$?"
  [[ -n $lpid ]] && load_stop "$lpid"
  confine off
}

# A (unconfined, quiet) is the isolation A/B's on-arms; B (confined, quiet) is already
# measured as piv-ab-4-on-confined (0 xruns, 19 late blocks, p99 441 us). Only the vm-load
# pair is still missing, so it is the only thing this runs.
run_unconfined piv-cab-c-unconf-vm   yes
run_confined   piv-cab-d-confined-vm yes

echo "=== done; summarise with rt-arm-summary.py ==="
