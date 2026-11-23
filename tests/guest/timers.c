/* Timers (B9): ITIMER_REAL with SIGALRM (one-shot and interval), getitimer and the disarm, POSIX timers with
 * SIGEV_SIGNAL and SIGEV_THREAD_ID (the si_code and si_value of each signal), an absolute time in the past,
 * the errors of setitimer and timer_create, and a fork child that has no timers. Freestanding.
 *
 * The output holds only comparisons, fixed counts and small numbers, never a time or a timer id, so it is the
 * same natively and in the engine. Where the count of a periodic timer depends on the host's speed, the test
 * prints whether the count reached a bound. */
#include "sys.h"

#define SYS_NANOSLEEP 101
#define SYS_SETITIMER 103
#define SYS_GETITIMER 102
#define SYS_TIMER_CREATE 107
#define SYS_TIMER_GETTIME 108
#define SYS_TIMER_GETOVERRUN 109
#define SYS_TIMER_SETTIME 110
#define SYS_TIMER_DELETE 111
#define SYS_RT_SIGACTION 134
#define SYS_GETTID 178
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SYS_EXIT 93

#define SIGUSR1 10
#define SIGALRM 14
#define SIGCHLD 17
#define SA_SIGINFO 0x4ull
#define SA_RESTORER 0x04000000ull
#define SI_KERNEL 0x80
#define SI_TIMER (-2)
#define SIGEV_SIGNAL 0
#define SIGEV_THREAD_ID 4
#define TIMER_ABSTIME 1
#define CLOCK_MONOTONIC 1
#define ITIMER_REAL 0
#define EINTR 4
#define NS 1000000000ll

typedef struct {
    u64 handler, flags, restorer, mask;
} ksigaction;

/* siginfo: si_signo, si_errno, si_code, then the union at 16 (si_timerid or si_pid), 24 (si_value). */
typedef struct {
    i32 signo, err, code, pad;
    u64 w[14];
} kinfo;

/* struct sigevent (arm64, 64 bytes): sigev_value, sigev_signo, sigev_notify, the union (tid at 16). */
typedef struct {
    u64 value;
    i32 signo;
    i32 notify;
    i32 tid;
    i32 pad;
    u64 rest[5];
} ksigevent;

typedef struct {
    i64 int_sec, int_nsec, val_sec, val_nsec; /* itimerspec, or itimerval with usec */
} kitimer;

typedef struct {
    i64 sec, nsec;
} kts;

/* Returns through the restorer, as bionic does. */
__asm__(".text\n"
        ".global xiaoji_restore\n"
        "xiaoji_restore:\n"
        "  mov x8, #139\n"
        "  svc #0\n");
extern void xiaoji_restore(void);
#define RESTORER ((u64)xiaoji_restore)

static volatile int got_alrm, last_alrm_code;
static volatile int got_usr1, last_usr1_code;
static volatile i64 last_usr1_value;

static void alrm_handler(int sig, kinfo *info, void *uc)
{
    (void)sig;
    (void)uc;
    got_alrm++;
    last_alrm_code = info->code;
}

static void usr1_handler(int sig, kinfo *info, void *uc)
{
    (void)sig;
    (void)uc;
    got_usr1++;
    last_usr1_code = info->code;
    last_usr1_value = (i64)info->w[1];
}

static void install(int sig, void *handler)
{
    ksigaction sa;
    sa.handler = (u64)handler;
    sa.flags = SA_SIGINFO | SA_RESTORER;
    sa.restorer = RESTORER;
    sa.mask = 0;
    sc6(SYS_RT_SIGACTION, sig, (i64)&sa, 0, 8, 0, 0);
}

static i64 sys_setitimer(i64 which, kitimer *nv, kitimer *ov)
{
    return sc6(SYS_SETITIMER, which, (i64)nv, (i64)ov, 0, 0, 0);
}
static i64 sys_getitimer(i64 which, kitimer *cur) { return sc6(SYS_GETITIMER, which, (i64)cur, 0, 0, 0, 0); }
static i64 sys_timer_create(i64 clk, ksigevent *sev, i32 *id)
{
    return sc6(SYS_TIMER_CREATE, clk, (i64)sev, (i64)id, 0, 0, 0);
}
static i64 sys_timer_settime(i64 id, i64 flags, kitimer *nv, kitimer *ov)
{
    return sc6(SYS_TIMER_SETTIME, id, flags, (i64)nv, (i64)ov, 0, 0);
}
static i64 sys_timer_gettime(i64 id, kitimer *cur) { return sc6(SYS_TIMER_GETTIME, id, (i64)cur, 0, 0, 0, 0); }
static i64 sys_timer_getoverrun(i64 id) { return sc6(SYS_TIMER_GETOVERRUN, id, 0, 0, 0, 0, 0); }
static i64 sys_timer_delete(i64 id) { return sc6(SYS_TIMER_DELETE, id, 0, 0, 0, 0, 0); }
static i64 sys_nanosleep(kts *req) { return sc6(SYS_NANOSLEEP, (i64)req, 0, 0, 0, 0, 0); }
static i64 sys_gettid(void) { return sc6(SYS_GETTID, 0, 0, 0, 0, 0, 0); }

/* Sleeps (2 s each, at most 20 times) until the count reaches want; a signal ends each sleep early (EINTR). */
static void wait_until_alrm(int want)
{
    kts req = {2, 0};
    int tries = 0;
    while (got_alrm < want && tries < 20) {
        (void)sys_nanosleep(&req);
        tries++;
    }
}
static void wait_until_usr1(int base, int want)
{
    kts req = {2, 0};
    int tries = 0;
    while (got_usr1 - base < want && tries < 20) {
        (void)sys_nanosleep(&req);
        tries++;
    }
}

static void put_rc(const char *label, i64 rc)
{
    put(label);
    putd(rc);
    put("\n");
}

static void put_bool(const char *label, int ok)
{
    put(label);
    put(ok ? "1\n" : "0\n");
}

static i64 do_fork(void) { return sc6(SYS_CLONE, SIGCHLD, 0, 0, 0, 0, 0); }
static i64 wait_for(i64 pid, i32 *st) { return sc6(SYS_WAIT4, pid, (i64)st, 0, 0, 0, 0); }

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    install(SIGALRM, alrm_handler);
    install(SIGUSR1, usr1_handler);
    int base_alrm = 0, base_usr1 = 0;

    /* 1. ITIMER_REAL one-shot: a 2 s sleep is cut short by the 20 ms alarm (EINTR), once. */
    kitimer nv = {0, 0, 0, 20000};
    put_rc("setitimer one-shot: rc=", sys_setitimer(ITIMER_REAL, &nv, 0));
    kts req = {2, 0};
    put_rc("sleep cut short: rc=", sys_nanosleep(&req));
    put_bool("alarm count is 1: ", got_alrm == 1);
    put_bool("alarm si_code is SI_KERNEL: ", last_alrm_code == SI_KERNEL);
    kitimer cur = {-1, -1, -1, -1};
    put_rc("getitimer after the one-shot: rc=", sys_getitimer(ITIMER_REAL, &cur));
    put_bool("one-shot is disarmed: ", cur.int_sec == 0 && cur.int_nsec == 0 && cur.val_sec == 0 && cur.val_nsec == 0);

    /* 2. Remaining time and disarm: 2 s is set, then read back (within 2 s and above zero), then disarmed. */
    kitimer two = {0, 0, 2, 0};
    (void)sys_setitimer(ITIMER_REAL, &two, 0);
    cur.val_sec = -1;
    (void)sys_getitimer(ITIMER_REAL, &cur);
    put_bool("remaining is in (0, 2 s]: ",
             (cur.val_sec > 0 || cur.val_nsec > 0) && (cur.val_sec < 2 || (cur.val_sec == 2 && cur.val_nsec == 0)));
    kitimer zero = {0, 0, 0, 0};
    kitimer old = {0, 0, 0, 0};
    put_rc("disarm: rc=", sys_setitimer(ITIMER_REAL, &zero, &old));
    put_bool("the old value was still armed: ", old.val_sec > 0 || old.val_nsec > 0);

    /* 3. Interval: 10 ms, then 10 ms. At least three alarms (each sleep is cut short), then disarm. */
    kitimer per = {0, 10000, 0, 10000};
    base_alrm = got_alrm;
    (void)sys_setitimer(ITIMER_REAL, &per, 0);
    wait_until_alrm(base_alrm + 3);
    (void)sys_setitimer(ITIMER_REAL, &zero, 0);
    put_bool("interval alarms reached 3: ", got_alrm - base_alrm >= 3);

    /* 4. POSIX timer, SIGEV_SIGNAL to SIGUSR1 with value 42: a one-shot 10 ms. */
    ksigevent sev = {0};
    sev.value = 42;
    sev.signo = SIGUSR1;
    sev.notify = SIGEV_SIGNAL;
    i32 id = -1;
    put_rc("timer_create SIGEV_SIGNAL: rc=", sys_timer_create(CLOCK_MONOTONIC, &sev, &id));
    kitimer shot = {0, 0, 0, 10000000};
    put_rc("timer_settime one-shot: rc=", sys_timer_settime(id, 0, &shot, 0));
    base_usr1 = got_usr1;
    wait_until_usr1(base_usr1, 1);
    put_bool("POSIX one-shot: si_code is SI_TIMER: ", last_usr1_code == SI_TIMER);
    put_bool("POSIX one-shot: si_value is 42: ", last_usr1_value == 42);
    kitimer g = {-1, -1, -1, -1};
    put_rc("timer_gettime after the one-shot: rc=", sys_timer_gettime(id, &g));
    put_bool("POSIX one-shot is disarmed: ", g.val_sec == 0 && g.val_nsec == 0);
    put_rc("timer_getoverrun: rc=", sys_timer_getoverrun(id));
    put_rc("timer_delete: rc=", sys_timer_delete(id));
    put_rc("timer_gettime after delete: rc=", sys_timer_gettime(id, &g));

    /* 5. SIGEV_THREAD_ID to this thread, periodic 5 ms, value 7. */
    ksigevent tsev = {0};
    tsev.value = 7;
    tsev.signo = SIGUSR1;
    tsev.notify = SIGEV_THREAD_ID;
    tsev.tid = (i32)sys_gettid();
    i32 tid_id = -1;
    put_rc("timer_create SIGEV_THREAD_ID: rc=", sys_timer_create(CLOCK_MONOTONIC, &tsev, &tid_id));
    kitimer per5 = {0, 5000000, 0, 5000000};
    put_rc("timer_settime periodic: rc=", sys_timer_settime(tid_id, 0, &per5, 0));
    base_usr1 = got_usr1;
    wait_until_usr1(base_usr1, 2);
    put_bool("thread-id timer: two signals: ", got_usr1 - base_usr1 >= 2);
    put_bool("thread-id timer: si_code is SI_TIMER: ", last_usr1_code == SI_TIMER);
    put_bool("thread-id timer: si_value is 7: ", last_usr1_value == 7);
    kitimer none = {0, 0, 0, 0};
    put_rc("timer_settime disarm: rc=", sys_timer_settime(tid_id, 0, &none, 0));
    put_rc("timer_delete thread-id timer: rc=", sys_timer_delete(tid_id));

    /* 6. An absolute time in the past (CLOCK_MONOTONIC) fires at once. */
    ksigevent asev = {0};
    asev.value = 9;
    asev.signo = SIGUSR1;
    asev.notify = SIGEV_SIGNAL;
    i32 abs_id = -1;
    put_rc("timer_create absolute: rc=", sys_timer_create(CLOCK_MONOTONIC, &asev, &abs_id));
    /* 1 ns on CLOCK_MONOTONIC is in the past for any clock that has run (and not zero, which disarms). */
    kitimer past = {0, 0, 0, 1};
    base_usr1 = got_usr1;
    put_rc("timer_settime absolute past: rc=", sys_timer_settime(abs_id, TIMER_ABSTIME, &past, 0));
    wait_until_usr1(base_usr1, 1);
    put_bool("absolute past time fired: ", got_usr1 - base_usr1 >= 1);
    (void)sys_timer_delete(abs_id);

    /* 7. Errors: the same values on Linux and the engine. */
    ksigevent bad = {0};
    bad.signo = SIGUSR1;
    /* Clock 2 (CPU time) is not compared: Linux has it and the engine does not (docs/boot.md B9). */
    put_rc("timer_create clock 99: rc=", sys_timer_create(99, &bad, &id));
    bad.notify = SIGEV_SIGNAL;
    bad.signo = 0;
    put_rc("timer_create signal 0: rc=", sys_timer_create(CLOCK_MONOTONIC, &bad, &id));
    bad.signo = SIGUSR1;
    bad.notify = SIGEV_THREAD_ID;
    bad.tid = 999999;
    put_rc("timer_create thread not in the process: rc=", sys_timer_create(CLOCK_MONOTONIC, &bad, &id));
    put_rc("timer_settime bad id: rc=", sys_timer_settime(99, 0, &shot, 0));
    put_rc("timer_settime bad flags: rc=", sys_timer_settime(99, 2, &shot, 0));
    kitimer usec_bad = {0, 0, 0, 1000000};
    put_rc("setitimer usec out of range: rc=", sys_setitimer(ITIMER_REAL, &usec_bad, 0));
    kitimer nsec_bad = {0, 0, 0, 0};
    nsec_bad.val_nsec = NS;
    put_rc("timer_settime nsec out of range: rc=", sys_timer_settime(99, 0, &nsec_bad, 0));

    /* 8. A fork child has no timers: the parent's 2 s itimer is not inherited. */
    kitimer parent = {0, 0, 2, 0};
    (void)sys_setitimer(ITIMER_REAL, &parent, 0);
    i64 pid = do_fork();
    if (pid == 0) {
        kitimer c = {-1, -1, -1, -1};
        (void)sys_getitimer(ITIMER_REAL, &c);
        sc6(SYS_EXIT, c.val_sec == 0 && c.val_nsec == 0 ? 0 : 1, 0, 0, 0, 0, 0);
    }
    i32 st = -1;
    (void)wait_for(pid, &st);
    put_bool("fork child has no itimer: ", ((st >> 8) & 0xff) == 0 && (st & 0x7f) == 0);
    (void)sys_setitimer(ITIMER_REAL, &zero, 0);

    put("done\n");
    return 0;
}
