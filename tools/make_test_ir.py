#!/usr/bin/env python3
"""Generate a small synthetic cab IR as a NAM `Linear` model.

Used for M0 benchmarking of the IR path (`nc.cab.ir` in plan.md §6.2) so that no
third-party impulse response needs to be committed. The IR is analytic and
deterministic: a decaying two-resonance response, which is roughly what a guitar
cabinet looks like in the time domain.

Schema (NAM/linear.cpp:424-434):
    config.receptive_field : number of taps
    config.bias            : bool
    config.in_channels / out_channels, implementation : optional
    weights                : receptive_field (+1 if bias) floats

Usage: make_test_ir.py <output.nam> [taps] [sample_rate]
"""
import json
import math
import sys

DEFAULT_TAPS = 2048
DEFAULT_RATE = 48000.0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2

    out = sys.argv[1]
    taps = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_TAPS
    rate = float(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_RATE

    weights = []
    for n in range(taps):
        t = n / rate
        # Two decaying resonances plus a touch of direct sound; normalize below.
        v = (
            1.0 * math.exp(-t * 90.0) * math.sin(2 * math.pi * 110.0 * t)
            + 0.6 * math.exp(-t * 220.0) * math.sin(2 * math.pi * 2300.0 * t)
            + (1.0 if n == 0 else 0.0)
        )
        weights.append(v)

    peak = max(abs(w) for w in weights)
    weights = [w / peak for w in weights]

    doc = {
        "version": "0.5.4",
        "architecture": "Linear",
        "config": {
            "receptive_field": taps,
            "bias": False,
            "in_channels": 1,
            "out_channels": 1,
            "implementation": "auto",
        },
        "weights": weights,
    }

    with open(out, "w", encoding="utf-8") as fh:
        json.dump(doc, fh)
    print(f"wrote {out} (Linear, {taps} taps, rate={rate:g})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
