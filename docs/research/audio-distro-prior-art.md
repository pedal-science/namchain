# What audio-optimised Linux distributions actually change on top of their base

**Method note.** Where possible I extracted the *shipping* configuration files rather than
trusting documentation: Ubuntu Studio from the `ubuntustudio-default-settings` source tarballs
on `archive.ubuntu.com`, Arch's `realtime-privileges` from the Arch package mirror. Everything
quoted is what actually ships.

## 1. AudioLinux (Arch-based)

AudioLinux states plainly that it "is not a separate Linux distribution but only a customized
Archlinux image for audio"
([audio-linux.com](https://www.audio-linux.com/)). Everything below is additive config on Arch.

**Kernel.** Custom `linux-audiolinux-rt` (RT-patched, CLANG/LTO-full, 2000 Hz tick). Menu-selectable
alternatives include Audiolinux RT, AMD RT (graysky patch, ALSA base rates to 1536 kHz, aloop patch,
NUMA disabled, tickrate 1000), RT BFQ, RT LTS and XANMOD RT
([headless release notes](https://www.audio-linux.com/html/headless.html)). V2 runs on any CPU,
V3 needs AVX2 (x86-64-v3), V4 needs AVX-512 (x86-64-v4) — so V3/V4 are hard-blocked on x86 only.

**Boot-to-RAM.** "With ramroot option you can load the entire system to RAM for a record
reactivity… you can run a system without drives" ([audio-linux.com](https://www.audio-linux.com/)).
Ramroot v2 and lzo support in initramfs were added in early releases.

**CPU isolation and realtime priority.** v7.0.0 replaced polling daemons with permanent mechanisms:
"NEW isolated cores framework using systemd slices … You must have a kernel with cgroup v2 enabled"
and "NEW realtime priority framework using systemd CPUSchedulingPriority"; the old `rtapp`
check-every-minute service is gone ([release notes](https://www.audio-linux.com/html/headless.html)).
Isolation state lives in `/opt/configuration/isolated.conf`. The menu flow asks for the core set
(e.g. `2,3`), the service to confine (`diretta_alsa_target`), and auto-pins matching NIC IRQs
(`end0`) to the same cores. Extra kernel cmdline parameters go in
`/boot/loader/entries/audiolinux.conf`; hyper-threading is disabled when cores are isolated; boot
modes "standard/extreme/extreme2" drive C-states. sysctl sets `kernel.sched_rt_runtime_us = -1`,
i.e. RT throttling off ([release notes](https://www.audio-linux.com/html/headless.html)).

**Proprietary tooling.** RTPRIORITY assigns rtirqs/rtapp/rtnet priorities; PPower selects the
governor driver (acpi, intel_pstate, amd_pstate, amd_pstate_epp) ([audio-linux.com](https://www.audio-linux.com/)).

**Service/feature removal.** Journal disabling was offered but repeatedly flagged as able to
"break systemd and D-Bus with last systemd and kernel versions"; the wetty web console was
disabled; v2.1.0 "Removed all audio applications installed… reducing the size of the image"
([release notes](https://www.audio-linux.com/html/headless.html)). "Purist mode" is a *user*
script, not a distro feature: it stops `chronyd`, disables DNS lookups and installs a blackhole
default route ([Diretta.md](https://github.com/dsnyder0pc/rpi-for-roon/blob/main/Diretta.md)).

**Numbers.** AudioLinux claims "record CPU latency that can arrive at less than 1 microsecond
average on some systems" and "a performance uplift from 10% to 36%"
([audio-linux.com](https://www.audio-linux.com/)). Neither cites a benchmark, hardware or method —
both are **marketing**. Its community measures with `cyclictest` and `hackbench` and reports values
in microseconds ([nexthardware forum](https://www.nexthardware.com/forum/showthread.php?t=85668&page=10)),
but I found no published reproducible latency table. The official setup guide is only in the
purchase email, so it could not be checked directly; the website's "mini guide" is marked
work-in-progress and is a screenshot gallery.

## 2. Ubuntu Studio — far smaller than assumed

Extracted from `ubuntustudio-default-settings` ([Launchpad/packages.ubuntu.com](https://packages.ubuntu.com/noble/all/ubuntustudio-performance-tweaks/filelist)).

**24.04 `ubuntustudio-performance-tweaks` contains exactly two files:**
`/etc/sysctl.d/50-ubuntustudio.conf` and `/lib/udev/rules.d/40-timer-permissions.rules`.
The sysctl file sets only `vm.swappiness = 10`; `fs.inotify.max_user_watches` and
`dev.hpet.max-user-freq=3072` are present but commented out. The udev rule sets `GROUP="audio"`
on `rtc0` and `hpet`. **No governor, no cpufreq, no rtirq, no limits.conf.**

**Realtime limits come from a different package.** 24.04's `ubuntustudio-default-settings.postinst`
copies `/usr/share/ubuntustudio/audio.conf` → `/etc/security/limits.d/audio.conf`, containing
`@audio - rtprio 95` and `@audio - memlock unlimited`.

**25.10/26.04 moved and extended this.** `ubuntustudio-performance-tweaks` now ships
`/usr/share/ubuntustudio/30-ubuntustudio-audio.conf`, copied by postinst to
`/etc/security/limits.d/30-ubuntustudio-audio.conf`, adding `@pipewire - rtprio 95` and
`@pipewire - memlock unlimited`, and renames the old `audio.conf` to `audio.conf.old.disabled`.
It adds `/usr/lib/udev/rules.d/99-cpu-dma-latency.rules`
(`DEVPATH=="/devices/virtual/misc/cpu_dma_latency", OWNER="root", GROUP="audio", MODE="0660"`)
and `41-ubuntustudio-timer-permissions.rules`.

**`ubuntustudio-lowlatency-settings` ships one file**, `/etc/default/grub.d/ubuntustudio.cfg`:
24.04 = `GRUB_CMDLINE_LINUX_DEFAULT="$GRUB_CMDLINE_LINUX_DEFAULT threadirqs"` plus
`GRUB_FLAVOUR_ORDER="lowlatency $GRUB_FLAVOUR_ORDER"`; 26.04 =
`… preempt=full threadirqs rcu_nocbs=all`. Its postinst also writes a `refind_linux.conf`
"lowlatency options" entry.

**Since 24.10** Ubuntu Studio dropped the dedicated lowlatency kernel and uses the **generic**
kernel with `preempt=full` + `threadirqs`, managed by the Ubuntu Studio Audio Configuration
utility; Studio Controls (the governor/RT-admin GUI) "has halted development"
([ubuntustudio.org](https://ubuntustudio.org/audio-configuration/)).

Net delta for RT audio: swappiness + `threadirqs` (`preempt=full`, `rcu_nocbs=all` from 25.10)
+ rtc/hpet/cpu_dma_latency group permissions + PAM limits + kernel-flavour ordering.

## 3. Arch Linux and Gentoo

[ArchWiki Professional audio](https://wiki.archlinux.org/title/Professional_audio): a vanilla Arch
kernel (`CONFIG_PREEMPT=y`) "is sufficient for low latency operation in most use cases";
`linux-rt`/`linux-rt-lts` only for hard realtime. `threadirqs` is "enforced by default by the
realtime kernel". Other concrete items: performance governor; `realtime-privileges` + `realtime`
group; `noatime`; `echo 2048 > /sys/class/rtc/rtc0/max_user_freq` and
`/proc/sys/dev/hpet/max-user-freq`; `vm.swappiness = 10` in `/etc/sysctl.d/90-swappiness.conf`;
`fs.inotify.max_user_watches = 600000`; `setpci … latency_timer=b0`/`ff`; do not run `irqbalance`;
use `rtirq` for IRQ-thread priorities. The page notes RT support was merged into Linux 6.12 and is
flagged for expansion.

[realtime-privileges](https://archlinux.org/packages/extra/any/realtime-privileges/files/) exact
contents: `99-realtime-privileges.conf` = `@realtime - rtprio 98`, `memlock unlimited`, `nice -11`;
`sysusers.d/realtime-privileges.conf` creating group `realtime`; `40-realtime-privileges.rules` =
`KERNEL=="cpu_dma_latency", GROUP="realtime"`. It is an optional dependency of jack2 and pipewire
([package page](https://archlinux.org/packages/extra/any/realtime-privileges/)).

[udev-rtirq](https://github.com/jhernberg/udev-rtirq): a udev rule that on soundcard add/remove finds
the threaded IRQ handler and sets priority 95 (back to 50 on removal); requires a realtime kernel or
`threadirqs`, and depends on systemd.

Gentoo [proaudio-gentoo](https://github.com/domichel/proaudio-gentoo): RT patch no longer needed
since the mainline merge; standard config is "Preemptible Kernel (Low-Latency Desktop)", full RT for
lowest latency; notes PREEMPT_RT and `RT_CGROUP_SCHED` are mutually exclusive.

## 4. Other distributions and projects

- **AV Linux MX Edition**: MX Linux 25 / Debian Trixie base, "Low latency preempt Liquorix kernel
  with threaded IRQs", MX "ahs" repo ([bandshed.net](https://www.bandshed.net/avlinux/)).
- **KXStudio**: no longer a distribution — repositories only, and the
  [extras repo is deprecated](https://kx.studio/Repositories:Extras). Its Cadence GUI exposed
  governor selection and audio-group/RT checks ([Cadence manual](https://kx.studio/Documentation:Manual:cadence_introduction)).
- **piCorePlayer**: piCore Linux (Tiny Core) — "only about 12 MB", "running entirely in RAM, it
  doesn't write to the SD-card unless you want to save a new setting"
  ([picoreplayer.org](https://www.picoreplayer.org/)). This is the genuine strip-a-distro case.
- **moOde**: the opposite — a pi-gen build of Raspberry Pi OS **Lite** that *adds* packages from its
  own pkgbuild repo and `apt-mark hold`s pinned versions
  ([imgbuild](https://github.com/moode-player/imgbuild)).
- **Volumio 4**: rebuilt on Debian Bookworm for Pi and PC; Pi 1/Zero dropped
  ([volumio.com](https://volumio.com/volumio-4-os/)).
- **[rtcqs](https://codeberg.org/rtcqs/rtcqs)** (formerly realtimeconfigquickscan): checks root,
  audio group, CPU frequency, high-res timers, Preempt RT, rtprio, swappiness, filesystem, plus SMT,
  Spectre/Meltdown mitigations, basic IRQ and power management.

## Portable to Armbian/Ubuntu on ARM: yes/no + why

- **PAM realtime limits** (`limits.d`, `rtprio`/`memlock`): **yes** — file is architecture-independent.
- **`realtime-privileges` package**: **no** (Arch-only), but the three files copy over: `sysusers.d`
  and `udev` rules work on any systemd distro.
- **`cpu_dma_latency` / `rtc0` / `hpet` group udev rules**: **yes**, but many ARM SBCs have no `hpet`,
  and the group name must match (Arch uses `realtime`, Ubuntu uses `audio`).
- **`threadirqs` / `preempt=full` / `rcu_nocbs=all`**: **partly** — arm64 supports all three, but
  `preempt=full` needs `CONFIG_PREEMPT_DYNAMIC`; Armbian vendor kernels often lack it.
- **`linux-rt` / Liquorix / `linux-audiolinux-rt`**: **no** — x86-centric; on ARM you must build or
  find an Armbian `-rt` variant.
- **Performance governor**: **yes**, via `/sys/devices/system/cpu/*/cpufreq/scaling_governor`, but
  many SBCs expose only `ondemand`/`schedutil` or lack cpufreq entirely.
- **CPU isolation (AudioLinux's systemd-slice + `CPUSchedulingPriority` model)**: **yes** — this is
  the single most portable AudioLinux technique; plain `CPUAffinity=` /
  `CPUSchedulingPolicy=` / `CPUSchedulingPriority=` unit properties need only cgroup v2 and systemd.
- **`rtirq`/`udev-rtirq`**: **yes with `threadirqs`**, but IRQ threading and affinity control vary
  widely on ARM; `isolcpus` + `/proc/irq/*/smp_affinity` is the practical route.
- **`sched_rt_runtime_us=-1`**: **yes**, portable, but removes the RT throttling safety net.
- **`vm.swappiness=10`, inotify watches, `noatime`, journald `Storage=volatile`**: **yes**, fully portable.
- **Boot-to-RAM (ramroot / piCorePlayer model)**: **no for a full Ubuntu/Armbian desktop** — needs a
  custom initramfs and enough RAM; feasible only for a minimal audio-only image.
- **AudioLinux menus, RTPRIORITY, PPower**: **no** — proprietary and Arch-specific.
