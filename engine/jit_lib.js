// JS side of the block translator (engine/jit.c). The C side emits one wasm module per
// batch of blocks and calls js_jit_compile with its bytes. The module imports the
// engine's memory and four C helpers (jit_ld, jit_st, jit_pure, jit_simd) by their
// exports. Every block is one exported function. The C side gives each function its
// table slot (js_jit_compile), and calls blocks by table index. js_jit_table_base is the
// first free index of the table; js_jit_clear_table empties a range of slots.
addToLibrary({
  js_jit_table_base: function () {
    return wasmTable.length;
  },
  js_jit_clear_table: function (first, n) {
    for (var i = first; i < first + n && i < wasmTable.length; i++) wasmTable.set(i, null);
  },
  js_jit_compile: function (buf, len, nfuncs, slots) {
    // Pointers at or above 2 GB arrive negative: >>> 0 (docs/demo.md, P1).
    buf >>>= 0;
    slots >>>= 0;
    var inst;
    try {
      var mod = new WebAssembly.Module(HEAPU8.slice(buf, buf + len));
      inst = new WebAssembly.Instance(mod, {
        e: {
          m: wasmMemory,
          ld: wasmExports['jit_ld'],
          st: wasmExports['jit_st'],
          pure: wasmExports['jit_pure'],
          simd: wasmExports['jit_simd'],
          t: wasmTable,
        },
      });
    } catch (e) {
      console.error('[xiaoji] jit module failed to compile', e);
      return -1;
    }
  for (var i = 0; i < nfuncs; i++) {
    var slot = HEAP32[(slots >>> 2) + i];
    if (slot >= wasmTable.length) wasmTable.grow(slot + 1 - wasmTable.length);
    wasmTable.set(slot, inst.exports['f' + i]);
  }
    return 0;
  },
  // The input ring (web/input.js, docs/input.md): the page's events for /dev/input (engine/evdev.c, XIAOJI_WEB). Copies
  // the next event (dev, type, code, value) to out and returns 1, or returns 0 when the ring is empty or the page gave none.
  // The ring is Module.inputRing (web/worker.js): word 0 the head, word 1 the tail, then the entries (4 words each).
  xiaoji_input_pop: function (out) {
    out >>>= 0;
    var r = Module['inputRing'];
    if (!r) return 0;
    var head = Atomics.load(r, 0), tail = Atomics.load(r, 1);
    if (((head - tail) & 0x7fffffff) === 0) return 0;
    var cap = (r.length - 2) >> 2;
    var at = 2 + 4 * (tail % cap);
    for (var i = 0; i < 4; i++) HEAP32[(out >>> 2) + i] = r[at + i];
    Atomics.store(r, 1, (tail + 1) & 0x7fffffff);
    return 1;
  },
});
