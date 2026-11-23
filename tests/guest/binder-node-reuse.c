/* binder-node-reuse (B4e): a binder node of a live owner is freed once nothing refers to it, so a
 * later object at the same address (a new object, with a new cookie) gets a new node. A different
 * cookie at the address of a node that is still referenced is refused: the send fails with
 * BR_FAILED_REPLY (binder_translate_binder's cookie check in drivers/android/binder.c).
 *
 * The thermal fault of B4d reused a weakref address. The engine kept the old node and gave the new
 * object its old cookie, so a call through the handle reached the address of a freed object.
 *
 * The parent is the owner (A). A forked child is the context manager (B): the kernel refuses a
 * sync transaction between two binder fds of one process. The parent sends with a write and no
 * read, and it reads with O_NONBLOCK until EAGAIN, so it never waits on the kernel. The child
 * blocks in its own reads, and the two sides take turns through two pipes (a command, then an
 * acknowledgement that the command is done).
 *
 * The native oracle is the builder VM's Linux binder (run with sudo -n: /dev/binder is root-only
 * there), so the expected output is the kernel's answer. The constants are the kernel UAPI's
 * (linux/android/binder.h), and the rules are the kernel's (binder_translate_binder,
 * binder_dec_node_nilocked, the BINDER_WORK_NODE case). */
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
#define OPEN_RDWR_CLOEXEC 0x80002     /* O_RDWR | O_CLOEXEC */
#define OPEN_RDWR_NB_CLOEXEC 0x80802  /* O_RDWR | O_NONBLOCK | O_CLOEXEC */
#define PROT_READ_ 1
#define MAP_PRIVATE_ 2
#define MAP_NORESERVE_ 0x4000
#define MAP_LEN 65536

#define BINDER_WRITE_READ 0xc0306201u
#define BINDER_SET_CONTEXT_MGR_EXT 0x4018620du
#define BC_TRANSACTION 0x40406300u
#define BC_REPLY 0x40406301u
#define BC_FREE_BUFFER 0x40086303u
#define BC_INCREFS_DONE 0x40106308u
#define BC_ACQUIRE_DONE 0x40106309u
#define BR_NOOP 0x720cu
#define BR_TRANSACTION_COMPLETE 0x7206u
#define BR_DEAD_REPLY 0x7205u
#define BR_FAILED_REPLY 0x7211u
#define BR_SPAWN_LOOPER 0x720du
#define BR_TRANSACTION 0x80407202u
#define BR_REPLY 0x80407203u
#define BR_INCREFS 0x80107207u
#define BR_ACQUIRE 0x80107208u
#define BR_RELEASE 0x80107209u
#define BR_DECREFS 0x8010720au

#define TF_ONE_WAY 1u
#define OBJ_BINDER 0x73622a85u
#define OBJ_HANDLE 0x73682a85u

#define CODE_OBJECT 1u
#define CODE_CALLBACK 9u

#define PTR_ADDR 0x1000ull /* the object's address, the same in every step */
#define COOKIE_1 0xc001ull
#define COOKIE_2 0xc002ull
#define COOKIE_3 0xc003ull

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

/* What one receive saw. */
struct cnt {
    u32 txn_complete, acq, incr, rel, decr, reply, failed, dead, requests;
    u64 buffer; /* the last request's buffer, in this process's mapping */
    u64 cookie; /* the last request's cookie */
    u32 code, flags;
};

static u8 rbuf[4096];
static u8 pend[4096];
static u64 npend;
static u32 status_ok; /* the reply's status word, 0 */

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

/* One BINDER_WRITE_READ. */
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

static void pq(const void *p, u64 n)
{
    memcpy(pend + npend, p, n);
    npend += n;
}

static void pq_cmd(u32 c) { pq(&c, 4); }

static void pq_u64(u64 v) { pq(&v, 8); }

/* Writes the queued commands (no read). */
static void flush(int fd)
{
    if (npend) {
        write_read(fd, pend, npend, NULL, 0, NULL);
        npend = 0;
    }
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

/* A sync request from fd to the context manager (handle 0): one object, a binder (ptr, cookie). */
static void send_object(int fd, u64 ptr, u64 cookie)
{
    struct flat obj = {OBJ_BINDER, 0, ptr, cookie};
    u64 offs[1] = {0};
    struct btd t;
    memset(&t, 0, sizeof t);
    t.code = CODE_OBJECT;
    t.flags = 0;
    t.data_size = sizeof obj;
    t.offsets_size = sizeof offs;
    t.buffer = (u64)(size_t)&obj;
    t.offsets = (u64)(size_t)offs;
    u8 w[4 + sizeof t];
    u32 cmd = BC_TRANSACTION;
    memcpy(w, &cmd, 4);
    memcpy(w + 4, &t, sizeof t);
    write_read(fd, w, sizeof w, NULL, 0, NULL);
}

/* A oneway request from fd to handle h (eight bytes of data). */
static void send_oneway(int fd, u32 h)
{
    u8 dummy[8] = {0};
    struct btd t;
    memset(&t, 0, sizeof t);
    t.target = h;
    t.code = CODE_CALLBACK;
    t.flags = TF_ONE_WAY;
    t.data_size = sizeof dummy;
    t.buffer = (u64)(size_t)dummy;
    u8 w[4 + sizeof t];
    u32 cmd = BC_TRANSACTION;
    memcpy(w, &cmd, 4);
    memcpy(w + 4, &t, sizeof t);
    write_read(fd, w, sizeof w, NULL, 0, NULL);
}

/* Parses the records in rbuf (used bytes). Counts them into c and acknowledges the owner's
 * counts (queued). A request is recorded (buffer, cookie, code, flags) for the caller to answer.
 * Returns 0, or -1 for a record this test does not expect. */
static int parse(u64 used, struct cnt *c)
{
    u64 pos = 0;
    while (pos + 4 <= used) {
        u32 cmd = ld32(rbuf + pos);
        if (cmd == BR_NOOP || cmd == BR_SPAWN_LOOPER) {
            pos += 4;
        } else if (cmd == BR_TRANSACTION_COMPLETE) {
            c->txn_complete++;
            pos += 4;
        } else if (cmd == BR_FAILED_REPLY) {
            c->failed++;
            pos += 4;
        } else if (cmd == BR_DEAD_REPLY) {
            c->dead++;
            pos += 4;
        } else if (cmd == BR_REPLY) {
            c->reply++;
            pos += 4 + 64;
        } else if (cmd == BR_ACQUIRE || cmd == BR_INCREFS) {
            u64 p = ld64(rbuf + pos + 4), k = ld64(rbuf + pos + 12);
            if (cmd == BR_ACQUIRE) {
                c->acq++;
                pq_cmd(BC_ACQUIRE_DONE);
            } else {
                c->incr++;
                pq_cmd(BC_INCREFS_DONE);
            }
            pq_u64(p);
            pq_u64(k);
            pos += 4 + 16;
        } else if (cmd == BR_RELEASE) {
            c->rel++;
            pos += 4 + 16;
        } else if (cmd == BR_DECREFS) {
            c->decr++;
            pos += 4 + 16;
        } else if (cmd == BR_TRANSACTION) {
            const u8 *t = rbuf + pos + 4;
            c->requests++;
            c->cookie = ld64(t + 8);
            c->code = ld32(t + 16);
            c->flags = ld32(t + 20);
            c->buffer = ld64(t + 48);
            pos += 4 + 64;
        } else {
            return -1;
        }
    }
    return 0;
}

/* The owner's side: reads until EAGAIN (nothing blocks). */
static void receive(int fd, struct cnt *c)
{
    for (;;) {
        u64 used = 0;
        i64 rc = write_read(fd, NULL, 0, rbuf, sizeof rbuf, &used);
        if (rc < 0 || used == 0)
            break;
        if (parse(used, c) < 0) {
            put("binder-node-reuse: unexpected return\n");
            break;
        }
        flush(fd);
    }
}

/* The context manager's side: reads until one request has arrived (blocks until it does). */
static void receive_request(int fd, struct cnt *c)
{
    while (!c->requests) {
        u64 used = 0;
        i64 rc = write_read(fd, NULL, 0, rbuf, sizeof rbuf, &used);
        if (rc < 0)
            break;
        if (parse(used, c) < 0) {
            put("binder-node-reuse: unexpected return (server)\n");
            break;
        }
        flush(fd);
    }
}

/* Answers a request: frees its buffer (the reference its object holds is dropped), and replies with
 * a status word if asked. */
static void answer(int fd, u64 buffer, int free_buf, int reply)
{
    if (free_buf) {
        pq_cmd(BC_FREE_BUFFER);
        pq_u64(buffer);
    }
    if (reply) {
        struct btd r;
        memset(&r, 0, sizeof r);
        r.buffer = (u64)(size_t)&status_ok;
        r.data_size = 4;
        pq_cmd(BC_REPLY);
        pq(&r, sizeof r);
    }
    flush(fd);
}

/* The owner's view of one step, for the steps whose count traffic the kernel and the engine give the
 * same way (the transaction's own completion and its reply). The owner's count acknowledgements
 * are not printed: the first translation of a new node gets BR_ACQUIRE and BR_INCREFS from the
 * engine and none from the kernel, so they are not compared. */
static void line_reply(const char *step, const struct cnt *c)
{
    put(step);
    put(": txn-complete ");
    putd(c->txn_complete);
    put(" reply ");
    putd(c->reply);
    put("\n");
}

/* The context manager. Commands from the parent, one byte each, each acknowledged with 'k':
 * 'R' takes the object, frees its buffer and replies; 'K' takes it, keeps the buffer and replies;
 * 'C' calls back through the kept handle (oneway); 'F' frees the kept buffer. EOF ends it. */
static void server(int cmd_fd, int ack_fd)
{
    int fd;
    struct flat zero;
    memset(&zero, 0, sizeof zero);
    if (open_map(OPEN_RDWR_CLOEXEC, &fd)) {
        put("binder-node-reuse: server open failed\n");
        sys_exit_group(2);
    }
    sc_ioctl(fd, BINDER_SET_CONTEXT_MGR_EXT, &zero);
    char ok = 'k';
    sc_write(ack_fd, &ok, 1);
    u32 h2 = 0;
    u64 keep = 0;
    for (;;) {
        char c;
        if (sc_read(cmd_fd, &c, 1) != 1)
            break;
        struct cnt cb;
        memset(&cb, 0, sizeof cb);
        if (c == 'R' || c == 'K') {
            receive_request(fd, &cb);
            if (ld32((const u8 *)(size_t)cb.buffer) == OBJ_HANDLE)
                h2 = ld32((const u8 *)(size_t)cb.buffer + 8);
            if (c == 'R') {
                answer(fd, cb.buffer, 1, 1);
            } else {
                keep = cb.buffer;
                answer(fd, cb.buffer, 0, 1);
            }
        } else if (c == 'C') {
            send_oneway(fd, h2);
        } else if (c == 'F') {
            answer(fd, keep, 1, 0);
        }
        sc_write(ack_fd, &ok, 1);
    }
    sys_exit_group(0);
}

static void wait_child(int ack_fd)
{
    char b;
    sc_read(ack_fd, &b, 1);
}

static void command(int cmd_fd, int ack_fd, char c)
{
    sc_write(cmd_fd, &c, 1);
    wait_child(ack_fd);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int cmd[2], ack[2];
    if (sc6(SYS_PIPE2, (i64)cmd, 0, 0, 0, 0, 0) < 0 || sc6(SYS_PIPE2, (i64)ack, 0, 0, 0, 0, 0) < 0) {
        put("binder-node-reuse: pipe failed\n");
        return 1;
    }
    i64 pid = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (pid < 0) {
        put("binder-node-reuse: fork failed\n");
        return 1;
    }
    if (pid == 0) {
        /* The server keeps only its ends: the pipe's EOF must reach it when the parent closes. */
        sc6(SYS_CLOSE, cmd[1], 0, 0, 0, 0, 0);
        sc6(SYS_CLOSE, ack[0], 0, 0, 0, 0, 0);
        server(cmd[0], ack[1]);
        sys_exit_group(3);
    }
    /* The parent waits until the child is the context manager. */
    wait_child(ack[0]);

    int A;
    if (open_map(OPEN_RDWR_NB_CLOEXEC, &A)) {
        put("binder-node-reuse: owner open failed\n");
        return 1;
    }
    struct cnt ca;

    /* Step 1: the owner gives the object (PTR, cookie 1). The server takes it, frees its buffer
     * (dropping its reference) and replies. The counts are released to the owner. */
    memset(&ca, 0, sizeof ca);
    send_object(A, PTR_ADDR, COOKIE_1);
    command(cmd[1], ack[0], 'R');
    receive(A, &ca);
    line_reply("step 1", &ca);

    /* Step 2: the owner gives the object at the same address, with cookie 2 (a new object: the old
     * one is gone). The server keeps the object (its buffer lives on) and replies. */
    memset(&ca, 0, sizeof ca);
    send_object(A, PTR_ADDR, COOKIE_2);
    command(cmd[1], ack[0], 'K');
    receive(A, &ca);
    line_reply("step 2", &ca);

    /* Step 3: the server calls back through its handle for the object (a oneway request). The
     * owner's request carries the cookie of the node the handle names. */
    memset(&ca, 0, sizeof ca);
    command(cmd[1], ack[0], 'C');
    receive(A, &ca);
    put("step 3: callback code ");
    putd(ca.code);
    put(" requests ");
    putd(ca.requests);
    put(" cookie ");
    putx(ca.cookie);
    put("\n");
    answer(A, ca.buffer, 1, 0);

    /* Step 4: the owner gives the address with cookie 3 while the server still holds the node with
     * cookie 2. The driver refuses the object: the send fails, and nothing is delivered. */
    memset(&ca, 0, sizeof ca);
    send_object(A, PTR_ADDR, COOKIE_3);
    receive(A, &ca);
    put("step 4: failed ");
    putd(ca.failed);
    put(" reply ");
    putd(ca.reply);
    put("\n");

    /* Step 5: the server frees the step-2 object. The owner is told to release it. */
    memset(&ca, 0, sizeof ca);
    command(cmd[1], ack[0], 'F');
    receive(A, &ca);
    put("step 5: release ");
    putd(ca.rel);
    put(" decref ");
    putd(ca.decr);
    put("\n");

    /* Closing the command pipe ends the server (its read sees EOF). */
    sc6(SYS_CLOSE, cmd[1], 0, 0, 0, 0, 0);
    int status = 0;
    sc6(SYS_WAIT4, pid, (i64)&status, 0, 0, 0, 0);
    return 0;
}
