# SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0
# Copyright (C) 2026 Henri David (hdavid), Optimist (github.com/w0ts/optimist)
"""tools/fm1_remote.py 'wifi tune': the letters to the switches' mask device/main_fm1.c wifi_frame op 9 takes
(wifi/wifi_tune.h WT_CAL 1, WT_FILT 2, WT_AGC 4, WT_RATE 8). Run from tests/wifi/run.sh."""
import importlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
fm1_remote = importlib.import_module("fm1_remote")    # (the tool's own module, its directory on the path first)

FAILS = 0


def check(ok: bool, what: str) -> None:
    global FAILS
    if not ok:
        FAILS += 1
        print("FAIL:", what)


check(fm1_remote.WIFI_OP["tune"] == 9, "op 9")
check(fm1_remote.tune_mask("abcf") == 15, "abcf = 15")
check(fm1_remote.tune_mask("fa") == 9, "fa = 9")
check(fm1_remote.tune_mask("bb") == 2, "a letter twice counts once")
check(fm1_remote.tune_mask("none") == 0, "none = 0")
for bad in ("", "x", "abcd"):
    try:
        fm1_remote.tune_mask(bad)
        check(False, f"{bad!r} refused")
    except SystemExit:
        pass
print("remote_tune:", "ok" if not FAILS else f"{FAILS} failed")
sys.exit(FAILS)
