# flowEQ: `nc.eq.flow`

**Status:** built — the node, its model table, its report (`rig.state`) and rigpa's panel. This is the
engine's design and the provenance. The panel is rigpa's (`../rigpa/docs/floweq.md`).

flowEQ is Christian Steinmetz's EQ, presented at the AES MATLAB plugin competition (2019) and
published as [flowEQ](https://github.com/csteinmetz1/flowEQ): a **disentangled variational
autoencoder** over the thirteen parameters of a five band parametric equaliser. Rather than moving
five bands by hand, a player moves a point through a three dimensional latent space and the decoder
turns it into all thirteen parameters at once, so one gesture is a whole timbre — the "tone control"
the project is about.

The upstream plugin is MATLAB (`audioPlugin` + the Audio Toolbox). None of that can run in this
engine, so the *algorithm* was re-implemented and the *model* was converted. Nothing in MATLAB runs
on the pedal; what is kept is what flowEQ actually is — one small decoder and five biquads.

## What the lite version is, and what is not here

flowEQ ships a full plugin and a **lite** one. The lite interface is what the pedal's one screen can
carry, and it is what `nc.eq.flow` implements:

| control | what it does | here |
|---|---|---|
| **X, Y, Z** | the latent point; the decoder turns it into the thirteen EQ parameters | `x`, `y`, `z`, −2…2 |
| **Extend** | doubles the latent values before the decoder, reaching further out | `extend` |
| **Strength** | scales every band's gain; 0 is the identity filter | `strength`, 0…1 (published as %) |
| **Auto Gain** | matches the output loudness to the input's (BS.1770 short-term weighting), clamped to ±12 dB | `auto_gain` |
| **In Gain / Out Gain** | level before and after the EQ; Out is not in the path while Auto Gain is on | `in_db`, `out_db`, −60…+12 |

Not here, and deliberately: the full plugin's **semantic mode** (the "Embedding A/B" codes and
their interpolation), its **Latent** (1D/2D/3D) and **β** model selectors — twelve decoders in all —
its **manual** mode with per-band controls and bypass switches, and its Auto Gain update-rate and
range controls.

The model is fixed to the one flowEQ-lite hardcodes, `vae3d_beta_0.0010`: three latent dimensions,
β = 0.001. Adding the other eleven is mechanical — `tools/floweq/generate_model.py` already writes
whichever `.mat` it is given — but each brings a model selector and, for 1D and 2D, a latent space
whose unused axes have no meaning, which is a UI question before it is an engine one.

## The model, and where the weights come from

```
z (3) ──► Dense 1024, ReLU ──► Dense 13, sigmoid ──► 0…1 ──► gains, frequencies, Qs
```

`models/20261017-090756/` in the flowEQ checkout holds the trained Keras models and
`plugin/assets/vae3d_beta_0.0010.mat` the decoder converted for the plugin. That `.mat` is the source
of `include/namchain/floweq/model.h`:

```sh
tools/floweq/generate_model.py third-party/flowEQ/plugin/assets/vae3d_beta_0.0010.mat
```

The generator is plain Python 3 — it parses MAT v5 with `struct`, so it needs no numpy, scipy or
h5py — and it refuses a file whose arrays are not `(3,1024)`, `(1,1024)`, `(1024,13)`, `(1,13)`, or
a weight that does not round-trip through float32. It emits the four arrays row-major as hex float
literals, which are exact, plus the model's name, the hash of its weights, `Decoder.m`'s
normalisation ranges and the parameter names. **The header is checked in**: the engine builds without
the flowEQ checkout beside it, and the table is 70 KB of read-only data.

`tests/test_floweq.cpp` checks the C++ decoder against the same forward pass in double precision for
three latent points (`generate_model.py --golden` prints them), so a transposed weight matrix or a
header generated from the wrong file cannot pass.

**Licence.** flowEQ is BSD 3-Clause, Copyright (c) 2019 Christian Steinmetz; the full text is in that
repo's `LICENSE`. The decoder weights in `model.h`, the EQ's five sections and the auto-gain law are
derived from it, and the generated header and this page are where that is recorded. The training data
is SAFE-DB's parametric-EQ user data (`data/safe/` in the flowEQ checkout).

## The render path (`src/nodes/floweq.cpp`)

Three decisions are worth knowing:

**The decoder runs at most once per block.** A client dragging three knobs streams three
`set_param` dispatches in one block; each only marks the coefficients dirty, and `process` decodes
once. The pass is about 18 000 multiply-adds — a round-off beside a NAM model, but not something to
do three times for one gesture.

**A latent change is a crossfade, not an interpolation.** The five sections in force keep running in
a second lane while the new ones take over, and the two lanes are equal-power mixed over `ramp_ms`
(`rt::Crossfade`, the same law a plan transition uses). Interpolating b0…a2 between two stable
filters is not guaranteed stable and is not the response anyone asked for in the middle. A second
change mid-fade retargets the incoming lane instead of restarting the mix. `ramp_ms` is also the time
constant of the two gain smoothers, so `ramp_ms = 0` means "everything immediate" rather than
"immediate filters, slow gains".

**`strength = 0` is exactly the identity.** Every gain becomes 0 dB, which makes each section's
numerator and denominator coefficients identical; the node notices (`Chain::identity`) and skips the
filters altogether. That is partly cost and partly honesty: under FMA contraction the all-identity
recurrence is not bit-exact, and a strength of zero should not leave a −110 dB noise floor the plugin
does not have. `nc.mix` has the same fast path for the same reason.

Auto Gain is a measurement, not a knob: the pre-filter and post-filter signals (both after In Gain,
neither after Out) run through BS.1770‑4's K-weighting, and a one-pole mean square over the standard's
3 s short-term window gives each side's loudness. The output gain follows the ratio, clamped to ±12 dB
(the plugin's default range) with a one-pole at `ramp_ms`; while the two mean squares are still
round-off it holds rather than moving. The weighting is the standard's shelf and high-pass,
parameterised for any sample rate — at 48 kHz they reproduce the published coefficients exactly. The
channel weights are not here because the engine is mono and a ratio needs no absolute loudness.

## What a client can see: `rig.state`

The node publishes a snapshot of what it has derived — the latent point, strength, the gains in
force and the thirteen decoded parameters — into atomics with an epoch (odd while writing), and its
`describe_state` builds the report from it on the control thread. The render thread never allocates,
and the reader never sees half an update. `rig.state` (docs/daemon.md) serves it:

```jsonc
{ "chain": "main", "node": "eq", "element": "eq", "role": "", "type": "nc.eq.flow",
  "state": {
    "model": "vae3d_beta_0.0010",
    "latent": [0.5, 0.0, 0.0], "strength": 0.5, "extend": false,
    "auto_gain": false, "in_db": 0.0, "out_db": 0.0, "gain_db": 0.0,
    "bands": [ { "type": "lowshelf", "gain_db": -0.25, "freq": 150.48, "q": 0.71 }, … ],
    "curve": { "f_min": 20.0, "f_max": 20000.0, "points": [ … 96 dB values … ] } } }
```

- `bands` are the five sections with **strength applied** — what the curve shows — rounded to a
  hundredth of a dB and a hundredth of a percent.
- `curve.points` is the magnitude response in dB at 96 log-spaced frequencies from 20 Hz to
  `f_max` (20 kHz, or 0.45 × the rate if the rig runs lower), computed from the same coefficients
  the render uses. A client draws it; it does not have to re-derive the filters.
- `gain_db` is the output gain **actually applied**, which with Auto Gain on is the measured
  compensation rather than `out_db`.

The response maths lives in this repo only (`floweq::response_db`), so a UI cannot disagree with the
audio about what the EQ is doing.

## Ranges, and the one place the engine is stricter than the source

`Decoder.m` maps the sigmoid's 0…1 onto fixed ranges: gains −12…+12 dB, the low shelf 22…1000 Hz,
the three bands 82…3900, 180…4700 and 220…10000 Hz with Q 0.1…10, and the high shelf 580…20000 Hz.
At 44.1 or 48 kHz every frequency the decoder can produce is below Nyquist. At a lower rate (this
engine can run at 32 kHz) the high shelf's range does not fit, and rather than design an aliased
filter the node clamps a frequency to 0.45 × the rate. The plugin itself assumes 44.1/48 kHz and has
no guard; the pedal runs at 48 kHz, where the clamp never bites.

## Cost

| | |
|---|---|
| code | one file, ~800 lines; the model table is 70 KB of `.rodata` |
| state | about 1 KB a node, plus two buffers of `max_quantum` |
| steady state | five biquads (25 multiply-adds a sample) + two K-weighted meters (~25) + two gains |
| a change | one decoder pass (~18 000 MACs) once per block in which a latent, strength or Extend change arrives, then the crossfade runs two chains for `ramp_ms` |

The cost table charges a node type it has no row for as `unknown` (D8), so a rig with a flowEQ is
admitted against that until `pedal-measure` measures the row on the device.

## Files

- `include/namchain/floweq.h` — the decoder, the five-section design and the response maths.
- `include/namchain/floweq/model.h` — **generated**, the trained weights and their normalisation.
- `src/nodes/floweq.cpp` — `nc.eq.flow`: parameters, render path, auto gain, report.
- `tools/floweq/generate_model.py` — the converter, and `--golden` for the test's fixtures.
- `tests/test_floweq.cpp` — the decoder against its reference, the response against the plugin's
  formulas, the ramps, auto gain and the report.
- `../rigpa/docs/floweq.md` — the panel that draws it on the pedal.
