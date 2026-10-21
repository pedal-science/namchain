# The tuner: `nc.tuner`

**Status:** built — the node, its reading (`rig.state`), and the ephemeral-node contract the two front
ends follow. rigpa's main view is `../rigpa/docs/tuner.md`; ljoss's panel is in its
`docs/ARCHITECTURE.md`.

A pedal tuner is not an effect. It hears the instrument and takes it out of the signal path: the
amplifier is silent while the player tunes, and the reading is the only thing on screen. `nc.tuner`
is that, as an engine node — and unlike every other node, it is **not a block**. No palette offers
it, no ribbon draws it, no rig file contains it. It is a UI element's node, mounted by a client while
its tuner is visible and removed afterwards.

## The core, and where it comes from

[`squatch-dsp`](https://github.com/Squatch-Stack/squatch-dsp), `blocks/tuner` — MIT, © 2026 Squatch
Stack. Header-only C++17 with no dependencies, fixed-size buffers, nothing allocated after
`configure()`. It is a **strobe** tuner: the McLeod pitch method finds which note is playing every
5.3 ms, and a bank of software strobes follows the partials of that note to a hundredth of a cent,
with the string's stiffness estimated per note so the fundamental is reported rather than the sharp
average of the harmonics.

The headers are vendored verbatim in `third_party/squatch-tuner/` (pinned at squatch-dsp
`8cb420be…`; `README.md` there says how to update them), and the engine builds without a squatch
checkout beside it — the same rule flowEQ's model table lives by. The node itself is
`src/nodes/tuner.cpp`; this page is the design.

## What the node does

| | |
|---|---|
| Ports | one in, one out |
| Output | **silence** — every sample is exactly `0.0f`, whatever came in |
| Input | read in full; it is what the tuner listens to |
| Parameters | none |
| State | ~128 kB of fixed buffers, allocated at build, never touched by the allocator afterwards |
| Cost | squatch-dsp measures 158.9 ns per 48 kHz sample while a note sounds (0.76 % of one M1 Max core) and 7.2 ns quiet. The pedal's own row is `unknown.node` until `pedal-measure` has measured it |

The mute is the audio behaviour in one line, and it is bit-exact on purpose: a leak of a few
samples is audible when the rig behind the tuner has gain. `process()` calls the core's
`process()` with the block's first channel, then fills the output with zeros, then publishes. The
compiler binds an unconnected input port to the plan's silence buffer, so a tuner with nothing
feeding it is legal and hears silence.

There are **no parameters**. A4 is the 440 Hz a guitar pedal tunes to, and the search covers 38 Hz
(a 5-string bass's low B) to 1400 Hz (the 24th fret of the high E) — both in `src/nodes/tuner.cpp`
as `kA4`, `kMinHz`, `kMaxHz`. A reference-pitch control is a parameter and a UI; when it is wanted it
arrives as `a4` with a range around it, and the schema test that every declared parameter changes a
render gets its exception with a reason.

## The reading: `rig.state`

The node publishes what it has heard as `rig.state`'s entry for it, once per block through an
epoch-guarded snapshot — the pattern `nc.eq.flow` established (`docs/daemon.md`, "What a node has
derived"). A client draws it; it never runs a tuner of its own.

```jsonc
{ "id": "…", "method": "rig.state" }
```
```jsonc
{ "ok": true, "generation": 3,
  "nodes": [ { "chain": "main", "node": "tuner", "element": "", "role": "",
               "type": "nc.tuner",
               "state": { "a4": 440.0, "blocks": 1450, "valid": true, "level_db": -17.2,
                          "midi": 45, "note": "A", "octave": 2, "cents": -1.73,
                          "hz": 109.56, "steady_hz": 109.98, "clarity": 0.93,
                          "uncertainty_cents": 0.08, "partials": 5 } } ] }
```

| field | meaning |
|---|---|
| `a4` | the reference the note and cents are read against |
| `blocks` | blocks rendered since the node was built. It wraps (a uint32 at 750 blocks a second is 66 days), and a client compares two reads of it to tell a live reading from one the render thread stopped updating |
| `valid` | whether the rest is a reading to trust |
| `level_db` | the input's RMS at the tuner's work rate, floored at −120. Always present, so a client can show "something is plugged in but nothing is ringing" |
| `midi`, `note`, `octave` | the nearest equal-tempered note. `note` and `octave` are computed here so no client keeps a note-name table; `octave` is scientific pitch (MIDI 60 is C4) |
| `cents` | the fundamental's offset from that note, signed |
| `hz` | the fundamental now, glides included — what a strobe animates to |
| `steady_hz` | the fundamental averaged over the fit window — what a number and a needle show |
| `clarity` | the coarse stage's confidence, 0…1 |
| `uncertainty_cents` | the fit's own standard error; a reading noisier than 1 cent is withheld, which is why a valid reading is a real one |
| `partials` | how many partials the fit used |

**When `valid` is false the note keys are absent**, not stale: a client shows "no note" rather than
the last note played. `a4`, `blocks`, `valid` and `level_db` are always there.

The engine withholds a reading in more cases than a player would expect, deliberately: while a note
decays into the room's hum and noise floor, while partials disagree by more than 3 cents (a
sympathetic string, hum), while the mains frequency starts a "note", and when a stale decimal would
be a lie. That is the vendored core's design, measured in squatch-dsp's `blocks/tuner/RESULTS.md`.

## Why it is not a block

`engine.schema` publishes `flags.in_palette: false` for the type. That flag is the engine saying what
a palette should offer, so a client keeps no list of exclusions of its own (`docs/schema.md`) — the
alternative is a table in every client that goes stale silently.

It is **not** `internal`: an internal type (`nc.xfade`) is the engine's to place and a document may
not name it at all. This one exists to be named by a client, which is how it is mounted. What the
flag buys is that no player can add a tuner to a chain — and a rig that holds a tuner node as a block
would be a rig that is silent for a reason nobody could see.

## The ephemeral node

The contract a client follows. It is defined here because it is the document that carries the node.

1. **Mount**: to show its tuner, a client splices one `nc.tuner` into **each chain, immediately after
   that chain's `nc.input`**, and repoints every connection whose `from` was the input node to the
   tuner's id. Everything the input fed now hangs off the tuner, which is silence.
2. **Mark it**: the node carries `"meta": {"ephemeral": "tuner"}`. `meta` is the engine's passthrough
   — kept exactly as written, returned by `rig.get`, kept through a `rig.patch` — so a client can
   recognise *its own* node and nothing else, and a rig that happens to hold a hand-written tuner
   is not touched by a strip.
3. **Id**: `"tuner"`, or `"tuner-1"`, `"tuner-2"`… if the chain already uses that id. Ids are
   chain-local, so two chains each have their own; the marker, not the id, is the identity.
4. **Never record it**: it goes into the document that is *published*, not into the client's model,
   so it is not drawn, not undoable, and never saved. A client that reads the engine's document back
   (`rig.get`) strips it before parsing, or its canvas and its library would acquire the node.
5. **Unmount**: removing it is the exact inverse — each consumer of the tuner's output is repointed
   at the tuner's predecessor (the input node), the connections into and out of the tuner are
   dropped, and the node goes.

The splice and the strip are document-JSON operations, not tree ones, so they reach chains a client
considers un-editable (a hand-written fan-out) and the bare-graph form of a one-chain document
equally.

Consequences worth knowing: a mount and an unmount are each one structural publish, so each is one
crossfade (a few milliseconds, no click); a rig with two chains gets two tuner nodes, one per chain
channel, and a client shows one of the readings; and while a node exists the chain is silent — which
is why a client that cannot publish the document must say so rather than pretend the rig is muted.

## What is not here

- **A moving strobe.** The core publishes per-partial phases and a client *could* draw a strobe from
  them, but a 30 Hz poll of an absolute phase shimmers. A real strobe wants either a phase rate from
  the engine or a client integrating `cents` on its own clock; neither is in this slice.
- **A reference-pitch control** (see above), and the polyphonic strum check (`poly.hpp` is vendored
  and unused).
- **A footswitch path.** The pedal's tuner will be a footswitch through `pedald` → rigpa; that path
  is the Pico controls work, not this node. Until then the tuner is a menu button.

## Where this is implemented

| | |
|---|---|
| the node | `src/nodes/tuner.cpp`, `include/namchain/tuner.h` |
| the vendored core | `third_party/squatch-tuner/` (README, `SHA256SUMS`, `LICENSE`) |
| the fetcher | `tools/vendor/fetch_squatch_tuner.py` (`--check` verifies the tree) |
| the flag | `NodeType::in_palette` (`include/namchain/graph/plan.h`), published in `engine.schema` |
| the tests | `tests/test_tuner.cpp` (mute, reading, silence, counter, render-path allocation), `tests/test_schema.cpp` and `tests/test_daemon.cpp` (a document may name it; `rig.state` reports it per chain) |
