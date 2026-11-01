# Does `namchaind` bank a startup xrun? — pro, 2026-11-01

The engine's own panel is meant to be trustworthy: `engine.status`'s `xruns` feeds a health `warn`
(`daemon.cpp`, the `xruns` issue), and the pedal resumes its rig at power-up, so if starting a stream
costs an xrun then every power-on raises a warning nobody can act on. `namchain-soak` has carried a
bring-up cycle, a prefill and a separate startup count since §9.5; `namchaind` had none of the three.

**The answer is yes, conditionally, and the condition is the output-queue prime.** With no queue
target the daemon reported `xruns 1` from the first block; with the four-period target every pedal host
configures, it reports none. This document is in the order it was measured, including a wrong turn: the
first five soak runs below read 0 and were briefly taken as evidence that pro produces no bring-up
xrun, which nine committed runs in `pedal.project` contradict. The correction, the reproduction and the
mechanism follow them. The decisive measurement is "the measurement that should have been taken first",
near the end.

## Setup

| | |
|---|---|
| Host | `pro` — HP ProDesk 600 G5 Mini, i5-9500T, Ubuntu Server 26.04, signed amd64 RT kernel |
| Device | Behringer UMC202HD, `hw:CARD=U192k,DEV=0` |
| Stream | 48000 Hz, 64 frames, 6 periods, queue cap 4 / floor 2, `rt_priority` 80 on core 5 |
| Engine | `/opt/namchain/efcdae7` (namchain `efcdae7`, deployed 2026-10-31) |
| Rig resumed | `last.json` — `nc.input` → `nc.nam` (Ampeg GVT52 ch.2) → `nc.cab.ir` (York ORNG 412) → `nc.output` |
| Lock | taken through `pedal.project/scripts/host-lock --host pro`; no soak running |

## Ten fresh starts of `namchaind`

`systemctl restart namchaind`, then `engine.status` at 0.5 s, 2 s, 5 s and 20 s. Every sample of every
run, identical:

```
run  at_s    blocks  xruns  under  over  recov  gate  health
  0  0.52       318      0      0     0      0     0  ok
  0  2.02      1443      0      0     0      0     0  ok
  0  5.02      3693      0      0     0      0     0  ok
  0 20.02     14943      0      0     0      0     0  ok
  …  (runs 1-9 the same to within 3 blocks)
```

`under`/`over`/`recov`/`gate` are `backend_diagnostics`' `playback_underruns`, `capture_overruns`,
`recoveries` and `gate_capture_xruns`. **0 xruns on all 40 samples, `health ok` throughout.** The
long-running instance that preceded them (12.65 M blocks, ≈4.7 h) also read `xruns 0`.

## Five soak runs on the same device

Passthrough, 10 s each, same rate/period/queue, `--json`:

```
run 1: startup_xruns=0 driver_xruns=0 blocks=7500 result=true
run 2: startup_xruns=0 driver_xruns=0 blocks=7500 result=true
run 3: startup_xruns=0 driver_xruns=0 blocks=7500 result=true
run 4: startup_xruns=0 driver_xruns=0 blocks=7500 result=true
run 5: startup_xruns=0 driver_xruns=0 blocks=7500 result=true
```

The text report prints its startup line only when the count is non-zero
(`tools/soak/namchain-soak.cpp`, `if (result.stream_mode && result.startup_driver_xruns > 0)`), so the
JSON was read instead of inferring zero from a missing line.

## Correction: those five runs were a non-reproduction, not a refutation

The first draft of this note read the five zeros above as "pro does not produce a bring-up xrun". That
was wrong, and `pedal.project` had nine committed lines on `main` saying so, every one of them
`bring-up xruns:   1 (before the measured loop)`:

| File | Arms |
|---|---|
| `bench-results/2026-10-31-soak-pro/sweep.txt` | 6 — quanta 128, 64, 48, 32, 24, 16, stock `7.0.0-42-generic` |
| `bench-results/2026-10-31-soak-pro/soak-hour.txt:25` | the failing hour, same load |
| `bench-results/2026-11-01-soak-pro-rt/run-20261101T013140Z/soak.txt:38` | RT kernel, tuned and isolated |
| `bench-results/2026-11-01-soak-pro-rt/run-20261101T023421Z/soak.txt:35` | RT kernel, the 45-minute pass |

The print is gated on `result.startup_driver_xruns > 0`, so the line's presence proves the value.

**What differed was my own command.** All nine committed arms run through
`pedal.project/bench-results/2026-11-01-soak-pro-rt/soak.sh`, which passes `--channels 2 --periods 6
--quantum 64 --model ORNG-V30-421-Center.nam --instances 2 --cpu 5 --rt-priority 80` and **no**
`--output-queue-periods`. My five passed `--output-queue-periods 4 --output-queue-floor-periods 2`.

Three arms, each run exactly as `soak.sh` runs it (under `pedal-rt` on core 5, `namchaind` stopped,
same device and period), differing only in those flags:

| Arm | Configuration | `bring-up xruns` |
|---|---|---|
| A | `soak.sh` verbatim: 2 × A2-Full, no queue options | **1** |
| B | the same, plus `--output-queue-periods 4 --output-queue-floor-periods 2` | **0** (and `prefill: 8 frames`) |
| C | passthrough, no model, no queue options | **1** |

So the artefact is **real on pro and reproducible**, the model and the instance count are irrelevant
(A and C agree), and it is absent exactly when the output queue has a target.

### The mechanism, from the source

`AlsaRawDuplex::prime_playback` (`src/io/alsa_raw.cpp:490`) primes the playback ring inside `start()`,
and its size is `alsa::queue_prime_frames(period_size, requested_queue_periods_)`, which is
`period_frames * max(1, queue_periods)` (`src/io/alsa_params.cpp:222`):

- **no target → one period primed (64 frames).** The device eats it while the bring-up cycle does its
  wait, read and write, the ring runs dry, and that is the one xrun.
- **a target of 4 → four periods primed (256 frames).** Three periods of cushion, and nothing
  underruns.

It is the *prime*, not the prefill, that decides this. The prefill is a later, smaller step that only
replaces what the bring-up spent — 8 frames, 0.167 ms, which is why arm B reports that and not a
period's worth.

## Why the daemon showed nothing

Two independent protections, and the deployed daemon has both.

**Its configured queue target.** `/etc/namchain/namchaind.json` sets `output_queue_periods: 4`, so
`namchaind` has always been in arm B's configuration, never arm A's — a four-period prime.

**The order it does things in**, which is the ordering the soak's own comment credits
(`src/soak/soak.cpp`, "with the model loaded afterwards instead, the device was already starving while
the plan was built"):

```
[namchaind] cost: no usable table at '/etc/namchain/cost.json' …
[namchaind] published /var/lib/pedal/library/rigs/last.json (generation 1, 1 chain: crossfade)
[namchaind] render thread: SCHED_FIFO priority 80, cpu 5
[namchaind] warm thread on cpu 1 at priority 70
```

The rig — NAM model, IR, plan — is published **before** the render loop opens the device, so the daemon
never loads a model with a stream already running.

Note what this does *not* say: `Config::output_queue_periods` defaults to **0**
(`include/namchain/daemon/config.h:186`). A `namchaind` whose config omits that key is in arm A's
configuration — one period primed, and the artefact available. That configuration is measured below.

The earlier observation that a long-running instance read `xruns 2` while a fresh start read `xruns 0`
is consistent with all of this: the 2 were accrued while running, not at start-up, and the instance
measured here accrued none in 4.7 hours.

## A consequence worth acting on: the harness and the engine measure different streams

`soak.sh` passes no `--output-queue-periods`, so every committed soak arm on pro characterises a
stream with a **one-period** prime. Every pedal host runs a **four-period** one — checked directly,
not inferred: `devices/vip`, `devices/piv` and `devices/pro`'s
`rootfs/etc/namchain/namchaind.json` each carry `output_queue_periods: 4` and
`output_queue_floor_periods: 2`, and pro's running `/etc/namchain/namchaind.json` has them too.

So the soak's `bring-up xruns: 1` has never described the configuration the engine actually plays in,
and neither has its ring-occupancy or underrun behaviour. Neither tool is wrong; they are measuring
two different streams.

The suggestion, for whoever changes `soak.sh` (it lives in `pedal.project`): **add** an arm with
`--output-queue-periods 4 --output-queue-floor-periods 2` rather than changing the existing one. Two
reasons. The nine committed arms are the baseline the RT-kernel work on pro is measured against, and
re-flagging them makes future runs incomparable with that history. And the queue target is not a
free switch: it brings the cap's refusals and the floor's insertions with it, which is what §9.5 item
8 is about (8 playback underruns at a 4-period target against 92 for the bare 4-period ring). A run
with a target and a run without answer different questions and both are worth keeping.

## After the change — same host, same device, ten more fresh starts

`namchaind` rebuilt from the commit that adds `Daemon::bring_stream_up` and deployed to pro
(`scripts/deploy-engine --host pro`; its own 5/5 test run passed natively on x86-64, 89.6 s). Ten more
restarts, same four sample points:

```
run  at_s   blocks  xruns  startup_xruns  under  over  health
  0  0.52      315      0              0      0     0  ok
  0  2.02     1440      0              0      0     0  ok
  0  5.02     3690      0              0      0     0  ok
  0 20.02    14941      0              0      0     0  ok
  …
40 samples: 0 with a non-zero `xruns`, 0 with health other than `ok`
`startup_xruns` values seen across all 40 samples: {0}
```

Unchanged, which on this device is the expected result: there was no artefact to move. What the run
shows positively is that the new path *runs* and costs almost nothing — the blocks counted at 0.52 s
are 311–318, against 316–319 before, so the untimed cycle and the prefill are inside the restart's own
scatter.

**The prefill is the one measurable change, and it is visible now that `engine.status` reports it:**

```
queue: {"output_queue_periods": 4, "output_queue_floor_periods": 2, "output_drops": 0,
        "output_target_drops": 0, "floor_insertions": 0, "capture_bursts": 0, "prefill_frames": 8}
```

Eight frames of silence written at open to bring the output ring to its target — **0.167 ms of output
latency, paid once**, and the same eight frames the soak reports on this interface. Before the change
the daemon never called `prefill_output_queue` at all, so it ran its whole life a period short of the
cushion its own configuration asked for (plan.md §9.5 item 7: "a queue that starts a period short
stays a period short for the whole run"). That is the part of this change that does something on every
device, xrun or no xrun.

## The measurement that should have been taken first: the daemon with no queue target

`Config::output_queue_periods` defaults to 0, so this is `namchaind`'s own default and arm A's
configuration. Both binaries are installed on pro, so it is a direct A/B: same config file, same
device, `--output-queue-periods 0 --output-queue-floor-periods 0` on the command line, three rounds
each, sampled ~4 s after the socket appears.

| Round | Binary | `xruns` | `startup_xruns` | `playback_underruns` |
|---|---|---|---|---|
| 1 | `efcdae7` (before) | **1** | *(field absent)* | 1 |
| 1 | `fb7d4a5` (after) | **0** | **1** | 1 |
| 2 | `efcdae7` (before) | **1** | *(field absent)* | 1 |
| 2 | `fb7d4a5` (after) | **0** | **1** | 1 |
| 3 | `efcdae7` (before) | **1** | *(field absent)* | 1 |
| 3 | `fb7d4a5` (after) | **0** | **1** | 1 |

**So the daemon does bank a startup xrun, and the change moves it out of `xruns` into
`startup_xruns`.** The underlying `playback_underruns: 1` is identical on both sides, as it must be —
the artefact is not prevented, it is attributed. This is the fault the work went looking for; the first
measurement in this note missed it only because the deployed config sets `output_queue_periods: 4`.

### How long the health light was actually amber

Worth measuring rather than asserting, because "a warning at every power-up" was the motivation.
Polling the **before** binary every ~30 ms from the instant its socket appears:

```
t=0.039 blocks=4    xruns=1  health=warn   issue: xruns
t=0.069 blocks=26   xruns=1  health=warn
t=0.098 blocks=48   xruns=1  health=warn
t=0.127 blocks=70   xruns=1  health=warn
t=0.157 blocks=92   xruns=1  health=warn
t=0.186 blocks=114  xruns=1  health=ok     <- issue gone
…  xruns stays 1 for the life of the process
```

The `warn` lasts **one decay step — about 0.16 s**, which is exactly what the code predicts:
`recent_xruns_` is drained every `kRecentDecayBlocks` = 100 blocks (133 ms at 64 frames / 48 kHz) and
`decay_recent_trouble(1)` is 0. So the honest claim is narrower than the one this work started from: a
startup xrun does **not** leave a pedal's health light amber for a player to see. What it does leave is
`status.xruns == 1` for the life of the process — cumulative, never cleared — which is what
`scripts/status`, a diagnostic, or any panel showing the engine's xrun count reports. That is the thing
the change fixes.

It also means the `Health warn` seen in the 2026-10-13 two-rig playtest cannot have been a startup
xrun: 0.16 s is far too brief. The cost table's unknown-cost row remains the explanation.

## Consequence for the change

What starting a stream costs is now published as `startup_xruns` and kept out of `xruns`, the
recent-trouble bucket and the health `warn`. In the daemon's **default** configuration that number is
1 on this device and the change is what stops it being reported as a missed deadline. In the pedal's
**deployed** configuration (`output_queue_periods: 4`) it is 0, because a four-period prime leaves the
bring-up nothing to underrun on — so on vip as configured today nothing moves, and the value there is
that the artefact is named if a device ever does charge for it.

One claim this note made in draft and withdraws: that `namchaind` "ran its whole life a period short of
its configured output-queue target" because it never called `prefill_output_queue`. It did not.
`prime_playback` establishes the target inside `start()`. The prefill becomes necessary *because* the
bring-up cycle added here spends part of that primed queue — the two go together, and on this device
that is the 8 frames `queue.prefill_frames` now reports, not a period. It is the completion of the
bring-up, not an independent defect.

## Limits

- One device, one host. vip and its WM8960, where the pedal actually lives, were not measured; the Pi
  hosts are a different I2S path and a different kernel. They share the four-period configuration, so
  the expectation there is the deployed daemon's result, not the default one's — untested.
- `occupancy_mismatches` reads 31.6 M on the long-running instance, which is the known delay-vs-pointer
  disagreement on this interface (`docs/audio-dma-irq.md`), not an xrun.
- The three A/B rounds sampled ~4 s in. The health-warn window was measured separately, on the before
  binary only; the after binary was not polled that early, because with `startup_xruns` carrying the
  count there is no `xruns` delta for the bucket to be fed.
