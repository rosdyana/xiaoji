/* Green threads: guest threads share one address space and run one at a time
 * on the host thread, each for a slice of instructions. */
#ifndef XIAOJI_SCHED_H
#define XIAOJI_SCHED_H

#include <stdint.h>

#include "cpu_a64.h"

#define SCHED_SLICE 200000u /* instructions per scheduling slice */

/* Runs the process from main_cpu until its last thread exits or the process
 * exits. Returns the process exit status. */
int sched_run(cpu_t *main_cpu);

/* Ends the whole process (exit_group, unimplemented instructions). The calling
 * thread stops at once. */
void sched_exit_process(uint64_t code);

/* Ends the calling thread (exit). Clears its clear_child_tid word and wakes a
 * waiter on it. */
void sched_thread_exit(cpu_t *c, uint64_t code);

/* clone for a new thread, sharing the address space. Returns the child tid, or
 * a negative Linux errno. */
int64_t sched_clone(cpu_t *parent, uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t tls, uint64_t ctid);

uint64_t sched_tid(void);
void sched_set_clear_child_tid(uint64_t va);

/* Blocks the calling thread on a futex key. The wake-up value is set in x0 when
 * it wakes: 0 for a wake, -ETIMEDOUT for a timeout. */
void sched_block_futex(uint64_t key, uint32_t bitset, int has_deadline, uint64_t deadline);
void sched_sleep_until(uint64_t deadline);
int64_t sched_futex_wake(uint64_t key, uint64_t count, uint32_t bitset);
int64_t sched_futex_requeue(uint64_t from, uint64_t to, uint64_t wake_n, uint64_t requeue_n);

/* Thread name (16 bytes, NUL-terminated), per thread. */
void sched_set_name(const char *name);
const char *sched_name(void);

/* Instructions executed by all threads. */
uint64_t sched_icount(void);

#endif
