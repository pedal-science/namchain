# Sessions: recording a client, and replaying it

A **session** is what a client sent `namchaind`, with the time it sent each request, and the rig
the daemon was running when it began. Replaying one sends the same requests at the same offsets,
so a test that used to need someone editing live in ljoss can run unattended, the same way each
time, against any daemon: a new commit, another kernel, another host.

Two tools in `tools/session/`, Python 3 with the standard library only, so they run on the Mac and
on the device:

| Tool | Does |
|---|---|
| `namchain-session-record` | A proxy. Point the client at it instead of the daemon, and it writes every request to a session file as it passes them on unchanged |
| `namchain-session-replay` | Restores a session's starting rig, then sends its requests on the recorded timing. Loops, speeds up, and summarises the answers |

## Recording

```console
$ tools/session/namchain-session-record --upstream piv:47810 --out edits.session.jsonl
listening on 127.0.0.1:47811, passing on to piv:47810; Ctrl-C ends the recording
```

Then point ljoss at `127.0.0.1:47811` with the device's usual token, edit, and press Ctrl-C.

- When the client has authenticated, and before its first request goes through, the recorder reads
  the daemon's rig (`rig.get`) and each chain's mute (`engine.status`) on a connection of its own.
  That is the session's starting point.
- It authenticates with the token the client sent, which it holds in memory only. **The token is
  never written.** `engine.hello` isn't recorded at all, since the replay does its own.
- Replies pass back unchanged. The file keeps only a summary of each: `ok`, `kind`, `generation`,
  the error code, and how long it took.
- The file is written a line at a time, so a recording that is interrupted is still a session.
- A client that reconnects is recorded on the same timeline, with its connection number (`conn`).
  A replay sends everything on one connection.

## Replaying

```console
$ tools/session/namchain-session-replay edits.session.jsonl --host piv \
    --token-command "ssh piv sudo -n cat /etc/namchain/token" --duration 3600
```

1. It connects and checks that every model and IR the session names is in the daemon's library
   (`engine.assets`), and stops before sending anything if one isn't (`--no-asset-check` skips it).
2. It restores the starting rig with `rig.apply` and each chain's `engine.mute`, then waits
   `--settle` seconds (2 by default).
3. It sends each request at its offset, divided by `--speed`, and doesn't wait for an answer before
   the next one is due, as a client streaming a knob doesn't. Answers are matched by `id`.
4. With `--loops N` or `--duration S` it goes round again, restoring the starting rig each time.
   That restore is a structural transition, like switching back to a rig.

`--writes-only` leaves out requests that only ask: `engine.status`, `engine.schema`,
`engine.assets`, `rig.get` and `engine.record`. The token comes from `--token-file`,
`--token-command` (the first line of its output) or `NAMCHAIN_TOKEN`, and is never printed.

The summary, as JSON on stdout or in `--summary FILE`, has:
- requests sent, by method;
- answers by `kind` (`smooth`, `structural`) and refusals by method and error code;
- how long each method took to answer (p50, p99, max);
- how late the replay itself sent (`send_lag_ms`), which says whether the replay kept to time;
- the most requests in flight at once, and the first and last generation.

`--log FILE` writes every request and its answer as JSON lines. The exit status is 0 when every
answer was ok, 1 when any was refused or unanswered, and 2 when it couldn't run.

## The file

JSON lines. The first is the header; the rest are in time order.

```jsonc
{"namchain_session": 1, "name": "edits", "source": "recorded",      // or "rebuilt"
 "recorded_at": "2026-09-29T18:36:40+00:00", "upstream": "piv:47810",
 "daemon": {"version": "0.1.0", "protocol": 2}, "notes": ["..."],
 "initial": {"document": {...}, "muted": {"<chain id>": false}, "generation": 6}}
{"t": 3.368, "conn": 1, "request": {"method": "rig.apply", "document": {...}}}
{"t": 3.401, "conn": 1, "reply_to": "rig.apply", "ok": true, "ms": 32.4, "generation": 7}
{"t": 19.209, "conn": 1, "request": {"method": "rig.patch", "chain": "...", "ops": [...]}}
```

`t` is seconds from the start. A request is exactly what the client sent, less its `id` and any
`token`. Reply lines are for reading; a replay ignores them. A session built some other way than
recording says so in `source` and explains itself in `notes`: pedal.project's
`scripts/soak/sessions/` has one rebuilt from a watch log.
