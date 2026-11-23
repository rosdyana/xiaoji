/* Green threads, grouped into processes. Each guest thread has its own cpu_t. The
 * scheduler runs the current thread for SCHED_SLICE instructions, then the next
 * runnable thread of any process; a switch to another process installs its address
 * space, fd table and signal table (proc_switch). A thread also gives up the host
 * when it blocks (a futex, a sleep, a pipe, a wait for a child) or exits. Futex waits
 * and sleeps have deadlines (host monotonic time). When every live thread is blocked,
 * the host sleeps until the earliest deadline. If none has one, the scheduler reports
 * a deadlock.
 *
 * A process ends when its last thread exits, on exit_group, or on a fatal signal. Then
 * its other threads are reaped, its fds and address space are released (pipes see end
 * of file), its children go to the initial process, and the parent is told (SIGCHLD,
 * and a wake for its waits). The process stays a zombie until it is reaped. The
 * initial process's exit ends the engine. */
#include "sched.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "linux.h"
#include "mem.h"
#include "prof.h"
#include "proc.h"
#include "signals.h"

#define MAX_THREADS SCHED_MAX_THREADS
#define TID_MAIN 1000u
#define TID_FIRST_NEW 1001u
#define DEADLOCK_STATUS 134
#define KEY_MASK 0x00FFFFFFFFFFFFFFull /* futex keys ignore the address tag */
#define SIGCHLD_N 17
#define CLD_EXITED 1
#define CLD_KILLED 2

#define CLONE_VM 0x00000100ull
#define CLONE_NEWNS 0x00020000ull
#define CLONE_THREAD 0x00010000ull
#define CLONE_SETTLS 0x00080000ull
#define CLONE_PARENT_SETTID 0x00100000ull
#define CLONE_CHILD_CLEARTID 0x00200000ull
#define CLONE_VFORK 0x00004000ull
#define CLONE_CHILD_SETTID 0x01000000ull

typedef enum { TS_RUNNABLE, TS_FUTEX, TS_SLEEP, TS_SIGWAIT, TS_RESTART, TS_VFORK, TS_SUSPEND } thr_state;

typedef struct thread {
    cpu_t cpu;
    thr_state state;
    uint64_t key;        /* futex key, or the wait key (a pipe, a process) while blocked */
    uint64_t key_obj;    /* futex: the object the key is in (mem_futex_key) */
    uint32_t bitset;     /* wake mask of a FUTEX_WAIT_BITSET */
    int has_deadline;
    uint64_t deadline;   /* host monotonic ns */
    int restart_eintr;   /* TS_RESTART from sched_block_restart_eintr: a signal ends the call with EINTR */
    uint64_t clear_child_tid;
    int settid_pending;  /* CLONE_CHILD_SETTID: write the tid into the child's memory on its first slice */
    uint64_t settid_va;
    int64_t vfork_ret;   /* TS_VFORK: the x0 the parent gets when its child execs or exits */
    char comm[16];
    int policy, prio;    /* scheduling policy and priority (sched_policy_get/set) */
    proc_t *proc; /* the process this thread belongs to */
} thread_t;

/* sched_wait_kind and sched_wake take a cpu_t pointer and cast it to thread_t. */
_Static_assert(offsetof(thread_t, cpu) == 0, "thread_t must start with its cpu_t");

static thread_t *threads[MAX_THREADS];
static int cur_slot;          /* slot of the running thread */
static proc_t *init_proc;     /* the initial process: its exit ends the engine */
static uint64_t next_tid = TID_FIRST_NEW; /* tids and pids share one number space */
static uint64_t dead_icount;  /* instructions of threads already reaped */
static char init_name[16] = "xiaoji";

static thread_t *cur(void) { return threads[cur_slot]; }

static int live_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i])
            n++;
    return n;
}

static thread_t *add_thread(const cpu_t *from, uint64_t tid, proc_t *p)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i])
            continue;
        thread_t *t = calloc(1, sizeof *t);
        if (!t)
            return NULL;
        t->cpu = *from;
        t->cpu.tid = tid;
        t->cpu.stop = 0;
        t->cpu.blocked = 0;
        t->cpu.exited = 0;
        t->cpu.icount = 0; /* the copy is the parent: a new thread counts its own instructions */
        t->cpu.excl_valid = 0; /* a new thread never inherits the exclusive monitor */
        t->cpu.io_done = 0;
        t->state = TS_RUNNABLE;
        t->proc = p;
        p->nthreads++;
        threads[i] = t;
        return t;
    }
    return NULL;
}

static void reap(int slot)
{
    if (prof_on)
        prof_thread_end(threads[slot]->cpu.tid, threads[slot]->cpu.icount);
    dead_icount += threads[slot]->cpu.icount;
    threads[slot]->proc->dead_icount += threads[slot]->cpu.icount;
    threads[slot]->proc->nthreads--;
    free(threads[slot]);
    threads[slot] = NULL;
}

/* Makes a blocked thread runnable with x0 set to ret. */
static void wake(thread_t *t, int64_t ret)
{
    t->state = TS_RUNNABLE;
    t->has_deadline = 0;
    t->cpu.x[0] = (uint64_t)ret;
    t->cpu.blocked = 0;
}

/* Makes a restart-blocked thread runnable. Its registers are left as they are, so
 * its svc runs again. */
static void wake_restart(thread_t *t)
{
    t->state = TS_RUNNABLE;
    t->has_deadline = 0;
    t->cpu.blocked = 0;
}

static void block_current(thr_state st, uint64_t key, uint32_t bitset, int has_deadline, uint64_t deadline)
{
    thread_t *t = cur();
    t->state = st;
    t->key = key & KEY_MASK;
    t->bitset = bitset;
    t->has_deadline = has_deadline;
    t->deadline = deadline;
    t->restart_eintr = 0;
    t->cpu.blocked = 1;
    t->cpu.stop = 1;
}

void sched_block_futex(uint64_t key, uint64_t obj, uint32_t bitset, int has_deadline, uint64_t deadline)
{
    block_current(TS_FUTEX, key, bitset, has_deadline, deadline);
    cur()->key_obj = obj;
}

void sched_sleep_until(uint64_t deadline)
{
    block_current(TS_SLEEP, 0, 0, 1, deadline);
}

void sched_block_sigwait(int has_deadline, uint64_t deadline)
{
    block_current(TS_SIGWAIT, 0, 0, has_deadline, deadline);
}

void sched_block_suspend(void)
{
    block_current(TS_SUSPEND, 0, 0, 0, 0);
}

void sched_block_restart(uint64_t key)
{
    thread_t *t = cur();
    t->cpu.pc -= 4; /* the svc runs again when the thread wakes */
    block_current(TS_RESTART, key, 0, 0, 0);
}

/* A restart wait that a signal ends with -EINTR instead of a restart (locks.c: F_SETLKW, flock). */
void sched_block_restart_eintr(uint64_t key)
{
    thread_t *t = cur();
    t->cpu.pc -= 4;
    block_current(TS_RESTART, key, 0, 0, 0);
    t->restart_eintr = 1;
}

void sched_block_restart_until(uint64_t key, int has_deadline, uint64_t deadline)
{
    thread_t *t = cur();
    t->cpu.pc -= 4;
    block_current(TS_RESTART, key, 0, has_deadline, deadline);
}

void sched_block_vfork(uint64_t key, int64_t ret)
{
    thread_t *t = cur();
    t->vfork_ret = ret;
    block_current(TS_VFORK, key, 0, 0, 0);
}

void sched_wake_key(uint64_t key)
{
    key &= KEY_MASK;
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t || t->key != key)
            continue;
        if (t->state == TS_RESTART)
            wake_restart(t);
        else if (t->state == TS_VFORK)
            wake(t, t->vfork_ret);
    }
}

cpu_t *sched_cur_cpu(void)
{
    return cur() ? &cur()->cpu : NULL;
}

proc_t *sched_proc_of(const cpu_t *c)
{
    /* cpu_t is the first member of thread_t, so the pointer is the thread's. */
    return ((const thread_t *)c)->proc;
}

sched_wait_t sched_wait_kind(const cpu_t *c)
{
    const thread_t *t = (const thread_t *)c;
    switch (t->state) {
    case TS_FUTEX: return SW_FUTEX;
    case TS_SLEEP: return SW_SLEEP;
    case TS_SIGWAIT: return SW_SIGWAIT;
    case TS_RESTART: return SW_RESTART;
    case TS_SUSPEND: return SW_SUSPEND;
    default: return SW_NONE;
    }
}

void sched_wake(cpu_t *c, int64_t ret)
{
    thread_t *t = (thread_t *)c;
    if (t->state == TS_RESTART && t->restart_eintr && ret < 0) {
        /* The interrupted call returns the error, and the signal's frame returns past the svc
         * (Linux without SA_RESTART). The signal stays pending for its handler. */
        t->cpu.pc += 4;
        wake(t, ret);
    } else if (t->state == TS_RESTART)
        wake_restart(t);
    else if (t->state != TS_RUNNABLE)
        wake(t, ret);
}

int sched_threads(cpu_t **out, int max)
{
    int n = 0;
    for (int i = 0; i < MAX_THREADS && n < max; i++)
        if (threads[i])
            out[n++] = &threads[i]->cpu;
    return n;
}

int sched_threads_of(const proc_t *p, cpu_t **out, int max)
{
    int n = 0;
    for (int i = 0; i < MAX_THREADS && n < max; i++)
        if (threads[i] && threads[i]->proc == p)
            out[n++] = &threads[i]->cpu;
    return n;
}

cpu_t *sched_find_thread(uint64_t tid)
{
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i] && threads[i]->cpu.tid == tid)
            return &threads[i]->cpu;
    return NULL;
}

/* A new thread (clone, fork) starts with its parent's policy and priority, as in Linux.
 * parent is the calling thread's cpu, the first member of its thread_t. */
static void inherit_sched(thread_t *t, const cpu_t *parent)
{
    const thread_t *pt = (const thread_t *)parent;
    t->policy = pt->policy;
    t->prio = pt->prio;
}

int sched_policy_get(uint64_t tid, int *policy, int *prio)
{
    cpu_t *c = sched_find_thread(tid);
    if (!c)
        return -1;
    *policy = ((thread_t *)c)->policy;
    *prio = ((thread_t *)c)->prio;
    return 0;
}

int sched_policy_set(uint64_t tid, int policy, int prio)
{
    cpu_t *c = sched_find_thread(tid);
    if (!c)
        return -1;
    ((thread_t *)c)->policy = policy;
    ((thread_t *)c)->prio = prio;
    return 0;
}

int64_t sched_futex_wake(uint64_t key, uint64_t obj, uint64_t count, uint32_t bitset)
{
    int64_t n = 0;
    key &= KEY_MASK;
    for (int i = 0; i < MAX_THREADS && (uint64_t)n < count; i++) {
        thread_t *t = threads[i];
        if (t && t->state == TS_FUTEX && t->key == key && t->key_obj == obj && (t->bitset & bitset)) {
            wake(t, 0);
            n++;
        }
    }
    return n;
}

int64_t sched_futex_requeue(uint64_t from, uint64_t from_obj, uint64_t to, uint64_t to_obj, uint64_t wake_n,
                            uint64_t requeue_n)
{
    int64_t woke = 0;
    uint64_t moved = 0;
    from &= KEY_MASK;
    to &= KEY_MASK;
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t || t->state != TS_FUTEX || t->key != from || t->key_obj != from_obj)
            continue;
        if ((uint64_t)woke < wake_n) {
            wake(t, 0);
            woke++;
        } else if (moved < requeue_n) {
            t->key = to;
            t->key_obj = to_obj;
            moved++;
        }
    }
    return woke;
}

static void expire_timers(uint64_t now)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t || t->state == TS_RUNNABLE || !t->has_deadline || t->deadline > now)
            continue;
        /* A timed-out futex wait returns ETIMEDOUT; a sleep returns 0. */
        int64_t ret = 0;
        if (t->state == TS_FUTEX)
            ret = -(int64_t)LE_TIMEDOUT;
        else if (t->state == TS_SIGWAIT)
            ret = -(int64_t)LE_AGAIN;
        if (t->state == TS_RESTART) { /* a timed restart reruns its call, which checks its deadline */
            wake_restart(t);
            continue;
        }
        wake(t, ret);
    }
}

/* The next runnable thread after the current one (round robin). */
static thread_t *pick(void)
{
    for (int k = 1; k <= MAX_THREADS; k++) {
        int i = (cur_slot + k) % MAX_THREADS;
        thread_t *t = threads[i];
        if (t && t->state == TS_RUNNABLE) {
            cur_slot = i; /* now the candidate; the caller runs it */
            return t;
        }
    }
    return NULL;
}

/* Sleeps the host until the earliest deadline. Returns 0 if no thread has one. */
static int wait_for_timers(void)
{
    int any = 0;
    uint64_t earliest = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t || t->state == TS_RUNNABLE || !t->has_deadline)
            continue;
        if (!any || t->deadline < earliest)
            earliest = t->deadline;
        any = 1;
    }
    if (!any)
        return 0;
    uint64_t now = host_monotonic_ns();
    if (earliest > now) {
        if (prof_on)
            prof_idle(earliest - now);
        host_sleep_ns(earliest - now);
    }
    return 1;
}

/* The threads with a deadline: returns how many, and the earliest deadline in *earliest. */
static int pending_deadlines(uint64_t *earliest)
{
    int n = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t || t->state == TS_RUNNABLE || !t->has_deadline)
            continue;
        if (!n || t->deadline < *earliest)
            *earliest = t->deadline;
        n++;
    }
    return n;
}

static void report_threads(void)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t)
            continue;
        fprintf(stderr, "xiaoji:   tid %llu %s pc 0x%llx\n", (unsigned long long)t->cpu.tid,
                t->state == TS_FUTEX      ? "blocked on futex"
                : t->state == TS_SLEEP    ? "sleeping"
                : t->state == TS_SIGWAIT  ? "waiting in sigtimedwait"
                : t->state == TS_RESTART  ? "waiting on a pipe or a child"
                : t->state == TS_VFORK    ? "waiting for a vfork child"
                : t->state == TS_SUSPEND  ? "in sigsuspend"
                                          : "runnable",
                (unsigned long long)t->cpu.pc);
    }
}

static void report_deadlock(void)
{
    fflush(stdout);
    fprintf(stderr, "xiaoji: deadlock: every thread is blocked and none can wake\n");
    report_threads();
}

/* A process has ended (its last thread is gone, or exit_group or a fatal signal): its
 * remaining threads are reaped, and unless it is the initial process it becomes a
 * zombie. Its children go to the initial process, and its parent learns of it. */
static void finish_process(proc_t *p)
{
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i] && threads[i]->proc == p)
            reap(i);
    if (p == init_proc && p->fds)
        vfs_fdtab_exit_ns(p->fds);
    if (p == init_proc || p->zombie)
        return;
    p->zombie = 1;
    proc_reparent_children(p, init_proc);
    proc_release(p);
    if (p->parent) {
        int code = p->term_sig ? CLD_KILLED : CLD_EXITED;
        int status = p->term_sig ? p->term_sig : p->exit_code;
        sig_child_event(p->parent, SIGCHLD_N, code, (uint32_t)p->pid, status);
        sched_wake_key((uint64_t)(uintptr_t)p->parent); /* wait4 and waitid waiters */
    }
    sched_wake_key((uint64_t)(uintptr_t)p); /* a vfork parent waiting for this process */
}

/* Idle in init mode: sleep until init's next deadline, or at most IDLE_POLL_NS, so that init
 * notices its stop flag and its timers. */
#define IDLE_POLL_NS 10000000ull
static void idle_wait(uint64_t (*deadline)(void))
{
    uint64_t now = host_monotonic_ns();
    uint64_t dl = deadline ? deadline() : 0;
    if (dl && dl <= now)
        return;
    uint64_t wait = IDLE_POLL_NS;
    if (dl && dl - now < wait)
        wait = dl - now;
    host_sleep_ns(wait);
}

/* The scheduler loop. main_cpu starts the initial process's main thread. In init mode
 * (main_cpu NULL) the initial process has no thread: tick runs between slices and whenever
 * nothing is runnable, and deadline gives the host time of init's next timer that can wake a
 * thread (0 for none). The deadlock check reads it: a timer that only stops the run does not count. */
static int run_loop(cpu_t *main_cpu, void (*tick)(void), uint64_t (*deadline)(void))
{
    /* Init mode only: idle_since is the start of the current idle streak. A thread that
     * runs ends the streak, so only continuous idleness counts (see the idle branch below). */
    uint64_t idle_since = 0;
    int idle_reported = 0, idle_long_reported = 0;
    if (main_cpu) {
        thread_t *m = add_thread(main_cpu, TID_MAIN, proc_cur());
        if (!m) {
            fprintf(stderr, "xiaoji: out of host memory\n");
            return 1;
        }
        memcpy(m->comm, init_name, sizeof m->comm);
        if (prof_on)
            prof_thread_new(TID_MAIN, m->comm);
    }
    cur_slot = 0;
    init_proc = proc_cur();
    init_proc->exited = 0;

    while (!init_proc->exited) {
        expire_timers(host_monotonic_ns());
        if (tick) {
            tick();
            if (init_proc->exited)
                break;
        }
        thread_t *t = pick();
        if (t && tick) {
            idle_since = 0; /* a thread ran: the idle streak is over */
            idle_reported = idle_long_reported = 0;
        }
        if (!t) {
            if (tick) {
                uint64_t now = host_monotonic_ns();
                uint64_t earliest = 0;
                int pending = pending_deadlines(&earliest);
                int init_pending = deadline && deadline() != 0;
                if (!idle_since)
                    idle_since = now;
                uint64_t idle = now - idle_since;
                /* A hang: every thread is blocked with no deadline, and init has no timer. Nothing in
                 * the guest can wake it. Printed once per idle streak, after 5 s of continuous idleness. */
                if (!idle_reported && idle > 5000000000ull && !pending && !init_pending) {
                    idle_reported = 1;
                    fprintf(stderr, "xiaoji: deadlock: every thread is blocked with no deadline, and none can wake\n");
                    report_threads();
                }
                /* Deadlines pending but nothing has run for a long continuous time: the timers are not
                 * firing (a diagnostic; the run continues). */
                if (!idle_long_reported && idle > 60000000000ull && (pending || init_pending)) {
                    idle_long_reported = 1;
                    fprintf(stderr, "xiaoji: init: nothing has run for 60 s, with deadlines pending\n");
                    report_threads();
                }
                idle_wait(deadline);
                continue;
            }
            if (!wait_for_timers()) {
                report_deadlock();
                init_proc->exit_code = DEADLOCK_STATUS;
                break;
            }
            continue;
        }
        if (t->proc != proc_cur())
            proc_switch(t->proc); /* another process: its address space and files */
        if (t->settid_pending) {
            /* CLONE_CHILD_SETTID: the child's own tid word, written in its address space. */
            uint32_t tid32 = (uint32_t)t->cpu.tid;
            (void)mem_copy_in(t->settid_va, &tid32, sizeof tid32);
            t->settid_pending = 0;
        }
        t->cpu.stop = 0;
        t->cpu.budget = SCHED_SLICE;
        /* A context switch clears the exclusive monitor, as on hardware, so
         * an LDXR from an earlier slice can't pair with a STXR after another
         * thread has written the location. */
        t->cpu.excl_valid = 0;
        cpu_run(&t->cpu);
        if (prof_on)
            prof_slice(t->cpu.tid, t->cpu.budget == 0);
        if (init_proc->exited)
            break;
        proc_t *p = t->proc;
        if (t->cpu.exited) {
            if (p->nthreads == 1 && !p->exited) {
                /* The last thread: its status is the process status. */
                p->exit_code = t->cpu.exit_code;
                p->exited = 1;
            }
            reap(cur_slot);
        }
        if (p->exited)
            finish_process(p);
    }
    /* Unreaped threads (the process ended while they lived) add their counts. */
    return init_proc->exit_code;
}

int sched_run(cpu_t *main_cpu)
{
    return run_loop(main_cpu, NULL, NULL);
}

int sched_run_init(void (*tick)(void), uint64_t (*deadline)(void))
{
    return run_loop(NULL, tick, deadline);
}

proc_t *sched_proc_new_child(void)
{
    /* Before the loop starts, init is the running process (init_main switches to it). */
    proc_t *parent = init_proc ? init_proc : proc_cur();
    uint64_t pid = next_tid;
    proc_t *child = proc_fork(parent, (int)pid, 0);
    if (child)
        next_tid++;
    return child;
}

int sched_start_thread(proc_t *p, uint64_t pc, uint64_t sp, const char *name, int trace)
{
    cpu_t start;
    memset(&start, 0, sizeof start);
    start.pc = pc;
    start.sp = sp;
    start.trace = trace;
    thread_t *t = add_thread(&start, p->pid, p);
    if (!t)
        return -1;
    memset(t->comm, 0, sizeof t->comm);
    strncpy(t->comm, name, sizeof t->comm - 1);
    if (prof_on)
        prof_thread_new((uint64_t)p->pid, t->comm);
    return 0;
}

void sched_exit_process(uint64_t code)
{
    proc_t *p = cur() ? cur()->proc : proc_cur();
    if (!p->exited) {
        p->exited = 1;
        p->exit_code = (int)(code & 0xFF);
    }
    if (cur()) {
        cur()->cpu.exited = 1;
        cur()->cpu.stop = 1;
    }
}

void sched_exit_signal(int sig)
{
    proc_t *p = cur() ? cur()->proc : proc_cur();
    if (!p->exited) {
        p->exited = 1;
        p->exit_code = (128 + sig) & 0xFF; /* the engine's status for a fatal signal */
        p->term_sig = sig;
    }
    if (cur()) {
        cur()->cpu.exited = 1;
        cur()->cpu.stop = 1;
    }
}

void sched_thread_exit(cpu_t *c, uint64_t code)
{
    thread_t *t = cur();
    c->exit_code = (int)(code & 0xFF);
    c->exited = 1;
    c->stop = 1;
    if (t && t->clear_child_tid) {
        /* A thread-exit clear must not kill the process: ignore a bad address. */
        uint8_t zero[4] = {0, 0, 0, 0};
        if (mem_copy_in(t->clear_child_tid, zero, sizeof zero) == 0) {
            uint64_t obj;
            uint64_t key = mem_futex_key(t->clear_child_tid, &obj);
            sched_futex_wake(key, obj, 1, 0xFFFFFFFFu);
        }
        t->clear_child_tid = 0;
    }
}

/* clone for a new thread of the calling process. */
static int64_t clone_thread(cpu_t *parent, uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t tls, uint64_t ctid)
{
    thread_t *me = cur();
    if (live_count() >= MAX_THREADS)
        return -LE_AGAIN;
    uint64_t tid = next_tid;
    uint32_t tid32 = (uint32_t)tid;
    /* The tid words are written before either thread runs. */
    if (flags & CLONE_PARENT_SETTID) {
        if (mem_copy_in(ptid, &tid32, sizeof tid32) < 0)
            return -LE_FAULT;
    }
    if (flags & CLONE_CHILD_SETTID) {
        if (mem_copy_in(ctid, &tid32, sizeof tid32) < 0)
            return -LE_FAULT;
    }
    thread_t *t = add_thread(parent, tid, me->proc);
    if (!t)
        return -LE_NOMEM;
    inherit_sched(t, parent);
    next_tid++;
    t->cpu.x[0] = 0;
    /* The child keeps the mask but starts with no pending signals, no alternate
     * stack and no signal wait, as Linux does. */
    t->cpu.sig_pending = 0;
    t->cpu.sig_wait_set = 0;
    t->cpu.sig_wait_info = 0;
    t->cpu.alt_sp = 0;
    t->cpu.alt_size = 0;
    t->cpu.alt_flags = 0;
    if (newsp)
        t->cpu.sp = newsp;
    if (flags & CLONE_SETTLS)
        t->cpu.tpidr_el0 = tls;
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_child_tid = ctid;
    memcpy(t->comm, me->comm, sizeof t->comm);
    if (prof_on)
        prof_thread_new(tid, t->comm);
    return (int64_t)tid;
}

/* fork (no CLONE_THREAD): a new process with a copy of the address space (copy-on-write,
 * or shared with the parent for vfork), its own main thread, and the pid as its tid.
 * The child starts with x0 = 0. A vfork parent is suspended until the child exits. */
static int64_t clone_process(cpu_t *parent, uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t tls,
                             uint64_t ctid)
{
    thread_t *me = cur();
    if (live_count() >= MAX_THREADS)
        return -LE_AGAIN;
    int vfork = (flags & CLONE_VM) != 0;
    uint64_t pid = next_tid;
    uint32_t pid32 = (uint32_t)pid;
    if (flags & CLONE_PARENT_SETTID) {
        if (mem_copy_in(ptid, &pid32, sizeof pid32) < 0)
            return -LE_FAULT;
    }
    proc_t *child = proc_fork(me->proc, (int)pid, vfork);
    if (!child)
        return -LE_NOMEM;
    if (flags & CLONE_NEWNS) /* the child gets a copy of the mount namespace */
        vfs_fdtab_copy_ns(child->fds);
    thread_t *t = add_thread(parent, pid, child);
    if (!t) /* not reached: live_count left a free slot */
        return -LE_NOMEM;
    inherit_sched(t, parent);
    next_tid++;
    t->cpu.x[0] = 0;
    t->cpu.sig_pending = 0;
    t->cpu.sig_wait_set = 0;
    t->cpu.sig_wait_info = 0;
    t->cpu.alt_sp = 0;
    t->cpu.alt_size = 0;
    t->cpu.alt_flags = 0;
    if (newsp)
        t->cpu.sp = newsp;
    if (flags & CLONE_SETTLS)
        t->cpu.tpidr_el0 = tls;
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_child_tid = ctid;
    if (flags & CLONE_CHILD_SETTID) {
        /* The child's tid word is written in the child's memory, on its first slice. */
        t->settid_pending = 1;
        t->settid_va = ctid;
    }
    memcpy(t->comm, me->comm, sizeof t->comm);
    if (prof_on)
        prof_thread_new(pid, t->comm);
    if (flags & CLONE_VFORK)
        sched_block_vfork((uint64_t)(uintptr_t)child, (int64_t)pid);
    return (int64_t)pid;
}

int64_t sched_clone(cpu_t *parent, uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t tls, uint64_t ctid)
{
    if (flags & CLONE_THREAD) {
        if (!(flags & CLONE_VM))
            return -LE_NOSYS;
        return clone_thread(parent, flags, newsp, ptid, tls, ctid);
    }
    return clone_process(parent, flags, newsp, ptid, tls, ctid);
}

uint64_t sched_tid(void)
{
    return cur() ? cur()->cpu.tid : TID_MAIN;
}

void sched_set_clear_child_tid(uint64_t va)
{
    if (cur())
        cur()->clear_child_tid = va;
}

void sched_set_name(const char *name)
{
    char buf[16];
    memset(buf, 0, sizeof buf);
    strncpy(buf, name, sizeof buf - 1);
    if (cur()) {
        memcpy(cur()->comm, buf, sizeof buf);
        if (prof_on)
            prof_thread_name(cur()->cpu.tid, buf);
    }
    else
        memcpy(init_name, buf, sizeof buf);
}

const char *sched_name(void)
{
    return cur() ? cur()->comm : init_name;
}

/* execve: the calling thread (self) is the only thread left in its process. The others
 * are reaped; the caller takes the process's id as its tid (it may have been a
 * non-leader thread), and its per-thread wait state is cleared. */
void sched_exec_threads(cpu_t *self)
{
    thread_t *me = NULL;
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i] && &threads[i]->cpu == self)
            me = threads[i];
    if (!me)
        return;
    proc_t *p = me->proc;
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i] && threads[i] != me && threads[i]->proc == p)
            reap(i);
    me->clear_child_tid = 0; /* the new image's exit must not clear the old word */
    me->settid_pending = 0;
    me->has_deadline = 0;
    me->state = TS_RUNNABLE;
    self->tid = (uint64_t)p->pid;
}

uint64_t sched_icount_of(const proc_t *p)
{
    uint64_t n = p->dead_icount;
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i] && threads[i]->proc == p)
            n += threads[i]->cpu.icount;
    return n;
}

uint64_t sched_icount(void)
{
    uint64_t n = dead_icount;
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i])
            n += threads[i]->cpu.icount;
    return n;
}
