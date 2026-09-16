#!/usr/bin/env bash
# tib-usb-recover.sh — bring tib's USB-A ports back after a power-protection latch.
#
# THE FAILURE, AND WHY IT IS CONFUSING
#
# tib's four USB-A ports hang off ONE on-board hub (Genesys 05e3:0610) on bus 1. That hub's
# descriptor reads wHubCharacteristic 0x0008: **ganged power switching with per-port
# over-current protection**. Ganged means one switch feeds all four ports, so one electrical
# event on one port takes every port, and the switch latches off.
#
# **A warm reboot does not clear it.** That is the part worth writing down, because it makes
# the symptom look like a dead board: `reboot` never removes VBUS, so the hub's power switch
# keeps its latched state, the hub re-probes reporting its ports as it left them, and the
# machine comes back with four dead ports and *no error anywhere*. `lsusb` shows exactly what
# it showed before, dmesg says nothing, and nothing plugged in enumerates -- not the device
# you were testing, and not a mouse either, because the ports have no VBUS and a device with
# no power never pulls up D+, so the host never even sees an attach attempt. A failed
# connection logs "device descriptor read/64, error -71"; dead port power logs nothing at all,
# and that difference is the whole diagnosis.
#
# WHAT CLEARS IT
#
#   * a device reset of the hub (USBDEVFS_RESET), which is what this script issues, or
#   * a cold power cycle -- removing VBUS resets the hub's own logic. Removing the power lead,
#     not `reboot`.
#
# Measured on tib 2026-09-16: the SSL 2+ Mk II's inrush on a bare port took the board down;
# after the warm reboot all four ports were dead and nothing enumerated; this reset brought
# them back, and a card reader behind an external hub enumerated immediately at 480 Mbit/s
# and read data at 2 MB/s. The lesson for the SSL is to put it behind a *powered* hub, so its
# inrush never reaches the board's rail in the first place.
#
# Usage: tib-usb-recover.sh [VID:PID]     (default 05e3:0610, the on-board hub)
# Exit: 0 reset issued, 2 could not find the hub, 3 the ioctl failed.

set -u

VIDPID="${1:-05e3:0610}"
vid="${VIDPID%%:*}"
pid="${VIDPID##*:}"

# Find the bus and device numbers from the vendor/product ids rather than trusting a number:
# bus and device numbers are reassigned on every reboot, and resetting the wrong device on a
# board whose storage can be USB would be a poor trade for a convenience script.
line=$(lsusb | awk -v v="$vid" -v p="$pid" '
  { bus=$2; dev=$4; sub(":", "", dev); id=$6; sub(":", "", id);
    if (tolower(id) == tolower(v p)) { print bus, dev; exit } }')

if [[ -z $line ]]; then
  echo "tib-usb-recover: no device $VIDPID found." >&2
  echo "  If the ports are dead, the on-board hub should still be listed -- check 'lsusb'." >&2
  echo "  If it is genuinely absent, this is a cold power cycle, not a reset." >&2
  exit 2
fi

bus=${line% *}
dev=${line#* }
node="/dev/bus/usb/$(printf '%03d' "$((10#$bus))")/$(printf '%03d' "$((10#$dev))")"

echo "tib-usb-recover: resetting $VIDPID at bus $bus device $dev ($node)"
sudo python3 - "$node" <<'PY'
import fcntl, os, sys
USBDEVFS_RESET = 0x5514
node = sys.argv[1]
try:
    fd = os.open(node, os.O_RDWR)
except OSError as e:
    print(f"tib-usb-recover: cannot open {node}: {e.strerror}", file=sys.stderr)
    sys.exit(3)
try:
    fcntl.ioctl(fd, USBDEVFS_RESET, 0)
finally:
    os.close(fd)
PY
rc=$?
[[ $rc -eq 0 ]] || exit 3

# The reset re-enumerates the hub; anything already plugged into it appears now if the ports
# were the problem, which is itself the test of whether this was the fault.
sleep 3
echo "tib-usb-recover: hub reset issued. Devices now:"
lsusb
echo
echo "If a device you had plugged in has appeared, the ports had latched off -- put that"
echo "device behind a powered hub before retrying, or it can trip the same switch again."
