#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Charles Vestal
"""app.h's store places against where the store may be (tools/build.py store_values, store_errors): APP_STORE_MAP's
A/B pairs and the pool in the data area, above the app area (0x97000, or fm1.toml app_end's end), or at Felucca's
pairs (0xFC000, 0xE5000); anything else stops the build, as the store would refuse those sectors at run time. The
probe's IR read in both forms the compiler writes. Run from the repo root:
  python3 tests/store_place_test.py"""
import importlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
build = importlib.import_module("build")

failed = 0


def ok(cond, what):
    global failed
    print(("  ok   " if cond else "  FAIL ") + what)
    failed += not cond


def area(data_lo=0x97000, pool=None):
    """the probe's area: (data_lo, data_hi, pooled, base, n, old, obj0, objn)"""
    return (data_lo, 0xE0000, 1, *pool) if pool else (data_lo, 0xE0000, 0, 0, 0, 0, 0, 0)


TEMPLATE = [0xFC000, 0xDC000, 0xDE000]
ok(build.store_errors(TEMPLATE, area()) == [], "the template's map: no error")
ok(build.store_errors(TEMPLATE, area(0xCE000)) == [], "the template's map with app_end 0xCE000: no error")
ok(build.store_errors([0xE5000], area()) == [], "Felucca's AUTO pair at 0xE5000: no error")
got = build.store_errors([0xFC000, 0x90000], area())
ok(len(got) == 1 and "object 1" in got[0] and "0x90000" in got[0], f"an object inside the app area: stops ({got})")
got = build.store_errors([0xFC000, 0xA0000], area(0xCE000))
ok(len(got) == 1 and "0xa0000" in got[0] and "app_end" in got[0],
   f"an object below a bigger slot's end (0xCE000): stops, naming app_end ({got})")
ok(build.store_errors([0xCC000], area(0xCE000)) != [], "a pair that ends past app_end but starts below it: stops")
ok(build.store_errors([0xDF000], area()) != [], "a pair across the data area's end (0xE0000): stops")
ok(build.store_errors([0xDC800], area()) != [], "a pair not on a sector: stops")
ok(build.store_errors([0x95000], area()) != [], "a pair in XDATA0 (below 0x97000): stops")
# LOOM-8's: the project objects (2..22) pooled in 20 sectors from 0x97000, where 42 sectors of A/B pairs were
LOOM_MAP = [0xDC000, 0xDE000, *[0x97000 + 0x2000 * i for i in range(21)], 0xFC000]
LOOM_POOL = (0x97000, 20, 42, 2, 21)
ok(build.store_errors(LOOM_MAP, area(pool=LOOM_POOL)) == [], "LOOM-8's pool from 0x97000: no error")
got = build.store_errors(LOOM_MAP, area(0xB0000, LOOM_POOL))
ok(len(got) == 1 and "APP_STORE_POOL_BASE" in got[0],
   f"the same pool under a bigger slot's end (0xB0000): stops; the pooled objects' map entries are not used ({got})")
ok(build.store_errors(LOOM_MAP, area(pool=(0x97000, 20, 80, 2, 21))) != [],
   "a pool whose old pairs reach past the data area: stops")

# the SMK-37 (board = "smk37"): its own pairs from its flash map (felucca-bsp boards/smk37/fm1_flash.h FL_GLOB, FL_AUTO)
build.BOARD = "smk37"
SMK = (0xC2000, 0xEA000, 0, 0, 0, 0, 0, 0)
ok(build.store_errors([0xEB000, 0xC4000, 0xA1000], SMK) == [], "the SMK-37's pairs (0xEB000, 0xA1000) and its data area: no error")
got = build.store_errors([0xFC000], SMK)
ok(len(got) == 1 and "0xeb000" in got[0], f"the FM-1's 0xFC000 on the SMK-37 (its USR): stops, naming its own pairs ({got})")
build.BOARD = "fm1"
ok(build.store_errors([0xEB000], area()) != [], "the SMK-37's 0xEB000 on the FM-1: stops")

ir = ("@fm1b_probe_map = local_unnamed_addr constant [3 x i32] [i32 1032192, i32 901120, i32 909312], align 4\n"
      "@fm1b_probe_area = local_unnamed_addr constant [8 x i32] [i32 618496, i32 917504, i32 0, i32 0, i32 0, "
      "i32 0, i32 0, i32 0], align 4\n")
ok(build.store_values(ir) == (TEMPLATE, (0x97000, 0xE0000, 0, 0, 0, 0, 0, 0)), "the probe's IR read: an array")
ir = ("@fm1b_probe_map = constant <{ i32, [2 x i32] }> <{ i32 1032192, [2 x i32] zeroinitializer }>, align 4\n"
      "@fm1b_probe_area = constant <{ i32, i32, [6 x i32] }> <{ i32 618496, i32 917504, [6 x i32] zeroinitializer }>, "
      "align 4\n")
got = build.store_values(ir)
ok(got[0][-3:] == [0xFC000, 0, 0] and got[1] == (0x97000, 0xE0000, 0, 0, 0, 0, 0, 0),
   f"the probe's IR read: a packed struct ending in zeros ({got})")
ir = "@fm1b_probe_area = constant [8 x i32] zeroinitializer, align 4\n"
ok(build.store_values(ir) == (None, (0,) * 8), "no APP_STORE_MAP: none")

print("store-place: " + ("FAILED" if failed else "all passed"))
sys.exit(1 if failed else 0)
