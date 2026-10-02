# The rig spec pipeline: load, parse, compile, apply

**Status:** design, partly built. Load, parse and compile are implemented and tested. Apply is
partial. The `smooth` class (parameter-only) never recompiles and is ramped inside the node, over both
the daemon's `rig.patch` and the offline `namchain-render` timeline. A *structural* change — a
whole-rig load, the `add_node`/`remove_node`/`connect`/`disconnect`/`rewire`/`swap_model`/`bypass`
patch ops, or a non-rampable `set_param` — builds a new graph, compiles it, dual-renders it against
the old plan for `fade_ms` with an equal-power crossfade at the chain outputs, and retires the old
after a grace window. Bypass keeps the node running with its output replaced by its dry input, so
un-bypassing does not step on a frozen state. Every node's output is guarded for non-finite samples.
What is missing is the CPU cost model and admission control, and a tail-hold for a bypassed node with
a tail; the daemon's model budget (docs/daemon.md) slims or refuses a document for the memory its
models read per block, which is the one cost it checks. Control-side awareness is partial: the daemon's socket, protocol, `signalfd` reload and
device-loss survival exist, and the offline edit timeline now exists; telemetry subscription does not.
The audio-side command ring is drained at the top of each block and carries plan identity and
parameter dispatches. The last section says exactly which is which, so nothing here is mistaken for
running code.

Since docs/ui-integration-plan.md was agreed, the **document** layer has begun to land ahead of the
engine half: a rig spec may hold a name and several **chains**, each with a client-owned id, two
device channels and its own graph, and node identity is three separate things (`element`, `role`,
`id`) with the engine minting the id when a client does not want to write one. The parser and
`validate(Document)` are done and tested; per-chain plans, per-chain generations and channel binding
are slice A2, and the protocol that reads a document back is A3.

**Names.** Four stages, four words, because using one word for all of them is how a conversation
about latency ends up talking about the wrong thing:

| | what it is | where it lives |
|---|---|---|
| **rig spec** | the JSON document, on disk or on the wire | `rigs/*.json`, the `rig.load` request body |
| **`Document`** | the parsed envelope: a name and one or more `Chain`s, each with its channels and a `Graph` | `include/namchain/graph/rig.h` |
| **`Graph`** | the parsed, validated in-memory IR — one chain's nodes and connections | `include/namchain/graph/graph.h` |
| **`RenderPlan`** | the compiled, immutable execution artifact | `include/namchain/graph/plan.h` |

One wrinkle worth knowing: plan §5.1 reserves "Rig" for a superset — a set of chains plus scenes —
and docs/ui-integration-plan.md is building its first half. What exists today is the **document**:
the parser understands a name and N chains, each with its own id, channels and graph, and
`validate(Document)` checks the document-level rules and names the chain in any failure. What does
*not* exist yet is the engine half: the compiler still takes one `Graph`, the daemon still holds one
plan slot, and a chain's `input_channel`/`output_channel` are parsed and validated but not yet bound
to device channels (that is slice A2 of the plan). Calling the document a **rig spec** sidesteps the
collision between the two meanings of "Rig" while that lands.

---

## 1. Load a rig spec

Control thread, ordinary file I/O: read the bytes, hand them to the parser. Nothing clever, with one
subtlety that is already implemented — a string parameter that looks like a path (a `model` path)
resolves against the **rig spec's own directory** first, so an example rig copied next to its models
needs no editing. The committed `rigs/*.json` are copied into the generated-fixture directory in CI
for exactly this reason.

`load_document_json(path, Document&, error)` in `include/namchain/graph/rig.h`, and
`load_graph_json(path, Graph&, error)` for the single-chain callers that are most of them.

## 2. Parse a rig spec

A rig spec is a **document**: a name, and one or more **chains**, each chain a graph plus the two
device channels it reads and writes and an id the client owns.

```json
{
  "name": "Guitar + Vocal",
  "chains": [
    { "id": "gtr", "name": "Guitar", "input_channel": 0, "output_channel": 0,
      "nodes": [ … ], "connections": [ … ] }
  ]
}
```

A document that is a **bare graph** — top-level `nodes`/`connections` and no `chains` — is one chain
with id `"main"` on channels 0 and 0. That is a compatibility rule rather than a special case: every
rig committed before chains existed is in that shape, and `parse_graph_json` (which most callers,
including the offline tools and the patch ops, still use) accepts either shape and insists on exactly
one chain.

Text to `Graph`: nodes `{id, type, params}` and connections `{from, from_port, to, to_port}`, where
`id`, `type`, `bypass`, `element` and `role` are reserved and every other key is a parameter — a
number or boolean becomes a numeric param, a string becomes a string param, so `"model": "amp.nam"`
needs no wrapper. Explicit `"params"`/`"strings"` objects are also accepted.

**Identity** (docs/ui-integration-plan.md §1.2) is deliberately three separate things, and none of
them is composed out of another:

| | owner | what it is |
|---|---|---|
| `element` | the client | the document thing this node belongs to — a block, a lane, a parallel section. Opaque, stored and returned verbatim, never parsed, and **not** required to be unique: several nodes may share one element, which is the normal case once a macro lowers into primitives. Defaults to the node's `id` |
| `role` | the engine, declared per node type | which node of that element this is (`"line"`, `"ts.bass"`). Empty for a node that is its own element |
| `id` | the engine | the node's name inside the graph. **May be omitted** when the node has an `element`: the engine mints `"<element>"` or `"<element>/<role>"`, disambiguating with `~2`, `~3`. No client addresses it, so nothing depends on its spelling, and no character has to be forbidden in a client's own ids |

The rule of thumb for minting: it is for a node the client does not have to **name**. A connection
names nodes, so a client that writes the connections writes the ids; a client that addresses nodes by
`element` and `role` — which is what a macro user does, and what the engine itself does when it wires
up a lowering — never needs to know an engine node id at all. A node with neither an `id` nor an
`element` is an error, because there would be nothing to call it. A client's `element` may be a UUID
with punctuation in it; nothing looks inside it.

A structured value (an array or object) is **rejected rather than dropped**, because silently
ignoring a nested object would compile and run while discarding what the rig author meant. So is a
document that carries both `chains` and a top-level `nodes`. Relative paths resolve as described
above, per chain.

Then structural validation: unknown node type, duplicate id, dangling connection, port out of range,
two connections into one input port, cycles (rejected via Kahn's algorithm), and exactly one input
and one output node — **per chain**, with the chain named in any failure. Errors are structured
strings — nothing throws across the boundary, which is what lets a failed load be reported to a
client instead of killing the daemon.

`parse_document_json`/`validate(const Document&)` and the validation pass in `src/graph/plan.cpp`,
with `tests/test_document.cpp` covering the envelope and the identity rules.

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

1. **A protocol request — the primary path.** A client sends `rig.load` on a control socket, and
   that read *is* the awareness. On the Unix `SOCK_SEQPACKET` socket the control thread `recv()`s one
   whole message (SEQPACKET preserves message boundaries, so there is no framing to reimplement); on
   the TCP one it reassembles a length-prefixed body from the connection's own buffer (docs/daemon.md
   §"Two transports"). The file is read **after** the request, on this thread, because this is
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
| load | **implemented** — `load_document_json`, `load_graph_json` for single-chain callers, and `rig.apply` for a document that arrives over the socket rather than from a file. Relative paths resolve against the rig file's directory, or the rigs root for an applied document |
| parse | **implemented** — `parse_document_json` (name plus N chains, ids and device channels) and `parse_graph_json`; full structural validation per chain, with the chain named in the failure, plus the document-level rules in `validate(const Document&)` |
| identity | **implemented** — `element` (client-owned, opaque, may repeat), `role` (declared per node type) and `id` (engine-minted when omitted); a bare graph is still one chain `"main"` whose nodes are their own elements |
| compile | **implemented** for validation, Kahn ordering, liveness/colouring, arena allocation and pointer binding, and per chain: a document compiles to one plan per chain, all-or-nothing, with the chains' device channels checked here; lowering, PDC and the cost model still to come |
| apply | **partial** — the `smooth` class is built: a parameter-only change resolves to a vtable dispatch (`NodeType::set_param`) and ramps inside the node, no compile and no step, over both the daemon's `rig.patch` and the offline timeline. A structural change is built from a graph mutation (`graph::apply_patch`: `add_node`/`remove_node`/`connect`/`disconnect`/`rewire`/`swap_model`/`bypass`/non-rampable `set_param`), applied all-or-nothing to a copy, compiled, dual-rendered for `fade_ms` and equal-power crossfaded (`rt::Crossfade`) before the old plan is retired. `rig.patch` routes smooth vs structural and reports which it took. Bypass keeps the node running with its dry input substituted, so its state stays warm. Every node's output is guarded for non-finite samples and a failed node is taken out of the path and reported. Missing: admission control (the fade is fixed and an expensive dual-render is not rejected) and a tail-hold for a bypassed node with a tail |
| control-side awareness | **partial** — `namchaind` is the daemon: a Unix `SOCK_SEQPACKET` socket (and an opt-in, token-authenticated TCP listener) with the versioned JSON protocol (2, with `features`; `engine.hello/status/mute/panic`, `rig.load/apply/get/patch`), `signalfd` for SIGHUP reload and TERM/INT stop, startup-from-config, and a `poll` loop whose timeout is the reclamation window. A document can be applied whole and read back (`rig.apply`/`rig.get`, the latter a diagnostic rather than a store). The offline edit timeline exists (`namchain-render --graph --timeline/--torture`); the in-process API and telemetry subscription are not built |
| audio-side awareness | **partial** — the daemon drains the SPSC command ring at the top of each block and carries two kinds of record: plan identity (start a crossfade, or adopt the first plan) and parameter dispatches (ramp a node). The deferred-commit scheduler is M3 |
| stall avoidance | mixed. `rt/arena.h` is in use today — the compiler allocates every intermediate buffer from one — and `rt/splice_fader.h` is in use in the live device loop, though for the output-queue splice rather than for plan transitions. `rt/spsc_ring.h` and `rt/reclaimer.h` are wired into `namchaind`: the render thread drains the ring each block and retires the old plan, and the control loop's timeout is the reclamation window. The ring now carries a `set_param` dispatch as well as plan identity; the structural ops of §4 are still missing |

`RenderPlan::execute(input, output, frames)` still has no plan-swap indirection of its own; the
indirection lives in `namchaind`, which swaps the whole slot pointer at a block boundary while the
soak continues to compile one plan and execute it for its whole run. Everything in §4–§6 not listed
as partial above is designed in plan §3.2, §3.3, §5.3 and §5.4, and unbuilt. That is why the glitch
detector is the M2 headline test rather than an afterthought: it is how this design gets proved
rather than asserted.
