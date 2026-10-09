# The warm set: a document rendered beside the audible one (namchain M3)

**Written:** 2026-10-09. This is P6 of `pedal.project
docs/managing-processing-power-and-optimising-for-fast-switching.md` — the item everything else warm
waits on, because P4's first-sight measurement needs an inactive core and P8's catch-up needs
somewhere to catch up *to*. It is the design the code follows; where the code is the argument, this
says why.

**Scope of this slice.** One warm document, beside the audible one. The warm set is the footswitch
*neighbours* and nothing more: D4's packing of many rigs into one core, P8's catch-up and P4's
measurement are later work, and the last section says what is deliberately not here. What *is* here
generalises: a document belongs to a thread, the device thread renders what is audible, and a switch
hands a document over rather than loading one.

## The rules this implements

From the pedal's document, as amended by the owner on 2026-10-09:

* A rig renders on one core, all of its chains, and a switch never splits one.
* One core is housekeeping. It is **not** where the audio interrupt lives: the audio IRQ is handled
  by one of the render cores, and that core is never the active one. It follows that warm work must
  tolerate being preempted, because on the pedal the audio IRQ (SCHED_FIFO 85) shares a core with a
  warm thread (70).
* Every other core keeps rigs warm. Exactly one render core at a time is **active**: it owns the
  device and renders the rig the player hears. The rest are **inactive**.
* A switch makes an already-warm rig audible with **no load, no parse and no allocation on a render
  thread**. It changes *which document the device thread renders*; it is not a publish.
* If the target is not warm, a switch **falls back** to a cold publish rather than refusing: a player
  pressing a footswitch always gets sound.

Withdrawn by the owner on 2026-10-09: "no rig ever migrates between cores". A rig's *execution* may
move cores at a switch. What must not move is its compiled state, and it does not: the plans, their
arenas and their buffers stay where they were allocated, and only which thread executes them changes.

## Topology

On the pedal (`vip`, four cores):

| core | role | priority |
|---|---|---|
| 0 | housekeeping: `pedald`, the engine's control side, the OS, every IRQ except the audio one | — |
| 1 | warm, **and** the audio DMA interrupt's delivery | warm 70, IRQ 85 |
| 2 | warm | warm 70 |
| 3 | active: owns the device and renders the rig being heard | 80 |

The configuration names this: `cpu` is the device core (3) and `warm_cpus` the warm ones (1, 2). The
device core may not appear in `warm_cpus`: a warm core that is also the device core would be a core
asked to render a document nobody can hear while the engine's own thread is pinned there.

This slice uses one warm core for one warm document. The second is in the layout because the layout
follows the rules, not because this slice needs it.

## Ownership: a document belongs to exactly one thread

The refactor this rests on is that the render state is **per document** (`DocRuntime`) rather than per
daemon: the chain array, each chain's `in`/`out`/`old`/`incoming` buffers and crossfade, the
document's output bus, its chain count, its generation bookkeeping and its one-transition-at-a-time
flag. A fixed array of them (`kMaxDocuments`, two today) *is* the warm set. Everything the render path
did to `chains_` and `bus_` it now does to one document's copy, and with one document that is the same
instructions in the same order — which is what makes "warmth off" bit-identical rather than merely
similar.

A document is owned by one thread at a time, and ownership is a value either thread can read without a
lock:

```cpp
enum class DocOwner : uint8_t { Free, Device, Warm };
```

Only the owner executes that document's plans; only the owner writes its buffers. Ownership changes by
a **two-phase handover**, never by a swap both threads perform:

1. The giver stops rendering at a block boundary, clears its chain pointers and stores `Free` with
   release ordering. Everything it did to those plans is now visible to whoever takes them.
2. The taker sees `Free`, claims it with a compare-exchange to its own owner, and only then touches the
   plans.

The device thread never blocks on this. It *asks* for a handover and looks once per block: if the
document is not free yet it renders what it has and looks again next block. A switch that has to wait
is counted, never waited for — a missed deadline is worse than a switch that is one block late, and
1.33 ms is not something a player can hear.

## The warm thread

One thread per warm document, **born on its own core**: a thread inherits its creator's affinity, and a
thread created on the device's core cannot run to pin itself once the device thread is `SCHED_FIFO`
there. Then `rt::apply_scheduling(warm_priority, warm_cpu)`, and the stack it will run on is touched
before the clock starts.

It is **paced by the device thread, not by the clock**: the device thread publishes the input block it
has just read, and the warm thread renders as many published blocks as are waiting. That is what makes
falling behind harmless and catching up automatic:

* warm work is not on a deadline. It is *warmth*, and a document one block behind is still a document
  that can be switched to inside D10's 50 ms;
* a warm thread the audio IRQ preempts renders its block late and then renders the next one
  immediately, so the lag does not accumulate;
* nothing the warm thread does can make the device thread miss: they share no buffer the warm thread
  writes.

Input crosses as a small **pool of blocks**. The device thread decodes the capture into a free block
and pushes its index on a single-producer ring; the warm thread pops, renders, and returns the index
on a second ring. A full ring means the warm document is behind by the whole pool, so the device
thread drops that block (counted) rather than growing anything: the document's state then has a gap in
its input, which is what "behind" means, and the count is what T8's falsifier reads. Storage is
fixed-size and sized when the device opens, and the rings are the same `rt::SpscRing` the command path
uses.

The warm thread parks when it has nothing to do, using the same publish/`parked`/`notify` handshake
`RenderPool` uses, so a busy warm thread makes no syscall and an idle one does not burn a core.

**What it renders.** Every document that is `Free`, is not the active one, and has a content id: the
thread claims it and renders it warm. That one rule covers the whole of the handover — after a switch
the warm thread picks up the document the device thread has just released, with no message between
them — and it is why the device thread does not have to tell it what to do.

## The device thread's block

Unchanged in shape, extended by one document and one publication:

1. drain commands (plans published to the *active* document, as today);
2. decode the capture into each document the device thread owns, and — if a warm document exists —
   publish one decoded copy into the warm input pool;
3. run every device-owned document's chains;
4. per document: the chain crossfades, the local transitions and the bus sum into that document's bus;
5. the document switch, if one is running: the incoming document's bus is crossfaded against the
   outgoing document's published block, one `rt::Crossfade` per playback channel;
6. encode and write, as today.

Steps 2–4 for a single document, with no switch running and no warm document, are exactly what ran
before, which is asserted rather than assumed: a test renders one rig and one input through a `file`
backend with warmth off and with warmth on but unused, and compares the two outputs byte for byte
against each other and against the recorded golden output.

## A switch, block by block

The incoming document is already rendering: it has plans, it has state, and it has just been fed the
same input as the audible one. A switch does not compile anything. It hands a document over and fades
between two documents that are both already correct.

**The device thread does not render both documents during the fade.** That is the one thing this
design is shaped around. Two documents on the device core would be twice its rig's cost for the length
of the fade, and D3's headroom (0.8) is sized for a *document plus one chain* — the existing staggered
publish — not for two documents. So during the fade each thread renders one document, and the device
thread mixes its own fresh block against the outgoing document's published one:

| block | device thread | warm thread |
|---|---|---|
| N | asked to switch to *B*. Renders *A*, publishes its bus. Asks for *B*'s release | sees the release request, stops, clears its pointers, stores `Free` |
| N+1 | sees `Free`, claims *B*, renders it, starts the fade. Releases *A* and marks it to be published | — |
| N+1..N+k | renders *B*; mixes it against *A*'s last published block, updating that side from *A*'s ring each block | claims *A*, renders it warm, publishes its bus |
| N+k | fade closed: *B* alone is audible, *A* is warm and current. Stops reading *A*'s ring | continues rendering *A* |

Two properties fall out of that ordering, and they are the reason to accept a cross-thread read in the
middle of a fade at all:

* **Both sides of the fade are continuous.** *B* was rendered by the warm thread up to block N and by
  the device thread from N+1, with no gap in its input; *A* was rendered by the device thread up to
  block N and by the warm thread from N+2, likewise. The fade joins two live streams rather than
  fading to something that is about to start.
* **The device thread never waits for the warm thread.** It reads the published ring and, if the warm
  thread has not published this block, holds the last block it read. A hold is counted; it is a
  plateau on the *fading-out* side, never a stall on the audible one.

The outgoing side is therefore one block old (1.33 ms) at worst, inside a fade of `fade_ms` (5 ms by
default), between two signals that are uncorrelated by construction — two different rigs. That is the
case `rt::Crossfade` documents itself as correct for.

The handover is one block of setup: the release request is answered at the warm thread's next block
boundary, not immediately. A switch therefore costs `fade_ms` plus a block or two — 6.3–8 ms at 64
frames, against D10's 50 ms — and can be deferred by any number of blocks when the warm thread is late,
which on the pedal it will be whenever the audio IRQ lands on it. Deferrals are counted and reported
rather than hidden.

If the warm document is behind — its state is older input — the switch is still correct as *audio*: it
fades from what is playing to what the warm document is playing, and both are continuous. What is not
guaranteed is that the incoming document's history is the one the player would have heard had their
foot been on that switch all along. That is what `behind_blocks` measures and what T8 falsifies.

## What is guaranteed, and what is not

* **No allocation, no lock and no blocking on either render thread.** The warm path allocates nothing
  after start-up; a document's buffers are sized when the device opens. `NAMCHAIN_RT_CHECK` covers both
  threads.
* **A switch never fails for timing.** It may be deferred; it cannot be dropped. If the document is
  cooled or replaced between the request and the claim, the request is withdrawn — the device thread
  checks the document's *content* as well as its owner — and the audible document keeps playing.
* **The audible audio never waits for warm work.** The device thread reads the warm side through
  atomics and a ring it drains non-blockingly.
* **A rig's execution may move cores, and that has a cost.** The plans move to the device core with the
  document, so the first blocks after a switch read a model whose memory was last touched by another
  core. It is a transient — a working set that already fits one core's L2 — and it happens inside the
  fade. This is the L2 effect the owner accepted when the "no rig migrates" rule was withdrawn, and it
  is part of why a switch is a fade and not a cut.
* **The cold fallback is a publish.** A rig that is not warm is loaded, compiled and crossfaded exactly
  as `rig.load` does it today, with the same cost and the same latency. Warmth is what makes a switch
  fast; it is not what makes it possible.
* **Warmth is not measured by the cost table.** A row's `n` was measured on a quiet core; warm work
  shares its core with the audio IRQ on the pedal, so the table's number is an upper bound on what the
  warm document gets and `behind_blocks` is how the difference shows.
* **`render_threads` above 1 and warmth together are refused**, by name, at startup. M4 is chain-level
  parallelism *inside* one document and has never been measured with a second document rendering beside
  it (docs/multi-core.md); D1 keeps both off in the shipped configuration, and a combination nobody has
  measured is not one to guess at.

## Reclamation

A plan is still only ever freed by the control thread, after a grace window: the render path retires
and the control thread frees (rig-pipeline.md §6.4). There are now two producers rather than one, so
there are two `rt::Reclaimer`s — the device thread's and the warm thread's — and the control loop
collects both. A document handed from one thread to the other retires nothing: the plans move, they are
not freed.

Retirement happens when a document's *content* is replaced (a publish into the active document, a
`rig.warm` over a warm one) or when a document is cooled. The first is done by whoever owns the
document at that moment. The second is different: once the warm thread has released a cooled document,
no thread owns it and the control thread frees its plans directly — which is why `rig.cool` waits,
bounded, for that release to be visible. The alternative, freeing a plan a render thread might be
inside, is the failure the reclaimer exists to prevent.

## Configuration

| key | default | meaning |
|---|---|---|
| `warm_cpus` | *(empty)* | the cores a warm thread may be pinned to; empty means warming is off. The device core (`cpu`) may not appear here |
| `warm_priority` | `70` | `SCHED_FIFO` priority for a warm thread: below the render thread (80) and below the audio IRQ (85) |
| `warm_max_behind_blocks` | `8` | how far behind a warm document may fall before `engine.status` carries the `warm_behind` issue |

Two fields exist for tests and are **never parsed from a file or the command line**:
`warm_test_hold_ms` (the warm thread holds each block this long, so a test can starve it) and
`warm_test_pace_us` (the device thread paces its loop, so a test can drive a switch mid-render against
the unpaced `file` backend).

## The protocol

Version 2, additive, like every other method here.

| method | request | reply |
|---|---|---|
| `rig.warm` | `{"rig": "name.json"}`, or `{"document": {…}}`; `{"replace": false}` to refuse rather than displace a warm rig | `rig`, `name`, `doc`, `core`, `generation`, `state` (`warming`\|`warm`), `cost` (the same object `rig.cost` returns), `replaced` (what was displaced, or null) |
| `rig.cool` | `{"rig": "name.json"}`, or `{}` for whatever is warm | `cooled` (the rigs that stopped being warm), `docs_freed` |
| `rig.switch` | `{"rig": "name.json"}`; `{"fallback": false}` to refuse instead of loading cold | `switched` (`warm`\|`cold`), `rig`, `doc`, `core`, `generation`, `handover_blocks`, `fade_ms`, `completed`, and `reason` when it was cold |

`rig.switch` against a rig that is not warm is the fallback: it publishes that rig exactly as
`rig.load` does and says so. A footswitch press is never answered with a refusal.

Error codes: `warm_disabled` (no `warm_cpus` configured), `warm_full` (the warm set is full and
`replace` was false), `warm_not_ready` (`fallback: false` and the rig has not rendered yet, or the
named rig is not the warm one), plus the existing `rig_not_found`, `compile_failed`, `asset_missing`
and `over_budget`.

## Engine status

```json
"warm": {
  "enabled": true,
  "device_core": 3,
  "cores": [1, 2],
  "priority": 70,
  "active_doc": 0,
  "switches": 4, "cold_switches": 1, "deferred": 0, "withdrawn": 0,
  "set": [
    {"rig": "/var/lib/pedal/library/rigs/next.json", "name": "next", "doc": 1, "core": 1,
     "cost": 0.41, "verdict": "fits", "generation": 7, "state": "warm",
     "blocks": 9120, "rendered": 9120, "behind_blocks": 0, "worst_behind_blocks": 2,
     "blocks_dropped": 0, "publish_holds": 0, "handover_blocks": 3}
  ]
}
```

`state` is `warming` until the document has rendered its first block, `warm` after that, and `audible`
for the one the device is hearing. The whole object is absent when warming is off, which is how
`levels` says it is not measuring.

Issue codes, worded like their neighbours: `warm_behind` (warning: a warm document is more than
`warm_max_behind_blocks` behind, the live form of T8's falsifier) and `warm_not_ready` (informational:
a warmed document has not rendered its first block yet).

## Measurements: T7 and T8

* **T7 — the switch's latency at the jack.** Two halves. Offline, `namchain-warm-bench switch` runs a
  rig pair the way the two threads do — a paced device thread, a warm thread on another core, the real
  handover — and reports the handover in blocks and microseconds, the device thread's block times
  while warm work runs, and a glitch-detector verdict on the crossfaded output, in both directions.
  On the pedal, the engine's own counters are the other half: `warm.switches`, `deferred`, `withdrawn`
  and each entry's `handover_blocks`, which the harness that measures at the jack correlates with what
  it heard.
* **T8 — the warm-lag falsifier.** A warm document that is permanently behind is *reported*, never
  silently switched to: `behind_blocks`, `worst_behind_blocks`, `blocks_dropped` and `publish_holds`
  per document, the `warm_behind` issue, and the bench arm's own lag distribution under a starved warm
  thread (`--warm-hold-ms`).

## Tests

* Unit (doctest, no threads): the ownership state machine and its refusals — a claim of a document that
  is not free fails, a release is visible to the claim, a stale content id withdraws a request — the
  input pool's rings (a full ring drops and counts rather than growing), and the bench's handover
  bookkeeping.
* Null backend, end to end: `rig.warm` → status says `warming`, then `warm` with blocks advancing;
  `rig.switch` moves the audible document and leaves the outgoing one warm; `rig.cool` while warm is
  safe and frees the plans; a switch to a rig that is not warm falls back and says `cold`.
* File backend, end to end: a switch mid-render, judged for continuity — no gap (no run of silence in a
  rig that cannot produce one) and no click (no sample-to-sample step larger than the fade's own
  slope). Warmth off against warmth on-and-unused: byte-identical output.
* The starvation hook (`warm_test_hold_ms`): the warm document falls behind, `behind_blocks` and
  `blocks_dropped` report it, the `warm_behind` issue appears, the audible output is unaffected, and
  nothing is corrupted — the plans' own guard counters stay at zero.

## Deliberately not here

* **D4's packing** — several warm rigs per core, chosen by cost. The mechanism is a document owned by a
  warm thread; a bank that packs is a policy over the same mechanism, and it is the policy's (D9's)
  job rather than the engine's.
* **P8's catch-up** — a rig that is not warm is switched to cold here. Replaying recent input into a
  cold instance is its own slice, and it wants the tiers the pedal's document describes.
* **P4's first-sight measurement** — measuring a unit on an inactive core while the player plays. Now
  possible, because there is an inactive core with a defined owner.
* **More than two documents.** With two, a switch is a swap and the warm document is the one that is
  not audible. With more, something has to decide which rig is warmed when a rig stops being a
  neighbour, which is a bank policy.
* **`render_threads` above 1 together with warmth** — refused by name, above.
* **Un-warming on the way out.** A rig that stops being a neighbour is cooled by the client
  (`rig.cool`); the engine does not guess.

## The failsafe: the deadline is the truth (2026-10-09)

The coexistence charge (pedal.project D3) is a *measurement* — one board, one arrangement, one set of
rigs — and a measurement can be wrong for the next rig or the next board. So the engine also watches
what actually happened. `check_warm_pressure()` runs on the control thread beside `sync_warm_switch()`
and diffs the render load histogram (one bucket per percent of the deadline, the last one "over it")
against the reading from the last check: the window is the blocks rendered since, so a spike an hour
ago cannot keep the warm set cooled now.

The window is pressure when **any** block in it went over the deadline — D7's bar is zero overruns, so
one is already a fault — or when a hundredth of them ran past nine tenths of it, which a rig with room
does not do (T5 measured 63% at a 0.41 rig and 108% at 0.735). On pressure the warm set is cooled,
`pressure_cools` goes up, and a new `rig.warm` is refused with `warm_pressure` for ten seconds, because
a service asking every second is how a failsafe becomes a loop.

Nothing on the render path consults it: it changes what the engine *holds*, not what it renders, so
the worst case of getting it wrong is that the next press is a cold switch instead of a handover.
`warm_pressure()` is a free function of two histograms, tested without a render thread
(`tests/test_cost_table.cpp`).
