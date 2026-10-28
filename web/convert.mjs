// SPDX-License-Identifier: GPL-3.0-only
// The song converters (host/modconv.c, host/modwrite.c built to WebAssembly: host/wasm_api.c).
//   const conv = await loadConverter(wasmBytes);
//   conv.toImage(xmOrModBytes) -> Uint8Array (FMTK)        (throws Error with the reason)
//   conv.toXM(fmtk) -> Uint8Array;  conv.toMOD(fmtk) -> {data, warning}

export async function loadConverter(bytes) {
  let mem;
  // The C needs no system: the few WASI calls the libc may name get harmless stubs.
  const wasi = new Proxy({}, {
    get: (_, name) => (...a) => {
      if (name === "fd_write" && mem) {           // (stdio would print here: not used)
        const v = new DataView(mem.buffer);
        let n = 0;
        for (let i = 0; i < a[2]; i++) n += v.getUint32(a[1] + i * 8 + 4, true);
        v.setUint32(a[3], n, true);
      }
      if (name === "proc_exit") throw new Error("converter exited");
      return 0;
    },
  });
  const { instance } = await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasi });
  const x = instance.exports;
  mem = x.memory;
  if (x._initialize) x._initialize();

  const str = p => {
    const b = new Uint8Array(mem.buffer, p);
    let n = 0;
    while (b[n]) n++;
    return new TextDecoder().decode(b.subarray(0, n));
  };
  const run = (kind, input) => {
    const p = x.buf_alloc(input.length);
    new Uint8Array(mem.buffer, p, input.length).set(input);
    const r = x.convert(kind, p, input.length);
    x.buf_free(p);
    const message = str(x.message_ptr());
    if (r) throw new Error(message || "conversion failed");
    return { data: new Uint8Array(mem.buffer, x.result_ptr(), x.result_len()).slice(), message };
  };
  return {
    toImage: f => run(0, f).data,
    toImage8: f => run(3, f).data,                 // 16-bit samples as 8-bit: about half the size
    toXM: img => run(1, img).data,
    toMOD: img => { const r = run(2, img); return { data: r.data, warning: r.message }; },
  };
}
