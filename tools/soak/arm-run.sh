#!/usr/bin/env bash
# arm-run.sh — run one measurement arm and leave the four artifacts every arm must leave.
#
# A soak that silently stops is indistinguishable from one that is running, and a run whose
# host state is unrecorded is not comparable to anything. The earlier rounds produced
# `.env` + `.heartbeat` + `.txt` by hand, which works but drifts; this makes the triple
# uniform and adds the fourth file the RT re-test needs — `.isolation`, the proof that the
# intervention was in force for the whole arm.
#
# What lands in bench-results/<name>.* :
#   .env        commit, start/end, kernel, isolation, governor, throttled, temp, loadavg, exit
#   .heartbeat  one timestamp a minute, written by heartbeat-run.sh, dies with the run
#   .txt        the arm's stdout (the soak's report, then the isolation verdict)
#   .isolation  before/after interrupt deltas and the tickless verdict
#   .pre/.post  the raw snapshots the verdict is computed from
#
# Usage: arm-run.sh PREFIX COMMAND [ARG...]
#        NAMCHAIN_ARM_LABEL='cap 4 / floor 2' arm-run.sh bench-results/rt-piv-h1 ...
#
# Exit: the command's own status, so a failing gate fails the arm.

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

if [[ $# -lt 2 ]]; then
  echo "usage: arm-run.sh PREFIX COMMAND [ARG...]" >&2
  exit 2
fi

prefix="$1"; shift
mkdir -p "$(dirname "$prefix")"

# The header keys keep the names the earlier .env files used, so old and new evidence reads
# the same way; the health lines are repeated at the end so a run that got hot is visible
# without opening anything else.
snap_head() {
  {
    echo "commit      $(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "start       $(date -Is)"
    echo "label       ${NAMCHAIN_ARM_LABEL:-}"
    echo "kernel      $(uname -r)"
    echo "isolated    $(cat /sys/devices/system/cpu/isolated 2>/dev/null)"
    echo "nohz_full   $(cat /sys/devices/system/cpu/nohz_full 2>/dev/null)"
    echo "governor    $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null)"
  } >> "$prefix.env"
  snap_health
}

snap_health() {
  {
    # vcgencmd needs root (/dev/vcio is 0600 root); degrade rather than alarm.
    echo "throttled   $(sudo -n vcgencmd get_throttled 2>/dev/null | sed 's/^throttled=//' || echo unavailable)"
    echo "temp        $(awk '{ printf "%.1fC", $1 / 1000 }' /sys/class/hwmon/hwmon0/temp1_input 2>/dev/null || echo unknown)"
    echo "loadavg     $(cat /proc/loadavg)"
  } >> "$prefix.env"
}

snap_head

# The arm's stdout goes to the .txt; the isolation check appends its verdict to the same
# stream, which is what makes a report readable a week later without cross-referencing files.
"$HERE/heartbeat-run.sh" "$prefix" \
  "$HERE/rt-manipulation-check.sh" arm "$prefix" "$@" > "$prefix.txt" 2>&1
status=$?

echo "end         $(date -Is)" >> "$prefix.env"
echo "exit        ${status}" >> "$prefix.env"
snap_health

exit "$status"
