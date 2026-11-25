# dlmalloc

`malloc.c` is Doug Lea's malloc, version 2.8.6, as Emscripten ships it (`system/lib/dlmalloc.c` of the emsdk
the wasm builds use; its additions are under `#if __EMSCRIPTEN__` and are not compiled here). dlmalloc is
released to the public domain (CC0, http://creativecommons.org/publicdomain/zero/1.0/); Emscripten's changes
are under the MIT and University of Illinois/NCSA licenses.

The native engine compiles it from `engine/arena.c`, which gives it a fixed-address arena (docs/snapshot.md):
every engine allocation then lives at an address that is the same in every run, so a snapshot of the arena
can be restored. The wasm builds use Emscripten's own copy, inside linear memory.
