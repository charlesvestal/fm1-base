#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Henri David
"""The companion services (device/companion.c, docs/COMPANION.md) in the host simulator, through tools/fm1_companion.py:
tests/companion_app built --host, driven over --serve as an FM-1 over USB. Checked: the capability query's answer (the
services, the identity, the app's name, version and tags), the app's own SysEx still the app's, a frame in the update's
framing with another cmd or a damaged one unanswered; then the same query in a script ("sysex", "expect_sysex": the
path the simulator's scripts and the browser take).
    python3 tests/companion_test.py [OUTDIR]"""
import io
import random
import struct
import subprocess
import sys
import zlib
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
    want = {"capability query", "files", "parameters", "screen reader", "backup", "the app's own SysEx", "USB audio in", "USB audio out"}
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


def raw_op(c, op, i, body=b""):
    return c.file_op(op, i, body)


def test_files(out):
    link, c = serve(out)
    h = c.hello()
    check("files in the services, 3 of them", "files" in h["services"] and h["files"] == 3, (h["services"], h["files"]))
    fs = c.files()
    check("the list", [(f["name"], f["kind"], f["size"], f["room"], f["flags"]) for f in fs] ==
          [("notes.txt", "text", 0, 4064, 15), ("sample.raw", "sample", 0, 65504, 7), ("info", "settings", 300, 300, 1)],
          fs)
    st, b = raw_op(c, C.OP_LIST, 3)
    check("past the last: no such file, the count", st == C.ST_NONE and b[:2] == b"\x03\x00", (st, b))
    info = c.read(2)
    check("a file the app makes, read whole and checked", info == bytes((k * 7 + 3) & 255 for k in range(300)))
    st, b = raw_op(c, C.OP_READ, 2, struct.pack("<IH", 290, 256))
    check("a read at the end: the rest only", st == 0 and b[4:] == info[290:], (st, len(b)))
    st, _ = raw_op(c, C.OP_READ, 2, struct.pack("<IH", 301, 4))
    check("a read past the end: out of range", st == 4, st)
    st, _ = raw_op(c, C.OP_BEGIN, 2, struct.pack("<II", 4, 0))
    check("writing a read-only file: not allowed", st == 3, st)
    st, _ = raw_op(c, C.OP_DELETE, 2)
    check("deleting it: not allowed", st == 3, st)
    st, _ = raw_op(c, C.OP_BEGIN, 0, struct.pack("<II", 5000, 0))
    check("more than its room: out of range", st == 4, st)
    # a write, another (the other copy), a delete
    c.write(0, b"first notes\n" * 8)
    check("notes.txt written, read back", c.read(0) == b"first notes\n" * 8)
    c.write(0, b"second")
    check("written again (the other copy), read back", c.read(0) == b"second" and c.files()[0]["size"] == 6)
    c.delete(0)
    check("deleted: empty", c.files()[0]["size"] == 0 and c.read(0) == b"")
    # a sample, cut short after 10 KB and resumed by another client (the cable out and in, the FM-1 on)
    sample = bytes(random.Random(7).randrange(256) for _ in range(40000))
    c.write(1, sample, stop_after=10000)
    check("cut short: nothing current yet", c.files()[1]["size"] == 0)
    resumed = C.Client(link, tries=1).write(1, sample)
    check("resumed where it stopped", 10000 <= resumed < 40000, resumed)
    check("the sample read back whole", c.read(1) == sample)
    # a write abandoned by another begin: the old file stays
    other = bytes(reversed(sample))
    c.write(1, other, stop_after=20000)
    c.write(0, b"notes meanwhile")
    check("abandoned: the old sample current still", c.read(1) == sample)
    # a CRC-32 that does not match: refused at commit, the old one stays
    st, _ = raw_op(c, C.OP_BEGIN, 0, struct.pack("<II", 4, 0x12345678))
    st2, b = raw_op(c, C.OP_DATA, 0, struct.pack("<I", 0) + b"abcd")
    st3, _ = raw_op(c, C.OP_COMMIT, 0)
    check("a wrong CRC-32: refused at commit", (st, st2, st3) == (0, 0, 5), (st, st2, st3))
    check("and the old notes current", c.read(0) == b"notes meanwhile")
    # out of order, and a frame again
    raw_op(c, C.OP_BEGIN, 0, struct.pack("<II", 8, zlib.crc32(b"abcdefgh")))
    st, b = raw_op(c, C.OP_DATA, 0, struct.pack("<I", 4) + b"efgh")
    check("data past next: out of order, with next", st == 7 and b[:4] == b"\0\0\0\0", (st, b))
    raw_op(c, C.OP_DATA, 0, struct.pack("<I", 0) + b"abcd")
    st, b = raw_op(c, C.OP_DATA, 0, struct.pack("<I", 0) + b"abcd")
    check("the same frame again: answered, not written twice", st == 0 and b[:4] == b"\x04\0\0\0", (st, b))
    raw_op(c, C.OP_DATA, 0, struct.pack("<I", 2) + b"cdefgh")
    st, _ = raw_op(c, C.OP_COMMIT, 0)
    check("overlapping the end of what came: the rest taken", st == 0 and c.read(0) == b"abcdefgh", st)
    # busy: the app plays
    link.send(bytes([0xF0, 0x7D, 0x43, 0x50, 0x02, 0xF7]))
    link.run(5)
    try:
        c.write(1, b"x" * 100)
        busy = None
    except C.CompanionError as e:
        busy = e.code
    check("playing: a sample refused (busy)", busy == "busy", busy)
    try:
        c.delete(1)
        busy = None
    except C.CompanionError as e:
        busy = e.code
    check("playing: its delete refused", busy == "busy", busy)
    c.write(0, b"while playing")
    check("playing: notes.txt (written while playing too) taken", c.read(0) == b"while playing")
    check("playing: reads go on", c.read(1) == sample)
    link.send(bytes([0xF0, 0x7D, 0x43, 0x50, 0x03, 0xF7]))
    link.run(5)
    c.write(1, b"stopped")
    check("stopped: the sample taken", c.read(1) == b"stopped")
    check("commits counted by the app", link.cmd("expect commits 7")[-1].startswith("  ok"))
    link.close()


def test_tool(out):
    """the command line: ls, put, get, rm on a simulator"""
    serve_cmd = f"{HOST} --serve {out}"
    src, back = out / "notes-in.txt", out / "notes-back.txt"
    src.write_bytes(b"from the tool\n" * 50)
    def t(*a):
        return C.main([*a, "--serve-cmd", serve_cmd], out=io.StringIO())
    check("put", t("put", str(src), "notes.txt") == 0)
    check("get of a file not written in this run: none there (a new simulator each run)",
          t("get", "notes.txt", str(back)) == 0 and back.read_bytes() == b"")
    check("rm of a read-only file: refused", t("rm", "info") == 1)
    check("get of no such file", t("get", "nothing") == 6)


def app_tempo(link, v):
    """the app's own change (its panel would make it): TEMPO = v (cmd 4, v - 40)"""
    link.send(bytes([0xF0, 0x7D, 0x43, 0x50, 0x04, v - 40, 0xF7]))


def test_params(out):
    link, c = serve(out)
    h = c.hello()
    check("parameters in the services, 6 of them", "parameters" in h["services"] and h["params"] == 6,
          (h["services"], h["params"]))
    check("the groups", c.groups() == ["GLOBAL", "SOUND", "INFO"], c.groups())
    ps = c.params()
    check("the descriptions", [(p["id"], p["name"], p["type"], p["group"], p["min"], p["max"], p["default"], p["unit"])
                               for p in ps] ==
          [(10, "TEMPO", "number", "GLOBAL", 40, 240, 120, "BPM"), (11, "SWING", "number", "GLOBAL", 0, 100, 0, "%"),
           (20, "WAVE", "enum", "SOUND", 0, 3, 0, ""), (21, "CUTOFF", "number", "SOUND", 0, 127, 100, ""),
           (30, "LOAD", "number", "INFO", 0, 100, 0, "%"), (40, "MODE", "enum", "SOUND", -20, 19, 0, "")], ps)
    check("an enum's labels", ps[2]["labels"] == ["SAW", "SQR", "TRI", "SIN"], ps[2]["labels"])
    check("40 labels, asked for in parts", ps[5]["labels"] == [f"MODE {k:02}" for k in range(40)], ps[5]["labels"])
    check("read only flagged", ps[4]["flags"] == 1 and all(p["flags"] == 0 for p in ps if p["id"] != 30))
    st, b = c.param_op(C.PQ_LABELS, struct.pack("<HH", 5, 0))
    check("a labels answer holds 200 bytes of names at most", st == 0 and 0 < b[4] < 40 and len(b) - 5 <= 200,
          (st, b[4], len(b)))
    check("get", c.get([10, 11, 20, 21, 30, 40]) == {10: 120, 11: 0, 20: 0, 21: 100, 30: 37, 40: 0})
    st, b = c.param_op(C.PQ_GET, struct.pack("<HH", 10, 99))
    check("get with an id the app has not: the others, status 2", st == 2 and b[0] == 1, (st, b))
    check("set", c.set(11, 33) == 33 and c.get([11]) == {11: 33})
    check("set above max: clamped", c.set(10, 999) == 240)
    check("set below min: clamped", c.set(40, -100) == -20)
    check("set as the app takes it (CUTOFF even)", c.set(21, 77) == 76)
    for pid, code in ((30, "failed"), (99, "nofile")):
        try:
            c.set(pid, 1)
            got = None
        except C.CompanionError as e:
            got = e.code
        check(f"set {pid}: refused", got == code, got)
    # watching: the app's own changes come, the computer's own sets do not come back
    check("watch on", c.watch(True))
    app_tempo(link, 100)
    link.run(120)
    notes = c.poll()
    check("the app changed TEMPO: told", (10, 100) in notes, notes)
    c.set(11, 5)
    link.run(120)
    check("the computer's own set: not sent back", c.poll() == [], c.notes)
    app_tempo(link, 101)
    st, _ = c.file_op(C.OP_LIST, 0)                   # (a change arriving while another answer is awaited: kept)
    link.run(120)
    check("a change while waiting for another answer: kept", (10, 101) in c.poll())
    for v in range(60, 80):                           # (more changes than a frame: all of them, in order)
        app_tempo(link, v)
        link.run(2)
    link.run(200)
    notes = c.poll()
    check("many changes: the last value reaches the computer", notes and notes[-1] == (10, 79), notes[-3:])
    check("watch off", not c.watch(False))
    app_tempo(link, 90)
    link.run(120)
    check("off: nothing comes", c.poll() == [])
    c.watch(True)
    link.run(10500)                                    # (no request for 10.5 s: the watch lapses)
    app_tempo(link, 91)
    link.run(120)
    check("10 s without a request: the watch lapsed", c.poll() == [])
    link.close()


def test_watch_script(out):
    """a watch as the device's ring sees it (a script: "usb stall", "usb unplug" / "plug"): changes held once the ring
    is full while the computer does not read, the last value sent once it does (never waiting), a new connection ends
    the watch"""
    w = INST.frame(C.PARAMS, bytes([C.PQ_WATCH, 1]))
    def t(v):
        return "sysex F0 7D 43 50 04 %02X F7" % (v - 40)

    def changed(v):
        return INST.frame(C.PARAMS, bytes([C.PQ_CHANGED, 0, 1]) + struct.pack("<Hi", 10, v)).hex(" ").upper()

    lines = ["wait 300", f"sysex {w.hex(' ').upper()}", "sysex_out",
             t(100), "wait 120", f"expect_sysex {changed(100)}", "sysex_out",
             "usb stall", *[s for v in range(110, 140) for s in (t(v), "wait 60")], "echo --stalled--\n", "sysex_out",
             "echo --held--\n", "usb read", "wait 200", f"expect_sysex {changed(139)}", "sysex_out",
             "usb unplug", "usb plug", t(102), "wait 120", "echo --replugged--\n", "sysex_out"]
    script = out / "watch.txt"
    script.write_text("\n".join(lines) + "\n")
    r = subprocess.run([str(HOST), str(script), str(out)], capture_output=True, text=True)
    stalled = r.stdout.split("--stalled--")[-1].split("--held--")[0].count("sysex<") if "--held--" in r.stdout else -1
    check("changes sent, the last one once the computer read again", r.returncode == 0, r.stdout[-600:])
    check("the computer not reading: the ring full, changes held (never waiting)", 0 < stalled < 30, stalled)
    check("a new USB connection: the watch ended", "sysex<" not in r.stdout.split("--replugged--")[-1],
          r.stdout[-300:])


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
    test_files(out)
    test_tool(out)
    test_params(out)
    test_watch_script(out)
    test_script(out)
    print("companion: " + ("all ok" if not fails else f"{fails} FAILED"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
