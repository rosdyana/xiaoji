/* pidfd_open (B16): installd's wait_child_with_timeout polls a pidfd of its dex2oat child. A child sleeps 50 ms and
 * exits with status 3. The pidfd is close-on-exec; it is not readable while the child runs and is readable (POLLIN)
 * once the child has ended, as a zombie and after the reap. Bad pids and flags fail, and a pidfd has no read, write
 * or pread. Only results are printed. */
#include "sys.h"

#define SYS_READ 63
#define SYS_CLOSE 57
#define SYS_PREAD64 67
#define SYS_FCNTL 25
#define SYS_PPOLL 73
#define SYS_NANOSLEEP 101
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SYS_PIDFD_OPEN 434

#define POLLIN 0x1
#define F_GETFD 1
#define FD_CLOEXEC 1
#define SIGCHLD_N 17

struct pollfd_t {
    int fd;
    short events;
    short revents;
};

static void line(const char *tag, i64 rc)
{
    put(tag);
    put(" ");
    putd(rc);
    put("\n");
}

/* ppoll on one fd. ms < 0 blocks; returns the count and sets *rev. */
static i64 poll1(int fd, i64 ms, short *rev)
{
    struct pollfd_t p = {fd, POLLIN, 0};
    i64 ts[2] = {ms / 1000, (ms % 1000) * 1000000};
    i64 r = sc6(SYS_PPOLL, (i64)&p, 1, ms < 0 ? 0 : (i64)ts, 0, 8, 0);
    *rev = p.revents;
    return r;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    i64 pid = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (pid == 0) {
        const i64 nap[2] = {0, 50000000};
        sc6(SYS_NANOSLEEP, (i64)nap, 0, 0, 0, 0, 0);
        sys_exit_group(3);
    }
    if (pid < 0) {
        line("fork", pid);
        return 1;
    }

    i64 pfd = sc6(SYS_PIDFD_OPEN, pid, 0, 0, 0, 0, 0);
    line("pidfd_open child", pfd >= 0 ? 0 : pfd);
    if (pfd < 0)
        return 1;
    line("cloexec", (sc6(SYS_FCNTL, pfd, F_GETFD, 0, 0, 0, 0) & FD_CLOEXEC) ? 1 : 0);

    short rev = 0;
    line("poll before exit (0 ms)", poll1((int)pfd, 0, &rev));
    line("poll before exit revents", rev);

    rev = 0;
    i64 n = poll1((int)pfd, -1, &rev);
    line("poll after exit", n);
    line("poll after exit POLLIN", (rev & POLLIN) ? 1 : 0);

    /* The child is a zombie now: it is still a process, so a new pidfd opens. */
    i64 zfd = sc6(SYS_PIDFD_OPEN, pid, 0, 0, 0, 0, 0);
    line("pidfd_open zombie", zfd >= 0 ? 0 : zfd);
    if (zfd >= 0)
        sc6(SYS_CLOSE, zfd, 0, 0, 0, 0, 0);

    char buf[4];
    line("read pidfd", sc6(SYS_READ, pfd, (i64)buf, 1, 0, 0, 0));
    line("write pidfd", sc6(SYS_WRITE, pfd, (i64)buf, 1, 0, 0, 0));
    line("pread pidfd", sc6(SYS_PREAD64, pfd, (i64)buf, 1, 0, 0, 0));

    line("pidfd_open pid 0", sc6(SYS_PIDFD_OPEN, 0, 0, 0, 0, 0, 0));
    line("pidfd_open pid 999999", sc6(SYS_PIDFD_OPEN, 999999, 0, 0, 0, 0, 0));
    line("pidfd_open flags 1", sc6(SYS_PIDFD_OPEN, pid, 1, 0, 0, 0, 0));

    i32 status = 0;
    i64 w = sc6(SYS_WAIT4, pid, (i64)&status, 0, 0, 0, 0);
    line("reaped", w == pid ? 0 : w);
    line("child exit status", (status >> 8) & 0xff);

    /* Reaped: the old pidfd still polls readable, and a new one is ESRCH. */
    rev = 0;
    line("poll after reap", poll1((int)pfd, 0, &rev));
    line("poll after reap POLLIN", (rev & POLLIN) ? 1 : 0);
    line("pidfd_open after reap", sc6(SYS_PIDFD_OPEN, pid, 0, 0, 0, 0, 0));
    sc6(SYS_CLOSE, pfd, 0, 0, 0, 0, 0);
    return 0;
}
