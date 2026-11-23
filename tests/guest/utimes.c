/* utimensat (88) and futimens (utimensat with a NULL path): explicit times, UTIME_OMIT on one of
 * them, UTIME_NOW and the NULL form (checked against the clock), both OMIT (a no-op that does not
 * look at the path), EINVAL for a bad nanosecond field, ENOENT, and AT_EMPTY_PATH on a descriptor.
 * The times are read back with newfstatat and fstat (arm64 struct stat offsets). Only the owner
 * case is checked: the native run is not root, so no permission case can be compared with it. */
#include "sys.h"

#define SYS_CLOSE 57
#define SYS_OPENAT 56
#define SYS_WRITE 64
#define SYS_NEWFSTATAT 79
#define SYS_FSTAT 80
#define SYS_UTIMENSAT 88
#define SYS_CLOCK_GETTIME_ 113

#define AT_FDCWD (-100)
#define AT_EMPTY_PATH 0x1000
#define UTIME_NOW ((1ll << 30) - 1)
#define UTIME_OMIT ((1ll << 30) - 2)
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000

static const char path[] = "/tmp/xiaoji-utimes-test";

static void put_ts(const char *label, const u64 *st)
{
    /* st: the stat buffer; atime at byte 72, mtime at byte 88. Whole seconds only: the Node build (NODERAWFS)
     * keeps no nanoseconds, so the nanosecond fields are not compared. */
    put(label);
    put(" atime=");
    putd((i64)st[72 / 8]);
    put(" mtime=");
    putd((i64)st[88 / 8]);
    put("\n");
}

static void show_path(const char *label)
{
    u64 st[16];
    memset(st, 0, sizeof st);
    i64 rc = sc6(SYS_NEWFSTATAT, AT_FDCWD, (i64)(u64)path, (i64)(u64)st, 0, 0, 0);
    if (rc) {
        put(label);
        put(" stat failed\n");
        return;
    }
    put_ts(label, st);
}

static void show_fd(const char *label, int fd)
{
    u64 st[16];
    memset(st, 0, sizeof st);
    sc6(SYS_FSTAT, fd, (i64)(u64)st, 0, 0, 0, 0);
    put_ts(label, st);
}

static i64 utimes_(i64 dirfd, const char *p, i64 a, i64 an, i64 b, i64 bn, i64 flags)
{
    i64 t[4] = { a, an, b, bn };
    return sc6(SYS_UTIMENSAT, dirfd, (i64)(u64)p, (i64)(u64)t, flags, 0, 0);
}

static void now_seconds(i64 *s, i64 *ns)
{
    i64 ts[2];
    sc6(SYS_CLOCK_GETTIME_, 0, (i64)(u64)ts, 0, 0, 0, 0);
    *s = ts[0];
    *ns = ts[1];
}

/* The time is within the two clock readings (whole seconds, as put_ts prints them). */
static int in_window(const u64 *st, int which, i64 s0, i64 n0, i64 s1, i64 n1)
{
    i64 s = (i64)st[(which ? 88 : 72) / 8];
    (void)n0;
    (void)n1;
    return s >= s0 && s <= s1;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int fd = (int)sc6(SYS_OPENAT, AT_FDCWD, (i64)(u64)path, O_RDWR | O_CREAT | O_TRUNC, 0644, 0, 0);
    sc6(SYS_WRITE, fd, (i64)(u64)"x", 1, 0, 0, 0);

    /* Explicit times, both set. */
    put("set rc=");
    putd(utimes_(AT_FDCWD, path, 1000000000, 0, 1100000000, 500, 0));
    put("\n");
    show_path("A");

    /* UTIME_OMIT leaves atime alone; mtime is set. */
    put("omit rc=");
    putd(utimes_(AT_FDCWD, path, 0, UTIME_OMIT, 1200000000, 7, 0));
    put("\n");
    show_path("B");

    /* futimens (a NULL path) on the descriptor. */
    i64 fut[4] = { 1300000000, 1, 1300000000, 2 };
    put("futimens rc=");
    putd(sc6(SYS_UTIMENSAT, fd, 0, (i64)(u64)fut, 0, 0, 0));
    put("\n");
    show_fd("C", fd);

    /* Both OMIT: a no-op, which does not look at the path (a missing one is not ENOENT). */
    put("both omit, missing path rc=");
    putd(utimes_(AT_FDCWD, "/tmp/xiaoji-no-such-file", 0, UTIME_OMIT, 0, UTIME_OMIT, 0));
    put("\n");

    /* Bad nanoseconds: EINVAL; a missing file: ENOENT. */
    put("nsec 1e9 rc=");
    putd(utimes_(AT_FDCWD, path, 5, 1000000000, 5, 0, 0));
    put("\n");
    put("nsec -1 rc=");
    putd(utimes_(AT_FDCWD, path, 5, -1, 5, 0, 0));
    put("\n");
    put("missing file rc=");
    putd(utimes_(AT_FDCWD, "/tmp/xiaoji-no-such-file", 5, 0, 5, 0, 0));
    put("\n");

    /* UTIME_NOW on both times, and the NULL form: the time is between two clock readings. */
    i64 s0, n0, s1, n1;
    now_seconds(&s0, &n0);
    put("now rc=");
    putd(utimes_(AT_FDCWD, path, 0, UTIME_NOW, 0, UTIME_NOW, 0));
    put("\n");
    now_seconds(&s1, &n1);
    u64 st[16];
    memset(st, 0, sizeof st);
    sc6(SYS_NEWFSTATAT, AT_FDCWD, (i64)(u64)path, (i64)(u64)st, 0, 0, 0);
    put("now in window: ");
    putd(in_window(st, 0, s0, n0, s1, n1) && in_window(st, 1, s0, n0, s1, n1));
    put("\n");

    now_seconds(&s0, &n0);
    put("null times rc=");
    putd(sc6(SYS_UTIMENSAT, AT_FDCWD, (i64)(u64)path, 0, 0, 0, 0));
    put("\n");
    now_seconds(&s1, &n1);
    memset(st, 0, sizeof st);
    sc6(SYS_NEWFSTATAT, AT_FDCWD, (i64)(u64)path, (i64)(u64)st, 0, 0, 0);
    put("null in window: ");
    putd(in_window(st, 0, s0, n0, s1, n1) && in_window(st, 1, s0, n0, s1, n1));
    put("\n");

    /* AT_EMPTY_PATH with a descriptor. */
    i64 em[4] = { 1400000000, 0, 1400000000, 0 };
    put("empty path rc=");
    putd(sc6(SYS_UTIMENSAT, fd, (i64)(u64)"", (i64)(u64)em, AT_EMPTY_PATH, 0, 0));
    put("\n");
    show_fd("D", fd);

    sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    put("done\n");
    return 0;
}
