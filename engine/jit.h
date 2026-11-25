/* Block translator for the wasm build (see engine/jit.c). The native build does not
 * compile jit.c, and the hooks in the other engine files are #ifdef __EMSCRIPTEN__. */
#ifndef XIAOJI_JIT_H
#define XIAOJI_JIT_H

#include <stdint.h>

#include "cpu_a64.h"

/* Called by the interpreter loop at a non-sequential pc (after a taken branch, a
 * block exit, or an interpreted instruction that ends translated code). Returns:
 *   1: a translated block ran from c->pc; its instructions are charged to c->icount
 *      and *budget, and the loop continues at the pc it left;
 *   2: the interpreter must run the instruction at c->pc, which is known not to be
 *      translated, and the instruction after it may start a block;
 *   0: the interpreter runs the instruction at c->pc, and the flow is sequential. */
int jit_dispatch(cpu_t *c, uint64_t *budget);

/* A fault longjmped out of a translated block at fault_pc: charges the instructions
 * of the block up to and including the faulting one. No-op outside a block. */
void jit_fault(cpu_t *c, uint64_t fault_pc);

/* A host page's bytes are about to change, or the page is about to be freed. Drops
 * every translated block on it. Called on any write path (TLB slow path, poke,
 * memfd writes) and before munmap, mprotect, mremap and drop. */
void jit_page_changed(const uint8_t *host_page);

/* Empties the block chain cache; the memory layer calls it on every mapping change. */
void jit_chain_flush(void);

/* At a snapshot, before the clean file pages are dropped: records the translated blocks' pcs (the pre-hot set). */
void jit_snap_mark(void);

/* After a restore (snap.c): drops every translated block of the image (they name another instance's table). */
void jit_snap_reset(void);

/* --jit-prehot: after a restore, a pc whose block the image had translated is queued for translation at its first
 * entry instead of after JIT_HOT entries. The counts: pcs in the set, pcs queued from it so far. */
void jit_set_prehot(int on);
void jit_prehot_counts(uint64_t *set, uint64_t *queued);

/* 1 if the host page holds translated code (any block on it, live or not). */
int jit_page_is_code(const uint8_t *host_page);

/* The translator's running counts, for the load report (sched.c): instructions run in translated code, blocks
 * translated, modules compiled, host nanoseconds spent translating and compiling. */
void jit_load_counts(uint64_t out[4]);

/* XIAOJI_STATS=1 line for the translator. total_insns is the engine's count. */
void jit_print_stats(uint64_t total_insns);

/* Runs one AdvSIMD/FP/SIMD load/store instruction as the interpreter does (cpu_a64.c).
 * Returns 0, or 1 if the engine does not implement it. Wasm builds only. */
int cpu_exec_simd(cpu_t *c, uint32_t insn, uint64_t pc);

#endif
