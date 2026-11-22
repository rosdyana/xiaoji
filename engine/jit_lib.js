// JS side of the block translator (engine/jit.c). The C side emits one wasm module per
// batch of blocks and calls js_jit_compile with its bytes. The module imports the
// engine's memory and three C helpers (jit_ld, jit_st, jit_pure) by their exports.
// Every block is one exported function; they are placed in the Emscripten function
// table, and the C side calls them by table index.
addToLibrary({
  js_jit_compile: function (buf, len, nfuncs) {
    var inst;
    try {
      var mod = new WebAssembly.Module(HEAPU8.slice(buf, buf + len));
      inst = new WebAssembly.Instance(mod, {
        e: {
          m: wasmMemory,
          ld: wasmExports['jit_ld'],
          st: wasmExports['jit_st'],
          pure: wasmExports['jit_pure'],
        },
      });
    } catch (e) {
      console.error('[xiaoji] jit module failed to compile', e);
      return -1;
    }
    var first = wasmTable.grow(nfuncs);
    for (var i = 0; i < nfuncs; i++) wasmTable.set(first + i, inst.exports['f' + i]);
    return first;
  },
});
