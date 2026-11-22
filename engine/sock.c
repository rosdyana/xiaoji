/* AF_UNIX sockets. See sock.h for the design: objects, names, connect and accept, the
 * message queues with fd passing, and the blocking rules. */
#include <stdlib.h>
#include <string.h>

#include "fdobj.h"
#include "logd.h"
#include "poll.h"
#include "proc.h"
#include "props.h"
#include "sched.h"
#include "sock.h"
#include "vfs.h"

#define SOCK_DEFAULT_BUF 425984u /* what Linux reports for SO_RCVBUF and SO_SNDBUF by default */
#define SOCK_MIN_BUF 4608u
#define SOCK_MAX_RMEM 212992u /* the largest requested size before doubling (rmem_max) */
#define SOCK_MAX_BACKLOG 4096
#define SOL_SOCKET_LEVEL 1
#define SO_TYPE_OPT 3
#define SO_ERROR_OPT 4
#define SO_SNDBUF_OPT 7
#define SO_RCVBUF_OPT 8
#define SO_PASSCRED_OPT 16
#define SO_PEERCRED_OPT 17
#define SO_RCVTIMEO_OPT 20
#define SO_SNDTIMEO_OPT 21
#define SO_PROTOCOL_OPT 38
#define SO_DOMAIN_OPT 39
#define SO_ACCEPTCONN_OPT 30
#define NS_PER_SEC 1000000000ull

typedef struct msg {
    struct msg *next;
    uint8_t *data;
    size_t len, off; /* off: bytes already read (a stream's head message only) */
    void *fds[SOCK_MAX_FDS];
    int nfds;
    sock_cred_t cred;
    uint8_t from[SOCK_PATH_MAX + 2];
    size_t fromlen;
    int fromfs;
} msg_t;

struct sock {
    int refs;    /* descriptions */
    int hold;    /* internal references: a pending connection, a datagram destination */
    int dead;    /* the close effects have run */
    int type;
    sock_cred_t cred;     /* the creator (a listener's cred is the peer's cred of its clients) */
    sock_cred_t peercred; /* SO_PEERCRED: the other end's creds, as of the connection */
    uint8_t *name;        /* bound name (sun_path bytes), NULL when unbound */
    size_t nlen;
    uint8_t *disp;        /* the name as bound (what getsockname reports) */
    size_t dlen;
    int fsnode;
    int bound;
    struct sock *bnext;   /* the list of live bound sockets */
    int listening, backlog;
    struct sock **acc;    /* pending server-side sockets, in order */
    int nacc, acccap;
    struct sock *peer;    /* a connected stream or seqpacket socket, or a socketpair end */
    int peer_gone;        /* the peer closed */
    int connected;
    struct sock *dest;    /* a connected datagram socket's destination (held) */
    msg_t *qh, *qt;       /* queued messages (the receive side of this socket) */
    size_t qbytes;
    size_t rcvbuf, sndbuf;
    uint64_t rcvtmo, sndtmo; /* ns, 0 for none */
    int passcred, shut_rd, shut_wr;
    const sock_ops *ops; /* an engine endpoint, or NULL */
    void *conn;
};

typedef struct fsnode {
    char *path;
    struct fsnode *next;
} fsnode_t;

static sock_t *bound_list;
static fsnode_t *fs_list;

/* The deadline of a timed wait, per thread: a rerun of the blocked call keeps it. */
typedef struct {
    uint64_t tid;
    const void *key;
    uint64_t deadline;
    int active;
} tmo_t;
static tmo_t tmos[SCHED_MAX_THREADS];

static sock_cred_t cur_cred(void)
{
    proc_t *p = proc_cur();
    sock_cred_t c = {p ? p->pid : 0, p ? p->cred.euid : 0, p ? p->cred.egid : 0};
    return c;
}

static void wake(const sock_t *s)
{
    sched_wake_key((uint64_t)(uintptr_t)s);
    poll_notify();
}

static tmo_t *tmo_find(uint64_t tid)
{
    for (int i = 0; i < SCHED_MAX_THREADS; i++)
        if (tmos[i].active && tmos[i].tid == tid)
            return &tmos[i];
    return NULL;
}

/* A successful (or failed, non-EAGAIN) call ends the thread's timed wait. */
static void tmo_drop(void)
{
    tmo_t *t = tmo_find(sched_tid());
    if (t)
        t->active = 0;
}

int sock_block(const sock_t *key, uint64_t tmo_ns, int nonblock)
{
    if (nonblock)
        return 0;
    uint64_t tid = sched_tid();
    if (!tmo_ns) {
        tmo_drop();
        sched_block_restart((uint64_t)(uintptr_t)key);
        return 1;
    }
    uint64_t now = fdobj_now();
    tmo_t *w = tmo_find(tid);
    if (w && w->key != key) {
        w->active = 0;
        w = NULL;
    }
    if (!w) {
        for (int i = 0; i < SCHED_MAX_THREADS && !w; i++)
            if (!tmos[i].active) {
                w = &tmos[i];
                w->active = 1;
                w->tid = tid;
                w->key = key;
                w->deadline = now + tmo_ns;
            }
        if (!w) { /* no slot: wait without a deadline */
            sched_block_restart((uint64_t)(uintptr_t)key);
            return 1;
        }
    }
    if (now >= w->deadline) {
        w->active = 0;
        return 0;
    }
    sched_block_restart_until((uint64_t)(uintptr_t)key, 1, w->deadline);
    return 1;
}

sock_t *sock_send_target(sock_t *s, sock_t *to)
{
    if (to)
        return to;
    if (s->peer)
        return s->peer;
    if (s->dest)
        return s->dest;
    return s;
}

uint64_t sock_timeo(const sock_t *s, int write)
{
    return write ? s->sndtmo : s->rcvtmo;
}

int sock_io_block(sock_t *s, int write, int nonblock)
{
    sock_t *key = write ? sock_send_target(s, NULL) : s;
    return sock_block(key, sock_timeo(s, write), nonblock);
}

/* ---------- lifetime ---------- */

static void msg_free(msg_t *m)
{
    for (int i = 0; i < m->nfds; i++)
        vfs_desc_drop((vfs_fd *)m->fds[i]);
    free(m->data);
    free(m);
}

static void msg_flush(sock_t *s)
{
    while (s->qh) {
        msg_t *m = s->qh;
        s->qh = m->next;
        msg_free(m);
    }
    s->qt = NULL;
    s->qbytes = 0;
}

static void bound_add(sock_t *s)
{
    s->bnext = bound_list;
    bound_list = s;
    s->bound = 1;
}

static void bound_del(sock_t *s)
{
    for (sock_t **pp = &bound_list; *pp; pp = &(*pp)->bnext) {
        if (*pp == s) {
            *pp = s->bnext;
            break;
        }
    }
    s->bnext = NULL;
    s->bound = 0;
}

static void sock_free(sock_t *s)
{
    if (s->ops)
        s->ops->close(s->conn);
    free(s->name);
    free(s->disp);
    free(s->acc);
    free(s);
}

static void maybe_free(sock_t *s)
{
    if (s->dead && s->refs == 0 && s->hold == 0)
        sock_free(s);
}

static void drop_hold(sock_t *s)
{
    s->hold--;
    maybe_free(s);
}

/* The close effects: the name is unbound, the peer sees a hangup, queued messages are
 * dropped (their descriptions too), and pending connections are abandoned. */
static void sock_release(sock_t *s)
{
    if (s->dead)
        return;
    s->dead = 1;
    if (s->bound)
        bound_del(s);
    wake(s);
    if (s->peer) {
        sock_t *p = s->peer;
        s->peer = NULL;
        if (p->peer == s)
            p->peer = NULL;
        p->peer_gone = 1;
        wake(p);
    }
    if (s->dest) {
        sock_t *d = s->dest;
        s->dest = NULL;
        drop_hold(d);
    }
    msg_flush(s);
    if (s->listening) {
        for (int i = 0; i < s->nacc; i++) {
            sock_t *c = s->acc[i];
            s->acc[i] = NULL;
            c->hold--;
            sock_release(c);
            maybe_free(c);
        }
        s->nacc = 0;
        s->listening = 0;
    }
}

static sock_t *sock_alloc(int type)
{
    sock_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->type = type;
    s->rcvbuf = SOCK_DEFAULT_BUF;
    s->sndbuf = SOCK_DEFAULT_BUF;
    return s;
}

sock_t *sock_new(int type)
{
    sock_t *s = sock_alloc(type);
    if (s) {
        s->refs = 1;
        s->cred = cur_cred();
    }
    return s;
}

int sock_pair(int type, sock_t **a, sock_t **b)
{
    sock_t *x = sock_new(type);
    sock_t *y = sock_new(type);
    if (!x || !y) {
        if (x)
            sock_unref(x);
        if (y)
            sock_unref(y);
        return -LE_NOMEM;
    }
    x->peer = y;
    y->peer = x;
    x->connected = y->connected = 1;
    x->peercred = y->peercred = cur_cred();
    *a = x;
    *b = y;
    return 0;
}

int sock_type(const sock_t *s)
{
    return s->type;
}

int sock_is_dgram(const sock_t *s)
{
    return s->type == SOCK_T_DGRAM || s->type == SOCK_T_SEQPACKET;
}

void sock_ref(sock_t *s)
{
    s->refs++;
}

void sock_unref(sock_t *s)
{
    if (!s || --s->refs > 0)
        return;
    sock_release(s);
    maybe_free(s);
}

int sock_connected(const sock_t *s)
{
    return s->connected;
}

/* ---------- names ---------- */

sock_t *sock_find(const uint8_t *name, size_t nlen, int fsnode)
{
    for (sock_t *s = bound_list; s; s = s->bnext)
        if (s->nlen == nlen && s->fsnode == fsnode && !memcmp(s->name, name, nlen))
            return s;
    return NULL;
}

int sock_fsnode_add(const char *guest)
{
    if (sock_is_fsnode(guest))
        return 0;
    fsnode_t *n = malloc(sizeof *n);
    if (!n)
        return -LE_NOMEM;
    size_t gl = strlen(guest);
    n->path = malloc(gl + 1);
    if (n->path)
        memcpy(n->path, guest, gl + 1);
    if (!n->path) {
        free(n);
        return -LE_NOMEM;
    }
    n->next = fs_list;
    fs_list = n;
    return 0;
}

int sock_is_fsnode(const char *guest)
{
    for (fsnode_t *n = fs_list; n; n = n->next)
        if (!strcmp(n->path, guest))
            return 1;
    return 0;
}

void sock_fsnode_del(const char *guest)
{
    for (fsnode_t **pp = &fs_list; *pp; pp = &(*pp)->next) {
        if (!strcmp((*pp)->path, guest)) {
            fsnode_t *n = *pp;
            *pp = n->next;
            free(n->path);
            free(n);
            return;
        }
    }
}

int sock_bind(sock_t *s, const uint8_t *name, size_t nlen, int fsnode, const uint8_t *disp, size_t dlen)
{
    if (s->bound || s->ops || s->listening)
        return -LE_INVAL;
    if (sock_find(name, nlen, fsnode))
        return -LE_ADDRINUSE;
    s->name = malloc(nlen ? nlen : 1);
    if (!s->name)
        return -LE_NOMEM;
    memcpy(s->name, name, nlen);
    s->nlen = nlen;
    s->disp = malloc(dlen ? dlen : 1);
    if (!s->disp) {
        free(s->name);
        s->name = NULL;
        return -LE_NOMEM;
    }
    memcpy(s->disp, disp, dlen);
    s->dlen = dlen;
    s->fsnode = fsnode != 0;
    bound_add(s);
    return 0;
}

int sock_can_bind(const sock_t *s)
{
    return !s->bound && !s->ops && !s->listening;
}

int sock_passcred(const sock_t *s)
{
    return s->passcred;
}

int sock_listen(sock_t *s, int backlog)
{
    if (s->type == SOCK_T_DGRAM || s->ops)
        return -LE_NOTSUP;
    if (!s->bound || s->connected)
        return -LE_INVAL;
    if (backlog < 0)
        backlog = SOCK_MAX_BACKLOG;
    if (backlog > SOCK_MAX_BACKLOG)
        backlog = SOCK_MAX_BACKLOG;
    s->listening = 1;
    s->backlog = backlog;
    s->cred = cur_cred();
    wake(s);
    return 0;
}

size_t sock_addr(const sock_t *s, int peer, uint8_t *sa, size_t cap)
{
    const sock_t *t = s;
    if (peer)
        t = s->peer ? s->peer : s->dest;
    uint8_t tmp[2 + SOCK_PATH_MAX + 1];
    size_t len = 2;
    tmp[0] = 1; /* AF_UNIX, little-endian */
    tmp[1] = 0;
    if (t && t->name) {
        memcpy(tmp + 2, t->disp, t->dlen);
        len = 2 + t->dlen;
        if (t->fsnode)
            tmp[len++] = 0;
    }
    memcpy(sa, tmp, len < cap ? len : cap);
    return len;
}

/* ---------- endpoints ---------- */

int sock_is_endpoint(const uint8_t *name, size_t nlen, int dgram)
{
    return sock_endpoint_ops(name, nlen, dgram) != NULL;
}

const sock_ops *sock_endpoint_ops(const uint8_t *name, size_t nlen, int dgram)
{
    if (dgram) {
        if (nlen == strlen(LOGD_PATH) && !memcmp(name, LOGD_PATH, nlen))
            return logd_ops();
    } else if (nlen == strlen(PROPS_SERVICE_PATH) && !memcmp(name, PROPS_SERVICE_PATH, nlen)) {
        return props_service_ops();
    }
    return NULL;
}

/* ---------- connect and accept ---------- */

int sock_connect(sock_t *s, const uint8_t *name, size_t nlen, int fsnode, int exists, sock_t **wait)
{
    if (s->listening)
        return -LE_INVAL;
    int dg = s->type == SOCK_T_DGRAM;
    if (s->ops || (s->connected && !dg))
        return -LE_ISCONN;
    if (fsnode) {
        const sock_ops *ops = sock_endpoint_ops(name, nlen, dg);
        if (ops) {
            void *conn = ops->connect();
            if (!conn)
                return -LE_CONNREFUSED;
            s->ops = ops;
            s->conn = conn;
            s->connected = 1;
            return 0;
        }
    }
    sock_t *l = sock_find(name, nlen, fsnode);
    if (!l || l->type != s->type) {
        if (fsnode && !exists)
            return -LE_NOENT;
        return -LE_CONNREFUSED;
    }
    if (dg) {
        if (s->dest) {
            sock_t *old = s->dest;
            s->dest = NULL;
            drop_hold(old);
        }
        l->hold++;
        s->dest = l;
        s->connected = 1;
        s->peercred = l->cred;
        tmo_drop();
        return 0;
    }
    if (!l->listening)
        return -LE_CONNREFUSED;
    if (l->nacc > l->backlog) {
        if (wait)
            *wait = l;
        return -LE_AGAIN;
    }
    sock_t *c = sock_alloc(s->type);
    if (!c)
        return -LE_NOMEM;
    if (l->nacc == l->acccap) {
        int cap = l->acccap ? 2 * l->acccap : 4;
        sock_t **a = realloc(l->acc, (size_t)cap * sizeof *a);
        if (!a) {
            free(c);
            return -LE_NOMEM;
        }
        l->acc = a;
        l->acccap = cap;
    }
    c->hold = 1; /* the accept queue's reference */
    c->connected = 1;
    c->peer = s;
    c->peercred = cur_cred();
    c->cred = l->cred;
    s->peer = c;
    s->connected = 1;
    s->peercred = l->cred;
    l->acc[l->nacc++] = c;
    wake(l);
    tmo_drop();
    return 0;
}

int sock_accept(sock_t *l, sock_t **out)
{
    if (!l->listening)
        return -LE_INVAL;
    if (!l->nacc)
        return -LE_AGAIN;
    sock_t *c = l->acc[0];
    memmove(l->acc, l->acc + 1, (size_t)(l->nacc - 1) * sizeof *l->acc);
    l->nacc--;
    c->hold--;
    c->refs++; /* the caller's description */
    *out = c;
    wake(l);
    tmo_drop();
    return 0;
}

int sock_shutdown(sock_t *s, int how)
{
    if (how < 0 || how > 2)
        return -LE_INVAL;
    if (s->ops || !s->connected)
        return -LE_NOTCONN;
    if (how != 1)
        s->shut_rd = 1;
    if (how != 0)
        s->shut_wr = 1;
    wake(s);
    if (s->peer)
        wake(s->peer);
    return 0;
}

/* ---------- data ---------- */

/* Readers see end of file when nothing is queued and no more data can come. */
static int eof_of(const sock_t *s)
{
    return s->shut_rd || s->peer_gone || (s->peer && s->peer->shut_wr);
}

static msg_t *msg_new(const uint8_t *data, size_t n, const sock_t *from)
{
    msg_t *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    if (n) {
        m->data = malloc(n);
        if (!m->data) {
            free(m);
            return NULL;
        }
        memcpy(m->data, data, n);
    }
    m->len = n;
    if (from && from->name) {
        memcpy(m->from, from->disp, from->dlen);
        m->fromlen = from->dlen;
        m->fromfs = from->fsnode;
    }
    return m;
}

static void enqueue(sock_t *t, msg_t *m)
{
    m->next = NULL;
    if (t->qt)
        t->qt->next = m;
    else
        t->qh = m;
    t->qt = m;
    t->qbytes += m->len;
    wake(t);
}

int64_t sock_send(sock_t *s, const uint8_t *buf, size_t n, void **fds, int nfds, sock_t *to)
{
    if (s->ops) {
        if (nfds)
            return -LE_NOTSUP;
        return s->ops->write(s->conn, buf, n);
    }
    if (s->shut_wr)
        return -LE_PIPE;
    sock_t *t = to ? to : (s->peer ? s->peer : s->dest);
    if (!t) {
        if (s->type == SOCK_T_DGRAM)
            return -LE_DESTADDRREQ;
        return s->connected ? -LE_PIPE : -LE_NOTCONN;
    }
    if (t->dead)
        return s->type == SOCK_T_DGRAM ? -LE_CONNREFUSED : -LE_PIPE;
    if (t->shut_rd)
        return -LE_PIPE;
    size_t k;
    if (sock_is_dgram(s)) {
        if (n > t->rcvbuf)
            return -LE_MSGSIZE;
        if (t->qbytes + n > t->rcvbuf)
            return -LE_AGAIN;
        k = n;
    } else {
        size_t space = t->rcvbuf > t->qbytes ? t->rcvbuf - t->qbytes : 0;
        if (n > 0 && space == 0)
            return -LE_AGAIN;
        k = n < space ? n : space;
    }
    msg_t *m = msg_new(buf, k, s);
    if (!m)
        return -LE_NOMEM;
    for (int i = 0; i < nfds; i++)
        m->fds[i] = fds[i];
    m->nfds = nfds;
    m->cred = cur_cred();
    enqueue(t, m);
    tmo_drop();
    return (int64_t)k;
}

/* Moves (or, for a peek, holds) the descriptions of a message into the receive request. */
static void take_fds(sock_recv_t *r, msg_t *m, int peek)
{
    for (int i = 0; i < m->nfds; i++) {
        vfs_fd *d = (vfs_fd *)m->fds[i];
        if (peek)
            vfs_desc_ref(d);
        r->fds[r->nfds++] = d;
    }
    if (!peek)
        m->nfds = 0;
}

static int64_t recv_stream(sock_t *s, sock_recv_t *r)
{
    size_t got = 0;
    if (r->peek) {
        for (msg_t *m = s->qh; m && got < r->cap; m = m->next) {
            if (m->nfds) {
                if (got > 0 || r->nfds > 0)
                    break;
                take_fds(r, m, 1);
                r->cred = m->cred;
            } else if (got == 0) {
                r->cred = m->cred;
            }
            size_t avail = m->len - m->off;
            size_t take = avail < r->cap - got ? avail : r->cap - got;
            memcpy(r->buf + got, m->data + m->off, take);
            got += take;
        }
        return (int64_t)got;
    }
    while (s->qh && got < r->cap) {
        msg_t *m = s->qh;
        if (m->nfds) {
            if (got > 0 || r->nfds > 0)
                break;
            take_fds(r, m, 0);
            r->cred = m->cred;
        } else if (got == 0) {
            r->cred = m->cred;
        }
        size_t avail = m->len - m->off;
        size_t take = avail < r->cap - got ? avail : r->cap - got;
        memcpy(r->buf + got, m->data + m->off, take);
        got += take;
        m->off += take;
        s->qbytes -= take;
        if (m->off < m->len)
            break;
        s->qh = m->next;
        if (!s->qh)
            s->qt = NULL;
        msg_free(m);
    }
    return (int64_t)got;
}

static int64_t recv_msg(sock_t *s, sock_recv_t *r)
{
    msg_t *m = s->qh;
    size_t full = m->len - m->off;
    size_t take = full < r->cap ? full : r->cap;
    memcpy(r->buf, m->data + m->off, take);
    r->msglen = full;
    r->trunc = full > r->cap;
    r->cred = m->cred;
    r->fromlen = m->fromlen;
    r->fromfs = m->fromfs;
    memcpy(r->from, m->from, m->fromlen);
    if (m->nfds)
        take_fds(r, m, r->peek);
    if (!r->peek) {
        s->qh = m->next;
        if (!s->qh)
            s->qt = NULL;
        s->qbytes -= full;
        msg_free(m);
    }
    return (int64_t)take;
}

int64_t sock_recv(sock_t *s, sock_recv_t *r)
{
    r->nfds = 0;
    r->msglen = 0;
    r->trunc = 0;
    r->fromlen = 0;
    r->fromfs = 0;
    if (s->ops) {
        if (!r->cap)
            return 0;
        return s->ops->read(s->conn, r->buf, r->cap);
    }
    if (!s->qh) {
        if (s->type != SOCK_T_DGRAM && s->listening)
            return -LE_NOTCONN;
        if (s->type != SOCK_T_DGRAM && !s->connected)
            return -LE_NOTCONN;
        if (eof_of(s))
            return 0;
        return -LE_AGAIN;
    }
    if (!sock_is_dgram(s) && r->waitall && r->cap) {
        size_t need = r->cap < s->rcvbuf ? r->cap : s->rcvbuf;
        if (s->qbytes < need && !eof_of(s))
            return -LE_AGAIN;
    }
    int64_t got = sock_is_dgram(s) ? recv_msg(s, r) : recv_stream(s, r);
    /* A stream whose queued bytes were all empty messages: nothing was read, so wait. */
    if (!sock_is_dgram(s) && got == 0 && r->cap && !r->nfds && s->qh == NULL && !eof_of(s))
        return -LE_AGAIN;
    tmo_drop();
    if (!r->peek)
        wake(s);
    return got;
}

int64_t sock_read(sock_t *s, void *buf, size_t n)
{
    sock_recv_t r;
    memset(&r, 0, sizeof r);
    r.buf = buf;
    r.cap = n;
    int64_t got = sock_recv(s, &r);
    for (int i = 0; i < r.nfds; i++)
        vfs_desc_drop((vfs_fd *)r.fds[i]);
    return got;
}

int64_t sock_write(sock_t *s, const void *buf, size_t n)
{
    return sock_send(s, (const uint8_t *)buf, n, NULL, 0, NULL);
}

/* ---------- readiness and options ---------- */

void sock_state(const sock_t *s, int *readable, int *writable, int *hup)
{
    *readable = *writable = *hup = 0;
    if (s->ops) {
        *readable = *writable = 1;
        return;
    }
    if (s->listening) {
        *readable = s->nacc > 0;
        return;
    }
    if (s->qh || eof_of(s))
        *readable = 1;
    if (s->peer_gone)
        *hup = 1;
    if (s->type == SOCK_T_DGRAM) {
        *writable = 1;
        return;
    }
    const sock_t *t = s->peer;
    if (t && !t->shut_rd && t->qbytes < t->rcvbuf)
        *writable = 1;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static void put_timeval(uint8_t *p, uint64_t ns)
{
    put64(p, ns / NS_PER_SEC);
    put64(p + 8, (ns % NS_PER_SEC) / 1000);
}

int sock_getopt(sock_t *s, int level, int opt, uint8_t *out, size_t *len)
{
    if (level != SOL_SOCKET_LEVEL)
        return -LE_NOPROTOOPT;
    uint8_t v[16];
    size_t n = 4;
    switch (opt) {
    case SO_TYPE_OPT: put32(v, (uint32_t)s->type); break;
    case SO_ERROR_OPT: put32(v, 0); break;
    case SO_ACCEPTCONN_OPT: put32(v, (uint32_t)s->listening); break;
    case SO_DOMAIN_OPT: put32(v, 1); break;
    case SO_PROTOCOL_OPT: put32(v, 0); break;
    case SO_PASSCRED_OPT: put32(v, (uint32_t)s->passcred); break;
    case SO_SNDBUF_OPT: put32(v, (uint32_t)s->sndbuf); break;
    case SO_RCVBUF_OPT: put32(v, (uint32_t)s->rcvbuf); break;
    case SO_PEERCRED_OPT:
        put32(v, (uint32_t)s->peercred.pid);
        put32(v + 4, s->peercred.uid);
        put32(v + 8, s->peercred.gid);
        n = 12;
        break;
    case SO_RCVTIMEO_OPT: put_timeval(v, s->rcvtmo); n = 16; break;
    case SO_SNDTIMEO_OPT: put_timeval(v, s->sndtmo); n = 16; break;
    default: return -LE_NOPROTOOPT;
    }
    memcpy(out, v, n < *len ? n : *len);
    *len = n;
    return 0;
}

int sock_setopt(sock_t *s, int level, int opt, const uint8_t *val, size_t len)
{
    if (level != SOL_SOCKET_LEVEL)
        return -LE_NOPROTOOPT;
    switch (opt) {
    case SO_PASSCRED_OPT:
        if (len < 4)
            return -LE_INVAL;
        s->passcred = get32(val) != 0;
        return 0;
    case SO_SNDBUF_OPT:
    case SO_RCVBUF_OPT: {
        if (len < 4)
            return -LE_INVAL;
        uint32_t req = get32(val);
        if (req > SOCK_MAX_RMEM)
            req = SOCK_MAX_RMEM;
        uint32_t eff = 2 * req;
        if (eff < SOCK_MIN_BUF)
            eff = SOCK_MIN_BUF;
        if (opt == SO_SNDBUF_OPT)
            s->sndbuf = eff;
        else
            s->rcvbuf = eff;
        return 0;
    }
    case SO_RCVTIMEO_OPT:
    case SO_SNDTIMEO_OPT: {
        if (len < 16)
            return -LE_INVAL;
        int64_t sec = (int64_t)get64(val);
        int64_t usec = (int64_t)get64(val + 8);
        if (sec < 0 || usec < 0 || usec >= 1000000)
            return -LE_DOM;
        uint64_t ns = sec > 1000000000 ? (uint64_t)1000000000 * NS_PER_SEC
                                       : (uint64_t)sec * NS_PER_SEC + (uint64_t)usec * 1000;
        if (opt == SO_SNDTIMEO_OPT)
            s->sndtmo = ns;
        else
            s->rcvtmo = ns;
        return 0;
    }
    default: return -LE_NOPROTOOPT;
    }
}
