/* Readiness and waiting: ppoll and level-triggered epoll (see poll.h). */
#include "poll.h"

#include <stdlib.h>
#include <string.h>

#include "binder.h"
#include "fdobj.h"
#include "goldfish.h"
#include "linux.h"
#include "mem.h"
#include "pipe.h"
#include "sched.h"

#define MAX_POLL_FDS 1024
#define MAX_EPOLL_EVENTS 4096

/* One key for every poll waiter: the address of this byte. */
static uint8_t notify_anchor;

static uint64_t notify_key(void)
{
    return (uint64_t)(uintptr_t)&notify_anchor;
}

void poll_notify(void)
{
    sched_wake_key(notify_key());
}

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t ld64(const uint8_t *p)
{
    return ld32(p) | ((uint64_t)ld32(p + 4) << 32);
}

static void st32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void st64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

uint32_t poll_revents(vfs_fd *f, uint32_t events)
{
    uint32_t r = 0;
    switch (f->kind) {
    case VFS_PIPE: {
        int readable, writable, readers, writers;
        pipe_state(f->pipe, &readable, &writable, &readers, &writers);
        if ((f->lflags & LO_ACCMODE) == LO_RDONLY) {
            if (readable)
                r |= PX_IN;
            if (!writers)
                r |= PX_HUP;
        } else {
            if (writable)
                r |= PX_OUT;
            if (!readers)
                r |= PX_ERR;
        }
        break;
    }
    case VFS_BINDER:
        if (binder_readable(f->binder))
            r |= PX_IN;
        r |= PX_OUT;
        break;
    case VFS_EVENTFD:
        if (efd_readable(f->obj))
            r |= PX_IN;
        if (efd_writable(f->obj))
            r |= PX_OUT;
        break;
    case VFS_GOLDFISH:
        if (goldfish_readable(f->obj))
            r |= PX_IN;
        r |= PX_OUT;
        break;
    case VFS_TIMERFD:
        if (tfd_readable(f->obj))
            r |= PX_IN;
        break;
    case VFS_SOCK: {
        int readable, writable, hup;
        sock_state(f->sock, &readable, &writable, &hup);
        if (readable)
            r |= PX_IN;
        if (writable)
            r |= PX_OUT;
        if (hup)
            r |= PX_HUP;
        break;
    }
    case VFS_EPOLL:
        break; /* an epoll fd is not itself polled (nesting is not modelled) */
    case VFS_INOTIFY:
        break; /* no event is ever queued (inotify.c): never readable */
    case VFS_DIR:
        r |= PX_IN;
        break;
    default:
        /* Regular files, memfds and synthetic files (their reads never block). */
        r |= PX_IN | PX_OUT;
        break;
    }
    return r & (events | PX_ERR | PX_HUP | PX_NVAL);
}

uint64_t poll_timer_deadline(const vfs_fd *f)
{
    uint64_t dl = 0;
    if (f && f->kind == VFS_TIMERFD && tfd_deadline(f->obj, &dl))
        return dl;
    if (f && f->kind == VFS_GOLDFISH && goldfish_deadline(f->obj, &dl))
        return dl;
    return 0;
}

/* ---------- waiting ---------- */

/* A timed wait keeps its absolute deadline here, so the rerun of the call does not
 * restart the timeout. */
typedef struct {
    uint64_t tid;
    uint64_t deadline;
    int active;
} waiter_t;

static waiter_t waiters[SCHED_MAX_THREADS];

static waiter_t *waiter_find(uint64_t tid)
{
    for (int i = 0; i < SCHED_MAX_THREADS; i++)
        if (waiters[i].active && waiters[i].tid == tid)
            return &waiters[i];
    return NULL;
}

static waiter_t *waiter_new(uint64_t tid, uint64_t deadline)
{
    for (int i = 0; i < SCHED_MAX_THREADS; i++)
        if (!waiters[i].active) {
            waiters[i].active = 1;
            waiters[i].tid = tid;
            waiters[i].deadline = deadline;
            return &waiters[i];
        }
    return NULL;
}

/* The end of a poll-like call that found ready fds. Returns 1 when the thread must wait:
 * the call then runs again on a notify or at the deadline. Otherwise 0, with *ret set.
 * timer_dl is the earliest timerfd expiry among the watched fds (0 for none). */
static int poll_finish(int64_t ready, int has_tmo, uint64_t tmo_ns, uint64_t timer_dl, int64_t *ret)
{
    uint64_t tid = sched_tid();
    waiter_t *w = waiter_find(tid);
    if (ready > 0 || (has_tmo && tmo_ns == 0)) {
        if (w)
            w->active = 0;
        *ret = ready;
        return 0;
    }
    uint64_t now = fdobj_now();
    int has_dl = 0;
    uint64_t dl = 0;
    if (has_tmo) {
        if (!w) {
            w = waiter_new(tid, now + tmo_ns);
            if (!w) {
                *ret = -LE_NOMEM;
                return 0;
            }
        }
        if (now >= w->deadline) {
            w->active = 0;
            *ret = 0;
            return 0;
        }
        has_dl = 1;
        dl = w->deadline;
    } else if (w) {
        w->active = 0;
    }
    if (timer_dl && (!has_dl || timer_dl < dl)) {
        has_dl = 1;
        dl = timer_dl;
    }
    sched_block_restart_until(notify_key(), has_dl, dl);
    return 1;
}

/* ---------- ppoll ---------- */

int64_t poll_ppoll(uint64_t fds, uint64_t nfds, uint64_t tmo, uint64_t sigmask, uint64_t sigsize)
{
    /* sigmask is not applied: no signal is held back while waiting (see docs/boot.md). */
    (void)sigmask;
    (void)sigsize;
    if (nfds > MAX_POLL_FDS)
        return -LE_INVAL;
    int has_tmo = tmo != 0;
    uint64_t tmo_ns = 0;
    if (has_tmo) {
        uint8_t ts[16];
        if (mem_copy_out(ts, tmo, 16) < 0)
            return -LE_FAULT;
        int64_t s = (int64_t)ld64(ts), ns = (int64_t)ld64(ts + 8);
        if (s < 0 || ns < 0 || ns >= 1000000000)
            return -LE_INVAL;
        if (s > 1000000000)
            s = 1000000000;
        tmo_ns = (uint64_t)s * 1000000000ull + (uint64_t)ns;
    }
    uint8_t *raw = malloc(nfds ? nfds * 8 : 1);
    if (!raw)
        return -LE_NOMEM;
    if (nfds && mem_copy_out(raw, fds, nfds * 8) < 0) {
        free(raw);
        return -LE_FAULT;
    }
    int64_t ready = 0;
    uint64_t timer_dl = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        uint8_t *p = raw + 8 * i;
        int fd = (int)ld32(p);
        uint32_t events = (uint32_t)p[4] | ((uint32_t)p[5] << 8);
        uint32_t rev = 0;
        if (fd >= 0) {
            vfs_fd *d = vfs_get(fd);
            if (!d) {
                rev = PX_NVAL;
            } else {
                rev = poll_revents(d, events);
                uint64_t dl = poll_timer_deadline(d);
                if (dl && (!timer_dl || dl < timer_dl))
                    timer_dl = dl;
            }
        }
        p[6] = (uint8_t)rev;
        p[7] = (uint8_t)(rev >> 8);
        if (rev)
            ready++;
    }
    int bad = nfds && mem_copy_in(fds, raw, nfds * 8) < 0;
    free(raw);
    if (bad)
        return -LE_FAULT;
    int64_t ret;
    if (poll_finish(ready, has_tmo, tmo_ns, timer_dl, &ret))
        return 0; /* blocked: the return value is not used */
    return ret;
}

/* ---------- epoll ---------- */

typedef struct {
    int fd;
    vfs_fd *desc; /* the description watched; a closed or reused fd drops the item */
    uint32_t events;
    uint64_t data;
} epitem_t;

typedef struct {
    epitem_t *items;
    int n, cap;
} epoll_t;

void *poll_epoll_new(void)
{
    return calloc(1, sizeof(epoll_t));
}

void poll_epoll_free(void *o)
{
    if (!o)
        return;
    free(((epoll_t *)o)->items);
    free(o);
}

static void epitem_remove(epoll_t *ep, int i)
{
    memmove(&ep->items[i], &ep->items[i + 1], (size_t)(ep->n - i - 1) * sizeof(epitem_t));
    ep->n--;
}

int64_t poll_epoll_ctl(int epfd, int op, int fd, uint64_t ev_va)
{
    vfs_fd *e = vfs_get(epfd);
    if (!e)
        return -LE_BADF;
    if (e->kind != VFS_EPOLL)
        return -LE_INVAL;
    vfs_fd *d = vfs_get(fd);
    if (!d)
        return -LE_BADF;
    if (d == e)
        return -LE_INVAL;
    uint32_t events = 0;
    uint64_t data = 0;
    if (op != 2) { /* ADD and MOD take an event: u32 events, then the u64 data (16 bytes) */
        uint8_t ev[16];
        if (!ev_va || mem_copy_out(ev, ev_va, 16) < 0)
            return -LE_FAULT;
        events = ld32(ev);
        data = ld64(ev + 8);
    }
    epoll_t *ep = e->obj;
    int idx = -1;
    for (int i = 0; i < ep->n; i++)
        if (ep->items[i].fd == fd && ep->items[i].desc == d) {
            idx = i;
            break;
        }
    switch (op) {
    case 1: /* EPOLL_CTL_ADD */
        if (idx >= 0)
            return -LE_EXIST;
        if (ep->n == ep->cap) {
            int cap = ep->cap ? ep->cap * 2 : 8;
            epitem_t *ni = realloc(ep->items, (size_t)cap * sizeof *ni);
            if (!ni)
                return -LE_NOMEM;
            ep->items = ni;
            ep->cap = cap;
        }
        ep->items[ep->n].fd = fd;
        ep->items[ep->n].desc = d;
        ep->items[ep->n].events = events;
        ep->items[ep->n].data = data;
        ep->n++;
        return 0;
    case 3: /* EPOLL_CTL_MOD */
        if (idx < 0)
            return -LE_NOENT;
        ep->items[idx].events = events;
        ep->items[idx].data = data;
        return 0;
    case 2: /* EPOLL_CTL_DEL */
        if (idx < 0)
            return -LE_NOENT;
        epitem_remove(ep, idx);
        return 0;
    default:
        return -LE_INVAL;
    }
}

int64_t poll_epoll_wait(int epfd, uint64_t events_va, int64_t maxevents, int64_t timeout_ms)
{
    vfs_fd *e = vfs_get(epfd);
    if (!e)
        return -LE_BADF;
    if (e->kind != VFS_EPOLL)
        return -LE_INVAL;
    if (maxevents <= 0 || maxevents > MAX_EPOLL_EVENTS)
        return -LE_INVAL;
    epoll_t *ep = e->obj;
    uint8_t *out = malloc((size_t)maxevents * 16);
    if (!out)
        return -LE_NOMEM;
    int64_t ready = 0, written = 0;
    uint64_t timer_dl = 0;
    for (int i = 0; i < ep->n;) {
        epitem_t *it = &ep->items[i];
        vfs_fd *d = vfs_get(it->fd);
        if (d != it->desc) {
            epitem_remove(ep, i);
            continue;
        }
        uint64_t dl = poll_timer_deadline(d);
        if (dl && (!timer_dl || dl < timer_dl))
            timer_dl = dl;
        uint32_t rev = poll_revents(d, it->events);
        if (rev) {
            if (written < maxevents) {
                st32(out + 16 * written, rev);
                st64(out + 16 * written + 8, it->data);
                written++;
            }
            ready++;
        }
        i++;
    }
    int bad = written && mem_copy_in(events_va, out, (size_t)written * 16) < 0;
    free(out);
    if (bad)
        return -LE_FAULT;
    int has_tmo = timeout_ms >= 0;
    uint64_t tmo_ns = has_tmo ? (uint64_t)timeout_ms * 1000000ull : 0;
    int64_t ret;
    if (poll_finish(written, has_tmo, tmo_ns, timer_dl, &ret))
        return 0;
    (void)ready;
    return ret;
}
