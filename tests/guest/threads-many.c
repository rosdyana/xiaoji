/* Many threads: (1) three rounds of 600 threads alive at once, each round released and joined; the
 * old engine had a 256-slot thread table and failed the second spawn. (2) 2000 sequential create and
 * join pairs in one long-lived process. (3) RLIMIT_NPROC: a child that switches to uid 31337 (not
 * root) with a soft limit of 20 tasks creates threads until clone returns EAGAIN. Linux counts every
 * task of the real uid, so the child's main thread takes one slot and 19 threads are created. The
 * child then joins them, and creates 19 more, which must succeed again (the slots were given back).
 * Only counts are printed. */
#include "sys.h"

#define SYS_FUTEX 98
#define SYS_SCHED_YIELD 124
#define SYS_SETRLIMIT 164
#define SYS_SETUID 146
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SYS_WAITID 95
#define SYS_PIPE2 59
#define SYS_READ 63
#define SYS_KILL 129
#define SYS_NANOSLEEP 101
#define P_PID 1
#define WEXITED 4
#define WNOWAIT 0x1000000

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE 128
#define EAGAIN_RC (-11)

#define ROUNDS 3
#define LIVE 600
#define LOOPS 2000
#define NPROC_UID 31337
#define NPROC_LIMIT 20
#define STACK_BYTES 16384
#define RLIMIT_NPROC_RES 6

/* clone flags: VM, FS, FILES, SIGHAND, THREAD, SYSVSEM, PARENT_SETTID, CHILD_CLEARTID. */
#define THREAD_FLAGS 0x350F00ull

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

static u32 gate;                       /* 0 until the main thread releases the workers */
static u32 started;                    /* workers that have begun (the round waits for all) */
static u32 child_tid[LIVE];            /* CLONE_CHILD_CLEARTID words: zero once the thread is gone */
static unsigned char stacks[LIVE][STACK_BYTES] __attribute__((aligned(16)));
static u32 done_work;                  /* loop-phase thread: counts how often it ran */

static i64 futex(u32 *addr, i64 op, u32 val)
{
    return sc6(SYS_FUTEX, (i64)addr, op, val, 0, 0, 0);
}

static void blocked_worker(void *arg)
{
    (void)arg;
    __atomic_fetch_add(&started, 1u, __ATOMIC_ACQ_REL);
    futex(&started, FUTEX_WAKE | FUTEX_PRIVATE, 1);
    while (__atomic_load_n(&gate, __ATOMIC_ACQUIRE) == 0)
        futex(&gate, FUTEX_WAIT | FUTEX_PRIVATE, 0);
}

static void quick_worker(void *arg)
{
    (void)arg;
    __atomic_fetch_add(&done_work, 1u, __ATOMIC_RELAXED);
}

static void join_one(u32 *word)
{
    for (;;) {
        u32 v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        if (v == 0)
            return;
        futex(word, FUTEX_WAIT, v); /* shared key: the kernel wakes CLEARTID words as shared */
    }
}

/* Spawns up to n blocked workers (stacks[0..n-1]); returns the number created. A failed clone stops
 * the round and sets *err to its return value (-EAGAIN is the limit case). */
static int spawn_blocked(int n, i64 *err)
{
    int made = 0;
    *err = 0;
    for (int i = 0; i < n; i++) {
        i64 tid = xt_spawn(THREAD_FLAGS, stacks[i] + STACK_BYTES, &child_tid[i], &child_tid[i], blocked_worker, 0);
        if (tid <= 0) {
            *err = tid;
            break;
        }
        made++;
    }
    return made;
}

static void wait_started(u32 want)
{
    while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) < want)
        sc6(SYS_SCHED_YIELD, 0, 0, 0, 0, 0, 0);
}

static void release_and_join(int made)
{
    __atomic_store_n(&gate, 1u, __ATOMIC_RELEASE);
    futex(&gate, FUTEX_WAKE | FUTEX_PRIVATE, 0x7fffffff);
    for (int i = 0; i < made; i++)
        join_one(&child_tid[i]);
    __atomic_store_n(&gate, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&started, 0u, __ATOMIC_RELEASE);
}

static int phase_live(void)
{
    for (int r = 1; r <= ROUNDS; r++) {
        i64 err = 0;
        __atomic_store_n(&started, 0u, __ATOMIC_RELEASE);
        int made = spawn_blocked(LIVE, &err);
        wait_started((u32)made);
        put("live: round ");
        putu((u64)r);
        put(" created ");
        putu((u64)made);
        put(err ? " (clone failed)\n" : "\n");
        release_and_join(made);
        if (err)
            return 1;
    }
    return 0;
}

static int phase_loop(void)
{
    u32 tid_word = 0;
    for (u64 i = 0; i < LOOPS; i++) {
        i64 tid = xt_spawn(THREAD_FLAGS, stacks[0] + STACK_BYTES, &tid_word, &tid_word, quick_worker, 0);
        if (tid <= 0) {
            put("loop: clone failed at ");
            putu(i);
            put("\n");
            return 1;
        }
        join_one(&tid_word);
    }
    put("loop: ");
    putu(LOOPS);
    put(" create and join pairs, ran ");
    putu(__atomic_load_n(&done_work, __ATOMIC_RELAXED));
    put("\n");
    return 0;
}

/* Spawns blocked workers until clone refuses, joins them, and prints the count under tag. */
/* Linux releases an exited thread's task a little after its CLEARTID join word is cleared, so the count
 * right after a join can still include it. A 20 ms pause lets the task go before the next count. */
static void settle(void)
{
    const i64 nap[2] = {0, 20000000};
    sc6(SYS_NANOSLEEP, (i64)nap, 0, 0, 0, 0, 0);
}

static int nproc_round(const char *tag)
{
    i64 err = 0;
    __atomic_store_n(&started, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gate, 0u, __ATOMIC_RELEASE);
    int n = 0;
    for (int i = 0; i < 64; i++) {
        i64 tid = xt_spawn(THREAD_FLAGS, stacks[i] + STACK_BYTES, &child_tid[i], &child_tid[i], blocked_worker, 0);
        if (tid <= 0) {
            err = tid;
            break;
        }
        n++;
    }
    put("nproc: uid ");
    putu(NPROC_UID);
    put(" limit ");
    putu(NPROC_LIMIT);
    put(" ");
    put(tag);
    put(": ");
    putu((u64)n);
    put(" threads created before ");
    putd(err);
    put("\n");
    wait_started((u32)n);
    release_and_join(n);
    settle();
    return n;
}

static int nproc_child(void)
{
    if (sc6(SYS_SETUID, NPROC_UID, 0, 0, 0, 0, 0) != 0) {
        put("nproc: setuid failed\n");
        return 1;
    }
    const u64 lim[2] = {NPROC_LIMIT, NPROC_LIMIT};
    if (sc6(SYS_SETRLIMIT, RLIMIT_NPROC_RES, (i64)lim, 0, 0, 0, 0) != 0) {
        put("nproc: setrlimit failed\n");
        return 1;
    }
    nproc_round("round 1");
    nproc_round("round 2");

    /* A grandchild (a task of the same uid) that exits and is not reaped: a zombie. waitid with WNOWAIT
     * waits for the exit and leaves the zombie in place. The count shows whether Linux counts it. */
    i64 gc = sc6(SYS_CLONE, 17 /* SIGCHLD */, 0, 0, 0, 0, 0);
    if (gc == 0)
        sys_exit_group(0);
    if (gc < 0) {
        put("nproc: zombie fork failed\n");
        return 1;
    }
    u64 info[16] __attribute__((aligned(16)));
    for (int k = 0; k < 16; k++)
        info[k] = 0;
    if (sc6(SYS_WAITID, P_PID, gc, (i64)info, WEXITED | WNOWAIT, 0, 0) != 0) {
        put("nproc: waitid failed\n");
        return 1;
    }
    nproc_round("with zombie");
    i32 status = 0;
    if (sc6(SYS_WAIT4, gc, (i64)&status, 0, 0, 0, 0) != gc) {
        put("nproc: reap failed\n");
        return 1;
    }
    nproc_round("after reap");
    return 0;
}

/* SIGKILL to a process with LIVE threads: every one of them must die (the signal reaches each thread of the
 * process, not only the first 256). The child makes its threads, then tells the parent through a pipe. */
static int phase_kill(void)
{
    int fds[2];
    if (sc6(SYS_PIPE2, (i64)fds, 0, 0, 0, 0, 0) != 0) {
        put("kill: pipe failed\n");
        return 1;
    }
    i64 pid = sc6(SYS_CLONE, 17 /* SIGCHLD */, 0, 0, 0, 0, 0);
    if (pid == 0) {
        for (int i = 0; i < LIVE; i++)
            if (xt_spawn(THREAD_FLAGS, stacks[i] + STACK_BYTES, &child_tid[i], &child_tid[i], blocked_worker, 0) <= 0)
                break;
        sc6(SYS_WRITE, fds[1], (i64)"x", 1, 0, 0, 0);
        for (;;) /* the main thread waits on gate, which nothing opens */
            futex(&gate, FUTEX_WAIT | FUTEX_PRIVATE, 0);
    }
    if (pid < 0) {
        put("kill: fork failed\n");
        return 1;
    }
    char c = 0;
    if (sc6(SYS_READ, fds[0], (i64)&c, 1, 0, 0, 0) != 1) {
        put("kill: no word from the child\n");
        return 1;
    }
    sc6(SYS_KILL, pid, 9, 0, 0, 0, 0);
    i32 status = 0;
    if (sc6(SYS_WAIT4, pid, (i64)&status, 0, 0, 0, 0) != pid) {
        put("kill: wait failed\n");
        return 1;
    }
    put("kill: SIGKILL to a process with ");
    putu(LIVE);
    put(" threads: signal ");
    putd(status & 0x7f);
    put("\n");
    return (status & 0x7f) == 9 ? 0 : 1;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (phase_live())
        return 1;
    if (phase_loop())
        return 1;
    if (phase_kill())
        return 1;

    /* The child keeps the process's own clone (no CLONE_THREAD): a plain fork. */
    i64 pid = sc6(SYS_CLONE, 17 /* SIGCHLD */, 0, 0, 0, 0, 0);
    if (pid == 0)
        sys_exit_group(nproc_child());
    if (pid < 0) {
        put("nproc: fork failed\n");
        return 1;
    }
    i32 status = 0;
    i64 w = sc6(SYS_WAIT4, pid, (i64)&status, 0, 0, 0, 0);
    if (w != pid) {
        put("nproc: wait failed\n");
        return 1;
    }
    put("nproc: child status ");
    putd(status);
    put("\n");
    return status != 0;
}
