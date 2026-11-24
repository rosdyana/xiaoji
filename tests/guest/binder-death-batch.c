/* binder-death-batch (B16): a binder read ends after a BR_DEAD_BINDER, as the driver's binder_thread_read does
 * ("goto done": the notification's handler can start a transaction, and libbinder does not read while commands are
 * unread). Two death notifications that are pending together come back one per read, each with the commands that
 * precede it in its own read.
 *
 * The bug (B16): the engine batched both notifications into one read. libbinder's handler for the first one made a
 * sync call, and the call's write-and-read took the second notification's command as its reply (the trace in
 * docs/boot.md B16 shows BR_REPLY reaching a looper, and the system_server aborts that followed).
 *
 * The parent is the context manager. A forked child is the owner of two binder nodes: it sends each node to the
 * parent in a sync request and waits for the parent's reply before it sends the next one (so the requests are not
 * nested: a sender with a request still pending routes its next request to the thread that serves the first). The
 * parent then requests a death notification for each node's handle, tells the child to exit, and reads with a large
 * buffer. The child's exit is the death of both nodes.
 *
 * Output: one line per read, with the commands it returned, a death as "BR_DEAD_BINDER", then the two cookies sorted
 * (the order of the deaths is the driver's, so it is not printed). Expected (the kernel): a read returns at most one
 * death. The native oracle is the builder VM's Linux binder (run with sudo -n). The constants are the kernel UAPI's
 * (linux/android/binder.h). */
#include "sys.h"

#define SYS_IOCTL 29
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_READ 63
#define SYS_WRITE_ 64
#define SYS_PIPE2 59
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SIGCHLD_N 17
#define AT_FDCWD_ (-100)
#define OPEN_RDWR_CLOEXEC 0x80002 /* O_RDWR | O_CLOEXEC */
#define PROT_READ_ 1
#define MAP_PRIVATE_ 2
#define MAP_NORESERVE_ 0x4000
#define MAP_LEN 65536

#define BINDER_WRITE_READ 0xc0306201u
#define BINDER_SET_CONTEXT_MGR_EXT 0x4018620du
#define BC_TRANSACTION 0x40406300u
#define BC_REPLY 0x40406301u
#define BC_REQUEST_DEATH_NOTIFICATION 0x400c630eu
#define BC_DEAD_BINDER_DONE 0x40086310u
#define BC_ENTER_LOOPER 0x0000630cu
#define BR_NOOP 0x720cu
#define BR_SPAWN_LOOPER 0x720du
#define BR_TRANSACTION_COMPLETE 0x7206u
#define BR_DEAD_BINDER 0x8008720fu
#define BR_TRANSACTION 0x80407202u
#define BR_REPLY 0x80407203u

#define OBJ_BINDER 0x73622a85u
#define CODE_OBJECT 1u
#define NODES 2

struct btd { /* binder_transaction_data */
    u64 target, cookie;
    u32 code, flags;
    u32 sender_pid, sender_euid;
    u64 data_size, offsets_size, buffer, offsets;
};
struct bwr {
    u64 write_size, write_consumed, write_buffer;
    u64 read_size, read_consumed, read_buffer;
};
struct flat { /* flat_binder_object */
    u32 type, flags;
    u64 binder, cookie;
};

_Static_assert(sizeof(struct btd) == 64, "binder_transaction_data");
_Static_assert(sizeof(struct bwr) == 48, "binder_write_read");
_Static_assert(sizeof(struct flat) == 24, "flat_binder_object");

static u8 rbuf[4096];
static u64 handles[NODES]; /* the handle of each node, from the translated object */
static u64 cookies[NODES]; /* the death cookies */

static i64 sc_ioctl(int fd, u32 req, void *arg) { return sc6(SYS_IOCTL, fd, (i64)req, (i64)arg, 0, 0, 0); }

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

/* One BINDER_WRITE_READ; *consumed gets the bytes the read filled. */
static i64 write_read(int fd, const void *wbuf, u64 wsize, void *rb, u64 rsize, u64 *consumed)
{
    struct bwr b;
    memset(&b, 0, sizeof b);
    b.write_size = wsize;
    b.write_buffer = (u64)(size_t)wbuf;
    b.read_size = rsize;
    b.read_buffer = (u64)(size_t)rb;
    i64 rc = sc_ioctl(fd, BINDER_WRITE_READ, &b);
    if (consumed)
        *consumed = b.read_consumed;
    return rc;
}

static int open_map(int *out)
{
    i64 fd = sc6(SYS_OPENAT, AT_FDCWD_, (i64)"/dev/binder", OPEN_RDWR_CLOEXEC, 0, 0, 0);
    if (fd < 0)
        return -1;
    i64 m = sys_mmap(0, MAP_LEN, PROT_READ_, MAP_PRIVATE_ | MAP_NORESERVE_, fd, 0);
    if (m < 0 && m > -4096)
        return -1;
    *out = (int)fd;
    return 0;
}

/* One child: owns node `which`. It sends the node to the context manager in one sync request, waits for the reply,
 * then waits for the parent's go (one byte) and exits. Each node has its own child, so no thread sends a second
 * request while its first is pending (a pending request routes the next one to the thread that serves the first:
 * the driver's nested-call rule). */
static int owner(int go_rd, int which)
{
    int fd;
    if (open_map(&fd) < 0)
        return 2;
    struct flat obj = {OBJ_BINDER, 0, 0x1000ull + (u64)which * 0x100, 0xc0ull + (u64)which};
    u64 offs[1] = {0};
    struct btd t;
    memset(&t, 0, sizeof t);
    t.code = CODE_OBJECT;
    t.data_size = sizeof obj;
    t.offsets_size = sizeof offs;
    t.buffer = (u64)(size_t)&obj;
    t.offsets = (u64)(size_t)offs;
    u8 w[4 + sizeof t];
    u32 cmd = BC_TRANSACTION;
    memcpy(w, &cmd, 4);
    memcpy(w + 4, &t, sizeof t);
    int answered = 0;
    for (int tries = 0; tries < 16 && !answered; tries++) {
        u8 rb[256];
        u64 used = 0;
        if (tries == 0)
            write_read(fd, w, sizeof w, rb, sizeof rb, &used);
        else
            write_read(fd, NULL, 0, rb, sizeof rb, &used);
        for (u64 pos = 0; pos + 4 <= used; pos += 4) {
            u32 rc = ld32(rb + pos);
            if (rc == BR_REPLY) {
                answered = 1;
                break;
            }
            if (rc != BR_TRANSACTION_COMPLETE && rc != BR_NOOP && rc != BR_SPAWN_LOOPER)
                break;
        }
    }
    u8 one;
    sc6(SYS_READ, go_rd, (i64)&one, 1, 0, 0, 0);
    return answered ? 0 : 4;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int fd;
    if (open_map(&fd) < 0) {
        put("open /dev/binder failed\n");
        return 1;
    }
    /* The parent is the context manager (BINDER_SET_CONTEXT_MGR_EXT with an empty object) and a looper. */
    struct flat none = {0, 0, 0, 0};
    if (sc_ioctl(fd, BINDER_SET_CONTEXT_MGR_EXT, &none) < 0) {
        put("set context manager failed\n");
        return 1;
    }
    u32 enter = BC_ENTER_LOOPER;
    write_read(fd, &enter, 4, NULL, 0, NULL);
    int go[2];
    if (sc6(SYS_PIPE2, (i64)go, 0x80000, 0, 0, 0, 0) < 0) {
        put("pipe failed\n");
        return 1;
    }
    i64 child[NODES];
    for (int i = 0; i < NODES; i++) {
        child[i] = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
        if (child[i] == 0) {
            sc6(SYS_CLOSE, go[1], 0, 0, 0, 0, 0);
            return owner(go[0], i);
        }
    }
    sc6(SYS_CLOSE, go[0], 0, 0, 0, 0, 0);

    /* Answer each request. The node's handle is in the translated object (the kernel writes BINDER_TYPE_HANDLE with
     * the handle in the object's second word). The request's buffer is kept: freeing it drops the references its
     * handles hold, and the death requests below need them. */
    int got = 0;
    while (got < NODES) {
        u64 used = 0;
        if (write_read(fd, NULL, 0, rbuf, sizeof rbuf, &used) < 0)
            break;
        for (u64 pos = 0; pos + 4 <= used;) {
            u32 cmd = ld32(rbuf + pos);
            if (cmd == BR_TRANSACTION) {
                const u8 *t = rbuf + pos + 4;
                u64 buf = ld64(t + 48);
                handles[got] = ld32((const u8 *)(size_t)buf + 8);
                got++;
                u32 status = 0;
                struct btd r;
                u8 reply[4 + sizeof r];
                memset(&r, 0, sizeof r);
                r.data_size = 4;
                r.buffer = (u64)(size_t)&status;
                u32 rcmd = BC_REPLY;
                memcpy(reply, &rcmd, 4);
                memcpy(reply + 4, &r, sizeof r);
                write_read(fd, reply, sizeof reply, NULL, 0, NULL);
                pos += 4 + 64;
            } else if (cmd == BR_NOOP || cmd == BR_SPAWN_LOOPER || cmd == BR_TRANSACTION_COMPLETE) {
                pos += 4;
            } else {
                break;
            }
        }
    }

    /* A death notification per node's handle: BC_REQUEST_DEATH_NOTIFICATION (handle, cookie), packed. */
    u8 req[NODES * 16];
    for (int i = 0; i < NODES; i++) {
        u32 c = BC_REQUEST_DEATH_NOTIFICATION;
        u32 h = (u32)handles[i];
        u64 cookie = i == 0 ? 0xd1ull : 0xd2ull;
        memcpy(req + 16 * i, &c, 4);
        memcpy(req + 16 * i + 4, &h, 4);
        memcpy(req + 16 * i + 8, &cookie, 8);
    }
    write_read(fd, req, sizeof req, NULL, 0, NULL);

    /* The child may exit now: both nodes die with it. */
    u8 one[NODES];
    memset(one, 1, sizeof one);
    sc6(SYS_WRITE_, go[1], (i64)one, NODES, 0, 0, 0);

    int deaths = 0;
    for (int reads = 1; reads <= 8 && deaths < NODES; reads++) {
        u64 used = 0;
        if (write_read(fd, NULL, 0, rbuf, sizeof rbuf, &used) < 0)
            break;
        put("read ");
        putd(reads);
        put(":");
        u64 pos = 0;
        u64 acks = 0;
        u8 ack[64];
        while (pos + 4 <= used) {
            u32 cmd = ld32(rbuf + pos);
            if (cmd == BR_NOOP) {
                put(" BR_NOOP");
                pos += 4;
            } else if (cmd == BR_SPAWN_LOOPER) {
                put(" BR_SPAWN_LOOPER");
                pos += 4;
            } else if (cmd == BR_TRANSACTION_COMPLETE) {
                put(" BR_TRANSACTION_COMPLETE");
                pos += 4;
            } else if (cmd == BR_DEAD_BINDER) {
                u64 c = ld64(rbuf + pos + 4);
                put(" BR_DEAD_BINDER");
                if (deaths < NODES)
                    cookies[deaths] = c;
                deaths++;
                u32 done = BC_DEAD_BINDER_DONE;
                memcpy(ack + acks, &done, 4);
                memcpy(ack + acks + 4, &c, 8);
                acks += 12;
                pos += 4 + 8;
            } else {
                put(" ?");
                break;
            }
        }
        put("\n");
        if (acks)
            write_read(fd, ack, acks, NULL, 0, NULL);
    }
    put("deaths: ");
    putd(deaths);
    if (deaths == NODES) {
        if (cookies[0] > cookies[1]) {
            u64 t = cookies[0];
            cookies[0] = cookies[1];
            cookies[1] = t;
        }
        put(" cookies: ");
        putd((i64)cookies[0]);
        put(" ");
        putd((i64)cookies[1]);
    }
    put("\n");

    for (int i = 0; i < NODES; i++) {
        int status = 0;
        (void)sc6(SYS_WAIT4, child[i], (i64)&status, 0, 0, 0, 0);
    }
    sc6(SYS_CLOSE, go[1], 0, 0, 0, 0, 0);
    return deaths == NODES ? 0 : 3;
}
