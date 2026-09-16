#!/usr/bin/env bash
# rt-tier3-dose.sh — how much housekeeping load does this engine tolerate, and why does it
# lose when it loses?
#
# The l1 arm (15 min of stress-ng + hackbench pinned to cpus 0-1) took the audio path apart:
# 3119 driver xruns, 26014 deadline overruns, block p99 at 156% of the deadline, and a
# chain whose own CPU time reached 4.1 ms -- with the CPU frequency constant at 2400 MHz for
# every one of 382 samples and no thermal throttling. That is not a scheduling failure; the
# reserved core was never descheduled. It is the DSP's work getting slower.
#
# Two questions follow, and they are the ones a reader of docs/tuning.md will actually ask:
#
#   1. Does it take a heavy load, or does a light one do it? (d1 light, d2 medium)
#   2. Is the mechanism the memory system, as the constant frequency suggests? (perf counters)
#
# The perf arms are 120 s rather than 15 min on purpose: the question is the *rate* of work
# and cache misses per block, not the rare tail, and a shorter arm keeps the two runs close
# together in time so a session drift cannot masquerade as a dose effect.
#
# Usage: rt-tier3-dose.sh   (from the repo root)

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

SOAK=./build/namchain-soak
COMMON=(--device hw:2,0 --capture-channels 8 --playback-channels 6
        --quantum 64 --periods 6 --output-queue-periods 4 --output-queue-floor-periods 0
        --instances 4 --model build/generated/A2-Full.nam
        --input-channel 0 --output-channel 0
        --self-oracle --oracle-window 5 --rt-priority 80 --cpu 3)

run() {  # run NAME LABEL SECONDS [extra args...]
  local name="$1" label="$2" secs="$3"; shift 3
  echo "##### $name  ($label)  $(date -Is)"
  NAMCHAIN_ARM_LABEL="$label" "$HERE/arm-run.sh" "bench-results/$name" \
    "$SOAK" "${COMMON[@]}" --seconds "$secs" "$@"
  echo "##### $name exit=$?  $(date -Is)"
}

# perf counters, if the kernel will let us have them (perf_event_paranoid=4 here, so root).
PERF=(sudo perf stat -e task-clock,cycles,instructions,cache-references,cache-misses,branch-misses)

stop_load() { pkill -x stress-ng 2>/dev/null; pkill -x hackbench 2>/dev/null; sleep 2; }

# --- d0: quiet baseline, with counters ------------------------------------------------
echo "##### d0 starting $(date -Is)"
NAMCHAIN_ARM_LABEL="d0: quiet baseline with perf counters" \
  "$HERE/arm-run.sh" bench-results/rt-piv-d0-perf-quiet \
  "${PERF[@]}" "$SOAK" "${COMMON[@]}" --seconds 120
echo "##### d0 exit=$? $(date -Is)"

# --- d1: light load — one busy housekeeping core, no memory stress ---------------------
taskset -c 0 stress-ng --cpu 1 --timeout 330s --quiet >/dev/null 2>&1 &
run rt-piv-d1-light "d1: one busy housekeeping core (stress-ng --cpu 1 on cpu 0)" 300
stop_load

# --- d2: medium load — both housekeeping cores busy, still no vm/io/hackbench ----------
taskset -c 0,1 stress-ng --cpu 2 --timeout 330s --quiet >/dev/null 2>&1 &
run rt-piv-d2-medium "d2: both housekeeping cores busy (stress-ng --cpu 2 on cpus 0-1)" 300
stop_load

# --- c2: cap 3 for 15 min -- the latency win, at a duration that can falsify it ---------
run rt-piv-c2-cap3-long "c2: cap 3 for 15 min (p50 occupancy 1.94 vs 2.94 periods at cap 4)" 900 \
    --output-queue-periods 3

echo "##### DOSE BATTERY COMPLETE $(date -Is)"
