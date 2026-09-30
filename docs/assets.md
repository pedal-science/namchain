# The asset library: models and impulse responses

Slice D of `docs/ui-integration-plan.md`. Until this slice a `nc.nam` node named a **file**: "which
amps has this device got" was a question only the filesystem could answer, and an app had to be told
the layout out of band. A model is now something the engine can **list**, and a document can name it
by **id** — which is what makes a portable rig, a palette, and a network client possible at all
(decision 5: the amp and cab palette comes from the device's own library).

## Two roots

| config | default | what it is |
|---|---|---|
| `models_root` | `/etc/namchain/models` | every `.nam` under it is a **model** |
| `irs_root` | `/etc/namchain/irs` | every `.nam` or `.wav` under it is an **impulse response** |

The **root decides the kind**, not the file: a `.wav` in the models root is ignored rather than
guessed at, and a `.nam` in the IR root is an IR. That is the operator's filing doing the labelling —
there is no second place to declare what a file is.

Scanning is recursive, and a file or directory whose name begins with `.` is skipped. A symlink is
followed only while it stays inside the root, for the same reason `rig.load` refuses one that escapes
`rigs_root`: an id promises "a file in this library", and a link out of it would make that a promise
the registry cannot keep.

A root that is missing, unreadable, or a file rather than a directory is **not** a startup failure.
It is an empty library, `engine.assets` says which of the three it is, and `engine.status` carries an
informational issue (`models_root_missing` / `irs_root_missing`) so that "no amps installed" and "the
amps directory moved" do not look the same from a client. Informational means it does not colour
`health`.

## Ids

An **id** is the path under the root with the extension removed, `/`-separated:

| file | id | name | group |
|---|---|---|---|
| `models/tweed/deluxe.nam` | `tweed/deluxe` | `deluxe` | `tweed` |
| `models/clean.nam` | `clean` | `clean` | *(empty)* |
| `irs/cab-b.wav` | `cab-b` | `cab-b` | *(empty)* |

A reference is resolved in this order:

1. **A file relative to the document**, exactly as before this slice existed. A rig that names the
   model sitting next to it keeps meaning that model.
2. **The library**: an exact relative filename (`tweed/deluxe.nam`, which is what a hand-written rig
   says), then an id (`tweed/deluxe`).
3. **An absolute path**, as written.

> **A departure from the plan, recorded.** Slice D says "by registry id first, then as a path". This is
> deliberately the other way round. The library going first would let a library entry shadow the file
> a rig was written against — silently, because both resolve to *a* model — and it would make the
> daemon and the offline tools (`namchain-render`, `namchain-soak`, which have no registry) resolve
> the same rig differently. The two orders agree in every case where they can both answer, because an
> id is normally extension-less and a filename is not.

A reference that resolves nowhere does **not** refuse the rig. The block that names it plays **as if
bypassed**: on the copy the engine compiles, the node is replaced by an `nc.bypass` with the same id,
element and role, so connections and ops still name it, and `rig.get` still hands back what was
written. The rig plays, just not as written, and every client is told which block: an `asset_missing`
issue per block (severity `warn`, with its `chain`, `node`, `element`, `param`, `kind` and
`reference`), `missing_assets` in each chain of `engine.status`, and a `missing_assets` list in the
reply to `rig.load`, `rig.apply`, `rig.patch` and `rig.get`. A client marks the block as not
configured (ljoss does).

Why not refuse: a pedal whose library is incomplete -- a fresh card before the library is synced, a
model deleted from the library -- would otherwise start silent with nothing playing, and the pedal's
boot guard would take the failed engine for a broken system (pedal.project, vip's first card,
2026-09-30).

When the library changes, a rig with stand-ins is re-resolved: `engine.assets` with `refresh` (what
the pedal's `sync-library` sends) and SIGHUP both republish it, so a model copied in afterwards
replaces its stand-in without anyone touching the rig. Whether a chain changed is decided on the
resolved document, so this is a new generation for exactly the chains it affects.

Only a 1->1 block can stand in (it needs a dry path). A reference on any other node still refuses the
document as **`asset_missing`**. The message is the same in both cases, naming the library that was
searched, how many entries it has, and the nearest ids:

```
no model 'tweed/delux' in '/etc/namchain/models' (12 models in the library);
did you mean 'tweed/deluxe'; a reference is a library id, a path relative to the rig, or an absolute path
```

Near misses are ranked by edit distance on the id and on its last component, with containment
(`deluxe` finds `tweed/deluxe-reverb`) counted as close. It is a hint, not a resolution.

## `engine.assets`

```json
{
  "ok": true,
  "models_root": "/etc/namchain/models",
  "irs_root": "/etc/namchain/irs",
  "roots": {
    "models": { "path": "…", "readable": true, "error": null, "count": 12 },
    "irs":    { "path": "…", "readable": false, "error": "no such directory", "count": 0 }
  },
  "scan": 3,
  "files_parsed": 0,
  "models": [
    { "id": "tweed/deluxe", "relative": "tweed/deluxe.nam",
      "path": "/etc/namchain/models/tweed/deluxe.nam",
      "name": "deluxe", "group": "tweed", "kind": "model",
      "size": 42841, "mtime": 1757800000,
      "sample_rate": 48000.0, "frames": null, "architecture": "WaveNet" }
  ],
  "irs": [ … ]
}
```

- **It is a scan, not a directory listing.** Two answers are comparable: a difference between them is
  a difference on disk, because the daemon does not re-read the directory between calls.
- **`{"refresh": true}`** rescans first. That is the only way to see a file added while the daemon has
  been running, and `SIGHUP` does it too (a reload is when an operator expects a newly copied model to
  appear). `namchainctl assets --refresh` is the same thing from the shell.
- **A refresh is cheap when nothing changed.** The cache key is size and modification time — at
  nanosecond resolution where the filesystem has it, because a model saved twice in the same second
  with the same size is exactly the case whole seconds get wrong. `files_parsed` is the number of
  files whose headers were actually read by the last scan, so `0` is a refresh that read nothing, and
  it is the number the tests assert rather than a claim in a comment.
- **`sample_rate` and `frames` are `null` when the file did not say.** Not `0`, which would be a
  number a palette would render as a lie.
- **`path` is informational.** It is what an operator on the same machine wants, and it is what a
  *remote* client must never put in a document: a document applied over TCP may not name an absolute
  path (`path_refused`, docs/daemon.md). The value to send is the **id**.

## What is read, and what is not

The registry never loads a model. Loading is what compiling does, and doing it for every file in a
library would make a refresh cost seconds.

- **`.nam`** — `architecture` and `sample_rate`, found by looking for two keys in the front 16 KiB and
  the last 1 KiB of the file. That is where the reference writer puts them (`architecture` near the
  front, `sample_rate` after the weights, which can be hundreds of kilobytes further on). It is a
  **sniff, not a parse**: a file that puts those fields elsewhere reports `null` for them, which is a
  palette with no rate shown rather than a wrong rate.
- **`.wav`** — `fmt ` and `data`, read from the header. This one is exact: rate, channel count, and
  the length in frames. Reading is clamped to the first 4 KiB, so a WAV whose `data` chunk header is
  buried behind megabytes of metadata reports no frame count.

Nothing functional depends on any of it. The load is still what decides whether a model is usable,
at what rate, and whether an IR matches the engine's rate.

## In the schema, and on the wire

- **`engine.schema`** resolves `options_from: "models"` / `"irs"` through the same scan, so a palette
  comes from one call: each such parameter carries `options: [{ "value": <id>, "label": <id> }]`.
  `options_from` stays in the answer, because it is why those options are there and what to fetch
  again after a refresh. The label is the id: prettifying `tweed/deluxe` into "Deluxe" is presentation,
  and presentation is the app's job (`docs/ui-integration-plan.md` §1.9).
- **An empty library is an empty list**, which a client shows as "no models installed" — a true answer,
  and a different one from an error.
- **`engine.status.assets`** carries the roots and the two counts, so a client can tell "no models
  installed" from "not asked yet" without pulling the whole listing.
- **`features`** gained `assets`.

## What is not here

- **No write path.** Nothing in the protocol adds, removes, renames or uploads a model. The library is
  what the operator put on the device, and the engine reads it.
- **No presets, tags, favourites or folders beyond the directory tree.** Presentation metadata is the
  app's (the same §1.9 rule), and a device-side preset store is a different feature.
- **No NAM warm start.** The registry is the *listing* that feature will need, not the feature
  (M3 proper, out of scope for this plan).
- **No `model.load` / `model.unload` / `model.list` methods.** `engine.assets` is the listing;
  loading happens when a document that names a model is compiled, and unloading when no plan
  references it any more.
- **No automatic rescan.** The library is scanned at startup and rescanned when a client asks or on
  `SIGHUP`. Nothing watches the directories and nothing scans on a timer: a library that changes
  underneath a running plan is something an operator should be able to do deliberately.
