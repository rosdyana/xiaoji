/* Event objects (B3): eventfd, timerfd, epoll and ppoll, from a freestanding guest
 * program. Every line is a return value or a value read, so the output is the same
 * on Linux and in the engine.
 *
 * Covered: the eventfd counter and semaphore modes, EAGAIN on an empty non-blocking
 * eventfd, epoll add, modify, delete and their errors, a timed epoll_pwait woken by a
 * timerfd, a blocking timerfd read, ppoll with a timeout and without one, and a blocking
 * eventfd read woken by a forked child's write. */
#include <stdint.h>
#include "sys.h"

#define SYS_CLOSE 57
#define SYS_READ 63
#define SYS_EVENTFD2 19
#define SYS_EPOLL_CREATE1 20
#define SYS_EPOLL_CTL 21
#define SYS_EPOLL_PWAIT 22
#define SYS_PPOLL 73
#define SYS_NANOSLEEP 101
#define SYS_TIMERFD_CREATE 85
#define SYS_TIMERFD_SETTIME 86
#define SYS_TIMERFD_GETTIME 87
#define SYS_CLONE 220
#define SYS_WAIT4 260

#define O_CLOEXEC_V 02000000
#define O_NONBLOCK_V 04000
#define EFD_SEMAPHORE_V 1
#define EPOLL_ADD 1
#define EPOLL_DEL 2
#define EPOLL_MOD 3
#define EPOLLIN_V 1
#define POLLIN_V 1
#define CLOCK_MONOTONIC_V 1
#define SIGCHLD_V 17

struct ep_event {
    u32 events;
    u64 data;
};

struct pollfd_v {
    i32 fd;
    int16_t events;
    int16_t revents;
};

struct timespec_v {
    i64 sec, nsec;
};

struct itimerspec_v {
    struct timespec_v interval, value;
};

static i64 rd(i64 fd, void *buf, size_t n) { return sc6(SYS_READ, fd, (i64)buf, (i64)n, 0, 0, 0); }
static i64 wr(i64 fd, const void *buf, size_t n) { return sys_write((int)fd, buf, n); }
static i64 closefd(i64 fd) { return sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0); }

static void line(const char *name, i64 v)
{
    put(name);
    put(" ");
    putd(v);
    put("\n");
}

static void sleep_ns(i64 ns)
{
    struct timespec_v t = {ns / 1000000000, ns % 1000000000};
    sc6(SYS_NANOSLEEP, (i64)&t, 0, 0, 0, 0, 0);
}

static i64 eventfd_new(u64 init, i64 flags) { return sc6(SYS_EVENTFD2, (i64)init, flags, 0, 0, 0, 0); }

static i64 epoll_ctl_v(i64 ep, i64 op, i64 fd, u32 events, u64 data)
{
    struct ep_event ev = {events, data};
    return sc6(SYS_EPOLL_CTL, ep, op, fd, (i64)&ev, 0, 0);
}

static i64 epoll_wait_v(i64 ep, struct ep_event *out, i64 max, i64 ms)
{
    return sc6(SYS_EPOLL_PWAIT, ep, (i64)out, max, ms, 0, 0);
}

static void write_u64(i64 fd, u64 v)
{
    wr(fd, &v, 8);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* eventfd: counter mode */
    i64 e = eventfd_new(5, O_NONBLOCK_V | O_CLOEXEC_V);
    line("efd fd ok", e >= 0);
    u64 v = 0;
    i64 r = rd(e, &v, 8);
    line("efd read", r);
    line("efd value", (i64)v);
    line("efd empty", rd(e, &v, 8));
    write_u64(e, 3);
    r = rd(e, &v, 8);
    line("efd after write", (i64)v);
    line("efd short read", rd(e, &v, 4));

    /* eventfd: semaphore mode, one per read */
    i64 s = eventfd_new(2, EFD_SEMAPHORE_V | O_NONBLOCK_V);
    r = rd(s, &v, 8);
    line("sem read", r);
    line("sem value", (i64)v);
    r = rd(s, &v, 8);
    line("sem read2", (i64)v);
    line("sem empty", rd(s, &v, 8));

    /* epoll: add, duplicate add, wait, modify, delete */
    i64 ep = sc6(SYS_EPOLL_CREATE1, O_CLOEXEC_V, 0, 0, 0, 0, 0);
    line("ep fd ok", ep >= 0);
    line("ep add", epoll_ctl_v(ep, EPOLL_ADD, e, EPOLLIN_V, 42));
    line("ep add again", epoll_ctl_v(ep, EPOLL_ADD, e, EPOLLIN_V, 42));
    struct ep_event out[4];
    line("ep wait empty", epoll_wait_v(ep, out, 4, 0));
    write_u64(e, 1);
    line("ep wait ready", epoll_wait_v(ep, out, 4, 0));
    line("ep event", (i64)out[0].events);
    line("ep data", (i64)out[0].data);
    line("ep mod", epoll_ctl_v(ep, EPOLL_MOD, e, EPOLLIN_V, 7));
    line("ep mod data", (i64)(epoll_wait_v(ep, out, 4, 0) == 1 ? out[0].data : 0));
    rd(e, &v, 8); /* drain: the counter is 0 again */
    line("ep del", epoll_ctl_v(ep, EPOLL_DEL, e, 0, 0));
    line("ep del again", epoll_ctl_v(ep, EPOLL_DEL, e, 0, 0));

    /* timerfd: one shot, a timed epoll_pwait woken by the expiry */
    i64 t = sc6(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC_V, O_NONBLOCK_V | O_CLOEXEC_V, 0, 0, 0, 0);
    line("tfd fd ok", t >= 0);
    line("tfd read unarmed", rd(t, &v, 8));
    struct itimerspec_v its = {{0, 0}, {0, 20000000}};
    line("tfd settime", sc6(SYS_TIMERFD_SETTIME, t, 0, (i64)&its, 0, 0, 0));
    struct itimerspec_v cur = {{0, 0}, {0, 0}};
    line("tfd gettime", sc6(SYS_TIMERFD_GETTIME, t, (i64)&cur, 0, 0, 0, 0));
    line("tfd armed", cur.value.sec > 0 || cur.value.nsec > 0);
    line("tfd read early", rd(t, &v, 8));
    i64 ep2 = sc6(SYS_EPOLL_CREATE1, O_CLOEXEC_V, 0, 0, 0, 0, 0);
    epoll_ctl_v(ep2, EPOLL_ADD, t, EPOLLIN_V, 3);
    line("tfd epoll wait", epoll_wait_v(ep2, out, 4, 2000));
    line("tfd epoll data", (i64)out[0].data);
    line("tfd expirations", rd(t, &v, 8) == 8 ? (i64)v : -1);
    closefd(ep2);
    closefd(t);

    /* timerfd: blocking read, a timer that is not non-blocking */
    i64 tb = sc6(SYS_TIMERFD_CREATE, CLOCK_MONOTONIC_V, O_CLOEXEC_V, 0, 0, 0, 0);
    struct itimerspec_v one = {{0, 0}, {0, 10000000}};
    sc6(SYS_TIMERFD_SETTIME, tb, 0, (i64)&one, 0, 0, 0);
    line("tfd blocking read", rd(tb, &v, 8));
    line("tfd blocking count", (i64)v);
    closefd(tb);

    /* ppoll: a timeout with nothing ready, then ready without a timeout */
    struct pollfd_v pf = {(i32)e, POLLIN_V, 0};
    struct timespec_v to = {0, 50000000};
    line("ppoll timeout", sc6(SYS_PPOLL, (i64)&pf, 1, (i64)&to, 0, 8, 0));
    line("ppoll timeout revents", pf.revents);
    write_u64(e, 1);
    pf.revents = 0;
    line("ppoll ready", sc6(SYS_PPOLL, (i64)&pf, 1, 0, 0, 8, 0));
    line("ppoll revents", pf.revents & POLLIN_V);
    rd(e, &v, 8);

    /* a blocking eventfd read, woken by a child that writes after a delay */
    i64 b = eventfd_new(0, O_CLOEXEC_V);
    i64 pid = sc6(SYS_CLONE, SIGCHLD_V, 0, 0, 0, 0, 0);
    if (pid == 0) {
        sleep_ns(20000000);
        write_u64(b, 7);
        sys_exit_group(0);
    }
    line("efd blocking read", rd(b, &v, 8));
    line("efd blocking value", (i64)v);
    i64 st = 0;
    line("wait child", sc6(SYS_WAIT4, pid, (i64)&st, 0, 0, 0, 0) == pid);
    closefd(b);
    closefd(s);
    closefd(ep);
    closefd(e);
    put("done\n");
    return 0;
}
