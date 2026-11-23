/* Record locks and flock (locks.c): conflicts between processes, F_GETLK's first conflict and its
 * pid, the split and merge of ranges (seen through F_GETLK), the release of a process's locks on a
 * close of any descriptor of the file, F_SETLKW blocking and wake-up on an unlock, EDEADLK with two
 * processes, EINTR from a signal with no SA_RESTART, and flock (exclusive, shared, LOCK_NB, a
 * downgrade, and release at the last close of a description). Output never prints a pid or a
 * descriptor number. A parent prints only after it has reaped the child that printed before it,
 * or after a read that waits for the child's message, so the output does not depend on the
 * interleaving. The native run in the builder VM is the oracle. */
#include "sys.h"

#define SYS_CLOSE 57
#define SYS_OPENAT 56
#define SYS_READ 63
#define SYS_PIPE2 59
#define SYS_DUP 23
#define SYS_FCNTL 25
#define SYS_FLOCK 32
#define SYS_KILL 129
#define SYS_RT_SIGACTION 134
#define SYS_NANOSLEEP 101
#define SYS_GETPPID 173
#define SYS_CLONE 220
#define SYS_WAIT4 260

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_SETFL 4
#define O_NONBLOCK 04000
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define O_RDONLY 0
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define AT_FDCWD (-100)
#define SIGUSR1 10
#define SIGCHLD_N 17

/* struct flock on arm64: 32 bytes. */
struct flk {
    int16_t type, whence;
    int64_t start, len;
    int32_t pid;
};

static const char path[] = "/tmp/xiaoji-flock-test";
static int g_fd, g_fd2, g_rfd;

static i64 fcntl_(int fd, int cmd, i64 arg) { return sc6(SYS_FCNTL, fd, cmd, arg, 0, 0, 0); }
static i64 setlk(int fd, int cmd, int type, int whence, i64 start, i64 len)
{
    struct flk fl;
    memset(&fl, 0, sizeof fl);
    fl.type = (int16_t)type;
    fl.whence = (int16_t)whence;
    fl.start = start;
    fl.len = len;
    return fcntl_(fd, cmd, (i64)(u64)&fl);
}

/* F_GETLK over [start, start+len): prints the answer and returns the struct. */
static void getlk(const char *label, int fd, int type, i64 start, i64 len)
{
    struct flk fl;
    memset(&fl, 0, sizeof fl);
    fl.type = (int16_t)type;
    fl.whence = SEEK_SET;
    fl.start = start;
    fl.len = len;
    i64 rc = fcntl_(fd, F_GETLK, (i64)(u64)&fl);
    put(label);
    put(" rc=");
    putd(rc);
    put(" type=");
    putd(fl.type);
    put(" start=");
    putd(fl.start);
    put(" len=");
    putd(fl.len);
    if (fl.type != F_UNLCK) {
        put(" holder=");
        put(fl.pid == (int32_t)sc6(SYS_GETPPID, 0, 0, 0, 0, 0, 0) ? "parent" : "other");
    }
    put("\n");
}

static void line(const char *label, i64 rc)
{
    put(label);
    put(" rc=");
    putd(rc);
    put("\n");
}

static void nap_ms(i64 ms)
{
    i64 ts[2] = { ms / 1000, (ms % 1000) * 1000000 };
    sc6(SYS_NANOSLEEP, (i64)(u64)ts, 0, 0, 0, 0, 0);
}

static i64 spawn(void (*fn)(void))
{
    i64 pid = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (pid == 0) {
        fn();
        sys_exit_group(0);
    }
    return pid;
}

static i64 reap(i64 pid)
{
    int st = -1;
    sc6(SYS_WAIT4, pid, (i64)(u64)&st, 0, 0, 0, 0);
    return st;
}

/* ---------- children ---------- */

/* Sees the parent's locks: conflicts, the first conflicting lock in list order, ranges and pids. */
static void child_view(void)
{
    getlk("c WR 0..0", g_fd, F_WRLCK, 0, 1);
    getlk("c RD 12..12", g_fd, F_RDLCK, 12, 1);
    getlk("c WR 12..12", g_fd, F_WRLCK, 12, 1);
    line("c SETLK WR 0..0", setlk(g_fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1));
    line("c SETLK RD 0..0", setlk(g_fd, F_SETLK, F_RDLCK, SEEK_SET, 0, 1));
    getlk("c WR 200..200", g_fd, F_WRLCK, 200, 1);
    getlk("c WR 45..45", g_fd, F_WRLCK, 45, 1);
    getlk("c WR 28..28", g_fd, F_WRLCK, 28, 1);
    getlk("c WR 20..20", g_fd, F_WRLCK, 20, 1);
    getlk("c WR 23..23", g_fd, F_WRLCK, 23, 1);
    getlk("c RD 26..26", g_fd, F_RDLCK, 26, 1);
}

static void child_after_split_unlock(void)
{
    getlk("c RD 22..22", g_fd, F_RDLCK, 22, 1);
}

static void child_after_close(void)
{
    getlk("c WR 28..28 after close", g_fd, F_WRLCK, 28, 1);
}

static int g_pipe_w;

/* SETLKW on a range the parent holds: blocks until the parent unlocks, then writes 'A'. */
static void child_wait_unlock(void)
{
    i64 rc = setlk(g_fd, F_SETLKW, F_WRLCK, SEEK_SET, 60, 10);
    char c = rc == 0 ? 'A' : '?';
    sc6(64, g_pipe_w, (i64)(u64)&c, 1, 0, 0, 0);
}

/* Holds [81], then waits for [80], which the parent holds. It wakes when the parent unlocks. */
static void child_deadlock(void)
{
    line("c SETLK 81", setlk(g_fd, F_SETLK, F_WRLCK, SEEK_SET, 81, 1));
    i64 rc = setlk(g_fd, F_SETLKW, F_WRLCK, SEEK_SET, 80, 1);
    line("c SETLKW 80 after parent unlock", rc);
}

static volatile int g_handled;

static void on_usr1(int sig)
{
    (void)sig;
    g_handled++;
    put("handler ran\n");
}

/* SIGUSR1 with a handler and no SA_RESTART: a blocked SETLKW returns EINTR. */
static void child_eintr(void)
{
    struct {
        u64 handler, flags, restorer, mask;
    } sa = { (u64)(uintptr_t)on_usr1, 0, 0, 0 };
    sc6(SYS_RT_SIGACTION, SIGUSR1, (i64)(u64)&sa, 0, 8, 0, 0);
    line("c SETLKW EINTR", setlk(g_fd, F_SETLKW, F_WRLCK, SEEK_SET, 100, 1));
}

/* Blocks on an exclusive flock held by the parent's second description. Then writes 'B'. */
static void child_flock_block(void)
{
    int fd = (int)sc6(SYS_OPENAT, AT_FDCWD, (i64)(u64)path, O_RDWR, 0, 0, 0);
    i64 rc = sc6(SYS_FLOCK, fd, LOCK_EX, 0, 0, 0, 0);
    char c = rc == 0 ? 'B' : '?';
    sc6(64, g_pipe_w, (i64)(u64)&c, 1, 0, 0, 0);
}

static void child_flock_probe(void)
{
    int fd = (int)sc6(SYS_OPENAT, AT_FDCWD, (i64)(u64)path, O_RDWR, 0, 0, 0);
    line("c flock SH NB while EX held", sc6(SYS_FLOCK, fd, LOCK_SH | LOCK_NB, 0, 0, 0, 0));
}

static void wait_pipe_char(int rfd, const char *label)
{
    char c = 0;
    i64 rc = sc6(SYS_READ, rfd, (i64)(u64)&c, 1, 0, 0, 0);
    put(label);
    put(" rc=");
    putd(rc);
    put(" ");
    sc6(64, 1, (i64)(u64)&c, 1, 0, 0, 0);
    put("\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    g_fd = (int)sc6(SYS_OPENAT, AT_FDCWD, (i64)(u64)path, O_RDWR | O_CREAT | O_TRUNC, 0644, 0, 0);
    g_rfd = (int)sc6(SYS_OPENAT, AT_FDCWD, (i64)(u64)path, O_RDONLY, 0, 0, 0);
    g_fd2 = (int)sc6(SYS_OPENAT, AT_FDCWD, (i64)(u64)path, O_RDWR, 0, 0, 0);
    char data[100];
    memset(data, 'x', sizeof data);
    sc6(64, g_fd, (i64)(u64)data, sizeof data, 0, 0, 0);

    /* Same owner never conflicts. The ranges are: WR [0,9], RD [10,19], RD [200,EOF], WR [40,49]. */
    line("own WR 0..9", setlk(g_fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 10));
    line("own RD 10..19", setlk(g_fd, F_SETLK, F_RDLCK, SEEK_SET, 10, 10));
    line("own RD 200..EOF", setlk(g_fd, F_SETLK, F_RDLCK, SEEK_SET, 200, 0));
    line("own WR 50 len -10 (40..49)", setlk(g_fd, F_SETLK, F_WRLCK, SEEK_SET, 50, -10));
    line("WR on read-only fd", setlk(g_rfd, F_SETLK, F_WRLCK, SEEK_SET, 300, 1));
    line("bad whence", setlk(g_fd, F_SETLK, F_RDLCK, 5, 0, 1));
    line("bad type", setlk(g_fd, F_SETLK, 7, SEEK_SET, 0, 1));
    line("SEEK_END len 1 at byte 99", setlk(g_fd, F_SETLK, F_WRLCK, SEEK_END, -1, 1));
    line("unlock 99", setlk(g_fd, F_SETLK, F_UNLCK, SEEK_END, -1, 1));
    line("unlock never held", setlk(g_fd, F_SETLK, F_UNLCK, SEEK_SET, 500, 5));

    /* A child (another owner) sees the locks: the first conflict, its range and its holder. */
    reap(spawn(child_view));
    put("parent: child_view done\n");

    /* Split and merge: WR [20,29], then RD [22,25] splits it, unlock [22,25], then WR [21,26]
     * merges with the pieces to WR [20,29]. */
    line("own WR 20..29", setlk(g_fd, F_SETLK, F_WRLCK, SEEK_SET, 20, 10));
    line("own RD 22..25 (split)", setlk(g_fd, F_SETLK, F_RDLCK, SEEK_SET, 22, 4));
    reap(spawn(child_view));
    put("parent: split seen\n");
    line("own UN 22..25", setlk(g_fd, F_SETLK, F_UNLCK, SEEK_SET, 22, 4));
    reap(spawn(child_after_split_unlock));
    line("own WR 21..26 (merge)", setlk(g_fd, F_SETLK, F_WRLCK, SEEK_SET, 21, 6));
    reap(spawn(child_after_split_unlock));
    put("parent: merge done\n");

    /* Release on a close of a different descriptor of the same file: all of this process's locks on
     * the file go. */
    int fd3 = (int)sc6(SYS_OPENAT, AT_FDCWD, (i64)(u64)path, O_RDWR, 0, 0, 0);
    line("close another fd of the file", sc6(SYS_CLOSE, fd3, 0, 0, 0, 0, 0));
    reap(spawn(child_after_close));
    put("parent: close released\n");

    /* F_SETLKW: the child blocks on [60,69] until the parent unlocks; the pipe shows it blocked. */
    int p[2];
    sc6(SYS_PIPE2, (i64)(u64)p, 0, 0, 0, 0, 0);
    g_pipe_w = p[1];
    line("own WR 60..69", setlk(g_fd, F_SETLK, F_WRLCK, SEEK_SET, 60, 10));
    i64 child = spawn(child_wait_unlock);
    nap_ms(50);
    fcntl_(p[0], F_SETFL, O_NONBLOCK);
    i64 blocked = sc6(SYS_READ, p[0], (i64)(u64)&(char){ 0 }, 1, 0, 0, 0);
    line("child blocked before unlock", blocked);
    line("own UN 60..69", setlk(g_fd, F_SETLK, F_UNLCK, SEEK_SET, 60, 10));
    fcntl_(p[0], F_SETFL, 0);
    wait_pipe_char(p[0], "child woke");
    line("child exit", reap(child));
    getlk("GETLK after child exit", g_fd, F_WRLCK, 60, 10);

    /* EDEADLK, two processes: the child holds [81] and waits for [80]; the parent holds [80] and asks
     * for [81] (the child's). The parent's request is the one that would close the cycle. */
    line("own WR 80", setlk(g_fd, F_SETLK, F_WRLCK, SEEK_SET, 80, 1));
    child = spawn(child_deadlock);
    nap_ms(100);
    line("parent SETLKW 81 (EDEADLK)", setlk(g_fd, F_SETLKW, F_WRLCK, SEEK_SET, 81, 1));
    i64 unlock80 = setlk(g_fd, F_SETLK, F_UNLCK, SEEK_SET, 80, 1);
    line("child exit", reap(child)); /* the child wakes and prints: the parent prints after it */
    line("own UN 80", unlock80);

    /* EINTR: the parent holds [100,109]; the child waits for [100] and a signal interrupts it. */
    line("own WR 100..109", setlk(g_fd, F_SETLK, F_WRLCK, SEEK_SET, 100, 10));
    child = spawn(child_eintr);
    nap_ms(50);
    sc6(SYS_KILL, child, SIGUSR1, 0, 0, 0, 0);
    line("child exit", reap(child));
    line("own UN 100..109", setlk(g_fd, F_SETLK, F_UNLCK, SEEK_SET, 100, 10));

    /* flock: exclusive, shared, NB, downgrade, unlock; two descriptions in one process. */
    int fd2 = g_fd2; /* still open: the close above used fd3 */
    line("flock EX", sc6(SYS_FLOCK, g_fd, LOCK_EX, 0, 0, 0, 0));
    line("flock SH NB (other description, EX held)", sc6(SYS_FLOCK, fd2, LOCK_SH | LOCK_NB, 0, 0, 0, 0));
    line("flock SH (downgrade)", sc6(SYS_FLOCK, g_fd, LOCK_SH, 0, 0, 0, 0));
    line("flock SH NB (now shared)", sc6(SYS_FLOCK, fd2, LOCK_SH | LOCK_NB, 0, 0, 0, 0));
    line("flock EX NB (SH held)", sc6(SYS_FLOCK, fd2, LOCK_EX | LOCK_NB, 0, 0, 0, 0));
    line("flock UN", sc6(SYS_FLOCK, fd2, LOCK_UN, 0, 0, 0, 0));
    line("flock UN again", sc6(SYS_FLOCK, fd2, LOCK_UN, 0, 0, 0, 0));
    line("flock UN (first description)", sc6(SYS_FLOCK, g_fd, LOCK_UN, 0, 0, 0, 0));
    line("flock EX (free)", sc6(SYS_FLOCK, fd2, LOCK_EX, 0, 0, 0, 0));
    reap(spawn(child_flock_probe));
    put("parent: flock probe done\n");

    /* A blocking flock: the child waits for the parent's EX, and the pipe shows it blocked. */
    int q[2];
    sc6(SYS_PIPE2, (i64)(u64)q, 0, 0, 0, 0, 0);
    g_pipe_w = q[1];
    fcntl_(q[0], F_SETFL, O_NONBLOCK);
    child = spawn(child_flock_block);
    nap_ms(50);
    i64 fb = sc6(SYS_READ, q[0], (i64)(u64)&(char){ 0 }, 1, 0, 0, 0);
    line("flock child blocked before unlock", fb);
    line("flock UN (wakes child)", sc6(SYS_FLOCK, fd2, LOCK_UN, 0, 0, 0, 0));
    fcntl_(q[0], F_SETFL, 0);
    wait_pipe_char(q[0], "flock child woke");
    line("flock child exit", reap(child));

    /* Released at the last close of the description: a dup keeps it, closing the last reference frees. */
    line("flock EX (for dup test)", sc6(SYS_FLOCK, fd2, LOCK_EX, 0, 0, 0, 0));
    int fd4 = (int)sc6(SYS_DUP, fd2, 0, 0, 0, 0, 0);
    sc6(SYS_CLOSE, fd2, 0, 0, 0, 0, 0);
    int fd5 = (int)sc6(SYS_OPENAT, AT_FDCWD, (i64)(u64)path, O_RDWR, 0, 0, 0);
    line("flock SH NB after close of one dup", sc6(SYS_FLOCK, fd5, LOCK_SH | LOCK_NB, 0, 0, 0, 0));
    sc6(SYS_CLOSE, fd4, 0, 0, 0, 0, 0);
    line("flock SH NB after last close", sc6(SYS_FLOCK, fd5, LOCK_SH | LOCK_NB, 0, 0, 0, 0));
    line("flock UN", sc6(SYS_FLOCK, fd5, LOCK_UN, 0, 0, 0, 0));
    line("flock bad op", sc6(SYS_FLOCK, fd5, 3, 0, 0, 0, 0));

    put("done\n");
    return 0;
}
