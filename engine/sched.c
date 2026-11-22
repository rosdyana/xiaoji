/* Green threads. Each guest thread has its own cpu_t. The scheduler runs the
 * current thread for SCHED_SLICE instructions, then the next runnable thread.
 * A thread also gives up the host when it blocks on a futex or sleeps, or when
 * it exits. Futex waits and sleeps have deadlines (host monotonic time). When
 * every live thread is blocked, the host sleeps until the earliest deadline. If
 * none has one, the scheduler reports a deadlock. */
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

#define MAX_THREADS SCHED_MAX_THREADS
#define TID_MAIN 1000u
#define TID_FIRST_NEW 1001u
#define DEADLOCK_STATUS 134
#define KEY_MASK 0x00FFFFFFFFFFFFFFull /* futex keys ignore the address tag */

#define CLONE_VM 0x00000100ull
#define CLONE_THREAD 0x00010000ull
#define CLONE_SETTLS 0x00080000ull
#define CLONE_PARENT_SETTID 0x00100000ull
#define CLONE_CHILD_CLEARTID 0x00200000ull
#define CLONE_CHILD_SETTID 0x01000000ull

typedef enum { TS_RUNNABLE, TS_FUTEX, TS_SLEEP, TS_SIGWAIT } thr_state;

typedef struct thread {
    cpu_t cpu;
    thr_state state;
    uint64_t key;        /* futex key while blocked on a futex */
    uint32_t bitset;     /* wake mask of a FUTEX_WAIT_BITSET */
    int has_deadline;
    uint64_t deadline;   /* host monotonic ns */
    uint64_t clear_child_tid;
    char comm[16];
    proc_t *proc; /* the process this thread belongs to */
} thread_t;

/* sched_wait_kind and sched_wake take a cpu_t pointer and cast it to thread_t. */
_Static_assert(offsetof(thread_t, cpu) == 0, "thread_t must start with its cpu_t");

static thread_t *threads[MAX_THREADS];
static int cur_slot;          /* slot of the running thread */
static proc_t *init_proc; /* the initial process: its exit ends the engine */
static uint64_t next_tid = TID_FIRST_NEW;
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

static void block_current(thr_state st, uint64_t key, uint32_t bitset, int has_deadline, uint64_t deadline)
{
    thread_t *t = cur();
    t->state = st;
    t->key = key & KEY_MASK;
    t->bitset = bitset;
    t->has_deadline = has_deadline;
    t->deadline = deadline;
    t->cpu.blocked = 1;
    t->cpu.stop = 1;
}

void sched_block_futex(uint64_t key, uint32_t bitset, int has_deadline, uint64_t deadline)
{
    block_current(TS_FUTEX, key, bitset, has_deadline, deadline);
}

void sched_sleep_until(uint64_t deadline)
{
    block_current(TS_SLEEP, 0, 0, 1, deadline);
}

void sched_block_sigwait(int has_deadline, uint64_t deadline)
{
    block_current(TS_SIGWAIT, 0, 0, has_deadline, deadline);
}

sched_wait_t sched_wait_kind(const cpu_t *c)
{
    /* cpu_t is the first member of thread_t, so the pointer is the thread's. */
    const thread_t *t = (const thread_t *)c;
    switch (t->state) {
    case TS_FUTEX: return SW_FUTEX;
    case TS_SLEEP: return SW_SLEEP;
    case TS_SIGWAIT: return SW_SIGWAIT;
    default: return SW_NONE;
    }
}

void sched_wake(cpu_t *c, int64_t ret)
{
    thread_t *t = (thread_t *)c;
    if (t->state != TS_RUNNABLE)
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

cpu_t *sched_find_thread(uint64_t tid)
{
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i] && threads[i]->cpu.tid == tid)
            return &threads[i]->cpu;
    return NULL;
}

int64_t sched_futex_wake(uint64_t key, uint64_t count, uint32_t bitset)
{
    int64_t n = 0;
    key &= KEY_MASK;
    for (int i = 0; i < MAX_THREADS && (uint64_t)n < count; i++) {
        thread_t *t = threads[i];
        if (t && t->state == TS_FUTEX && t->key == key && (t->bitset & bitset)) {
            wake(t, 0);
            n++;
        }
    }
    return n;
}

int64_t sched_futex_requeue(uint64_t from, uint64_t to, uint64_t wake_n, uint64_t requeue_n)
{
    int64_t woke = 0;
    uint64_t moved = 0;
    from &= KEY_MASK;
    to &= KEY_MASK;
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t || t->state != TS_FUTEX || t->key != from)
            continue;
        if ((uint64_t)woke < wake_n) {
            wake(t, 0);
            woke++;
        } else if (moved < requeue_n) {
            t->key = to;
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

static void report_deadlock(void)
{
    fflush(stdout);
    fprintf(stderr, "xiaoji: deadlock: every thread is blocked and none can wake\n");
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t)
            continue;
        fprintf(stderr, "xiaoji:   tid %llu %s pc 0x%llx\n", (unsigned long long)t->cpu.tid,
                t->state == TS_FUTEX      ? "blocked on futex"
                : t->state == TS_SLEEP    ? "sleeping"
                : t->state == TS_SIGWAIT  ? "waiting in sigtimedwait"
                                          : "runnable",
                (unsigned long long)t->cpu.pc);
    }
}

int sched_run(cpu_t *main_cpu)
{
    thread_t *m = add_thread(main_cpu, TID_MAIN, proc_cur());
    if (!m) {
        fprintf(stderr, "xiaoji: out of host memory\n");
        return 1;
    }
    memcpy(m->comm, init_name, sizeof m->comm);
    if (prof_on)
        prof_thread_new(TID_MAIN, m->comm);
    cur_slot = 0;
    init_proc = proc_cur();
    init_proc->exited = 0;

    while (!init_proc->exited) {
        expire_timers(host_monotonic_ns());
        thread_t *t = pick();
        if (!t) {
            if (!wait_for_timers()) {
                report_deadlock();
                init_proc->exit_code = DEADLOCK_STATUS;
                break;
            }
            continue;
        }
        if (t->proc != proc_cur())
            proc_switch(t->proc); /* another process: its address space and files */
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
        if (t->cpu.exited) {
            if (live_count() == 1) {
                /* The last thread: its status is the process status. */
                t->proc->exit_code = t->cpu.exit_code;
                t->proc->exited = 1;
            }
            reap(cur_slot);
        }
    }
    /* Unreaped threads (the process ended while they lived) add their counts. */
    return init_proc->exit_code;
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

void sched_thread_exit(cpu_t *c, uint64_t code)
{
    thread_t *t = cur();
    c->exit_code = (int)(code & 0xFF);
    c->exited = 1;
    c->stop = 1;
    if (t && t->clear_child_tid) {
        /* A thread-exit clear must not kill the process: ignore a bad address. */
        uint8_t zero[4] = {0, 0, 0, 0};
        if (mem_copy_in(t->clear_child_tid, zero, sizeof zero) == 0)
            sched_futex_wake(t->clear_child_tid, 1, 0xFFFFFFFFu);
        t->clear_child_tid = 0;
    }
}

int64_t sched_clone(cpu_t *parent, uint64_t flags, uint64_t newsp, uint64_t ptid, uint64_t tls, uint64_t ctid)
{
    thread_t *me = cur();
    if (!(flags & CLONE_VM) || !(flags & CLONE_THREAD)) {
        static int notice;
        if (!notice) {
            notice = 1;
            fprintf(stderr, "xiaoji: process creation (clone without CLONE_THREAD) is not supported yet\n");
        }
        return -LE_NOSYS;
    }
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

uint64_t sched_icount(void)
{
    uint64_t n = dead_icount;
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i])
            n += threads[i]->cpu.icount;
    return n;
}
