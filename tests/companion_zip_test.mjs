// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Charles Vestal
// The companion page's zip files (web/companion/zip.js) under node: a backup as "Back up all" makes it (stored, a
// UTF-8 name, an empty file, a folder) read back by unzip() and by Python's zipfile (another tool's reader); a zip
// Python wrote deflated read by unzip(); a damaged one refused.
//   node tests/companion_zip_test.mjs OUTDIR
import { writeFileSync, readFileSync, mkdirSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { zip, unzip } from "../web/companion/zip.js";
import { crc32 } from "../web/companion/companion.js";

const out = process.argv[2] || "/tmp";
const py = process.env.PY || "python3";
mkdirSync(out, { recursive: true });
let fails = 0;
const check = (ok, what, got) => { console.log(`  ${ok ? "ok" : "FAIL"} ${what}${ok || got === undefined ? "" : ": " + JSON.stringify(got)}`); fails += !ok; };
const same = (a, b) => a.length === b.length && a.every((v, k) => v === b[k]);

const big = Uint8Array.from({ length: 70000 }, (_, k) => (k * 7 + (k >> 9)) & 255);
const entries = [
  { name: "fm1-companion.json", data: new TextEncoder().encode('{"name": "TEMPLATE"}\n') },
  { name: "files/settings", data: Uint8Array.from([0x31, 0x4c, 0x50, 0x54, 1, 0, 0, 0]) },
  { name: "files/empty.bin", data: new Uint8Array(0) },
  { name: "files/chanson d’été.txt", data: new TextEncoder().encode("la la la\n") },
  { name: "files/sample.raw", data: big },
];
const z = zip(entries, new Date(2026, 10, 18, 12, 34, 56));
const back = await unzip(z);
check(back.length === entries.length && back.every((e, k) => e.name === entries[k].name && same(e.data, entries[k].data) &&
  e.crc === crc32(entries[k].data)), "zip, then unzip: the same names, bytes and CRC-32s", back.map((e) => e.name));
writeFileSync(`${out}/backup.zip`, z);
const listed = execFileSync(py, ["-c", `
import sys, zipfile, zlib
z = zipfile.ZipFile(sys.argv[1])
assert z.testzip() is None
for i in z.infolist():
    print(i.filename, i.file_size, i.compress_type, "%08x" % zlib.crc32(z.read(i)), i.date_time)
`, `${out}/backup.zip`], { encoding: "utf8" });
const lines = listed.trim().split("\n");
check(lines.length === 5 && lines[3].startsWith("files/chanson d’été.txt 9 0 ") && lines[4].startsWith("files/sample.raw 70000 0 ") &&
  lines[0].endsWith("(2026, 11, 18, 12, 34, 56)"), "Python's zipfile reads it: names, sizes, stored, the date", lines);

execFileSync(py, ["-c", `
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("files/", "")
    z.writestr("files/sample.raw", open(sys.argv[2], "rb").read()[30:])
    z.writestr("files/settings", b"TPL1")
`, `${out}/deflated.zip`, `${out}/backup.zip`]);
const d = await unzip(new Uint8Array(readFileSync(`${out}/deflated.zip`)));
check(d.length === 2 && d[1].name === "files/settings" && new TextDecoder().decode(d[1].data) === "TPL1" &&
  d[0].data.length === z.length - 30, "a deflated zip from another tool read, its folder left out", d.map((e) => [e.name, e.data.length]));

const bad = z.slice();
bad[1000] ^= 0xff;                                // (a byte of the stored sample: its CRC-32 differs)
let err = null;
try { await unzip(bad); } catch (e) { err = e.message; }
check(err !== null, "a damaged zip refused", err);
err = null;
try { await unzip(new Uint8Array(100)); } catch (e) { err = e.message; }
check(err === "not a zip file", "not a zip at all", err);
console.log(fails ? `companion zip: ${fails} FAILED` : "companion zip: all ok");
process.exit(fails ? 1 : 0);
