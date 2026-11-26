#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Henri David
# Copyright (C) 2026 Charles Vestal
"""app.h's APP_RES_BASE as tools/build.py reads it (res_base): a region at or above the data area's start passes
(0x97000, or fm1.toml app_end's end above it); one below it stops the build with a message, as the update loader
refuses to write that region at run time (felucca-bsp ldr_res: base < LDR_DATA_LO; fm1pkg_make.py below 0x97000) and
the app would ship without its resources. Then app_end's own checks (tools/build.py app_end): a 4 KiB boundary in
[0x93000, 0xE0000], app_clear_end above it and only with it. Run from the repo root:
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
ok(base(0x97000) == 0x97000, "no app_end, a region at the data area's start 0x97000: passes")
got = base(0x93000)
ok(isinstance(got, str) and "APP_RES_BASE" in got and "0x97000" in got,
   f"no app_end, a region at 0x93000 (XDATA0, which the loader and the packer refuse): stops ({got})")
got = base(0x90000)
ok(isinstance(got, str) and "APP_RES_BASE" in got and "0x97000" in got,
   f"no app_end, a region at 0x90000 (inside the app area): the build stops and says why ({got})")
got = base(0x94000, 0x95000)
ok(isinstance(got, str) and "0x97000" in got, f"app_end 0x95000 (below 0x97000), a region at 0x94000: stops ({got})")
ok(base(0xD0000, 0xCE000) == 0xD0000, "app_end 0xCE000, a region at 0xD0000: passes")
ok(base(0xCE000, 0xCE000) == 0xCE000, "app_end 0xCE000, a region at 0xCE000 (the end itself): passes")
got = base(0xAB000, 0xCE000)
ok(isinstance(got, str) and "APP_RES_BASE" in got and "0xce000" in got and "app_end" in got,
   f"app_end 0xCE000, a region at 0xAB000 (inside the bigger slot): the build stops and says why ({got})")



def end(conf, env=None):
    """-> app_end() for fm1.toml conf (and FM1B_APP_END env), or the stop message"""
    build.CONF = conf
    old = build.os.environ.pop("FM1B_APP_END", None)
    if env:
        build.os.environ["FM1B_APP_END"] = env
    try:
        return build.app_end()
    except SystemExit as e:
        return f"stop: {e}"
    finally:
        build.os.environ.pop("FM1B_APP_END", None)
        if old is not None:
            build.os.environ["FM1B_APP_END"] = old


ok(end({}) is None, "no app_end: none (0x93000)")
ok(end({"app_end": 0xCE000, "app_clear_end": 0xDA000}) == 0xCE000, "app_end 0xCE000, app_clear_end 0xDA000: passes")
ok(end({}, "0xB0000") == 0xB0000, "FM1B_APP_END=0xB0000 in the environment: passes")
ok(str(end({"app_end": 0xCE800})).startswith("stop"), "app_end off a 4 KiB boundary: stops")
ok(str(end({"app_end": 0x90000})).startswith("stop"), "app_end below 0x93000: stops")
ok(str(end({"app_end": 0xCE000, "app_clear_end": 0xCE000})).startswith("stop"), "app_clear_end at app_end: stops")
got = end({"app_clear_end": 0xDA000})
ok(isinstance(got, str) and "app_clear_end" in got and "app_end" in got,
   f"app_clear_end without app_end: stops ({got})")


def steps(conf):
    """-> app_steps() for fm1.toml conf, or the stop message"""
    build.CONF = conf
    old = build.os.environ.pop("FM1B_APP_END", None)
    try:
        return build.app_steps()
    except SystemExit as e:
        return f"stop: {e}"
    finally:
        if old is not None:
            build.os.environ["FM1B_APP_END"] = old


ROWS = [[0x93000, 0, 0x93000], [0xB5000, 3, 0xD8000], [0xCE000, 1, 0xDA000]]
ok(steps({}) == [], "no app_steps: none")
ok(steps({"app_end": 0xB5000, "app_steps": ROWS, "app_clear_top": 0xDA000}) == [tuple(r) for r in ROWS],
   "app_end 0xB5000 one of the steps' ends, app_clear_top 0xDA000: passes")
got = steps({"app_steps": ROWS, "app_clear_top": 0xDA000})
ok(isinstance(got, str) and "app_steps" in got and "app_end" in got, f"app_steps without app_end: stops ({got})")
got = steps({"app_end": 0xA0000, "app_steps": ROWS, "app_clear_top": 0xDA000})
ok(isinstance(got, str) and "none of app_steps' ends" in got, f"app_end 0xA0000, none of the steps' ends: stops ({got})")
got = steps({"app_end": 0xB5000, "app_steps": ROWS})
ok(isinstance(got, str) and "app_clear_top" in got, f"app_steps without app_clear_top: stops ({got})")

print("res-base: " + ("FAILED" if failed else "all passed"))
sys.exit(1 if failed else 0)
