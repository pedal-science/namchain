# Macros: the lowering stage

Slice E of `docs/ui-integration-plan.md`, and the answer to the largest gap in
`docs/ljoss-comparison.md` §3.1: this engine has eleven primitives and ljoss has 102 blocks, and the
missing stage between them is **lowering**.

A macro is a document-level node the engine expands into primitives. It exists so that a client can
say "a feedback delay" without knowing that this engine expresses feedback as a mix, a delay line, a
gain and a unit delay in a cycle — and so that there is one implementation of that expansion rather
than one per client.

```
ljoss says            the document says                          the engine runs
delay.tape       →    { "element": "b3",                     →    b3:dry, b3:sum, b3:line,
(time, feedback,        "type": "fx.delay.feedback",              b3:fb, b3:tap, b3:wet, b3:out
 mix)                   "time": 420, "feedback": 0.45,
                        "mix": 0.3 }
```

Macros are lowered **per compile** and never stored lowered. `rig.get` returns the document the user
wrote; a lowering can change without breaking anything a client holds.

## What is declared

Five things, all in the macro's own table (`src/nodes/macros.cpp`), and none of them discovered at
runtime:

| | |
|---|---|
| **schema** | the same `ParamDesc` fields a node type publishes — kind, unit, default, bounds, step, scale, `rampable`, `options`, `options_from`. A client cannot tell a macro from a primitive except by `kind` |
| **shape** | the roles it lowers to, each with a node type, and the edges between them |
| **port links** | which role each of the macro's input and output ports connects to. A list, because one port may fan out (a delay's input feeds both its dry leg and its line) |
| **param map** | each macro parameter → one or more `(role, node param, transform)` targets |
| **bypass rule** | `roles` or `crossfade` — see below |

The **param map** is what makes a macro-level `set_param` possible at all: the client sends one value
and the engine derives the rest. The transform set is closed and small — `direct`, `db_to_linear`,
`linear_to_db`, `one_minus`, `scale_by` — with no expressions, no templating and nothing evaluated on
the audio thread. A parameter with **no targets** is consumed by the lowering itself (a band count, a
slope) and is structural by definition.

## The seven

| macro | what it is | params | lowers to |
|---|---|---|---|
| `fx.eq.bands` | four-band EQ with shelving ends | `bands`, and `freq`/`gain`/`q` per band | 4 × `nc.eq.biquad` (lowshelf, peaking, peaking, highshelf) |
| `fx.eq.tilt` | tilt EQ: one knob, two shelves | `tilt`, `freq` | 2 × `nc.eq.biquad` (lowshelf + highshelf, ±tilt/2) |
| `fx.filter.cut` | low cut and high cut, each with a slope | `low_cut`, `low_slope`, `high_cut`, `high_slope` | 8 × `nc.eq.biquad` (4 highpass + 4 lowpass) |
| `fx.mix.drywet` | a dry/wet crossfade for a parallel section | `mix`, `level_db` | 2 × `nc.gain` + 2 × `nc.mix` |
| `fx.delay.feedback` | a delay with feedback and a mix | `time`, `feedback`, `mix` | `nc.gain` ×3, `nc.mix` ×2, `nc.delay`, `nc.unit_delay` |
| `fx.amp.tonestack` | bass, mid and treble | `bass`, `mid`, `treble`, `mid_freq` | 3 × `nc.eq.biquad` |
| `fx.cab.ir` | a cabinet IR with the cuts it needs | `ir`, `low_cut`, `high_cut`, `level_db` | `nc.eq.biquad` ×2, `nc.cab.ir`, `nc.gain` |

Two properties of that table are deliberate and worth knowing before writing another macro:

- **No DSP was written for any of them.** Every macro lowers to biquads, gains, mixes and the
  existing delay line. The interesting numbers — the Butterworth stage Qs a 24 dB/oct cut needs
  (0.541 and 1.307, not 0.707 twice), the ±tilt/2 split — are *constants in the lowering*, which is
  the point: they are DSP vocabulary, and they belong in the engine rather than in every client.
- **A macro's shape does not depend on its parameter values.** `bands` and `slope` pick how many
  stages are *live*, and the rest are **bypassed** rather than removed. A bypassed node still runs, so
  a slope change does not invalidate the parameter values a client is holding, and `low_cut` keeps
  meaning the same thing at every slope. The cost is that a one-stage cut still runs four biquads,
  which is a few tens of flops.

## Roles, and how a client addresses a macro

A lowered node carries the macro's `element` and its declared `role`, so a macro is addressable at
two levels:

```jsonc
// the macro's own parameter: one knob, and the engine works out what it drives
{ "op": "set_param", "element": "b3", "param": "feedback", "value": 0.6 }

// one node of the lowering: the escape hatch, for poking a primitive inside a macro
{ "op": "set_param", "element": "b3", "role": "line", "param": "ms", "value": 250 }

// a plain node, whose element defaults to its id
{ "op": "set_param", "element": "g", "param": "gain_db", "value": -6 }
```

The node ids the engine mints are `<element>:<role>` — `b3:line`, `b3:fb` — which is what makes a
graph dump readable and a bug report diagnosable. They are returned by `rig.get` and are **advisory**:
a client addresses `element` + `role`, and the engine guarantees uniqueness itself (a collision with an
id the document already used becomes `~2`, `~3`, …).

An address that names nothing is an error that lists what exists; an element with several nodes and no
role is an error that lists the roles. No op ever silently does nothing.

## Bypass

`NodeSpec::bypass` is a 1→1 substitution — a node keeps running and its output becomes its dry input —
and a macro is not always 1→1. So each macro **declares** its rule, and there are two:

- **`roles`** — every lowered node takes the flag. Correct for a serial chain of primitives that are
  transparent when bypassed, which is every EQ, filter, tone stack and cab: each node substitutes its
  dry input, so the chain passes the signal and every node keeps running. The macro's flag is OR-ed
  with whatever the lowering decided (a `slope` above the live count already bypasses those stages).
- **`crossfade`** — the lowering is not a serial chain, so bypass is a wet/dry crossfade: the two
  declared roles are gains, and bypass sets the dry one to unity and the wet one to silence. The wet
  path keeps running, so a bypassed delay line is still fed and un-bypassing it does not produce a
  hole. Used by `fx.delay.feedback`, whose output is a mixer.

Bypassing a whole macro is one op on the macro:

```jsonc
{ "op": "bypass", "element": "b3", "bypass": true }
```

## What a macro edit records

A macro parameter that maps to rampable node parameters is the **smooth** class: one ring record per
target, pushed consecutively. The render thread drains the ring at the top of a block, so the targets
normally land in the same block; a knob whose two targets landed one block apart would be a 1.3 ms
skew on two ramped values, which is not worth a transaction mechanism the ring does not need.

A parameter with no targets, or one whose target cannot ramp (an option, a string, a band count), is
**structural**: the patch is applied to the document and the whole chain is dual-rendered and
crossfaded, as any other structural edit is.

Two things are recorded in the document when a smooth edit lands, and one deliberately is not:

- a **node parameter** — `rig.get` shows it, and a later structural edit does not silently revert it;
- a **macro parameter** — recorded on the macro node, under the macro's own name, so the document
  keeps saying `"feedback": 0.6` rather than the two node gains it became;
- a **per-port parameter**, or a parameter addressed at one node *inside* a macro via `element` +
  `role` — **not** recorded. The first is runtime state by design (a lane's level is not a document
  field, `docs/daemon.md`), and the second is a value the document has no field for; inventing a
  mapping back onto the macro's parameters would be guessing.

## What is published

`engine.schema` lists primitives and macros in **one array**, told apart by `kind` (`"primitive"` /
`"macro"`), so a palette needs one call and one order. A macro entry carries everything a primitive
does, plus:

```json
{
  "id": "fx.delay.feedback", "kind": "macro", "category": "delay",
  "inputs": 1, "outputs": 1, "bypass": "crossfade",
  "roles": [ { "role": "line", "type": "nc.delay" }, … ],
  "params": [
    { "id": "feedback", "kind": "number", "unit": null, "default": 0.35,
      "min": 0.0, "max": 0.95, "rampable": true, "targets": [
        { "role": "fb", "param": "gain", "transform": "direct", "scale": 0.0 } ] }
  ]
}
```

`targets` is published because "why did my one knob move two nodes" should be answerable from the
daemon rather than from the source. `rig.get` publishes both views: `document` (what the client wrote)
and `lowered` (the primitives it became, with the minted ids and roles).

## Writing another macro

1. Write the shape and the param map as tables in `src/nodes/macros.cpp`; write `apply` only for what
   they cannot express (a mode, an arity, a line length, a selection's bypasses).
2. Declare the bypass rule. If it is `crossfade`, name the two gain roles.
3. Run the tests. Three of them are properties rather than examples and will catch most mistakes:
   every target names a role and a parameter that exist; changing a parameter changes **exactly** its
   declared targets; and the macro renders sample-identically to the hand-built graph the test writes
   out independently.

That last test is the one that matters. It is the same oracle argument the compiler uses for its
reference interpreter: the lowering is a rewrite, so it must produce the same samples, and "nearly the
same" is a bug.

## What is not here

- **No macros the app did not ask for.** Seven, and each one is a block in ljoss's first tier.
- **No nesting.** A macro's lowering is primitives; a macro inside a macro is not expressible, and
  nothing needs it yet — the app composes blocks in series like it always did.
- **No `nc.split` `mode`** (`y`/`aB`/`crossover`/`dynamic`). The `crossover` pick in particular is real
  DSP — a Linkwitz-Riley needs cascaded stages and a polarity decision — so it is a macro of its own
  when something asks for one, not a knob on the split.
- **No parameter that is a function of two others.** A transform reads one macro parameter; a knob
  that needs two (a crossover's frequency and slope interacting) belongs in a lowering's `apply`.
- **No client-side `engine.schema` caching story.** The catalogue is small and static except for the
  library-backed options; a client may fetch it once per connection.
