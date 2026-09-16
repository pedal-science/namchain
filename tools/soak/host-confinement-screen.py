#!/usr/bin/env python3
"""E1 screen: does cgroup v2 cpuset confinement of the housekeeping side move the tail?

This is the experiment the prior research named and could not run without root.
It measures the same hypothesis the project already established for the engine
("other processes' memory access, not their CPU use, is what breaks the tail"),
using cyclictest on the audio cores instead of the audio device.

Arms (each DURATION seconds):
  quiet-unconfined   no load on the housekeeping cores, no confinement
  vm-unconfined      stress-ng --vm on cpus 0-1, no confinement
  vm-confined        stress-ng --vm on cpus 0-1, system.slice+user.slice AllowedCPUs=0-1
  cpu-unconfined     stress-ng --cpu on cpus 0-1, no confinement   (control)

Output: one JSON file with every arm's samples, IRQ deltas and host state.
Nothing here is persistent; the cgroup property is --runtime and rolled back.
"""

import argparse
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time

AUDIO_CPUS = "2-3"          # the isolated cores on piv
HK_CPUS = "0-1"             # housekeeping
CYCLIC_PRIO = 80            # below the audio IRQ thread at 85, as the soak runs


def sh(cmd, check=False, timeout=None):
    p = subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True,
                       text=True, timeout=timeout)
    if check and p.returncode != 0:
        raise RuntimeError(f"{cmd!r} failed rc={p.returncode}: {p.stderr.strip()}")
    return p


def cpu_irq_snapshot():
    """Total interrupt count per cpu from /proc/interrupts header width."""
    out = sh(["cat", "/proc/interrupts"]).stdout.splitlines()
    ncpu = len(out[0].split())
    totals = [0] * ncpu
    for line in out[1:]:
        parts = line.split()
        if len(parts) < ncpu + 2 or not parts[0].endswith(":"):
            continue
        for i in range(ncpu):
            try:
                totals[i] += int(parts[1 + i])
            except (ValueError, IndexError):
                pass
    # arch_timer specifically: the tick counter, the thing nohz_full removes
    timer = [0] * ncpu
    for line in out[1:]:
        parts = line.split()
        if parts and parts[-1].startswith("arch_timer"):
            for i in range(ncpu):
                try:
                    timer[i] = int(parts[1 + i])
                except (ValueError, IndexError):
                    pass
    return totals, timer


def confine(enable):
    slices = ["system.slice", "user.slice"]
    if enable:
        for s in slices:
            sh(["sudo", "-n", "systemctl", "set-property", "--runtime", s,
                "AllowedCPUs=" + HK_CPUS], check=True)
    else:
        for s in slices:
            sh(["sudo", "-n", "systemctl", "revert", "--runtime", s])


def cpuset_effective(slice_name):
    try:
        with open(f"/sys/fs/cgroup/{slice_name}/cpuset.cpus.effective") as fh:
            return fh.read().strip()
    except OSError:
        return "(absent)"


def run_arm(name, duration, load, confined, outdir):
    print(f"\n=== {name}: load={load} confined={confined} duration={duration}s ===",
          flush=True)
    if confined:
        confine(True)
    else:
        confine(False)

    # housekeeping load, pinned to 0-1
    stress = None
    if load == "vm":
        stress = subprocess.Popen(
            ["taskset", "-c", HK_CPUS, "stress-ng", "--vm", "1", "--vm-bytes", "128M",
             "--vm-keep", "--timeout", f"{duration + 20}s", "-q"])
    elif load == "cpu":
        stress = subprocess.Popen(
            ["taskset", "-c", HK_CPUS, "stress-ng", "--cpu", "2",
             "--timeout", f"{duration + 20}s", "-q"])

    time.sleep(5)  # let the load reach steady state before measuring

    irq_before, timer_before = cpu_irq_snapshot()
    json_path = os.path.join(outdir, f"cyc-{name}.json")
    cyc = sh(["cyclictest", "-a", AUDIO_CPUS, "-t2", f"-p{CYCLIC_PRIO}", "-i200",
              "-m", "-h400", "-D", f"{duration}s", "-q", f"--json={json_path}"])
    time.sleep(2)
    irq_after, timer_after = cpu_irq_snapshot()

    if stress:
        stress.terminate()
        try:
            stress.wait(timeout=15)
        except subprocess.TimeoutExpired:
            stress.kill()

    result = {"arm": name, "load": load, "confined": confined,
              "duration_s": duration, "returncode": cyc.returncode,
              "stderr": cyc.stderr.strip()[:500],
              "cpuset": {s: cpuset_effective(s)
                         for s in ("system.slice", "user.slice", "init.scope")},
              "irq_total_delta": [a - b for a, b in zip(irq_after, irq_before)],
              "arch_timer_delta": [a - b for a, b in zip(timer_after, timer_before)],
              "threads": []}

    if os.path.exists(json_path):
        with open(json_path) as fh:
            data = json.load(fh)
        # cyclictest V2.80 emits "thread" as an object keyed by index, not a list.
        raw_threads = data.get("thread", data.get("threads", []))
        if isinstance(raw_threads, dict):
            raw_threads = [raw_threads[k] for k in sorted(raw_threads, key=lambda x: int(x))]
        for th in raw_threads:
            hist = th.get("histogram", {})
            vals = {int(k): v for k, v in hist.items() if str(k).lstrip("-").isdigit()}
            total = sum(vals.values()) or 1

            def pct(p):
                acc = 0
                for us in sorted(vals):
                    acc += vals[us]
                    if acc / total >= p:
                        return us
                return max(vals) if vals else None

            result["threads"].append({
                "cpu": th.get("cpu"),
                "min": th.get("min"), "avg": th.get("avg"), "max": th.get("max"),
                "p99": pct(0.99), "p999": pct(0.999), "samples": total,
            })
        os.remove(json_path)

    if contained := [t for t in result["threads"] if t["max"] is not None]:
        result["max_of_max"] = max(t["max"] for t in contained)
    print("  max_of_max =", result.get("max_of_max"),
          " threads:", json.dumps(result["threads"]), flush=True)
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=int, default=120)
    ap.add_argument("--out", required=True)
    ap.add_argument("--arms", default="quiet-unconfined,vm-unconfined,vm-confined,cpu-unconfined")
    args = ap.parse_args()

    plan = {
        "quiet-unconfined": ("quiet", False),
        "vm-unconfined": ("vm", False),
        "vm-confined": ("vm", True),
        "cpu-unconfined": ("cpu", False),
    }
    outdir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(outdir, exist_ok=True)

    evidence = {
        "schema": "namchain-host-confinement-screen/1",
        "host": sh(["uname", "-a"]).stdout.strip(),
        "cmdline": sh(["cat", "/proc/cmdline"]).stdout.strip(),
        "isolated": sh(["cat", "/sys/devices/system/cpu/isolated"]).stdout.strip(),
        "nohz_full": sh(["cat", "/sys/devices/system/cpu/nohz_full"]).stdout.strip(),
        "subtree_control": sh(["cat", "/sys/fs/cgroup/cgroup.subtree_control"]).stdout.strip(),
        "vcgencmd_throttled": sh(["sudo", "-n", "vcgencmd", "get_throttled"]).stdout.strip(),
        "arms": [],
    }

    try:
        for arm in args.arms.split(","):
            load, confined = plan[arm]
            evidence["arms"].append(run_arm(arm, args.duration, load, confined, outdir))
    finally:
        confine(False)
        print("\n(all confinement rolled back)", flush=True)

    evidence["after_rollback"] = {
        s: cpuset_effective(s) for s in ("system.slice", "user.slice", "init.scope")}
    with open(args.out, "w") as fh:
        json.dump(evidence, fh, indent=2)
    print("wrote", args.out)

    print("\n%-18s %8s %8s %8s   timer-delta(2,3)" % ("arm", "max", "p999", "p99"))
    for a in evidence["arms"]:
        ths = [t for t in a["threads"] if t["max"] is not None]
        if not ths:
            print(f"{a['arm']:<18} {'NO DATA':>8}  {a['stderr'][:60]}")
            continue
        print("%-18s %8s %8s %8s   %s" % (
            a["arm"],
            max(t["max"] for t in ths),
            max(t["p999"] or 0 for t in ths),
            max(t["p99"] or 0 for t in ths),
            a["arch_timer_delta"][2:4]))


if __name__ == "__main__":
    sys.exit(main())
