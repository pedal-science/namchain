#!/usr/bin/env bash
# tib E5c: device arms with a MANAGED output queue.
#
# E5b ran --output-queue-periods 0 (natural occupancy: "the loop writes whenever the
# kernel has room"), which gives the playback ring no headroom on purpose. The quiet
# arm then reported 2341 driver xruns while its own deadline overruns were 2, and the
# write tail was 3.12 ms -- i.e. the xruns were queue underruns, not engine lateness.
# The project's own rule (plan-rt-piv.md section 6.8) is that a thrashing queue is a
# configuration result, not a repair measurement. So hold the queue fixed and vary only
# the housekeeping load, exactly as E5b did for the load but not for the queue.
#
# Queue config mirrors the piv shipping shape: ring 6, cap 4, floor 2.
set -u
cd "$(dirname "$0")"

SOAK="./bin-neon/namchain-soak"
COMMON=(--device hw:2,0 --capture-channels 8 --playback-channels 6
        --quantum 64 --periods 6
        --output-queue-periods 4 --output-queue-floor-periods 2
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
  EXPECT_NO_ISOLATION=1 NAMCHAIN_ARM_LABEL="tib E5c $name: SSL 8/6ch q64 ring6 cap4 floor2, 3xA2-Lite NEON, load=$load" \
    ./tools/soak/rt-manipulation-check.sh arm "bench-results/$name" \
    $SOAK "${COMMON[@]}" > "bench-results/$name.txt" 2>&1
  local rc=$?
  if [[ -n $spid ]]; then kill $spid 2>/dev/null; wait $spid 2>/dev/null; fi
  confine off
  echo "  rc=$rc"
  grep -E '^block time|^p99/deadline|^deadline overruns|^driver xruns|^max lateness|^result:|^queue control|write p99' \
    "bench-results/$name.txt" 2>/dev/null | sed 's/^/    /'
}

run_arm tib-e5c-quiet    off none
run_arm tib-e5c-vm-unconf off vm
run_arm tib-e5c-vm-conf   on  vm
echo "ALL ARMS DONE"
