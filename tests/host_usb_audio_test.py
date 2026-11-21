#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 henri (hdavid)
"""The host simulator's USB audio (host/plat_host.c usb_play, usb_rec) with the template, which mixes the computer's
audio at MASTER and records the master (no APP_USB_PLAY, no APP_USB_CAPTURE): a computer plays a known stereo signal
(16-bit: a sine on L, a ramp on R) while it records; with MASTER at full and the template silent, the recording is
the signal, every frame, bit for bit, from the render that started after usb_play. Then MASTER at half: the
recording the signal at master^2 (main_fm1.c's uac_play gain), within a rounding.

  tests/host_usb_audio_test.py HOST_EXE OUTDIR"""
import math
import struct
import subprocess
import sys
from pathlib import Path


def wav_write(path: Path, frames: list[tuple[int, int]]) -> None:
    data = b"".join(struct.pack("<hh", l, r) for l, r in frames)
    path.write_bytes(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
                     struct.pack("<IHHIIHH", 16, 1, 2, 44100, 44100 * 4, 4, 16) + b"data" +
                     struct.pack("<I", len(data)) + data)


def wav_read(path: Path) -> tuple[int, int, list[tuple[int, ...]]]:
    b = path.read_bytes()
    assert b[:4] == b"RIFF" and b[8:12] == b"WAVE", path
    nch, rate = struct.unpack_from("<HI", b, 22)
    bits = struct.unpack_from("<H", b, 34)[0]
    assert b[36:40] == b"data" and rate == 44100, path
    n = struct.unpack_from("<I", b, 40)[0]
    w = bits // 8
    raw = b[44:44 + n]
    vals = [int.from_bytes(raw[i:i + w], "little", signed=True) for i in range(0, len(raw), w)]
    return nch, bits, [tuple(vals[i:i + nch]) for i in range(0, len(vals), nch)]


def run(exe: Path, out: Path, script: str) -> str:
    sc = out / "usb_audio.txt"
    sc.write_text(script)
    r = subprocess.run([str(exe), str(sc), str(out)], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"host_usb_audio: the simulator failed ({r.returncode}):\n{r.stdout}{r.stderr}")
    return r.stdout


def main() -> int:
    exe, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    n = 44100
    sig = [(round(12000 * math.sin(2 * math.pi * 997 * i / 44100)), (i * 7) % 65536 - 32768) for i in range(n)]
    wav_write(out / "usb_in.wav", sig)
    for master, exact in ((4096, True), (2048, False)):
        run(exe, out, f"master {master}\nwait 100\nusb_rec rec.wav\nusb_play usb_in.wav\nwait 1200\n"
                      f"expect usb_play_frames {n}\nusb_recstop\n")
        nch, bits, rec = wav_read(out / "rec.wav")
        assert nch == 2 and bits == 16, (nch, bits)
        g = (master * master >> 12) / 4096.0
        start = next(i for i, f in enumerate(rec) if f != (0, 0))   # (the first render after usb_play)
        got = rec[start:start + n]
        assert len(got) == n, len(got)
        if exact:
            bad = [i for i in range(n) if got[i] != sig[i]]
            assert not bad, f"MASTER full: frame {bad[0]}: {got[bad[0]]} != {sig[bad[0]]}"
        else:
            err = max(abs(got[i][c] - sig[i][c] * g) for i in range(n) for c in (0, 1))
            assert err <= 1.0, err
        assert all(f == (0, 0) for f in rec[start + n:]), "silence after the computer's audio"
        print(f"  ok host USB audio, MASTER {master}: {n} frames recorded as played"
              f"{' bit for bit' if exact else f' x {g:.3f}'} (from frame {start}), silence after")
    return 0


if __name__ == "__main__":
    sys.exit(main())
