#!/usr/bin/env bash
# Is the engine clean when it is asked to do nothing?
#
# Two rigs that must not change the signal at all -- an empty chain, and a chain whose one block is
# a gain left at 0 dB -- rendered over a WAV and compared with the WAV. A third, at +6 dB, is the
# control: a null test that cannot fail is not measuring anything.
#
# Each rig is rendered twice, once through namchain-render and once through `namchaind --backend
# file`, because "the DSP is clean", "the daemon's render path is clean" and "the client's JSON is
# right" are three claims and conflating them is how an afternoon disappears.
#
# usage: null-test.sh [INPUT.wav]
#   NAMCHAIN_RIGS=DIR   read empty.json / gain0.json / gain6.json from here instead of writing
#                       throwaway ones. Point it at what Rig Designer's Tools/dump-engine-document.sh
#                       produced and this is testing the app's own bytes.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${NAMCHAIN_BUILD:-$root/build}"
work="${NAMCHAIN_WORK:-/tmp/nc}"
input="${1:-$work/input-f32.wav}"
rigs="${NAMCHAIN_RIGS:-$work/null-test-rigs}"

render="$build/namchain-render"
wavcmp="$build/namchain-wavcmp"
daemon="$build/namchaind"
for tool in "$render" "$wavcmp" "$daemon"; do
  [[ -x "$tool" ]] || { echo "no $tool -- build it first" >&2; exit 1; }
done
[[ -f "$input" ]] || { echo "no input WAV at $input" >&2; exit 1; }

out="$work/null-test"
mkdir -p "$out" "$out/rigs" "$out/models" "$out/irs"

if [[ ! -d "$rigs" ]]; then
  mkdir -p "$rigs"
  cat > "$rigs/empty.json" <<'JSON'
{ "nodes": [ { "id": "in", "type": "nc.input" }, { "id": "out", "type": "nc.output" } ],
  "connections": [ { "from": "in", "to": "out" } ] }
JSON
  cat > "$rigs/gain0.json" <<'JSON'
{ "nodes": [ { "id": "in", "type": "nc.input" }, { "id": "g", "type": "nc.gain", "gain_db": 0 },
             { "id": "out", "type": "nc.output" } ],
  "connections": [ { "from": "in", "to": "g" }, { "from": "g", "to": "out" } ] }
JSON
  cat > "$rigs/gain6.json" <<'JSON'
{ "nodes": [ { "id": "in", "type": "nc.input" }, { "id": "g", "type": "nc.gain", "gain_db": 6 },
             { "id": "out", "type": "nc.output" } ],
  "connections": [ { "from": "in", "to": "g" }, { "from": "g", "to": "out" } ] }
JSON
fi

failures=0

# A startup rig is read before the clock starts, so it is in place from sample 0. A rig that arrives
# later is a structural transition and fades in over --fade-ms, which is correct behaviour and would
# make every one of these comparisons fail by a few milliseconds.
render_through_daemon() {
  local rig="$1" destination="$2"
  rm -f "$destination"
  "$daemon" --backend file --capture-path "$input" --playback-path "$destination" \
    --sample-rate 48000 --period-size 64 --capture-channels 1 --playback-channels 1 \
    --rig "$rig" --rigs-root "$out/rigs" --models-root "$out/models" --irs-root "$out/irs" \
    --socket "$out/d.sock" --no-lock-memory --rt-priority 0 >/dev/null 2>&1 &
  local pid=$!
  # The render thread stops itself at end of input; the control loop does not, because the socket
  # stays up. So wait for the output to appear and then end it.
  local waited=0
  while [[ ! -s "$destination" ]] && (( waited < 600 )); do sleep 0.5; waited=$((waited + 1)); done
  sleep 0.5
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  [[ -s "$destination" ]]
}

check() {
  local label="$1" expectation="$2" a="$3" b="$4"
  local status=0
  "$wavcmp" --max-abs 0 "$a" "$b" > "$out/cmp.txt" 2>&1 || status=$?
  local summary
  summary="$(grep -E 'max\|a-b\||rms\(a-b\)|mismatch' "$out/cmp.txt" | tr '\n' ' ' | tr -s ' ')"
  if [[ "$expectation" == identical && $status -eq 0 ]] ||
     [[ "$expectation" == different && $status -ne 0 ]]; then
    printf 'PASS  %-34s %s\n' "$label" "$summary"
  else
    printf 'FAIL  %-34s %s\n' "$label" "$summary"
    failures=$((failures + 1))
  fi
}

echo "input: $input"
echo "rigs:  $rigs"
echo

for rig in empty gain0; do
  "$render" --graph "$rigs/$rig.json" "$input" "$out/render-$rig.wav" >/dev/null
  check "$rig: renderer is transparent" identical "$out/render-$rig.wav" "$input"
  if render_through_daemon "$rigs/$rig.json" "$out/daemon-$rig.wav"; then
    check "$rig: daemon is transparent" identical "$out/daemon-$rig.wav" "$input"
    check "$rig: daemon agrees with renderer" identical "$out/daemon-$rig.wav" "$out/render-$rig.wav"
  else
    printf 'FAIL  %-34s the daemon wrote no output\n' "$rig: daemon is transparent"
    failures=$((failures + 1))
  fi
done

# The control. +6 dB must come out louder by 10^(6/20) = 1.9953, so a comparison that passes here
# would mean the gain is not reaching the engine at all.
"$render" --graph "$rigs/gain6.json" "$input" "$out/render-gain6.wav" >/dev/null
check "gain6: the test can fail" different "$out/render-gain6.wav" "$input"
ratio="$(python3 - "$out/render-gain6.wav" "$input" <<'PY'
import array, struct, sys
def peak(path):
    data = open(path, 'rb').read()
    i = data.find(b'data', 12)
    size = struct.unpack('<I', data[i+4:i+8])[0]
    a = array.array('f'); a.frombytes(data[i+8:i+8+size])
    return max(abs(x) for x in a)
print("%.4f" % (peak(sys.argv[1]) / peak(sys.argv[2])))
PY
)"
if python3 -c "import sys; sys.exit(0 if abs($ratio - 1.9953) < 0.001 else 1)"; then
  printf 'PASS  %-34s peak ratio %s (10^(6/20) = 1.9953)\n' "gain6: +6 dB is +6 dB" "$ratio"
else
  printf 'FAIL  %-34s peak ratio %s, expected 1.9953\n' "gain6: +6 dB is +6 dB" "$ratio"
  failures=$((failures + 1))
fi

echo
if (( failures == 0 )); then echo "PASS"; else echo "$failures FAILED"; fi
exit $(( failures > 0 ))
