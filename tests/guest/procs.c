/* Processes (B1 step 3): fork with copy-on-write, wait4 statuses (exit and death by
 * SIGKILL), a pipe between parent and child, vfork, SIGCHLD delivery to a handler, and
 * reparenting of an orphan. Output never prints a pid; each check prints what it saw,
 * and the parent prints after each child has been reaped, so the output does not
 * depend on the interleaving. */
#include "sys.h"

#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SYS_GETPID 172
#define SYS_GETPPID 173
#define SYS_KILL 129
#define SYS_CLOSE 57
#define SYS_OPENAT 56
#define SYS_PIPE2 59
#define SYS_READ 63
#define SYS_RT_SIGACTION 134
#define SYS_NANOSLEEP 101
#define SYS_FUTEX 98
#define SYS_MMAP 222
#define SYS_WAITID 95
#define SYS_GETPGID 155
#define SYS_GETSID 156
#define SYS_SETSID 157
#define SYS_SETPGID 154

#define SIGKILL_N 9
#define SIGCHLD_N 17
#define CLONE_VM 0x00000100
#define CLONE_VFORK 0x00004000
#define SA_SIGINFO 0x00000004
#define SA_RESTORER 0x04000000
#define SA_RESTART 0x10000000
#define CLONE_CHILD_SETTID 0x01000000
#define CLONE_CHILD_CLEARTID 0x00200000
#define MAP_SHARED_ANON 0x21

/* The signal return trampoline (SA_RESTORER): rt_sigreturn. */
extern void xp_restore(void);
__asm__(".text\n"
        ".global xp_restore\n"
        "xp_restore:\n"
        "  mov x8, #139\n"
        "  svc #0\n");

static u64 shared_word;     /* copy-on-write check: a parent's value, changed by the child */
static u64 big[4096];       /* 32 KiB of private data: several pages */
static volatile int vfork_flag;
static volatile int chld_count;
static volatile u64 chld_pid;
static volatile int chld_code;
static u32 ctid_word;       /* CLONE_CHILD_SETTID / CLEARTID word */

static i64 do_fork(void) { return sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0); }

static i64 wait_for(i64 pid, i64 *st) { return sc6(SYS_WAIT4, pid, (i64)st, 0, 0, 0, 0); }

static void nap_ms(u64 ms)
{
    /* struct timespec: tv_nsec must stay below one second. */
    const i64 ts[2] = {(i64)(ms / 1000), (i64)((ms % 1000) * 1000000ull)};
    sc6(SYS_NANOSLEEP, (i64)ts, 0, 0, 0, 0, 0);
}

static void on_chld(int sig, void *info, void *uc)
{
    (void)sig;
    (void)uc;
    chld_count++;
    /* siginfo: si_pid at offset 16, si_code at offset 8. */
    chld_pid = *(const u32 *)((const u8 *)info + 16);
    chld_code = *(const i32 *)((const u8 *)info + 8);
}

static void exit_now(int code) { sc6(94, code, 0, 0, 0, 0, 0); }

/* 1 if buf[0..n) contains the text needle. */
static int contains(const char *buf, u64 n, const char *needle)
{
    size_t m = strlen(needle);
    for (u64 i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m && buf[i + k] == needle[k])
            k++;
        if (k == m)
            return 1;
    }
    return 0;
}

/* "/proc/<pid><suffix>" into dst. */
static void proc_path(char *dst, u64 pid, const char *suffix)
{
    char digits[24];
    int n = 0;
    do {
        digits[n++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid);
    size_t k = 0;
    const char *head = "/proc/";
    while (*head)
        dst[k++] = *head++;
    while (n)
        dst[k++] = digits[--n];
    while (*suffix)
        dst[k++] = *suffix++;
    dst[k] = 0;
}


int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shared_word = 100;
    for (u64 i = 0; i < 4096; i++)
        big[i] = i * 3;

    /* 1. fork and copy-on-write: the child writes, the parent keeps its data. */
    i64 pid = do_fork();
    if (pid == 0) {
        shared_word = 200;
        big[5] = 1;
        big[3000] = 2;
        exit_now(7);
    }
    if (pid < 0) {
        put("procs: fork failed\n");
        return 1;
    }
    i64 st = -1;
    i64 rc = wait_for(pid, &st);
    put("procs: cow shared_word=");
    putu(shared_word);
    put(" big_ok=");
    put(big[5] == 15 && big[3000] == 9000 ? "1" : "0");
    put("\n");

    /* 2. wait4 status of a normal exit. */
    put("procs: exit wait_rc_is_pid=");
    put(rc == pid ? "1" : "0");
    put(" exited=");
    put((st & 0x7f) == 0 ? "1" : "0");
    put(" status=");
    putu(((u64)st >> 8) & 0xff);
    put("\n");

    /* 3. death by SIGKILL, raised by the parent while the child sleeps. */
    pid = do_fork();
    if (pid == 0) {
        nap_ms(10000);
        exit_now(0);
    }
    sc6(SYS_KILL, pid, SIGKILL_N, 0, 0, 0, 0);
    st = -1;
    rc = wait_for(pid, &st);
    put("procs: killed wait_rc_is_pid=");
    put(rc == pid ? "1" : "0");
    put(" signaled=");
    put((st & 0x7f) == SIGKILL_N ? "1" : "0");
    put(" sig=");
    putu((u64)(st & 0x7f));
    put("\n");

    /* 4. a pipe between parent and child: the child writes, exits; the parent reads to
     * end of file (the child's exit releases its end, the parent's own end is closed). */
    int fds[2];
    sc6(SYS_PIPE2, (i64)fds, 0, 0, 0, 0, 0);
    pid = do_fork();
    if (pid == 0) {
        sys_write(fds[1], "child says hi\n", 14);
        sc6(SYS_CLOSE, fds[0], 0, 0, 0, 0, 0);
        exit_now(0);
    }
    sc6(SYS_CLOSE, fds[1], 0, 0, 0, 0, 0);
    char text[64];
    u64 got = 0;
    for (;;) {
        i64 n = sc6(SYS_READ, fds[0], (i64)(text + got), 16, 0, 0, 0);
        if (n <= 0)
            break;
        got += (u64)n;
    }
    st = -1;
    rc = wait_for(pid, &st);
    put("procs: pipe bytes=");
    putu(got);
    put(" text=");
    sys_write(1, text, got);
    put(" reaped=");
    put(rc == pid ? "1" : "0");
    put("\n");

    /* 5. vfork: the child shares the address space and the parent waits for its exit. */
    pid = sc6(SYS_CLONE, CLONE_VM | CLONE_VFORK | SIGCHLD_N, 0, 0, 0, 0, 0);
    if (pid == 0) {
        vfork_flag = 1;
        exit_now(3);
    }
    put("procs: vfork flag=");
    putu((u64)vfork_flag);
    st = -1;
    rc = wait_for(pid, &st);
    put(" status=");
    putu(((u64)st >> 8) & 0xff);
    put(" reaped=");
    put(rc == pid ? "1" : "0");
    put("\n");

    /* 6. SIGCHLD: a handler runs when the child exits; the blocked wait4 is restarted. */
    u64 sa[4] = {(u64)on_chld, SA_SIGINFO | SA_RESTART | SA_RESTORER, (u64)xp_restore, 0};
    sc6(SYS_RT_SIGACTION, SIGCHLD_N, (i64)sa, 0, 8, 0, 0);
    pid = do_fork();
    if (pid == 0) {
        nap_ms(2);
        exit_now(4);
    }
    st = -1;
    rc = wait_for(pid, &st);
    put("procs: sigchld count=");
    putu((u64)chld_count);
    put(" pid_match=");
    put(chld_pid == (u64)pid ? "1" : "0");
    put(" code_exited=");
    put(chld_code == 1 ? "1" : "0");
    put(" status=");
    putu(((u64)st >> 8) & 0xff);
    put(" reaped=");
    put(rc == pid ? "1" : "0");
    put("\n");
    sa[0] = 0; /* SIG_DFL */
    sc6(SYS_RT_SIGACTION, SIGCHLD_N, (i64)sa, 0, 8, 0, 0);

    /* 7. reparenting: the child forks a grandchild and exits without waiting. The
     * grandchild waits until its parent is gone, then reports that its parent changed. */
    int rp[2], sy[2];
    sc6(SYS_PIPE2, (i64)rp, 0, 0, 0, 0, 0);
    sc6(SYS_PIPE2, (i64)sy, 0, 0, 0, 0, 0);
    pid = do_fork();
    if (pid == 0) {
        sc6(SYS_CLOSE, rp[0], 0, 0, 0, 0, 0);
        i64 gc = do_fork();
        if (gc == 0) {
            sc6(SYS_CLOSE, sy[0], 0, 0, 0, 0, 0);
            i64 orig = sc6(SYS_GETPPID, 0, 0, 0, 0, 0, 0);
            /* Tell the child its parent id has been read, so it may exit. */
            char s = 'S';
            sys_write(sy[1], &s, 1);
            int moved = 0;
            for (int i = 0; i < 5000 && !moved; i++) {
                if (sc6(SYS_GETPPID, 0, 0, 0, 0, 0, 0) != orig)
                    moved = 1;
                else
                    nap_ms(1);
            }
            char c = moved ? 'R' : 'N';
            sys_write(rp[1], &c, 1);
            exit_now(0);
        }
        /* The child waits for the grandchild's start, then exits without waiting for it. */
        sc6(SYS_CLOSE, sy[1], 0, 0, 0, 0, 0);
        char go = 0;
        sc6(SYS_READ, sy[0], (i64)&go, 1, 0, 0, 0);
        exit_now(0);
    }
    sc6(SYS_CLOSE, sy[0], 0, 0, 0, 0, 0);
    sc6(SYS_CLOSE, sy[1], 0, 0, 0, 0, 0);
    sc6(SYS_CLOSE, rp[1], 0, 0, 0, 0, 0);
    char report = 0;
    i64 n = sc6(SYS_READ, rp[0], (i64)&report, 1, 0, 0, 0);
    i64 eof = sc6(SYS_READ, rp[0], (i64)&report, 1, 0, 0, 0);
    st = -1;
    rc = wait_for(pid, &st);
    put("procs: orphan read=");
    putd(n);
    put(" report=");
    put(report == 'R' ? "R" : "other");
    put(" then_eof=");
    put(eof == 0 ? "1" : "0");
    put(" reaped=");
    put(rc == pid ? "1" : "0");
    put("\n");
    /* 8. sessions and groups: the child makes a session and waits (two pipes) while the
     * parent looks at its group and session; then the child exits and is reaped, and its
     * group is gone. */
    int ga[2], gb[2];
    sc6(SYS_PIPE2, (i64)ga, 0, 0, 0, 0, 0);
    sc6(SYS_PIPE2, (i64)gb, 0, 0, 0, 0, 0);
    pid = do_fork();
    if (pid == 0) {
        sc6(SYS_CLOSE, ga[0], 0, 0, 0, 0, 0);
        sc6(SYS_CLOSE, gb[1], 0, 0, 0, 0, 0);
        i64 sid = sc6(SYS_SETSID, 0, 0, 0, 0, 0, 0);
        put("procs: child setsid leader=");
        put(sid == sc6(SYS_GETPID, 0, 0, 0, 0, 0, 0) ? "1" : "0");
        put("\n");
        char ready = 'r';
        sys_write(ga[1], &ready, 1);
        char go = 0;
        sc6(SYS_READ, gb[0], (i64)&go, 1, 0, 0, 0);
        exit_now(0);
    }
    sc6(SYS_CLOSE, ga[1], 0, 0, 0, 0, 0);
    sc6(SYS_CLOSE, gb[0], 0, 0, 0, 0, 0);
    char seen = 0;
    sc6(SYS_READ, ga[0], (i64)&seen, 1, 0, 0, 0);
    put("procs: child group=");
    put(sc6(SYS_GETPGID, pid, 0, 0, 0, 0, 0) == pid ? "1" : "0");
    put(" session=");
    put(sc6(SYS_GETSID, pid, 0, 0, 0, 0, 0) == pid ? "1" : "0");
    put("\n");
    char done = 'g';
    sys_write(gb[1], &done, 1);
    sc6(SYS_CLOSE, gb[1], 0, 0, 0, 0, 0);
    st = -1;
    rc = wait_for(pid, &st);
    put("procs: group gone after reap=");
    put(sc6(SYS_KILL, -pid, 0, 0, 0, 0, 0) == -3 ? "1" : "0");
    put("\n");

    /* 9. waitid with P_PID and WEXITED: the siginfo fields, then the child is gone. */
    pid = do_fork();
    if (pid == 0)
        exit_now(5);
    u8 info[128];
    memset(info, 0, sizeof info);
    i64 wr = sc6(SYS_WAITID, 1, pid, (i64)info, 4, 0, 0);
    i32 si_signo = *(i32 *)(info + 0), si_code = *(i32 *)(info + 8);
    i32 si_pid = *(i32 *)(info + 16), si_status = *(i32 *)(info + 24);
    put("procs: waitid rc=");
    putd(wr);
    put(" signo=");
    putd(si_signo);
    put(" code=");
    putd(si_code);
    put(" status=");
    putd(si_status);
    put(" pid_match=");
    put(si_pid == pid ? "1" : "0");
    st = -1;
    rc = wait_for(pid, &st);
    put(" again=");
    putd(rc);
    put("\n");

    /* 10. CLONE_CHILD_SETTID: the child's own word holds its pid; CLEARTID clears the
     * child's copy at exit, so the parent's copy stays zero. */
    pid = sc6(SYS_CLONE, SIGCHLD_N | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID, 0, 0, 0, (i64)&ctid_word, 0);
    if (pid == 0) {
        put("procs: child settid ok=");
        put(ctid_word == (u32)sc6(SYS_GETPID, 0, 0, 0, 0, 0, 0) ? "1" : "0");
        put("\n");
        exit_now(0);
    }
    st = -1;
    rc = wait_for(pid, &st);
    put("procs: parent word zero=");
    put(ctid_word == 0 ? "1" : "0");
    put("\n");

    /* 11. a shared anonymous mapping is shared with the child, and a futex on it wakes
     * the parent across the two processes. */
    u32 *sh = (u32 *)sc6(SYS_MMAP, 0, 4096, 3, MAP_SHARED_ANON, -1, 0);
    sh[0] = 0;
    sh[1] = 0;
    pid = do_fork();
    if (pid == 0) {
        sh[1] = 42;
        __atomic_store_n(&sh[0], 1u, __ATOMIC_RELEASE);
        sc6(SYS_FUTEX, (i64)&sh[0], 1 /* FUTEX_WAKE */, 1, 0, 0, 0);
        exit_now(0);
    }
    while (__atomic_load_n(&sh[0], __ATOMIC_ACQUIRE) == 0)
        sc6(SYS_FUTEX, (i64)&sh[0], 0 /* FUTEX_WAIT */, 0, 0, 0, 0);
    st = -1;
    rc = wait_for(pid, &st);
    put("procs: shared value=");
    putu(sh[1]);
    put(" reaped=");
    put(rc == pid ? "1" : "0");
    put("\n");
    /* 12. /proc/<pid> of a live child: stat names the program, cmdline holds its argv.
     * After the child is reaped the entry is gone (ENOENT). */
    pid = do_fork();
    if (pid == 0) {
        nap_ms(10000);
        exit_now(0);
    }
    char path[64];
    char buf[512];
    proc_path(path, (u64)pid, "/stat");
    i64 fd = sc6(SYS_OPENAT, -100, (i64)path, 0, 0, 0, 0);
    put("procs: proc stat open=");
    put(fd >= 0 ? "1" : "0");
    i64 nread = fd >= 0 ? sc6(SYS_READ, fd, (i64)buf, sizeof buf, 0, 0, 0) : 0;
    put(" names_program=");
    put(nread > 0 && contains(buf, (u64)nread, "(procs-") ? "1" : "0");
    if (fd >= 0)
        sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    proc_path(path, (u64)pid, "/cmdline");
    fd = sc6(SYS_OPENAT, -100, (i64)path, 0, 0, 0, 0);
    nread = fd >= 0 ? sc6(SYS_READ, fd, (i64)buf, sizeof buf, 0, 0, 0) : 0;
    put(" cmdline_has_argv0=");
    put(nread > 0 && contains(buf, (u64)nread, "procs-") ? "1" : "0");
    if (fd >= 0)
        sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    put("\n");
    sc6(SYS_KILL, pid, SIGKILL_N, 0, 0, 0, 0);
    st = -1;
    rc = wait_for(pid, &st);
    proc_path(path, (u64)pid, "/stat");
    fd = sc6(SYS_OPENAT, -100, (i64)path, 0, 0, 0, 0);
    put("procs: proc after reap=");
    putd(fd);
    put("\n");
    /* 13. kill to a process group: the child makes its own group (setpgid) and reports
     * it; the parent kills the group, and the child dies of SIGKILL. */
    int gp[2];
    sc6(SYS_PIPE2, (i64)gp, 0, 0, 0, 0, 0);
    pid = do_fork();
    if (pid == 0) {
        sc6(SYS_CLOSE, gp[0], 0, 0, 0, 0, 0);
        sc6(SYS_SETPGID, 0, 0, 0, 0, 0, 0);
        char r = 'r';
        sys_write(gp[1], &r, 1);
        nap_ms(10000);
        exit_now(0);
    }
    sc6(SYS_CLOSE, gp[1], 0, 0, 0, 0, 0);
    char ready = 0;
    sc6(SYS_READ, gp[0], (i64)&ready, 1, 0, 0, 0);
    i64 krc = sc6(SYS_KILL, -pid, SIGKILL_N, 0, 0, 0, 0);
    st = -1;
    rc = wait_for(pid, &st);
    put("procs: group kill rc=");
    putd(krc);
    put(" signaled=");
    put((st & 0x7f) == SIGKILL_N ? "1" : "0");
    put(" reaped=");
    put(rc == pid ? "1" : "0");
    put("\n");
    /* 14. default SIGPIPE: a child writes to a pipe with no reader and dies of it. */
    int bp[2];
    sc6(SYS_PIPE2, (i64)bp, 0, 0, 0, 0, 0);
    sc6(SYS_CLOSE, bp[0], 0, 0, 0, 0, 0);
    pid = do_fork();
    if (pid == 0) {
        sys_write(bp[1], "x", 1);
        exit_now(0);
    }
    sc6(SYS_CLOSE, bp[1], 0, 0, 0, 0, 0);
    st = -1;
    rc = wait_for(pid, &st);
    put("procs: sigpipe signaled=");
    put((st & 0x7f) == 13 ? "1" : "0");
    put(" sig=");
    putu((u64)(st & 0x7f));
    put(" reaped=");
    put(rc == pid ? "1" : "0");
    put("\n");
    /* 15. copy-on-write under the translator: after fork the child and the parent each
     * rewrite all of big[] in a hot loop with their own values, and check them. The
     * child writes its own copy while the parent works. */
    pid = do_fork();
    if (pid == 0) {
        for (u64 r = 0; r < 16; r++)
            for (u64 i = 0; i < 4096; i++)
                big[i] = i * 5 + r;
        int ok = 1;
        for (u64 i = 0; i < 4096; i++)
            if (big[i] != i * 5 + 15)
                ok = 0;
        put("procs: child rewrite ok=");
        put(ok ? "1" : "0");
        put("\n");
        exit_now(0);
    }
    for (u64 r = 0; r < 16; r++)
        for (u64 i = 0; i < 4096; i++)
            big[i] = i * 7 + r;
    int pok = 1;
    for (u64 i = 0; i < 4096; i++)
        if (big[i] != i * 7 + 15)
            pok = 0;
    st = -1;
    rc = wait_for(pid, &st);
    put("procs: parent rewrite ok=");
    put(pok ? "1" : "0");
    put(" reaped=");
    put(rc == pid ? "1" : "0");
    put("\n");
    put("procs: done\n");
    return 0;
}
