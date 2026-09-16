#!/usr/bin/env bash
# tib-which-port.sh — say which USB controller the SSL 2+ landed on, and whether it can work.
#
# On tib the answer decides everything, because the four USB-A sockets are not equivalent:
#
#   Bus 003  ehci-platform   <- the port that works. Isochronous playback holds its ring.
#   Bus 001  dwc2 -> on-board Genesys hub (4 ports)  <- underruns continuously, even with no
#            DSP in the path (739 underruns in 60 s of pure passthrough, one period lost each).
#   Bus 002  dwc2 OTG        <- the micro-USB. It is a *device* port (gadget mode); a host
#            device will never appear here.
#
# Measured 2026-09-16: SSL on bus 1 = 739-1692 underruns; Realtek audio on bus 3 = 0 underruns
# and RTF 1.0000 from the same binary, same config.
#
# Usage: tib-which-port.sh          (run on tib)
set -u

VID=31e9
PID=0009

found=0
for d in /sys/bus/usb/devices/*/; do
  [ -f "$d/idVendor" ] || continue
  [ "$(cat "$d/idVendor" 2>/dev/null)" = "$VID" ] || continue
  [ "$(cat "$d/idProduct" 2>/dev/null)" = "$PID" ] || continue
  found=1
  path=$(basename "$d")
  bus=$(cat "$d/busnum" 2>/dev/null)
  speed=$(cat "$d/speed" 2>/dev/null)
  echo "SSL 2+ Mk II found at $path  (bus $bus, speed $speed)"
  case "$bus" in
    3) echo "  *** THIS IS THE GOOD PORT (EHCI). Run the soak here. ***" ;;
    1) echo "  This is bus 1: dwc2 behind the on-board hub. It will underrun -- measured"
       echo "  739 times in 60 s of passthrough. Try another socket." ;;
    2) echo "  This is bus 2, the micro-USB OTG port. It is a device port, not a host port;" 
       echo "  it cannot drive an interface. Use a Type-A socket." ;;
    *) echo "  Unexpected bus $bus -- record it." ;;
  esac
done

if [ "$found" = 0 ]; then
  cat <<'MSG'
No SSL 2+ found at all. Before trying another socket, reset the on-board hub -- a port that
has just changed device stays latched off until it is reset, and a reboot does NOT clear it:

    sudo usbreset 05e3:0610      # or: ~/namchain/tools/soak/tib-usb-recover.sh

Then run this again.
MSG
  exit 1
fi
