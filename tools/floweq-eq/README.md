# floweq-eq

An offline renderer for the engine's `nc.eq.flow` block — flowEQ-lite — so its EQ can be heard and
measured in isolation. Give it a WAV and the block's parameters; it gives back the EQ'd WAV and the
node's own response report.

It is **not** a second implementation. It builds the same one-node graph the flowEQ tests use:

```
nc.input -> nc.eq.flow -> nc.output
```

compiles it into a `RenderPlan` and executes it a block at a time. The audio therefore comes from
the block's real `create`/`process` path: the decoder, the five biquads, the gain one-pole, the
coefficient crossfade and Auto Gain's BS.1770 loudness meter. The report is the block's
`describe_state`, the same JSON `rig.state` serves to rigpa's panel.

## Build

This lived in a git worktree of `namchain` (branch `floweq-eq-cli`) so the working checkout was
not touched. From the tree containing this directory:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNAMCHAIN_BUILD_TESTS=OFF
cmake --build build --target floweq-eq
```

The target is in `CMakeLists.txt` under `NAMCHAIN_BUILD_TOOLS`. A fresh worktree has no NAM
submodule checked out; either run `git submodule update --init --recursive` there or point
`-DNAMCHAIN_NAM_CORE_DIR` at another checkout's `vendor/NeuralAmpModelerCore` (this worktree's
build uses the main checkout's).

## Use

```sh
floweq-eq --input in.wav --output out.wav [EQ options]
floweq-eq --input in.wav --analyze [EQ options]
floweq-eq --analyze [--rate 48000] [EQ options]
```

EQ options are the block's:

| option | meaning | range / default |
|---|---|---|
| `--x`, `--y`, `--z` | the latent point | −2…2, default 0 |
| `--strength` | scales every band's gain | 0…1, default 1; **0 is the identity filter** |
| `--extend` | doubles the latent values before the decoder | off |
| `--auto-gain` | match output loudness to the input (BS.1770 short-term), clamped ±12 dB | off |
| `--in-db`, `--out-db` | gain before / after the EQ | −60…12 dB, default 0 |
| `--ramp-ms` | coefficient crossfade and gain smoothing time | 0…1000, default 5 |

Other options: `--block <frames>` (default 64, the engine quantum), `--report <path>` (write the
node's `describe_state` plus peak/RMS), `--json` (print that JSON), `--quiet`, `--rate` (for
`--analyze` with no input).

Examples:

```sh
# the block's own default curve
build/floweq-eq --input guitar-di.wav --output default.wav --x 0 --y 0 --z 0

# print the bands and 96-point curve without rendering
build/floweq-eq --analyze --x 0 --y 0 --z 1

# strength 0, for a null test
build/floweq-eq --input guitar-di.wav --output null.wav --strength 0

# a report beside the audio
build/floweq-eq --input guitar-di.wav --output out.wav --x 1 --y 1 --z 1 --report out.json
```

## Notes

- **Mono engine, multichannel files.** Each channel is run through its own fresh node and written
  back with its channel count. That is N independent mono EQs, not a channel-linked one; Auto Gain
  meters each channel separately.
- **32-bit float output** at the input's rate. A boost can push a peak above 0 dBFS; the summary
  reports the peak and warns. Use `--out-db` to trim a listening file.
- **`--auto-gain` puts Out out of the path**, as the block does; `out_db` is ignored while it is on.
- **`--strength 0` is bit-exact.** The block's identity fast path is exercised, not approximated.
- **The latent origin is not flat.** `x=y=z=0` is the decoder's own gentle low-mid lift; the
  identity is `strength 0`. That distinction is usually the surprise in a listening test.
