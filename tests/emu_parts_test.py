#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Henri David
"""The browser emulator's optional parts (fm1.toml emu, FM1B_EMU; tools/build.py --web): the template built with none
makes the page, the worklet and app.wasm byte for byte as with the key unset, and no app.js; an unknown part is
refused with the list; with every part there is, app.js and app_worklet.js load each one and
its files are there; a build without them again leaves nothing of them and the same bytes.
  tests/emu_parts_test.py"""
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EMU = ROOT / "template" / "build" / "emu"
PARTS = ROOT / "web" / "emu" / "parts"
FAILS = 0


def check(ok, what):
    global FAILS
    print(f"  {'ok' if ok else 'FAIL'} {what}")
    FAILS += not ok


def build(parts):
    env = {k: v for k, v in os.environ.items() if k != "FM1B_EMU"}
    if parts is not None:
        env["FM1B_EMU"] = parts
    return subprocess.run([sys.executable, "tools/build.py", "template", "--web"], cwd=ROOT, env=env,
                          capture_output=True, text=True)


def sums():
    return {f: hashlib.sha256((EMU / f).read_bytes()).hexdigest() for f in ("index.html", "worklet.js", "app.wasm")}


r = build(None)
check(r.returncode == 0, "the template builds for the web")
plain = sums()
check(not (EMU / "app.js").exists() and not (EMU / "parts").exists(), "without parts: no app.js, no parts/")
r = build("")
check(r.returncode == 0 and sums() == plain, "FM1B_EMU=\"\": the same page, worklet and app.wasm")
r = build("nosuch")
check(r.returncode != 0 and "no part 'nosuch'" in r.stdout + r.stderr, "an unknown part is refused")
names = sorted({m[1] for f in PARTS.glob("*.js") if (m := re.fullmatch(r"([a-z0-9]+)(_worklet)?\.js", f.name))} - {"parts"})
if names:
    r = build(" ".join(names))
    check(r.returncode == 0, f"every part builds: {' '.join(names)}")
    page, work = (EMU / "app.js").read_text(), (EMU / "app_worklet.js").read_text()
    for n in names:
        check((PARTS / f"{n}.js").exists() <= (f'"./parts/{n}.js"' in page)
              and (PARTS / f"{n}_worklet.js").exists() <= (f'"./parts/{n}_worklet.js"' in work)
              and all((EMU / "parts" / f.name).exists() for f in PARTS.glob(f"{n}*.js")), f"{n}: loaded and copied")
    check(sums()["index.html"] == plain["index.html"] and sums()["worklet.js"] == plain["worklet.js"],
          "the page and worklet.js themselves unchanged with parts")
    r = build(None)
    check(r.returncode == 0 and sums() == plain and not (EMU / "app.js").exists() and not (EMU / "parts").exists(),
          "built again without: the same bytes, the parts' files gone")
print("all passed" if not FAILS else f"{FAILS} failed")
sys.exit(1 if FAILS else 0)
