#!/usr/bin/env bash
# rt-tier2-battery.sh — the arms that explain the residual tail and measure the headroom.
#
# Run after the headline hour. Each arm is 5-15 minutes and the whole battery is ~55 min,
# unattended. It must run on an otherwise IDLE machine: the 2026-09-16 screen showed that
# routine housekeeping (a filesystem `find`, kernel md5sums) during an arm inflates the DSP's
# CPU time on an isolated core by ~50% and turns a passing 5-minute arm into a failing one.
# That is not a nuisance to be averaged away; it is the H-work mechanism, and it is what
# arm L1 below measures deliberately instead of accidentally.
#
# The arms, and what each one is for:
#
#   v1    the shipping config WITHOUT the splice verifier and WITHOUT the floor. The hour
#         that failed ran with both. This is the instrument-free baseline, and it is also
#         the closest match to the non-RT hour's configuration (which had neither).
#   l1    v1 plus load on the housekeeping cores only (cpus 0-1), with the CPU frequency
#         sampled throughout. If the tail grows, it is shared memory/IO bandwidth and the
#         core reservation cannot shield the engine from it; if the tail is unchanged, the
#         reservation is doing what it was bought for. The frequency log rules out the
#         boring explanation (DVFS) for any spike seen.
#   t1    instances 6 and 8. Criterion 3 asks for 4; this is how much headroom the RT host
#         actually bought, and whether the tail scales with the work.
#   q1    quantum 32 and 128. q32 is a 666 us period, which the old 1000 Hz tick landed
#         inside on every single period; on a tickless core that argument disappears, so
#         q32 should improve out of proportion. It is also the only way to halve the
#         end-to-end latency, since the queue cap cannot go below 4 periods (see c1).
#   c1    cap 3. The measured natural queue band on this host is p1 2.69 / p50 2.94
#         periods, so the placement rule says a cap inside that band must manufacture
#         drops. This arm tests that the rule still holds on an RT host -- and therefore
#         that cap 4 really is the lowest latency the queue can carry.
#
# Usage: rt-tier2-battery.sh   (from the repo root; no arguments)

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

SOAK=./build/namchain-soak
COMMON=(--device hw:2,0 --capture-channels 8 --playback-channels 6
        --quantum 64 --periods 6 --output-queue-periods 4
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

# --- v1: the instrument-free baseline ------------------------------------------------
# --output-queue-floor-periods 0 and no --splice-verify. The floor never fired in the hour
# (0 insertions), so removing it must not change anything; if it does, that is a finding.
run rt-piv-v1-noverify "v1: cap 4, floor off, NO splice-verify (instrument-free baseline)" 900 \
    --output-queue-floor-periods 0

# --- l1: load on the housekeeping cores ----------------------------------------------
FREQLOG=bench-results/rt-piv-l1-freq.txt
{
  echo "arm rt-piv-l1-load, sampled every 2 s"
  echo "time cur_khz temp_mC loadavg"
} > "$FREQLOG"
(
  while true; do
    printf '%s %s %s %s\n' "$(date +%s)" \
      "$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq 2>/dev/null)" \
      "$(cat /sys/class/hwmon/hwmon0/temp1_input 2>/dev/null)" \
      "$(cut -d' ' -f1 /proc/loadavg)" >> "$FREQLOG"
    sleep 2
  done
) &
freq_pid=$!
# 2 cpu workers to match the 2 housekeeping cores, plus io and vm workers, plus a
# hackbench loop pinned to the same cores. Self-terminating so a lost shell cannot leave
# the machine pinned at 100% for ever.
taskset -c 0,1 stress-ng --cpu 2 --io 1 --vm 1 --vm-bytes 128M --timeout 930s --quiet >/dev/null 2>&1 &
stress_pid=$!
( end=$(( $(date +%s) + 920 ))
  while [ "$(date +%s)" -lt "$end" ]; do
    taskset -c 0,1 hackbench -l 200 >/dev/null 2>&1 || break
  done ) &
hb_pid=$!

run rt-piv-l1-load "l1: v1 + stress-ng(2 cpu,io,vm) + hackbench, pinned to cpus 0-1" 900 \
    --output-queue-floor-periods 0

kill "$freq_pid" "$stress_pid" "$hb_pid" 2>/dev/null
wait "$freq_pid" "$stress_pid" "$hb_pid" 2>/dev/null
pkill -x stress-ng 2>/dev/null; pkill -x hackbench 2>/dev/null
echo "##### load generators stopped $(date -Is)"

# --- t1: how many instances fit ------------------------------------------------------
run rt-piv-t1-inst6 "t1: 6 x A2-Full (criterion asks for 4)" 300 \
    --output-queue-floor-periods 0 --instances 6
run rt-piv-t1-inst8 "t1: 8 x A2-Full" 300 \
    --output-queue-floor-periods 0 --instances 8

# --- q1: the quantum, and the tickless q32 prediction --------------------------------
run rt-piv-q1-q32 "q1: quantum 32 (666 us period; no tick on this core)" 300 \
    --output-queue-floor-periods 0 --quantum 32
run rt-piv-q1-q128 "q1: quantum 128" 300 \
    --output-queue-floor-periods 0 --quantum 128

# --- c1: is cap 4 really the lowest the queue can go? --------------------------------
run rt-piv-c1-cap3 "c1: cap 3, inside the measured natural band (p50 2.94) -- expect drops" 300 \
    --output-queue-floor-periods 0 --output-queue-periods 3

echo "##### BATTERY COMPLETE $(date -Is)"
