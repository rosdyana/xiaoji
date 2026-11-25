#ifndef XIAOJI_ASLR_H
#define XIAOJI_ASLR_H

/* Native builds: re-executes the engine with ASLR off, unless this run is that re-execution (aslr.c). Returns 0 when
 * the layout is fixed (this run is the re-execution, or a wasm build), or -1 when the re-execution failed. */
int aslr_off(char **argv);

#endif
