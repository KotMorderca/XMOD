// SPDX-License-Identifier: GPL-3.0-only
// The FM-1's song slots over its USB serial port (firmware/console.c, raw mode). The transport is
// any pair { write(Uint8Array) -> Promise, read() -> Promise<Uint8Array | null> }: WebSerial in
// the page, or anything else that moves bytes.

const enc = new TextEncoder(), dec = new TextDecoder();

export function crc32(data) {
  let c = ~0;
  for (let i = 0; i < data.length; i++) {
    c ^= data[i];
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
  }
  return ~c >>> 0;
}

function b64(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return btoa(s);
}

function unb64(s) {
  const t = atob(s), out = new Uint8Array(t.length);
  for (let i = 0; i < t.length; i++) out[i] = t.charCodeAt(i);
  return out;
}

export class FM1Error extends Error {}

export class FM1Songs {
  constructor(transport, log) {
    this.t = transport;
    this.log = log || (() => {});
    this.buf = "";
    this.lines = [];
    this.waiter = null;
    this.closed = false;
    this.busy = Promise.resolve();
    this.pump();
  }

  async pump() {                                   // bytes in -> lines
    try {
      for (;;) {
        const d = await this.t.read();
        if (!d) break;
        this.buf += dec.decode(d, { stream: true });
        let i;
        while ((i = this.buf.indexOf("\n")) >= 0) {
          const ln = this.buf.slice(0, i).replace(/\r$/, "").trim();
          this.buf = this.buf.slice(i + 1);
          if (ln && !ln.startsWith(">") && !/^XMOD .* console/.test(ln)) this.lines.push(ln);
        }
        if (this.waiter) { const w = this.waiter; this.waiter = null; w(); }
      }
    } catch (e) { this.log("serial: " + e.message); }
    this.closed = true;
    if (this.waiter) { const w = this.waiter; this.waiter = null; w(); }
  }

  async line(ms = 10000) {
    const end = Date.now() + ms;
    while (!this.lines.length) {
      if (this.closed) throw new FM1Error("the FM-1 was disconnected");
      const left = end - Date.now();
      if (left <= 0) throw new FM1Error("the FM-1 does not answer");
      let tm;
      await Promise.race([new Promise((r) => { this.waiter = r; }), new Promise((r) => { tm = setTimeout(r, left); })]);
      clearTimeout(tm);
      this.waiter = null;
    }
    return this.lines.shift();
  }

  send(text) { return this.t.write(enc.encode(text + "\r")); }

  // one command at a time
  run(fn) {
    const p = this.busy.then(fn, fn);
    this.busy = p.catch(() => {});
    return p;
  }

  async cmdNow(text, ms) {                         // the lines up to ok (err: thrown)
    await this.send(text);
    const out = [];
    for (;;) {
      const ln = await this.line(ms);
      if (ln === "ok" || ln.startsWith("ok ")) { out.push(ln); return out; }
      if (ln.startsWith("err")) throw new FM1Error(ln.replace(/^err\s*-?\d*\s*/, "") || ln);
      if (ln === text) continue;                   // (the echo before raw mode)
      out.push(ln);
    }
  }

  cmd(text, ms) { return this.run(() => this.cmdNow(text, ms)); }

  async start() {
    await this.send("");
    await new Promise((r) => setTimeout(r, 200));
    this.lines.length = 0;
    await this.cmd("raw");
    return this.info();
  }

  async info() {
    const r = {};
    for (const ln of await this.cmd("info")) {
      const m = /^(\w+) ?(.*)$/.exec(ln);
      if (m && m[1] !== "ok") r[m[1]] = /^-?\d+$/.test(m[2]) ? parseInt(m[2], 10) : m[2];
    }
    return r;
  }

  async list() {
    const out = [];
    for (const ln of await this.cmd("ls")) {
      const m = /^slot (\d+) (\d+) (\d+) ?(.*)$/.exec(ln);
      if (m) out.push({ index: +m[1], size: +m[2], seq: +m[3], name: m[4] });
    }
    return out;
  }

  get(index, onProgress) {
    return this.run(async () => {
      await this.send(`get ${index}`);
      const head = await this.line();
      const m = /^data (\d+) ([0-9a-f]+)$/.exec(head);
      if (!m) throw new FM1Error(head.replace(/^err\s*-?\d*\s*/, ""));
      const size = +m[1], crc = parseInt(m[2], 16), out = new Uint8Array(size);
      let n = 0;
      for (;;) {
        const ln = await this.line();
        if (ln === "ok") break;
        const b = unb64(ln);
        if (n + b.length > size) throw new FM1Error("the song came back too long");
        out.set(b, n);
        n += b.length;
        if (onProgress) onProgress(n / size);
      }
      if (n !== size || crc32(out) !== crc) throw new FM1Error("the song came back damaged (checksum)");
      return out;
    });
  }

  // a song image (FMTK) into a new slot -> its index
  put(image, name, onProgress) {
    return this.run(async () => {
      if (dec.decode(image.subarray(0, 4)) !== "FMTK") throw new FM1Error("not a song image");
      name = name.replace(/[^\x20-\x7e]/g, "_").slice(0, 23);
      await this.send(`put ${image.length} ${crc32(image).toString(16).padStart(8, "0")} ${name}`);
      const r = await this.line(30000);
      if (r !== "ready") throw new FM1Error(r.replace(/^err\s*-?\d*\s*/, ""));
      const CH = 96, WINDOW = 16384;               // (the device acks every 4 KiB)
      let sent = 0, acked = 0;
      while (sent < image.length) {
        if (sent - acked >= WINDOW) {
          const ln = await this.line(30000);
          if (ln.startsWith("ack ")) { acked = +ln.slice(4); continue; }
          throw new FM1Error(ln.replace(/^err\s*-?\d*\s*/, ""));
        }
        const part = image.subarray(sent, sent + CH);
        await this.send("d " + b64(part));
        sent += part.length;
        if (onProgress) onProgress(sent / image.length);
      }
      for (;;) {
        const ln = await this.line(30000);
        if (ln.startsWith("ack ")) continue;
        const m = /^saved (\d+)$/.exec(ln);
        if (m) return +m[1];
        throw new FM1Error(ln.replace(/^err\s*-?\d*\s*/, ""));
      }
    });
  }

  remove(index) { return this.cmd(`del ${index}`); }
  rename(index, name) { return this.cmd(`ren ${index} ${name.replace(/[^\x20-\x7e]/g, "_").slice(0, 23)}`, 30000); }
  load(index, force) { return this.cmd(`load ${index}${force ? " force" : ""}`); }
}
