# Minimal Linux userspace for latency-sensitive audio on ARM SBCs

Scope: how to strip or replace Debian/Ubuntu/Armbian for a headless audio node, with notes for an RK3288 (ARMv7, 2 GB) and Raspberry Pi 5 / BCM2712 (ARMv8). Uncertain items are labelled.

## 1. Armbian: minimal images and the build framework

Armbian builds three flavours from one framework: `BUILD_MINIMAL=yes` (bare CLI, "only the essential packages", no desktop and none of the extra server utilities), the default standard CLI/server image (both switches unset), and `BUILD_DESKTOP=yes` with `DESKTOP_ENVIRONMENT=xfce|gnome|kde-plasma|…` and `DESKTOP_TIER=minimal|mid|full` ([image-type switches](https://docs.armbian.com/build-framework/switches/image-type/)). `BUILD_MINIMAL` is explicitly incompatible with `BUILD_DESKTOP=yes`.

Switches that matter for a stripped audio node ([contents](https://docs.armbian.com/build-framework/switches/image-contents/), [host](https://docs.armbian.com/build-framework/switches/host-docker/)):

- `EXPERT=yes` unlocks development boards/branches/releases in the interactive menu; it is a visibility flag, not a minimality flag.
- `PREFER_DOCKER` (default `yes`) is the only `PREFER_*` switch documented; there is no `PREFER_*` package-selection switch. I found **no `CORE` build switch** in current Armbian docs — treat "CORE" as uncertain/legacy. Trimming is done via `BUILD_MINIMAL` plus the switches below.
- `NETWORKING_STACK`: with `BUILD_MINIMAL=yes` it defaults to `systemd-networkd` + `systemd-timesyncd`; otherwise `network-manager` + `chrony`; `none` skips networking extensions.
- `INSTALL_ARMBIAN_FIRMWARE=no` (default `yes`), `BOARD_FIRMWARE_INSTALL=-full`, `INSTALL_HEADERS=yes`, `EXTRAWIFI=no` (default `yes`), `BSPFREEZE=yes` (apt-hold kernel/U-Boot/DTB), `SKIP_ARMBIAN_REPO=yes`, `ENABLE_EXTENSIONS=…`.

`armbian-config` is the on-device config/app tool and the kernel switcher. Critically, it lists only Armbian's own kernels; the forum admin explains that per-board kernels/DTBs make a generic distro kernel useless on most SBCs ([Armbian forum](https://forum.armbian.com/topic/28657-installing-linux-image-rt-arm64-from-repositories/)).

**RT status:** Armbian does **not** ship PREEMPT_RT images or `linux-rt` packages across its matrix. `apt install linux-image-rt-arm64` installs but does not boot (same per-board reason). Armbian does carry RT kernel config work — e.g. `linux-k3-beagle-current-rt` ([PR #8395](https://github.com/armbian/build/pull/8395)) and a `ti` `current` RT config ([PR #8280](https://github.com/armbian/build/pull/8280)) — and has had bugs applying the `preempt_rt` patch ([issue #6553](https://github.com/armbian/build/issues/6553)). Realistic RT path: `./compile.sh KERNEL_CONFIGURE=yes` and add `userpatches` kernel patches/fragments.

## 2. Ubuntu minimal / cloud / ubuntu-base

The leanest official Ubuntu rootfs is the `ubuntu-base` tarball: `ubuntu-base-24.04.5-base-arm64.tar.gz` ≈29 MB, `…-armhf.tar.gz` ≈26 MB ([cdimage listing](https://cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/)). Typical flow: extract into a partition, bind-mount `/dev /proc /sys`, copy `qemu-user-static` in (binfmt), `chroot`, create a user, `apt update`, install `linux-image-*`, `linux-headers-*`, `u-boot`/bootloader files for your board, `firmware-*`, then flash. Debian equivalents: `mmdebstrap --variant=minbase` or `debootstrap --variant=minbase` plus `apt-get --no-install-recommends` (for example `mmdebstrap --variant=minbase --include=linux-image-rt-arm64 noble rootfs.tar`).

For managed/immutable deployments, Ubuntu Core images are produced with `ubuntu-image` + `snapcraft` ([docs](https://documentation.ubuntu.com/core/how-to-guides/image-creation/use-ubuntu-image/index.html)); RT is obtained by rebuilding the kernel **snap** with the PREEMPT_RT patch ([snapcraft forum](https://forum.snapcraft.io/t/building-an-ubuntu-core-realtime-kernel-snap/26225)).

Canonical's supported easy path is Ubuntu Pro: `pro attach` then `pro enable realtime-kernel` (arm64/amd64, v6.8-based), or `pro enable realtime-kernel --variant=raspi` for Raspberry Pi 4/5 ([Canonical](https://ubuntu.com/blog/real-time-24-04)). A `linux-raspi-realtime` package exists ([Launchpad](https://launchpad.net/ubuntu/resolute/+package/linux-raspi-realtime)); `linux-lowlatency` is the softer non-RT option ([Ubuntu package](https://packages.ubuntu.com/noble/linux-lowlatency)).

What must stay: `systemd` (PID 1, journald, udev), `dbus`, `apt`/`dpkg` + `gpg` and their deps, `libc`/`libstdc++`, the kernel + matching modules + firmware. Removing `systemd-resolved` without a replacement resolver breaks DNS.

## 3. Buildroot and Yocto/OpenEmbedded

Buildroot produces a complete cross-compiled system and, by design, **no binary packages and no compiler on target** ([manual](https://buildroot.org/downloads/manual/manual.html)). RT support in current master is not a dedicated `BR2_LINUX_KERNEL_PREEMPT_RT` bool; instead you pick `BR2_LINUX_KERNEL_LATEST_CIP_RT_VERSION` (5.10.254-cip72-rt32) or apply RT via `BR2_LINUX_KERNEL_PATCH` / a `BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES` containing `CONFIG_PREEMPT_RT=y` ([linux/Config.in](https://github.com/buildroot/buildroot/blob/master/linux/Config.in)). Because PREEMPT_RT became mainline in 6.12 ([overview](https://kernel-internals.org/locking/preempt-rt/)), a ≥6.12 kernel needs only the config symbol. Audio stack: ALSA lib/utils, `BR2_PACKAGE_JACK2` (legacy `jackd` or D-Bus `jackdbus`; selects `alsa-lib` seq/rawmidi/hwdep — [Config.in](https://github.com/buildroot/buildroot/blob/master/package/jack2/Config.in)), `BR2_PACKAGE_PIPEWIRE` ([Config.in](https://github.com/buildroot/buildroot/blob/master/package/pipewire/Config.in)), and `BR2_PACKAGE_RT_TESTS` (cyclictest et al., needs NPTL + headers ≥4.5 — [Config.in](https://github.com/buildroot/buildroot/blob/master/package/rt-tests/Config.in)).

Yocto: the [`meta-realtime`](https://layers.openembedded.org/layerindex/branch/master/layer/meta-realtime/) layer (on `openembedded-core` + `meta-oe`) provides `linux-yocto-rt`, `rt-app`, `latencytop`, `schedtool-dl` and `core-image-rt-extended`. Tradeoff vs. stripping a distro: total control and a tiny image, but you cross-compile your whole app stack, lose `apt`, and security updates mean a rebuild+reflash (Buildroot has CVE tooling via `make pkg-stats`).

## 4. Existing RT/audio-oriented projects

- **Elk Audio OS** — base: custom **Yocto/OpenEmbedded** distro; kernel: Linux + **EVL Core** (Xenomai project) dual-kernel; custom audio/sensor drivers instead of ALSA, Sushi plugin host, Raspa/Twine libs; runs on BCM2711 (Pi 4), i.MX8M, Intel ([architecture](https://www.elk.audio/architecture), [meta-raspberrypi-elk](https://github.com/elk-audio/meta-raspberrypi-elk), [rpi-evl-audio-driver](https://github.com/elk-audio/rpi-evl-audio-driver)).
- **Bela** — base: BeagleBone Black; kernel: **Xenomai** real-time extensions; uses the BeagleBone **PRU** for audio/sensor I/O ([Bela docs](https://docs.bela.io/)).
- **Zynthian** — base: Raspberry Pi OS (now Pi 5); kernel: stock preempt first, optional PREEMPT_RT (`4.9.47-rt37-v7+` tested historically); JACK with `-P 70 -p 128 -n 2`; built with CustomPiOS ([discourse thread](https://discourse.zynthian.org/t/realtime-kernel-for-raspi/1063), [wiki](https://wiki.zynthian.org/index.php/Building_a_custom_Zynthian)).
- **Patchbox OS** (Blokas) — base: **Raspbian Lite** + `4.19.71-rt24` RT kernel + JACK; module manager; targeted at Pisound ([Blokas](https://blokas.io/patchbox-os/), [build docs](https://raw.githubusercontent.com/wiki/BlokasLabs/patchbox-os-gen/docs/index.md)).
- **MOD Devices MOD Duo** — embedded Linux running `3.4.104-rt139-modduo … PREEMPT RT` with `mod-ui`/LV2, verbatim from a MOD Duo `uname` ([Zynthian thread](https://discourse.zynthian.org/t/realtime-kernel-for-raspi/1063)).
- **Sonic Pi** — not an OS; an application that runs on Raspberry Pi OS / desktop Linux (ALSA, optional PipeWire via a [PR](https://github.com/sonic-pi-net/sonic-pi/pull/3347)).

## 5. Package-level slimming (Debian/Ubuntu)

Start from `--variant=minbase` + `--no-install-recommends`, or Armbian `BUILD_MINIMAL=yes`; then audit (`apt-mark showmanual`, `dpkg-query -W -f='${Installed-Size}\t${Package}\n' | sort -rn`) and purge headless-irrelevant packages. Commonly reported removals: `snapd`, `cloud-init`, `modemmanager`, `avahi-daemon`, `cups` (+ `cups-browsed`), `bluez`/`bluetooth`, `plymouth`, `apparmor`, `unattended-upgrades`, printer/scan stacks, X11/GNOME packages, compilers. On SBCs, mask rather than remove where a dependency may exist: `systemctl mask avahi-daemon bluetooth ModemManager cups`. This audit/removal workflow is documented in [this minimal-Ubuntu guide](https://raw.githubusercontent.com/OneUptime/blog/refs/heads/master/posts/2026-03-02-how-to-remove-unnecessary-software-for-a-minimal-ubuntu-installation/README.md). Do **not** remove `systemd`, `dbus`, `apt`/`dpkg`, `libc`, `udev`, or the kernel/firmware packages; keep `alsa-lib`/`alsa-utils`, `jackd`/`pipewire`, `rt-tests`. Armbian's minimal rootfs is already close, so most users only add their audio stack.

## 6. Kernel + userspace matching, RT flavours, firmware

Kernel modules must match the exact running kernel, so `linux-headers-$(uname -r)`/DKMS is required for out-of-tree audio drivers. Debian ships `linux-image-rt-arm64` for arm64 and `linux-image-rt-armmp` for ARMv7 multiplatform ([arm64](https://packages.debian.org/sid/arm64/linux-image-rt-arm64), [armmp](https://packages.debian.org/bullseye/linux-image-5.10.0-30-rt-armmp)). Ubuntu offers `linux-lowlatency` (not hard RT) and Pro `realtime-kernel` with the `raspi` variant; the `raspi-realtime` kernel is tracked as a [Launchpad bug](https://lists.snapcraft.io/archives/foundations-bugs/2024-October/526496.html). Community Pi RT: [remusmp/rpi-rt-kernel](https://github.com/remusmp/rpi-rt-kernel) builds a PREEMPT_RT image for Pi 3/4/5 (`./scripts/config --enable PREEMPT_RT`, verified with `uname -a` showing `PREEMPT_RT`). Firmware/DTB must match the board (Armbian `armbian-firmware`, Raspberry Pi `dtoverlay=`); on Pi 5 a HifiBerry HAT needed `dtoverlay=hifiberry-dacplusadcpro`, `force_eeprom_read=0`, and disabling HDMI audio. RT tuning beyond the kernel: `preempt=full`, threaded IRQs, CPU isolation (`isolcpus`/cgroups), `cpufreq` governor `performance`, and `cyclictest` from `rt-tests`.

## Fit for the two target boards

- **RK3288 (ARMv7, 2 GB):** Armbian supports RK3288 boards (Tinker Board/MiQi class) on its 32-bit rockchip `current`/`edge` kernel ([PR #8649](https://github.com/armbian/build/pull/8649)). Use Armbian minimal or Debian `--variant=minbase` armhf. There is **no Armbian RT kernel** for it; either build `CONFIG_PREEMPT_RT=y` from Armbian's rockchip source, or (uncertain) try Debian's generic `linux-image-rt-armmp` with a correct DTB/U-Boot. 2 GB is ample for a JACK/ALSA node; isolated-core tuning is limited (4 cores).
- **Raspberry Pi 5 / BCM2712 (ARMv8):** best-supported path — Raspberry Pi OS Lite (already minimal) + Ubuntu Pro `realtime-kernel --variant=raspi`, or the community `rpi-rt-kernel` (Pi 5 tested). Armbian has no official Pi 5 support (community only — uncertain). Use 64-bit; 4–8 cores allow real `isolcpus` for the audio thread.

## Comparison

| Approach | How minimal | Effort | apt/updates | RT support | Fit for ARM SBC audio |
|---|---|---|---|---|---|
| Armbian `BUILD_MINIMAL=yes` | Very minimal CLI; switch off firmware/WiFi/headers | Low–medium (learn switches) | Yes, apt + Armbian repo (`SKIP_ARMBIAN_REPO` optional) | Build-it-yourself; no shipped RT | **High** — per-board kernel/DTB already solved |
| Ubuntu/Debian `mmdebstrap --variant=minbase` / `ubuntu-base` | Smallest distro base (~26–29 MB tarball + deps) | Medium–high (bootloader, kernel, DTB, chroot) | Yes, full apt | Debian `linux-image-rt-arm64`/`-rt-armmp` | Medium — best on well-maintained mainline boards |
| Ubuntu Core + RT kernel snap | Immutable, small, no apt on target | High (snapcraft + kernel snap) | Transactional snaps | Yes, custom PREEMPT_RT snap | Medium — good for fleets, steep learning curve |
| Buildroot | Smallest possible (~MBs, BusyBox, no package manager) | High (cross-compile everything, no apt) | None — rebuild + reflash | CIP-RT or `CONFIG_PREEMPT_RT=y` fragment | Medium — great once stable, poor iterability |
| Yocto/OpenEmbedded + `meta-realtime` | Tiny and reproducible | Very high | None (package feeds optional) | `linux-yocto-rt`, `meta-realtime` | Medium–high (Elk proves it) but heavy |
| Elk Audio OS | Purpose-built Yocto audio OS | Low if supported board | Vendor release | EVL/Xenomai hard RT | High on supported boards (Pi 4, i.MX8) |
| Patchbox OS / Zynthian | Raspbian/RPi OS Lite + audio stack | Very low | apt | Bundled `-rt` kernel | High for Pi, not portable to RK3288 |

**Bottom line:** on RK3288 the pragmatic route is Armbian minimal or `minbase` + a self-built `CONFIG_PREEMPT_RT=y` rockchip kernel; on Pi 5, Raspberry Pi OS Lite + `pro enable realtime-kernel --variant=raspi` (or `rpi-rt-kernel`) is by far the least effort. Buildroot/Yocto only pay off when you must own the whole stack.
