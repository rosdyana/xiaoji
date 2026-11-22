/* binder-sg (B4c part 2): the binder driver's scatter-gather objects with parents, as libhwbinder
 * sends them (Parcel::writeEmbeddedBuffer, and the FDA of writeFileDescriptors).
 *
 * A forked server is the context manager (handle 0). The client sends BC_TRANSACTION_SG requests
 * whose data holds BINDER_TYPE_PTR objects. A child with BINDER_BUFFER_FLAG_HAS_PARENT must find
 * its own address in the receiver at parent_offset of its parent (libhwbinder's verifyBufferObject
 * check). The client also sends a BINDER_TYPE_FDA in a parent buffer, a plain PTR whose parent
 * fields are ignored, and two children of one parent, in order and out of order. The server reports
 * what the driver delivered in a status word. Case 7's reply is a BC_REPLY_SG with the same embedded
 * shape, which the client checks. The malformed cases must fail with BR_FAILED_REPLY.
 *
 * The data starts with an 8-byte word that is not an object: the Linux driver cannot fix up into a
 * parent that is at data offset 0 (binder_validate_fixup treats offset 0 as "nothing to fix up").
 *
 * The native oracle is the builder VM's Linux binder, run with sudo -n (/dev/binder is root-only
 * there), so the expected output is the kernel's answer. The constants are the kernel UAPI's
 * (linux/android/binder.h), checked in the builder VM. The rules are the kernel's
 * (drivers/android/binder.c: binder_validate_fixup, binder_fixup_parent, the FDA branch). */
#include "sys.h"

#define SYS_IOCTL 29
#define SYS_OPENAT 56
#define SYS_PIPE2 59
#define SYS_READ 63
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SIGCHLD_N 17
#define AT_FDCWD_ (-100)
#define OPEN_RDWR_CLOEXEC 0x80002
#define PROT_READ_ 1
#define MAP_PRIVATE_ 2
#define MAP_NORESERVE_ 0x4000
#define MAP_LEN 65536

#define BINDER_WRITE_READ 0xc0306201u
#define BINDER_SET_CONTEXT_MGR_EXT 0x4018620du
#define FLAT_BINDER_FLAG_ACCEPTS_FDS 0x100u
#define BC_TRANSACTION_SG 0x40486311u
#define BC_REPLY 0x40406301u
#define BC_REPLY_SG 0x40486312u
#define BC_FREE_BUFFER 0x40086303u
#define BC_ENTER_LOOPER 0x630cu
#define BR_ERROR 0x80047200u
#define BR_NOOP 0x720cu
#define BR_TRANSACTION_COMPLETE 0x7206u
#define BR_DEAD_REPLY 0x7205u
#define BR_FAILED_REPLY 0x7211u
#define BR_SPAWN_LOOPER 0x720du
#define BR_TRANSACTION 0x80407202u
#define BR_REPLY 0x80407203u

#define TF_ACCEPT_FDS 0x10u
#define OBJ_PTR 0x70742a85u
#define OBJ_FDA 0x66646185u
#define OBJ_FD 0x66642a85u
#define FLAG_HAS_PARENT 1u

#define CODE_PARENT_EMBEDDED 1u
#define CODE_FDA 2u
#define CODE_FORWARD_PARENT 3u
#define CODE_OFFSET_OUT 4u
#define CODE_NONPTR_PARENT 5u
#define CODE_NO_FLAG 6u
#define CODE_REPLY_EMBEDDED 7u
#define CODE_TWO_CHILDREN 8u
#define CODE_OUT_OF_ORDER 9u
#define CODE_DONE 0xffu

/* Server status bits. */
#define ST_POINTER 1u /* the child at parent_offset 0 (or 8, for the FDA's parent) holds its address */
#define ST_CHILD 2u   /* the child's payload */
#define ST_PARENT 4u  /* the parent's payload */
#define ST_FD 8u      /* the FDA's fd reads "hello" */
#define ST_SECOND 16u /* the second child, at parent_offset 8, holds its address and payload */

struct btd {
    u64 target, cookie;
    u32 code, flags;
    u32 sender_pid, sender_euid;
    u64 data_size, offsets_size, buffer, offsets;
};
struct btd_sg {
    struct btd t;
    u64 buffers_size;
};
struct bwr {
    u64 write_size, write_consumed, write_buffer;
    u64 read_size, read_consumed, read_buffer;
};
struct bbo { /* binder_buffer_object */
    u32 type, flags;
    u64 buffer, length, parent, parent_offset;
};
struct bfda { /* binder_fd_array_object */
    u32 type, pad;
    u64 num_fds, parent, parent_offset;
};
struct bfd { /* binder_fd_object */
    u32 type, pad_flags;
    u64 fd, cookie;
};

_Static_assert(sizeof(struct btd) == 64, "binder_transaction_data");
_Static_assert(sizeof(struct btd_sg) == 72, "binder_transaction_data_sg");
_Static_assert(sizeof(struct bwr) == 48, "binder_write_read");
_Static_assert(sizeof(struct bbo) == 40, "binder_buffer_object");
_Static_assert(sizeof(struct bfda) == 32, "binder_fd_array_object");
_Static_assert(sizeof(struct bfd) == 24, "binder_fd_object");

static const char CHILD[16] = "child-payload-16";
static const char SECOND[16] = "second-payload-1";
static const char PARENT[8] = "parent00";
static const char HELLO[5] = "hello";
static const char REPLY_CHILD[16] = "reply-payload-16";

static i64 sc_ioctl(int fd, u32 req, void *arg) { return sc6(SYS_IOCTL, fd, (i64)req, (i64)arg, 0, 0, 0); }
static i64 sc_read(int fd, void *b, size_t n) { return sc6(SYS_READ, fd, (i64)b, (i64)n, 0, 0, 0); }

static void put_fail(const char *what, i64 rc)
{
    put("binder-sg: ");
    put(what);
    put(" failed (");
    putd(rc);
    put(")\n");
}

/* Opens /dev/binder and maps it. Returns the fd, or a negative value. */
static int binder_open_map(void)
{
    i64 fd = sc6(SYS_OPENAT, AT_FDCWD_, (i64)"/dev/binder", OPEN_RDWR_CLOEXEC, 0, 0, 0);
    if (fd < 0)
        return (int)fd;
    i64 m = sys_mmap(0, MAP_LEN, PROT_READ_, MAP_PRIVATE_ | MAP_NORESERVE_, fd, 0);
    if (m < 0 && m > -4096)
        return (int)m;
    return (int)fd;
}

/* One BINDER_WRITE_READ: wbuf (wsize bytes) to write, then a read of up to rsize bytes into rbuf.
 * *rconsumed gets the bytes read. Returns the ioctl result. */
static i64 write_read(int fd, const void *wbuf, u64 wsize, void *rbuf, u64 rsize, u64 *rconsumed)
{
    struct bwr b;
    memset(&b, 0, sizeof b);
    b.write_size = wsize;
    b.write_buffer = (u64)(size_t)wbuf;
    b.read_size = rsize;
    b.read_buffer = (u64)(size_t)rbuf;
    i64 rc = sc_ioctl(fd, BINDER_WRITE_READ, &b);
    if (rconsumed)
        *rconsumed = b.read_consumed;
    return rc;
}

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

static int same(const u8 *p, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != (u8)s[i])
            return 0;
    return 1;
}

/* The server's reply to one request: BC_FREE_BUFFER of its buffer, then BC_REPLY with a status. */
static void reply_status(int fd, const struct btd *tr, u32 status)
{
    u8 w[8 + 4 + 64];
    u64 n = 0;
    u32 cmd = BC_FREE_BUFFER;
    memcpy(w + n, &cmd, 4);
    n += 4;
    memcpy(w + n, &tr->buffer, 8);
    n += 8;
    cmd = BC_REPLY;
    memcpy(w + n, &cmd, 4);
    n += 4;
    struct btd r;
    memset(&r, 0, sizeof r);
    r.buffer = (u64)(size_t)&status;
    r.data_size = 4;
    memcpy(w + n, &r, sizeof r);
    n += sizeof r;
    write_read(fd, w, n, NULL, 0, NULL);
}

/* The server's check of one request. Returns the status word. */
static u32 check_request(int fd, const struct btd *tr)
{
    const u8 *data = (const u8 *)(size_t)tr->buffer;
    const u64 *offs = (const u64 *)(size_t)tr->offsets;
    u32 st = 0;
    if (tr->code == CODE_PARENT_EMBEDDED) {
        const struct bbo *p = (const struct bbo *)(data + offs[0]);
        const struct bbo *c = (const struct bbo *)(data + offs[1]);
        const u8 *pb = (const u8 *)(size_t)p->buffer;
        const u8 *cb = (const u8 *)(size_t)c->buffer;
        if (ld64(pb + c->parent_offset) == (u64)(size_t)cb)
            st |= ST_POINTER;
        if (same(cb, CHILD, 16))
            st |= ST_CHILD;
        if (same(pb, PARENT, 8))
            st |= ST_PARENT;
    } else if (tr->code == CODE_FDA) {
        const struct bbo *p = (const struct bbo *)(data + offs[0]);
        const u8 *pb = (const u8 *)(size_t)p->buffer;
        if (same(pb, PARENT, 8))
            st |= ST_PARENT;
        u8 got[5];
        if (sc_read((int)ld32(pb + 8), got, 5) == 5 && same(got, HELLO, 5))
            st |= ST_FD;
    } else if (tr->code == CODE_NO_FLAG) {
        const struct bbo *p = (const struct bbo *)(data + offs[0]);
        if (same((const u8 *)(size_t)p->buffer, CHILD, 16))
            st |= ST_CHILD;
    } else if (tr->code == CODE_TWO_CHILDREN) {
        const struct bbo *p = (const struct bbo *)(data + offs[0]);
        const struct bbo *c1 = (const struct bbo *)(data + offs[1]);
        const struct bbo *c2 = (const struct bbo *)(data + offs[2]);
        const u8 *pb = (const u8 *)(size_t)p->buffer;
        const u8 *c1b = (const u8 *)(size_t)c1->buffer;
        const u8 *c2b = (const u8 *)(size_t)c2->buffer;
        if (ld64(pb) == (u64)(size_t)c1b)
            st |= ST_POINTER;
        if (same(c1b, CHILD, 16))
            st |= ST_CHILD;
        if (same(pb + 16, PARENT, 8))
            st |= ST_PARENT;
        if (ld64(pb + 8) == (u64)(size_t)c2b && same(c2b, SECOND, 16))
            st |= ST_SECOND;
    }
    reply_status(fd, tr, st);
    return st;
}

/* Parses the read buffer: each record is a command, then its data. Returns 1 for a record (its
 * length in *len), 0 at the end of the used bytes, or -1 for a command this test does not expect. */
static int next_record(const u8 *buf, u64 used, u64 pos, u32 *cmd, u64 *len)
{
    if (pos + 4 > used)
        return 0;
    *cmd = ld32(buf + pos);
    switch (*cmd) {
    case BR_NOOP:
    case BR_TRANSACTION_COMPLETE:
    case BR_SPAWN_LOOPER:
    case BR_DEAD_REPLY:
    case BR_FAILED_REPLY:
        *len = 4;
        return 1;
    case BR_ERROR:
        *len = 8;
        return 1;
    case BR_TRANSACTION:
    case BR_REPLY:
        *len = 4 + 64;
        return 1;
    default:
        return -1;
    }
}

/* The server: the context manager. Answers each request until CODE_DONE. */
static void server(int sync_w)
{
    int fd = binder_open_map();
    if (fd < 0) {
        put_fail("server open", fd);
        sys_exit_group(1);
    }
    /* The manager node accepts fds (FLAT_BINDER_FLAG_ACCEPTS_FDS): the driver refuses fds for a
     * target node that does not (binder_translate_fd). */
    struct flat_obj {
        u32 type, flags;
        u64 binder, cookie;
    } fbo = {0, FLAT_BINDER_FLAG_ACCEPTS_FDS, 0, 0};
    i64 rc = sc_ioctl(fd, BINDER_SET_CONTEXT_MGR_EXT, &fbo);
    if (rc < 0) {
        put_fail("set context manager", rc);
        sys_exit_group(1);
    }
    sys_write(sync_w, "r", 1);
    u32 enter = BC_ENTER_LOOPER;
    write_read(fd, &enter, 4, NULL, 0, NULL);
    static u8 rbuf[1024] __attribute__((aligned(8)));
    for (;;) {
        u64 used = 0;
        rc = write_read(fd, NULL, 0, rbuf, sizeof rbuf, &used);
        if (rc < 0) {
            put_fail("server read", rc);
            sys_exit_group(1);
        }
        u64 pos = 0;
        u32 cmd = 0;
        u64 len = 0;
        int k;
        while ((k = next_record(rbuf, used, pos, &cmd, &len)) == 1) {
            if (cmd == BR_TRANSACTION) {
                struct btd tr;
                memcpy(&tr, rbuf + pos + 4, sizeof tr);
                if (tr.code == CODE_DONE) {
                    reply_status(fd, &tr, 0);
                    sys_exit_group(0);
                }
                if (tr.code == CODE_REPLY_EMBEDDED) {
                    /* The reply carries the embedded shape (case 7), built in the server. */
                    static u8 pbuf[32] __attribute__((aligned(8)));
                    static u8 cbuf[16] __attribute__((aligned(8)));
                    static u8 data[88] __attribute__((aligned(8)));
                    static u64 offs[2];
                    memset(pbuf, 0, sizeof pbuf);
                    memset(cbuf, 0, sizeof cbuf);
                    memcpy(pbuf, PARENT, 8);
                    memcpy(cbuf, REPLY_CHILD, 16);
                    u64 slot = (u64)(size_t)cbuf;
                    memcpy(pbuf + 8, &slot, 8);
                    memset(data, 0, sizeof data);
                    struct bbo o0 = {OBJ_PTR, 0, (u64)(size_t)pbuf, 32, 0, 0};
                    struct bbo o1 = {OBJ_PTR, FLAG_HAS_PARENT, (u64)(size_t)cbuf, 16, 0, 8};
                    memcpy(data + 8, &o0, sizeof o0);
                    memcpy(data + 48, &o1, sizeof o1);
                    offs[0] = 8;
                    offs[1] = 48;
                    u8 w[8 + 4 + 72];
                    u64 n = 0;
                    u32 wc = BC_FREE_BUFFER;
                    memcpy(w + n, &wc, 4);
                    n += 4;
                    memcpy(w + n, &tr.buffer, 8);
                    n += 8;
                    wc = BC_REPLY_SG;
                    memcpy(w + n, &wc, 4);
                    n += 4;
                    struct btd_sg r;
                    memset(&r, 0, sizeof r);
                    r.t.buffer = (u64)(size_t)data;
                    r.t.data_size = 88;
                    r.t.offsets = (u64)(size_t)offs;
                    r.t.offsets_size = 16;
                    r.buffers_size = 48;
                    memcpy(w + n, &r, sizeof r);
                    n += sizeof r;
                    write_read(fd, w, n, NULL, 0, NULL);
                } else {
                    check_request(fd, &tr);
                }
            }
            pos += len;
        }
        if (k < 0) {
            put("binder-sg: server: unexpected command "); putx(cmd); put("\n");
            sys_exit_group(2);
        }
    }
}

/* Sends one request and waits for its reply. Returns 1 with *rtr set for BR_REPLY, 2 for
 * BR_FAILED_REPLY, 3 for BR_DEAD_REPLY. */
static int request(int fd, u32 code, const u8 *data, u64 data_size, const u64 *offs, u64 offs_size,
                   u64 bufs_size, struct btd *rtr)
{
    u8 w[4 + 72];
    u32 cmd = BC_TRANSACTION_SG;
    memcpy(w, &cmd, 4);
    struct btd_sg t;
    memset(&t, 0, sizeof t);
    t.t.target = 0;
    t.t.code = code;
    t.t.flags = TF_ACCEPT_FDS;
    t.t.data_size = data_size;
    t.t.offsets_size = offs_size;
    t.t.buffer = (u64)(size_t)data;
    t.t.offsets = (u64)(size_t)offs;
    t.buffers_size = bufs_size;
    memcpy(w + 4, &t, sizeof t);
    i64 rc = write_read(fd, w, sizeof w, NULL, 0, NULL);
    if (rc < 0) {
        put_fail("transaction", rc);
        sys_exit_group(1);
    }
    static u8 rbuf[1024] __attribute__((aligned(8)));
    for (int tries = 0; tries < 16; tries++) {
        u64 used = 0;
        rc = write_read(fd, NULL, 0, rbuf, sizeof rbuf, &used);
        if (rc < 0) {
            put_fail("client read", rc);
            sys_exit_group(1);
        }
        u64 pos = 0;
        u32 rcmd = 0;
        u64 len = 0;
        int k;
        while ((k = next_record(rbuf, used, pos, &rcmd, &len)) == 1) {
            if (rcmd == BR_REPLY) {
                memcpy(rtr, rbuf + pos + 4, sizeof *rtr);
                return 1;
            }
            if (rcmd == BR_FAILED_REPLY)
                return 2;
            if (rcmd == BR_DEAD_REPLY)
                return 3;
            pos += len;
        }
        if (k < 0) {
            put("binder-sg: client: unexpected command "); putx(rcmd); put("\n");
            sys_exit_group(2);
        }
    }
    put("binder-sg: client: no reply\n");
    sys_exit_group(2);
    return 0;
}

static void free_reply(int fd, const struct btd *rtr)
{
    u8 w[12];
    u32 cmd = BC_FREE_BUFFER;
    memcpy(w, &cmd, 4);
    memcpy(w + 4, &rtr->buffer, 8);
    write_read(fd, w, sizeof w, NULL, 0, NULL);
}

static void put_bit(const char *name, u32 st, u32 bit)
{
    put(name);
    put(" ");
    putu((st & bit) ? 1 : 0);
}

/* Prints a case whose reply is a status word (the server's check). */
static void report_status(const char *name, int kind, const struct btd *rtr, int fd)
{
    put(name);
    if (kind == 1) {
        u32 st = ld32((const u8 *)(size_t)rtr->buffer);
        put(": reply status ");
        putu(st);
        put(" (");
        put_bit("pointer", st, ST_POINTER);
        put(", ");
        put_bit("child", st, ST_CHILD);
        put(", ");
        put_bit("parent", st, ST_PARENT);
        put(", ");
        put_bit("fd", st, ST_FD);
        put(", ");
        put_bit("second", st, ST_SECOND);
        put(")\n");
        free_reply(fd, rtr);
    } else if (kind == 2) {
        put(": BR_FAILED_REPLY\n");
    } else {
        put(": BR_DEAD_REPLY\n");
    }
}

/* Case 7: the reply's objects, checked the way libhwbinder checks a read buffer. */
static void report_reply_embedded(int fd, int kind, const struct btd *rtr)
{
    put("reply-embedded");
    if (kind != 1) {
        put(": not a reply\n");
        return;
    }
    const u8 *data = (const u8 *)(size_t)rtr->buffer;
    const u64 *offs = (const u64 *)(size_t)rtr->offsets;
    const struct bbo *p = (const struct bbo *)(data + offs[0]);
    const struct bbo *c = (const struct bbo *)(data + offs[1]);
    const u8 *pb = (const u8 *)(size_t)p->buffer;
    const u8 *cb = (const u8 *)(size_t)c->buffer;
    int pointer = ld64(pb + c->parent_offset) == (u64)(size_t)cb;
    int payload = same(cb, REPLY_CHILD, 16);
    int parent = same(pb, PARENT, 8);
    put(": pointer ");
    putu(pointer);
    put(" child ");
    putu(payload);
    put(" parent ");
    putu(parent);
    put("\n");
    free_reply(fd, rtr);
}

/* The client's requests. Each one is a case of the test. Data offset 0 is a pad word. */
static void client(int fd)
{
    struct btd rtr;
    int kind;
    static u8 pbuf[32] __attribute__((aligned(8)));
    static u8 cbuf[16] __attribute__((aligned(8)));
    static u8 c2buf[16] __attribute__((aligned(8)));
    static u8 pbuf2[16] __attribute__((aligned(8)));
    static u8 pbuf3[32] __attribute__((aligned(8)));
    static u8 data[256] __attribute__((aligned(8)));
    static u64 offs[4];
    int pfd[2];

    memset(pbuf, 0, sizeof pbuf);
    memset(cbuf, 0, sizeof cbuf);
    memset(pbuf2, 0, sizeof pbuf2);
    memset(pbuf3, 0, sizeof pbuf3);
    memset(c2buf, 0, sizeof c2buf);
    memcpy(pbuf, PARENT, 8);
    memcpy(cbuf, CHILD, 16);
    memcpy(c2buf, SECOND, 16);
    memcpy(pbuf3, PARENT, 8);
    u64 slot = (u64)(size_t)cbuf;
    memcpy(pbuf + 8, &slot, 8);

    /* 1. A parent PTR (at data offset 8) and a child with BINDER_BUFFER_FLAG_HAS_PARENT. */
    memset(data, 0, sizeof data);
    struct bbo o0 = {OBJ_PTR, 0, (u64)(size_t)pbuf, 32, 0, 0};
    struct bbo o1 = {OBJ_PTR, FLAG_HAS_PARENT, (u64)(size_t)cbuf, 16, 0, 8};
    memcpy(data + 8, &o0, sizeof o0);
    memcpy(data + 48, &o1, sizeof o1);
    offs[0] = 8;
    offs[1] = 48;
    kind = request(fd, CODE_PARENT_EMBEDDED, data, 88, offs, 16, 48, &rtr);
    report_status("parent-embedded", kind, &rtr, fd);

    /* 2. A parent PTR and a BINDER_TYPE_FDA in it (one fd: a pipe's read end, holding "hello"). */
    if (sc6(SYS_PIPE2, (i64)pfd, 0, 0, 0, 0, 0) < 0) {
        put("binder-sg: pipe failed\n");
        sys_exit_group(1);
    }
    sys_write(pfd[1], HELLO, 5);
    memset(pbuf2, 0, sizeof pbuf2);
    memcpy(pbuf2, PARENT, 8);
    u32 fd32 = (u32)pfd[0];
    memcpy(pbuf2 + 8, &fd32, 4);
    memset(data, 0, sizeof data);
    struct bbo f0 = {OBJ_PTR, 0, (u64)(size_t)pbuf2, 16, 0, 0};
    struct bfda f1 = {OBJ_FDA, 0, 1, 0, 8};
    memcpy(data + 8, &f0, sizeof f0);
    memcpy(data + 48, &f1, sizeof f1);
    offs[0] = 8;
    offs[1] = 48;
    kind = request(fd, CODE_FDA, data, 80, offs, 16, 16, &rtr);
    report_status("fda", kind, &rtr, fd);

    /* 3. The child's parent is itself (not an earlier object). */
    memset(data, 0, sizeof data);
    struct bbo g0 = {OBJ_PTR, 0, (u64)(size_t)pbuf, 32, 0, 0};
    struct bbo g1 = {OBJ_PTR, FLAG_HAS_PARENT, (u64)(size_t)cbuf, 16, 1, 8};
    memcpy(data + 8, &g0, sizeof g0);
    memcpy(data + 48, &g1, sizeof g1);
    offs[0] = 8;
    offs[1] = 48;
    kind = request(fd, CODE_FORWARD_PARENT, data, 88, offs, 16, 48, &rtr);
    report_status("forward-parent", kind, &rtr, fd);

    /* 4. The child's slot runs past the end of its parent (offset 12 in 16 bytes). */
    memset(data, 0, sizeof data);
    struct bbo h0 = {OBJ_PTR, 0, (u64)(size_t)pbuf2, 16, 0, 0};
    struct bbo h1 = {OBJ_PTR, FLAG_HAS_PARENT, (u64)(size_t)cbuf, 16, 0, 12};
    memcpy(data + 8, &h0, sizeof h0);
    memcpy(data + 48, &h1, sizeof h1);
    kind = request(fd, CODE_OFFSET_OUT, data, 88, offs, 16, 32, &rtr);
    report_status("offset-out", kind, &rtr, fd);

    /* 5. The parent is a BINDER_TYPE_FD object, not a buffer. */
    memset(data, 0, sizeof data);
    struct bfd n0 = {OBJ_FD, 0, (u64)pfd[0], 0};
    struct bbo n1 = {OBJ_PTR, FLAG_HAS_PARENT, (u64)(size_t)cbuf, 16, 0, 0};
    memcpy(data + 8, &n0, sizeof n0);
    memcpy(data + 32, &n1, sizeof n1);
    offs[0] = 8;
    offs[1] = 32;
    kind = request(fd, CODE_NONPTR_PARENT, data, 72, offs, 16, 16, &rtr);
    report_status("nonptr-parent", kind, &rtr, fd);

    /* 6. A PTR with the flag clear: its parent fields are ignored. */
    memset(data, 0, sizeof data);
    struct bbo q0 = {OBJ_PTR, 0, (u64)(size_t)cbuf, 16, 7, 3};
    memcpy(data + 8, &q0, sizeof q0);
    offs[0] = 8;
    kind = request(fd, CODE_NO_FLAG, data, 48, offs, 8, 16, &rtr);
    report_status("no-flag", kind, &rtr, fd);

    /* 7. The server's reply carries the embedded shape (BC_REPLY_SG). */
    memset(data, 0, sizeof data);
    kind = request(fd, CODE_REPLY_EMBEDDED, data, 4, offs, 0, 0, &rtr);
    report_reply_embedded(fd, kind, &rtr);

    /* 8. Two children of one parent, at increasing parent offsets (0, then 8): in order. */
    /* The parent's slots: 0 for the first child, 8 for the second; its payload is at 16. */
    memset(data, 0, sizeof data);
    memset(pbuf3, 0, sizeof pbuf3);
    memcpy(pbuf3 + 16, PARENT, 8);
    u64 slot1 = (u64)(size_t)cbuf;
    u64 slot2 = (u64)(size_t)c2buf;
    memcpy(pbuf3, &slot1, 8);
    memcpy(pbuf3 + 8, &slot2, 8);
    struct bbo t0 = {OBJ_PTR, 0, (u64)(size_t)pbuf3, 32, 0, 0};
    struct bbo t1 = {OBJ_PTR, FLAG_HAS_PARENT, (u64)(size_t)cbuf, 16, 0, 0};
    struct bbo t2 = {OBJ_PTR, FLAG_HAS_PARENT, (u64)(size_t)c2buf, 16, 0, 8};
    memcpy(data + 8, &t0, sizeof t0);
    memcpy(data + 48, &t1, sizeof t1);
    memcpy(data + 88, &t2, sizeof t2);
    offs[0] = 8;
    offs[1] = 48;
    offs[2] = 88;
    kind = request(fd, CODE_TWO_CHILDREN, data, 128, offs, 24, 64, &rtr);
    report_status("two-children", kind, &rtr, fd);

    /* 9. The same two children with their parent offsets out of order (8, then 0). */
    memset(data, 0, sizeof data);
    struct bbo u1 = {OBJ_PTR, FLAG_HAS_PARENT, (u64)(size_t)cbuf, 16, 0, 8};
    struct bbo u2 = {OBJ_PTR, FLAG_HAS_PARENT, (u64)(size_t)c2buf, 16, 0, 0};
    memcpy(data + 8, &t0, sizeof t0);
    memcpy(data + 48, &u1, sizeof u1);
    memcpy(data + 88, &u2, sizeof u2);
    kind = request(fd, CODE_OUT_OF_ORDER, data, 128, offs, 24, 64, &rtr);
    report_status("out-of-order", kind, &rtr, fd);

    /* Done: the server replies and exits. */
    memset(data, 0, sizeof data);
    kind = request(fd, CODE_DONE, data, 4, offs, 0, 0, &rtr);
    report_status("done", kind, &rtr, fd);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int sp[2];
    if (sc6(SYS_PIPE2, (i64)sp, 0, 0, 0, 0, 0) < 0) {
        put("binder-sg: pipe failed\n");
        return 1;
    }
    i64 pid = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (pid < 0) {
        put("binder-sg: fork failed\n");
        return 1;
    }
    if (pid == 0) {
        server(sp[1]);
        sys_exit_group(3);
    }
    /* The client waits until the server is the context manager. */
    char ready;
    if (sc_read(sp[0], &ready, 1) != 1) {
        put("binder-sg: server did not start\n");
        return 1;
    }
    int fd = binder_open_map();
    if (fd < 0) {
        put_fail("client open", fd);
        return 1;
    }
    client(fd);
    int status = 0;
    i64 w = sc6(SYS_WAIT4, pid, (i64)&status, 0, 0, 0, 0);
    put("server: ");
    putd(w == pid ? (status >> 8) & 0xff : -1);
    put("\n");
    return 0;
}
