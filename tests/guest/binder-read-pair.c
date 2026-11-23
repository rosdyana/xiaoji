/* binder-read-pair (B4f): a binder read returns one transaction, and a reply pairs with the request the
 * thread is serving (the driver's binder_thread_read ends its loop after a BR_TRANSACTION or BR_REPLY,
 * and BC_REPLY takes thread->transaction_stack).
 *
 * The bug (B4f): the engine's read kept batching after a transaction, so two requests came back in one
 * read. The server answered the first with the reply stack's head, which was the second request, so
 * each client got the other's reply. servicemanager's checkService("thermalservice") and
 * ("activity") were answered that way in the boot, and the thermal lookup returned null.
 *
 * The parent is the orchestrator. The server (a forked child) is the context manager and is single-
 * threaded. Two forked clients each send one sync request (code 11, then code 12) and then wait for
 * their reply. The parent sequences them: each client has queued its request before the next starts,
 * and the server reads only after both are queued, so both requests wait in the proc queue at once.
 *
 * Expected (the kernel): each server read has one request, and each client gets its own code back.
 * Each client exits 0 if its reply carries its code, 2 if not.
 *
 * The native oracle is the builder VM's Linux binder (run with sudo -n: /dev/binder is root-only
 * there). The constants are the kernel UAPI's (linux/android/binder.h). */
#include "sys.h"

#define SYS_IOCTL 29
#define SYS_OPENAT 56
#define SYS_READ 63
#define SYS_WRITE_ 64
#define SYS_PIPE2 59
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SIGCHLD_N 17
#define AT_FDCWD_ (-100)
#define OPEN_RDWR_CLOEXEC 0x80002     /* O_RDWR | O_CLOEXEC */
#define PROT_READ_ 1
#define MAP_PRIVATE_ 2
#define MAP_NORESERVE_ 0x4000
#define MAP_LEN 65536
#define REQUESTS 2

#define BINDER_WRITE_READ 0xc0306201u
#define BINDER_SET_CONTEXT_MGR_EXT 0x4018620du
#define BC_TRANSACTION 0x40406300u
#define BC_REPLY 0x40406301u
#define BC_FREE_BUFFER 0x40086303u
#define BC_ENTER_LOOPER 0x630cu
#define BR_OK 0x7201u
#define BR_NOOP 0x720cu
#define BR_SPAWN_LOOPER 0x720du
#define BR_TRANSACTION_COMPLETE 0x7206u
#define BR_DEAD_REPLY 0x7205u
#define BR_FAILED_REPLY 0x7211u
#define BR_TRANSACTION 0x80407202u
#define BR_REPLY 0x80407203u
#define BR_INCREFS 0x80107207u
#define BR_ACQUIRE 0x80107208u
#define BR_RELEASE 0x80107209u
#define BR_DECREFS 0x8010720au

#define CODE_FIRST 11u
#define CODE_SECOND 12u

struct btd { /* binder_transaction_data: the body of BR_TRANSACTION and BR_REPLY (64 bytes) */
    u64 target, cookie;
    u32 code, flags;
    u32 sender_pid, sender_euid;
    u64 data_size, offsets_size, buffer, offsets;
};
struct bwr {
    u64 write_size, write_consumed, write_buffer;
    u64 read_size, read_consumed, read_buffer;
};

_Static_assert(sizeof(struct btd) == 64, "binder_transaction_data");
_Static_assert(sizeof(struct bwr) == 48, "binder_write_read");

/* The offsets of the fields in a return's body (the struct above, after its command word). */
#define BODY_CODE 16
#define BODY_DATA_SIZE 32
#define BODY_BUFFER 48

static u8 wbuf[256];
static u64 wlen;

static i64 sc_ioctl(int fd, u32 req, void *arg) { return sc6(SYS_IOCTL, fd, (i64)req, (i64)arg, 0, 0, 0); }
static i64 sc_read(int fd, void *b, size_t n) { return sc6(SYS_READ, fd, (i64)b, (i64)n, 0, 0, 0); }
static i64 sc_write(int fd, const void *b, size_t n) { return sc6(SYS_WRITE_, fd, (i64)b, (i64)n, 0, 0, 0); }

static u32 ld32(const u8 *p)
{
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

static u64 ld64(const u8 *p)
{
    u64 v;
    memcpy(&v, p, 8);
    return v;
}

static int open_map(int flags, int *out)
{
    i64 fd = sc6(SYS_OPENAT, AT_FDCWD_, (i64)"/dev/binder", flags, 0, 0, 0);
    if (fd < 0)
        return -1;
    i64 m = sys_mmap(0, MAP_LEN, PROT_READ_, MAP_PRIVATE_ | MAP_NORESERVE_, fd, 0);
    if (m < 0 && m > -4096)
        return -1;
    *out = (int)fd;
    return 0;
}

/* One BINDER_WRITE_READ of the queued commands (wbuf, wlen) and a read of rsize bytes at rb. */
static i64 write_read(int fd, void *rb, u64 rsize, u64 *consumed)
{
    struct bwr b;
    memset(&b, 0, sizeof b);
    b.write_size = wlen;
    b.write_buffer = (u64)(size_t)wbuf;
    b.read_size = rsize;
    b.read_buffer = (u64)(size_t)rb;
    i64 rc = sc_ioctl(fd, BINDER_WRITE_READ, &b);
    if (consumed)
        *consumed = b.read_consumed;
    wlen = 0;
    return rc;
}

static void put_cmd(u32 c)
{
    memcpy(wbuf + wlen, &c, 4);
    wlen += 4;
}

/* A BC_TRANSACTION (to handle 0, the context manager) or a BC_REPLY with a 4-byte payload at *payload. */
static void put_send(u32 cmd, u32 code, u32 *payload)
{
    struct btd t;
    memset(&t, 0, sizeof t);
    t.code = code;
    t.data_size = 4;
    t.buffer = (u64)(size_t)payload;
    put_cmd(cmd);
    memcpy(wbuf + wlen, &t, sizeof t);
    wlen += sizeof t;
}

static void put_free(u64 buffer)
{
    put_cmd(BC_FREE_BUFFER);
    memcpy(wbuf + wlen, &buffer, 8);
    wlen += 8;
}

/* The body size of a return this test knows (after its 4-byte command), or -1. */
static int ret_body(u32 cmd)
{
    switch (cmd) {
    case BR_NOOP:
    case BR_OK:
    case BR_TRANSACTION_COMPLETE:
    case BR_SPAWN_LOOPER:
    case BR_DEAD_REPLY:
    case BR_FAILED_REPLY:
        return 0;
    case BR_INCREFS:
    case BR_ACQUIRE:
    case BR_RELEASE:
    case BR_DECREFS:
        return 16;
    case BR_TRANSACTION:
    case BR_REPLY:
        return 64;
    default:
        return -1;
    }
}

/* The returns of one read, in order. bad is set on a return this test does not know. */
struct rd {
    const u8 *p;
    u64 left;
    int bad;
};

static int next_ret(struct rd *r, u32 *cmd, const u8 **body)
{
    if (r->left < 4)
        return 0;
    u32 c = ld32(r->p);
    int n = ret_body(c);
    if (n < 0 || r->left < 4 + (u64)n) {
        r->bad = 1;
        return 0;
    }
    *cmd = c;
    *body = r->p + 4;
    r->p += 4 + (u64)n;
    r->left -= 4 + (u64)n;
    return 1;
}

/* The context manager: one thread, one read per turn. Each read's requests are answered with their
 * codes. The line per read says how many requests the read carried, and their codes. */
static void server(int ready_w, int go_r)
{
    int fd;
    u64 zero[3] = {0, 0, 0};
    if (open_map(OPEN_RDWR_CLOEXEC, &fd)) {
        put("binder-read-pair: server open failed\n");
        sys_exit_group(2);
    }
    sc_ioctl(fd, BINDER_SET_CONTEXT_MGR_EXT, zero);
    put_cmd(BC_ENTER_LOOPER);
    write_read(fd, NULL, 0, NULL);
    char ok = 'r';
    sc_write(ready_w, &ok, 1);
    char go;
    sc_read(go_r, &go, 1); /* both requests are queued */
    u32 handled = 0;
    while (handled < REQUESTS) {
        u8 rb[512];
        u64 consumed = 0;
        write_read(fd, rb, sizeof rb, &consumed);
        struct rd r = {rb, consumed, 0};
        u32 cmd, codes[REQUESTS], pay[REQUESTS], nreq = 0;
        u64 bufs[REQUESTS];
        const u8 *body;
        while (next_ret(&r, &cmd, &body)) {
            if (cmd == BR_TRANSACTION && nreq < REQUESTS) {
                codes[nreq] = ld32(body + BODY_CODE);
                pay[nreq] = codes[nreq];
                bufs[nreq] = ld64(body + BODY_BUFFER);
                nreq++;
            }
        }
        if (r.bad) {
            put("binder-read-pair: server: unexpected return\n");
            sys_exit_group(4);
        }
        if (nreq == 0)
            continue;
        put("server read: requests ");
        putd(nreq);
        put(" codes");
        for (u32 i = 0; i < nreq; i++) {
            put(" ");
            putd(codes[i]);
        }
        put("\n");
        for (u32 i = 0; i < nreq; i++) {
            put_free(bufs[i]);
            put_send(BC_REPLY, 0, &pay[i]);
        }
        write_read(fd, NULL, 0, NULL);
        handled += nreq;
    }
    sys_exit_group(0);
}

/* A client: one sync request, then the reply. Exit 0 if the reply carries the request's code. */
static int client(u32 code, int queued_w)
{
    int fd;
    if (open_map(OPEN_RDWR_CLOEXEC, &fd))
        return 10;
    u32 payload = code;
    put_send(BC_TRANSACTION, code, &payload);
    write_read(fd, NULL, 0, NULL);
    char q = 'q';
    sc_write(queued_w, &q, 1);
    for (;;) {
        u8 rb[256];
        u64 consumed = 0;
        if (write_read(fd, rb, sizeof rb, &consumed) < 0)
            return 11;
        struct rd r = {rb, consumed, 0};
        u32 cmd;
        const u8 *body;
        while (next_ret(&r, &cmd, &body)) {
            if (cmd == BR_REPLY) {
                u64 buf = ld64(body + BODY_BUFFER);
                u64 dsz = ld64(body + BODY_DATA_SIZE);
                u32 got = dsz >= 4 ? ld32((const u8 *)(size_t)buf) : 0xffffffffu;
                put_free(buf);
                write_read(fd, NULL, 0, NULL);
                return got == code ? 0 : 2;
            }
            if (cmd == BR_FAILED_REPLY || cmd == BR_DEAD_REPLY)
                return 3;
        }
        if (r.bad)
            return 4;
    }
}

static void report(int n, u32 code, int status)
{
    int rc = (status >> 8) & 0xff;
    put("client ");
    putd(n);
    put(" (code ");
    putd(code);
    put("): ");
    if (rc == 0)
        put("reply ok\n");
    else if (rc == 2)
        put("reply wrong\n");
    else {
        put("no reply, exit ");
        putd(rc);
        put("\n");
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int ready[2], go[2], queued[2];
    if (sc6(SYS_PIPE2, (i64)ready, 0, 0, 0, 0, 0) < 0 || sc6(SYS_PIPE2, (i64)go, 0, 0, 0, 0, 0) < 0 ||
        sc6(SYS_PIPE2, (i64)queued, 0, 0, 0, 0, 0) < 0) {
        put("binder-read-pair: pipe failed\n");
        return 1;
    }
    i64 srv = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (srv < 0) {
        put("binder-read-pair: fork failed\n");
        return 1;
    }
    if (srv == 0) {
        server(ready[1], go[0]);
        sys_exit_group(3);
    }
    char b;
    sc_read(ready[0], &b, 1); /* the server is the context manager */

    i64 c1 = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (c1 == 0)
        sys_exit_group(client(CODE_FIRST, queued[1]));
    sc_read(queued[0], &b, 1); /* the first request is queued */

    i64 c2 = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (c2 == 0)
        sys_exit_group(client(CODE_SECOND, queued[1]));
    sc_read(queued[0], &b, 1); /* the second request is queued: both wait in the proc queue */

    sc_write(go[1], "g", 1);
    int st1 = 0, st2 = 0, sst = 0;
    sc6(SYS_WAIT4, c1, (i64)&st1, 0, 0, 0, 0);
    sc6(SYS_WAIT4, c2, (i64)&st2, 0, 0, 0, 0);
    sc6(SYS_WAIT4, srv, (i64)&sst, 0, 0, 0, 0);
    report(1, CODE_FIRST, st1);
    report(2, CODE_SECOND, st2);
    return 0;
}
