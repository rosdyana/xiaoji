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

    /* Signal state, per thread. Bit n-1 of a mask or pending set is signal n. */
    uint64_t sig_mask;        /* blocked signals */
    uint64_t sig_pending;     /* raised for this thread, not yet delivered */
    int32_t sig_code[65];     /* si_code of each pending signal, by signal number */
    uint32_t sig_pid[65];     /* si_pid of each pending signal, by signal number */
    uint64_t sig_wait_set;    /* rt_sigtimedwait: the set being waited for */
    uint64_t sig_wait_info;   /* rt_sigtimedwait: guest siginfo pointer, or 0 */
    uint64_t alt_sp;          /* sigaltstack: ss_sp and ss_size (size 0 means none) */
    uint64_t alt_size;
    uint32_t alt_flags;       /* SS_DISABLE or 0 */
    int sig_restore;          /* the syscall just run was rt_sigreturn: keep x0 */
} cpu_t;

/* Runs the thread until it stops (exit, block, yield) or its slice budget is
 * spent. Pending signals are delivered at instruction boundaries. A fault or an
 * unimplemented instruction raises SIGSEGV or SIGILL; without a handler, the
 * process ends with the default action (139 for a fault, 132 for an unimplemented
 * instruction, after a message). */
void cpu_run(cpu_t *c);

#endif
