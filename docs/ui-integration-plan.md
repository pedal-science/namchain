# UI integration plan: the namchain changes ljoss needs

**Status:** in progress. **A1 landed** — the document envelope (a name and N chains with ids and
device channels), three-space node identity (`element`/`role`/`id`), chain-named validation, and
`tests/test_document.cpp`. **A2 landed** — one plan per chain in the daemon, per-chain generations,
mute and transitions, channel binding with summing, staggered applies, unchanged chains left alone,
`engine.status.chains`, `rig.patch`'s request-level `chain`, and the `file` backend wired into the
daemon so a document can be rendered offline end to end. **A3 landed** — `rig.apply` for a whole
document in memory, `rig.get` returning the engine's parse of what it is running (minted ids,
`element`, `role`), protocol 2 with `features`, and relative paths resolving against the rigs root for
an applied document. **B1 and B2 landed** — every node type declares its parameters, the compiler
refuses a name it does not declare (or a value outside the declared bounds, or an option outside the
set, or a missing required parameter), and `engine.schema` publishes the catalogue. **B3 landed** — a
filter's `freq`/`q`/`gain_db` and a delay's `ms`/`samples` ramp in-node, by crossfading two filter
lanes and two delay taps rather than by interpolating coefficients or moving a read pointer. **B4 and B5 landed** —
`nc.mix` takes a level, a mute and a polarity per input, addressed by port and ramped, with a port that
does not exist refused by name; and `engine.status` carries `health` plus `issues` with stable codes,
derived from the current state so a light can go back to green. Slice B is complete. **C1–C3 landed** —
the TCP transport: u32 little-endian framing over a non-blocking acceptor with per-connection in/out
buffers, a mandatory pre-shared token compared in constant time with three attempts per connection and
an authentication deadline, the caps (connections, message bytes, pending output), `path_refused` for
an absolute path from a network client, `namchainctl` as the reference client over either transport,
and a threat model in `docs/daemon.md`. Slice C is complete. **D1–D3 landed** — the asset library: `models_root`/`irs_root` with a cached
recursive scan (size and mtime, at nanosecond resolution where the filesystem has it), `engine.assets`
with an explicit `refresh`, `options_from` resolved through the same scan in `engine.schema`, model and
IR references resolved by id with an `asset_missing` refusal that lists the near misses, and
`docs/assets.md` as the reference. One departure, recorded in §D3 below: a reference that names a file
next to the document is resolved **before** the library, not after. Slice D is complete; slice E
(lowering and the first macros) is next.
Follows
`docs/ljoss-comparison.md` (the analysis) and the seven decisions recorded below. Slices are ordered
by dependency, not by appeal; each is independently committable and independently testable, and none
of them requires the app to exist first.

---

## 0. Decisions recorded

| # | Question | Decision |
|---|---|---|
| 1 | Remote control or tethered? | **Remote.** TCP listener in `namchaind`, off by default |
| 2 | When multi-chain? | **First.** Chains become a first-class part of the document before anything else changes shape |
| 3 | Who lowers a block into nodes? | **The engine** |
| 4 | Stereo? | **Declare and refuse** — the schema carries channel counts, the compiler refuses non-mono |
| 5 | Amp/cab palette source? | **The device's model/IR registry** |
| 6 | First macros? | Multi-band and shelving EQ, cut-with-slope, dry/wet and lane mixing, feedback delay, amp tone stack, cab |
| 7 | Hardware params (phantom, pad, impedance, destination, FX loop)? | **Keep in the document, label as out-of-band**, never sent as engine params |

Also agreed and carried into the design: `isEnabled` becomes **bypass** in ljoss (tile dimmed but
alive); a drag is local and a drop is one commit; a knob drag streams `set_param` only for params the
schema marks rampable; batching is free and should be used; failure is a normal outcome; adoption is
asynchronous; engine health is on screen as a small control that glows until it doesn't; no scenes in
the engine; no device control; no sidechain/modulation; no 102-block DSP library; no tuner, looper,
FX loop or DI.

---

## 1. The contract

Everything in this section is what the app programs against. It is the part that must be right before
the app is written, which is why Slices A and B come first.

### 1.1 The document

One JSON document is one rig. It carries rig metadata and **N chains**, each chain a graph:

```json
{
  "name": "Guitar + Vocal",
  "chains": [
    {
      "id": "6f1c…", "name": "Guitar",
      "input_channel": 0, "output_channel": 0,
      "nodes": [ … ], "connections": [ … ]
    },
    {
      "id": "9a04…", "name": "Vocal",
      "input_channel": 1, "output_channel": 1,
      "nodes": [ … ], "connections": [ … ]
    }
  ]
}
```

- **Back-compatible:** a document with top-level `nodes`/`connections` and no `chains` is one chain
  with id `"main"` on channels 0/0. Every existing `rigs/*.json` and every existing test keeps
  working unchanged.
- **Channel binding lives on the chain**, not on `nc.input`/`nc.output`. That is the smallest thing
  that expresses what ljoss models (guitar on input 0, vocal on input 1). Node-level port binding
  arrives when buses and an FX loop do — that is where `{device, channel}` on a node belongs, and it
  is deferred with the FX loop.
- **Two chains may read the same input channel** (a split physical input) and **may write the same
  output channel, in which case they are summed** into the device buffer and clipped on the way out.
  Summing is the pedalboard-correct behaviour; the alternative (reject) would forbid two chains into
  one master bus.
- A document that does not compile is **rejected whole**, with the error naming the chain. Nothing is
  half-applied; the running document keeps rendering (the transactional property already holds and
  extends naturally).
- **The document is the unit of atomicity and the unit of a generation.** A publish advances one
  document generation; each chain also has its own `audible_generation` during a staggered fade.

### 1.2 Identity: three spaces, none of them composed

Your instinct is right and I have adopted it without a compromise: **a client-owned id must be stored
and returned as its own value, never as a substring of something the engine assembled.** There are
three id spaces:

| Space | Owner | Meaning | Rules |
|---|---|---|---|
| `element` | **the client** (ljoss: block, lane, parallel, chain UUID) | the document thing a node belongs to | opaque; stored and returned verbatim; never parsed; **not required to be unique** (several nodes may share one element); optional, defaults to the node's `id` |
| `role` | **the engine**, declared per macro | which node of that element this is (`"line"`, `"ts.bass"`, `"split"`) | stable, documented in the schema, never invented by a client that is using macros |
| `id` | **the engine**, mintable | the node's identity inside the graph | optional in a document: a node with `element` + `role` and no `id` gets a deterministic engine-minted id. Unique in the graph; the engine breaks ties itself if a client-supplied id collides with a minted one |

Node object:

```json
{ "element": "b7f3…", "role": "line", "type": "fx.delay.feedback",
  "bypass": false, "params": { "time": 420, "feedback": 0.45 } }
```

`id`, `type`, `bypass`, `element` and `role` are the reserved keys; everything else is a parameter as
today.

What this buys:

- **The app never invents or parses an engine id.** It sends `element` + `role`, receives the rest.
- A macro can lower to any number of nodes without the client's identity being disturbed, and the
  engine can change a lowering without breaking anything the client holds.
- **Dropped:** the earlier idea of a reserved separator character. It is unnecessary — the engine
  mints ids in its own space and guarantees uniqueness itself — and a rule that made client ids
  illegal would have been the tail wagging the dog.
- The engine-minted ids are still *readable* (derived from element and role) because debugging a rig
  dump matters; but they are advisory, returned in `rig.get`, and nothing addresses them.

### 1.3 Addressing: how an op names its target

`rig.patch` ops gain three ways to name a target, in order of preference:

```json
{ "op": "set_param", "chain": "6f1c…", "element": "b7f3…", "param": "feedback", "value": 0.6 }
{ "op": "set_param", "chain": "6f1c…", "element": "p31a…", "role": "mix", "port": 1,
  "param": "level_db", "value": -3 }
{ "op": "set_param", "chain": "6f1c…", "node": "gain2", "param": "gain_db", "value": -6 }
```

- `element` alone addresses the element's *macro-level* parameter (the engine maps it onto whichever
  nodes implement it — see §1.5).
- `element` + `role` addresses one node of that element; `port` addresses a per-port parameter.
- `node` is the existing form, kept for hand-written rigs, scripts and tests.
- `chain` is optional for a single-chain document and required otherwise; a missing or unknown chain
  is an error that **lists the chains that exist**.
- An unresolved target is an error that lists the valid elements/roles/params. **No op ever silently
  does nothing** — that is the whole point of Slice B.

### 1.4 The schema (Slice B), including units, ranges and steps

`engine.schema` returns the engine's catalogue: every node type — primitive **and macro** — with its
ports, parameters and flags. This is the palette, the inspector, and the validator, from one source.

Node type descriptor:

```json
{
  "id": "nc.eq.biquad",
  "kind": "primitive",
  "version": 1,
  "name": "Biquad EQ",
  "category": "eq",
  "flags": { "stateful": true, "has_tail": false, "has_lookback": false,
             "bypassable": true, "block_delay": false },
  "latency_samples": 0,
  "tail_samples": 0,
  "ports": {
    "in":  [ { "name": "in",  "channels": 1, "optional": false } ],
    "out": [ { "name": "out", "channels": 1, "optional": false } ]
  },
  "arity": null,
  "params": [ … ],
  "per_port_params": []
}
```

| Field | Why it is on the wire |
|---|---|
| `kind` | `primitive` or `macro`. A client shows macros (its blocks) and never needs to know a primitive exists |
| `category` | the palette's grouping, engine-owned so a new node type cannot be invisible |
| `flags.stateful` / `has_tail` | explains why bypass keeps a node running, and what a scene change would do to a reverb tail |
| `flags.bypassable` | the compiler already refuses bypass on anything but 1→1; publishing it lets the UI grey the control instead of failing |
| `flags.block_delay` | `nc.unit_delay` is a scheduling primitive; the UI should not offer it as a pedal |
| `latency_samples`, `tail_samples` | what the UI will need for a total-latency readout and the deferred `tail_hold` |
| `ports[].channels` | decision 4: declared, and the compiler refuses > 1 until stereo exists |
| `ports[].optional` | an unconnected input is legal and is bound to silence |
| `arity` | for `nc.split`/`nc.mix`: which param sets the port count, and its bounds — a UI "add lane" control needs exactly this |
| `per_port_params` | the lane mixer: per-input gain/mute/phase, var-arity |
| `version` | lets the engine evolve a node type and lets a client say "this rig needs a newer engine" |

Parameter descriptor:

```json
{
  "id": "freq",
  "name": "Frequency",
  "kind": "number",
  "unit": "Hz",
  "default": 1000,
  "min": 10, "min_inclusive": true,
  "max": null, "max_inclusive": true,
  "step": null, "snap": false,
  "scale": "log",
  "rampable": true,
  "patchable": true,
  "hidden": false,
  "aliases": [],
  "options": null,
  "options_from": null
}
```

Answering your three questions directly, and then the rest:

- **"granularity" or "step"?** Use **`step`**. "Granularity" is ambiguous between "the increment" and
  "how finely it is *displayed*"; `step` has one meaning. It is paired with **`snap`**, because
  "quantised to 0.5" and "displayed to one decimal place" are different things: `step` + `snap: true`
  rounds (or rejects, `snap: false`, which is the honest default for a knob the user can only land on
  exact values). `step: null` means continuous.
- **Range with inclusive/exclusive made explicit:** `min`/`max` plus `min_inclusive`/`max_inclusive`
  (default `true`). Real cases need it — a lowpass at 0 Hz, a Q of 0, a delay of 0 ms are all
  *exclusive* bounds, and a suffix convention in a string (`"10"` vs `"10!"`) is exactly the kind of
  thing that gets mis-parsed.
- **`null` means unbounded *by the schema*, not unlimited.** The engine still has hard limits
  (Nyquist, the delay buffer, `int` frame counts). The distinction is kept in the error: an
  out-of-range value reports `param_out_of_range` with `"limit": "schema"` or `"limit": "engine"`, so
  a client can tell "your UI let the user do something silly" from "the schema did not say".
- **`unit`** is a string (`"Hz"`, `"dB"`, `"ms"`, `"s"`, `"%"`, `"cent"`, `"st"`, `"x"`) or null. It
  is what stopped `util.gain`'s dB knob from being wired to `nc.gain`'s linear one.
- **`kind`** is `number` | `option` | `flag` — your three value kinds, so the inspector picks a
  slider, a picker or a switch without a type table of its own.
- **`options`** carries `{value, label}` pairs, not bare strings: the display label is not the wire
  value (`"Dyn 57"` vs `"dyn57"`) and separating them now avoids a translation table later.
- **`options_from`** is a dynamic option source — `"models"`, `"irs"` — which is how decision 5 plugs
  the device registry into the same schema (`nc.nam`'s `model` param declares `options_from: "models"`
  and the app's picker is the device's library, not a hardcoded list).
- **`rampable`** is the smooth-class flag: true means a change is ramped inside the node with no
  recompile, false means the change is structural and crossfaded. This is what the inspector uses to
  decide stream-versus-commit-on-release, and it is the flag that fixes the crossfade-storm problem.
- **`patchable`** distinguishes a param that can be changed on a live graph from one that is
  build-only. Both are legitimate; conflating them is what produced `gain_db`'s silent structural
  fallback.
- **`hidden`** marks a param a generic inspector should not show (an alias or an internal knob).
  `nc.gain` keeps `gain_db` as the visible, dB, rampable param and `gain` as a hidden linear alias.
- **`aliases`** keeps hand-written rigs working when a param is renamed.
- **`scale`** (`linear` | `log`) is presentation, but it is presentation *the engine knows* (a
  frequency slider is logarithmic) and re-deriving it in every client is how two clients end up
  disagreeing.
- **`default`** is the *effective* default, published. The rule is absolute: **the app never
  substitutes its own default, and a node created with no params must sound identical to a node
  created with every declared default.** That one sentence kills the two-default-tables bug class and
  is a test in Slice B.

The engine also publishes, in `engine.hello` and `engine.status`:

- `features`: `["multi_chain","schema","macros","registry","tcp"]` — growing as slices land. Absence
  is how a client degrades gracefully instead of guessing (and it is how "declare and refuse" reaches
  the UI: a client that sees no `stereo` does not offer a pan control).
- `limits`: max chains, max nodes per chain, max ports, max quantum, supported rates.

**Absence at the type level, refusal at the value level.** A node type that cannot work at all in the
engine's current state (a stereo ping-pong delay) is **not registered**, so it is absent from the
schema and from the palette. A *mode or value* inside a node that exists but is not built
(`nc.split` with `mode: "crossover"`) is **declared and refused** at compile with a clear reason. That
is a coherent reading of decision 4: it is the difference between "this engine has no such pedal" and
"this pedal has a switch that is not wired up yet".

### 1.5 Macros (Slice E)

A macro is a document-level node the engine lowers into primitives. Declared in C++, not in data —
the same vtable style as `NodeType`, with:

- the same schema fields as a node type (`params`, `ports`, `flags`), so a client cannot tell a macro
  from a primitive except by `kind`;
- a **param map**: each macro param → one or more `(role, engine param, transform)`, where transform
  is a small closed set (`direct`, `db_to_linear`, `linear_to_db`, `one_minus`, `scale_by`). No
  templating language, no expressions, no evaluation on the audio thread;
- a **bypass rule**: either "role *X* carries the bypass flag" (when the macro is 1→1) or "lower
  `bypass` into a wet/dry crossfade" (when it is not). This is the rule the comparison flagged as
  unavoidable, and it is decided per macro at declaration time rather than discovered later;
- a **lowering function** producing nodes and connections with `element` = the macro's element and
  `role` from the declaration.

Compile pipeline becomes: parse → validate → **lint params against the schema** → lower → validate
again → memory plan → `RenderPlan`. Macros are lowered *per compile*, not stored lowered, so
`rig.get` returns the document the user wrote.

### 1.6 Health and issues (Slice B)

The small glowing control needs a cheap, stable answer, not English prose to pattern-match. Add to
`engine.status`:

```json
"health": "ok",
"issues": [
  { "code": "device_lost", "severity": "error", "count": 1, "message": "…" },
  { "code": "guard_tripped", "severity": "warn", "chain": "6f1c…", "node": "amp", "count": 2 }
]
```

- `health` is `ok` | `warn` | `error`, derived from the issues, so the glow is one field.
- `severity` is per issue, `code` is **stable and machine-readable** (the UI maps code → text and
  icon; it never parses `message`).
- Issues are *current state*, not a log: `count` accumulates, and an issue disappears when it stops
  being true (a device that comes back clears `device_lost`).
- The initial code set: `device_lost`, `device_restored`, `xrun`, `render_error`, `guard_tripped`,
  `node_failed`, `asset_missing`, `transition_active`, `degraded`, `not_realtime`, `memory_not_locked`.

### 1.7 Errors

Stable codes, because the UI must react to them rather than show them: `unknown_param`,
`param_out_of_range`, `unknown_option`, `unknown_element`, `unknown_role`, `unknown_chain`,
`unknown_node`, `not_patchable`, `not_rampable`, `stereo_not_supported`, `mode_not_supported`,
`macro_unavailable`, `asset_missing`, `compile_failed`, `patch_rejected`, `unauthorized`,
`message_too_large`, `too_many_connections`, `unsupported_protocol`, `bad_json`, `bad_request`,
`unknown_method`, `rig_not_found`, `path_refused`. Each keeps the existing human `message`, which
names the node/element/chain and lists the valid alternatives. `path_refused` is the transport's
addition (slice C): an absolute asset path in a document or patch that arrived over TCP, refused
because a network client is not the operator — see §1.8.

### 1.8 Transports (Slice C)

| | Unix socket (default) | TCP (opt-in) |
|---|---|---|
| Framing | none — `SOCK_SEQPACKET` preserves messages | **u32 little-endian length prefix** + one JSON body |
| Auth | filesystem permissions (`socket_mode`) | **pre-shared token** in `engine.hello`, constant-time compare |
| Trust | local operator | untrusted network; off unless configured |
| Caps | existing | max message bytes (1 MiB), max connections (4), 3 auth failures then close |

The JSON body is identical on both. The daemon refuses to start a TCP listener without a token rather
than exposing an unauthenticated port that can rewrite the rig and mute the output, and refuses a
token shorter than 16 characters, because three attempts per connection is only a bound if guessing is
hopeless. TLS is out of scope; the documented recommendation for anything beyond a trusted LAN is a
tunnel. Two further caps belong with the token: an unauthenticated connection is closed after
`tcp_auth_timeout_ms` (so idle connections cannot hold the table), and pending output is bounded at one
message per connection (so a client that stops reading costs its own connection rather than the control
loop).

A network client is not the local operator, so it may not name an absolute path at all: model and IR
parameters in a document it applies — or in the ops it patches with — resolve relative to `rigs_root`.
This is the one place the two transports differ in what they *allow* rather than in how they carry it,
and it is refused as `path_refused` before anything is compiled.

### 1.9 Who does what

| Job | Owner | Why |
|---|---|---|
| The document (library, tree edits, drag invariants, undo someday) | **app** | it is the only side with a canvas; ljoss's `normalize()` and address model already live there |
| Flattening the tree into a graph (parallel → `nc.split` + lanes + `nc.mix`) | **app** | it is topology, not DSP; the app already knows the lanes, and a split/mix node carries no engine vocabulary the app cannot see in the schema |
| Lowering a block into primitives (EQ bands, feedback delay, tone stack) | **engine** | it is DSP and vocabulary; one implementation, and `rig.get` can return what the user wrote |
| The running plan, transitions, ramps, health | **engine** | as now |
| Assets (models, IRs) | **engine**, listed through the registry | the files are on the device |
| Presentation metadata (subtitle, SF Symbol, source suitability) | **app**, keyed by type id, with a generic fallback | the engine should not know what a guitar is |

**This is the one place I am departing from a strict reading of decision 3**, and it is worth
confirming: *blocks* are lowered by the engine; the *tree topology* is flattened by the app. The
alternative — the engine accepting ljoss's tree as its document model — was seriously considered and
rejected because it would mean the engine owning an ordered-chain/lane/parallel model that exists
purely for authoring, duplicating ljoss's edit algorithms and invariants in C++, and doubling the
document forms the patch protocol must support. The convention the app must honour is small, fixed,
and testable: *a parallel section is one `nc.split` with `outputs = lane count`, one `nc.mix` with
`inputs = lane count`, lane *i* on port *i*, the parallel element id on both nodes, and the lane
params on the mix node's ports.*

---

## 2. Slices

Each slice is a run of commits, each commit green on GCC and clang-20/RTSan, with docs updated in the
same commit that changes the behaviour (the existing convention).

### Slice A — Chained documents and client-owned identity

**Goal:** a document can hold N chains; nodes carry client-owned identity; the protocol can read back
what it is running.

| Commit | Contents |
|---|---|
| **A1** ✅ | IR: `NodeSpec.element` / `NodeSpec.role`, optional `id`, reserved-key list; `Graph` unchanged. Parser: chain envelope with single-graph back-compat. `graph::validate` gains chain context in messages |
| **A2** ✅ | Compiler/daemon: per-chain plan slots, per-chain generation, adoption and transition, staggered multi-chain transitions, channel binding and summing, per-chain mute + global mute/panic. A chain the new document does not have stops immediately, which is stated rather than faded. The `file` backend is wired into the daemon so a document renders end to end offline |
| **A3** ✅ | Protocol 2: `engine.hello` (version range + `features`), `rig.apply` (whole document, in memory, paths resolved against the rigs root), `rig.get` (the document as the engine parsed it + per-chain status), `engine.status.chains`; `chain` on `rig.patch` and `engine.mute`; docs (`docs/daemon.md`, `docs/rig-pipeline.md`, `plan.md`). The *lowered* view in `rig.get` waits for the macro layer, because until then there is nothing to lower |

**Files:** `include/namchain/graph/{graph,rig,patch}.h`, `src/graph/{rig,plan,patch}.cpp`,
`include/namchain/daemon/daemon.h`, `src/daemon/daemon.cpp`, `src/nodes/builtin.cpp` (`nc.output`),
`tests/test_{rig,graph,patch}.cpp`, `tests/test_daemon.cpp`, new `tests/test_document.cpp`.

**Tests, beyond the existing suite staying green:**
- a two-chain document renders through the `null` backend with each chain on its own channel and each
  plan's output present in the right one; two chains into one channel sum;
- a structural patch to chain B leaves chain A's `audible_generation` untouched (the point of
  per-chain generations);
- a document apply that changes both chains transitions them **one at a time** (assert the stagger,
  because it is the thing standing in for the missing admission control);
- `rig.get` round-trips a document: apply, get, apply the result, same generation and same samples;
- a rejected chain names the chain, and the previously running document keeps rendering;
- addressing by `element`, by `element`+`role`+`port`, and by `node`, with an error listing valid
  targets when it misses;
- back-compat: every committed `rigs/*.json` still loads and renders identically.

**Done when:** the starter "Guitar + Vocal" rig is expressible as one namchain document, both chains
audible on separate channels, and a patch to one chain provably does not disturb the other.

**Risk:** the daemon's single-slot transition machinery is the most intricate code being touched.
Mitigation: A2 keeps the transition code shape (one in flight per chain) and only makes the slot a
vector; the staggering is a queue, not a scheduler.

### Slice B — The schema, the catalogue, and no silent parameters

**Goal:** the engine describes itself, and a parameter can never be ignored.

| Commit | Contents |
|---|---|
| **B1** ✅ | `ParamDesc` + node-type metadata on `NodeType`; every built-in declares its schema; `engine.schema` returns it; `docs/schema.md` |
| **B2** ✅ (less the string patch path) | Validation against the schema at load and at patch: unknown param, unknown option, out of range (schema vs engine limit distinguished), missing `required`, unsatisfied `one_of`. Still to do: a patch path for string-valued (option) parameters |
| **B3** ✅ | Smooth coverage. The gains ramp (`nc.gain`'s `gain_db` and its linear alias), and so do the two parameters that cannot simply be interpolated: a biquad runs the old and new coefficient sets in two lanes and equal-power crossfades them (interpolating coefficients is not guaranteed stable and is not the response anyone asked for mid-move), and a delay crossfades between two read taps (interpolating the position would be a varispeed slide). The incoming filter lane inherits the outgoing one's state, so a high-Q change does not start deaf. The delay line's length is now declared by the rig (`max_ms`/`max_samples`), because the render path may not allocate and a ring sized for the built time would wrap if the time were lengthened. Caveat, measured and written up in docs/transitions.md: the glitch detector's continuity limit is exactly the +6 dB comb peak a tap crossfade produces on correlated material, and its HF rule needs material with HF in it |
| **B4** ✅ | `nc.mix` gains per-port `level_db`/`mute`/`phase`, addressed by port, ramped like any gain, and refused by name when the port does not exist — a port-addressed op has no structural fallback, because a per-port parameter lives in the node's state rather than in the graph. Ports declare `channels` and non-mono is refused (B1). Still open from this row: `nc.split`'s `mode` (the `y`/`aB`/`crossover`/`dynamic` split), which is a DSP decision rather than a schema one and is not needed until a client asks for a crossover |
| **B5** ✅ | `health` + `issues` in `engine.status`: stable codes, per-issue severity, `health` derived from the worst of them, and matters that are *current state* rather than a log — the xrun and error signals go through a leaky bucket so they can clear, which is what makes the light worth showing. The code list is in docs/daemon.md |

**Tests, beyond the above:**
- **the defaults test**: for every node type, a node created with no params renders sample-identical
  to one created with every declared default — the two-default-tables bug class, closed by
  construction;
- **the no-dead-params test**: every declared, non-hidden param either has a targeted assertion that
  setting it away from its default changes the rendered output (or the ramp, for `ramp_ms`) or is in
  an explicit exception table with a stated reason. A declared-but-ignored param fails the build;
- every rampable param, set while a tone is playing, passes the existing glitch detector's continuity
  and slope rules; every non-rampable param is refused by the smooth router and takes the structural
  path;
- range tests including exclusive bounds (Q > 0, frequency > 0) and the `limit` field;
- a `set_param` to an unknown param or with an unknown option is rejected with the valid list;
- `engine.schema` covers every registered type, and every type's schema parses as documented.

**Done when:** the app can build its palette and a generic inspector from `engine.schema` alone, and
there is no input a client can send that produces a knob which does nothing.

This is the highest-value slice in the plan: it is the one that removes a whole class of user-visible
lie, and it unblocks the app's inspector.

### Slice C — TCP transport

| Commit | Contents |
|---|---|
| **C1** ✅ | Framing (`include/namchain/daemon/framing.h`, shared by daemon, client and tests) + non-blocking acceptor and per-connection in/out buffers in the existing `poll` loop; `tcp_listen`, `tcp_address`, `tcp_port`, `tcp_max_connections`, `tcp_max_message_bytes`, `tcp_auth_timeout_ms`; `control` in `engine.status` |
| **C2** ✅ | Token in `engine.hello` (constant-time, checked before the version), the unauthorised state machine (three failures then close, an authentication deadline, `too_many_connections`), refusal to start without a token or with one under 16 characters, `tcp_token_file` with a permissions warning, `path_refused` for an absolute path from a network client; `docs/daemon.md` transport + threat model |
| **C3** ✅ | `tools/namchainctl` (hello/status/schema/get/apply/patch/set/mute/panic over either transport), `tests/test_tcp.cpp`, and `tests/cli/run_ctl.cmake` (the daemon as a process, driven by the real client) |

**Tests, as built:** a framed round trip over loopback; a message delivered one byte at a time
reassembles, and two messages in one write answer twice in order; a partial write to a slow reader does
not stall the control loop (a second client is served while the first has stopped reading); an
oversized message is refused with `message_too_large` from its header, before a byte of it is buffered;
wrong token → `unauthorized` and nothing else, three times, then the connection closes; the
authentication deadline releases the slot; two concurrent clients interleave without mixing up; a
client that vanishes mid-message is forgotten; non-JSON is `bad_json` rather than fatal; the two
transports answer the same request identically; an absolute path is refused remotely and accepted
locally; and the UDS tests are unchanged.

**Done when:** a machine on the LAN can drive the daemon with a token, and `namchainctl` is the
reference for whoever writes the Swift client. **Done:** `namchainctl --host 127.0.0.1 --port … --token-file
… set --node g --param gain_db --value -6` answers `{"kind":"smooth"}` against a real daemon, and one
correction to the client (it sent `{"method":"hello"}`) is exactly what `ctl_cli` exists to catch.

### Slice D — The asset registry

| Commit | Contents |
|---|---|
| **D1** ✅ | `models_root` / `irs_root` config, a cached scan (id, relative path, kind, size, mtime, sample rate and architecture when the file's header says so), `engine.assets` with `{"refresh": true}`, `engine.status.assets`, `SIGHUP` rescanning, and `src/assets/registry.cpp` |
| **D2** ✅ | `options_from: "models"` / `"irs"` resolved through the same scan in `engine.schema` — each such parameter carries `options: [{value: id, label: id}]` beside `options_from` |
| **D3** ✅ | Model/IR params resolve by library id, then as a path; a miss is `asset_missing` with the registry's near misses. **Revised order:** a reference that names an *existing file relative to the document* is resolved before the library, because the library going first would let a device's entry shadow the model a rig was written against — silently — and would make the daemon and the offline tools (which have no registry) resolve the same rig differently. The two orders agree wherever both can answer |

**Tests, as built:** fixture models and IRs (written by the test, because the registry never loads a
model); scan shape and ordering; the cache asserted through `ScanStats::parsed` (0 on an unchanged
refresh, 1 for a same-size edit in the same second); a file added after startup appearing only on
refresh; resolution by id, by relative filename, by a path relative to the document and by an absolute
path; an unknown id refused with the near miss named; a missing root as an empty library with an
informational issue rather than a failure; a symlink out of the root skipped; a document's asset
parameters rewritten while a non-asset string is left alone; a real model named by id compiling into a
plan against the generated fixtures (tests/test_daemon.cpp); and, over TCP, a remote client applying a
document that names a model by id while the absolute path to the very same file is `path_refused`
(tests/test_tcp.cpp).

### Slice E — Lowering and the first macros

| Commit | Contents |
|---|---|
| **E1** | `MacroType` infrastructure: declaration, param map with transforms, deterministic id minting, provenance, macro-level `set_param` (smooth iff every target is rampable, one ring record per target in one drain), the bypass rule, macro-level lint |
| **E2** | `fx.eq.bands`, `fx.eq.tilt`, `fx.filter.cut`, `fx.mix.drywet` |
| **E3** | `fx.delay.feedback`, `fx.amp.tonestack`, `fx.cab.ir` |

**Tests:** for each macro, the lowered shape (nodes, roles, edges) and an **equivalence test** — the
macro's output is sample-identical to the hand-built graph it stands for, which is the same
oracle-style argument the compiler already uses; every declared macro param is consumed; every
rampable macro param ramps through to all its targets; bypass behaves per the declared rule; a macro
addressed by element and one addressed by the same param on the underlying node produce the same
result.

**Done when:** ljoss's first-macro blocks (parametric/graphic/shelving EQ, low/high cut with slope,
dry/wet, feedback delay, amp with tone stack, cab with IR and cuts) are renderable from the schema,
and no DSP beyond biquads, gains, mixes and the existing delay line was written.

---

## 3. What ljoss needs (after the engine slices, not before)

Listed so the contract can be checked against it, and because it is the other half of the work.

| App change | Enabled by |
|---|---|
| `EngineClient`: TCP + framing + token; hello/status/schema/assets/apply/patch/get — `tools/namchainctl/namchainctl.cpp` is the worked example | C |
| `DocumentEncoder`: rig tree → engine document (chains, element ids, split/mix flattening, macros) | A, E |
| Palette and inspector driven by `engine.schema` (+ a local presentation side table keyed by type id, with a generic fallback for an unknown type) | B |
| `isEnabled` → **bypass**: tile dimmed but alive; bypass toggles are one structural commit | A, B |
| Slider streams only when `rampable`; other params commit on release; "pending" until `params_applied` | B |
| Drag is local, drop is one commit; the whole document goes as one `rig.apply` | A |
| Amps and cabs listed from `engine.assets` — ids to send back, with `group` for the palette's folders | D |
| Hardware params in a labelled out-of-band section, never sent | A |
| Health control: a small glow bound to `health`, expanding to `issues` on tap | B |
| `layout` stays a viewing preference; a hidden chain is explicitly muted and shown as muted (see open question 3) | A |
| Chain mute in the chain header; `engine.mute {"chain": …}` | A |
| Offline mode: design with no engine attached, and reconcile on connect | A |

---

## 4. Out of scope, explicitly

The 102-block DSP library; tuner, looper, FX loop, DI; scenes (client-side parameter sets only, and
not yet); device control (phantom, pad, impedance, destination); sidechain and any audio-rate
modulation; stereo *implementation* (declared only); NAM warm start (M3 proper — this plan builds the
registry it needs); admission control and the cost model (still on the roadmap, and now more
important — see the risk below); PDC; the offline timeline gaining structural ops; undo in either
tree; and any serialisation change beyond the envelope in §1.1.

## 5. Risks

1. **No admission control, now with N chains.** A document apply that changes several chains could
   dual-render all of them. Mitigation: staggered per-chain transitions (A2), asserted by a test. The
   cost model stays outstanding and is the first thing to promote after this plan.
2. **The flattening convention lives in the app.** If the app emits the split/mix topology, the
   convention can drift. Mitigation: a committed fixture document that a namchain test compiles, plus
   the arity/port facts in the schema so the app is not guessing.
3. **Protocol churn while the app is written.** Mitigation: `features` + the protocol version, and the
   rule that responses are supersets; a client that ignores unknown fields keeps working.
4. **TCP exposure.** Mitigation: off by default, mandatory token, caps, and an honest threat-model
   section; a tunnel is the documented answer off-LAN.
5. **Slice A is the most intricate code being changed.** Mitigation: the transition mechanism keeps
   its shape; the slot becomes a vector; staggering is a queue.

## 6. Open sub-decisions (recommendations attached)

1. **Tree flattening in the app** (§1.9) — confirm, or say the engine should own a tree document.
2. **`nc.split` gains a `mode` param** rather than new node types. Recommended: yes; the shape (1→N)
   is identical and only what each output carries differs.
3. **A hidden chain**: should the app *mute* it (visible) or leave it audible? Recommended: mute it,
   shown as muted, because a layout tap that silently changes what is audible is the failure ljoss's
   grow-only layout was designed to avoid.
4. **Summing two chains on one output channel**: recommended, with clipping at the device write.
5. **`rig.get` is a diagnostic, not a document store** — the engine returns its own parse and drops
   unknown metadata, and the app must never rehydrate its library from it. Recommended: yes, with
   `rig.get` echoing the document generously for debugging but documented as non-authoritative.
6. **`element` defaults to `id`** so hand-written rigs need no new field. Recommended: yes.
7. ~~**`nc.output` params** (`level_db`, `mute`) as document params, while *chain* mute is runtime
   state.~~ **Revised, and implemented that way:** an output fader is a `nc.gain` node in the
   flattened graph, and chain silence is the engine's per-chain mute. The reason is the compiler's
   output binding: the plan's output *is* the buffer feeding `nc.output`'s input port
   (`src/graph/plan.cpp`), which may also feed another consumer, so a gain on that node would have to
   scale a shared buffer in place. Making `nc.output` 1→1 to give it its own buffer would change how
   the plan's output is bound, for a knob a `nc.gain` already provides — ramped, and with a schema
   entry of its own. The fader still exists; it is one node further up.
8. **`nc.gain`'s canonical param becomes `gain_db`** (dB, rampable, visible) with the existing linear
   `gain` kept as a hidden rampable alias — the reverse of today, where `gain` is the settable one and
   `gain_db` is build-only. Recommended: yes, because dB is the unit a player thinks in and the alias
   rule keeps existing rigs working; but it does change which param the timeline and the tests name,
   so it is worth an explicit yes.

## 7. Order and the first commit

```
A  chained documents + identity + rig.apply/rig.get      ← start here
B  schema, catalogue, no silent params, health/issues
C  TCP transport + namchainctl
D  asset registry
E  lowering + first macros
```

Slice B is the one the app most needs for *quality*; Slice A is the one it needs for *shape*, and it
is the one whose design is most expensive to change later — which is why A is first, per decision 2.

**First commit (A1):** IR and parser only — `element`/`role`/optional `id` on `NodeSpec`, the chain
envelope with single-graph back-compat, chain-aware validation messages, and the tests for all of it.
No daemon change yet, so it lands as a pure, reviewable contract change.
