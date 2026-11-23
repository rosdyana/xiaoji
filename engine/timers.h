/* ITIMER_REAL and POSIX timers (B9): setitimer, getitimer, timer_create, timer_settime,
 * timer_gettime, timer_getoverrun and timer_delete. The clock is the engine's (timers.c). */
#ifndef XIAOJI_TIMERS_H
#define XIAOJI_TIMERS_H

#include <stdint.h>

struct proc;

/* The syscalls. Each returns 0 or a negative Linux errno, as the other sys_ functions do. */
int64_t timers_setitimer(uint64_t which, uint64_t newp, uint64_t oldp);
int64_t timers_getitimer(uint64_t which, uint64_t curp);
int64_t timers_create(uint64_t clk, uint64_t sevp, uint64_t idp);
int64_t timers_settime(uint64_t id, uint64_t flags, uint64_t newp, uint64_t oldp);
int64_t timers_gettime(uint64_t id, uint64_t curp);
int64_t timers_getoverrun(uint64_t id);
int64_t timers_delete(uint64_t id);

/* The scheduler: raises the signals of the timers that are due now (sched.c expire_timers). */
void timers_expire(void);

/* The earliest armed deadline, in host monotonic ns, and 1; or 0 when no timer is armed. A
 * deadline that has passed is reported as the current time, so the scheduler does not sleep. */
int timers_next(uint64_t *when);

/* A process has ended: all of its timers go. */
void timers_proc_gone(struct proc *p);

/* execve: the POSIX timers of the process go, and its ITIMER_REAL stays (as on Linux). */
void timers_proc_exec(struct proc *p);

#endif
