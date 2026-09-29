"""Protocol-2 plumbing and the session file format, shared by namchain-session-record and
namchain-session-replay. Standard library only, so both run on the Mac and on the device.

The wire is docs/daemon.md, "The wire, byte for byte": a u32 little-endian length, then one
JSON object. A request may be up to 1 MiB and a reply up to 16 MiB.

A session file is JSON lines (docs/sessions.md):
  line 1   {"namchain_session": 1, "name", "source", "initial": {"document", "muted"}, ...}
  then     {"t": seconds, "request": {...}}          what a client sent, without id or token
  and      {"t": seconds, "reply_to": method, ...}   a summary of the answer (recordings only)
"""
import asyncio
import json
import os
import shlex
import struct
import subprocess
import sys

FORMAT = 1
MAX_REQUEST = 1 << 20
MAX_REPLY = 16 << 20

# Asked often and change nothing: a replay can leave them out (--writes-only).
QUERIES = {"engine.status", "engine.schema", "engine.assets", "rig.get", "engine.record"}


def frame(obj):
    body = json.dumps(obj, separators=(",", ":")).encode()
    return struct.pack("<I", len(body)) + body


async def read_frame(reader, limit=MAX_REPLY):
    """One message's body, or None at a clean end of stream."""
    try:
        header = await reader.readexactly(4)
    except asyncio.IncompleteReadError as e:
        if e.partial:
            raise
        return None
    (n,) = struct.unpack("<I", header)
    if n > limit:
        raise ValueError("a message of %d bytes is over the %d-byte limit" % (n, limit))
    return await reader.readexactly(n)


def read_token(token_file=None, token_command=None):
    """The token from a file, a command's first line of output, or $NAMCHAIN_TOKEN. Never printed."""
    if token_file:
        with open(os.path.expanduser(token_file)) as f:
            token = f.readline().strip()
    elif token_command:
        out = subprocess.run(token_command, shell=True, capture_output=True, text=True)
        if out.returncode != 0:
            sys.exit("the token command failed (exit %d): %s"
                     % (out.returncode, shlex.quote(token_command)))
        token = out.stdout.splitlines()[0].strip() if out.stdout else ""
    else:
        token = os.environ.get("NAMCHAIN_TOKEN", "").strip()
    if not token:
        sys.exit("no token: give --token-file, --token-command or NAMCHAIN_TOKEN")
    return token


class Client:
    """One TCP connection. `send` does not wait; `call` does. Replies are matched by id."""

    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer
        self.waiting = {}
        self.next_id = 0
        self.on_reply = None
        self.task = asyncio.create_task(self._read())

    @classmethod
    async def connect(cls, host, port, token):
        reader, writer = await asyncio.open_connection(host, port, limit=MAX_REPLY)
        client = cls(reader, writer)
        hello = await client.call({"method": "engine.hello", "protocol": 2, "token": token})
        if not hello.get("ok"):
            raise ConnectionError("engine.hello refused: %s" % describe_error(hello))
        return client, hello

    async def _read(self):
        try:
            while True:
                body = await read_frame(self.reader)
                if body is None:
                    break
                reply = json.loads(body)
                future = self.waiting.pop(str(reply.get("id", "")), None)
                if future and not future.done():
                    future.set_result(reply)
        except (asyncio.IncompleteReadError, ConnectionError, ValueError) as e:
            error = e
        else:
            error = ConnectionError("the daemon closed the connection")
        for future in self.waiting.values():
            if not future.done():
                future.set_exception(error)
        self.waiting.clear()

    def send(self, request):
        """Send now; returns a future for the reply."""
        self.next_id += 1
        rid = str(self.next_id)
        future = asyncio.get_running_loop().create_future()
        self.waiting[rid] = future
        self.writer.write(frame(dict(request, id=rid)))
        return future

    async def call(self, request, timeout=30):
        future = self.send(request)
        await self.writer.drain()
        return await asyncio.wait_for(future, timeout)

    async def close(self):
        self.task.cancel()
        self.writer.close()
        try:
            await self.writer.wait_closed()
        except (ConnectionError, OSError):
            pass


def describe_error(reply):
    error = reply.get("error") or {}
    return "%s: %s" % (error.get("code", "?"), error.get("message", ""))


def split_host(text, default_port):
    host, _, port = text.rpartition(":")
    if not host:
        return text, default_port
    return host, int(port)


# ------------------------------------------------------------------------------ session files

def load(path):
    with open(path) as f:
        lines = [json.loads(line) for line in f if line.strip()]
    if not lines or lines[0].get("namchain_session") != FORMAT:
        sys.exit("%s is not a namchain session file (format %d)" % (path, FORMAT))
    return lines[0], lines[1:]


def assets_named(document):
    """Every model and IR id a document names, as (kind, id)."""
    found = set()
    for chain in document.get("chains", []):
        for node in chain.get("nodes", []):
            for key, kind in (("model", "model"), ("ir", "ir")):
                value = node.get(key, node.get("params", {}).get(key))
                if isinstance(value, str) and value:
                    found.add((kind, value))
    return found


def assets_in_session(header, events):
    found = assets_named(header["initial"]["document"])
    for event in events:
        request = event.get("request") or {}
        if request.get("method") == "rig.apply":
            found |= assets_named(request.get("document", {}))
        for op in request.get("ops", []):
            if op.get("param") in ("model", "ir") and isinstance(op.get("value"), str):
                found.add((op["param"], op["value"]))
            if op.get("op") == "swap_model" and isinstance(op.get("model"), str):
                found.add(("model", op["model"]))
    return found
