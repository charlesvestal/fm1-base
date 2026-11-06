#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Henri David
"""app.h's APP_RES_BASE as tools/build.py reads it (res_base): a region at or above the app area's end passes (0x93000,
or fm1.toml app_end's); one below it stops the build with a message, as the update loader refuses to write that region
at run time (felucca-bsp ldr_res: base < LDR_DATA_LO) and the app would ship without its resources. Run from the repo
root:
  python3 tests/res_base_test.py"""
import importlib
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
build = importlib.import_module("build")

failed = 0


def ok(cond, what):
    global failed
    print(("  ok   " if cond else "  FAIL ") + what)
    failed += not cond


def base(res, end=None):
    """-> res_base() for an app.h with APP_RES_BASE res and fm1.toml app_end end (None: not set), or the stop message"""
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "app.h").write_text(f"#define APP_RES_BASE {res:#x}u\n")
        build.APP = Path(tmp)
        build.CONF = {} if end is None else {"app_end": end}
        try:
            return build.res_base()
        except SystemExit as e:
            return f"stop: {e}"


ok(base(0xAB000) == 0xAB000, "no app_end, a region at 0xAB000 (above 0x93000): passes")
ok(base(0x93000) == 0x93000, "no app_end, a region at the normal end 0x93000: passes")
got = base(0x90000)
ok(isinstance(got, str) and "APP_RES_BASE" in got and "0x93000" in got,
   f"no app_end, a region at 0x90000 (inside the app area): the build stops and says why ({got})")
ok(base(0xD0000, 0xCE000) == 0xD0000, "app_end 0xCE000, a region at 0xD0000: passes")
ok(base(0xCE000, 0xCE000) == 0xCE000, "app_end 0xCE000, a region at 0xCE000 (the end itself): passes")
got = base(0xAB000, 0xCE000)
ok(isinstance(got, str) and "APP_RES_BASE" in got and "0xce000" in got and "app_end" in got,
   f"app_end 0xCE000, a region at 0xAB000 (inside the bigger slot): the build stops and says why ({got})")

print("res-base: " + ("FAILED" if failed else "all passed"))
sys.exit(1 if failed else 0)
