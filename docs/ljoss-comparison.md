# namchain × ljoss: what each is, where they meet, and what the first implementation should take from each

**Status:** analysis, not a plan of record. Written against namchain `799688e` and ljoss `7618314`.
No code changed. Every claim about namchain below is checkable in this tree; §9 lists the commands.
The final section (§10) is the list of decisions this analysis needs from you.

---

## 0. The one-paragraph version

They are not two implementations of the same thing, so a feature-by-feature "who has more" comparison
answers the wrong question. ljoss is the **front half** — a SwiftUI authoring tool with a rich rig
*document* (two chains, parallel sections, 102 catalogued blocks, per-lane mixing) and no audio at
all. namchain is the **back half** — a headless real-time engine with a rigorous *execution* model
(compile, crossfade, ramp, guard, never-silence) and a thin document (one chain, eleven primitive
node types, almost no authoring vocabulary). Almost everything ljoss models, namchain cannot yet
execute; almost everything namchain guarantees, ljoss has no concept of. They are compatible in
*shape* — ljoss's series/parallel tree embeds exactly into namchain's DAG, and ljoss's three value
kinds embed into namchain's number/string params — and incompatible in *granularity*: ljoss's unit of
authoring is a block with a dozen parameters; namchain's unit of execution is a node with one. The
missing layer between them is one namchain's own plan already names and has not built: **lowering**.

---

## 1. What each thing actually is

### 1.1 namchain (`799688e`)

A C++20 real-time guitar-chain engine and its reference daemon, for a headless Linux box (Pi/board,
ALSA, systemd unit). The truth is the compiled `RenderPlan`: a topological node array with every
buffer and pointer bound at compile time, executed without allocation, locks or syscalls.

| | |
|---|---|
| Document | one rig spec = one `Graph` = **one chain**. Enforced: *"a chain needs exactly one nc.input, found N"* (`src/graph/plan.cpp:231–239`) |
| Node vocabulary | 11 types: `nc.input`, `nc.output`, `nc.gain`, `nc.split`, `nc.mix`, `nc.bypass`, `nc.nam`, `nc.cab.ir`, `nc.eq.biquad`, `nc.delay`, `nc.unit_delay` |
| Routing | arbitrary DAG; cycles via `nc.unit_delay`; N-way `nc.split`/`nc.mix` (1–64 ports, per-spec arity) |
| Channels | **mono.** `plan.h`: *"Buffers are mono for now; per-port channel counts arrive with the multichannel work."* The daemon decodes device channel 0 and writes channel 0 |
| Parameters | a name → double map plus a name → string map. No schema, no ranges, no units, no defaults published, **no rejection of unknown names** (verified: a node with `"gainz": 3.0` loads clean) |
| Rampable params | **one, engine-wide**: `nc.gain.gain` (`src/nodes/builtin.cpp:166`) |
| Patchable values | `rig.patch`'s `set_param` takes **numbers only** (`src/graph/patch.cpp:280`); string/option params have no patch path except `swap_model`, which is hardcoded to `nc.nam`/`nc.cab.ir` |
| Edit classes | smooth (ramp in-node, no recompile) vs structural (build new plan, dual-render, equal-power crossfade, retire) — built, with one transition in flight and a coalescing pending slot |
| Client protocol | versioned JSON over `AF_UNIX`/`SOCK_SEQPACKET` only (`src/daemon/daemon.cpp:172`). `engine.hello/status/mute/panic`, `rig.load/get/patch`. `rig.get` returns path + generation + loaded — **not the graph**. No write-back to disk. No telemetry subscription |
| Health telemetry | rich: device state, xruns, device lost/restored, mute, RT policy, retired-pending, `params_applied`/`params_stale`, `failed_nodes`/`guard_trips`, transition state, audible/target generation |

### 1.2 ljoss (`7618314`)

A SwiftUI signal-chain designer for guitar, bass and vocal rigs — macOS 27+ / iOS 27+, one
multiplatform target, `Model/` free of SwiftUI so it compiles with plain `swiftc` for its tests. It
contains **no audio code, no networking, no export/import and no undo** (verified by grep, §9).

| | |
|---|---|
| Document | `Rig` = name, layout (`single`/`dual`), createdAt/modifiedAt, and 1–2 `SignalChain`s. Each chain: name, `source: SourceKind` (guitar/bass/vocal), an `input` and an `output` `IONode` carrying parameters, and `[ChainElement]` |
| Routing | a **series/parallel tree**, deliberately not a DAG: `ChainElement = .block(Block) | .parallel(ParallelSection)`, `ParallelSection.lanes: [Lane]`, `Lane.elements: [ChainElement]`. Depth capped at 1 (`Rig.maxParallelDepth`) |
| Addressing | `ContainerPath(chainID, [LaneStep(parallelID: UUID, laneIndex)])` + index; `InsertionSlot` vs `ElementAddress`; a separate `RenderSlot` for what is on screen mid-drag. Parallels are addressed by **UUID**, so an earlier removal cannot invalidate a later path |
| Split semantics | `SplitMode { yCable, aB, crossover, dynamic }` + `crossoverHz`/`balance`; per-lane `level`, `pan`, `isMuted`, `invertPhase` |
| Blocks | 102 `ComponentSpec`s in 12 categories, each with id, name, subtitle, category, `sources`, SF Symbol and **typed parameters** (`ParamKind = continuous | stepped | options | toggle`, with min/max/step/options, default and unit) |
| Instances | `Block { id: UUID, componentID: String, isEnabled: Bool, values: [String: ParamValue] }`; a missing value falls back to the catalogue default; an unknown `componentID` renders as an "Unknown Block" placeholder rather than failing the load |
| Invariants | `normalize()` after every mutation: <2 lanes → splice back to series; all lanes empty → prune the section; an *individually* empty lane is preserved (the dry path) |
| Persistence | one JSON file per rig under `~/Library/Application Support/RigDesigner/Rigs/`, atomic write, 400 ms debounce, flush on backgrounding. Grow-only layout: hiding chain 2 never deletes it |

### 1.3 Why the overlap is smaller than it looks

ljoss's 102 blocks are **specifications, not implementations** — a name, a category, an icon and a
parameter list. namchain's 11 node types are implementations with no presentation metadata at all.
Measured against each other:

| | ljoss | namchain |
|---|---|---|
| Named units of "an effect" | 102 | 11 |
| With DSP behind them | 0 | 11 |
| With a declared parameter schema | 102 | 0 |
| Direct 1:1 mapping between the two lists | — | **1** (`util.gain` → `nc.gain`, and only after a dB→linear conversion) |

That last row is the whole problem in one number. Everything else is a consequence of it.

---

## 2. Feature comparison, axis by axis

| Axis | ljoss | namchain | Verdict |
|---|---|---|---|
| Top-level container | 1–2 chains per rig, each with its own input/output | exactly 1 chain per document | **Conflict** — needs the `Rig = {chains}` superset plan §5.1 already reserves |
| Chain endpoint I/O | `IONode` params: source select, gain, impedance, pad, phantom, low cut / destination, level, pan, mono | `nc.input`/`nc.output` are parameterless terminators; device mapping is global daemon config | **Partial** — digital trim and low cut map; the rest is hardware or absent |
| Signal shape | series/parallel tree, depth ≤ 1 | arbitrary DAG, any depth, fan-in/fan-out, N-way split/mix | **Compatible** — tree ⊂ DAG, exactly. The reverse needs a recogniser |
| Split behaviour | 4 modes, 2 of which are frequency/level dependent | `nc.split` = plain 1→N fan-out; no params | **Conflict of kind** — Y split maps; crossover and dynamic are DSP, not routing |
| Merge/mix | per-lane level, pan, mute, invert phase | `nc.mix` = unity-gain sum, **no params at all** | **Conflict** — level/mute/phase need settable mix params (or a per-lane gain node); pan needs stereo |
| Channels | stereo intent throughout (lane pan, output pan/mono, ping-pong, auto-pan, stereo width, dual chains) | mono end to end | **Conflict** — the deepest one; pan-dependent blocks are unimplementable as specified |
| Block granularity | 1 block = 1 tile = 1 concept (amp with 6 knobs, 3-band EQ, delay with feedback) | 1 node = 1 primitive (one biquad, one bare delay line) | **Conflict** — needs a macro/lowering layer |
| Parameters | typed (number/option/flag), kind, range, step, options, default, unit | numeric or string; unvalidated; no ranges/units/defaults published | **Compatible values, missing schema** |
| Continuous edit | slider writes on every frame | only `nc.gain.gain` ramps; everything else is a structural crossfade | **Conflict today** — a knob drag becomes a transition storm |
| Discrete edit | picker/toggle | numeric only, or `swap_model` for `model`/`ir` | **Conflict** — an `options` param cannot be patched at all |
| Enable/disable | `Block.isEnabled` (a UI flag with no semantics yet) | `NodeSpec::bypass`: node keeps running, output replaced by dry input, crossfaded, 1→1 only | **Compatible, and namchain's is the better semantics** |
| Edit atomicity | instant local mutation, then `normalize()` | all-or-nothing patch on a copy; failure leaves the running plan untouched | **Compatible in spirit** — document invariants vs graph validation |
| Undo | none | none | Same gap on both sides |
| Truth after an edit | the document (it is the only copy) | the plan; the document is not updated and cannot be read back | **Conflict** — `rig.get` doesn't return the graph and nothing is written to disk |
| Assets | catalogue id + block-local `slot`/`mic` picks; no files | file paths in string params, resolved against the rig's directory | **Conflict** — needs a device-side model/IR registry as the palette's source |
| Transport | none; macOS/iOS app | `AF_UNIX`/`SOCK_SEQPACKET` on the engine host | **Conflict** — an iOS app cannot reach it |
| Health/latency/cost | no concept | xruns, device loss, guard trips, latency declarations, cost model (planned) | namchain has it; the UI has nowhere to show it |
| Scenes/footswitches | none (a rig is the only level) | planned, not built; the smooth op class is the mechanism | **Compatible** — a scene is a client-side set of `set_param` ops today |

---

## 3. The structural differences, in the order they cost money

### 3.1 Granularity: 102 blocks vs 11 primitives — the missing lowering stage

This is the largest gap and the one that explains most of the others.

`nc.delay` is a bare delay line with no feedback and no mix. The engine's own comment says what a
feedback delay therefore is:

> *"A plain fixed delay, which is also the only way to write feedback: §5.1 makes a cycle a compile
> error, so a feedback loop is `split, delay, gain, mix` with the delay breaking the cycle."*
> — `src/nodes/builtin.cpp`, `nc.delay`

So ljoss's `delay.tape` (time, feedback, wow, age, mix) is **five engine nodes and a cycle** in
namchain, not one. The same is true of every composite block in the catalogue: a 3-band parametric
EQ is three `nc.eq.biquad`s; a 24 dB/oct low cut is four cascaded highpass biquads (the `slope` pick
*is* a count); a crossover split is a lowpass lane and a highpass lane; an amp is a NAM model plus a
tone stack that the model does not have.

namchain's own compile pipeline already has a stage for this and it is not built:

> *"`JSON rig` → parse/validate → IR → lint → **lower** (expand `nc.nam` slots, insert
> resamplers/PDC delays, insert mixers for fades) → memory plan → cost model → immutable
> `RenderPlan`"* — plan.md §5.2

**Lowering is where the two projects can meet without either being redesigned.** ljoss's catalogue is
the specification of what it must be able to lower; namchain's primitives are what it lowers to. The
alternative — teaching the UI to emit raw graphs — fails for three reasons: every client
re-implements it, the engine can no longer say what a document meant, and `rig.get` cannot return
what the user wrote.

A worked example, using only node types that exist today:

```
ljoss document                              lowered namchain graph
────────────────────────────────────────    ─────────────────────────────────────────────────────
chain "Guitar"                              nodes
  amp.britplexi      (b1)                      in                     nc.input
  cab.412brit        (b2)                      b1:nam                 nc.nam      model=…
  parallel           (p1)                      b1:ts.bass             nc.eq.biquad mode=lowshelf …
    lane 0: delay.tape (b3)                    b1:ts.mid              nc.eq.biquad mode=peaking  …
    lane 1: (dry)                              b2:ir                  nc.cab.ir   ir=…
  eq.highcut         (b4)                      b2:level               nc.gain     gain_db=0
                                               p1:split               nc.split    outputs=2
                                               b3:line                nc.delay    ms=420
                                               b3:fb                  nc.gain     gain=0.5
                                               b3:sum                 nc.mix      inputs=2
                                               p1:mix                 nc.mix      inputs=2
                                               b4:hp                  nc.eq.biquad mode=highpass freq=8000
                                               out                    nc.output
                                            connections
                                               in→b1:nam→…→b2:level→p1:split
                                               p1:split[0]→b3:line→b3:sum[0]   (feedback tap:
                                               p1:split[1]→p1:mix[1]            b3:sum→b3:fb→b3:sum[1])
                                               p1:mix→b4:hp→out
```

Four ljoss blocks became fifteen engine nodes. The block UUIDs survive as id prefixes, which is what
makes the graph recognisable and re-renderable as a tree on the way back.

Two design consequences to settle when lowering is designed:

1. **Bypass of a macro.** `NodeSpec::bypass` works only for 1→1 nodes, and a macro is often not 1→1
   (a feedback delay is 1→1 in the small, fine; a parallel section is not). So the lowering must
   either guarantee a 1→1 "outer" node to carry the flag, or lower `isEnabled = false` into an
   explicit wet/dry crossfade inside the macro. Deciding this per macro is unavoidable; deciding it
   once, as a rule ("a macro declares one node as its bypass point, or declares itself
   crossfade-bypassed"), is the design work.
2. **Composite parameters.** `balance` on an A/B split is one UI knob driving two engine gains; a
   tone stack's `mid` may drive several biquads. A macro's params are therefore not a subset of its
   nodes' params — the client should send the macro-level param and let lowering derive the rest,
   which means `rig.patch` needs to address macro params too, not only engine params.

### 3.2 The container: two chains vs one

ljoss's headline dual rig — "Guitar + Vocal", a starter rig, two fully independent chains each with
its own input and output — has no expressible form in namchain today:

> *"a chain needs exactly one nc.input, found 3"* — `src/graph/plan.cpp:237`

The two chains are **not** a split of one signal: each has its own physical input (`source`:
Instrument vs Mic), its own parameters (pad, phantom, impedance), and its own output destination
(`Main L/R`, `Amp Out`, `Send 1/2`). They are two signal paths that happen to live in one document
and one UI. Modelling them as one graph with a split would be wrong in every respect.

The good news is that this is the *cheapest* of the big conflicts, because it needs no new DSP:

- The document becomes `Rig = { chains: [Graph], … }` with each chain's `nc.input`/`nc.output` bound
  to `{device, channel}` or a bus — which is exactly what plan.md §5.1 already reserves.
- The compiler already compiles one graph; it now compiles N. The daemon's `PlanSlot` becomes a
  vector; the generation becomes per-chain (plan §5.3 already says *"one structural transition in
  flight **per chain**"*), or a set generation plus per-chain sub-generations.
- `engine.status` grows a per-chain array: state, transition, failed nodes.

Cost is in the daemon and the transition bookkeeping, not in the audio path. It is a bounded change,
and it is a prerequisite for the UI to mean anything.

One question it forces, worth deciding deliberately: **does hiding a chain stop it rendering?** In
ljoss, `layout: single` hides chain 2 but keeps it in the document; that is a *viewing* preference
and must not silently change what is audible. namchain has no "hidden" state. So: the engine's chain
set comes from the document, the canvas decides what is drawn, and silencing a chain is an explicit,
visible `mute` (which the engine also does not have per chain — it has one global mute). That gives
the engine three small additions: per-chain mute, per-chain status, per-chain generation.

### 3.3 Channels: stereo intent, mono engine

ljoss pans in six places (lane, output, auto-pan, doubler, ping-pong, `util.pan`, stereo widths) and
its dual-chain rig implies at least two output paths. namchain is mono end to end, and says so:
*"Buffers are mono for now; per-port channel counts arrive with the multichannel work"* (`plan.h`).

This is not a mapping problem; it is a type problem. Making ports multichannel touches the buffer
plan, the ports/arity model, the memory colouring, the reference interpreter, the crossfade, the
guard, `nc.mix`'s summing law, and the daemon's device encoding. It is the single most expensive
item in this document.

My recommendation is to **declare it before building it**: put the port channel count in the node
schema on the wire now (so the palette can say "this block needs a stereo path" and the compiler can
refuse with *"stereo ports are not built yet"* rather than mis-rendering), and implement stereo for
the handful of nodes that need it — `nc.pan`, `nc.mix`, `nc.cab.ir` (stereo IRs), `nc.output` — when
a stereo feature is actually the thing blocking a release. Meanwhile ljoss's stereo-only blocks
(ping-pong, auto-pan, doubler, dual delay, stereo IRs) are honestly marked unavailable rather than
silently collapsed to mono.

### 3.4 Split modes are DSP, not routing

| ljoss mode | what it is | namchain today |
|---|---|---|
| `yCable` | fan out the same signal to every lane | `nc.split` — **exact match** |
| `aB` | balance between lane 0 and lane 1 | two `nc.mix` input gains driven by one knob — needs settable mix params |
| `crossover` | low band to lane 0, high band to lane 1, at `crossoverHz` | a lowering: `split → lowpass lane / highpass lane → mix`. A single biquad per band will **not** sum flat at the crossover; a proper Linkwitz-Riley needs cascaded biquads, so this belongs in the macro layer with real DSP behind it |
| `dynamic` | distribution driven by level | a node that reads a detector and drives two complementary gains; namchain has no audio-rate modulation of anything, and params are control-rate ramps. Expressible only as a new node type (`1→2`, two internal gains) — which is cheap — not as a general sidechain system, which is not |

So of the four modes, one is already the engine's split, two are lowerings, and one is a new node
type. None of them is a property of a *connection*, which is what namchain's split is. Keeping the
four modes as one UI affordance and letting lowering pick the shape is right; the UI should stop
thinking of the mode as belonging to the parallel section's *routing*.

### 3.5 Parameters: a typed schema vs a name-lookup map

Mapping ljoss's `ParamValue`/`ParamKind` onto namchain's params works cleanly:

| ljoss | namchain | notes |
|---|---|---|
| `.number(Double)` continuous | numeric param | rampable if the node declares it |
| `.number` stepped | numeric param + `step` in the schema | the engine does not care about the step; the UI enforces it |
| `.flag(Bool)` toggle | numeric `0`/`1` | works today |
| `.option(String)` | **string param** (`NodeSpec::strings`) | **cannot be patched at all today** — `set_param` requires `is_number()`, and `swap_model` only knows `nc.nam`/`nc.cab.ir` |

Three real defects follow, all of which a UI turns from theoretical into daily:

1. **An unknown parameter name is silently accepted.** I loaded a rig with `"gainz": 3.0` where
   `"gain"` was meant and the daemon published it happily. In a hand-written rig that is a typo in a
   text file; from a UI it is *a knob that does nothing*, which is the worst failure mode a control
   surface can have. Unknown params should be a compile error (or at minimum a warning list in the
   response), which requires the schema to exist.
2. **Defaults live in two places.** ljoss falls back to the catalogue's `defaultValue`; namchain's
   node `create` falls back to its own default (`number_param(spec, "gain", 1.0)`). Nothing keeps
   those numbers equal, and when they diverge a rig *sounds different in the app than in the
   engine*. One declared default, published in the schema, removes the class.
3. **Units and scales are invisible.** ljoss's `util.gain` is a dB knob (−24…+24); `nc.gain`'s
   ramped param is **linear** and its `gain_db` is build-time only. A client cannot know that from
   the wire, so it either guesses or gets it wrong. `unit` and `kind` in the schema is the fix.

And the ramping gap: only `nc.gain.gain` is settable (`settable_params`, `builtin.cpp:166`). Every
other continuous parameter — EQ frequency and gain, delay time, mix levels — can only be changed by
a **structural** edit: build a new plan, dual-render it, crossfade, retire. The inspector binds a
SwiftUI `Slider` straight to the model value, so a single drag emits a continuous stream of updates.
Wired to the daemon as-is, one knob sweep is a crossfade storm: dozens of compiles, a generation
counter in the hundreds, and a permanent dual-render. The op-class router is doing exactly what it
was designed to do; the coverage of the `smooth` class is what is missing.

This is the highest-value, lowest-risk work in the whole comparison, and it is already specified:
plan.md §6.1's ABI sketch has `params_schema_json` (*"name, id, range, default, smoothing class"*)
and an `RT_PARAM_SAFE` flag.

### 3.6 Where the truth lives: plan, document, and the missing read-back

namchain's arrangement is deliberate and correct: the control thread compiles and publishes, the
render thread swaps an index, the old plan is retired, and **the document on disk is never
rewritten**. `rig.patch` mutates the running graph only; after a `SIGHUP` reload, every patch is
gone. `rig.get` returns the path, the generation and `loaded` — not the graph.

For a design tool this is a problem in both directions:

- **Push**: the app authors a document; the protocol can only name a file under `rigs_root`, or send
  a list of graph ops. There is no "here is the document I authored" path. Building a tree insert
  out of `connect`/`disconnect`/`rewire` is client-side graph arithmetic that every client would
  re-implement, and it has to maintain `nc.split`/`nc.mix` arity params by hand.
- **Pull**: the app cannot read back the graph, so after another client's patch or an operator's
  `systemctl reload`, the app's canvas and the engine's plan have silently diverged and nothing can
  detect it.

Minimum fix, and it is small: a whole-document apply (`rig.apply {spec}` — or `rig.load` accepting an
inline spec) plus `rig.get` returning the current graph (nodes with type, params, bypass; plus
connections). One wrinkle to design with it: a `model` param is a *path*, resolved relative to the
rig spec's directory today. An inline spec has no directory, so the request needs an explicit asset
base (a configured models/IR root) or the daemon resolves everything against one.

The `generation`/`adopted_generation`/`params_stale` machinery already gives a client what it needs
to reconcile asynchronously: a patch is an acknowledgement, not a completion, and a parameter that
arrives after its target generation has moved on is counted as stale rather than silently applied to
the wrong plan. That is a *better* contract than most UIs get, and the app should be built to use it
rather than to assume synchronous success.

### 3.7 Assets: catalogue ids vs file paths, and the device registry

A ljoss block says `amp.britplexi`. A namchain `nc.nam` node says `"model": "BritPlexi.nam"` — a file
that must exist where the engine can read it, and whose absence is a load-time failure. So:

- The **amp and cab palettes cannot be static catalogues**. Ten amp names in Swift are not ten amps;
  they are ten entries in a table that has no relationship to what is on the device. The amp list
  the user picks from must come from the engine: a model registry (`engine.models`) with names,
  sample rates and metadata, which is also where M3's warm-start work lands.
- The **IR loader's `slot` pick** is the same idea for IRs.
- The **mic/distance/axis** parameters on ljoss's cab blocks are not parameters at all in this
  architecture: they are *different IR files*. Either the cab palette exposes a real IR library, or
  those knobs are presentation over a fixed set of captures.
- `lowcut`/`highcut`/`level` on a cab *are* engine params (biquads and a gain), so a cab macro has
  both a device-library part and a renderable part.

Everything else about persistence is compatible as-is, and the division of labour is clear: **the app
owns the library and the document; the engine owns the running plan and the assets.** ljoss's
one-file-per-rig, unknown-component-placeholder, missing-param-defaults decisions are all good and
should survive; they are exactly the robustness the document layer needs. Nothing about the
serialised format needs to change for functional reasons — only an envelope is needed so that one
document can carry rig metadata plus N chains instead of exactly one graph. (Per your note, that is
the only format point in this document.)

### 3.8 Transport: `AF_UNIX` on a Pi vs a SwiftUI app on a phone

namchaind listens on `AF_UNIX`/`SOCK_SEQPACKET` and nothing else. ljoss has no networking code at
all. An iPhone or iPad cannot reach a Unix socket on a Raspberry Pi, so the intended pairing does not
work today, and this is a real functional gap rather than a packaging detail:

- **Framing.** The protocol's "one JSON message per `SEQPACKET` datagram" is a property of the socket
  type. Over TCP it needs a length prefix or newline-delimited JSON. That is a protocol change, not
  a format debate.
- **Authentication.** There is none beyond filesystem permissions, which is right for a local socket
  and wrong for a network one: any peer that can reach the port can rewrite the rig and mute the
  output. A pre-shared token in `engine.hello` is the minimum.
- **Asset transfer.** If the app pushes documents inline, the models still have to be on the device.
  The registry (§3.7) should say what is there; getting files there is a separate, deliberately
  out-of-band problem (scp/rsync/USB), and the app should be able to show "this rig needs a model the
  device does not have" *before* the user tries to play it.

Options, cheapest first: an SSH bridge from a macOS-only app (no engine work, no iOS); a small bridge
process that speaks `AF_UNIX` locally and TCP remotely; or a TCP listener in the daemon itself. I
would do the last one, opt-in and off by default, with the token, and keep the local socket as the
trusted path. The Mac app can also work **offline** — designing without an engine is a legitimate and
useful mode, and it is the one ljoss already implements perfectly.

### 3.9 Physical I/O: phantom, pad, impedance, destination, FX loop

ljoss models, on the input node, things that are *not* signal processing: 48 V phantom, a −10/−20 dB
pad, input impedance, and the analog gain; on the output node, the physical destination; and as a
block, an FX loop with send and return levels.

None of these is renderable by an engine that reads channel 0 of a PCM device. Some are hardware
controls that an ALSA mixer might expose (device-specific, and a bad first target — setting phantom
power from a phone app is a support call waiting to happen); some need extra physical channels
(`Send 1/2`, an FX loop); `lowcut` and a digital trim are genuinely renderable.

The rule I would adopt: **a parameter the engine cannot honour must never be sent as if it could.**
Keep them in the document, mark them in the UI as hardware/out-of-band, and split the IO blocks into
"renderable" (gain trim, low cut, level, pan when stereo exists, mono sum) and "hardware" (phantom,
pad, impedance, source select, destination). That keeps ljoss's idea — a rig is a complete
description of a pedalboard, including the bits the app does not control — without creating silent
no-ops.

### 3.10 Editing: local tree edits vs ramped and crossfaded commits

ljoss's edit model and namchain's are complementary rather than conflicting, and both are unusually
careful about the same thing (silent corruption):

- ljoss normalizes the **document** after every mutation (splice a one-lane parallel back into
  series; prune a spent split; *preserve* an individually empty lane), removes without normalizing
  during a move so the destination cannot be destroyed mid-move, and rejects dropping a parallel into
  its own subtree before anything is removed.
- namchain validates the **graph** (one input, one output, no cycles, port ranges, one edge per input
  port) and applies a patch all-or-nothing to a copy, so a rejected edit leaves the running plan
  untouched.

The interface consequences are concrete and worth designing into the canvas now:

1. **A drag is local; a drop is one commit.** Never stream structural edits during a gesture. The
   engine will coalesce them, but each commit costs a dual-render for `fade_ms`, so a drag at 60 Hz
   would keep the engine permanently in transition.
2. **A knob drag streams `set_param`, but only for params the schema marks rampable.** Discrete and
   option params commit on release. This is the same "four op classes" distinction the engine already
   makes; the UI has to make it too, visibly (see §6.2).
3. **Batching is free and should be used.** One patch = one compile = one crossfade. Adding a block in
   a parallel lane is "bump the split's `outputs`, add the node, add the connections, bump the mix's
   `inputs`" — one patch, one crossfade, no intermediate audible state.
4. **Failure is a normal outcome, not an exception.** `compile_failed` with a message, plan
   unchanged, generation unchanged: the app should keep its document, show the reason, and be able to
   re-send the whole document as a recovery path.
5. **Adoption is asynchronous.** `params_applied`/`params_stale` and `adopted_generation` exist so a
   client can tell "sent" from "audible". A UI that shows a knob as applied the moment it is sent is
   lying about the engine's state in exactly the case (a structural transition in flight) where the
   user is most likely to be confused.
6. **The engine's health belongs on screen.** `failed_nodes`, `guard_trips`, `xruns`,
   `device_lost`, `transition_active` have no counterpart in ljoss at all, and a rig that is running
   with one node failed looks identical to a healthy one in a canvas that does not know about it.

---

## 4. Verdict, in one table

**Compatible as-is (no design work):**
- ljoss's series/parallel tree embeds exactly into namchain's DAG; `yCable` split = `nc.split`.
- `number`/`flag`/`option` values embed into numeric/string params. `options` needs a patch path,
  which is small work, not a redesign.
- `isEnabled` → `NodeSpec::bypass`, with better semantics already implemented (state stays warm,
  crossfaded, reported).
- All-or-nothing patch = transactional edit; ljoss's `normalize()` and namchain's `validate()` are the
  same idea applied to the document and the graph respectively.
- One patch = one crossfade, so a tree edit compiles to one commit.
- Grow-only layout, one-file-per-rig persistence, unknown-component placeholders, default-fallback
  for missing params: all good document-layer decisions that should survive unchanged.
- A scene is a set of `set_param` ops; the smooth class is already built. No engine scene concept is
  needed for the first implementation.

**Compatible with a defined mapping (work, no redesign):**
- Block → macro lowering (the big one, §3.1), with the catalogue as its specification.
- Split `aB` and `crossover` as lowerings; `dynamic` as a new 1→2 node type.
- Lane level/mute/phase as settable `nc.mix` input gains (phase as a ramp through zero, which is
  click-free, or as an explicit polarity node).
- Block UUIDs as node-id prefixes, so the graph round-trips to a tree.
- A client-owned scene/preset layer.

**Genuinely incompatible today (a decision and real work):**
1. Two chains vs one (§3.2) — needs `Rig = {chains}` and per-chain plan/generation/status.
2. Stereo intent vs a mono engine (§3.3) — declare port arity now, implement when a stereo feature
   blocks a release.
3. 102 blocks vs 11 primitives with no lowering (§3.1) — the macro/lowering layer.
4. No parameter schema, no catalogue over the wire, unknown params accepted (§3.5).
5. No document push and no graph read-back (§3.6).
6. Assets by path with no device registry, and a static amp/cab palette (§3.7).
7. `AF_UNIX`-only transport for an iOS/macOS client (§3.8).
8. Hardware/out-of-band controls modelled as engine parameters (§3.9).

---

## 5. What the first implementation of namchain should take from ljoss

### 5.1 The catalogue and the parameter schema — **do this first**

ljoss's single best idea, and the one namchain most obviously lacks: *a component declares its
parameters* — kind, range, step, options, default, unit — separately from any instance's values.

Make that real on the engine side: every `NodeType` publishes a schema (id, display name, category,
ports with channel counts, params with kind/min/max/step/options/default/unit/rampable, plus
latency/tail/stateful flags), and a protocol method returns the whole catalogue. Then:

- ljoss's palette is driven by what the engine can actually instantiate, with the not-yet-implemented
  entries shown as unavailable rather than failing on drop.
- The inspector can be generic, and its sliders can be labelled in real units.
- Unknown parameters stop being silent, defaults stop existing in two places, and the app can
  validate before it sends.
- The two catalogues can be diffed by a test, which is the only way they stay in step.

Doubling as the schema's home: extend `settable_params`/`set_param` from `nc.gain` to **every
continuous parameter of every built-in**, so a knob drag is the smooth class by construction. This is
the piece that makes a UI feel like a control surface rather than a build system.

### 5.2 The macro/lowering layer

ljoss's catalogue is a *specification of macros*: this is what a "tape echo" is, this is what a
"3-band parametric EQ" is, this is what an "amp" is. namchain's plan already has the stage
(plan.md §5.2, "lower"); ljoss supplies its content. Do it engine-side so every client gets it and
`rig.get` can return the document the user wrote. Start with the macros that make the largest part of
the catalogue renderable with primitives that already exist: multi-band and shelving EQ, high/low
cut with slope, dry/wet and lane mixing, and the feedback delay. Add a bypass rule for macros at the
same time (§3.1).

### 5.3 The document: N chains, whole-document apply, graph read-back

Three things that only make sense together, and that together turn namchain from a thing you
configure into a thing you drive:

- `Rig = { chains: [Graph] }` with per-chain input/output binding, from plan §5.1.
- `rig.apply {spec}` (or an inline `rig.load`), with an explicit asset base.
- `rig.get` returning the current graph, and a per-chain status.

### 5.4 The model/IR registry as the palette's source

Take ljoss's "the palette is the truth about what you can add" and push the dynamic parts to the
engine: amps and cabs come from what is installed. This is the natural first half of M3, and it also
answers "why does this rig not load" before the user hits play.

### 5.5 Invariants in one place

ljoss's `normalize()` is a good pattern and namchain's `validate()` is its graph-level twin. The
lowering layer is where they must agree: a one-lane parallel spliced back into series (ljoss) is what
stops a pointless split/mix pair (namchain); a pruned all-empty parallel is what stops a split with
no consumers; a preserved empty lane is exactly namchain's "unconnected input port is bound to
silence" — legal, and the dry path.

---

## 6. What ljoss should take from namchain

### 6.1 `isEnabled` → bypass, and the visual language for warm state

namchain's bypass keeps the node **running** with its output replaced by its dry input, so
un-bypassing does not step on a frozen state (G14). ljoss's `isEnabled` should mean exactly that, and
the canvas should show it: a bypassed tile is dimmed but *alive* (it is still consuming CPU to stay
warm). That is a real piece of information, and it is also why a bypassed block must not be shown as
"off" in any CPU/latency readout.

### 6.2 Smooth vs structural, made visible

A parameter that the engine ramps is not the same as one that needs a crossfade, and the user can
feel the difference. Mark rampable parameters in the inspector (the schema tells you), stream those
during a drag, commit the rest on release, and treat a structural commit as a discrete action with
visible feedback (`transition_active`, `audible_generation` → `target_generation`). This is also the
honest answer to "why did that knob click" and "why did the generation jump".

### 6.3 The engine is a sink, not a mirror

Design the app around: I own the document; the engine owns the plan; the plan may differ from my
document at any time (another client, an operator's reload, a rejected patch, a failed node). Show
generation and adoption, surface `compile_failed` messages verbatim, and offer "re-send the whole
document" as the recovery path. Until there is telemetry subscription, poll `engine.status` at a
sensible rate (a few hertz) rather than per frame.

### 6.4 Hardware parameters are not engine parameters

Split the IO blocks (§3.9) and label the hardware half as such. A remembered phantom-power setting
that the app never sends is honest; a knob that looks live and is not is not.

### 6.5 `layout` is a viewing preference; muting a chain is not

ljoss already makes this argument for `wrap` (*"a viewing preference, not rig data"*), and `layout`
is the same kind of thing: it changes what is drawn, not what is played. If the engine runs both
chains, `layout: single` must not be mistaken for "chain 2 is silent". Make chain silence an explicit,
visible control (per-chain mute), and keep `layout` out of anything the engine sees.

### 6.6 One vocabulary for parameters

Decide the naming rule once and enforce it in both trees — app-side catalogue vs engine schema, with
a test that diffs them. `time` vs `ms`, and a 0–10 `gain` knob vs a linear or dB engine gain, are
small divergences that produce the "the knob works but the sound is wrong" class of bug. Units and
kinds on the wire (§5.1) are what make the rule checkable rather than aspirational.

### 6.7 Keep the depth cap — it is a UI constraint, not an engine one

`maxParallelDepth = 1` is right for the canvas (readable columns, unambiguous hit testing) and wrong
as a statement about the engine, which nests freely. Keep it in the app, and give the graph→tree
recogniser an explicit "this graph uses routing the canvas cannot draw" outcome for rigs authored
elsewhere.

---

## 7. Recommended first slice, concretely

Ordered by value per unit of risk. The first two are small and unblock everything else; the third is
the big one.

1. **Schema and catalogue on the wire.** `params_schema_json` per `NodeType`; a protocol method that
   returns the catalogue; unknown param names rejected (or reported); `set_param` extended to every
   continuous param of every built-in; numbers-only `set_param` relaxed so `options` params have a
   structural path. *Small, no RT risk, fixes the silent-no-op class, makes a generic inspector
   possible.*
2. **`rig.get` returns the graph; `rig.apply` accepts a whole document.** Plus an explicit asset base
   for inline specs. *Small, and it is what makes the app's canvas and the engine's plan able to
   agree.*
3. **Multi-chain rigs** (`Rig = {chains}`, per-chain plan/generation/status/mute), from plan §5.1.
   *Medium; daemon and transition bookkeeping only, no DSP. Unblocks the dual rig, which is ljoss's
   headline feature.*
4. **Lowering and the first macros**, with ljoss's catalogue as the specification: multi-band and
   shelving EQ, cut with slope, dry/wet and lane mixing (needs settable `nc.mix` gains), feedback
   delay, amp = model + tone stack. *Large, but it is the difference between one renderable block
   and most of the catalogue.*
5. **Transport**: TCP (opt-in, token, explicit framing) so the app can be remote; or scope the first
   release to a macOS app over SSH and skip it. *Small if done as a listener, and it can be deferred
   if the first release is tethered.*
6. **Stereo**, declared in the schema from step 1 and implemented for `nc.pan`/`nc.mix`/`nc.cab.ir`/
   `nc.output` when a stereo feature actually blocks a release. *Large; do not start it early, but do
   not let the schema pretend the engine is stereo either.*

## 8. What I would deliberately not do yet

- **The 102-block DSP library.** Drives, dynamics, modulation, reverb, pitch, filters: real DSP, and
  a separate project from the integration. The palette should show them as unavailable.
- **Tuner, looper, FX loop, DI.** These are not chain blocks: a tuner needs pitch detection and its
  own UI, a looper needs transport control and a long buffer, an FX loop needs physical ports.
- **Scenes in the engine.** A scene is a client-side parameter set over the smooth class until
  footswitch latency says otherwise.
- **Device control** (phantom/pad/impedance/destination). Out-of-band until there is a device whose
  mixer controls are worth driving.
- **A general sidechain/modulation system** for the dynamic split. One node type if it is wanted.
- **Serialisation changes beyond the envelope.** The only functional requirements are: metadata plus
  N chains, stable ids across edits, an asset base, and atomic write.

## 9. Evidence

Verified in this tree at `799688e` (and `~/code.local/ljoss` at `7618314`):

```
# exactly one chain per document
grep -n "a chain needs exactly one" src/graph/plan.cpp

# mono buffers, one settable param engine-wide
grep -n "Buffers are mono for now" include/namchain/graph/plan.h
grep -n "settable_params" src/nodes/builtin.cpp src/graph/plan.cpp   # only kGainSettable

# numbers only, and swap_model's hardcoded keys
sed -n '274,297p' src/graph/patch.cpp
sed -n '/bool op_swap_model/,/^}/p' src/graph/patch.cpp

# AF_UNIX only
grep -n "AF_UNIX\|AF_INET" src/daemon/daemon.cpp

# rig.get returns path/generation/loaded and nothing else
sed -n '/method == "rig.get"/,/^  }/p' src/daemon/daemon.cpp

# no write-back of the patched graph
grep -n "ofstream\|rename(" src/daemon/daemon.cpp src/graph/patch.cpp

# an unknown parameter name is accepted silently
cat > /tmp/typo.json <<'EOF'
{ "nodes": [ { "id": "in", "type": "nc.input" },
             { "id": "g", "type": "nc.gain", "gainz": 3.0, "gain": 1.0 },
             { "id": "out", "type": "nc.output" } ],
  "connections": [ { "from": "in", "to": "g" }, { "from": "g", "to": "out" } ] }
EOF
./build/namchaind --backend null --no-lock-memory --rig /tmp/typo.json --socket /tmp/nc.sock --verbose
# → "[namchaind] published /tmp/typo.json (generation 1, 3 buffers)"  — no complaint

# ljoss: no audio, no networking, no export, no undo
grep -rniE "urlsession|socket|bonjour" --include=*.swift ~/code.local/ljoss    # nothing
grep -rn "undo\|UndoManager" --include=*.swift ~/code.local/ljoss              # nothing
```

## 10. Decisions this analysis needs from you

1. **Is ljoss the UI for the same box namchaind runs on, or a remote control for it?** The answer
   decides whether TCP transport is in the first slice or can wait behind a tethered macOS app.
2. **Dual chains in the first slice, or single-chain until the macros exist?** Dual is ljoss's
   headline feature; it is also the change that touches the daemon's plan slot, generations and
   status.
3. **Who lowers a block into nodes — the engine or the app?** I recommend the engine (§3.1); it
   decides whether `rig.get` can return what the user actually wrote.
4. **Stereo: declare-and-refuse, or build it?** I recommend declare now, build when it blocks a
   release — but if pan is a headline feature of the UI, that ordering flips.
5. **The amp palette: static names or the device's model registry?** I recommend the registry, which
   makes M3's warm start and this the same piece of work.
6. **Which of the 102 blocks are the *first* macros?** My suggestion — the ones that need no new DSP:
   multi-band/shelving EQ, cut-with-slope, dry/wet mixing, lane level/mute/phase, feedback delay,
   amp-tone-stack — which together make a usable guitar rig out of primitives that already exist.
7. **Hardware parameters: keep them in the document as out-of-band, or remove them?** I recommend
   keeping and labelling; removing them loses what a rig is supposed to describe.
