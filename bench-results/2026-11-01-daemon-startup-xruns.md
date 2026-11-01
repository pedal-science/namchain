# Does `namchaind` bank a startup xrun? — pro, 2026-11-01

The engine's own panel is meant to be trustworthy: `engine.status`'s `xruns` feeds a health `warn`
(`daemon.cpp`, the `xruns` issue), and the pedal resumes its rig at power-up, so if starting a stream
costs an xrun then every power-on raises a warning nobody can act on. `namchain-soak` has carried a
bring-up cycle, a prefill and a separate startup count since §9.5; `namchaind` had none of the three.

This measures the daemon **before** that change, on the only device available to this session.

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

## What this says

**On this device, neither the daemon nor the soak banks a startup xrun.** The premise this went looking
for — "every soak run on every device reports exactly one bring-up xrun" — does not hold on pro today.

The reason the daemon is already clean is in its own journal, and it is the ordering the soak's comment
credits (`src/soak/soak.cpp`, "with the model loaded afterwards instead, the device was already
starving while the plan was built"):

```
[namchaind] cost: no usable table at '/etc/namchain/cost.json' …
[namchaind] published /var/lib/pedal/library/rigs/last.json (generation 1, 1 chain: crossfade)
[namchaind] render thread: SCHED_FIFO priority 80, cpu 5
[namchaind] warm thread on cpu 1 at priority 70
```

The rig — NAM model, IR, plan — is published **before** the render loop opens the device. The daemon
never had the bug the soak's bring-up cycle was built to absorb, because it never loads a model with a
stream already running.

The earlier observation that a long-running instance read `xruns 2` while a fresh start read `xruns 0`
is consistent with this: the 2 were accrued while running, not at start-up, and the instance measured
here accrued none in 4.7 hours.

## Limits

- One device, one host. vip and its WM8960, where the pedal actually lives, were not measured; the Pi
  hosts are a different I2S path and a different kernel.
- The soak runs were passthrough (0 nodes). A soak rendering a Full model was not run.
- `occupancy_mismatches` reads 31.6 M on the long-running instance, which is the known delay-vs-pointer
  disagreement on this interface (`docs/audio-dma-irq.md`), not an xrun.

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

## Consequence for the change

The change was made anyway, on the owner's instruction, and is the right accounting regardless: what
starting a stream costs is now published as `startup_xruns` and kept out of `xruns`, the
recent-trouble bucket and the health `warn`. On pro that number is 0 and nothing moves. Its value is
that on a device which *does* charge for bring-up, the artefact is named rather than reported as a
real-time fault — and that `namchaind` now establishes its output queue's target before the first
period, which it never did (`prefill_output_queue`, plan.md §9.5 item 7).
