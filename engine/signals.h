#ifndef XIAOJI_SIGNALS_H
#define XIAOJI_SIGNALS_H

#include <stddef.h>
#include <stdint.h>

#include "cpu_a64.h"

#define SIGILL_N 4
#define SIGBUS_N 7
#define SIGSEGV_N 11
#define ILL_ILLOPC 1 /* si_code for SIGILL */
#define BUS_ADRERR 2 /* si_code for SIGBUS: a file-backed page past the end of its file */

/* Per-process handler tables. sig_tab_switch makes t the running table. */
typedef struct sigtab sigtab_t;
size_t sig_tab_size(void); /* bytes for one zeroed table */
void sig_tab_switch(sigtab_t *t);
void sig_tab_copy(sigtab_t *dst, const sigtab_t *src); /* fork: the handlers are copied */

/* Syscalls. Each returns the Linux result (negative errno) unless noted. */
int64_t sig_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oact, uint64_t sigsetsize);
int64_t sig_rt_sigprocmask(cpu_t *c, uint64_t how, uint64_t set, uint64_t oset, uint64_t sigsetsize);
int64_t sig_sigaltstack(cpu_t *c, uint64_t ss, uint64_t oss);
int64_t sig_kill(uint64_t pid, uint64_t sig);
int64_t sig_tkill(uint64_t tid, uint64_t sig);
int64_t sig_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig);
int64_t sig_rt_sigqueueinfo(uint64_t pid, uint64_t sig, uint64_t uinfo);
int64_t sig_rt_tgsigqueueinfo(uint64_t tgid, uint64_t tid, uint64_t sig, uint64_t uinfo);
/* execve, for the calling thread c: handlers reset to the default (ignored ones stay). */
void sig_exec_reset(cpu_t *c);
/* Raises sig for the running thread, as a kernel-generated signal (SIGPIPE). */
void sig_raise_current(int sig);
/* SIGCHLD (or another child-state signal) for a parent process: code is the si_code
 * (CLD_EXITED or CLD_KILLED), pid the child, status its exit status or signal. */
struct proc;
void sig_child_event(struct proc *parent, int sig, int code, uint32_t pid, int status);
int64_t sig_timedwait(cpu_t *c, uint64_t wanted, uint64_t info, int poll, int has_deadline, uint64_t deadline);
int64_t sig_sigsuspend(cpu_t *c, uint64_t set, uint64_t sigsetsize);
/* rt_sigreturn: restores the thread from the frame at sp. Sets c->sig_restore so
 * the syscall result does not overwrite the restored x0. */
void sig_rt_sigreturn(cpu_t *c);

/* Called at an instruction boundary when an unmasked signal is pending. Delivers
 * one signal: a handler frame, or the default action (which may end the process). */
void sig_deliver_pending(cpu_t *c);

/* 1 when a signal is pending and not blocked: a wait that signals can interrupt returns EINTR (locks.c). */
int sig_interrupt_pending(const cpu_t *c);

/* A synchronous fault in the instruction at c->pc (SIGSEGV or SIGILL). Returns 1 if
 * a handler frame was built. Returns 0 if the default action applies (no usable
 * handler), and the caller reports and exits. */
int sig_fault(cpu_t *c, int sig, int code, uint64_t addr);

/* 1 if a fault with this signal would enter a guest handler now (installed and not
 * blocked). */
int sig_catchable(const cpu_t *c, int sig);
/* XIAOJI_DEBUG_FAULTS=1: sig_set_debug_faults(1) turns on a stderr line per fault
 * delivered to a guest handler. */
void sig_set_debug_faults(int on);
int sig_debug_faults(void);

/* The default SIGSEGV action: message and exit 139. */
void sig_segv_fatal(uint64_t addr, uint64_t pc);
/* The default SIGBUS action: message and death by SIGBUS. */
void sig_bus_fatal(uint64_t addr, uint64_t pc);

/* Timers (timers.c): a timer's signal to process p (tid 0) or to thread tid of p; 0, or -1 for no target.
 * sig_timer_pending says whether sig is already pending on that target. */
int sig_timer_raise(struct proc *p, int tid, int sig, int code, uint32_t id, uint64_t value);
int sig_timer_pending(struct proc *p, int tid, int sig);

#endif
