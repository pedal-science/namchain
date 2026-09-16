#!/usr/bin/env python3
"""rt-arm-summary.py — turn a set of arm evidence files into one comparable table.

Reads the `<prefix>.txt` (the soak's report plus the isolation verdict), `<prefix>.env`
(health and commit) and `<prefix>.isolation` that `arm-run.sh` writes, and prints a table
whose columns are the ones the M1 criterion-3 question is actually decided by.

The point is not pretty formatting. It is that 20 arms of evidence otherwise get compared by
reading 20 files, and the failure mode of that is comparing the arm you just ran against the
arm you remember. Every column here is read from a file, and anything missing prints "-"
rather than a plausible default.

Usage:
    rt-arm-summary.py bench-results/rt-piv-scr-*
    rt-arm-summary.py --detail bench-results/rt-piv-h1-shipping
"""

from __future__ import annotations

import argparse
import glob
import re
import sys
from pathlib import Path


def one(pattern: str, text: str, group: int = 1, default: str = "-") -> str:
    m = re.search(pattern, text, re.MULTILINE)
    return m.group(group) if m else default


def load(prefix: Path) -> dict[str, str]:
    """Parse one arm. Missing files yield "-" fields, never an exception: a partial arm is
    still worth seeing in the table, precisely so it is not silently absent."""
    txt = prefix.with_suffix(".txt").read_text(errors="replace") if prefix.with_suffix(".txt").exists() else ""
    env = prefix.with_suffix(".env").read_text(errors="replace") if prefix.with_suffix(".env").exists() else ""
    iso = prefix.with_suffix(".isolation").read_text(errors="replace") if prefix.with_suffix(".isolation").exists() else ""

    d: dict[str, str] = {"arm": prefix.name}
    d["cpu"] = one(r"^scheduling:\s+.*?cpu (\d+)", txt)
    d["chain"] = one(r"^chain:\s+(.+?)\s*$", txt)
    d["rtf"] = one(r"\(RTF ([\d.]+)\)", txt)
    d["secs"] = one(r"^audio:\s+([\d.]+) s", txt)
    d["blocks"] = one(r"^blocks:\s+(\d+)", txt)
    d["deadline_ns"] = one(r"^deadline:\s+(\d+) ns", txt)

    # The tail — the only two clauses of criterion 3 that have ever failed.
    d["xruns"] = one(r"^driver xruns:\s+(\d+)", txt)
    d["cap_over"] = one(r"by direction:\s+(\d+) capture overruns", txt)
    d["play_under"] = one(r"capture overruns,\s+(\d+) playback underruns", txt)
    d["late"] = one(r"^deadline overruns:(\d+)", txt)
    d["blk_p50"] = one(r"^block time:\s+p50 (\d+) ns", txt)
    d["blk_p99"] = one(r"^block time:\s+p50 \d+ ns, p99 (\d+) ns", txt)
    d["blk_max"] = one(r"^block time:\s+p50 \d+ ns, p99 \d+ ns, max (\d+) ns", txt)
    d["p99_pct"] = one(r"^p99/deadline:\s+([\d.]+)%", txt)
    d["chain_p99"] = one(r"^chain p99:\s+(\d+) ns", txt)
    d["chain_max"] = one(r"^split \(tail\):.*?chain p99 \d+ max (\d+)", txt)
    d["cpu_max"] = one(r"^render path cpu:.*?cpu max (\d+) ns", txt)
    d["wall_max"] = one(r"^render path cpu:.*?wall max (\d+) ns", txt)
    d["spikes"] = one(r"cpu spike bursts:\s+(\d+) blocks over half a period", txt)
    d["spike_runs"] = one(r"cpu spike bursts:.*?in (\d+) runs", txt)
    d["wait_max"] = one(r"^device wait:.*?max (\d+) ns", txt)

    # The queue, which is the latency knob and the place a stall is absorbed.
    d["q_target"] = one(r"^queue target:\s+(\d+) periods", txt)
    d["q_floor"] = one(r"^queue floor:\s+(\d+) periods", txt)
    d["q_p1"] = one(r"p1 ([\d.]+), p50 [\d.]+ periods", txt)
    d["q_p50"] = one(r"p1 [\d.]+ p50 ([\d.]+) periods", txt.replace(",", ""))
    d["q_below"] = one(r"(\d+) of \d+ blocks held less than one", txt)
    d["drops"] = one(r"^queue control:\s+(\d+) output drops", txt)
    d["capture_bursts"] = one(r"output drops.*?,\s+(\d+) capture bursts", txt)
    d["floor_ins"] = one(r"held by inserting silence (\d+) times", txt)
    d["floor_frames"] = one(r"inserting silence \d+ times \((\d+) frames", txt)
    d["splice_armed"] = one(r"^splice repair:.*?armed (\d+) times", txt)

    d["alloc"] = one(r"render-path alloc:(\d+) allocations", txt)
    d["oracle_diff"] = one(r"max\|diff\|:\s+(\S+) over", txt)
    d["result"] = one(r"^result:\s+(\w+)", txt)
    d["throttled"] = one(r"^throttled\s+(\S+)", env)
    d["temp"] = one(r"^temp\s+(\S+)", env)
    # The isolation verdict, per reserved core. A missing verdict is itself a finding.
    d["tick_c2"] = one(r"cpu 2 took \d+ arch_timer interrupts in \d+s \(([\d.]+)/s\)", iso)
    d["tick_c3"] = one(r"cpu 3 took \d+ arch_timer interrupts in \d+s \(([\d.]+)/s\)", iso)
    d["iso_fail"] = "yes" if "FAIL:" in iso else ("-" if not iso else "no")
    d["audio_irq"] = one(r"audio IRQ \((\d+)\) was delivered (\d+) times", iso, 2)
    return d


HEAD = [
    ("arm", "arm"),
    ("cpu", "cpu"),
    ("xruns", "xruns"),
    ("cap/play", "cap/play"),
    ("late", "late blks"),
    ("blk_p50", "blk p50 us"),
    ("blk_p99", "blk p99 us"),
    ("blk_max", "blk max us"),
    ("p99_pct", "p99 %"),
    ("cpu_max", "cpu max us"),
    ("wall_max", "wall max us"),
    ("spikes", "spike blks"),
    ("drops", "drops"),
    ("floor_ins", "floor ins"),
    ("tick", "tick c2/c3 /s"),
    ("throttled", "throttled"),
    ("result", "result"),
]

DETAIL_KEYS = [
    "arm", "cpu", "chain", "secs", "blocks", "deadline_ns", "rtf", "result",
    "xruns", "cap_over", "play_under", "late", "blk_p50", "blk_p99", "blk_max",
    "p99_pct", "chain_p99", "chain_max", "cpu_max", "wall_max", "wait_max",
    "spikes", "spike_runs", "q_target", "q_floor", "q_p1", "q_p50", "q_below",
    "drops", "capture_bursts", "floor_ins", "floor_frames", "splice_armed",
    "alloc", "oracle_diff", "tick_c2", "tick_c3", "iso_fail", "audio_irq",
    "throttled", "temp",
]


def us(v: str) -> str:
    try:
        return f"{int(v) / 1000:.0f}"
    except ValueError:
        return "-"


def cell(d: dict[str, str], key: str) -> str:
    if key == "cap/play":
        return f"{d.get('cap_over', '-')}/{d.get('play_under', '-')}"
    if key == "tick":
        return f"{d.get('tick_c2', '-')}/{d.get('tick_c3', '-')}"
    if key in ("blk_p50", "blk_p99", "blk_max", "cpu_max", "wall_max"):
        return us(d.get(key, "-"))
    return d.get(key, "-")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prefixes", nargs="+", help="arm prefixes (glob is expanded by the shell)")
    ap.add_argument("--detail", action="store_true", help="key: value dump per arm instead of a table")
    args = ap.parse_args(argv)

    prefixes: list[Path] = []
    for pat in args.prefixes:
        hits = sorted(Path(p).with_suffix("") for p in glob.glob(pat))
        prefixes.extend(hits if hits else [Path(pat).with_suffix("")])
    rows = [load(p) for p in prefixes]
    if not rows:
        print("no arms matched", file=sys.stderr)
        return 2

    if args.detail:
        for d in rows:
            print(f"--- {d['arm']}")
            for k in DETAIL_KEYS:
                print(f"    {k:16} {d.get(k, '-')}")
        return 0

    widths = []
    for key, title in HEAD:
        widths.append(max(len(title), *(len(cell(d, key)) for d in rows)))
    print("| " + " | ".join(t.ljust(w) for (_, t), w in zip(HEAD, widths)) + " |")
    print("|" + "|".join("-" * (w + 2) for w in widths) + "|")
    for d in rows:
        print("| " + " | ".join(cell(d, k).ljust(w) for (k, _), w in zip(HEAD, widths)) + " |")
    # Anything that looks like a broken arm is called out rather than left for the reader to
    # notice: a missing isolation verdict or a non-zero allocation count invalidates the arm.
    for d in rows:
        if d.get("iso_fail") == "yes":
            print(f"!! {d['arm']}: isolation verdict FAILED -- discard this arm's numbers", file=sys.stderr)
        if d.get("alloc", "0") != "0":
            print(f"!! {d['arm']}: {d['alloc']} render-path allocations -- engine bug, not a host result",
                  file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
