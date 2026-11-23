/* The Android binder driver (see binder.h and docs/boot.md, B3).
 *
 * The model follows the Linux driver. A node belongs to the proc that made it. A ref
 * maps a handle in one proc to a node. A transaction is copied into the receiver's
 * buffer (a memfd the receiver maps read-only), and its objects are translated on the
 * way. Each thread has a todo queue, and each proc has one for work any looper thread
 * may take. A thread that waits for a reply takes only its own todo, as in the driver.
 *
 * Blocking is a restart (sched_block_restart): a thread waiting for work sleeps on its
 * key (its thread, or its proc for proc work), and the ioctl runs again from its start.
 * The write half is consumed before any wait, and write_consumed is written back to
 * guest memory first, so the rerun skips it.
 *
 * The engine runs one guest thread at a time, so there are no locks. */
#include "binder.h"
#include "selinuxfs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "linux.h"
#include "mem.h"
#include "poll.h"
#include "proc.h"
#include "sched.h"
#include "vfs.h"

#define MAP_CAP (4u << 20)
#define NDEV 3
#define BINDER_PROTOCOL 8
#define ALIGN8(x) (((x) + 7u) & ~(uint64_t)7)

/* ioctl numbers (arm64 Linux encoding: dir << 30 | size << 16 | type << 8 | nr). */
#define IOC_(dir, type, nr, size) \
    ((uint32_t)(((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((uint32_t)(type) << 8) | (uint32_t)(nr)))
#define IOC_NONE 0u
#define IOC_W 1u
#define IOC_R 2u
#define IOC_RW 3u

#define BINDER_WRITE_READ IOC_(IOC_RW, 'b', 1, 48)
#define BINDER_SET_IDLE_TIMEOUT IOC_(IOC_W, 'b', 3, 8)
#define BINDER_SET_MAX_THREADS IOC_(IOC_W, 'b', 5, 4)
#define BINDER_SET_IDLE_PRIORITY IOC_(IOC_W, 'b', 6, 4)
#define BINDER_SET_CONTEXT_MGR IOC_(IOC_W, 'b', 7, 4)
#define BINDER_THREAD_EXIT IOC_(IOC_W, 'b', 8, 4)
#define BINDER_VERSION IOC_(IOC_RW, 'b', 9, 4)
#define BINDER_GET_NODE_DEBUG_INFO IOC_(IOC_RW, 'b', 11, 24)
#define BINDER_GET_NODE_INFO_FOR_REF IOC_(IOC_RW, 'b', 12, 24)
#define BINDER_SET_CONTEXT_MGR_EXT IOC_(IOC_W, 'b', 13, 24)
#define BINDER_FREEZE IOC_(IOC_W, 'b', 14, 12)
#define BINDER_GET_FROZEN_INFO IOC_(IOC_RW, 'b', 15, 12)
#define BINDER_ENABLE_ONEWAY_SPAM_DETECTION IOC_(IOC_W, 'b', 16, 4)

/* Commands from userspace (the write buffer). */
#define BC_TRANSACTION IOC_(IOC_W, 'c', 0, 64)
#define BC_REPLY IOC_(IOC_W, 'c', 1, 64)
#define BC_ACQUIRE_RESULT IOC_(IOC_W, 'c', 2, 4)
#define BC_FREE_BUFFER IOC_(IOC_W, 'c', 3, 8)
#define BC_INCREFS IOC_(IOC_W, 'c', 4, 4)
#define BC_ACQUIRE IOC_(IOC_W, 'c', 5, 4)
#define BC_RELEASE IOC_(IOC_W, 'c', 6, 4)
#define BC_DECREFS IOC_(IOC_W, 'c', 7, 4)
#define BC_INCREFS_DONE IOC_(IOC_W, 'c', 8, 16)
#define BC_ACQUIRE_DONE IOC_(IOC_W, 'c', 9, 16)
#define BC_ATTEMPT_ACQUIRE IOC_(IOC_W, 'c', 10, 8)
#define BC_REGISTER_LOOPER IOC_(IOC_NONE, 'c', 11, 0)
#define BC_ENTER_LOOPER IOC_(IOC_NONE, 'c', 12, 0)
#define BC_EXIT_LOOPER IOC_(IOC_NONE, 'c', 13, 0)
#define BC_REQUEST_DEATH_NOTIFICATION IOC_(IOC_W, 'c', 14, 12)
#define BC_CLEAR_DEATH_NOTIFICATION IOC_(IOC_W, 'c', 15, 12)
#define BC_DEAD_BINDER_DONE IOC_(IOC_W, 'c', 16, 8)
#define BC_TRANSACTION_SG IOC_(IOC_W, 'c', 17, 72)
#define BC_REPLY_SG IOC_(IOC_W, 'c', 18, 72)

/* Returns to userspace (the read buffer). */
#define BR_TRANSACTION_SEC_CTX IOC_(IOC_R, 'r', 2, 72)
#define BR_TRANSACTION IOC_(IOC_R, 'r', 2, 64)
#define BR_REPLY IOC_(IOC_R, 'r', 3, 64)
#define BR_DEAD_REPLY IOC_(IOC_NONE, 'r', 5, 0)
#define BR_TRANSACTION_COMPLETE IOC_(IOC_NONE, 'r', 6, 0)
#define BR_INCREFS IOC_(IOC_R, 'r', 7, 16)
#define BR_ACQUIRE IOC_(IOC_R, 'r', 8, 16)
#define BR_RELEASE IOC_(IOC_R, 'r', 9, 16)
#define BR_DECREFS IOC_(IOC_R, 'r', 10, 16)
#define BR_NOOP IOC_(IOC_NONE, 'r', 12, 0)
#define BR_SPAWN_LOOPER IOC_(IOC_NONE, 'r', 13, 0)
#define BR_DEAD_BINDER IOC_(IOC_R, 'r', 15, 8)
#define BR_CLEAR_DEATH_NOTIFICATION_DONE IOC_(IOC_R, 'r', 16, 8)
#define BR_FAILED_REPLY IOC_(IOC_NONE, 'r', 17, 0)

/* Object types: B_PACK_CHARS(c1, c2, c3, 0x85). */
#define BOBJ(a, b, c) ((((uint32_t)(a)) << 24) | (((uint32_t)(b)) << 16) | (((uint32_t)(c)) << 8) | 0x85u)
#define OBJ_BINDER BOBJ('s', 'b', '*')
#define OBJ_WEAK_BINDER BOBJ('w', 'b', '*')
#define OBJ_HANDLE BOBJ('s', 'h', '*')
#define OBJ_WEAK_HANDLE BOBJ('w', 'h', '*')
#define OBJ_FD BOBJ('f', 'd', '*')
#define OBJ_FDA BOBJ('f', 'd', 'a')
#define OBJ_PTR BOBJ('p', 't', '*')

/* flat_binder_object flags: the owner wants the sender's security context per request. */
#define FLAT_BINDER_FLAG_TXN_SECURITY_CTX 0x1000
/* The node's minimum scheduling policy (2 bits at 9) and priority (the low byte), and whether
 * a request raises the receiving thread to a real-time sender's policy. */
#define FLAT_BINDER_FLAG_INHERIT_RT 0x800
#define FLAT_BINDER_FLAG_SCHED_POLICY_SHIFT 9
#define FLAT_BINDER_FLAG_PRIORITY_MASK 0xff
#define SCHED_FIFO_POLICY 1
#define SCHED_RR_POLICY 2
#define TF_ONE_WAY 0x01u

/* A transaction as userspace gives it (binder_transaction_data, 64 bytes). */
typedef struct {
    uint64_t target, cookie;
    uint32_t code, flags;
    int32_t sender_pid;
    uint32_t sender_euid;
    uint64_t data_size, offsets_size, buffer, offsets;
    uint64_t buffers_size; /* the scatter-gather area (BC_*_SG only; 0 otherwise) */
} btd_t;

/* ---------- objects ---------- */

typedef struct bproc bproc_t;
typedef struct bthread bthread_t;
typedef struct bnode bnode_t;
typedef struct bref bref_t;
typedef struct btxn btxn_t;
typedef struct balloc balloc_t;
typedef struct bwork bwork_t;

typedef struct {
    vfs_fd *d;
    uint64_t at; /* the buffer offset where the fd number is written */
} bfdref_t;

typedef struct {
    bwork_t *head, *tail;
} bq_t;

typedef enum {
    WK_TXN_COMPLETE, /* BR_TRANSACTION_COMPLETE */
    WK_TXN,          /* an incoming request (BR_TRANSACTION) */
    WK_REPLY,        /* the reply to a request this thread sent (BR_REPLY) */
    WK_FAILED,       /* BR_DEAD_REPLY or BR_FAILED_REPLY (br) */
    WK_OBJREF,       /* BR_INCREFS, BR_ACQUIRE, BR_RELEASE or BR_DECREFS (br; a = ptr, b = cookie) */
    WK_DEAD_BINDER,  /* BR_DEAD_BINDER (a = cookie) */
    WK_CLEAR_DONE    /* BR_CLEAR_DEATH_NOTIFICATION_DONE (a = cookie) */
} wk_t;

struct bwork {
    wk_t kind;
    uint32_t br;
    uint64_t a, b;
    btxn_t *txn;
    bwork_t *next;
};

struct btxn {
    uint64_t id;     /* a number for the trace (XIAOJI_BINDER_TRACE) */
    bthread_t *from; /* the sending thread, while its call waits for the reply (NULL: none) */
    bthread_t *to;   /* the thread that took the request */
    balloc_t *buf;   /* the buffer in the receiving proc */
    /* The target node's flags, taken at the send. A node is not kept by pointer: it can be freed
     * between the send and the delivery, once nothing refers to it. */
    int node_set;
    uint32_t node_flags;
    uint64_t target, cookie;
    uint32_t code, flags;
    int32_t sender_pid;
    int oneway, reply, secctx;
    uint64_t secctx_at;
    /* A sync request raised its receiving thread's priority: restored when the request ends. */
    int prio_saved, prio_policy, prio_prio;
    uint64_t prio_tid;
    bfdref_t *fds;
    uint32_t nfds;
    btxn_t *sent_next;  /* in from->sent */
    btxn_t *recv_next;  /* in to->recv */
    btxn_t *async_next; /* in node->async */
};

struct balloc {
    uint64_t off, size, data_size, offs_size;
    uint64_t *offsets; /* the object offsets in the data */
    uint32_t noffs;
    bnode_t *oneway_node; /* set for the buffer of a oneway request */
    balloc_t *next;
};

struct bnode {
    bproc_t *proc; /* the owner, NULL once it is gone */
    uint64_t ptr, cookie;
    uint32_t flags;
    int int_strong, int_weak; /* refs holding a strong count, and refs holding any count */
    int has_strong, has_weak; /* the owner has acknowledged the count */
    int pend_strong, pend_weak;
    int dead;
    int nrefs;
    bref_t *refs;
    bnode_t *next;
    btxn_t *async, *async_tail;
    int async_busy;
};

struct bref {
    bproc_t *proc;
    bnode_t *node;
    uint32_t desc;
    int strong, weak;
    int has_death, death_pending;
    uint64_t death_cookie;
    bref_t *next_proc, *next_node;
};

struct bthread {
    uint64_t tid;
    bproc_t *proc;
    int looper;
    int waiting_proc; /* blocked for proc work (an idle looper), as of its last read */
    bq_t todo;
    btxn_t *sent, *recv;
    bthread_t *next;
};

typedef struct {
    bnode_t *mgr;
} bctx_t;

struct bproc {
    bctx_t *ctx;
    int pid;      /* the guest pid that opened the device (trace only) */
    memfd_t *mfd; /* the receive buffer (this proc holds one reference) */
    uint64_t map_va, map_size;
    int prepared, mapped, closing;
    bthread_t *threads;
    bnode_t *nodes;
    bref_t *refs;
    balloc_t *allocs;
    bq_t todo;
    uint32_t max_threads;
    uint32_t requested_threads; /* BR_SPAWN_LOOPER sent, not yet registered */
    uint32_t started_threads;   /* loopers registered with BC_REGISTER_LOOPER */
};

static bctx_t contexts[NDEV];

/* A wait key: the address of the object that sleeps on it. */
static uint64_t key_of(const void *p)
{
    return (uint64_t)(uintptr_t)p;
}

int binder_device(const char *path)
{
    if (!strcmp(path, "/dev/binder")) return 0;
    if (!strcmp(path, "/dev/hwbinder")) return 1;
    if (!strcmp(path, "/dev/vndbinder")) return 2;
    return -1;
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

/* ---------- queues and wake-ups ---------- */

static void q_push(bq_t *q, bwork_t *w)
{
    w->next = NULL;
    if (q->tail)
        q->tail->next = w;
    else
        q->head = w;
    q->tail = w;
}

static bwork_t *q_pop(bq_t *q)
{
    bwork_t *w = q->head;
    if (w) {
        q->head = w->next;
        if (!q->head)
            q->tail = NULL;
    }
    return w;
}

/* The command that delivers a request: BR_TRANSACTION_SEC_CTX when its buffer carries the
 * sender's security context (the target node asked for it), else BR_TRANSACTION. */
static uint32_t txn_br(const btxn_t *x)
{
    return x->secctx ? BR_TRANSACTION_SEC_CTX : BR_TRANSACTION;
}

static bwork_t *work_new(wk_t kind, uint32_t br, uint64_t a, uint64_t b, btxn_t *txn)
{
    bwork_t *w = calloc(1, sizeof *w);
    if (w) {
        w->kind = kind;
        w->br = br;
        w->a = a;
        w->b = b;
        w->txn = txn;
    }
    return w;
}

static void queue_thread(bthread_t *t, bwork_t *w)
{
    q_push(&t->todo, w);
    sched_wake_key(key_of(t));
    poll_notify();
}

static void queue_proc(bproc_t *p, bwork_t *w)
{
    q_push(&p->todo, w);
    sched_wake_key(key_of(p));
    poll_notify();
}

/* ---------- trace: XIAOJI_BINDER_TRACE=1 (off by default) ----------
 * One line per binder event on stderr: transactions (sender, target node, code, flags, the
 * interface token and, for handle 0, the String16 argument), replies and failures, object
 * translations, node and ref lifetimes, count traffic and acknowledgements. Off, each event is
 * one test of a static flag. */

static int bt_state = -1; /* -1: the variable is not read yet; 0: off; 1: on */

static int bt_read(void)
{
    if (bt_state < 0) {
        const char *e = getenv("XIAOJI_BINDER_TRACE");
        bt_state = e && e[0] == '1';
    }
    return bt_state;
}

#define BT_ON (bt_state > 0 || (bt_state < 0 && bt_read()))
#define BT(...)                \
    do {                       \
        if (BT_ON)             \
            bt_log(__VA_ARGS__); \
    } while (0)

static uint64_t bt_seq;

static void bt_log(const char *fmt, ...)
{
    static uint64_t t0;
    if (!t0)
        t0 = host_monotonic_ns() | 1;
    proc_t *p = proc_cur();
    fprintf(stderr, "[%llu ms %llu insn] binder pid=%d tid=%llu: ",
            (unsigned long long)((host_monotonic_ns() - t0) / 1000000ull),
            (unsigned long long)sched_icount(), p ? p->pid : 0, (unsigned long long)sched_tid());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static const char *bt_br(uint32_t br)
{
    switch (br) {
    case BR_TRANSACTION: return "TRANSACTION";
    case BR_REPLY: return "REPLY";
    case BR_DEAD_REPLY: return "DEAD_REPLY";
    case BR_FAILED_REPLY: return "FAILED_REPLY";
    case BR_TRANSACTION_COMPLETE: return "TRANSACTION_COMPLETE";
    case BR_INCREFS: return "INCREFS";
    case BR_ACQUIRE: return "ACQUIRE";
    case BR_RELEASE: return "RELEASE";
    case BR_DECREFS: return "DECREFS";
    default: return "other";
    }
}

static const char *bt_obj(uint32_t t)
{
    switch (t) {
    case OBJ_BINDER: return "BINDER";
    case OBJ_WEAK_BINDER: return "WEAK_BINDER";
    case OBJ_HANDLE: return "HANDLE";
    case OBJ_WEAK_HANDLE: return "WEAK_HANDLE";
    case OBJ_FD: return "FD";
    case OBJ_FDA: return "FDA";
    case OBJ_PTR: return "PTR";
    default: return "?";
    }
}

/* The String16 at data offset off (a length word, then UTF-16 units and a NUL): copies its ASCII
 * text to out and returns the offset past it (4-byte aligned, as Parcel::writeString16 leaves it). */
static uint64_t bt_str16(const uint8_t *d, uint64_t dsz, uint64_t off, char *out, size_t cap)
{
    size_t k = 0;
    out[0] = 0;
    if (off + 4 > dsz)
        return dsz;
    uint64_t n = ld32(d + off);
    for (uint64_t i = 0; i < n && off + 4 + 2 * i + 2 <= dsz; i++) {
        uint32_t c = d[off + 4 + 2 * i] | ((uint32_t)d[off + 5 + 2 * i] << 8);
        if (k + 1 < cap)
            out[k++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    out[k] = 0;
    return off + 4 + ((2 * (n + 1) + 3) & ~(uint64_t)3);
}

/* Logs a sync or oneway request from a thread: the target handle and node, the code and flags,
 * and the interface token (Parcel::writeInterfaceToken: strict mode, work source, 'SYST', then the
 * String16). For handle 0 the String16 after the token is logged too (the service name). */
static void bt_send(bproc_t *b, bthread_t *t, const btd_t *d, uint32_t h, bnode_t *node, uint64_t id)
{
    uint8_t head[256];
    char tok[128] = "", arg[128] = "";
    uint64_t n = d->data_size < sizeof head ? d->data_size : sizeof head;
    if (n && mem_copy_out(head, d->buffer, (size_t)n) == 0 && n >= 16) {
        uint64_t next = bt_str16(head, n, 12, tok, sizeof tok);
        if (h == 0 && next < n)
            bt_str16(head, n, next, arg, sizeof arg);
    }
    if (!node) {
        BT("send txn=%llu from pid=%d handle=%u: NO REF (code=%u flags=%x) token=\"%s\"",
           (unsigned long long)id, b->pid, h, d->code, d->flags, tok);
        return;
    }
    BT("send txn=%llu from pid=%d tid=%llu handle=%u -> owner pid=%d ptr=%llx cookie=%llx "
       "node-flags=%x code=%u flags=%x oneway=%d data=%llu offsets=%llu token=\"%s\"%s%s%s",
       (unsigned long long)id, b->pid, (unsigned long long)t->tid, h, node->proc ? node->proc->pid : -1,
       (unsigned long long)node->ptr, (unsigned long long)node->cookie, node->flags, d->code, d->flags,
       (d->flags & TF_ONE_WAY) != 0, (unsigned long long)d->data_size, (unsigned long long)d->offsets_size,
       tok, h == 0 ? " name=\"" : "", h == 0 ? arg : "", h == 0 ? "\"" : "");
}

/* The thread whose transaction is being translated, while it is (else NULL). A count the
 * transaction gives to its own sender's node (the sender owns the binder it sends) goes to
 * that thread: a thread waiting for its reply reads its own work, not the proc's, as the
 * driver does (binder_inc_node with the sender's todo list). */
static bthread_t *xl_sender;

/* Tells a node's owner about a count (BR_INCREFS, BR_ACQUIRE, BR_RELEASE, BR_DECREFS). */
static void node_notify_owner(bnode_t *n, uint32_t br)
{
    if (!n->proc || n->proc->closing)
        return;
    bwork_t *w = work_new(WK_OBJREF, br, n->ptr, n->cookie, NULL);
    if (!w)
        return;
    BT("owner notify %s to pid=%d ptr=%llx cookie=%llx via %s", bt_br(br), n->proc->pid,
       (unsigned long long)n->ptr, (unsigned long long)n->cookie,
       xl_sender && xl_sender->proc == n->proc ? "sender thread" : "proc queue");
    if (xl_sender && xl_sender->proc == n->proc)
        queue_thread(xl_sender, w);
    else
        queue_proc(n->proc, w);
}

/* ---------- lookups ---------- */

static bthread_t *thread_of(bproc_t *b, int create)
{
    uint64_t tid = (uint64_t)sched_tid();
    for (bthread_t *t = b->threads; t; t = t->next)
        if (t->tid == tid)
            return t;
    if (!create)
        return NULL;
    bthread_t *t = calloc(1, sizeof *t);
    if (t) {
        t->tid = tid;
        t->proc = b;
        t->next = b->threads;
        b->threads = t;
    }
    return t;
}

static bnode_t *node_by_ptr(bproc_t *b, uint64_t ptr)
{
    for (bnode_t *n = b->nodes; n; n = n->next)
        if (n->ptr == ptr)
            return n;
    return NULL;
}

static bref_t *ref_by_handle(bproc_t *b, uint32_t h)
{
    for (bref_t *r = b->refs; r; r = r->next_proc)
        if (r->desc == h)
            return r;
    return NULL;
}

static bref_t *ref_to_node(bproc_t *b, bnode_t *n)
{
    for (bref_t *r = n->refs; r; r = r->next_node)
        if (r->proc == b)
            return r;
    return NULL;
}

static bnode_t *node_new(bproc_t *b, uint64_t ptr, uint64_t cookie, uint32_t flags)
{
    bnode_t *n = calloc(1, sizeof *n);
    if (n) {
        n->proc = b;
        n->ptr = ptr;
        n->cookie = cookie;
        n->flags = flags;
        n->next = b->nodes;
        b->nodes = n;
        BT("node create owner pid=%d ptr=%llx cookie=%llx flags=%x", b->pid, (unsigned long long)ptr,
           (unsigned long long)cookie, flags);
    }
    return n;
}

static void node_unlink(bproc_t *b, bnode_t *n)
{
    for (bnode_t **pp = &b->nodes; *pp; pp = &(*pp)->next)
        if (*pp == n) {
            *pp = n->next;
            return;
        }
}

/* Frees a node once nothing needs it. A live owner's node goes when no ref holds it, the owner
 * holds no count of it (none acknowledged, none pending) and no request is queued on it. This is
 * the driver's rule (binder_dec_node_nilocked and BINDER_WORK_NODE, which erase the node). A later
 * object at the same address then gets a new node with its own cookie. The context manager's node
 * is kept while the manager lives. A dead owner's node goes when no ref is left. */
static void node_maybe_free(bnode_t *n)
{
    if (n->proc) {
        bproc_t *o = n->proc;
        if (n->nrefs || n->int_strong || n->int_weak || n->has_strong || n->has_weak || n->pend_strong ||
            n->pend_weak || n->async || n->async_busy || o->ctx->mgr == n)
            return;
        BT("node free (owner alive) pid=%d ptr=%llx cookie=%llx", o->pid, (unsigned long long)n->ptr,
           (unsigned long long)n->cookie);
        node_unlink(o, n);
        free(n);
        return;
    }
    if (!n->dead || n->nrefs)
        return;
    BT("node free ptr=%llx cookie=%llx", (unsigned long long)n->ptr, (unsigned long long)n->cookie);
    for (btxn_t *t = n->async; t;) {
        btxn_t *next = t->async_next;
        for (uint32_t i = 0; i < t->nfds; i++)
            vfs_desc_drop(t->fds[i].d);
        free(t->fds);
        free(t);
        t = next;
    }
    free(n);
}

/* A ref in b to node n: a new one takes the lowest free handle (the context manager
 * takes handle 0). */
static bref_t *ref_new(bproc_t *b, bnode_t *n)
{
    bref_t *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    uint32_t desc = (n == b->ctx->mgr && !ref_by_handle(b, 0)) ? 0 : 1;
    while (ref_by_handle(b, desc))
        desc++;
    r->proc = b;
    r->node = n;
    r->desc = desc;
    r->next_proc = b->refs;
    b->refs = r;
    r->next_node = n->refs;
    n->refs = r;
    n->nrefs++;
    BT("ref create pid=%d handle=%u -> node owner=%d ptr=%llx cookie=%llx", b->pid, desc,
       n->proc ? n->proc->pid : -1, (unsigned long long)n->ptr, (unsigned long long)n->cookie);
    return r;
}

/* The ref for a handle. Handle 0 is the context manager, and its ref is made on demand. */
static bref_t *ref_for_handle(bproc_t *b, uint32_t h)
{
    bref_t *r = ref_by_handle(b, h);
    if (r || h != 0 || !b->ctx->mgr)
        return r;
    return ref_new(b, b->ctx->mgr);
}

/* ---------- counts ---------- */

/* The context manager's node starts with its strong and weak counts acknowledged by its
 * owner (the driver's binder_become_context_manager sets has_strong_ref and has_weak_ref).
 * Without this, a reference to it would send BR_INCREFS with ptr and cookie 0 to the
 * owner, and libbinder would increment a null weakref. */
static void mgr_counts_acked(bnode_t *n)
{
    n->int_strong = 1;
    n->int_weak = 1;
    n->has_strong = 1;
    n->has_weak = 1;
}

static void node_inc(bnode_t *n, int strong)
{
    if (strong) {
        n->int_strong++;
        if (n->int_strong == 1 && !n->has_strong && !n->pend_strong && n->proc) {
            n->pend_strong = 1;
            node_notify_owner(n, BR_ACQUIRE);
        }
    } else {
        n->int_weak++;
        if (n->int_weak == 1 && !n->has_weak && !n->pend_weak && n->proc) {
            n->pend_weak = 1;
            node_notify_owner(n, BR_INCREFS);
        }
    }
}

static void node_dec(bnode_t *n, int strong)
{
    if (strong) {
        if (n->int_strong > 0 && --n->int_strong == 0 && n->has_strong) {
            n->has_strong = 0;
            node_notify_owner(n, BR_RELEASE);
        }
    } else {
        if (n->int_weak > 0 && --n->int_weak == 0 && n->has_weak) {
            n->has_weak = 0;
            node_notify_owner(n, BR_DECREFS);
        }
    }
}

static void ref_free(bref_t *r)
{
    bproc_t *b = r->proc;
    bnode_t *n = r->node;
    BT("ref free pid=%d handle=%u -> node owner=%d ptr=%llx cookie=%llx", b->pid, r->desc,
       n->proc ? n->proc->pid : -1, (unsigned long long)n->ptr, (unsigned long long)n->cookie);
    for (bref_t **pp = &b->refs; *pp; pp = &(*pp)->next_proc)
        if (*pp == r) {
            *pp = r->next_proc;
            break;
        }
    for (bref_t **pp = &n->refs; *pp; pp = &(*pp)->next_node)
        if (*pp == r) {
            *pp = r->next_node;
            break;
        }
    n->nrefs--;
    free(r);
    node_maybe_free(n);
}

/* Adds a strong or weak count to a ref. */
static void ref_inc(bref_t *r, int strong)
{
    if (strong) {
        if (r->strong == 0)
            node_inc(r->node, 1);
        r->strong++;
    } else {
        r->weak++;
    }
    if (r->strong + r->weak == 1)
        node_inc(r->node, 0);
}

/* Drops a count from a ref. A ref with no counts left is freed. */
static void ref_dec(bref_t *r, int strong)
{
    if (strong) {
        if (r->strong == 0)
            return;
        if (--r->strong == 0)
            node_dec(r->node, 1);
    } else {
        if (r->weak == 0)
            return;
        r->weak--;
    }
    if (r->strong + r->weak == 0) {
        node_dec(r->node, 0);
        ref_free(r);
    }
}

/* ---------- buffers ---------- */

/* The offset of a free range of size bytes in b's receive buffer, or -1. */
static int64_t buf_find(bproc_t *b, uint64_t size)
{
    uint64_t off = 0;
    for (;;) {
        if (off + size > b->map_size)
            return -1;
        int clash = 0;
        for (balloc_t *a = b->allocs; a && !clash; a = a->next)
            if (a->off < off + size && off < a->off + a->size) {
                off = a->off + a->size;
                clash = 1;
            }
        if (!clash)
            return (int64_t)off;
    }
}

static int buf_write(bproc_t *b, uint64_t off, const void *p, size_t n)
{
    if (!n)
        return 0;
    return memfd_pwrite(b->mfd, p, n, off) == (int64_t)n ? 0 : -1;
}

static int buf_read(bproc_t *b, uint64_t off, void *p, size_t n)
{
    return memfd_pread(b->mfd, p, n, off) == (int64_t)n ? 0 : -1;
}

/* The handles of a buffer give back their counts when the buffer is freed (the counts
 * were taken when the objects were translated). */
static void buf_release_refs(bproc_t *b, balloc_t *a)
{
    for (uint32_t i = 0; i < a->noffs; i++) {
        /* A flat_binder_object: type, flags, then the handle (or binder) at offset 8. */
        uint8_t obj[24];
        if (a->offsets[i] + 24 > a->data_size || buf_read(b, a->off + a->offsets[i], obj, 24) < 0)
            continue;
        uint32_t type = ld32(obj);
        if (type == OBJ_HANDLE || type == OBJ_WEAK_HANDLE) {
            bref_t *r = ref_by_handle(b, ld32(obj + 8));
            BT("buffer free pid=%d: release %s handle %u (%s)", b->pid, bt_obj(type), ld32(obj + 8),
               r ? "ref" : "NO REF");
            if (r)
                ref_dec(r, type == OBJ_HANDLE);
        }
    }
}

/* ---------- transactions ---------- */

/* A sync request to a node with a minimum real-time policy, or with inherit-RT and a real-time
 * sender, runs its thread at that policy and priority until the request ends (the kernel's
 * binder_transaction_priority). Only real-time policies are raised; oneway requests are not. */
static void sched_raise(btxn_t *x, bthread_t *t)
{
    if (x->oneway || !x->node_set)
        return;
    uint32_t f = x->node_flags;
    int have = 0, pol = 0, pri = 0;
    int node_pol = (int)((f >> FLAT_BINDER_FLAG_SCHED_POLICY_SHIFT) & 3);
    int node_pri = (int)(f & FLAT_BINDER_FLAG_PRIORITY_MASK);
    if (node_pol == SCHED_FIFO_POLICY || node_pol == SCHED_RR_POLICY) {
        pol = node_pol;
        pri = node_pri;
        have = 1;
    }
    if ((f & FLAT_BINDER_FLAG_INHERIT_RT) && x->from) {
        int sp = 0, sq = 0;
        if (sched_policy_get(x->from->tid, &sp, &sq) == 0 &&
            (sp == SCHED_FIFO_POLICY || sp == SCHED_RR_POLICY) && (!have || sq > pri)) {
            pol = sp;
            pri = sq;
            have = 1;
        }
    }
    if (!have)
        return;
    int op = 0, oq = 0;
    if (sched_policy_get(t->tid, &op, &oq) != 0 || (op == pol && oq == pri))
        return;
    x->prio_tid = t->tid;
    x->prio_policy = op;
    x->prio_prio = oq;
    x->prio_saved = 1;
    sched_policy_set(t->tid, pol, pri);
}

static void txn_free(btxn_t *t)
{
    if (t->prio_saved)
        sched_policy_set(t->prio_tid, t->prio_policy, t->prio_prio); /* the thread's own policy back */
    for (uint32_t i = 0; i < t->nfds; i++)
        vfs_desc_drop(t->fds[i].d);
    free(t->fds);
    free(t);
}

static void sent_remove(bthread_t *from, btxn_t *t)
{
    for (btxn_t **pp = &from->sent; *pp; pp = &(*pp)->sent_next)
        if (*pp == t) {
            *pp = t->sent_next;
            break;
        }
}

/* The sender's call ends with br (its request failed or its target died). */
static void fail_to_sender(btxn_t *t, uint32_t br)
{
    bthread_t *s = t->from;
    BT("fail txn=%llu to waiting tid=%llu: %s", (unsigned long long)t->id, s ? (unsigned long long)s->tid : 0ull,
       bt_br(br));
    if (!s)
        return;
    sent_remove(s, t);
    t->from = NULL;
    bwork_t *w = work_new(WK_FAILED, br, 0, 0, NULL);
    if (w)
        queue_thread(s, w);
}

/* Empties a queue when its owner is going: requests fail for their senders. */
static void queue_drain(bq_t *q)
{
    bwork_t *w;
    while ((w = q_pop(q))) {
        if (w->txn) {
            if (w->kind == WK_TXN && !w->txn->reply)
                fail_to_sender(w->txn, BR_DEAD_REPLY);
            txn_free(w->txn);
        }
        free(w);
    }
}

/* The state of one transaction while its objects are translated. */
typedef struct {
    bproc_t *src, *dst;
    uint8_t *data;
    uint64_t dsize;
    /* The scatter-gather area: the next free offset, and its end (both relative to the buffer
     * start). The area starts after the data and the offsets, and is buffers_size long. */
    uint64_t sg_cur, sg_lim;
    uint32_t nobj;
    uint64_t *offs;    /* the offsets array (object offsets), valid while the objects are read */
    int64_t *obj_blob; /* per object: its scatter-gather blob, or -1 */
    /* binder_validate_fixup's state: the last object fixed up (0: none) and the minimum fixup
     * offset within it. */
    uint64_t last_obj, last_min;
    struct {
        uint8_t *data;
        uint64_t len, at;
    } *blobs;
    uint32_t nblobs, cap_blobs;
    struct {
        uint64_t field, blob_at; /* a buffer address field to patch once the buffer exists */
    } *fix;
    uint32_t nfix, cap_fix;
    /* A parent's slot to patch with a child's address once the buffer exists: at offset poff of
     * blob blob, the address of the child blob at offset at. */
    struct {
        uint32_t blob;
        uint64_t poff, at;
    } *pfix;
    uint32_t npfix, cap_pfix;
    bfdref_t *fds;
    uint32_t nfds, cap_fds;
} xlate_t;

static int xl_fd(xlate_t *x, vfs_fd *d, uint64_t at)
{
    if (x->nfds == x->cap_fds) {
        uint32_t cap = x->cap_fds ? x->cap_fds * 2 : 4;
        bfdref_t *nf = realloc(x->fds, cap * sizeof *nf);
        if (!nf) {
            vfs_desc_drop(d);
            return -LE_NOMEM;
        }
        x->fds = nf;
        x->cap_fds = cap;
    }
    x->fds[x->nfds].d = d;
    x->fds[x->nfds].at = at;
    x->nfds++;
    return 0;
}

/* Translates a binder object of the sender (node n, with its strong or weak flag) for
 * the receiver. */
static int xl_binder(xlate_t *x, uint8_t *obj, int strong, bnode_t *n)
{
    if (n->proc == x->dst) {
        /* The receiver owns the node: it gets its own binder back. */
        BT("  translate %s: node owned by the receiver pid=%d, binder back ptr=%llx cookie=%llx",
           strong ? "BINDER" : "WEAK_BINDER", x->dst->pid, (unsigned long long)n->ptr,
           (unsigned long long)n->cookie);
        st32(obj, strong ? OBJ_BINDER : OBJ_WEAK_BINDER);
        st64(obj + 8, n->ptr);
        st64(obj + 16, n->cookie);
        return 0;
    }
    bref_t *r = ref_to_node(x->dst, n);
    if (!r) {
        r = ref_new(x->dst, n);
        if (!r)
            return -LE_NOMEM;
    }
    ref_inc(r, strong);
    BT("  translate %s: node owner pid=%d ptr=%llx cookie=%llx -> handle %llu in pid=%d",
       strong ? "BINDER" : "WEAK_BINDER", n->proc ? n->proc->pid : -1, (unsigned long long)n->ptr,
       (unsigned long long)n->cookie, (unsigned long long)r->desc, x->dst->pid);
    st32(obj, strong ? OBJ_HANDLE : OBJ_WEAK_HANDLE);
    st64(obj + 8, r->desc);
    st64(obj + 16, 0);
    return 0;
}

static int xl_fix(xlate_t *x, uint64_t field, uint64_t blob_at)
{
    if (x->nfix == x->cap_fix) {
        uint32_t cap = x->cap_fix ? x->cap_fix * 2 : 4;
        void *nf = realloc(x->fix, cap * sizeof *x->fix);
        if (!nf)
            return -LE_NOMEM;
        x->fix = nf;
        x->cap_fix = cap;
    }
    x->fix[x->nfix].field = field;
    x->fix[x->nfix].blob_at = blob_at;
    x->nfix++;
    return 0;
}

#define BINDER_BUFFER_FLAG_HAS_PARENT 0x1u

/* binder_validate_fixup (Linux 6.12, drivers/android/binder.c). A fixup inside the parent object at
 * parent_obj, at offset fixup_off, is allowed when the last object fixed up is that parent or one of
 * its parents, and the fixups inside one object come at increasing offsets. An offset of 0 is the
 * driver's "no object" sentinel: nothing is fixed up until another object has been. */
static int xl_fixup_ok(xlate_t *x, uint64_t parent_obj, uint64_t fixup_off)
{
    if (!x->last_obj)
        return 0;
    uint64_t cur = x->last_obj, min = x->last_min;
    while (cur != parent_obj) {
        const uint8_t *o = x->data + cur;
        /* The state only holds PTR objects, and each step goes to an earlier object (the parent
         * index of an object is checked when it is translated), so this ends. */
        if (cur + 40 > x->dsize || ld32(o) != OBJ_PTR || !(ld32(o + 4) & BINDER_BUFFER_FLAG_HAS_PARENT))
            return 0;
        min = ld64(o + 32) + 8;
        uint64_t pi = ld64(o + 24);
        if (pi >= x->nobj)
            return 0;
        cur = x->offs[pi];
    }
    return fixup_off >= min;
}

/* Records a parent slot to patch once the buffer exists (binder_fixup_parent's deferred fixup). */
static int xl_pfix(xlate_t *x, uint32_t blob, uint64_t poff, uint64_t at)
{
    if (x->npfix == x->cap_pfix) {
        uint32_t cap = x->cap_pfix ? x->cap_pfix * 2 : 4;
        void *np = realloc(x->pfix, cap * sizeof *x->pfix);
        if (!np)
            return -LE_NOMEM;
        x->pfix = np;
        x->cap_pfix = cap;
    }
    x->pfix[x->npfix].blob = blob;
    x->pfix[x->npfix].poff = poff;
    x->pfix[x->npfix].at = at;
    x->npfix++;
    return 0;
}

/* Translates the object at data offset off, which is object number idx. */
static int xl_object(xlate_t *x, uint32_t idx, uint64_t off)
{
    uint8_t *obj = x->data + off;
    uint32_t type = ld32(obj);
    BT("  object %u at data offset %llu: type %s (from pid=%d to pid=%d)", idx, (unsigned long long)off,
       bt_obj(type), x->src->pid, x->dst->pid);
    switch (type) {
    case OBJ_BINDER:
    case OBJ_WEAK_BINDER: {
        if (off + 24 > x->dsize)
            return -LE_INVAL;
        uint64_t ptr = ld64(obj + 8), cookie = ld64(obj + 16);
        bnode_t *n = node_by_ptr(x->src, ptr);
        if (!n) {
            n = node_new(x->src, ptr, cookie, ld32(obj + 4));
            if (!n)
                return -LE_NOMEM;
        } else if (n->cookie != cookie) {
            /* binder_translate_binder: a node is one object, so a different cookie at the same
             * address is refused (the send fails). The stored cookie is kept. */
            BT("  %s sent by pid=%d: node ptr=%llx has cookie %llx, sent %llx: REFUSED (cookie mismatch)",
               bt_obj(type), x->src->pid, (unsigned long long)ptr, (unsigned long long)n->cookie,
               (unsigned long long)cookie);
            return -LE_INVAL;
        }
        return xl_binder(x, obj, type == OBJ_BINDER, n);
    }
    case OBJ_HANDLE:
    case OBJ_WEAK_HANDLE: {
        if (off + 24 > x->dsize)
            return -LE_INVAL;
        bref_t *r = ref_for_handle(x->src, ld32(obj + 8));
        if (!r) {
            BT("  %s handle %u in pid=%d: NO REF", bt_obj(type), ld32(obj + 8), x->src->pid);
            return -LE_INVAL;
        }
        BT("  %s handle %u in pid=%d -> node owner=%d ptr=%llx cookie=%llx strong=%d weak=%d",
           bt_obj(type), r->desc, x->src->pid, r->node->proc ? r->node->proc->pid : -1,
           (unsigned long long)r->node->ptr, (unsigned long long)r->node->cookie, r->strong, r->weak);
        /* A weak-only ref cannot be sent as a strong one (the driver's need_strong_ref). */
        if (type == OBJ_HANDLE && r->strong == 0)
            return -LE_INVAL;
        if (r->node->proc == x->dst) {
            st32(obj, type == OBJ_HANDLE ? OBJ_BINDER : OBJ_WEAK_BINDER);
            st64(obj + 8, r->node->ptr);
            st64(obj + 16, r->node->cookie);
            return 0;
        }
        return xl_binder(x, obj, type == OBJ_HANDLE, r->node);
    }
    case OBJ_FD: {
        if (off + 24 > x->dsize)
            return -LE_INVAL;
        vfs_fd *d = vfs_desc_hold((int)ld32(obj + 8));
        if (!d)
            return -LE_BADF;
        return xl_fd(x, d, off + 8);
    }
    case OBJ_PTR: {
        if (off + 40 > x->dsize)
            return -LE_INVAL;
        uint64_t va = ld64(obj + 8), len = ld64(obj + 16);
        uint32_t flags = ld32(obj + 4);
        if (len > MAP_CAP)
            return -LE_INVAL;
        /* The driver's bound: the buffer must fit in the scatter-gather area that is left. */
        if (len > x->sg_lim - x->sg_cur)
            return -LE_INVAL;
        if (x->nblobs == x->cap_blobs) {
            uint32_t cap = x->cap_blobs ? x->cap_blobs * 2 : 4;
            void *nb = realloc(x->blobs, cap * sizeof *x->blobs);
            if (!nb)
                return -LE_NOMEM;
            x->blobs = nb;
            x->cap_blobs = cap;
        }
        uint8_t *blob = malloc(len ? len : 1);
        if (!blob)
            return -LE_NOMEM;
        if (len && mem_copy_out(blob, va, len) < 0) {
            free(blob);
            return -LE_FAULT;
        }
        uint64_t at = x->sg_cur;
        x->sg_cur = at + ALIGN8(len);
        uint32_t bi = x->nblobs;
        x->blobs[bi].data = blob;
        x->blobs[bi].len = len;
        x->blobs[bi].at = at;
        x->obj_blob[idx] = (int64_t)bi;
        x->nblobs++;
        int rc = xl_fix(x, off + 8, at);
        if (rc)
            return rc;
        if (flags & BINDER_BUFFER_FLAG_HAS_PARENT) {
            uint64_t parent = ld64(obj + 24), poff = ld64(obj + 32);
            /* binder_validate_ptr: the parent is an earlier PTR object of this transaction. */
            if (parent >= idx || x->obj_blob[parent] < 0)
                return -LE_INVAL;
            uint32_t pb = (uint32_t)x->obj_blob[parent];
            if (!xl_fixup_ok(x, x->offs[parent], poff))
                return -LE_INVAL;
            /* binder_fixup_parent: the slot must lie inside the parent. */
            if (x->blobs[pb].len < 8 || poff > x->blobs[pb].len - 8)
                return -LE_INVAL;
            rc = xl_pfix(x, pb, poff, at);
            if (rc)
                return rc;
        }
        /* binder_transaction: every PTR object is the last one fixed up, at minimum offset 0. */
        x->last_obj = off;
        x->last_min = 0;
        return 0;
    }
    case OBJ_FDA: {
        if (off + 32 > x->dsize)
            return -LE_INVAL;
        uint64_t nfds = ld64(obj + 8), parent = ld64(obj + 16), poff = ld64(obj + 24);
        /* binder_validate_ptr: the parent is an earlier PTR object; the array lies inside its blob. */
        if (parent >= idx || x->obj_blob[parent] < 0)
            return -LE_INVAL;
        uint64_t pobj = x->offs[parent];
        uint32_t bi = (uint32_t)x->obj_blob[parent];
        if (!xl_fixup_ok(x, pobj, poff))
            return -LE_INVAL;
        uint64_t blen = x->blobs[bi].len;
        /* binder_translate_fd_array: an empty array needs no room in the parent. */
        if (nfds && (poff % 4 || poff > blen || nfds > (blen - poff) / 4))
            return -LE_INVAL;
        for (uint64_t i = 0; i < nfds; i++) {
            uint8_t *fp = x->blobs[bi].data + poff + 4 * i;
            vfs_fd *d = vfs_desc_hold((int)ld32(fp));
            if (!d)
                return -LE_BADF;
            int rc = xl_fd(x, d, x->blobs[bi].at + poff + 4 * i);
            if (rc)
                return rc;
        }
        x->last_obj = pobj;
        x->last_min = poff + 4 * nfds;
        return 0;
    }
    default:
        return -LE_INVAL;
    }
}

/* Offsets must be 4-aligned, increasing, and leave room for an object header. */
static int offsets_valid(const uint64_t *offs, uint64_t n, uint64_t dsize)
{
    uint64_t prev = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (offs[i] % 4 || offs[i] + 4 > dsize || (i && offs[i] < prev))
            return 0;
        prev = offs[i] + 4;
    }
    return 1;
}

static void xl_free(xlate_t *x)
{
    for (uint32_t i = 0; i < x->nblobs; i++)
        free(x->blobs[i].data);
    free(x->blobs);
    free(x->fix);
    free(x->pfix);
    free(x->obj_blob);
    free(x->data);
}

/* Builds a request or reply for dst from the sender's memory (src, d). The data is
 * copied into a new buffer in dst, and its objects are translated. Returns the
 * transaction, or NULL with *err set to a -LE_* errno. */
static btxn_t *txn_build(bproc_t *src, bproc_t *dst, const btd_t *d, int secctx, int *err)
{
    xlate_t x;
    btxn_t *t = NULL;
    balloc_t *a = NULL;
    uint8_t *obytes = NULL;
    uint64_t *offs = NULL;
    memset(&x, 0, sizeof x);
    x.src = src;
    x.dst = dst;
    *err = 0;
    if (d->data_size > MAP_CAP || d->offsets_size > MAP_CAP || d->offsets_size % 8 ||
        d->buffers_size > MAP_CAP || d->buffers_size % 8) {
        *err = -LE_INVAL;
        return NULL;
    }
    x.dsize = d->data_size;
    x.data = malloc(x.dsize ? x.dsize : 1);
    obytes = malloc(d->offsets_size ? d->offsets_size : 1);
    if (!x.data || !obytes) {
        *err = -LE_NOMEM;
        goto out;
    }
    if ((x.dsize && mem_copy_out(x.data, d->buffer, x.dsize) < 0) ||
        (d->offsets_size && mem_copy_out(obytes, d->offsets, d->offsets_size) < 0)) {
        *err = -LE_FAULT;
        goto out;
    }
    x.nobj = (uint32_t)(d->offsets_size / 8);
    offs = malloc(x.nobj ? x.nobj * sizeof *offs : 1);
    x.obj_blob = malloc(x.nobj ? x.nobj * sizeof *x.obj_blob : 1);
    if (!offs || !x.obj_blob) {
        *err = -LE_NOMEM;
        goto out;
    }
    for (uint32_t i = 0; i < x.nobj; i++) {
        offs[i] = ld64(obytes + 8 * i);
        x.obj_blob[i] = -1;
    }
    if (!offsets_valid(offs, x.nobj, x.dsize)) {
        *err = -LE_INVAL;
        goto out;
    }
    /* The buffer: the data, the offsets, then the scatter-gather area of buffers_size bytes. */
    x.sg_cur = ALIGN8(x.dsize) + d->offsets_size;
    x.sg_lim = x.sg_cur + d->buffers_size;
    x.offs = offs;
    for (uint32_t i = 0; i < x.nobj; i++) {
        *err = xl_object(&x, i, offs[i]);
        if (*err)
            goto out;
    }
    uint64_t secctx_at = 0, total = x.sg_lim;
    if (secctx) {
        /* The context string (SELINUX_CTX and its NUL, 14 bytes) fits in 16. */
        secctx_at = x.sg_lim;
        total = secctx_at + 16;
    }
    if (total < 8)
        total = 8;
    /* The buffer: data, offsets, scatter-gather blobs, and the security context. */
    int64_t off = buf_find(dst, total);
    if (off < 0) {
        *err = -LE_NOSPC;
        goto out;
    }
    a = calloc(1, sizeof *a);
    if (!a) {
        *err = -LE_NOMEM;
        goto out;
    }
    a->off = (uint64_t)off;
    a->size = total;
    a->data_size = x.dsize;
    a->offs_size = d->offsets_size;
    a->offsets = offs;
    a->noffs = x.nobj;
    offs = NULL;
    t = calloc(1, sizeof *t);
    if (!t) {
        *err = -LE_NOMEM;
        goto out;
    }
    for (uint32_t i = 0; i < x.nfix; i++)
        st64(x.data + x.fix[i].field, dst->map_va + a->off + x.fix[i].blob_at);
    /* A child's address goes into its parent's blob, before the blobs are written. */
    for (uint32_t i = 0; i < x.npfix; i++)
        st64(x.blobs[x.pfix[i].blob].data + x.pfix[i].poff, dst->map_va + a->off + x.pfix[i].at);
    if (buf_write(dst, a->off, x.data, (size_t)x.dsize) < 0 ||
        buf_write(dst, a->off + ALIGN8(x.dsize), obytes, (size_t)d->offsets_size) < 0) {
        *err = -LE_NOMEM;
        goto out_t;
    }
    for (uint32_t i = 0; i < x.nblobs; i++)
        if (buf_write(dst, a->off + x.blobs[i].at, x.blobs[i].data, (size_t)x.blobs[i].len) < 0) {
            *err = -LE_NOMEM;
            goto out_t;
        }
    if (secctx) {
        /* The sender's context, as the driver supplies it: every process is SELINUX_CTX. */
        if (buf_write(dst, a->off + secctx_at, SELINUX_CTX, sizeof SELINUX_CTX) < 0) {
            *err = -LE_NOMEM;
            goto out_t;
        }
    }
    /* The buffer is in place: it is linked into dst, and the translated fds are noted
     * at their buffer offsets. */
    a->next = dst->allocs;
    dst->allocs = a;
    t->buf = a;
    t->secctx = secctx;
    t->secctx_at = secctx_at;
    for (uint32_t i = 0; i < x.nfds; i++)
        x.fds[i].at += a->off;
    t->fds = x.fds;
    t->nfds = x.nfds;
    x.fds = NULL;
    x.nfds = 0;
    xl_free(&x);
    free(obytes);
    return t;

out_t:
    free(t);
    t = NULL;
out:
    if (a) {
        free(a->offsets);
        free(a);
    }
    free(offs);
    for (uint32_t i = 0; i < x.nfds; i++)
        vfs_desc_drop(x.fds[i].d);
    free(x.fds);
    xl_free(&x);
    free(obytes);
    return NULL;
}

/* ---------- commands and returns ---------- */

/* binder_transaction_data, from the guest's bytes (the handle is the low word of target). */
/* binder_transaction_data (64 bytes); with sg, binder_transaction_data_sg adds buffers_size. */
static void btd_parse(const uint8_t *p, btd_t *d, int sg)
{
    d->target = ld64(p);
    d->cookie = ld64(p + 8);
    d->code = ld32(p + 16);
    d->flags = ld32(p + 20);
    d->sender_pid = (int32_t)ld32(p + 24);
    d->sender_euid = ld32(p + 28);
    d->data_size = ld64(p + 32);
    d->offsets_size = ld64(p + 40);
    d->buffer = ld64(p + 48);
    d->offsets = ld64(p + 56);
    d->buffers_size = sg ? ld64(p + 64) : 0;
}

static void queue_txn_complete(bthread_t *t)
{
    bwork_t *w = work_new(WK_TXN_COMPLETE, BR_TRANSACTION_COMPLETE, 0, 0, NULL);
    if (w)
        queue_thread(t, w);
}

static void queue_failed(bthread_t *t, uint32_t br)
{
    BT("fail to tid=%llu: %s", (unsigned long long)t->tid, bt_br(br));
    bwork_t *w = work_new(WK_FAILED, br, 0, 0, NULL);
    if (w)
        queue_thread(t, w);
}

/* Frees a buffer (BC_FREE_BUFFER). A oneway buffer that frees lets the next oneway
 * request to its node go to the proc. */
static void free_buffer(bproc_t *b, balloc_t *a)
{
    buf_release_refs(b, a);
    for (balloc_t **pp = &b->allocs; *pp; pp = &(*pp)->next)
        if (*pp == a) {
            *pp = a->next;
            break;
        }
    bnode_t *n = a->oneway_node;
    free(a->offsets);
    free(a);
    if (!n)
        return;
    btxn_t *nx = n->async;
    if (nx) {
        n->async = nx->async_next;
        if (!n->async)
            n->async_tail = NULL;
        bwork_t *w = work_new(WK_TXN, txn_br(nx), 0, 0, nx);
        if (w && n->proc)
            queue_proc(n->proc, w);
        else
            txn_free(nx);
    } else {
        n->async_busy = 0;
    }
}

/* Installs the fds of a transaction in the running proc (the receiver, b), and writes
 * each fd number into the buffer. A failed install writes -1. */
static void txn_install_fds(bproc_t *b, btxn_t *x)
{
    for (uint32_t i = 0; i < x->nfds; i++) {
        int fd = vfs_desc_install(x->fds[i].d, 1);
        uint8_t v[4];
        st32(v, fd < 0 ? 0xFFFFFFFFu : (uint32_t)fd);
        buf_write(b, x->fds[i].at, v, 4);
    }
    free(x->fds);
    x->fds = NULL;
    x->nfds = 0;
}

/* The bytes a work item takes in the read buffer, its BR_ command included. */
static uint64_t work_size(const bwork_t *w)
{
    switch (w->kind) {
    case WK_TXN:
        return 4 + (w->txn->secctx ? 72 : 64);
    case WK_REPLY:
        return 4 + 64;
    case WK_OBJREF:
        return 4 + 16;
    case WK_DEAD_BINDER:
    case WK_CLEAR_DONE:
        return 4 + 8;
    default:
        return 4;
    }
}

/* Writes one work item at guest address va (the read buffer of thread t, in proc b). */
static int emit(bproc_t *b, bthread_t *t, bwork_t *w, uint64_t va)
{
    uint8_t out[80];
    memset(out, 0, sizeof out);
    st32(out, w->br);
    switch (w->kind) {
    case WK_TXN_COMPLETE:
    case WK_FAILED:
        break;
    case WK_OBJREF:
        st64(out + 4, w->a);
        st64(out + 12, w->b);
        break;
    case WK_DEAD_BINDER:
    case WK_CLEAR_DONE:
        st64(out + 4, w->a);
        break;
    case WK_TXN:
    case WK_REPLY: {
        btxn_t *x = w->txn;
        uint64_t base = b->map_va + x->buf->off;
        uint64_t dsize = x->buf->data_size;
        st64(out + 4, x->target);
        st64(out + 12, x->cookie);
        st32(out + 20, x->code);
        st32(out + 24, x->flags);
        st32(out + 28, (uint32_t)x->sender_pid);
        st32(out + 32, 0); /* the sender's euid: root */
        st64(out + 36, dsize);
        st64(out + 44, x->buf->offs_size);
        st64(out + 52, base);
        st64(out + 60, base + ALIGN8(dsize));
        if (x->secctx)
            st64(out + 68, base + x->secctx_at);
        txn_install_fds(b, x);
        if (w->kind == WK_TXN) {
            x->to = t;
            BT("deliver request txn=%llu code=%u to pid=%d tid=%llu: node ptr=%llx cookie=%llx sender pid=%d",
               (unsigned long long)x->id, x->code, b->pid, (unsigned long long)t->tid,
               (unsigned long long)x->target, (unsigned long long)x->cookie, x->sender_pid);
            sched_raise(x, t);
            if (!x->oneway) {
                x->recv_next = t->recv;
                t->recv = x;
            } else {
                txn_free(x);
            }
        } else {
            txn_free(x);
        }
        break;
    }
    }
    return mem_copy_in(va, out, work_size(w)) < 0 ? -LE_FAULT : 0;
}

/* Whether the read may take proc work: the thread is not waiting for a reply, and it has
 * none of its own queued. The kernel decides this once per read (wait_for_proc_work), so a
 * oneway call's BR_TRANSACTION_COMPLETE in the thread's list keeps the rest of the read to
 * the thread's own work. */
static int may_take_proc(const bthread_t *t)
{
    return !t->sent && !t->recv && !t->todo.head;
}

static int proc_has_work(bproc_t *b, bthread_t *t, int proc_ok)
{
    if (t->todo.head)
        return 1;
    return proc_ok && b->todo.head != NULL;
}

/* The read half of BINDER_WRITE_READ for thread t. Returns 0 (with *consumed updated),
 * -LE_AGAIN for a nonblocking fd with nothing to read, or -LE_* on an error. A call with
 * nothing to do and a blocking fd sets *block and the wait key instead. */
static int spawn_wanted(bproc_t *b, bthread_t *t);

static int64_t bread(bproc_t *b, bthread_t *t, uint64_t buf, uint64_t size, uint64_t *consumed,
                     int nonblock, int *block, uint64_t *key)
{
    uint64_t start = *consumed, cur = start;
    int items = 0;
    t->waiting_proc = 0; /* a rerun after a block starts again from here */
    if (start == 0 && size >= 4) {
        uint8_t noop[4];
        st32(noop, BR_NOOP);
        if (mem_copy_in(buf, noop, 4) < 0)
            return -LE_FAULT;
        cur = 4; /* counted only if an item follows */
    }
    int proc_ok = may_take_proc(t);
    for (;;) {
        int have = proc_has_work(b, t, proc_ok);
        if (!have)
            break;
        bq_t *q = t->todo.head ? &t->todo : &b->todo;
        bwork_t *w = q->head;
        uint64_t need = work_size(w);
        if (cur + need > size) {
            if (start == 0 && items == 0)
                return -LE_INVAL; /* the buffer cannot hold even one return */
            break;
        }
        q_pop(q);
        int rc = emit(b, t, w, buf + cur);
        free(w);
        if (rc)
            return rc;
        cur += need;
        items++;
    }
    if (items == 0 && start == 0) {
        if (nonblock)
            return -LE_AGAIN;
        int wait_proc = !t->sent && !t->recv;
        t->waiting_proc = wait_proc;
        *block = 1;
        *key = wait_proc ? key_of(b) : key_of(t);
        return 0;
    }
    /* BR_SPAWN_LOOPER: a looper returns with the proc short of idle loopers, under its
     * maximum, and no spawn pending. The driver writes it at the start of the buffer, over
     * the leading BR_NOOP; a read that already returned items (start != 0) sends none. */
    if (start == 0 && cur > 4 && spawn_wanted(b, t)) {
        uint8_t sp[4];
        st32(sp, BR_SPAWN_LOOPER);
        if (mem_copy_in(buf, sp, 4) < 0)
            return -LE_FAULT;
        b->requested_threads++;
    }
    *consumed = cur;
    return 0;
}

/* Whether a looper that is returning from a read should ask for a new looper: no other
 * looper is idle, the proc is under its maximum, and no spawn is pending. */
static int spawn_wanted(bproc_t *b, bthread_t *t)
{
    if (!t->looper || b->requested_threads || b->started_threads >= b->max_threads)
        return 0;
    for (bthread_t *o = b->threads; o; o = o->next)
        if (o != t && o->waiting_proc)
            return 0;
    return 1;
}

/* The write half: each command in turn. Returns a -LE_* for a malformed buffer. */
static int bcmd(bproc_t *b, bthread_t *t, uint32_t cmd, const uint8_t *arg);

static int64_t bwrite(bproc_t *b, bthread_t *t, uint64_t buf, uint64_t size, uint64_t *consumed)
{
    while (*consumed + 4 <= size) {
        uint8_t hd[4];
        if (mem_copy_out(hd, buf + *consumed, 4) < 0)
            return -LE_FAULT;
        uint32_t cmd = ld32(hd);
        uint64_t argsz;
        switch (cmd) {
        case BC_TRANSACTION:
        case BC_REPLY:
            argsz = 64;
            break;
        case BC_TRANSACTION_SG:
        case BC_REPLY_SG:
            argsz = 72;
            break;
        case BC_FREE_BUFFER:
        case BC_DEAD_BINDER_DONE:
        case BC_ATTEMPT_ACQUIRE:
            argsz = 8;
            break;
        case BC_ACQUIRE_RESULT:
        case BC_INCREFS:
        case BC_ACQUIRE:
        case BC_RELEASE:
        case BC_DECREFS:
            argsz = 4;
            break;
        case BC_INCREFS_DONE:
        case BC_ACQUIRE_DONE:
            argsz = 16;
            break;
        case BC_REQUEST_DEATH_NOTIFICATION:
        case BC_CLEAR_DEATH_NOTIFICATION:
            argsz = 12;
            break;
        case BC_REGISTER_LOOPER:
        case BC_ENTER_LOOPER:
        case BC_EXIT_LOOPER:
            argsz = 0;
            break;
        default:
            return -LE_INVAL;
        }
        if (*consumed + 4 + argsz > size)
            return -LE_INVAL;
        uint8_t arg[72];
        if (argsz && mem_copy_out(arg, buf + *consumed + 4, argsz) < 0)
            return -LE_FAULT;
        int rc = bcmd(b, t, cmd, arg);
        *consumed += 4 + argsz;
        if (rc)
            return rc;
    }
    return 0;
}

/* A request from thread t (of proc b) to a handle, or a reply to the request t is
 * serving (reply != 0). A failure is sent back as a return, not as an ioctl error. */
static void cmd_send(bproc_t *b, bthread_t *t, const btd_t *d, int reply)
{
    if (reply) {
        btxn_t *orig = t->recv;
        if (!orig) {
            queue_failed(t, BR_FAILED_REPLY);
            return;
        }
        t->recv = orig->recv_next;
        bthread_t *s = orig->from;
        orig->from = NULL;
        BT("reply to txn=%llu: waiting tid=%llu (pid=%d), replied by pid=%d tid=%llu",
           (unsigned long long)orig->id, s ? (unsigned long long)s->tid : 0ull,
           s ? s->proc->pid : -1, b->pid, (unsigned long long)t->tid);
        if (s)
            sent_remove(s, orig);
        int err = 0;
        btxn_t *rep = NULL;
        if (s && s->proc->mapped) {
            xl_sender = t;
            rep = txn_build(b, s->proc, d, 0, &err);
            xl_sender = NULL;
        }
        if (s) {
            if (rep) {
                rep->reply = 1;
                rep->flags = d->flags;
                rep->to = s;
                rep->id = ++bt_seq;
                BT("reply txn=%llu built as txn=%llu for tid=%llu: flags=%x data=%llu",
                   (unsigned long long)orig->id, (unsigned long long)rep->id, (unsigned long long)s->tid,
                   d->flags, (unsigned long long)d->data_size);
                rep->sender_pid = proc_cur()->pid;
                bwork_t *w = work_new(WK_REPLY, BR_REPLY, 0, 0, rep);
                if (w)
                    queue_thread(s, w);
                else
                    txn_free(rep);
            } else {
                queue_failed(s, BR_FAILED_REPLY);
            }
        }
        txn_free(orig);
        queue_txn_complete(t);
        return;
    }

    uint32_t h = (uint32_t)d->target;
    bref_t *r = ref_for_handle(b, h);
    bnode_t *node = r ? r->node : NULL;
    uint64_t txid = ++bt_seq;
    if (BT_ON)
        bt_send(b, t, d, h, node, txid);
    if (!node) {
        queue_failed(t, h == 0 ? BR_DEAD_REPLY : BR_FAILED_REPLY);
        return;
    }
    if (node->dead || !node->proc) {
        queue_failed(t, BR_DEAD_REPLY);
        return;
    }
    bproc_t *dst = node->proc;
    if (!dst->mapped) {
        queue_failed(t, BR_FAILED_REPLY);
        return;
    }
    int oneway = (d->flags & TF_ONE_WAY) != 0;
    /* The security context is sent when the target node asks for it (the driver's rule). */
    int secctx = (node->flags & FLAT_BINDER_FLAG_TXN_SECURITY_CTX) != 0;
    int err = 0;
    xl_sender = t;
    btxn_t *x = txn_build(b, dst, d, secctx, &err);
    xl_sender = NULL;
    if (!x) {
        queue_failed(t, BR_FAILED_REPLY);
        return;
    }
    x->code = d->code;
    x->flags = d->flags;
    x->id = txid;
    x->sender_pid = proc_cur()->pid;
    x->target = node->ptr;
    x->cookie = node->cookie;
    x->node_set = 1;
    x->node_flags = node->flags;
    x->oneway = oneway;
    queue_txn_complete(t);
    if (oneway) {
        /* One oneway request per node at a time: the rest wait on the node. */
        x->buf->oneway_node = node;
        if (node->async_busy) {
            x->async_next = NULL;
            if (node->async_tail)
                node->async_tail->async_next = x;
            else
                node->async = x;
            node->async_tail = x;
        } else {
            node->async_busy = 1;
            bwork_t *w = work_new(WK_TXN, txn_br(x), 0, 0, x);
            if (w)
                queue_proc(dst, w);
            else
                txn_free(x);
        }
        return;
    }
    x->from = t;
    x->sent_next = t->sent;
    t->sent = x;
    /* Prefer the thread that this thread's calls are already being served by, if it is
     * in the target proc (a nested call), as the driver does. Otherwise any looper. */
    bthread_t *tt = NULL;
    for (btxn_t *s = x->sent_next; s && !tt; s = s->sent_next)
        if (s->to && s->to->proc == dst)
            tt = s->to;
    bwork_t *w = work_new(WK_TXN, txn_br(x), 0, 0, x);
    if (!w) {
        fail_to_sender(x, BR_FAILED_REPLY);
        txn_free(x);
        return;
    }
    if (tt)
        queue_thread(tt, w);
    else
        queue_proc(dst, w);
}

static int bcmd(bproc_t *b, bthread_t *t, uint32_t cmd, const uint8_t *arg)
{
    switch (cmd) {
    case BC_TRANSACTION:
    case BC_TRANSACTION_SG:
    case BC_REPLY:
    case BC_REPLY_SG: {
        btd_t d;
        btd_parse(arg, &d, cmd == BC_TRANSACTION_SG || cmd == BC_REPLY_SG);
        cmd_send(b, t, &d, cmd == BC_REPLY || cmd == BC_REPLY_SG);
        return 0;
    }
    case BC_FREE_BUFFER: {
        uint64_t va = ld64(arg);
        if (!b->mapped || va < b->map_va)
            return 0;
        uint64_t off = va - b->map_va;
        for (balloc_t *a = b->allocs; a; a = a->next)
            if (a->off == off) {
                free_buffer(b, a);
                break;
            }
        return 0;
    }
    case BC_INCREFS:
    case BC_ACQUIRE: {
        bref_t *r = ref_for_handle(b, ld32(arg));
        BT("count %s pid=%d handle=%u: %s", cmd == BC_ACQUIRE ? "ACQUIRE" : "INCREFS", b->pid, ld32(arg),
           r ? "ref" : "NO REF");
        if (r)
            ref_inc(r, cmd == BC_ACQUIRE);
        return 0;
    }
    case BC_RELEASE:
    case BC_DECREFS: {
        bref_t *r = ref_by_handle(b, ld32(arg));
        BT("count %s pid=%d handle=%u: %s", cmd == BC_RELEASE ? "RELEASE" : "DECREFS", b->pid, ld32(arg),
           r ? "ref" : "NO REF");
        if (r)
            ref_dec(r, cmd == BC_RELEASE);
        return 0;
    }
    case BC_INCREFS_DONE:
    case BC_ACQUIRE_DONE: {
        bnode_t *n = node_by_ptr(b, ld64(arg));
        int strong = cmd == BC_ACQUIRE_DONE;
        BT("ack %s pid=%d ptr=%llx: %s", strong ? "ACQUIRE_DONE" : "INCREFS_DONE", b->pid,
           (unsigned long long)ld64(arg), n ? "node" : "NO NODE");
        if (!n)
            return 0;
        if (strong ? n->pend_strong : n->pend_weak) {
            if (strong) {
                n->pend_strong = 0;
                n->has_strong = 1;
                if (n->int_strong == 0) {
                    n->has_strong = 0;
                    node_notify_owner(n, BR_RELEASE);
                }
            } else {
                n->pend_weak = 0;
                n->has_weak = 1;
                if (n->int_weak == 0) {
                    n->has_weak = 0;
                    node_notify_owner(n, BR_DECREFS);
                }
            }
        }
        node_maybe_free(n);
        return 0;
    }
    case BC_REQUEST_DEATH_NOTIFICATION: {
        bref_t *r = ref_for_handle(b, ld32(arg));
        uint64_t cookie = ld64(arg + 4);
        if (!r || r->has_death)
            return 0;
        r->has_death = 1;
        r->death_cookie = cookie;
        if (r->node->dead) {
            r->death_pending = 1;
            bwork_t *w = work_new(WK_DEAD_BINDER, BR_DEAD_BINDER, cookie, 0, NULL);
            if (w)
                queue_proc(b, w);
        }
        return 0;
    }
    case BC_CLEAR_DEATH_NOTIFICATION: {
        bref_t *r = ref_for_handle(b, ld32(arg));
        if (!r || !r->has_death)
            return 0;
        r->has_death = 0;
        bwork_t *w = work_new(WK_CLEAR_DONE, BR_CLEAR_DEATH_NOTIFICATION_DONE, ld64(arg + 4), 0, NULL);
        if (w)
            queue_thread(t, w);
        return 0;
    }
    case BC_DEAD_BINDER_DONE: {
        uint64_t cookie = ld64(arg);
        for (bref_t *r = b->refs; r; r = r->next_proc)
            if (r->death_pending && r->death_cookie == cookie) {
                r->death_pending = 0;
                r->has_death = 0;
                break;
            }
        return 0;
    }
    case BC_REGISTER_LOOPER:
        if (b->requested_threads)
            b->requested_threads--;
        b->started_threads++;
        t->looper = 1;
        return 0;
    case BC_ENTER_LOOPER:
        t->looper = 1;
        return 0;
    case BC_EXIT_LOOPER:
        t->looper = 0;
        return 0;
    case BC_ACQUIRE_RESULT:
    case BC_ATTEMPT_ACQUIRE:
        return 0; /* not used by libbinder; accepted and ignored */
    default:
        return -LE_INVAL;
    }
}

/* BINDER_WRITE_READ. The write half runs first; then the read half, which may wait. */
static int64_t bwr_ioctl(bproc_t *b, uint64_t arg, int nonblock)
{
    uint8_t raw[48];
    if (mem_copy_out(raw, arg, sizeof raw) < 0)
        return -LE_FAULT;
    uint64_t wsize = ld64(raw), wcons = ld64(raw + 8), wbuf = ld64(raw + 16);
    uint64_t rsize = ld64(raw + 24), rcons = ld64(raw + 32), rbuf = ld64(raw + 40);
    bthread_t *t = thread_of(b, 1);
    if (!t)
        return -LE_NOMEM;
    int64_t ret = 0;
    int block = 0;
    uint64_t key = 0;
    if (wsize > wcons)
        ret = bwrite(b, t, wbuf, wsize, &wcons);
    if (ret == 0 && rsize > 0) {
        ret = bread(b, t, rbuf, rsize, &rcons, nonblock, &block, &key);
    }
    st64(raw + 8, wcons);
    st64(raw + 32, rcons);
    if (mem_copy_in(arg, raw, sizeof raw) < 0)
        return -LE_FAULT;
    if (block) {
        /* The rerun takes the write half as consumed (write_consumed is in guest memory). */
        sched_block_restart(key);
        return 0;
    }
    return ret;
}

/* Drops a thread that has finished (BINDER_THREAD_EXIT), if it has no work left. */
static void thread_exit(bproc_t *b)
{
    bthread_t *t = thread_of(b, 0);
    if (!t || t->sent || t->recv || t->todo.head)
        return;
    for (bthread_t **pp = &b->threads; *pp; pp = &(*pp)->next)
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    free(t);
}

int64_t binder_ioctl(binder_proc_t *b, uint32_t cmd, uint64_t arg, int nonblock)
{
    uint8_t buf[24];
    switch (cmd) {
    case BINDER_WRITE_READ:
        return bwr_ioctl(b, arg, nonblock);
    case BINDER_VERSION:
        st32(buf, BINDER_PROTOCOL);
        return mem_copy_in(arg, buf, 4) < 0 ? -LE_FAULT : 0;
    case BINDER_SET_MAX_THREADS:
        if (mem_copy_out(buf, arg, 4) < 0)
            return -LE_FAULT;
        b->max_threads = ld32(buf);
        return 0;
    case BINDER_SET_IDLE_TIMEOUT:
        return mem_copy_out(buf, arg, 8) < 0 ? -LE_FAULT : -LE_INVAL;
    case BINDER_SET_IDLE_PRIORITY:
        return mem_copy_out(buf, arg, 4) < 0 ? -LE_FAULT : -LE_INVAL;
    case BINDER_SET_CONTEXT_MGR: {
        if (b->ctx->mgr)
            return -LE_BUSY;
        bnode_t *n = node_new(b, 0, 0, 0);
        if (!n)
            return -LE_NOMEM;
        mgr_counts_acked(n);
        b->ctx->mgr = n;
        return 0;
    }
    case BINDER_SET_CONTEXT_MGR_EXT: {
        if (mem_copy_out(buf, arg, 24) < 0)
            return -LE_FAULT;
        if (b->ctx->mgr)
            return -LE_BUSY;
        /* The flags are the node's (FLAT_BINDER_FLAG_TXN_SECURITY_CTX, ACCEPTS_FDS, ...). */
        bnode_t *n = node_new(b, ld64(buf + 8), ld64(buf + 16), ld32(buf + 4));
        if (!n)
            return -LE_NOMEM;
        mgr_counts_acked(n);
        b->ctx->mgr = n;
        return 0;
    }
    case BINDER_THREAD_EXIT:
        thread_exit(b);
        return 0;
    case BINDER_ENABLE_ONEWAY_SPAM_DETECTION:
        return mem_copy_out(buf, arg, 4) < 0 ? -LE_FAULT : 0;
    case BINDER_GET_NODE_INFO_FOR_REF: {
        if (mem_copy_out(buf, arg, 24) < 0)
            return -LE_FAULT;
        bref_t *r = ref_by_handle(b, ld32(buf));
        if (!r)
            return -LE_NOENT;
        st32(buf + 4, (uint32_t)r->node->int_strong);
        st32(buf + 8, (uint32_t)r->node->int_weak);
        st32(buf + 12, 0);
        st32(buf + 16, 0);
        st32(buf + 20, 0);
        return mem_copy_in(arg, buf, 24) < 0 ? -LE_FAULT : 0;
    }
    case BINDER_GET_NODE_DEBUG_INFO: {
        if (mem_copy_out(buf, arg, 24) < 0)
            return -LE_FAULT;
        uint64_t from = ld64(buf);
        bnode_t *best = NULL;
        for (bnode_t *n = b->nodes; n; n = n->next)
            if (n->ptr >= from && (!best || n->ptr < best->ptr))
                best = n;
        memset(buf, 0, 24);
        if (best) {
            st64(buf, best->ptr);
            st64(buf + 8, best->cookie);
            st32(buf + 16, best->has_strong);
            st32(buf + 20, best->has_weak);
        }
        return mem_copy_in(arg, buf, 24) < 0 ? -LE_FAULT : 0;
    }
    case BINDER_FREEZE:
        return -LE_INVAL; /* freezing is not modelled */
    case BINDER_GET_FROZEN_INFO:
        if (mem_copy_out(buf, arg, 12) < 0)
            return -LE_FAULT;
        memset(buf, 0, 12); /* no proc is frozen */
        return mem_copy_in(arg, buf, 12) < 0 ? -LE_FAULT : 0;
    default:
        return -LE_INVAL;
    }
}

binder_proc_t *binder_open(int which)
{
    if (which < 0 || which >= NDEV)
        return NULL;
    bproc_t *b = calloc(1, sizeof *b);
    if (b) {
        b->ctx = &contexts[which];
        proc_t *p = proc_cur();
        b->pid = p ? p->pid : 0;
    }
    return b;
}

/* A proc goes away (its last description closed). Requests it had in hand fail for
 * their senders; the refs it holds drop their counts; its nodes die, and the refs to
 * them get death notifications. Its buffers and threads are freed. */
void binder_release(binder_proc_t *b)
{
    bctx_t *c = b->ctx;
    b->closing = 1;
    if (c->mgr && c->mgr->proc == b)
        c->mgr = NULL;
    for (bthread_t *t = b->threads; t; t = t->next) {
        while (t->recv) {
            btxn_t *x = t->recv;
            t->recv = x->recv_next;
            fail_to_sender(x, BR_DEAD_REPLY);
            txn_free(x);
        }
        for (btxn_t *x = t->sent; x; x = x->sent_next)
            x->from = NULL; /* its reply (when the receiver makes one) goes nowhere */
        t->sent = NULL;
    }
    for (bthread_t *t = b->threads; t; t = t->next)
        queue_drain(&t->todo);
    queue_drain(&b->todo);
    while (b->refs) {
        bref_t *r = b->refs;
        bnode_t *n = r->node;
        if (r->strong)
            node_dec(n, 1);
        if (r->strong + r->weak)
            node_dec(n, 0);
        r->strong = r->weak = 0;
        ref_free(r);
    }
    bnode_t *n = b->nodes;
    b->nodes = NULL;
    while (n) {
        bnode_t *next = n->next;
        n->proc = NULL;
        n->dead = 1;
        for (bref_t *r = n->refs; r; r = r->next_node)
            if (r->has_death && !r->death_pending && r->proc && !r->proc->closing) {
                r->death_pending = 1;
                bwork_t *w = work_new(WK_DEAD_BINDER, BR_DEAD_BINDER, r->death_cookie, 0, NULL);
                if (w)
                    queue_proc(r->proc, w);
            }
        for (btxn_t *x = n->async; x;) {
            btxn_t *nx = x->async_next;
            txn_free(x);
            x = nx;
        }
        n->async = n->async_tail = NULL;
        node_maybe_free(n);
        n = next;
    }
    for (balloc_t *a = b->allocs; a;) {
        balloc_t *nx = a->next;
        free(a->offsets);
        free(a);
        a = nx;
    }
    b->allocs = NULL;
    for (bthread_t *t = b->threads; t;) {
        bthread_t *nx = t->next;
        free(t);
        t = nx;
    }
    b->threads = NULL;
    if (b->mfd)
        memfd_unref(b->mfd);
    free(b);
}

int binder_mmap_prepare(binder_proc_t *b, uint64_t len, int prot_write, memfd_t **out)
{
    if (prot_write)
        return -LE_PERM;
    if (b->prepared)
        return -LE_BUSY;
    uint64_t size = len > MAP_CAP ? MAP_CAP : len;
    memfd_t *m = memfd_new("binder");
    if (!m)
        return -LE_NOMEM;
    if (memfd_truncate(m, size) < 0) {
        memfd_unref(m);
        return -LE_NOMEM;
    }
    b->mfd = m;
    b->map_size = size;
    b->prepared = 1;
    memfd_ref(m); /* the caller's description holds one reference */
    *out = m;
    return 0;
}

void binder_mmap_done(binder_proc_t *b, uint64_t va)
{
    b->map_va = va;
    b->mapped = 1;
}

int binder_readable(binder_proc_t *b)
{
    bthread_t *t = thread_of(b, 0);
    if (t && t->todo.head)
        return 1;
    int wait_proc = !t || (!t->sent && !t->recv);
    return wait_proc && b->todo.head != NULL;
}
