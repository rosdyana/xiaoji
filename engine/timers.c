/* ITIMER_REAL and POSIX timers (B9).
 *
 * The clock is the engine's: host monotonic time for ITIMER_REAL, CLOCK_MONOTONIC and CLOCK_BOOTTIME, and
 * host realtime for CLOCK_REALTIME. The scheduler calls timers_expire on every pass (sched.c expire_timers):
 * a due timer raises its signal (SIGALRM for ITIMER_REAL, the sigevent's signal for a POSIX timer, to the
 * process or to one thread by tid), which wakes a blocked thread with EINTR as any other signal does. The
 * scheduler's wait ends at the earliest armed deadline (timers_next), so nothing spins.
 *
 * Rules (Linux 6.12, kernel/time/itimer.c and posix-timers.c, and timer_create(2)):
 * - setitimer and getitimer: ITIMER_REAL only. ITIMER_VIRTUAL and ITIMER_PROF give ENOSYS (not built).
 *   A value of zero disarms, an interval of zero makes a one-shot. A timeval with usec outside
 *   [0, 10^6) or a negative field is EINVAL.
 * - timer_create: CLOCK_REALTIME, CLOCK_MONOTONIC and CLOCK_BOOTTIME. SIGEV_SIGNAL, SIGEV_NONE and
 *   SIGEV_THREAD_ID (the kernel's; SIGEV_THREAD is for the library, and is EINVAL here, as on Linux). A
 *   NULL sigevent is SIGEV_SIGNAL with SIGALRM and the timer id as the value. The id is the table slot.
 * - timer_settime: TIMER_ABSTIME is the only flag. A value of zero disarms. It resets the overrun count.
 * - Overrun: when a timer fires and its signal is still pending on the target, the expirations are counted
 *   and no new signal is raised. When the signal was delivered, the count is the expirations this firing
 *   skipped. (Linux keeps the count for the last delivered signal; this engine counts at the firing.)
 * - A fork gets no timers (its timers are keyed by the owning process). An exec keeps ITIMER_REAL and
 *   drops the POSIX timers. A process's end drops all of them.
 */
#include "timers.h"

#include <string.h>

#include "host.h"
#include "linux.h"
#include "mem.h"
#include "proc.h"
#include "sched.h"
#include "signals.h"

#define TMR_MAX 128
#define TMR_ITIMER 0
#define TMR_POSIX 1

#define SIG_ALRM_N 14
#define SI_KERNEL_CODE 0x80
#define SI_TIMER_CODE (-2)
#define SIGEV_SIGNAL_N 0
#define SIGEV_NONE_N 1
#define SIGEV_THREAD_ID_N 4
#define TIMER_ABSTIME_F 1
#define CLOCK_REALTIME_N 0
#define CLOCK_MONOTONIC_N 1
#define CLOCK_BOOTTIME_N 7
#define NSEC 1000000000ull
#define USEC 1000ull
#define SIG_MAX_N 64
#define SAT_MAX (UINT64_MAX / 4)

typedef struct {
    int used;
    int kind;     /* TMR_ITIMER or TMR_POSIX */
    int real;     /* the clock is CLOCK_REALTIME */
    int armed;
    int notify;   /* SIGEV_* (ITIMER: the process) */
    int signo;
    int tid;      /* SIGEV_THREAD_ID: the target thread, else 0 */
    int id;       /* POSIX: the table slot, which is the timer id */
    int overrun;
    proc_t *owner;
    uint64_t sigval;   /* the sigevent value, passed in si_value */
    uint64_t when;     /* the expiry on its own clock, in ns */
    uint64_t interval; /* 0 for a one-shot */
} tmr_t;

static tmr_t tmr[TMR_MAX];

static uint64_t rd64(const uint8_t *b) { uint64_t v; memcpy(&v, b, 8); return v; }
static uint32_t rd32(const uint8_t *b) { uint32_t v; memcpy(&v, b, 4); return v; }

/* Saturating arithmetic: a huge guest time stays a far future, not a wrapped past. */
static uint64_t sat_add(uint64_t a, uint64_t b) { return a > SAT_MAX - b ? SAT_MAX : a + b; }
static uint64_t sat_ns(uint64_t sec, uint64_t frac, uint64_t unit)
{
    if (sec > SAT_MAX / NSEC)
        return SAT_MAX;
    return sat_add(sec * NSEC, frac * unit);
}

static uint64_t clk_now(const tmr_t *t) { return t->real ? host_realtime_ns() : host_monotonic_ns(); }

/* The host monotonic time at which t is due (the scheduler's deadline). A due timer reports now. */
static uint64_t mono_due(const tmr_t *t)
{
    uint64_t mo = host_monotonic_ns();
    if (!t->real)
        return t->when;
    uint64_t rt = host_realtime_ns();
    if (t->when <= rt)
        return mo;
    return sat_add(mo, t->when - rt);
}

static uint64_t remaining(const tmr_t *t)
{
    if (!t->armed)
        return 0;
    uint64_t now = clk_now(t);
    return t->when > now ? t->when - now : 0;
}

static tmr_t *itimer_of(proc_t *p)
{
    for (int i = 0; i < TMR_MAX; i++)
        if (tmr[i].used && tmr[i].kind == TMR_ITIMER && tmr[i].owner == p)
            return &tmr[i];
    return NULL;
}

static tmr_t *itimer_make(proc_t *p)
{
    for (int i = 0; i < TMR_MAX; i++) {
        if (tmr[i].used)
            continue;
        memset(&tmr[i], 0, sizeof tmr[i]);
        tmr[i].used = 1;
        tmr[i].kind = TMR_ITIMER;
        tmr[i].notify = SIGEV_SIGNAL_N;
        tmr[i].signo = SIG_ALRM_N;
        tmr[i].owner = p;
        tmr[i].id = i;
        return &tmr[i];
    }
    return NULL;
}

static tmr_t *posix_of(proc_t *p, uint64_t id)
{
    if (id >= TMR_MAX)
        return NULL;
    tmr_t *t = &tmr[id];
    return t->used && t->kind == TMR_POSIX && t->owner == p ? t : NULL;
}

/* Writes a timer's (interval, remaining) as two timevals (itimerval) or two timespecs (itimerspec). */
static int64_t put_pair(uint64_t va, uint64_t interval, uint64_t rem, int usec)
{
    uint64_t v[4];
    uint64_t unit = usec ? USEC : 1;
    v[0] = interval / NSEC;
    v[1] = (interval % NSEC) / unit;
    v[2] = rem / NSEC;
    v[3] = (rem % NSEC) / unit;
    return mem_copy_in(va, v, sizeof v) < 0 ? -LE_FAULT : 0;
}

/* Reads (interval, value) from a timeval pair (usec) or a timespec pair (nsec). Returns 0, or EINVAL. */
static int64_t get_pair(uint64_t va, uint64_t *interval, uint64_t *value, int usec)
{
    uint64_t v[4];
    if (mem_copy_out(v, va, sizeof v) < 0)
        return -LE_FAULT;
    uint64_t unit = usec ? USEC : 1;
    for (int k = 0; k < 4; k += 2) {
        if ((int64_t)v[k] < 0 || (int64_t)v[k + 1] < 0 || v[k + 1] >= (usec ? 1000000u : NSEC))
            return -LE_INVAL;
    }
    *interval = sat_ns(v[0], v[1], unit);
    *value = sat_ns(v[2], v[3], unit);
    return 0;
}

static void drop_timer(tmr_t *t)
{
    memset(t, 0, sizeof *t);
}

/* ---------- the scheduler's side ---------- */

static void fire(tmr_t *t, uint64_t now)
{
    uint64_t k = 1;
    if (t->interval) {
        uint64_t late = now - t->when;
        k = 1 + late / t->interval;
        t->when = sat_add(t->when, k * t->interval);
    } else {
        t->armed = 0;
    }
    if (t->notify == SIGEV_NONE_N)
        return;
    int tid = (t->kind == TMR_POSIX && t->notify == SIGEV_THREAD_ID_N) ? t->tid : 0;
    int sig = t->kind == TMR_ITIMER ? SIG_ALRM_N : t->signo;
    int code = t->kind == TMR_ITIMER ? SI_KERNEL_CODE : SI_TIMER_CODE;
    if (sig_timer_pending(t->owner, tid, sig)) {
        /* Still pending from an earlier firing: count the expirations and raise no second signal. */
        t->overrun = (int)(t->overrun + (int)(k > 0x7fffffff ? 0x7fffffff : k));
        return;
    }
    t->overrun = (int)((k - 1) > 0x7fffffff ? 0x7fffffff : (k - 1));
    (void)sig_timer_raise(t->owner, tid, sig, code, (uint32_t)t->id, t->sigval);
}

void timers_expire(void)
{
    for (int i = 0; i < TMR_MAX; i++) {
        tmr_t *t = &tmr[i];
        if (!t->used || !t->armed)
            continue;
        if (t->owner->zombie || t->owner->exited)
            continue; /* the process is ending: finish_process drops its timers */
        uint64_t now = clk_now(t);
        if (now >= t->when)
            fire(t, now);
    }
}

int timers_next(uint64_t *when)
{
    int any = 0;
    uint64_t best = 0;
    for (int i = 0; i < TMR_MAX; i++) {
        tmr_t *t = &tmr[i];
        if (!t->used || !t->armed)
            continue;
        uint64_t d = mono_due(t);
        if (!any || d < best)
            best = d;
        any = 1;
    }
    if (any)
        *when = best;
    return any;
}

void timers_proc_gone(struct proc *p)
{
    for (int i = 0; i < TMR_MAX; i++)
        if (tmr[i].used && tmr[i].owner == p)
            drop_timer(&tmr[i]);
}

void timers_proc_exec(struct proc *p)
{
    for (int i = 0; i < TMR_MAX; i++)
        if (tmr[i].used && tmr[i].kind == TMR_POSIX && tmr[i].owner == p)
            drop_timer(&tmr[i]);
}

/* ---------- ITIMER_REAL ---------- */

int64_t timers_setitimer(uint64_t which, uint64_t newp, uint64_t oldp)
{
    if (which != 0)
        return -LE_NOSYS; /* ITIMER_VIRTUAL, ITIMER_PROF: not built (B9) */
    proc_t *p = proc_cur();
    uint64_t interval = 0, value = 0;
    if (newp) {
        int64_t rc = get_pair(newp, &interval, &value, 1);
        if (rc < 0)
            return rc;
    }
    tmr_t *t = itimer_of(p);
    if (oldp) {
        int64_t rc = put_pair(oldp, t ? t->interval : 0, t ? remaining(t) : 0, 1);
        if (rc < 0)
            return rc;
    }
    if (!newp)
        return 0;
    if (!t && value) {
        t = itimer_make(p);
        if (!t)
            return -LE_NOMEM;
    }
    if (!t)
        return 0;
    t->interval = interval;
    t->overrun = 0;
    if (value) {
        t->when = sat_add(host_monotonic_ns(), value);
        t->armed = 1;
    } else {
        t->armed = 0;
    }
    return 0;
}

int64_t timers_getitimer(uint64_t which, uint64_t curp)
{
    if (which != 0)
        return -LE_NOSYS;
    tmr_t *t = itimer_of(proc_cur());
    return put_pair(curp, t ? t->interval : 0, t ? remaining(t) : 0, 1);
}

/* ---------- POSIX timers ---------- */

int64_t timers_create(uint64_t clk, uint64_t sevp, uint64_t idp)
{
    proc_t *p = proc_cur();
    if (clk != CLOCK_REALTIME_N && clk != CLOCK_MONOTONIC_N && clk != CLOCK_BOOTTIME_N)
        return -LE_INVAL;
    int notify = SIGEV_SIGNAL_N, signo = SIG_ALRM_N, tid = 0;
    uint64_t sigval = 0;
    int have_sigval = 0;
    if (sevp) {
        /* struct sigevent (arm64): sigev_value at 0, sigev_signo at 8, sigev_notify at 12, the union at 16. */
        uint8_t b[32];
        if (mem_copy_out(b, sevp, sizeof b) < 0)
            return -LE_FAULT;
        sigval = rd64(b);
        signo = (int)rd32(b + 8);
        notify = (int)rd32(b + 12);
        tid = (int)rd32(b + 16);
        have_sigval = 1;
        if (notify == SIGEV_SIGNAL_N || notify == SIGEV_THREAD_ID_N) {
            if (signo < 1 || signo > SIG_MAX_N)
                return -LE_INVAL;
        }
        if (notify == SIGEV_THREAD_ID_N) {
            cpu_t *list[p->nthreads ? p->nthreads : 1]; /* every thread of the process: no fixed cap (B16) */
            int n = sched_threads_of(p, list, p->nthreads ? p->nthreads : 1), found = 0;
            for (int i = 0; i < n; i++)
                if ((int)list[i]->tid == tid)
                    found = 1;
            if (!found)
                return -LE_INVAL;
        } else if (notify != SIGEV_SIGNAL_N && notify != SIGEV_NONE_N) {
            return -LE_INVAL;
        }
    }
    int slot = -1;
    for (int i = 0; i < TMR_MAX; i++)
        if (!tmr[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        return -LE_AGAIN;
    tmr_t *t = &tmr[slot];
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->kind = TMR_POSIX;
    t->real = clk == CLOCK_REALTIME_N;
    t->notify = notify;
    t->signo = signo;
    t->tid = notify == SIGEV_THREAD_ID_N ? tid : 0;
    t->owner = p;
    t->id = slot;
    t->sigval = have_sigval ? sigval : (uint64_t)slot;
    int32_t id32 = slot;
    if (mem_copy_in(idp, &id32, sizeof id32) < 0) {
        drop_timer(t);
        return -LE_FAULT;
    }
    return 0;
}

int64_t timers_settime(uint64_t id, uint64_t flags, uint64_t newp, uint64_t oldp)
{
    proc_t *p = proc_cur();
    if (flags & ~(uint64_t)TIMER_ABSTIME_F)
        return -LE_INVAL;
    tmr_t *t = posix_of(p, id);
    if (!t)
        return -LE_INVAL;
    uint64_t interval = 0, value = 0;
    int64_t rc = get_pair(newp, &interval, &value, 0);
    if (rc < 0)
        return rc;
    if (oldp) {
        rc = put_pair(oldp, t->interval, remaining(t), 0);
        if (rc < 0)
            return rc;
    }
    t->interval = interval;
    t->overrun = 0;
    if (!value) {
        t->armed = 0;
        return 0;
    }
    t->when = (flags & TIMER_ABSTIME_F) ? value : sat_add(clk_now(t), value);
    t->armed = 1;
    return 0;
}

int64_t timers_gettime(uint64_t id, uint64_t curp)
{
    tmr_t *t = posix_of(proc_cur(), id);
    if (!t)
        return -LE_INVAL;
    return put_pair(curp, t->interval, remaining(t), 0);
}

int64_t timers_getoverrun(uint64_t id)
{
    tmr_t *t = posix_of(proc_cur(), id);
    if (!t)
        return -LE_INVAL;
    return t->overrun;
}

int64_t timers_delete(uint64_t id)
{
    tmr_t *t = posix_of(proc_cur(), id);
    if (!t)
        return -LE_INVAL;
    drop_timer(t);
    return 0;
}
