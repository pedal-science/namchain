# squatch-dsp's tuner, vendored

The guitar and bass tuner from [squatch-dsp](https://github.com/Squatch-Stack/squatch-dsp), MIT,
2026 Squatch Stack. It is header-only C++17 with no dependencies: fixed-size buffers, nothing
allocated after `configure()`, so the same code runs on a Cortex-M7 and on the Core host.
`nc.tuner` (`docs/tuner.md`) is this core behind the engine's node contract.

**Pinned at** squatch-dsp `8cb420bee1ae90db0ed64f8cdcb5884d04f4bfc0` (`main`), taken from
`blocks/tuner/include/squatch/tuner/`. Every file is verbatim; nothing here is edited, and a change
belongs upstream.

| | |
|---|---|
| `include/squatch/tuner/*.hpp` | the core: `tuner.hpp` (the mono tuner), `period.hpp` (MPM/YIN), `partial_fit.hpp`, `phase_slope.hpp`, `strobe.hpp`, `level.hpp`, `decimator.hpp`, `stiffness.hpp` and the guards it is built from |
| `LICENSE` | squatch-dsp's MIT licence, which the vendored copy must carry |
| `SHA256SUMS` | one SHA-256 per header, as fetched |

The headers are **checked in**, so the engine builds without a squatch checkout beside it and
without the network — the same rule flowEQ's generated model table lives by.

## Updating

```sh
tools/vendor/fetch_squatch_tuner.py            # re-fetch from the pin, rewrite SHA256SUMS
tools/vendor/fetch_squatch_tuner.py --check    # verify this tree; exit 1 on any difference
```

To move the pin, change `COMMIT` in that script to the new squatch-dsp commit, run it, read the diff
of the headers, and update this file's commit line. The tuner's own accuracy and cost numbers live
in squatch-dsp's `blocks/tuner/RESULTS.md` and move with it; `nc.tuner`'s cost row is
`unknown.node` until `pedal-measure` measures it.

`poly.hpp` (the polyphonic strum check) is vendored for completeness and is not used by the node.

## Why not a submodule

A vendored tree that is committed is what keeps the pedal image's build hermetic
(`devices/common/image/build-namchain` builds a namchain clone in a container). A submodule would
mean the image build had another checkout to fetch, for 15 files that never change except when
someone deliberately re-pins them.
