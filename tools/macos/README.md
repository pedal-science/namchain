# namchain on a Mac

The engine runs on a Linux single-board computer. This directory is about the other machine: a
laptop with no audio interface attached, where you want to drive the engine from a control client
and hear what it does.

There is no hardware in this picture. The input is a WAV on a loop, and there are two ways to hear it:

- **`--backend coreaudio`** (preferred): one process. The loop is read inside the device's own IO
  callback and the engine's output goes straight to a CoreAudio device. One clock, no external
  player, and `engine.status` reports the device's real period.
- **`--backend pipe`**: the looped WAV is written to a FIFO as raw floats and `listen.sh` plays it.
  Useful when the thing you want to hear it through is not a CoreAudio device, or when you want the
  engine's output to be a stream another tool can read.

Both answer the two questions a front end needs answered — *does the engine pass a signal through
unchanged, and does it hear what I send it* — without a guitar, an interface, or the board.

## What builds here

`namchaind` and `namchainctl` build on macOS. The four Linux spellings the control transport used go
through `include/namchain/daemon/platform.h`:

| Linux | macOS |
| --- | --- |
| `signalfd` | a `sigwait` thread writing the signal number into a pipe |
| `accept4` | `accept` plus two `fcntl`s |
| `SOCK_CLOEXEC \| SOCK_NONBLOCK` in `socket()` | the same two `fcntl`s |
| a `SOCK_SEQPACKET` local listener | a `SOCK_STREAM` one, carrying the length prefix |

The last is the only one that changes anything observable, and it changes less than it looks:
`framing.h` already says the framed form is byte-identical to what the SEQPACKET transport carries,
so the two were always the same bytes in different envelopes. `PF_LOCAL` has no `SOCK_SEQPACKET` on
macOS, so there the local socket uses the envelope that survives everywhere.

`alsa_raw` stays Linux-only and asking for it here is refused by name rather than quietly given
something else. `coreaudio` is the reverse: `src/io/coreaudio.cpp` compiles to an empty translation
unit anywhere but macOS, and `docs/daemon.md` has the details of what it does and does not promise.

## Running it

With the device backend — no FIFO, no listener, and the daemon can start before anything is playing:

```sh
cmake -S . -B build -G Ninja && cmake --build build
ffmpeg -i your-guitar-di.wav -c:a pcm_f32le -ar 48000 -ac 1 /tmp/nc/input-f32.wav

./build/namchaind --backend coreaudio \
  --capture-path /tmp/nc/input-f32.wav \
  --period-size 1024 \
  --rigs-root /tmp/nc/rigs --models-root /tmp/nc/models --irs-root /tmp/nc/irs \
  --socket /tmp/nc/d.sock --no-lock-memory --rt-priority 0 \
  --tcp-listen --tcp-address 127.0.0.1 --tcp-port 47810 --tcp-token-file /tmp/nc/token
```

`--period-size 1024` is a ceiling rather than a request: the backend measures the device's own period
and compiles for that. 1024 is above anything a Mac's default output delivers. Omit `--device` for
the default output device, or give a CoreAudio UID to pick one. A control client connects to
`127.0.0.1:47810` and presents the token.

`run-namchaind.sh` is the `pipe` form of the same thing, and it prints its token. Start `listen.sh`
first: the daemon opens the FIFO for writing, which needs a reader, and reports a lost device and
retries until one appears — the same thing it does when an interface is unplugged. Detaching and
reattaching a player works for the same reason.

**Whatever is reading the FIFO is the engine's clock.** The `pipe` backend paces nothing itself; the
write blocks when the pipe is full and the listener's device clock does the rest. That is what keeps
a long session from drifting, and it is also why a `namchain-soak` figure taken against this backend
measures the listener's scheduling rather than the engine's work. Don't publish one. The `coreaudio`
backend has no such caveat — its block time is the engine's work plus the device's buffer, and
`engine.status` says what that buffer is.

## Proving it is clean

```sh
tools/macos/null-test.sh
```

Two rigs that must not change the signal — an empty chain, and a chain whose one block is a gain at
0 dB — rendered over a WAV and compared with the WAV, both through `namchain-render` and through
`namchaind --backend file`. A third at +6 dB is the control, because a null test that cannot fail is
not measuring anything.

`NAMCHAIN_RIGS=DIR` points it at documents written by something else. That is the useful form: aim
it at what a front end's own encoder produced and the test is about that front end's bytes rather
than about JSON that was written to resemble them.

Expect bit-exact. `--max-abs 0` is the default and both null cases meet it — `nc.gain` at 0 dB is
`pow(10, 0/20)`, which is exactly 1.0, and multiplying by it changes no bits.

Two things that are correct and look wrong:

- **A rig sent while the engine is running fades in over `--fade-ms`.** Only a `--rig` given at
  startup is in place from sample 0, which is why the null test uses that form. A few milliseconds
  of ramp at the top of a live `rig.apply` is a structural transition working.
- **The `pipe` output saturates at full scale and the offline render does not.** `pcm_encode` clamps,
  so a device path — and this is one — behaves like a converter. `save_wav_float32` does not, so a
  +6 dB render peaks at 1.97 in the file and at 1.0 in your ears. Both are right for what they are.
