/* Pipes (B1 step 2): one process with two threads. The writer thread sends text
 * with a pause (so the reader blocks on an empty pipe), then one 200000-byte write
 * (more than the 64 KiB ring, so it blocks until the reader drains it), then closes
 * the write end. The main thread reads to end of file. Also checked: O_CLOEXEC and
 * O_NONBLOCK flags, EAGAIN on an empty and on a full non-blocking pipe, and EPIPE
 * with SIGPIPE ignored. Results are printed after the join, so the output does not
 * depend on the interleaving. */
#include "sys.h"

#define SYS_CLOSE 57
#define SYS_PIPE2 59
#define SYS_READ 63
#define SYS_WRITEV 66
#define SYS_READV 65
#define SYS_FCNTL 25
#define SYS_FUTEX 98
#define SYS_NANOSLEEP 101
#define SYS_RT_SIGACTION 134

#define O_CLOEXEC 02000000
#define O_NONBLOCK 04000
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
#define SIGPIPE 13
#define FUTEX_WAIT 0
#define FUTEX_PRIVATE 128

#define BIG 200000
#define STACK_BYTES 65536
/* clone flags: VM, FS, FILES, SIGHAND, THREAD, SYSVSEM, PARENT_SETTID, CHILD_CLEARTID. */
#define THREAD_FLAGS 0x350F00ull

extern i64 xp_spawn(u64 flags, void *stack_top, u32 *ptid, u32 *ctid, void (*fn)(void *), void *arg);
__asm__(".text\n"
        ".global xp_spawn\n"
        "xp_spawn:\n"
        "  stp x4, x5, [x1, #-16]!\n"
        "  mov x4, x3\n"
        "  mov x3, xzr\n"
        "  mov x8, #220\n"
        "  svc #0\n"
        "  cbnz x0, 1f\n"
        "  ldp x9, x0, [sp]\n"
        "  blr x9\n"
        "  mov x0, #0\n"
        "  mov x8, #93\n"
        "  svc #0\n"
        "1:\n"
        "  ret\n");

static int fd_rd, fd_wr;        /* the data pipe */
static u32 child_tid;           /* cleared by the kernel when the writer exits */
static unsigned char stack[STACK_BYTES] __attribute__((aligned(16)));
static u8 buf[4096];
static i64 write_rc;  /* the writer's big write: bytes it returned */
static i64 writev_rc; /* the writer's writev of two segments */

static i64 fcntl(int fd, i64 cmd, i64 arg) { return sc6(SYS_FCNTL, fd, cmd, arg, 0, 0, 0); }
static i64 pread_(int fd, void *p, u64 n) { return sc6(SYS_READ, fd, (i64)p, (i64)n, 0, 0, 0); }
static i64 pwrite_(int fd, const void *p, u64 n) { return sys_write(fd, p, n); }
static i64 close_(int fd) { return sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0); }

static u8 pattern(u64 i) { return (u8)((i * 7 + 3) & 0xFF); }

static void writer(void *arg)
{
    (void)arg;
    pwrite_(fd_wr, "hello ", 6);
    const i64 nap[2] = {0, 2000000}; /* 2 ms: the reader blocks in between */
    sc6(SYS_NANOSLEEP, (i64)nap, 0, 0, 0, 0, 0);
    pwrite_(fd_wr, "world", 5);

    static u8 big[BIG];
    for (u64 i = 0; i < BIG; i++)
        big[i] = pattern(i);
    i64 w = pwrite_(fd_wr, big, BIG); /* blocks until the reader has made room */
    write_rc = w;

    /* writev of two segments (70000 bytes of 'W'): a block can fall between them. */
    static u8 wseg[70000];
    for (u64 i = 0; i < sizeof wseg; i++)
        wseg[i] = 'W';
    const u64 iov[4] = {(u64)wseg, 30000, (u64)(wseg + 30000), 40000};
    writev_rc = sc6(SYS_WRITEV, fd_wr, (i64)iov, 2, 0, 0, 0);
    close_(fd_wr);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int fds[2];
    i64 rc = sc6(SYS_PIPE2, (i64)fds, O_CLOEXEC, 0, 0, 0, 0);
    put("pipes: pipe2 cloexec rc=");
    putd(rc);
    put("\n");
    fd_rd = fds[0];
    fd_wr = fds[1];
    put("pipes: fd cloexec read=");
    putd(fcntl(fd_rd, F_GETFD, 0));
    put(" write=");
    putd(fcntl(fd_wr, F_GETFD, 0));
    put("\n");
    put("pipes: access read=");
    putd(fcntl(fd_rd, F_GETFL, 0) & 3);
    put(" write=");
    putd(fcntl(fd_wr, F_GETFL, 0) & 3);
    put("\n");
    fcntl(fd_rd, F_SETFD, 0);
    put("pipes: cloexec cleared=");
    putd(fcntl(fd_rd, F_GETFD, 0));
    put("\n");

    /* Empty pipe, non-blocking read end: EAGAIN. */
    fcntl(fd_rd, F_SETFL, O_NONBLOCK);
    put("pipes: empty nonblock read=");
    putd(pread_(fd_rd, buf, sizeof buf));
    put(" nonblock flag=");
    putd((fcntl(fd_rd, F_GETFL, 0) & O_NONBLOCK) != 0);
    put("\n");
    fcntl(fd_rd, F_SETFL, 0);

    /* Writer thread, then the main thread reads until end of file. */
    if (xp_spawn(THREAD_FLAGS, stack + STACK_BYTES, &child_tid, &child_tid, writer, 0) <= 0) {
        put("pipes: clone failed\n");
        return 1;
    }
    /* One process, so the threads share one fd table: the write end is the writer's
     * alone to close, and the reader sees end of file when it does. */

    u64 total = 0, sum = 0, bad = 0;
    int head_ok = 1;
    static const char head[] = "hello world";
    for (;;) {
        /* readv with a 3-byte segment first: the rest of the buffer is the second one. */
        const u64 riov[4] = {(u64)buf, 3, (u64)(buf + 3), sizeof buf - 3};
        i64 n = sc6(SYS_READV, fd_rd, (i64)riov, 2, 0, 0, 0);
        if (n < 0) {
            put("pipes: read error ");
            putd(n);
            put("\n");
            return 1;
        }
        if (n == 0)
            break;
        for (i64 k = 0; k < n; k++) {
            u64 pos = total + (u64)k; /* "hello world", the pattern, then the 'W's */
            if (pos < 11) {
                if (buf[k] != (u8)head[pos])
                    head_ok = 0;
            } else if (pos < 11 + BIG) {
                if (buf[k] != pattern(pos - 11))
                    bad++;
                sum += buf[k];
            } else {
                if (buf[k] != 'W')
                    bad++;
                sum += buf[k];
            }
        }
        total += (u64)n;
    }
    close_(fd_rd);
    put("pipes: eof after bytes=");
    putu(total);
    put(" text=");
    put(head_ok ? "ok" : "bad");
    put(" pattern_bad=");
    putu(bad);
    put(" sum=");
    putu(sum);
    put("\n");

    /* Wait for the writer thread to exit (CLEARTID), then check its results. */
    for (;;) {
        u32 t = __atomic_load_n(&child_tid, __ATOMIC_ACQUIRE);
        if (t == 0)
            break;
        sc6(SYS_FUTEX, (i64)&child_tid, FUTEX_WAIT | FUTEX_PRIVATE, t, 0, 0, 0);
    }
    put("pipes: blocked write returned ");
    putd(write_rc);
    put(" writev returned ");
    putd(writev_rc);
    put("\n");

    /* Full non-blocking write end: 4 KiB writes until EAGAIN (16 fit in the ring). */
    int fds2[2];
    sc6(SYS_PIPE2, (i64)fds2, 0, 0, 0, 0, 0);
    fcntl(fds2[1], F_SETFL, O_NONBLOCK);
    u64 fits = 0;
    while (pwrite_(fds2[1], buf, sizeof buf) == (i64)sizeof buf)
        fits++;
    put("pipes: full nonblock writes=");
    putu(fits);
    put(" last=");
    putd(pwrite_(fds2[1], "x", 1));
    put("\n");
    close_(fds2[0]);
    put("pipes: no reader write=");
    /* SIGPIPE ignored: the write fails with EPIPE. */
    const u64 ign[4] = {1, 0, 0, 0}; /* SIG_IGN */
    sc6(SYS_RT_SIGACTION, SIGPIPE, (i64)ign, 0, 8, 0, 0);
    putd(pwrite_(fds2[1], "y", 1));
    put("\n");
    close_(fds2[1]);
    put("pipes: done\n");
    return 0;
}
