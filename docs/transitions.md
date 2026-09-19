# Transitions: the op classes, the timeline, and the detector

**Status:** the `smooth` op class, its offline harness, the structural dual-render crossfade, the
structural patch protocol, bypass and the non-finite guard are built. Warm-start model swapping is not
(M3), and neither is admission control. This document is the operational half of
docs/rig-pipeline.md §4–§5, which is the design half. Read that for *why*; read this for *what runs*.

## The four op classes (plan.md §5.3)

| edit | mechanism | state |
|---|---|---|
| Parameter change, `smooth` class | per-sample ramp inside the node's param slot | **built** — `NodeType::set_param`, no recompile, no step |
| Bypass / enable | equal-power crossfade to/from dry | **built** — a `bypass` flag in the IR; the node keeps running and its output is replaced by its dry input |
| Model swap in `nc.nam` | warm start, then a short crossfade | **partial** — the `swap_model` op rebuilds the node and crossfades; warm start is M3, so it is not yet state-accurate |
| Rewire / add / remove / scene recall | dual-render old and new plan, equal-power crossfade, retire | **built** — `graph::apply_patch` builds the modified graph, `rt::Crossfade` carries it |

`engine.status` says which is which: `"param_edits": "ramped"`, `"structural_transitions":
"crossfade"`, plus `transition_active`, `audible_generation` and `target_generation` while a fade is
in flight, and `failed_nodes`/`guard_trips` from the non-finite guard.

## The patch protocol

`rig.patch` takes an `ops` array, applied **all-or-nothing**. Each op is applied to a *copy* of the
current graph; the copy is compiled and published only if every op succeeded, so a rejected patch
leaves the running plan rendering exactly as it was.

| op | effect |
|---|---|
| `add_node` | `{"id","type",...}` — the node object is parsed by the rig parser, so a patched node and a declared node mean the same thing |
| `remove_node` | drops the node and every connection that touched it |
| `connect` | adds an edge; a duplicate input driver is caught by the compiler's validation |
| `disconnect` | removes the edge into a port, by destination or by both ends; nothing to remove is an error |
| `rewire` | repoints one input port: the existing driver is removed and the named output drives it |
| `swap_model` | writes `model`/`ir` on a model node and rebuilds it (crossfaded, not warm-started) |
| `set_param` | routed: see below |
| `bypass` | sets a flag in the IR; the node keeps running and its output becomes its dry input |

**The router is the op class.** If *every* op is a `set_param` that the live plan's vtable can
dispatch (`RenderPlan::resolve_param`), the whole patch is the smooth class: one POD record per op on
the ring, no compile, no fade. A single non-rampable op makes the whole patch structural. That is why
`set_param gain` is smooth but `set_param gain_db` is structural — `nc.gain` ramps `gain` and only
reads `gain_db` at build, so a rebuild is the only way to honour it. The response carries
`"kind": "smooth" | "structural"` so a client never has to guess which happened.

Param *names* on the structural path are not validated beyond the node's own build-time reading: like
a rig, a numeric param no node uses is accepted and has no effect. Rejecting it needs the per-node
parameter schema that plan.md §6.1's `params_schema_json` promises; until then the smooth router is
the only place a name is checked.

## Bypass

`NodeSpec::bypass` is a flag in the IR, not a connection rewrite, so un-bypass is one flag and the
original graph is never lost. A bypassed node **keeps running** — the compiler still creates it and
calls its `process` — and its output is then replaced by its dry input
(`graph::substitute_dry`). That is the whole design, and it is what keeps the node's state warm:

- Removing the node from the plan instead would be simpler, and would freeze its state. A NAM model
  un-bypassed after a minute of silence would then start from a stale history — plan.md G14's "skipping
  inference freezes state and glitches on unmute".
- A node with several inputs has no single dry signal, so only a 1→1 node can be bypassed; the compiler
  refuses the rest by name rather than guessing a port.
- The equal-power part of "crossfade to/from dry" is the *structural transition* above, not a second
  crossfade inside the node: bypass changes the plan, the render thread dual-renders and crossfades.
- The plan also asks a tail node to keep ringing through a bypass (`tail_hold`). No built-in has a tail
  yet, and the flag has no tail state, so that is deferred to the reverb/tail work.

`nc.bypass` also exists as an ordinary 1→1 pass-through node, for a rig that wants an explicit dry
path. It is not how `bypass: true` is implemented.

## The non-finite guard

Every node's output is scanned for non-finite samples after it runs. One compare per sample, against
buffers the node has just written and so are hot in cache. A node that returns NaN or infinity is
**taken out of the signal path for the rest of the plan's life** and reported
(`failed_node_count()`, `guard_trips()`, surfaced as `engine.status`'s `failed_nodes` and
`guard_trips`), and its output is replaced by its dry input.

The plan says "always on for third-party nodes"; it is on for every node, because the scan is cheap
relative to the work that produced the samples and a NaN that reaches the device is not a degradation
the never-silence guarantee covers. A failed node is not run again — a deliberate exception to G14's
"keep it running", because a node whose output cannot be trusted is not a node whose state is worth
keeping warm.

The oracle deliberately does **not** mirror the guard. It mirrors bypass (that is a different graph),
but a node returning NaN is a runtime fault rather than a different graph to compare against, so an
oracle comparison that fails because a node failed is telling the truth.

## The structural transition

A structural publish (`rig.load`, a SIGHUP reload, and the structural `rig.patch` ops) compiles
and prewarms a complete new plan on the control thread, exactly as before. What changed is what the
render thread does with it:

```
on PublishPlan:
  if no plan is loaded yet      -> adopt it (nothing to fade from)
  else if a fade is in flight   -> keep it as the single pending slot, newest wins
  else                          -> start a transition

per block during a transition:
  current->execute(in, old)          # both plans run on the same input
  next->execute(in, new)
  crossfade.mix(old, new, out)       # equal-power, fade_ms long
  when the fade closes: retire current, current = next
  if a pending plan exists: start the next transition from the new current
```

Two properties matter:

- **Equal-power, not linear.** `rt::Crossfade` weights the two chain outputs by `cos`/`sin` of a
  quarter turn, so `from² + to² == 1` across the whole fade. A linear crossfade of two uncorrelated
  signals dips 3 dB in the middle — audible on sustained material. The identity is checked directly in
  `tests/test_crossfade.cpp` rather than assumed.
- **The fade lands exactly.** With `length` samples there are `length - 1` steps between them, so the
  table index is `position * max / (length - 1)`; reading `position * max / length` leaves a residual
  `cos(π/2·(length-1)/length)` of the old signal at the handoff — 0.008 for a 240-frame fade, which
  is a step the detector measures. This was found by the endpoint test, not by ear.

Both plans keep running for the whole fade: a chain that stops mid-transition freezes its state and
would step on the way out (plan.md G14). The old plan is *retired* into the epoch reclaimer when the
fade closes and freed by the control thread after the grace window; no plan is deleted on the render
thread.

A burst of structural requests coalesces to the newest: one transition in flight, one pending slot,
the superseded pending plan retired. Parameter edits are routed to whichever live plan names their
generation, and a patch for a superseded plan is counted stale rather than applied to the wrong chain.


## The `smooth` class

A node declares what can be set and how to set it in its vtable:

```cpp
struct NodeType {
  const char* const* settable_params; // name i takes id i + 1
  int settable_param_count;
  bool (*set_param)(void* state, uint32_t param_id, float value) noexcept; // render path
};
```

`nc.gain` is the first implementation. Its `gain` param ramps over the rig's `ramp_ms` (default 5 ms)
with a **smoothstep**:

```
g(t) = from + (to - from) * t*t*(3 - 2t),   t = ramp_pos / ramp_len
```

Smoothstep rather than a linear ramp because the detector measures a *second* difference: a linear
ramp has a slope discontinuity at each end and scores that corner. Smoothstep matches value and slope
at both ends. `ramp_ms = 0` is a legal instant change, which is what the negative control uses.

A second `set_param` mid-ramp starts from the *current* value, so two edits close together cannot
jump back to a stale endpoint.

### Why the node owns the ramp length

The plan's ABI (§6.1) puts the smoothing class in the node's parameter schema, so the node decides
how long its own ramp is and the render path never has to carry a duration. Time-to-samples is
control-thread work at build (`ramp_ms` -> samples once), which is the same rule every other node
follows.

## The scripted timeline

`render::EditTimeline` is a list of `{at_sample, node, param, value}` ops applied at exact samples on
a virtual clock. It is rendered by `render::render_timeline_to_wav`, and driven by `namchain-render`:

```
# a hand-written timeline
namchain-render --graph rigs/guitar-chain.json --timeline edits.json in.wav out.wav

# a generated, repeatable torture timeline
namchain-render --graph rigs/guitar-chain.json --torture 500 --seed 7 in.wav out.wav
```

A timeline file is an array of ops (or `{"edits": [...]}`):

```json
[ {"at_sample": 8000,  "node": "g", "param": "gain", "value": 0.5},
  {"at_sample": 16000, "node": "g", "param": "gain", "value": 1.6} ]
```

Ops must be in nondecreasing order — sorting silently would hide a caller's bug — and an unknown op
kind, node or parameter is rejected before anything renders, so a timeline cannot "pass" by applying
nothing. The only op kind implemented is `set_param`; naming `rewire` is a structured error, not a
no-op.

Sample-accuracy is why the renderer splits blocks at edits instead of quantising to the next block
boundary: an edit lands at exactly the sample the timeline names, on every run. That is the whole
reason this offline path is the primary test tool for §5.3 and §5.4.

## How an edit is judged

For each edit the renderer builds two oracle chains from the same graph and drives them to the
parameter snapshot that applies at that edit — one for before, one for after — and hands the span to
the glitch detector (`src/glitch/glitch.cpp`, plan.md §10.3), which checks four rules: continuity, no
dropout, no excess high-frequency energy, and convergence to the right oracle either side.

Two properties of the oracle model matter when reading a result:

- Each oracle is rendered **from frame 0** with its parameter held, so its state at the span is the
  state the live chain had. This is exact for any node whose state is a function of the input and the
  parameters, and it is why the generated timeline leaves a gap between edits: a parameter still
  ramping when the next oracle is taken has no settled value to compare against.
- The continuity reference is **relative** to the signal's own second difference out of the windows
  (`kappa = 2`). On program material with transients that reference is large, so the rule is weakest
  exactly where the signal is most forgiving. Use a hard test signal (a tone, or the §10.3 torture
  signal), not a guitar recording, when the continuity number is the point — an instant gain step on
  the vendored sample audio passes, and the same step on a 1 kHz tone fails by ~13x.

The `--torture` renderer's per-edit oracle cost is currently O(edits x file), because each oracle
re-renders from frame 0. That is invisible at test sizes and is the first thing to optimise when the
full 60 s / 500-edit torture becomes a routine run.

## What is next

1. **Admission control and the deferred-commit scheduler** (§5.3, §5.4): `worst_case_block`, the 70 %
   budget, shorten-fade -> drop-warming -> slim -> reject-with-reason. The cost model it needs does
   not exist yet. Today the fade is fixed and an expensive dual-render is admitted — including a
   bypassed node, which deliberately keeps costing CPU to stay warm.
2. The **tail path**: a `tail_hold` for a bypassed node with a tail, which needs the first tail node
   (reverb, M5) to have anything to hold.
3. A **structural op in the timeline**, so the offline harness can drive a plan change and the
   detector can judge it per edit. Today the transition is measured by `tests/test_crossfade.cpp`
   with two compiled plans and the same primitive; the timeline knows only `set_param`.
4. The **node parameter schema** (`params_schema_json`, §6.1), so the structural `set_param` path can
   reject a name no node reads instead of accepting it as a rig does.
5. The **acceptance sweep**: 60 s / 500 edits at 32/64/128/256 and 44.1/48/96 kHz, with the per-edit
   oracle cost reduced from O(edits x file) first.

