# Stripping a general-purpose distro for latency-sensitive audio

**Question this answers:** what have others done to take a normal Armbian/Ubuntu (or
Debian-family) distribution and strip it down so a real-time audio process is not disturbed
by other workloads — and what should *this* project do, given two specific boards.

**Status:** reconnaissance complete 2026-09-16. Nothing here has been turned into a script
yet; §7 is the experiment list and §8 is the deliverable that should come out of it.
Every claim about the two boards marked *[measured]* was read off the live host while
writing this; the three companion surveys are in `docs/research/`.

| companion | what it covers |
|---|---|
| `docs/research/audio-distro-prior-art.md` | what AudioLinux, Ubuntu Studio, Arch, AV Linux, piCorePlayer actually ship |
| `docs/research/arm-sbc-minimal-userspace.md` | Armbian/ubuntu-base/Buildroot/Yocto/Elk/Bela, and what each costs |
| `docs/research/isolation-and-stripping-techniques.md` | kernel-cmdline, cgroup v2, service masking, IRQ, limits, measurement |

---

## 1. The finding that changes the question

The research premise — *"strip the distro so other processes stop causing latency"* — is
right, but on this project the mechanism is narrower than "other processes steal CPU". The
tail was already attributed by measurement (`docs/tuning.md`, `bench-results/rt-piv-summary.txt`):

> With the load **pinned to the housekeeping cores** (cpus 0–1), pure CPU load is tolerated
> completely. `stress-ng --vm 1 --vm-bytes 128M` on those same cores produced **5 460 driver
> xruns**, block p99 2 793 µs, max 10 020 µs. Instructions per block fell 2.6 % while cycles
> per block rose 141 % (IPC 1.70 → 0.69) with **no change in cache misses per block**, at a
> constant 2 400 MHz and `throttled=0x0`.

So the audio core is not being scheduled away, and it is not doing more instructions. **It is
waiting longer for the same memory accesses because another workload is saturating a shared
memory system.** This is exactly the failure mode the kernel's own documentation names first:

> "Main memory and the associated caches are the most common shared resources among tasks in
> a system. One task can dominate the available caches… This issue can be partly mitigated if
> applications do not share the same CPU cache."
> — [Considering hardware](https://docs.kernel.org/core-api/real-time/hardware.html)

Three consequences for what "stripping" is for:

1. **It is about memory traffic, not CPU time.** Removing a service that idles costs nothing
   measurable; removing a service that *touches memory* (a build, a container runtime, an
   indexer, a logging pipeline, a compressed swap) is the lever. "Fewer packages" is a proxy
   for the real quantity, which is bytes moved per second on the housekeeping side.
2. **Isolating the CPU is still necessary and is not sufficient.** On both boards it is a
   *precondition* — you cannot confine a workload you have not confined — but the project's
   own numbers say the residue after isolation is memory-shaped.
3. **The one thing a stripped distro buys that nothing else does is *predictability*.** The
   two consecutive RT hours produced 15 and 109 late blocks of identical configuration. A
   machine that runs nothing else removes the largest uncontrolled variance term.

There is no cache-partitioning escape hatch on these boards: the kernel exposes resctrl
(CAT/PQoS/MPAM) for exactly this, but BCM2712 and RK3288 do not implement MPAM, and the
kernel documentation lists "Intel processors with integrated graphics" and Xilinx ZynqMP as
the affected examples, not Pis or Tinker Boards
([kernel-configuration](https://docs.kernel.org/core-api/real-time/kernel-configuration.html)).
Disabling the GPU/DRM path is therefore worth more than usual here (see §4.3).

---

## 2. Where the two boards actually are *[measured 2026-09-16]*

The two "targets" are not two instances of the same problem. They are 7× apart.

| | **tib** (Tinker Board S, RK3288, ARMv7) | **piv** (Pi 500, BCM2712, ARM64) |
|---|---|---|
| OS | Armbian 26.8.3 resolute | Ubuntu 26.04 LTS |
| kernel | `7.2.6 #1 SMP PREEMPT_RT` (own build) | `7.0.0-1017-raspi-realtime` |
| installed packages | **376** | **2 587** |
| enabled service units | **30** | **69** |
| running services | 17 | 31 |
| RAM | 2 GB | 7.9 GB |
| isolation | **none** — `NO_HZ_FULL` not even compiled in | `isolcpus=managed_irq,domain,2-3 nohz_full=2-3 rcu_nocbs=2-3 irqaffinity=0-1 skew_tick=1`, verified in force |
| swap | **zram0 = 998 MB**, plus `armbian-zram-config`, `armbian-ramlog` (`/var/log` on zram1) | none |
| top RSS | NetworkManager 13.7 MB, systemd-resolve 9.7 MB | **node (DSH web) 852 MB**, dockerd 109 MB, node 83 MB, containerd 56 MB, snapd 40 MB, libvirtd 27 MB |
| realtime limits | `@audio rtprio 95`, `memlock unlimited`, `rik` in `audio` | same, plus `@pipewire rtprio 95 / nice -19 / memlock 4194304` |
| governor | `performance` at 1 416 000 kHz (Armbian-capped) | `performance` at 2 400 000 |
| `usbcore.autosuspend` | — | `-1` (set by `namchain-realtime-tune.service`) |
| `irqbalance` | not installed | `inactive` |
| `CONFIG_RT_GROUP_SCHED` | **not set** (correct — see §5.4) | not set |
| `CONFIG_DRM` | **`y`** | — |

Two userspace facts outrank everything else in this table:

- **piv already carries a full development stack.** The 852 MB `node` process is the DSH web
  GUI this session runs in; Docker, containerd, libvirt/QEMU, snapd, samba, nginx and rsyslog
  are all resident. That is not 137 idle processes — it is a memory-bandwidth competitor that
  the project's own `--vm` experiment says is the one thing that destroys the audio path.
- **tib is already close to stripped** (376 packages, 17 running services, a second
  fallback kernel, no compiler). The remaining work there is not "remove more packages";
  it is the 2 GB/1 GB-zram memory budget and the absence of isolation in the kernel config.

---

## 3. What prior art actually does (short version)

Full detail and citations in `docs/research/audio-distro-prior-art.md`. The recurring delta
over a base distro is smaller than its reputation:

| project | base | what it really adds |
|---|---|---|
| **AudioLinux** | Arch | custom RT kernel, boot-to-RAM, and — since v7.0.0 — **systemd slices for CPU isolation + `CPUSchedulingPriority` for RT priority**, replacing its old polling daemons. `kernel.sched_rt_runtime_us=-1`. Headline latency numbers are unsourced marketing. |
| **Ubuntu Studio** | Ubuntu | 24.04's `performance-tweaks` is **two files**: `vm.swappiness=10` and a udev rule giving `rtc0`/`hpet` to group `audio`. RT limits come from a *different* package's postinst. Since 24.10 the dedicated low-latency kernel is gone in favour of the generic kernel + `preempt=full threadirqs rcu_nocbs=all`. |
| **Arch pro audio** | Arch | `realtime-privileges` (group + limits + `cpu_dma_latency` udev), `udev-rtirq` for IRQ-thread priority, performance governor, `vm.swappiness=10`, **do not run `irqbalance`**. |
| **AV Linux** | MX/Debian | Liquorix low-latency preempt kernel with threaded IRQs. |
| **piCorePlayer** | piCore/Tiny Core | the genuine strip-a-distro case: ~12 MB, entirely in RAM, no SD writes. |
| **moOde / Volumio** | RPi OS Lite / Debian | the opposite: add packages to a Lite base and `apt-mark hold`. |
| **Elk / Bela** | Yocto / BeagleBone | own the whole stack; Elk uses **EVL (Xenomai) dual-kernel**, not PREEMPT_RT. Heavy, and proves the "strip from scratch" route is viable but expensive. |

Portable to these boards: PAM limits (already correct), the `cpu_dma_latency`/`rtc0` udev
rules, performance governor (already correct), `swappiness`, journald, **systemd-slice CPU
isolation**, `rtirq`. Not portable: x86 RT kernels, AudioLinux's proprietary tools,
boot-to-RAM for a full desktop.

---

## 4. What to do on each board

### 4.1 piv — confine the housekeeping with cgroup v2, don't strip Ubuntu

`isolcpus=domain` removes cores from load balancing. It does **not** remove tasks that are
already affine to them, and on piv **[measured] 133 tasks have a CPU mask that includes
cores 2–3**. The audio thread is protected by its own affinity, but every other task is one
scheduler decision from landing on a "reserved" core. The kernel now calls `isolcpus=domain`
the *less* flexible option:

> "Control Group v2 cpuset isolated partitions are recommended because they are tunable at
> runtime. The `isolcpus=` kernel boot parameter with the `domain` flag is a less flexible
> alternative that doesn't allow for runtime reconfiguration."
> — [CPU Isolation](https://docs.kernel.org/admin-guide/cpu-isolation.html)

**This was run, and it works — on both boards *[verified 2026-09-16]*.** A transient
`systemctl set-property --runtime <slice> AllowedCPUs=0-1`:

| | before | after | read-back | rollback |
|---|---|---|---|---|
| **piv** `user.slice` | `cpuset.cpus.effective` 0-3, 133 tasks with cpu2-3 in mask | **0-1**, `partition=member` | new `systemd-run` child reported `Cpus_allowed_list: 0-1` | 105 → `revert` → 134 |
| **tib** `system.slice`+`user.slice` | cpuset absent from `subtree_control`; 115 tasks on cpu2-3 | **0-1** on both, and `cpuset` appeared in `subtree_control` automatically | new child reported `0-1` | 94 → `revert` → cpuset back to absent |

Two things this settles that the documentation does not say plainly:

- **Existing tasks are migrated, not just new ones.** The count of tasks whose mask includes
  the audio cores dropped 133→105 on piv and 115→94 on tib immediately after the property
  was set. What remains are kernel threads and services in slices that were not constrained
  (`system.slice` on piv, `init.scope` on both), which is why **every top-level slice** has
  to be covered, not only the one holding user sessions.
- **tib needs no kernel change for this.** Its `cpuset` controller was not even enabled in
  `cgroup.subtree_control`; systemd enabled it on demand when the property was set. So
  `AllowedCPUs=` is available on tib today, independently of the `NO_HZ_FULL` deferral in §4.3.

Concretely, on this system (systemd 259 on **both** boards, cgroup v2; root has tasks so
`+cpuset` into `/sys/fs/cgroup/cgroup.subtree_control` is **refused** — the "no internal
process" constraint):

```ini
# /etc/systemd/system/system.slice.d/10-housekeeping.conf   (also user.slice, machine.slice,
#                                                            init.scope as needed)
[Slice]
AllowedCPUs=0-1
```

`AllowedCPUs=` is the systemd spelling of the `cpuset` controller and exists since systemd
244, so it works here without a kernel change and without a reboot-with-risk. `CPUSetPartition=isolated`
is the direct `cpuset.cpus.partition=isolated` equivalent from systemd 261 — **not available
on this box (259.5)**, so write `cpuset.cpus.partition` directly if a true isolated partition
is wanted:

```bash
# partition root for the audio work, members for everything else
echo "+cpuset" > /sys/fs/cgroup/cgroup.subtree_control      # refused while root has tasks
```

Note the trap: **on cgroup v2 `cset shield` does not work at all**, and the "root has tasks"
constraint means the cpuset controller has to be delegated through systemd or a non-root
subtree, not bolted onto `/sys/fs/cgroup` by hand (SUSE's
[Shielding Linux Resources](https://documentation.suse.com/sle-rt/15-SP6/single-html/SLE-RT-shielding/index.html)
covers this; it declares `cset` unsupported on v2).

Second, and cheap: **`vm.stat_interval=1` *[measured]* is the default on piv.** The kernel
documentation lists `vmstat` as one of the things `nohz_full` offloads, which is why a quieter
housekeeping side is worth having. This is a **runtime-writable sysctl** — `echo 10 >
/proc/sys/vm/stat_interval` works as root and reads back 10 *[verified 2026-09-16]* — but it
does **not** survive a reboot, so either set it in the same oneshot service as the governor or
pass it on the kernel command line. An unprivileged write fails with a permission error and
leaves the value at 1, which is what the earlier reading of this file recorded:

```bash
echo 10 | sudo tee /proc/sys/vm/stat_interval
cat /proc/sys/vm/stat_interval          # must read 10; the file is write-only, so read back
```

Third, the services. Nothing here is about CPU: it is about **bytes and disk activity from
things the machine does not need to be an audio appliance**. Masks, not disables, for
anything that should never start (`disable` still allows socket/dependency activation):

```bash
sudo systemctl mask apt-daily.timer apt-daily-upgrade.timer apt-daily.service \
  apt-daily-upgrade.service unattended-upgrades.service snapshot.service
sudo systemctl mask cloud-init.service cloud-init-local.service cloud-final.service \
  cloud-config.service cloud-init-main.service cloud-init-network.service
sudo systemctl mask ModemManager.service bluetooth.service snapd.service snapd.socket \
  snapd.seeded.service snapd.apparmor.service
sudo systemctl mask NetworkManager-wait-online.service systemd-timesyncd.service
```

On this host that is ~69 enabled units down to the 15 or so that matter, and it removes the
`unattended-upgrades` hazard `rtpi`'s README already documents (a dracut upgrade re-running
flash-kernel and destroying the rollback slot mid-test). It is also the direct answer to
`ubuntu-advantage.service`, `ua-reboot-cmds`, `pollinate`, `rsyslog` (journald already
captures), `sysstat`, `apport`, `udisks2`, `multipathd`, `open-iscsi`, `qemu-kvm`,
`libvirtd`, `virtlogd`, `virtlockd`, `nginx` and `smbd` — all enabled here and all
irrelevant to a headless audio appliance.

**The elephant:** the 852 MB Node process is this agent's own UI. It cannot be masked away
without ending the session that is doing the work. What it *can* be is confined:

```bash
# conceptually: keep the whole interactive/work slice off the audio cores
systemctl set-property user.slice AllowedCPUs=0-1
systemctl set-property system.slice AllowedCPUs=0-1   # careful: this moves everything, incl. sshd
```

and measured. That is experiment E1 in §7, and it is the single highest-value arm in this
document, because it tests the premise directly: *if the dev stack is confined to the
housekeeping cores and the tail does not move, then no amount of package removal will help.*

### 4.2 piv — keep the RT setup that already exists

Do not re-litigate what is already right *[all verified 2026-09-16]*:
`CONFIG_PREEMPT_RT=y`, `NO_HZ_FULL=y`, `RCU_NOCB_CPU=y`, `RT_GROUP_SCHED` **not set**,
`HZ=1000`, the osnoise/timerlat/hwlat tracers present so `rtla` works, soft/hard lockup
detectors **not** set; `isolcpus`/`nohz_full` in force on cores 2–3; `@audio` limits;
performance governor at 2 400 000 kHz; `usbcore.autosuspend=-1`; `irqbalance` absent;
`namchain-realtime-tune.service` and `rtpi-audio-tune.service` both enabled; the archive
package `linux-image-7.0.0-1017-raspi-realtime` (no UA/Pro subscription needed, so
`ubuntu-advantage` can be masked). The audio IRQ `xhci-hcd:usb2` is pinned to cpu 2 with its
thread at `SCHED_FIFO 85`, above the engine's 80.

Two loose ends: the older `linux-image-6.8.0-1064-raspi` is still installed from 24.04 and is
one more kernel a careless `update-initramfs` can rebuild — remove it if the rollback story
allows. And `/dev/cpu_dma_latency` is `0600 root:root`, so nothing in the `audio` group can
use the PM-QoS interface (see §5.8).

### 4.3 tib — the strip is done; the levers are memory and a kernel option

tib at 376 packages cannot be productised much further by deletion. Three real items:

1. **Write a small script that masks the Armbian conveniences and calls it the strip.**
   `armbian-hardware-monitor`, `armbian-hardware-optimize`, `armbian-led-state`,
   `armbian-zram-config`, `armbian-ramlog`, `fake-hwclock-load/save`, `systemd-networkd-wait-online`,
   `cron`, `rsyslog`. Two of these matter beyond tidiness:
   **`armbian-ramlog`** puts `/var/log` on `zram1` (50 MB, `ext4`, `discard`) and
   **`armbian-zram-config`** creates `zram0` as a **998 MB compressed swap on a 2 GB board** —
   compression is CPU-and-memory-bandwidth work in `zram`/`kworker` context, i.e. exactly the
   resource the project measured as the tail. *[Measured 2026-09-16]* **zram0 is currently
   empty** — 0 MB original, 0 MB compressed, `USED 0B`, `vm.swappiness=60` — so the cost today
   is the reserved 998 MB and the `lzo-rle` thread, not active traffic. It becomes traffic the
   moment something allocates; on a 2 GB board the honest settings for an appliance are
   **no swap at all** or a much smaller zram, plus `vm.swappiness=1`. Runtime `swappiness`
   writes work (`echo 1 > /proc/sys/vm/swappiness` *[verified]*), but do not survive a reboot.
2. **Decide whether `CONFIG_DRM=y` belongs on a headless audio node.** The kernel
   documentation is explicit that GPU-accelerated workloads share LLC and memory bandwidth
   with the CPU and recommends `CONFIG_DRM` disabled for determinism. *[Measured on tib
   2026-09-16]* the claim does **not** transfer cleanly: the active driver is **`simpledrm`**
   (16 KB, a framebuffer shim, `drm_sysfb_helper`), there is no GPU driver bound, and the
   only DRM clients are `card0`/`card1` plus the HDMI audio codec. The console is
   `console=tty1 consoleblank=0` on a virtual terminal, so there is no compositor and no
   rendering traffic. **Disabling DRM on tib would buy almost nothing** and would cost the
   out-of-band video-console recovery path the `rtpi` README documents
   (`UBOOT_HAS_VIDEO_CONSOLE=yes`). Drop this item; the largest remaining shared-memory
   consumer on tib is the memory-loaded housekeeping side, addressed in §7a.
3. **Isolation is gated on a kernel option, not on userspace.** *[measured]* tib's own
   kernel has `# CONFIG_NO_HZ_FULL is not set` and `CONFIG_NO_HZ_IDLE=y` — deliberate, and
   the fragment says why (`config/tib-rt.fragment`: "on a 4-core board running a full
   desktop-ish Armbian userspace there are none to spare"). The cgroup-v2 `cpuset` route
   does **not** need `NO_HZ_FULL`: `AllowedCPUs=` on the top-level slices still gets the
   housekeeping tasks off cores 2–3, and it is runtime-tunable. What it cannot do on tib is
   stop the tick, and §7b measures that this is enough to help and not enough to pass.

4. **The device situation has changed, and it unblocks the device arms.** *[measured
   2026-09-16]* the **SSL 2+ Mk II is attached to tib** — card 2, `hw:2,0`, on the
   `ehci_hcd:usb3` controller (IRQ 54) — and it is **S32_LE, 8 capture / 6 playback,
   44.1–192 kHz**, i.e. exactly the interface piv has been measured with. `plan-rt-tib.md`
   §2 assumed it was absent and that only the 8 kHz Realtek (`hw:0,0`, dwc2) was available;
   that assumption is now wrong. A 3 s probe with
   `--device hw:2,0 --capture-channels 8 --playback-channels 6 --quantum 64 --periods 4
   --output-queue-periods 0 --instances 3` reached `chain p99 401.6 µs (30.1 % of deadline)`
   with the kernel reporting `period/buffer 64/256 frames` and `SCHED_FIFO priority 80,
   cpu 3`. **Note the two USB paths are not interchangeable** on this board: the SSL sits on
   EHCI and enumerates cleanly, while the Realtek hangs off `dwc2` — the controller whose
   port-power latch-off is documented in `plan-rt-tib.md` §1a and in `rtpi`. Keep the SSL on
   EHCI.

   Also worth recording for the same reason: `sched_setaffinity(cpu 3)` from the soak failed
   with `EINVAL` on one 3 s probe that ran *while `stress-ng` was starting*, leaving the
   engine on "any of 2 cpus". The same command succeeded moments later. Treat a refused pin
   on tib as a reason to re-check `running_cpu` in the report rather than trusting the
   command line — the failure is reported, not fatal, which is exactly what §6.9 of the
   piv plan says to look for.

### 4.4 Both — what "stripped" should mean for a *shipped* appliance

For redistribution (`plan.md` §4's static-link question), the realistic menu is in
`docs/research/arm-sbc-minimal-userspace.md`:

- **Armbian `BUILD_MINIMAL=yes`** (bare CLI, incompatible with `BUILD_DESKTOP`) — the
  least-effort minimal base, and it already builds the per-board kernel/DTB, which is the
  part a generic `linux-image-rt-arm64` cannot solve. Armbian ships **no** PREEMPT_RT, so the
  RT kernel stays this project's own build (which is what `rtpi` already does for tib).
- **`mmdebstrap --variant=minbase` / `ubuntu-base` (~29 MB tarball)** — smallest apt-based
  base; you own bootloader, kernel, DTB and firmware.
- **Buildroot / Yocto** — smallest and most reproducible, but no apt and security updates
  means rebuild+reflash. Elk proves an audio OS on this route; it also shows the cost.
- **Do not** plan on Debian's `linux-image-rt-armmp` booting an RK3288 with the right DTB;
  that is unverified and the per-board kernel is the reason.

---

## 5. The traps, in one place

These are the ones that would waste a day each. All are cited in
`docs/research/isolation-and-stripping-techniques.md`.

1. **`RT_GROUP_SCHED` + cgroup v2 refuses `SCHED_FIFO` to every non-root cgroup, including
   root.** tib's stock Armbian kernel had it on; this project's build turns it off, and
   Ubuntu's realtime flavour ships it off. A cgroup maintainer confirmed in Sep 2025 that
   "v2 de facto doesn't support RT group scheduling… v2 systems should simply unset
   `CONFIG_RT_GROUP_SCHED`". Check `grep CONFIG_RT_GROUP_SCHED /boot/config-$(uname -r)` on
   any new board before blaming anything else.
2. **`isolcpus=` does not move tasks that are already affine to the core, and nothing
   migrates there by itself.** Measured on piv: 137 tasks with cores 2–3 in their mask. Pin
   explicitly, or confine the slices (§4.1).
3. **`nohz_full` needs exactly one runnable task on the core.** A multi-threaded engine, a
   worker thread, or a second runnable task re-arms the tick immediately. `rcu_nocbs` on a
   `nohz_full` CPU is redundant (the kernel offloads automatically) but harmless — and this
   project's `skew_tick`/`arch_timer` measurement is the proof the intervention held, so
   keep recording it.
4. **`taskset`/`sched_setaffinity` on an IRQ thread fails** — they carry
   `PF_NO_SETAFFINITY`; `/proc/irq/N/smp_affinity_list` is the only interface, and the
   change only takes effect at the *next* interrupt (an idle device looks like a failure).
   *[Verified on piv 2026-09-16]* the live audio IRQ is **`xhci-hcd:usb2` = IRQ 134** (the
   number moved from the 139 in `plan-rt-piv.md` — match by name, never by number), affinity
   reads back `2`, and its thread `irq/134-xhci-hcd:usb2` is `SCHED_FIFO 85` as intended.
   `sudo taskset -pc 2 <tid>` fails with *"affinity cannot be set due to PF_NO_SETAFFINITY
   flag set"* and leaves the mask at `0-3`; writing `/proc/irq/134/smp_affinity_list`
   succeeds and reads back `2`. The Pi 5's RP1 GPIO IRQ thread ignores even that
   ([raspberrypi/linux#7301](https://github.com/raspberrypi/linux/issues/7301)); the xHCI
   audio IRQ is steerable, which is the one that matters here.
   **This interacts with §4.1 — measured, and the interaction is benign.** *[Verified]* the
   audio IRQ thread `irq/134-xhci-hcd:usb2` sits in the **root cgroup (`0::/`)**, not in
   `system.slice`. Constraining `system.slice`/`user.slice` leaves it alone, and a
   `set-property --runtime -- -.slice AllowedCPUs=0-1` was accepted but **had no effect**:
   root's `cpuset.cpus.effective` stayed `0-3` and the thread's mask stayed `0-3`, while the
   IRQ kept counting on cpu 2 (`xhci-hcd:usb2` 525 487 interrupts on cpu2, 0 on cpu1/cpu3 at
   the time of the check). So the confinement scheme that works does **not** silently undo the
   IRQ pinning. Keep the read-back check anyway: the affinity file keeps reading `2` because
   that is the interrupt controller's routing, not the thread's cpuset, so the two can
   disagree without either one erroring. The Pi 5's RP1 GPIO IRQ thread ignores steering
   entirely ([raspberrypi/linux#7301](https://github.com/raspberrypi/linux/issues/7301)); the
   xHCI audio IRQ is steerable, which is the one that matters here.
   *[Also measured]* even with no device attached, `xhci-hcd:usb2` accumulates roughly
   **30 interrupts/s on cpu 2** — the reserved audio core is not literally quiet, and that
   rate is the floor to compare a real device against.
5. **`threadirqs` is a no-op on PREEMPT_RT** — the `early_param` is compiled out. Already
   recorded in `docs/tuning.md`; worth re-checking that no script adds it.
6. **`sched_rt_runtime_us=-1` removes the only automatic escape from a runaway `SCHED_FIFO`
   loop** on a 4-core SBC. AudioLinux sets it; the kernel warns against it; on cgroup v2 you
   cannot lower it below 950000 either because `cpu.rt_runtime_us` is gone. Leave it alone
   unless a specific measurement justifies it.
7. **`systemd-oomd` kills whole cgroups** on memory pressure. On a headless node, that is the
   audio engine. Mask it, or set `ManagedOOMPreference=avoid`.
8. **`/dev/cpu_dma_latency` is `0600 root:root` on both boards *[verified]*.** The PM-QoS
   interface that lets an application refuse deep C-states is therefore unusable by the
   `audio` group as shipped. Ubuntu Studio patches this with a udev rule
   (`99-cpu-dma-latency.rules`, `GROUP="audio"`); tib has no equivalent. One udev rule, and
   it is the kind of thing an audio appliance should ship rather than inherit.
9. **RT throttling differs between the two boards *[verified]*:** piv runs
   `sched_rt_runtime_us=950000/1000000` (the 5 % reserve), tib runs `1000000/1000000`
   (the reserve disabled). Neither is `-1`; the difference is worth recording beside any
   cross-board comparison, because it is a scheduler setting that was not held constant.
10. **`Storage=volatile` for journald stops log writes reaching the disk** at the cost of logs
    vanishing on reboot. With 2 GB of RAM on tib and zram already in play, budget it explicitly.
11. **Mask, don't disable.** `disable` leaves socket and dependency activation alive.
12. **A stripped system is a bare system.** Removing `systemd-resolved` without a replacement
    breaks DNS; removing `dbus`, `udev`, `apt`/`dpkg` or the matching kernel modules breaks
    the machine. `apt-mark showmanual` and Installed-Size sorting are the audit, not a purge list.

---

## 6. What this does and does not buy

- **Does buy:** fewer wakeups and fewer memory-touching background tasks on the housekeeping
  side; a controlled, auditable answer to "is anything else on the audio cores"; and the
  variance reduction the project needs before it can trust a two-hour xrun count.
  *[Measured]* the largest single effect is not the latency at all — it is that **`nohz_full`
  starts working**: `arch_timer` on the audio cores fell from ~475 000 per 120 s to 23 / 0
  once the housekeeping tasks were confined (§7a). On a real device the driver-xrun damage
  from a memory-loaded housekeeping side went **746 → 0** on tib (§7d).
- **Does not buy:** a quiet memory system by itself. The `--vm` signature is a property of the
  shared bus and LLC, and deleting packages cannot partition either. E1 (§7a) shows the
  confined tail still 5× the quiet max under the same load; E5c (§7d) shows driver xruns
  eliminated but block p99 still above the deadline. The remaining lever is therefore *the
  engine's* memory traffic (fewer models, narrower weights, better reuse) — which is
  `plan.md` §14's existing memory-traffic question, not a distro question.
- **Does not replace the RT kernel or the IRQ pinning.** Both are already done on piv; on
  tib the RT kernel is the load-bearing part and the cgroup confinement is the step that
  measured well.
- **Does not carry over between boards.** Confinement cut p99 by 58 % on piv and by 14–47 %
  on tib depending on the workload, and it eliminated driver xruns on tib's device arms. The
  direction is consistent; the magnitude is not portable, so each board has to be measured.

---

## 7. Experiments, in the order that makes each one interpretable

Every arm follows the project's existing discipline: one variable, a manipulation check
taken *during* the run, `bench-results/rt-*` evidence files, and the oracle outranking the
host. All of these are cheap and reversible; none needs a reboot except where noted.

| id | arm | what it decides |
|---|---|---|
| **E1** | **piv, A/B**: 15 min soak, housekeeping unconfined vs `AllowedCPUs=0-1` on `user.slice`+`system.slice`, identical audio config | The premise. If the tail does not move, package removal is the wrong lever and §6's third bullet is the answer. **Done as a cyclictest screen, not an engine soak — see §7a.** |
| **E2** | **verify the cgroup** — `cpuset.cpus.effective` per slice; a 10 s per-CPU `/proc/interrupts` delta with the audio thread pinned to 3; `cpuset.cpus.partition` if written directly | Proves E1's intervention happened. Without this, E1 is the unreadable 16-arm experiment again. Needs sudo. |
| **E3** | **piv**: `vm.stat_interval=1` vs `10`, set at runtime as root (verified writable, reads back 10) and made persistent via the governor oneshot or cmdline | Whether the 1 Hz vmstat shepherd is measurable at all. No reboot needed for the A/B. |
| **E4** | **piv**: soak while building (`cmake --build` on all cores) with and without E1's confinement | Puts a number on the dev-stack question the project actually lives with daily. |
| **E5** | **tib**: `AllowedCPUs=0-1` on top-level slices, audio on 3, **no** kernel change; compare against the existing deviceless 3 × A2-Lite arm (`p99 29.7 %`, max 628 µs) | **Done — see §7b.** Confinement is worth doing and is not sufficient; the board's job has to stay small. |
| **E6** | **tib**: `zram` off vs the current 998 MB zram0, `vm.swappiness=1`, under a memory-loaded housekeeping arm | Whether compressed swap is a tail contributor on the 2 GB board. |
| **E7** | **tib** (kernel rebuild, low priority): `NO_HZ_FULL=y` + `rcu_nocbs` on cores 2–3 only, cross-built on piv and staged through the existing candidate/known-good slot | The deferred decision, made against E5's result rather than in the abstract. |

Stop rules are the existing ones: non-zero oracle diff, `isolated` empty, `arch_timer`
growing on the soak core, `throttled != 0x0`, or the device failing to open.

### 7.0 Correction (2026-09-17): the confined arms moved the workload too

Added after the fact, because it affects **E1, E5, E5b and E5c**. Their own instruments recorded
the problem -- the conclusions drawn from those arms did not account for it.

`AllowedCPUs=` on `system.slice`, `user.slice` and `init.scope` confines *everything in those
slices*, and the measurement process lives in them. So in every "confined" arm the workload was
pushed off the audio cores **along with** everything else, rather than the other work being kept
off while the audio work stayed. The evidence is in the arms' own output:

| arm | what it reported |
|---|---|
| E1 (`cyclictest`) | `WARN: Couldn't setaffinity in main thread: Invalid argument`, `WARN: Could not set CPU affinity to CPU #2 / #3` |
| E5 (`null` soak) | `any of 2 cpus (not as requested: sched_setaffinity(cpu 3): Invalid argument)` |
| E5b (SSL), E5c (SSL) | the same |

So each table contrasts two different **placements**, not one placement with and without
confinement:

  * unconfined: the measurement runs on the audio cores, with the rest of the system free to join it;
  * confined: the measurement runs on the **housekeeping** cores, alongside the load.

That changes what each number means:

  * **E1's arch_timer fall from ~475 000/120 s to 23/0 is not "`nohz_full` starts working"** -- it
    is that nothing was left running on cores 2-3. And the latency comparison (max 215 → 169 µs)
    is cyclictest *on the audio cores* against cyclictest *on the loaded housekeeping cores*.
  * **E5/E5b/E5c's "confinement takes driver xruns 746 → 0"** is the same confound: the engine
    moved onto cpus 0-1 with the vm worker and did better there. Whatever that is, it is not a
    confinement effect, because confinement was not applied to the configuration it is credited
    with improving -- it was applied to a different one.

**The research this document is for is unaffected.** §1's reframing (the tail is memory traffic,
not CPU time), §2's board survey, §3's prior art and §4's recommended actions all rest on
measurements that do not depend on these arms.

**The correct method, and what these must be re-run as:**

1. hold the top-level slices at 0-1 (`system.slice`, `user.slice`, `machine.slice`, `init.scope`),
   so all the *other* work is off the audio cores;
2. put the audio work in **its own** cpuset partition with `AllowedCPUs=2-3`, so it is the one
   thing allowed to use them;
3. pass the rlimits to the unit, because a systemd service does not get `pam_limits` and
   `SCHED_FIFO` is therefore refused to it:

```sh
systemd-run --slice=audio.slice -p AllowedCPUs=2-3 \
            -p LimitRTPRIO=95 -p LimitMEMLOCK=infinity --uid=rik ...
```

Verified on piv 2026-09-17 with a 20 s smoke test: `scheduling: SCHED_FIFO priority 80, cpu 3`.
Without the two `Limit...` properties the identical invocation silently reports
`SCHED_OTHER priority 0` -- real-time refused -- which is its own warning about how easy this is
to get wrong, and the reason step 3 is written down rather than assumed.

### 7.0b The re-run, 2026-09-17: on tib, confinement does not move the engine

Run with the method above -- top-level slices at 0-1, engine in its own cpuset partition at 2-3,
rlimits through the unit, four arms in one session with **no reboots**, balanced. Every device arm
confirmed `SCHED_FIFO priority 80, cpu 3`, which is the check §7's arms failed. Evidence:
`bench-results/tib-confinement-rerun.txt` and `bench-results/tib-{cx,dx}-*`.

**Deviceless, under `stress-ng --vm 1 --vm-bytes 128M --vm-keep` on cpus 0-1** (`tib-cx-*`, 900 s
per arm, `--backend null`, 3 × A2-Lite):

| arm | condition | late blocks | blk p99 | blk max | p99 % |
|---|---|---|---|---|---|
| cx-1 / cx-4 | unconfined | 73 607 / 71 264 | 1760 / 1749 | 9827 / 2033 | 131.97 / 131.21 |
| cx-2 / cx-3 | confined | 68 364 / 72 356 | 1739 / 1742 | 3326 / 2051 | 130.40 / 130.64 |

**3 % on late blocks and 14 µs on p99** — nothing measurable, and the worst block varies by a
factor of five *within* each condition. §7b's "overruns −31 %, p99 −14 %" was the placement
change.

**The device path** (`tib-dx-*`, 600 s per arm, SSL 2+ as card 2 through the powered hub, 3 ×
A2-Lite, ring 6, cap 4, floor 2, engine pinned to cpu 3 in all four arms):

| arm | load | confined | xruns | late blocks | blk p99 | p99 % | result |
|---|---|---|---|---|---|---|---|
| dx-1-quiet-unc | none | no | **0** | **0** | 493 µs | 36.95 | **PASS** |
| dx-4-quiet-conf | none | yes | **0** | **0** | 492 µs | 36.90 | **PASS** |
| dx-2-vm-unc | vm | no | 2876 | 51 048 | 1786 µs | 133.92 | FAIL |
| dx-3-vm-conf | vm | yes | 2701 | 48 780 | 1796 µs | 134.73 | FAIL |

1. **tib's device ceiling is real and good:** 0 xruns, 0 deadline overruns, p99 36.9 % of the
   deadline, worst block under 800 µs — and identical with and without confinement. This is the
   number §7c warned must not be quoted from the wrong arm, now measured on the right one.
2. **Confinement does not rescue the device path under memory pressure.** 2876 → 2701 xruns is
   6 %, inside the noise; p99 133.9 % → 134.7 % is if anything the wrong way. §7d's "746 → 0" was
   the placement change, and the real effect is nil.

**So on this board confinement buys nothing when the memory system is busy** — the housekeeping
CPUs and the audio CPU share one bus, and there is no spare core to move anything to. That
strengthens the conclusion §7b reached by a different route ("the variable is not controllable
from userspace; run nothing else on it"), but the evidence is now the *absence* of an effect
rather than a large one.

The two boards genuinely differ, and that is the durable finding:

| | confinement, quiet | confinement, under a vm load |
|---|---|---|
| **piv** (core to spare, quieter memory system) | late blocks 7/5 → 1/2 (~4×) | not measured |
| **tib** (no spare core, shared bus) | device: 0 xruns and 0 late blocks either way | 3 % on late blocks; 6 % on device xruns |

### 7a. E1 result

> **Confounded -- see §7.0.** The confined arm here did not keep the workload on
> the audio cores, so this table compares two placements rather than confinement. — run 2026-09-16, and it moves the tail

The SSL 2+ is not currently attached to piv (only Dummy/Loopback/HDMI cards were present), so
the engine soak could not run. The hypothesis was tested without it: `cyclictest` on the audio
cores (`-a 2-3 -t2 -p80 -i200 -m`, 600 000 samples per thread, 120 s per arm) under loads
pinned to the housekeeping cores. Evidence: `bench-results/host-confine-screen.json`; driver
`tools/soak/host-confinement-screen.py`.

| arm | max | p99.9 | p99 | avg | `arch_timer` on cpu2/cpu3 |
|---|---|---|---|---|---|
| `quiet-unconfined` | 32 µs | 6 | 4 | 2.2 | 511 751 / 473 924 |
| `vm-unconfined` | **215 µs** | **51** | **19** | 3.7 | 470 953 / 509 458 |
| `vm-confined` | **169 µs** | **26** | **8** | 2.6 | **23 / 0** |
| `cpu-unconfined` | 22 µs | 5 | 3 | 2.2 | 465 860 / 409 558 |

Four readings, in order of importance:

1. **The premise is confirmed, and CPU use is not the mechanism.** `stress-ng --cpu 2` pinned
   to cpus 0–1 costs *nothing* (max 22 µs, p99 3 µs). `stress-ng --vm 1 --vm-bytes 128M` on
   the same cores takes max from 32 → **215 µs (6.7×)** and p99.9 from 6 → 51 µs (8.5×). This
   is the `--vm` signature from `docs/tuning.md`, reproduced on a latency timer instead of an
   audio device.
2. **Confinement helps: max −21 %, p99.9 −49 %, p99 −58 %, mean −31 %.** Real, but not a
   cure — the confined arm still sits 5× above the quiet max. Confining the housekeeping side
   is worth doing and does not by itself make the memory system quiet.
3. **The biggest effect is not the latency — it is that `nohz_full` starts working.**
   `arch_timer` on the audio cores falls from ~475 000 per 120 s (a 1 kHz tick) to **23 and
   0**. Without confinement, `isolcpus=domain` leaves ~130 unbound tasks free to drift onto
   cores 2–3, each drift re-arming the tick; with the slices pinned, the isolated cores are
   genuinely tickless. This is exactly the manipulation check `plan-rt-piv.md` §3 P4 demanded,
   and it says the isolation had been *partly* nominal.
4. `throttled=0x0` throughout; rollback verified (`cpuset.cpus.effective` back to `0-3` on
   `system.slice`, `user.slice`, `init.scope`).

**What this changes:** E1's premise is answered — confinement is worth having for the tick
suppression alone. E2 is now the prerequisite for any engine soak, because a confined
`system.slice` plus a *root-confined* cgroup could still interact with the IRQ thread, and the
soak must record `arch_timer` per core the way this screen did. The engine-level question
(whether 4 × A2-Full survives the memory load once confined) is still open and still needs the
device.

### 7b. E5 result

> **Confounded -- see §7.0.** The confined arm here did not keep the workload on
> the audio cores, so this table compares two placements rather than confinement. — tib, run 2026-09-16: confinement helps and is not enough

tib has no `isolcpus` and no `NO_HZ_FULL` (deliberately — 4 cores, no spares), so
`AllowedCPUs=` on the top-level slices is the only isolation lever it has. Both arms ran the
project's own deviceless configuration — `bin-neon/namchain-soak --backend null --quantum 64
--instances 3 --model models/A2-Lite.nam --input-offset 1 --self-oracle --rt-priority 80
--cpu 3 --seconds 180` — with `stress-ng --vm 1 --vm-bytes 128M --vm-keep` pinned to cpus 0–1.
Driver: `tools/soak/tib-e5-confinement.sh`; evidence `bench-results/tib-e5-vm-*` (on tib).

| arm | block p50 | block p99 | block max | deadline overruns | p99/deadline |
|---|---|---|---|---|---|
| reference (`tib-b2-null`, no load) | 351 µs | **395.8 µs** | **628 µs** | 0 | 29.7 % |
| `vm-unconfined` | 478.6 µs | **1759 µs** | 2169 µs | **12 722** | 131.9 % |
| `vm-confined` | 497.9 µs | **1513 µs** | 1713 µs | **8 838** | 113.5 % |

- **Confinement moves it: p99 −14 %, max −21 %, overruns −31 %.** Real, and much smaller
  than on piv (where p99 fell 58 %).
- **It does not come close to saving the arm.** p99 is 13 % *above* the deadline and 8 838
  blocks are late. The unconfined arm is worse but both fail.
- **The `p50` got slightly worse (478 → 498 µs)** while the tail improved — the same RT
  trade the project already documented in `docs/tuning.md`: RT compresses the tail and can
  cost the median. Not a regression.
- The manipulation check confirms the intervention: `arch_timer` on cpu3 fell 334 972 →
  36 264 over the arm, and the `.isolation` verdict records "no CPUs are isolated, and that
  is this host's expected configuration".

**The conclusion for tib is the strongest result in this document, and it is a reason to keep
the board's job small.** The bare 3 × A2-Lite loop (29.7 % of deadline, 0 overruns) and the
memory-loaded loop (113 % of deadline, 8 838 overruns) differ only in whether a 128 MB worker
is touching the shared memory system. On a 4-core ARMv7 board with no core to spare, **that
variable is not controllable from userspace** — nothing you delete or confine changes the fact
that the housekeeping CPUs and the audio CPU share one bus. So for tib the design rule is not
"strip the distro harder", it is **run nothing else on it**: no builds, no containers, no
indexers, no desktop. If a machine must do both, piv (where the confined p99 stayed well
under the deadline) is the one that tolerates it, and tib is the one that does not.

### 7c. E5b — tib with the real device

> **Confounded -- see §7.0.** The confined arm here did not keep the workload on
> the audio cores, so this table compares two placements rather than confinement., on the SSL 2+ Mk II

The SSL moved to tib mid-session (§4.3), which allowed the device arms that
`plan-rt-tib.md` §2 had assumed were impossible. Arms: 3 × A2-Lite, NEON build, 48 kHz, q64,
`--device hw:2,0 --capture-channels 8 --playback-channels 6 --periods 4
--output-queue-periods 0`, 180 s, cpu 3. Evidence: `bench-results/tib-e5b-*`.

| arm | block p99 | block max | deadline overruns | **driver xruns** | write p99/max |
|---|---|---|---|---|---|
| `quiet` | 430.5 µs | 3 578 µs | 2 | **2 341** | 39.4 µs / **3 118 µs** |
| `vm-unconf` | 1 816 µs | 9 795 µs | 13 439 | **5 423** | — |
| `vm-conf` | 965 µs | 4 138 µs | 195 | **2 158** | — |

- **The confinement effect is large and consistent:** under the same memory load, confining
  the housekeeping slices cut driver xruns from 5 423 → **2 158 (−60 %)** and deadline
  overruns from 13 439 → **195 (−99 %)**, with block p99 1 816 → 965 µs. On a real device the
  argument for `AllowedCPUs=` on tib is therefore stronger than the deviceless screen in §7b
  suggested.
- **The quiet arm is not a valid baseline, and the reason is instructive.**
  `--output-queue-periods 0` means "the loop writes whenever the kernel has room" — the
  playback ring gets no headroom *by design*, and the report says so: `769 blocks wrote into
  a full playback ring`, `270 745 samples where delay disagreed with the pointer pair`, and a
  **write** tail of 3.118 ms against a chain tail of 577 µs. 2 341 xruns with only 2 deadline
  overruns is the signature of queue underruns, not of engine lateness — the rule
  `plan-rt-piv.md` §6.8 already states: a thrashing queue is a configuration result, not a
  repair measurement. **Do not quote the quiet row as tib's device ceiling.** E5c re-runs the
  same three arms with the piv shipping queue shape (ring 6, cap 4, floor 2) to get one.
- **A real bug surfaced:** the manipulation check prints `audio_IRQ (line not found)` on tib.
  It matches the IRQ by the name `xhci-hcd:usb2`, but tib's SSL is on **EHCI** (`ehci_hcd:usb3`,
  IRQ 54), so on this host the one interrupt that matters is never recorded in the deltas.
  That is a one-line fix in `tools/soak/rt-manipulation-check.sh` (take the IRQ name as an
  argument, as `plan-rt-tib.md` §6 deliverable 7 already asks) and it means **E5b's IRQ
  deltas are incomplete for the audio path.**
  **Fixed:** the check now takes `AUDIO_IRQ_CANDIDATES` (default covers xhci/ehci/dwc2) and
  resolves the first name actually present, so tib records `ehci_hcd:usb3`. Synced to both
  boards.

### 7d. E5c — the fair device comparison

> **Confounded -- see §7.0.** The confined arm here did not keep the workload on
> the audio cores, so this table compares two placements rather than confinement., and tib's real ceiling

Same as E5b but with the piv shipping queue shape (`--periods 6 --output-queue-periods 4
--output-queue-floor-periods 2`), which removes the queue artifact. Evidence:
`bench-results/tib-e5c-*`.

| arm | block p99 | block max | deadline overruns | **driver xruns** | queue control |
|---|---|---|---|---|---|
| `quiet` (no load) | **620.1 µs (46.5 %)** | 770.3 µs | **0** | **0** | 0 drops, 0 bursts |
| `vm-unconf` | 1 799.6 µs (135.0 %) | 6 136 µs | 14 090 | **746** | 242 output drops, 11 938 capture bursts |
| `vm-conf` | 969.8 µs (72.7 %) | 1 480 µs | **1** | **0** | 0 drops, 0 bursts |

1. **tib's real device ceiling is p99 46.5 %, 0 xruns, 0 overruns** — but *only* with no
   memory-loaded housekeeping. The earlier E5b "2 341 xruns while quiet" reading was entirely
   the unmanaged queue and must not be quoted. Note how close 46.5 % sits to the 50 % gate:
   the device is already near its margin with nothing else running.
2. **Confinement makes the device path survive memory pressure.** Under an identical
   128 MB vm worker, confining the top-level slices takes **driver xruns 746 → 0** and
   **deadline overruns 14 090 → 1**, and removes `242` output drops and `11 938` capture
   bursts. The p99 is still 72.7 %, i.e. the deadline is not met, but the failure mode changes
   from "the queue starves and the driver underruns" to "the engine is late occasionally".
3. **This is the strongest case in the document for a runtime cgroup change on a board that
   cannot use `isolcpus` at all.** tib gets none of the boot-time levers; `AllowedCPUs=` on
   `system.slice`+`user.slice` recovered essentially all of the driver-xrun damage on a real
   UAC2 device, at runtime, with a one-line rollback.

---

## 8. The deliverable: `tools/soak/host-slim.sh`

Written and run end-to-end on both boards on 2026-09-17. `--apply`, `--check`, `--diff`,
`--revert`, `--list-services`, plus `--dry-run`. It does the six things §4 describes:

1. `AllowedCPUs=` on `system.slice`, `user.slice` and `machine.slice` — transiently *and* as
   drop-ins — plus `init.scope`, which cannot take a drop-in at all and gets a boot oneshot
   instead (§8.1).
2. Masks the per-board service list, `mask` rather than `disable`.
3. journald `Storage=volatile` + `RuntimeMaxUse=64M`, so logging never reaches the disk.
4. `vm.stat_interval=10` via a oneshot unit (runtime-writable but not persistent).
5. A udev rule giving `/dev/cpu_dma_latency` to group `audio`.
6. `rttime 200000` for `@audio` as a runaway backstop — deliberately **not**
   `sched_rt_runtime_us=-1`.

Design decisions worth knowing before using it:

- **Every action is recorded in `/var/lib/namchain/host-slim/manifest`, and `--revert` undoes
  exactly that list.** No guessing which units were ours.
- **The manifest is merged, never reset.** See §8.1 — resetting it was the more damaging of
  the two defects that section records.
- **`--revert` disables the oneshot units before deleting their files**, or systemd is left
  with a dangling enablement symlink.
- **A time-sync guard.** `systemd-timesyncd` is on the piv list, but masking the only time
  source would break TLS, apt and every timestamp a measurement is compared against, and piv
  has no chrony installed. So the guard holds it back and `--check` reports it as intentional
  (`--  held back: only time source`) rather than as a failure. Install `chrony` or set
  `NAMSLIM_ALLOW_NO_TIME=1` to override.
- **`armbian-zram-config` is masked on tib but `armbian-ramlog` is not**, and the script says
  so at run time: zram swap costs CPU and memory bandwidth on a 2 GB board, whereas relocating
  `/var/log` off zram would put log writes on tib's only block device.
- **Masking a timer and the service it triggers leaves the timer reporting
  `result=resources`**, which shows up in `systemctl --failed` and would eventually hide a
  real failure. The script clears that cosmetic state with `reset-failed` after masking.
- **`--check` distinguishes `masked` from merely `disabled`, and checks the masks against the
  manifest.** "Not enabled" and "we masked it, and can therefore unmask it" are different
  claims, and conflating them is what let a half-reverted board pass as green.
- It touches **no** package, **no** boot asset and **no** kernel setting.

### 8.1 Two defects, and why the transcript exists

The first version of this section claimed a verified round trip. That claim was too strong,
and the reason it survived is instructive: `--check` answers "is the intended state in force
*right now*", and that answer is gone the moment the terminal closes. It cannot tell an
applied configuration from one that came back after a reboot, and it never said which revision
of the tool produced the state. Adding `tools/soak/host-slim-capture.sh` — which copies the
local tool to the board, re-reads its md5 there, and writes one file per board containing the
check, the manifest, the persisted files verbatim and the unit-transience facts — immediately
showed that piv, believed applied, was **half reverted**: cpusets back to `0-3` and all five
files absent, but 32 units still masked.

1. **The manifest was truncated on every apply, so `--revert` could not undo the masks.** The
   mask step records a `mask <unit>` line only for units it actually masks. A second `--apply`
   found them already masked, recorded nothing, and left a manifest that no longer mentioned
   any of them — so `--revert` unmasked nothing and reported success for the parts it could
   see. Fixed by merging the manifest instead of resetting it, and by making `--check` count
   masked units that have no manifest line and report them with a `WARN`. `--list-services`
   was added so a reconciliation is driven by the script's own set rather than a hand-written
   list. piv was repaired by unmasking exactly the 32 units in `service_list` that had no
   manifest line (the vendor's separately-masked units — `cryptdisks`, `hwclock`, `nbd-client`,
   `screen-cleanup`, `serial-getty@ttyAMA0`, `sudo`, `x11-common` — were left alone), then
   re-applying. The raw output is in `bench-results/host-slim-remediation-piv.txt`: after the
   fix, `--revert` took the board from 45 masked units to 9, with **0** of ours remaining.

2. **`init.scope` confinement did not survive a reboot, and could not.** `init.scope` is a
   *transient* unit (`systemctl show init.scope -p Transient` → `yes`, `FragmentPath` empty).
   Its drop-in directory is `/run/systemd/transient/init.scope.d/`, created by
   `set-property --runtime`; a drop-in written to `/etc/systemd/system/init.scope.d/` is
   silently ignored for it, and `/run` is tmpfs. Verified directly: with the drop-in in place
   and the runtime property cleared, `cpuset.cpus.effective` fell straight back to `0-3`.
   tib's first post-reboot check showed exactly this decay. The fix is a boot oneshot,
   `namchain-host-slim-initconfine.service`, ordered `After=systemd-sysctl.service
   Before=sysinit.target`, whose `ExecStartPost` asserts the read-back so a failure to confine
   shows up as a failed unit rather than as silent decay. `machine.slice` is *not* transient,
   so it now gets a drop-in like the other slices.

Both fixes are in the same revision, and **tib's confinement was re-verified across a real
reboot**: the transcript at `boot id c82e4bc5` (uptime 4 minutes, i.e. after the reboot, not
before) shows `init.scope cpuset.cpus.effective 0-1` with the unit `enabled`, `active` and
`result=success`. piv was not rebooted — it is the workstation running this session — so its
row below is a live-state check only, and it is the one row that has *not* been shown to
survive a reboot.

**Verified state, 2026-09-17** (transcripts: `bench-results/host-slim-state-piv.txt`,
`bench-results/host-slim-state-tib.txt`; both run tool md5 `fb5a335d…`):

| | piv | tib |
|---|---|---|
| cpuset read-back | `system.slice`/`user.slice`/`machine.slice`/`init.scope` → **0-1** | `system.slice`/`user.slice`/`init.scope` → **0-1** (`machine.slice` absent) |
| across a reboot | **not tested** (host of this session) | **yes** — boot id `c82e4bc5`, init.scope still `0-1`, unit `result=success` |
| audio IRQ after confinement | `xhci-hcd:usb2` (IRQ 134) still `smp_affinity_list=2`, thread at rtprio 85 — **the pin survives**, because the thread is in the root cgroup that none of the drop-ins touch | n/a (EHCI) |
| `arch_timer` 10 s delta, audio cores | **cpu2 = 1, cpu3 = 0** | cpu2 ≈ 100–440, cpu3 ≈ 250 — expected, tib has no `NO_HZ_FULL` |
| masked services | 32, all recorded in the manifest | 12, all recorded |
| `--revert` round trip | 45 → 9 masked (all 32 of ours unmasked), 8 files removed, cpusets back to `0-3`, then re-applied | — |
| essentials after apply | sshd, systemd-resolved, NetworkManager, dbus, journald all active; `timedatectl` still synchronised | sshd reachable; engine still runs |

Two notes on the piv result. First, the `arch_timer` collapse is the whole point: it is the
same measurement as §7a's screen, now produced by the persistent configuration rather than a
transient one. Second, `systemctl --failed` on piv shows `libvirtd`, `logrotate` and
`pg-backup` — all three predate this work (09:17, 00:32 and 00:00 respectively), and
`docker.service` was stopped before `--apply`; none of it is attributable to the script. Check
that distinction before blaming a change. Also note that `journald` is `Storage=volatile`, so
the boot oneshot's own log lines may be gone by the time anyone looks — the durable evidence is
the unit's `result` and the cgroup read-back, not the journal.

`host-slim.sh` is **currently applied on both boards** — piv via
`/home/rik/code.local/namchain/tools/soak/host-slim.sh`, tib via
`/home/rik/namchain/tools/soak/host-slim.sh`. To re-record the evidence:
`tools/soak/host-slim-capture.sh piv` (or `tib`). To undo:
`sudo tools/soak/host-slim.sh --revert`.

The kernel half stays where it already is — `rtpi` for piv and tib, `docs/tuning.md` for
what a *user* should do. This document and its script are the third layer: what to do to the
distribution between the kernel and the engine.
