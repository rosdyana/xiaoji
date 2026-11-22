/* AF_UNIX sockets (B4a step 1): one program, output compared with native. It covers
 * socketpair (stream, seqpacket, datagram), bind to a path (S_IFSOCK in stat) and to the
 * abstract namespace, listen, accept (blocking and nonblocking), connect (ECONNREFUSED,
 * ENOENT, EAGAIN on a full backlog), a fork with a stream server and client, SCM_RIGHTS
 * (a pipe's write end passed to the child), SO_PEERCRED, SO_PASSCRED, SO_TYPE, SO_RCVBUF,
 * SO_RCVTIMEO, MSG_PEEK, MSG_TRUNC on a seqpacket, shutdown and EPIPE, end of file on peer
 * close, and ppoll readiness. Nothing printed depends on a pid, fd number or inode. */
#include "sys.h"

#define SYS_CLOSE 57
#define SYS_PIPE2 59
#define SYS_READ 63
#define SYS_NEWFSTATAT 79
#define SYS_PPOLL 73
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SYS_UNLINKAT 35
#define SYS_NANOSLEEP 101
#define SYS_SOCKET 198
#define SYS_SOCKETPAIR 199
#define SYS_BIND 200
#define SYS_LISTEN 201
#define SYS_ACCEPT 202
#define SYS_CONNECT 203
#define SYS_GETSOCKNAME 204
#define SYS_GETPEERNAME 205
#define SYS_SENDTO 206
#define SYS_RECVFROM 207
#define SYS_SETSOCKOPT 208
#define SYS_GETSOCKOPT 209
#define SYS_SHUTDOWN 210
#define SYS_SENDMSG 211
#define SYS_RECVMSG 212
#define SYS_ACCEPT4 242
#define SYS_GETPID 172

typedef int16_t i16;

#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC 02000000
#define SOL_SOCKET 1
#define SO_RCVBUF 8
#define SO_PASSCRED 16
#define SO_PEERCRED 17
#define SO_RCVTIMEO 20
#define SO_TYPE 3
#define SO_ACCEPTCONN 30
#define MSG_PEEK 0x2
#define MSG_TRUNC 0x20
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
#define SCM_RIGHTS 1
#define SCM_CREDENTIALS 2
#define SHUT_RD 0
#define SHUT_WR 1
#define S_IFMT 0170000
#define S_IFSOCK 0140000
#define POLLIN 1
#define POLLOUT 4
#define O_NONBLOCK 04000
#define O_CLOEXEC 02000000
#define F_GETFL 3
#define F_SETFL 4

#define EAGAIN 11
#define EPIPE 32
#define ECONNREFUSED 111
#define ENOENT 2
#define ENOTCONN 107

struct iovec {
    void *base;
    u64 len;
};

struct msghdr {
    void *msg_name;
    u32 msg_namelen;
    u32 pad0;
    struct iovec *msg_iov;
    u64 msg_iovlen;
    void *msg_control;
    u64 msg_controllen;
    i32 msg_flags;
    i32 pad1;
};

struct pollfd {
    i32 fd;
    i16 events;
    i16 revents;
};

static i64 r_socket(i64 d, i64 t, i64 p) { return sc6(SYS_SOCKET, d, t, p, 0, 0, 0); }
static i64 r_socketpair(i64 d, i64 t, i64 p, int *sv) { return sc6(SYS_SOCKETPAIR, d, t, p, (i64)sv, 0, 0); }
static i64 r_bind(int fd, const void *sa, u32 len) { return sc6(SYS_BIND, fd, (i64)sa, len, 0, 0, 0); }
static i64 r_listen(int fd, int n) { return sc6(SYS_LISTEN, fd, n, 0, 0, 0, 0); }
static i64 r_accept4(int fd, void *sa, u32 *len, int flags) { return sc6(SYS_ACCEPT4, fd, (i64)sa, (i64)len, flags, 0, 0); }
static i64 r_connect(int fd, const void *sa, u32 len) { return sc6(SYS_CONNECT, fd, (i64)sa, len, 0, 0, 0); }
static i64 r_getsockname(int fd, void *sa, u32 *len) { return sc6(SYS_GETSOCKNAME, fd, (i64)sa, (i64)len, 0, 0, 0); }
static i64 r_getpeername(int fd, void *sa, u32 *len) { return sc6(SYS_GETPEERNAME, fd, (i64)sa, (i64)len, 0, 0, 0); }
static i64 r_sendto(int fd, const void *b, u64 n, int flags) { return sc6(SYS_SENDTO, fd, (i64)b, (i64)n, flags, 0, 0); }
static i64 r_recvfrom(int fd, void *b, u64 n, int flags) { return sc6(SYS_RECVFROM, fd, (i64)b, (i64)n, flags, 0, 0); }
static i64 r_setsockopt(int fd, int lvl, int opt, const void *v, u32 len) { return sc6(SYS_SETSOCKOPT, fd, lvl, opt, (i64)v, len, 0); }
static i64 r_getsockopt(int fd, int lvl, int opt, void *v, u32 *len) { return sc6(SYS_GETSOCKOPT, fd, lvl, opt, (i64)v, (i64)len, 0); }
static i64 r_shutdown(int fd, int how) { return sc6(SYS_SHUTDOWN, fd, how, 0, 0, 0, 0); }
static i64 r_sendmsg(int fd, struct msghdr *m, int flags) { return sc6(SYS_SENDMSG, fd, (i64)m, flags, 0, 0, 0); }
static i64 r_recvmsg(int fd, struct msghdr *m, int flags) { return sc6(SYS_RECVMSG, fd, (i64)m, flags, 0, 0, 0); }
static i64 r_close(int fd) { return sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0); }
static i64 r_read(int fd, void *b, u64 n) { return sc6(SYS_READ, fd, (i64)b, (i64)n, 0, 0, 0); }
static i64 r_write(int fd, const void *b, u64 n) { return sc6(64, fd, (i64)b, (i64)n, 0, 0, 0); }

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int memeq(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static void line(const char *name, int ok)
{
    put(name);
    put(ok ? ": ok\n" : ": FAIL\n");
}

/* A sockaddr_un for a path (NUL-terminated) or an abstract name (len bytes, NUL first).
 * Returns the length to pass to bind, connect and getsockname. */
static u32 mkaddr(u8 *sa, const char *path, size_t abs_len)
{
    for (int i = 0; i < 110; i++)
        sa[i] = 0;
    sa[0] = AF_UNIX;
    sa[1] = 0;
    if (abs_len) {
        for (size_t i = 0; i < abs_len; i++)
            sa[2 + i] = (u8)path[i];
        return (u32)(2 + abs_len);
    }
    size_t n = strlen(path);
    for (size_t i = 0; i <= n; i++)
        sa[2 + i] = (u8)path[i];
    return (u32)(2 + n + 1);
}

/* A message's cmsg with one descriptor (SCM_RIGHTS): 16 header bytes and the fd. */
static void put_rights(u8 *ctl, int fd)
{
    for (int i = 0; i < 32; i++)
        ctl[i] = 0;
    ctl[0] = 20; /* cmsg_len = 16 + 4 */
    ctl[8] = SOL_SOCKET;
    ctl[12] = SCM_RIGHTS;
    ctl[16] = (u8)fd;
    ctl[17] = (u8)(fd >> 8);
    ctl[18] = (u8)(fd >> 16);
    ctl[19] = (u8)(fd >> 24);
}

static int get_rights(const u8 *ctl, u64 ctllen)
{
    if (ctllen < 20 || ctl[8] != SOL_SOCKET || ctl[12] != SCM_RIGHTS)
        return -1;
    return (int)((u32)ctl[16] | (u32)ctl[17] << 8 | (u32)ctl[18] << 16 | (u32)ctl[19] << 24);
}

static u32 get_u32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static int ppoll_one(int fd, i16 events, i16 *revents)
{
    struct pollfd p = {fd, events, 0};
    struct {
        i64 sec, nsec;
    } zero = {0, 0};
    i64 r = sc6(SYS_PPOLL, (i64)&p, 1, (i64)&zero, 0, 8, 0);
    *revents = p.revents;
    return (int)r;
}

#define SOCK_PATH2 "/tmp/xiaoji-unixsock-seq"
#define SOCK_PATH3 "/tmp/xiaoji-unixsock-poll"
#define BULK 1048576
#define EPOLLIN 0x1
#define EPOLLHUP 0x10
#define SYS_EPOLL_CREATE1 20
#define SYS_EPOLL_CTL 21
#define SYS_EPOLL_PWAIT 22
#define SYS_NANOSLEEP_NR 101

static u8 blob[BULK];
static u8 rb[65536];

static u8 pat(u64 i)
{
    return (u8)((i * 7) ^ (i >> 8));
}

static void nap_ms(i64 ms)
{
    i64 ts[2] = {ms / 1000, (ms % 1000) * 1000000};
    sc6(SYS_NANOSLEEP_NR, (i64)ts, 0, 0, 0, 0, 0);
}

/* ppoll with no timeout: blocks until the fd is ready. */
static int ppoll_block(int fd, i16 events, i16 *revents)
{
    struct pollfd p = {fd, events, 0};
    i64 r = sc6(SYS_PPOLL, (i64)&p, 1, 0, 0, 8, 0);
    *revents = p.revents;
    return (int)r;
}

/* An epoll_event (16 bytes on arm64): events, then the data word (the fd). */
static i64 epoll_add(int ep, int fd)
{
    u8 e[16];
    for (int i = 0; i < 16; i++)
        e[i] = 0;
    e[0] = EPOLLIN;
    e[8] = (u8)fd;
    return sc6(SYS_EPOLL_CTL, ep, 1, fd, (i64)e, 0, 0);
}

static int epoll_wait_block(int ep, u8 *evs, int max)
{
    return (int)sc6(SYS_EPOLL_PWAIT, ep, (i64)evs, max, -1, 0, 8);
}

static int wait_status(int pid)
{
    int status = 0;
    sc6(SYS_WAIT4, pid, (i64)&status, 0, 0, 0, 0);
    return (status >> 8) & 0xff;
}

/* Children of the fork tests. Each returns an exit status. */
static int poll_child(const u8 *sa, u32 salen)
{
    int cl = (int)r_socket(AF_UNIX, SOCK_STREAM, 0);
    if (r_connect(cl, sa, salen) != 0)
        return 1;
    nap_ms(30);
    if (r_write(cl, "poke", 4) != 4)
        return 1;
    nap_ms(30);
    r_close(cl);
    return 0;
}

static int bulk_child(int w)
{
    for (u64 i = 0; i < BULK; i++)
        blob[i] = pat(i);
    return r_write(w, blob, BULK) == BULK ? 0 : 1;
}

static int seq_child(const u8 *sa, u32 salen)
{
    int cl = (int)r_socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (r_connect(cl, sa, salen) != 0)
        return 1;
    if (r_sendto(cl, "sp-hello", 8, 0) != 8)
        return 1;
    char rep[16];
    int ok = r_recvfrom(cl, rep, 16, 0) == 5 && memeq(rep, "reply", 5);
    r_close(cl);
    return ok ? 0 : 1;
}

#define SOCK_PATH "/tmp/xiaoji-unixsock-test"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    u8 sa[110];
    u32 salen;
    int sv[2];
    char buf[256];

    /* socketpair, stream: bytes both ways. */
    line("pair stream", r_socketpair(1, SOCK_STREAM, 0, sv) == 0 && r_write(sv[0], "ping", 4) == 4 &&
                            r_read(sv[1], buf, 4) == 4 && memeq(buf, "ping", 4) &&
                            r_write(sv[1], "pong", 4) == 4 && r_read(sv[0], buf, 4) == 4 &&
                            memeq(buf, "pong", 4));

    /* seqpacket: message boundaries, and a short buffer truncates the message. */
    r_socketpair(1, SOCK_SEQPACKET, 0, sv);
    r_write(sv[0], "abcde", 5);
    r_write(sv[0], "xy", 2);
    i64 m1 = r_recvfrom(sv[1], buf, 64, 0);
    i64 m2 = r_recvfrom(sv[1], buf, 64, 0);
    line("seqpacket boundaries", m1 == 5 && m2 == 2 && memeq(buf, "xy", 2));
    r_write(sv[0], "0123456789", 10);
    i64 short_r = r_recvfrom(sv[1], buf, 3, 0);
    r_write(sv[0], "0123456789", 10);
    i64 full_r = r_recvfrom(sv[1], buf, 3, MSG_TRUNC);
    line("seqpacket truncation", short_r == 3 && full_r == 10);

    /* MSG_PEEK leaves the bytes queued. */
    r_write(sv[0], "peek", 4);
    i64 pk = r_recvfrom(sv[1], buf, 4, MSG_PEEK);
    i64 pk2 = r_recvfrom(sv[1], buf, 4, 0);
    line("msg peek", pk == 4 && pk2 == 4 && memeq(buf, "peek", 4));
    r_close(sv[0]);
    r_close(sv[1]);

    /* datagram pair: each send is one message. */
    r_socketpair(1, SOCK_DGRAM, 0, sv);
    r_write(sv[0], "d1", 2);
    r_write(sv[0], "d22", 3);
    i64 d1 = r_recvfrom(sv[1], buf, 64, 0);
    i64 d2 = r_recvfrom(sv[1], buf, 64, 0);
    line("dgram boundaries", d1 == 2 && d2 == 3);
    r_close(sv[0]);
    r_close(sv[1]);

    /* EOF on peer close. */
    r_socketpair(1, SOCK_STREAM, 0, sv);
    r_close(sv[0]);
    i64 eof = r_read(sv[1], buf, 16);
    line("eof on peer close", eof == 0);
    r_close(sv[1]);

    /* shutdown(SHUT_WR): the peer reads end of file, and a send fails with EPIPE. */
    r_socketpair(1, SOCK_STREAM, 0, sv);
    r_shutdown(sv[0], SHUT_WR);
    i64 sd_eof = r_read(sv[1], buf, 16);
    i64 sd_pipe = r_sendto(sv[0], "x", 1, MSG_NOSIGNAL);
    line("shutdown wr", sd_eof == 0 && sd_pipe == -EPIPE);
    r_close(sv[0]);
    r_close(sv[1]);

    /* SO_TYPE, SO_RCVBUF (set, then at least the request), SO_RCVTIMEO. */
    r_socketpair(1, SOCK_STREAM, 0, sv);
    u8 optv[16];
    u32 optl = 4;
    r_getsockopt(sv[0], SOL_SOCKET, SO_TYPE, optv, &optl);
    int typ_ok = get_u32(optv) == SOCK_STREAM;
    u32 want = 65536;
    u8 wv[4] = {0, 0, 1, 0};
    r_setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, wv, 4);
    optl = 4;
    r_getsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, optv, &optl);
    line("sockopt type and rcvbuf", typ_ok && get_u32(optv) >= want);
    u8 tv[16];
    for (int i = 0; i < 16; i++)
        tv[i] = 0;
    /* timeval: 0 seconds, 50000 microseconds (at offset 8) */
    tv[8] = (u8)(50000 & 0xff);
    tv[9] = (u8)((50000 >> 8) & 0xff);
    tv[10] = (u8)((50000 >> 16) & 0xff);
    r_setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, tv, 16);
    i64 tmo = r_recvfrom(sv[0], buf, 8, 0);
    line("rcvtimeo expires", tmo == -EAGAIN);
    r_close(sv[0]);
    r_close(sv[1]);

    /* ppoll readiness: a fresh pair is writable, not readable; after a write it is readable. */
    r_socketpair(1, SOCK_STREAM, 0, sv);
    i16 rev_a = 0, rev_b = 0;
    ppoll_one(sv[0], POLLIN | POLLOUT, &rev_a);
    ppoll_one(sv[1], POLLIN, &rev_b);
    int before = (rev_a & POLLOUT) && !(rev_b & POLLIN);
    r_write(sv[0], "p", 1);
    ppoll_one(sv[1], POLLIN, &rev_b);
    line("ppoll readiness", before && (rev_b & POLLIN));
    r_close(sv[0]);
    r_close(sv[1]);

    /* Bound stream server at a path: stat shows a socket, getsockname returns the path,
     * and a nonblocking accept has nothing to take (EAGAIN). */
    sc6(SYS_UNLINKAT, -100, (i64)SOCK_PATH, 0, 0, 0, 0);
    int srv = (int)r_socket(AF_UNIX, SOCK_STREAM, 0);
    salen = mkaddr(sa, SOCK_PATH, 0);
    line("bind path", r_bind(srv, sa, salen) == 0);
    r_listen(srv, 1);
    u8 st[160];
    for (int i = 0; i < 160; i++)
        st[i] = 0;
    i64 sr = sc6(SYS_NEWFSTATAT, -100, (i64)SOCK_PATH, (i64)st, 0, 0, 0);
    u32 mode = get_u32(st + 16);
    line("stat is socket", sr == 0 && (mode & S_IFMT) == S_IFSOCK);
    u32 nl = 110;
    r_getsockname(srv, sa, &nl);
    line("getsockname path", nl == salen && streq((char *)sa + 2, SOCK_PATH));
    sc6(25, srv, F_SETFL, O_NONBLOCK, 0, 0, 0); /* the listener itself is nonblocking */
    i64 nb = r_accept4(srv, 0, 0, 0);
    line("nonblocking accept EAGAIN", nb == -EAGAIN);
    int lt = 0;
    optl = 4;
    r_getsockopt(srv, SOL_SOCKET, SO_ACCEPTCONN, optv, &optl);
    lt = get_u32(optv) == 1;
    line("so_acceptconn", lt);

    /* Connect with a full backlog: listen(1) takes two pending connections, the third
     * nonblocking connect is EAGAIN. */
    int c1 = (int)r_socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int c2 = (int)r_socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int c3 = (int)r_socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    i64 k1 = r_connect(c1, sa, salen);
    i64 k2 = r_connect(c2, sa, salen);
    i64 k3 = r_connect(c3, sa, salen);
    line("connect backlog", k1 == 0 && k2 == 0 && k3 == -EAGAIN);
    int a1 = (int)r_accept4(srv, 0, 0, 0);
    int a2 = (int)r_accept4(srv, 0, 0, 0);
    line("accept queued", a1 >= 0 && a2 >= 0);
    r_close(c1);
    r_close(c2);
    r_close(c3);
    r_close(a1);
    r_close(a2);

    /* A closed listener leaves its socket node: connect is refused (ECONNREFUSED). */
    r_close(srv);
    int dead = (int)r_socket(AF_UNIX, SOCK_STREAM, 0);
    i64 refused = r_connect(dead, sa, salen);
    line("connect refused", refused == -ECONNREFUSED);
    r_close(dead);
    sc6(SYS_UNLINKAT, -100, (i64)SOCK_PATH, 0, 0, 0, 0);

    /* A missing path is ENOENT. */
    int miss = (int)r_socket(AF_UNIX, SOCK_STREAM, 0);
    salen = mkaddr(sa, "/tmp/xiaoji-unixsock-missing", 0);
    line("connect missing", r_connect(miss, sa, salen) == -ENOENT);
    r_close(miss);

    /* Abstract namespace: the name is the bytes after a NUL, and it is not a file. */
    const char abs[] = "\0xiaoji-abstract"; /* leading NUL, then the name */
    int as = (int)r_socket(AF_UNIX, SOCK_STREAM, 0);
    salen = mkaddr(sa, abs, sizeof abs - 1);
    int abs_ok = r_bind(as, sa, salen) == 0 && r_listen(as, 2) == 0;
    int ac = (int)r_socket(AF_UNIX, SOCK_STREAM, 0);
    int abs_conn = r_connect(ac, sa, salen) == 0;
    int ad = (int)r_accept4(as, 0, 0, 0);
    line("abstract connect", abs_ok && abs_conn && ad >= 0);
    if (ad >= 0 && r_write(ac, "abs", 3) == 3 && r_read(ad, buf, 3) == 3)
        line("abstract data", memeq(buf, "abs", 3));
    else
        line("abstract data", 0);
    r_close(ac);
    r_close(ad);
    r_close(as);

    /* Fork: a stream server in the parent, a client in the child. The child checks
     * SCM_RIGHTS: it receives the write end of a pipe the parent made, and writes into it. */
    int lsn = (int)r_socket(AF_UNIX, SOCK_STREAM, 0);
    salen = mkaddr(sa, SOCK_PATH, 0);
    r_bind(lsn, sa, salen);
    r_listen(lsn, 4);
    int pfd[2];
    sc6(SYS_PIPE2, (i64)pfd, 0, 0, 0, 0, 0);
    int cs[2];
    r_socketpair(1, SOCK_STREAM, 0, cs); /* child's channel for the fd: cs[0] parent, cs[1] child */
    i64 pid = sc6(SYS_CLONE, 17, 0, 0, 0, 0, 0);
    if (pid == 0) {
        int cl = (int)r_socket(AF_UNIX, SOCK_STREAM, 0);
        int c_ok = r_connect(cl, sa, salen) == 0;
        char req[8];
        int ok = c_ok && r_write(cl, "hello", 5) == 5 && r_read(cl, req, 5) == 5 && memeq(req, "world", 5);
        /* SCM_RIGHTS in: the parent's pipe write end arrives on cs[1]. */
        u8 cb[32];
        u8 data[4];
        struct iovec iv = {data, 4};
        struct msghdr mh = {.msg_iov = &iv, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = 32};
        i64 rr = r_recvmsg(cs[1], &mh, 0);
        int rfd = get_rights(cb, mh.msg_controllen);
        if (rr >= 0 && rfd >= 0) {
            r_write(rfd, "via-fd", 6);
            r_close(rfd);
        } else {
            ok = 0;
        }
        r_close(cl);
        sc6(93, ok ? 0 : 1, 0, 0, 0, 0, 0);
    }
    int sfd = (int)r_accept4(lsn, 0, 0, 0);
    /* SO_PEERCRED of the accepted socket is the connecting child. */
    u8 cred[12];
    u32 credl = 12;
    r_getsockopt(sfd, SOL_SOCKET, SO_PEERCRED, cred, &credl);
    line("peercred is the child", (i64)get_u32(cred) == pid);
    u8 req2[8];
    int got_hello = r_read(sfd, req2, 5) == 5 && memeq(req2, "hello", 5);
    r_write(sfd, "world", 5);
    line("stream fork", got_hello);
    /* Pass the pipe's write end to the child over the socket pair. */
    u8 cbuf[32];
    put_rights(cbuf, pfd[1]);
    u8 d1b[1] = {'x'};
    struct iovec piv = {d1b, 1};
    struct msghdr pmh = {.msg_iov = &piv, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = 24};
    i64 sm = r_sendmsg(cs[0], &pmh, 0);
    line("sendmsg rights", sm == 1);
    /* The child closes its write end: the parent reads the child's bytes from the pipe. */
    r_close(pfd[1]);
    int status = 0;
    sc6(SYS_WAIT4, pid, (i64)&status, 0, 0, 0, 0);
    i64 via = r_read(pfd[0], buf, 16);
    line("scm_rights pipe", via == 6 && memeq(buf, "via-fd", 6));
    line("child exit", ((status >> 8) & 0xff) == 0);
    r_close(sfd);
    r_close(lsn);
    r_close(pfd[0]);
    r_close(cs[0]);
    r_close(cs[1]);
    sc6(SYS_UNLINKAT, -100, (i64)SOCK_PATH, 0, 0, 0, 0);

    /* SCM_CREDENTIALS with SO_PASSCRED: the receiver gets its sender's pid. */
    r_socketpair(1, SOCK_STREAM, 0, sv);
    u8 one[4] = {1, 0, 0, 0};
    r_setsockopt(sv[1], SOL_SOCKET, SO_PASSCRED, one, 4);
    r_write(sv[0], "c", 1);
    u8 cbuf2[64];
    u8 cdat[1];
    struct iovec civ = {cdat, 1};
    struct msghdr cmh = {.msg_iov = &civ, .msg_iovlen = 1, .msg_control = cbuf2, .msg_controllen = 64};
    i64 cr = r_recvmsg(sv[1], &cmh, 0);
    int cred_ok = cr == 1 && cbuf2[12] == SCM_CREDENTIALS && get_u32(cbuf2 + 16) != 0;
    line("scm_credentials", cred_ok);
    r_close(sv[0]);
    r_close(sv[1]);

    /* getpeername of a connected pair is unnamed (family only). */
    r_socketpair(1, SOCK_STREAM, 0, sv);
    u8 pn[110];
    u32 pnl = 110;
    r_getpeername(sv[0], pn, &pnl);
    line("getpeername unnamed", pnl == 2);
    r_close(sv[0]);
    r_close(sv[1]);

    /* getpeername on a closed pair: ENOTCONN. */
    int lone = (int)r_socket(AF_UNIX, SOCK_STREAM, 0);
    line("getpeername unconnected", r_getpeername(lone, pn, &pnl) == -ENOTCONN);
    r_close(lone);

    /* Seqpacket listener with a forked client: message boundaries across processes. */
    sc6(SYS_UNLINKAT, -100, (i64)SOCK_PATH2, 0, 0, 0, 0);
    salen = mkaddr(sa, SOCK_PATH2, 0);
    int sl = (int)r_socket(AF_UNIX, SOCK_SEQPACKET, 0);
    r_bind(sl, sa, salen);
    r_listen(sl, 2);
    pid = sc6(SYS_CLONE, 17, 0, 0, 0, 0, 0);
    if (pid == 0)
        sc6(93, seq_child(sa, salen), 0, 0, 0, 0, 0);
    int sq = (int)r_accept4(sl, 0, 0, 0);
    char spm[16];
    int sp_ok = r_recvfrom(sq, spm, 16, 0) == 8 && memeq(spm, "sp-hello", 8);
    r_sendto(sq, "reply", 5, 0);
    int sp_exit = wait_status((int)pid);
    line("seqpacket fork", sp_ok && sp_exit == 0);
    r_close(sq);
    r_close(sl);
    sc6(SYS_UNLINKAT, -100, (i64)SOCK_PATH2, 0, 0, 0, 0);

    /* A blocking write of 1 MiB to a slow reader, in a forked writer: the call sends in
     * chunks and blocks while the reader is away, and the bytes arrive in order. */
    r_socketpair(1, SOCK_STREAM, 0, sv);
    pid = sc6(SYS_CLONE, 17, 0, 0, 0, 0, 0);
    if (pid == 0)
        sc6(93, bulk_child(sv[0]), 0, 0, 0, 0, 0);
    nap_ms(20);
    u64 got_bulk = 0;
    int bad_bulk = 0;
    for (;;) {
        i64 n = r_read(sv[1], rb, sizeof rb);
        if (n <= 0)
            break;
        for (i64 i = 0; i < n; i++)
            if (rb[i] != pat(got_bulk + (u64)i))
                bad_bulk = 1;
        got_bulk += (u64)n;
        if (got_bulk == BULK)
            break;
    }
    int bulk_exit = wait_status((int)pid);
    line("blocking stream write", got_bulk == BULK && !bad_bulk && bulk_exit == 0);
    r_close(sv[0]);
    r_close(sv[1]);

    /* Readiness wakeups: ppoll wakes on a pending accept, epoll on data, and epoll on the
     * hangup when the peer closes. The client sleeps between its steps. */
    sc6(SYS_UNLINKAT, -100, (i64)SOCK_PATH3, 0, 0, 0, 0);
    salen = mkaddr(sa, SOCK_PATH3, 0);
    int ps = (int)r_socket(AF_UNIX, SOCK_STREAM, 0);
    r_bind(ps, sa, salen);
    r_listen(ps, 2);
    pid = sc6(SYS_CLONE, 17, 0, 0, 0, 0, 0);
    if (pid == 0)
        sc6(93, poll_child(sa, salen), 0, 0, 0, 0, 0);
    i16 pr = 0;
    int pwake = ppoll_block(ps, POLLIN, &pr);
    line("ppoll wakes on pending accept", pwake == 1 && (pr & POLLIN));
    int pc = (int)r_accept4(ps, 0, 0, 0);
    int ep = (int)sc6(SYS_EPOLL_CREATE1, 0, 0, 0, 0, 0, 0);
    epoll_add(ep, pc);
    u8 evs[16 * 4];
    int en = epoll_wait_block(ep, evs, 4);
    char pokebuf[8];
    int data_ok = en == 1 && (get_u32(evs) & EPOLLIN) && r_read(pc, pokebuf, 4) == 4 && memeq(pokebuf, "poke", 4);
    line("epoll wakes on data", data_ok);
    en = epoll_wait_block(ep, evs, 4);
    line("epoll wakes on hangup", en == 1 && (get_u32(evs) & EPOLLHUP));
    line("poll child exit", wait_status((int)pid) == 0);
    r_close(pc);
    r_close(ps);
    r_close(ep);
    sc6(SYS_UNLINKAT, -100, (i64)SOCK_PATH3, 0, 0, 0, 0);

    sc6(SYS_UNLINKAT, -100, (i64)SOCK_PATH, 0, 0, 0, 0);
    put("done\n");
    return 0;
}
