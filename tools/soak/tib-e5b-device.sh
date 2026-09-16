#!/usr/bin/env bash
# tib E5b: the real device arms, now that the SSL 2+ Mk II is attached to tib.
#
# Three arms at 48 kHz / q64 / 3 x A2-Lite on the NEON build, all pinned to cpu 3:
#   quiet      no housekeeping load                     (the device reference)
#   vm-unconf  128 MB vm worker on cpus 0-1, no confinement
#   vm-conf    128 MB vm worker on cpus 0-1, system.slice+user.slice AllowedCPUs=0-1
#
# The SSL exposes 8 capture / 6 playback channels and S32_LE only; other channel
# counts are refused by SNDRV_PCM_IOCTL_HW_REFINE. Keep --output-queue-periods 0 so
# the run reports its natural occupancy (the floor was measured on piv, not here).
set -u
cd "$(dirname "$0")"

SOAK="./bin-neon/namchain-soak"
COMMON=(--device hw:2,0 --capture-channels 8 --playback-channels 6
        --quantum 64 --periods 4 --output-queue-periods 0
        --instances 3 --model models/A2-Lite.nam
        --rt-priority 80 --cpu 3 --seconds 180)
HK=0-1

confine() {
  local mode=$1 s
  for s in system.slice user.slice; do
    if [[ $mode == on ]]; then
      sudo -n systemctl set-property --runtime "$s" AllowedCPUs=$HK >/dev/null 2>&1
    else
      sudo -n systemctl revert --runtime "$s" >/dev/null 2>&1
    fi
  done
  echo "  confinement=$mode  system.slice=$(cat /sys/fs/cgroup/system.slice/cpuset.cpus.effective 2>/dev/null) user.slice=$(cat /sys/fs/cgroup/user.slice/cpuset.cpus.effective 2>/dev/null)"
}

run_arm() {
  local name=$1 confined=$2 load=$3 spid=""
  echo "=== $name (confined=$confined load=$load) ==="
  confine "$confined"
  if [[ $load == vm ]]; then
    taskset -c $HK stress-ng --vm 1 --vm-bytes 128M --vm-keep --timeout 230s -q &
    spid=$!
    sleep 5
  fi
  EXPECT_NO_ISOLATION=1 NAMCHAIN_ARM_LABEL="tib E5b $name: SSL hw:2,0 8/6ch q64, 3xA2-Lite NEON, load=$load" \
    ./tools/soak/rt-manipulation-check.sh arm "bench-results/$name" \
    $SOAK "${COMMON[@]}" > "bench-results/$name.txt" 2>&1
  local rc=$?
  if [[ -n $spid ]]; then kill $spid 2>/dev/null; wait $spid 2>/dev/null; fi
  confine off
  echo "  rc=$rc"
  grep -E '^block time|^p99/deadline|^deadline overruns|^driver xruns|^max lateness|^result:' \
    "bench-results/$name.txt" 2>/dev/null | sed 's/^/    /'
}

run_arm tib-e5b-quiet    off none
run_arm tib-e5b-vm-unconf off vm
run_arm tib-e5b-vm-conf   on  vm
echo "ALL ARMS DONE"
