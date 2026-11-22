/* Signals in the guest: a SIGUSR1 handler raised with tgkill; a SIGSEGV handler
 * that recovers from a data fault and from an instruction fetch fault by editing
 * the saved pc; sigaltstack with a handler on the alternate stack; and a sigprocmask
 * block whose pending signal is delivered on unblock. Freestanding.
 *
 * The output holds only comparisons and small numbers, no addresses or pids, so it
 * is the same natively and in the engine. */
#include "sys.h"

#define SYS_RT_SIGACTION 134
#define SYS_RT_SIGPROCMASK 135
#define SYS_SIGALTSTACK 132
#define SYS_TGKILL 131
#define SYS_GETPID 172
#define SYS_GETTID 178

#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SA_SIGINFO 0x4ull
#define SA_ONSTACK 0x08000000ull
#define SA_RESTORER 0x04000000ull
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define PROT_NONE 0
#define MAP_PRIVATE_ANON 0x22

/* Kernel structures, arm64 layout. */
typedef struct {
    u64 handler, flags, restorer, mask;
} ksigaction;

typedef struct {
    void *ss_sp;
    i32 ss_flags;
    u64 ss_size;
} kstack;

typedef struct {
    i32 signo, err, code, pad;
    u64 w[14]; /* w[0] is the union at offset 16: si_addr, or si_pid in the low 32 bits */
} kinfo;

typedef struct {
    u64 fault_address;
    u64 regs[31];
    u64 sp, pc, pstate;
    u8 reserved[4096] __attribute__((aligned(16)));
} kmcontext;

typedef struct {
    u64 uc_flags;
    u64 uc_link;
    kstack uc_stack;
    u64 uc_sigmask;
    u8 unused[127];
    kmcontext uc_mcontext;
} kucontext;

/* Returns through the restorer, as bionic does. */
__asm__(".text\n"
        ".global xiaoji_restore\n"
        "xiaoji_restore:\n"
        "  mov x8, #139\n"
        "  svc #0\n");
extern void xiaoji_restore(void);
#define RESTORER ((u64)xiaoji_restore)

static i64 sys_getpid(void) { return sc6(SYS_GETPID, 0, 0, 0, 0, 0, 0); }
static i64 sys_gettid(void) { return sc6(SYS_GETTID, 0, 0, 0, 0, 0, 0); }
static void sys_tgkill(i64 tgid, i64 tid, i64 sig) { sc6(SYS_TGKILL, tgid, tid, sig, 0, 0, 0); }
static void install(int sig, void *handler, u64 flags, u64 restorer)
{
    ksigaction sa;
    sa.handler = (u64)handler;
    sa.flags = flags;
    sa.restorer = restorer;
    sa.mask = 0;
    sc6(SYS_RT_SIGACTION, sig, (i64)&sa, 0, 8, 0, 0);
}
static i64 sys_sigprocmask(i64 how, const u64 *set, u64 *old)
{
    return sc6(SYS_RT_SIGPROCMASK, how, (i64)set, (i64)old, 8, 0, 0);
}
static i64 sys_sigaltstack(const kstack *ss, kstack *old)
{
    return sc6(SYS_SIGALTSTACK, (i64)ss, (i64)old, 0, 0, 0, 0);
}

/* ---------- SIGUSR1 via tgkill ---------- */

static volatile int usr1_count, usr1_signo, usr1_code, usr1_pid_ok, usr1_in_mask, usr1_fp_ok;
static volatile u64 usr1_saved_mask;

static void usr1_handler(int sig, kinfo *info, kucontext *uc)
{
    u64 cur = 0;
    usr1_count++;
    usr1_signo = sig;
    usr1_code = info->code;
    usr1_pid_ok = (info->w[0] & 0xFFFFFFFFu) == (u64)sys_getpid();
    usr1_saved_mask = uc->uc_sigmask;
    /* __reserved starts with the fpsimd record (magic, size 528), then a null record. */
    const u32 *res = (const u32 *)uc->uc_mcontext.reserved;
    usr1_fp_ok = res[0] == 0x46508001u && res[1] == 528 && res[528 / 4] == 0 && res[528 / 4 + 1] == 0;
    sys_sigprocmask(SIG_BLOCK, 0, &cur);
    usr1_in_mask = (int)((cur >> (SIGUSR1 - 1)) & 1);
}

static void test_usr1(void)
{
    install(SIGUSR1, usr1_handler, SA_SIGINFO | SA_RESTORER, RESTORER);
    sys_tgkill(sys_getpid(), sys_gettid(), SIGUSR1);
    put("usr1: count=");
    putu(usr1_count);
    put(" signo=");
    putd(usr1_signo);
    put(" code=");
    putd(usr1_code);
    put(" pid_ok=");
    putu(usr1_pid_ok);
    put(" saved_mask=");
    putu(usr1_saved_mask);
    put(" blocked_in_handler=");
    putu(usr1_in_mask);
    put(" fpsimd_record=");
    putu(usr1_fp_ok);
    put("\n");
    u64 cur = 0;
    sys_sigprocmask(SIG_BLOCK, 0, &cur);
    put("usr1: blocked_after=");
    putu((cur >> (SIGUSR1 - 1)) & 1);
    put("\n");
}

/* ---------- SIGSEGV: data fault and fetch fault, recovered by editing pc ---------- */

static volatile int segv_count, segv_signo, segv_code, segv_addr_ok, segv_fault_ok, segv_pc_ok;
static volatile u64 segv_expect;
static volatile int segv_fetch;

__attribute__((noinline)) static void store_at(u64 addr) { __asm__ volatile("str wzr, [%0]" : : "r"(addr) : "memory"); }

static void segv_handler(int sig, kinfo *info, kucontext *uc)
{
    segv_count++;
    segv_signo = sig;
    segv_code = info->code;
    segv_addr_ok = info->w[0] == segv_expect;
    segv_fault_ok = uc->uc_mcontext.fault_address == segv_expect;
    if (segv_fetch) {
        segv_pc_ok = uc->uc_mcontext.pc == segv_expect;
        uc->uc_mcontext.pc = uc->uc_mcontext.regs[30]; /* return to the caller, past the call */
    } else {
        segv_pc_ok = (uc->uc_mcontext.pc - (u64)store_at) < 64; /* the pc is the store */
        uc->uc_mcontext.pc += 4;                                /* step past the one-insn store */
    }
}

static void segv_report(const char *label)
{
    put(label);
    put(": count=");
    putu(segv_count);
    put(" signo=");
    putd(segv_signo);
    put(" code=");
    putd(segv_code);
    put(" addr_ok=");
    putu(segv_addr_ok);
    put(" fault_ok=");
    putu(segv_fault_ok);
    put(" pc_ok=");
    putu(segv_pc_ok);
    put("\n");
}

static void test_segv(void)
{
    install(SIGSEGV, segv_handler, SA_SIGINFO | SA_RESTORER, RESTORER);

    segv_fetch = 0;
    segv_expect = 0;
    store_at(0); /* unmapped page */
    segv_report("segv data");

    i64 page = sys_mmap(0, 4096, PROT_NONE, MAP_PRIVATE_ANON, -1, 0);
    segv_expect = (u64)page + 8;
    store_at((u64)page + 8); /* mapped, but not writable */
    segv_report("segv access");

    segv_fetch = 1;
    segv_expect = 0;
    void (*volatile fn)(void) = 0;
    fn(); /* instruction fetch from address 0 */
    segv_report("segv fetch");
}

/* ---------- sigaltstack ---------- */

static u8 altbuf[16384] __attribute__((aligned(16)));
static volatile int alt_count, alt_inside, alt_flags_in;

static void alt_handler(int sig, kinfo *info, kucontext *uc)
{
    volatile u64 marker = 0;
    kstack st;
    (void)sig;
    (void)info;
    (void)uc;
    alt_count++;
    alt_inside = ((u64)&marker - (u64)altbuf) < sizeof altbuf;
    sys_sigaltstack(0, &st);
    alt_flags_in = st.ss_flags;
}

static void test_altstack(void)
{
    kstack ss = {altbuf, 0, sizeof altbuf};
    sys_sigaltstack(&ss, 0);
    install(SIGUSR1, alt_handler, SA_SIGINFO | SA_ONSTACK | SA_RESTORER, RESTORER);
    sys_tgkill(sys_getpid(), sys_gettid(), SIGUSR1);
    put("altstack: count=");
    putu(alt_count);
    put(" on_altstack=");
    putu(alt_inside);
    put(" flags_in_handler=");
    putd(alt_flags_in);
    put("\n");
    kstack after;
    sys_sigaltstack(0, &after);
    put("altstack: after flags=");
    putd(after.ss_flags);
    put(" sp_ok=");
    putu(after.ss_sp == altbuf);
    put(" size_ok=");
    putu(after.ss_size == sizeof altbuf);
    put("\n");
}

/* ---------- sigprocmask: a blocked signal is delivered on unblock ---------- */

static volatile int mask_count;

/* No SA_RESTORER here, so the engine's trampoline page returns from the handler. */
static void mask_handler(int sig)
{
    (void)sig;
    mask_count++;
}

static void test_mask(void)
{
    install(SIGUSR2, mask_handler, 0, 0);
    u64 set = 1ull << (SIGUSR2 - 1), old = 99;
    sys_sigprocmask(SIG_BLOCK, &set, &old);
    put("mask: old=");
    putu(old);
    put("\n");
    sys_tgkill(sys_getpid(), sys_gettid(), SIGUSR2);
    put("mask: blocked count=");
    putu(mask_count);
    put("\n");
    sys_sigprocmask(SIG_UNBLOCK, &set, 0);
    put("mask: unblocked count=");
    putu(mask_count);
    put("\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    u64 zero = 0;
    sys_sigprocmask(SIG_SETMASK, &zero, 0); /* start from an empty mask */
    put("signals\n");
    test_usr1();
    test_segv();
    test_altstack();
    test_mask();
    put("done\n");
    return 0;
}
