/* POSIX record locks and flock(2) (locks.h). The rules are Linux 6.12 fs/locks.c:
 *
 * - posix_lock_inode: the same-owner walk that merges adjacent or overlapping locks of one type,
 *   and splits or replaces locks of another type. The list order is kept as Linux keeps it (a new
 *   lock goes before the first lock of its owner that lies after it, else at the tail), because
 *   F_GETLK reports the first conflicting lock in list order.
 * - posix_locks_conflict and locks_conflict: a read lock conflicts only with a write lock, and no
 *   lock conflicts with a lock of the same owner.
 * - posix_locks_deadlock: the waiter walk (at most 10 steps) before a thread may block with EDEADLK.
 * - flock_lock_inode: a lock of the same description is replaced (its old lock goes before the
 *   conflict test, so a failed upgrade drops the old lock, as on Linux).
 *
 * An inode is named by (dev, ino) of the host file (the identity meta.c uses), or by the object of a
 * memfd or a pipe. Each inode has one record with both lists. Blocking is a restart on the record:
 * any change on the inode wakes its waiters, and each rerun repeats the request from the start. A
 * POSIX waiter keeps an edge to the lock it waits for (the deadlock walk follows it). The edge is
 * dropped at every rerun, and when its thread is gone (the thread is found by its tid). */

#include "locks.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "linux.h"
#include "sched.h"
#include "signals.h"

#define LK_BUCKETS 64
#define LK_MAX_DEADLK 10
#define LK_DEV_MEMFD (1ull << 62)
#define LK_DEV_PIPE ((1ull << 62) | 1)
#define LK_DEV_DESC ((1ull << 62) | 2)

typedef struct lk {
    struct lk *prev, *next;
    void *owner;  /* POSIX: the fd table; flock: unused (the description is file) */
    vfs_fd *file; /* flock: the open file description; NULL for POSIX */
    int pid;
    int type;
    int64_t start, end; /* inclusive */
} lk_t;

typedef struct lkino {
    uint64_t dev, ino;
    lk_t *posix, *flock;
    struct lkino *next;
} lkino_t;

typedef struct waiter {
    struct waiter *next;
    uint64_t tid;
    void *owner;
    lk_t *blocker; /* the POSIX lock this thread waits for (NULL once that lock is gone) */
} waiter_t;

static lkino_t *inodes[LK_BUCKETS];
static waiter_t *waiters;

static void die_oom(void)
{
    fputs("xiaoji: out of host memory\n", stderr);
    exit(1);
}

/* ---------- identities ---------- */

/* The key of a POSIX-lockable descriptor. Returns 0 for kinds that cannot hold POSIX locks. */
static int posix_key(const vfs_fd *f, uint64_t *dev, uint64_t *ino)
{
    switch (f->kind) {
    case VFS_FILE:
        if (!f->has_id)
            return 0;
        *dev = f->hdev;
        *ino = f->hino;
        return 1;
    case VFS_MEMFD:
        *dev = LK_DEV_MEMFD;
        *ino = (uint64_t)(uintptr_t)f->mfd;
        return 1;
    case VFS_PIPE:
        *dev = LK_DEV_PIPE;
        *ino = f->ino;
        return 1;
    default:
        return 0;
    }
}

/* flock works on every descriptor. A kind with no inode is keyed by its description. */
static void flock_key(const vfs_fd *f, uint64_t *dev, uint64_t *ino)
{
    if (posix_key(f, dev, ino))
        return;
    *dev = LK_DEV_DESC;
    *ino = (uint64_t)(uintptr_t)f;
}

static unsigned bucket_of(uint64_t dev, uint64_t ino)
{
    uint64_t h = dev * 0x9E3779B97F4A7C15ull ^ (ino + 0x632BE59BD9B4E019ull);
    h ^= h >> 29;
    return (unsigned)(h % LK_BUCKETS);
}

static lkino_t *ino_find(uint64_t dev, uint64_t ino)
{
    for (lkino_t *r = inodes[bucket_of(dev, ino)]; r; r = r->next)
        if (r->dev == dev && r->ino == ino)
            return r;
    return NULL;
}

static lkino_t *ino_get(uint64_t dev, uint64_t ino)
{
    lkino_t *r = ino_find(dev, ino);
    if (r)
        return r;
    r = calloc(1, sizeof *r);
    if (!r)
        die_oom();
    r->dev = dev;
    r->ino = ino;
    unsigned b = bucket_of(dev, ino);
    r->next = inodes[b];
    inodes[b] = r;
    return r;
}

static void ino_free_if_empty(lkino_t *ir)
{
    if (ir->posix || ir->flock)
        return;
    lkino_t **p = &inodes[bucket_of(ir->dev, ir->ino)];
    while (*p && *p != ir)
        p = &(*p)->next;
    if (*p)
        *p = ir->next;
    free(ir);
}

/* Wakes every thread parked on the inode (they rerun their request). Must come before the record
 * is freed, since the record's address is the wake key. */
static void ino_wake(lkino_t *ir)
{
    sched_wake_key((uint64_t)(uintptr_t)ir);
}

/* ---------- list and node helpers ---------- */

static lk_t *lk_new(void *owner, int pid, int type, int64_t start, int64_t end)
{
    lk_t *n = calloc(1, sizeof *n);
    if (!n)
        die_oom();
    n->owner = owner;
    n->pid = pid;
    n->type = type;
    n->start = start;
    n->end = end;
    return n;
}

/* Inserts n before pos in the list at *head, or at the tail when pos is NULL (Linux's
 * locks_insert_lock_ctx with the list head as the anchor). */
static void list_insert(lk_t **head, lk_t *n, lk_t *pos)
{
    if (!pos) {
        lk_t *t = *head;
        n->next = NULL;
        if (!t) {
            n->prev = NULL;
            *head = n;
            return;
        }
        while (t->next)
            t = t->next;
        t->next = n;
        n->prev = t;
        return;
    }
    n->next = pos;
    n->prev = pos->prev;
    if (pos->prev)
        pos->prev->next = n;
    else
        *head = n;
    pos->prev = n;
}

static void lk_unlink_free(lk_t **head, lk_t *n)
{
    if (n->prev)
        n->prev->next = n->next;
    else
        *head = n->next;
    if (n->next)
        n->next->prev = n->prev;
    for (waiter_t *w = waiters; w; w = w->next)
        if (w->blocker == n)
            w->blocker = NULL;
    free(n);
}

/* ---------- waiters (the deadlock edges) ---------- */

static void waiter_drop_self(void)
{
    uint64_t tid = sched_tid();
    for (waiter_t **p = &waiters; *p;) {
        if ((*p)->tid == tid) {
            waiter_t *w = *p;
            *p = w->next;
            free(w);
            return;
        }
        p = &(*p)->next;
    }
}

/* Drops the records of threads that no longer exist. */
static void waiters_prune(void)
{
    for (waiter_t **p = &waiters; *p;) {
        if (!sched_find_thread((*p)->tid)) {
            waiter_t *w = *p;
            *p = w->next;
            free(w);
        } else {
            p = &(*p)->next;
        }
    }
}

static void waiter_add(void *owner, lk_t *blocker)
{
    waiter_t *w = calloc(1, sizeof *w);
    if (!w)
        die_oom();
    w->tid = sched_tid();
    w->owner = owner;
    w->blocker = blocker;
    w->next = waiters;
    waiters = w;
}

/* what_owner_is_waiting_for: the lock that some waiter of blocker's owner waits for, or NULL. */
static lk_t *waiting_for(const lk_t *blocker)
{
    for (waiter_t *w = waiters; w; w = w->next)
        if (w->blocker && w->owner == blocker->owner)
            return w->blocker;
    return NULL;
}

/* posix_locks_deadlock: 1 when a thread that owner would wait for (through the chain of waiters)
 * is owned by owner itself. */
static int posix_deadlock(void *caller, lk_t *block)
{
    lk_t *b = block;
    int i = 0;
    waiters_prune();
    for (;;) {
        lk_t *root = waiting_for(b);
        if (!root)
            return 0;
        if (i++ > LK_MAX_DEADLK)
            return 0;
        if (root->owner == caller)
            return 1;
        b = root;
    }
}

/* ---------- POSIX locks ---------- */

/* posix_locks_conflict: another owner's overlapping lock, unless both are read locks. */
static int posix_conflicts(const void *owner, int type, int64_t s, int64_t e, const lk_t *l)
{
    if (l->owner == owner)
        return 0;
    if (!(e >= l->start && l->end >= s))
        return 0;
    return type == LK_WR || l->type == LK_WR;
}

/* posix_lock_inode, the part that changes the list (the conflict test is the caller's). The same
 * steps, in the same order, as Linux: see the file comment. Locals rs and re are the request's range
 * (Linux's request struct); keep is the list lock the request became (Linux's request pointer). */
static void posix_apply(lkino_t *ir, void *owner, int pid, int rt, int64_t rs, int64_t re)
{
    lk_t *first = NULL, *cur, *tmp, *keep = NULL, *left = NULL, *right = NULL;
    int added = 0;

    for (cur = ir->posix; cur; cur = cur->next)
        if (cur->owner == owner) {
            first = cur;
            break;
        }

    for (cur = first; cur && cur->owner == owner; cur = tmp) {
        tmp = cur->next;
        if (rt == cur->type) {
            /* Same type: merge when adjacent or overlapping (start - 1 keeps end + 1 from overflowing). */
            if (cur->end < rs - 1)
                continue;
            if (cur->start - 1 > re)
                break;
            if (cur->start > rs)
                cur->start = rs;
            else
                rs = cur->start;
            if (cur->end < re)
                cur->end = re;
            else
                re = cur->end;
            if (added) {
                lk_unlink_free(&ir->posix, cur);
                continue;
            }
            keep = cur;
            added = 1;
        } else {
            /* Another type: split, or replace, or drop (an unlock). */
            if (cur->end < rs)
                continue;
            if (cur->start > re)
                break;
            if (rt == LK_UN)
                added = 1;
            if (cur->start < rs)
                left = cur;
            if (cur->end > re) {
                right = cur;
                break;
            }
            if (cur->start >= rs) {
                if (added) {
                    lk_unlink_free(&ir->posix, cur);
                    continue;
                }
                lk_t *n = lk_new(owner, pid, rt, rs, re);
                list_insert(&ir->posix, n, cur);
                lk_unlink_free(&ir->posix, cur);
                keep = n;
                added = 1;
            }
        }
    }

    /* cur is now the list element where the loop stopped (NULL: the tail). */
    if (!added) {
        if (rt == LK_UN)
            return;
        keep = lk_new(owner, pid, rt, rs, re);
        list_insert(&ir->posix, keep, cur);
        added = 1;
    }
    if (right) {
        if (left == right) {
            /* The request splits right in two: the lower part is a copy, inserted before right. */
            left = lk_new(right->owner, right->pid, right->type, right->start, right->end);
            list_insert(&ir->posix, left, right);
        }
        right->start = re + 1;
    }
    if (left)
        left->end = rs - 1;
    if (keep) {
        keep->start = rs;
        keep->end = re;
    }
}

int lk_posix_test(void *owner, vfs_fd *f, int type, int64_t start, int64_t end, lk_conflict_t *out)
{
    uint64_t dev, ino;
    if (!posix_key(f, &dev, &ino))
        return -LE_INVAL;
    lkino_t *ir = ino_find(dev, ino);
    if (!ir)
        return 0;
    for (lk_t *l = ir->posix; l; l = l->next)
        if (posix_conflicts(owner, type, start, end, l)) {
            out->type = l->type;
            out->pid = l->pid;
            out->start = l->start;
            out->end = l->end;
            return 1;
        }
    return 0;
}

int lk_posix_set(void *owner, int pid, vfs_fd *f, int type, int64_t start, int64_t end, int wait)
{
    uint64_t dev, ino;
    if (!posix_key(f, &dev, &ino))
        return -LE_INVAL;
    waiter_drop_self(); /* a rerun starts from scratch; the old edge is stale */

    if (type == LK_UN) {
        lkino_t *ir = ino_find(dev, ino);
        if (!ir)
            return 0;
        posix_apply(ir, owner, pid, LK_UN, start, end);
        ino_wake(ir);
        ino_free_if_empty(ir);
        return 0;
    }

    lkino_t *ir = ino_get(dev, ino);
    lk_t *cf = NULL;
    for (lk_t *l = ir->posix; l; l = l->next)
        if (posix_conflicts(owner, type, start, end, l)) {
            cf = l;
            break;
        }
    if (cf) {
        if (!wait) {
            ino_free_if_empty(ir);
            return -LE_AGAIN;
        }
        if (posix_deadlock(owner, cf)) {
            ino_free_if_empty(ir);
            return -LE_DEADLK;
        }
        if (sig_interrupt_pending(sched_cur_cpu())) {
            ino_free_if_empty(ir);
            return -LE_INTR;
        }
        waiter_add(owner, cf);
        sched_block_restart_eintr((uint64_t)(uintptr_t)ir);
        return LK_BLOCKED;
    }
    posix_apply(ir, owner, pid, type, start, end);
    ino_wake(ir);
    ino_free_if_empty(ir);
    return 0;
}

void lk_posix_close(void *owner, vfs_fd *f)
{
    uint64_t dev, ino;
    if (!posix_key(f, &dev, &ino))
        return;
    lkino_t *ir = ino_find(dev, ino);
    if (!ir)
        return;
    int changed = 0;
    for (lk_t *l = ir->posix, *n; l; l = n) {
        n = l->next;
        if (l->owner == owner) {
            lk_unlink_free(&ir->posix, l);
            changed = 1;
        }
    }
    if (changed)
        ino_wake(ir);
    ino_free_if_empty(ir);
}

void lk_posix_owner_release(void *owner)
{
    for (unsigned b = 0; b < LK_BUCKETS; b++) {
        lkino_t *next;
        for (lkino_t *ir = inodes[b]; ir; ir = next) {
            next = ir->next;
            int changed = 0;
            for (lk_t *l = ir->posix, *n; l; l = n) {
                n = l->next;
                if (l->owner == owner) {
                    lk_unlink_free(&ir->posix, l);
                    changed = 1;
                }
            }
            if (changed)
                ino_wake(ir);
            ino_free_if_empty(ir);
        }
    }
    for (waiter_t **p = &waiters; *p;) {
        if ((*p)->owner == owner) {
            waiter_t *w = *p;
            *p = w->next;
            free(w);
        } else {
            p = &(*p)->next;
        }
    }
}

/* ---------- flock ---------- */

/* flock_locks_conflict: another description's lock, unless both are shared. */
static int flock_conflicts(const vfs_fd *f, int type, const lk_t *l)
{
    if (l->file == f)
        return 0;
    return type == LK_WR || l->type == LK_WR;
}

int lk_flock(vfs_fd *f, int type, int wait)
{
    uint64_t dev, ino;
    flock_key(f, &dev, &ino);
    lkino_t *ir = type == LK_UN ? ino_find(dev, ino) : ino_get(dev, ino);
    if (!ir)
        return 0;

    for (lk_t *l = ir->flock; l; l = l->next) {
        if (l->file != f)
            continue;
        if (l->type == type)
            return 0; /* the same lock again */
        /* A different type: the old lock goes before the conflict test (as Linux does). */
        lk_unlink_free(&ir->flock, l);
        ino_wake(ir);
        break;
    }
    if (type == LK_UN) {
        ino_free_if_empty(ir);
        return 0;
    }

    for (lk_t *l = ir->flock; l; l = l->next) {
        if (!flock_conflicts(f, type, l))
            continue;
        if (!wait) {
            ino_free_if_empty(ir);
            return -LE_AGAIN;
        }
        if (sig_interrupt_pending(sched_cur_cpu())) {
            ino_free_if_empty(ir);
            return -LE_INTR;
        }
        sched_block_restart_eintr((uint64_t)(uintptr_t)ir);
        return LK_BLOCKED;
    }

    lk_t *n = lk_new(NULL, 0, type, 0, LK_EOF);
    n->file = f;
    list_insert(&ir->flock, n, NULL);
    ino_free_if_empty(ir);
    return 0;
}

void lk_flock_release(vfs_fd *f)
{
    for (unsigned b = 0; b < LK_BUCKETS; b++) {
        lkino_t *next;
        for (lkino_t *ir = inodes[b]; ir; ir = next) {
            next = ir->next;
            int changed = 0;
            for (lk_t *l = ir->flock, *n; l; l = n) {
                n = l->next;
                if (l->file == f) {
                    lk_unlink_free(&ir->flock, l);
                    changed = 1;
                }
            }
            if (changed)
                ino_wake(ir);
            ino_free_if_empty(ir);
        }
    }
}
