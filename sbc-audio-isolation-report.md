# Keeping an SBC quiet for latency-sensitive audio: strip down + isolate

Scope: Armbian (Debian, ARMv7, RK3288, 4 cores) and Ubuntu (ARM64, Raspberry Pi 5, 4 cores) running a real-time audio engine as a `SCHED_FIFO` thread. Kernel building is out of scope. Obsolete or x86-only items are flagged inline.

## 1. Core isolation and ticklessness

The kernel calls everything you push off the audio CPU "housekeeping": unbound workqueues/timers, RCU callbacks, vmstat, lockup watchdog, managed IRQs, and the residual 1 Hz remote scheduler tick. The hard constraint: **"There must always be at least one online housekeeping CPU at any time"** ([housekeeping.rst](https://docs.kernel.org/core-api/housekeeping.html)). On four cores that dominates every other decision.

```text
# /boot/armbianEnv.txt or /boot/firmware/cmdline.txt — isolate cores 2-3, housekeeping on 0-1
isolcpus=domain,managed_irq,2-3 nohz_full=2-3 rcu_nocbs=2-3 irqaffinity=0-1 skew_tick=1
```

- `isolcpus=domain,managed_irq,<cpus>` removes CPUs from scheduler load balancing and moves unbound workqueues/timers; `managed_irq` additionally tries to move IRQs whose affinity is kernel-managed. Per [cpu-isolation.rst](https://docs.kernel.org/admin-guide/cpu-isolation.html), `isolcpus=domain` is now the "less flexible alternative that doesn't allow for runtime reconfiguration" to cgroup-v2 cpuset isolated partitions.
- `nohz_full=<cpus>` enables adaptive ticks: the tick stops only while the CPU has **exactly one runnable task**. It needs `CONFIG_NO_HZ_FULL`, at least two CPUs, and cannot cover all CPUs; RCU callbacks must be offloaded (nohz_full CPUs are offloaded automatically); POSIX CPU timers defeat it; and the kernel warns it does nothing useful for "one runnable SCHED_FIFO task and an arbitrary number of SCHED_OTHER tasks" ([no_hz.rst](https://docs.kernel.org/timers/no_hz.html)). A residual 1 Hz scheduler tick is emitted remotely by a housekeeping CPU ([cpu-isolation.rst](https://docs.kernel.org/admin-guide/cpu-isolation.html)).
- `rcu_nocbs=<cpus>` moves RCU callback processing to `rcuo/N` kthreads (pin them to housekeeping); it is redundant with `nohz_full` but harmless and explicit ([no_hz.rst](https://docs.kernel.org/timers/no_hz.html)).
- `rcu_nocb_poll` makes the `rcuo/N` kthreads poll instead of being woken, removing a wakeup but burning a housekeeping CPU — only sensible when housekeeping has slack.
- `irqaffinity=` sets the default IRQ affinity mask; `skew_tick=1` staggers ticks so they do not coincide across CPUs ([kernel-parameters](https://docs.kernel.org/admin-guide/kernel-parameters.html)).

**Four-core pain.** With `nohz_full=1-3`, CPU0 alone absorbs all unbound work, RCU, vmstat, every IRQ (`irqaffinity=0`), *and* the remote ticks for three CPUs. On an RK3288 that core saturates, and isolated cores still see global work (TLB shootdowns, RCU grace periods). The usual SBC compromise is to isolate **two** cores (`nohz_full=2-3 irqaffinity=0-1`), or to isolate only CPU3 and leave 0-2 as housekeeping. Remember nohz_full only pays off if the audio thread is genuinely the sole runnable task on that CPU; a multi-threaded engine re-arms the tick immediately.

## 2. cgroup v2 / systemd slicing

Two systemd knobs are often confused. `CPUAffinity=` is a **process property** in [systemd.exec](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html) implemented with `sched_setaffinity()`, inherited by children. `AllowedCPUs=` is the **`cpuset` cgroup v2 controller** in [systemd.resource-control](https://www.freedesktop.org/software/systemd/man/latest/systemd.resource-control.html), "supported only with the unified control group hierarchy". `CPUWeight=`/`CPUQuota=` drive `cpu.weight`/`cpu.max` (CFS only). systemd 261 adds `CPUSetPartition=isolated`, which writes `cpuset.cpus.partition=isolated` — the kernel's recommended, runtime-tunable replacement for `isolcpus=domain` — and it requires `AllowedCPUs=`.

```ini
# /etc/systemd/system/audio.slice.d/10-isolate.conf
[Slice]
AllowedCPUs=2-3
CPUSetPartition=isolated
```
```ini
# /etc/systemd/system/system.slice.d/10-housekeeping.conf  (also user.slice, machine.slice)
[Slice]
AllowedCPUs=0-1
```
```bash
systemctl daemon-reload
systemd-run --slice=audio.slice --property=AllowedCPUs=2-3 \
  --property=CPUSetPartition=isolated --property=CPUWeight=10000 /usr/bin/audio-engine
```

Constrain *all* top-level slices (`init.scope`, `system.slice`, `user.slice`, `machine.slice`), not just the audio slice; SUSE's [Shielding Linux Resources](https://documentation.suse.com/sle-rt/15-SP6/single-html/SLE-RT-shielding/index.html) covers this and notes that existing processes cannot be moved into a cpuset — only new ones started in it. It also declares the classical tool dead on v2: "The `cset` utility supports cpuset controller only on v1 hierarchy … On a system with the unified (v2) hierarchy, `cset` is not supported and cpuset controller can be used via systemd." So `cset shield --cpu 2-3 --kthread=on` is **obsolete on cgroup v2**; use `AllowedCPUs=` + `CPUSetPartition=isolated`.

**The `RT_GROUP_SCHED` gotcha (still true).** With `CONFIG_RT_GROUP_SCHED=y`, every cgroup must be granted RT bandwidth via `cpu.rt_runtime_us` before it accepts real-time tasks ([sched-rt-group.rst](https://docs.kernel.org/scheduler/sched-rt-group.html)). cgroup v2's CPU controller does not support realtime threads and exposes no `cpu.rt_runtime_us`, so `sched_setscheduler()` returns `EPERM` in a non-root cgroup **regardless of `CAP_SYS_NICE`** ([siderolabs/pkgs#1484](https://github.com/siderolabs/pkgs/issues/1484), quoting [cgroup-v2](https://docs.kernel.org/admin-guide/cgroup-v2.html)). A cgroup maintainer confirmed (Sep 2025) that "v2 de facto doesn't support RT group scheduling as of today … v2 systems should simply unset `CONFIG_RT_GROUP_SCHED`" ([LKML](https://lkml.iu.edu/hypermail/linux/kernel/2509.2/01220.html)); a Feb 2026 thread shows `cpu.rt_runtime_us` is still absent, which also blocks lowering the global `sched_rt_runtime_us` ([LKML](https://lkml.iu.edu/hypermail/linux/kernel/2602.0/01102.html)). Check `grep CONFIG_RT_GROUP_SCHED /boot/config-$(uname -r)`: Ubuntu/Debian/Fedora/Arch cgroup-v2 kernels ship `=n`, an Armbian vendor kernel may not. A `rt_group_sched=` boot switch was [proposed](https://lkml.iu.edu/hypermail/linux/kernel/2503.1/02420.html) but had not merged as of that thread; the clean fix is a kernel with `=n`.

## 3. Process/service stripping for a headless audio node

Prefer `systemctl mask` (symlink to `/dev/null`) for anything you never want started; `disable` alone leaves socket/dependency activation possible. Common candidates: `systemd-timesyncd` (timer wakeups; sync once then stop, or accept drift), `apt-daily.service apt-daily-upgrade.service apt-daily.timer unattended-upgrades` (background CPU/IO), `man-db.timer`, `plocate-updatedb.timer`/`mlocate`, `fwupd`, `snapd`, `ModemManager`, `avahi-daemon`, `bluetooth`, `cups*`, `NetworkManager-wait-online.service` (pure boot-blocker), `cloud-init`, `polkit`, `rsyslog` (journald already captures; forwarding adds a writer), and `irqbalance`. On `irqbalance` the Arch Wiki is blunt: "Do not use the irqbalance daemon, or do so carefully", citing Red Hat's [RT tuning guide](https://access.redhat.com/documentation/en-us/red_hat_enterprise_for_real_time/7/html/tuning_guide/interrupt_and_process_binding) — manual affinity beats migration ([Arch: Professional audio](https://wiki.archlinux.org/title/Professional_audio)).

```ini
# /etc/systemd/journald.conf.d/10-audio.conf
[Journal]
Storage=volatile
RuntimeMaxUse=64M
Audit=no
```
`Storage=volatile` keeps logs in `/run/log/journal` (tmpfs) so logging never touches the disk; `RuntimeMaxUse` caps that RAM, and `Audit=no` stops journald re-enabling kernel auditing ([journald.conf](https://www.freedesktop.org/software/systemd/man/latest/journald.conf.html)). Tradeoff: logs vanish on reboot. `systemd-oomd` SIGKILLs whole cgroups on memory-pressure/swap breaches ([systemd-oomd.service](https://www.freedesktop.org/software/systemd/man/latest/systemd-oomd.service.html)) — mask it on a headless node where a kill would take the audio engine with it.

Memory: with `mlockall` and `memlock unlimited` the RT working set is resident, so swap matters mainly for other tasks. Prefer no swap or `zram` (compressed RAM swap, [`zram`](https://docs.kernel.org/admin-guide/blockdev/zram.html)); zram costs CPU on an ARMv7 and can itself add jitter. Keep `vm.swappiness=10` and, to cut housekeeping timer work, raise `vm.stat_interval` from 1 s ([sysctl/vm](https://docs.kernel.org/admin-guide/sysctl/vm.html)). `transparent_hugepage=never` reduces khugepaged/THP-fault spikes ([transhuge](https://docs.kernel.org/admin-guide/mm/transhuge.html)), but check THP is even enabled on your SBC kernel. Set the cpufreq governor to `performance` on every CPU (LinuxAudio [system configuration](https://wiki.linuxaudio.org/wiki/system_configuration)). Kernel-cmdline extras: `audit=0`; `nmi_watchdog=0` (**x86-only — no NMI watchdog on ARM, and `nohz_full` already disables the lockup watchdog as housekeeping**); `mitigations=off`, a real security downgrade with little ARM benefit ([LinuxAudio](https://wiki.linuxaudio.org/wiki/system_configuration)).

## 4. Interrupt handling

`rtirq` raises threaded-IRQ priorities so the soundcard bottom-half is not queued behind unrelated interrupts. Its config is a priority ladder ([rtirq](https://github.com/rncbc/rtirq), [LinuxAudio](https://wiki.linuxaudio.org/wiki/system_configuration)):

```bash
# /etc/default/rtirq
RTIRQ_NAME_LIST="rtc snd usb i8042"   # first match = highest priority
RTIRQ_PRIO_HIGH=90                    # rtc=90, snd=85, usb=80, i8042=75
RTIRQ_PRIO_DECR=5                     # all other IRQ threads default to 50
RTIRQ_HIGH_LIST=""                    # services forced into 99-91
```
`RTIRQ_NON_THREADED` and `RTIRQ_RESET_ALL` are **legacy no-ops**: the `CONFIG_PREEMPT_VOLUNTARY` non-threaded-IRQ option they relied on no longer exists. `/etc/init.d/rtirq status` lists the result.

Steering by name and by IRQ number:

```bash
grep -iE 'snd|usb|xhci|ohci|rtc' /proc/interrupts
echo 0-1 > /proc/irq/45/smp_affinity_list      # keep IRQ 45 off the isolated cores
grep . /proc/irq/*/smp_affinity_list
```
`/proc/irq/N/smp_affinity`/`_list` are the supported interface; if the controller does not support affinity, the value silently will not change ([irq-affinity](https://docs.kernel.org/core-api/irq/irq-affinity.html)). **IRQ threads are created with `PF_NO_SETAFFINITY`**, so `taskset`/`sched_setaffinity` on the `irq/N-*` kthread fails with `EINVAL`; only the `/proc/irq/N/...` path works. This bites on the Pi 5 specifically: the RP1 GPIO IRQ thread ignores `smp_affinity`, and PF_NO_SETAFFINITY blocks the userspace workaround ([raspberrypi/linux#7301](https://github.com/raspberrypi/linux/issues/7301)). For USB audio, all ports behind one xHCI controller usually share a single IRQ, so per-device steering is often impossible.

`threadirqs` forces handler threading on non-RT kernels via `CONFIG_IRQ_FORCED_THREADING`. **On PREEMPT_RT it is redundant** — all IRQs are threaded by default except explicit `IRQF_NO_THREAD` users; since Linux 6.12 PREEMPT_RT is mainline and since 6.13 it is a Kconfig option ([sched(7)](https://man7.org/linux/man-pages/man7/sched.7.html), [LinuxAudio](https://wiki.linuxaudio.org/wiki/system_configuration)).

Priority ordering: the rtirq/LinuxAudio convention puts the **device IRQ thread slightly above** the audio thread (soundcard ≈ 85–95, JACK ≈ 70–89; Arch's example is `jackd -R -P89` with device IRQs above it). The reasoning is that the IRQ thread that refills/drains the FIFO must run within the period; if the audio thread outranks it, the IRQ is delayed by up to one full period. The cost is that a long IRQ handler can preempt audio, so handlers must stay short. Some playback-only setups invert this and put audio above IRQ; measure both.

## 5. Privileges / limits

```text
# /etc/security/limits.d/audio.conf
@audio   -  rtprio     95
@audio   -  memlock    unlimited
#@audio  -  nice       -19
```
[limits.conf(5)](https://man7.org/linux/man-pages/man5/limits.conf.5.html) documents `rtprio` (ceiling for unprivileged `SCHED_FIFO`/`SCHED_RR`) and `memlock`. Debian/Ubuntu's `jackd2` ships the example as `/usr/share/jackd/audio.conf` ([file list](https://packages.debian.org/bookworm/armhf/jackd2/filelist)) and the `audio` group. **`realtime-privileges` does not exist in Debian or Ubuntu** (package searches return nothing); it is an Arch/Fedora/openSUSE package. Arch's version raises `rtprio` to **98** and `memlock` to unlimited for the `realtime` group ([Arch: Realtime process management](https://wiki.archlinux.org/title/Realtime_process_management)). PAM ordering matters: `pam_limits.so` must be in the `session` stack (`grep pam_limits.so /etc/pam.d/*`), and later modules override earlier ones.

systemd services do not read `limits.conf`; use the unit:
```ini
[Service]
AmbientCapabilities=CAP_SYS_NICE CAP_IPC_LOCK
LimitRTPRIO=95
LimitMEMLOCK=infinity
LimitRTTIME=200000        # 200 ms runaway guard; SIGXCPU on breach
```
`CAP_SYS_NICE` permits RT policy/priority and `CAP_IPC_LOCK` permits `mlockall` ([systemd.exec](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html)). `mlockall(MCL_CURRENT|MCL_FUTURE)` is the standard way to avoid page-fault pauses ([sched(7)](https://man7.org/linux/man-pages/man7/sched.7.html)); `RLIMIT_RTTIME` (or `rttime` in `limits.conf`) caps CPU time a RT task may consume without blocking — a guardrail, but a too-tight value kills the engine.

RT throttling: `/proc/sys/kernel/sched_rt_runtime_us` defaults to 950000 µs of a 1000000 µs `sched_rt_period_us`, reserving 5% for non-RT work; `-1` means "no limit" (pre-2.6.25 behaviour). The kernel doc explicitly warns that "Fiddling with these settings can result in an unstable system" and that the migration thread and kstopmachine are themselves RT threads ([sched-rt-group](https://docs.kernel.org/scheduler/sched-rt-group.html), [sched(7)](https://man7.org/linux/man-pages/man7/sched.7.html)). Setting `-1` removes the only automatic escape hatch from a runaway `SCHED_FIFO` loop and can hard-lock a 4-core SBC; prefer leaving the default, or a modest value. On cgroup v2 you cannot even lower it below 950000 because `cpu.rt_runtime_us` is gone ([LKML](https://lkml.iu.edu/hypermail/linux/kernel/2602.0/01102.html)).

## 6. Measurement / verification of the intervention

```bash
# classic timer latency, pinned to the isolated cores, mlocked
cyclictest -a 2-3 -t2 -p95 -i200 -m -h400 -D 10m --json=cyc.json
# histogram/summary: read T, Min, Act, Avg, Max; judge Max and the tail, not Avg
```
`-m` mlockall, `-a` CPU set, `-p` priority, `-i` base interval, `-h` histogram ([cyclictest(8)](https://manpages.debian.org/testing/rt-tests/cyclictest.8.en.html), [LinuxAudio](https://wiki.linuxaudio.org/wiki/system_configuration)). For kernel-internal noise use `rtla`: `rtla osnoise top -P f:1 -c 2-3 -r 900000 -d 5m -q` prints per-CPU noise counters broken down into HW/NMI/IRQ/Softirq/Thread ([rtla-osnoise-top(1)](https://manpages.debian.org/testing/rtla/rtla-osnoise-top.1.en.html), [osnoise-tracer](https://docs.kernel.org/trace/osnoise-tracer.html)); `rtla timerlat top -c 2-3 -d 5m` (and `rtla timerlat hist`) measures per-CPU timer wakeup latency ([rtla-timerlat(1)](https://manpages.debian.org/bookworm/rtla/rtla-timerlat.1)). `hwlatdetect --duration=60s --threshold=10 --hardlimit=50 --cpu-list=2-3` uses the ftrace `hwlat` detector to catch hardware/firmware (SMI-like) gaps by stopping all CPUs and polling the TSC — nothing x86-specific, but it monopolises the machine while running ([hwlatdetect(8)](https://manpages.debian.org/testing/rt-tests/hwlatdetect.8.en.html)). Load the housekeeping side while measuring: `hackbench -l 1000 -g 10` (from rt-tests) and `stress-ng --cpu 1 --io 1 --vm 1 --timeout 10m` ([stress-ng](https://github.com/ColinIanKing/stress-ng)). `perf sched record -- sleep 10; perf sched latency` exposes maximum scheduling latency per task ([perf-sched(1)](https://man7.org/linux/man-pages/man1/perf-sched.1.html)). `cpupower frequency-info`/`monitor` shows governor and idle states; **`turbostat` is x86-only** — on ARM read `/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor` and `scaling_cur_freq`. For deep dives, tracefs or `trace-cmd record -e sched -e irq_vectors` then `trace-cmd report` ([trace-cmd(1)](https://man7.org/linux/man-pages/man1/trace-cmd.1.html)).

**Manipulation check** — prove in the run's own artifacts that isolation was actually in force, rather than assuming it:
```bash
cat /proc/cmdline
cat /sys/devices/system/cpu/isolated        # expect 2-3
cat /sys/devices/system/cpu/nohz_full       # expect 2-3
cat /sys/fs/cgroup/audio.slice/cpuset.cpus.effective
chrt -p <audio_tid>; taskset -pc <audio_tid>
# per-CPU IRQ deltas: sample twice, expect ~0 increments on isolated CPUs
grep -E 'CPU|snd|usb|xhci' /proc/interrupts; sleep 10; grep -E 'CPU|snd|usb|xhci' /proc/interrupts
```
Additionally, in tracefs enable `events/sched/sched_switch`, `events/irq_vectors` and the `tick_stop` event to prove the tick actually stopped on isolated CPUs; [cpu-isolation.rst](https://docs.kernel.org/admin-guide/cpu-isolation.html) gives a 10-second `user_loop` example whose clean output is exactly "no noise between the first trace and the second". Attach these outputs (cmdline, `isolated`/`nohz_full`, affinity, IRQ deltas, cyclictest JSON, rtla summary) to every result so the intervention is auditable rather than asserted.
