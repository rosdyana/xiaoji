/* seccomp (B19): strict mode and classic BPF filters, with Linux's semantics (kernel/seccomp.c). The state is per
 * thread, in its cpu_t: the mode and the newest filter of its chain (filters are immutable and shared by reference:
 * a fork or clone copies the pointer, TSYNC moves other threads to the caller's chain). */
#ifndef XIAOJI_SECCOMP_H
#define XIAOJI_SECCOMP_H

#include <stdint.h>

#include "cpu_a64.h"

#define SECCOMP_MODE_DISABLED 0
#define SECCOMP_MODE_STRICT 1
#define SECCOMP_MODE_FILTER 2

/* seccomp(2) */
int64_t seccomp_syscall(cpu_t *c, uint64_t op, uint64_t flags, uint64_t uargs);
/* prctl(PR_SET_SECCOMP, mode, filter) and prctl(PR_GET_SECCOMP) */
int64_t seccomp_prctl_set(cpu_t *c, uint64_t mode, uint64_t filter);
int64_t seccomp_prctl_get(const cpu_t *c);

/* What seccomp_enter decided. */
enum {
    SECCOMP_RUN = 0,  /* run the system call */
    SECCOMP_SKIP = 1, /* skip it: x0 is *ret */
    SECCOMP_GONE = 2, /* skip it and leave the registers alone: a SIGSYS frame was built, or the thread is ending */
};

/* Called for every system call of a thread whose mode is not DISABLED (syscall_dispatch tests c->sc_mode first, so
 * a thread without seccomp pays one branch). nr and a[] are the call's x8 and x0..x5; c->pc is the address after
 * the svc. */
int seccomp_enter(cpu_t *c, uint64_t nr, const uint64_t a[6], int64_t *ret);

/* The number of filters in the thread's chain (/proc/<pid>/status). */
int seccomp_chain_length(const cpu_t *c);

/* A new thread (fork, clone, the first thread) took a copy of its parent's cpu_t: take a reference on the chain. */
void seccomp_thread_copy(cpu_t *c);
/* A thread is freed: drop its reference. */
void seccomp_thread_release(cpu_t *c);

#endif
