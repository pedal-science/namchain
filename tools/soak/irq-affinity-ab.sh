#!/usr/bin/env bash
#
# Does steering device interrupts away from the audio core change the loop's tail?
#
# The hypothesis has been open since §9.3 and the loop's own instrumentation cannot answer it: `cpu ==
# wall` with 0 blocks descheduled rules out preemption, but softirq/IRQ work is charged to whichever task
# the CPU was running, so an interrupt burst on the audio core looks exactly like slow engine code.
#
# Design: alternating arms, A B A B ..., one 300 s soak each. Alternating is the point -- the host drifts
# enough that two arms of the *same* configuration 20 minutes apart differed by 8.8 points of chain p99,
# so only the difference *within* an adjacent pair is trustworthy, and the pairs are averaged.
#
#   A: IRQ affinity left alone          B: every steerable IRQ's mask minus cpu 3
#
# Every run records cpu 3's interrupt count for its own duration, so the manipulation is checked rather
# than assumed: if the B arms' deltas are not well below the A arms', nothing was tested.
#
# LIMIT, stated because it bounds what a null result means: only *steerable* IRQs move. The arch timer,
# IPIs and RCU callbacks are per-CPU and cannot be steered from /proc, and those are exactly the sources a
# nohz_full/rcu_nocbs/threadirqs boot configuration would address. So this tests the device-IRQ half.
#
# Usage: sudo -n tools/soak/irq-affinity-ab.sh [pairs] [seconds]
set -u
cd "$(dirname "$0")/../.."
PAIRS=${1:-8}
SECONDS_PER_ARM=${2:-300}
SOAK=./build/namchain-soak
MODEL=build/generated/A2-Full.nam
ORIG=/tmp/irq-affinity-original.txt
EVID=bench-results/irq-affinity-experiment.txt

expand_cpulist() {
  echo "$1" | tr ',' '\n' | while read -r part; do
    case "$part" in *-*) seq "${part%-*}" "${part#*-}";; *) echo "$part";; esac
  done
}
strip_cpu3() { expand_cpulist "$1" | grep -v '^3$' | paste -sd, -; }
cpu3_irqs() { awk 'NR==1 {n=NF} /^ *[0-9]+:/ {s+=$(1+n)} END {print s+0}' /proc/interrupts; }

: > "$ORIG"; : > "$EVID"
if systemctl is-active --quiet irqbalance 2>/dev/null; then IRQB=active; else IRQB=inactive; fi
echo "irqbalance        $IRQB" >> "$EVID"
echo "pairs             $PAIRS x $SECONDS_PER_ARM s" >> "$EVID"
steerable=0; on3=0; moved=0
for d in /proc/irq/[0-9]*; do
  n=${d#/proc/irq/}
  [ -r "$d/smp_affinity_list" ] || continue
  list=$(cat "$d/smp_affinity_list" 2>/dev/null) || continue
  [ -n "$list" ] || continue
  echo "$n $list" >> "$ORIG"
  steerable=$((steerable+1))
  if [ -n "$(expand_cpulist "$list" | grep '^3$')" ]; then
    on3=$((on3+1)); moved=$((moved+1))
  fi
done
echo "steerable IRQs    $steerable, deliverable to cpu 3: $on3 (all of these move in arm B)" >> "$EVID"

restore_all() {
  while read -r n list; do
    echo "$list" | sudo -n tee "/proc/irq/$n/smp_affinity_list" >/dev/null 2>&1 || true
  done < "$ORIG"
}
off_cpu3() {
  while read -r n list; do
    local want; want=$(strip_cpu3 "$list")
    [ -n "$want" ] || continue
    [ "$want" = "$list" ] && continue
    echo "$want" | sudo -n tee "/proc/irq/$n/smp_affinity_list" >/dev/null 2>&1 || true
  done < "$ORIG"
}
sudo -n systemctl stop irqbalance 2>/dev/null || true
trap 'restore_all; [ "$IRQB" = active ] && sudo -n systemctl start irqbalance 2>/dev/null; echo "restored, differences: $(d=0; while read -r n l; do [ "$(cat /proc/irq/$n/smp_affinity_list 2>/dev/null)" = "$l" ] || d=$((d+1)); done < "$ORIG"; echo $d)" >> "$EVID"' EXIT INT TERM

metric() { grep -m1 -E "$2" "$1" 2>/dev/null | sed -E 's/.*: *//' | tr -d '\n'; }

run() {
  local name="$1"
  local mode="$2"
  local out="bench-results/soak-irq-${name}"
  if [ "$mode" = off ]; then off_cpu3; else restore_all; fi
  local before after
  before=$(cpu3_irqs)
  {
    echo "commit      $(git rev-parse HEAD)"
    echo "arm         $name mode=$mode"
    echo "start       $(date -Is)"
    echo "loadavg     $(cat /proc/loadavg)"
  } > "$out.env"
  tools/soak/heartbeat-run.sh "$out" taskset -c 3 $SOAK --device hw:2,0 --capture-channels 8 \
    --playback-channels 6 --quantum 64 --periods 6 --output-queue-periods 4 --seconds "$SECONDS_PER_ARM" \
    --instances 4 --model "$MODEL" --input-channel 0 --output-channel 0 --self-oracle \
    --oracle-window 5 --rt-priority 80 --cpu 3 > "$out.txt" 2>&1
  local rc=$?
  after=$(cpu3_irqs)
  echo "exit        $rc" >> "$out.env"
  echo "cpu3_delta  $((after-before))" >> "$out.env"
  local cp99 overruns xruns spikes
  cp99=$(grep -m1 '^chain p99:' "$out.txt" | awk '{print $3}')
  overruns=$(grep -m1 '^deadline overruns:' "$out.txt" | awk '{print $3}')
  xruns=$(grep -m1 '^driver xruns:' "$out.txt" | awk '{print $3}')
  spikes=$(grep -m1 '^cpu spike bursts:' "$out.txt" | awk '{print $3}')
  printf '%-4s %-3s cpu3 %-8s chain_p99 %-8s overruns %-5s xruns %-3s spikes %s\n' \
    "$name" "$mode" "$((after-before))" "$cp99" "$overruns" "$xruns" "$spikes" >> "$EVID"
  echo "== $name ($mode) rc=$rc cpu3_delta $((after-before)) chain_p99 $cp99 overruns $overruns xruns $xruns spikes $spikes"
}

for p in $(seq 1 "$PAIRS"); do
  run "a${p}" on
  run "b${p}" off
done
echo "IRQ PAIRED AB DONE"
