# Host tuning for glitch-free audio

This is the host-side half of the engine's requirements: what the operating system has to be
told, why, and what it is worth. Every claim below was measured on the reference host
(Raspberry Pi 500, Ubuntu 26.04, kernel 7.0.0-1017-raspi-realtime) during the 2026-09-16 M1
re-test; the evidence is in `bench-results/rt-piv-summary.txt` and `bench-results/rt-piv-*`.

The engine does not *require* any of this. §9.3 of `plan.md` is explicit that correctness is
not allowed to depend on the kernel, and the same binary runs on a stock kernel. What the
settings below buy is margin against the tail, and one of them (removing driver xruns
entirely) turned out to be worth a great deal.

---

## What actually matters, in order

1. **A PREEMPT_RT kernel with the audio cores isolated and tickless.** This is the one that
   removed the driver-reported xruns: 8 in an hour on a stock kernel, **0 in 3 h 10 min**
   with this in place.
2. **The audio interrupt steered deliberately and its thread prioritised.** Cheap, reversible
   at runtime, and it puts the interface's 2000 interrupts/s where you chose.
3. **`SCHED_FIFO` + `mlockall` + performance governor.** Necessary, not sufficient — the
   engine does these itself (`--rt-priority`, `mlockall` is unconditional) and reports what
   it was granted rather than assuming.
4. **A quiet memory system.** Not a tuning knob, and the thing that actually limits this
   engine — see "The limit nobody expects" below.

## Kernel

| symbol | value | why |
|---|---|---|
| `CONFIG_PREEMPT_RT` | `y` | the point of the exercise; most spinlocks become priority-inheriting mutexes and IRQ handlers become threads |
| `CONFIG_HZ` | `1000` | 1 ms granularity against a 1.333 ms period |
| `CONFIG_HIGH_RES_TIMERS` | `y` | |
| `CONFIG_NO_HZ_FULL` | `y` | activated by *command line*, not by the config alone |
| `CONFIG_RCU_NOCB_CPU` | `y` | ditto |
| `CONFIG_RCU_BOOST` | `y` | |
| `CONFIG_RT_GROUP_SCHED` | **not set** | with cgroup v2 this refuses `SCHED_FIFO` to every task outside the root cgroup — *including root*. On the ARM board this was the single blocking defect in the vendor kernel |

Detect RT at runtime with the version banner, which is portable:

```sh
uname -v | grep -q PREEMPT_RT     # correct
cat /sys/kernel/realtime          # exists on Ubuntu's raspi-realtime, absent on mainline
```

`/sys/kernel/realtime` is an out-of-tree artifact that never reached mainline. Ubuntu's
realtime flavour does ship it and it reads `1`, but its **absence is not a negative** — do
not test for it.

**Do not pass `threadirqs` on a PREEMPT_RT kernel.** `kernel/irq/manage.c` guards that
`early_param` with `#if defined(CONFIG_IRQ_FORCED_THREADING) && !defined(CONFIG_PREEMPT_RT)`.
Under RT it is not registered and forced threading is unconditional. The argument is a no-op
that will make you believe you configured something.

## Command line

```
isolcpus=managed_irq,domain,2-3 nohz_full=2-3 rcu_nocbs=2-3 irqaffinity=0-1 skew_tick=1
```

| argument | what it does | measured |
|---|---|---|
| `isolcpus=...,domain,` | takes the cores out of the scheduler's load balancing, so nothing drifts onto them | nothing else runs there |
| `managed_irq` | keeps driver-steered (multiqueue) interrupts off the isolated cores | |
| `nohz_full=2-3` | stops the 1000 Hz tick while a single task runs | **cpu 3: 0 timer interrupts/s**, against 1000/s expected |
| `rcu_nocbs=2-3` | moves RCU callbacks off the cores; without it `nohz_full` does not hold | |
| `irqaffinity=0-1` | defaults every interrupt to the housekeeping cores | |
| `skew_tick=1` | offsets the per-CPU ticks so the cores stop contending for the same locks at the same instant | |

This matters because the tick and the per-CPU IPIs were **98.8 %** of the audio core's
interrupts on the stock setup (16.1 M timer + 16.4 M rescheduling IPIs against ~1 % of
steerable device interrupts), and *no runtime affinity write can move them*. They are a
boot-time lever or they are nothing.

`isolcpus` reserves cores; it does not put anything on them. Nothing migrates there by
itself. Pin the audio thread explicitly:

```sh
namchain-soak --device hw:2,0 --cpu 3 --rt-priority 80 ...
```

## Runtime

- `/etc/security/limits.d/audio.conf` → `@audio - rtprio 95`, `@audio - memlock unlimited`.
- The engine sets `SCHED_FIFO` and affinity itself and reports `rt_priority_held`,
  `cpu_pinned` and `running_cpu`. Trust those fields over the intent in your command line.
- `vcgencmd get_throttled` needs **root** (`/dev/vcio` is `0600 root`); unprivileged it fails
  in a way that looks like broken firmware. `hwmon0/temp1_input` needs no privilege.
- Match the audio interrupt **by name, never by number**. Numbers are allocated in probe
  order and have been observed to move across a single reboot (IRQ 138 -> 139 here). A
  config pinning "138" would silently pin an unrelated device and look like it worked.
- Do **not** `taskset` an interrupt thread: they carry `PF_NO_SETAFFINITY`. Writing
  `/proc/irq/N/smp_affinity_list` is the whole job; the thread follows.
- An affinity write takes effect at the **next interrupt**. On an idle device
  `/proc/interrupts` keeps showing the old core, which is indistinguishable from failure.
  Generate traffic before concluding anything, and read the affinity back:

```sh
grep xhci /proc/interrupts                       # find by name, take the number
cat /proc/irq/N/smp_affinity_list                # read back what was actually accepted
ps -eLo tid,cls,rtprio,comm | grep 'irq/N-'      # the thread is what carries the priority
```

- Keep the interrupt thread's priority **above** the audio thread's. Here the IRQ thread is
  `SCHED_FIFO 85` and the engine is 80: the interrupt that delivers the samples has to win
  against the thread waiting for them, or the thread wakes to no data.

`tools/soak/rt-manipulation-check.sh` does all of the above as a before/after snapshot and
prints the deltas, which is the only form in which these checks are worth anything — totals
accumulated since boot cannot show whether the setting was in force *during* a run.

## The limit nobody expects

Isolation protects the audio core from other *tasks*. It does not protect it from the
**memory system**, and for this engine that is the binding constraint. Measured with the
load pinned to the housekeeping cores (cpus 0-1), 300 s per arm, 4 × A2-Full at q64:

| load on cpus 0-1 | driver xruns | block p99 | block max | chain CPU max |
|---|---|---|---|---|
| none | 0 | 513 µs | 1288 µs | 1.25 ms |
| `stress-ng --cpu 1` | 0 | 493 µs | 884 µs | 0.85 ms |
| `stress-ng --cpu 2` | 0 | 493 µs | 1270 µs | 1.22 ms |
| `hackbench` loop | 0 | 822 µs | 1078 µs | 1.05 ms |
| **`stress-ng --vm 1 --vm-bytes 128M`** | **5 460** | **2793 µs** | **10 020 µs** | **4.28 ms** |

Pure CPU load on both housekeeping cores is completely tolerated. A scheduler storm costs
72 % on the median and still never breaks the deadline. Memory stress alone tears the audio
path apart. Perf counters over the same pairs explain it: **instructions per block fall 2.6 %
while cycles per block rise 141 %** (IPC 1.70 → 0.69), with *no change* in cache misses per
block. The core is not doing more work and is not being descheduled — it is stalling longer
on each memory access because another workload is saturating the shared memory system. CPU
frequency was 2400 MHz for every one of 382 samples and `throttled=0x0`, so this is not
frequency scaling and not thermal.

The Cortex-A76 PMU says the same thing more precisely, and it is worth knowing which counters
to read (quiet vs memory-loaded, 120 s each):

| counter | quiet | loaded | change |
|---|---|---|---|
| `instructions` | 132.58e9 | 132.83e9 | +0.2 % |
| `mem_access` | 40.35e9 | 40.30e9 | **−0.1 %** |
| `cycles` | 77.08e9 | 204.54e9 | +165 % |
| `stall_backend` | 39.17e9 | 157.10e9 | +301 % |
| `stall_frontend` | 2.19e9 | 11.63e9 | +430 % |
| `cache-misses` (per block) | 1.49e4 | 1.46e4 | −2 % |

**The number of memory accesses does not change; each one just takes longer.** Backend stalls
go from 51 % of all cycles to 77 %. `perf record -e cycles` then shows that the *mix* of hot
functions is also unchanged — the same A2 planar tile kernels do the same accesses in the same
proportions, ~63 % of cycles in one instantiation during both runs. There is therefore no
single loop to optimise: the stall is a property of the memory system, not of the access
pattern. The lever is the **number** of accesses per block (fewer or narrower models, more
weight reuse between blocks, better cache blocking), not their shape.

The practical rules that follow:

- Do not run memory-heavy work (builds, VMs, containers, large copies, a desktop under load)
  on the same machine while playing. "Isolated cores" does not mean isolated machine.
- The engine has a **throughput ceiling that is memory-shaped, not CPU-shaped**: 4 concurrent
  A2-Full holds, 6 costs 1.9× the p99 of 4 rather than the 1.5× the arithmetic predicts, and
  8 produces xruns. More resident models is more traffic per block.
- `namchain-soak`'s tail is CPU-time (`cpu == wall`, 0 blocks descheduled) whenever it is not
  a genuine wakeup problem. Read `render path cpu:` before blaming the scheduler — it is the
  line that says which of the two you have.
- If you profile the audio thread, expect the profiling itself to disturb what you are
  measuring: `perf`'s event multiplexing arms a ~1 kHz hrtimer on the profiled core. Compare
  the two arms against each other, never against an unprofiled run, and check the tick state
  with `.tick_stopped` in `/proc/timer_list` rather than by counting `arch_timer` interrupts
  (which include hrtimers and will read ~1000/s with the tick provably stopped).

## What tuning does not do

It does not make the residual tail disappear. After all of the above, two consecutive hours
of identical configuration still produced 15 and 109 blocks over the 1.333 ms deadline, with
maxima of 1.78 ms and 3.18 ms. The variance between two hours is larger than the effect of
most settings you can change, which is why:

- a zero measured over 5 minutes is not evidence about a rate near zero (a 0-xrun 5-minute
  configuration became 8 underruns in an hour on the stock kernel);
- every comparison needs repeats, and preferably alternation;
- and a claim about a rate should quote the duration it was measured over.
