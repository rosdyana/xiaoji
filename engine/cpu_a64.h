/* A64 integer interpreter state and main loop. */
#ifndef XIAOJI_CPU_A64_H
#define XIAOJI_CPU_A64_H

#include <stdint.h>

typedef struct cpu {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint32_t nzcv; /* N=bit31 Z=bit30 C=bit29 V=bit28 */
    uint64_t tpidr_el0;
    uint64_t v[32][2]; /* SIMD&FP registers as raw bits: v[n][0] is the low half */
    uint64_t fpcr;
    uint64_t fpsr;
    int excl_valid;     /* exclusive monitor */
    uint64_t excl_addr;
    uint64_t icount;    /* executed instructions */
    int exited;         /* this thread has exited */
    int exit_code;
    int trace;          /* print each syscall to stderr */
    uint64_t tid;       /* guest thread id, set by the scheduler */
    uint64_t budget;    /* instructions left in this scheduling slice */
    int stop;           /* leave cpu_run after the current instruction */
    int blocked;        /* a syscall blocked: x0 is written at wake-up, not now */
} cpu_t;

/* Runs the thread until it stops (exit, block, yield) or its slice budget is
 * spent. Unimplemented instructions print a message and end the process with
 * status 132 (sched_exit_process). */
void cpu_run(cpu_t *c);

#endif
