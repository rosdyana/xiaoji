/* Threads: three clone(CLONE_THREAD) workers share a counter under a futex
 * mutex, and one of them opens a gate the main thread waits on. Each worker
 * checks a futex timeout (ETIMEDOUT), a futex value mismatch (EAGAIN) and a
 * nanosleep. The main thread joins them with CLONE_CHILD_CLEARTID. Only results
 * that do not depend on the interleaving are printed. */
#include "sys.h"

#define SYS_FUTEX 98
#define SYS_NANOSLEEP 101
#define SYS_SCHED_YIELD 124

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE 128

#define NWORK 3
#define ITERS 20000
#define STACK_BYTES 65536

/* clone flags: VM, FS, FILES, SIGHAND, THREAD, SYSVSEM, PARENT_SETTID, CHILD_CLEARTID. */
#define THREAD_FLAGS 0x350F00ull

/* Child: pops fn and arg from its stack, calls fn(arg), then exits. Parent:
 * returns the child tid. Arguments: x0 flags, x1 stack top, x2 parent tid
 * address, x3 child tid address, x4 fn, x5 arg. */
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

static u32 lock_word;  /* 0 free, 1 locked, 2 locked with waiters */
static u32 gate;       /* set by worker 0 */
static u32 never;      /* nothing wakes it: its waits time out */
static u64 counter;    /* guarded by lock_word */
static u32 child_tid[NWORK];
static u64 local_sum[NWORK];
static i64 timeout_rc[NWORK];
static i64 eagain_rc[NWORK];
static i64 sleep_rc[NWORK];
static unsigned char stacks[NWORK][STACK_BYTES] __attribute__((aligned(16)));

static i64 futex(u32 *addr, i64 op, u32 val, const i64 *timeout)
{
    return sc6(SYS_FUTEX, (i64)addr, op, val, (i64)timeout, 0, 0);
}

static void lock(void)
{
    u32 c = 0;
    if (__atomic_compare_exchange_n(&lock_word, &c, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    if (c != 2)
        c = __atomic_exchange_n(&lock_word, 2u, __ATOMIC_ACQUIRE);
    while (c != 0) {
        futex(&lock_word, FUTEX_WAIT | FUTEX_PRIVATE, 2, 0);
        c = __atomic_exchange_n(&lock_word, 2u, __ATOMIC_ACQUIRE);
    }
}

static void unlock(void)
{
    if (__atomic_exchange_n(&lock_word, 0u, __ATOMIC_RELEASE) == 2)
        futex(&lock_word, FUTEX_WAKE | FUTEX_PRIVATE, 1, 0);
}

static void worker(void *arg)
{
    u64 idx = (u64)(i64)arg;
    u64 sum = 0;
    for (u64 i = 0; i < ITERS; i++) {
        lock();
        counter++;
        unlock();
        sum += i % 7;
        if (i % 1000 == 999)
            sc6(SYS_SCHED_YIELD, 0, 0, 0, 0, 0, 0);
    }
    local_sum[idx] = sum;

    if (idx == 0) {
        __atomic_store_n(&gate, 1u, __ATOMIC_RELEASE);
        futex(&gate, FUTEX_WAKE | FUTEX_PRIVATE, 1, 0);
    }

    const i64 timeout[2] = {0, 3000000}; /* 3 ms, relative */
    timeout_rc[idx] = futex(&never, FUTEX_WAIT | FUTEX_PRIVATE, 0, timeout);
    eagain_rc[idx] = futex(&never, FUTEX_WAIT | FUTEX_PRIVATE, 1, 0);

    const i64 nap[2] = {0, 2000000}; /* 2 ms */
    sleep_rc[idx] = sc6(SYS_NANOSLEEP, (i64)nap, 0, 0, 0, 0, 0);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (u64 i = 0; i < NWORK; i++) {
        i64 tid = xt_spawn(THREAD_FLAGS, stacks[i] + STACK_BYTES, &child_tid[i], &child_tid[i], worker,
                           (void *)(i64)i);
        if (tid <= 0) {
            put("threads: clone failed\n");
            return 1;
        }
    }

    /* Wait for the gate: re-check after every wake, since the value may already be set. */
    while (__atomic_load_n(&gate, __ATOMIC_ACQUIRE) == 0)
        futex(&gate, FUTEX_WAIT | FUTEX_PRIVATE, 0, 0);
    put("threads: gate open\n");

    /* Join. CLEARTID clears each tid word and wakes it; the wait uses the
     * shared (non-private) form, as the kernel's wake does. */
    for (u64 i = 0; i < NWORK; i++) {
        u32 t;
        while ((t = __atomic_load_n(&child_tid[i], __ATOMIC_ACQUIRE)) != 0)
            futex(&child_tid[i], FUTEX_WAIT, t, 0);
    }

    put("threads: counter=");
    putu(counter);
    put("\n");
    for (u64 i = 0; i < NWORK; i++) {
        put("threads: worker ");
        putu(i);
        put(" sum=");
        putu(local_sum[i]);
        put(" timeout=");
        putd(timeout_rc[i]);
        put(" eagain=");
        putd(eagain_rc[i]);
        put(" sleep=");
        putd(sleep_rc[i]);
        put("\n");
    }
    put("threads: joined\n");
    return 0;
}
