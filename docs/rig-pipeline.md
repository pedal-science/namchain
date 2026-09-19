# The rig spec pipeline: load, parse, compile, apply

**Status:** design, partly built. Load, parse and compile are implemented and tested. Apply is
partial: `namchaind` compiles a new plan on its control thread, publishes it through the SPSC ring
and retires the old one after a grace window, but there is no dual-render, no parameter ramp and no
admission control. Control-side awareness is partial: the daemon's socket, protocol, `signalfd`
reload and device-loss survival exist; the offline edit timeline and telemetry subscription do not.
The audio-side command ring is drained at the top of each block, but only plan identity travels on
it. The last section says exactly which is which, so nothing here is mistaken for running code.

**Names.** Three stages, three words, because using one word for all of them is how a conversation
about latency ends up talking about the wrong thing:

| | what it is | where it lives |
|---|---|---|
| **rig spec** | the JSON document, on disk or on the wire | `rigs/*.json`, the `rig.load` request body |
| **`Graph`** | the parsed, validated in-memory IR | `include/namchain/graph/graph.h` |
| **`RenderPlan`** | the compiled, immutable execution artifact | `include/namchain/graph/plan.h` |

One wrinkle worth knowing: plan §5.1 reserves "Rig" for a future *superset* — a set of chains plus
scenes — whereas today one rig spec is one graph is exactly one chain (enforced: exactly one
`nc.input` and one `nc.output`, `src/graph/plan.cpp`). Calling the document a **rig spec** sidesteps
that collision; "Rig" can then mean the superset without ambiguity.

---

## 1. Load a rig spec

Control thread, ordinary file I/O: read the bytes, hand them to the parser. Nothing clever, with one
subtlety that is already implemented — a string parameter that looks like a path (a `model` path)
resolves against the **rig spec's own directory** first, so an example rig copied next to its models
needs no editing. The committed `rigs/*.json` are copied into the generated-fixture directory in CI
for exactly this reason.

`load_graph_json(path, Graph&, error)` in `include/namchain/graph/rig.h`.

## 2. Parse a rig spec

Text to `Graph`: nodes `{id, type, params}` and connections `{from, from_port, to, to_port}`, where
`id` and `type` are reserved and every other key is a parameter — a number or boolean becomes a
numeric param, a string becomes a string param, so `"model": "amp.nam"` needs no wrapper. Explicit
`"params"`/`"strings"` objects are also accepted.

A structured value (an array or object) is **rejected rather than dropped**, because silently
ignoring a nested object would compile and run while discarding what the rig author meant. Relative
paths resolve as described above.

Then structural validation: unknown node type, duplicate id, dangling connection, port out of range,
two connections into one input port, cycles (rejected via Kahn's algorithm), and exactly one input
and one output node. Errors are structured strings — nothing throws across the boundary, which is
what lets a failed load be reported to a client instead of killing the daemon.

`parse_graph_json(text, Graph&, error)` and the validation pass in `src/graph/plan.cpp`.

## 3. Compile a rig spec

`Graph` to `RenderPlan`, on the control thread: lint → lower → memory plan → cost model → a
topological node array with every input and output pointer pre-bound.

The part worth understanding is the memory plan. Liveness analysis plus interval colouring over
intermediate buffers, all allocated out of one arena, means a serial chain of six gains collapses to
a handful of buffers rather than one per node — measured at 3 buffers where the naive layout needs 6,
and 4 where it needs 7 for two parallel chains. The compiler has an **oracle**: `reference.cpp` is a
naive evaluator of the same IR that gives every value its own storage and orders nodes by depth-first
post-order instead of Kahn. Comparing a plan against a second run of itself proves determinism and
nothing else; comparing it against the oracle catches schedule, colouring and binding bugs, block for
block, and the soak does that in process.

Compilation is **transactional**: any error leaves the running plan untouched and returns a
structured error. A plan is published by an atomic index store; the previous plan is retired (see
§6). Everything a plan needs — buffers, bound pointers, page-touched memory — exists before
publication, so the render path performs no allocation and takes no locks.

`src/graph/plan.cpp` (implemented for validation, ordering, colouring, binding; lowering, PDC and
the cost model are still to come).

## 4. Apply a rig spec — altering the live graph to match

Two very different costs, and the distinction is the whole design:

- **Parameter-only differences never recompile.** They become a POD ramp command applied per sample
  inside the node's gain or parameter slot, so the change is a slope rather than an edit. Cost ~0,
  glitch risk none.
- **Structural differences** (rewire, add, remove, scene recall, model swap) build the complete new
  plan, then **dual-render**: both plans run for `fade_ms`, each chain output is equal-power
  crossfaded from old to new, and only then is the old plan retired. Cost is roughly double for the
  fade duration.

Admission control is what keeps that safe. `worst_case_block = Σ cost(new plan) + Σ cost(old plan
during the fade)`; if that exceeds the budget (default 70 % of the deadline), the compiler shortens
the fade, drops speculative warming, slims models, or **rejects the edit with a structured error**
rather than risking a dropout. One structural transition in flight per chain; further requests
coalesce (last-writer-wins for parameters, serialised for structure). The engine keeps the current
plan rendering through any failure — the only route to silence is an explicit `mute`/`panic`.

A structural commit lands at a **future sample** (`t_commit`), not "next block": the control thread
prepares everything, and the commit happens at the first block boundary where its preconditions hold
(target instance adopted, `warm_through ≥ t_commit`, buffers resident). If `max_defer_ms` (default
15 ms) passes first, the engine commits in **degraded mode** — crossfaded from a silence-prewarmed
state — and reports `degraded: true`. The worst case is a short timbre transition, never a gap.

Plan §5.3 (the four op classes) and §5.4 (the deferred-commit scheduler).

## 5. Awareness: who notices, and on which thread

Two halves, and they are deliberately different. This section covers the **control side**; §6 covers
what the audio thread sees.

### 5.1 The control thread learns first, and is event-driven

The control thread owns the socket server, compilation, publication and reclamation (§3.1). It is
not a polling loop: it blocks in `poll()`/`epoll_wait()` on a small descriptor set, so "becoming
aware" is one of those descriptors becoming readable, or a timer expiring. Five ways it learns, in
order of importance:

1. **A protocol request — the primary path.** A client sends `rig.load` on the Unix
   `SOCK_SEQPACKET` listening socket. The socket becomes readable; the control thread `recv()`s one
   whole message (SEQPACKET preserves message boundaries, so there is no framing to reimplement),
   and that read *is* the awareness. The file is read **after** it, on this thread, because this is
   the thread allowed to block on I/O.
   The request names a rig, and the daemon resolves that name against a configured rigs root,
   canonicalises it, and rejects traversal and non-regular files. A client must not be able to make
   the daemon read an arbitrary path and hand back the parse error.
2. **Startup, from the config file or the command line.** Read synchronously before the engine
   starts, so the first plan exists before the clock does. This is what every tool does today:
   `namchain-soak --graph rigs/two-chains.json`, or `--model --instances N`, which builds the same
   thing through `chain_graph()`.
3. **The in-process API.** `nac_engine_apply_patch(json)` (§8.1), or the C++ equivalent, hands a
   spec over directly — no file, no socket. This is how unit tests drive the path today.
4. **A scripted timeline, for offline runs.** `namchain-render`'s edit timeline (§3.4, planned)
   walks a list of `{at_sample, op}` against the virtual clock and performs the same
   load → parse → compile → publish sequence at each offset. No sockets and no timing luck, which is
   why it is the primary test tool for §5.3 and §5.4: an edit lands at an exact sample, every run.
5. **`SIGHUP`, for the operator's "reload" gesture.** Handled with `signalfd` in the same poll set,
   re-reading the configured rig path. `systemctl reload namchaind` maps onto it, which is the
   natural way to reload a headless appliance from a script.

### 5.2 What I would deliberately not do: watch the rig file

inotify on `rigs/*.json` looks convenient and is the wrong mechanism. It reads half-written files,
it fires on editor temp-file churn, and it turns a deliberate all-or-nothing operation into a race
with whatever is writing the file. The rig should be re-read on an **explicit** request (socket,
`SIGHUP`, `systemctl reload`), and whoever writes a rig spec should publish it atomically — write a
temp file, then `rename()` — so no reader can ever observe a partial spec.

### 5.3 The ordering that matters

The control thread learns first and does all the work — read, parse, lint, compile, allocate, bind,
page-touch — and only then publishes. So the audio thread's "awareness" (§6) is not a notification
of *intent*; it is a notification of *prepared work*. Two consequences:

- A burst of `rig.load` requests coalesces: compile the newest, drop the stale, per §5.3.
- The control loop's poll timeout is the **reclamation window**, not zero and not infinite, because
  somebody has to call `collect()` once `fade + 2` epochs have passed, and that somebody is the
  control thread.

```
for (;;) {
  poll(fds, timeout = reclamation window);   // socket, signalfd, reclaimer timer
  if (rig.load readable)  { recv; read file; parse; lint; compile; publish; }
  if (SIGHUP)             { re-read the configured rig; same path; }
  reclaimer.collect();                       // free plans past fade + 2 epochs
}
```

## 6. Why the signal chain never stalls

The mechanisms, in the order they matter:

1. **Nothing expensive on the audio thread.** No allocation, lock, syscall, parse, log, or
   `std::string`/JSON on the render thread — enforced by RTSan and interposer tests (§10.4), not by
   convention.
2. **Work is done before publication.** Compilation happens entirely on the control thread; the
   artifact is complete — buffers allocated, pointers bound, pages touched — before it becomes
   visible, and the audio thread only swaps an index.
3. **The audio thread learns through one bounded poll.** A fixed-size POD command record on a
   lock-free SPSC ring, drained at the top of each block (§3.2 step 2). The audio thread never
   learns a path, never sees JSON, and never stats a file: it learns the identity of work that is
   already finished. That drain is the entire cost of "being aware".
4. **Retirement is deferred, never synchronous.** The audio thread does not free the old plan; it
   *retires* a pointer plus the current epoch, and the control thread frees it once `fade + 2` blocks
   have elapsed. That is what makes it safe to swap a plan while a block in flight is still reading
   the old one — no `delete` on the render path and no use-after-free.
5. **Commits land at a block boundary at a chosen sample**, with a degraded fallback if the warm
   deadline passes (§4). Worst case is a short crossfaded timbre change.
6. **Admission control is the backstop.** If a transition cannot fit inside the deadline budget, the
   edit is rejected with a reason and the current plan keeps rendering. The only path to silence is
   an explicit `engine.mute`/`panic`.
7. **And it is believed because it is measured.** The glitch detector (M2, §10.3) checks the
   crossfade region for discontinuity metrics — level, slope and second-difference bounds against a
   reference — rather than trusting that a fade was applied. "No click" is a measurement, not a
   promise.

## 7. What exists today, so none of the above is mistaken for running code

| stage | state |
|---|---|
| load | **implemented** — `load_graph_json` |
| parse | **implemented** — `parse_graph_json` plus full structural validation |
| compile | **implemented** for validation, Kahn ordering, liveness/colouring, arena allocation and pointer binding; lowering, PDC and the cost model still to come |
| apply | **partial** — `namchaind` builds the complete new plan on the control thread and publishes its identity on the SPSC ring; the render thread swaps it in at a block boundary and the old plan is freed by the control thread after `grace + 2` epochs. No dual-render, no equal-power crossfade, no admission control: a structural reload can click |
| control-side awareness | **partial** — `namchaind` is the daemon: a Unix `SOCK_SEQPACKET` socket with the versioned JSON protocol (`engine.hello/status/mute/panic`, `rig.load/get`), `signalfd` for SIGHUP reload and TERM/INT stop, startup-from-config, and a `poll` loop whose timeout is the reclamation window. The in-process API, the offline edit timeline and telemetry subscription are not built |
| audio-side awareness | **partial** — the daemon drains the SPSC command ring and swaps the plan pointer at the top of each block, so the mechanism of §6.3/§6.4 is live; the ring carries plan identity only, and the transition ops that make a structural change glitch-free are M2 |
| stall avoidance | mixed. `rt/arena.h` is in use today — the compiler allocates every intermediate buffer from one — and `rt/splice_fader.h` is in use in the live device loop, though for the output-queue splice rather than for plan transitions. `rt/spsc_ring.h` and `rt/reclaimer.h` are now wired into `namchaind`: the render thread drains the ring each block and retires the old plan, and the control loop's timeout is the reclamation window. What is still missing is the *content* of that ring — the transition ops of §4 |

`RenderPlan::execute(input, output, frames)` still has no plan-swap indirection of its own; the
indirection lives in `namchaind`, which swaps the whole slot pointer at a block boundary while the
soak continues to compile one plan and execute it for its whole run. Everything in §4–§6 not listed
as partial above is designed in plan §3.2, §3.3, §5.3 and §5.4, and unbuilt. That is why the glitch
detector is the M2 headline test rather than an afterthought: it is how this design gets proved
rather than asserted.
