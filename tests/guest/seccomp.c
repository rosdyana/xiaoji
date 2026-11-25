/* seccomp (B19): prctl(PR_SET_SECCOMP) and seccomp(2), strict mode and classic BPF filters. Each case runs in a
 * forked child (a filter cannot be removed), and the parent prints how the child ended. The checks: the action
 * queries, the program checker (EINVAL cases), the NO_NEW_PRIVS rule (EACCES without CAP_SYS_ADMIN), the actions
 * (ERRNO, TRAP with its SIGSYS siginfo and ucontext, KILL_PROCESS, KILL_THREAD, TRACE, LOG, USER_NOTIF, an unknown
 * action), the precedence of several filters, argument-dependent filters, the classic BPF arithmetic and scratch
 * memory, inheritance across fork and threads, TSYNC (and its failure), and the instruction limit. Runs as root in
 * the oracle (the EACCES case drops the effective capabilities itself). Nothing printed depends on a pid or an
 * address. */
#include "sys.h"

#define SYS_GETPID 172
#define SYS_GETPPID 173
#define SYS_GETTID 178
#define SYS_UNAME 160
#define SYS_PRCTL 167
#define SYS_SECCOMP 277
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SYS_EXIT 93
#define SYS_FUTEX 98
#define SYS_RT_SIGACTION 134
#define SYS_RT_SIGPROCMASK 135
#define SYS_CAPGET 90
#define SYS_CAPSET 91
#define SYS_GETPRIORITY 141

#define PR_GET_SECCOMP 21
#define PR_SET_SECCOMP 22
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39

#define SECCOMP_MODE_STRICT 1
#define SECCOMP_MODE_FILTER 2
#define SECCOMP_SET_MODE_STRICT 0
#define SECCOMP_SET_MODE_FILTER 1
#define SECCOMP_GET_ACTION_AVAIL 2
#define SECCOMP_GET_NOTIF_SIZES 3

#define FLAG_TSYNC 1u
#define FLAG_LOG 2u
#define FLAG_SPEC_ALLOW 4u
#define FLAG_NEW_LISTENER 8u
#define FLAG_TSYNC_ESRCH 16u

#define RET_KILL_PROCESS 0x80000000u
#define RET_KILL_THREAD 0x00000000u
#define RET_TRAP 0x00030000u
#define RET_ERRNO 0x00050000u
#define RET_USER_NOTIF 0x7fc00000u
#define RET_TRACE 0x7ff00000u
#define RET_LOG 0x7ffc0000u
#define RET_ALLOW 0x7fff0000u

#define AUDIT_ARCH_AARCH64 0xC00000B7u

/* Classic BPF. */
#define BPF_LD 0x00
#define BPF_LDX 0x01
#define BPF_ST 0x02
#define BPF_STX 0x03
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_RET 0x06
#define BPF_MISC 0x07
#define BPF_W 0x00
#define BPF_H 0x08
#define BPF_B 0x10
#define BPF_IMM 0x00
#define BPF_ABS 0x20
#define BPF_IND 0x40
#define BPF_MEM 0x60
#define BPF_LEN 0x80
#define BPF_ADD 0x00
#define BPF_SUB 0x10
#define BPF_MUL 0x20
#define BPF_DIV 0x30
#define BPF_OR 0x40
#define BPF_AND 0x50
#define BPF_LSH 0x60
#define BPF_RSH 0x70
#define BPF_NEG 0x80
#define BPF_MOD 0x90
#define BPF_XOR 0xa0
#define BPF_JA 0x00
#define BPF_JEQ 0x10
#define BPF_JGT 0x20
#define BPF_JGE 0x30
#define BPF_JSET 0x40
#define BPF_K 0x00
#define BPF_X 0x08
#define BPF_A 0x10
#define BPF_TAX 0x00
#define BPF_TXA 0x80

typedef struct {
    unsigned short code;
    unsigned char jt, jf;
    u32 k;
} sock_filter;

typedef struct {
    unsigned short len;
    sock_filter *filter;
} sock_fprog;

#define STMT(c, kk) ((sock_filter){(unsigned short)(c), 0, 0, (u32)(kk)})
#define JUMP(c, kk, t, f) ((sock_filter){(unsigned short)(c), (unsigned char)(t), (unsigned char)(f), (u32)(kk)})

/* seccomp_data offsets */
#define OFF_NR 0
#define OFF_ARCH 4
#define OFF_IP_LO 8
#define OFF_IP_HI 12
#define OFF_ARG_LO(n) (16 + 8 * (n))
#define OFF_ARG_HI(n) (20 + 8 * (n))

static i64 prctl(i64 o, i64 a, i64 b, i64 c, i64 d) { return sc6(SYS_PRCTL, o, a, b, c, d, 0); }
static i64 seccomp(i64 op, i64 flags, const void *args) { return sc6(SYS_SECCOMP, op, flags, (i64)args, 0, 0, 0); }
static i64 getppid_(void) { return sc6(SYS_GETPPID, 0, 0, 0, 0, 0, 0); }
static i64 getpid_(void) { return sc6(SYS_GETPID, 0, 0, 0, 0, 0, 0); }
static i64 gettid_(void) { return sc6(SYS_GETTID, 0, 0, 0, 0, 0, 0); }
static __attribute__((noreturn)) void thread_exit(int code)
{
    sc6(SYS_EXIT, code, 0, 0, 0, 0, 0);
    for (;;) {
    }
}

static void line(const char *name, i64 rc)
{
    put(name);
    put(" = ");
    putd(rc);
    put("\n");
}

static void linex(const char *name, u64 v)
{
    put(name);
    put(" = ");
    putx(v);
    put("\n");
}

static i64 install(i64 flags, sock_filter *f, unsigned n)
{
    sock_fprog p = {(unsigned short)n, f};
    return seccomp(SECCOMP_SET_MODE_FILTER, flags, &p);
}

static i64 install_prctl(sock_filter *f, unsigned n)
{
    sock_fprog p = {(unsigned short)n, f};
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, (i64)&p, 0, 0);
}

/* A filter that returns ret for syscall nr and allows the rest (with the arch check, as minijail writes it). */
static i64 install_nr(i64 flags, u32 nr, u32 ret)
{
    sock_filter f[] = {
        STMT(BPF_LD | BPF_W | BPF_ABS, OFF_ARCH),
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
        STMT(BPF_RET | BPF_K, RET_KILL_PROCESS),
        STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1),
        STMT(BPF_RET | BPF_K, ret),
        STMT(BPF_RET | BPF_K, RET_ALLOW),
    };
    return install(flags, f, sizeof f / sizeof f[0]);
}

static i64 nnp(void) { return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0); }

/* ---------- processes ---------- */

static i64 fork_(void) { return sc6(SYS_CLONE, 17 /* SIGCHLD */, 0, 0, 0, 0, 0); }

/* Waits for the child and prints how it ended. */
static void reap(const char *name, i64 pid)
{
    i32 st = 0;
    i64 rc = sc6(SYS_WAIT4, pid, (i64)&st, 0, 0, 0, 0);
    put(name);
    if (rc != pid) {
        put(": wait failed ");
        putd(rc);
        put("\n");
        return;
    }
    if ((st & 0x7f) == 0) {
        put(": exit ");
        putd((st >> 8) & 0xff);
    } else {
        put(": signal ");
        putd(st & 0x7f); /* WTERMSIG; the core-dump bit is not printed */
    }
    put("\n");
}

/* ---------- threads ---------- */

#define THREAD_FLAGS 0x350F00ull
#define STACK_BYTES 65536
extern i64 xt_spawn(u64 flags, void *stack_top, u32 *ptid, u32 *ctid, void (*fn)(void *), void *arg);
__asm__(".text\n"
        ".global xt_spawn\n"
        "xt_spawn:\n"
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

static unsigned char stacks[2][STACK_BYTES] __attribute__((aligned(16)));
static u32 ctid[2];

static void futex_wait(u32 *a, u32 v) { sc6(SYS_FUTEX, (i64)a, 0, v, 0, 0, 0); }
static void futex_wake(u32 *a) { sc6(SYS_FUTEX, (i64)a, 1, 0x7fffffff, 0, 0, 0); }

static void join(int i)
{
    u32 t;
    while ((t = __atomic_load_n(&ctid[i], __ATOMIC_ACQUIRE)) != 0)
        futex_wait(&ctid[i], t);
}

static i64 spawn(int i, void (*fn)(void *), void *arg)
{
    return xt_spawn(THREAD_FLAGS, stacks[i] + STACK_BYTES, &ctid[i], &ctid[i], fn, arg);
}

/* A step counter the threads and the main thread hand over with futexes. */
static u32 step;
static void step_to(u32 v)
{
    __atomic_store_n(&step, v, __ATOMIC_RELEASE);
    futex_wake(&step);
}
static void wait_step(u32 v)
{
    u32 s;
    while ((s = __atomic_load_n(&step, __ATOMIC_ACQUIRE)) < v)
        futex_wait(&step, s);
}

/* ---------- signals ---------- */

typedef struct {
    u64 handler, flags, restorer, mask;
} ksigaction;

typedef struct {
    i32 signo, err, code, pad;
    u64 call_addr; /* offset 16 */
    i32 syscall;   /* offset 24 */
    u32 arch;      /* offset 28 */
    u64 rest[12];
} ksysinfo;

typedef struct {
    u64 fault_address;
    u64 regs[31];
    u64 sp, pc, pstate;
} kmcontext;

typedef struct {
    u64 uc_flags;
    u64 uc_link;
    u64 ss_sp;
    i32 ss_flags;
    u64 ss_size;
    u64 uc_sigmask;
    u8 unused[120];
    kmcontext mc __attribute__((aligned(16)));
} kucontext;

__asm__(".text\n"
        ".global sc_restore\n"
        "sc_restore:\n"
        "  mov x8, #139\n"
        "  svc #0\n");
extern void sc_restore(void);

#define SA_SIGINFO 0x4ull
#define SA_RESTORER 0x04000000ull
#define SIGSYS 31

static void sigaction_(int sig, u64 handler, u64 flags)
{
    ksigaction sa = {handler, flags | SA_RESTORER, (u64)sc_restore, 0};
    sc6(SYS_RT_SIGACTION, sig, (i64)&sa, 0, 8, 0, 0);
}

/* getppid with x0 = 77 and x1 = 78, at a known place: trap_after is the address after its svc. */
extern i64 trap_call(void);
extern char trap_after[];
__asm__(".text\n"
        ".global trap_call\n"
        ".global trap_after\n"
        "trap_call:\n"
        "  mov x8, #173\n"
        "  mov x0, #77\n"
        "  mov x1, #78\n"
        "  svc #0\n"
        "trap_after:\n"
        "  ret\n");

static volatile int h_count, h_signo, h_code, h_err, h_sysno, h_arch_ok, h_addr_ok, h_pc_ok, h_x8, h_fault0, h_sp_ok;
static volatile u64 h_x0, h_x1;

static void sys_handler(int sig, ksysinfo *info, kucontext *uc)
{
    h_count++;
    h_signo = sig;
    h_code = info->code;
    h_err = info->err;
    h_sysno = info->syscall;
    h_arch_ok = info->arch == AUDIT_ARCH_AARCH64;
    h_addr_ok = info->call_addr == (u64)trap_after;
    h_pc_ok = uc->mc.pc == (u64)trap_after;
    h_x0 = uc->mc.regs[0];
    h_x1 = uc->mc.regs[1];
    h_x8 = (int)uc->mc.regs[8];
    h_fault0 = uc->mc.fault_address == 0;
    uc->mc.regs[0] = 1234; /* the "result" of the trapped call */
}

/* ---------- procfs ---------- */

static char fbuf[4096];

static i64 read_file(const char *path)
{
    i64 fd = sc6(56 /* openat */, -100, (i64)path, 0, 0, 0, 0);
    if (fd < 0)
        return fd;
    i64 n = sc6(63 /* read */, fd, (i64)fbuf, sizeof fbuf - 1, 0, 0, 0);
    sc6(57 /* close */, fd, 0, 0, 0, 0, 0);
    fbuf[n > 0 ? n : 0] = 0;
    return n;
}

static int starts(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}

/* The NoNewPrivs, Seccomp and Seccomp_filters lines of /proc/self/status. */
static void status_lines(const char *tag)
{
    if (read_file("/proc/self/status") < 0) {
        put("status: unreadable\n");
        return;
    }
    for (char *l = fbuf; *l;) {
        char *e = l;
        while (*e && *e != '\n')
            e++;
        if (starts(l, "NoNewPrivs:") || starts(l, "Seccomp:") || starts(l, "Seccomp_filters:")) {
            put(tag);
            put(": ");
            sys_write(1, l, (size_t)(e - l));
            put("\n");
        }
        l = *e ? e + 1 : e;
    }
}

/* ---------- cases ---------- */

static void case_queries(void)
{
    line("get_seccomp", prctl(PR_GET_SECCOMP, 0, 0, 0, 0));
    status_lines("status before");
    if (read_file("/proc/sys/kernel/seccomp/actions_avail") > 0) {
        put("actions_avail: ");
        put(fbuf);
    }
    if (read_file("/proc/sys/kernel/seccomp/actions_logged") > 0) {
        put("actions_logged: ");
        put(fbuf);
    }
    static const u32 acts[] = {RET_KILL_PROCESS, RET_KILL_THREAD, RET_TRAP, RET_ERRNO, RET_USER_NOTIF,
                               RET_TRACE, RET_LOG, RET_ALLOW, 0x00010000u, RET_ERRNO | 5, 0x7ffe0000u};
    for (unsigned i = 0; i < sizeof acts / sizeof acts[0]; i++) {
        u32 a = acts[i];
        put("action_avail ");
        putx(a);
        put(" = ");
        putd(seccomp(SECCOMP_GET_ACTION_AVAIL, 0, &a));
        put("\n");
    }
    u32 a = RET_ALLOW;
    line("action_avail flags 1", seccomp(SECCOMP_GET_ACTION_AVAIL, 1, &a));
    line("action_avail null", seccomp(SECCOMP_GET_ACTION_AVAIL, 0, 0));
    unsigned short sizes[3] = {0, 0, 0};
    line("notif_sizes", seccomp(SECCOMP_GET_NOTIF_SIZES, 0, sizes));
    put("notif_sizes values = ");
    putu(sizes[0]);
    put(" ");
    putu(sizes[1]);
    put(" ");
    putu(sizes[2]);
    put("\n");
    line("notif_sizes flags 1", seccomp(SECCOMP_GET_NOTIF_SIZES, 1, sizes));
    line("op 99", seccomp(99, 0, 0));
    line("strict flags 1", seccomp(SECCOMP_SET_MODE_STRICT, 1, 0));
    line("strict args", seccomp(SECCOMP_SET_MODE_STRICT, 0, sizes));
    line("prctl set mode 3", prctl(PR_SET_SECCOMP, 3, 0, 0, 0));
    line("prctl set mode 0", prctl(PR_SET_SECCOMP, 0, 0, 0, 0));
    line("get_seccomp after", prctl(PR_GET_SECCOMP, 0, 0, 0, 0));
}

typedef struct {
    u32 hdr_version;
    i32 hdr_pid;
} cap_hdr_t;
typedef struct {
    u32 eff, perm, inh;
} cap_data_t;

/* NO_NEW_PRIVS: without it and without CAP_SYS_ADMIN, a filter is refused (EACCES); with CAP_SYS_ADMIN or with it,
 * the filter goes in. */
static void case_nnp(void)
{
    i64 pid = fork_();
    if (pid == 0) {
        cap_hdr_t h = {0x20080522, 0};
        cap_data_t d[2];
        sc6(SYS_CAPGET, (i64)&h, (i64)d, 0, 0, 0, 0);
        d[0].eff = 0;
        d[1].eff = 0;
        line("nnp: drop effective caps", sc6(SYS_CAPSET, (i64)&h, (i64)d, 0, 0, 0, 0));
        line("nnp: get", prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0));
        line("nnp: filter without nnp (seccomp)", install_nr(0, SYS_GETPPID, RET_ERRNO | 5));
        sock_filter f[] = {STMT(BPF_RET | BPF_K, RET_ALLOW)};
        line("nnp: filter without nnp (prctl)", install_prctl(f, 1));
        line("nnp: get_seccomp", prctl(PR_GET_SECCOMP, 0, 0, 0, 0));
        line("nnp: strict needs no nnp: mode", prctl(PR_GET_SECCOMP, 0, 0, 0, 0));
        line("nnp: set", nnp());
        line("nnp: filter with nnp", install_nr(0, SYS_GETPPID, RET_ERRNO | 5));
        line("nnp: getppid", getppid_());
        line("nnp: get_seccomp now", prctl(PR_GET_SECCOMP, 0, 0, 0, 0));
        sys_exit_group(0);
    }
    reap("nnp child", pid);
    pid = fork_();
    if (pid == 0) {
        /* root with CAP_SYS_ADMIN: no NO_NEW_PRIVS needed */
        line("cap_sys_admin: filter without nnp", install_nr(0, SYS_GETPPID, RET_ERRNO | 6));
        line("cap_sys_admin: getppid", getppid_());
        line("cap_sys_admin: nnp still", prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0));
        sys_exit_group(0);
    }
    reap("cap_sys_admin child", pid);
}

static void bad(const char *name, sock_filter *f, unsigned n) { line(name, install(0, f, n)); }

/* The checker: every program here is refused with EINVAL (or EFAULT for bad pointers). */
static void case_invalid(void)
{
    i64 pid = fork_();
    if (pid != 0) {
        reap("invalid child", pid);
        return;
    }
    nnp();
    static sock_filter big[4097];
    for (int i = 0; i < 4097; i++)
        big[i] = STMT(BPF_RET | BPF_K, RET_ALLOW);
    sock_filter ok[] = {STMT(BPF_RET | BPF_K, RET_ALLOW)};
    bad("len 0", ok, 0);
    bad("len 4097", big, 4097);
    {
        sock_filter f[] = {STMT(BPF_LD | BPF_W | BPF_ABS, 0)};
        bad("no ret at end", f, 1);
    }
    {
        sock_filter f[] = {JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, 0), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("jt out of range", f, 2);
    }
    {
        sock_filter f[] = {JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 1), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("jf out of range", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_JMP | BPF_JA, 1), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("ja out of range", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_LD | BPF_W | BPF_ABS, 64), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("ld abs 64", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_LD | BPF_W | BPF_ABS, 2), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("ld abs unaligned", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_LD | BPF_H | BPF_ABS, 0), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("ld h abs", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_LD | BPF_B | BPF_ABS, 0), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("ld b abs", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_LD | BPF_W | BPF_IND, 0), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("ld w ind", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_LDX | BPF_B | 0xa0 /* MSH */, 0), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("ldx msh", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_ALU | BPF_MOD | BPF_K, 3), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("alu mod", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_ALU | BPF_DIV | BPF_K, 0), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("div k 0", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_ALU | BPF_LSH | BPF_K, 32), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("lsh k 32", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_ALU | BPF_RSH | BPF_K, 40), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("rsh k 40", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_LD | BPF_MEM, 0), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("ld mem before st", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_ST, 16), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("st 16", f, 2);
    }
    {
        /* M[1] is stored on one path only */
        sock_filter f[] = {
            STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
            JUMP(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, 1),
            STMT(BPF_ST, 1),
            STMT(BPF_LDX | BPF_MEM, 1),
            STMT(BPF_RET | BPF_K, RET_ALLOW),
        };
        bad("ldx mem on one path", f, 5);
    }
    {
        sock_filter f[] = {STMT(BPF_RET | BPF_X, 0), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("ret x", f, 2);
    }
    {
        sock_filter f[] = {STMT(0xffff, 0), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("code 0xffff", f, 2);
    }
    {
        sock_filter f[] = {STMT(BPF_LDX | BPF_W | BPF_ABS, 0), STMT(BPF_RET | BPF_K, RET_ALLOW)};
        bad("ldx w abs", f, 2);
    }
    line("fprog null", seccomp(SECCOMP_SET_MODE_FILTER, 0, 0));
    {
        sock_fprog p = {1, (sock_filter *)8};
        line("filter pointer bad", seccomp(SECCOMP_SET_MODE_FILTER, 0, &p));
    }
    line("flag 1<<10", install(1 << 10, ok, 1));
    line("tsync|listener", install(FLAG_TSYNC | FLAG_NEW_LISTENER, ok, 1));
    line("get_seccomp still", prctl(PR_GET_SECCOMP, 0, 0, 0, 0));
    /* The valid edge cases are accepted. */
    {
        sock_filter f[] = {
            STMT(BPF_LD | BPF_W | BPF_LEN, 0),
            JUMP(BPF_JMP | BPF_JEQ | BPF_K, 64, 1, 0),
            STMT(BPF_RET | BPF_K, RET_KILL_PROCESS),
            STMT(BPF_LDX | BPF_W | BPF_LEN, 0),
            STMT(BPF_MISC | BPF_TXA, 0),
            JUMP(BPF_JMP | BPF_JEQ | BPF_K, 64, 1, 0),
            STMT(BPF_RET | BPF_K, RET_KILL_PROCESS),
            STMT(BPF_LD | BPF_W | BPF_ABS, 60),
            STMT(BPF_ALU | BPF_RSH | BPF_K, 31),
            STMT(BPF_ALU | BPF_LSH | BPF_K, 0),
            STMT(BPF_JMP | BPF_JA, 0),
            STMT(BPF_RET | BPF_K, RET_ALLOW),
        };
        line("len loads, ja 0, abs 60", install(0, f, sizeof f / sizeof f[0]));
    }
    line("getpid ok", getpid_() > 0);
    sys_exit_group(0);
}

/* ERRNO, its data clamped to 4095, TRACE (no tracer: ENOSYS), USER_NOTIF (no listener: ENOSYS), LOG (allowed). */
static void case_actions(void)
{
    i64 pid = fork_();
    if (pid != 0) {
        reap("actions child", pid);
        return;
    }
    nnp();
    line("errno 5 install", install_nr(0, SYS_GETPPID, RET_ERRNO | 5));
    line("errno 5 getppid", getppid_());
    line("getpid allowed", getpid_() > 0);
    line("errno 0xffff install", install_nr(0, SYS_GETPRIORITY, RET_ERRNO | 0xffff));
    line("errno 0xffff getpriority", sc6(SYS_GETPRIORITY, 0, 0, 0, 0, 0, 0));
    line("errno 0 install", install_nr(0, SYS_GETTID, RET_ERRNO | 0));
    line("errno 0 gettid", gettid_());
    line("trace install", install_nr(0, SYS_UNAME, RET_TRACE | 3));
    line("trace uname", sc6(SYS_UNAME, 0, 0, 0, 0, 0, 0));
    line("log install", install_nr(FLAG_LOG, 62 /* lseek */, RET_LOG));
    line("log lseek", sc6(62, 1, 0, 1, 0, 0, 0) >= -1000 ? 1 : 0);
    line("user_notif install", install_nr(0, 124 /* sched_yield */, RET_USER_NOTIF));
    line("user_notif sched_yield", sc6(124, 0, 0, 0, 0, 0, 0));
    line("get_seccomp", prctl(PR_GET_SECCOMP, 0, 0, 0, 0));
    status_lines("status filters");
    line("strict from filter mode", prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT, 0, 0, 0));
    line("strict from filter mode (seccomp)", seccomp(SECCOMP_SET_MODE_STRICT, 0, 0));
    sys_exit_group(0);
}

/* Several filters: the most restrictive action wins (KILL_PROCESS < KILL_THREAD < TRAP < ERRNO < USER_NOTIF < TRACE <
 * LOG < ALLOW), and for the same action the newest filter's data. */
static void case_precedence(void)
{
    i64 pid = fork_();
    if (pid == 0) {
        nnp();
        install_nr(0, SYS_GETPPID, RET_ERRNO | 1);
        install_nr(0, SYS_GETPPID, RET_ERRNO | 2);
        line("prec: errno 1 then errno 2", getppid_());
        install_nr(0, SYS_GETPPID, RET_ALLOW);
        line("prec: then allow", getppid_());
        install_nr(0, SYS_GETPPID, RET_TRACE);
        line("prec: then trace", getppid_());
        install_nr(0, SYS_GETTID, RET_TRACE);
        install_nr(0, SYS_GETTID, RET_LOG);
        line("prec: trace then log", gettid_());
        install_nr(0, SYS_GETPID, RET_ERRNO | 9);
        install_nr(0, SYS_GETPID, RET_KILL_PROCESS);
        install_nr(0, SYS_GETPID, RET_TRAP);
        getpid_();
        put("prec: not reached\n");
        sys_exit_group(0);
    }
    reap("prec: errno, kill_process, trap", pid);
}

/* Arguments, the instruction pointer and the arithmetic. */
static void case_args(void)
{
    i64 pid = fork_();
    if (pid != 0) {
        reap("args child", pid);
        return;
    }
    nnp();
    /* uname(buf): ERRNO 7 when the high word of arg0 is 0xdead, else allowed */
    sock_filter f[] = {
        STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_UNAME, 0, 3),
        STMT(BPF_LD | BPF_W | BPF_ABS, OFF_ARG_HI(0)),
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xdead, 0, 1),
        STMT(BPF_RET | BPF_K, RET_ERRNO | 7),
        STMT(BPF_RET | BPF_K, RET_ALLOW),
    };
    line("args: install", install(0, f, sizeof f / sizeof f[0]));
    static u8 ubuf[390 * 2];
    line("args: uname(buf)", sc6(SYS_UNAME, (i64)ubuf, 0, 0, 0, 0, 0));
    line("args: uname(0xdead00000000)", sc6(SYS_UNAME, (i64)0xdead00000000ll, 0, 0, 0, 0, 0));
    line("args: uname(0xbeef00000000)", sc6(SYS_UNAME, (i64)0xbeef00000000ll, 0, 0, 0, 0, 0));
    /* getpriority(which, who): ERRNO (arg5 low word & 0xff) + (arg4 low * 3) when JSET of bit 4 in arg1, via M[], X,
     * TAX/TXA, ADD/SUB/MUL/DIV/AND/OR/XOR/LSH/RSH/NEG */
    sock_filter g[] = {
        STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),                /* 0 */
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_GETPRIORITY, 1, 0), /* 1 */
        STMT(BPF_RET | BPF_K, RET_ALLOW),                      /* 2 */
        STMT(BPF_LD | BPF_W | BPF_ABS, OFF_ARG_LO(1)),         /* 3 */
        JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x10, 1, 0),          /* 4 */
        STMT(BPF_RET | BPF_K, RET_ALLOW),                      /* 5 */
        STMT(BPF_LD | BPF_W | BPF_ABS, OFF_ARG_LO(4)),         /* 6 */
        STMT(BPF_ALU | BPF_MUL | BPF_K, 3),                    /* 7 */
        STMT(BPF_ST, 2),                                       /* 8 */
        STMT(BPF_LD | BPF_W | BPF_ABS, OFF_ARG_LO(5)),         /* 9 */
        STMT(BPF_ALU | BPF_AND | BPF_K, 0xff),                 /* 10 */
        STMT(BPF_LDX | BPF_MEM, 2),                            /* 11 */
        STMT(BPF_ALU | BPF_ADD | BPF_X, 0),                    /* 12: A = arg5&0xff + arg4*3 */
        STMT(BPF_MISC | BPF_TAX, 0),                           /* 13 */
        STMT(BPF_LD | BPF_IMM, 1000),                          /* 14 */
        STMT(BPF_ALU | BPF_SUB | BPF_X, 0),                    /* 15: A = 1000 - v */
        STMT(BPF_ALU | BPF_NEG, 0),                            /* 16: A = v - 1000 */
        STMT(BPF_ALU | BPF_ADD | BPF_K, 1000),                 /* 17: A = v */
        STMT(BPF_ALU | BPF_LSH | BPF_K, 4),                    /* 18 */
        STMT(BPF_ALU | BPF_RSH | BPF_K, 4),                    /* 19 */
        STMT(BPF_ALU | BPF_XOR | BPF_K, 0x5),                  /* 20 */
        STMT(BPF_ALU | BPF_XOR | BPF_K, 0x5),                  /* 21 */
        STMT(BPF_ALU | BPF_OR | BPF_K, 0),                     /* 22 */
        STMT(BPF_LDX | BPF_IMM, 2),                            /* 23 */
        STMT(BPF_ALU | BPF_MUL | BPF_X, 0),                    /* 24 */
        STMT(BPF_ALU | BPF_DIV | BPF_X, 0),                    /* 25: A = v */
        STMT(BPF_ALU | BPF_DIV | BPF_K, 1),                    /* 26 */
        JUMP(BPF_JMP | BPF_JGT | BPF_K, 4095, 0, 1),           /* 27 */
        STMT(BPF_LD | BPF_IMM, 4095),                          /* 28 */
        STMT(BPF_ALU | BPF_OR | BPF_K, RET_ERRNO),             /* 29 */
        STMT(BPF_RET | BPF_A, 0),                              /* 30 */
    };
    line("alu: install", install(0, g, sizeof g / sizeof g[0]));
    line("alu: getpriority bit clear", sc6(SYS_GETPRIORITY, 0, 0, 0, 0, 7, 9) >= 0);
    line("alu: getpriority 4, 9", sc6(SYS_GETPRIORITY, 0, 0x10, 0, 0, 4, 9));
    line("alu: getpriority 100, 0x1ff", sc6(SYS_GETPRIORITY, 0, 0x30, 0, 0, 100, 0x1ff));
    line("alu: getpriority 5000, 0", sc6(SYS_GETPRIORITY, 0, 0x10, 0, 0, 5000, 0));
    /* JGE with X, the shift by X, and the instruction pointer: getppid at trap_call gives ERRNO (ip & 0xfff)
     * compared with the address computed here */
    sock_filter h[] = {
        STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_GETPPID, 1, 0),
        STMT(BPF_RET | BPF_K, RET_ALLOW),
        STMT(BPF_LD | BPF_W | BPF_ABS, OFF_IP_LO),
        STMT(BPF_ALU | BPF_AND | BPF_K, 0xfff),
        STMT(BPF_ALU | BPF_OR | BPF_K, RET_ERRNO),
        STMT(BPF_RET | BPF_A, 0),
    };
    line("ip: install", install(0, h, sizeof h / sizeof h[0]));
    i64 r = trap_call();
    line("ip: errno is the call address", r == -(i64)((u64)trap_after & 0xfff) ? 1 : 0);
    sock_filter k[] = {
        STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
        JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_GETTID, 1, 0),
        STMT(BPF_RET | BPF_K, RET_ALLOW),
        STMT(BPF_LD | BPF_W | BPF_ABS, OFF_ARG_LO(0)), /* A = arg0 */
        STMT(BPF_MISC | BPF_TAX, 0),                   /* X = arg0 */
        STMT(BPF_LD | BPF_IMM, 1),
        STMT(BPF_ALU | BPF_LSH | BPF_X, 0),            /* A = 1 << arg0 */
        STMT(BPF_LDX | BPF_IMM, 100),
        JUMP(BPF_JMP | BPF_JGE | BPF_X, 0, 0, 1),      /* A >= 100 */
        STMT(BPF_RET | BPF_K, RET_ERRNO | 11),
        JUMP(BPF_JMP | BPF_JGT | BPF_X, 0, 1, 0),
        STMT(BPF_RET | BPF_K, RET_ERRNO | 12),
        JUMP(BPF_JMP | BPF_JEQ | BPF_X, 0, 0, 1),
        STMT(BPF_RET | BPF_K, RET_ERRNO | 13),
        JUMP(BPF_JMP | BPF_JSET | BPF_X, 0, 1, 0),
        STMT(BPF_RET | BPF_K, RET_ERRNO | 14),
        STMT(BPF_RET | BPF_K, RET_ERRNO | 15),
    };
    line("x: install", install(0, k, sizeof k / sizeof k[0]));
    line("x: 1<<3", sc6(SYS_GETTID, 3, 0, 0, 0, 0, 0));
    line("x: 1<<7", sc6(SYS_GETTID, 7, 0, 0, 0, 0, 0));
    line("x: 1<<6", sc6(SYS_GETTID, 6, 0, 0, 0, 0, 0));
    line("x: 1<<34 (shift by 34)", sc6(SYS_GETTID, 34, 0, 0, 0, 0, 0));
    sys_exit_group(0);
}

static void case_div_zero(void)
{
    i64 pid = fork_();
    if (pid == 0) {
        nnp();
        sock_filter f[] = {
            STMT(BPF_LD | BPF_W | BPF_ABS, OFF_NR),
            JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_GETPPID, 1, 0),
            STMT(BPF_RET | BPF_K, RET_ALLOW),
            STMT(BPF_LDX | BPF_IMM, 0),
            STMT(BPF_LD | BPF_IMM, 10),
            STMT(BPF_ALU | BPF_DIV | BPF_X, 0),
            STMT(BPF_RET | BPF_K, RET_ERRNO | 1),
        };
        line("div x 0: install", install(0, f, sizeof f / sizeof f[0]));
        getppid_();
        put("div x 0: not reached\n");
        sys_exit_group(0);
    }
    reap("div x 0 (program returns 0, KILL_THREAD)", pid);
}

/* TRAP: SIGSYS with si_code SYS_SECCOMP, si_errno the data, si_call_addr, si_syscall, si_arch; the ucontext holds the
 * original x0 and the pc after the svc; the handler's x0 becomes the call's result. */
static void case_trap(void)
{
    i64 pid = fork_();
    if (pid == 0) {
        nnp();
        sigaction_(SIGSYS, (u64)sys_handler, SA_SIGINFO);
        line("trap: install", install_nr(0, SYS_GETPPID, RET_TRAP | 0x42));
        i64 r = trap_call();
        line("trap: result", r);
        line("trap: count", h_count);
        line("trap: signo", h_signo);
        line("trap: code", h_code);
        linex("trap: errno", (u64)h_err);
        line("trap: syscall", h_sysno);
        line("trap: arch ok", h_arch_ok);
        line("trap: call_addr ok", h_addr_ok);
        line("trap: uc pc ok", h_pc_ok);
        line("trap: uc x0", (i64)h_x0);
        line("trap: uc x1", (i64)h_x1);
        line("trap: uc x8", h_x8);
        line("trap: fault_address 0", h_fault0);
        sys_exit_group(0);
    }
    reap("trap child", pid);
    pid = fork_();
    if (pid == 0) {
        nnp();
        sigaction_(SIGSYS, (u64)sys_handler, SA_SIGINFO);
        u64 set = 1ull << (SIGSYS - 1);
        sc6(SYS_RT_SIGPROCMASK, 0 /* SIG_BLOCK */, (i64)&set, 0, 8, 0, 0);
        install_nr(0, SYS_GETPPID, RET_TRAP);
        trap_call();
        put("trap blocked: not reached\n");
        sys_exit_group(0);
    }
    reap("trap with SIGSYS blocked", pid);
    pid = fork_();
    if (pid == 0) {
        nnp();
        sigaction_(SIGSYS, 1 /* SIG_IGN */, 0);
        install_nr(0, SYS_GETPPID, RET_TRAP);
        trap_call();
        put("trap ignored: not reached\n");
        sys_exit_group(0);
    }
    reap("trap with SIGSYS ignored", pid);
    pid = fork_();
    if (pid == 0) {
        nnp();
        install_nr(0, SYS_GETPPID, RET_TRAP);
        trap_call();
        put("trap default: not reached\n");
        sys_exit_group(0);
    }
    reap("trap with SIGSYS default", pid);
}

static void kill_thread_worker(void *arg)
{
    (void)arg;
    install_nr(0, SYS_GETPPID, RET_KILL_THREAD);
    getppid_();
    put("kill_thread worker: not reached\n");
    thread_exit(0);
}

static void case_kill(void)
{
    i64 pid = fork_();
    if (pid == 0) {
        nnp();
        install_nr(0, SYS_GETPPID, RET_KILL_PROCESS);
        getppid_();
        put("kill_process: not reached\n");
        sys_exit_group(0);
    }
    reap("kill_process", pid);
    pid = fork_();
    if (pid == 0) {
        nnp();
        install_nr(0, SYS_GETPPID, RET_KILL_THREAD);
        getppid_();
        put("kill_thread single: not reached\n");
        sys_exit_group(0);
    }
    reap("kill_thread, one thread", pid);
    pid = fork_();
    if (pid == 0) {
        nnp();
        if (spawn(0, kill_thread_worker, 0) <= 0)
            put("kill_thread: spawn failed\n");
        join(0);
        put("kill_thread: main goes on, getppid ok = ");
        putd(getppid_() > 0);
        put("\n");
        sys_exit_group(3);
    }
    reap("kill_thread, two threads", pid);
    pid = fork_();
    if (pid == 0) {
        nnp();
        install_nr(0, SYS_GETPPID, 0x00010000u);
        getppid_();
        put("unknown action: not reached\n");
        sys_exit_group(0);
    }
    reap("unknown action 0x10000", pid);
    pid = fork_();
    if (pid == 0) {
        nnp();
        install_nr(0, SYS_GETPPID, 0x7ffe0000u);
        getppid_();
        put("unknown action 0x7ffe0000: not reached\n");
        sys_exit_group(0);
    }
    reap("unknown action 0x7ffe0000", pid);
}

static void case_strict(void)
{
    i64 pid = fork_();
    if (pid == 0) {
        line("strict: prctl", prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT, 0, 0, 0));
        put("strict: write works\n");
        getpid_();
        put("strict: not reached\n");
        sys_exit_group(0);
    }
    reap("strict, getpid", pid);
    pid = fork_();
    if (pid == 0) {
        line("strict: seccomp(2)", seccomp(SECCOMP_SET_MODE_STRICT, 0, 0));
        thread_exit(7);
    }
    reap("strict, exit 7", pid);
    pid = fork_();
    if (pid == 0) {
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT, 1234, 0, 0); /* arg3 is ignored */
        sys_exit_group(0); /* exit_group is not in the strict set */
    }
    reap("strict, exit_group", pid);
}

/* ---------- inheritance ---------- */

static volatile i64 t_rc[4];

static void probe_worker(void *arg)
{
    (void)arg;
    wait_step(1);
    t_rc[0] = getppid_();
    t_rc[1] = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
    t_rc[2] = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    step_to(2);
    thread_exit(0);
}

static void own_filter_worker(void *arg)
{
    (void)arg;
    t_rc[3] = install_nr(0, SYS_GETTID, RET_ERRNO | 3);
    step_to(1);
    wait_step(2);
    thread_exit(0);
}

static void case_inherit(void)
{
    i64 pid = fork_();
    if (pid == 0) {
        nnp();
        install_nr(0, SYS_GETPPID, RET_ERRNO | 5);
        i64 c = fork_();
        if (c == 0) {
            line("fork: child getppid", getppid_());
            line("fork: child get_seccomp", prctl(PR_GET_SECCOMP, 0, 0, 0, 0));
            line("fork: child nnp", prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0));
            sys_exit_group(0);
        }
        reap("fork: grandchild", c);
        step = 0;
        spawn(0, probe_worker, 0);
        step_to(1);
        wait_step(2);
        join(0);
        line("thread after filter: getppid", t_rc[0]);
        line("thread after filter: get_seccomp", t_rc[1]);
        sys_exit_group(0);
    }
    reap("inherit child", pid);

    /* a thread made before the filter: unfiltered without TSYNC, filtered with it (and NO_NEW_PRIVS set for it) */
    pid = fork_();
    if (pid == 0) {
        step = 0;
        spawn(0, probe_worker, 0);
        nnp();
        install_nr(0, SYS_GETPPID, RET_ERRNO | 5);
        step_to(1);
        wait_step(2);
        join(0);
        line("no tsync: thread getppid > 0", t_rc[0] > 0);
        line("no tsync: thread get_seccomp", t_rc[1]);
        line("no tsync: thread nnp", t_rc[2]);
        sys_exit_group(0);
    }
    reap("no tsync child", pid);
    pid = fork_();
    if (pid == 0) {
        step = 0;
        spawn(0, probe_worker, 0);
        line("tsync: nnp", nnp());
        line("tsync: install", install_nr(FLAG_TSYNC, SYS_GETPPID, RET_ERRNO | 6));
        step_to(1);
        wait_step(2);
        join(0);
        line("tsync: thread getppid", t_rc[0]);
        line("tsync: thread get_seccomp", t_rc[1]);
        line("tsync: thread nnp", t_rc[2]);
        sys_exit_group(0);
    }
    reap("tsync child", pid);
    /* TSYNC fails when another thread has a filter that is not an ancestor: the result is that thread's tid (or
     * ESRCH with TSYNC_ESRCH), and nothing is installed */
    pid = fork_();
    if (pid == 0) {
        nnp();
        step = 0;
        i64 tid = spawn(0, own_filter_worker, 0);
        wait_step(1);
        line("tsync fail: worker install", t_rc[3]);
        i64 r = install_nr(FLAG_TSYNC, SYS_GETPPID, RET_ERRNO | 6);
        line("tsync fail: result is the worker's tid", r == tid);
        line("tsync fail: esrch", install_nr(FLAG_TSYNC | FLAG_TSYNC_ESRCH, SYS_GETPPID, RET_ERRNO | 6));
        line("tsync fail: getppid not filtered", getppid_() > 0);
        line("tsync fail: get_seccomp", prctl(PR_GET_SECCOMP, 0, 0, 0, 0));
        step_to(2);
        join(0);
        line("tsync after exit: install", install_nr(FLAG_TSYNC, SYS_GETPPID, RET_ERRNO | 6));
        line("tsync after exit: getppid", getppid_());
        sys_exit_group(0);
    }
    reap("tsync fail child", pid);
    /* TSYNC with a thread whose filter is an ancestor of the caller's: allowed */
    pid = fork_();
    if (pid == 0) {
        nnp();
        install_nr(0, SYS_GETTID, RET_ERRNO | 4);
        step = 0;
        spawn(0, probe_worker, 0);
        install_nr(0, SYS_GETPID, RET_ERRNO | 8);
        line("tsync ancestor: install", install_nr(FLAG_TSYNC, SYS_GETPPID, RET_ERRNO | 9));
        step_to(1);
        wait_step(2);
        join(0);
        line("tsync ancestor: thread getppid", t_rc[0]);
        sys_exit_group(0);
    }
    reap("tsync ancestor child", pid);
}

/* The instruction limit: 32768 instructions in a thread's filters, each filter counting 4 more (ENOMEM). */
static void case_limit(void)
{
    i64 pid = fork_();
    if (pid != 0) {
        reap("limit child", pid);
        return;
    }
    nnp();
    static sock_filter big[4096];
    for (int i = 0; i < 4095; i++)
        big[i] = STMT(BPF_LD | BPF_IMM, (u32)i);
    big[4095] = STMT(BPF_RET | BPF_K, RET_ALLOW);
    for (int i = 0; i < 8; i++) {
        put("limit: filter ");
        putd(i);
        put(" = ");
        putd(install(0, big, 4096));
        put("\n");
    }
    sock_filter small[] = {STMT(BPF_RET | BPF_K, RET_ALLOW)};
    line("limit: small one", install(0, small, 1));
    line("limit: getpid ok", getpid_() > 0);
    sys_exit_group(0);
}

/* execve keeps the filters, the mode and NO_NEW_PRIVS. */
static void case_exec(char *self)
{
    i64 pid = fork_();
    if (pid == 0) {
        nnp();
        install_nr(0, SYS_GETPPID, RET_ERRNO | 5);
        /* the program's own path (argv[0] may be relative to a directory the guest does not see) */
        static char exe[512];
        i64 n = sc6(78 /* readlinkat */, -100, (i64)"/proc/self/exe", (i64)exe, sizeof exe - 1, 0, 0);
        exe[n > 0 ? n : 0] = 0;
        char *args[] = {self, "exec-child", 0};
        i64 r = sc6(221 /* execve */, (i64)exe, (i64)args, (i64)xiaoji_envp, 0, 0, 0);
        line("exec: execve failed", r);
        sys_exit_group(1);
    }
    reap("exec child", pid);
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0] == 'e') {
        line("exec: getppid", getppid_());
        line("exec: get_seccomp", prctl(PR_GET_SECCOMP, 0, 0, 0, 0));
        line("exec: nnp", prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0));
        return 0;
    }
    case_queries();
    case_nnp();
    case_invalid();
    case_actions();
    case_precedence();
    case_args();
    case_div_zero();
    case_trap();
    case_kill();
    case_strict();
    case_inherit();
    case_limit();
    case_exec(argv[0]);
    put("done\n");
    return 0;
}
