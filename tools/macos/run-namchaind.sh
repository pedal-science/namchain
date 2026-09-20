#!/usr/bin/env bash
# Run namchaind on a Mac with a WAV on a loop for input and a FIFO for output.
#
# There is no audio interface in this picture. The input is a file, the output is a pipe, and
# whatever reads that pipe -- tools/macos/listen.sh -- is the clock. That is enough to drive the
# engine from a control client over TCP and hear the result, which is what a laptop is for.
#
# usage: run-namchaind.sh [INPUT.wav]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${NAMCHAIN_BUILD:-$root/build}"
work="${NAMCHAIN_WORK:-/tmp/nc}"
input="${1:-$work/input-f32.wav}"

daemon="$build/namchaind"
[[ -x "$daemon" ]] || { echo "no namchaind at $daemon -- build it first" >&2; exit 1; }
[[ -f "$input" ]] || { echo "no input WAV at $input" >&2; exit 1; }

mkdir -p "$work/rigs" "$work/models" "$work/irs"

fifo="$work/monitor.pcm"
[[ -p "$fifo" ]] || { rm -f "$fifo"; mkfifo -m 0600 "$fifo"; }

# A token, once, kept on disk rather than on the command line: `ps` is readable by every process
# on this machine and the daemon refuses anything under 16 characters anyway.
token="$work/token"
if [[ ! -s "$token" ]]; then
  head -c 48 /dev/urandom | base64 | tr -d '\n/+=' | head -c 32 > "$token"
fi
# Unconditionally, not just on the run that creates it: the daemon warns about a readable token and
# it is right to, and a file left over from an earlier session is exactly the one nobody rechecks.
chmod 0600 "$token"

cat <<INFO
namchaind on 127.0.0.1:47810
  input     $input (looped)
  monitor   $fifo   -- run tools/macos/listen.sh to hear it
  token     $(cat "$token")

Point Rig Designer at host 127.0.0.1, port 47810, and paste that token.
The daemon waits for a listener before it starts: start listen.sh first, or it will
report the device lost and retry until one shows up.
INFO

exec "$daemon" \
  --backend pipe \
  --capture-path "$input" \
  --playback-path "$fifo" \
  --sample-rate 48000 --period-size 64 --periods 4 \
  --capture-channels 1 --playback-channels 1 \
  --rigs-root "$work/rigs" --models-root "$work/models" --irs-root "$work/irs" \
  --socket "$work/d.sock" \
  --no-lock-memory --rt-priority 0 \
  --tcp-listen --tcp-address 127.0.0.1 --tcp-port 47810 --tcp-token-file "$token" \
  --verbose "${@:2}"
