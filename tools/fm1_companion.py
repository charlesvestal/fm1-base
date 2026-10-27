#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Henri David
"""Talk to any fm1-base firmware built with companion = true (fm1.toml; device/companion.c, docs/COMPANION.md) over
USB-MIDI: what it is and has.

    python3 tools/fm1_companion.py info [--port NAME]
    python3 tools/fm1_companion.py info --serve-cmd "tests/companion_app/build/host/cptest_host --serve"

--serve-cmd runs a host simulator (NAME_host --serve) instead of an FM-1: the same requests, the same answers.

As a library: Client(link) with hello(); ServeLink(cmd) for a simulator, find(backend, port) for an FM-1.
Needs mido with python-rtmidi for an FM-1 (as tools/fm1_install.py). Exit codes: 0 done, 1 failed, 2 bad arguments,
3 FM-1 not found, 4 no answer (no companion services, or the connection lost).
"""
import argparse
import importlib
import os
import shlex
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
INST = importlib.import_module("fm1_install")   # (tools/: the update's wire format, the port finder)

HELLO = 0x60
SERVICES = ["capability query", "files", "parameters", "backup", "mirror", "screen reader", "Bluetooth MIDI", "WiFi",
            "USB audio in", "USB audio out", "the app's own SysEx", "the app's own commands", "the second core",
            "USB MIDI host", "Bluetooth audio"]
EXIT = {"usage": 2, "notfound": 3, "silent": 4}


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
                if not a or a[0] != cmd:
                    continue
                if self.keep(a[1]):
                    continue
                if match is None or match(a[1]):
                    return a[1]
        raise CompanionError("silent", f"no answer to 0x{cmd:02X}: no companion services, or it stopped answering")

    def keep(self, body):
        return False

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


def main(argv=None, backend=None, out=sys.stdout):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["info"])
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
        return show_info(Client(link), out)
    except CompanionError as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT.get(e.code, 1)
    finally:
        if link:
            link.close()


if __name__ == "__main__":
    sys.exit(main())
