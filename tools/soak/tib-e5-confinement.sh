#!/usr/bin/env bash
# tib E5: does cgroup-v2 cpuset confinement move the deviceless engine tail?
#
# tib has no NO_HZ_FULL and no isolcpus (deliberately: 4 cores, no spares), so
# AllowedCPUs= on the top-level slices is the ONLY isolation lever it has. Both arms
# carry the same memory load on the housekeeping cores, because the project's evidence
# says that is the load that matters.
set -u
cd "$(dirname "$0")"

SOAK="./bin-neon/namchain-soak"
COMMON=(--backend null --quantum 64 --instances 3 --model models/A2-Lite.nam
        --input-offset 1 --self-oracle --rt-priority 80 --cpu 3 --seconds 180)
HK=0-1

confine() {
  local mode=$1
  if [[ $mode == on ]]; then
    for s in system.slice user.slice; do
      sudo -n systemctl set-property --runtime "$s" AllowedCPUs=$HK >/dev/null 2>&1
    done
  else
    for s in system.slice user.slice; do
      sudo -n systemctl revert --runtime "$s" >/dev/null 2>&1
    done
  fi
  echo "  confinement=$mode  system.slice=$(cat /sys/fs/cgroup/system.slice/cpuset.cpus.effective 2>/dev/null) user.slice=$(cat /sys/fs/cgroup/user.slice/cpuset.cpus.effective 2>/dev/null)"
}

run_arm() {
  local name=$1 confined=$2
  echo "=== $name (confined=$confined) ==="
  confine "$confined"
  taskset -c $HK stress-ng --vm 1 --vm-bytes 128M --vm-keep --timeout 230s -q &
  local spid=$!
  sleep 5
  # EXPECT_NO_ISOLATION=1: tib has no isolcpus/nohz_full by design (its config fragment
  # declines both -- 4 cores, no spares). The check would otherwise fail the arm for
  # being exactly what it is. The cgroup confinement below IS the intervention here.
  EXPECT_NO_ISOLATION=1 NAMCHAIN_ARM_LABEL="tib E5 $name: 3xA2-Lite q64 null, stress-ng --vm on cpus $HK" \
    ./tools/soak/rt-manipulation-check.sh arm "bench-results/$name" \
    $SOAK "${COMMON[@]}" > "bench-results/$name.txt" 2>&1
  local rc=$?
  kill $spid 2>/dev/null; wait $spid 2>/dev/null
  confine off
  echo "  rc=$rc  -> bench-results/$name.txt"
}

run_arm tib-e5-vm-unconfined off
run_arm tib-e5-vm-confined   on
echo "ALL ARMS DONE"
