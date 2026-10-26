#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Charles Vestal
"""Drive an FM-1 running an fm1-base firmware over USB-MIDI, as a player would, and read it back: for testing by hand
or by a program (an agent). Needs mido with python-rtmidi.

  tools/fm1_remote.py COMMAND ...           one or more commands, run in order:
    press BTN | release BTN | tap BTN       a button (FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+)
    key K down | key K up | tapkey K        a key, 0..26 (0 = F3), as pressed (no velocity reading)
    turn ENC N                              an encoder, N detents (SELECT ALGO PRESET K1 K2 K3 K4)
    master N | master pot                   MASTER at 0..4096, or the pot's own again
    wait MS                                 let it run (with listen: print what it says meanwhile)
    listen                                  a screen reader starts listening: every message it says is printed
    screen                                  what is on the screen, in words (the screen reader's channel)
    ble                                     compact BLE health counters (USB only)
    log                                     the last crash, the registers at the last four, the supplies, the
                                            clock as the boot loader left it, and the radio's log
                                            (all survive a restart)
    release-all                             every button and key up, MASTER the pot's own
    wifi on | wifi off | wifi scan          WiFi (a firmware with wifi = true): on, off, look for networks
    wifi status | wifi found | wifi saved   its state, network, address and signal; the networks the scan found;
                                            the networks kept
    wifi join SSID [PASSWORD]               join a network (kept once joined); no password: a kept one's, or none
    wifi forget N                           forget the network kept at N (0 = the most recent)
  e.g.  tools/fm1_remote.py tap REC wait 800 screen

The firmware's side: device/main_fm1.c remote_frame (F0 7D 4C 43 ...), log_frame (F0 7D 4C 47), wifi_frame
(F0 7D 4C 57), and the screen
reader's protocol (fm1-reader PROTOCOL.md: F0 7D 53 52)."""
import sys
import time

import mido

BTN = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"]
ENC = ["SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"]
LC, LG, LW, SR = [0x7D, 0x4C, 0x43], [0x7D, 0x4C, 0x47], [0x7D, 0x4C, 0x57], [0x7D, 0x53, 0x52]
WIFI_OP = {"on": 1, "off": 2, "scan": 3, "status": 4, "saved": 5, "forget": 6, "join": 7, "found": 8}


def nibbles(text):
    return [v for byte in text.encode() for v in (byte >> 4, byte & 15)]


class FM1:
    def __init__(self, port=None):
        # Explicit selection avoids the identically named BlueZ BLE-MIDI port.
        names = [n for n in mido.get_output_names() if (n == port if port else "FM-1" in n)]
        if not names:
            raise SystemExit("no FM-1 on USB-MIDI")
        self.out = mido.open_output(names[0])
        inputs = [n for n in mido.get_input_names() if (n == port if port else "FM-1" in n)]
        if not inputs:
            self.out.close()
            raise RuntimeError("no matching FM-1 USB-MIDI input")
        self.inp = mido.open_input(inputs[0])
        self.btn = self.keys = 0
        self.listening = False

    def close(self):
        self.inp.close()
        self.out.close()

    def read_log(self, compact=False):
        # A reconnect can interrupt an LG transfer. Retry the whole snapshot,
        # discard incomplete attempts, and bound the operation to three reads.
        for attempt in range(3):
            if attempt:
                time.sleep(0.25)
            for _ in self.inp.iter_pending():
                pass
            self.sysex(LG + ([1] if compact else []))
            frames = self.frames(LG, 8.0, until=lambda f: f == b"end")
            if frames and frames[-1] == b"end":
                return "\n".join(f.decode("ascii", "replace") for f in frames[:-1]) + "\n"
        raise RuntimeError("USB log incomplete (missing end) after 3 attempts; close other FM-1 clients")

    def sysex(self, data):
        self.out.send(mido.Message("sysex", data=data))

    def frames(self, head, secs, until=None):
        got, t = [], time.monotonic()
        while time.monotonic() - t < secs:
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
            elif c in ("log", "ble"):
                for line in self.read_log(compact=c == "ble").splitlines():
                    print("log:", line)
                i += 1
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


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    args=sys.argv[1:]
    port=None
    if args[:1]==["--port"]:
        if len(args)<3: sys.exit("--port USB_MIDI_NAME COMMAND ...")
        port,args=args[1],args[2:]
    remote=FM1(port)
    try:
        remote.run(args)
    finally:
        remote.close()
