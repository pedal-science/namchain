#!/usr/bin/env bash
# Listen to what namchaind's `pipe` backend is producing, through the default output device.
#
# Attach and detach as often as you like: the daemon treats a listener leaving as a device loss and
# picks the next one up, the same way it handles an interface being unplugged. Whatever this player
# buffers is the monitoring latency, and it is also the only clock the engine has -- see the
# PipeBackend comment in src/io/backend.cpp.
set -euo pipefail

work="${NAMCHAIN_WORK:-/tmp/nc}"
fifo="${1:-$work/monitor.pcm}"
rate="${NAMCHAIN_RATE:-48000}"

[[ -p "$fifo" ]] || { echo "no FIFO at $fifo -- start run-namchaind.sh first" >&2; exit 1; }

if command -v ffplay >/dev/null 2>&1; then
  # nobuffer/low_delay keep ffplay from sitting on a second of audio before it starts.
  exec ffplay -hide_banner -loglevel warning -nodisp \
    -fflags nobuffer -flags low_delay \
    -f f32le -ar "$rate" -ch_layout mono -i "$fifo"
elif command -v play >/dev/null 2>&1; then
  exec play -q -t raw -r "$rate" -e float -b 32 -c 1 "$fifo"
else
  echo "need ffplay or sox's play: brew install ffmpeg" >&2
  exit 1
fi
