# The node schema: what `engine.schema` publishes, and what it is for

**Status:** built. Every built-in declares its parameters, the compiler validates against them, and
`engine.schema` serves the catalogue. It is slice B of docs/ui-integration-plan.md; the design
reasoning is in §1.4 of that document, and this one is the operational reference.

A client builds three things from this one answer: a **palette** (what can be added), an **inspector**
(what can be turned, in what units, over what range) and a **validator** (what a name means). Before it
existed, `{"gainz": 3.0}` was accepted, published and played — from a text file a typo, from a UI a
knob that does nothing. That is the class of bug this closes.

```
{"id":"1","method":"engine.schema"}
```

returns

```json
{ "ok": true, "protocol": 2, "max_chains": 8, "stereo": false,
  "nodes": [ { "id": "nc.gain", "name": "Gain", "category": "utility", "kind": "primitive",
               "ports": { "in":  [ {"name":"in0",  "channels":1, "optional":true} ],
                          "out": [ {"name":"out0", "channels":1, "optional":false} ] },
               "arity": null,
               "flags": { "stateful": true, "has_lookback": false, "has_tail": false,
                          "bypassable": true, "block_delay": false },
               "latency_samples": 0, "tail_samples": 0,
               "params": [ … ] } ] }
```

## The node type

| field | meaning |
|---|---|
| `id` | what a document writes as `"type"` |
| `name`, `category` | what a palette calls it and groups it under. The engine owns these because a type the app cannot name is a type the app cannot offer |
| `kind` | `primitive` today; `macro` arrives with the lowering layer (slice E), and a client should not care which it is looking at |
| `ports.in[]`, `ports.out[]` | per-port `channels` and `optional`. **An unconnected input is legal and is bound to silence**, which is what makes a dry lane a matter of not wiring it rather than of wiring a special node |
| `arity` | for a node whose port count comes from a parameter (`nc.split`, `nc.mix`): the parameter, its bounds and its default. `null` for a fixed shape |
| `flags.stateful` | explains why bypass keeps the node running rather than removing it |
| `flags.bypassable` | the compiler refuses bypass on anything but 1→1; publishing it lets a UI grey the control instead of finding out |
| `flags.block_delay` | a scheduling primitive (`nc.unit_delay`), not a pedal: a palette should leave it out |
| `latency_samples`, `tail_samples` | 0 for every built-in today; what a total-latency readout and the deferred `tail_hold` will need |

`stereo` at the top level is `false`, and it is the same decision as `ports[].channels` being 1
everywhere: the engine declares the field and refuses anything wider, so a client can see that a
stereo block is not available rather than discovering it in an error (docs/ui-integration-plan.md
§1.4, "absence at the type level, refusal at the value level").

## The parameter

| field | meaning |
|---|---|
| `id` | the name a document and a patch use |
| `name` | what a control is labelled |
| `kind` | `number`, `option` (a string value) or `flag` (0/1) |
| `unit` | `"Hz"`, `"dB"`, `"ms"`, `"samples"`, `"%"`, … or `null`. This is what stops a dB knob being wired to a linear gain |
| `default` | the **effective** value when the parameter is absent, or `null` when it has none (a `required` parameter, or one of a `one_of` group) |
| `min`, `max` | bounds, or `null` for unbounded *by the schema*. `min_inclusive`/`max_inclusive` say whether the bound itself is allowed: a Q of 0 and a frequency of 0 are excluded rather than clamped |
| `step` | the smallest increment, or `null` for continuous. `snap` says what an off-grid value means: rounded to the grid, or refused |
| `scale` | `linear` or `log` — presentation, but presentation the engine knows: a frequency control is logarithmic, and every client re-deriving that is how two clients disagree |
| `rampable` | a change is ramped inside the node with no recompile (§5.3's smooth class). **This is what a slider streams**; a parameter without it is a structural edit, dual-rendered and crossfaded, and belongs on a commit-on-release control |
| `patchable` | whether `rig.patch` may name it at all |
| `hidden` | an alias or an internal knob: a generic inspector leaves it out. `nc.gain` publishes `gain_db` (dB, visible) and `gain` (linear, hidden) for one underlying value |
| `required` | the node cannot be built without it |
| `one_of` | a group name for alternatives: `nc.delay` takes `samples` **or** `ms`. Giving two members of a group is refused rather than resolved by precedence |
| `one_of_required` | whether at least one member of that group must be present. True for the delay's time, false for its line length, which may be declared in either unit or not at all |
| `options` | for `kind: "option"`, an array of `{value, label}` — the wire value and the display label are not assumed to be the same string |
| `options_from` | for an option whose values live on the device: `"models"` or `"irs"`, resolved through the asset registry (slice D). Exactly one of `options`/`options_from` is present |

### Ranges: the schema's bounds and the engine's

Some limits cannot be written down in a schema because they depend on the rig: a filter's real
maximum is Nyquist **at the rig's sample rate**, and a delay's is the buffer the engine sizes for it.
Those parameters publish no `max` at all, and the node's own check owns the bound and names it in the
error. The distinction shows up in the message: *"the schema's maximum is 24"* is a client bug,
*"30000 Hz is at or above Nyquist for 48000 Hz"* is a client asking for something the rig cannot do.

## What enforcing it buys

`validate()` lints every node against its type before anything else looks at the graph, so a rig — from
a file, from `rig.apply`, or built by a patch — is rejected with a message that names the node and
lists what it takes:

```
node 'g' (nc.gain) has no parameter 'gainz'; it takes 'gain_db', 'gain', 'ramp_ms' (alias)
node 'n' (nc.eq.biquad): 'q' is 0.000000, and the schema's minimum is more than 0.000000
node 'n' (nc.eq.biquad): 'mode' is 'squelch'; it takes 'peaking', 'lowshelf', …
node 'amp' (nc.nam) needs 'model'
node 'd' (nc.delay) needs one of 'samples' or 'ms'
```

The compiler is where a rig is rejected, not the render path: a bad graph never reaches a plan, and a
render thread never sees an unknown name.

Three claims are asserted rather than promised, in `tests/test_schema.cpp`:

1. **Every type describes itself** — a name, a category, unique parameter ids, an option that is either
   a fixed set or a runtime source and never both.
2. **A declared default is the engine's own fallback.** For every type, a node built with no
   parameters renders identically to one built with every declared default written out. This is what
   keeps a rig from sounding different in the app than in the engine.
3. **Every declared parameter does something.** Each one either changes a render when set away from
   its default, or is in an explicit exception table with a reason (`ramp_ms` changes how a *later*
   ramp moves; `outputs` changes how many ports exist; …). A declared-but-ignored parameter fails the
   build.

## What is not here yet

- **Per-port parameters.** `nc.mix` can sum N inputs but has no per-input gain: a lane's level is a
  `nc.gain` node in the lane, which is what a client's flattening already produces. Per-port gain,
  mute and phase are slice B4.
- **Ramping for a filter or a delay time, and how it is done.** These are rampable, and not by
  interpolating anything: a biquad runs the old and the new coefficient sets in two lanes and
  equal-power crossfades their outputs (`rt::Crossfade`, the same law a plan transition uses), because
  linearly interpolating `b0..a2` between two stable filters is not guaranteed stable and is not the
  response anyone asked for at the midpoint; a delay crossfades between the old and new read taps,
  because interpolating the read position would be a varispeed slide rather than a new delay time.
  Two consequences a client can see: a change is a *transition* of one smoothing time (`ramp_ms`), and
  a delay can only be lengthened into a line the rig declared — hence `max_ms`/`max_samples`.
- **Macros** — a `kind: "macro"` entry with the same parameter shape, once there is a lowering layer to
  put behind it (slice E).
