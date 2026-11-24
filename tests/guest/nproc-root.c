/* RLIMIT_NPROC and real uid 0 (B16). A child keeps uid 0 but drops every capability with capset, then sets
 * RLIMIT_NPROC to 1 (its own main thread is the one task) and creates three threads that exit at once. Linux's
 * copy_process exempts the root user (real_cred->user == INIT_USER) from the check, so all three are created.
 * A second child keeps CAP_SYS_RESOURCE and the same limit: also all three. The engine must agree, and the counts
 * are the only output. */
#include "sys.h"

#define SYS_CLONE 220
#define SYS_CAPSET 91
#define SYS_SETRLIMIT 164
#define SYS_WAIT4 260
#define SYS_FUTEX 98
#define SIGCHLD_N 17
#define RLIMIT_NPROC_RES 6
#define THREAD_FLAGS 0x350F00ull
#define STACK_BYTES 16384
#define NTHREADS 3
#define CAP_SYS_RESOURCE 24

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

static u32 child_tid[NTHREADS];
static unsigned char stacks[NTHREADS][STACK_BYTES] __attribute__((aligned(16)));

static void quick(void *arg)
{
    (void)arg;
}

static void join_one(u32 *word)
{
    for (;;) {
        u32 v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        if (v == 0)
            return;
        sc6(SYS_FUTEX, (i64)word, 0, v, 0, 0, 0); /* FUTEX_WAIT, shared key (the kernel wakes CLEARTID words so) */
    }
}

/* Creates up to NTHREADS threads, joins them, and returns how many were created. */
static int spawn_and_join(void)
{
    int made = 0;
    for (int i = 0; i < NTHREADS; i++) {
        i64 tid = xt_spawn(THREAD_FLAGS, stacks[i] + STACK_BYTES, &child_tid[i], &child_tid[i], quick, 0);
        if (tid <= 0)
            break;
        made++;
    }
    for (int i = 0; i < made; i++)
        join_one(&child_tid[i]);
    return made;
}

/* capset with version 3 (two 32-bit data words per set) and an empty set, or the current set kept. */
struct cap_hdr {
    u32 version;
    int pid;
};
struct cap_data {
    u32 effective, permitted, inheritable;
};

static int drop_caps(int keep_sys_resource)
{
    struct cap_hdr h = {0x20080522u, 0};
    struct cap_data d[2];
    u32 keep0 = keep_sys_resource ? (1u << (CAP_SYS_RESOURCE)) : 0;
    d[0].effective = d[0].permitted = keep0;
    d[0].inheritable = 0;
    d[1].effective = d[1].permitted = 0;
    d[1].inheritable = 0;
    return (int)sc6(SYS_CAPSET, (i64)&h, (i64)d, 0, 0, 0, 0);
}

static int run_child(int keep_sys_resource)
{
    if (drop_caps(keep_sys_resource) != 0) {
        put("nproc-root: capset failed\n");
        return 1;
    }
    const u64 lim[2] = {1, 1};
    if (sc6(SYS_SETRLIMIT, RLIMIT_NPROC_RES, (i64)lim, 0, 0, 0, 0) != 0) {
        put("nproc-root: setrlimit failed\n");
        return 1;
    }
    int made = spawn_and_join();
    put(keep_sys_resource ? "nproc-root: uid 0 with CAP_SYS_RESOURCE, limit 1: created " : "nproc-root: uid 0 without caps, limit 1: created ");
    putu((u64)made);
    put(" of ");
    putu(NTHREADS);
    put("\n");
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (int k = 0; k < 2; k++) {
        i64 pid = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
        if (pid == 0)
            sys_exit_group(run_child(k == 1));
        if (pid < 0) {
            put("nproc-root: fork failed\n");
            return 1;
        }
        i32 status = 0;
        if (sc6(SYS_WAIT4, pid, (i64)&status, 0, 0, 0, 0) != pid) {
            put("nproc-root: wait failed\n");
            return 1;
        }
        if (status != 0)
            return 1;
    }
    return 0;
}
