# Re-testing M1 criterion 3 on piv, now that piv runs PREEMPT_RT

**Status:** proposed, not started. Nothing in this file has been measured yet; every number marked
*[before]* is from existing evidence in `bench-results/`, and every number marked *[verified]* was read
off the host while writing this.
**Written:** 2026-09-16. **Host:** piv (Raspberry Pi 500, BCM2712, 4× Cortex-A76).
**Companion repos:** `namchain` (the engine under test), `rtpi@ff08927` (the kernel, boot args and
runtime tuning this plan reuses and does not reinvent).

---

## 0. Why this run exists — it is the experiment the earlier work named

`bench-results/queue-target-sweep.txt` ends by listing three ways forward. The third is:

> **Fix the host** — IRQ affinity and the RT kernel. The tail is unchanged by anything in this layer:
> `cpu == wall` and 0 descheduled blocks rule out preemption, which leaves interrupt work charged to our
> own CPU time as the candidate, and **that is the experiment nobody has run.**

It is now runnable. piv boots `7.0.0-1017-raspi-realtime` with `isolcpus`/`nohz_full`/`rcu_nocbs` on
cores 2–3, `irqaffinity=0-1`, `skew_tick=1`, and the audio controller's interrupt pinned to core 2 at
`SCHED_FIFO 85` — the host side of §9.3's tuning checklist, which the plan has carried since M0 as
*"untested: the engine is confined to a core, but IRQs and other processes are not moved off it, and
that is where the rest of the tail is."*

So this plan is narrow and falsifiable: **does the host fix that was named actually move the tail, and
how far does it move M1's criterion 3?**

---

## 1. What is actually in force — verified, not assumed

Read from the host today, because the previous IRQ-affinity experiment failed by never proving its own
intervention (16 arms, an uninterpretable null).

| property | value | how it was read |
|---|---|---|
| kernel | `7.0.0-1017-raspi-realtime #17-Ubuntu SMP PREEMPT_RT` | `uname -a` |
| `CONFIG_PREEMPT_RT`, `HZ=1000`, `NO_HZ_FULL`, `RCU_BOOST`, `RCU_NOCB_CPU`, `RCU_NOCB_CPU_CB_BOOST`, `HIGH_RES_TIMERS` | all `=y` | `/boot/config-$(uname -r)` |
| isolated cores | `2-3` | `/sys/devices/system/cpu/isolated` |
| tickless cores | `2-3` | `/sys/devices/system/cpu/nohz_full` |
| boot args | `isolcpus=managed_irq,domain,2-3 nohz_full=2-3 rcu_nocbs=2-3 irqaffinity=0-1 skew_tick=1` | `/proc/cmdline` |
| audio IRQ | `xhci-hcd:usb2` = **IRQ 139**, `smp_affinity_list = 2`, thread `irq/139-xhci-hcd:usb2` at `FF 85` | `/proc/interrupts`, `/proc/irq/139/`, `ps -eLo` |
| audio tune service | `rtpi-audio-tune.service` enabled **and active**; udev re-applies it on replug | `systemctl`, `/etc/udev/rules.d/90-rtpi-audio-tune.rules` |
| governor / frequency | `performance`, 2 400 000 kHz, `throttled=0x0`, 38.1 °C | cpufreq, `sudo vcgencmd` |
| device | SSL 2+ Mk II = **card 2**, `hw:2,0`, 8 capture / 6 playback, `as usb-xhci-hcd.1-1` | `aplay -l`/`arecord -l` |

### The manipulation check that the previous experiment could not make

10 s idle sample of `/proc/interrupts` on 2026-09-16:

| source | cpu0 | cpu1 | cpu2 | cpu3 |
|---|---|---|---|---|
| `arch_timer` (IRQ 13) Δ | **+10 486** | +1 737 | **0** | **0** |
| `IPI0` resched Δ | +771 | +1 066 | **0** | +10 |
| `IPI5` IRQ-work Δ | 0 | +4 | **0** | +5 |

**The tick is genuinely off on both reserved cores**, and the audio core is taking essentially nothing.
This is the measurement the earlier IRQ work lacked: 98.8 % of cpu 3's interrupts were then the
`arch_timer` and IPIs, which no runtime mask can move — and `nohz_full` + `rcu_nocbs` are exactly the
levers that remove them. The number to carry into every arm is **`arch_timer` on the soak core**, which
should grow at ~0/s instead of ~1000/s (≈1.33 M over an hour).

Two corrections to `rtpi`'s own notes, worth a PR over there:

- `/sys/kernel/realtime` **does** exist on Ubuntu's `raspi-realtime` flavour and reads `1`; `rtpi`'s
  README says it does not (that is true of mainline, not of this flavour). Detection should still use
  `uname -v | grep PREEMPT_RT` as the README says, but the file's absence is not a reliable negative.
- `vcgencmd` needs root here (`/dev/vcio` is `0600 root`); the health loop must `sudo` it or read
  `hwmon0 cpu_thermal`. An unprivileged `vcgencmd get_throttled` fails with a missing-device message
  that reads like the firmware is broken.

---

## 2. The scoreboard: what "closer to the goals" means, numerically

Criterion 3 (§1) is the only acceptance clause not yet met, and the 1 h measurement exists:
`bench-results/soak-1h-ring6-cap4.*` — 4 × A2-Full, q64, ring 6, cap 4, cpu 3, `SCHED_FIFO 80`,
`mlockall`, performance governor, **non-RT kernel `7.0.0-1017-raspi`, no isolation** *[before]*.

| criterion-3 clause | target | *[before]* | verdict |
|---|---|---|---|
| driver-reported xruns over 1 h | **0** | **8** playback underruns, 0 capture | ✘ |
| blocks exceeding the 1333.3 µs deadline | 0 | **204** of 2 700 000 | ✘ |
| max block time | **< 1333.3 µs** | **3427.7 µs** (2.57×) | ✘ |
| p99 block time | ≤ 50 % of deadline (666.7 µs) | 501.1 µs (37.6 %) | ✔ |
| render-path allocations | 0 | 0 allocations, 0 frees | ✔ |
| thermal throttle flags | none raised | `throttled=0x0`, ~45 °C | ✔ |
| captured output vs offline render | sample-identical | `max\|diff\| = 0` over 240 000 frames | ✔ |
| **result** | PASS | **FAIL** | |

**Two clauses fail and they are the same thing: the tail.** 8 underruns and 204 late blocks are the
2–3.4 ms events, described in that run as `cpu spike bursts: 4096 blocks over half a period of CPU, in
1987 runs`, with `cpu max 2 119 463 ns` against `wall max 2 115 323 ns` and
`0 blocks lost ≥ 0.5 ms to not being on CPU`.

That last pair is the whole question. **`cpu ≈ wall` means the process was running, not waiting.** So
there are two mutually exclusive explanations, and they have different fixes:

- **H-lateness** — the 2 ms is time the thread spent *not* running, mis-attributed because the block
  boundary sampling missed the preemption. Fix: RT kernel, isolation, IRQ off the core. **This is what
  we just bought.**
- **H-work** — the 2 ms is instructions genuinely executed. One A2-Full at q64 costs ≈67 µs on this host
  (§11 planar table: RTF 0.0501 × 1333 µs; four in series measured `chain p50` 296 µs, which agrees), so
  a 2 ms block is **~7× the expected work** and must come from memory bandwidth, cache eviction by other
  cores, or a clock drop — none of which PREEMPT_RT addresses. Fix: fewer models, different quantum,
  memory/page work, or accept the ceiling.

The plan is built to decide between these before it is built to optimise anything.

### The three-dimension answer

Criterion 3 is throughput-shaped, but the project has two more dimensions this host change touches, and
the plan reports all three:

1. **Integrity** — 0 xruns, 0 late blocks, oracle exact (the scoreboard above).
2. **Latency** — output latency is the queue's occupancy, not the ring size (§9.2). The shipping
   `cap 4` bound is 5.33 ms; §9.5 item 8's placement rule said the cap must sit above the queue's
   natural band, and that band was measured on a *non-RT* host. A quieter host should have a lower
   band, so **the cap may be re-derivable downward** — that is a real latency win, and it is a
   measurement, not a hope.
3. **Headroom** — how many A2-Full instances now fit under 50 % at q64, and whether q32 (666 µs
   period) is newly reachable. On the old host a 1 kHz tick (1000 µs) landed *inside every* 666 µs
   period; a tickless core removes that by construction.

---

## 3. Predictions, stated so they can be wrong

- **P1** The shipping-configuration hour reports **0 driver xruns and 0 deadline overruns**.
- **P2** `max block` falls from 3427.7 µs to **< 1333 µs**; `chain p99` barely moves (501 → ~450 µs),
  because p99 was never the problem. The mean may get slightly *worse* — PREEMPT_RT converts spinlocks
  to PI mutexes and threads the IRQs, so median cost can rise while the tail collapses. A p50
  regression is not a failure and must not be reported as one.
- **P3** `cpu spike bursts` either shrink below the deadline, **or** they do not and `cpu ≈ wall`
  still holds — in which case P1 is false for a reason no host tuning fixes, and §7's H-work branch is
  the answer.
- **P4** `arch_timer` on the soak core grows **~0 over the hour** (measured during the run, not before
  it). If it grows at ~1000/s, `nohz_full` did not hold and the arm did not test the hypothesis.
- **P5** q32 improves disproportionately versus its old ranking (it was the *worst* quantum, 12.54 %
  vs 9.94 % at q256), because the 1 ms tick no longer lands inside every period.

---

## 4. Gate 0 — preconditions, before any number is taken (≈20 min)

1. **Tree and tests.** `build/` is from 2026-09-15 22:08 against HEAD `82356e3`; confirm, rebuild, and
   re-run the gates at the commit being measured:
   ```sh
   cmake --build build -j4 && ctest --test-dir build --output-on-failure
   ./build/namchain-soak --seconds 300 --backend null --self-oracle build/generated/A2-Full.nam \
     vendor/NeuralAmpModelerCore/example_audio/input.wav   # deviceless: timers, gates, oracle
   ```
   `ctest` 4/4 and 133 unit cases must be green *before* the device is touched; a host measurement taken
   on a tree that fails its own tests is not evidence.
2. **Lightweight bit-identity re-check** on the new kernel — the kernel does not change NEON results,
   but §0's rule is that a changed environment invalidates the toolchain provenance of every existing
   number, so re-run `golden_a2_lite` and record it rather than assuming.
3. **Host state into the evidence file**, not into my head: kernel/version, `/proc/cmdline`,
   `isolated`, `nohz_full`, governor, `throttled`, temp, loadavg, card map, IRQ 139's number/affinity
   /thread priority, and the 10 s interrupt delta table from §1.
4. **Write the missing instrument** (see §8): `tools/soak/rt-manipulation-check.sh before|after FILE`
   — it snapshots exactly the state above and emits *deltas* (per-CPU `arch_timer`, `IPI0`, `IPI5`,
   the audio IRQ's counts, its affinity and its thread priority). Every arm gets one; it is the check
   whose absence made the last host experiment unreadable.
5. **Confirm the input path is unchanged.** The criterion-3 soak runs with **nothing in the analogue
   input** — measured, not assumed: the planar A2 path has no silence short-circuit, so silence costs
   the same as signal (326.3 µs vs 317.2 µs p50, §10.5.1). Input channel 0 is the analogue jack;
   capture 2–7 are bit-exact digital loopbacks. A run whose oracle window is silent is still a valid
   load measurement, and the oracle still proves determinism.

---

## 5. The arms

Every arm writes `bench-results/rt-piv-<id>.{env,heartbeat,txt}` through `tools/soak/heartbeat-run.sh`,
wrapped so that `.env` carries the Gate-0 snapshot plus `start`/`exit`. **Never `/tmp`.** Arms are run
one at a time; the device is exclusive.

### Tier 1 — answers the question

**A1/A2, the 5-minute screen (2 alternating pairs, 20 min).** The one genuinely new design question the
isolation creates: **which reserved core should the audio thread use?**

- `--cpu 3` — tickless, no device IRQ on the core. But the USB completion IRQ is delivered on **cpu 2**,
  so every period costs a cross-core wakeup.
- `--cpu 2` — same core as the `xhci` IRQ thread at prio 85 (above the soak's 80, so sample delivery
  always wins). Lower wakeup latency, but two runnable tasks on the core can re-arm the tick, which
  `nohz_full` only suppresses when exactly one task is runnable.

That tension is measured, not argued: the arm's own `arch_timer` delta on the soak core says whether
ticklessness survived, and the block-time tail says which effect dominates.

**H1, the headline hour (65 min).** The shipping configuration, exactly as it would ship:

```sh
tools/soak/heartbeat-run.sh bench-results/rt-piv-h1-shipping \
  taskset -c 3 ./build/namchain-soak \
    --device hw:2,0 --capture-channels 8 --playback-channels 6 \
    --quantum 64 --periods 6 \
    --output-queue-periods 4 --output-queue-floor-periods 2 \
    --splice-fade-ms 1.333 --splice-verify \
    --seconds 3600 --instances 4 --model build/generated/A2-Full.nam \
    --input-channel 0 --output-channel 0 \
    --self-oracle --oracle-window 5 \
    --rt-priority 80 --cpu 3
```

Read from it, in this order:
`driver xruns` + `by direction` → `deadline overruns` → `block time` max → `chain p99` →
`render path cpu` max vs wall max → `cpu spike bursts` → `device wait` max →
`queue floor` insertions and `floor_inserted_frames` → `occupancy check` → oracle.
**The floor reading is load-bearing:** if the hour reports 0 xruns *and* 0 insertions, the host did it.
If it reports insertions, part of the "zero xruns" was bought by inserting silence, and the plan must
say so in those words.

**H2, the floor-off twin (25 min, only if H1's insertions > 0 or H1 fails).** Identical but
`--output-queue-floor-periods 0`, to separate "the host removed the stalls" from "the floor hid them".

### Tier 2 — explains it and measures the headroom

| id | arm | duration | what it decides |
|---|---|---|---|
| **L1** | shipping config + `stress-ng --cpu 2 --io 2 --vm 1` and a `hackbench` loop **pinned to cpus 0-1** | 15 min | H-work vs H-lateness at its sharpest: if the tail grows, it is shared memory bandwidth and isolation cannot shield it; if it is unchanged, the reservation is real. Also the "always-on machine" case the checklist exists for. |
| **T1** | `--instances 4, 6, 8` at q64 | 5 min each | the new throughput ceiling: where p99 crosses 50 % and where xruns begin (§11 predicts 9 fit by p99; the soak adds capture/write and a device). |
| **Q1** | `--quantum 32, 64, 128` at 4 instances | 5 min each | P5. q32 = 666 µs periods, only reachable on a UAC2 device, and the quantum the old tick sat inside. |
| **C1** | `--periods 6 --output-queue-periods 2 --output-queue-floor-periods 0`, and `--periods 4 --output-queue-periods 2 --output-queue-floor-periods 0` | 5 min each, paired against H1's config | re-derives the placement rule's lower bound on a quiet host — i.e. **how much latency the RT kernel buys** (cap 4 = 5.33 ms bound, cap 2 = 2.67 ms). Ring 4 was 92 xruns in an hour on the non-RT host; that number is the thing to beat. |
| **M1** | `namchain-latency --device hw:2,0 --capture-channels 8 --playback-channels 6 --quantum 64 --periods 6` with the TRS patch output→input | 10 min | the true end-to-end number, occupancy-corrected, for the shipping and the cap-2 configurations. This is the number a player actually feels. |

### Tier 3 — makes the causal claim, and needs a reboot (your call)

The comparison above is **not paired**: the *before* hour was a different kernel, five days earlier,
possibly a different session. The drop rate on *identical* configuration has already been measured at
13–18 per 900 s one afternoon and 0 in 50 minutes an evening — so "the RT kernel fixed it" cannot be
established by comparing today against a file.

To make it causal, using `rtpi`'s existing `tryboot` rig (`make piv-audio-boot-args`, one-shot, with
automatic rollback and the rehearsed failure path):

- **K1** stock `7.0.0-1017-raspi`, no isolation, no IRQ pinning → 15 min
- **K2** `7.0.0-1017-raspi-realtime` + isolation + IRQ pinning → 15 min
- **K3** repeat K1, **K4** repeat K2 — alternating, balanced, so the session is not confounded with the
  kernel.

4 reboots, ~2 h. Only K1/K2 differ in the intended variable; the same binary, same config, same
`--cpu 3` (on the stock kernel cpu 3 is simply an ordinary core, so the pin is meaningless but
harmless and keeps the invocation identical).

---

## 6. Discipline carried from the earlier runs (these are the bugs, not the theory)

1. **A rate near zero needs an hour.** 5-minute runs screen; only the hour may be quoted for xruns.
   The 0-xrun 5-minute config became 8 in an hour.
2. **Every arm gets its manipulation check**, and the check is a *delta measured during the run*:
   `arch_timer` per CPU, the audio IRQ's affinity **read back**, its thread's priority, the soak's own
   `running_cpu` / `cpu_pinned` / `rt_priority_held`. An intervention that is not shown to have happened
   makes its null uninterpretable — the exact failure of the 16-arm IRQ experiment.
3. **One variable per arm, and state the control out loud.** Paired and alternating where the question
   is comparative; balanced order so a drift in the session is not attributed to the change.
4. **A throttled or overheated run is void, not a data point.** `throttled != 0x0` or ≥ 70 °C ⇒ discard
   and re-run. (piv has headroom — 38 °C idle, critical at 110 °C — but the flag is the check.)
5. **A run that cannot open the device is not a measurement.** `rc=2` / "Device or resource busy" means
   a stale soak holds `pcmC2D0c`; `pkill -x namchain-soak` (never `pkill -f`, which matches the
   killing shell itself), verify with a *non-blocking* open, then re-run.
6. **Never `kill -9` a host-manipulation script** — it bypasses the restore trap and leaves IRQ masks
   steered (it happened: 25 of 59 left steered). Verify `/proc/irq/*/smp_affinity` against a saved
   copy before and after anything that touches the host.
7. **The oracle outranks the host.** If `max|diff| != 0`, stop: that is an engine bug and no kernel
   result is meaningful until it is fixed.
8. **A thrashing queue is not a repair measurement.** If the splice bridge fires on more than ~5 % of
   blocks, the tool already warns; treat the run as a configuration result, not a splice result.
9. Report `cpu` vs `wall` for every arm. That one line decides which branch of §2 we are in.

---

## 7. If the tail survives: attribution, in the order the evidence would put it

1. **`rtla osnoise` / `rtla timerlat` on cores 2–3, concurrently with a soak arm.** The `rtpi` config
   deliberately keeps `OSNOISE`/`TIMERLAT`/`HWLAT` enabled for exactly this: they name a *thread* or an
   *IRQ* as the source of a latency event instead of leaving a 2 ms hole. This is the tool the whole RT
   kernel decision was made to preserve.
2. **H-work test A — frequency.** Sample `scaling_cur_freq` and `scaling_cur_freq` per CPU at ~10 Hz
   during an arm. A dip to 1.5 GHz is a 1.6× inflation on its own and would explain a large part of
   2.1 ms. (`performance` governor and `throttled=0x0` do not exclude the firmware clamping the top bin
   under whole-cluster load.)
3. **H-work test B — memory pressure.** Re-run L1 with fewer models and with the ring size changed; if
   the spike amplitude tracks the working-set size rather than the model count, it is bandwidth/cache,
   and the levers are model count, channel count and page size — not the scheduler.
4. **`perf stat -e cache-misses,stalled-cycles-backend`** on the soak if the kernel permits it, purely
   to see whether the long blocks are cache-miss-shaped.
5. **Only then** consider the structural levers: cap the model count for q64, adopt q128 (more slack per
   period, more latency), or record the ceiling honestly in §14 and let the floor/splice path carry the
   audible consequence — which is what it was built for and what it has been measured doing (drop
   splice 4.3× → 1.0×, insertion 0.8×).

---

## 8. Deliverables

| # | artifact | why |
|---|---|---|
| 1 | `bench-results/rt-piv-*.{env,heartbeat,txt}` for every arm | the evidence, in the place the project keeps evidence |
| 2 | `bench-results/rt-piv-summary.txt` | the §2 scoreboard with before/after columns, the P1–P5 verdicts, and the limits stated beside the numbers |
| 3 | `tools/soak/rt-manipulation-check.sh` | the missing instrument: proves isolation/IRQ pinning/ticklessness per arm, as deltas |
| 4 | **`docs/tuning.md`** | §9.3 has promised this since M0 and no `docs/` directory exists. This work produces its content from measurement: `isolcpus`/`nohz_full`/`rcu_nocbs`/`irqaffinity`/`skew_tick`, the audio IRQ matched **by name**, *no* `threadirqs` on PREEMPT_RT, `RT_GROUP_SCHED`, governor, `mlockall`, `rtprio`, and the runtime detection string |
| 5 | `plan.md` edits | §9.3: the "isolcpus half is untested" sentence becomes a measured result. §11: note that the criterion-3 tail is now measured on an RT host. §12 M1 row: criterion 3's status. §14: the `--cpu 2` vs `--cpu 3` default decision, which is currently open and is answered by A1/A2 |
| 6 | one honest paragraph | "how much closer to the goals" in the user's terms: which clauses of criterion 3 now pass, which do not, and what the remaining gap is made of |

---

## 9. Cost, order, stop rules

**Order** (each step's output can change the next):

```
Gate 0 (20 m)  →  A1/A2 screen (20 m)  →  H1 hour (65 m)  →  [H2 if needed (25 m)]
               →  L1 (20 m)  →  T1 (20 m)  →  Q1 (20 m)  →  C1 + M1 (35 m)
               →  [Tier 3 K1..K4, with your go-ahead: 4 reboots + ~2 h]
```

**Tier 1 + 2 ≈ 3.5 h of machine time**, all of it unattended-safe: every arm has a heartbeat file and a
hard duration, and the harness gates its own exit code.

**Stop rules** — stop the whole plan and report, rather than continuing to collect arms, if any of:

- the oracle diff is non-zero (engine bug — the host question is moot);
- the manipulation check shows `isolated_cpus` empty, or `arch_timer` on the soak core growing at
  ≥100/s during an arm (the arm did not test the hypothesis);
- `throttled != 0x0` or a trip above 70 °C (the run measures a different machine);
- the device cannot be opened, or the net negotiated period differs from the requested quantum
  (`period_exact` false — the silent-downgrade failure §9.5 records).

---

## 10. The answer this produces, in one of three shapes

- **Criterion 3 passes.** 0 xruns, 0 late blocks, oracle exact over the hour, no throttle. Then M1's
  last open clause closes on piv, the RT host is the reason, and the plan records the tuning recipe and
  moves to M2. The remaining work is the latency and headroom rows of §2, not the gate.
- **It gets closer but does not pass.** Report the new numbers plainly (e.g. 8 → 1–2 underruns, max
  block 3428 → 900 µs), attribute the residue with §7, and state whether the residue is *host* or
  *work*. This is a real result: it converts a 2.57× overshoot into a bounded, explained margin.
- **It does not move.** If `cpu ≈ wall` and the spike amplitude is unchanged under a tickless isolated
  core, then the tail was never lateness and PREEMPT_RT was the wrong lever for it — a conclusion worth
  exactly as much as the two that pass, and the one that changes §14 rather than §12.
