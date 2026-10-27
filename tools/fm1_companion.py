#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Henri David
"""Talk to any fm1-base firmware built with companion = true (fm1.toml; device/companion.c, docs/COMPANION.md) over
USB-MIDI: what it is and has, the app's files and parameters.

    python3 tools/fm1_companion.py info [--port NAME]
    python3 tools/fm1_companion.py ls
    python3 tools/fm1_companion.py get NAME [FILE]       (FILE default: NAME)
    python3 tools/fm1_companion.py put FILE [NAME]       (NAME default: FILE's name)
    python3 tools/fm1_companion.py rm NAME
    python3 tools/fm1_companion.py params                  (every parameter: its group, name, value, range)
    python3 tools/fm1_companion.py get-param ID | set-param ID VALUE
    python3 tools/fm1_companion.py info --serve-cmd "tests/companion_app/build/host/cptest_host --serve"

--serve-cmd runs a host simulator (NAME_host --serve) instead of an FM-1: the same requests, the same answers. A file
read is checked against the FM-1's CRC-32 of it; a file written counts only once the FM-1 has checked it whole, and a
put cut short (a cable pulled) goes on where it stopped when run again while the FM-1 stayed on.

As a library: Client(link) with hello(), files(), read(i), write(i, data), delete(i); ServeLink(cmd) for a simulator,
find(backend, port) for an FM-1. Needs mido with python-rtmidi for an FM-1 (as tools/fm1_install.py). Exit codes:
0 done, 1 failed, 2 bad arguments, 3 FM-1 not found, 4 no answer (no companion services, or the connection lost),
5 busy (the app plays: stop it first), 6 no such file.
"""
import argparse
import importlib
import os
import shlex
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
INST = importlib.import_module("fm1_install")   # (tools/: the update's wire format, the port finder)

HELLO = 0x60
SERVICES = ["capability query", "files", "parameters", "backup", "mirror", "screen reader", "Bluetooth MIDI", "WiFi",
            "USB audio in", "USB audio out", "the app's own SysEx", "the app's own commands", "the second core",
            "USB MIDI host", "Bluetooth audio"]
FILES = 0x61
PARAMS = 0x62
PQ_INFO, PQ_LABELS, PQ_GROUP, PQ_GET, PQ_SET, PQ_WATCH, PQ_CHANGED = range(7)
EXIT = {"usage": 2, "notfound": 3, "silent": 4, "busy": 5, "nofile": 6}
STATUS = ["ok", "busy (the app plays: stop it first)", "no such file", "not allowed", "out of range",
          "CRC-32 mismatch", "the flash refused", "out of order", "a malformed request", "an unknown operation"]
KINDS = ["other", "project", "preset", "sample", "bank", "pattern", "settings", "text", "log"]
F_READ, F_WRITE, F_DELETE, F_PLAYING = 1, 2, 4, 8
OP_LIST, OP_READ, OP_SUM, OP_BEGIN, OP_DATA, OP_COMMIT, OP_DELETE = range(7)
ST_OK, ST_BUSY, ST_NONE, ST_ORDER = 0, 1, 2, 7


class CompanionError(Exception):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code


def decode(pkt):
    """an answer frame -> (cmd, body) or None (not the update's framing, or damaged)"""
    if len(pkt) < 3 or pkt[0] != 0xF0 or pkt[-1] != 0xF7:
        return None
    u = INST.unpack7(pkt[1:-1])
    if len(u) < 7 or u[0] != 0 or u[1] != 0x59:
        return None
    n = u[3] | u[4] << 8 | u[5] << 16
    if len(u) < n + 7 or ~sum(u[6:6 + n]) & 0xFF != u[6 + n]:
        return None
    return u[2], bytes(u[6:6 + n])


def cstrings(b, k):
    """k NUL-terminated strings from b -> ([str], the rest)"""
    out = []
    for _ in range(k):
        z = b.find(b"\0")
        z = len(b) if z < 0 else z
        out.append(b[:z].decode("utf-8", "replace"))
        b = b[z + 1:]
    return out, b


class Client:
    """the companion's requests over a link (send(pkt), read(timeout) -> pkt or None, drain())"""

    def __init__(self, link, timeout=1.0, tries=3):
        self.link, self.timeout, self.tries = link, timeout, tries
        self.notes = []                               # parameter changes the device sent meanwhile (0x62 op 6)

    def request(self, cmd, body=b"", match=None):
        """send one request, -> the body of its answer (the same cmd; match(body) picks among several)"""
        for _ in range(self.tries):
            if not self.link.send(INST.frame(cmd, bytes(body))):
                raise CompanionError("silent", "the connection is lost")
            end = time.monotonic() + self.timeout
            while (left := end - time.monotonic()) > 0:
                p = self.link.read(left)
                if p is None:
                    break
                a = decode(p)
                if a and a[0] == PARAMS and a[1][:1] == bytes([PQ_CHANGED]):
                    self.note(a[1])
                    continue
                if not a or a[0] != cmd:
                    continue
                if match is None or match(a[1]):
                    return a[1]
        raise CompanionError("silent", f"no answer to 0x{cmd:02X}: no companion services, or it stopped answering")

    def note(self, body):
        """a parameter change the device sent (0x62 op 6): kept in notes as (id, value)"""
        n = body[2] if len(body) > 2 else 0
        for k in range(n):
            self.notes.append(struct.unpack_from("<Hi", body, 3 + 6 * k))

    def poll(self, seconds=0.0):
        """what came meanwhile (parameter changes) -> notes so far, cleared"""
        end = time.monotonic() + seconds
        while True:
            p = self.link.read(max(end - time.monotonic(), 0))
            if p is None:
                break
            a = decode(p)
            if a and a[0] == PARAMS and a[1][:1] == bytes([PQ_CHANGED]):
                self.note(a[1])
        out, self.notes = self.notes, []
        return out

    def hello(self):
        b = self.request(HELLO)
        ver, svc, rmax, wmax, nf, np_ = struct.unpack_from("<BIHHHH", b)
        (ident, name, version, extras), rest = cstrings(b[13:], 4)
        backup = None                                 # protocol 2 (an answer's end an old reader skips): the backup's format
        if ver >= 2 and len(rest) >= 7:
            magic, bver = struct.unpack_from("<4sH", rest)
            if magic != bytes(4):
                backup = {"magic": magic.decode("latin-1"), "version": bver, "ext": cstrings(rest[6:], 1)[0][0]}
        return {"backup": backup, "version": ver, "services": [s for i, s in enumerate(SERVICES) if svc >> i & 1], "bits": svc,
                "read_max": rmax, "write_max": wmax, "files": nf, "params": np_, "identity": ident, "name": name,
                "app_version": version,
                "extras": dict(kv.split("=", 1) if "=" in kv else (kv, "") for kv in extras.split(";") if kv)}

    # ------------------------------------------------------------ files ---
    def file_op(self, op, i, body=b""):
        """-> (status, the answer after op, status, index)"""
        b = self.request(FILES, struct.pack("<BH", op, i) + bytes(body),
                         match=lambda a: len(a) >= 4 and a[0] == op and struct.unpack_from("<H", a, 2)[0] == i)
        return b[1], b[4:]

    def fail(self, st, what):
        code = "busy" if st == ST_BUSY else "nofile" if st == ST_NONE else "failed"
        raise CompanionError(code, f"{what}: {STATUS[st] if st < len(STATUS) else st}")

    def files(self):
        """every file: [{index, name, kind, flags, size, room}]"""
        out, i, count = [], 0, 1
        while i < count:
            st, b = self.file_op(OP_LIST, i)
            count = struct.unpack_from("<H", b)[0]
            if st != ST_OK:
                break
            kind, flags, size, room = struct.unpack_from("<BBII", b, 2)
            (name,), _ = cstrings(b[12:], 1)
            out.append({"index": i, "name": name, "kind": KINDS[kind] if kind < len(KINDS) else str(kind),
                        "flags": flags, "size": size, "room": room})
            i += 1
        return out

    def index_of(self, name):
        for f in self.files():
            if f["name"] == name:
                return f
        raise CompanionError("nofile", f"no file {name!r} on the FM-1")

    def sum(self, i):
        st, b = self.file_op(OP_SUM, i)
        if st != ST_OK:
            self.fail(st, "sum")
        return struct.unpack_from("<II", b)

    def read(self, i, progress=None):
        """the whole file, checked against the device's CRC-32 of it"""
        size, crc = self.sum(i)
        data = bytearray()
        while len(data) < size:
            st, b = self.file_op(OP_READ, i, struct.pack("<IH", len(data), 256))
            if st != ST_OK:
                self.fail(st, f"read at {len(data)}")
            off = struct.unpack_from("<I", b)[0]
            if off == len(data):
                data += b[4:]
            if len(b) <= 4:
                raise CompanionError("failed", f"read at {len(data)}: nothing came")
            if progress:
                progress(len(data), size)
        if zlib.crc32(bytes(data)) != crc:
            raise CompanionError("failed", "read: the CRC-32 differs from the FM-1's (the file changed meanwhile?)")
        return bytes(data)

    def write(self, i, data, chunk=512, progress=None, stop_after=None):
        """the whole file: begin (resumes a write cut short), the data in order, commit. stop_after: give up after
        that many bytes (a test's cut cable)"""
        data = bytes(data)
        crc = zlib.crc32(data)
        st, b = self.file_op(OP_BEGIN, i, struct.pack("<II", len(data), crc))
        if st != ST_OK:
            self.fail(st, "begin")
        nxt = struct.unpack_from("<I", b)[0]
        resumed = nxt
        while nxt < len(data):
            if stop_after is not None and nxt >= stop_after:
                return resumed
            st, b = self.file_op(OP_DATA, i, struct.pack("<I", nxt) + data[nxt:nxt + chunk])
            if st not in (ST_OK, ST_ORDER) or len(b) < 4:
                self.fail(st, f"data at {nxt}")
            nxt = struct.unpack_from("<I", b)[0]
            if progress:
                progress(nxt, len(data))
        st, b = self.file_op(OP_COMMIT, i)
        if st != ST_OK:
            self.fail(st, "commit")
        return resumed

    def delete(self, i):
        st, _ = self.file_op(OP_DELETE, i)
        if st != ST_OK:
            self.fail(st, "delete")

    # ------------------------------------------------------- parameters ---
    def param_op(self, op, body=b""):
        """-> (status, the answer after op, status)"""
        b = self.request(PARAMS, bytes([op]) + bytes(body), match=lambda a: len(a) >= 2 and a[0] == op)
        return b[1], b[2:]

    def groups(self):
        out, g, count = [], 0, 1
        while g < count:
            st, b = self.param_op(PQ_GROUP, bytes([g]))
            count = b[1]
            if st != ST_OK:
                break
            out.append(cstrings(b[2:], 1)[0][0])
            g += 1
        return out

    def labels(self, i, total):
        out = []
        while len(out) < total:
            st, b = self.param_op(PQ_LABELS, struct.pack("<HH", i, len(out)))
            if st != ST_OK or not b[4]:
                raise CompanionError("failed", f"labels of parameter {i}: {STATUS[st] if st < len(STATUS) else st}")
            out += cstrings(b[5:], b[4])[0]
        return out

    def params(self):
        """every parameter: [{index, id, name, type ("number" / "enum"), group (its name), flags, min, max, default,
        unit, labels}]"""
        groups, out, i, count = self.groups(), [], 0, 1
        while i < count:
            st, b = self.param_op(PQ_INFO, struct.pack("<H", i))
            count = struct.unpack_from("<H", b, 2)[0]
            if st != ST_OK:
                break
            pid, typ, grp, flags, mn, mx, df, nl = struct.unpack_from("<HBBBiiiH", b, 4)
            (name, unit), _ = cstrings(b[23:], 2)
            out.append({"index": i, "id": pid, "name": name, "type": "enum" if typ == 1 else "number",
                        "group": groups[grp] if grp < len(groups) else str(grp), "flags": flags, "min": mn,
                        "max": mx, "default": df, "unit": unit, "labels": self.labels(i, nl) if nl else []})
            i += 1
        return out

    def get(self, ids):
        """{id: value} for the ids the app has"""
        out = {}
        ids = list(ids)
        for k in range(0, len(ids), 32):
            st, b = self.param_op(PQ_GET, struct.pack(f"<{len(ids[k:k + 32])}H", *ids[k:k + 32]))
            for j in range(b[0]):
                pid, v = struct.unpack_from("<Hi", b, 1 + 6 * j)
                out[pid] = v
        return out

    def set(self, pid, value):
        """-> the value now (clamped, or as the app took it)"""
        st, b = self.param_op(PQ_SET, struct.pack("<Hi", pid, value))
        if st != ST_OK:
            raise CompanionError("nofile" if st == ST_NONE else "failed",
                                 f"set {pid}: {STATUS[st] if st < len(STATUS) else st}")
        return struct.unpack_from("<i", b, 2)[0]

    def watch(self, on=True):
        st, b = self.param_op(PQ_WATCH, bytes([1 if on else 0]))
        return bool(b[0])


class ServeLink:
    """a host simulator (NAME_host --serve) as the FM-1: each request is sent, the device runs step_ms, its frames
    come back"""

    def __init__(self, cmd, step_ms=5):
        self.p = subprocess.Popen(shlex.split(cmd) if isinstance(cmd, str) else cmd, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)
        self.q, self.step = [], step_ms

    def cmd(self, line):
        """one line to the simulator, -> its output lines up to its "ok" (or "fail")"""
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        out = []
        while True:
            s = self.p.stdout.readline()
            if not s:
                raise CompanionError("silent", "the simulator stopped")
            s = s.rstrip("\n")
            if s.startswith("sx "):
                self.q.append(bytes.fromhex(s[3:]))
            elif s.startswith(("ok", "  ok", "FAIL", "fail")):
                out.append(s)
                return out
            else:
                out.append(s)

    def run(self, ms):
        self.cmd(f"run {ms}")

    def send(self, pkt):
        self.p.stdin.write("sx " + pkt.hex().upper() + "\n")
        return True

    def read(self, timeout):
        if not self.q:
            self.run(self.step)
        return self.q.pop(0) if self.q else None

    def drain(self):
        self.q.clear()

    def close(self):
        if self.p.poll() is None:
            self.p.stdin.write("quit\n")
            self.p.stdin.close()
            self.p.wait(10)


def find(backend, port=None):
    """the first FM-1 that answers the update's handshake -> its link"""
    dev = INST.Updater(backend, port).find()
    if not dev:
        raise CompanionError("notfound", "no FM-1 found (--port NAME: part of its MIDI port's name)")
    return dev.link


def show_info(c, out):
    h = c.hello()
    print(f"{h['name']} {h['app_version']}  ({h['identity']}, companion protocol {h['version']})", file=out)
    print("services: " + ", ".join(h["services"]), file=out)
    print(f"files: {h['files']}, parameters: {h['params']} (chunks: read {h['read_max']}, write {h['write_max']})",
          file=out)
    if h["backup"]:
        print(f"backup: {h['backup']['magic']} version {h['backup']['version']}, files .{h['backup']['ext']}", file=out)
    for k, v in h["extras"].items():
        print(f"  {k} = {v}", file=out)
    return 0


def show_files(c, out):
    fs = c.files()
    for f in fs:
        rw = "".join(ch if f["flags"] & bit else "-" for ch, bit in (("r", F_READ), ("w", F_WRITE), ("d", F_DELETE)))
        print(f"{f['index']:3}  {rw}{'p' if f['flags'] & F_PLAYING else ' '}  {f['kind']:8} {f['size']:8} / "
              f"{f['room']:<8} {f['name']}", file=out)
    if not fs:
        print("no files", file=out)
    return 0


def show_params(c, out):
    ps = c.params()
    vals = c.get([p["id"] for p in ps])
    for p in ps:
        v = vals.get(p["id"])
        shown = p["labels"][v - p["min"]] if p["labels"] and v is not None and 0 <= v - p["min"] < len(p["labels"]) else v
        rng = "/".join(p["labels"][:6]) + ("/..." if len(p["labels"]) > 6 else "") if p["labels"] else \
            f"{p['min']}..{p['max']}"
        print(f"{p['id']:5}  {p['group']:10} {p['name']:12} {shown!s:>8} {p['unit']:5} {rng}"
              + ("  (read only)" if p["flags"] & 1 else ""), file=out)
    if not ps:
        print("no parameters", file=out)
    return 0


def run(a, c, out):
    if a.what == "info":
        return show_info(c, out)
    if a.what == "ls":
        return show_files(c, out)
    if a.what == "params":
        return show_params(c, out)
    if a.what in ("get-param", "set-param"):
        if len(a.args) < (1 if a.what == "get-param" else 2) or not all(x.lstrip("-").isdigit() for x in a.args[:2]):
            raise CompanionError("usage", a.what + (" ID VALUE" if a.what == "set-param" else " ID") + ": numbers")
        pid = int(a.args[0])
        if a.what == "set-param":
            print(f"{pid} = {c.set(pid, int(a.args[1]))}", file=out)
            return 0
        got = c.get([pid])
        if pid not in got:
            raise CompanionError("nofile", f"no parameter {pid}")
        print(f"{pid} = {got[pid]}", file=out)
        return 0
    if not a.args:
        raise CompanionError("usage", f"{a.what}: which file?")
    if a.what == "get":
        f = c.index_of(a.args[0])
        data = c.read(f["index"])
        path = a.args[1] if len(a.args) > 1 else os.path.basename(a.args[0])
        with open(path, "wb") as fh:
            fh.write(data)
        print(f"{f['name']}: {len(data)} bytes -> {path} (CRC-32 checked)", file=out)
        return 0
    if a.what == "put":
        try:
            with open(a.args[0], "rb") as fh:
                data = fh.read()
        except OSError as e:
            raise CompanionError("usage", f"{a.args[0]}: {e.strerror}")
        f = c.index_of(a.args[1] if len(a.args) > 1 else os.path.basename(a.args[0]))
        resumed = c.write(f["index"], data)
        print(f"{f['name']}: {len(data)} bytes written and checked" + (f" (resumed at {resumed})" if resumed else ""),
              file=out)
        return 0
    f = c.index_of(a.args[0])
    c.delete(f["index"])
    print(f"{f['name']}: deleted", file=out)
    return 0


def main(argv=None, backend=None, out=sys.stdout):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["info", "ls", "get", "put", "rm", "params", "get-param", "set-param"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--port", metavar="NAME", help="MIDI port to use (part of its name)")
    ap.add_argument("--serve-cmd", metavar="CMD", help="a host simulator instead (NAME_host --serve)")
    a = ap.parse_args(argv)
    link = None
    try:
        if a.serve_cmd:
            link = ServeLink(a.serve_cmd)
        else:
            if backend is None:
                try:
                    backend = INST.MidoBackend()
                except ImportError:
                    raise CompanionError("usage", "needs mido and python-rtmidi (pip install mido python-rtmidi)")
            link = find(backend, a.port)
        return run(a, Client(link), out)
    except CompanionError as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT.get(e.code, 1)
    finally:
        if link:
            link.close()


if __name__ == "__main__":
    sys.exit(main())
