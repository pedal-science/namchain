#!/usr/bin/env python3
"""Extract the A2-Full and A2-Lite submodels out of a SlimmableContainer `.nam`.

The vendored core's `example_models/A2.nam` is a `SlimmableContainer` whose two
submodels are exactly the two A2 architectures we care about:

    max_value = 0.5  -> WaveNet channels=3  (A2-Lite)
    max_value = 1.0  -> WaveNet channels=8  (A2-Full)

Keeping the derived files out of the repository means `.nam` model data is never
committed by us; they are regenerated at build time from the pinned vendored core.

Usage: extract_a2_submodels.py <A2.nam> <output_dir>
"""
import json
import os
import sys

FULL_CHANNELS = 8
LITE_CHANNELS = 3


def channel_count(model: dict) -> set:
    return {layer["channels"] for layer in model["config"]["layers"] if "channels" in layer}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    src, out_dir = sys.argv[1], sys.argv[2]
    with open(src, "r", encoding="utf-8") as fh:
        doc = json.load(fh)

    if doc.get("architecture") != "SlimmableContainer":
        print(f"{src}: expected a SlimmableContainer, got {doc.get('architecture')!r}", file=sys.stderr)
        return 1

    os.makedirs(out_dir, exist_ok=True)
    written = []
    for entry in doc["config"]["submodels"]:
        model = entry["model"]
        channels = channel_count(model)
        if FULL_CHANNELS in channels:
            name = "A2-Full"
        elif LITE_CHANNELS in channels:
            name = "A2-Lite"
        else:
            print(f"{src}: submodel with channels={sorted(channels)} is not an A2 shape", file=sys.stderr)
            return 1
        path = os.path.join(out_dir, f"{name}.nam")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(model, fh)
        written.append((name, sorted(channels), entry.get("max_value"), path))

    for name, channels, max_value, path in written:
        print(f"wrote {path} (channels={channels}, max_value={max_value})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
