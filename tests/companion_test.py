#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Henri David
"""The companion services (device/companion.c, docs/COMPANION.md) in the host simulator, through tools/fm1_companion.py:
tests/companion_app built --host, driven over --serve as an FM-1 over USB. Checked: the capability query's answer (the
services, the identity, the app's name, version and tags), the app's own SysEx still the app's, a frame in the update's
framing with another cmd or a damaged one unanswered; then the same query in a script ("sysex", "expect_sysex": the
path the simulator's scripts and the browser take).
    python3 tests/companion_test.py [OUTDIR]"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
C = __import__("fm1_companion")
INST = C.INST

APP = ROOT / "tests" / "companion_app"
HOST = APP / "build" / "host" / "cptest_host"
fails = 0


def check(what, ok, got=""):
    global fails
    print(f"  {'ok  ' if ok else 'FAIL'} {what}" + (f": {got}" if not ok and got != "" else ""))
    fails += not ok


def serve(out):
    link = C.ServeLink([str(HOST), "--serve", str(out)])
    link.run(300)
    return link, C.Client(link, tries=1)


def test_hello(out):
    link, c = serve(out)
    h = c.hello()
    check("protocol version 2", h["version"] == 2, h["version"])
    check("the backup format", h["backup"] == {"magic": "CPTB", "version": 2, "ext": "cpb"}, h["backup"])
    check("identity FM-1_1400", h["identity"] == "FM-1_1400", h["identity"])
    check("name and version", (h["name"], h["app_version"]) == ("CPTEST", "1.2"), (h["name"], h["app_version"]))
    check("the app's tags", h["extras"] == {"editor": "cptest/1", "site": "https://example.invalid/cptest"},
          h["extras"])
    want = {"capability query", "screen reader", "backup", "the app's own SysEx", "USB audio in", "USB audio out"}
    check("services", set(h["services"]) == want, h["services"])
    old = c.request(0x60)                         # a version 1 reader: the same fields up to the extras, the end unread
    (ident, name, ver, extras), tail = C.cstrings(old[13:], 4)
    check("a version 1 reader's fields", (ident, name, ver) == (h["identity"], "CPTEST", "1.2") and extras.startswith("editor="), (ident, name, ver, extras))
    check("the backup format at the answer's end", tail == b"CPTB" + bytes([2, 0]) + b"cpb\0", tail)
    check("chunks", (h["read_max"], h["write_max"]) == (256, 512), (h["read_max"], h["write_max"]))
    # the app's own SysEx: the app's, not the companion's (no answer)
    link.send(bytes([0xF0, 0x7D, 0x43, 0x50, 0x01, 0xF7]))
    link.run(5)
    check("the app's frame reached the app", link.cmd("expect own 1")[-1].startswith("  ok"))
    check("and nothing came back", not link.q, link.q)
    # another cmd in the update's framing, and a damaged hello: no answer
    link.send(INST.frame(0x6F))
    bad = bytearray(INST.frame(0x60, b"\x01"))
    bad[-2] ^= 1
    link.send(bytes(bad))
    link.run(20)
    check("cmd 0x6F and a damaged frame unanswered", not link.q, link.q)
    link.close()


def test_script(out):
    """the script's "sysex" (hsx_in): the same answer, as expect_sysex sees it"""
    link, c = serve(out)
    c.hello()
    link.drain()
    link.send(INST.frame(C.HELLO))
    link.run(5)
    answer = link.q[0]
    link.close()
    script = out / "hello.txt"
    script.write_text(f"wait 300\nsysex {INST.frame(C.HELLO).hex(' ').upper()}\nexpect_sysex {answer.hex(' ').upper()}\n"
                      "sysex F0 7D 43 50 02 F7\nexpect own 1\n")
    r = subprocess.run([str(HOST), str(script), str(out)], capture_output=True, text=True)
    check("a script's sysex: answered the same", r.returncode == 0, r.stdout[-400:])


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else APP / "build" / "tests"
    out.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([sys.executable, str(ROOT / "tools" / "build.py"), str(APP), "--host"], capture_output=True,
                       text=True)
    if r.returncode:
        print(r.stdout[-2000:], r.stderr[-2000:])
        return 1
    test_hello(out)
    test_script(out)
    print("companion: " + ("all ok" if not fails else f"{fails} FAILED"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
