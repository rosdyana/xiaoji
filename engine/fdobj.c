/* eventfd and timerfd objects (see fdobj.h). */
#include "fdobj.h"

#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "linux.h"
#include "poll.h"
#include "sched.h"

#define EFD_MAX 0xfffffffffffffffeull

typedef struct {
    uint64_t counter;
    int semaphore;
} efd_t;

typedef struct {
    int clockid;
    uint64_t next;     /* the next expiry (host ns); 0 when disarmed */
    uint64_t interval; /* 0 for a one-shot timer */
    uint64_t expired;  /* expirations not yet read */
} tfd_t;

uint64_t fdobj_now(void)
{
    return host_monotonic_ns();
}

static uint64_t key_of(const void *p)
{
    return (uint64_t)(uintptr_t)p;
}

/* ---------- eventfd ---------- */

void *efd_new(uint64_t init, int semaphore)
{
    efd_t *e = calloc(1, sizeof *e);
    if (e) {
        e->counter = init;
        e->semaphore = semaphore != 0;
    }
    return e;
}

void efd_free(void *o)
{
    free(o);
}

int efd_readable(void *o)
{
    return ((efd_t *)o)->counter > 0;
}

int efd_writable(void *o)
{
    return ((efd_t *)o)->counter < EFD_MAX;
}

/* Reads take 8 bytes: the counter (or 1 for a semaphore), which is then reduced. */
int64_t efd_read(void *o, void *buf, size_t n)
{
    efd_t *e = o;
    if (n < 8)
        return -LE_INVAL;
    if (e->counter == 0)
        return -LE_AGAIN;
    uint64_t v = e->semaphore ? 1 : e->counter;
    e->counter -= v;
    memcpy(buf, &v, 8);
    sched_wake_key(key_of(e)); /* a writer blocked on a full counter may go on */
    poll_notify();
    return 8;
}

int64_t efd_write(void *o, const void *buf, size_t n)
{
    efd_t *e = o;
    if (n < 8)
        return -LE_INVAL;
    uint64_t v;
    memcpy(&v, buf, 8);
    if (v == 0xffffffffffffffffull)
        return -LE_INVAL;
    if (v > EFD_MAX - e->counter)
        return -LE_AGAIN;
    e->counter += v;
    sched_wake_key(key_of(e));
    poll_notify();
    return 8;
}

/* ---------- timerfd ---------- */

void *tfd_new(int clockid)
{
    tfd_t *t = calloc(1, sizeof *t);
    if (t)
        t->clockid = clockid;
    return t;
}

void tfd_free(void *o)
{
    free(o);
}

/* Counts the expirations up to now and moves the timer to its next one. */
static void tfd_update(tfd_t *t, uint64_t now)
{
    if (!t->next || now < t->next)
        return;
    uint64_t n = 1;
    if (t->interval) {
        n = 1 + (now - t->next) / t->interval;
        t->next += n * t->interval;
    } else {
        t->next = 0;
    }
    t->expired += n;
    poll_notify();
}

int tfd_readable(void *o)
{
    tfd_t *t = o;
    tfd_update(t, fdobj_now());
    return t->expired > 0;
}

int tfd_deadline(void *o, uint64_t *dl)
{
    tfd_t *t = o;
    if (!t->next)
        return 0;
    *dl = t->next;
    return 1;
}

int64_t tfd_read(void *o, void *buf, size_t n)
{
    tfd_t *t = o;
    if (n < 8)
        return -LE_INVAL;
    tfd_update(t, fdobj_now());
    if (t->expired == 0)
        return -LE_AGAIN;
    memcpy(buf, &t->expired, 8);
    t->expired = 0;
    return 8;
}

void tfd_settime(void *o, int abs, uint64_t value_ns, uint64_t interval_ns)
{
    tfd_t *t = o;
    t->expired = 0;
    t->interval = interval_ns;
    if (value_ns == 0) {
        t->next = 0;
        t->interval = 0;
        return;
    }
    t->next = abs ? value_ns : fdobj_now() + value_ns;
    if (t->next == 0)
        t->next = 1; /* an absolute time of 0 still arms the timer */
    poll_notify();
}

void tfd_gettime(void *o, uint64_t *remain_ns, uint64_t *interval_ns)
{
    tfd_t *t = o;
    uint64_t now = fdobj_now();
    tfd_update(t, now);
    *remain_ns = t->next > now ? t->next - now : 0;
    *interval_ns = t->interval;
}
