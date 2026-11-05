#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Charles Vestal
"""tools/firmware_json.py: firmware.json for a few fm1.toml files (no Bluetooth; every part; a key left out; an
unknown tag or a long description refused), the keys a release needs (refused without them, a dev build warned),
where a build writes it, the flash parts against device/fm1_flash_ok.h,
and the vocabularies the same in the code, docs/firmware.schema.json and docs/FIRMWARE-JSON.md. The output is
checked against the schema when jsonschema is there.
  python3 tests/firmware_json_test.py"""
import contextlib
import io
import json
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import firmware_json as F  # noqa: E402
import build as B  # noqa: E402

SCHEMA = json.loads((ROOT / "docs" / "firmware.schema.json").read_text())
DOC = (ROOT / "docs" / "FIRMWARE-JSON.md").read_text()
try:
    import jsonschema
except ImportError:
    jsonschema = None
failed = 0


def ok(cond, what):
    global failed
    print(f"{what:<72} {'ok' if cond else 'FAIL'}")
    failed += not cond


def refused(fn, words):
    try:
        fn()
    except SystemExit as e:
        return words in str(e)
    return False


def valid(d):
    if jsonschema is None:
        return True
    try:
        jsonschema.validate(d, SCHEMA)
        return True
    except jsonschema.ValidationError as e:
        print("   ", e.message)
        return False


def conf_of(text):
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp) / "fm1.toml"
        p.write_text(text)
        return B.read_conf(p)


# the flash parts: what fm1_flash_ok.h accepts
h = (ROOT / "device" / "fm1_flash_ok.h").read_text()
want = sorted(f"{int(x, 16):06X}" for x in re.findall(r"0x([0-9A-Fa-f]{6})u", h))
ok(F.flash_ids() == ["856014", "B36014"] and sorted(F.flash_ids()) == want, "flash parts: fm1_flash_ok.h's (856014, B36014)")

# no Bluetooth, USB audio off, a key missing: left out, nothing invented
c = conf_of('name = "Quiet"\nusb_manufacturer = "QUIET-1"\nusb_audio_in = false\nlicence = "MPL-2.0"\ncredits = ""\n')
have = F.parts(c, ble=False, own=False, wifi=False, cpu2=False, touch=False, words=False, env={})
d = F.make(c, "quiet", F.display_name(c, ROOT), have)
ok(d["hardware"]["features"] == [], "no Bluetooth, USB audio off: no features")
ok(list(d) == ["schema", "id", "name", "license", "hardware"], "missing keys (and an empty credits) left out")
ok(d["name"] == "QUIET-1" and d["id"] == "quiet" and d["schema"] == 1, "name from usb_manufacturer, id from name")
ok(valid(d), "  validates against the schema" + (" (skipped: no jsonschema)" if jsonschema is None else ""))

# every part, and the tags; the keys in order
c = conf_of('''name = "Full"
usb_manufacturer = "FULL-1"
ble = true
ble_stack = "own"
ble_central = true
bt_audio = true
usb_host = true
msc = true
cpu2 = true
mirror = true
backup = true
resources = "res.bin"
author = "Someone Else"
licence = "MPL-2.0"
tagline = "Everything at once"
description = "A test firmware with every part, café included."
tags = ["synth", "sequencer",
        "experimental"]
install = "https://example.org/full/install/"
site = "https://example.org/full/"
repo = "https://github.com/example/full"
credits = "Built on fm1-base."
''')
have = F.parts(c, ble=True, own=True, wifi=True, cpu2=True, touch=True, words=True, env={})
d = F.make(c, "full", F.display_name(c, ROOT), have)
ok(d["hardware"]["features"] == list(F.FEATURE_NAMES), "every part: every feature, in the vocabulary's order")
ok(list(d) == list(F.KEYS), "every key, in the file's order")
ok(d["tags"] == ["synth", "sequencer", "experimental"] and d["source"] == "https://github.com/example/full",
   "tags (a list over lines), source from repo")
text = F.dumps(d)
ok(text.endswith("}\n") and "café" in text and '\n  "id": "full",' in text, "2-space indent, UTF-8, a trailing newline")
ok('\n  "tags": ["synth", "sequencer", "experimental"],\n' in text and '\n    "models": ["FM-1"],\n' in text,
   "a list of strings on one line")
ok(json.loads(text) == d and json.loads(F.dumps({"a": [{"b": []}], "c": {}})) == {"a": [{"b": []}], "c": {}},
   "reads back as it was")
ok(valid(d), "  validates against the schema" + (" (skipped: no jsonschema)" if jsonschema is None else ""))

# the environment's overrides and the dependent parts, as the build reads them
have = F.parts(c, ble=False, own=True, wifi=False, cpu2=False, touch=False, words=False,
               env={"FM1B_MIRROR": "0", "FM1B_UAC": "0"})
ok(not have["bluetooth-midi"] and not have["bluetooth-midi-devices"] and not have["bluetooth-audio"],
   "--no-ble: no Bluetooth part")
ok(not have["screen-mirror"] and not have["usb-audio-in"] and not have["usb-audio-out"],
   "FM1B_MIRROR=0, FM1B_UAC=0: without (and no USB audio out without in)")
have = F.parts({"ble": True, "bt_audio": True}, ble=True, own=False, wifi=False, cpu2=False, touch=False, words=False,
               env={})
ok(not have["bluetooth-audio"] and have["usb-audio-in"] and have["usb-audio-out"],
   "bt_audio only on the own stack; USB audio on by default")

# refused
base = {"name": "X"}
ok(refused(lambda: F.make({**base, "tags": ["synth", "techno"]}, "x", "X", {}), "'techno' not in the vocabulary"),
   "an unknown tag refused, by name")
ok(refused(lambda: F.make({**base, "tags": ["synth", "synth"]}, "x", "X", {}), "twice"), "a tag twice refused")
ok(refused(lambda: F.make({**base, "description": "x" * 601}, "x", "X", {}), "at most 600"),
   "a description over 600 characters refused")
ok(not refused(lambda: F.make({**base, "description": "x" * 600}, "x", "X", {}), "600"),
   "one of 600 taken")
ok(refused(lambda: F.make({**base, "install": "http://example.org/"}, "x", "X", {}), "https"),
   "an install page not https refused")

# what a release needs: refused without, by name; any other build warned on one line
full = {k: "x" for k in F.REQUIRED} | {"tags": ["synth"]}


def warned(conf):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        gone = F.require(conf, False)
    return gone, out.getvalue()


ok(F.REQUIRED == ("author", "licence", "tagline", "description", "tags", "install", "repo"), "the keys a release needs")
ok(F.missing(full) == [] and F.require(full, True) == [], "a release with all of them: taken")
ok(F.missing({**full, "author": "", "authors": ["A", "B"]}) == [], "  authors (the .rfm1's) for author")
lacking = {k: v for k, v in full.items() if k not in ("tags", "install")} | {"description": ""}
ok(F.missing(lacking) == ["description", "tags", "install"], "left out or empty: missing, in order")
ok(refused(lambda: F.require(lacking, True), "a release needs description, tags, install") and
   refused(lambda: F.require(lacking, True), "FIRMWARE-JSON.md"), "a release without them refused, by name and doc")
gone, said = warned(lacking)
ok(gone == ["description", "tags", "install"] and said.count("\n") == 1 and "warning:" in said and
   "description, tags, install" in said, "a dev build: one warning line naming them, and on")
ok(warned(full) == ([], ""), "a dev build with all of them: no warning")
ok(warned({"name": "X"})[0] == list(F.REQUIRED), "  an fm1.toml with none: every one named")
tmpl = B.read_conf(ROOT / "template" / "fm1.toml")
ok(F.missing(tmpl) == [], "the template's fm1.toml: everything a release needs")
ok(" and ".join([", ".join(f"`{k}`" + (" (or `authors`)" if k == "author" else "") for k in F.REQUIRED[:-1]),
                 f"`{F.REQUIRED[-1]}`"]) + "." in DOC, "doc: the keys a release needs")

# the build: --release refuses before anything is built; --host, --web and --rfm1 do not ask
with tempfile.TemporaryDirectory() as tmp:
    app = Path(tmp) / "app"
    app.mkdir()
    (app / "fm1.toml").write_text('name = "Bare"\nid_prefix = "FM-1_99"\nlicence = "MPL-2.0"\n')
    argv = sys.argv
    try:
        sys.argv = ["build.py", str(app), "--release", "1.0"]
        ok(refused(B.main, "a release needs author, tagline, description, tags, install, repo") and
           not (app / "build").exists(), "build.py --release: refused, nothing written")
    finally:
        sys.argv = argv

# where it is written
with tempfile.TemporaryDirectory() as tmp:
    repo = Path(tmp) / "fm1-thing"
    (repo / "app").mkdir(parents=True)
    t = F.targets(repo / "app")
    ok(t == [(repo / "app" / "build" / "firmware.json").resolve(), (repo / "firmware.json").resolve()],
       "an app in a firmware's repository: build/ and the repository's root")
    ok(F.targets(repo / "app", no_ble=True) == [(repo / "app" / "build" / "firmware-usb.json").resolve()],
       "--no-ble: build/firmware-usb.json only")
    ok(F.targets(ROOT / "template") == [ROOT / "template" / "build" / "firmware.json"],
       "the template: template/build/ only, never fm1-base's root")
    f = repo / "firmware.json"
    ok(F.write(f, "a\n") and not F.write(f, "a\n") and F.write(f, "b\n"), "written only when changed")

# the vocabularies: the code's, the schema's, the doc's
props = SCHEMA["properties"]
ok(tuple(props["tags"]["items"]["enum"]) == F.TAGS, "schema: the tag vocabulary")
ok(tuple(props["hardware"]["properties"]["features"]["items"]["enum"]) == F.FEATURE_NAMES,
   "schema: the feature vocabulary")
ok(", ".join(F.TAGS) + "." in " ".join(DOC.split()), "doc: the tag vocabulary")
feats_doc = DOC[DOC.index("## Features"):DOC.index("## Rules")]
ok(re.findall(r"^\| `([a-z0-9-]+)` \|", feats_doc, re.M) == list(F.FEATURE_NAMES), "doc: the feature vocabulary")

print("all ok" if not failed else f"{failed} FAILED")
sys.exit(1 if failed else 0)
