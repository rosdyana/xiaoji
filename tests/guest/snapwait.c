/* Waits that a snapshot must carry (docs/snapshot.md, S2). The program sets up threads and a child process that are
 * all blocked while the main thread sleeps 600 ms: a thread in read on an empty pipe, a thread in FUTEX_WAIT with no
 * timeout, a thread in epoll_pwait on an eventfd and a timerfd (armed for 1000 ms), and a child in read on another
 * pipe. Then the main thread wakes each of them, joins the threads, reaps the child, and prints what each saw.
 *
 * Run alone, it is an ordinary oracle case. tests/run.sh (snapshot) also runs it with --snapshot-at time:0.3 (inside
 * the sleep, every waiter blocked), which writes a snapshot and stops, then with --restore: the output of the two runs,
 * one after the other, must be the uninterrupted run's. The sleep must last 600 ms or a little more on the guest's
 * monotonic clock, which a restore continues, and the timer must fire once. */
#include "sys.h"

#define SYS_EVENTFD2 19
#define SYS_EPOLL_CREATE1 20
#define SYS_EPOLL_CTL 21
#define SYS_EPOLL_PWAIT 22
#define SYS_CLOSE 57
#define SYS_PIPE2 59
#define SYS_READ 63
#define SYS_EXIT 93
#define SYS_TIMERFD_CREATE 85
#define SYS_TIMERFD_SETTIME 86
#define SYS_FUTEX 98
#define SYS_NANOSLEEP 101
#define SYS_CLONE 220
#define SYS_WAIT4 260

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define EPOLLIN 1u
#define CLOCK_MONOTONIC 1
#define SIGCHLD 17
#define STACK_BYTES 65536
#define THREAD_FLAGS 0x350F00ull /* VM, FS, FILES, SIGHAND, THREAD, SYSVSEM, PARENT_SETTID, CHILD_CLEARTID */

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

static i64 sc(i64 nr, i64 a, i64 b, i64 c, i64 d) { return sc6(nr, a, b, c, d, 0, 0); }

static int pipe_t[2], pipe_c[2];
static int efd, tfd, epfd;
static u32 word;
static u32 tids[3];
static unsigned char stacks[3][STACK_BYTES] __attribute__((aligned(16)));
static char got_pipe[16];
static i64 futex_rc = -999;
static u64 got_event, got_timer;

static void put_num(i64 v)
{
    char b[24];
    int n = 0;
    u64 u = v < 0 ? (u64)-v : (u64)v;
    do {
        b[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0)
        b[n++] = '-';
    char o[24];
    for (int i = 0; i < n; i++)
        o[i] = b[n - 1 - i];
    sys_write(1, o, (size_t)n);
}

static u64 now_ms(void)
{
    i64 ts[2];
    sys_clock_gettime(CLOCK_MONOTONIC, ts);
    return (u64)ts[0] * 1000 + (u64)ts[1] / 1000000;
}

static void t_pipe(void *a)
{
    (void)a;
    i64 n = sc(SYS_READ, pipe_t[0], (i64)got_pipe, sizeof got_pipe - 1, 0);
    if (n < 0)
        got_pipe[0] = '!';
}

static void t_futex(void *a)
{
    (void)a;
    do
        futex_rc = sc(SYS_FUTEX, (i64)&word, FUTEX_WAIT, 0, 0);
    while (futex_rc == 0 && word == 0); /* a spurious wake goes back to sleep */
}

static void t_epoll(void *a)
{
    (void)a;
    while (!got_event || !got_timer) {
        u8 evs[2][16];
        i64 n = sc6(SYS_EPOLL_PWAIT, epfd, (i64)evs, 2, -1, 0, 8);
        for (i64 i = 0; i < n; i++) {
            u64 data;
            memcpy(&data, evs[i] + 8, 8);
            u64 v = 0;
            if (data == 1 && sc(SYS_READ, efd, (i64)&v, 8, 0) == 8)
                got_event += v;
            if (data == 2 && sc(SYS_READ, tfd, (i64)&v, 8, 0) == 8)
                got_timer += v;
        }
        if (n < 0)
            return;
    }
}

static void join(u32 *tid)
{
    while (*tid)
        sc(SYS_FUTEX, (i64)tid, FUTEX_WAIT, (i64)*tid, 0);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (sc(SYS_PIPE2, (i64)pipe_t, 0, 0, 0) || sc(SYS_PIPE2, (i64)pipe_c, 0, 0, 0))
        return 1;
    efd = (int)sc(SYS_EVENTFD2, 0, 0, 0, 0);
    tfd = (int)sc(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC, 0, 0, 0);
    epfd = (int)sc(SYS_EPOLL_CREATE1, 0, 0, 0, 0);
    u8 ev[16];
    u32 in = EPOLLIN;
    u64 d1 = 1, d2 = 2;
    memcpy(ev, &in, 4);
    memcpy(ev + 8, &d1, 8);
    sc(SYS_EPOLL_CTL, epfd, 1, efd, (i64)ev);
    memcpy(ev + 8, &d2, 8);
    sc(SYS_EPOLL_CTL, epfd, 1, tfd, (i64)ev);
    i64 its[4] = {0, 0, 1, 0}; /* one shot, 1 s from now */
    sc(SYS_TIMERFD_SETTIME, tfd, 0, (i64)its, 0);

    i64 child = sc6(SYS_CLONE, SIGCHLD, 0, 0, 0, 0, 0);
    if (child == 0) {
        char b[8];
        i64 n = sc(SYS_READ, pipe_c[0], (i64)b, sizeof b, 0);
        sc(SYS_EXIT, n == 3 && b[0] == 'g' ? 7 : 9, 0, 0, 0);
    }
    void (*fns[3])(void *) = {t_pipe, t_futex, t_epoll};
    for (int i = 0; i < 3; i++)
        if (xt_spawn(THREAD_FLAGS, stacks[i] + STACK_BYTES, &tids[i], &tids[i], fns[i], 0) <= 0)
            return 2;

    put("waiters started\n");
    u64 t0 = now_ms();
    i64 req[2] = {0, 600000000};
    sc(SYS_NANOSLEEP, (i64)req, 0, 0, 0);
    u64 dt = now_ms() - t0;
    put(dt >= 600 && dt < 3000 ? "sleep: 600 ms or a little more\n" : "sleep: wrong length\n");

    sys_write(pipe_t[1], "hello", 5);
    sys_write(pipe_c[1], "go!", 3);
    word = 1;
    i64 woke = sc(SYS_FUTEX, (i64)&word, FUTEX_WAKE, 1, 0);
    u64 one = 1;
    sys_write(efd, &one, 8);
    for (int i = 0; i < 3; i++)
        join(&tids[i]);
    int status = 0;
    i64 w = sc(SYS_WAIT4, child, (i64)&status, 0, 0);

    put("pipe: ");
    put(got_pipe);
    put("\nfutex: wait ");
    put_num(futex_rc);
    put(", woke ");
    put_num(woke);
    put("\nepoll: event ");
    put_num((i64)got_event);
    put(", timer ");
    put_num((i64)got_timer);
    put("\nchild: ");
    put(w == child ? "reaped" : "not reaped");
    put(", exit ");
    put_num((status >> 8) & 0xff);
    put("\n");
    return 0;
}
