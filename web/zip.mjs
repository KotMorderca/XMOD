// SPDX-License-Identifier: GPL-3.0-only
// A plain .zip (files stored, not compressed) for the song backups, and a reader for those (and
// any zip with stored entries).
import { crc32 } from "./fm1serial.mjs";

const enc = new TextEncoder(), dec = new TextDecoder();

export function zipFiles(files) {                  // [{name, data: Uint8Array}] -> Uint8Array
  const parts = [], central = [];
  let off = 0;
  for (const f of files) {
    const name = enc.encode(f.name), crc = crc32(f.data), n = f.data.length;
    const h = new DataView(new ArrayBuffer(30));
    h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true); h.setUint16(6, 0x0800, true);
    h.setUint16(8, 0, true); h.setUint32(14, crc, true); h.setUint32(18, n, true); h.setUint32(22, n, true);
    h.setUint16(26, name.length, true);
    parts.push(new Uint8Array(h.buffer), name, f.data);
    const c = new DataView(new ArrayBuffer(46));
    c.setUint32(0, 0x02014b50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true); c.setUint16(8, 0x0800, true);
    c.setUint32(16, crc, true); c.setUint32(20, n, true); c.setUint32(24, n, true); c.setUint16(28, name.length, true);
    c.setUint32(42, off, true);
    central.push(new Uint8Array(c.buffer), name);
    off += 30 + name.length + n;
  }
  const cdSize = central.reduce((s, p) => s + p.length, 0);
  const e = new DataView(new ArrayBuffer(22));
  e.setUint32(0, 0x06054b50, true); e.setUint16(8, files.length, true); e.setUint16(10, files.length, true);
  e.setUint32(12, cdSize, true); e.setUint32(16, off, true);
  const all = [...parts, ...central, new Uint8Array(e.buffer)];
  const out = new Uint8Array(all.reduce((s, p) => s + p.length, 0));
  let p = 0;
  for (const a of all) { out.set(a, p); p += a.length; }
  return out;
}

export function unzipFiles(zip) {                  // -> [{name, data}] (stored entries only)
  const v = new DataView(zip.buffer, zip.byteOffset, zip.byteLength);
  let e = zip.length - 22;
  while (e >= 0 && v.getUint32(e, true) !== 0x06054b50) e--;
  if (e < 0) throw new Error("not a zip file");
  const count = v.getUint16(e + 10, true);
  let c = v.getUint32(e + 16, true);
  const out = [];
  for (let i = 0; i < count; i++) {
    if (v.getUint32(c, true) !== 0x02014b50) throw new Error("the zip is damaged");
    const method = v.getUint16(c + 10, true), size = v.getUint32(c + 20, true);
    const nlen = v.getUint16(c + 28, true), xlen = v.getUint16(c + 30, true), clen = v.getUint16(c + 32, true);
    const local = v.getUint32(c + 42, true);
    const name = dec.decode(zip.subarray(c + 46, c + 46 + nlen));
    c += 46 + nlen + xlen + clen;
    if (name.endsWith("/")) continue;
    if (method !== 0) throw new Error(`${name}: compressed (only plain zips; unpack it and pick the files)`);
    const at = local + 30 + v.getUint16(local + 26, true) + v.getUint16(local + 28, true);
    out.push({ name, data: zip.slice(at, at + size) });
  }
  return out;
}
