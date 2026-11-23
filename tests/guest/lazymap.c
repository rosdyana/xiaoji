/* Lazy file mappings (argv[1] is a scratch directory). The file spans three 64 KiB windows
 * and a partial page. The parent touches the first window only; the fork child reads and
 * writes the third; a SIGBUS child touches the fourth window, past the end of the file. Also:
 * a private view with a write and madvise(DONTNEED), a shared read-only view, mprotect, and
 * an unhandled SIGBUS. Every line is checked against the file's bytes, so the native oracle
 * and the engine print the same lines. */
#include "sys.h"

#define SYS_UNLINKAT 35
#define SYS_MKDIRAT 34
#define SYS_PREAD64 67
#define SYS_PWRITE64 68
#define SYS_MADVISE 233

#define AT_FDCWD (-100)
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_NONE 0
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MADV_DONTNEED 4
#define SIGBUS_N 7
#define SIGCHLD_N 17
#define SA_SIGINFO 0x4ull
#define SA_RESTORER 0x04000000ull
#define BUS_ADRERR 2

#define PAGE 4096u
#define WIN 65536u                          /* the default lazy window */
#define FILE_SIZE (3 * WIN + 100)           /* three windows and a partial page */
#define PRIV_LEN (3 * WIN + PAGE)           /* the file's pages, the last one partial */
#define BUS_SPAN (4 * WIN)                  /* the fourth window is wholly past the end */

typedef struct {
    u64 handler, flags, restorer, mask;
} ksigaction;

extern void lz_restore(void);
__asm__(".text\n"
        ".global lz_restore\n"
        "lz_restore:\n"
        "  mov x8, #139\n"
        "  svc #0\n");

static u8 file_bytes[FILE_SIZE];
static volatile u64 bus_base; /* the SIGBUS mapping, set before the fork */

static u8 pattern(u64 i) { return (u8)(i * 7 + (i >> 16) * 13 + 3); }

static void report(const char *label, int ok)
{
    put(label);
    put(ok ? "=ok\n" : "=FAIL\n");
}

static void bus_handler(int sig, void *info, void *uc)
{
    (void)uc;
    int code = *(int *)((u8 *)info + 8);
    u64 addr = *(u64 *)((u8 *)info + 16);
    /* The child reports through its exit status: 0 if this is the expected fault. */
    int ok = sig == SIGBUS_N && code == BUS_ADRERR && addr >= bus_base && addr < bus_base + BUS_SPAN;
    sc6(94, ok ? 0 : 1, 0, 0, 0, 0, 0); /* exit_group */
}

static i64 sys_sigaction(int sig, ksigaction *sa) { return sc6(134, sig, (i64)sa, 0, 8, 0, 0); }

static i64 do_fork(void) { return sc6(220, SIGCHLD_N, 0, 0, 0, 0, 0); }

static i64 wait_for(i64 pid, i64 *st) { return sc6(260, pid, (i64)st, 0, 0, 0, 0); }

static int exited_ok(i64 st) { return (st & 0x7f) == 0 && ((st >> 8) & 0xff) == 0; }

int main(int argc, char **argv)
{
    int fails = 0;
    int ok;
    if (argc < 2) {
        put("usage: lazymap DIR\n");
        return 2;
    }
    char path[256];
    u64 n = 0;
    for (const char *p = argv[1]; *p && n < sizeof path - 16; p++)
        path[n++] = *p;
    for (const char *p = "/lazy.bin"; *p; p++)
        path[n++] = *p;
    path[n] = 0;
    sc6(SYS_MKDIRAT, AT_FDCWD, (i64)argv[1], 0755, 0, 0, 0);

    for (u64 i = 0; i < FILE_SIZE; i++)
        file_bytes[i] = pattern(i);
    i64 fd = sc6(56, AT_FDCWD, (i64)path, O_RDWR | O_CREAT | O_TRUNC, 0644, 0, 0);
    if (fd < 0) {
        put("open=FAIL\n");
        return 1;
    }
    ok = sc6(SYS_PWRITE64, fd, (i64)file_bytes, FILE_SIZE, 0, 0, 0) == FILE_SIZE;
    report("write file", ok);
    fails += !ok;

    /* Private view of the whole file. The parent touches only the first window. */
    u8 *priv = (u8 *)(u64)sys_mmap(0, PRIV_LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if ((i64)(u64)priv < 0) {
        report("mmap private", 0);
        return 1;
    }
    ok = priv[PAGE + 5] == pattern(PAGE + 5);
    report("private partial touch", ok);
    fails += !ok;
    ok = priv[WIN - 1] == pattern(WIN - 1) && priv[WIN] == pattern(WIN);
    report("private window edge", ok);
    fails += !ok;

    /* The partial page at the end of the file: the file's bytes, then zeros to the page end. */
    ok = priv[3 * WIN + 99] == pattern(3 * WIN + 99) && priv[3 * WIN + 100] == 0 && priv[3 * WIN + PAGE - 1] == 0;
    report("private partial last page", ok);
    fails += !ok;

    /* A write to a private page: visible in the mapping, not in the file. */
    priv[PAGE + 10] = 0x5a;
    u8 b = 0;
    sc6(SYS_PREAD64, fd, (i64)&b, 1, PAGE + 10, 0, 0);
    ok = priv[PAGE + 10] == 0x5a && b == pattern(PAGE + 10);
    report("private write isolated from file", ok);
    fails += !ok;

    /* A shared read-only view of the same file: the file's bytes, not the private write. */
    u8 *shr = (u8 *)(u64)sys_mmap(0, PRIV_LEN, PROT_READ, MAP_SHARED, fd, 0);
    ok = (i64)(u64)shr > 0 && shr[PAGE + 10] == pattern(PAGE + 10) && shr[2 * WIN + 4] == pattern(2 * WIN + 4);
    report("shared view reads the file", ok);
    fails += !ok;

    /* madvise(DONTNEED) drops the private copy: the page reads the file again. */
    sc6(SYS_MADVISE, (i64)(priv + PAGE), PAGE, MADV_DONTNEED, 0, 0, 0);
    ok = priv[PAGE + 10] == pattern(PAGE + 10);
    report("dontneed refills from file", ok);
    fails += !ok;

    /* mprotect to PROT_NONE and back: the page keeps its file contents. */
    sys_mprotect((u64)(u64)(priv + 2 * PAGE), PAGE, PROT_NONE);
    sys_mprotect((u64)(u64)(priv + 2 * PAGE), PAGE, PROT_READ | PROT_WRITE);
    ok = priv[2 * PAGE + 7] == pattern(2 * PAGE + 7);
    report("mprotect round trip", ok);
    fails += !ok;

    /* Fork after the touch. The child sees the private write, reads the third window (never
     * touched by the parent) from the file, and writes there; the parent keeps its bytes. */
    priv[PAGE + 10] = 0x5a; /* the write the child must see */
    i64 pid = do_fork();
    if (pid == 0) {
        int cok = priv[PAGE + 10] == 0x5a && priv[2 * WIN + 7] == pattern(2 * WIN + 7);
        priv[2 * WIN + 7] = 0x6b;
        sc6(94, cok ? 0 : 1, 0, 0, 0, 0, 0);
    }
    i64 st = 0;
    wait_for(pid, &st);
    ok = exited_ok(st);
    report("fork child reads and writes privately", ok);
    fails += !ok;
    ok = priv[2 * WIN + 7] == pattern(2 * WIN + 7);
    report("fork child write stays in child", ok);
    fails += !ok;

    /* SIGBUS for a page wholly past the end of the file, in a window that also holds the
     * partial page. The parent reads the partial page first, so the window is filled. */
    u8 *bus = (u8 *)(u64)sys_mmap(0, BUS_SPAN, PROT_READ, MAP_PRIVATE, fd, 0);
    ok = bus[3 * WIN + 99] == pattern(3 * WIN + 99) && bus[3 * WIN + 200] == 0;
    report("partial page reads without sigbus", ok);
    fails += !ok;
    bus_base = (u64)(u64)bus;
    pid = do_fork();
    if (pid == 0) {
        ksigaction sa = {(u64)(u64)bus_handler, SA_SIGINFO | SA_RESTORER, (u64)(u64)lz_restore, 0};
        sys_sigaction(SIGBUS_N, &sa);
        volatile u8 v = bus[50 * PAGE + 3];
        (void)v;
        sc6(94, 1, 0, 0, 0, 0, 0); /* not reached: the access faults */
    }
    st = 0;
    wait_for(pid, &st);
    ok = exited_ok(st);
    report("sigbus past eof, handled", ok);
    fails += !ok;

    /* The same access with no handler kills the process with SIGBUS. */
    pid = do_fork();
    if (pid == 0) {
        volatile u8 v = bus[50 * PAGE + 3];
        (void)v;
        sc6(94, 1, 0, 0, 0, 0, 0);
    }
    st = 0;
    wait_for(pid, &st);
    ok = (st & 0x7f) == SIGBUS_N;
    report("sigbus past eof, unhandled kills", ok);
    fails += !ok;

    ok = sys_munmap((u64)(u64)shr, PRIV_LEN) == 0;
    report("munmap shared", ok);
    fails += !ok;
    ok = sys_munmap((u64)(u64)priv, PRIV_LEN) == 0 && sys_munmap((u64)(u64)bus, BUS_SPAN) == 0;
    report("munmap private and sigbus views", ok);
    fails += !ok;
    sc6(57, fd, 0, 0, 0, 0, 0);
    sc6(SYS_UNLINKAT, AT_FDCWD, (i64)path, 0, 0, 0, 0);
    put(fails ? "lazymap: FAIL\n" : "lazymap: all ok\n");
    return fails ? 1 : 0;
}
