# namchaind: the reference daemon

**Status:** built and tested, M2 in progress. `namchaind` starts an audio backend, compiles a
**document of up to eight chains** into one plan per chain, runs them on a dedicated real-time
thread, and serves a Unix-socket control protocol. A parameter edit is ramped inside its node with no
recompile, and so is a bypass, crossfaded inside the running plan; a structural change — a reload, a
patch that alters the graph, a model swap — fades only what changed, or crossfades the whole chain, one
chain at a time. Bypass keeps the node running so its state stays warm. Every node's output is guarded for non-finite samples. The daemon reloads on
`SIGHUP` and survives the interface being unplugged. §"What is built, and what is not" is the honest
list.

The pieces:

| file | what it is |
|---|---|
| `tools/daemon/namchaind.cpp` | argument handling and the exit code; everything else is in the library |
| `include/namchain/daemon/{config,daemon}.h`, `src/daemon/{config,daemon}.cpp` | config parsing, rig resolution, the render thread and the control loop |
| `packaging/systemd/namchaind.service` | the unit: rlimits, `Restart=always`, `RuntimeDirectory` |
| `packaging/namchaind.json` | an example config to install at `/etc/namchain/namchaind.json` |
| `tests/test_document.cpp` | the document envelope and node identity, without a daemon |
| `tests/test_daemon.cpp` | config, protocol and end-to-end tests against the `null` and `file` backends |

## Run it

```
cmake --build build --target namchaind
./build/namchaind --help
```

A daemon with no device, for a smoke test:

```
mkdir -p /tmp/nc/rigs
cp build/generated/two-chains.json /tmp/nc/rigs/
./build/namchaind --backend null --no-lock-memory \
  --rigs-root /tmp/nc/rigs --rig two-chains.json \
  --socket /tmp/nc/namchaind.sock --verbose
```

`--backend null` is a paced clock with no hardware: it is the fastest way to see whether the
threads, the plan publication and the socket work before a device is in the picture. The default
backend is `alsa_raw`.

`--backend coreaudio` is the macOS device backend: the engine's output to a CoreAudio output device
and its capture from a live CoreAudio input device or a WAV on a loop, in one process.
§"The CoreAudio backend" below has what it promises.

`--backend file` renders a document offline, start to finish, with the same engine, the same channel
binding and the same summing as the device path — `--capture-path in.wav --playback-path out.wav`,
with `--capture-channels 2 --playback-channels 2` for a two-chain rig. It stops when the input runs
out and writes the output WAV on the way out, which is how the multi-chain tests assert where each
chain's audio landed rather than only that it ran.

The startup sequence is the one plan.md §8.2 asks for: read and compile the rig, prewarm every node
on the control thread, publish the plan, **then** open and start the device. The first block never
pays a node's cold-start cost.

## Configuration

Every field has a long flag and a JSON key of the same name (`--period-size` / `"period_size"`). The
file is read first and the command line overrides it in order, so a unit can carry the appliance
settings and an operator can vary one of them for a run. An unknown key is an error rather than
silently ignored.

| field | default | meaning |
|---|---|---|
| `backend` | `alsa_raw` | `alsa_raw`, `coreaudio`, `null`, `file`, `pipe`, or `pipewire` (refused by name) |
| `device` | *(empty)* | the device to open. On Linux an ALSA spec: `hw:CARD=<id>[,DEV=<d>]`, `hw:<id>,<d>`, `hw:4,0`, `4,0`, or a `/dev/snd` path — prefer an id (the names in `/proc/asound`, e.g. `II` for the SSL 2+ Mk II), because card numbers follow probe order and change across reboots. On macOS the **output** CoreAudio device **UID**, empty asking for the system's default output device |
| `sample_rate` | `48000` | |
| `period_size` | `64` | quantum in frames. On `coreaudio` it is a **ceiling**, not a request: see "The CoreAudio backend" below |
| `periods` | `4` | device buffer length in periods |
| `capture_channels` | `1` | requested; the daemon decodes the channel each chain names. On `coreaudio` with `input_device` this is the width of the capture bus, and the device must offer at least this many input channels |
| `playback_channels` | `1` | requested; each chain writes the channel it names, and the rest is zeroed |
| `capture_path` | *(empty)* | `file`, `pipe` and `coreaudio`: the WAV read as input. `pipe` and `coreaudio` loop it. On `coreaudio` it is the alternative to `input_device`, and giving both is refused |
| `input_device` | *(empty)* | `coreaudio` only: a CoreAudio input device **UID** to capture from live. Empty means the capture comes from `capture_path`. Reading a device is what makes macOS ask for microphone permission, so this happens only when the key is set |
| `device_buffer_frames` | `0` | `coreaudio` only: the device's own IO buffer to ask it for, in frames; 0 leaves it as it is found. This is where most of the monitoring latency lives — 512 frames is 10.67 ms at 48 kHz — and the daemon **puts the previous value back** when it stops |
| `playback_path` | *(empty)* | `file`: the output WAV, written when the input runs out. `pipe`: the FIFO the output is streamed to |
| `rig` | *(empty)* | startup rig; empty starts silent and waits for `rig.load` |
| `rigs_root` | `/etc/namchain/rigs` | the directory a `rig.load` **name** resolves under |
| `models_root` | `/etc/namchain/models` | the device's `.nam` model library; a document names a model by id (docs/assets.md) |
| `irs_root` | `/etc/namchain/irs` | the device's `.nam`/`.wav` impulse responses |
| `socket` | `/run/namchain/namchaind.sock` | Unix `SOCK_SEQPACKET` control socket |
| `socket_mode` | `"0660"` | octal string |
| `rt_priority` | `80` | `SCHED_FIFO` for the render thread; `0` leaves the policy alone |
| `cpu` | `-1` | pin the render thread to one CPU; `-1` leaves the affinity alone. With `render_threads` above 1 this is the first core of the render set |
| `render_threads` | `1` | cores the render path uses: one coordinator plus this many minus one workers; `1` is the single-threaded engine. See "Render threads" below |
| `lock_memory` | `true` | `mlockall(MCL_CURRENT)`, never `MCL_FUTURE` (plan.md §14.10) |
| `output_queue_periods` | `0` | output-ring cap in periods; `0` is off |
| `output_queue_floor_periods` | `0` | queue floor in periods; `0` is off |
| `fade_ms` | `5.0` | equal-power crossfade for a structural plan transition |
| `cost_table` | `/etc/namchain/cost.json` | this device's measured cost table: what each unit costs on one core here. A rig over the table's `headroom` has its dearest models trimmed Full -> Lite, and one that still does not fit is refused. A table that cannot be read leaves every unit at its `unknown` cost, which is the safe direction. See "Cost and admission" below |
| `device_retry_ms` | `250` | backoff between attempts to open a lost device |
| `reclamation_timeout_ms` | `20` | the control `poll` timeout, i.e. the reclamation window |
| `tcp_listen` | `false` | serve control connections on TCP as well |
| `tcp_address` | `127.0.0.1` | TCP address to bind; numeric only (no DNS at startup) |
| `tcp_port` | `47810` | TCP port; `0` lets the kernel choose, and `engine.status` reports it |
| `tcp_max_connections` | `4` | TCP connections open at once, authenticated or not |
| `tcp_max_message_bytes` | `1048576` | largest framed request accepted, checked before the body is read. Replies have their own limit, 16 MiB ("The wire, byte for byte") |
| `tcp_token` | *(empty)* | pre-shared token a TCP client presents in `engine.hello` |
| `tcp_token_file` | *(empty)* | read the token from the first line of this file instead |
| `tcp_auth_timeout_ms` | `10000` | how long a connection may stay unauthenticated; `0` disables |
| `record_dir` | `/var/lib/namchain/recordings` | where `engine.record` writes, one directory per recording; empty disables it |
| `verbose` | `false` | log lifecycle events to stderr |

The daemon refuses a device that does not give the configured quantum and rate exactly
(`require_exact_period`): a plan is compiled for one quantum, and running it at another would read
past every node's buffers. That refusal is a configuration error, not a device failure, and it is
reported with both numbers.

### The CoreAudio backend

`--backend coreaudio` is the other half of `pipe`, for a Mac with no board attached: it plays the
engine's output through `--device` (a CoreAudio UID, or the default output device when empty) and
takes its capture from `--input-device` (a live CoreAudio input) or `--capture-path` (a WAV on a
loop) — **in one process**, with no FIFO and no external player. The engine's render plan is
unchanged: the device's IOProc produces capture periods into a lock-free ring and drains processed
periods from another, so the render thread keeps its `wait_capture → read → DSP → write` loop.

What is worth knowing before quoting a number from it:

- **The device's own period is the engine's quantum, and it is measured, not requested.**
  `kAudioDevicePropertyBufferFrameSize` reads back as whatever was written to it while the callback
  is handed something else entirely. So the backend runs a short probe IOProc at open time and takes
  the *smallest* callback it sees; that number is what `engine.status` reports as `period_size`, and
  on this machine it is 512 while `--period-size 1024` is the configured ceiling.
- **`--period-size` is therefore a ceiling here.** A plan compiled for 1024 carries a device that
  runs at 512: `RenderPlan::execute` takes a frame count and every node's buffers were sized for the
  maximum, so a smaller period runs the plan inside its own limit. A device that wants *more* than
  the plan was built for is refused, because there is genuinely nowhere for those frames to go. This
  is a deliberate departure from §9.2's "quantum = device period, clamped to [32, 256]", and it is
  bounded: the configured value is the plan's buffer size, so nothing overflows.
- **The device's buffer layout decides the indexing, and the two are not the same thing.** This
  machine presents **one interleaved buffer** holding both channels (`mNumberBuffers == 1`,
  `mNumberChannels == 2`), so channel `c` of frame `f` is `mData[f * channels + c]`. The callback
  originally advanced by `period` floats per period and addressed `mBuffers[b]` as though each
  channel had its own buffer. On an interleaved device that writes the source's sample *k* at index
  `2k`: the left channel hears samples 0, 2, 4 …, the right channel is never written at all and stays
  silent, the pitch doubles, and there is a one-sample offset between the channels. Reported as
  "harsh, metallic, digital", and the gating at the device period comes from the buffer never being
  filled past its first half.
- **A channel the engine never wrote is silence, and repeating it is the backend's job.** A document
  with one chain fills output channel 0 and leaves the rest of the bus at zero — correctly, because
  no chain having that channel means silence. A device with more channels than the document uses must
  be sent the *last channel the engine actually wrote*, repeated, rather than that zero. The backend
  remembers which channel that was, from the engine's own bytes.
- **The capture instrument has to read the buffer in the layout the callback wrote it in.** The
  recorder takes planar slices; handed an interleaved buffer as though it were one channel it reads
  both channels as consecutive frames, which duplicates every sample — a 1 kHz tone recorded as
  500 Hz with every value repeated. That misreading accused the engine of a bug for a while before
  the instrument was fixed, which is worth remembering: a measurement that cannot be wrong in the
  direction the bug would take it is not a measurement.
- **A callback is not always one period, and the backend does not assume it is.** A device is free to
  present a larger IO buffer than the engine's quantum: on this project's machine the probe sees 512
  frames per callback and the stream's own IOProc is then handed 1024. So the callback converts frames
  into *periods* — one callback carrying two periods' worth hands the engine two and asks for two back
  — and both directions are quantised to the engine's period. What must agree over a second is the
  frame count the device consumes and the frame count the engine produces, and that is what is
  checked. An equality test on the frame count, which is what this used to do, refuses a device that
  is working perfectly.
- **Frames, not samples.** `mBuffers[0].mDataByteSize` counts every channel: 4096 bytes at 2 channels
  of float is 1024 samples and **512 frames**. Dividing by the sample size alone doubles every figure
  derived from it, and that mistake was made here — it produced a "the device delivers 96,000 frames a
  second" reading that led to a rate check refusing perfectly good hardware. The channel count is read
  from the buffer and is not optional.
- **The live device path can be null-tested, when the build allows it.** An empty chain
  (`nc.input` → `nc.output`) is bit-exact end to end: offline through `--backend file`, through
  `--backend pipe` against the looping source, and through `coreaudio` itself when built with
  `-DNAMCHAIN_COREAUDIO_CAPTURE=ON`, which adds `--capture-device-path FILE` and records what the
  callback hands the device. Measured on the file used for it — 5 whole passes, 960,000 frames, every
  frame compared, `max|a-b| = 0` — with the two output channels identical, as a mono signal into a
  stereo device should be.

  The recording is not real-time safe (it can block on a slow disk) and is off by default for that
  reason; a build without it refuses `--capture-device-path` by name rather than running unrecorded.
  What the recording starts with is one period of silence: the backend primes the output ring before
  starting the device, so the first thing the device is handed is that priming period.

- **The probe runs the device for a few callback periods before the stream's own IOProc is created.**
  That is the only way to know the number, it writes no device property and changes no setting, and
  the device is returned to the system between the two.
- **Block times measured here include the device's buffer**, so a 1024-frame period means a ~21 ms
  block even when the engine's work is a fraction of it. Compare against `file` when the question is
  the engine's own cost.
- **Capture is a live input device or a file, in that order of preference, and they are alternatives.**
  `--input-device UID` captures from a CoreAudio input device: a guitar through an interface on the
  Mac, with no board and no fifth process. `--capture-path FILE` loops a WAV instead, for a machine
  with no interface attached. Giving both is refused by name, and so is giving neither, because one of
  them silently winning (or an engine running on silence for ever) is not something the sound would
  tell you. **The permission is the reason this is a flag and not a default**: opening an input device
  is what makes macOS ask for microphone access, the grant belongs to the process that *spawns* this
  one, and a monitoring backend that asked for the microphone because somebody wanted to hear a WAV
  would be asking on the wrong authority. Whoever spawns `namchaind` therefore needs
  `NSMicrophoneUsageDescription` and — under the hardened runtime — the
  `com.apple.security.device.audio-input` entitlement, and the prompt appears on first use. A refused
  microphone is **silence**, not an error: the HAL hands zeros, which is why `engine.status` cannot
  say whether the room was quiet or the permission was never granted. When the input and the output
  are *different* devices the backend makes a **private aggregate** of the two — see below.
- **Two devices have two clocks, and the answer is an aggregate.** `--input-device` and `--device`
  naming different HAL devices would mean two independent clocks with no resampler anywhere in this
  engine: at an ordinary 100 ppm that is a period dropped or inserted every couple of minutes, counted
  as an xrun and indistinguishable from the engine being late. So the backend asks the HAL for a
  private aggregate device instead: one clock (the **output**, because the plan is compiled for its
  period and its callback paces the engine) with the input drift-compensated, which is the HAL
  resampling it to follow. Private, so it never appears in Audio MIDI Setup, and destroyed when the
  daemon closes the device. When one device serves both directions — the common case for an interface
  — no aggregate is made at all. `engine.status.backend_diagnostics` says which happened
  (`live_input`, `aggregate_device`, `device_period_frames`).
- **The device's buffer is the latency knob, and it is put back.** `--device-buffer-frames 32` asks the
  device for a 32-frame IO buffer (0.67 ms at 48 kHz) instead of whatever it had, which is most of the
  monitoring latency; the request is checked against the device's own range and read back, so
  `engine.status.backend_diagnostics.device_buffer_frames` reports what the device actually took. The
  value it had before is restored in `stop()`, because a monitoring tool that leaves somebody's
  interface — and every other application that opens it — at 32 frames has taken a decision that was
  not its to take. A crash between the write and the restore leaves it set; that is the price of
  writing a device property at all, and Audio MIDI Setup is where it is put right.
- **The render thread asks macOS for real-time scheduling, and it gets it.** There is no settable
  `SCHED_FIFO` for an unprivileged process the way Linux has one — but `pthread_setschedparam` with
  `SCHED_FIFO` *does* work here, and it is **read back** (`engine.status.realtime`, `rt_policy 4`).
  On top of that the thread is given a `THREAD_TIME_CONSTRAINT_POLICY` window sized to the audio
  period (the configured ceiling at start-up, then the device's measured period as soon as it is
  known), which is the facility CoreAudio's own IO threads use; macOS accepts it and then reports it
  back as the default, so the report says "accepted, and these are the numbers asked for" rather than
  pretending to have read it. Two things about it are not obvious and were measured on macOS 27:
  a further `pthread_setschedparam` **fails with `EINVAL`** once a window is in force (so the daemon
  skips the call when the thread is already what was asked for — it re-applies scheduling on every
  device open), and the window's period must be converted with the mach timebase **the other way round
  from the way it reads** (`ticks = ns * denom / numer`), or the `uint32_t` overflows and the kernel
  answers `KERN_INVALID_ARGUMENT`.
- **A UID is matched by reading every device's UID, not by `kAudioHardwarePropertyTranslateUIDToDevice`.**
  That property is the documented way and it refuses every size this code can offer it on macOS 27
  (`kAudioHardwareBadPropertySizeError`, `!siz`), including the size of `AudioValueTranslation`
  itself. Nothing noticed for a while because everything opened the *default* output device — an empty
  UID — and the one refusal test passes a deliberately bogus UID, which fails the same way a working
  call refuses a bad string. Live input is what made it visible, because there is no "default input"
  to fall back on.
- **A device that dies or is unplugged is a device loss**, exactly as it is on ALSA: the daemon
  closes it, reports `lost`, and reopens on its `device_retry_ms` backoff. A change to the *default*
  output device while running does not migrate the stream — the opened device is the one being
  driven until it goes away.

## Two transports

One protocol, two ways to reach it. The JSON bodies are identical — the same `engine.status`, the same
`rig.apply`, the same error codes, the same tests on both sides — so a client is written once and the
transport is a detail it picks at startup.

| | Unix socket (default) | TCP (opt-in) |
|---|---|---|
| Framing | none: `SOCK_SEQPACKET` preserves message boundaries | **u32 little-endian length prefix** + one JSON body |
| Auth | filesystem permissions (`socket_mode`) | **pre-shared token** in `engine.hello`, constant-time compare |
| Trust | the local operator | untrusted network; off unless `tcp_listen` |
| Caps | 16 connections, 256 KiB request, a reply no larger than one datagram (below) | `tcp_max_connections` (4), `tcp_max_message_bytes` (1 MiB request), 16 MiB reply, 3 auth failures then close |
| Bind | `socket` | `tcp_address`/`tcp_port`, **loopback by default** |

**On Linux, the Unix socket carries each reply as one datagram.** A datagram can be no larger than
the socket's send buffer, about 208 KiB with the kernel's default `net.core.wmem_default`. So a larger
reply can't be sent there, and `namchainctl` reports "the daemon closed the connection". That includes
`engine.assets` or `engine.schema` for a library of more than a few hundred impulse responses. TCP
has no such limit, so ask over it instead, on the device itself if need be:
`namchainctl --host 127.0.0.1 --token-file /etc/namchain/token assets`. On macOS the local socket is
a stream with the length prefix, so the limit doesn't apply there.

TCP is off unless an operator turns it on, and the daemon **refuses to start** with `tcp_listen` set
and no token: an unauthenticated control port that can rewrite the rig and mute the output is worse
than a daemon that will not start. A token shorter than 16 characters is refused outright, so a
four-digit PIN cannot be configured by accident.

The client side is `namchainctl` (slice C), which is both the operator's tool and the reference for
whoever writes the next client:

```console
$ namchainctl --socket /run/namchain/namchaind.sock status --compact
$ namchainctl --host 127.0.0.1 --port 47810 --token-file /etc/namchain/token get
$ namchainctl --host studio.local --token-file /etc/namchain/token set --node g --param gain_db --value -6
{"applied":1,"chain":"main","generation":4,"kind":"smooth","ok":true}
```

`set` is the shape a UI uses while a knob is being dragged: it is one `set_param` op, and the answer's
`kind` says whether the engine ramped it (`smooth`) or had to rebuild the plan (`structural`).
`namchainctl` exits 0 when the daemon said yes, 1 when the command line or the connection failed, and
2 when the daemon answered with an error — printing the error's code and message on stderr.

### The wire, byte for byte

A request body is one JSON object; on TCP it is preceded by its length:

```
+--------+--------+--------+--------+--------------------------------+
| len & 0xff | len >> 8 | len >> 16 | len >> 24 | len bytes of JSON  |
+--------+--------+--------+--------+--------------------------------+
```

Three consequences a client has to get right, and the daemon's own tests assert:

- **A message may arrive in pieces, or several may arrive together.** Bytes are reassembled by the
  length prefix; one `recv` is not one message, and code that assumes it works in a test on a loopback
  and fails on a network.
- **The length is checked before the body is read.** A message declaring more than
  `tcp_max_message_bytes` is answered with `message_too_large` from the header alone, and the
  connection is then closed — a stream cannot be resynchronised after a framing error, so
  "keep reading and hope" is not an option.
- **Writes are non-blocking, and a reply may be queued rather than sent.** A client that stops reading
  fills its own socket buffer and then the daemon's per-connection outbox. Past 16 MiB of undelivered
  reply (`kMaxReplyBytes`, in `framing.h`) the connection is closed. The control loop never waits for
  a client, which is what keeps a slow reader from being everyone's problem.
- **A reply can be much larger than a request.** Replies have their own limit because they list what
  the daemon holds. `engine.assets` and `engine.schema` carry the whole library, about 1 MB each for
  1,500 impulse responses, while `tcp_max_message_bytes` bounds only what a client may send. A client
  has to be ready to read a reply of up to 16 MiB.

`engine.status` reports the transports as they are, not as configured — `control` carries
`unix_socket`, `unix_connections`, `tcp_listening`, `tcp_address`, `tcp_port`, `tcp_connections`,
`tcp_max_connections`, `tcp_max_message_bytes` and `tcp_token_required`. A client that cannot connect
gets a better answer from `namchainctl --socket … status` than from a failed `connect`.

### Threat model

What the transport defends, and what it does not. Written down because a control port that can mute
the output and rewrite the rig deserves an explicit account of itself rather than a promise.

**Defended:**

- **No unauthenticated port.** The listener will not start without a token, the token is compared in
  constant time, three failures close the connection, and a connection that does not authenticate
  within `tcp_auth_timeout_ms` is dropped — so four idle connections cannot hold every slot.
- **Bounded work per peer.** A message size, a connection count, a bounded outbox, and a bounded
  receive buffer (the length prefix is checked before the body is read). None of these is a rate
  limit; together they mean no single peer can make the daemon allocate without bound.
- **No arbitrary file access from the network.** A document or patch arriving over TCP may not name an
  absolute path: model and IR parameters resolve relative to `rigs_root`, which is the same boundary a
  `rig.load` name lives behind. Refused as `path_refused`, naming the path, before anything is
  compiled. On the Unix socket an absolute path is still the operator's business — the filesystem
  already decided who may talk to the daemon.
- **No remote file writes at all.** Nothing in the protocol writes a file; the daemon's only outputs
  are the audio device and, with the `file` backend, a WAV the operator configured.

**Not defended, deliberately:**

- **The wire is plaintext, and the token travels in it.** Anyone who can read the traffic can read the
  token and the whole rig. TLS is out of scope; for anything past a trusted LAN the answer is a
  tunnel (`ssh -L`, WireGuard, a TLS terminator), and `tcp_address` defaults to loopback so that
  exposure is a decision rather than an accident.
- **Three guesses per connection is not a rate limit.** A client can reconnect and guess again. The
  bound that matters is the token's own entropy: 16 characters minimum, and a generated 32-byte secret
  is what an appliance should use. There is no lockout, no fail2ban hook and no per-address memory.
- **`--tcp-token` on a command line is visible** to every local user through `/proc/<pid>/cmdline`.
  `tcp_token_file` exists for that reason, and the daemon warns at startup when the file is readable
  by anyone but its owner.
- **A token holder can do anything the protocol allows**: rewrite the rig, mute the output, and load
  any model under `rigs_root`. There are no roles, no per-method scopes and no audit log. This is one
  credential for one appliance, not an account system.
- **Denial of service is only partly addressed.** The audio thread is separate and unaffected by the
  control loop, and a full connection table is answered with `too_many_connections` rather than a
  silent hangup — but an attacker on the path can still flood the listener, and there is no admission
  control on expensive operations (a large document apply costs what it costs).

## The control protocol

One JSON object per message in each direction, over either transport above. A request is:

```json
{ "id": "7", "method": "rig.load", "rig": "two-chains.json" }
```

`id` is optional and echoed. A response is `{"ok": true, ...}` or:

```json
{ "ok": false, "error": { "code": "compile_failed", "message": "…" } }
```

| method | request | response |
|---|---|---|
| `engine.hello` | `{"protocol": 2, "token": "…"?}` | `role`, `version`, `protocol` (what the server speaks), `features`, `transport` (`unix`/`tcp`) and `authenticated`; anything from 1 to the server's version is accepted, and anything else is `unsupported_protocol`. On TCP the token is required and is checked **before** the version, so a peer without it learns nothing else |
| `engine.schema` | — | the catalogue: every node type **and every macro** in one array, told apart by `kind` (`primitive`/`macro`), with ports, flags, arity and parameters — kinds, units, defaults, bounds and whether each can be ramped. A macro additionally carries the `roles` it lowers to and, per parameter, the `targets` it drives. A parameter whose choices live in the library carries them, resolved from the same scan `engine.assets` reports. See docs/schema.md and docs/macros.md |
| `engine.assets` | `{"refresh": true?}` | the model and impulse-response library: an entry per file with its `id`, where it is, its size and mtime, and the rate/architecture/length when the file's own header says so. See docs/assets.md |
| `engine.status` | — | device state, backend, channels, rig/generation, blocks, xruns, a `queue` object (the output queue's cap and floor, and its `output_drops`, `output_target_drops`, `floor_insertions` and `capture_bursts`), a `record` object (as `engine.record`), a `render` object (each block's time from its capture read to its playback write against the period's deadline: `deadline_us`, `worst_us`, `worst_percent`, `over_90_percent`, `over_deadline`, and `load_histogram` as `[percent, blocks]` pairs for the non-empty 1% buckets, 100 meaning at or past the deadline; all cumulative, so a poller takes deltas), device lost/restored counts, mute, RT state, retired-pending, `params_applied`/`params_stale`, `failed_nodes`/`guard_trips`, a `chains` array (below), a `control` object (the transports, above), a `cost` object ("Cost and admission", below), `health`/`issues` (below), the PCM `format` the device was opened with, the audio `levels` ("Levels", below, with the same shape `engine.levels` returns), and — for ALSA — a `backend_diagnostics` object that splits the one `xruns` number into `capture_overruns` and `playback_underruns` beside `recoveries`, `gate_capture_xruns`, `floor_inserted_frames`, `settle_discarded_frames`, `occupancy_mismatches` and `period_exact` |
| `engine.levels` | — | the audio levels, and only them: `state`, `blocks`, `generation`/`audible_generation`, `sample_rate`, `period_size`, `input` and `output` (per device channel: a `peak` in dBFS, an `rms`, a `clipped` count and a `frames` count), and a `chains` array of `{id, name, output_channel, muted, out: {peak, rms, frames, clipped}, nodes: [{node, element, role, peak, rms, frames}]}`. **Read-only and small**, which is why it is a method of its own: a meter polls this rather than building the whole of `engine.status` twenty times a second. Each read *takes* its window (see "Levels", below) |
| `engine.mute` | `{"mute": true\|false, "chain": "id"?}` | the new state; with `chain`, that chain alone is muted and the engine-wide flag is left alone |
| `engine.panic` | — | mutes unconditionally (the only other route to silence) |
| `engine.record` | `{"action": "start", "name": "…", "seconds": 600?}`, `{"action": "stop"}`, or `{}` | `record`: whether one is running, its name and path, periods and seconds so far, ring overflows, and the dropped/inserted/xrun totals. See "Recording what the device heard", below |
| `rig.load` | `{"rig": "name.json"}` | compiled and published; returns `rig`, `generation` |
| `rig.apply` | `{"document": { … }}` — a whole document, parsed by the same parser a rig file goes through | `name`, `chains`, `generation`; relative model paths resolve against the rigs root |
| `rig.patch` | `{"ops": [...], "chain": "id"?}` — `set_param`, `add_node`, `remove_node`, `connect`, `disconnect`, `rewire`, `swap_model`, `bypass`. A target is named by `node`, or by `element` with an optional `role` (and `port` for a per-port parameter) | `kind` (`smooth` or `structural`), `applied`, `chain`, `generation` |
| `rig.get` | — | the rig path (empty for an applied document), `source` (`file`/`applied`), `generation`, `loaded`, the current `document` as the engine parsed it, the `lowered` graph it became (macros expanded, with the engine-minted ids), and a per-chain summary |
| `rig.cost` | `rig` (a name, as `rig.load` takes) or `document` (a whole document) | what that rig would cost on this device, and the verdict: the same table, trimming and `over` a publish would apply, on a copy that is thrown away. Nothing is published, compiled or loaded. `cost` is the object `engine.status` reports, and `slimmed_models`/`missing_assets` come with it, so a service can cost its whole bank without a second cost model of its own (D9) |

Unknown fields are ignored; an unknown method is `unknown_method`. A malformed message never kills
the connection or the daemon.

`chain` is optional for a document with one chain and **required** when there are several: a patch or
a mute that did not say which chain would otherwise edit an arbitrary one, and guessing is not a
service. An unknown chain is `unknown_chain` and names the one that was asked for.

An op names its target one of three ways (`docs/ui-integration-plan.md` §1.3):

```jsonc
{ "op": "set_param", "element": "b3", "param": "feedback", "value": 0.6 }        // a macro parameter
{ "op": "set_param", "element": "b3", "role": "line", "param": "ms", "value": 250 } // one lowered node
{ "op": "set_param", "node": "gain2", "param": "gain_db", "value": -6 }         // a node by id
```

`element` is the client's own identity for a block and `role` is the engine's name for one node of it
(docs/macros.md). An element with several nodes and no role is refused with the roles to choose from,
and an address that names nothing is refused with what exists — no op ever silently does nothing. A
value may be a number or, for an option parameter, a string.

### Where a document comes from, and what a client can read back

Two ways in, and they differ only in where the bytes live:

- **`rig.load`** names a file under `rigs_root`. Relative model/IR paths in it resolve against *that
  file's own directory* first, which is what lets a rig be checked in next to its models.
- **`rig.apply`** carries the document itself, which is the path a UI takes: it owns the document, so
  a structural edit is one publish rather than a sequence of graph ops it would have to get right
  (docs/ui-integration-plan.md §1.1). Relative paths resolve against `rigs_root`, the same trust
  boundary a `rig.load` name lives behind. An absolute path is the operator's business on the Unix
  socket and is refused (`path_refused`) for a document that arrived over TCP — see "Rig names and
  trust" below.

`engine.status` reports which one it was as `rig_source`, with `rig` (the path, empty for an applied
document) and `rig_name` (the document's own name) beside it.

**`rig.get` hands back the engine's parse of the document it is running**: every chain with its id,
name and channels, and every node with its `id`, `element`, `role`, `type`, `bypass` and parameters —
including the node ids the engine *minted*, which is the only place a client can learn them. It is a
**diagnostic, not a document store** (docs/ui-integration-plan.md §1.1): the app owns the document,
and nothing should be rehydrated from this. But it makes two things answerable that otherwise are
not — "did my edit land, and where did it go", and "is the engine still running what I think it is".

It is also a **fixed point**: applying what `rig.get` returned changes nothing, so it does not advance
the generation. That is what makes the read-back usable for reconciliation rather than something to
be careful with.

**A client's own data goes in `meta`.** The document, each chain and each node may carry a `meta`
object, which the engine keeps exactly as written, returns from `rig.get` (and keeps through a
`rig.patch`), and never reads. A version, a colour, a layout hint: whatever the client needs to find
again. A change to `meta` alone compiles to the same plan, so it rebuilds nothing and the generation
stays where it was. `meta` must be an object. **Every other key is checked**: on a node, any key that
isn't `id`, `type`, `element`, `role`, `bypass`, `params`, `strings` or `meta` is a parameter, checked
against the node's schema; on the document, a chain or a connection, a key the engine doesn't define
is refused (`bad_request`, naming the key), so a misspelt `output_chanel` is an error rather than a
chain quietly playing on channel 0.

`features` in `engine.hello` and `engine.status` is the list of what a client can rely on:
`multi_chain`, `documents`, `rig_apply`, `rig_get_document`, `structural_crossfade`, `smooth_params`,
`bypass`, `guard`, `per_chain_mute`, `schema`, `tcp_transport`, `assets`, `record`, `local_transitions`,
`meta`, `bypass_ramp`, `cost`, `levels`. It grows as the slices land, and
absence is how a client degrades rather than guesses: a client that does not see `levels` hides its
meters rather than drawing a floor.

The error codes a client may have to act on are `bad_json`, `bad_request`, `unknown_method`,
`unknown_chain`, `rig_not_found`, `patch_rejected`, `compile_failed`, `asset_missing`, `over_budget`,
`path_refused`, `unauthorized`, `message_too_large`, `too_many_connections` and `unsupported_protocol`. A refusal is a normal answer:
the connection stays open (except after a framing error, a third failed authentication, or a full
connection table) and the running plan is untouched.

A parameter name the engine does not know is refused with the list of what it takes, at load and at
patch time alike: `node 'g' (nc.gain) has no parameter 'gainz'; it takes 'gain_db', 'gain', 'ramp_ms'
(alias)`. The catalogue those names come from is `engine.schema`, and docs/schema.md is the reference
for it — including which parameters a client may stream (a slider) and which it must commit on release
(a structural edit).

### One document, several chains

A rig spec may hold up to `max_chains` (8) chains, each with an id the client owns, an input and an
output device channel, and its own graph (docs/rig-pipeline.md §2). What that means at runtime:

- **One plan per chain, one generation per document.** Every chain is compiled, prewarmed and bound
  before anything becomes visible, and a document that fails to compile anywhere fails whole — the
  running document keeps rendering.
- **Two chains may read the same input channel**, and **two chains writing one channel are summed**,
  with the sum clamped on the way to the device. That is what makes a guitar chain and a vocal chain
  two rows of one mixer rather than a fight over a channel.
- **A document apply crossfades its chains one at a time.** With no admission control, dual-rendering
  every changed chain at once is how a two-chain rig asks for three times the CPU it has; the stagger
  keeps the peak at two of the *largest* chain instead. `engine.status` reports
  `max_concurrent_transitions`, which is that rule as a number: 1 means it has never overlapped. The
  cost is that applying a document takes up to `chains × fade_ms` rather than `fade_ms`.
- **A structural edit fades only where it changed the chain.** What the edit did not touch keeps its
  instance and state, and a change only to values that ramp builds no plan at all
  (docs/transitions.md, "Local transitions"). The reply says what each changed chain got
  (`transitions`), and `engine.status` counts `local_transitions` and repeats the newest list as
  `last_transitions`. `local_base_mismatches` counts a local plan that found a different plan playing
  than the one it was built against and switched straight to its final plan, without a fade; it
  should stay 0.
- **A chain whose graph and channels did not change is not replanned and does not fade.** A patch to
  the guitar chain leaves the vocal chain's plan, its models and its generation exactly where they
  were. A publish that changes nothing at all does not advance the generation, so reloading the same
  file reports that nothing happened rather than leaving a client waiting for an adoption that will
  never come.
- **Per-chain mute keeps the chain running.** Its output is dropped from the bus after the chain has
  run, for the same reason `engine.mute` zeroes after the chain: a muted chain whose inference is
  skipped freezes its state and clicks on unmute (G14).
- **A chain the new document does not have stops immediately.** Its plans are retired rather than
  faded — a removal is not a transition between two plans, and inventing a fade-to-silence path for it
  would be a mechanism nothing else needs. A client that wants the removal to be gradual mutes the
  chain first, which is a ramp.
- **A chain's channels must exist.** A chain naming a capture or playback channel beyond
  `capture_channels`/`playback_channels` is refused at load with a message naming the chain, and a
  device that turns out to give fewer channels than configured is refused when it is opened — reading
  a channel that is not there is not something to discover on the audio thread.

`engine.status` therefore carries a `chains` array, in the document's order, one entry per chain:

```json
{ "id": "voc", "name": "Vocal", "input_channel": 1, "output_channel": 1,
  "generation": 3, "audible_generation": 3, "adopted_generation": 3, "target_generation": 3,
  "transition_active": false, "muted": false, "failed_nodes": 0, "guard_trips": 0 }
```

### Health, and what a status light binds to

`health` is one word — `ok`, `warn` or `error` — derived from `issues`, which is what a small control
binds to without parsing anything:

```json
"health": "error",
"issues": [ { "code": "device_lost", "severity": "error",
              "message": "the audio device is not open; the render thread is retrying every 250 ms",
              "count": 1 } ]
```

`code` is stable and machine-readable (the client maps it to its own words and icon); `severity` is
per issue and `health` is the worst of them, so the two cannot disagree; `count` accumulates; and
`chain`/`node` name the culprit where there is one. Issues are **current state, not a log** — one that
stops being true disappears, which is the property that makes the light worth having:

| code | severity | true while |
|---|---|---|
| `device_lost` | error | the device is not open and the render thread is retrying |
| `node_failed` | error | a node returned a non-finite sample and has been taken out of the path |
| `xruns` | warn | the device has missed deadlines *recently* — fed by the deltas and drained, not the driver's cumulative counter, which never clears |
| `render_errors` | warn | the render thread has reported errors recently |
| `not_realtime` | warn | a real-time policy was asked for and is not in force |
| `memory_not_locked` | warn | `mlockall` was refused while `lock_memory` is on |
| `asset_missing` | warn | a block's model or IR is not in the library, so it plays as if bypassed. One per block, with `element`, `param`, `kind` and `reference` besides `chain` and `node` (docs/assets.md) |
| `cost_over` | warn | the running document's units cost more than the table's `headroom` with every model that can be trimmed trimmed. Only the configured rig gets here (a client's publish is refused instead); it plays, and may miss deadlines |
| `cost_unknown` | warn | a node was charged its `unknown` cost because this device has no measured row for it: admission is guessing, in the expensive direction (D8). One per node, with `element`, `reference` and `row` |
| `cost_table_missing` | info | there is no readable cost table at `cost_table`, so every unit is charged its unknown cost. The rigs that fit still play; the device has simply not been measured |
| `model_slimmed` | info | a block plays its smallest model to fit the model budget. One per block, with `element` and `reference`: informational, because it is the budget doing its job, but named, because a Lite does not sound like the Full the rig names |
| `transition` | info | a structural change is being crossfaded (normal, and it does not colour `health`) |

Two things deliberately *not* issues: a daemon asked for no real-time priority that has none, and one
asked not to lock memory, are doing exactly what they were told — the test for a healthy daemon
asserts both are quiet. `params_stale` stays a counter, because a parameter that arrived after its
target generation was superseded is an ordinary consequence of an asynchronous edit rather than a
fault.

The document-level `generation`, `adopted_generation`, `audible_generation`, `transition_active` and
`failed_nodes`/`guard_trips` are derived from those: `audible_generation` is the *previous* document
generation while any chain is still moving, so it means "the generation the device is hearing" even
in the middle of an apply. `max_chains` is the limit, published rather than assumed.

`mute` zeroes the *emitted* copy **after** the chain has run: a muted chain whose inference is
skipped freezes the model's state and clicks on unmute (plan.md G14).

`rig.patch` **routes by op class** (plan.md §5.3), and says which path it took in `kind`:

- If every op is a `set_param` the live plan's vtable can dispatch, the whole patch is `"smooth"`: one
  POD record per op on the ring, ramped inside the node over its own smoothing time. No recompile and
  no step; `engine.status` reports `params_applied` once the render thread has taken it. The response
  is an acknowledgement, not a completion — adoption is asynchronous by design.
- Any other op — or a `set_param` the vtable cannot ramp, such as `gain_db`, which `nc.gain` only
  reads at build — makes the whole patch `"structural"`: the ops are applied to a *copy* of the
  current graph, and the copy is compiled and published only if every op succeeded. The render thread
  then dual-renders the old and new plans for `fade_ms` and equal-power crossfades their outputs
  (`rt::Crossfade`) before retiring the old one. `engine.status` reports `"structural_transitions":
  "crossfade"`, with `transition_active` and `audible_generation`/`target_generation` during the fade.
  A burst coalesces to the newest: one transition in flight, one pending.

`add_node` takes a node object in the rig's own syntax, parsed by the same parser, so a patched node
and a declared node mean the same thing. `rewire` repoints one input port. `bypass` sets a flag: the
node **keeps running** so its state stays warm, and its output becomes its dry input — which is why
un-bypassing does not step (plan.md G14). A patch that only bypasses (and ramps) answers
`"kind": "structural"`, because it changes the document, but builds no plan: its `transitions` say
`ramps`, with a `bypasses` count, the running plan crossfades each node between its output and its
input over `fade_ms`, and `generation` stays where it was. A patch is **all-or-nothing**: a rejected op or a graph the
compiler will not accept leaves the running plan rendering exactly as it was, and `generation`
unchanged. The full op list and semantics are in docs/transitions.md.

Every node's output is scanned for non-finite samples; a node that returns one is taken out of the
signal path for good and reported as `failed_nodes`/`guard_trips` in `engine.status`. A well-behaved
rig reports zero — which is the number to check first when audio is not what a patch expected.

### Rig names and trust

Two different trust levels, two different rules:

- The **operator** (`--rig`, config `rig`) may name any path; an absolute path or a path that exists
  relative to the working directory is taken as written, and a bare name resolves under `rigs_root`.
- A **socket client** (`rig.load`) may name only a file under `rigs_root`. Absolute paths, `..`
  traversal and symlinks that resolve outside the root are refused before any file is opened, and
  the target must be a regular file. A client must not be able to make the daemon read an arbitrary
  path and hand back the parse error (docs/rig-pipeline.md §5.1 item 1).
- A **network client** (`rig.apply`/`rig.patch` over TCP) may not name an absolute path *at all*:
  model and IR parameters in a document it applies, or in the ops it sends, must be relative to
  `rigs_root`. The local operator's absolute paths stay the operator's business, and a remote client
  may still *edit* a document that contains one — the rule is about what the client writes, not about
  what it can reach. Refused as `path_refused`, quoting the path, before anything is compiled.

### Macros: a block the engine lowers

A document may hold **macros** as well as primitives: `fx.eq.bands`, `fx.eq.tilt`, `fx.filter.cut`,
`fx.mix.drywet`, `fx.delay.feedback`, `fx.amp.tonestack`, `fx.cab.ir`. They are lowered into
primitives when a chain is compiled, so nothing on the render path — and no node type — knows that
macros exist. `rig.get` returns both views: the document with its macros, and the primitives they
became.

A macro is addressed by `element` (its own parameters) or `element` + `role` (one node inside it), and
`engine.schema` publishes both its parameter map and its roles. docs/macros.md is the reference,
including what a macro-level `set_param` records and the two bypass rules.

### Assets: a model is an id, not a file

A `nc.nam` node's `model` and a `nc.cab.ir` node's `ir` are **references**, resolved when a document
is compiled: a file relative to the document first (as they always were), then the device's library by
id, then an absolute path. `engine.assets` lists the library and `engine.schema` resolves the same scan
into the palette's option lists, so an app never has to know the layout of the disk — and a document
that names `"model": "tweed/deluxe"` means the same thing on every device that has that model.

Resolution happens on the **copy the engine compiles**, never on the document it remembers: `rig.get`
hands back the ids the client wrote, and reapplying it is still a fixed point. A reference that
resolves nowhere makes its block play as if bypassed, reported as an `asset_missing` issue and in
`missing_assets` (in the rig replies and each chain of `engine.status`), naming the library, its size
and the nearest ids; a rescan republishes the rig, so a model copied in later takes over. Only a
reference on a node that isn't 1->1 still refuses the document as `asset_missing`.

docs/assets.md is the reference — ids, the two roots, what is read from a file (a header sniff, never
a load), and the `engine.assets` response in full.

### Render threads: how many cores the render path uses

`render_threads` is how many cores `namchaind` renders on. `1`, the default, is the engine that has
always run here: one render thread that reads the device, decodes each chain's input, executes every
chain's plan, crossfades, sums the output bus and writes the device. Above one, the chains are shared
out over that many threads pinned to `cpu` and the cores above it, and a block becomes:

1. **the coordinator** (the render thread, on `cpu`) reads the device and decodes every chain's input;
2. **every participant** executes its share of the chains — participant `p` takes chain `p`, `p+N`,
   `p+2N`, ... — including *both* plans of a chain that is mid-transition;
3. **the coordinator** mixes the crossfades, retires the plan that has faded out, skips muted chains,
   sums the output bus and writes the device.

What does not change: a chain is executed by exactly one thread, in its original node order, and only
the coordinator touches the transition bookkeeping, the reclaimer or the output bus. A document
rendered with `render_threads: 2` is therefore **bit-identical** to the same document rendered with
`1`, and `tests/test_daemon.cpp` asserts that sample by sample rather than approximately.

**What it does not help.** A chain cannot be split, so this shares out *chains* and nothing else. A
document whose weight is one long chain — an amp into an amp into a cabinet, which is what the pedal's
heaviest rig is — gets nothing from it, and the second half of a chain cannot start before the first
half has finished. The gain is for documents with more than one chain's worth of work: dual-mono rigs,
a guitar and a bass, a parallel path that exists for its own sake. The measurement and the trade-offs
are in `pedal.project/docs/multi-core.md` and its report.

`engine.status` reports `render_threads` (what the pool actually has), `render_threads_configured`
(what was asked for), `render_cores` (a bit per CPU the participants were confined to, read back from
the kernel rather than remembered) and `render_degraded`. A run whose `render_threads` is below its
configured value is not a multi-core measurement; a worker that cannot be created is reported rather
than silently dropped, and a configuration naming cores this process may not run on is refused at
startup, by name.

### Cost and admission: what a rig costs on this device

A rig is admitted by what its units **cost**, measured on this device rather than assumed. The rules,
the units and the measurements are pedal.project
`docs/managing-processing-power-and-optimising-for-fast-switching.md` (D1–D9, P1–P9); this is the
engine's half of D9, which gives the engine the cost table, the per-document fit check and the
refusal, and gives the device the numbers.

**The table.** `cost_table` (default `/etc/namchain/cost.json`) is this device's measured table. A
**unit** is a NAM model at a submodel size (`NAM.<family>.<full|lite>`), an IR at a tap count
(`IR.<taps>`), or a node type (`node.<nc.type>`). Its cost is `1/n`, where `n` is the largest number
of copies of that unit that rendered a block within the deadline on one core with zero overruns, and
**costs add** (D2). The family of a NAM model is the architecture its file declares, or its
submodels' when it is a `SlimmableContainer` (which is a wrapper, not a network;
`model_family_from_file`). An IR is charged the cheapest row that is not shorter than it, so a
6000-tap cab pays the 8192 row. The format is the engine's; the numbers are the device's, measured by
`namchain-warm-bench measure` on the device and installed with `devices/<host>/cost.json`.

**Admission.** Every publish — `rig.load`, `rig.apply`, `rig.patch`, startup, SIGHUP, a rescan's
republish — costs the resolved document and applies D3's headroom, which is the table's own
`headroom` (0.8):

1. **Every node is charged**, in document order: chains in order, each chain's nodes as declared. A
   bypassed node counts, because it keeps running. What a charge needs — a model's family, an IR's
   taps — is read from the file and cached by path, size and mtime, so a publish does not reparse the
   library.
2. **A unit with no row costs `unknown`** (D8), never nothing: `unknown.NAM`, `unknown.IR` and
   `unknown.node`, each at least the dearest measured row of its kind. An unmeasured device therefore
   refuses heavy rigs rather than waving them through, and every node charged that way raises a
   `cost_unknown` warning naming it.
3. **Over the headroom, models are trimmed Full → Lite** (D6): the dearest saving first, as few nodes
   as it takes, and only nodes whose Lite row is measured. A trimmed node runs the submodel
   `SetSlimmableSize(0.0)` picks, chosen before the model is prepared, so the Full is never allocated
   or prewarmed. A node whose Lite row is not measured saves nothing and is not trimmed for show.
4. **If it is still over**, a client's publish is refused as `over_budget`, with the total, the
   headroom and every node's cost and row in the message. The running document keeps playing.
   The configured rig — at startup, on SIGHUP and on a rescan's republish — is **admitted anyway**,
   with a `cost_over` warning: refusing the rig a pedal powers up with would leave it silent, which is
   worse than a rig that may miss a deadline.

The rig is the unit, not the chain (D1): a rig renders on one core, all of its chains, so the whole
document is costed against one core's headroom. A chain is never split across cores, which is why
`render_threads > 1` is not part of this scheme.

**What a client sees.** `slimmed_models` in every rig reply (`rig.load`, `rig.apply`, `rig.patch`,
`rig.get`) and in each chain of `engine.status`, an entry per trimmed node with `chain` (in the
replies), `node`, `element`, `reference`, `row`, `cost` and `full_cost`; a `model_slimmed` issue per
trimmed node; and `cost` in `engine.status`, with `cost` (as it stands), `full_cost` (as written),
`headroom`, `over`, `verdict` (`fits`, `trimmed` or `over`), `trimmed`, `table` and `table_ok`,
`table_detail`, `table_host`, `table_measured`, `table_engine_commit`, `table_rows`, `unknown` and
`nodes` (every node's `row`, `cost`, `measured`, `slimmed` and `working_set_bytes`). `rig.cost` asks
the same question about a rig that is not playing, which is how the service costs its bank without a
second cost model of its own (D9).

The document `rig.get` returns is the one written: trimming is the engine's choice about the copy it
compiles, made afresh on every publish, so no document carries it and a stale choice never sticks.

**What it does not do yet**, each with the remedy drafted:

- **One row per architecture family.** Two WaveNets of different shapes are one family, which is the
  honest limit of what a file declares; a finer taxonomy needs the file to say more. Remedy: a family
  the model names itself, or a per-file row.
- **The dual render is not counted.** During a structural crossfade both plans render, so a
  transition between two documents that each fit costs up to twice the headroom for `fade_ms`. Remedy:
  the admission control of plan.md §5.3, which costs the fade; the doc's T4 tests the same boundary
  while switching.
- **Costs add, and that is an assumption.** D2's additive model is tested by the doc's T2, not
  assumed: two models crowding one core's cache cost more than either alone, which is why a row's
  `1/n` is measured at the fit point rather than from one model. `working_set_bytes` is reported
  beside every NAM node's cost so the working set against L2 stays visible as T2's falsifier.
- **Full or smallest, nothing between.** A container with more than two submodels is never trimmed
  part-way, and a `SlimmableWaveNet` (one model that slims by channels) counts at full size and is
  never trimmed. Remedy: a row per size the model offers and a trim that steps down one at a time.
- **The client cannot say which amp gives way.** The dearest is trimmed first and a tie goes to the
  later-declared node, so inserting an amp before an existing one can trim the existing one: a sound
  the player did not touch changes. Remedy: a per-node `size` parameter (`auto`, `full`, `smallest`),
  or a trim choice that sticks across publishes.
- **The offline tools do not apply it.** `namchain-render` and the soak render what they are given;
  admission is the daemon's.

## Levels

Every other number this protocol reports is about *timing* — how long a block took, how close it came
to its deadline, how many xruns there were. None of them can say what the audio is doing. A client
that wants a level meter, and a person asking "is the input dead or is the level just low", need a
different question answered: **how loud is the signal here, now** — at the device's input, at its
output, at each chain, and at every node inside each chain.

`engine.levels` answers it, and `engine.status.levels` carries the same object for a client that only
polls status. The answer is in **dBFS, one decimal**, with **-200.0 as the floor** — a number rather
than `-inf`, because the wire is JSON and a client doing arithmetic on a sentinel is a client that
works.

### What is measured, and where

| Place | Reported as | Why there |
|---|---|---|
| The device's input | `input.peak[c]`, `input.rms[c]`, `input.clipped[c]`, `input.frames[c]` per capture channel | The ADC's own level, before any chain routing or mute: a meter has to show what the device handed over, or a routing mistake reads as a dead input |
| The device's output | `output.peak[c]` … per playback channel | The sum of every chain on that channel, and `clipped` is the encode's own clamps — the saturation a listener hears |
| Each chain | `chains[].out` | What this chain contributed to the bus |
| Each node | `chains[].nodes[]` | Every primitive of every block, labelled with `node` (its id), `element` (what the client named — its own block, or the macro it was lowered from) and `role` (which primitive of that macro it is) |

A **macro has no runtime existence** — it is lowered into primitives that keep the client's
`element` — so "the level of every block in the rig" is a grouping by `element`, and the parts are
there for a client that wants to know which half of a cabinet block is hot. A node's float output is
*allowed* past 1.0 and routinely is, so a node's entry carries no `clipped`: full scale is the
device's limit, not the graph's.

A node's entry is its **output**, which is what the next node reads (the graph binds a producing
buffer straight to its consumers) — so "the level at the input of node X" is the entry of whatever
feeds X, and for the first node in a chain that is `nc.input`, whose output is the chain's block
input. The measurement runs after a bypass substitution, so a bypassed node's entry is the dry signal
its consumers actually see.

**Every entry carries `frames`**, the device's and a chain's as well as a node's, and all four numbers
of one entry come from **one read of one window**: `take` empties a probe as it reads it, so reading a
probe twice to fill in two fields would report the second field from the next, empty window.

`rms` is the **root of the mean square** of the window, in dBFS — the quantity a crest-factor reading
needs, rather than a rectified average (the two differ by 0.91 dB for a sine and agree for DC). It is a
level, not a loudness weighting: there is no attack, release or hold — that is the client's, and two
clients polling at different rates get different windows and so different means (the `peak` is the max
over the window and does not depend on how long the window was).

### Read and reset, and why

Every read **takes** its window: the accumulators are emptied as they are read, so two reads describe
disjoint windows and a poller's numbers add up over a long run. The alternative — a peak that holds
until it is cleared — makes "the peak since this client connected" the answer, which is not a meter.

The cost of that choice is one thing to know: **two reads in quick succession each get a window**, so
a client polling faster than the engine renders blocks will see empty (`frames: 0`) windows between
populated ones. A level with `frames: 0` is not a level; it is a window in which nothing was measured,
and a client should hold its display rather than draw the floor. (A test that reads `engine.status`
and then immediately reads `engine.levels` sees exactly this: the status call emptied the probes on its
way through.)

### What it costs the audio thread

The probes are **relaxed atomics written per sample**, in the loop the engine already walks:
`outputs_finite` scans every output sample for the non-finite guard, so a peak and a sum over the same
samples is a compare and an add on buffers that are already hot in cache. Per sample that is a
compare-exchange for the peak, an atomic add for the sum, another for the frame count, and — where
full scale means something — a third for the clip count. The square is accumulated as `x * x * 2^26` in
a `uint64_t`, each square clamped at `|x| = 16` so that a runaway or non-finite sample cannot wrap the
sum: exact, order-independent, and no floating-point accumulator shared between threads. There is **no
logarithm and no square root on the audio thread** — the mean, the root and the dB conversion all
happen on the control thread, where the JSON is built.

A node's probe lives in the plan, not in the daemon, because a plan is executed by whichever pool
worker owns its chain and it dies with the plan — a level from a plan nobody is running is not a
level. `render_threads` is 1 on the pedal today, and this is correct when it is not.

The probe is measuring **what the next node reads**: it runs after a bypass substitution, so a
bypassed node reports its dry level, which is what is actually there. A node the guard has failed is
left out of the answer entirely (its output is substituted dry, and reporting it as silence would send
somebody after the wrong thing) — the `node_failed` issue is what names it.

### Turning it off: `--levels` / `--no-levels`

Measuring is **on** by default, and the switch exists for the two cases where its per-sample cost is
not wanted: an appliance with no client polling levels at all, and a measurement that has to separate
the engine's own cost from the cost of watching it.

`--no-levels` (config key `"levels": false`) turns it off **at the source**, rather than answering
with floors:

| | measuring (default) | `--no-levels` |
|---|---|---|
| A node's probe | one per node, in the plan | none built — `CompileOptions::levels` |
| The render path | peak, sum, frames and clip per sample | no level work at all |
| `engine.levels` | the levels object | refused, `levels_disabled` |
| `engine.status.levels` | present | absent |
| `features` | includes `"levels"` | omits it |

It is **decided once**, when the daemon is constructed and when each plan is compiled, and it cannot
change while the daemon runs. That is not a limitation to work around: a plan is compiled on the
render path by a local transition, so a value that could change under it would be a probe that some
plans write and others do not — a meter reading a mixture of two engines.

The three absences are the point, and they are what the tests assert: a client that does not see
`"levels"` in `features` builds no meters, one that calls the method anyway is told why by name, and
`engine.status` does not carry a levels object full of `-200 dBFS`, which would say "silence" where
the truth is "nobody is measuring".

This is a switch for **cost**, not for safety: with it off, `engine.levels` is the only thing that
stops working. Nothing else in this document changes.

### When a probe is written, and how often it is read

Two knobs that decide what measuring costs. The first is **on by default** (2026-10-06); the second
defaults to **8 Hz**. Both came out of the campaign in the pedal repo's `bench-results/`, and are
documented because a knob nobody can find is a knob nobody can reproduce a number with.

| Option | Default | What it does | What it costs |
|---|---|---|---|
| `--levels-publish-period` / `--no-levels-publish-period` | **on** | A probe folds samples into `level::Window` — plain private memory — and adds that window to its atomics **once per period**, instead of writing them per sample | Latency: a reader can see a window that closed up to one period ago (1.33 ms at q=64). Nothing else: **every sample is still measured** |
| `--levels-rate-hz X` / `--no-levels-rate-hz` | **8** | Drains the probes at most X times a second; a read in between is answered from the held reply. `--no-levels-rate-hz` (or `0`) means every read is a real measurement | Freshness. At 8 Hz a level is up to 125 ms old — half what a 20 Hz meter can show, and below the point where a decaying bar reads as sluggish |
| `--levels-first-frame` / `--no-levels-first-frame` | off | A probe folds the period's **first** frame and skips the rest, on the device's input, each chain's output, the playback bus and every node | What the meter reads. One sample per period misses nearly every transient the peak exists to catch, and `rms` is that sample's magnitude rather than a mean over the window |

**Why publishing per period is the default.** A probe is four unpadded atomics, so probes share cache
lines; writing them per sample means the render thread loses those lines to its reader on every
sample. Folding into private memory and publishing once a period recovers **55% of the whole cost of
having meters** (5.40% → 3.66% of one core on a 34-node rig) *while measuring every sample* — unlike
`--levels-first-frame`, which buys a similar saving by not looking at 63 samples in 64. The meter
reads the same thing either way, because a publish **adds** rather than sets: add and max against the
zeroed reset a reader performs are order-independent, so no sample is lost or counted twice.

**Why 8 Hz.** The conversion — the square root, the logarithms, the JSON — is the one part of this
feature that buys nothing on the render core, and 8 Hz is half what a 20 Hz meter can show. ljoss
polls at 20 Hz, so five reads in six are answered from the held value. Raise it (or pass
`--no-levels-rate-hz`) for a client that wants a real window per read: a diagnostic counting windows,
or a measurement campaign, both of which want every one.

Four things worth knowing about the read path:

- **`--levels-rate-hz` does not change the render path at all.** The fold is the render thread's and
  it runs whoever is polling; only the *reading* is rate-limited, and reading happens on the control
  thread. If the question is render headroom, the publish mode is the knob and this is not.
- **The reply says which it gave you.** `cache.hits`, `cache.misses`, `cache.drains`, `cache.rate_hz`,
  `cache.first_frame` and `cache.publish` ride in the levels object, so a client — or a campaign
  measuring what a poll costs — can check that the configuration it asked for is the one it got
  instead of assuming it.
- **`frames` stays honest under first-frame.** It reports 1 per period rather than the period's
  length, because that is how many samples the level is a mean of. A client that reads `frames` (and
  ljoss does, to tell an empty window from a silent one) is told a sparse window from a full one.
- **A rate at or above the client's own changes nothing**, which is the point of choosing it rather
  than a fixed cap: a 20 Hz meter against a 20 Hz rate costs exactly what it costs with no limit.

Neither switch changes the audio. `--levels` alone decides whether the render path folds anything,
and the audio it renders is identical either way — pinned by a test that compares two plans compiled
with the flag on and off, sample for sample.

## Recording what the device heard

`engine.record` (`namchainctl record start NAME [--seconds S]`, `record stop`, `record`) answers "where
does this click come from". It writes `record_dir/NAME/`:

- `capture.wav`: every capture channel, exactly the bytes the device delivered, period after period.
  On an interface that returns its playback on a loopback channel, that channel is what the interface
  received.
- `playback.wav`: every playback period the render loop offered, all channels, whether or not the
  backend wrote it.
- `events.tsv`: one line per event, `period`, `frame` (in `capture.wav`), `event`, `count`. `dropped`
  means that period was not written (the output queue was full); `inserted` means the floor
  controller wrote that many periods of its own after it; `xrun` and `burst` are the driver's and the
  loop's. `gap` means the recorder's ring overflowed and that many periods are missing from both
  files, so everything after it is early by that much.
- `meta.json`: the format, the channels, and the rig and chains when it started.

The two WAVs are the same length and period-aligned: period *n* of one is period *n* of the other. A
drop or an insertion is a click that is not an xrun, which is why the queue counters are in
`engine.status` as well.

The render thread only copies each period into a four-second ring; a writer thread does the I/O. A
recording ends at its length (default 600 s, at most 3600 s, and never past a WAV's 4 GiB), on
`stop`, when the device reopens with another shape, or at shutdown. A name is one plain directory
component and an existing one is refused, so a client can create files under `record_dir` and nowhere
else, and cannot overwrite anything.

## Reload and device loss

- `SIGHUP` re-resolves and reloads the configured rig, exactly as `rig.load` does, and rescans the
  asset library at the same time. That is what
  `systemctl reload namchaind` means. Watch the rig file yourself and re-read on an explicit request;
  the daemon deliberately does **not** `inotify` the file (docs/rig-pipeline.md §5.2).
- `SIGTERM`/`SIGINT` stop the loop, stop the render thread and close the device. Exit code 0.
- A device that fails or is unplugged does **not** end the process. The render thread closes it,
  publishes `state: "lost"` and `device_lost++`, and reopens it every `device_retry_ms` until it
  returns, at which point `device_restored++` and streaming resumes. `engine.status` answers
  throughout. The systemd unit intentionally has no `BindsTo=`/`Requires=` on a `dev-snd-*` unit,
  which would stop the service at exactly the moment it is supposed to survive.

## A client

`namchainctl` is the reference implementation, over either transport (`tools/namchainctl/namchainctl.cpp`).
To record what a client sends and replay it later, as a repeatable test, see docs/sessions.md.
The smallest thing that works over the Unix socket is still four lines, because the datagram transport
has no framing to implement:

```python
import socket, json
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.connect("/run/namchain/namchaind.sock")
def call(message):
    s.send(json.dumps(message).encode())
    return json.loads(s.recv(65536))
print(call({"id": "1", "method": "engine.hello", "protocol": 2}))
print(call({"id": "2", "method": "rig.load", "rig": "two-chains.json"}))
print(call({"id": "3", "method": "engine.status"})["status"])
```

Over TCP the handshake comes first — nothing else is answered until it succeeds — and every body
carries a length prefix:

```python
import socket, json, struct
s = socket.create_connection(("127.0.0.1", 47810))
def call(message):
    body = json.dumps(message).encode()
    s.sendall(struct.pack("<I", len(body)) + body)
    length = struct.unpack("<I", s.recv(4))[0]
    body = b""
    while len(body) < length:                 # one recv is not one message
        body += s.recv(length - len(body))
    return json.loads(body)
print(call({"method": "engine.hello", "protocol": 2, "token": open("/etc/namchain/token").read().strip()}))
print(call({"method": "engine.status"})["status"]["health"])
```

That is the whole client contract: a length prefix, a token, and the JSON above. A client that gets
those three right talks to the same daemon `namchainctl` does — which is asserted, not assumed:
`tests/cli/run_ctl.cmake` runs the daemon as a process and drives it with the real client over both
transports, and `tests/test_tcp.cpp` covers the framing and the token from the socket side.

## Install the unit

```
sudo install -Dm644 packaging/namchaind.json  /etc/namchain/namchaind.json
sudo install -Dm644 packaging/systemd/namchaind.service \
     /etc/systemd/system/namchaind.service
sudo install -Dm755 build/namchaind /usr/bin/namchaind
sudo install -Dm755 build/namchainctl /usr/bin/namchainctl
sudo install -d /etc/namchain/rigs          # put rig specs and models here
sudo systemctl daemon-reload
sudo systemctl enable --now namchaind
```

To drive it from another machine, turn the TCP transport on and give it a token **from a file** — a
token on the command line is visible to every local user in `ps`:

```
sudo install -Dm600 /dev/stdin /etc/namchain/token <<< "$(head -c 32 /dev/urandom | base64)"
# then in /etc/namchain/namchaind.json:
#   "tcp_listen": true, "tcp_address": "0.0.0.0", "tcp_token_file": "/etc/namchain/token"
sudo systemctl restart namchaind
namchainctl --host <this-host> --token-file /etc/namchain/token status
```

`tcp_address` defaults to `127.0.0.1`, so reaching the daemon across a network is a line an operator
writes deliberately. Off a trusted LAN, put it behind a tunnel rather than exposing the port: the
token travels in clear text, and the threat model above says what that does and does not cost.

The unit exists because a systemd service does not get `pam_limits`: without `LimitRTPRIO` and
`LimitMEMLOCK`, the render thread's `sched_setscheduler(SCHED_FIFO)` is refused and the daemon
silently runs `SCHED_OTHER` (docs/tuning.md). `RuntimeDirectory=namchain` creates `/run/namchain`
for the socket and removes it on stop, so a stale socket can never be the reason a start fails.
`Restart=always` is the coarse backstop; the daemon's own device handling is the fine one.

Run `systemd-analyze verify packaging/systemd/namchaind.service` after editing it. The two warnings
on an uninstalled tree (the binary is not at `/usr/bin/namchaind` yet, and there is no man page) are
expected.

## What is built, and what is not

Built: **the model budget** — a ceiling on what a document's models read per block, estimated from
each model's config, met by slimming slimmable models last-declared first and enforced by refusing a
client's publish that still does not fit (`over_budget`), with the configured rig admitted and
warned about, and `slimmed_models`, `cost` and three issue codes to show it ("Cost and admission",
above); **the macro layer** — seven macros (`fx.eq.bands`, `fx.eq.tilt`, `fx.filter.cut`,
`fx.mix.drywet`, `fx.delay.feedback`, `fx.amp.tonestack`, `fx.cab.ir`) lowered per compile into
biquads, gains, mixes and the existing delay line; a declared param map with a closed transform set,
so one knob drives as many node parameters as it means to and a macro-level `set_param` writes exactly
what the next compile produces; deterministic `element:role` ids; two bypass rules (`roles` and
`crossfade`) declared per macro; `element`/`role` addressing in `rig.patch`; macros published beside
primitives in `engine.schema` and expanded in `rig.get`'s `lowered` view; and an equivalence test per
macro against the hand-built graph it stands for (docs/macros.md); **the asset library** — `models_root` and `irs_root`, a cached recursive scan (id, relative
path, kind, size, mtime, and the sample rate/architecture/length a file's own header declares),
`engine.assets` with an explicit `refresh`, `options_from` resolved through the same scan in
`engine.schema`, model and IR references resolved by id with an `asset_missing` refusal that lists the
near misses, and the offline tools' path behaviour left exactly as it was (docs/assets.md); **the TCP transport** — length-prefixed framing over a non-blocking acceptor and per-connection
in/out buffers, a mandatory token with a constant-time compare, three authentication attempts per
connection, an authentication deadline, and the caps above, with `namchainctl` as the reference client
and `control` in `engine.status` reporting what actually came up; the lane mixer (`nc.mix` takes a
level, a mute and a polarity per input, addressed by port,
ramped, and refused by name when the port does not exist) and the health summary (`health` plus
`issues` with stable codes, derived from the state rather than tracked beside it); the node schema — every built-in declares its parameters (kind, unit, default, bounds with
inclusive/exclusive made explicit, step, scale, rampable), the compiler refuses a name it does not
declare, and `engine.schema` publishes the catalogue (docs/schema.md); and the smooth class across
every value a node has: gains by ramping, a filter by crossfading two coefficient sets, a delay time
by crossfading two read taps; the document protocol — `rig.apply` for a whole document in memory, `rig.get` returning the
engine's parse of what it is running, protocol 2 with `features`, and the per-chain array in
`engine.status`; the `smooth` op class (a `rig.patch` set_param ramps inside the node, no recompile, no step),
the offline scripted timeline that measures it (`namchain-render --graph rig.json --timeline
edits.json`, or `--torture N --seed S`, judged by the glitch detector), the structural dual-render
crossfade that a `rig.load`, a SIGHUP reload or a structural patch takes, the structural patch
protocol (`add_node`/`remove_node`/`connect`/`disconnect`/`rewire`/`swap_model`/`bypass` and a
non-rampable `set_param`), the per-node non-finite guard, and — since slice A2 of
docs/ui-integration-plan.md — documents of up to eight chains with per-chain plans, per-chain
generations, per-chain mute and device-channel binding, applied with staggered crossfades and with
unchanged chains left alone. See docs/transitions.md.

Not built, named in docs/rig-pipeline.md §7 so it is not mistaken for present:

- no `tail_hold` for a bypassed node with a tail (no built-in has one yet);
- no admission control or CPU cost model, so a plan too expensive to dual-render is not rejected before
  it runs, and the fade length is fixed rather than shortened under load — the staggered apply is what
  stands in for it. The model budget is the one rule that refuses a document for what it costs, and it
  costs memory read per block, not time;
- no *document-level* per-port setting: a lane's level, mute and polarity are runtime state addressed
  by port (`rig.patch`), so a rig file expresses them with a gain node in the lane rather than as a
  mixer setting (slice E's lowering is where that belongs);
- no option (string) parameter can be patched yet, so `mode` and `model` are changed by a reload or a
  `swap_model` rather than by `set_param` (slice B2's remainder);
- no macro that lowers to another macro, and no `nc.split` `mode` (`y`/`aB`/`crossover`/`dynamic`): a
  crossover is real DSP and is a macro of its own when something asks for one (docs/macros.md);
- no option parameter can be *ramped*, so changing a model, an IR or a filter mode is always a
  structural edit and a crossfade -- which is also the honest answer, a cabinet swap not being a knob;
- no TLS and no per-client identity on the TCP transport: one pre-shared token for one appliance, with
  a tunnel as the documented answer off-LAN (see the threat model above);
- no write path to the library and no `model.load`/`model.unload`/`model.list`: it is what the
  operator put on the device, listed by `engine.assets` (docs/assets.md §"What is not here");
- no presets, tags or favourites, and no amp/cab metadata beyond the directory tree;
- no *telemetry subscription* and no `scene.*`, `model.*`, `bench.run` methods. Levels are the one
  thing a client polls rather than subscribes to (`engine.levels`, at whatever rate it likes; see
  "Levels"), which is enough for a meter and is deliberately not the beginning of an event stream;
- no state-accurate NAM warm start (M3), so `swap_model` crossfades rather than replaying history.
