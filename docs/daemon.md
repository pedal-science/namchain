# namchaind: the reference daemon

**Status:** built and tested, M2 in progress. `namchaind` starts an audio backend, compiles a
**document of up to eight chains** into one plan per chain, runs them on a dedicated real-time
thread, and serves a Unix-socket control protocol. A parameter edit is ramped inside its node with no
recompile; a structural change — a reload, a patch that alters the graph, a model swap, a bypass — is
dual-rendered and equal-power crossfaded, one chain at a time. Bypass keeps the node running so its
state stays warm. Every node's output is guarded for non-finite samples. The daemon reloads on
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
| `backend` | `alsa_raw` | `alsa_raw`, `null`, `file`, or `pipewire` (refused by name) |
| `device` | *(empty)* | ALSA spec: `hw:4,0`, `4,0`, or a `/dev/snd` path |
| `sample_rate` | `48000` | |
| `period_size` | `64` | quantum in frames |
| `periods` | `4` | device buffer length in periods |
| `capture_channels` | `1` | requested; the daemon decodes the channel each chain names |
| `playback_channels` | `1` | requested; each chain writes the channel it names, and the rest is zeroed |
| `capture_path` | *(empty)* | `file` backend only: the input WAV |
| `playback_path` | *(empty)* | `file` backend only: the output WAV, written when the input runs out |
| `rig` | *(empty)* | startup rig; empty starts silent and waits for `rig.load` |
| `rigs_root` | `/etc/namchain/rigs` | the directory a `rig.load` **name** resolves under |
| `socket` | `/run/namchain/namchaind.sock` | Unix `SOCK_SEQPACKET` control socket |
| `socket_mode` | `"0660"` | octal string |
| `rt_priority` | `80` | `SCHED_FIFO` for the render thread; `0` leaves the policy alone |
| `cpu` | `-1` | pin the render thread to one CPU; `-1` leaves the affinity alone |
| `lock_memory` | `true` | `mlockall(MCL_CURRENT)`, never `MCL_FUTURE` (plan.md §14.10) |
| `output_queue_periods` | `0` | output-ring cap in periods; `0` is off |
| `output_queue_floor_periods` | `0` | queue floor in periods; `0` is off |
| `fade_ms` | `5.0` | equal-power crossfade for a structural plan transition |
| `device_retry_ms` | `250` | backoff between attempts to open a lost device |
| `reclamation_timeout_ms` | `20` | the control `poll` timeout, i.e. the reclamation window |
| `verbose` | `false` | log lifecycle events to stderr |

The daemon refuses a device that does not give the configured quantum and rate exactly
(`require_exact_period`): a plan is compiled for one quantum, and running it at another would read
past every node's buffers. That refusal is a configuration error, not a device failure, and it is
reported with both numbers.

## The control protocol

`SOCK_SEQPACKET`, one JSON object per message in each direction. No framing to reimplement and no
length prefix to get wrong. A request is:

```json
{ "id": "7", "method": "rig.load", "rig": "two-chains.json" }
```

`id` is optional and echoed. A response is `{"ok": true, ...}` or:

```json
{ "ok": false, "error": { "code": "compile_failed", "message": "…" } }
```

| method | request | response |
|---|---|---|
| `engine.hello` | `{"protocol": 1}` | `role`, `version`, negotiated `protocol`; a mismatch is `unsupported_protocol` |
| `engine.status` | — | device state, backend, channels, rig/generation, blocks, xruns, device lost/restored counts, mute, RT state, retired-pending, `params_applied`/`params_stale`, `failed_nodes`/`guard_trips`, and a `chains` array (below) |
| `engine.mute` | `{"mute": true\|false, "chain": "id"?}` | the new state; with `chain`, that chain alone is muted and the engine-wide flag is left alone |
| `engine.panic` | — | mutes unconditionally (the only other route to silence) |
| `rig.load` | `{"rig": "name.json"}` | compiled and published; returns `rig`, `generation` |
| `rig.patch` | `{"ops": [...], "chain": "id"?}` — `set_param`, `add_node`, `remove_node`, `connect`, `disconnect`, `rewire`, `swap_model`, `bypass` | `kind` (`smooth` or `structural`), `applied`, `chain`, `generation` |
| `rig.get` | — | the current rig path, generation, `loaded` |

Unknown fields are ignored; an unknown method is `unknown_method`. A malformed message never kills
the connection or the daemon.

`chain` is optional for a document with one chain and **required** when there are several: a patch or
a mute that did not say which chain would otherwise edit an arbitrary one, and guessing is not a
service. An unknown chain is `unknown_chain` and names the one that was asked for.

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
un-bypassing does not step (plan.md G14). A patch is **all-or-nothing**: a rejected op or a graph the
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

## Reload and device loss

- `SIGHUP` re-resolves and reloads the configured rig, exactly as `rig.load` does. That is what
  `systemctl reload namchaind` means. Watch the rig file yourself and re-read on an explicit request;
  the daemon deliberately does **not** `inotify` the file (docs/rig-pipeline.md §5.2).
- `SIGTERM`/`SIGINT` stop the loop, stop the render thread and close the device. Exit code 0.
- A device that fails or is unplugged does **not** end the process. The render thread closes it,
  publishes `state: "lost"` and `device_lost++`, and reopens it every `device_retry_ms` until it
  returns, at which point `device_restored++` and streaming resumes. `engine.status` answers
  throughout. The systemd unit intentionally has no `BindsTo=`/`Requires=` on a `dev-snd-*` unit,
  which would stop the service at exactly the moment it is supposed to survive.

## A client

```python
import socket, json
s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
s.connect("/run/namchain/namchaind.sock")
def call(message):
    s.send(json.dumps(message).encode())
    return json.loads(s.recv(65536))
print(call({"id": "1", "method": "engine.hello", "protocol": 1}))
print(call({"id": "2", "method": "rig.load", "rig": "two-chains.json"}))
print(call({"id": "3", "method": "engine.status"})["status"])
```

## Install the unit

```
sudo install -Dm644 packaging/namchaind.json  /etc/namchain/namchaind.json
sudo install -Dm644 packaging/systemd/namchaind.service \
     /etc/systemd/system/namchaind.service
sudo install -Dm755 build/namchaind /usr/bin/namchaind
sudo install -d /etc/namchain/rigs          # put rig specs and models here
sudo systemctl daemon-reload
sudo systemctl enable --now namchaind
```

The unit exists because a systemd service does not get `pam_limits`: without `LimitRTPRIO` and
`LimitMEMLOCK`, the render thread's `sched_setscheduler(SCHED_FIFO)` is refused and the daemon
silently runs `SCHED_OTHER` (docs/tuning.md). `RuntimeDirectory=namchain` creates `/run/namchain`
for the socket and removes it on stop, so a stale socket can never be the reason a start fails.
`Restart=always` is the coarse backstop; the daemon's own device handling is the fine one.

Run `systemd-analyze verify packaging/systemd/namchaind.service` after editing it. The two warnings
on an uninstalled tree (the binary is not at `/usr/bin/namchaind` yet, and there is no man page) are
expected.

## What is built, and what is not

Built: the `smooth` op class (a `rig.patch` set_param ramps inside the node, no recompile, no step),
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
- no admission control or cost model, so a plan too expensive to dual-render is not rejected before it
  runs, and the fade length is fixed rather than shortened under load — the staggered apply is what
  stands in for it;
- no per-chain *patching* protocol beyond the request-level `chain` (a patch is one chain's graph, and
  a document apply is the whole thing), no `rig.apply` for a document on the wire, and no `rig.get`
  that returns the graph;
- no parameter schema or catalogue on the wire, so an unknown parameter name is still accepted
  silently (slice B);
- no TCP listener, so the socket is local to the machine (slice C);
- no model/IR registry, so a model is a path rather than a device-library id (slice D);
- no macros, so a block that is really a subgraph has to be written out as one (slice E);
- no telemetry subscription and no `scene.*`, `model.*`, `bench.run` methods;
- no state-accurate NAM warm start (M3), so `swap_model` crossfades rather than replaying history.
