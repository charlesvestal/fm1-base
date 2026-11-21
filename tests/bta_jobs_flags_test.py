#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Henri David
"""fm1.toml bt_audio_jobs as tools/build.py reads it (bta_jobs_flags): off gives no flag; on needs cpu2 and stops the
build, with a message, without it and with cpu2_fallback (a second core retired in the middle of a job would leave its
frame in flight, and the feed would wait for it in the audio interrupt). Run from the repo root:
  python3 tests/bta_jobs_flags_test.py"""
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


def flags(conf):
    """-> the flags for the fm1.toml settings conf, or the build's stop message"""
    build.CONF = conf
    try:
        return build.bta_jobs_flags()
    except SystemExit as e:
        return f"stop: {e}"


ok(flags({}) == [], "no bt_audio_jobs: no flag")
ok(flags({"cpu2": True, "cpu2_fallback": True}) == [], "bt_audio_jobs off: cpu2 and cpu2_fallback leave the build as it was")
ok(flags({"bt_audio_jobs": True, "cpu2": True}) == ["-DFM1B_BTA_JOBS=1"], "bt_audio_jobs with cpu2: FM1B_BTA_JOBS=1")
got = flags({"bt_audio_jobs": True})
ok(isinstance(got, str) and "needs cpu2 = true" in got, f"bt_audio_jobs without cpu2: the build stops and says why ({got})")
got = flags({"bt_audio_jobs": True, "cpu2": True, "cpu2_fallback": True})
ok(isinstance(got, str) and "cpu2_fallback" in got, f"bt_audio_jobs with cpu2_fallback: the build stops and says why ({got})")
ok(flags({"bt_audio_jobs": True, "cpu2": True, "cpu2_fallback": False}) == ["-DFM1B_BTA_JOBS=1"], "cpu2_fallback = false passes")

print("bta-jobs-flags: " + ("FAILED" if failed else "all passed"))
sys.exit(1 if failed else 0)
