# namchain — a glitch-free, headless NAM signal-chain engine

**Status:** approved and in progress. **Phase 0, M0 and M1 are complete** — the repo builds green in CI
on all five jobs (§12), §11 carries *measured* numbers rather than estimates, and every M1 acceptance
clause is met on the reference interface (§9.5) except one of criterion 3's clauses. That clause was
re-attacked on 2026-09-16 by moving the host to a PREEMPT_RT kernel with the audio cores isolated and
tickless: **five of the six clauses now pass, including the driver-xrun clause that failed 8 times an
hour** (0 xruns in 4 h 10 min), and the survivor is `max block < deadline`, which is memory-stall
shaped rather than scheduling-shaped and is not fixed by anything in §9.3. The result, its limits and
the arms behind it are in `bench-results/rt-piv-summary.txt`, run from `plan-rt-piv.md`. **M2 is next**,
and its first task is the glitch detector, by the standing instruction below.
**Author:** planning session (research grounded in the sources cited in §2 and §16), resumed for
implementation.
**Updated:** 2026-09-16. piv now runs `7.0.0-1017-raspi-realtime` with `isolcpus`/`nohz_full`/`rcu_nocbs`
on cores 2–3; §9.3, §11, §12 and §14 carry the measured consequences, and `docs/tuning.md` — promised
since M0 — is now written from those measurements. §0's 2026-09-13 note still applies to the *earlier*
host numbers: every measurement carries the toolchain and kernel it was taken on.

---

## How to resume

**Environment verified and ready (2026-09-11).** Phase 0 is complete on this host and M0 has begun.
§0 records the verified inventory, the corrections to what was previously believed about this box,
and the commands that actually work.

```sh
echo ok; uname -m; nproc          # ok / aarch64 / 4
cmake --version; ninja --version  # 4.2.3 / 1.13.2
```

**The host was upgraded under the project on 2026-09-13** (Ubuntu 24.04.4 → **26.04 LTS**, kernel
6.8.0-1064-raspi → **7.0.0-1017-raspi**, GCC 13.3 → **GCC 15.2**, cmake 3.28.3 → 4.2.3, ninja 1.11.1 →
1.13.2, Python 3.12 → 3.14.4). Three consequences, in order of how much they cost:

1. **The device number moved.** Card numbers are assigned by probe order, and the SSL 2+ Mk II is now
   **card 2** (`hw:2,0`), not card 4 — card 4 is a `vc4hdmi` output. Every command in this file that
   names `hw:4,0` is a command from the old boot; check with `aplay -l`/`arecord -l` and use the card
   the interface actually has. The interface itself is unchanged (§9.5's measurements stand as
   *device* measurements, but see 3).
2. **The 1 h criterion-3 soak was lost with it.** It was running when the machine rebooted; `/tmp` was
   cleared, taking its JSON, log and evidence file. Nothing was salvaged — the run had reached ~15 of
   its 60 minutes. It is being re-run, and its output now goes to `bench-results/` rather than `/tmp`
   precisely so that a reboot, a stall or a crash leaves evidence rather than silence.
3. **Every "measured on this host" number now carries a toolchain it was not measured on.** §11's
   figures, the golden tolerance notes in `CMakeLists.txt` and the latency sweep were all taken under
   GCC 13 / kernel 6.8. On AArch64 the A2 kernels are NEON intrinsics, so the plan expects the golden
   difference to stay exactly 0 across the compiler change — but that is a re-measurement, not an
   assumption. **Re-measured 2026-09-13: it is still exactly 0**, so the bit-identity claim survived a
   major compiler version and matches the §10.2 table's new row.

**Re-verification on the new toolchain (2026-09-13), because a rebuilt tree is not the same claim as a
green one:** a fresh `build/` configures and compiles clean under CMake 4.2.3 and **GCC 15.2 with
`-DNAMCHAIN_WERROR=ON`**, `ctest` is 4/4, the unit binary is **120 cases / 230 719 assertions**, and
`golden_a2_lite` reports `max|a-b| = 0`. Two things the newer compiler broke, both now fixed and both
recorded where they happened:

- `tests/test_rt_check.cpp` exercised the allocation interposer through a local `std::vector` whose
  allocation was never read. GCC 15 compiles that to a bare `ret` at `-O3` — the allocation is elided
  under N3664 — so the test that proves the instrument *can* see an allocation was passing while
  watching nothing. It now calls `::operator new`/`::operator delete` directly, which cannot be
  elided. This is the one failure mode that flatters every other result.
- The glitch detector's dropout rule counted a black run as a dropout only where the oracle had energy
  *sample by sample*; a 1 kHz sine crosses zero a thousand times a second, so a half-second dropout
  fragmented into ~1 ms pieces and passed. The oracle is now asked about the run as a whole.

What survived the upgrade, verified after it: `/etc/security/limits.d/99-namchain-audio.conf`,
`sudo -n chrt -f 80` (works), `ulimit -l` = unlimited, `performance` governor, and
`namchain-realtime-tune.service` (enabled and active). One thing got stricter: `/dev/vcio` is root-only
now, so `vcgencmd get_throttled`/`measure_temp` need `sudo -n` — the health loop and the soak evidence
file must call it that way (§9.3).

**Toolchain that had to be installed** (cmake and clang were already present, contrary to the earlier
note):

```sh
sudo apt-get install -y ninja-build pkg-config libpipewire-0.3-dev libasound2-dev clang-20
```

`clang-20` is needed for RealtimeSanitizer (§4.2, §10.4). It was installed because Ubuntu 24.04's
default clang 18 rejects `-fsanitize=realtime`; after the 2026-09-13 upgrade the default is clang 21,
which supports it, but the pinned `clang-20` is still present and is what CI installs, so the local
and CI RTSan runs continue to use the same compiler. `libeigen3-dev` is *not* needed — Eigen is
vendored via the core's submodule.

**RT privileges are configured** (they were entirely absent before):
`/etc/security/limits.d/99-namchain-audio.conf` grants `@audio` `rtprio 95` and `memlock unlimited`,
and `namchain-realtime-tune.service` sets the `performance` governor and disables USB autosuspend at
boot. (An earlier revision of this file called that unit `namchain-cpu-performance.service`; that name
does not exist on the box — `systemctl cat namchain-realtime-tune.service` is the real one.) Verified
both through a fresh login session and through a transient systemd service with `LimitRTPRIO=95` — the
path `namchaind` will use. A harness-spawned shell that predates the limits file still cannot `chrt`,
so RT tests must run via a login session or a systemd unit. Both survived the OS upgrade.

**Vendored NAM core:** `vendor/NeuralAmpModelerCore` as a git submodule pinned at G2, with
`Dependencies/eigen` and `Dependencies/AudioDSPTools` initialised recursively.

**Artifacts produced on this host (M0 and M1):**

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build                    # unit, soak_cli, golden_setup + golden_a2_lite
./build/namchain-tests                    # 110 cases, all passing
./build/namchain-bench build/generated/A2-Full.nam --quantum 32,64,128,256

# The soak harness with no hardware at all: a paced clock, or a WAV file in and out.
./build/namchain-soak --backend null --seconds 10 --instances 4 --model build/generated/A2-Full.nam
./build/namchain-soak --backend file --model build/generated/A2-Lite.nam in.wav --output out.wav

# The device soak, and the round-trip measurement, both under SCHED_FIFO.
# Card numbers are assigned by probe order and DO move between boots and kernel upgrades: read the
# interface's own card from `arecord -l`/`aplay -l` first. It was `hw:4,0` before the 2026-09-13
# upgrade and is `hw:2,0` after it. The soak writes its evidence to bench-results/, not /tmp.
CARD=$(arecord -l | sed -n 's/^card \([0-9]*\): II \[SSL 2+ Mk II\].*/\1/p' | head -1)
sudo -n chrt -f 80 ./build/namchain-soak --device "hw:$CARD,0" --capture-channels 8 --playback-channels 6 \
  --quantum 64 --periods 4 --seconds 3600 --instances 4 --model build/generated/A2-Full.nam \
  --self-oracle --json
sudo -n chrt -f 80 ./build/namchain-latency --device "hw:$CARD,0" --capture-channels 8 \
  --playback-channels 6 --quantum 32 --periods 4 --output-channel 2 --measure 0,4 --runs 6
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

**Order of work, still standing:** M0 ✔, M1 ✔, then M2 — and within M2 the glitch detector is written
*before* the transition machinery (§10.3), because a transition you cannot measure is a transition you
cannot claim. The one M1 item left open is the 1 h criterion-3 soak, which needs the machine to itself
for an hour and nothing else. As of 2026-09-13 that item is also the *re*-run: the first attempt was
killed 15 minutes in by the OS upgrade's reboot, and the toolchain changed underneath the build, so M1
exit now reads "rebuilt and re-verified on GCC 15 / kernel 7.0, then one clean hour". A soak run's
output belongs in `bench-results/`, never `/tmp`: the reboot took the JSON, the log and the evidence
file with it, and a lost hour is indistinguishable from an hour that was never spent. **The heartbeat
taught the same lesson a second way, on 2026-09-14:** the job wrote timestamps in a background subshell
and killed it with `$!`, but the capture and the next command shared a line, so `$!` went into that
command's environment instead of the shell's, `kill` was given no argument and did nothing, and the
writer outlived the soak by 17 hours -- 975 extra timestamps in the evidence file, which had to be
trimmed back to the run window before it meant anything. Evidence writers must be killed by a recorded
PID (`trap ... EXIT`, or a PID file), never by a variable that only *usually* holds one.

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
| Debian 12, container, pid 1 = `tini` | **Ubuntu 24.04.4 LTS, bare metal, pid 1 = `systemd`** (now 26.04 — see the upgrade note above) |
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
  After the 2026-09-13 upgrade this is `7.0.0-1017-raspi`, still not PREEMPT_RT, and the absence of
  `/sys/kernel/realtime` is the check that still holds (§9.3).
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
3. **Throughput and real-time integrity (Pi 5, bare metal, 48 kHz, 64-sample quantum).** ≥ 4 concurrent
   A2-Full (8-channel) **or** ≥ 12 concurrent A2-Lite (3-channel) audible NAM models, with **all** of:
   *(the model count is a property of the reference host, not of the engine: on ARMv7 the same clause
   is read as **one A2-Full within 90 % of one core**, which is what that class of CPU can carry —
   measured on tib 2026-09-16, 1 × A2-Full at q64 uses 58.5 % of a core at p99 with 0 xruns over a
   full hour; see §9.4 and §11)*
   p99 block time ≤ 50 % of the 1.33 ms deadline; max block time < the deadline; **zero driver-reported
   xruns over 1 h** — `-EPIPE` or `SNDRV_PCM_STATE_XRUN`, counted, never silently recovered from;
   no thermal throttle flags raised; and the captured output **sample-identical** to the deterministic
   offline render of the same input. Established from measurements by `namchain-soak`, not by ear: the
   procedure and the reasoning are in §10.5.
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
- Every buffer is preallocated at `maxQuantum` and page-touched at plan build; `mlockall(MCL_CURRENT)`
  is called **after** the plan and the stream buffers exist, and never with `MCL_FUTURE` — a finite
  `RLIMIT_MEMLOCK` turns `MCL_FUTURE` into `std::bad_alloc` on the next mapping (§14.10).
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
`libasound` and JACK's client library are LGPL-2.1; PipeWire and WirePlumber are MIT. The plan
therefore ships:
- `nac_io_file` / `nac_io_null` — no deps; used by CI and offline rendering (mandatory anyway for
  determinism).
- `nac_io_pipewire` — MIT client, default for desktop/coexistence.
- `nac_io_alsa_raw` — **direct kernel UAPI ioctls on `/dev/snd/pcmC*D*p`** (hw params, prepare,
  writei/readi, card enumeration via control ioctls). No `libasound`, hence no LGPL. This is the
  appliance backend and the one that matters for latency on a Pi.
- `nac_io_jack` — deferred, and **not for the reason this document used to give.** Checked against the
  sources rather than assumed: JACK's *client* library is LGPL-2.1-or-later in both implementations —
  JACK1's `COPYING` says the `libjack/` tree "is licensed under the LGPL **in order to allow for
  proprietary usage of Jack**", and JACK2's `common/jack/jack.h` carries an LGPL-2.1-or-later header.
  Only the *server* (`jackd`) and the tree around it are GPL-2.0, and a separate process is not a
  derivative work, so that side does not reach us. Dynamically linking a proprietary client against
  libjack is the intended use and is routine — that is how proprietary DAWs ship. So the real
  obstacles are, in order of weight:
  1. **Architecture, not licensing.** JACK is a *server*. Being a client means the period, the buffer
     size and the scheduling belong to `jackd`, not to us — and §1 criterion 3 is a guarantee about
     exactly those three. Handing them to a third-party server is a downgrade in the one area this
     project is trying to be strong, whatever the license says.
  2. **Our own policy.** §4.3's rule is *permissive-only*, and LGPL is weak copyleft, so JACK is
     excluded by a self-imposed constraint rather than by law. If the policy is ever relaxed, this is
     the dependency to relax it for.
  3. **Static linking into an appliance image.** LGPL-2.1 §6 requires that a recipient be able to
     relink the work against a modified library: shipping object files or a relinkable form, plus the
     library's source. A stripped, signed, self-contained image is the case where that becomes real
     per-release work — and it is the concrete blocker, not the dynamic-linking case.
  The interoperability this node was meant to buy is available under MIT anyway: PipeWire ships a JACK
  implementation (`pipewire-jack-audio-connection-kit`, license field **MIT**), which presents a
  drop-in `libjack`. A user on a PipeWire desktop is therefore served by the MIT desktop backend, and
  `nac_io_jack` is only needed by someone who specifically wants to be a client inside another
  machine's `jackd` graph.

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

**Built so far (2026-09-19, slices A1 and A2 of docs/ui-integration-plan.md):** the document half of the
chain model — a rig spec may name several chains, each with a client-owned id, an input and output
device channel, and its own graph — plus the identity model that goes with it: `element` is the
client's name for a block and is stored and returned verbatim, `role` says which node of it a node
is, and `id` is minted by the engine when a client does not write one. The parser, the document-level
validation (with the chain named in any failure) and the tests are in. The engine half is in as well: `namchaind` compiles one plan per chain (up to
eight), binds each chain to the device channels it names (two chains writing one channel are summed),
gives each chain its own generation, mute and transition, applies a document with the chains
crossfaded **one at a time** so the peak cost stays at two of the largest chain, and leaves a chain
whose graph did not change unreplanned and unfaded. A chain the new document does not have stops
immediately. Still to come: the protocol that puts a whole document on the wire and reads one back
(`rig.apply`/`rig.get`, A3), and per-chain patching beyond the request-level `chain`.

### 5.2 Compile pipeline (control thread)

`JSON rig` → parse/validate → IR → lint (cycles, arity, param ranges, missing models) → **lower**
(expand `nc.nam` slots, insert resamplers/PDC delays, insert mixers for fades) → **memory plan**
(liveness analysis + interval colouring over intermediate buffers, one arena, zero steady-state
allocation) → **cost model** (per-node predicted µs from the measured cost table) → immutable
`RenderPlan` (node array in topological order, buffer bindings, fade/mixer slots, `admission` record).

Compile is transactional: any error leaves the running plan untouched and returns a structured error.
A plan is published by an atomic index store; the previous plan is retired via §3.2/6.

**Implemented at M1** (`src/graph/plan.cpp`): validation (unknown types, duplicate ids, dangling
connections, port ranges, two connections into one input port, cycles via Kahn), deterministic
topological order, **liveness + interval colouring** so a serial chain of six gains collapses to a
handful of buffers rather than one per node, buffer allocation out of an `rt::Arena`, and all pointer
binding done at compile time so `execute()` is allocation-free. Unconnected input ports are bound to
one shared zeroed buffer rather than trusting a node to ignore a null pointer, and every node's output
array is null-terminated so fan-out nodes can walk it. Tests cover rejection of malformed and cyclic
graphs, topological ordering with nodes declared out of order, buffer reuse, silence on unconnected
ports, determinism, zero allocations inside an `RtScope`, and `nc.nam` producing the same samples as
running the model directly.

**Rigs are JSON** (`src/graph/rig.cpp`, §5.1's `Graph` IR): `{"nodes": [...], "connections": [...]}`,
where a node's `id`, `type`, `bypass`, `element` and `role` are reserved and every other key is a
parameter — a number or boolean becomes a numeric param, a string becomes a string param, so
`"model": "amp.nam"` needs no wrapper. Explicit `"params"`/`"strings"` objects are also accepted. A
document may carry several chains (`{"name": …, "chains": [{"id", "input_channel", "output_channel",
"nodes", "connections"}]}`), and a bare `{"nodes", "connections"}` is one chain called `"main"`; a
node's `element` is the client's opaque name for the block it belongs to, kept verbatim, and the
engine mints the node's `id` if the client does not supply one (docs/rig-pipeline.md §2). A
structured value (an array) is rejected rather than dropped, because dropping it would compile and
run while quietly ignoring what the rig author meant. Relative paths in string params resolve against
the rig's own directory first, so an example rig copied next to its models needs no editing (the
committed `rigs/*.json` are copied into the generated-fixture directory by CMake for exactly this
reason). `chain_graph(model, n)` builds the `nc.input → n × nc.nam → nc.output` chain that
`--model --instances N` means, so the CLI, the offline renders and the unit tests all go through the
same compiler rather than each having its own loop.

**Feedback is a schedule property, not a node's memory** (`nc.unit_delay`, marked `block_delay` in its
vtable). §5.1 wants cycles to be a compile error, which leaves the question of how a feedback loop is
ever expressed. The answer here: a node whose input is read as of *the previous block* breaks the
cycle, because an edge into it is not a scheduling dependency — it reads whatever its input buffer
already holds. The compiler drops those edges when building the dependency graph, schedules
block-delay nodes before everything else, and pins any value one of them reads to the end of the block
so its buffer survives until the next block's early read. `nc.unit_delay` is therefore a plain
pass-through whose one block of delay comes from *when* it runs; a stateful version would add a second
block on top of the one the schedule already provides. This forced one more change: `nc.input` samples
the incoming block at its own point in the schedule (via a `set_block_input` hook `execute` calls
before any node runs) rather than having its buffer pre-filled, or a delay fed straight from the input
would come out as zero. The reference interpreter implements the same rule, deliberately — it exists to
check the compiler's colouring and binding, not to second-guess a documented semantic.

**The compiler has an oracle** (`src/graph/reference.cpp`): a naive evaluator of the same IR that gives
every value its own storage, orders nodes by depth-first post-order instead of Kahn's algorithm, and
reuses nothing. Comparing a plan against a second run of that plan proves determinism and nothing else;
comparing it against this catches schedule, colouring and binding bugs — which is what the soak's
`--graph --self-oracle` does, block for block, in process. A rig reports both counts, so "the colouring
works" is a measurement: the guitar chain needs 3 buffers where the naive layout needs 6, two parallel
chains 4 where it needs 7.

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
  error** rather than risking a dropout.- **Never-silence guarantee**: if any op cannot complete safely, the engine keeps the current plan
  rendering and reports; the only path to silence is an explicit `engine.mute`/`panic`.
- **Failure isolation**: a node returning non-finite samples has its output crossfaded to a bypassed
  path and is marked failed (per-node guard, always on for third-party nodes).

**Built so far (2026-09-19):** the `smooth` row (a `set_param` ramps inside the node), the bypass row
(a `NodeSpec::bypass` flag: the node keeps running for state warmth and its output becomes its dry
input; only 1->1 nodes have a dry path), the failure-isolation row (a per-node non-finite scan takes a
faulty node out of the path and reports it), the dual-render crossfade of the rewire/add/remove row
(`rt::Crossfade`, `fade_ms`, one transition in flight with a coalescing pending slot, old plan
retired), and the structural patch ops that build the modified graph (`graph::apply_patch`:
add/remove/connect/disconnect/rewire/swap_model/bypass, applied all-or-nothing to a copy). Still
outstanding: `tail_hold` for a bypassed node with a tail, warm-start model swapping (M3), and of the
two rules above only coalescing exists — admission control does not. `docs/transitions.md` is the
operational account.

**The fade's law is wrong for correlated signals, and that is audible** (measured 2026-10-07,
`bench-results/e2-fade-thump.txt`). `rt::Crossfade` is equal-power so that `from^2 + to^2 == 1`, which
is right when the two signals are uncorrelated and +3 dB at the midpoint when they are not — the power
of `g1*a + g2*b` is `g1^2 + g2^2 + 2*rho*g1*g2`. Every Lite/Full handover measured (rho 0.91-1.00)
bumped +2.6 to +3.0 dB, heard as a thud, and none of the glitch detector's four rules can see a level
bump: it is continuous, is not a dropout, adds no out-of-band energy and converges. A bypass toggle is
the same case by construction (the two sides are the same audio), so this is every `nc.xfade` of a
bypass flip, and every whole-chain fade between two similar rigs; a fade between two *different* rigs
(rho -0.7 to +0.5) is the case the current law is for. **Fixed 2026-10-07, with the measurement the entry above asked for.**
`rt::Crossfade` now builds five weight tables in its constructor (rho = 0, 0.25, 0.5, 0.75, 1), each
dividing both weights by `sqrt(1 + 2*rho*g1*g2)`, and `start(frames, rho)` picks the nearest —
measuring rho from the first block when the caller does not know it, which no call site does: a
bypass of a NAM model is two unrelated signals, a bypass of a gain is the same audio twice, and an
EQ ramp is the same audio through two filters. Measured through this class, through `nc.xfade` and
through a plan-level transition: **+3.0 dB before, 0.00 after**, with the unrelated pair (which
equal power exists for) unchanged at -0.04 dB. `tests/test_crossfade.cpp` has all three, and the
suite passes on the Mac and in the aarch64 build (341 cases, 270,069 assertions).

A second defect came out of it: `prewarm` runs one silent block through the whole plan and only
`prewarm_fresh` honoured `skip_prewarm`, so `nc.xfade` — marked `skip_prewarm` so its fade would
*not* be spent on silence — entered its first audible block 64 frames into a 240-frame fade. Every
local transition was 3.7 ms of crossfade where 5 ms was asked for. `prewarm` now skips those nodes,
and a test asserts where the fade's midpoint lands.

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
`nc.pan`, `nc.eq.biquad`, `nc.delay`, `nc.cab.ir` (AudioDSPTools' partitioned convolver, zero-latency, lookback-aware),
`nc.nam` (§7), `nc.gate`, `nc.limiter` (declares latency), `nc.reverb` (declares tail; M5),
`nc.unit_delay`, `nc.resample` (rate adapter, declares latency), `nc.meter` (telemetry tap,
audio-transparent), `nc.bypass` (wrapper).

**Implemented at M1** (`src/nodes/builtin.cpp`): `nc.input`, `nc.output`, `nc.gain`, `nc.split`,
`nc.mix`, `nc.bypass` (added with the bypass work), `nc.nam`, `nc.cab.ir`, `nc.eq.biquad`, `nc.delay`,
`nc.unit_delay`. Notes that are decisions rather than transcriptions:

- **A node converts time and frequency to samples once, at build.** `CompileOptions` carries the
  engine rate and the maximum quantum, so `nc.delay ms` → samples and an EQ's coefficients are
  control-thread work, not per-block arithmetic. It is also the hook a resampler will need.
- **`nc.eq.biquad`** is RBJ audio-EQ-cookbook: `peaking` (default), `lowshelf`, `highshelf`,
  `lowpass`, `highpass`, `bandpass`, `notch`; `freq`, `q`, `gain_db`. DF2T, two state floats.
  Shelves have no bandwidth, so RBJ parameterises them by *slope*; `q` is used as that slope, clamped
  to the range that stays monotonic, rather than accepted and ignored. Out-of-range settings (a
  frequency at or above Nyquist, `q <= 0`) are **rejected, not clamped** — a filter that silently
  ignores a 30 kHz request is a bug report about "the EQ does nothing", and one that wraps is a bug
  report about noise.
- **`nc.delay`** takes `samples` or `ms` and nothing else; a sub-sample delay or a typo'd
  millisecond value is refused instead of approximated. The ring is `delay + 1` slots so the read
  pointer is visibly `delay` behind the write pointer rather than coinciding with it.
- **`nc.cab.ir`** loads a mono `.wav` IR or a `Linear` `.nam`, and convolves it with
  the core's `nam::Linear` (Auto: direct for short IRs, FFT for long ones). From 79939b4 to
  2026-09-26 it used `dsp::PartitionedConvolution` from rikkus/AudioDSPTools `partitioned-ir`, which
  spreads the partition multiplies across the quiet callbacks; that fork is deprecated, so it is back
  on `nam::Linear`. The cost of going back is on `tib`: at 8192 taps `nam::Linear` was p99 62.5% of a
  64-frame deadline and 117% of a 32-frame one, against 10% and 16% (`bench-results/tib-cab-ir.txt`).
  The IR is prepared the way the NAM plugin
  prepares one, less its -18 dB gain: cubic-resampled to the engine rate and truncated at 8192 taps.
  The tap order is the natural one, so a WAV's first sample is the direct sound; a test pins that
  with a two-tap IR, because a reversal is inaudible as an error and audible as a wrong-sounding
  cabinet. The FFT path is not bit-identical to a direct FIR; the tests hold it to -110 dB of the
  peak at 2 to 8192 taps and at block sizes that straddle its transform boundary.
- **`nc.nam` refuses a rate mismatch.** The core has no rate guard of its own: a 44.1 kHz model
  driven at 48 kHz runs happily and is simply wrong. `nc.cab.ir` resamples instead, since an IR is
  data that can be converted and a model is not.
- **Arity is per-spec**: `nc.split` fans out to the rig's `"outputs"` and `nc.mix` sums its
  `"inputs"`, both defaulting to 2 so a rig written before this means exactly what it did. This is
  per-spec arity in the vtable (`NodeType::arity_param`, bounds 1..64), resolved from the spec by
  validation, the compiler and the oracle together so the three cannot disagree;
  `rigs/four-chains.json` is the example. Both process functions already walked their port array to
  the `nullptr` terminator, so the four-way fan-out needed no new render-path shape -- only the mix
  sum changed from `in[0] + in[1]` to an accumulation.

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

Primary: Unix domain **`SOCK_SEQPACKET`** (message boundaries + filesystem permissions). Remote
control: an **opt-in TCP listener** carrying the same JSON bodies behind a u32 little-endian length
prefix, with a mandatory pre-shared token in `engine.hello`, connection/message/output caps and no
TLS (a tunnel is the documented answer off-LAN) — built as slice C of docs/ui-integration-plan.md and
specified, with its threat model, in docs/daemon.md. Continuous
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

### 9.1 Backends (identical interface) — **implemented at M1**

`include/namchain/io/backend.h` is the interface (§9.1's "identical interface", now a real one):
`AudioBackend` with `info/start/stop/wait_capture/read_capture/write_playback/driver_xruns/finish`,
byte-oriented and direction-symmetric because that is what the ioctls do (`namchain/pcm.h` converts).
`open_backend(config, kind)` is the only way to get one, and a backend that does not exist **fails by
name** rather than falling back — otherwise "I measured it on PipeWire" would not be checkable.

| backend | state | notes |
|---|---|---|
| `alsa_raw` | **done** | adapter over `AlsaRawDuplex`; converts at the boundary and holds nothing, so the code measured on hardware (§9.5) is the code the soak drives |
| `file` | **done** | WAV in, WAV out; the offline instance of the same interface. Bit-identical to `namchain-render` on the A2-Lite fixture, and checked by a CTest |
| `null` | **done** | real-time paced against the wall clock, silence in and discarded out. Clocks off an absolute deadline, so a late period does not make every later one late |
| `coreaudio` | **done** (rest of M6 open) | the engine's output to a CoreAudio output device and its capture from a live CoreAudio input device (`--input-device`) or a WAV on a loop (`--capture-path`), in one process and one clock: the IOProc produces capture periods into an SPSC ring and drains processed ones from another, so the render loop is unchanged. Two *different* devices are joined by a private aggregate (the output is the clock and the HAL resamples the input), because the engine has no resampler. Reading an input device is the one microphone-permission path, so it happens only when the flag is given, and the grant belongs to whichever process spawns the daemon. §9.2's "quantum = device period, clamped" is deliberately departed from: the device's period is measured with a probe IOProc and is the engine's quantum, and `period_size` is a ceiling for it.  The device's IO buffer can be asked for (`--device-buffer-frames`, restored on stop) and the render thread gets macOS real-time scheduling — `SCHED_FIFO` plus a time-constraint window sized to the audio period — which is what makes a 32-frame period (0.67 ms) survivable. Workgroups, `QOS_CLASS_USER_INTERACTIVE` and `.measurement` remain M6, and they are what the iOS target needs |
| `pipewire` | refused by name | MIT, so no licensing question; the work is the graph/stream plumbing, not the licence (see §4.3 for why JACK is the deferred one) |
| `jack` | deferred | LGPL client library deliberately; the reason is architectural, not legal (§4.3) |

Two consequences worth naming. First, the soak harness's stream path is now written once and runs
against a device, a paced clock with no device, or a WAV file — which is what lets CI exercise the
block timers, the deadline gates and the oracle on a machine with no audio at all (`ctest -R soak_cli`).
Second, `BackendInfo::needs_bringup_cycle` is a capability, not a detail: a device's first period after
START is an artefact of filling an empty ring, and running that "bring-up" cycle on a backend with no
rings writes a silent period to the output WAV. That bug is why the file backend's output was one
period longer than the renderer's before it was found.

The **multichannel WAV reader** (`namchain::load_wav`, `src/io/wav.cpp`) came with this: the core's own
loader refuses anything but mono (`ERROR_NOT_MONO`), and a device capture that is going to be replayed
through the engine usually is not. It handles 16/24/32-bit PCM and 32-bit float, any channel count, and
chunks in any order. `load_wav_mono` deliberately still delegates to the core's loader, because the
golden test's whole premise is that our path and the core's `render` decode identical floats;
`tests/test_wav.cpp` checks the two agree on the same bytes.

### 9.2 Clock, rate, latency policy

- **Engine rate = 48 kHz** (NAM models are 48 kHz-native; the core has no resampler and throws on
  mismatch, G11). The I/O layer negotiates 48 kHz with the device; if the device can only do 44.1/96
  kHz, a fixed-latency polyphase SRC at the I/O boundary carries the whole graph (one resampler, not
  one per model) and the added latency is reported and compensated.
- **Quantum = device period**, clamped to [32, 256], fixed for the session. **On full-speed USB the
  practical floor is 48 frames (1 ms)** — see §9.5 — so `alsa_raw` must verify the negotiated period
  rather than trust the request. Device period change ⇒ recompile off-thread + crossfade; instances
  are re-`Reset()` on a worker before handoff.
- Engine algorithmic latency = **I/O queue occupancy** + any declared node/PDC latency; target 64
  samples (1.33 ms) at 48 kHz on Pi 5. Note the correction §9.5 item 8 measured: it is the *occupancy*
  and not the buffer *size* that is in the signal path, so the ring can be made larger for free (it is
  headroom for a burst) while the latency stays a separate, chosen number. Configuring a 4-period ring
  to keep latency down was paying in the wrong currency.

### 9.3 Linux RT tuning checklist (shipped as `docs/tuning.md` + a `namchain-doctor` script)

`/etc/security/limits.d/audio.conf` → `@audio - rtprio 95`, `@audio - memlock unlimited`;
`SCHED_FIFO` 80 via `sched_setscheduler`; `mlockall`; governor `performance`; an RT kernel
(`linux-image-*-realtime`; PREEMPT_RT mainlined 6.12 but Pi OS stock is not RT); `usbcore.autosuspend=-1`;
audio thread pinned to a dedicated core (workers and warm threads excluded; only use `isolcpus` if
IRQs are also moved); `vcgencmd get_throttled`/`measure_temp` in the health loop with a fan/Active
Cooler mandatory for always-on low-latency use (G18). **Do not add `threadirqs` to a PREEMPT_RT
kernel:** `kernel/irq/manage.c` guards that `early_param` with `!defined(CONFIG_PREEMPT_RT)`, so it is
never registered, forced threading is already unconditional, and passing it is a no-op that misleads.
The earlier wording of this checklist listed it, which was wrong.

**Shipped as `docs/tuning.md` (2026-09-16).** The checklist is now a document that carries
the measurements behind each line rather than a paragraph of assertions; `namchain-doctor` is still
outstanding.

**The boot arguments are worth a factor of ten, measured against each other rather than against
a stock kernel (2026-09-16/17).** A balanced four-arm A/B on piv -- same kernel image, only the
command line differing, off/on/on/off, one arm per boot -- gives **10x fewer driver xruns**
(97 vs 9.5), **2.4x fewer late blocks** (1010 vs 428), a worst block of 3.3 ms against 5.8 ms and
3.4x fewer queue drops with the arguments in force. The manipulation check confirms each arm ran
what it claims: the off arms report no isolated CPUs, and the on arms report `.tick_stopped=1` on
both reserved cores. Evidence: `bench-results/piv-isolation-ab.txt`. Caveat: those arms ran 1800 s
rather than the 3600 s intended (the harness reads its duration from the environment, which
systemd does not pass), and all four still fail the p99 gate, so this shows the arguments reduce
the damage rather than remove it.

**The `isolcpus`/IRQ-affinity half is no longer untested — measured 2026-09-16.** piv was moved to
`7.0.0-1017-raspi-realtime` with `isolcpus=managed_irq,domain,2-3 nohz_full=2-3 rcu_nocbs=2-3
irqaffinity=0-1 skew_tick=1`, and the audio interrupt (`xhci-hcd:usb2`) pinned to cpu 2 at
`SCHED_FIFO 85`. The measurement is in `bench-results/rt-piv-summary.txt`, with every arm recording its
own before/after interrupt deltas via `tools/soak/rt-manipulation-check.sh` — the check the 16-arm IRQ
experiment lacked, and the reason its null was uninterpretable. What it bought, over 4 h 10 min of soak
time at ≤ 4 instances: **0 driver xruns against 8 in the single non-RT hour**, with `cpu 3` taking
**0 timer interrupts/s** (the tick and the per-CPU IPIs were 98.8 % of that core's interrupts and no
runtime affinity write can move them — only `nohz_full` removes them). What it did **not** buy: the
residual late blocks. Two consecutive hours of the same configuration produced 15 and 109 blocks over
the deadline with `cpu == wall` and 0 blocks descheduled, and the mechanism is the memory system, not
the scheduler — under `stress-ng --vm 1 --vm-bytes 128M` on the housekeeping cores the same
instructions take 2.4× the cycles (IPC 1.70 → 0.69) with no change in cache misses per block, at a
constant 2400 MHz. Pinning the engine (`--cpu`) is therefore necessary but nowhere near sufficient,
which resolves the `--cpu` question in §14 and opens a memory-traffic one in its place.

**ARMv7 hosts differ in three ways worth knowing before tuning one** (measured 2026-09-16, `docs/tuning.md` has the detail): `isolcpus`/`nohz_full`/`rcu_nocbs` are usually unavailable, because a 4-core board with a full userspace has no cores to spare — the RT scheduler is then the only protection, and pinning the audio thread is still free; realtime rlimits are absent by default on Armbian, so `SCHED_FIFO` **and** `mlockall` are refused for the normal user until `/etc/security/limits.d/audio.conf` provides `@audio - rtprio 95` and `memlock unlimited` (root bypasses both via `CAP_SYS_NICE`/`CAP_IPC_LOCK`, which is why this hides during a root-only benchmark); and a bus-powered USB interface may need a powered hub, since on one board the engine underran continuously on a bare socket and was perfect behind one.

**Host target.** The assumed host is Linux with `PREEMPT_RT` *where the platform can
supply one* — this Pi 500 can (`linux-image-*-realtime`), and the checklist above is what we ship to
make a stock kernel behave as well as it can. The engine must nevertheless be **correct on a non-RT
kernel**: this box has already held RTF 0.9999 and a sample-exact self-oracle for an hour under a stock
`PREEMPT_DYNAMIC` kernel, and every failure seen so far has been a control-loop bug that an RT kernel
would not have touched (§9.5). RT buys margin against the *tail* — wakeup jitter, long kernel critical
sections, a desktop waking up — and is worth having, but **no acceptance criterion may depend on it**,
because we cannot require it of a user's host. **Measured 2026-09-16:** the RT host did exactly that
and no more. It removed the driver xruns outright (8 in the non-RT hour → 0 in 4 h 10 min) and it did
not remove the CPU-time tail, because that tail is memory-stall shaped and identical in signature on
both kernels. A host with a quiet scheduler and a busy memory system is still not a host this engine
can hold 4 × A2-Full on at q64. The escalation path for a device with neither an RT kernel nor a quiet
one is a dedicated (non-Linux) RTOS; that is deliberately deferred and recorded in
§14, and it is the reason the I/O layer is an interface (§9.1) rather than ALSA calls in the engine.

### 9.4 Platform constraints carried into the design

- **iOS: no runtime-loaded node plugins** (G17) ⇒ the ABI is designed now so third-party DSP is
  statically linkable and everything else (models, IRs, rigs) is data. GUI-less iOS means "app in the
  background" (`UIBackgroundModes: audio`), not a daemon.
- **ARMv7**: 32-bit VA and bandwidth limits (G19) ⇒ resident-model budget is configurable and small by
  default; NEON is runtime-detected for redistributable packages; FMA-less NEON halves FLOPs/cycle, so
  A2-Full counts will be much lower there — the engine should prefer A2-Lite and aggressive slimming
  on that target. **Measured on tib (Tinker Board S, Cortex-A17), 2026-09-16:** the prediction was directionally right but understated. The shipped `-mfpu=vfpv3-d16` build **cannot run A2-Full at all** (RTF 1.3251, so one instance needs 133 % of a core and its median block already exceeds the deadline); `-mfpu=neon-vfpv4` runs **exactly one instance per core** (RTF 0.5199, p50 690 µs against a 1333 µs deadline). Enabling NEON — which the shipped default does not do, on a CPU that has a NEON unit: 1 256 against 10 614 vector instructions — is worth **2.55× on A2-Full and 1.48× on A2-Lite**, and the A2-Lite figure lands on the 1.42–1.55× predicted below. Both builds run A2-Lite comfortably (RTF 0.1249 shipped, 0.0842 NEON). The ARMv7 planar fast path is **verified bit-identical on the hardware** (133 cases, 230 854 assertions, natively on the Cortex-A17), which matters because ARMv7 bit-identity rests on `FPSCR.FZ`. One defect: `NAMCHAIN_A2_PLANAR_ACTIVE` in `CMakeLists.txt` is gated on `aarch64|arm64|armv8` while `a2_planar.h` also selects on `__arm__ && __ARM_NEON && __ARM_FEATURE_FMA`, so an armv7 NEON build reports "A2 planar NEON: OFF" while running the planar kernels. Full account: `bench-results/tib-summary.txt`. **Interaction with PR #313 (§7.8):** its ARMv7 arm is a *compile-time* gate requiring
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

### 9.5 Reference USB interface — measured capabilities (2026-09-12)

The development interface is a **TI/Burr-Brown PCM2902** (`08bb:2902`, "USB Audio CODEC", card 4) on a
**full-speed (12 Mbit/s)** bus. Measured from `/proc/asound/card4/stream0`, `--dump-hw-params`, and
direct playback/capture runs:

| property | value |
|---|---|
| playback | S16_LE (also S8/U8), 1–2 ch, RATE 32000/44100/48000, endpoint **ADAPTIVE** |
| capture | S16_LE (also S8), 1–2 ch, 48000/44100/32000/22050/16000 (+8-bit 8000/11025), endpoint **ASYNC** |
| period | PERIOD_TIME floor **1000 µs**; PERIOD_SIZE [32–48000] playback, [16–48000] capture |
| buffer | BUFFER_SIZE [64–96000]; PERIODS [2–1024] |
| formats | **no float, no 24/32-bit — S16_LE at 48 kHz** |
| controls | `PCM Playback Volume` (S16, −128…0 dB), `PCM Playback Switch` (**INV_BOOLEAN**) |

Three consequences that change implementation details:

1. **The quantum floor at 48 kHz is 48 frames, not 32.** Full-speed USB services transfers on 1 ms
   frames, so `PERIOD_TIME ≥ 1000 µs`. Requesting `--period-size=32` **silently yields 48** — verified:
   the negotiated parameters report `period_size: 48, period_time: 1000`. `alsa_raw` must therefore
   **read the negotiated `period_size` back** after `SNDRV_PCM_IOCTL_HW_PARAMS` and fail loudly when it
   differs from the requested quantum: an engine whose quantum silently differs from the device period
   has a wrong render plan and wrong buffer accounting. It also means **§1 criterion 3 and the §10.3
   torture render can only be validated at q ≥ 48 on this interface**; q = 32 stays valid for the
   offline paths, which is precisely why those are specified across 32/64/128/256.
2. **A float32 engine meets a 16-bit device.** S16_LE conversion belongs at the I/O boundary, and the
   playback volume control is real (currently near 0 dB) — gain must not be assumed unity.
3. **Capture is ASYNC with no explicit feedback endpoint**, so the driver derives implicit feedback.
   The engine should open both directions together, as a full-duplex chain does anyway. Verified
   working: 10 s of simultaneous capture+playback at 48 kHz / period 64 with no errors, and capture
   alone for 5 s likewise clean.

**Measured passthrough baseline (2026-09-12, `namchain-soak --device hw:4,0` — the card number on
that boot; see §0).** With no DSP in the
path — capture straight to playback through the raw-ioctl backend — the host's I/O cost is:

| quantum | channels | duration | blocks | p50 | p99 | max | p99/deadline | deadline overruns | driver xruns |
|---|---|---|---|---|---|---|---|---|---|
| 64 | 1 | **60 s** | 45 000 | 8.7 µs | 11.7 µs | 25.3 µs | 0.88 % | **0** | **0** |
| 128 | 1 | 20 s | 7 500 | 8.8 µs | 15.2 µs | 24.6 µs | 0.57 % | 0 | 0 |
| 256 | 1 | 20 s | 3 750 | 9.3 µs | 14.5 µs | 37.4 µs | 0.27 % | 0 | 0 |
| 64 | 2 | 20 s | 15 000 | 8.7 µs | 11.6 µs | 22.0 µs | 0.87 % | 0 | 0 |

So the I/O layer consumes well under 1 % of the deadline and leaves essentially the whole budget to
the engine — which is the number the §11 per-model figures were always meant to be spent against.
The driver itself confirms the floor: HW_REFINE reports `period_size [48..48000]`,
`period_time [1000..1000000] µs`, so a request for 32 fails loudly rather than being rounded.

**Implementation note worth carrying (cost an afternoon).** `SNDRV_PCM_IOCTL_HW_REFINE` only refines
the parameters named in `snd_pcm_hw_params::rmask`. Leaving `rmask` at zero returns the input space
**unchanged** — no error, every bound still reads as "any" — so negotiation appears to work while
doing nothing, and the failure only surfaces later as an `EINVAL` from `HW_PARAMS` on the sizes that
were never really constrained. Two smaller ones from the same session: the kernel expects the buffer
to be a whole multiple of the period (4 × 32 = 128 is not a multiple of the clamped 48), and playback
must not be `START`ed on an empty ring — it underruns immediately and the `START` ioctl itself returns
`-EPIPE`. Priming one buffer of silence fixes the latter; the hardware then auto-starts, so an
explicit `START` may report `EBADFD`, which means "already running" and is checked by state rather
than assumed.

**Interface comparison, measured (2026-09-12).** Three class-compliant interfaces through the same
passthrough soak:

| interface | class | bus | formats | period floor @48 k | q = 32 | q = 64 |
|---|---|---|---|---|---|---|
| TI PCM2902 | UAC1 | full speed | S16_LE, 2 ch | 1000 µs → 48 fr | refused (above) | ✓ 0 xruns, 0.88 % |
| Joyo MOMIX | UAC1 | full speed | S16_LE, 2 ch, 48 k only | 1000 µs → 48 fr | refused | ✓ 0 xruns, 0.94 % |
| **SSL 2+ Mk II** | **UAC2** | **high speed** | **S32_LE, 4 ch, 44.1–192 k** | **125 µs → 6 fr** | **✓ 30 s, RTF 1.000, 0 xruns, 1.70 %** | ✓ 0 xruns, 0.93 % |

The UAC1/UAC2 split is the whole story: full-speed USB services transfers on 1 ms frames, high-speed on
125 µs microframes, so **only the UAC2 device can honour the q = 32 column that §1 and §10.3 specify**.
On this evidence the **SSL 2+ Mk II is the reference interface** for real-time work — the decision §11
said should be made by measurement, and this is that measurement. The two UAC1 devices stay useful as
the cheap end of the range the engine must still support, and the MOMIX additionally pins 48 kHz with
no other rate available.

**Firmware V1.16 changed the device, and the topology-with-it (2026-09-13).** SSL's V1.16 adds two more
stereo loopbacks (three in total) and reports round-trip latency improvements. The channel counts moved
from 4/4 to **6 playback / 8 capture**, and the channel map was established by measurement rather than
from the descriptors, which are generic (`FL FR FC LFE RL RR`):

| capture channel | is | fed by |
|---|---|---|
| 0, 1 | analogue inputs 1, 2 | the combo jacks |
| 2, 3 | loopback 1 | playback 0, 1 (the monitor pair) |
| 4, 5 | loopback 2 | playback 2, 3 (physical outputs 3 and 4) |
| 6, 7 | loopback 3 | playback 4, 5 (**no physical output**) |

Two consequences worth stating. **The loopbacks are bit-exact digital taps** of the playback stream —
an amplitude-coded probe came back at exactly the amplitudes sent, to four decimals — so they measure
the USB and driver round trip with no converters in it. And **playback 4/5 exist only as a loopback
source**: there is no fifth or sixth output on the box, so those two channels are a way to inject audio
into a stream without it appearing on any physical output. That is the feature a podcaster wants, and
it is also why the capture side gained two channels and the playback side gained two.

A channel count the device does not offer now fails to open rather than being trimmed: USB audio
exposes one altset per channel count, so asking the V1.16 device for 4 channels returns `EINVAL` from
`HW_REFINE`. `alsa_raw` reports the count it asked for and the rate in that message, because "Invalid
argument" from an ioctl named HW_REFINE is not a diagnosis.

**True round-trip latency, measured (2026-09-13, `namchain-latency`).** The cable is a TRS patch from
output 3 to input 1, with input 1 in LINE mode at minimum gain (the manual's +14.5 dBu maximum output
against its +24 dBu maximum line input budgets 9.5 dB of headroom, and the measured path gain is
**-10.2 dB** by sine and **-10.5 dB** by RMS of a broadband stimulus — the two agree, and the
prediction is right to within a decibel).

The measurement sends a 4096-frame pseudo-random stimulus four times and cross-correlates, so the delay
falls out of the data rather than out of an assumption about the device's buffering. Both the analogue
loop and the internal digital loopback are measured **in the same run**, from the same stimulus on the
same playback channel.

**The occupancy open item is closed, and it changed the conclusion.** The first version of this section
presented the delays as if they were properties of the device. They are not: what you measure from "I
handed the stimulus to the playback ring" to "it came back" includes however much audio was already
queued in the playback ring and buffered in the capture ring at that instant, and that settles
differently every run. `namchain-latency` now reads both directions' ring occupancy with
`SNDRV_PCM_IOCTL_STATUS` at the instant the stimulus is written and subtracts it. The identity it uses
is derived in `src/latency/latency.cpp`; the short version is that the four measured terms
(`frames_read`, `capture_delay`, `playback_delay`, and the stimulus's offset within its period) are an
exact correction rather than a statistical one, because they are sampled at the one moment that makes
the difference between the two streams' origins cancel.

Three runs at each configuration, with the raw and corrected analogue figures:

| quantum | periods | buffer | raw analogue | **corrected analogue** | raw digital | corrected digital | difference |
|---|---|---|---|---|---|---|---|
| 16 | 2 | 32 | 185–190 | 80–103 | 169–188 | 82–87 | 16, but −2 and 17 once |
| 16 | 4 | 64 | 247–259 | 87–99 | 230–242 | 70–82 | 16, 17 |
| 32 | 2 | 64 | 277–334 | 105–134 | 261–318 | 89–118 | 16 |
| 32 | 4 | 128 | 317–353 | 93–129 | 301–337 | 77–113 | 16 |
| 64 | 2 | 128 | 420–433 | 116–129 | 404–417 | 100–113 | 16 |
| **64** | **4** | **256** | 348–438 | 92–134 | 332–422 | 76–118 | 16 |
| 128 | 2 | 256 | 556–586 | 92–122 | 540–570 | 76–106 | 16 |
| 128 | 4 | 512 | 568–604 | 104–140 | 552–588 | 88–124 | 16 |

**The period-size dependence was ours, not the device's.** The raw figure grows from 169 frames at
q = 16 to 604 at q = 128 — a factor of 3.6 — while the corrected figure sits between 70 and 140 frames
with no trend at all. So the earlier reading of this table ("the device's internal buffering scales with
the period it is given") was wrong: what scales with the period is the *host's* ring occupancy, which is
buffering we chose. The device's own contribution to the round trip is ~2 ms and roughly flat across
every configuration measured. Latency still has to be bought in period size, but because of the rings,
not the converters.

**What the correction does not remove is the reason the corrected figure is still not frame-exact.**
Six consecutive runs of the same configuration (q = 64, 4 periods) give raw 387–470 frames (spread 83)
and corrected 76–129 (spread 53): the correction takes out 36 % of the movement and leaves about a
millisecond of it. What is left is the *relative alignment of the two streams' origins* — when each
`START` took effect — which is quantised by the USB frame clock (48 frames at 48 kHz) and is a property
of how the host started the streams, not of the device's latency. ALSA's pointer interface cannot
resolve it, because the pointers wrap; relating the two streams needs a common clock, and the driver
will not give one. It accepts `SNDRV_PCM_IOCTL_TSTAMP` and still reports every status timestamp as
zero, which `namchain-latency --json` reports as `timestamps_enabled: true` alongside zeros so the
negative result is on the record.

`--runs N` therefore exists: the tool repeats the whole measurement, stream start included, and reports
the spread itself rather than leaving it to be guessed. Its output says plainly to compare channels
*within* one run, and that is the figure to quote:

- **The analogue-minus-digital difference is 16 frames = 0.333 ms in every configuration and every run**
  — 16 in all 24 runs of the sweep above, in all 6 runs of the repeatability test, and 16 in every run
  of the original table. That is the DAC plus ADC plus analogue path, and it is exact because both
  channels are measured in one run from one stimulus: the ring occupancy, the start alignment and our
  own priming are all common to both and cancel. It is the whole argument for measuring both loops at
  once rather than one per session.
- **At q = 16 the device cannot keep real time**, and the correction shows it: the difference came out
  as 16, then −2, then 17, which is a stream that is not transferring frames consistently. Every other
  configuration gives 16 every time.

The delays are stable to the frame across all four sends *within* a run — because the sends are 9024
frames apart, an exact multiple of the 48-frame USB frame, so all four share the same phase against it.
The digital loopback's correlation is bit-exact (peak-to-sidelobe 999 dB) where the analogue path's is
32 dB, the expected difference between a digital tap and a path through two converters.

**q = 32 with 4 periods is the config to prefer**, and that is a change from the q = 64 this section
assumed. It is lower-latency (6.98 ms analogue against 10.00) and comfortable for the guitar-chain rig:
p99 7.6 % of the 667 µs deadline, 0 deadline overruns, 0 driver xruns, 0 render-path allocations over
20 s. The period *size* is what the latency lives in; the period *count* is what the safety margin
lives in — at 2 periods the same rig reports p99 37 % of the deadline at both q = 32 and q = 64, which
is the buffer running out of room rather than the engine being slow. p99 varies enough between runs
(18.7 % at q = 64 / 4 periods on one 20 s run) that the latency table, not the percentile, is the
stable comparison here; that is what the 1 h soak is for.

**Criterion 3, measured in the device loop (2026-09-12).** The same harness, now running real models
between the backend's read and write: SSL 2+ Mk II, 48 kHz, q = 64 (1333 us deadline), 4 channels,
10 s each, `SCHED_FIFO` 80 with `mlockall`, steady state (bring-up excluded).

| chain | p50 | p99 | max | p99/deadline | overruns | driver xruns |
|---|---|---|---|---|---|---|
| passthrough | 8.6 us | 11.4 us | 51.5 us | 0.85 % | 0 | 0 |
| 1 x A2-Full | 79.2 us | 108.9 us | 225.2 us | 8.17 % | 0 | 0 |
| 2 x A2-Full | 156.0 us | 195.1 us | 374.3 us | 14.6 % | 0 | 0 |
| **4 x A2-Full** | **327.9 us** | **433.8 us** | **832.4 us** | **32.5 %** | **0** | **0** |
| 8 x A2-Full | 968.6 us | 1378.7 us | 5770 us | 103 % | 93 | 22 |

**§1 criterion 3's throughput clause now passes on hardware**: four concurrent A2-Full at 32.5 % of the
deadline against a 50 % budget, zero deadline overruns and zero driver xruns, in a real full-duplex
loop. The ceiling sits between 4 and 8 instances: eight cost ~121 us each rather than 82 us, i.e.
superlinear, which is the cache/memory pressure §11 predicted rather than an arithmetic wall.

**The device loop now renders a compiled plan, not a model chain** (§11 has the table): 2 × A2-Lite in
parallel from a rig JSON at 7.57 % of the deadline over 60 s, 2 × A2-Full at 15.29 %, and 4 × A2-Full in
series at 34.95 % — the last of which is this same 32.5 % workload re-run through the compiler, so the
two tables together are the evidence that the graph costs nothing measurable. Every one of those runs
reports 0 driver xruns, 0 deadline overruns and 0 render-path allocations.

Three harness bugs were found getting here, each of which would have produced a confidently wrong
number:
1. **Recovery re-`PREPARE`d the playback ring without re-priming it**, so after any xrun the stream
   underran again immediately -- reported as 495 "xruns" in a run whose median work was 8 % of the
   deadline. Re-priming with a preallocated silence buffer fixed it.
2. **Priming the whole ring** (which is effectively what a file player does) left it permanently full,
   so every write blocked for a full period: the reported 1492 us block time was 70 us of engine and
   1416 us of the device's back-pressure. Priming one period fixed it; splitting the timed section
   into read / chain / write is what made it visible, and the split is worth keeping for that reason.
3. **The first block after START overran in every configuration, passthrough included** -- it is the
   initial fill of an empty capture ring, not a real-time property. Bring-up is now untimed and its
   xruns reported separately, rather than being discarded or mistaken for load.
4. **The output queue ratchets to full and has no restoring force** -- the one *design* fault this work
   exposed, and the reason the 1 h criterion-3 soak failed. Priming one period (item 2) starts the
   playback ring at one period of occupancy, but the loop then adds exactly one period per period the
   device drains, so occupancy is a **conserved quantity**: any burst the capture gate allows pushes it
   up by a period, and nothing pulls it back down. The ceiling is a fixed point, because a blocking
   write still delivers its period -- it just charges the wait to the block. Measured after the cadence
   fix (`67a6ed7`), 300 s: `write` p50 **698 816 ns**, block p50 1 019 910 ns, block p99 **112.6 %** of
   the deadline, **224 239 of 225 000 blocks** writing into a ring with less than a period of room --
   while the engine's own `chain` p99 was **45.7 %** and the self-oracle stayed exact.
   - **Fixed 2026-09-14, in three parts** (`cb5cc19`, `dde1aa9`): the loop calls
     `try_write_playback`, which asks the kernel's own `poll()` whether the ring can take a period and
     **drops and counts the period if it cannot** instead of waiting for it; `wait_capture` gates on the
     pointer-derived ring occupancy and counts releases that find two periods waiting; and bring-up
     drains the capture backlog, replacing each discarded period with a silence period so the playback
     ring is exactly as full as the one-period prime left it. Measured over 300 s: `write` p50 699 us ->
     **11 815 ns**, block p50 1 019 910 -> **313 204 ns**, the playback ring 15..255 frames instead of
     pinned at 256, and 12 dropped periods out of 225 000 rather than 224 239 blocked ones.
   - **The 544-frames-against-256 anomaly is explained, and it was `delay` that was wrong.** Read
     against `/proc/asound/card2/pcm0[cp]/sub0/status` while a stream ran: playback
     `delay == (appl - hw) + 72` frames of USB in-flight audio, and capture `delay` carries an
     intermittent `+48`, while `avail` agrees with the pointer pair in both directions at every one of
     22 samples. So `delay` was never the ring occupancy, and the occupancy figures quoted before this
     date were describing the wrong quantity. `ring_occupancy()` now derives the figure from the
     pointers (`alsa::pcm_ring_frames`, unit-tested against those 22 samples), and
     `StreamQueueStats::occupancy_mismatches` reports how often a backend's `delay` disagrees with it.
5. **An xrun freezes `hw_ptr`, so a gate waiting on occupancy waits for ever.** Found at block 52 443
   of an hour-long run: the soak died on `capture timed out after 2000 ms` with the capture ring frozen
   at 13 frames. Nothing else could notice, because the gate is exactly what would have let a read
   through, and a read is what reports `-EPIPE`. `wait_capture` now checks the PCM state, counts the
   xrun, recovers, and carries the last state and ring occupancy in its timeout message, so the next
   occurrence explains itself instead of ending a long run. **This is a stall the loop survives now**,
   and it is counted separately (`gate_capture_xruns`) because a run that reports it is a different
   claim from one that never met it.
6. **What is left is a tail, and the tail is not the queue.** With the queue fixed, the failures are
   isolated blocks that miss the deadline: 114 of 225 000 over 5 min, ~50 in 90 s, with the render path
   costing **2.7-3.8 ms of CPU** where p50 is 300 us. Three facts, all measured, and the third is the
   one that matters:
   - The instrument is not the cost: a `CLOCK_THREAD_CPUTIME_ID` pair measures **333 ns**, against
     300 us of work.
   - The spikes arrive in **runs of consecutive blocks**, not scattered: typical groups of 2-10 blocks
     (~3-13 ms), and in one 90 s run 430 such blocks formed 88 runs with one cluster every few
     seconds. That is an external event or a shared-resource effect, not engine code.
   - `cpu == wall` with **0 blocks losing time to not being on CPU** -- so this is *not* preemption, and
     an RT kernel is not automatically the answer. **But it is not proof that our code ran slowly
     either: softirq work is charged to whichever task the CPU was running**, so an IRQ burst on the
     audio core appears as the engine's own CPU time. Distinguishing those two needs IRQ affinity or
     `threadirqs`, not a better timer.
   - **Pinning helps and is measured.** `taskset -c 3` over the same 90 s: spikes 430 -> 290 blocks, runs
     88 -> 67, deadline overruns 47 -> 25, drops 7 -> 1, bursts 18 -> 6. Half the tail, so the rest is not
     merely migration -- the §9.3 list's `isolcpus`/IRQ-affinity half is still outstanding. As of
     `2c3cdd4` the **engine sets its own** `SCHED_FIFO` and affinity and reports what it was granted, so
     this is no longer a property of the command line that launched it.
7. **The one-hour non-RT baseline, and the trade the fix bought** (`9aa25e6`, pinned, 4 x A2-Full, q=64,
   3600 s = 2 700 000 blocks):

   | | before (`67a6ed7`, latched) | after the queue fix |
   |---|---|---|
   | RTF | 0.9999 | **0.9999** |
   | block p50 | 993 167 ns | **315 613 ns** |
   | block p99 | 112.55 % | **41.19 %** |
   | chain p50 / p99 | 295 204 / 547 243 ns | 300 501 / 529 612 ns |
   | render-path CPU | not measured | **836.3 s of CPU per 3600 s of audio (23 % of one core)** |
   | descheduled blocks | not measured | **0** |
   | deadline overruns | 805 415 | **981** |
   | driver xruns | 23 | **92** |
   | dropped periods | n/a (it blocked instead) | 69 |
   | oracle | exact, 2 880 000 frames | exact, 240 000-frame window |

   The engine holds real time for an hour at 23 % of one core, never loses the CPU, and gets 821x fewer
   missed deadlines. **And it is audibly worse: 92 xruns plus 69 dropped periods in the hour, against 23
   xruns before.** That is not a regression to be hidden, it is the trade being taken deliberately:
   the old loop kept the playback ring at its ceiling, which is ~5.3 ms of output latency and, for
   exactly that reason, a four-period cushion against host jitter. The fixed loop keeps 1-2 periods
   queued, so the same jitter now reaches the device. **Occupancy stopped being a runaway and became a
   tunable**: the queue policy can hold a chosen target, which makes latency-against-dropout-tolerance a
   parameter with a measured cost on both sides instead of an accident. Choosing that default is an
   open decision (§14), and the honest reading of this table is that the fix bought control, not a
   smaller number of dropouts.
8. **The knob exists and has been swept, and the sweep changed what the knob means** (2026-09-14,
   `cb5cc19`+, SSL 2+ Mk II, 4 x A2-Full, q=64, 300 s per run, pinned + self-scheduled). Full table and
   evidence in `bench-results/queue-target-sweep.txt`; the shape of it:

   | run | ring | cap | queue min..max / last (periods) | pb underruns | drops (cap) | block p99 | overruns |
   |---|---|---|---|---|---|---|---|
   | 4-period ring, no cap (the old fixed loop) | 4 | none | 0.00..3.98 / 2.00 | 7 | 9 (0) | 43.1 % | 11 |
   | 4-period ring, cap 2 | 4 | 2 | 0.00..3.88 / 0.95 | 6 | 17 (17) | 38.9 % | 9 |
   | 6-period ring, no cap | 6 | none | 0.27..5.88 / 3.12 | 1 | 0 | 46.8 % | 36 |
   | 6-period ring, cap 2 | 6 | 2 | 0.00..5.98 / 1.17 | 9 | 25 (25) | 41.1 % | 30 |
   | **6-period ring, cap 4** | 6 | 4 | 0.20..3.52 / 3.25 | **0** | **0** | 39.2 % | 30 |
   | 4-period ring, cap 1 (90 s, fails immediately) | 4 | 1 | 0.00..3.98 / 0.36 | 2620 | 2696 | 47.7 % | 39 |

   Four things came out of it, and the first two are corrections to this section's earlier reading:

   - **The ring's size is the cushion, and it is not latency.** 4 -> 6 periods at no cap took playback
     underruns from 7 to 1 per five minutes and drops from 9 to 0, at the same queue *level* (last 2.00
     vs 3.12 periods). The ring is not in the signal path; only its contents are. The 4-period ring was
     never too small for the audio, it was too small for the *recovery*: its ceiling sat close to the
     level the queue naturally reaches, so a burst arriving near the top was refused, and a refusal is a
     period of audio lost. This also corrects §9.2's "engine algorithmic latency = I/O buffer": the
     buffer is a ceiling, the occupancy is the latency.
   - **The cap bounds latency, and must not be set below the level the queue reaches by itself.** At
     ring 6 the queue accumulates to ~3.1-3.3 periods unaided. Capping at 4 held it at 3.25 (4.7 ms
     worst case) against 5.88 (7.8 ms) unbounded, with no measured underrun cost. Capping at 2 --
     *below* the natural level -- produced the worst result of the six: 25 drops and 9 underruns. A cap
     under the natural level does not trade latency for tolerance, it manufactures drops, and each drop
     leaves the ring shallower for the next burst.
   - **A cap of one period is a positive-feedback failure rather than a tight margin.** 2620 underruns
     in 90 s, 29 per second, 2696 refusals. A 64-frame period is smaller than the device's own
     consumption step (48-frame USB frames at 48 kHz), so the sample is nearly always above a
     one-period cap even in a healthy stream: the loop refuses, the ring drains, the driver underruns,
     and the recovery re-primes into the same cap. The floor is the period plus the device's step, not
     the period.
   - **The zero-xrun run still had 30 deadline overruns, which is the trade stated cleanly.** It
     absorbed 30 blocks over the 1333.3 us deadline (block max 2.2 ms) and never starved the device,
     because the queue sat 3.25 periods deep. Every earlier configuration had overruns *become* xruns;
     this one separated the two, which is what the knob is for.

   **Recommendation: `--periods 6 --output-queue-periods 4`.** The hour run taken at it (`331da80`,
   3600 s, same configuration, quiet machine) is the number to quote:

   | | 4-ring, no cap (the 2026-09-14 hour, `9aa25e6`) | **6-ring, cap 4** |
   |---|---|---|
   | playback underruns | ≤ 80 (92 total, ≥ 12 of them capture) | **8** |
   | capture overruns | ≥ 12 | **0** |
   | queue held (min..max, last) | 0.00..3.98, 2.00 periods | 0.00..5.98, **3.59** periods |
   | output drops (cap) | 69 | 34 (34) |
   | capture bursts | 234 | 225 |
   | block p99 | 41.19 % | **37.58 %** |
   | chain p99 | 39.72 % | **36.39 %** |
   | deadline overruns | 981 | **204** |
   | render-path CPU / descheduled / alloc | 836.3 s, 0, 0 | 820.5 s, 0, 0 |
   | oracle | exact, 240 000 frames | exact, 240 000 frames |

   **And the five-minute version of that run said zero xruns and zero drops, which is why the hour was
   worth taking.** The 5-minute figure was luck; the honest recommendation is "about one dropout every
   seven minutes, with the tail unchanged", not "dropout-free". The tail did not move at all: the
   slowest render paths are the same 2.1 ms (1.6 periods), the spikes still arrive in runs (4096+ blocks
   capped in 1987 runs), `cpu == wall` with 0 blocks descheduled, and the device wait still reaches
   4.8 ms. The queue changed the *consequence* of the tail and not the tail.

   Three ways forward, in the order the evidence puts them. (i) **Keep the queue's floor up**: the 8
   underruns are stalls that arrived near the bottom of the queue's sawtooth, not at its mean (`last`
   3.59 periods against a worst case of 5.98), and the missing instrument is the occupancy
   *distribution* -- after that, a floor controller, which can only work by inserting a period of
   silence rather than by producing audio it does not have: a deliberate, fadable gap in place of a
   driver-level underrun (an open decision, §14). (ii) **Make each dropout inaudible** -- the
   `rt::SpliceFader` of §10.3; it costs no latency and is the half the user asked for. (iii) **Fix the
   host** -- IRQ affinity and the RT kernel, because `cpu == wall` with 0 descheduled blocks rules out
   preemption and leaves interrupt work charged to our own CPU time as the candidate, which is the
   experiment nobody has run.

   Two bugs were found by building it, and both are worth more than the table. A cap applied to a
   stream that is not running starves it of the write that would report and recover the xrun -- the
   pointers freeze, "occupancy" reads full, and the loop refuses for ever while the report claims zero
   underruns (measured: 2285 consecutive refusals, one real underrun, a report saying none) -- so the
   cap now governs a `RUNNING` stream only. And the cap has to be established immediately before the
   timed loop, because the bring-up path spends a period of the primed queue and the ring's contents
   are conserved: a queue that starts a period short stays a period short for the whole run, which is
   why `AudioBackend::prefill_output_queue` exists and why `prefill_frames` is reported.
9. **The occupancy distribution, and a floor the loop can hold** (2026-09-15). Item 8 ended on a missing
   instrument and an open mechanism, and this is both.

   The instrument is a **histogram of the per-block occupancy**, one bin per frame and clamped into the
   ring's length, which makes the percentiles exact rather than extrapolated and costs 3 KB for an hour
   (`histogram_percentile`, nearest rank, tested). It answers the question `min` cannot: `min` is 0 in
   every run that underran once, so it describes the worst run and not the typical one, and says nothing
   about where a stall arrived. Two new figures in the report, both in periods: `p1`/`p50`, and the
   **count of blocks whose queue held less than one period** -- the exposure window, since a stall that
   arrives in one of those is an underrun and a stall that arrives anywhere else is absorbed. The first
   run of it, `--periods 6 --output-queue-periods 4`, 20 s: `p1 2.75, p50 2.94`, and **0 of 15 000
   blocks below one period**, against a `min` of 2.05 -- the floor and the level, separated at last.

   The mechanism is a **floor controller** (`--output-queue-floor-periods N`): after each successful
   write, if the queue is below the floor, insert a period of silence. The reasoning is in
   `alsa::queue_needs_top_up` and it rests on what an underrun actually costs -- the driver stops the
   stream and recovery `PREPARE`s it, which discards **the whole queued ring** and slips the timeline by
   an arbitrary amount. A period of inserted silence costs 1.33 ms and slips nothing. So the choice is
   not "dropout or not", it is "one bounded gap now or the entire queue later", and the controller makes
   the first choice available. It is bounded at two insertions per write (a controller that spins in the
   render path would be a worse fault than the dropout), it cannot block -- the configuration refuses a
   floor at or above the ring's length, so a queue below the floor always leaves room for a period, which
   is why the room check in the policy is a guard rather than a live path -- and it counts what it did
   (`floor_insertions`, reported with the milliseconds of audio delayed rather than skipped).

   **Semantics, stated because the off-by-one is the whole meaning:** the floor is enforced *after* a
   write, so the guaranteed worst-instant tolerance is `floor - 1` periods -- the queue drains a period
   between the write and the next one, and it is the instant before a write that a stall finds. Asking
   for `N` therefore buys `N - 1` periods of stall tolerance, and `p1`/`min` in the report are that
   figure rather than the configured one. Inserted silence is audible at both ends without a splice fade;
   that fade is §10.3's mechanism and is the next piece, and until it lands the insertion count is the
   price of the floor stated in the open.

   **Both bounds placed inside the band squeeze the queue, and that is the placement rule's origin.**
   `--periods 6 --output-queue-periods 5 --output-queue-floor-periods 4`, 900 s: **0 xruns**, `p1 3.73`,
   `p50 3.94`, 3 of 675 000 blocks below one period -- and **71 insertions against 71 drops**. That
   equality is mechanical rather than a coincidence: the controller fires when the *post*-write queue is
   under 4 periods and the cap refuses when the *pre*-write queue is over 4, so pre <= 256 and post >= 256
   pins the queue into a one-period band (pre = 192, post = 256) and every excursion across either edge is
   corrected -- 142 audible events in 15 minutes. So, for both bounds, and derived rather than fitted:

     * the **cap** must sit **above** the band the queue actually occupies, or it refuses ordinary
       bursts (a refusal is a period of audio skipped);
     * the **floor** must sit **below** it, or it fires on ordinary dips (an insertion is 1.33 ms of
       audio delayed);
     * and the two must be **at least a couple of periods apart**, or the queue is squeezed between
       them and both fire constantly.

    The band is invisible without the occupancy distribution, which is why that instrument came first.

    **And the floor earns its place, which a balanced A/B settled.** A single hour at
    `--output-queue-periods 4 --output-queue-floor-periods 2` looked like a timing regression (chain p99
    51.81 % against 36.39 %, 874 deadline overruns against 204) and like the floor was spending its
    insertions as extra cap drops (65 insertions, 79 drops against 0 and 34). Re-running the comparison
    as **A B B A** -- two configurations, four 900 s runs, drift cancelling between the arms --
    (`bench-results/queue-floor.txt`) reversed both readings: chain p99 **34.02 % with the floor against
    35.92 % without**, and drops **16.0 against 13.0 per arm with completely overlapping ranges**. The
    design also measured its own noise: the *same* configuration swung 38.65 % -> 33.19 % between its two
    runs, 5.5 points against a 1.9-point effect, so no single run of that comparison can be believed in
    either direction. What the floor really does is halve the exposure -- blocks holding under one period,
    **24.5 -> 10.0 per 675 000**, consistent in both orderings -- for about 40 inserted periods per hour
    (53 ms of inserted silence). The underrun difference (3.0 -> 1.5 per 15 min) is suggestive at n=2 per
    arm; the exposure difference is the stronger evidence because it measures the mechanism rather than a
    rare event.

    So the floor ships **off** by default, and **with `rt::SpliceFader` of §10.3 it should be enabled**:
    40 bridged 1.33 ms gaps per hour against ~6 underruns per hour, each of which discards the whole
    queued ring and slips the timeline. That is the trade the floor exists to offer, and the bridge is
    what makes it a good one -- which is why measuring the floor promoted the bridge from a nicety to the
    critical path, and why the bridge is now implemented rather than specified. Its remaining limit is
    named there: the *entry* of an inserted period is a slope break the loop cannot reach, because the
    loop does not write that period. The ring's size remains the only lever that reduces exposure with no
    hole at all; the bounds only decide which holes a run gets when it fails.

**A negotiated period is necessary but not sufficient.** The SSL *accepts* a 16-frame period (floor 6),
but the run does not keep real time at it: RTF 1.333, i.e. it loses a third. Instrumenting the loop to
separate **device wait** from **work** shows where the time goes:

| quantum | nominal period | wait p50 | work p50 | mean wait + work | RTF |
|---|---|---|---|---|---|
| 48 | 1000 µs | 991 µs | 8.8 µs | ≈ 1000 µs | 1.000 |
| 32 | 667 µs | 741 µs | 8.6 µs | ≈ 667 µs | 1.000 |
| 16 | 333 µs | 366 µs | 8.7 µs | ≈ 444 µs | **1.333** |

At q = 32 and 48 the *mean* wait per period equals the nominal period almost exactly, so the loop tracks
real time. At q = 16 the mean wait is a third longer than the nominal period: the device/driver is
offering a 16-frame period at roughly 2.25 kHz rather than 3 kHz. Our work is ~9 µs — under 3 % of the
budget — so this is not something more efficient code can fix; it is the device's period delivery at that
size. Why microframe packetisation produces exactly 4/3 is **not established** (high-speed USB carries
6 frames per 125 µs microframe at 48 kHz, and 16 is not a multiple of 6, but that arithmetic does not by
itself predict 444 µs); the thing to test next is `sw_params` wakeup thresholds, which we deliberately
left at kernel defaults. The operational conclusion stands either way: **q = 32 is the floor on this
interface**, well below the plan's default of 64.

Note also that per-iteration waits are *elastic*: with a 4-period buffer the median wait at q = 32
(741 µs) exceeds the nominal period while the run keeps exact time, because some iterations return
immediately and others wait longer. Only the mean tracks the clock — which is why the harness reports
RTF and mean-relevant totals rather than reading a single wait figure.

**Bug this exposed.** The transfer buffers were sized as `int16_t` counts (`frames × channels`
elements), i.e. half the bytes required on any device wider than 16-bit — a heap overflow that
corrupted the allocator (`malloc(): corrupted top size`) on the S32_LE SSL. Both UAC1 devices are
16-bit, so it worked by luck until a 32-bit device appeared. The API now takes `void*` and documents
the requirement in **bytes**, so the mistake cannot be expressed.

---

## 10. Verification — how "bulletproof" is proven

### 10.1 Unit tests

Graph validation (cycles, arity, dangling ports, unknown types), memory planner (overlap/liveness),
patch application (all-or-nothing, error cases), ramp/equal-power fade math, SPSC rings and epoch
reclamation under TSan, lookback calculator vs. hand-computed A2 numbers (6346/6347), model capability
probe, resampler.

**Implemented at M1:** `rt::Arena` (bump allocator whose *backing store* is aligned, not just the
offset -- the first version returned 32 mod 64 for a "64-byte aligned" request), `rt::SpscRing<T>`
(wait-free, cache-line-separated indices, a full ring *refuses* rather than overwriting and counts the
refusal) and `rt::Reclaimer` (epoch-based with a grace window, type-erased deleter held as a function
pointer rather than `std::function`, which would allocate on the render path). Each is tested for
behaviour *and* for allocating nothing inside an `RtScope`; the ring additionally runs under
ThreadSanitizer in a dedicated CI job, since a single-threaded test cannot see a missing
acquire/release pair.

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

**Implemented at M0 (`tests/golden/`, `tests/test_render.cpp`).** `namchain-render` renders a `.nam`
over a WAV and `namchain-wavcmp` compares two WAVs with a tolerance, so the golden test runs our
renderer and the vendored core's own `render` tool over the same model and input, then compares sample
by sample. **Measured, against a reference built `-Ofast` while ours is `-O3`:**

| host / toolchain | max&#124;a−b&#124; |
|---|---|
| Cortex-A76, GCC 13 `-O3` | **0** (byte-identical) |
| Cortex-A76, **GCC 15.2 `-O3`, kernel 7.0** — re-measured after the 2026-09-13 upgrade | **0** (byte-identical) |
| Cortex-A76, clang 20 `-O2` + RTSan | **0** (byte-identical) |
| x86_64, GCC `-O3` | 1.71e-07 (≈ −154 dBFS RMS) |

On AArch64 the planar kernels fix the arithmetic in NEON intrinsics, so the result does not move with
the compiler *or* the optimisation level — which is a stronger form of the §7.8 bit-identity claim than
originally established. Only x86 differs, where A2 takes the Eigen path and `-O3` vs `-Ofast`
legitimately diverges. The tolerance is therefore **1e-5**: ~58× above the only non-zero measurement,
~−100 dBFS against a signal peaking at 0.19, and far tighter than any real pipeline error. Alongside it,
the offline path is pinned by tests for run-to-run determinism (byte-identical), block-size invariance
across 1/3/32/64/128/256 frames, exact frame accounting (one output frame per input frame), refusal to
resample a rate mismatch (G11), and error paths for missing/malformed inputs. The core's `render` tool
is a mono pipeline, so the golden comparison covers the mono path only.

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

**Implemented, for the `smooth` class only (2026-09-19).** `namchain-render --graph rig.json
--timeline edits.json` (or `--torture N --seed S`) applies `set_param` ops at exact samples and hands
each edit a span with its own before/after oracle renders, compiled from the same graph and driven to
the parameter snapshot that applies at that edit. The oracle is rendered from frame 0 so its state at
the span is the live state, which fixes the earlier sketch's whole-file `A`/`B`: those only bracket a
single edit, whereas a timeline needs a pair per edit. The full quanta/rates sweep and the structural
classes remain to be run -- and the continuity rule is *relative* to the signal's own second
difference, so the torture signal has to be a hard one, not the vendored guitar sample.

**The second splice source: a dropped or inserted period, and the bridge that repairs it.** §9.5
item 8 made the output queue a knob, and both of its controls put a hole in the emitted waveform: a *cap*
refuses a period, and the ring is a FIFO of period-sized buffers, so the device plays `out(k-1)` and then
`out(k+1)` -- a 64-frame jump, 1.33 ms of missing signal, a step a listener hears as a click; a *floor*
inserts a period, and the step is at its entry. Neither can be repaired where it happens (the previous
period is already in the ring, and queued audio cannot be rewritten), so the repair is a **loop policy**
applied to the emitted copy between the render plan and the write -- which is what keeps it independent of
the oracle: the bridge writes `mono_emit` while the oracle keeps comparing the pristine `mono_out`, so
"the engine is sample-exact" and "the emitted signal is continuous" are two measurements of one run.

`rt::SpliceFader` is implemented and `--splice-fade-ms` wires it into the loop, and the shape was **chosen
by the detector, not by preference** -- it rejected three reasonable ones first, each with a number on a
997 Hz tone at amplitude 0.4 against a reference second difference of 0.0068 and a kappa = 2 limit of
0.0136:

  * **hold the last sample**: the first difference at the join is zero and the rule measures a *second*
    difference, so a hold scores the signal's own first difference -- 0.052, 3.8x over the limit.
  * **decaying extrapolation** `a1 + d*(1-w)`: zeroes the join and puts it all back at the next sample,
    because the slope collapses from `d` to zero in one step -- 0.0521, no better.
  * **a 5 ms crossfade**: holds the value for its first samples and ducks the signal for 5 ms. A swell,
    not a repair, and it fails for the hold's reason.

What works is matching **value and slope**: a cubic Hermite leaving from `a1 + d` (one sample of
extrapolation, which makes the join's second difference exactly zero -- the best score the rule admits)
with the value mismatch `(a1 + d) - x0` and the slope mismatch `(d - xs)*(K-1)` taken smoothly to zero
over `K` frames. The residual is ~|e0|/K^2 + |m0|/K, and the window length is measured rather than
chosen: 4 -> 0.0477, 16 -> 0.0213, 32 -> 0.0142, **64 -> 0.0097**, 128 -> 0.0082, 240 -> 0.0079. Long
windows are better for continuity and worse for the signal (the bridging offset is held for the whole
window), so the default is **K = 64, one period** -- the quantum the system is already built around, and
the shortest window that passes. Measured on the device (`soak-5min-bridge`, cap 3 + floor 2 + a 1.33 ms
bridge, 300 s): **6 bridges armed, 3 for dropped periods and 3 for inserted ones, 0 driver xruns, 0
render-path allocations, and the oracle still exact over 240 000 frames** -- the bridge repaired the
splices and the engine's own output was untouched, in the same run.

**The window-length test exists because it caught a real bug, and it is worth naming:** the first version
recomputed the two mismatches on every period of the bridge instead of freezing them at its start, which
re-injected the mismatch each period so the offset never reached zero. At K = 64 (a single period) that
is invisible; at K = 240 it showed up immediately. The lesson is the same one §9.5 keeps teaching -- a
parameter that looks cosmetic (`K`) can be the only thing that exercises a code path.

**The entry of an inserted period is fixed too, and it was the last audible step in the repair.**
The loop's bridge reaches the exit of an inserted period and cannot reach its entry, because the loop
never writes that period -- the backend does. A payload that merely *holds* the last frame removes the
step in *value* but leaves one in *slope*: the signal's slope goes to zero in a single sample, which the
continuity rule sees as a second difference of |slope|. The backend now writes a shaped payload instead
-- `rt::splice_insert_sample`, one cubic Hermite that continues the signal's value *and* slope at the
entry (`p[0] = last + slope`, which makes the second difference against the two real samples before it
exactly zero) and arrives flat at the exit, which is what the loop's bridge expects when it arms with
`Cause::Inserted`. It needs two frames of history rather than one, and it is format-agnostic because it
decodes and re-encodes through the same `pcm_*` helpers the rest of the I/O layer uses.

Measured by the same detector, over a window covering the *whole* inserted period so the entry is judged
rather than left out: the silent payload trips continuity, the held payload trips continuity, and the
shaped payload **passes every rule** with its worst second difference inside the allowance derived from
the signal itself (`tests/test_splice_fader.cpp`). **And it is verified on device now, both joints.** `--inject-insert-every S` is the insertion half of the
injector and the verifier judges an insertion over a window covering **entry, payload and exit** -- so a
payload cannot pass by having its entry left out, which is the whole point for the joint the loop cannot
repair for itself. With a clean span (ring 12, one injection every 10 s, 90 s: 8 inserted, 8 judged, 0
drops, 0 xruns, oracle exact, 0 allocations):

    worst second difference 0.053954 against a reference of 0.068797  =  0.8x

Below the signal's own worst curvature elsewhere in the same span, so the inserted period -- entry,
shaped payload and exit bridge -- is no longer distinguishable from the signal by the detector's measure,
exactly as the drop splice is not (4.3x -> 1.0x). Both splice sources are now verified end to end on the
device, and no change to the loop was needed for the insertion: the payload was right, the *measurement*
was wrong.

**Five bugs had to be found to get there, and every one of them was the same mistake in different clothes:
the neighbourhood or the state a measurement referenced was off by one period, or had already moved on.**

  1. the detector's guard was wider than the span, so it rejected the span -- and the rejection was
     swallowed, which looks exactly like finding nothing wrong;
  2. the spacing guard *deferred* a recording instead of aborting it, judging a block where no bridge was
     acting (955 "judged" splices whose raw and emitted numbers were identical to six decimals);
  3. the drop span omitted the last period the device had heard, which inverted the comparison;
  4. `rt::SpliceFader::apply()` advanced the anchor for periods the loop then refused, so the bridge left
     from the last thing it *computed* rather than the last thing it *played* -- invisible to a unit test,
     which hands the fader every period;
  5. the insertion recording fired in the same block as the insertion, using a history that omitted the
     very period the payload continues (an insertion happens *after* its own period, where a drop skips
     its write and so defers naturally), and the verifier's payload reconstruction read the fader's anchor
     a block late, after that block's commit had moved it.

Every one was found by comparing the emitted signal against itself on the device, none by the unit tests,
and the pattern is now the first thing to check in this area: **which period does this measurement think
it is standing next to, and has that state moved since?** Evidence: `bench-results/splice-injector.txt`.

### 10.4 RT-safety tests

`clang -fsanitize=realtime` on the render path in CI; a global `operator new`/`free`/
`pthread_mutex_lock`/`read`/`write` interposer that aborts when called from the audio thread; a 10⁶-block
randomised edit storm with the interposer armed; assertion that `frames <= max_quantum` always (G3).

**Implemented at M1 — and the boundary it forced.** `namchain::RtScope` marks the render path, and a
global `operator new`/`delete` replacement counts allocations and frees observed inside it — see
`src/rt/rt_check.cpp`, enabled by `-DNAMCHAIN_RT_CHECK=ON` (default) and off for a production daemon.
Building the instrument *first* settled a question the engine would otherwise have been written around:

> **The rule is about a scope, not a thread.** §3.3 says "no allocation, lock, syscall on the audio
> thread", but a device loop *must* issue read/write syscalls to move audio at all — read literally the
> rule is unsatisfiable. It means the engine's **render function**: everything between the backend's
> read and its write is in scope, and everything the backend does is out of it. That seam is now drawn
> in code in both soak paths, so the engine gets written into the right shape rather than retrofitted.

Division of labour, because neither instrument covers everything:

| instrument | covers | runs where |
|---|---|---|
| `RtScope` + allocation interposer | allocations and frees, in Release, any compiler | the target (GCC/aarch64) **and** CI |
| RealtimeSanitizer (`clang ≥ 20`) | allocations, **locks and syscalls**, via `[[clang::nonblocking]]` | CI only (clang-only) |

Known limitation, stated rather than implied: interposing `pthread_mutex_lock` or `read`/`write`
portably would mean preempting libc symbols process-wide, so locks and syscalls are left to RTSan, and
a raw `malloc` on the render path would also go unseen. Both are reasons the RTSan job stays.

**First result:** `A2-Full`, `A2-Lite` (the adopted planar kernels) and the IR path are
**allocation-free across buffer sizes 1…256** after `Reset()`, verified on the target. Upstream's own
allocation test only ever covered the generic `a2_fast` path, so this is the first such check of the
code vendored in `vendor/patches/0001`.

### 10.5 Soak, fuzz, and tools

8–24 h soak on bare-metal Pi 5 with periodic edit storms, thermal logging, `vcgencmd get_throttled`,
asserting zero xruns and reporting p50/p99/max block time. libFuzzer targets for the `.nam` loader and
the control-protocol parser (malformed input must produce errors, never crashes/hangs/OOM). On-device
tools: `cyclictest`/`rtla timerlat`, `perf stat` (round-trip latency is measured by
`namchain-latency` instead of `jdelay`, which would need JACK — §4.3)
for cache/cycles, Instruments + `os_signpost` on Apple.

#### 10.5.1 How "zero xruns" is actually established

**Listening cannot establish it, and is not required.** An xrun is a *timing* failure — the DMA ring
buffer ran dry (playback) or overflowed (capture) because the app missed a period deadline — and the
kernel reports it. A single dropped 64-sample period is 1.3 ms and is frequently inaudible, while an
*audible* click may be our own engine glitching while the driver reports nothing. Human listening is a
sanity check for "right sound"; it is neither sensitive, reproducible, nor diagnostic.

Three programmatic signals, all of which suit the no-libasound `alsa_raw` design (§4.3) because they
are plain ioctls:

1. **Return codes.** `readi`/`writei` return `-EPIPE` on xrun (`-ESTRPIPE` on suspend, `-ENODEV` on
   unplug). **Rule: every one is counted and reported; the engine never silently re-`prepare`s.** A
   recover-and-continue handler is how hosts end up believing they never xrun.
2. **PCM state.** `SNDRV_PCM_IOCTL_STATUS` → `state == SNDRV_PCM_STATE_XRUN`. The same state is
   readable out-of-process from `/proc/asound/card*/pcm*/sub*/status` (`state: RUNNING`, `avail`,
   `avail_max`), which gives a supervisor that can watch for transitions without being in the audio
   path. Note this kernel (6.8.0-1064-raspi at the time) exposes **no `xrun_debug` knob** on
   `snd_pcm`, so app-level detection is the reliable route, not kernel logging. Re-check on 7.0.0
   before relying on it either way: the claim is about a knob, and kernels do grow knobs.
3. **Timing margin.** Timestamp every block and report p50/p99/max against the deadline. This is the
   *proactive* half: if p99 stays far under the deadline, xruns are structurally impossible, and
   near-misses that never became xruns are still visible. Waiting to observe an xrun proves less and
   takes longer.

**The stronger test is an oracle, not a counter.** Because the engine is deterministic and A2 measured
block-size invariant (§10.2), the audio a real-time run produces can be compared sample-for-sample
against the offline render of the same input. Sample-identical output proves no sample was dropped,
duplicated or reordered — a *positive* result — and it catches the failures ALSA never sees (model
switches, transition bugs; the M2 detector's whole premise). Xrun counters validate the host and
driver; the oracle validates the engine. Both are required.

**Does the input have to be non-silent? Measured — no, not for A2 on this build.** The concern was
real: NAM's silence handling (§7.5) is supposed to skip work below −80 dBFS, so a silence-based soak
would under-load the CPU and could "pass" where a real signal would xrun. So it was measured rather
than assumed. 4 × A2-Full at q = 64, 30 s, looping the fixture from 0 s (half silence) against looping
from 1 s (all signal):

| loop start | p50 | p99 | max |
|---|---|---|---|
| 0.0 s (starts in silence) | 326.3 µs | 439.3 µs | 848.5 µs |
| 1.0 s (all signal) | 317.2 µs | 472.5 µs | 904.5 µs |

Silence is not cheaper: the adopted planar fast path (§4.2) has no silence short-circuit, so the
p50 differs by 2.8 % in the *noisy* direction and the difference is not systematic. `--input-offset`
therefore stays — it is still the right thing for the offline harness, where the fixture's 1 s of
digital silence followed by 1 s of signal would otherwise make half the run unrepresentative — but a
device soak with nothing plugged into the input is a valid measurement of load, and that is what the
1 h criterion-3 run uses. Had this gone the other way, the device soak would have needed a source, and
the plan would say so here.

**Layered, because the device determines what can be concluded:**

| layer | what it proves | what it cannot |
|---|---|---|
| `null`/`file` paced run + self-oracle (**implemented**, `ctest -R soak_cli`) | engine determinism and CPU margin under a wall-clock schedule; sample-exactness of the block loop | nothing about the driver, DMA clock, or true latency |
| `snd-aloop` (virtual, loaded on this box) | a real capture/playback API feeding the engine from a file loop; genuine xruns of that device | its periods come from a timer, not hardware; cannot validate the USB stack or round-trip latency |
| USB interface (bare metal) | the real driver, DMA clock, USB scheduling and IRQ behaviour — the authoritative run | — |

**The authoritative run (no human needed):** 1 h at 48 k/64 counting `-EPIPE`/XRUN with timestamps;
block-time p50/p99/max against the deadline; true round-trip latency measured with a **physical cable
from output back to input** and with the device's internal loopback, using a pseudo-random stimulus and
cross-correlation (what `jdelay`/`jack_iodelay` automate) rather than a stopwatch;
`cyclictest`/`rtla timerlat` for host scheduling latency, which
degrades before xruns appear; `vcgencmd get_throttled`/`measure_temp` for thermals; and the oracle diff
on the captured output.

**Implemented at M0/M1:** `namchain-soak` runs a bounded soak and applies the criterion as a
machine-checkable gate (exit 0/1), reporting the evidence as text or `--json`: block percentiles,
deadline overruns, `driver_xruns` (a first-class field, driven by the ALSA backend and structurally 0
for the backends with no driver), `max_lateness_ms`, and the oracle result. `--input-offset` exists so
an offline soak runs in the signal rather than in the fixture's silence, and `--backend` selects the
I/O layer (§9.1) so the same loop runs against a device, a paced clock or a WAV.

**The oracle now runs in device mode too, which is what criterion 3's last clause needed.** On a device
the input is live capture, so "the same input" means recording it: the run keeps the first
`--oracle-window` seconds (default 60) of the input it consumed and the output it produced, then pushes
the recorded input through a *freshly compiled* plan of the same graph, prewarmed the same way, and
compares frame by frame. It runs after the stream has stopped, with its own arena, because the first
plan's state is the thing under suspicion.

The window is a stated bound rather than an unstated one: an hour of captured input is 1.4 GB, and "the
first 60 s are sample-identical to the offline render" is a weaker claim than "the whole run is". The
rest of the hour is covered by the xrun and timing gates. The recording copy happens *after* the timed
section on purpose — charging the oracle to the block would make it change the number it exists to
validate — and the run reports what it did either way. Verified at 6 s: `max|diff| = 0` over 288 000
frames, 4 × A2-Full, through the device.

---

## 11. Performance — MEASURED on the target host (2026-09-11)

**This section previously contained estimates. These are measurements.** Host: Raspberry Pi 500
(BCM2712, 4× Cortex-A76 @ 2.4 GHz), Ubuntu 24.04 / kernel 6.8.0-1064-raspi at the time of
measurement (§0 records the 2026-09-13 upgrade to 26.04 / 7.0.0-1017-raspi, which every number below
predates), `performance` governor, 40 °C, `throttled=0x0`. Engine configuration: 48 kHz,
`NAM_ENABLE_A2_FAST=ON`, `double` samples, single-threaded, `-O3`, 2 s of audio × 10 iterations, RTF
excludes `Reset()`/prewarm. Produced by `namchain-bench` and cross-checked against the core's own
`benchmodel_bufsize` (agreement within 2 %).

**These numbers are the `a2_fast` kernels and therefore conservative.** They were measured before the
PR #313 planar kernels (§7.8) were on by default on AArch64, and the table was not re-run afterwards:
the effect is visible in the device soak, where 4 × A2-Full in series now costs **chain p50 ≈ 300 µs**,
against the 4 × 142.6 µs = 570 µs this table implies. Re-running `namchain-bench` through the planar
path is an M2 cleanup; until then read this table as a floor on cost, not the current cost.

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
| Concurrent audible A2-Full (8ch) | ≥ 4, p99 ≤ 50 % deadline, 0 xruns/1 h | **engine work met, host tail not**: the one-hour run of 2026-09-14 held RTF 0.9999 at 23 % of one core with chain p99 39.72 %, block p99 41.19 %, 0 render-path allocations and an exact oracle -- and still failed the gate as worded, with 92 driver xruns and 69 dropped periods driven by ~1100 bursts of host interference that the engine neither caused nor can schedule around (§9.5 item 7). The queue cause of the 09-13 failure is fixed (`cb5cc19`); this is a different failure with a different fix, and the gate wording is an open decision (§14) |
| Concurrent audible A2-Lite (3ch) | ≥ 12 | **met** (17 at 50 %) |
| Resident switchable instances | ≥ 16 (≥ 64 if RAM allows) | **RAM supports ≥ 64**; switching itself is M3 |
| Switch ready-time (p99, warm set) | ≤ 5 ms, zero click | M3 |
| Cold switch (deferred) | ≤ 15 ms, zero gap, `exact` when possible | M3 |
| Added CPU for `balanced` warming | ≤ 2 × RTF of hot set | M3 |
| Engine latency @48 k/64 q | 1.33 ms I/O + 0 algorithmic | **1.33 ms I/O confirmed on the SSL (period 64 frames, 4 periods); 0 algorithmic, and every M1 built-in is zero-latency** |
| Block-time jitter | p99/max measured and published | **measured above** (max/p99 is noisy: see the q=64 outlier) |

**The engine, measured through the graph compiler on a real device** (SSL 2+ Mk II, `hw:4,0` on the
pre-upgrade boot and `hw:2,0` after it — the card number is probe-ordered, not a property of the
interface,
UAC2/S32_LE, q = 64, 4 periods, `SCHED_FIFO` 80 + `mlockall`, `namchain-soak` driving a compiled
`RenderPlan` — not a private model loop). Steady state; the two bring-up xruns the device produces
before the measured loop are reported separately, as always.

| Rig | nodes | buffers (naive) | p50 | p99 | p99 % deadline | xruns | allocs |
|---|---|---|---|---|---|---|---|
| 2 × A2-Lite in parallel, 60 s | 6 | 4 (7) | 35.8 µs | 100.9 µs | 7.57 % | 0 | 0 |
| 2 × A2-Full in parallel, 30 s | 6 | 4 (7) | 157.7 µs | 203.9 µs | 15.29 % | 0 | 0 |
| 4 × A2-Full in series, 30 s | 6 | 3 (5) | 323.3 µs | 466.0 µs | 34.95 % | 0 | 0 |

Three things this settles:

- **The compiler's indirection is free.** The 4 × A2-Full row is the same workload the earlier
  hand-rolled chain loop measured at p50 327.9 µs / p99 433.8 µs (32.5 %): through the plan it is
  323.3 / 466.0 µs (34.95 %). p50 is inside run-to-run noise and slightly *better*; the p99 difference
  is the Pi's scheduler, not the engine. A graph of six nodes and three buffers costs what five
  values and no graph cost.
- **The memory plan does something on real rigs, not just in unit tests.** A two-chain rig needs 4
  buffers where one-per-value needs 7; the serial chain 3 where it needs 5. Both numbers are printed
  by every soak, so the claim is re-measured on each run rather than asserted once.
- **Nothing allocates in the render path, from the first measured block.** That required warming the
  plan before the loop: a partitioned convolution builds its FFT plan lazily on first `process`, which
  showed up as exactly one allocation (duration-independent) inside the `RtScope`. `RenderPlan::prewarm`
  removes it, and it is sample-neutral — the A2 self-oracle against the core's own render tool is still
  `max|diff| = 0` with warming on. The soak now warms the plan *and* the reference interpreter, so the
  oracle stays like-for-like and the allocation count is an unconditional claim.

The 60 s row also shows the honest spread: p99 was 53.7 µs over 5 s and 100.9 µs over 60 s on the same
rig, so p99 grows with the observation window on a desktop-class OS. Short soaks understate tail
latency; criterion 3's "1 h" is not a formality.

**Caveats on these numbers:** the first two tables are single-threaded throughput measurements with no
audio device attached, taken on a bare-metal host with a desktop-class OS (no `threadirqs`, no
PREEMPT_RT, no `isolcpus`). They establish the CPU budget, not xrun behaviour. The device tables further
down are the ones that speak to xruns, and they were taken with the SSL 2+ Mk II attached (a later
addition — §0's original inventory had no capture-capable device, and risk 9 below is closed by it).

Levers, in measured order: **planar NEON A2 kernels (PR #313 — 2.0–3.1×, written and verified, §7.8)**
→ memory traffic (weight sharing, larger quantums, cache blocking) → A2 fast path coverage incl.
slimmable submodels (§7.8) → FP16/dot-product kernels on A76/Apple (accuracy-validated) →
multi-thread parallel rendering. **Parallel rendering is M4 and off by default**, because RT-safe
worker synchronization is the single largest reliability risk in the design; it is admitted only if
soak + glitch tests pass with it enabled. With PR #313 in place the arithmetic budget roughly doubles,
so parallel rendering's urgency drops further and the attention belongs on memory bandwidth and I/O.

### The ARMv7 target, measured (2026-09-16)

tib (Tinker Board S, RK3288 Cortex-A17, armv7l), kernel `7.2.6` PREEMPT_RT, governor performance at
1416 MHz, `namchain-bench` at 48 kHz with `double` samples. **RTF is CPU-seconds per audio-second, so
RTF < 1 is faster than real time.** Full account in `bench-results/tib-summary.txt`.

| model | q | shipped `-mfpu=vfpv3-d16` | `-mfpu=neon-vfpv4` |
|---|---|---|---|
| **A2-Full** | 64 | RTF **1.3251**, p50 1764.9 µs, p99 136 % of deadline | RTF **0.5199**, p50 690.1 µs, p99 55 % |
| **A2-Lite** | 64 | RTF 0.1249, p50 165.1 µs, p99 13.2 % | RTF **0.0842**, p50 110.2 µs, p99 9.6 % |
| A2-Full | 32 / 128 / 256 | 1.3717 / 1.2805 / 1.2659 | 0.5356 / 0.5132 / 0.5120 |
| A2-Lite | 32 / 128 / 256 | 0.1336 / 0.1206 / 0.1187 | 0.0897 / 0.0810 / 0.0788 |

Instances at q64, by the 50 % p99 gate / by the deadline (100 %):

| | shipped | NEON |
|---|---|---|
| A2-Full | 0 / 0 | 0 / **1** |
| A2-Lite | 3 / 8 | **5** / 11 |

Three readings of this table. **The shipped armv7 default cannot run A2-Full at all** — one instance
needs 133 % of a core and its median block already exceeds the deadline. **NEON gives exactly one
A2-Full per core**, which is genuine real time (2 × 690.1 = 1380 µs shows a second would not fit even
at the median). And **criterion 3's "≥ 4 concurrent A2-Full" is unreachable on this target for a reason
about the engine rather than the board**: it renders single-threaded on one pinned core, so four
instances need 4 × 0.52 = 2.1 cores of throughput; the p99 gate compounds it but is not binding. On
piv the same model is RTF 0.0501, so four cost 0.2 of a core — which is what the clause encodes.

The device side, once a powered hub was inserted (see §9.4): a 1 h soak with 3 × A2-Lite at q64 and
cap 3 gives **0 driver xruns, 0 deadline overruns, max block 748.7 µs (56 % of the deadline)**, queue
p50 1.94 periods, RTF 0.9999 and an exact oracle — **the first clean criterion hour in this project.**
piv's two hours gave 15 and 109 late blocks and both failed; the caveat is in the summary (the loads
differ by 31 % against 38 % of the deadline, and piv's own two hours differed by 7×, so this does not
rank the hosts — it is consistent with the memory-contention explanation of piv's tail).

Under load, tib degrades: `stress-ng --cpu 4 --io 2 --vm 1` + `hackbench` on all four cores, with no
reservation to protect the audio thread, produced 36 xruns and 1138 late blocks in 5 minutes with the
engine's own CPU time more than doubled (chain p99 410 → 851 µs) at a pinned 1416 MHz.

### The RT host, measured (2026-09-16)

The same engine, same rig, on `7.0.0-1017-raspi-realtime` with cores 2–3 isolated and tickless and the
audio IRQ pinned to cpu 2. Full evidence in `bench-results/rt-piv-summary.txt`.

| clause | non-RT (1 h, `331da80`) | RT + isolation (H1 1 h) | RT (H2 1 h) |
|---|---|---|---|
| driver xruns | 8 (0 cap, 8 play) | **0** | **0** |
| blocks over the 1333.3 µs deadline | 204 | 15 | 109 |
| max block | 3427.7 µs (2.57×) | 1782.5 µs | 3176.4 µs |
| p99 block | 501.1 µs (37.6 %) | 524.7 µs (39.4 %) | 531.6 µs (39.9 %) |
| chain p99 | 485.2 µs | 505.4 µs | 511.6 µs |
| allocations / oracle / throttle | 0 / 0 / 0x0 | 0 / 0 / 0x0 | 0 / 0 / 0x0 |

**The xrun clause is fixed; the late-block clause is not, and the reason is memory.** Over 4 h 10 min
of RT soak time at ≤ 4 instances there were **zero** driver xruns. The remaining late blocks have
`cpu == wall` with 0 blocks descheduled in every arm, so they are work, not lateness — and the work is
stalling. With the load on the *housekeeping* cores (300 s per arm):

| load on cpus 0-1 | xruns | block p99 | block max | instructions/block | cycles/block | IPC |
|---|---|---|---|---|---|---|
| none | 0 | 513 µs | 1288 µs | 1.472 M | 0.864 M | 1.70 |
| `stress-ng --cpu 2` | 0 | 493 µs | 1270 µs | — | — | — |
| `hackbench` loop | 0 | 822 µs | 1078 µs | — | — | — |
| **`stress-ng --vm 1 --vm-bytes 128M`** | **5 460** | **2793 µs** | **10 020 µs** | 1.433 M | 2.081 M | **0.69** |

Pure CPU load on both housekeeping cores is fully tolerated; memory stress alone is catastrophic. The
same instructions take 2.4× the cycles with *no change* in cache misses per block, at a constant
2400 MHz (`scaling_cur_freq` identical in all 382 samples) and `throttled=0x0`. That is the
memory-bound ceiling of risk 5 below, measured rather than assumed, and it means the levers above are
in the right order — but it also means **core isolation is not a substitute for a quiet memory
system**. Two other measured consequences: the cap can come down to 3 periods (0 drops and 0 refusals
over a full hour, p99 528 µs, max 1422 µs, queue held at 1.94 periods — 1.33 ms of latency saved), and
6 × A2-Full costs 1.9× the p99 of 4 rather than the 1.5×
the arithmetic predicts, i.e. the throughput ceiling is memory-shaped too.

---

## 12. Milestones

| M | Deliverable | Acceptance |
|---|---|---|
| **M0** | Phase 0 environment fixed ✔; repo, CMake, CI, vendored core pinned ✔; `namchain-bench` measuring RTF per model/quantum ✔; lookback calculator + golden test vs. hand-computed A2 numbers ✔; **`namchain-render` offline WAV→WAV with `file` I/O ✔ (+ `namchain-wavcmp`, determinism / block-invariance / golden tests)** | Builds green **in CI on all four jobs** — x86_64 (18 cases / 192,245 assertions), x86_64 under RTSan, aarch64 cross + qemu (full suite *and* the 146-assertion planar parity gate), armv7 cross ✔; bench table for A2-Full/A2-Lite/IR on this Pi 500 ✔; offline render byte-identical to the core's own `render` output ✔; §11 table revised with real numbers ✔ |
| **M1** | `libnamchain` core: **RT primitives, arena, command ring, epoch reclamation ✔**; **graph IR + validation + compiler + liveness/colour memory plan + `RenderPlan` execution ✔**; **rig JSON loader + `chain_graph` ✔**; **reference interpreter as the compiler's oracle ✔**; **built-in nodes `nc.input/output/gain/split/mix/nam/cab.ir/eq.biquad/delay/unit_delay` ✔**; static DAG render with block-delay feedback ✔; **`alsa_raw` backend ✔ — raw ioctls, full-duplex, xrun counting, verified on the PCM2902 and the SSL 2+ Mk II (§9.5)**; **`AudioBackend` interface with `null` and `file` implemented and `pipewire` refused by name ✔ (§9.1) — the stream loop is now written once and runs against a device, a paced clock or a WAV, so `ctest -R soak_cli` exercises the timers, the gates and the oracle with no hardware**; **memory locking made safe and the block timer sized from the run — `MCL_FUTURE` plus a fixed 44 MB timer reserve aborted the `null` backend with `std::bad_alloc` under a finite `RLIMIT_MEMLOCK`, and `soak_cli` now reproduces that limit ✔ (§14.10)**; **multichannel WAV reader ✔**; **daemon skeleton — `namchaind`: JSON config + CLI, Unix `SOCK_SEQPACKET` control socket with the versioned JSON protocol (`engine.hello/status/mute/panic`, `rig.load/get`), startup plan compiled and prewarmed before the clock, `SIGHUP` reload, plan publication through the SPSC ring with epoch reclamation, and device-loss survival (state `lost`, retry and reopen) ✔; systemd unit with `LimitRTPRIO`/`LimitMEMLOCK`/`CPUSchedulingPolicy=fifo` (`packaging/systemd/namchaind.service`) ✔**; **N-way `split`/`mix` arity — per-spec `"outputs"`/`"inputs"`, resolved by validation, the compiler and the oracle together; `rigs/four-chains.json` passes the self-oracle at `max|diff| = 0` ✔**; **A2 planar kernels live on AArch64 with the bit-identity parity gate in CI (§4.2, §7.8)**; **`namchain-soak` acceptance harness — block percentiles, xrun counters, offline and in-process oracle (§10.5.1)**; **the device soak runs a compiled `RenderPlan`, not a private loop ✔**; **the loop's cadence and its output-queue policy — `sw_params` now sets `avail_min` to the period so `poll` wakes on the boundary the loop consumes at, and the one-period prime plus the resulting occupancy ratchet are diagnosed in §9.5 items 2 and 4**; **the output queue became a knob — `--output-queue-periods` caps the queue, `prefill_output_queue` establishes it before block 0, the xruns are split into capture overruns and playback underruns, and the hour at `--periods 6 --output-queue-periods 4` reports 8 playback underruns against 92 for the 4-period ring, measured in §9.5 item 8 ✔** | Plays a guitar through 2 chains at 64 samples — **met on the SSL 2+ Mk II: 2 parallel A2-Lite chains, p99 53.7 µs = 4.0 % of the 1.333 ms deadline, 0 xruns, 0 allocations (§11)**; **device-mode self-oracle ✔ — the run records the input it consumed and compares its output against a freshly compiled offline render of it, `max|diff| = 0` over 2 880 000 frames (§10.5.1)**; **1 h soak vs §1 criterion 3 — re-run on the RT host 2026-09-16, *five of six clauses met*: driver xruns 0 (was 8), 0 render-path allocations, p99 39.4 % against the ≤ 50 % gate, no throttle flag, oracle `max|diff| = 0`; the clause still failing is `max block < deadline` — 15 and 109 late blocks in two consecutive hours, maxima 1.78 and 3.18 ms, `cpu == wall` in every arm so the residue is work and not lateness, and the work is memory-stall shaped (§9.3, §11, `bench-results/rt-piv-summary.txt`)** **On ARMv7 the throughput clause is read as one A2-Full within 90 % of one core (amended 2026-09-16, at the user's direction), and tib meets it over a full hour: 1 × A2-Full at q64 gives 0 driver xruns and 0 deadline overruns of 2 700 000 with block p99 58.6 % of the deadline and a worst block of 1140.7 µs (85.6 % of the deadline, so inside the bar), with the 300 s arm agreeing to within 0.14 points and 5 µs; 3 × A2-Lite likewise runs a clean hour, with max 748.7 µs (`bench-results/tib-summary.txt`); allocation interposer clean; **true round-trip latency measured ✔ — `namchain-latency` over both a physical loopback cable and the device's internal digital loopback, giving the converter round trip as an exact 0.333 ms difference, and the ring occupancy measured and removed so the residual spread is reported rather than assumed (§9.5)**; **parity test green at release flags** |
| **M2** | Graph compiler + memory planner + **patch protocol ✔ — `graph::apply_patch` (`add_node`/`remove_node`/`connect`/`disconnect`/`rewire`/`swap_model`/`bypass`/`set_param`), applied all-or-nothing to a copy; `rig.patch` routes smooth vs structural and reports which**; transition ops (**param ramp ✔ — `NodeType::set_param` + `nc.gain` over a smoothstep, reached from `rig.patch` and the timeline**; **structural dual-render ✔ — `rt::Crossfade` equal-power over `fade_ms`, one transition in flight with a coalescing pending slot, old plan retired**; **bypass ✔ — a `NodeSpec::bypass` flag: the node keeps running (state warm, G14) and its output becomes its dry input, crossfaded by the structural transition; `tail_hold` not built**; **failure isolation ✔ — a per-node non-finite guard takes a faulty node out of the path and reports `failed_nodes`/`guard_trips`**); **`namchain-render` edit timeline ✔ — `--graph` with `--timeline`/`--torture`, sample-accurate `set_param` ops judged per edit by before/after oracles**; **glitch detector ✔** | Torture render passes all 4 metrics at all quanta/rates for non-NAM nodes (**partial: the `smooth` class and the structural crossfade pass, each with a negative control failing as it must; the timeline does not yet drive a structural edit, and the full 60 s / 500-edit / all-quanta sweep is not run**); RTSan clean in CI |
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
7. **ALSA-without-libasound** was real work (~600–900 lines of ioctl plumbing) and loses the
   `plug`/`dmix` conveniences. **Largely resolved:** `alsa_raw` is written and measured, and the
   `AudioBackend` interface plus `null`/`file` (§9.1) means nothing above the I/O layer depends on it.
   Residual: `plug`-style format/rate conversion is not implemented, so a device that cannot do 48 kHz
   exactly is still refused rather than converted (§9.2).
8. **Thermals on Pi 5** (throttle from 80 °C) can silently destroy the CPU budget. Mitigation: health
   loop + governor + mandatory cooling for always-on use.
9. ~~**Measurement host has no capture-capable audio device** (§0).~~ **Closed 2026-09-12** by the
   SSL 2+ Mk II (§9.5), which is the reference interface the I/O layer was measured against. Residual:
   thermals under a sustained hour of load are only now being observed, and one interface is not a
   survey — the latency and xrun figures are that device's, and the plan says so wherever they appear.
10. **`mlockall` can kill the process it is meant to protect.** `MCL_CURRENT|MCL_FUTURE` charges every
    mapping made afterwards against `RLIMIT_MEMLOCK`, so a limit generous enough for the pages already
    mapped is not necessarily generous enough for the next large allocation — and the failure is a
    `std::bad_alloc` inside a process that has just been told its memory is locked. CI found exactly
    that, on the `null` backend with no device and no audio involved: `mlockall` succeeded at ~6 MB of
    RSS and the next allocation, a 44 MB `BlockTimer` reserve, came back `EAGAIN`. The developer host
    allows ~1 GB of memlock and never saw it, which is why it survived until CI. **Fixed 2026-09-13,
    both halves:** the lock uses `MCL_CURRENT` alone, after the model, the plan and every stream buffer
    exist; and `BlockTimer` reserves the series its caller expects instead of a fixed "one hour at 32
    frames", which was 44 MB × 5 timers on a one-second run. `soak_cli` now runs the `null` soak under
    an 8 MiB limit, which reproduced the abort before the fix and is the reason it cannot come back
    unseen. Residual: on a host whose hard limit is below the working set, `mlockall` fails and the
    soak reports that as a caveat — the measurement then includes page faults, which is why the report
    prints the lock status next to the percentiles instead of assuming it.
11. **Host RT capability is not ours to choose.** A user's machine may offer no PREEMPT_RT kernel and no
    way to quiet it, and this box is the exception rather than the rule (the Pi can run
    `linux-image-rt-*`; a laptop cannot be told to stop running the desktop). Mitigation, and the reason
    §9.3 states it as policy: nothing above the I/O layer may depend on kernel latency behaviour, the
    loop must degrade by *choosing* a discontinuity rather than by blocking, and the acceptance criteria
    are written against engine work and driver-reported xruns so that they mean the same thing on both
    kinds of host. Deferred decision: if a device we want to support can have neither an RT kernel nor a
    quiet one, the fallback is a dedicated non-Linux RTOS, which would cost us ALSA/USB-class
    compliance and the current I/O layer — the `AudioBackend` interface (§9.1) is what keeps that door
    open, and this is deliberately not being decided now.

**Assumptions** — engine runs at 48 kHz internally; graphs are DAGs; permissive-only dependencies (with
the ALSA note in §4.3); no UI in this project; `.nam` files are user-supplied, not redistributed; Linux
is the reference platform for RT behaviour, with PREEMPT_RT assumed available on the reference device
and *not* assumed on a user's host (risk 11, §9.3).

**Open decisions you may want to change** (each has a default in the plan): default warming policy mode
(`balanced`); `fade_ms` default (5 ms) and `max_defer_ms` (15 ms); whether to accept LGPL
`libasound`/JACK backends; whether §7.8 patch #3 (shared weights) is worth carrying a standing fork
patch; project name (`namchain` is a placeholder); **the output queue's cap and ring size**, which are
now the latency-against-dropout-tolerance knob and the most consequential of these. §9.5 item 7
measured what each end of the old loop cost (1-2 periods queued: 41 % block p99 and 92 xruns/hour; a
saturated ring: the same engine work reported as 112 % and 23 xruns/hour); §9.5 item 8 swept the knob
and found the shape of the trade -- the ring size is the cushion and is free, the cap is the latency
and must sit at or above the level the queue reaches by itself -- with `--periods 6
--output-queue-periods 4` as the measured recommendation (8 playback underruns and 0 capture overruns
in an hour, queue held at 3.59 periods = 4.8 ms, against 92 xruns for the 4-period ring). **Updated
2026-09-16:** on the RT host the queue's natural band is tighter (p1 2.72 / p50 2.94 periods) and
`cap 3` does not manufacture drops -- it settles the queue lower instead (p1 1.69 / p50 1.94) and, over
a full hour, held 0 xruns, 0 drops and 0 cap refusals with one late block; so
**`--output-queue-periods 3` with the floor off is now the recommended cap and is confirmed over the
criterion's own duration, worth 1.33 ms of latency** (4.000 ms bound against 5.333 ms); the ring stays
6. Note the floor must be off at cap 3, not 2: cap and floor one period apart is the squeezed-queue
configuration that thrashes (§9.5). And **whether
a queue floor controller should insert a period of silence when the occupancy falls too low**, which is
the only mechanism that can hold a floor the loop cannot produce audio for, and which wants the
occupancy-distribution instrument first; a floor of 2 never fired once in 4 h 10 min of RT soak time,
so the default stays off until a configuration is found that needs it; and **whether `--cpu` defaults to
pinning at all**, or
stays off until `namchain-doctor` can pick a core and report the IRQ mask it shares it with.
**Resolved 2026-09-16 for the reference host:** pin, and pick the core by measurement. A balanced
A B B A screen (cpu 3, tickless and IRQ-free, against cpu 2, co-located with the prio-85 audio IRQ
thread) found no material difference -- clean arms at p99 533 vs 505 µs and max 1201 vs 1172 µs, one
sample each -- so `--cpu` defaults to pinning and the *specific* core does not matter, provided it is
one of the isolated ones. A prediction was wrong here and the arm says so: cpu 2 was expected to
re-arm the 1000 Hz tick (a second runnable task on a `nohz_full` core), and measured 2.0 timer
interrupts/s against cpu 3's 0. `namchain-doctor` should still check the IRQ mask, because the same
question on a board with fewer cores or without `nohz_full` has a different answer.

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
- Audio-stack licensing, read directly rather than taken on trust: JACK1 `COPYING` (the `libjack/`
  tree is LGPL, "in order to allow for proprietary usage of Jack") —
  https://raw.githubusercontent.com/jackaudio/jack1/master/COPYING ; JACK2 `COPYING` (GPL-2.0 for the
  tree) — https://raw.githubusercontent.com/jackaudio/jack2/develop/COPYING ; JACK2 public client
  header `common/jack/jack.h` (LGPL-2.1-or-later per-file) —
  https://raw.githubusercontent.com/jackaudio/jack2/develop/common/jack/jack.h ; PipeWire `COPYING`
  (MIT/Expat) — https://gitlab.freedesktop.org/pipewire/pipewire/-/raw/master/COPYING ; and the
  packaged JACK implementation's license field (**MIT**) —
  https://packages.fedoraproject.org/pkgs/pipewire/pipewire-jack-audio-connection-kit/

Local environment facts were established by direct inspection (no shell available): `/proc/cpuinfo`
(Cortex-A76, part `0xd0b`), `/etc/os-release`, `/etc/apt/sources.list.d/debian.sources`,
`/proc/1/comm`, and package-path probes under `/usr/share/doc/`.

