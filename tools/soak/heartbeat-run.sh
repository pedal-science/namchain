#!/usr/bin/env bash
#
# Run a command with a per-minute heartbeat file that dies with the run.
#
# A soak that silently stops is indistinguishable from one that is running, and a lost hour is
# indistinguishable from an hour that was never spent -- so the heartbeat is the evidence. It has to
# be *this* file rather than an inline one-liner because the obvious one-liner is wrong: if the
# `$!` capture shares a line with the next command, the assignment goes into that command's
# environment instead of the shell's, `kill` receives no argument, does nothing, and the writer
# appends to the evidence file for ever. That happened on 2026-09-14: 975 timestamps after a soak
# that had finished 17 hours earlier (plan.md section 0). A `trap` on EXIT cannot be skipped that way.
#
# Usage: heartbeat-run.sh OUTPUT_PREFIX COMMAND [ARG...]
#   Writes OUTPUT_PREFIX.heartbeat, and leaves the exit status of COMMAND in $?.
set -u

if [ "$#" -lt 2 ]; then
  echo "usage: $0 OUTPUT_PREFIX COMMAND [ARG...]" >&2
  exit 2
fi

prefix="$1"
shift

: > "${prefix}.heartbeat"
echo "start       $(date -Is)" >> "${prefix}.heartbeat"
echo "command     $*" >> "${prefix}.heartbeat"

(
  while true; do
    date -Is >> "${prefix}.heartbeat"
    sleep 60
  done
) &
heartbeat_pid=$!

# One handler for every way this script can end, including Ctrl-C and a signal from a monitor.
trap 'kill "${heartbeat_pid}" 2>/dev/null || true; wait "${heartbeat_pid}" 2>/dev/null || true' \
  EXIT INT TERM

"$@"
status=$?

echo "end         $(date -Is)" >> "${prefix}.heartbeat"
echo "exit        ${status}" >> "${prefix}.heartbeat"
exit "${status}"
