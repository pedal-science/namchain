# Testing namchain M1 on tib — the ARMv7 target

**Status:** proposed; Gate 0 not started. Everything marked *[verified]* was read off tib or this
checkout while writing.
**Written:** 2026-09-16. **Host:** tib (Tinker Board S, RK3288, Cortex-A17, armv7l, 4 cores).
**Companion:** `plan-rt-piv.md` (the piv round, finished), `rtpi@ff08927` (the kernel work on tib).

---

## 0. Why tib, and what it can and cannot answer

piv answered "does an RT kernel plus core isolation fix the M1 tail". tib answers a different
question, and it is the one §9.4 has been carrying as a prediction since M0:

> **ARMv7**: 32-bit VA and bandwidth limits ⇒ resident-model budget is configurable and small by
> default; NEON is runtime-detected for redistributable packages; **FMA-less NEON halves
> FLOPs/cycle, so A2-Full counts will be much lower there** — the engine should prefer A2-Lite and
> aggressive slimming on that target.

tib is that target, and it also differs from piv in three ways that change what a measurement means:

| | piv | tib |
|---|---|---|
| CPU | 4× Cortex-A76 @ 2.4 GHz, out-of-order, FP16+dot-product | 4× **Cortex-A17** @ 1.8 GHz, in-order, **no vector FMA** |
| kernel | Ubuntu `raspi-realtime`, `isolcpus`/`nohz_full` on 2-3 | mainline **7.2.6 PREEMPT_RT**, `NO_HZ_IDLE` only, **no isolation at all** |
| device | SSL 2+ Mk II, UAC2, 8 capture / 6 playback, 32-bit | **not attached** — see §2 |

Three consequences, stated before anything is run:

1. **There is no reservation to hide behind.** tib has no `isolcpus` and no `nohz_full` (its config
   fragment deliberately declines both: *"on a 4-core board running a full desktop-ish Armbian
   userspace there are none to spare"*). The RT kernel's own scheduler is the *only* protection, so
   a good result here is a stronger statement about the kernel than piv's was — and a bad one is
   less diagnosable, because there is no isolated core to compare against.
2. **The CPU ceiling is the point.** piv's finding was that the engine is memory-bound before it is
   CPU-bound. On a 1.8 GHz in-order A17 with the scalar FP path, CPU is the binding constraint
   again, which makes tib the measurement that sizes §9.4's "much lower".
3. **The deviceless arms are the reliable ones.** Any device arm depends on §2, so the plan is
   ordered to produce its most important number first, with no hardware dependency.

---

## 1. What is in force — verified, not assumed

| property | value | how it was read |
|---|---|---|
| kernel | `7.2.6 #1 SMP PREEMPT_RT` (mainline build, 2026-09-15) | `uname -a` on tib |
| `CONFIG_PREEMPT_RT`, `HZ=1000`, `NO_HZ_IDLE` | `y` | `/boot/config-7.2.6` |
| `CONFIG_NO_HZ_FULL`, `CONFIG_RT_GROUP_SCHED` | **not set** (both deliberate) | same |
| governor | **`ondemand`** at 1 800 MHz (needs setting per arm) | cpufreq |
| isolation | `/proc/cmdline` has no `isolcpus`/`nohz_full`/`irqaffinity` | tib |
| `SCHED_FIFO` for user `rik` | **refused** — `ulimit -r 0`, `ulimit -l` 8192 KB, no `audio.conf` | `chrt -f 50 true` |
| `SCHED_FIFO` via `sudo -n` | **works** (CAP_SYS_NICE is not subject to `RLIMIT_RTPRIO`) | `sudo -n chrt -f 50 true` |
| compiler on tib | **none** — no gcc/g++/cmake/ninja/make | `command -v` |
| `cyclictest`, `rtla`, `stress-ng`, `hackbench` | present; **`perf` is not** | `command -v` |
| thermal | 44.5 °C idle; the rtpi suite once hit the 90 °C critical trip | rtpi README |
| RAM / disk | 2 GB / 5.1 GB free on `/` | `free`, `df` |

**Reference latency already measured on this kernel** (rtpi, `results/tib-7.2.6-*`, under
`stress-ng --cpu 4 --io 2 --vm 1` + `hackbench`, performance governor, capped 1416 MHz):

| | q64 | q128 | q256 |
|---|---|---|---|
| `max` | **245 µs** | 299 µs | 305 µs |
| p99.9 | 131 µs | 151 µs | 173 µs |
| p99 | 83 µs | 83 µs | 99 µs |

The same suite on tib's **stock 6.18 Armbian kernel** gave `max` 7004–8667 µs. So the RT kernel is
worth ~30× on the tail on this board, and the machine is a legitimate RT host.

---

## 2. The device situation, which decides what "criterion 3" can mean

**The SSL 2+ Mk II is not attached to tib.** What tib has:

| card | device | formats | channels | rates | period floor |
|---|---|---|---|---|---|
| 0 | Realtek USB `0bda:49f6` (full speed) | S16_LE only | 2 | **8000 only** | 2 ms |
| 1 | on-board `rockchip,tinker-codec` (I2S) | S16/S24/S32_LE | 2 | playback **32k–48k**, capture 32k–192k | **166 µs** (playback), 41 µs (capture) |

So the USB audio present on tib **cannot do 48 kHz at all**, and criterion 3 is specified at 48 kHz.
The on-board codec *can*, with an excellent period floor, but:

- it is an I2S/DMA path, not USB — a different driver, and `alsa_raw` has only ever been measured on
  USB devices (PCM2902, Joyo MOMIX, SSL 2+ Mk II). That makes it a genuine test of the backend's
  generality rather than a repeat.
- it is 2 channels where piv was 8 capture / 6 playback, so the per-block I/O cost differs.
- `snd-aloop` and `snd-dummy` **load fine** (verified) and remain the no-hardware fallback.

**Decision needed from the user: is the SSL 2+ to be moved to tib?** With it, tib becomes a
like-for-like device comparison against piv and the *only* variable is the CPU/kernel. Without it,
the device arms run on the on-board codec and the comparison against piv is qualitative. Everything
in §4 works either way, and none of it is blocked on the answer.

---

## 3. What criterion 3 means on this host

The clauses, re-stated for ARMv7 rather than copied:

| clause | on piv it was | here it should be read as |
|---|---|---|
| ≥ 4 concurrent A2-Full at q64 | met | **almost certainly not met, and that is the measurement.** §9.4 predicts "much lower". Report the *measured ceiling* — how many A2-Full and A2-Lite fit under 50 % of the deadline — rather than a pass/fail against a number chosen for a 2.4 GHz A76. |
| p99 block ≤ 50 % of deadline | met | unchanged, and the honest gate on this host |
| max block < deadline | failed (memory) | unchanged, but CPU is the binding constraint here, not memory |
| zero driver xruns over 1 h | met (RT + isolation) | **the strongest available test of the RT kernel alone**, with no isolation to help it |
| no thermal throttle | met | **a real risk on this board** (90 °C trip, and the rtpi suite shut it down once). Cap at 1416 MHz as rtpi does, and record `thermal_zone0` throughout |
| captured output = offline render | met | unchanged; the oracle is device-independent |

**One more thing this host can measure that piv could not:** piv's tail turned out to be the shared
memory system, which no scheduler setting fixes. tib's memory system is smaller and its CPU is
slower, so if the same load arm that destroyed piv (3119 xruns in 300 s) behaves *differently* here,
that is evidence about which resource binds. The `--vm` arm is worth repeating verbatim for that
comparison, **not** because a second instance of the same result is useful but because a *different*
one would be.

---

## 4. Gate 0 — prerequisites (all on piv, none of it touches tib until it is green)

1. **Rebuild armv7 at HEAD.** `build-armv7/` is from 2026-09-13 and predates the queue, floor and
   splice work. Reconfigure and rebuild:
   ```sh
   cmake -S . -B build-armv7 -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/armv7-linux-gnueabihf.cmake \
     -DCMAKE_BUILD_TYPE=Release -DNAMCHAIN_WERROR=ON
   cmake --build build-armv7 -j4
   ```
2. **Run the armv7 unit tests under `qemu-arm-static` on piv** before deploying. A binary that fails
   its own tests on the target is not a measurement, and qemu catches the "built but wrong" case
   without touching tib. Confirm `ctest` passes under the emulator, and record how it was invoked.
3. **Record the arithmetic mode in the evidence.** `CMakeLists.txt` gates the planar kernels to
   `CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv8)"`, so **on armv7 the planar kernels are
   off** and this measures the scalar/`vfpv3-d16` path. That is the *shipped* ARMv7 configuration
   today, so the measurement is of what ships — but the number must be labelled with it, and §9.4's
   claim that a `-mfpu=neon-vfpv4` artifact would be needed is exactly what this quantifies.
4. **Get a binary and the fixtures onto tib** (`/home/rik/namchain/` on tib, which does not exist
   yet). tib has 5.1 GB free and no compiler, so everything is cross-built and copied.
5. **Decide the privilege path** (see §7): run the soak under `sudo` (works today, and CAP_IPC_LOCK
   makes `mlockall` unlimited for root), or install `/etc/security/limits.d/audio.conf` so the
   normal user path works as it would in production. The second is a one-line reversible change and
   is what `docs/tuning.md` tells a user to do; the first needs no change at all.

---

## 5. The arms

Every arm follows the piv convention — `bench-results/tib-<id>.{env,heartbeat,txt}` via
`tools/soak/arm-run.sh` — with a tib-specific host-state check (the tool's `rt-manipulation-check.sh`
is piv-shaped: it looks for `xhci-hcd:usb2` and assumes isolation; on tib it must report "no
isolation" as the expected state rather than a failure, and take the audio IRQ name as an argument).

### Tier 1 — the number that matters, with no hardware dependency

- **B1. ARMv7 throughput ceiling.** `namchain-bench` for A2-Full and A2-Lite at q32/64/128/256,
  48 kHz, `double` samples, `a2_fast` on, planar off. This is §9.4's prediction, measured. It
  produces the instances-at-50 % table and settles whether 4×A2-Full is even worth attempting.
- **B2. Paced engine loop, no device.** `namchain-soak --backend null` + self-oracle + the p99 gate,
  at the instance count B1 says is plausible. Tests the timers, the gates, the oracle and the RT
  scheduling of the loop with no driver in the path.

### Tier 2 — the real device

- **D1. First soak on the on-board codec.** `--device hw:1,0`, 48 kHz, 2 channels in and out,
  S32_LE if the driver takes it, q64. This is the first non-USB device `alsa_raw` has seen; expect
  the negotiated period and the buffer constraints to differ, and treat a refusal as a finding about
  the backend rather than a failure of the run.
- **D2. Instances sweep** at the count D1 tolerates, to find where p99 crosses 50 %.
- **D3. The load arm.** `stress-ng --cpu 4 --io 2 --vm 1 --vm-bytes 128M` + `hackbench`, unpinned:
  on a machine with no isolation this is the question. Compare the *shape* against piv's: piv lost
  to the `--vm` worker specifically and not to CPU load, and a different shape here would say the
  binding resource differs.
- **D4. The hour**, if D1–D3 leave a configuration that is close: zero xruns over 1 h at 48 k/64 is
  the clause worth an hour on this board.

### Tier 3 — the same engine on the SSL, if the interface moves

- Repeat D1/D2/D4 on `hw:0,0` with the SSL 2+ attached, which turns tib into a like-for-like
  comparison with piv's device arms and isolates CPU/kernel as the only difference.

**Thermal discipline at every arm:** cap `scaling_max_freq` to 1 416 000 kHz (rtpi's finding — the
board cannot hold its top bin and the 15-minute suite once drove it to the 90 °C critical trip),
set the governor to `performance`, sample `thermal_zone0` and `scaling_cur_freq` every 2 s, and
discard any arm that throttles. This is not optional on this board; it is the difference between a
measurement and a different machine.

---

## 6. Deliverables

| # | artifact | why |
|---|---|---|
| 1 | `bench-results/tib-*` for every arm | evidence, same convention as piv |
| 2 | `bench-results/tib-summary.txt` | the scoreboard: §9.4's prediction against measurement, the measured instance ceiling, and the xrun/thermal result if an hour runs |
| 3 | `plan.md` §9.4 | the ARMv7 bullet becomes measured rather than predicted |
| 4 | `plan.md` §11 | the new ARMv7 row, and an explicit note that the piv table is A76-only |
| 5 | `plan.md` §12 M1 row | criterion 3's status on the ARMv7 target |
| 6 | `docs/tuning.md` | a tib section: the rlimits prerequisite, the thermal cap, and the fact that `NO_HZ_FULL` is *not* available on a 4-core board with a desktop userspace |
| 7 | a host-state check that works on both boards | `rt-manipulation-check.sh` currently hard-codes piv's IRQ name and treats "no isolation" as a failure |

---

## 7. The two decisions this plan needs from the user

1. **Is the SSL 2+ Mk II to be moved from piv to tib?** It is the difference between a like-for-like
   device comparison and a qualitative one. Nothing in tiers 1–2 is blocked on the answer.
2. **May I configure tib's limits?** Installing `/etc/security/limits.d/audio.conf` with
   `@audio - rtprio 95` / `@audio - memlock unlimited` is one reversible file and is what a user of
   this engine needs anyway; without it, every measurement runs under `sudo`. I will default to
   `sudo` (needs no change to your machine) unless you prefer the limits file, and I will note in
   `docs/tuning.md` that the non-root path requires it.
