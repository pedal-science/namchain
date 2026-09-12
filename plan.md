# namchain — a glitch-free, headless NAM signal-chain engine

**Status:** approved and in progress. Phase 0 is done and M0 has started (§0, §12) — see §11 for the
first *measured* numbers, which replace the previous estimates.
**Author:** planning session (research grounded in the sources cited in §2 and §16), resumed for
implementation.
**Updated:** 2026-09-11, after the DSH environment was repaired and verified on the target host.

---

## How to resume

**Environment verified and ready (2026-09-11).** Phase 0 is complete on this host and M0 has begun.
§0 records the verified inventory, the corrections to what was previously believed about this box,
and the commands that actually work.

```sh
echo ok; uname -m; nproc          # ok / aarch64 / 4
cmake --version; ninja --version  # 3.28.3 / 1.11.1
```

**Toolchain that had to be installed** (cmake and clang were already present, contrary to the earlier
note):

```sh
sudo apt-get install -y ninja-build pkg-config libpipewire-0.3-dev libasound2-dev clang-20
```

`clang-20` is needed for RealtimeSanitizer (§4.2, §10.4): Ubuntu 24.04's default clang 18 rejects
`-fsanitize=realtime`. `libeigen3-dev` is *not* needed — Eigen is vendored via the core's submodule.

**RT privileges are configured** (they were entirely absent before):
`/etc/security/limits.d/99-namchain-audio.conf` grants `@audio` `rtprio 95` and `memlock unlimited`,
and `namchain-cpu-performance.service` sets the `performance` governor and disables USB autosuspend
at boot. Verified both through a fresh login session and through a transient systemd service with
`LimitRTPRIO=95` — the path `namchaind` will use. A harness-spawned shell that predates the limits
file still cannot `chrt`, so RT tests must run via a login session or a systemd unit.

**Vendored NAM core:** `vendor/NeuralAmpModelerCore` as a git submodule pinned at G2, with
`Dependencies/eigen` and `Dependencies/AudioDSPTools` initialised recursively.

**M0 artifacts already produced on this host:**

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/namchain-tests                                             # 11 cases, all passing
./build/namchain-bench build/generated/A2-Full.nam --quantum 32,64,128,256
```

Four corrections to the commands previously recorded here:

1. **`-DNAM_SAMPLE_FLOAT=ON` is not a CMake option of the core.** `NAM_SAMPLE_FLOAT` is only an
   `#ifdef` in `NAM/dsp.h`; CMake ignores the unused variable with a warning, so that configure line
   silently built the *double* path. Use our `-DNAMCHAIN_SAMPLE_FLOAT=ON`, which supplies it as a
   compile definition. (The core's own bench tools also hardcode `double` buffers, so they cannot
   measure the float path at all — `namchain-bench` can.)
2. The core's `benchmodel` hardcodes a 64-sample block; `benchmodel_bufsize` takes a buffer size.
   `namchain-bench` does the sweep and reports p50/p99/max, RTF and % of deadline.
3. `A2.nam` in the core's examples is a **`SlimmableContainer`**, not a plain WaveNet, so it must be
   split before `A2-Full`/`A2-Lite` can be measured separately. `tools/extract_a2_submodels.py` does
   this and runs automatically at build time; no model data is committed.
4. The core must be built as an **object** library, not a static one: its architectures register
   through file-scope static initializers that a static archive discards, giving
   `No config parser registered for architecture: WaveNet` at runtime.

**First three tasks, in this order:** M0 (in progress: repo/CMake/CI, pinned core, bench harness and
the measured RTF table are done; `namchain-render` is next), then M1, then M2. The instruction to
write the glitch detector *before* the transition machinery (§10.3, M2) stands unchanged.

---

## 0. Phase 0 — DONE (verified 2026-09-11)

**Original blocker: fixed, but the diagnosis here was wrong.** Every `bash` call used to fail with
`spawn bash ENOENT`. That symptom is gone, and the real fault was never `PATH`: under the
`workspace-write` file policy this host has **no usable sandbox backend**, so no command can be
launched at all. `bwrap` *is* present (`/usr/bin/bwrap`), but AppArmor blocks unprivileged user
namespaces (`kernel.apparmor_restrict_unprivileged_userns=1`), and the DSH Landlock helper
(`landlock-run`) is not installed anywhere on the box. Running the session under `danger-full-access`
makes the shell work. **If the file policy reverts to `workspace-write`, shell execution breaks
again** — the fix is the policy (or a working sandbox helper), not the spawn environment.

**The recorded inventory was wrong in several places**, because it was inferred from package paths
with no shell available. Corrected by direct measurement:

| Previously recorded | Actually is |
|---|---|
| Debian 12, container, pid 1 = `tini` | **Ubuntu 24.04.4 LTS, bare metal, pid 1 = `systemd`** |
| Raspberry Pi 5 | **Raspberry Pi 500 Rev 1.0** (same BCM2712/Cortex-A76 SoC class as the Pi 5) |
| `cmake` missing, `gcc-12`, `pkg-config` present | **cmake 3.28.3 and clang 18 present; gcc/g++ 13.3.0; `pkg-config` was missing** |
| No audio device, `/proc/asound` absent | **`/proc/asound` is present**: Loopback, Dummy, 2× vc4hdmi |
| CPU possibly cgroup-limited | 4× Cortex-A76 @ 2.4 GHz, 7.8 GB RAM, 42 GB free, 40 °C, `throttled=0x0` |

- **Audio, precisely:** four cards exist, but **none is capture-capable**. `snd-aloop` and `snd-dummy`
  are virtual, and the two `vc4hdmi` cards are playback-only (`MAI PCM i2s-hifi`). So the earlier
  *conclusion* holds — this is the offline DSP/benchmark/CI host — but it is **bare metal**, so
  attaching a single USB interface makes it a full real-time validation host. That is materially
  better than "run implementation on another machine".
- **Installed/verified toolchain:** gcc/g++ 13.3.0, clang 18.1.3, clang-20 20.1.2, cmake 3.28.3,
  ninja 1.11.1, pkg-config 1.8.1, libpipewire-0.3-dev 1.0.5, libasound2-dev 1.2.11, git 2.43.0,
  python3 3.12.3, doctest 2.4.11 (vendored header).
- **`/usr/include/sound/asound.h` is present**, so the no-libasound `alsa_raw` backend (§4.3) has the
  kernel UAPI headers it needs; `asoundef.h` is absent and not required.
- **Kernel is `6.8.0-1064-raspi` (PREEMPT_DYNAMIC), not PREEMPT_RT**; `/sys/kernel/realtime` is absent.
- **Network, apt and passwordless sudo all work**, and the G2 pin `2563c0fd…` is still identical to
  upstream `HEAD` — no drift to reconcile.

**Phase 0 exit criteria — all met:** `cmake --version` ✔, `ninja --version` ✔, a C++20 "hello world"
aarch64 build via both Make and Ninja ✔, `libpipewire-0.3-dev` installs ✔.

---

## 1. Goal and success criteria

**Goal.** A headless, embeddable engine (`libnamchain`), a reference daemon (`namchaind`), and an
offline renderer (`namchain-render`) that together: host N parallel chains as DAGs of pluggable
nodes; apply declarative graph/param edits with no sample gaps and no waveform discontinuities; keep
CPU use proportional to what is audible plus what must stay switch-ready; and make NAM (A2) model
switching genuinely seamless via state-accurate warm starts rather than crossfades alone.

**Measurable success criteria** (each verified by an automated test, §10):
1. **Zero-glitch edits.** The torture render (§10.3): 60 s of hard test signal with 500 randomised
   edits (param, bypass, model swap, rewire, scene recall) passes all four glitch metrics at quantum
   sizes 32/64/128/256 and rates 44.1/48/96 kHz.
2. **State-accurate NAM switching.** Warm-started instance output vs. an always-on instance differs
   by < −60 dBFS after the switch point; switch-in produces no discontinuity above the idle-signal
   second-difference bound.
3. **Throughput (Pi 5, bare metal, 48 kHz, 64-sample quantum).** ≥ 4 concurrent A2-Full (8-channel)
   **or** ≥ 12 concurrent A2-Lite (3-channel) audible NAM models with p99 block time ≤ 50 % of the
   1.33 ms deadline and zero xruns over 1 h.
4. **Resident/switchable set.** ≥ 16 loaded model instances switchable with p99 ready-time ≤ 5 ms and
   zero click; ≥ 64 resident instances where RAM allows (RAM is not the binding constraint;
   **warming CPU is** — §7.5).
5. **RT safety, enforced not asserted.** Zero heap allocation, zero locks, zero syscalls in the render
   path, proven by RealtimeSanitizer in CI plus a global `new`/`free`/pthread interposer in tests.
6. **Determinism.** Same graph + same input + same quantum ⇒ bit-identical output across runs.
7. **Availability.** Survives device unplug/replug, malformed `.nam` files, malformed control
   messages, NaN-producing third-party nodes, and CPU overload without crashing and without ever
   emitting silence where signal is expected; degradation is always crossfaded and reported.
8. **Honest capability reporting.** Every model instance exposes `rtf` (measured real-time factor),
   `lookback`, `warm_through`, `estimated_switch_ms`, so hosts can display readiness instead of
   guessing.

**Out of scope for this plan** (see §15): any UI, MIDI/USB-host control, VST3/LV2 hosting, model
training, network audio, iOS app shell.

---

## 2. Verified grounding (facts this plan depends on)

| # | Fact | Source |
|---|---|---|
| G1 | NAM core is **MIT** (`Copyright (c) 2023 Steven Atkinson`); C++20; vendored Eigen + nlohmann/json as submodules; version 0.5.5 | `LICENSE`, `CMakeLists.txt`, `NAM/version.h` |
| G2 | Pin: `sdatkinson/NeuralAmpModelerCore@2563c0fd4cb1f9ce457d89a761738ea15097e1f3` (main, 2026-08-23) | GitHub commits API |
| G3 | DSP contract: `process(NAM_SAMPLE** in, NAM_SAMPLE** out, int frames)`, `input[ch][frame]`; `Reset(sampleRate, maxBufferSize)` **must** precede `process`; `WaveNet::process` asserts `frames <= maxBufferSize`; `A2FastModel::process` **silently re-sizes (allocates)** if exceeded | `NAM/dsp.h`, `NAM/wavenet/model.cpp`, `NAM/wavenet/a2_fast.cpp` |
| G4 | `NAM_SAMPLE` is `double` unless `NAM_SAMPLE_FLOAT` is defined | `NAM/dsp.h` |
| G5 | WaveNet/A2 is **strictly causal, feed-forward, finite-memory**: `output[n] = f(input[n−RF+1..n])`. No recurrence; gating/BLENDED and FiLM are memoryless per-frame maps. RF = `Σ dilationᵢ(kernelᵢ−1) + head_dilation(head_kernel−1)` | `docs/wavenet_walkthrough.rst`, `detail::LayerArray::get_receptive_field()`, `NAM/film.h` |
| G6 | A2 shape: 23 layers; `channels = bottleneck ∈ {3,8}`; fixed kernel/dilation arrays; `a2_fast` fast path is **ON by default** (`NAM_ENABLE_A2_FAST`), selected by `is_a2_shape()` | `NAM/wavenet/a2_fast.h`, `NAM/wavenet/params.h`, `CMakeLists.txt` |
| G7 | A2 lookback = **6346 samples**, prewarm count **6347** (≈ **132.2 ms** @48 kHz); **algorithmic latency = 0 samples**. **Verified on this host**: `namchain-bench`/`GetPrewarmSamples()` both report 6347 for `A2.nam`, `A2-Full` and `A2-Lite`, matching the hand-computed arithmetic in the M0 test | arithmetic from `a2_fast.h` constants; confirmed by measurement 2026-09-11 |
| G8 | `prewarm()` runs the net on **zeros**; `WaveNet::prewarm()` is `HasCachedPrewarmState() ? PrewarmFromCache() : (DSP::prewarm(), CacheStateAsPrewarmed())`. The cache stores **one steady-state sample per conv** — i.e. it restores the **silence/zero state**, and re-prewarm is a memcpy, not a net run. Cache is disabled when `condition_dsp != nullptr` | `NAM/dsp.cpp`, `NAM/wavenet/model.cpp` |
| G9 | `Reset()` prewarms **by default**; suppress with `SetPrewarmOnReset(false)` or `DspLoadOptions{prewarm=false}`; `ScopedPrewarmOnResetDefault` is a thread-local RAII override | `NAM/dsp.h`, `NAM/get_dsp.h` |
| G10 | `SlimmableContainer`/`SlimmableWaveNet`: `SetSlimmableSize(double 0..1)` is thread-safe but **not RT-safe** (allocates + rebuilds, publishes via `atomic<shared_ptr>`); slimming is **structured channel pruning**, not interpolation. **CORRECTED after measurement — the two classes behave differently.** `ContainerModel::GetPrewarmSamples()` *delegates to the active submodel* (`container.cpp:135-139`), and `SlimmableContainer` submodels **do** take the `a2_fast` path because they are parsed by the `WaveNet` factory, which consults `is_a2_shape()` (`model.cpp:1315-1322`); disabling `a2_fast` costs 1.20× on `A2.nam`. The class with the real limitations is `nam::SlimmableWavenet`, which hardcodes `GetPrewarmSamples() == 0` (`wavenet/slimmable.h:66`) and builds submodels through its own factory (`wavenet/slimmable.cpp:587`) without the A2 detector | `NAM/container.cpp`, `NAM/wavenet/slimmable.{h,cpp}`, `NAM/wavenet/model.cpp:1315-1322` |
| G11 | Loaders: `nam::get_dsp(path|json|dspData, DspLoadOptions)`; `.nam` = JSON (`0.5.0`–`0.7.0`); **no binary loader in tree**; **no resampling container** — the host must resample on sample-rate mismatch (containers/sequential throw on child mismatch) | `NAM/get_dsp.h`, `tools/render.cpp` |
| G12 | Upstream RT-safety tests assert **zero allocations** in `process()` for buffers 1..256 after `Reset(48000,256)`; Eigen is effectively single-threaded (no OpenMP/TBB flags) | `tools/test/allocation_tracking.h`, `test_wavenet/test_real_time_safe.cpp` |
| G13 | The official plugin does **no crossfade ever**: it stages a model and swaps the pointer at a block boundary, **destroys the outgoing model (free) on the audio thread**, steps gains instantly, and can reach `prewarm()` on the audio thread via `OnReset` (diagnosed in open PR #627) | `NeuralAmpModelerPlugin/NeuralAmpModeler.cpp`, plugin PR #627 |
| G14 | Best prior art for the anti-click details is **closed PR #662**: ramp gains per sample (polarity flips and mutes click otherwise), latency-align slots with per-slot delay lines sized at reset, and **keep muted slots processing because skipping inference freezes state and glitches on unmute** | plugin PR #662 |
| G15 | Commercial state of the art: Helix "Preset Spillover" = dual-DSP render (~50 % DSP cost, not on Stadium); Quad Cortex = gapless **scenes** (parameter-only) but **gapped presets**; HeadRush Core = no public claim. Nobody publicly does state-accurate gapless model switching | Line 6 forum/manual, Neural DSP Dev Update #18, searches |
| G16 | Elk Sushi claims gapless load/delete/move of tracks and plugins, using an RT-safe "async work request" handoff — the closest architectural prior art to §8's protocol | Elk docs/forum |
| G17 | iOS forbids executing downloaded code (`dlopen` of downloaded dylibs; App Review 2.5.2). Third-party DSP must be statically linked / embedded and signed; **plugins must be data-only** | Apple docs |
| G18 | Linux RT needs `rtprio`/`memlock` limits, `threadirqs`, performance governor, `mlockall`, USB autosuspend off; PREEMPT_RT is mainlined (6.12) but Raspberry Pi OS ships non-RT; Pi throttles progressively from 80 °C | linuxaudio wiki, Pi docs, kernel docs |
| G19 | ARMv7 (A17) NEON is 128-bit but has **no FMA**, ~3 GB VA and ~5 GB/s bandwidth; the Debian armhf ABI baseline is VFPv3-D16, so NEON must be **runtime-detected** (`getauxval(AT_HWCAP) & HWCAP_NEON`) for redistributable packages | Eigen vectorization docs, Debian arm list |
| G20 | Pi 5 (A76) has FP16 arith + `SDOT`/`UDOT` and ~2× Pi 4's bandwidth; Apple's AMX/SME is **not public ISA** (Accelerate/BNNS only) | ARM/Pi/Apple docs |
| G21 | IPC: audio thread must never touch a socket; the canonical pattern is IO thread → validated POD command → SPSC ring → audio thread commits at block boundary; latency-matched crossfade (not naive swap) for topology edits | LV2 worker spec, Bencina RT-101, Apple workgroup docs |
| G22 | `clang -fsanitize=realtime` (RealtimeSanitizer) exists specifically to flag allocations/locks/syscalls in RT functions | Clang docs |

**The single most important consequence:** because of **G5 + G7 + G8**, a NAM model's internal state
is a pure function of its last `RF` input samples, and `prewarm()`-from-cache gives only the *silence*
state. Therefore a model placed into a live chain can be made **state-accurate before it becomes
audible** by replaying its last `lookback` samples of *real* input into a fresh instance on a worker
thread. That is the mechanism this engine is built around — no other NAM host does it (G13–G15).

---

## 3. Architecture

### 3.1 Threads and ownership

| Thread | Priority / affinity | Owns | Never does |
|---|---|---|---|
| **Audio (render)** | `SCHED_FIFO` 80 on Pi (below IRQ threads), pinned to a dedicated core; `THREAD_TIME_CONSTRAINT_POLICY`/`QOS_CLASS_USER_INTERACTIVE` on Apple; joined to the audio workgroup | The live `RenderPlan`, all live node instances, all transition state | Allocate, lock, syscall, log, parse, touch `std::string`/JSON, wait on anything unbounded |
| **Control** | normal | Graph IR, compilation, model registry, socket server, plan publication, **retirement/reclamation** | Touch live instances |
| **Warm workers (1..N)** | `SCHED_IDLE` / nice 19, affinity **excluded from the audio core** | Candidate (non-audible) instances, replay/catch-up, slimming rebuilds | Run on the audio core; touch live instances |

Ownership of any node instance is a single-owner token (`WORKER → AUDIO` on adoption;
`AUDIO → CONTROL` on retirement). No instance is ever touched by two threads at once. Adoption is
prepared by the control thread *before* the commit deadline, so the audio thread never waits for a
handoff.

### 3.2 Block data flow (per render quantum)

1. I/O backend fills input buffers; returns the quantum (fixed for the session).
2. Audio thread drains the SPSC **command ring** (fixed-size POD records) until empty.
3. Pending ops advance one quantum: fades ramp, dual-render regions blend, deferred commits are
   re-evaluated against `warm_through` markers.
4. `RenderPlan` executes: topologically ordered nodes, each reading its pre-bound input buffer and
   writing its pre-bound output buffer; fan-out buffers are written once and shared by reference (no
   copies).
5. I/O backend writes output; per-block timing is recorded into a lock-free telemetry ring.
6. Retired objects are pushed to a reclamation FIFO with an epoch; the control thread frees them after
   `epoch + fade + 2` blocks have elapsed (epoch counter bumped by the audio thread each block).

### 3.3 RT discipline (invariants, each with a test)

- No allocation/lock/syscall/exception/JSON on the audio thread (RTSan + interposer tests, §10.4).
- The audio thread is the **only** mutator of live audio state; all edits arrive as POD commands.
- Every structural change is compiled to a **new immutable `RenderPlan`**, fully allocated before
  publication; the audio thread only swaps an index.
- Reclamation is epoch-based; no `delete` ever runs on the audio thread.
- Every buffer is preallocated at `maxQuantum` and page-touched at plan build;
  `mlockall(MCL_CURRENT|MCL_FUTURE)` after start.
- No unbounded loop: graph size, node count, and transition count are validated at compile time.
- Denormals: scoped FTZ/DAZ guard on x86 only (ARM handles subnormals in hardware); no `-ffast-math`
  on the model path.

### 3.4 Component map

```
libnamchain
├── rt/          memory arena, SPSC rings, epoch reclamation, RT-safe ramp/delay, timing
├── graph/       IR, JSON schema, patch application, validation, memory planner, compiler
├── render/      RenderPlan executor, transition ops, governor, watchdog, telemetry
├── nodes/       built-in node library (incl. nc.nam, nc.cab.ir)
├── nam/         vendored-core integration: registry, loader, capability probe, warm engine
└── io/          backend interface + file/null/pipewire/alsa-raw/jack/coreaudio
libnamchain_abi   extern "C" API + node plugin ABI (versioned)
namchaind         daemon: config, transports, lifecycle, packaging
namchain-render   offline WAV→WAV renderer with a scripted edit timeline (primary test tool)
namchain-bench    model/chain/thread/quantum benchmark harness
```

---

## 4. Repository, build, dependencies, licensing

### 4.1 Layout

```
CMakeLists.txt  cmake/toolchains/{aarch64-linux-gnu,armv7-linux-gnueabihf,macos-universal,ios}.cmake
include/namchain/{abi.h,node_abi.h,version.h}     # public C surface
src/{rt,graph,render,nodes,nam,io}/               # as §3.4
tools/{render,bench,ctl}/                         # CLIs
tests/{unit,golden,glitch,rt_safety,soak,fuzz}/
third_party/{doctest.h}                           # MIT, single header
vendor/NeuralAmpModelerCore/                      # git submodule, pinned @G2; patches in vendor/patches/
docs/{architecture,node-sdk,protocol,transitions,tuning,benchmarks}.md
rigs/                                             # example chain sets + test signals generator
```

### 4.2 Build

- CMake ≥ 3.22, C++20, `-O3`; keep `NAM_ENABLE_A2_FAST=ON`.
- **A2 planar NEON kernels: ON by default on AArch64 (decision, 2026-09-11).** Carried as
  `vendor/patches/0001-a2-planar-neon-kernels.patch` (PR #313, §7.8) and applied idempotently to the
  pinned submodule at configure time; `-DNAMCHAIN_APPLY_VENDOR_PATCHES=OFF` disables the mechanism and
  `-DNAM_DISABLE_A2_PLANAR` disables only the kernels. Verified in-tree: the default Release build
  loads `A2PlanarFull`/`A2PlanarNano` and measures RTF 0.0500 (A2-Full) and 0.0086 (A2-Lite) at q = 64 —
  the §11 numbers — with the bit-identity parity test passing inside `namchain-tests` (12 cases, 188
  assertions). CI's aarch64+qemu job is the enforcement point: it sets `-DNAMCHAIN_REQUIRE_A2_PARITY=ON`,
  which turns the test's unoptimised-build skip into a hard failure, so the gate cannot pass vacuously
  (verified on this host — a Debug build with that flag fails loudly; the cross build run under qemu
  passes 146 parity assertions). The patch also touches `tools/`, which we do not build; that is
  deliberate, because
  keeping it byte-identical to the upstream PR makes it droppable the moment it merges. Consequence:
  the submodule working tree is **intentionally dirty** after configure, so `.gitmodules` marks it
  `ignore = dirty`. x86 targets compile the kernels out entirely and fall back to `a2_fast`.
- **`NAM_SAMPLE_FLOAT`: default OFF; do not define it (decision, 2026-09-11).** It is not a precision
  knob — the model arithmetic is float32 either way (Eigen `MatrixXf`, and `a2_fast` keeps explicit
  `std::vector<float> _cond` / `_head_out`, described in-source as "float32 copy of the double
  NAM_SAMPLE input", `a2_fast.cpp:143-144`). All it changes is the **element type of the process()
  boundary buffers**, and it removes a per-block double↔float conversion inside `a2_fast`. Measured:
  no benefit on this host (A2-Full q=64: 0.1079 double vs 0.1097 float; A2-Lite 0.0262 both). Revisit
  at M1 when the I/O backends land: PipeWire/ALSA/CoreAudio are float32-native and the §6.1 node ABI
  is already `float* const*`, so a float engine avoids conversion at both ends, and ARMv7's ~5 GB/s
  bandwidth (G19) is the other reason to retest. The option exists and both paths are verified to build.
  **Correction to the earlier text here:** the core has no CMake option for this — `NAM_SAMPLE_FLOAT`
  is only an `#ifdef` in `NAM/dsp.h`, so the originally planned `-DNAM_SAMPLE_FLOAT=ON` was silently
  ignored by CMake (it warns "unused variable") and always built the double path.
- **The core must be built as an object library, not a static one.** Architectures register via
  file-scope static initializers (`static ConfigParserHelper _register_WaveNet(...)`) that nothing
  references; any static archive discards those objects and every load fails at runtime with
  `No config parser registered for architecture: WaveNet`. Upstream sidesteps this by compiling
  `NAM_SOURCES` into every tool.
- Vendored NAM core patched only through `vendor/patches/*.patch` applied at configure time; every
  patch is upstream-PR-shaped (§7.8).
- **Bit-identity constraints from PR #313 (§7.8).** Where the planar kernels are active, the reference
  `A2FastModel` and the kernels agree bit-for-bit only under conditions we must hold deliberately:
  `-O3`-class optimisation with FMA contraction enabled, and **no `-ffast-math`** (§3.3 already forbids
  it on the model path). Two traps: (a) the core's own `render` and `bench_a2_fast` targets are
  compiled `-Ofast` in Release, so a comparison against *those* binaries is meaningless — a parity or
  golden comparison must use an `-O3` reference; (b) `-O0` builds break the premise too, which is why
  the PR's in-tree parity test currently fails (§7.8). Our CI must therefore build the A2 parity test
  at the same optimisation level as the shipping build, and treat "parity holds at our release flags"
  as the acceptance criterion rather than "parity holds somewhere". No Eigen
  `EIGEN_MAX_ALIGN_BYTES=0` hack unless measurement
  demands it (README "Sharp edges"), and a startup self-test that verifies Eigen alignment on the
  target.
- Cross-compile via toolchain files (aarch64, armv7 hard-float; both committed under
  `cmake/toolchains/`); native builds on the Pi for RT/NEON validation.
- CI matrix (`.github/workflows/ci.yml`): linux-x86_64 (build + unit tests), linux-x86_64 **RTSan on
  clang-20**, aarch64 cross + qemu smoke, armv7 cross. **RealtimeSanitizer requires clang ≥ 20**;
  Ubuntu 24.04's default clang 18 fails with `unsupported argument 'realtime'`, so the RTSan job
  installs `clang-20` explicitly. macOS (later) and a nightly self-hosted bare-metal Pi job running
  soak + `cyclictest` + bench remain to be added; cloud-runner numbers are not evidence for §1.

### 4.3 Dependencies and licensing (permissive-only, per your choice)

Vendored/pinned: NAM core (MIT), Eigen (MPL-2.0 — file-level copyleft, unmodified use only),
nlohmann/json (MIT), doctest (MIT). No JUCE (dual/GPL), no iPlug2, no GPL/LGPL host frameworks.

**One tension to flag:** the Linux audio stack is the only place where "permissive-only" bites. ALSA's
`libasound` and JACK2 are LGPL-2.1; PipeWire and WirePlumber are MIT. The plan therefore ships:
- `nac_io_file` / `nac_io_null` — no deps; used by CI and offline rendering (mandatory anyway for
  determinism).
- `nac_io_pipewire` — MIT client, default for desktop/coexistence.
- `nac_io_alsa_raw` — **direct kernel UAPI ioctls on `/dev/snd/pcmC*D*p`** (hw params, prepare,
  writei/readi, card enumeration via control ioctls). No `libasound`, hence no LGPL. This is the
  appliance backend and the one that matters for latency on a Pi.
- `nac_io_jack` — deferred; requires accepting LGPL dynamic linking.

**Decision:** build `alsa_raw` in M1 (≈600–900 lines incl. hw-param negotiation), keep the backend
interface identical so a `libasound`-based backend can be dropped in later if you'd rather trade the
license position for less code. Real round-trip latency comparisons decide the default at M1 exit.

---

## 5. Graph IR, compiler, and transition machinery

### 5.1 Chain model

- **Rig** = a set of **chains** + scenes. A chain has exactly one `nc.input` and one `nc.output` node
  (your requirement), both bound to physical ports (`{device, channel}` or a bus).
- **Node** = `{id, type, params{}, enabled, channel_arities}`; **Edge** = `{from: node:port, to:
  node:port}`. Mono/stereo/multichannel ports are typed and validated.
- **Cycles are a compile error.** Feedback is expressed with an explicit `nc.unit_delay` node (1
  quantum of delay). Rationale: keeps latency compensation trivial and the scheduler a simple
  topological walk; every real pedalboard is a DAG.
- **Latency**: every node declares `latency_samples`. All M1–M4 built-ins are **zero-latency** (NAM A2
  = 0 by G7, `nam::Linear` IR = 0 with lookback). Non-zero-latency nodes (lookahead limiter,
  resampler) are supported by inserting compensating delays on parallel paths at compile time (PDC),
  so crossfades stay phase-aligned (G21).

### 5.2 Compile pipeline (control thread)

`JSON rig` → parse/validate → IR → lint (cycles, arity, param ranges, missing models) → **lower**
(expand `nc.nam` slots, insert resamplers/PDC delays, insert mixers for fades) → **memory plan**
(liveness analysis + interval colouring over intermediate buffers, one arena, zero steady-state
allocation) → **cost model** (per-node predicted µs from the measured cost table) → immutable
`RenderPlan` (node array in topological order, buffer bindings, fade/mixer slots, `admission` record).

Compile is transactional: any error leaves the running plan untouched and returns a structured error.
A plan is published by an atomic index store; the previous plan is retired via §3.2/6.

### 5.3 Edit semantics — every edit is one of four op classes

| Edit | Mechanism | Cost | Glitch risk |
|---|---|---|---|
| Param change (`smooth` class) | Per-sample ramp inside the node's gain/param slot, scheduled at a sample offset | ~0 | None (ramped) |
| Param change (`structural` class) | Treated as a node swap: pre-build the alternative node, crossfade | dual-node | None (crossfaded) |
| Bypass / enable | Equal-power crossfade over `fade_ms` (default 5 ms) to/from dry; node keeps running for `tail_hold` if it has a tail | dual-path | None |
| Model swap in `nc.nam` | Warm-start (§7.4) then 2 ms equal-power crossfade, or deferred commit until warm | dual-model during fade | None; state-accurate in the common case |
| Rewire / add / remove node / scene recall | Dual-render old and new plan for `fade_ms`, equal-power crossfade at chain outputs, then retire old | dual-plan | None if admitted; else fade is lengthened, or the edit is rejected with a reason |

Rules that make this safe:
- **One structural transition in flight per chain**; new requests are coalesced (last-writer-wins for
  params, sequentialised for structure).
- **Admission control**: `worst_case_block = Σ cost(new plan) + Σ cost(old plan during fade)`. If
  `worst_case_block > budget` (default 70 % of deadline), the compiler (a) shortens the fade, (b) drops
  speculative warming first, (c) slims models (§7.7), or (d) **rejects the edit with a structured
  error** rather than risking a dropout.
- **Never-silence guarantee**: if any op cannot complete safely, the engine keeps the current plan
  rendering and reports; the only path to silence is an explicit `engine.mute`/`panic`.
- **Failure isolation**: a node returning non-finite samples has its output crossfaded to a bypassed
  path and is marked failed (per-node guard, always on for third-party nodes).

### 5.4 Deferred-commit scheduler

Structural commits are scheduled at a **future sample** (`t_commit`), not "next block". Commit happens
at the first block boundary where all preconditions hold (target instance adopted,
`warm_through ≥ t_commit`, buffers resident). If the deadline (`t_request + max_defer_ms`, default
15 ms) passes, the engine commits in **degraded mode** (crossfade from silence-prewarmed state) and
reports `degraded: true`. Musicians never hear a gap; worst case is a very short, crossfaded timbre
transition.

---

## 6. Node ABI and built-in library

### 6.1 ABI (stable, versioned, `extern "C"`)

```c
#define NAC_NODE_ABI_VERSION 1
typedef struct nac_node_desc {
  uint32_t abi_version;
  const char* id;                 /* "nc.gain" or "com.acme.ts9" */
  const char* version;
  uint32_t flags;                 /* STATEFUL | HAS_LOOKBACK | HAS_TAIL | RT_PARAM_SAFE | IN_PLACE_OK */
  uint32_t latency_samples;
  const char* params_schema_json; /* name, id, range, default, smoothing class */
} nac_node_desc;

typedef struct nac_node_vtable {
  const nac_node_desc* (*describe)(void);
  void* (*create)(const nac_host_services*, const char* params_json, double rate, int max_quantum);
  void  (*destroy)(void*);
  void  (*process)(void*, float* const* in, float* const* out, int frames);  /* RT */
  void  (*set_param)(void*, uint32_t param_id, float value);                 /* RT-safe */
  void  (*reset)(void*);
  int   (*lookback_samples)(void*);   /* >0 enables warm-start replay */
  int   (*tail_samples)(void*);       /* >0 enables spillover on bypass */
} nac_node_vtable;
```

Params are resolved to integer ids at compile time (no strings on the audio thread). Third-party nodes
load via `dlopen` on Linux/macOS **only**; on iOS they are statically registered (G17) and plugin code
is data-only.

**Open sample-type decision (settle at M1).** This ABI is `float`, but the vendored core defaults
`NAM_SAMPLE` to `double` (§4.2). Options: (a) build the core with `NAM_SAMPLE_FLOAT` so both sides are
float and no conversion exists at the `nc.nam` boundary; (b) keep the core in double and convert once
per block inside the `nc.nam` node; (c) run the whole engine in double. Measurement says the choice is
performance-neutral on the Pi (§4.2, §11), so decide it on interface cleanliness and on the −60 dBFS
state-continuity budget, not on speed. Option (a) would also make the engine's own input-history rings
(§7.3) float, which is the one place the precision question genuinely touches the warm-start claim.

### 6.2 Built-ins

`nc.input`, `nc.output`, `nc.gain` (ramped), `nc.split` (1→N), `nc.mix` (N→1 with ramped gains/pan),
`nc.pan`, `nc.eq.biquad`, `nc.delay`, `nc.cab.ir` (via `nam::Linear`, zero-latency, lookback-aware),
`nc.nam` (§7), `nc.gate`, `nc.limiter` (declares latency), `nc.reverb` (declares tail; M5),
`nc.unit_delay`, `nc.resample` (rate adapter, declares latency), `nc.meter` (telemetry tap,
audio-transparent), `nc.bypass` (wrapper).

---

## 7. NAM subsystem — the flagship capability

### 7.1 Registry and loader (worker thread, never the audio thread)

- `ModelRegistry` maps `model_id → {path, hash, dspData, metadata(arch, channels, sample_rate,
  loudness/levels), lookback, latency, flags}`.
- Load via `nam::get_dsp(path, DspLoadOptions{ .prewarm = false })` (G9) with the engine at 48 kHz.
  `.nam` only (G11); a local binary cache of parsed weights is a later optimization.
- **Capability probe** per model: architecture; whether `condition_dsp` exists and whether it is
  finite-memory (**LSTM ⇒ unbounded memory ⇒ not state-replayable**); whether it is a
  `SlimmableContainer` (G10); measured RTF per quantum size; RAM per instance.
- Instances are created, `SetPrewarmOnReset(false)` + `Reset(engine_rate, max_quantum)` (no expensive
  prewarm, G9), then optionally primed from the cached silence state (cheap, G8).
- **Lookback calculator (still required, but for different reasons than G10 first claimed):** the
  engine computes lookback itself by parsing the `.nam` JSON exactly as the core does
  (`Σ dilation·(kernel−1) + head`), because the core is unhelpful in four ways: (a)
  `SlimmableContainer` reports only its *active* submodel, which is wrong for planning a switch
  across a slimmable set; (b) `SlimmableWavenet::GetPrewarmSamples()` hardcodes 0; (c) `nam::Linear`
  (IR) never overrides it, so an FIR reports 0 despite needing its whole history; (d) legacy `.nam`
  files use a scalar `kernel_size` + `head_size` schema that must be replicated/zero-weighted exactly
  as `model.cpp:964-1039` does. The calculator recurses through `condition_dsp` and container
  submodels and reports **`Unbounded`** if *any* submodel is an LSTM (real case:
  `slimmable_container.nam` mixes LSTM and WaveNet). It cross-checks against `GetPrewarmSamples()`
  wherever that is non-zero. Implemented in `src/nam/lookback.cpp`, golden-tested against the A2
  numbers (6347) in `tests/test_lookback.cpp`.

### 7.2 Instance lifecycle

States: `LOADING → COLD → WARMING(lag Δ) → CURRENT(warm_through = t_audio) → AUDIBLE → RETIRED`.
Single-owner token per instance (§3.1). Adoption is `AUDIBLE ← CURRENT`; the worker immediately builds
a *replacement* candidate instance for that slot (allocation off the audio thread) so the slot always
has a warmable instance available.

### 7.3 Input history

Each `nc.nam` node keeps an SPSC ring of its **input** samples, capacity `lookback + 4·max_quantum`
(≈ 25 KB per channel per node — negligible). Written by the audio thread, read by warm workers. When a
node is bypassed but still in the graph, its input ring keeps filling, so its candidates stay
warmable. If the node's upstream is rewired in the same edit, the history is flagged `foreign` and the
switch is downgraded to crossfade-only (§7.6).

### 7.4 Warm-start algorithm (the core innovation)

```
target candidate I of node N, lookback L = N.lookback, engine sample index t_audio
worker:  while (t_worker < t_audio):
           x = input_ring[t_worker .. min(t_worker+chunk, t_audio))       # chunk = 64..512
           I.process(x) -> scratch                                        # state-only replay
           publish warm_through = t_worker + len(x)  (release)
audio:   on switch request at sample t_req:
           t_commit = t_req + deferral        # deferral starts at 0
           may_commit when  ownership(I)==AUDIO  &&  I.warm_through >= t_commit
           if may_commit: adopt I; crossfade 2 ms from the outgoing model (outputs are near-identical)
           elif t - t_req < max_defer_ms: keep rendering the outgoing model; re-check next quantum
           else: adopt I with a `fade_ms` crossfade from its cached silence state; report degraded
```

Why it is exact: by G5/G7, after `L` samples of true input a WaveNet's state is *exactly* the state it
would have had if it had been running all along (all state is a finite window of past input; the
initial condition is washed out after RF). Therefore the incoming model, when adopted, continues the
waveform as if it had never been bypassed — no click, no transient, no level jump.
`GetPrewarmSamples()` (= L+1 for A2) is the authoritative replay length.

**Incremental catch-up:** because state is continuous in time, an instance that went stale by Δ only
needs Δ samples of replay — not the full L. Consequences: (a) after the first warm, ping-pong A/B
switching (the guitarist's most common action) is nearly free; (b) a cold instance costs `L × RTF`
wall time to warm (A2: ~132 ms of audio ⇒ a few ms to tens of ms of CPU, measured in M0), which is why
cold switches use the deferral path.

### 7.5 Warming policy engine (the CPU/pre-warm trade-off, made explicit)

Keeping a candidate **fully current** costs its full RTF; letting it go stale by Δ costs ~0 CPU but
adds ~Δ·RTF to switch latency. Policies:

| Mode | Hot set (fully current) | Δmax for warm set | Typical added CPU | Switch latency (p99) |
|---|---|---|---|---|
| `instant` | 4 candidates | 0 | ≈ 4 × RTF | ≈ 0 + fade |
| `balanced` (default) | 2 | 250 ms | ≈ 2 × RTF | ≤ 5 ms + fade |
| `eco` | 0 | 2 s | ≈ 0 | ≤ 30 ms |
| `maxmodels` | 0 | ∞ (replay on demand) | ≈ 0 | ≤ `L × RTF`, else degraded |

Additional levers: **hints** (`scene.prepare {id}`, or the host telling us the next-likely model)
promote candidates to the hot set immediately; **silence detection** collapses cost (if the node input
is < −80 dBFS for ≥ L, every candidate's state equals the cached silence state — skip replay entirely,
G8); **LRU + recency weighting** chooses the hot set; a **token bucket** caps total warming CPU
(default 25 % of one core) and the governor suspends warming first under load.

### 7.6 What we can and cannot promise

- **Exact state continuity** when the candidate's input history is the same signal it will receive (the
  overwhelmingly common case: amp channel switching, model A/B, cab swap, pedal in/out on an unchanged
  upstream path).
- **Click-free but not state-exact** when the edit simultaneously changes the candidate's upstream
  (history flagged `foreign`) or the model has a `condition_dsp` LSTM (no finite memory). These use
  crossfade + a longer fade, and report `exact: false`.
- A useful middle case: if the upstream change is *parameter-only* (e.g. a drive knob), the history can
  be re-rendered through the new upstream parameters to produce an exact-ish state —
  `replay_with_params` flag, M3 stretch.

### 7.7 Slimming governor

For slimmable models (G10), `SetSlimmableSize` is called on a worker; the container publishes the new
submodel atomically, and the engine treats the publish as a **structural node swap**: dual-render +
crossfade (the swap itself is not state-continuous). This gives a graceful overload response: instead
of glitching, the engine slims the heaviest models (e.g. A2-Full → A2-Lite) with a crossfade and
reports the quality change. Measured breakpoints per model drive the choice.

### 7.8 Upstream patches/PRs (each small, testable, and upstream-friendly)

**Re-scoped after measurement (2026-09-11): patch #1 as originally written is unnecessary.**
`SlimmableContainer` submodels already reach `A2FastModel` through the `WaveNet` factory, and
`ContainerModel::GetPrewarmSamples()` already delegates to the active submodel. Measured cost of
`NAM_ENABLE_A2_FAST=OFF` at q=64 (2 s of audio, 10 iterations — higher is worse):
`A2-Full` 219.7 → 261.3 ms, `A2.nam` container 220.3 → 265.2 ms, `A2-Lite` 51.1 → 186.3 ms. The
patches that are actually worth carrying upstream:

0. **PR #313 — planar NEON kernels for the A2 fast path. ADOPTED 2026-09-11** — vendored as
   `vendor/patches/0001-a2-planar-neon-kernels.patch`, applied at configure time and ON by default on
   AArch64 (§4.2). It is the single largest measured win available, and it is already written.
   Authored in-house, base commit == our pin, so it applies to the vendored
   core as-is. **Independently reproduced on this host** (§11): A2-Full RTF 0.1072 → 0.0501 (**2.14×**)
   and A2-Lite 0.0262 → 0.0087 (**3.01×**) at q = 64, with 2.00–2.35× / 2.96–3.14× across q = 32…256
   (`bench-results/pr313-planar-ab.csv`). Output is **bit-identical** to `A2FastModel`, confirmed both
   by the PR's own harness ("bit-identical over all 192000 frames") and by our A/B build, which loads
   `A2PlanarFull` instead of `A2FastModel<8>`. Gating: auto-on for `__aarch64__`, and for
   `__arm__ && __ARM_NEON && __ARM_FEATURE_FMA`; `NAM_DISABLE_A2_PLANAR` opts out; on x86 it compiles
   to nothing at all. Constraints it imposes on us are spelled out in §4.2 (bit-identity is an `-O3`
   property and is void under `-ffast-math`), §9.4 (the ARMv7 arm is a **compile-time** gate, which
   collides with G19's runtime-NEON packaging policy), and §10.2 (it lets the golden test assert
   bit-exactness instead of a tolerance).
   **In-tree wiring bug to fix when vendoring:** the parity test lives in `run_tests`, which the core
   builds with `-O0`; at `-O0` the reference does not contract to FMA, so the test fails on the first
   sample of the first block size (verified on this host). Rebuilt at `-O3`, the same test passes all
   14 block sizes including the 1/3/4-frame tails. It needs its own `-O3` target or a per-source
   `COMPILE_OPTIONS "-O3"` override, otherwise it guards nothing.
1. `nam::SlimmableWavenet`: implement `GetPrewarmSamples()` from the layer config instead of returning
   0 (`wavenet/slimmable.h:66`), and route A2-shaped submodels through `is_a2_shape()` in its own
   factory (`wavenet/slimmable.cpp:587`). The A2-Lite fast path is **3.6×** faster on this host, so a
   missing route is a large, silent perf loss for exactly the models this engine targets.
2. `nam::Linear`: override `GetPrewarmSamples()` to return `receptive_field`, so IR nodes can be
   warmed exactly instead of resuming from silence (verified: the core reports 0 for a 2048-tap FIR).
3. Optional: expose per-instance weight sharing for the same model (biggest memory-traffic lever;
   G14/PR #662 and issue #124 note NAM is heavily memory-bound). Gated behind measurement in M5.

### 7.9 Explicit non-goals

No reimplementation of A2 math (we host the reference core bit-for-bit); no model training; no model
redistribution (TONE3000 terms don't grant redistribution — users supply their own `.nam` files; only an
optional, clearly-labelled download integration would engage the TONE3000 API terms).

---

## 8. Control surface: C ABI, daemon, protocol

### 8.1 C ABI (`include/namchain/abi.h`)

`nac_engine_create(cfg)`, `nac_engine_destroy`, `nac_engine_start/stop`,
`nac_engine_apply_patch(json)`, `nac_engine_submit(cmd, len)`, `nac_engine_poll_event(buf, len)`,
`nac_engine_get_status(json)`, `nac_engine_render_offline(...)` (same render path, virtual clock — the
determinism/test hook), `nac_register_node_type(vtable)`, `nac_engine_set_io_backend(...)`. Errors are
integer codes plus a JSON detail string; no exceptions cross the boundary.

### 8.2 Daemon (`namchaind`)

Config file (JSON) + CLI flags; systemd unit with `LimitRTPRIO`/`LimitMEMLOCK`,
`CPUSchedulingPolicy=fifo`; graceful start (device open → plan compile → warm → start clock → fade in)
and stop (fade out → stop → release).

### 8.3 Transports

Primary: Unix domain **`SOCK_SEQPACKET`** (message boundaries + filesystem permissions). Continuous
parameters and meters: a **shared-memory seqlock/atomic block** (zero-copy, wait-free) (G21). Later:
OSC/UDP bound to localhost for networked control, same wire structs.

### 8.4 Protocol (JSON, versioned, `id`-correlated)

`engine.hello|status|panic|mute`; `rig.load|get|patch` (ops: `add_node`, `remove_node`, `connect`,
`disconnect`, `set_param`, `bypass`, `swap_model`, `rewire`); `scene.recall|prepare`;
`model.load|unload|list`; `bench.run`; `telemetry.subscribe`. Events: `xrun`, `cpu_load`,
`edit_result`, `warm_ready`, `degraded`, `node_failed`, `device_lost/restored`. Unknown fields ignored;
unknown methods → structured error; protocol version negotiated at hello. **All-or-nothing**: a patch
either compiles and commits, or is rejected with a reason — never partially applied.

---

## 9. Platform I/O layer

### 9.1 Backends (identical interface)

`file` (WAV in/out, offline) and `null` (real-time-paced, no device) exist from M1 — they make CI and
determinism possible and are the reason M1 doesn't block on hardware. `pipewire` (MIT) and `alsa_raw`
(no libasound, §4.3) in M1. `jack` (LGPL, deferred, opt-in). `coreaudio` (macOS then iOS: RemoteIO/
AUHAL, workgroups, `QOS_CLASS_USER_INTERACTIVE`, `.measurement` session mode) in M6.

### 9.2 Clock, rate, latency policy

- **Engine rate = 48 kHz** (NAM models are 48 kHz-native; the core has no resampler and throws on
  mismatch, G11). The I/O layer negotiates 48 kHz with the device; if the device can only do 44.1/96
  kHz, a fixed-latency polyphase SRC at the I/O boundary carries the whole graph (one resampler, not
  one per model) and the added latency is reported and compensated.
- **Quantum = device period**, clamped to [32, 256], fixed for the session. Device period change ⇒
  recompile off-thread + crossfade; instances are re-`Reset()` on a worker before handoff.
- Engine algorithmic latency = I/O buffer + any declared node/PDC latency; target 64 samples (1.33 ms)
  at 48 kHz on Pi 5.

### 9.3 Linux RT tuning checklist (shipped as `docs/tuning.md` + a `namchain-doctor` script)

`/etc/security/limits.d/audio.conf` → `@audio - rtprio 95`, `@audio - memlock unlimited`;
`SCHED_FIFO` 80 via `sched_setscheduler`; `mlockall`; governor `performance`; `threadirqs`; RT kernel
(`linux-image-rt-*`, PREEMPT_RT mainlined 6.12 but Pi OS stock is not RT); `usbcore.autosuspend=-1`;
audio thread pinned to a dedicated core (workers and warm threads excluded; only use `isolcpus` if
IRQs are also moved); `vcgencmd get_throttled`/`measure_temp` in the health loop with a fan/Active
Cooler mandatory for always-on low-latency use (G18).

### 9.4 Platform constraints carried into the design

- **iOS: no runtime-loaded node plugins** (G17) ⇒ the ABI is designed now so third-party DSP is
  statically linkable and everything else (models, IRs, rigs) is data. GUI-less iOS means "app in the
  background" (`UIBackgroundModes: audio`), not a daemon.
- **ARMv7**: 32-bit VA and bandwidth limits (G19) ⇒ resident-model budget is configurable and small by
  default; NEON is runtime-detected for redistributable packages; FMA-less NEON halves FLOPs/cycle, so
  A2-Full counts will be much lower there — the engine should prefer A2-Lite and aggressive slimming
  on that target. **Interaction with PR #313 (§7.8):** its ARMv7 arm is a *compile-time* gate requiring
  `__ARM_NEON && __ARM_FEATURE_FMA`, so our default `-mfpu=vfpv3-d16` cross build compiles the kernels
  out and gets none of the 1.42–1.55× measured on a Cortex-A17. The FMA requirement is load-bearing,
  not defensive: without it Eigen picks the non-fused path and `a2_fast`'s own arithmetic changes, so
  the reference the kernels are compared against no longer exists. That means the ARMv7 win cannot be
  obtained by runtime-dispatching the kernel alone — it needs a separate `-mfpu=neon-vfpv4` artifact
  (Pi 2+/Pi 3-class and newer), which is a packaging decision for M6. The PR also documents that
  ARMv7 bit-identity rests on `FPSCR.FZ` being set (AArch32 NEON is unconditionally flush-to-zero,
  scalar VFP is not), which corrects §3.3's assumption that denormal handling needs no attention on
  ARM: for ARMv7 the engine must set FZ explicitly.
- **Pi 5 / Apple**: FP16 + dot-product available for a later validated fast path (G20); Accelerate on
  Apple behind a build flag, never required.

---

## 10. Verification — how "bulletproof" is proven

### 10.1 Unit tests

Graph validation (cycles, arity, dangling ports, unknown types), memory planner (overlap/liveness),
patch application (all-or-nothing, error cases), ramp/equal-power fade math, SPSC rings and epoch
reclamation under TSan, lookback calculator vs. hand-computed A2 numbers (6346/6347), model capability
probe, resampler.

### 10.2 Golden audio tests

Per node against references: gain/EQ/delay against analytic responses; `nc.cab.ir` and `nc.nam` against
the NAM core's own `tools/render` output (tolerance-based, bit-exactness not assumed because `a2_fast`
reorders operations). Determinism: repeated renders byte-identical. Quantum-invariance: outputs at
32/64/128/256 match within tolerance (state-continuity proof).

**Stronger A2 test now available (PR #313, §7.8).** On AArch64 the planar kernels are *bit-identical*
to `A2FastModel`, so the A2 golden test can assert exact equality (`memcmp`) against a reference
instance rather than a tolerance, across block sizes including the partial-tile and single-frame tails
(the PR ships such a test; it covers 1/3/4/7/8/15/16/31/32/33/64/65/128/512 frames and a
dispatcher check so it cannot pass on dead code). Two conditions: build it at the release optimisation
level, and note that this is a *GCC-on-AArch64* property — the PR documents that on ARMv7 under clang
the reference itself computes different bits, so the honest claim is "bit-identical wherever the
reference contracts its own FMAs". A tolerance-based fallback stays for other toolchains.

### 10.3 The glitch detector (the headline test)

`namchain-render` renders a file through a chain while applying a scripted edit timeline at exact
sample positions, then analyses output `y` against two oracle renders (`A` = chain before all edits,
`B` = chain after). For each edit at `t` with fade `F`:
1. **Continuity**: `d2[n] = |y[n] − 2y[n−1] + y[n−2]|`; let `M` = max `d2` outside edit windows across
   `A` and `B`; require max `d2` inside each edit window ≤ `κ·M` (κ = 2, tunable). Catches steps,
   polarity flips, mutes, non-ramped params.
2. **Gap/dropout**: no run of |y| < −90 dBFS longer than 1 ms where the oracle has energy; total
   sample count exactly as expected (no repeats or insertions).
3. **Spectral**: HF (5–20 kHz) energy in ±10 ms windows around each edit ≤ max(A, B) + 3 dB.
4. **Oracle convergence**: `y ≈ A` before `t−F−ε` and `y ≈ B` after `t+F+ε` within −60 dBFS.

Torture render: 60 s, 500 randomised edits, all node types and transition classes, at 32/64/128/256 and
44.1/48/96 kHz. Plus a **state-continuity** test: warm-started `nc.nam` output vs. always-on output
< −60 dBFS (this directly validates §7.4).

### 10.4 RT-safety tests

`clang -fsanitize=realtime` on the render path in CI; a global `operator new`/`free`/
`pthread_mutex_lock`/`read`/`write` interposer that aborts when called from the audio thread; a 10⁶-block
randomised edit storm with the interposer armed; assertion that `frames <= max_quantum` always (G3).

### 10.5 Soak, fuzz, and tools

8–24 h soak on bare-metal Pi 5 with periodic edit storms, thermal logging, `vcgencmd get_throttled`,
asserting zero xruns and reporting p50/p99/max block time. libFuzzer targets for the `.nam` loader and
the control-protocol parser (malformed input must produce errors, never crashes/hangs/OOM). On-device
tools: `cyclictest`/`rtla timerlat`, `jdelay`/`jack_iodelay` for true round-trip latency, `perf stat`
for cache/cycles, Instruments + `os_signpost` on Apple.

---

## 11. Performance — MEASURED on the target host (2026-09-11)

**This section previously contained estimates. These are measurements.** Host: Raspberry Pi 500
(BCM2712, 4× Cortex-A76 @ 2.4 GHz), Ubuntu 24.04, kernel 6.8.0-1064-raspi, `performance` governor,
40 °C, `throttled=0x0`. Engine configuration: 48 kHz, `NAM_ENABLE_A2_FAST=ON`, `double` samples,
single-threaded, `-O3`, 2 s of audio × 10 iterations, RTF excludes `Reset()`/prewarm. Produced by
`namchain-bench` and cross-checked against the core's own `benchmodel_bufsize` (agreement within 2 %).

| Model | q | p50 µs | p99 µs | max µs | RTF | p99 % of deadline | instances at 50 % | at 100 % |
|---|---|---|---|---|---|---|---|---|
| **A2-Full** (8ch) | 32 | 78.8 | 83.6 | 89.3 | 0.1187 | 12.54 % | 3.9 | 8.4 |
| | 64 | 142.6 | 148.6 | 314.5 | 0.1075 | 11.14 % | 4.5 | 9.3 |
| | 128 | 267.8 | 275.6 | 285.4 | 0.1008 | 10.34 % | 4.8 | 9.9 |
| | 256 | 521.4 | 530.3 | 535.0 | 0.0978 | 9.94 % | 5.0 | 10.2 |
| **A2-Lite** (3ch) | 32 | 18.1 | 20.5 | 34.9 | 0.0272 | 3.08 % | 16.2 | 36.8 |
| | 64 | 34.9 | 38.3 | 39.4 | 0.0262 | 2.88 % | 17.4 | 38.2 |
| | 128 | 67.9 | 71.5 | 72.3 | 0.0255 | 2.68 % | 18.7 | 39.2 |
| | 256 | 133.7 | 140.0 | 148.1 | 0.0252 | 2.63 % | 19.0 | 39.7 |
| **CabIR** (2048-tap FIR) | 64 | 11.8 | 33.3 | 35.9 | 0.0105 | 2.50 % | 20.0 | 95.2 |
| **A2.nam** (container) | 64 | 143.1 | 148.7 | 161.8 | 0.1078 | 11.15 % | 4.5 | 9.3 |

**Verdict against the §1 success criteria (q = 64):**
- **≥ 4 concurrent A2-Full ≤ 50 % deadline** — met, with little margin: 4 × 11.14 % = 44.6 %. 5 would
  be 55.7 %, so the target of 4 is the correct number rather than a conservative one.
- **≥ 12 concurrent A2-Lite** — met with room: 12 × 2.88 % = 34.6 % (17 fit at 50 %).
- The container case costs the same as its active submodel, so container overhead is negligible.
- q = 32 is the worst quantum (12.54 % vs 9.94 % at q = 256) — small blocks hurt, as predicted.

**Same host, same harness, with PR #313's planar NEON kernels** (back-to-back A/B, `NAM_DISABLE_A2_PLANAR`
as the control; `bench-results/pr313-planar-ab.csv`):

| Model | q | RTF off → on | p99 off → on | speedup | p99 % deadline (on) | instances at 50 % |
|---|---|---|---|---|---|---|
| A2-Full | 32 | 0.1186 → 0.0505 | 82.2 → 38.8 µs | **2.35×** | 5.81 % | 8.6 |
| A2-Full | 64 | 0.1072 → 0.0501 | 147.8 → 74.2 µs | **2.14×** | 5.57 % | 9.0 |
| A2-Full | 128 | 0.0999 → 0.0494 | 273.0 → 141.1 µs | 2.02× | 5.29 % | 9.5 |
| A2-Full | 256 | 0.0978 → 0.0490 | 536.3 → 275.6 µs | 2.00× | 5.17 % | 9.7 |
| A2-Lite | 32 | 0.0272 → 0.0092 | 20.5 → 8.2 µs | 2.96× | 1.23 % | 40.6 |
| A2-Lite | 64 | 0.0262 → 0.0087 | 37.4 → 13.9 µs | **3.01×** | 1.04 % | 48.1 |
| A2-Lite | 256 | 0.0251 → 0.0080 | 137.3 → 45.9 µs | 3.14× | 0.86 % | 58.1 |

This reproduces the PR's published Cortex-A76 numbers (2.15× / 3.08× at block 64), and an independent
third party reproduced >2× on Apple M1 and inside the plugin. Three consequences, all large:

- **§1 criterion 3 stops being the binding constraint.** A2-Full goes from 4 instances at 50 % of the
  deadline to **9**; A2-Lite from 17 to **48**. The throughput targets should be re-set in M1 rather
  than left at values chosen when CPU was the limit.
- **§14 risk 2 (cold-switch cost) largely dissolves.** Warm replay is 6347 samples = 132.2 ms of audio,
  so a cold A2-Full switch costs `132.2 ms × RTF`: **14.2 ms** on `a2_fast` — at or over the 15 ms
  `max_defer_ms` budget, which is exactly why it was the top technical risk — versus **6.6 ms** planar,
  and 1.15 ms for A2-Lite. The `instant` warming policy (§7.5) becomes affordable too: 4 hot A2-Full
  candidates cost ~20 % of a core instead of ~43 %.
- **The ceiling is no longer arithmetic.** At ~9 concurrent A2-Full, expect memory bandwidth and I/O to
  bind rather than FLOPs — which is what the §11 lever list already predicted.

**What the measurements did to the old estimates:**
- The predicted A2-Full/A2-Lite ratio was 6.7× (from 1.27 / 0.19 GFLOP/s); **measured is 4.1×**
  (0.1075 / 0.0262). The absolute GFLOP/s figures were not validated, and the "~6 GFLOPS effective on
  Pi 5" assumption behind the old "~7 A2-Full" bound was pessimistic — the real single-core ceiling is
  ~9 A2-Full. Use the measured table, not the FLOP derivation.
- The 3.6× A2-Lite fast-path effect (219.7 vs 51.1 ms) confirms `a2_fast` coverage is the single
  biggest lever for the models this engine targets (§7.8).
- **`NAM_SAMPLE_FLOAT` buys nothing on this CPU.** Back-to-back at q = 64, 2 s × 10 iterations:
  A2-Full 0.1079 (double) vs 0.1097 (float); A2-Lite 0.0262 both. The A76 has full-rate FP64 NEON, and
  the kernel is layout/memory-bound, so switching the sample type is not a performance lever — and it
  would cost numerical headroom against §1 criterion 2's −60 dBFS state-continuity bound. Default it
  **off**; revisit only if a target (ARMv7, Apple) measurably differs. `namchain-bench` supports both
  paths, which the core's own bench tools cannot.

**Resident-set RAM (analytical, from `a2_fast.cpp:315-320` `pow2(2·max_lookback + quantum)` per layer
+ weight array, at q = 64):** A2-Full ≈ **763 KiB**/instance (716 KiB history + 47 KiB weights);
A2-Lite ≈ 276 KiB. So 64 resident A2-Full instances ≈ 49 MB — **RAM is not the binding constraint;
warming CPU is**, exactly as §7.5 assumes. Full RSS (including allocator overhead) is still to be
measured in M0's remainder.

| Metric | Target (Pi 5 class, 48 kHz, q=64) | Status |
|---|---|---|
| Concurrent audible A2-Full (8ch) | ≥ 4, p99 ≤ 50 % deadline, 0 xruns/1 h | **met on p99; xruns/1 h not yet tested** (needs M1 + a device) |
| Concurrent audible A2-Lite (3ch) | ≥ 12 | **met** (17 at 50 %) |
| Resident switchable instances | ≥ 16 (≥ 64 if RAM allows) | **RAM supports ≥ 64**; switching itself is M3 |
| Switch ready-time (p99, warm set) | ≤ 5 ms, zero click | M3 |
| Cold switch (deferred) | ≤ 15 ms, zero gap, `exact` when possible | M3 |
| Added CPU for `balanced` warming | ≤ 2 × RTF of hot set | M3 |
| Engine latency @48 k/64 q | 1.33 ms I/O + 0 algorithmic | M1 |
| Block-time jitter | p99/max measured and published | **measured above** (max/p99 is noisy: see the q=64 outlier) |

**Caveats on these numbers:** they are single-threaded throughput measurements with no audio device
attached, taken on a bare-metal host with a desktop-class OS (no `threadirqs`, no PREEMPT_RT, no
`isolcpus`). They establish the CPU budget, not xrun behaviour. Real-time I/O claims (§1 criterion 3's
"zero xruns over 1 h") still require a capture-capable interface, which this box lacks (§0).

Levers, in measured order: **planar NEON A2 kernels (PR #313 — 2.0–3.1×, written and verified, §7.8)**
→ memory traffic (weight sharing, larger quantums, cache blocking) → A2 fast path coverage incl.
slimmable submodels (§7.8) → FP16/dot-product kernels on A76/Apple (accuracy-validated) →
multi-thread parallel rendering. **Parallel rendering is M4 and off by default**, because RT-safe
worker synchronization is the single largest reliability risk in the design; it is admitted only if
soak + glitch tests pass with it enabled. With PR #313 in place the arithmetic budget roughly doubles,
so parallel rendering's urgency drops further and the attention belongs on memory bandwidth and I/O.

---

## 12. Milestones

| M | Deliverable | Acceptance |
|---|---|---|
| **M0** | Phase 0 environment fixed ✔; repo, CMake, CI, vendored core pinned ✔; `namchain-bench` measuring RTF per model/quantum ✔; lookback calculator + golden test vs. hand-computed A2 numbers ✔; **`namchain-render` offline WAV→WAV with file I/O — not yet started** | Builds green locally (aarch64 native; x86_64 + armv7 cross defined in CI, not yet run); bench table for A2-Full/A2-Lite/IR on this Pi 500 ✔; §11 table revised with real numbers ✔ |
| **M1** | `libnamchain` core: RT primitives, arena, command ring, epoch reclamation; static DAG render; `nc.input/output/gain/split/mix/eq/delay`; `alsa_raw` + `pipewire` + `null` + `file` backends; daemon skeleton; **A2 planar kernels live on AArch64 with the bit-identity parity gate in CI (§4.2, §7.8)** | Plays a guitar through 2 chains at 64 samples; 1 h zero xruns; allocation interposer clean; latency measured with `jdelay`; **parity test green at release flags** |
| **M2** | Graph compiler + memory planner + patch protocol; transition ops (param ramp, bypass fade, dual-render rewire); `namchain-render` edit timeline; **glitch detector** | Torture render passes all 4 metrics at all quanta/rates for non-NAM nodes; RTSan clean in CI |
| **M3** | `nc.nam` + `nc.cab.ir`: registry, loader, capability probe, instance tokens, input rings, **warm-start replay**, warming policy engine, deferred commits; telemetry (`rtf`, `warm_through`, `estimated_switch_ms`) | **Flag demo:** A/B/C model switching with zero click and exact state continuity (< −60 dBFS), p99 ready ≤ 5 ms, 16 resident models; `maxmodels` vs `instant` modes measured |
| **M4** | Optional parallel rendering; governor/degradation ladder; slimming governor; watchdog + flight recorder | Soak 8 h with edit storms, 0 xruns, degradation ladder exercised; parallel mode passes every test or stays off |
| **M5** | Control surface: C ABI freeze, shm seqlock for continuous params, OSC bridge, `namchainctl`; fuzzing; upstream patches #1/#2 (§7.8); shared-weights experiment | Protocol conformance + fuzz clean; slimmable A2 submodels run the fast path with validated numerics; published "how many models" report |
| **M6** | macOS (CoreAudio) then iOS (RemoteIO + workgroups, static node registry); XCFramework; ARMv7 build + NEON runtime dispatch; Debian packages (arm64/armhf) | Engine runs on all four target classes; Apple latency/CPU report; armv7 resident-budget guidance |
| **M7** | Hardening: 24 h soak, thermal/throttle response, device-loss recovery, ARMv7 tuning, docs (`architecture`, `node-sdk`, `protocol`, `transitions`, `tuning`, `benchmarks`) | All §1 success criteria pass on bare metal; docs complete; release artifacts |

Critical path to the headline claim is **M0 → M1 → M2 → M3**; M4–M7 add scale, platforms, and hardening.

---

## 13. Edge cases and failure modes

| Case | Behaviour |
|---|---|
| `condition_dsp` is an LSTM (unbounded memory) | Marked non-replayable; crossfade-only switch, `exact: false` reported; longer default fade |
| Edit changes both the model and its upstream | History flagged `foreign`; crossfade-only, `exact: false`; parameter-only upstream changes may use `replay_with_params` |
| Slimmable model (`SetSlimmableSize`) | Worker-only call; treated as structural swap with crossfade; never on the audio thread (it allocates, G10) |
| `SlimmableWavenet` submodels (not containers) | `GetPrewarmSamples()` returns 0 and the A2 fast path is not selected; the engine uses its own lookback calculator and reports the model's fast-path status (measured 3.6× loss on A2-Lite). Fixed upstream by §7.8 patch #1 |
| Container mixing LSTM and WaveNet submodels | Whole container marked non-replayable (`Unbounded`); crossfade-only switch, `exact: false`. Real example: the core's own `slimmable_container.nam` |
| Model sample rate ≠ engine rate | Compiler inserts `nc.resample` (declared latency + PDC) or rejects the node with a reason |
| Quantum/period change at runtime | Recompile off-thread, `Reset()` instances on a worker, crossfade; no gap |
| Device unplug / lost | `device_lost` event, engine continues on a null clock (no crash, no partial graph), re-bind on replug with a fade |
| CPU overload | Governor ladder: suspend warming → lengthen fades → slim → crossfade-bypass optional nodes → mute + report. Never a dirty or gapped output |
| Malformed `.nam` / unsupported version | Structured load error; node stays in `failed` (passthrough); chain unaffected |
| Third-party node returns NaN/Inf or overruns | Per-node guard crossfades it out, marks failed, reports; engine keeps running |
| Edit storm | Coalescing (params last-writer-wins; structural serialised, one in flight per chain); bounds transition cost |
| Graph too large / cost exceeds budget | Compile-time rejection with a reason, or admitted with a shorter fade; never a dropout |
| Silent input | Replay skipped; candidates fall back to cached silence state (cheap, exact for silence) |
| ARMv7 memory ceiling | Resident-model budget enforced at load time; LRU eviction instead of OOM |

---

## 14. Risks, assumptions, open decisions

**Risks (with mitigations)**
1. ~~**No shell ⇒ no build** (§0).~~ **Closed 2026-09-11.** The shell works under the
   `danger-full-access` file policy, the toolchain is installed, and M0 builds and benchmarks run on
   the host. Residual risk: the host has no usable sandbox backend, so reverting the file policy to
   `workspace-write` disables shell execution entirely (§0).
2. **Cold-switch cost may exceed the deferral budget** — *largely resolved by measurement, 2026-09-11.*
   Replay is 132.2 ms of audio, so the cost is `132.2 ms × RTF`: on `a2_fast` that is **14.2 ms** of
   CPU for A2-Full, at/over the 15 ms `max_defer_ms` budget — the original risk was real. With PR #313
   (§7.8) it is **6.6 ms**, and 1.15 ms for A2-Lite, so cold switches fit the deferral budget with
   room and the `instant` warming policy becomes affordable. Residual: the numbers are for one
   instance on an idle core; under load with several candidates the token bucket (§7.5) still decides.
   Mitigations that remain available: hot-set warming, incremental catch-up, silence skip,
   `maxmodels` mode, slimming.
3. **Parallel rendering correctness** — largest RT risk. Mitigation: off by default; admitted only on
   soak + glitch passes.
4. **Upstream drift** — the core is active (commits in Aug 2026). Mitigation: pinned commit + patch set
   + golden tests; upgrade only through a PR-shaped review with numerics diffed.
5. **Memory-bound performance ceiling** (issue #124). Mitigation: weight sharing, quantum tuning,
   fast-path coverage; measured, not assumed.
6. **iOS plugin story is constrained** (G17). Mitigation: ABI designed for static linking now;
   data-only plugins; decide the third-party story before M6.
7. **ALSA-without-libasound** is real work (~600–900 lines of ioctl plumbing) and loses the
   `plug`/`dmix` conveniences. Mitigation: `null`/`file`/`pipewire` exist by then, so M1 isn't blocked;
   fall back to libasound if you accept LGPL dynamic linking.
8. **Thermals on Pi 5** (throttle from 80 °C) can silently destroy the CPU budget. Mitigation: health
   loop + governor + mandatory cooling for always-on use.
9. **Measurement host has no capture-capable audio device** (§0). It *is* bare metal of the target
   CPU class (Pi 500 / Cortex-A76), so CPU figures in §11 are target figures, but nothing about
   xruns, true round-trip latency or thermals under sustained audio load can be measured until a USB
   interface is attached. Mitigation: §1 criterion 3's "0 xruns over 1 h" is claimed only after that
   device exists; until then the internet-facing claims rest on §11's CPU budget.

**Assumptions** — engine runs at 48 kHz internally; graphs are DAGs; permissive-only dependencies (with
the ALSA note in §4.3); no UI in this project; `.nam` files are user-supplied, not redistributed; Linux
is the reference platform for RT behaviour.

**Open decisions you may want to change** (each has a default in the plan): default warming policy mode
(`balanced`); `fade_ms` default (5 ms) and `max_defer_ms` (15 ms); whether to accept LGPL
`libasound`/JACK backends; whether §7.8 patch #3 (shared weights) is worth carrying a standing fork
patch; project name (`namchain` is a placeholder).

---

## 15. Out of scope

Any UI/UX, MIDI/USB-host and footswitch integration, VST3/LV2/CLAP hosting, model training or
conversion, network audio, cloud/model-store integration, iOS/macOS app shells (the engine only),
out-of-process plugin sandboxing, and any redistribution of third-party `.nam` models.

---

## 16. Sources

Primary (fetched and read in full during planning):
- NAM core: `NAM/dsp.h`, `NAM/dsp.cpp`, `NAM/model_config.h`, `NAM/get_dsp.h`, `NAM/container.h`,
  `NAM/sequential.h`, `NAM/slimmable.h`, `NAM/film.h`, `NAM/lstm.h`, `NAM/linear.{h,cpp}`,
  `NAM/ring_buffer.h`, `NAM/wavenet/{model.h,model.cpp,params.h,detail.h,a2_fast.h,a2_fast.cpp,`
  `slimmable.h,slimmable.cpp}`, `tools/{benchmodel.cpp,render.cpp,bench_a2_fast.cpp,bench_linear.cpp,`
  `BENCHMARK_LINEAR.md}`, `tools/test/{allocation_tracking.h,test_wavenet/test_real_time_safe.cpp}`,
  `docs/wavenet_walkthrough.rst`, `docs/nam_file_version.rst`, `CMakeLists.txt`, `LICENSE`, `README.md`
  — https://github.com/sdatkinson/NeuralAmpModelerCore (pinned @G2)
- NAM plugin (swap behaviour, PR #627, PR #662) — https://github.com/sdatkinson/NeuralAmpModelerPlugin
- Core issues/PRs on prewarm + cached state: #238, #260, #266, #285, #300, #318/#319, #124
- Gemma/Elk Sushi overview and 0.11.0 release notes — https://elk-audio.github.io/elk-docs/ ,
  https://forum.elk.audio/t/sushi-0-11-0-feature-release/539/1
- LV2 worker extension — https://lv2plug.in/ns/ext/worker ; mod-host — https://github.com/mod-audio/mod-host
- Bencina, "Real-time audio programming 101" —
  http://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing
- RealtimeSanitizer — https://clang.llvm.org/docs/RealtimeSanitizer.html
- Linux RT/RT audio config — https://wiki.linuxaudio.org/wiki/system_configuration ,
  https://jackaudio.org/faq/linux_rt_config.html
- Raspberry Pi frequency management/thermals —
  https://www.raspberrypi.com/documentation/computers/raspberry-pi.html
- Apple: workgroups, AVAudioSourceNode realtime-safe block, App Review 2.5.2, Apple-silicon tuning —
  https://developer.apple.com/documentation/audiotoolbox/understanding-audio-workgroups ,
  https://developer.apple.com/documentation/avfaudio/avaudiosourcenode ,
  https://developer.apple.com/app-store/review/guidelines/ ,
  https://developer.apple.com/documentation/apple-silicon/tuning-your-code-s-performance-for-apple-silicon
- Commercial prior art: Line 6 Helix preset spillover (forum + 3.80 manual); Neural DSP Quad Cortex
  Development Update #18 — https://neuraldsp.com/quad-cortex-updates/quad-cortex-development-update-february-15th
- Model licensing: https://www.tone3000.com/terms , https://www.tone3000.com/api/terms

Local environment facts were established by direct inspection (no shell available): `/proc/cpuinfo`
(Cortex-A76, part `0xd0b`), `/etc/os-release`, `/etc/apt/sources.list.d/debian.sources`,
`/proc/1/comm`, and package-path probes under `/usr/share/doc/`.

