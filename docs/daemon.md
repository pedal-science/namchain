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
| `device` | *(empty)* | ALSA spec: `hw:CARD=<id>[,DEV=<d>]`, `hw:<id>,<d>`, `hw:4,0`, `4,0`, or a `/dev/snd` path. Prefer an id (the names in `/proc/asound`, e.g. `II` for the SSL 2+ Mk II): card numbers follow probe order and change across reboots |
| `sample_rate` | `48000` | |
| `period_size` | `64` | quantum in frames |
| `periods` | `4` | device buffer length in periods |
| `capture_channels` | `1` | requested; the daemon decodes the channel each chain names |
| `playback_channels` | `1` | requested; each chain writes the channel it names, and the rest is zeroed |
| `capture_path` | *(empty)* | `file` backend only: the input WAV |
| `playback_path` | *(empty)* | `file` backend only: the output WAV, written when the input runs out |
| `rig` | *(empty)* | startup rig; empty starts silent and waits for `rig.load` |
| `rigs_root` | `/etc/namchain/rigs` | the directory a `rig.load` **name** resolves under |
| `models_root` | `/etc/namchain/models` | the device's `.nam` model library; a document names a model by id (docs/assets.md) |
| `irs_root` | `/etc/namchain/irs` | the device's `.nam`/`.wav` impulse responses |
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
| `tcp_listen` | `false` | serve control connections on TCP as well |
| `tcp_address` | `127.0.0.1` | TCP address to bind; numeric only (no DNS at startup) |
| `tcp_port` | `47810` | TCP port; `0` lets the kernel choose, and `engine.status` reports it |
| `tcp_max_connections` | `4` | TCP connections open at once, authenticated or not |
| `tcp_max_message_bytes` | `1048576` | largest framed message accepted, checked before the body is read |
| `tcp_token` | *(empty)* | pre-shared token a TCP client presents in `engine.hello` |
| `tcp_token_file` | *(empty)* | read the token from the first line of this file instead |
| `tcp_auth_timeout_ms` | `10000` | how long a connection may stay unauthenticated; `0` disables |
| `record_dir` | `/var/lib/namchain/recordings` | where `engine.record` writes, one directory per recording; empty disables it |
| `verbose` | `false` | log lifecycle events to stderr |

The daemon refuses a device that does not give the configured quantum and rate exactly
(`require_exact_period`): a plan is compiled for one quantum, and running it at another would read
past every node's buffers. That refusal is a configuration error, not a device failure, and it is
reported with both numbers.

## Two transports

One protocol, two ways to reach it. The JSON bodies are identical — the same `engine.status`, the same
`rig.apply`, the same error codes, the same tests on both sides — so a client is written once and the
transport is a detail it picks at startup.

| | Unix socket (default) | TCP (opt-in) |
|---|---|---|
| Framing | none: `SOCK_SEQPACKET` preserves message boundaries | **u32 little-endian length prefix** + one JSON body |
| Auth | filesystem permissions (`socket_mode`) | **pre-shared token** in `engine.hello`, constant-time compare |
| Trust | the local operator | untrusted network; off unless `tcp_listen` |
| Caps | 16 connections, 256 KiB message | `tcp_max_connections` (4), `tcp_max_message_bytes` (1 MiB), 3 auth failures then close |
| Bind | `socket` | `tcp_address`/`tcp_port`, **loopback by default** |

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
  fills its own socket buffer and then the daemon's per-connection outbox; past one message's worth of
  pending replies the connection is closed. The control loop never waits for a client, which is what
  keeps a slow reader from being everyone's problem.

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
| `engine.status` | — | device state, backend, channels, rig/generation, blocks, xruns, a `queue` object (the output queue's cap and floor, and its `output_drops`, `output_target_drops`, `floor_insertions` and `capture_bursts`), a `record` object (as `engine.record`), a `render` object (each block's time from its capture read to its playback write against the period's deadline: `deadline_us`, `worst_us`, `worst_percent`, `over_90_percent`, `over_deadline`, and `load_histogram` as `[percent, blocks]` pairs for the non-empty 1% buckets, 100 meaning at or past the deadline; all cumulative, so a poller takes deltas), device lost/restored counts, mute, RT state, retired-pending, `params_applied`/`params_stale`, `failed_nodes`/`guard_trips`, a `chains` array (below), a `control` object (the transports, above), and `health`/`issues` (below) |
| `engine.mute` | `{"mute": true\|false, "chain": "id"?}` | the new state; with `chain`, that chain alone is muted and the engine-wide flag is left alone |
| `engine.panic` | — | mutes unconditionally (the only other route to silence) |
| `engine.record` | `{"action": "start", "name": "…", "seconds": 600?}`, `{"action": "stop"}`, or `{}` | `record`: whether one is running, its name and path, periods and seconds so far, ring overflows, and the dropped/inserted/xrun totals. See "Recording what the device heard", below |
| `rig.load` | `{"rig": "name.json"}` | compiled and published; returns `rig`, `generation` |
| `rig.apply` | `{"document": { … }}` — a whole document, parsed by the same parser a rig file goes through | `name`, `chains`, `generation`; relative model paths resolve against the rigs root |
| `rig.patch` | `{"ops": [...], "chain": "id"?}` — `set_param`, `add_node`, `remove_node`, `connect`, `disconnect`, `rewire`, `swap_model`, `bypass`. A target is named by `node`, or by `element` with an optional `role` (and `port` for a per-port parameter) | `kind` (`smooth` or `structural`), `applied`, `chain`, `generation` |
| `rig.get` | — | the rig path (empty for an applied document), `source` (`file`/`applied`), `generation`, `loaded`, the current `document` as the engine parsed it, the `lowered` graph it became (macros expanded, with the engine-minted ids), and a per-chain summary |

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

`features` in `engine.hello` and `engine.status` is the list of what a client can rely on:
`multi_chain`, `documents`, `rig_apply`, `rig_get_document`, `structural_crossfade`, `smooth_params`,
`bypass`, `guard`, `per_chain_mute`, `schema`, `tcp_transport`, `assets`. It grows as the slices land, and
absence is how a client degrades rather than guesses.

The error codes a client may have to act on are `bad_json`, `bad_request`, `unknown_method`,
`unknown_chain`, `rig_not_found`, `patch_rejected`, `compile_failed`, `asset_missing`, `path_refused`,
`unauthorized`, `message_too_large`, `too_many_connections` and `unsupported_protocol`. A refusal is a normal answer:
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
resolves nowhere is refused as `asset_missing`, naming the library, its size and the nearest ids.

docs/assets.md is the reference — ids, the two roots, what is read from a file (a header sniff, never
a load), and the `engine.assets` response in full.

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

Built: **the macro layer** — seven macros (`fx.eq.bands`, `fx.eq.tilt`, `fx.filter.cut`,
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
- no admission control or cost model, so a plan too expensive to dual-render is not rejected before it
  runs, and the fade length is fixed rather than shortened under load — the staggered apply is what
  stands in for it;
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
- no telemetry subscription and no `scene.*`, `model.*`, `bench.run` methods;
- no state-accurate NAM warm start (M3), so `swap_model` crossfades rather than replaying history.
