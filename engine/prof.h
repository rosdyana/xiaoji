/* Profiler, enabled by XIAOJI_PROFILE=1. Off by default. When off, the cost is one
 * branch per scheduling slice (choosing the instruction loop), one per syscall,
 * and one per thread start, exit or idle wait. The report goes to stderr at exit. */
#ifndef XIAOJI_PROF_H
#define XIAOJI_PROF_H

#include <stdint.h>

#include "cpu_a64.h"

extern int prof_on;

/* Events counted per thread. Hints and exclusives are counted per instruction in
 * the profiling loop; the rest are counted at syscalls and slice ends. */
enum prof_event {
    PE_WFE,
    PE_WFI,
    PE_YIELD,
    PE_ISB,
    PE_LDXR,
    PE_STXR,
    PE_STXR_FAIL,
    PE_FUTEX_WAIT,
    PE_FUTEX_BLOCK,
    PE_FUTEX_EAGAIN,
    PE_FUTEX_TIMEOUT,
    PE_SCHED_YIELD,
    PE_NANOSLEEP,
    PE_COUNT
};

/* Reads XIAOJI_PROFILE and, if it is 1, turns profiling on and registers the report.
 * root is the --root directory (NULL for the host root); it is removed from mapping paths. */
void prof_init(const char *root);

void prof_thread_new(uint64_t tid, const char *name);
void prof_thread_name(uint64_t tid, const char *name);
void prof_thread_end(uint64_t tid, uint64_t icount);
/* A slice ended; full is 1 if it used its whole budget. */
void prof_slice(uint64_t tid, int full);
/* The host slept ns nanoseconds with every thread blocked. */
void prof_idle(uint64_t ns);
/* After a syscall: its number, arguments, result and whether the thread blocked. */
void prof_syscall(uint64_t tid, uint64_t nr, const uint64_t a[6], int64_t ret, int blocked);

/* Used by the profiling instruction loop (cpu_a64.c). */
void prof_pre(cpu_t *c, uint32_t insn, uint64_t pc);
void prof_post(cpu_t *c, uint32_t insn);

#endif
