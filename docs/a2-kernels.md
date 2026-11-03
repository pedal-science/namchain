# The A2 kernels: what runs a NAM model, and which one you built

Two different pieces of code can render an A2 WaveNet model, they are chosen **at compile time**,
and they differ by a factor of three in speed. Nearly every performance number in this project is
only meaningful once you know which one was in the binary — "the Eigen build" and "the planar
build", the phrases the bench-results use, are these two.

## The two paths

| | `A2FastModel` — "the Eigen build" | the planar kernels — "the planar build" |
|---|---|---|
| Where | [`NAM/wavenet/a2_fast.cpp`](../vendor/NeuralAmpModelerCore/NAM/wavenet/a2_fast.cpp) | [`a2_planar.cpp`](../vendor/NeuralAmpModelerCore/NAM/wavenet/a2_planar.cpp) (NEON), [`a2_planar_avx2.cpp`](../vendor/NeuralAmpModelerCore/NAM/wavenet/a2_planar_avx2.cpp) |
| How it multiplies | `#include <Eigen/Dense>` — hands each layer to **Eigen's** own tuned GEMM | hand-written NEON or AVX2 intrinsics over a **planar** weight layout |
| Portability | plain C++, runs anywhere | one body per instruction set; absent on anything else |
| Who wrote it | upstream NAM, plus this fork's GEMM rework | this fork (`all-optimisations`) |
| Its role now | the **reference** the planar kernels are tested against | what ships |

**Eigen** is the C++ linear-algebra library the NAM core vendors
(`vendor/NeuralAmpModelerCore/Dependencies/eigen`). It is header-only and portable, and it
auto-vectorises to whatever the compiler is told to target — which is why one Eigen build can be
much faster than another with no source change at all (see the ladder below).

Both paths only exist for the A2 shape. `is_a2_shape()` checks an incoming WaveNet config against
the A2 signature and reports `Channels` 8 (**A2-Full**) or 3 (**A2-Lite**); anything else gets the
generic WaveNet, and neither of these is involved. The whole fast path is behind
`NAM_ENABLE_A2_FAST`, which `CMakeLists.txt` defines whenever `NAMCHAIN_A2_FAST` is on.

## Which one you get

The gate is [`a2_planar.h`](../vendor/NeuralAmpModelerCore/NAM/wavenet/a2_planar.h), and it is
preprocessor-only — **there is no runtime dispatch**:

| Target | Macro defined | Body |
|---|---|---|
| aarch64 | `NAM_A2_PLANAR` | `a2_planar.cpp`, NEON |
| armv7 **with** `__ARM_NEON` and `__ARM_FEATURE_FMA` | `NAM_A2_PLANAR`, `NAM_A2_PLANAR_A32` | `a2_planar.cpp`, ARMv7 tiles |
| x86-64 **with** `__AVX2__` and `__FMA__` | `NAM_A2_PLANAR`, `NAM_A2_PLANAR_AVX2` | `a2_planar_avx2.cpp` |
| anything else — armv7 without NEON, x86-64 without AVX2 | *none* | `A2FastModel`, i.e. Eigen |

So the path follows the **flags**, not the CPU. Two traps follow from that, and both have actually
bitten:

- **armhf's ABI baseline is `-mfpu=vfpv3-d16`, which has no NEON**, so a default armhf build emits
  no vector code at all on a board that has a NEON unit, and takes the Eigen path.
  [`tuning.md`](tuning.md) has the consequence: ship a separate `-mfpu=neon-vfpv4` artifact.
- **x86-64 had no planar path at all until core `97d7cd4`** (2026-11-02). Every x86 build before
  that was an Eigen build however it was compiled. This is why
  [pedal.project's 2026-10-31 run](../../pedal.project/bench-results/2026-10-31-cost-pro/README.md)
  measured two Eigen builds and no planar one.

`NAM_DISABLE_A2_PLANAR` forces the Eigen path anywhere, which is what makes an A/B measurement a
one-flag change.

## Checking what you built

The configure summary says, and names the instruction set:

```
  A2 fast path:    ON
  A2 planar:       ON (AVX2)
```

`NAMCHAIN_A2_PLANAR_ACTIVE` comes from a `check_cxx_source_compiles` that mirrors the header's gate
with the same flags, so it is the build's own answer rather than a guess. **Trust it only on a build
configured after 2026-11-03**: from 2026-11-02, when the core gained its AVX2 arm, until then, the
CMake check still mirrored the ARM-only gate and an x86-64 build printed `OFF` while the AVX2
kernels were compiled in and running. pro's deployed `ae6f853` was built that way — the kernels ran,
the summary denied it, and only the parity test and the timings showed otherwise.

The parity test is the other answer, and a stronger one, because it runs the code:
`tests/test_a2_planar_parity.cpp` builds both paths and compares them. It is meaningful **only in an
optimised build** — at `-O0` the reference does not contract to FMA and the comparison fails through
no fault of either side, so the test self-skips and CMake warns. `-DNAMCHAIN_REQUIRE_A2_PARITY=ON`
turns the skip into a failure.

## What the planar kernels promise, and it differs by target

- **On ARM: bit-identical** to `A2FastModel`. Same per-tap FMA order ("order A"), same result,
  every sample.
- **On AVX2: equal to an error floor**, asserted at −120 dB and measured at −133 to −134 dB. Not
  bit-identical, and the kernels do not claim to be: Eigen's AVX2 GEMM packs differently for N≥4,
  so the additions happen in a different order, and float addition is not associative.

The test asserts the property its target actually claims, which is why it had to become
target-aware when x86 gained the kernels.

## Why the distinction keeps mattering

Measured on pro (i5-9500T), one A2-Full model, p50 per copy — same source tree in the first two
rows, only the compiler's target changed:

| build | path | µs per copy | fits per core |
|---|---|---|---|
| no `-march` (x86-64 baseline, SSE2) | Eigen | 133 | 9 |
| `-march=native` (AVX2/FMA) | Eigen | 86 | 14 |
| `-march=skylake`, core `97d7cd4` | planar AVX2 | **41** | **25** |

Three times faster end to end, and roughly half of that is Eigen picking up wider registers on its
own. The numbers and their conditions live with the device, not here:
[2026-10-31](../../pedal.project/bench-results/2026-10-31-cost-pro/README.md) and
[2026-11-02](../../pedal.project/bench-results/2026-11-02-cost-table-pro/README.md).

The planar kernels win by moving more data per instruction, and that is also their weakness: they
lean harder on memory. Measured on 2026-11-03, with the other four render cores busy, the same work
costs **+86%** on the planar build against **+21%** on the Eigen one — and the cause is the shared
L3, not the clock
([2026-11-03](../../pedal.project/bench-results/2026-11-03-coexistence-pro/README.md)). An engine
built one way and sized the other way will be wrong in both directions at once, which is the reason
this page exists: **a cost figure without its build is not a cost figure.** A device's cost table
records `engine_commit` for exactly this reason.
