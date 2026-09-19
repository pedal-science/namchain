# Transitions: the op classes, the timeline, and the detector

**Status:** the `smooth` op class and its offline harness are built; the structural op classes are
not. This document is the operational half of docs/rig-pipeline.md §4–§5, which is the design half.
Read that for *why*; read this for *what runs*.

## The four op classes (plan.md §5.3)

| edit | mechanism | state |
|---|---|---|
| Parameter change, `smooth` class | per-sample ramp inside the node's param slot | **built** — `NodeType::set_param`, no recompile, no step |
| Bypass / enable | equal-power crossfade to/from dry | not built |
| Model swap in `nc.nam` | warm start, then a short crossfade | not built (M3) |
| Rewire / add / remove / scene recall | dual-render old and new plan, equal-power crossfade, retire | not built |

A structural reload today is still the M1 cold swap: the new plan is compiled and prewarmed on the
control thread and published, but the render thread exchanges it at a block boundary with no
crossfade. `engine.status` says exactly that: `"param_edits": "ramped"`, `"structural_transitions":
"cold-swap"`.

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

1. The structural op classes: dual-render old and new plans for `fade_ms`, equal-power crossfade at
   the chain outputs, then retire (§5.3). `rt::SpliceFader` is a splice repair and is **not** this
   crossfade; a chain crossfade is its own primitive.
2. Admission control and the deferred-commit scheduler (§5.3, §5.4): `worst_case_block`, the 70 %
   budget, shorten-fade -> drop-warming -> slim -> reject-with-reason, one structural transition in
   flight with coalescing.
3. Bypass/enable and the per-node non-finite guard, which need a dry path.
