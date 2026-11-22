/* execve and execveat (B1 step 4). Run without arguments, this is the parent: it execs the
 * sibling args binary (argv, wait status), checks that failed execs leave it running
 * (ENOENT, EACCES, ENOEXEC, E2BIG), runs a #! script, an AT_EMPTY_PATH execveat, a
 * vfork child that execs, FD_CLOEXEC across exec, and signal state across exec.
 *
 * Run with argv[1] == "probe", it is the program an exec started, and it reports what it
 * inherited: an environment variable, the state of two descriptors, the signal
 * dispositions and mask, and the alternate stack. Output never prints a pid or a fd
 * number, so it matches native Linux. */
#include "sys.h"

#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_READ 63
#define SYS_GETPID 172
#define SYS_GETTID 178
#define SYS_PRCTL 167
#define SYS_PIPE2 59
#define SYS_FCNTL 25
#define SYS_READLINKAT 78
#define SYS_CLONE 220
#define SYS_EXECVE 221
#define SYS_EXECVEAT 281
#define SYS_WAIT4 260
#define SYS_RT_SIGACTION 134
#define SYS_RT_SIGPROCMASK 135
#define SYS_SIGALTSTACK 132
#define SYS_NANOSLEEP 101
#define SYS_GETRUSAGE 165

#define AT_FDCWD (-100)
#define AT_EMPTY_PATH 0x1000
#define F_GETFD 1
#define F_SETFD 2
#define FD_CLOEXEC 1
#define O_RDONLY 0
#define O_WRONLY 01
#define O_CREAT 0100
#define O_TRUNC 01000
#define SIG_BLOCK 0
#define SIGUSR1_N 10
#define SIGUSR2_N 12
#define SIGTERM_N 15
#define SIGCHLD_N 17
#define CLONE_VM 0x00000100
#define CLONE_VFORK 0x00004000
/* CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM: a thread. */
#define CLONE_THREAD_FLAGS 0x00050F00
#define SS_DISABLE 2
#define PR_GET_NAME 16
#define EXEC_FAILED_RC 99
#define BIG_LEN (256u << 10) /* one argv string longer than the 128 KiB limit */
#define PATH_LEN 512

typedef struct {
    u64 handler, flags, restorer, mask;
} ksigaction;

typedef struct {
    u64 sp;
    i32 flags;
    i32 pad;
    u64 size;
} kstack;

static char *env_list[] = {"XIAOJI_EXEC_TEST=yes", 0};
static char self_path[PATH_LEN];
static char args_path[PATH_LEN];
static const char script_path[] = "/tmp/xiaoji-exec-script";
static const char text_path[] = "/tmp/xiaoji-exec-text";
static const char nox_path[] = "/tmp/xiaoji-exec-nox";
static const char nonl_path[] = "/tmp/xiaoji-exec-nonl";
static volatile int got_usr1;

static void on_usr1(int sig)
{
    (void)sig;
    got_usr1 = 1;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

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

static u64 slen(const char *s)
{
    u64 n = 0;
    while (s[n])
        n++;
    return n;
}

/* Appends a to dst (which holds a string), then b. */
static void cat2(char *dst, const char *a, const char *b)
{
    dst += slen(dst);
    while (*a)
        *dst++ = *a++;
    while (*b)
        *dst++ = *b++;
    *dst = 0;
}

static u64 parse_u(const char *s)
{
    u64 v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (u64)(*s++ - '0');
    return v;
}

static void fmt_u(char *dst, u64 v)
{
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (int i = 0; i < n; i++)
        dst[i] = tmp[n - 1 - i];
    dst[n] = 0;
}

static i64 sys_execve(const char *path, char **argv, char **envp)
{
    return sc6(SYS_EXECVE, (i64)path, (i64)argv, (i64)envp, 0, 0, 0);
}

/* wait4 writes a 32-bit status, so it goes through an i32 and is widened for the caller. */
static i64 wait_for(i64 pid, i64 *st)
{
    i32 status = -1;
    i64 rc = sc6(SYS_WAIT4, pid, (i64)&status, 0, 0, 0, 0);
    *st = status;
    return rc;
}

static int fd_open(u64 fd) { return sc6(SYS_FCNTL, (i64)fd, F_GETFD, 0, 0, 0, 0) >= 0; }

static void put_line(const char *label, const char *what)
{
    put(label);
    put(what);
    put("\n");
}

static const char *disposition(int sig)
{
    ksigaction act = {0, 0, 0, 0};
    sc6(SYS_RT_SIGACTION, sig, 0, (i64)&act, 8, 0, 0);
    if (act.handler == 0)
        return "default";
    if (act.handler == 1)
        return "ignored";
    return "caught";
}

/* The program an exec started: prints what it inherited, and exits 7. */
static int probe(int argc, char **argv)
{
    const char *val = "unset";
    for (char **e = xiaoji_envp; *e; e++)
        if (starts(*e, "XIAOJI_EXEC_TEST="))
            val = *e + 17;
    put_line("probe env: ", val);
    if (argc >= 4) {
        put_line("cloexec fd: ", fd_open(parse_u(argv[2])) ? "open" : "closed");
        put_line("plain fd: ", fd_open(parse_u(argv[3])) ? "open" : "closed");
    }
    put_line("usr1: ", disposition(SIGUSR1_N));
    put_line("term: ", disposition(SIGTERM_N));
    u64 mask = 0;
    sc6(SYS_RT_SIGPROCMASK, SIG_BLOCK, 0, (i64)&mask, 8, 0, 0);
    put_line("usr2 blocked: ", ((mask >> (SIGUSR2_N - 1)) & 1) ? "yes" : "no");
    kstack ss = {0, 0, 0, 0};
    sc6(SYS_SIGALTSTACK, 0, (i64)&ss, 0, 0, 0, 0);
    put_line("altstack: ", (ss.flags & SS_DISABLE) ? "disabled" : "enabled");

    /* The program's identity: /proc/self/exe, the thread name, and the argv bytes. */
    char exe[PATH_LEN] = "";
    i64 n = sc6(SYS_READLINKAT, AT_FDCWD, (i64)"/proc/self/exe", (i64)exe, PATH_LEN - 1, 0, 0);
    if (n > 0)
        exe[n] = 0;
    put_line("exe: ", exe);
    put_line("tid is pid: ", sc6(SYS_GETTID, 0, 0, 0, 0, 0, 0) == sc6(SYS_GETPID, 0, 0, 0, 0, 0, 0) ? "yes" : "no");
    char comm[16] = "";
    sc6(SYS_PRCTL, PR_GET_NAME, (i64)comm, 0, 0, 0, 0);
    put_line("comm: ", comm);
    char cl[512] = "";
    i64 cfd = sc6(SYS_OPENAT, AT_FDCWD, (i64)"/proc/self/cmdline", O_RDONLY, 0, 0, 0);
    i64 got = sc6(SYS_READ, cfd, (i64)cl, sizeof cl, 0, 0, 0);
    sc6(SYS_CLOSE, cfd, 0, 0, 0, 0, 0);
    u64 strings = 0;
    for (i64 i = 0; i < got; i++)
        if (cl[i] == 0)
            strings++;
    put("cmdline args: ");
    putu(strings);
    put("\n");
    put_line("cmdline arg 1: ", cl + slen(cl) + 1);
    return 7;
}

/* A forked child: execs path with argv, or exits EXEC_FAILED_RC. */
static void __attribute__((noinline, noreturn)) child_exec(const char *path, char **argv)
{
    sys_execve(path, argv, env_list);
    sys_exit_group(EXEC_FAILED_RC);
}

/* A forked child: execveat of an open descriptor with AT_EMPTY_PATH. */
static void __attribute__((noinline, noreturn)) child_execveat(int fd, char **argv)
{
    sc6(SYS_EXECVEAT, fd, (i64)"", (i64)argv, (i64)env_list, AT_EMPTY_PATH, 0);
    sys_exit_group(EXEC_FAILED_RC);
}

/* A vfork child: its own frame, below the parent's, since the stacks are shared. */
static void __attribute__((noinline, noreturn)) vfork_child(void)
{
    char *argv[] = {args_path, "vfork", "child", 0};
    sys_execve(args_path, argv, env_list);
    sys_exit_group(EXEC_FAILED_RC);
}

/* A thread, not the leader, that execs the probe; its stack is its own. The child thread
 * starts on the new stack in xp_thread_clone (not in C, whose frame it must not use: its
 * sp is the new stack, so a C local would live above that stack's top). */
static char thread_stack[16384] __attribute__((aligned(16)));
static char *thread_argv[3];
void xp_thread_exec(void) __attribute__((noreturn));
void xp_thread_exec(void)
{
    sys_execve(thread_argv[0], thread_argv, env_list);
    sys_exit_group(EXEC_FAILED_RC);
}
/* clone(flags, newsp) for a thread: the parent gets the tid; the child calls xp_thread_exec. */
i64 xp_thread_clone(i64 flags, i64 newsp);
__asm__(".text\n"
        ".global xp_thread_clone\n"
        "xp_thread_clone:\n"
        "  mov x8, #220\n"
        "  svc #0\n"
        "  cbnz x0, 1f\n"
        "  bl xp_thread_exec\n"
        "1:\n"
        "  ret\n");

/* A forked process whose leader thread sleeps while a second thread execs the probe. The
 * exec must end the other threads and make the exec'ing thread the leader. */
static void __attribute__((noinline, noreturn)) leader_child(const char *unused, char **unused2)
{
    (void)unused;
    (void)unused2;
    xp_thread_clone(CLONE_THREAD_FLAGS, (i64)(thread_stack + sizeof thread_stack));
    u64 ts[2] = {1, 0}; /* one second: the probe ends the process long before */
    sc6(SYS_NANOSLEEP, (i64)ts, 0, 0, 0, 0, 0);
    sys_exit_group(42);
}

/* Forks; the child runs f(path, argv); returns the child's pid, with its status in *st. */
static i64 fork_run(void (*f)(const char *, char **), const char *path, char **argv, i64 *st)
{
    i64 pid = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (pid == 0)
        f(path, argv);
    wait_for(pid, st);
    return pid;
}

static void report_status(const char *label, i64 st)
{
    put(label);
    putd(st);
    put("\n");
}

static void make_file(const char *path, u32 mode, const char *data, u64 len)
{
    i64 fd = sc6(SYS_OPENAT, AT_FDCWD, (i64)path, O_WRONLY | O_CREAT | O_TRUNC, mode, 0, 0);
    if (fd < 0)
        return;
    sc6(64, fd, (i64)data, (i64)len, 0, 0, 0); /* write */
    sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && streq(argv[1], "probe"))
        return probe(argc, argv);

    /* The args binary is the sibling of this one in the build directory. */
    i64 n = sc6(SYS_READLINKAT, AT_FDCWD, (i64)"/proc/self/exe", (i64)self_path, PATH_LEN - 1, 0, 0);
    if (n <= 0) {
        put("exec: cannot read /proc/self/exe\n");
        return 1;
    }
    self_path[n] = 0;
    char dir[PATH_LEN];
    dir[0] = 0;
    cat2(dir, self_path, "");
    for (u64 i = slen(dir); i > 0; i--)
        if (dir[i - 1] == '/') {
            dir[i - 1] = 0;
            break;
        }
    cat2(args_path, dir, "/args-O0");

    char *none[] = {"exec", 0};
    i64 st = 0;

    /* 1. Failed execs leave the caller running. */
    i64 r = sys_execve("/nonexistent/xiaoji-exec", none, env_list);
    put("execve ENOENT: ");
    putd(r);
    put("\n");
    r = sc6(SYS_EXECVEAT, AT_FDCWD, (i64)"/nonexistent/xiaoji-exec", (i64)none, (i64)env_list, 0, 0);
    put("execveat ENOENT: ");
    putd(r);
    put("\n");
    r = sys_execve("/tmp", none, env_list);
    put("execve directory: ");
    putd(r);
    put("\n");
    make_file(text_path, 0755, "plain text, not a program\n", 26);
    r = sys_execve(text_path, none, env_list);
    put("execve text without #!: ");
    putd(r);
    put("\n");
    make_file(nox_path, 0644, "plain text, not a program\n", 26);
    r = sys_execve(nox_path, none, env_list);
    put("execve no exec bit: ");
    putd(r);
    put("\n");
    static char big[BIG_LEN];
    for (u64 i = 0; i < BIG_LEN - 1; i++)
        big[i] = 'a';
    big[BIG_LEN - 1] = 0;
    char *big_argv[] = {"exec", big, 0};
    r = sys_execve(args_path, big_argv, env_list);
    put("execve E2BIG: ");
    putd(r);
    put("\n");
    put("caller still running\n");

    /* 2. fork, then execve of the args binary with argv and an environment. */
    char *av[] = {args_path, "alpha", "beta", 0};
    fork_run(child_exec, args_path, av, &st);
    report_status("fork+execve args status: ", st);

    /* 3. execveat with AT_EMPTY_PATH, through a descriptor of the binary. */
    i64 fd = sc6(SYS_OPENAT, AT_FDCWD, (i64)args_path, O_RDONLY, 0, 0, 0);
    char *eav[] = {args_path, "empty", "path", 0};
    i64 pid = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (pid == 0)
        child_execveat((int)fd, eav);
    wait_for(pid, &st);
    report_status("execveat AT_EMPTY_PATH status: ", st);
    sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);

    /* 4. A #! script whose interpreter is args, with one argument (the rest of the line). */
    char script[PATH_LEN + 32] = "";
    cat2(script, "#!", args_path);
    cat2(script, " one two", "\n");
    make_file(script_path, 0755, script, slen(script));
    char *sav[] = {"xiaoji-exec-script", "a1", "a2", 0};
    fork_run(child_exec, script_path, sav, &st);
    report_status("#! script status: ", st);

    /* 5. vfork: the child execs, and the parent resumes with the child's pid. */
    pid = sc6(SYS_CLONE, CLONE_VM | CLONE_VFORK | SIGCHLD_N, 0, 0, 0, 0, 0);
    if (pid == 0)
        vfork_child();
    wait_for(pid, &st);
    report_status("vfork+execve status: ", st);

    /* 6. FD_CLOEXEC across exec: the write end is close-on-exec, the read end is not. */
    int32_t fds[2] = {-1, -1};
    sc6(SYS_PIPE2, (i64)fds, 0, 0, 0, 0, 0);
    sc6(SYS_FCNTL, fds[1], F_SETFD, FD_CLOEXEC, 0, 0, 0);
    char wbuf[24] = "", rbuf[24] = "";
    fmt_u(wbuf, (u64)fds[1]);
    fmt_u(rbuf, (u64)fds[0]);
    char *pav[] = {self_path, "probe", wbuf, rbuf, 0};
    fork_run(child_exec, self_path, pav, &st);
    report_status("cloexec probe status: ", st);
    sc6(SYS_CLOSE, fds[0], 0, 0, 0, 0, 0);
    sc6(SYS_CLOSE, fds[1], 0, 0, 0, 0, 0);

    /* 7. Signals: a caught signal goes back to the default, an ignored one stays ignored,
     * and the blocked mask stays while the alternate stack is disabled. */
    ksigaction caught = {(u64)(uintptr_t)on_usr1, 0, 0, 0};
    ksigaction ignored = {1, 0, 0, 0};
    sc6(SYS_RT_SIGACTION, SIGUSR1_N, (i64)&caught, 0, 8, 0, 0);
    sc6(SYS_RT_SIGACTION, SIGTERM_N, (i64)&ignored, 0, 8, 0, 0);
    u64 block = (u64)1 << (SIGUSR2_N - 1);
    sc6(SYS_RT_SIGPROCMASK, SIG_BLOCK, (i64)&block, 0, 8, 0, 0);
    static char altbuf[16384];
    kstack ss = {(u64)(uintptr_t)altbuf, 0, 0, sizeof altbuf};
    sc6(SYS_SIGALTSTACK, (i64)&ss, 0, 0, 0, 0, 0);
    char *sgav[] = {self_path, "probe", 0};
    fork_run(child_exec, self_path, sgav, &st);
    report_status("signal probe status: ", st);

    /* 8. nanosleep: a tv_nsec out of range is EINVAL; getrusage reports usage (the values
     * differ from native, so only whether they are set is printed). */
    u64 bad_hi[2] = {0, 1000000000ull}, bad_neg[2] = {0, (u64)-1}, short_ok[2] = {0, 1000};
    put("nanosleep nsec=1e9: ");
    putd(sc6(SYS_NANOSLEEP, (i64)bad_hi, 0, 0, 0, 0, 0));
    put("\n");
    put("nanosleep nsec=-1: ");
    putd(sc6(SYS_NANOSLEEP, (i64)bad_neg, 0, 0, 0, 0, 0));
    put("\n");
    put("nanosleep 1us: ");
    putd(sc6(SYS_NANOSLEEP, (i64)short_ok, 0, 0, 0, 0, 0));
    put("\n");
    u64 ru[18] = {0};
    put("getrusage self: ");
    putd(sc6(SYS_GETRUSAGE, 0, (i64)ru, 0, 0, 0, 0));
    put(ru[4] > 0 ? " maxrss set\n" : " maxrss zero\n");
    put("getrusage who=2: ");
    putd(sc6(SYS_GETRUSAGE, 2, (i64)ru, 0, 0, 0, 0));
    put("\n");
    put("getrusage children: ");
    putd(sc6(SYS_GETRUSAGE, -1, (i64)ru, 0, 0, 0, 0));
    put(ru[4] > 0 ? " maxrss set\n" : " maxrss zero\n");

    /* 9. A #! line with no trailing newline runs (the interpreter gets "x", then the script
     * path). */
    char nonl_text[PATH_LEN + 16] = "";
    cat2(nonl_text, "#!", args_path);
    cat2(nonl_text, " x", "");
    make_file(nonl_path, 0755, nonl_text, slen(nonl_text));
    char *nav[] = {"xiaoji-exec-nonl", 0};
    fork_run(child_exec, nonl_path, nav, &st);
    report_status("shebang without newline status: ", st);

    /* 10. A thread that is not the leader execs. The probe checks that it is the leader now
     * (gettid == getpid). */
    thread_argv[0] = self_path;
    thread_argv[1] = "probe";
    thread_argv[2] = 0;
    fork_run(leader_child, self_path, thread_argv, &st);
    report_status("thread execve status: ", st);

    put("exec done\n"); /* the three /tmp files keep fixed names: each run rewrites them */
    return 0;
}
