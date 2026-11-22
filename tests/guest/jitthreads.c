/* Thread switches under the translator. Two threads each run a long chained loop, so the
 * slice budget runs out inside chained blocks while the other thread is runnable. The
 * child's result must equal the same loop run on this thread. The main thread waits on a
 * futex, not a spin, so the count of this program does not depend on how the threads
 * interleave. Output must match native. */
#include "sys.h"

#include <stdint.h>

#define CALLS 12000
#define SYS_FUTEX 98
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1


/* ---------- hot loops that chain through blocks ---------- */

/* chain_loop: every iteration leaves its block by a conditional branch, a direct branch,
 * an indirect call through a table (BLR) and a return (RET). Each exit is a hop to
 * the next block, so one iteration chains through several blocks. */
typedef u64 (*chain_fn_t)(u64);
static u64 chain_a(u64 x) { return x * 3 + 1; }
static u64 chain_b(u64 x) { return (x >> 1) ^ 0x55; }
static u64 chain_c(u64 x) { return x + ((x & 4) ? 7 : 9); }
static chain_fn_t volatile chain_tab[3] = {chain_a, chain_b, chain_c};

static u64 chain_loop(u64 seed, u64 n)
{
    u64 x = seed, acc = 0;
    for (u64 i = 0; i < n; i++) {
        u64 k = i % 3;
        if (x & 1)
            x = chain_tab[k](x);
        else
            x = chain_tab[(k + 1) % 3](x);
        switch (i & 3) {
        case 0: acc += x; break;
        case 1: acc ^= x; break;
        case 2: acc += x >> 3; break;
        default: acc -= x; break;
        }
        if (acc & (1ull << 40))
            acc = acc * 5 + 1;
        x ^= acc & 0xFFFF;
    }
    return acc ^ x;
}

/* Two threads, each running a long chained loop, so the slice budget runs out inside
 * chained blocks while the other thread is runnable. */
extern i64 jitstress_spawn(u64 flags, void *stack_top, u32 *ptid, u32 *ctid, void (*fn)(void *), void *arg);
__asm__(".text\n"
        ".global jitstress_spawn\n"
        "jitstress_spawn:\n"
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

#define THREAD_FLAGS 0x350F00ull /* VM, FS, FILES, SIGHAND, THREAD, SYSVSEM, PARENT/CHILD_SETTID */
static unsigned char tstack[65536] __attribute__((aligned(16)));
static u32 tid_parent, tid_child;
static volatile u32 child_done;
#define SYS_FUTEX 98
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
static u64 child_val;

static void thread_main(void *arg)
{
    (void)arg;
    child_val = chain_loop(777, CALLS * 16);
    child_done = 1;
    sc6(SYS_FUTEX, (i64)&child_done, FUTEX_WAKE, 1, 0, 0, 0);
}

static int checks;

static void check(const char *label, int ok)
{
    put(label);
    put(ok ? "=ok\n" : "=FAIL\n");
    if (!ok)
        checks++;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    // 9. Thread switches while both threads run chained loops. The child's result must
    // equal the same loop run on this thread, which it does only if neither thread's
    // blocks or budget were disturbed by the other.
    child_done = 0;
    jitstress_spawn(THREAD_FLAGS, tstack + sizeof tstack, &tid_parent, &tid_child, thread_main, 0);
    u64 main_val = chain_loop(777, CALLS * 16);
    /* Wait on the futex rather than spin. The wait is always entered once, so the program
     * makes the same system calls whether or not the child has finished: with a while loop,
     * the count depended on that race, and so on the slice boundaries. */
    do {
        sc6(SYS_FUTEX, (i64)&child_done, FUTEX_WAIT, 0, 0, 0, 0);
    } while (!child_done);
    check("thread_chained", child_val == main_val);
    put("thread_chained_val=");
    putx(main_val);
    put("\n");

    put(checks ? "jitthreads=FAIL\n" : "jitthreads=ok\n");
    return checks ? 1 : 0;
}
