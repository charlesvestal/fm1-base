#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Henri David
"""The USB serial number's digest of the USB audio configuration, as tools/build.py makes it (usb_serial_mix,
usb_serial_flags): a firmware with none of the options set (every one at its default) gets no digest and the old serial
flags, so its serial is today's; one with any set gets 24 hex digits that are the same for the same configuration and
id_prefix, whatever the order of the keys, and differ for another. Run from the repo root:
  python3 tests/usb_serial_test.py"""
import importlib
import re
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


def mix(conf):
    return build.usb_serial_mix(conf)


def flags(conf):
    build.CONF = conf
    return build.usb_serial_flags()


# no option on: no digest (the serial is today's)
ok(mix({}) is None, "no options: no digest")
ok(mix({"id_prefix": "FM-1_0", "usb_serial": True}) is None, "id_prefix and usb_serial alone: no digest")
ok(mix({"usb_audio_in": True, "usb_audio_out": True, "usb_audio_first": False, "usb_iad": False,
        "usb_midi_last": True, "usb_audio_channels": 2}) is None, "every option at its default, set or not: no digest")
ok(mix({"usb_audio_split": False, "usb_audio_out_feedback": False, "usb_audio_out_24bit": False,
        "usb_audio_in_host_clock": False}) is None, "the off options set to false: no digest")
ok(mix({"usb_audio_split": True, "usb_midi_last": False}) is not None, "split with its own default order: a digest (split is on)")
ok(mix({"usb_audio_mem": "app"}) is None, "usb_audio_mem (no descriptor changes): no digest")
ok(mix({"usb_manufacturer": "x", "usb_product": "y", "usb_console": True}) is None, "other USB keys: no digest")

# any option on: 24 hex digits, stable
each = {"usb_audio_in": False, "usb_audio_out": False, "usb_audio_first": True, "usb_midi_last": False, "usb_iad": True,
        "usb_audio_channels": 10, "usb_audio_in_channels": 4, "usb_audio_in_host_clock": True,
        "usb_audio_out_feedback": True, "usb_audio_out_24bit": True, "usb_audio_split": True}
seen = {}
for key, value in each.items():
    d = mix({key: value})
    ok(d is not None and re.fullmatch(r"[0-9A-F]{24}", d) is not None, f"{key} = {value!r}: 24 hex digits ({d})")
    seen[key] = d
ok(len(set(seen.values())) == len(seen), "every option alone: its own digest")
ok(mix({"usb_midi_last": 2}) not in (None, seen["usb_midi_last"]), "usb_midi_last = 2 differs from false")
ok(mix({"usb_audio_in_channels": 2}) not in (None, seen["usb_audio_in_channels"]), "2 channels differ from 4")
ok(mix({"usb_audio_in_channels": 8}) != mix({"usb_audio_in_channels": 4}), "8 channels differ from 4")

a = {"usb_audio_in_channels": 4, "usb_audio_out_feedback": True, "id_prefix": "FM-1_0"}
b = {"id_prefix": "FM-1_0", "usb_audio_out_feedback": True, "usb_audio_in_channels": 4}
ok(mix(a) == mix(b) == mix(dict(a)), "the same configuration: the same digest, whatever the key order")
ok(mix({**a, "usb_audio_out_feedback": False}) != mix(a), "two configurations: two digests")
ok(mix({**a, "usb_audio_in_channels": 8}) != mix(a), "another channel count: another digest")
ok(mix({**a, "id_prefix": "FM-1_7"}) != mix(a), "another id_prefix: another digest")
ok(mix({"usb_audio_out_feedback": True}) == mix({"usb_audio_out_feedback": True, "id_prefix": "FM-1_0"}),
   "no id_prefix is the default FM-1_0")
ok(mix({**a, "usb_audio_mem": "app", "usb_product": "z"}) == mix(a), "keys that change no descriptor leave it")
ok(mix({"usb_audio_split": True, "usb_audio_in_name": "A In"}) != mix({"usb_audio_split": True}), "a name changes it")
ok(mix({"usb_audio_split": True, "usb_audio_in_name": "A In"}) != mix({"usb_audio_split": True, "usb_audio_out_name": "A In"}),
   "the in name and the out name are not the same")
ok(mix({"usb_audio_split": True, "usb_audio_in_channel_names": ["a", "b"]})
   != mix({"usb_audio_split": True, "usb_audio_in_channel_names": ["a", "c"]}), "the channel names change it")
ok(mix({"usb_audio_split": True}) == mix({"usb_audio_split": 1}), "true and 1 are the same setting")
ok(mix({"usb_audio_in_channels": 4}) == "".join(re.findall("[0-9A-F]", mix({"usb_audio_in_channels": 4}))),
   "the digest is plain uppercase hex")
# a build is the same twice and across runs: one configuration, one digest, pinned
ok(mix({"id_prefix": "FM-1_0", "usb_audio_in_channels": 4, "usb_audio_out_feedback": True}) == "A9D5F67D730C2C3F57922BCA",
   "a configuration's digest is pinned (stable between builds)")

# the flags: usb_serial off none; on without options exactly the old one; on with options also the digest
ok(flags({}) == [], "no usb_serial: no serial flag")
ok(flags({"usb_audio_in_channels": 4}) == [], "options without usb_serial: no serial flag (the bsp has no serial then)")
ok(flags({"usb_serial": True}) == ["-DFELUCCA_USB_SERIAL_FN=fm1_usb_serial"],
   "usb_serial with no option on: the old flag alone (today's serial)")
ok(flags({"usb_serial": True, "usb_audio_mem": "app"}) == ["-DFELUCCA_USB_SERIAL_FN=fm1_usb_serial"],
   "usb_serial, usb_audio_mem only: the old flag alone")
f = flags({"usb_serial": True, "usb_audio_in_channels": 4})
d = mix({"usb_serial": True, "usb_audio_in_channels": 4})
ok(f == ["-DFELUCCA_USB_SERIAL_FN=fm1_usb_serial", f'-DFM1B_USB_SERIAL_MIX="{d}"'],
   "usb_serial with an option on: the old flag and the digest")
ok(f[1] != flags({"usb_serial": True, "usb_audio_in_channels": 8})[1], "two configurations: two digest flags")

print("usb-serial: " + ("FAILED" if failed else "all passed"))
sys.exit(1 if failed else 0)
