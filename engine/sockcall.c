/* The socket syscalls. They translate guest memory and descriptors for sock.c.
 *
 * Restart rule: a call that blocks (sock_block returns 1) returns 0 with the thread parked,
 * and runs again from its start. Before it parks it has changed nothing, except a stream
 * send: its progress is kept in the thread (io_done), and the descriptions of SCM_RIGHTS are
 * taken only by a send that succeeds, so a restart never sends twice. */
#include "sockcall.h"

#include <stdlib.h>
#include <string.h>

#include "cpu_a64.h"
#include "linux.h"
#include "mem.h"
#include "proc.h"
#include "sched.h"
#include "signals.h"
#include "sock.h"
#include "vfs.h"

#define SC_IOV_MAX 1024
#define SC_DGRAM_MAX 65536
#define SC_RECV_MAX (1u << 20)
#define SC_CHUNK 65536
#define SC_CTL_MAX 65536
#define SC_SIGPIPE 13
#define SC_AF_UNIX 1
#define SC_AF_NETLINK 16
#define SC_NETLINK_KOBJECT_UEVENT 15
#define SC_SOL_SOCKET 1
#define SC_SCM_RIGHTS 1
#define SC_SCM_CREDENTIALS 2
#define SC_SOCK_CLOEXEC 0x80000
#define SC_SOCK_NONBLOCK 0x800
#define SC_MSG_PEEK 0x2
#define SC_MSG_TRUNC 0x20
#define SC_MSG_CTRUNC 0x8
#define SC_MSG_DONTWAIT 0x40
#define SC_MSG_WAITALL 0x100
#define SC_MSG_NOSIGNAL 0x4000
#define SC_MSG_CMSG_CLOEXEC 0x40000000
#define SC_CMSG_HDR 16

static uint8_t sc_iobuf[SC_CHUNK];

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
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

int sockcall_handles(uint64_t nr)
{
    return (nr >= 198 && nr <= 212) || nr == 242;
}

/* The socket of fd, or the errno. */
static sock_t *sock_of(int fd, int *err)
{
    vfs_fd *f = vfs_get(fd);
    if (!f) {
        *err = -LE_BADF;
        return NULL;
    }
    if (f->kind != VFS_SOCK) {
        *err = -LE_NOTSOCK;
        return NULL;
    }
    return f->sock;
}

static int nonblock_of(const sock_t *unused, int fd, int flags)
{
    (void)unused;
    vfs_fd *f = vfs_get(fd);
    return (f && (f->lflags & LO_NONBLOCK) != 0) || (flags & SC_MSG_DONTWAIT) != 0;
}

static void drop_fds(void **fds, int n)
{
    for (int i = 0; i < n; i++)
        vfs_desc_drop((vfs_fd *)fds[i]);
}

/* A name from a sockaddr_un. For a path, the canonical guest path and whether it exists. */
typedef struct {
    uint8_t name[SOCK_PATH_MAX + 2]; /* canonical: the lookup key */
    size_t nlen;
    uint8_t disp[SOCK_PATH_MAX + 2]; /* as the guest spelled it */
    size_t dlen;
    int fsnode;
    int exists;
} sname_t;

static int64_t name_parse(uint64_t va, uint64_t alen, sname_t *n)
{
    if (alen < 2 || alen > 110)
        return -LE_INVAL;
    uint8_t sa[110];
    if (mem_copy_out(sa, va, (size_t)alen) < 0)
        return -LE_FAULT;
    if (sa[0] != SC_AF_UNIX || sa[1] != 0)
        return -LE_AFNOSUPPORT;
    size_t plen = (size_t)alen - 2;
    if (plen == 0)
        return -LE_INVAL;
    if (sa[2] == 0) { /* abstract: the bytes, leading NUL included */
        if (plen > SOCK_PATH_MAX)
            return -LE_INVAL;
        memcpy(n->name, sa + 2, plen);
        n->nlen = plen;
        memcpy(n->disp, sa + 2, plen);
        n->dlen = plen;
        n->fsnode = 0;
        n->exists = 0;
        return 0;
    }
    size_t len = 0;
    while (len < plen && sa[2 + len])
        len++;
    if (len == plen)
        return -LE_INVAL; /* not NUL-terminated */
    char path[110];
    memcpy(path, sa + 2, len);
    path[len] = 0;
    vfs_res r;
    int rc = vfs_resolve(LAT_FDCWD, path, 1, &r);
    /* A missing parent directory (the engine's own endpoints live under /dev/socket, which
     * need not exist in the root): the path is its own name, not in the file system. */
    const char *canon = path;
    int exists = 0;
    if (rc == -LE_NOENT) {
        canon = path;
    } else if (rc < 0) {
        return rc;
    } else {
        canon = r.guest;
        exists = r.exists;
    }
    size_t gl = strlen(canon);
    if (gl > SOCK_PATH_MAX - 1)
        return -LE_NAMETOOLONG;
    memcpy(n->name, canon, gl);
    n->nlen = gl;
    memcpy(n->disp, path, len);
    n->dlen = len;
    n->fsnode = 1;
    n->exists = exists || sock_is_fsnode(canon);
    return 0;
}

/* Writes a sockaddr_un (len bytes) to the guest: at most *alen_va's capacity, and the full
 * length is written back to *alen_va. */
static int64_t put_addr(uint64_t addr, uint64_t alen_va, const uint8_t *sa, size_t len)
{
    if (!addr || !alen_va)
        return 0;
    uint8_t b[4];
    if (mem_copy_out(b, alen_va, 4) < 0)
        return -LE_FAULT;
    uint32_t cap = get32(b);
    size_t w = len < cap ? len : cap;
    if (w && mem_copy_in(addr, sa, w) < 0)
        return -LE_FAULT;
    put32(b, (uint32_t)len);
    return mem_copy_in(alen_va, b, 4) < 0 ? -LE_FAULT : 0;
}

/* The sockaddr_un of a received message's sender (or of a socket's own name). */
static size_t addr_of_name(uint8_t *sa, size_t cap, const uint8_t *name, size_t nlen, int fs)
{
    uint8_t tmp[2 + SOCK_PATH_MAX + 1];
    size_t len = 2;
    tmp[0] = SC_AF_UNIX;
    tmp[1] = 0;
    if (nlen) {
        memcpy(tmp + 2, name, nlen);
        len = 2 + nlen;
        if (fs)
            tmp[len++] = 0;
    }
    memcpy(sa, tmp, len < cap ? len : cap);
    return len;
}

/* The total of an iovec array, read from the guest into ent (va, len pairs). */
static int64_t iov_read(uint64_t iov, uint64_t cnt, uint64_t **out, uint64_t *total)
{
    if (cnt > SC_IOV_MAX)
        return -LE_INVAL;
    uint64_t *ent = malloc((cnt ? (size_t)cnt : 1) * 2 * sizeof *ent);
    if (!ent)
        return -LE_NOMEM;
    *total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        uint8_t b[16];
        if (mem_copy_out(b, iov + i * 16, 16) < 0) {
            free(ent);
            return -LE_FAULT;
        }
        ent[2 * i] = get64(b);
        ent[2 * i + 1] = get64(b + 8);
        *total += ent[2 * i + 1];
    }
    *out = ent;
    return 0;
}

/* ---------- send ---------- */

/* Sends the guest segments ent (va, len pairs). The descriptions in fds are taken by a send
 * that succeeds and dropped otherwise. Blocking follows sock.c's rules. */
static int64_t send_ent(int fd, const uint64_t *ent, uint64_t cnt, int flags, void **fds, int nfds, sock_t *to,
                        const sock_cred_t *scm)
{
    vfs_fd *f = vfs_get(fd);
    if (!f || f->kind != VFS_SOCK) {
        drop_fds(fds, nfds);
        return f ? -LE_NOTSOCK : -LE_BADF;
    }
    sock_t *s = f->sock;
    cpu_t *c = sched_cur_cpu();
    int nb = nonblock_of(s, fd, flags);
    int nosig = (flags & SC_MSG_NOSIGNAL) != 0;
    uint64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++)
        total += ent[2 * i + 1];

    if (sock_is_dgram(s)) { /* one message: sent whole or not at all */
        if (total > SC_DGRAM_MAX) {
            drop_fds(fds, nfds);
            return -LE_MSGSIZE;
        }
        uint8_t *msg = malloc(total ? (size_t)total : 1);
        if (!msg) {
            drop_fds(fds, nfds);
            return -LE_NOMEM;
        }
        uint64_t off = 0;
        for (uint64_t i = 0; i < cnt; i++) {
            size_t len = (size_t)ent[2 * i + 1];
            if (len && mem_copy_out(msg + off, ent[2 * i], len) < 0) {
                free(msg);
                drop_fds(fds, nfds);
                return -LE_FAULT;
            }
            off += len;
        }
        int64_t w = sock_send(s, msg, (size_t)total, fds, nfds, to, scm);
        free(msg);
        if (w == -LE_AGAIN) {
            drop_fds(fds, nfds);
            if (sock_block(sock_send_target(s, to), sock_timeo(s, 1), nb))
                return 0;
            return -LE_AGAIN;
        }
        if (w < 0) {
            drop_fds(fds, nfds);
            if (w == -LE_PIPE && !nosig)
                sig_raise_current(SC_SIGPIPE);
        }
        return w;
    }

    /* A stream: as much as fits, in chunks, from the progress of an earlier attempt. */
    uint64_t pos = c->io_done;
    if (pos > total)
        pos = 0;
    do {
        size_t chunk = 0;
        if (pos < total) {
            uint64_t base = 0, va = 0;
            for (uint64_t i = 0; i < cnt; i++) {
                uint64_t len = ent[2 * i + 1];
                if (pos < base + len) {
                    va = ent[2 * i] + (pos - base);
                    uint64_t rest = len - (pos - base);
                    chunk = rest < SC_CHUNK ? (size_t)rest : SC_CHUNK;
                    break;
                }
                base += len;
            }
            if (mem_copy_out(sc_iobuf, va, chunk) < 0) {
                drop_fds(fds, nfds);
                c->io_done = 0;
                return pos ? (int64_t)pos : -LE_FAULT;
            }
        }
        /* Descriptions go with the first send only (pos is 0 there, and not after a restart). */
        void **fa = pos == 0 ? fds : NULL;
        int na = pos == 0 ? nfds : 0;
        int64_t w = sock_send(s, sc_iobuf, chunk, fa, na, to, scm);
        if (w == -LE_AGAIN) {
            drop_fds(fa ? fa : NULL, na);
            if (sock_block(sock_send_target(s, to), sock_timeo(s, 1), nb)) {
                c->io_done = pos;
                return 0;
            }
            c->io_done = 0;
            return pos ? (int64_t)pos : -LE_AGAIN;
        }
        if (w < 0) {
            drop_fds(fa ? fa : NULL, na);
            c->io_done = 0;
            if (w == -LE_PIPE && !nosig && pos == 0)
                sig_raise_current(SC_SIGPIPE);
            return pos ? (int64_t)pos : w;
        }
        pos += (uint64_t)w;
    } while (pos < total);
    c->io_done = 0;
    return (int64_t)pos;
}

/* Reads the SCM_RIGHTS descriptions of a control buffer (each is held), and the SCM_CREDENTIALS: the last one
 * wins, and each is checked as the kernel checks it (sock_scm_check). *have is set when there is one. */
static int64_t parse_rights(uint64_t ctlp, uint64_t ctln, void **fds, int *nfds, sock_cred_t *scm, int *have)
{
    uint64_t off = 0;
    while (off + SC_CMSG_HDR <= ctln) {
        uint8_t h[SC_CMSG_HDR];
        if (mem_copy_out(h, ctlp + off, SC_CMSG_HDR) < 0)
            return -LE_FAULT;
        uint64_t cl = get64(h);
        uint32_t level = get32(h + 8), type = get32(h + 12);
        if (cl < SC_CMSG_HDR || cl > ctln - off)
            return -LE_INVAL;
        if (level == SC_SOL_SOCKET && type == SC_SCM_RIGHTS) {
            size_t nf = (size_t)((cl - SC_CMSG_HDR) / 4);
            if (*nfds + (int)nf > SOCK_MAX_FDS)
                return -LE_INVAL;
            uint8_t fb[4 * SOCK_MAX_FDS];
            if (mem_copy_out(fb, ctlp + off + SC_CMSG_HDR, nf * 4) < 0)
                return -LE_FAULT;
            for (size_t i = 0; i < nf; i++) {
                vfs_fd *d = vfs_desc_hold((int)get32(fb + 4 * i));
                if (!d)
                    return -LE_BADF;
                fds[(*nfds)++] = d;
            }
        } else if (level == SC_SOL_SOCKET && type == SC_SCM_CREDENTIALS) {
            uint8_t cb[12];
            if (cl < SC_CMSG_HDR + sizeof cb || mem_copy_out(cb, ctlp + off + SC_CMSG_HDR, sizeof cb) < 0)
                return cl < SC_CMSG_HDR + sizeof cb ? -LE_INVAL : -LE_FAULT;
            sock_cred_t c = {(int32_t)get32(cb), get32(cb + 4), get32(cb + 8)};
            int rc = sock_scm_check(&c);
            if (rc)
                return rc;
            *scm = c;
            *have = 1;
        }
        off += (cl + 7) & ~(uint64_t)7;
    }
    return 0;
}

/* ---------- receive ---------- */

static int64_t recv_ent(int fd, const uint64_t *ent, uint64_t cnt, int flags, sock_recv_t *r, int *parked)
{
    vfs_fd *f = vfs_get(fd);
    *parked = 0;
    uint64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++)
        total += ent[2 * i + 1];
    if (total > SC_RECV_MAX)
        total = SC_RECV_MAX;
    uint8_t *tmp = malloc(total ? (size_t)total : 1);
    if (!tmp)
        return -LE_NOMEM;
    memset(r, 0, sizeof *r);
    r->buf = tmp;
    r->cap = (size_t)total;
    r->peek = (flags & SC_MSG_PEEK) != 0;
    r->waitall = (flags & SC_MSG_WAITALL) != 0;
    int64_t got = sock_recv(f->sock, r);
    if (got == -LE_AGAIN) {
        free(tmp);
        if (sock_block(f->sock, sock_timeo(f->sock, 0), nonblock_of(f->sock, fd, flags))) {
            *parked = 1;
            return 0;
        }
        return -LE_AGAIN;
    }
    if (got < 0) {
        free(tmp);
        return got;
    }
    uint64_t off = 0;
    for (uint64_t i = 0; i < cnt && off < (uint64_t)got; i++) {
        uint64_t take = ent[2 * i + 1];
        if (take > (uint64_t)got - off)
            take = (uint64_t)got - off;
        if (take && mem_copy_in(ent[2 * i], tmp + off, (size_t)take) < 0) {
            free(tmp);
            drop_fds(r->fds, r->nfds);
            r->nfds = 0;
            return -LE_FAULT;
        }
        off += take;
    }
    free(tmp);
    return got;
}

/* ---------- the syscalls ---------- */

/* socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT) (B10): the uevent socket of vold's NetlinkManager. It is an
 * unbound datagram that never receives, because the engine has no kernel to send uevents. Other netlink families
 * and protocols are EPROTONOSUPPORT. */
static int64_t sc_socket_netlink(uint64_t type, uint64_t proto)
{
    int kind = (int)(type & 0xF);
    if (kind != SOCK_T_DGRAM || proto != SC_NETLINK_KOBJECT_UEVENT)
        return -LE_PROTONOSUPPORT;
    int64_t fd = vfs_socket((type & SC_SOCK_CLOEXEC) != 0, kind, (type & SC_SOCK_NONBLOCK) != 0);
    if (fd < 0)
        return fd;
    int err = 0;
    sock_t *s = sock_of((int)fd, &err);
    if (!s) {
        vfs_close((int)fd);
        return err;
    }
    sock_set_netlink(s, 1);
    return fd;
}

static int64_t sc_socket(uint64_t domain, uint64_t type, uint64_t proto)
{
    if (domain == SC_AF_NETLINK)
        return sc_socket_netlink(type, proto);
    if (domain != SC_AF_UNIX)
        return -LE_AFNOSUPPORT;
    int kind = (int)(type & 0xF);
    if ((kind != SOCK_T_STREAM && kind != SOCK_T_DGRAM && kind != SOCK_T_SEQPACKET) || proto != 0)
        return -LE_PROTONOSUPPORT;
    return vfs_socket((type & SC_SOCK_CLOEXEC) != 0, kind, (type & SC_SOCK_NONBLOCK) != 0);
}

static int64_t sc_socketpair(uint64_t domain, uint64_t type, uint64_t proto, uint64_t sv)
{
    if (domain != SC_AF_UNIX)
        return -LE_AFNOSUPPORT;
    int kind = (int)(type & 0xF);
    if ((kind != SOCK_T_STREAM && kind != SOCK_T_DGRAM && kind != SOCK_T_SEQPACKET) || proto != 0)
        return -LE_PROTONOSUPPORT;
    int cl = (type & SC_SOCK_CLOEXEC) != 0, nb = (type & SC_SOCK_NONBLOCK) != 0;
    sock_t *a, *b;
    int rc = sock_pair(kind, &a, &b);
    if (rc)
        return rc;
    int fa = vfs_socket_adopt(a, cl, nb);
    if (fa < 0) {
        sock_unref(b);
        return fa;
    }
    int fb = vfs_socket_adopt(b, cl, nb);
    if (fb < 0) {
        vfs_close(fa);
        return fb;
    }
    uint8_t out[8];
    put32(out, (uint32_t)fa);
    put32(out + 4, (uint32_t)fb);
    if (mem_copy_in(sv, out, 8) < 0) {
        vfs_close(fa);
        vfs_close(fb);
        return -LE_FAULT;
    }
    return 0;
}

static int64_t sc_bind(uint64_t fd, uint64_t addr, uint64_t alen)
{
    int err = 0;
    sock_t *s = sock_of((int)(int32_t)fd, &err);
    if (!s)
        return err;
    if (sock_netlink(s)) {
        /* struct sockaddr_nl (B10): family AF_NETLINK, a 12-byte address. The pid and the groups are accepted as
         * given; nothing is ever sent to the group. A second bind is accepted, as the kernel accepts a bind with
         * the same pid. */
        uint8_t nl[12];
        if (alen < sizeof nl)
            return -LE_INVAL;
        if (mem_copy_out(nl, addr, sizeof nl) < 0)
            return -LE_FAULT;
        if (nl[0] != SC_AF_NETLINK || nl[1] != 0)
            return -LE_INVAL;
        sock_set_netlink(s, 2);
        return 0;
    }
    if (!sock_can_bind(s))
        return -LE_INVAL;
    sname_t n;
    int64_t rc = name_parse(addr, alen, &n);
    if (rc)
        return rc;
    if (sock_find(n.name, n.nlen, n.fsnode))
        return -LE_ADDRINUSE;
    if (n.fsnode) {
        if (n.exists)
            return -LE_ADDRINUSE;
        char path[SOCK_PATH_MAX];
        memcpy(path, n.name, n.nlen);
        path[n.nlen] = 0;
        /* /dev/socket is served by the engine (not a host directory): its nodes are names
         * only. Elsewhere the node is a placeholder file, which stat reports as a socket. */
        if (strncmp(path, "/dev/socket/", 12) != 0) {
            int nfd = vfs_open(LAT_FDCWD, path, LO_CREAT | LO_EXCL | LO_WRONLY, 0777);
            if (nfd < 0)
                return nfd;
            vfs_close(nfd);
        }
        rc = sock_fsnode_add(path);
        if (rc)
            return rc;
    }
    return sock_bind(s, n.name, n.nlen, n.fsnode, n.disp, n.dlen);
}

static int64_t sc_listen(uint64_t fd, uint64_t backlog)
{
    int err = 0;
    sock_t *s = sock_of((int)(int32_t)fd, &err);
    if (!s)
        return err;
    return sock_listen(s, (int)(int32_t)backlog);
}

static int64_t sc_accept(uint64_t fd, uint64_t addr, uint64_t alen_va, uint64_t flags)
{
    int err = 0;
    sock_t *s = sock_of((int)(int32_t)fd, &err);
    if (!s)
        return err;
    sock_t *c = NULL;
    int64_t rc = sock_accept(s, &c);
    if (rc == -LE_AGAIN) {
        if (sock_block(s, sock_timeo(s, 0), nonblock_of(s, (int)(int32_t)fd, 0)))
            return 0;
        return -LE_AGAIN;
    }
    if (rc)
        return rc;
    int nfd = vfs_socket_adopt(c, (flags & SC_SOCK_CLOEXEC) != 0, (flags & SC_SOCK_NONBLOCK) != 0);
    if (nfd < 0)
        return nfd;
    if (addr) {
        uint8_t sa[110];
        size_t len = sock_addr(c, 1, sa, sizeof sa);
        int64_t w = put_addr(addr, alen_va, sa, len);
        if (w < 0) {
            vfs_close(nfd);
            return w;
        }
    }
    return nfd;
}

static int64_t sc_connect(uint64_t fd, uint64_t addr, uint64_t alen)
{
    int err = 0;
    sock_t *s = sock_of((int)(int32_t)fd, &err);
    if (!s)
        return err;
    sname_t n;
    int64_t rc = name_parse(addr, alen, &n);
    if (rc)
        return rc;
    sock_t *wait = NULL;
    rc = sock_connect(s, n.name, n.nlen, n.fsnode, n.exists, &wait);
    if (rc == -LE_AGAIN) {
        if (sock_block(wait, sock_timeo(s, 1), nonblock_of(s, (int)(int32_t)fd, 0)))
            return 0;
        return -LE_AGAIN;
    }
    return rc;
}

static int64_t sc_getname(uint64_t fd, uint64_t addr, uint64_t alen_va, int peer)
{
    int err = 0;
    sock_t *s = sock_of((int)(int32_t)fd, &err);
    if (!s)
        return err;
    if (peer && !sock_connected(s))
        return -LE_NOTCONN;
    uint8_t sa[110];
    size_t len = sock_addr(s, peer, sa, sizeof sa);
    return put_addr(addr, alen_va, sa, len);
}

static int64_t sc_sendto(uint64_t fd, uint64_t buf, uint64_t n, uint64_t flags, uint64_t addr, uint64_t alen)
{
    int err = 0;
    sock_t *s = sock_of((int)(int32_t)fd, &err);
    if (!s)
        return err;
    sock_t *to = NULL;
    if (addr && alen) {
        if (sock_type(s) != SOCK_T_DGRAM)
            return -LE_ISCONN;
        sname_t nm;
        int64_t rc = name_parse(addr, alen, &nm);
        if (rc)
            return rc;
        to = sock_find(nm.name, nm.nlen, nm.fsnode);
        if (!to)
            return nm.fsnode && !nm.exists ? -LE_NOENT : -LE_CONNREFUSED;
    }
    uint64_t ent[2] = {buf, n};
    return send_ent((int)(int32_t)fd, ent, 1, (int)flags, NULL, 0, to, NULL);
}

static int64_t sc_recvfrom(uint64_t fd, uint64_t buf, uint64_t n, uint64_t flags, uint64_t addr, uint64_t alen)
{
    int err = 0;
    sock_t *s = sock_of((int)(int32_t)fd, &err);
    if (!s)
        return err;
    sock_recv_t *r = malloc(sizeof *r);
    if (!r)
        return -LE_NOMEM;
    uint64_t ent[2] = {buf, n};
    int parked = 0;
    int64_t got = recv_ent((int)(int32_t)fd, ent, 1, (int)flags, r, &parked);
    if (parked) {
        free(r);
        return 0;
    }
    if (got < 0) {
        free(r);
        return got;
    }
    drop_fds(r->fds, r->nfds); /* recvfrom has no control buffer: the descriptions are dropped */
    if (addr) {
        uint8_t sa[110];
        size_t len = addr_of_name(sa, sizeof sa, r->from, r->fromlen, r->fromfs);
        int64_t w = put_addr(addr, alen, sa, len);
        if (w < 0) {
            free(r);
            return w;
        }
    }
    int64_t res = (flags & SC_MSG_TRUNC) && sock_is_dgram(s) ? (int64_t)r->msglen : got;
    free(r);
    return res;
}

static int64_t sc_setsockopt(uint64_t fd, uint64_t level, uint64_t opt, uint64_t val, uint64_t len)
{
    int err = 0;
    sock_t *s = sock_of((int)(int32_t)fd, &err);
    if (!s)
        return err;
    if (len > 256)
        return -LE_INVAL;
    uint8_t b[256];
    if (len && mem_copy_out(b, val, (size_t)len) < 0)
        return -LE_FAULT;
    return sock_setopt(s, (int)(int32_t)level, (int)(int32_t)opt, b, (size_t)len);
}

static int64_t sc_getsockopt(uint64_t fd, uint64_t level, uint64_t opt, uint64_t val, uint64_t optlen_va)
{
    int err = 0;
    sock_t *s = sock_of((int)(int32_t)fd, &err);
    if (!s)
        return err;
    uint8_t lb[4];
    if (mem_copy_out(lb, optlen_va, 4) < 0)
        return -LE_FAULT;
    size_t cap = get32(lb);
    uint8_t b[64];
    size_t n = cap < sizeof b ? cap : sizeof b;
    int64_t rc = sock_getopt(s, (int)(int32_t)level, (int)(int32_t)opt, b, &n);
    if (rc)
        return rc;
    size_t w = n < cap ? n : cap;
    if (w && mem_copy_in(val, b, w) < 0)
        return -LE_FAULT;
    put32(lb, (uint32_t)n);
    return mem_copy_in(optlen_va, lb, 4) < 0 ? -LE_FAULT : 0;
}

static int64_t sc_shutdown(uint64_t fd, uint64_t how)
{
    int err = 0;
    sock_t *s = sock_of((int)(int32_t)fd, &err);
    if (!s)
        return err;
    return sock_shutdown(s, (int)(int32_t)how);
}

/* struct msghdr on arm64: name (0), namelen (8, u32), iov (16), iovlen (24), control (32),
 * controllen (40), flags (48). */
#define MH_SIZE 56

static int64_t sc_sendmsg(uint64_t fd, uint64_t msg, uint64_t flags)
{
    uint8_t mh[MH_SIZE];
    if (mem_copy_out(mh, msg, MH_SIZE) < 0)
        return -LE_FAULT;
    uint64_t namep = get64(mh), namelen = get32(mh + 8);
    uint64_t iovp = get64(mh + 16), iovn = get64(mh + 24);
    uint64_t ctlp = get64(mh + 32), ctln = get64(mh + 40);
    int err = 0;
    sock_t *s = sock_of((int)(int32_t)fd, &err);
    if (!s)
        return err;
    uint64_t *ent = NULL, total = 0;
    int64_t rc = iov_read(iovp, iovn, &ent, &total);
    if (rc)
        return rc;
    sock_t *to = NULL;
    if (namep && namelen) {
        sname_t nm;
        rc = name_parse(namep, namelen, &nm);
        if (rc) {
            free(ent);
            return rc;
        }
        if (sock_type(s) != SOCK_T_DGRAM) {
            free(ent);
            return -LE_ISCONN;
        }
        to = sock_find(nm.name, nm.nlen, nm.fsnode);
        if (!to) {
            free(ent);
            return nm.fsnode && !nm.exists ? -LE_NOENT : -LE_CONNREFUSED;
        }
    }
    void *fds[SOCK_MAX_FDS];
    int nfds = 0;
    sock_cred_t scm = {0, 0, 0};
    int have_scm = 0;
    /* A restart after progress (io_done) already sent the descriptions. */
    if (sched_cur_cpu()->io_done == 0 && ctln && ctlp) {
        rc = parse_rights(ctlp, ctln, fds, &nfds, &scm, &have_scm);
        if (rc) {
            drop_fds(fds, nfds);
            free(ent);
            return rc;
        }
    }
    int64_t res = send_ent((int)(int32_t)fd, ent, iovn, (int)flags, fds, nfds, to, have_scm ? &scm : NULL);
    free(ent);
    return res;
}

static int64_t sc_recvmsg(uint64_t fd, uint64_t msg, uint64_t flags)
{
    uint8_t mh[MH_SIZE];
    if (mem_copy_out(mh, msg, MH_SIZE) < 0)
        return -LE_FAULT;
    uint64_t namep = get64(mh), iovp = get64(mh + 16), iovn = get64(mh + 24);
    uint64_t ctlp = get64(mh + 32), ctln = get64(mh + 40);
    int err = 0;
    sock_t *s = sock_of((int)(int32_t)fd, &err);
    if (!s)
        return err;
    uint64_t *ent = NULL, total = 0;
    int64_t rc = iov_read(iovp, iovn, &ent, &total);
    if (rc)
        return rc;
    sock_recv_t *r = malloc(sizeof *r);
    if (!r) {
        free(ent);
        return -LE_NOMEM;
    }
    int parked = 0;
    int64_t got = recv_ent((int)(int32_t)fd, ent, iovn, (int)flags, r, &parked);
    free(ent);
    if (parked) {
        free(r);
        return 0;
    }
    if (got < 0) {
        free(r);
        return got;
    }
    /* The sender's name, through msg_namelen. */
    if (namep) {
        uint8_t sa[110];
        size_t len = addr_of_name(sa, sizeof sa, r->from, r->fromlen, r->fromfs);
        rc = put_addr(namep, msg + 8, sa, len);
        if (rc) {
            drop_fds(r->fds, r->nfds);
            free(r);
            return rc;
        }
    }
    /* Control: the credentials (when SO_PASSCRED is set), then the descriptions. */
    size_t cl = ctln < SC_CTL_MAX ? (size_t)ctln : SC_CTL_MAX;
    uint8_t *cb = malloc(cl ? cl : 1);
    if (!cb) {
        drop_fds(r->fds, r->nfds);
        free(cb);
        free(r);
        return -LE_NOMEM;
    }
    size_t pos = 0;
    uint32_t mflags = 0;
    if (sock_passcred(s)) {
        if (pos + SC_CMSG_HDR + 12 <= cl) {
            put64(cb + pos, SC_CMSG_HDR + 12);
            put32(cb + pos + 8, SC_SOL_SOCKET);
            put32(cb + pos + 12, SC_SCM_CREDENTIALS);
            put32(cb + pos + 16, (uint32_t)r->cred.pid);
            put32(cb + pos + 20, r->cred.uid);
            put32(cb + pos + 24, r->cred.gid);
            pos += 32;
        } else {
            mflags |= SC_MSG_CTRUNC;
        }
    }
    if (r->nfds) {
        int cloexec = (flags & SC_MSG_CMSG_CLOEXEC) != 0;
        int fit = 0;
        if (cl >= pos + SC_CMSG_HDR)
            fit = (int)((cl - pos - SC_CMSG_HDR) / 4);
        int installed = 0;
        uint8_t nums[4 * SOCK_MAX_FDS];
        for (int i = 0; i < r->nfds; i++) {
            if (i < fit) {
                int fdn = vfs_desc_install((vfs_fd *)r->fds[i], cloexec);
                if (fdn >= 0) {
                    put32(nums + 4 * installed, (uint32_t)fdn);
                    installed++;
                }
            } else {
                vfs_desc_drop((vfs_fd *)r->fds[i]);
                mflags |= SC_MSG_CTRUNC;
            }
        }
        if (installed) {
            size_t clen = SC_CMSG_HDR + 4 * (size_t)installed;
            put64(cb + pos, clen);
            put32(cb + pos + 8, SC_SOL_SOCKET);
            put32(cb + pos + 12, SC_SCM_RIGHTS);
            memcpy(cb + pos + SC_CMSG_HDR, nums, 4 * (size_t)installed);
            pos += (clen + 7) & ~(size_t)7;
        }
    }
    if (pos && mem_copy_in(ctlp, cb, pos) < 0) {
        free(cb);
        free(r);
        return -LE_FAULT;
    }
    uint8_t cw[8];
    put64(cw, pos);
    if (mem_copy_in(msg + 40, cw, 8) < 0) {
        free(cb);
        free(r);
        return -LE_FAULT;
    }
    if (r->trunc)
        mflags |= SC_MSG_TRUNC;
    uint8_t fw[4];
    put32(fw, mflags);
    if (mem_copy_in(msg + 48, fw, 4) < 0) {
        free(cb);
        free(r);
        return -LE_FAULT;
    }
    int64_t res = (flags & SC_MSG_TRUNC) && sock_is_dgram(s) ? (int64_t)r->msglen : got;
    free(cb);
    free(r);
    return res;
}

int64_t sockcall_run(uint64_t nr, const uint64_t *a)
{
    switch (nr) {
    case 198: return sc_socket(a[0], a[1], a[2]);
    case 199: return sc_socketpair(a[0], a[1], a[2], a[3]);
    case 200: return sc_bind(a[0], a[1], a[2]);
    case 201: return sc_listen(a[0], a[1]);
    case 202: return sc_accept(a[0], a[1], a[2], 0);
    case 203: return sc_connect(a[0], a[1], a[2]);
    case 204: return sc_getname(a[0], a[1], a[2], 0);
    case 205: return sc_getname(a[0], a[1], a[2], 1);
    case 206: return sc_sendto(a[0], a[1], a[2], a[3], a[4], a[5]);
    case 207: return sc_recvfrom(a[0], a[1], a[2], a[3], a[4], a[5]);
    case 208: return sc_setsockopt(a[0], a[1], a[2], a[3], a[4]);
    case 209: return sc_getsockopt(a[0], a[1], a[2], a[3], a[4]);
    case 210: return sc_shutdown(a[0], a[1]);
    case 211: return sc_sendmsg(a[0], a[1], a[2]);
    case 212: return sc_recvmsg(a[0], a[1], a[2]);
    case 242: return sc_accept(a[0], a[1], a[2], a[3]);
    default: return -LE_NOSYS;
    }
}

/* ---------- the plain data paths (read, write and their vector forms) ---------- */

static int64_t read_ent(int fd, const uint64_t *ent, uint64_t cnt)
{
    sock_recv_t *r = malloc(sizeof *r);
    if (!r)
        return -LE_NOMEM;
    int parked = 0;
    int64_t got = recv_ent(fd, ent, cnt, 0, r, &parked);
    if (!parked && got >= 0)
        drop_fds(r->fds, r->nfds); /* read has no control buffer */
    free(r);
    return parked ? 0 : got;
}

int64_t sockcall_read(int fd, uint64_t buf, uint64_t n)
{
    uint64_t ent[2] = {buf, n};
    return read_ent(fd, ent, 1);
}

int64_t sockcall_readv(int fd, uint64_t iov, uint64_t cnt)
{
    uint64_t *ent = NULL, total = 0;
    int64_t rc = iov_read(iov, cnt, &ent, &total);
    if (rc)
        return rc;
    int64_t got = read_ent(fd, ent, cnt);
    free(ent);
    return got;
}

int64_t sockcall_write(int fd, uint64_t buf, uint64_t n)
{
    uint64_t ent[2] = {buf, n};
    return send_ent(fd, ent, 1, 0, NULL, 0, NULL, NULL);
}

int64_t sockcall_writev(int fd, uint64_t iov, uint64_t cnt)
{
    uint64_t *ent = NULL, total = 0;
    int64_t rc = iov_read(iov, cnt, &ent, &total);
    if (rc)
        return rc;
    int64_t sent = send_ent(fd, ent, cnt, 0, NULL, 0, NULL, NULL);
    free(ent);
    return sent;
}
