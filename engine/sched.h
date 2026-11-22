/* Green threads: guest threads share one address space and run one at a time
 * on the host thread, each for a slice of instructions. */
#ifndef XIAOJI_SCHED_H
#define XIAOJI_SCHED_H

#include <stdint.h>

#include "cpu_a64.h"

struct proc;

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
void sched_block_futex(uint64_t key, uint64_t obj, uint32_t bitset, int has_deadline, uint64_t deadline);
void sched_sleep_until(uint64_t deadline);
/* Futex keys are (key, obj) pairs from mem_futex_key: the word's position and its object. */
int64_t sched_futex_wake(uint64_t key, uint64_t obj, uint64_t count, uint32_t bitset);
int64_t sched_futex_requeue(uint64_t from, uint64_t from_obj, uint64_t to, uint64_t to_obj, uint64_t wake_n,
                            uint64_t requeue_n);

/* Thread name (16 bytes, NUL-terminated), per thread. */
void sched_set_name(const char *name);
const char *sched_name(void);

/* Instructions executed by all threads. */
uint64_t sched_icount(void);
/* Instructions executed by the threads of process p (reaped threads included). */
uint64_t sched_icount_of(const struct proc *p);
/* execve: the calling thread is left alone in its process (see sched.c). */
void sched_exec_threads(cpu_t *self);

/* Signal waits. A thread in rt_sigtimedwait blocks as SW_SIGWAIT. sched_wake ends
 * any wait with the given x0 (a no-op for a runnable thread). sched_threads lists
 * the live threads in slot order. */
#define SCHED_MAX_THREADS 256
typedef enum { SW_NONE, SW_FUTEX, SW_SLEEP, SW_SIGWAIT, SW_RESTART, SW_SUSPEND } sched_wait_t;
/* rt_sigsuspend: the thread waits until a signal is raised for it (sched_wake gives -EINTR). */
void sched_block_suspend(void);
void sched_block_sigwait(int has_deadline, uint64_t deadline);
sched_wait_t sched_wait_kind(const cpu_t *c);
void sched_wake(cpu_t *c, int64_t ret);
int sched_threads(cpu_t **out, int max);
cpu_t *sched_find_thread(uint64_t tid);
/* The scheduling policy and priority of a thread (sched_setscheduler and friends). The engine
 * does not schedule by them: they are stored and reported, and binder raises them for a
 * request. Both return 0, or -1 when there is no such thread. */
int sched_policy_get(uint64_t tid, int *policy, int *prio);
int sched_policy_set(uint64_t tid, int policy, int prio);
struct proc;
/* The threads of one process, in slot order. */
int sched_threads_of(const struct proc *p, cpu_t **out, int max);
/* The process a thread (its cpu) belongs to. */
struct proc *sched_proc_of(const cpu_t *c);
/* vfork: the calling thread waits for key (its child's process); the wake gives it ret. */
void sched_block_vfork(uint64_t key, int64_t ret);
/* A fatal signal ends the calling process: the status is 128+sig, and the process
 * reports the signal to its parent (wait status). */
void sched_exit_signal(int sig);

/* Restart waits. The calling thread's syscall is rewound (pc goes back to its svc)
 * and the thread sleeps on key until sched_wake_key(key). The syscall then runs again
 * with the same registers; x0 is not written on that wake-up. A signal wakes it too
 * (sched_wake): the call restarts after the handler, except a part-done pipe write,
 * which returns the bytes it wrote. No EINTR is returned. */
void sched_block_restart(uint64_t key);
/* As sched_block_restart, and the thread also wakes (to rerun) at the host monotonic
 * deadline, if has_deadline. */
void sched_block_restart_until(uint64_t key, int has_deadline, uint64_t deadline);
void sched_wake_key(uint64_t key);
/* The cpu of the running thread (the one whose syscall is being run). */
cpu_t *sched_cur_cpu(void);

#endif
