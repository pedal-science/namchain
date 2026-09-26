# namchain

A real-time engine that runs Neural Amp Modeler chains. `namchaind` renders a rig document
(up to eight chains) on a dedicated real-time thread, drives ALSA directly (`alsa_raw`), and
serves a control protocol that ljoss and `namchainctl` talk to. It is a generic engine: the
pedal's rig library, resume and footswitch mapping live in the pedal repo, not here.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build            # or ./build/namchain-tests directly (doctest)
```

Cross builds, from piv (it has the cross compilers and qemu-user):

| Target | Toolchain file |
|---|---|
| aarch64 | `cmake/toolchains/aarch64-linux-gnu.cmake` |
| armv7, generic (VFPv3-D16, no NEON) | `cmake/toolchains/armv7-linux-gnueabihf.cmake` |
| armv7 with NEON: `tib` | `cmake/toolchains/armv7-neon-linux-gnueabihf.cmake` |

Run cross-built tests with `qemu-arm-static -cpu cortex-a15 -L /usr/arm-linux-gnueabihf` (or
`qemu-aarch64-static -L /usr/aarch64-linux-gnu`). `.github/workflows/ci.yml` is the reference.

## Constraints

- **Nothing on the render path allocates, locks or blocks.** `NAMCHAIN_RT_CHECK` interposes the
  allocator to catch it, and there are RTSan and TSan builds (`NAMCHAIN_RTSAN`, `NAMCHAIN_TSAN`).
- **The vendored NAM core is patched** from `vendor/patches/` at configure time. Change the
  patches, not the vendored files.
- **`nc.cab.ir` convolves with the core's `nam::Linear`**, and resamples with the AudioDSPTools the
  core vendors (sdatkinson's). rikkus's AudioDSPTools fork is deprecated; don't bring it back.
- **The A2 planar parity test only means something in Release.** At -O0 it self-skips;
  `-DNAMCHAIN_REQUIRE_A2_PARITY=ON` turns the skip into a failure.
- **`tib` is armv7 with no compiler.** Build for it on piv with the NEON toolchain file.
- Numbers measured on a cloud runner or under qemu are not performance numbers. Those come from
  piv and `tib` (`bench-results/`).

## Where things are

- `plan.md` is the engine's own plan and history. It is long, so start at "How to resume".
- `docs/`: `daemon.md` (run it, the protocol), `schema.md`, `rig-pipeline.md`,
  `transitions.md`, `assets.md`, `macros.md`, `tuning.md`.
- The CoreAudio and loop backends are on the `local-engine` branch, not `main`.

System context (the pedal, how the repos fit together, the plan): `~/code.local/pedal.project`, starting at its `CLAUDE.md` and `docs/working-across-repos.md`.
