// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Charles Vestal
// Zip files for the companion page's "Back up all" and its restore, with nothing to load: zip() writes the entries
// stored as they are (no compression: an FM-1's files are small, and most are packed already), unzip() reads a zip
// back, its entries stored or deflated (another tool may have zipped it again: deflated ones through the browser's
// own DecompressionStream). Names in UTF-8. CRC-32 is zlib's (companion.js).
//   const bytes = zip([{ name: "files/song.bin", data: Uint8Array }], new Date());
//   const entries = await unzip(bytes);       // [{ name, data, crc }], the folders left out
import { crc32 } from "./companion.js";

const enc = new TextEncoder();
const u16 = (v) => [v & 0xff, v >>> 8 & 0xff];
const u32 = (v) => [v & 0xff, v >>> 8 & 0xff, v >>> 16 & 0xff, v >>> 24 & 0xff];

function dosTime(d) {                           // -> [time, date] as zip keeps them (2-second steps, from 1980)
  const y = Math.max(1980, d.getFullYear());
  return [d.getHours() << 11 | d.getMinutes() << 5 | d.getSeconds() >> 1,
    (y - 1980) << 9 | (d.getMonth() + 1) << 5 | d.getDate()];
}

export function zip(entries, when = new Date()) {
  const [time, date] = dosTime(when);
  const parts = [], central = [];
  let at = 0;
  for (const { name, data } of entries) {
    const n = enc.encode(name), crc = crc32(data);
    if (data.length > 0xfffffffe || at > 0xfffffffe) throw new Error("zip: too big for a zip without zip64");
    // version 10, flag bit 11 (UTF-8 names), method 0 (stored)
    const common = [...u16(0x0800), ...u16(0), ...u16(time), ...u16(date), ...u32(crc), ...u32(data.length),
      ...u32(data.length), ...u16(n.length), ...u16(0)];
    const local = Uint8Array.from([...u32(0x04034b50), ...u16(10), ...common]);
    central.push(Uint8Array.from([...u32(0x02014b50), ...u16(10), ...u16(10), ...common, ...u16(0), ...u16(0),
      ...u16(0), ...u32(0), ...u32(at)]), n);
    parts.push(local, n, data);
    at += local.length + n.length + data.length;
  }
  const cdSize = central.reduce((s, p) => s + p.length, 0);
  const end = Uint8Array.from([...u32(0x06054b50), ...u16(0), ...u16(0), ...u16(entries.length),
    ...u16(entries.length), ...u32(cdSize), ...u32(at), ...u16(0)]);
  const out = new Uint8Array(at + cdSize + end.length);
  let o = 0;
  for (const p of [...parts, ...central, end]) { out.set(p, o); o += p.length; }
  return out;
}

async function inflate(data) {                  // a deflated entry, by the browser's own decompressor
  if (typeof DecompressionStream === "undefined") throw new Error("this browser cannot read compressed zips");
  const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

export async function unzip(bytes) {
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let e = bytes.length - 22;
  while (e >= 0 && v.getUint32(e, true) !== 0x06054b50) e--;   // (the end record, after any comment)
  if (e < 0) throw new Error("not a zip file");
  const count = v.getUint16(e + 10, true);
  let c = v.getUint32(e + 16, true);
  const out = [], dec = new TextDecoder();
  for (let k = 0; k < count; k++) {
    if (c + 46 > bytes.length || v.getUint32(c, true) !== 0x02014b50) throw new Error("a damaged zip file");
    const method = v.getUint16(c + 10, true), crc = v.getUint32(c + 16, true), packed = v.getUint32(c + 20, true);
    const size = v.getUint32(c + 24, true), nl = v.getUint16(c + 28, true), xl = v.getUint16(c + 30, true);
    const cl = v.getUint16(c + 32, true), lo = v.getUint32(c + 42, true);
    const name = dec.decode(bytes.subarray(c + 46, c + 46 + nl));
    c += 46 + nl + xl + cl;
    if (name.endsWith("/")) continue;           // (a folder)
    if (lo + 30 > bytes.length || v.getUint32(lo, true) !== 0x04034b50) throw new Error(`a damaged zip file (${name})`);
    const start = lo + 30 + v.getUint16(lo + 26, true) + v.getUint16(lo + 28, true);
    if (start + packed > bytes.length) throw new Error(`a damaged zip file (${name})`);
    const raw = bytes.subarray(start, start + packed);
    if (method !== 0 && method !== 8) throw new Error(`${name}: compressed in a way this page cannot read (method ${method})`);
    const data = method === 0 ? raw.slice() : await inflate(raw);
    if (data.length !== size || crc32(data) !== crc) throw new Error(`${name} is damaged in the zip file`);
    out.push({ name, data, crc });
  }
  return out;
}
