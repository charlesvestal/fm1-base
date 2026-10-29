#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Charles Vestal
"""Drive an FM-1 running an fm1-base firmware over USB-MIDI, as a player would, and read it back: for testing by hand
or by a program (an agent). Needs mido with python-rtmidi.

  tools/fm1_remote.py [--port NAME] COMMAND ...
  one or more commands, run in order. --port: the FM-1's MIDI port (part of its name); else it is found by its name
  or by its answer to the screen reader's HELLO (tools/fm1ports.py), whatever its port is called:
    press BTN | release BTN | tap BTN       a button (FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+)
    key K down | key K up | tapkey K        a key, 0..26 (0 = F3), as pressed (no velocity reading)
    turn ENC N                              an encoder, N detents (SELECT ALGO PRESET K1 K2 K3 K4)
    master N | master pot                   MASTER at 0..4096, or the pot's own again
    wait MS                                 let it run (with listen: print what it says meanwhile)
    listen                                  a screen reader starts listening: every message it says is printed
    screen                                  what is on the screen, in words (the screen reader's channel)
    log                                     the last crash, the registers at the last four, the crash history kept
                                            in flash, the supplies, the clock as the boot loader left it, and the
                                            radio's log (all survive a restart; the history a power-off too)
    crashes [--clear]                       the crash history kept in flash (the newest 8, oldest first: it
                                            survives a power-off; an install starts it again), each crash's pc
                                            and rets as function+offset with --elf; --clear erases it
  options (anywhere): --elf FILE            the firmware's ELF (build/NAME.elf) for crashes' symbols
    release-all                             every button and key up, MASTER the pot's own
    wifi on | wifi off | wifi scan          WiFi (a firmware with wifi = true): on, off, look for networks
    wifi status | wifi found | wifi saved   its state, network, address and signal; the networks the scan found;
                                            the networks kept
    wifi join SSID [PASSWORD]               join a network (kept once joined); no password: a kept one's, or none
    wifi forget N                           forget the network kept at N (0 = the most recent)
    bta on | bta off | bta scan             Bluetooth audio (ble_stack = "own" with bt_audio = true): on, off, look
    bta status | bta found | bta known      for headphones and speakers; its state; the devices found; remembered
    bta pick N | bta connect N              connect found device N / remembered device N (0 = the last one)
    bta hangup | bta forget N               end the link; forget remembered device N
    dev scan | dev stop | dev found         Bluetooth MIDI devices (ble_stack = "own", ble_central = true): look for
    dev connect N | dev disconnect          them, list them, connect to found device N, let go
    dev status                              its state, the last failure and its code
  e.g.  tools/fm1_remote.py tap REC wait 800 screen

The firmware's side: device/main_fm1.c remote_frame (F0 7D 4C 43 ...), log_frame (F0 7D 4C 47), wifi_frame
(F0 7D 4C 57), the crash history (device/crashlog.c, F0 7D 4C 48: its frames documented above log_frame), and the
screen reader's protocol (fm1-reader PROTOCOL.md: F0 7D 53 52).

The crash history's frames, for a program (parse_crash_frames reads them):
  F0 7D 4C 48 F7                  asks; the answer, one frame a record, the oldest first, F0 7D 4C 48 <ASCII> F7:
    crash I/N n COUNT vec HEX pc HEX rets HEX sp HEX ms DEC emu HEX dbg HEX psr HEX build HEX seq DEC
    regs I/N r0 HEX .. r15 HEX usp HEX ssp HEX        (when the record has the registers)
  then "end N" (N records: none is "end 0", no end is an answer lost)
  F0 7D 4C 48 43 4C 52 F7         ("LHCLR") erases it; the answer "cleared" or "clear failed"."""
import struct
import sys
import time

import fm1ports

BTN = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"]
ENC = ["SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"]
LC, LG, LW, SR = [0x7D, 0x4C, 0x43], [0x7D, 0x4C, 0x47], [0x7D, 0x4C, 0x57], [0x7D, 0x53, 0x52]
WIFI_OP = {"on": 1, "off": 2, "scan": 3, "status": 4, "saved": 5, "forget": 6, "join": 7, "found": 8}
LA = [0x7D, 0x4C, 0x41]                    # Bluetooth audio (device/bt_own_audio.c bta_frame)
BTA_OP = {"on": 1, "off": 2, "scan": 3, "status": 4, "found": 5, "pick": 6, "connect": 7, "known": 8, "hangup": 9,
          "forget": 10}
LD = [0x7D, 0x4C, 0x44]                    # Bluetooth MIDI devices, own stack (device/bt_own_central.c dev_frame)
DEV_OP = {"scan": 1, "stop": 2, "found": 3, "connect": 4, "disconnect": 5, "status": 6}


LH = [0x7D, 0x4C, 0x48]                    # the crash history in flash (device/crashlog.c cl_request)
LH_CLEAR = LH + [0x43, 0x4C, 0x52]         # "CLR": erase it
CRASH_HEX = ("vec", "pc", "rets", "sp", "emu", "dbg", "psr", "build")


def nibbles(text):
    return [v for byte in text.encode() for v in (byte >> 4, byte & 15)]


def parse_crash_frames(texts):
    """the history's answer (each frame's text after F0 7D 4C 48) into (records, end): records a list of dicts, the
    oldest first (i, n, count, vec, pc, rets, sp, ms, emu, dbg, psr, build, seq; regs: r0..r15, usp, ssp when sent),
    end the "end N" count, None when the end frame never came (an answer lost: not the same as none)"""
    recs, end = {}, None
    for t in texts:
        t = t.decode("latin1") if isinstance(t, (bytes, bytearray)) else t
        w = t.split()
        if not w:
            continue
        if w[0] == "end" and len(w) == 2:
            end = int(w[1])
        elif w[0] in ("crash", "regs") and len(w) % 2 == 0:
            i, n = (int(x) for x in w[1].split("/"))
            kv = dict(zip(w[2::2], w[3::2]))
            r = recs.setdefault(i, {"i": i, "n": n})
            if w[0] == "crash":
                for k, v in kv.items():
                    r[k] = int(v, 16) if k in CRASH_HEX else int(v)
                r["count"] = r.pop("n", r.get("count"))
                r["n"] = n
            else:
                r["regs"] = {k: int(v, 16) for k, v in kv.items()}
    return [recs[i] for i in sorted(recs)], end


class ElfSymbols:
    """an ELF's function and object symbols (32- or 64-bit, little-endian: the FM-1's is ELF32), for an address as
    function+offset, addr2line's way without the toolchain"""

    def __init__(self, path):
        d = open(path, "rb").read()
        if d[:4] != b"\x7fELF" or d[5] != 1:
            raise SystemExit(f"{path}: not a little-endian ELF")
        is64 = d[4] == 2
        if is64:
            shoff, = struct.unpack_from("<Q", d, 0x28)
            shentsize, shnum = struct.unpack_from("<HH", d, 0x3A)
        else:
            shoff, = struct.unpack_from("<I", d, 0x20)
            shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
        secs = []
        for k in range(shnum):
            o = shoff + k * shentsize
            if is64:
                _, typ, _, _, off, size, link, _, _, entsize = struct.unpack_from("<IIQQQQIIQQ", d, o)
            else:
                _, typ, _, _, off, size, link, _, _, entsize = struct.unpack_from("<IIIIIIIIII", d, o)
            secs.append((typ, off, size, link, entsize))
        syms = []
        for typ, off, size, link, entsize in secs:
            if typ != 2 or not entsize:                 # SHT_SYMTAB
                continue
            stroff = secs[link][1]
            for j in range(size // entsize):
                o = off + j * entsize
                if is64:
                    name, info, _, _, value, sz = struct.unpack_from("<IBBHQQ", d, o)
                else:
                    name, value, sz, info, _, _ = struct.unpack_from("<IIIBBH", d, o)
                if info & 15 not in (1, 2) or not value:      # STT_OBJECT, STT_FUNC
                    continue
                end = d.index(b"\0", stroff + name)
                syms.append((value, sz, d[stroff + name:end].decode("latin1")))
        self.syms = sorted(syms)

    def name(self, addr):
        """addr as "function+0xOFF" (the symbol at or below it: inside its size, or the nearest when sizes are 0);
        "?" none"""
        best = None
        for value, sz, nm in self.syms:
            if value > addr:
                break
            if not sz or addr < value + sz:
                best = (value, nm)
        if best is None:
            return "?"
        return f"{best[1]}+{addr - best[0]:#x}"


def format_crash(r, elf=None):
    """one record as lines for a reader"""
    where = (lambda a: f" ({elf.name(a)})") if elf else (lambda a: "")
    lines = [f"crash {r['i']}/{r['n']}: n {r['count']} vec {r['vec']:#x} pc {r['pc']:08X}{where(r['pc'])} "
             f"rets {r['rets']:08X}{where(r['rets'])}",
             f"  sp {r['sp']:08X} at {r['ms']} ms, emu {r['emu']:08X} dbg {r['dbg']:08X} psr {r['psr']:08X}, "
             f"build {r['build']:08X}, seq {r['seq']}"]
    regs = r.get("regs")
    if regs:
        rs = [f"r{k} {regs[f'r{k}']:08X}" for k in range(16)]
        lines += ["  " + " ".join(rs[k:k + 8]) for k in (0, 8)]
        lines.append(f"  usp {regs['usp']:08X} ssp {regs['ssp']:08X}")
    return lines


class FM1:
    def __init__(self, port=None, elf=None):
        import importlib
        self.mido = importlib.import_module("mido")   # (here: the parser above needs no MIDI)
        try:
            self.inp, self.out, self.name = fm1ports.connect(port, mido=self.mido)
        except LookupError as e:
            raise SystemExit(f"fm1_remote: {e}")
        self.btn = self.keys = 0
        self.listening = False
        self.elf = ElfSymbols(elf) if elf else None

    def sysex(self, data):
        self.out.send(self.mido.Message("sysex", data=data))

    def frames(self, head, secs, until=None):
        got, t = [], time.time()
        while time.time() - t < secs:
            for m in self.inp.iter_pending():
                if m.type == "sysex" and list(m.data[:3]) == head:
                    got.append(bytes(m.data[3:]))
                    if until and until(got[-1]):
                        return got
            time.sleep(0.01)
        return got

    def buttons(self):
        self.sysex(LC + [1, self.btn & 0x7F, (self.btn >> 7) & 0x7F])

    def keys_send(self):
        k = self.keys
        self.sysex(LC + [2, k & 0x7F, (k >> 7) & 0x7F, (k >> 14) & 0x7F, (k >> 21) & 0x7F])

    def run(self, args):
        i = 0
        while i < len(args):
            c = args[i]
            a = args[i + 1] if i + 1 < len(args) else ""
            b = args[i + 2] if i + 2 < len(args) else ""
            if c in ("press", "release", "tap"):
                bit = 1 << BTN.index(a.upper())
                if c != "release":
                    self.btn |= bit
                    self.buttons()
                if c == "tap":
                    time.sleep(0.08)
                if c != "press":
                    self.btn &= ~bit
                    self.buttons()
                time.sleep(0.03)
                i += 2
            elif c == "key":
                bit = 1 << int(a)
                self.keys = self.keys | bit if b == "down" else self.keys & ~bit
                self.keys_send()
                time.sleep(0.03)
                i += 3
            elif c == "tapkey":
                self.keys |= 1 << int(a)
                self.keys_send()
                time.sleep(0.1)
                self.keys &= ~(1 << int(a))
                self.keys_send()
                time.sleep(0.03)
                i += 2
            elif c == "turn":
                n = int(b)
                while n:
                    step = max(-63, min(63, n))
                    self.sysex(LC + [3, ENC.index(a.upper()), 64 + step])
                    n -= step
                time.sleep(0.05)
                i += 3
            elif c == "master":
                v = 4097 if a == "pot" else max(0, min(4096, int(a)))
                self.sysex(LC + [4, v & 0x7F, v >> 7])
                time.sleep(0.05)
                i += 2
            elif c == "wait":
                if self.listening:
                    for f in self.frames(SR, int(a) / 1000):
                        if f and f[0] == 16:
                            print("said:", f[2:].decode("latin1", "replace"))
                else:
                    time.sleep(int(a) / 1000)
                i += 2
            elif c == "listen":
                self.sysex(SR + [1, 1])                  # HELLO: from now on it says each change
                self.listening = True
                i += 1
            elif c == "screen":
                self.sysex(SR + [1, 1])                  # HELLO, then where am I
                time.sleep(0.2)
                for m in self.inp.iter_pending():
                    pass
                self.sysex(SR + [3])
                for f in self.frames(SR, 1.0):
                    if f and f[0] == 16:
                        print("screen:", f[2:].decode("latin1", "replace"))
                i += 1
            elif c == "log":
                self.sysex(LG)
                for f in self.frames(LG, 2.0, until=lambda f: f == b"end"):
                    if f != b"end":
                        print("log:", f.decode("latin1", "replace"))
                i += 1
            elif c == "crashes":
                clear = a == "--clear"
                if clear:
                    self.sysex(LH_CLEAR)
                    got = self.frames(LH, 3.0, until=lambda f: f in (b"cleared", b"clear failed"))
                    print("crashes:", got[-1].decode("latin1") if got else "no answer (an older firmware?)")
                else:
                    self.sysex(LH)
                    recs, end = parse_crash_frames(self.frames(LH, 3.0, until=lambda f: f.startswith(b"end ")))
                    if end is None:
                        print("crashes: no answer (an older firmware, or the answer lost)")
                    elif not recs:
                        print("crashes: none saved")
                    for r in recs:
                        for line in format_crash(r, self.elf):
                            print(line)
                    if end is not None and end != len(recs):
                        print(f"crashes: {end} sent, {len(recs)} read: some frames lost")
                i += 2 if clear else 1
            elif c == "wifi":
                op = WIFI_OP.get(a)
                if op is None:
                    raise SystemExit(f"wifi {a!r}: on, off, scan, status, found, saved, join, forget")
                data, used = [], 2
                if a == "forget":
                    data, used = [int(b)], 3
                elif a == "join":
                    pw = args[i + 3] if i + 3 < len(args) and args[i + 3] not in ("wait", "wifi", "log", "screen") else None
                    data = nibbles(b) + ([0, 0] + nibbles(pw) if pw is not None else [])
                    used = 4 if pw is not None else 3
                self.sysex(LW + [op] + data)
                for f in self.frames(LW, 3.0, until=lambda f: f[:1] == b"\x11"):
                    if f[:1] == b"\x10":
                        print("wifi:", f[1:].decode("latin1", "replace"))
                i += used
            elif c == "bta":
                op = BTA_OP.get(a)
                if op is None:
                    raise SystemExit(f"bta {a!r}: " + ", ".join(BTA_OP))
                data, used = ([int(b)], 3) if a in ("pick", "connect", "forget") else ([], 2)
                self.sysex(LA + [op] + data)
                for f in self.frames(LA, 3.0, until=lambda f: f[:1] == b"\x11"):
                    if f[:1] == b"\x10":
                        print("bta:", f[1:].decode("latin1", "replace"))
                i += used
            elif c == "dev":
                op = DEV_OP.get(a)
                if op is None:
                    raise SystemExit(f"dev {a!r}: " + ", ".join(DEV_OP))
                data, used = ([int(b)], 3) if a == "connect" else ([], 2)
                self.sysex(LD + [op] + data)
                for f in self.frames(LD, 3.0, until=lambda f: f[:1] == b"\x11"):
                    if f[:1] == b"\x10":
                        print("dev:", f[1:].decode("latin1", "replace"))
                i += used
            elif c == "release-all":
                self.btn = self.keys = 0           # a frame at a time: three back to back, the FM-1 may drop one
                self.buttons()
                time.sleep(0.03)
                self.keys_send()
                time.sleep(0.03)
                self.sysex(LC + [4, 4097 & 0x7F, 4097 >> 7])
                time.sleep(0.03)
                i += 1
            else:
                raise SystemExit(f"unknown command {c!r}\n\n{__doc__}")


def main(argv):
    port = None
    elf_path = None
    if "--elf" in argv:                          # (anywhere: the firmware's ELF for crashes' symbols)
        k = argv.index("--elf")
        if k + 1 >= len(argv):
            sys.exit("--elf FILE")
        elf_path = argv[k + 1]
        argv = argv[:k] + argv[k + 2:]
    if argv[:1] == ["--port"] and len(argv) > 1:
        port, argv = argv[1], argv[2:]
    if not argv:
        sys.exit(__doc__)
    FM1(port, elf_path).run(argv)


if __name__ == "__main__":
    main(sys.argv[1:])
