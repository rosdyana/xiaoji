/* fdnames (B4b): the readlink text of /proc/self/fd/N and the stat of each descriptor kind
 * that ART's fd allowlist reads (zygote's FileDescriptorTable). One program, output compared
 * with native. It opens, in a fixed order, a pipe, a socketpair, an unbound socket, an eventfd,
 * an epoll instance, a timerfd, a memfd and a dup of the pipe's read end, then lists
 * /proc/self/fd. A pipe or socket prints its link with the inode digits replaced by N, and
 * whether the inode in the link equals fstat's st_ino. Options:
 *   --stdio     also list descriptors 0 to 2 (the stdio of the process that runs it)
 *   --out PATH  write the report to PATH instead of descriptor 1 (for a service whose stdout
 *               is /dev/null, as under init)
 * Nothing printed depends on a pid or on a fd number other than the ones listed. */
#include <stdint.h>
#include "sys.h"

#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_GETDENTS64 61
#define SYS_PIPE2 59
#define SYS_DUP 23
#define SYS_READLINKAT 78
#define SYS_FSTAT 80
#define SYS_SOCKET 198
#define SYS_SOCKETPAIR 199
#define SYS_EVENTFD2 19
#define SYS_EPOLL_CREATE1 20
#define SYS_TIMERFD_CREATE 85
#define SYS_MEMFD_CREATE 279

#define AT_FDCWD (-100)
#define O_WRONLY 01
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_DIRECTORY 040000
#define S_IFMT 0170000
#define S_IFIFO 0010000
#define S_IFSOCK 0140000

static int outfd = 1;

static void emit(const char *s) { sys_write(outfd, s, strlen(s)); }

static void emitd(i64 v)
{
    char buf[24];
    int neg = v < 0;
    u64 u = neg ? (u64)0 - (u64)v : (u64)v;
    int i = (int)sizeof buf;
    buf[--i] = 0;
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg)
        buf[--i] = '-';
    emit(buf + i);
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* 1 when s starts with prefix. */
static int starts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix)
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

/* fstat of fd: st_ino and st_mode (the arm64 struct stat: ino at 8, mode at 16). */
static i64 fd_stat(int fd, u64 *ino, u32 *mode)
{
    u64 st[16];
    i64 rc = sc6(SYS_FSTAT, fd, (i64)st, 0, 0, 0, 0);
    if (rc < 0)
        return rc;
    *ino = st[1];
    *mode = ((u32 *)st)[4];
    return 0;
}

/* The decimal number at s; end receives the first character after it. */
static u64 parse_u(const char *s, const char **end)
{
    u64 v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u64)(*s - '0');
        s++;
    }
    *end = s;
    return v;
}

static void report_fd(int fd)
{
    char path[48];
    int n = 0;
    const char *pre = "/proc/self/fd/";
    while (pre[n]) {
        path[n] = pre[n];
        n++;
    }
    char num[24];
    int k = 0;
    u64 v = (u64)fd;
    do {
        num[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k > 0)
        path[n++] = num[--k];
    path[n] = 0;

    char link[256];
    i64 rc = sc6(SYS_READLINKAT, AT_FDCWD, (i64)path, (i64)link, sizeof link - 1, 0, 0);
    if (rc < 0) {
        emit("fd ");
        emitd(fd);
        emit(": readlink error\n");
        return;
    }
    link[rc] = 0;
    if (starts(link, "/proc/")) /* the directory being listed */
        return;

    emit("fd ");
    emitd(fd);
    emit(": ");
    if (starts(link, "pipe:[") || starts(link, "socket:[")) {
        /* The inode digits are replaced by N; the check is that they equal fstat's st_ino. */
        const char *lb = link;
        while (*lb != '[')
            lb++;
        const char *end;
        u64 li = parse_u(lb + 1, &end);
        u64 ino = 0;
        u32 mode = 0;
        int same = fd_stat(fd, &ino, &mode) == 0 && ino == li && *end == ']';
        emit(starts(link, "pipe:[") ? "pipe:[N]" : "socket:[N]");
        emit(same ? " ino=match" : " ino=differs");
        emit(" type=");
        emit((mode & S_IFMT) == S_IFIFO    ? "fifo"
             : (mode & S_IFMT) == S_IFSOCK ? "sock"
                                           : "other");
        emit("\n");
        return;
    }
    emit(link);
    emit("\n");
}

int main(int argc, char **argv)
{
    int with_stdio = 0;
    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "--stdio")) {
            with_stdio = 1;
        } else if (str_eq(argv[i], "--out") && i + 1 < argc) {
            i++;
            int fd = (int)sc6(SYS_OPENAT, AT_FDCWD, (i64)argv[i], O_WRONLY | O_CREAT | O_TRUNC, 0644, 0, 0);
            if (fd < 0)
                return 2;
            outfd = fd;
        }
    }

    int pfd[2] = {-1, -1};
    int sv[2] = {-1, -1};
    sc6(SYS_PIPE2, (i64)pfd, 0, 0, 0, 0, 0);
    sc6(SYS_SOCKETPAIR, 1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0, (i64)sv, 0, 0);
    sc6(SYS_SOCKET, 1, 1, 0, 0, 0, 0);
    sc6(SYS_EVENTFD2, 0, 0, 0, 0, 0, 0);
    sc6(SYS_EPOLL_CREATE1, 0, 0, 0, 0, 0, 0);
    sc6(SYS_TIMERFD_CREATE, 1 /* CLOCK_MONOTONIC */, 0, 0, 0, 0, 0);
    sc6(SYS_MEMFD_CREATE, (i64)"x", 0, 0, 0, 0, 0);
    sc6(SYS_DUP, pfd[0], 0, 0, 0, 0, 0);

    /* The open descriptors, in ascending order (the directory itself is skipped). */
    int dirfd = (int)sc6(SYS_OPENAT, AT_FDCWD, (i64)"/proc/self/fd", O_DIRECTORY, 0, 0, 0);
    static uint8_t dbuf[4096];
    int seen[128];
    int nseen = 0;
    for (;;) {
        i64 n = sc6(SYS_GETDENTS64, dirfd, (i64)dbuf, sizeof dbuf, 0, 0, 0);
        if (n <= 0)
            break;
        for (i64 off = 0; off < n;) {
            uint16_t reclen = (uint16_t)(dbuf[off + 16] | (dbuf[off + 17] << 8));
            const char *name = (const char *)(dbuf + off + 19);
            const char *end;
            u64 v = parse_u(name, &end);
            if (*end == 0 && name[0] && nseen < 128)
                seen[nseen++] = (int)v;
            off += reclen;
        }
    }
    for (int i = 1; i < nseen; i++)
        for (int j = i; j > 0 && seen[j - 1] > seen[j]; j--) {
            int t = seen[j - 1];
            seen[j - 1] = seen[j];
            seen[j] = t;
        }
    for (int i = 0; i < nseen; i++) {
        if (seen[i] <= 2 && !with_stdio)
            continue;
        report_fd(seen[i]);
    }
    /* The two ends of a pipe are one object. */
    u64 a = 0, b = 0;
    u32 m = 0;
    fd_stat(pfd[0], &a, &m);
    fd_stat(pfd[1], &b, &m);
    emit("pipe ends share an inode: ");
    emit(a == b ? "yes" : "no");
    emit("\n");
    sc6(SYS_CLOSE, dirfd, 0, 0, 0, 0, 0);
    return 0;
}
