# namchaind: the reference daemon

**Status:** M1 skeleton, built and tested. `namchaind` starts an audio backend, compiles a rig, runs
it on a dedicated real-time thread, and serves a Unix-socket control protocol. It reloads on
`SIGHUP` and survives the interface being unplugged. It does **not** yet have the M2 transition
machinery, so a structural reload is a cold swap at a block boundary and can click; §"What is not
built" is the honest list.

The pieces:

| file | what it is |
|---|---|
| `tools/daemon/namchaind.cpp` | argument handling and the exit code; everything else is in the library |
| `include/namchain/daemon/{config,daemon}.h`, `src/daemon/{config,daemon}.cpp` | config parsing, rig resolution, the render thread and the control loop |
| `packaging/systemd/namchaind.service` | the unit: rlimits, `Restart=always`, `RuntimeDirectory` |
| `packaging/namchaind.json` | an example config to install at `/etc/namchain/namchaind.json` |
| `tests/test_daemon.cpp` | config, protocol and end-to-end tests against the `null` backend |

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
| `capture_channels` | `1` | requested; the daemon decodes channel 0 |
| `playback_channels` | `1` | requested; the daemon encodes channel 0 and zeroes the rest |
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
| `engine.status` | — | device state, backend, channels, rig/generation, blocks, xruns, device lost/restored counts, mute, RT state, retired-pending, `params_applied`/`params_stale` |
| `engine.mute` | `{"mute": true\|false}` | the new state |
| `engine.panic` | — | mutes unconditionally (the only other route to silence) |
| `rig.load` | `{"rig": "name.json"}` | compiled and published; returns `rig`, `generation` |
| `rig.patch` | `{"ops": [{"op":"set_param","node":"g","param":"gain","value":0.5}]}` | ramped with no recompile; returns `applied`, `generation` |
| `rig.get` | — | the current rig path, generation, `loaded` |

Unknown fields are ignored; an unknown method is `unknown_method`. A malformed message never kills
the connection or the daemon.

`mute` zeroes the *emitted* copy **after** the chain has run: a muted chain whose inference is
skipped freezes the model's state and clicks on unmute (plan.md G14).

`rig.patch` is the `smooth` op class of plan.md §5.3: the control thread resolves the node and param
to the vtable dispatch, pushes one POD record on the ring, and the render thread ramps it over the
node's own smoothing time. It never recompiles and never steps, so it costs nothing and cannot click;
`engine.status` reports `params_applied` once the render thread has taken it. The response is an
acknowledgement, not a completion — adoption is asynchronous by design. `"op"` may be omitted and
defaults to `set_param`; any other op kind is a structured `unsupported_op`, so a client cannot
believe a rewire happened when it did not.

Structural reloads take the other path: `rig.load` (and a SIGHUP reload) publishes a complete new
plan, and the render thread dual-renders the old and new plans for `fade_ms`, crossfading their chain
outputs with `rt::Crossfade` before retiring the old one. `engine.status` reports
`"structural_transitions": "crossfade"`, and during the fade `transition_active` is true with
`audible_generation`/`target_generation` naming the two plans. A burst of requests coalesces to the
newest: one transition in flight, one pending. Nothing yet *builds* a modified graph from an op —
that is the structural patch protocol, still to come — so the only structural changes available today
are whole-rig loads.

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
edits.json`, or `--torture N --seed S`, judged by the glitch detector), and the structural dual-render
crossfade that a `rig.load` or SIGHUP reload now takes. See docs/transitions.md.

Not built, named in docs/rig-pipeline.md §7 so it is not mistaken for present:

- no structural patch ops, so nothing yet *builds* a modified graph; a structural change is always a
  whole-rig load (`add_node`/`remove_node`/`connect`/`disconnect`/`swap_model`/`rewire`);
- no bypass/enable or model-swap op class;
- no admission control or cost model, so a plan too expensive to dual-render is not rejected before it
  runs, and the fade length is fixed rather than shortened under load;
- no telemetry subscription and no `scene.*`, `model.*`, `bench.run` methods;
- no state-accurate NAM warm start (M3).
