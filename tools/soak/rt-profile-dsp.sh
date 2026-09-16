#!/usr/bin/env bash
# rt-profile-dsp.sh — where do the extra cycles go when the memory system is busy?
#
# The dose arms established that memory stress costs the same instructions 2.4x the cycles
# (IPC 1.70 -> 0.69) with no change in cache misses per block. This script asks the next
# question: *which code* absorbs those cycles. It is the difference between "the engine is
# memory-bound" (a fact) and "this loop, loading this data, is where it goes" (something a
# change can act on).
#
# Instruments, and why each:
#   perf stat   with the Cortex-A76 stall and mem_access events, quiet vs loaded, to name the
#               stall type (backend vs frontend) rather than infer it from IPC.
#   perf record -e cycles, quiet vs loaded, reported BY SYMBOL. The binary is Release (-O3,
#               no -g, no frame pointers), so there is no call graph -- but it is not
#               stripped and carries ~227 A2/WaveNet symbols, which is enough to say which
#               kernel absorbs the extra cycles. The *difference* between the two symbol
#               tables is the measurement; neither table alone means anything, because
#               whichever function is hottest is also hottest when the machine is idle.
#
# Both arms carry the same perf overhead, so the comparison is fair even though perf itself
# is noise. The four runs take about 6 minutes.
#
# Usage: rt-profile-dsp.sh   (from the repo root)

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

OUT=bench-results
SOAK=./build/namchain-soak
COMMON=(--device hw:2,0 --capture-channels 8 --playback-channels 6
        --quantum 64 --periods 6 --output-queue-periods 3 --output-queue-floor-periods 0
        --instances 4 --model build/generated/A2-Full.nam
        --input-channel 0 --output-channel 0 --self-oracle --oracle-window 5
        --rt-priority 80 --cpu 3)

# Cortex-A76 events. stall_backend is "no operation issued due to the backend", which is the
# direct question; mem_access counts the data-side accesses that can stall.
EVENTS=cycles,instructions,stall_backend,stall_frontend,mem_access,mem_access_rd,mem_access_wr,branch-misses

# A compact top-N: percentage and enough of the symbol to identify it. `perf report` prints
# "    10.47%  [.] symbol", so the match needs [[:space:]], not \s -- the first version of
# this script used \s and silently produced nothing.
top() {  # top FILE [N]
  sudo perf report --stdio --sort symbol -i "$1" --percent-limit 0.3 2>/dev/null \
    | grep -E '^[[:space:]]+[0-9]+\.[0-9]+%' \
    | sed 's/\[\.\]//; s/\[clon.*//; s/\[k\]//' \
    | awk '{ pct=$1; $1=""; sub(/^[[:space:]]+/, ""); printf "%9s  %s\n", pct, substr($0, 1, 88) }' \
    | head -"${2:-20}"
}

: > "$OUT/rt-piv-profile.txt"
{
  echo "DSP stall profiling, 4 x A2-Full at q64, cap 3, cpu 3"
  echo "quiet vs memory-loaded. The DELTA between the two tables is the measurement."
} >> "$OUT/rt-piv-profile.txt"

# --- quiet ---------------------------------------------------------------------------
echo "==> quiet: stat"
"$HERE/arm-run.sh" "$OUT/rt-piv-prof-quiet-stat" \
  sudo perf stat -e "$EVENTS" "$SOAK" "${COMMON[@]}" --seconds 120
grep -E 'seconds time elapsed|[0-9,]+ +[a-z]' "$OUT/rt-piv-prof-quiet-stat.txt" | tail -12 >> "$OUT/rt-piv-profile.txt"

echo "==> quiet: record"
rm -f "$OUT/rt-piv-quiet.data"
sudo perf record -e cycles -o "$OUT/rt-piv-quiet.data" -- "$SOAK" "${COMMON[@]}" --seconds 60 \
  >"$OUT/rt-piv-prof-quiet-rec.txt" 2>&1
{
  echo
  echo "########## QUIET — top symbols by cycles"
  top "$OUT/rt-piv-quiet.data" 22
} >> "$OUT/rt-piv-profile.txt"

# --- memory-loaded -------------------------------------------------------------------
# Only the vm worker: the dose arms showed the vm worker alone is what breaks the audio
# path (5460 xruns in 300 s), while pure cpu load and hackbench do not.
echo "==> loaded: stat + record"
taskset -c 0,1 stress-ng --vm 1 --vm-bytes 128M --timeout 230s --quiet >/dev/null 2>&1 &
stress_pid=$!
sleep 5

"$HERE/arm-run.sh" "$OUT/rt-piv-prof-load-stat" \
  sudo perf stat -e "$EVENTS" "$SOAK" "${COMMON[@]}" --seconds 120
grep -E 'seconds time elapsed|[0-9,]+ +[a-z]' "$OUT/rt-piv-prof-load-stat.txt" | tail -12 >> "$OUT/rt-piv-profile.txt"

rm -f "$OUT/rt-piv-load.data"
sudo perf record -e cycles -o "$OUT/rt-piv-load.data" -- "$SOAK" "${COMMON[@]}" --seconds 60 \
  >"$OUT/rt-piv-prof-load-rec.txt" 2>&1
{
  echo
  echo "########## MEMORY-LOADED (stress-ng --vm 1 --vm-bytes 128M on cpus 0-1) — top symbols by cycles"
  top "$OUT/rt-piv-load.data" 22
} >> "$OUT/rt-piv-profile.txt"

kill "$stress_pid" 2>/dev/null; pkill -x stress-ng 2>/dev/null; sleep 2

# perf data files are written by root.
sudo chown "$(id -u):$(id -g)" "$OUT/rt-piv-quiet.data" "$OUT/rt-piv-load.data" 2>/dev/null
chmod 0644 "$OUT/rt-piv-quiet.data" "$OUT/rt-piv-load.data" 2>/dev/null

echo "==> done: $OUT/rt-piv-profile.txt"
