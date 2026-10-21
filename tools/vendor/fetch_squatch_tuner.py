#!/usr/bin/env python3
"""Vendor squatch-dsp's tuner core into third_party/squatch-tuner/.

Squatch-dsp (https://github.com/Squatch-Stack/squatch-dsp, MIT, (c) 2026 Squatch Stack) carries a
guitar and bass tuner as header-only C++17: blocks/tuner/include/squatch/tuner/*.hpp, no
dependencies, fixed buffers, nothing allocated after configure(). nc.tuner (docs/tuner.md) is that
core behind the engine's node contract, and this script is how the copy under third_party/ is kept
honest.

The headers are **checked in**, the same way flowEQ's model table is: the engine builds without a
squatch checkout beside it and without the network, which is what every build of the pedal image
needs. This script re-fetches them from a pinned commit, writes SHA256SUMS beside them, and can
verify the tree without touching anything:

    tools/vendor/fetch_squatch_tuner.py            # update to the pin
    tools/vendor/fetch_squatch_tuner.py --check    # verify, exit 1 on a difference

A mismatch in --check is the thing to look at before a release: it means someone edited a vendored
header, or the tree was vendored from a different commit than SHA256SUMS records.

To move the pin: change COMMIT to the new squatch-dsp commit, run the script, and record the commit
in third_party/squatch-tuner/README.md. Read the diff of the headers before committing it -- the
tuner's accuracy numbers in its own RESULTS.md move with it.
"""

import argparse
import hashlib
import io
import os
import sys
import tarfile
import urllib.request

# The squatch-dsp commit the vendored headers come from. Bump deliberately, never automatically.
COMMIT = "8cb420bee1ae90db0ed64f8cdcb5884d04f4bfc0"

REPO = "Squatch-Stack/squatch-dsp"
# Where the core's include tree lives in that repository, and where it is copied to here. Everything
# below SOURCE_DIR keeps its path, so a vendored header is third_party/squatch-tuner/include/... and
# the engine's `#include <squatch/tuner/tuner.hpp>` is the upstream spelling.
SOURCE_DIR = "blocks/tuner/include"
# The include root: a vendored header is <TARGET_DIR>/squatch/tuner/<name>.hpp.
TARGET_DIR = "third_party/squatch-tuner/include"

TARBALL = f"https://codeload.github.com/{REPO}/tar.gz/{COMMIT}"
# GitHub names the archive's top directory after the repository and ref; any single top-level
# directory works, so the extraction strips whatever it finds rather than assuming the name.
SUMS_NAME = "SHA256SUMS"
SUMS_PATH = "third_party/squatch-tuner/" + SUMS_NAME


def download() -> tarfile.TarFile:
    sys.stderr.write(f"fetching {TARBALL}\n")
    with urllib.request.urlopen(TARBALL) as response:
        payload = response.read()
    return tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz")


def wanted(name: str) -> str | None:
    """The vendored-relative path for an archive member, or None when it is not part of the core."""
    parts = name.split("/")
    if len(parts) < 2:
        return None
    stripped = "/".join(parts[1:])  # drop the archive's top directory
    prefix = SOURCE_DIR + "/"
    if not stripped.startswith(prefix) or not stripped.endswith(".hpp"):
        return None
    return stripped[len(prefix):]  # e.g. "squatch/tuner/tuner.hpp"


def read_sums(path: str) -> dict[str, str]:
    sums: dict[str, str] = {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                digest, name = line.split(None, 1)
                sums[name.strip()] = digest
    except FileNotFoundError:
        pass
    return sums


def check() -> int:
    expected = read_sums(SUMS_PATH)
    if not expected:
        print(f"{SUMS_PATH} is missing or empty; run this script without --check", file=sys.stderr)
        return 1
    failures = 0
    for name in sorted(expected):
        try:
            with open(name, "rb") as f:
                actual = hashlib.sha256(f.read()).hexdigest()
        except FileNotFoundError:
            print(f"missing: {name}", file=sys.stderr)
            failures += 1
            continue
        if actual != expected[name]:
            print(f"changed: {name}\n  expected {expected[name]}\n  actual   {actual}", file=sys.stderr)
            failures += 1
    if failures:
        return 1
    print(f"ok: {len(expected)} headers match {SUMS_PATH} (squatch-dsp {COMMIT[:12]})")
    return 0


def update() -> int:
    archive = download()
    written: dict[str, str] = {}
    for member in archive.getmembers():
        if not member.isfile():
            continue
        relative = wanted(member.name)
        if relative is None:
            continue
        data = archive.extractfile(member)
        assert data is not None
        payload = data.read()
        target = f"{TARGET_DIR}/{relative}"
        written[target] = hashlib.sha256(payload).hexdigest()
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as f:
            f.write(payload)
        print(f"wrote {target} ({len(payload)} bytes)")

    if not written:
        print(f"no headers found under {SOURCE_DIR}/ in {TARBALL}", file=sys.stderr)
        return 1

    # The licence travels with the code: MIT requires the notice to be kept with the copy.
    for member in archive.getmembers():
        if member.name.split("/", 1)[-1] == "LICENSE" and member.isfile():
            data = archive.extractfile(member)
            assert data is not None
            with open("third_party/squatch-tuner/LICENSE", "wb") as f:
                f.write(data.read())
            print("wrote third_party/squatch-tuner/LICENSE")
            break
    else:
        print("no LICENSE in the archive; the vendored copy has no licence text", file=sys.stderr)
        return 1

    with open(SUMS_PATH, "w", encoding="utf-8") as f:
        f.write(
            f"# SHA-256 of each vendored header, as taken from {REPO} at {COMMIT}.\n"
            f"# Verify with: tools/vendor/fetch_squatch_tuner.py --check\n"
        )
        for name in sorted(written):
            f.write(f"{written[name]}  {name}\n")
    print(f"wrote {SUMS_PATH} ({len(written)} headers)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="verify the vendored tree against SHA256SUMS")
    args = parser.parse_args()
    return check() if args.check else update()


if __name__ == "__main__":
    sys.exit(main())
