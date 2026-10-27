// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Henri David
// The companion page's client (web/companion/companion.js) against the companion test app in the browser emulator
// (tests/companion_app, tools/build.py tests/companion_app --web: web/emu/emu_web.c web_sx_in / web_sx_out_*), under
// node: a MIDI port pair over app.wasm, as the emulator's port part gives one to a page. Checked: the capability query,
// the files (listed, a sample written and read back with its CRC-32, a write cut short and resumed, refused while the
// app plays), the parameters (described with their labels and groups, get, set clamped, a watched change told).
//   node tests/companion_web_test.mjs tests/companion_app/build/emu
import { readFileSync } from "node:fs";
import { Companion, findCompanions } from "../web/companion/companion.js";

const dir = process.argv[2] || "tests/companion_app/build/emu";
let fails = 0;
const check = (ok, what, got) => { console.log(`  ${ok ? "ok" : "FAIL"} ${what}${ok || got === undefined ? "" : ": " + JSON.stringify(got)}`); fails += !ok; };

let mem;
const wasi = new Proxy({
  clock_time_get: (id, prec, out) => { new DataView(mem.buffer).setBigUint64(out, 0n, true); return 0; },
  fd_write: (fd, iov, n, out) => { new DataView(mem.buffer).setUint32(out, 0, true); return 0; },
}, { get: (t, k) => (k in t ? t[k] : () => 0) });
const { instance } = await WebAssembly.instantiate(readFileSync(`${dir}/app.wasm`),
  { wasi_snapshot_preview1: wasi, env: new Proxy({}, { get: () => () => 0 }) });
const ex = instance.exports;
mem = ex.memory;
if (ex._initialize) ex._initialize();
ex.web_boot();
const run = (ms) => { for (let i = 0; i < Math.ceil(ms * 44.1 / 128); i++) ex.web_render(128); };

// the emulated FM-1 as a port pair: a message in through web_sx_in, a few ms of the device, its frames out
const listeners = new Set();
function drain() {
  const all = new Uint8Array(mem.buffer, ex.web_sx_out_buf(), ex.web_sx_out_len()).slice();
  ex.web_sx_out_clear();
  for (let at = 0; at < all.length;) {
    let k = at + 1;
    while (k < all.length && all[k] !== 0xf7 && all[k] !== 0xf0) k++;
    const end = k < all.length && all[k] === 0xf7 ? k + 1 : k;
    if (all[end - 1] === 0xf7) for (const f of listeners) f({ data: all.slice(at, end) });
    at = end;
  }
}
function into(bytes) {
  new Uint8Array(mem.buffer, ex.web_sx_in_buf(), bytes.length).set(bytes);
  ex.web_sx_in(bytes.length);
}
const input = { name: "CPTEST FM-1", addEventListener: (t, f) => listeners.add(f), removeEventListener: (t, f) => listeners.delete(f) };
const output = { name: "CPTEST FM-1", send: (bytes) => { into(bytes); run(3); drain(); } };
const time = (ms) => { run(ms); drain(); };

run(300);
const found = await findCompanions({ inputs: new Map([["i", input]]), outputs: new Map([["o", output]]) });
check(found.length === 1 && found[0].hello.name === "CPTEST", "the port answers the capability query", found.map((f) => f.hello));
const c = new Companion(input, output, { timeout: 200, tries: 1 });
const h = await c.hello();
check(h.identity === "FM-1_1400" && h.appVersion === "1.2" && h.extras.editor === "cptest/1", "identity, version, tags", h);
check(h.version === 2 && h.backup && h.backup.magic === "CPTB" && h.backup.version === 2 && h.backup.ext === "cpb", "the backup's format", h.backup);
check(["files", "parameters", "the app's own SysEx"].every((s) => h.services.includes(s)), "services", h.services);

const files = await c.files();
check(files.map((f) => f.name).join() === "notes.txt,sample.raw,info", "the files", files);
const info = await c.read(2);
check(info.length === 300 && info.every((v, k) => v === ((k * 7 + 3) & 255)), "info read whole, CRC-32 checked");
const sample = Uint8Array.from({ length: 20000 }, (_, k) => (k * 31 + (k >> 8)) & 255);
let seen = 0;
await c.write(1, sample, (at) => { seen = at; });
check(seen === sample.length, "a sample written, progress to the end", seen);
const back = await c.read(1);
check(back.length === sample.length && back.every((v, k) => v === sample[k]), "and read back");
const other = sample.map((v) => v ^ 0x5a);
await c.write(1, other, null, { stopAfter: 6000 });
const resumed = await c.write(1, other);
check(resumed >= 6000 && resumed < other.length, "a write cut short, resumed where it stopped", resumed);
check((await c.read(1)).every((v, k) => v === other[k]), "the resumed one read back");
into(Uint8Array.from([0xf0, 0x7d, 0x43, 0x50, 0x02, 0xf7]));        // (the app plays)
time(5);
let code = null;
try { await c.write(1, sample); } catch (e) { code = e.code; }
check(code === "busy", "playing: refused, busy", code);
into(Uint8Array.from([0xf0, 0x7d, 0x43, 0x50, 0x03, 0xf7]));
time(5);

const ps = await c.params();
check(ps.map((p) => p.name).join() === "TEMPO,SWING,WAVE,CUTOFF,LOAD,MODE", "the parameters", ps.map((p) => p.name));
check(ps[2].labels.join() === "SAW,SQR,TRI,SIN" && ps[5].labels.length === 40 && ps[5].labels[39] === "MODE 39",
  "their labels", [ps[2].labels, ps[5].labels.length]);
check(ps[0].group === "GLOBAL" && ps[2].group === "SOUND" && ps[4].group === "INFO" && ps[4].flags === 1 &&
  ps[0].unit === "BPM" && ps[5].min === -20, "groups, units, ranges, read only", ps[0]);
check((await c.set(10, 500)) === 240, "set clamped");
const vals = await c.get([10, 21, 30]);
check(vals.get(10) === 240 && vals.get(21) === 100 && vals.get(30) === 37, "get", [...vals]);
const told = [];
c.onchange = (id, v) => told.push([id, v]);
check(await c.watch(true), "watch on");
into(Uint8Array.from([0xf0, 0x7d, 0x43, 0x50, 0x04, 0x30, 0xf7]));  // (the app's TEMPO: 40 + 0x30)
time(120);
check(told.some(([id, v]) => id === 10 && v === 88), "a change on the FM-1: told", told);
c.close();
console.log(fails ? `companion page: ${fails} FAILED` : "companion page: all ok");
process.exit(fails ? 1 : 0);
