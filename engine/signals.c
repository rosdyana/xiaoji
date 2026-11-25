/* Signals: the process's handler table, per-thread masks, pending sets and
 * alternate stacks (all in cpu_t), delivery at instruction boundaries, the Linux
 * arm64 rt_sigframe, and the signal syscalls. */
#include "signals.h"

#include <stdio.h>
#include <string.h>

#include "linux.h"
#include "mem.h"
#include "proc.h"
#include "sched.h"

#define SIG_MAX 64
#define SIGSET_SIZE 8
#define SIGKILL_N 9
#define SIGSTOP_N 19
#define SIG_DFL_H 0ull
#define SIG_IGN_H 1ull
#define SA_SIZE 32

#define SA_SIGINFO 0x00000004ull
#define SA_ONSTACK 0x08000000ull
#define SA_RESTORER 0x04000000ull
#define SA_NODEFER 0x40000000ull
#define SA_RESETHAND 0x80000000ull

#define SS_ONSTACK 1u
#define SS_DISABLE 2u
#define STACK_T_SIZE 24

#define SI_USER 0
#define SI_TKILL (-6)

/* Signals that can never be blocked or caught. */
#define UNBLOCKABLE ((1ull << (SIGKILL_N - 1)) | (1ull << (SIGSTOP_N - 1)))

/* The Linux arm64 rt_sigframe. Offsets from the frame base (the handler's sp):
 *    0  siginfo_t, 128 bytes (si_signo 0, si_errno 4, si_code 8, union at 16)
 *  128  ucontext
 *        128 uc_flags, 136 uc_link
 *        144 uc_stack: ss_sp (8), ss_flags (4), pad, ss_size (8) at 160
 *        168 uc_sigmask (8), 176..303 sigset padding to 128 bytes
 *        304 uc_mcontext (sigcontext), 16-byte aligned:
 *              304 fault_address
 *              312 regs[0..30] (x0..x30), 8 bytes each
 *              560 sp, 568 pc, 576 pstate (NZCV in bits 31..28)
 *              592 __reserved[4096]:
 *                    592 fpsimd_context: magic 0x46508001, size 528,
 *                        fpsr (4) at 600, fpcr (4) at 604, vregs[32] at 608
 *                    1120 null _aarch64_ctx record (magic 0, size 0)
 * Total 4688 bytes. */
#define FRAME_SIZE 4688
#define UC_OFF 128
#define UC_STACK_OFF 144
#define UC_SIGMASK_OFF 168
#define MC_OFF 304
#define MC_REGS_OFF 312
#define MC_SP_OFF 560
#define MC_PC_OFF 568
#define MC_PSTATE_OFF 576
#define RES_OFF 592
#define FPSIMD_MAGIC 0x46508001u
#define FPSIMD_SIZE 528

typedef struct {
    uint64_t handler, flags, restorer, mask;
} guest_sigaction;

/* One process's handler table, and the return trampoline (`mov x8,#139; svc #0`,
 * used when SA_RESTORER is absent). */
struct sigtab {
    guest_sigaction act[SIG_MAX + 1];
    uint64_t tramp_va;
};

static sigtab_t *cur_sig;

size_t sig_tab_size(void) { return sizeof(sigtab_t); }

void sig_tab_switch(sigtab_t *t) { cur_sig = t; }

void sig_tab_copy(sigtab_t *dst, const sigtab_t *src) { memcpy(dst, src, sizeof *dst); }

static uint64_t bit(int sig) { return 1ull << (sig - 1); }

/* Default action "ignore": SIGCHLD, SIGCONT, SIGURG and SIGWINCH. */
static int default_ignored(int sig) { return sig == 17 || sig == 18 || sig == 23 || sig == 28; }

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

static int alt_enabled(const cpu_t *c) { return c->alt_size != 0 && !(c->alt_flags & SS_DISABLE); }

static int on_altstack(const cpu_t *c, uint64_t sp) { return alt_enabled(c) && sp - c->alt_sp < c->alt_size; }

static uint32_t altstack_flags(const cpu_t *c)
{
    if (on_altstack(c, c->sp))
        return SS_ONSTACK;
    return alt_enabled(c) ? 0 : SS_DISABLE;
}

/* The handler table of the process a thread belongs to (a signal may be raised for a
 * thread of another process). */
static sigtab_t *tab_of(const cpu_t *c) { return sched_proc_of(c)->sig; }

/* execve: a caught signal goes back to the default action, an ignored one stays
 * ignored, and the mask and the pending set stay. The alternate stack is disabled, and
 * the return trampoline is forgotten (it lives in the old address space and is mapped
 * again when needed). */
void sig_exec_reset(cpu_t *c)
{
    sigtab_t *t = tab_of(c);
    for (int sig = 1; sig <= SIG_MAX; sig++)
        if (t->act[sig].handler > SIG_IGN_H)
            t->act[sig] = (guest_sigaction){SIG_DFL_H, 0, 0, 0};
    t->tramp_va = 0;
    c->alt_sp = 0;
    c->alt_size = 0;
    c->alt_flags = 0;
}

/* A signal that is ignored when raised is dropped, unless it is blocked. */
static int ignored_for(const cpu_t *c, int sig)
{
    if (c->sig_mask & bit(sig))
        return 0;
    uint64_t h = tab_of(c)->act[sig].handler;
    return h == SIG_IGN_H || (h == SIG_DFL_H && default_ignored(sig));
}

/* A synchronous fault can be caught only by a real handler that is not blocked. */
static int catchable_now(const cpu_t *c, int sig)
{
    return tab_of(c)->act[sig].handler > SIG_IGN_H && !(c->sig_mask & bit(sig));
}

static void die_by_signal(int sig)
{
    fflush(stdout);
    fprintf(stderr, "xiaoji: killed by signal %d\n", sig);
    sched_exit_signal(sig);
}

void sig_segv_fatal(uint64_t addr, uint64_t pc)
{
    fflush(stdout);
    fprintf(stderr, "xiaoji: segfault at 0x%llx (pc 0x%llx)\n", (unsigned long long)addr,
            (unsigned long long)pc);
    sched_exit_signal(SIGSEGV_N);
}

void sig_bus_fatal(uint64_t addr, uint64_t pc)
{
    fflush(stdout);
    fprintf(stderr, "xiaoji: bus error at 0x%llx (pc 0x%llx)\n", (unsigned long long)addr,
            (unsigned long long)pc);
    sched_exit_signal(SIGBUS_N);
}

static int ensure_trampoline(void)
{
    if (cur_sig->tramp_va)
        return 0;
    uint64_t va = mem_find_free(MEM_PAGE_SIZE, 0);
    if (!va || mem_map(va, MEM_PAGE_SIZE, MEM_PROT_R | MEM_PROT_X) < 0)
        return -1;
    uint8_t code[8];
    put32(code, 0xD2801168u);     /* mov x8, #139 (rt_sigreturn) */
    put32(code + 4, 0xD4000001u); /* svc #0 */
    if (mem_poke(va, code, sizeof code) < 0)
        return -1;
    cur_sig->tramp_va = va;
    return 0;
}

/* Builds the rt_sigframe for sig and points the thread at the handler. For a
 * synchronous fault, si_addr and fault_address are addr; otherwise si_pid is pid. */
static int setup_frame(cpu_t *c, int sig, int code, int sync, uint64_t addr, uint32_t pid)
{
    const guest_sigaction *sa = &cur_sig->act[sig];
    /* A pipe write part done when a signal arrives (its svc is due to run again) returns
     * the bytes it wrote, as on Linux, rather than restarting: the handler must not see
     * a stale io_done. */
    if (!sync && c->io_done) {
        c->pc += 4;
        c->x[0] = c->io_done;
        c->io_done = 0;
    }
    uint64_t sp = c->sp;
    if ((sa->flags & SA_ONSTACK) && alt_enabled(c) && !on_altstack(c, sp))
        sp = c->alt_sp + c->alt_size;
    uint64_t frame = (sp - FRAME_SIZE) & ~15ull;
    uint64_t tramp = 0;
    if (!(sa->flags & SA_RESTORER)) {
        if (ensure_trampoline() < 0)
            return -1;
        tramp = cur_sig->tramp_va;
    }

    uint8_t f[FRAME_SIZE];
    memset(f, 0, sizeof f);
    put32(f, (uint32_t)sig);
    put32(f + 8, (uint32_t)code);
    if (sync) {
        put64(f + 16, addr);
    } else {
        put32(f + 16, pid);
        put64(f + 24, c->sig_value[sig]); /* si_value (sigqueue) or si_status (SIGCHLD) */
    }
    put64(f + UC_STACK_OFF, c->alt_sp);
    put32(f + UC_STACK_OFF + 8, altstack_flags(c));
    put64(f + UC_STACK_OFF + 16, c->alt_size);
    /* A signal that ends a rt_sigsuspend carries the mask from before the suspend. */
    put64(f + UC_SIGMASK_OFF, c->sig_suspended ? c->sig_saved : c->sig_mask);
    c->sig_suspended = 0;
    put64(f + MC_OFF, (sync && (sig == SIGSEGV_N || sig == SIGBUS_N)) ? addr : 0);
    for (int i = 0; i < 31; i++)
        put64(f + MC_REGS_OFF + 8 * i, c->x[i]);
    put64(f + MC_SP_OFF, c->sp);
    put64(f + MC_PC_OFF, c->pc);
    put64(f + MC_PSTATE_OFF, c->nzcv);
    put32(f + RES_OFF, FPSIMD_MAGIC);
    put32(f + RES_OFF + 4, FPSIMD_SIZE);
    put32(f + RES_OFF + 8, (uint32_t)c->fpsr);
    put32(f + RES_OFF + 12, (uint32_t)c->fpcr);
    for (int n = 0; n < 32; n++) {
        put64(f + RES_OFF + 16 + 16 * n, c->v[n][0]);
        put64(f + RES_OFF + 24 + 16 * n, c->v[n][1]);
    }
    if (mem_copy_in(frame, f, sizeof f) < 0) {
        if (sig_debug_faults()) {
            uint64_t rs = 0, re = 0, roff = 0;
            const char *rpath = NULL;
            int in = mem_region_next(frame, &rs, &re, &rpath, &roff) && rs <= frame && frame < re;
            fprintf(stderr, "xiaoji: signal %d: frame at 0x%llx not writable (sp 0x%llx, altstack 0x%llx+0x%llx flags %u, "
                            "SA_ONSTACK %d): %s prot %u region 0x%llx-0x%llx %s\n",
                    sig, (unsigned long long)frame, (unsigned long long)c->sp, (unsigned long long)c->alt_sp,
                    (unsigned long long)c->alt_size, (unsigned)c->alt_flags, (sa->flags & SA_ONSTACK) != 0,
                    in ? "mapped" : "not mapped", in ? mem_prot_at(frame) : 0u, (unsigned long long)rs,
                    (unsigned long long)re, in && rpath ? rpath : "(anon)");
        }
        return -1;
    }

    /* The handler's view: the signal number, then (with SA_SIGINFO) the siginfo and
     * ucontext pointers, the frame as sp, and the restorer as the return address. */
    c->x[0] = (uint64_t)sig;
    if (sa->flags & SA_SIGINFO) {
        c->x[1] = frame;
        c->x[2] = frame + UC_OFF;
    }
    c->x[30] = (sa->flags & SA_RESTORER) ? sa->restorer : tramp;
    c->sp = frame;
    c->pc = sa->handler;
    uint64_t m = c->sig_mask | sa->mask;
    if (!(sa->flags & SA_NODEFER))
        m |= bit(sig);
    c->sig_mask = m & ~UNBLOCKABLE;
    c->excl_valid = 0;
    if (sa->flags & SA_RESETHAND)
        cur_sig->act[sig].handler = SIG_DFL_H;
    return 0;
}

int sig_interrupt_pending(const cpu_t *c)
{
    return (c->sig_pending & ~c->sig_mask) != 0;
}

void sig_deliver_pending(cpu_t *c)
{
    uint64_t ready = c->sig_pending & ~c->sig_mask;
    if (!ready)
        return;
    int sig = __builtin_ctzll(ready) + 1;
    c->sig_pending &= ~bit(sig);
    uint64_t h = cur_sig->act[sig].handler;
    if (h == SIG_IGN_H || (h == SIG_DFL_H && default_ignored(sig)))
        return;
    if (h == SIG_DFL_H) {
        die_by_signal(sig);
        return;
    }
    if (setup_frame(c, sig, c->sig_code[sig], 0, 0, c->sig_pid[sig]) < 0)
        sig_segv_fatal(c->sp, c->pc);
}

int sig_catchable(const cpu_t *c, int sig)
{
    return catchable_now(c, sig);
}

/* XIAOJI_DEBUG_FAULTS=1 (read by main.c): a line on stderr for each fault delivered
 * to a guest handler. Other behaviour is unchanged. */
static int debug_faults;

void sig_set_debug_faults(int on)
{
    debug_faults = on;
}

int sig_debug_faults(void)
{
    return debug_faults;
}

int sig_fault(cpu_t *c, int sig, int code, uint64_t addr)
{
    if (!catchable_now(c, sig))
        return 0;
    if (sig_debug_faults()) {
        fprintf(stderr, "xiaoji: deliver signal %d (si_code %d) to guest handler: addr 0x%llx pc 0x%llx\n",
                sig, code, (unsigned long long)addr, (unsigned long long)c->pc);
        if (sig == 11 && code == 2) { /* SEGV_ACCERR: the mapping and its protection at the address (B16) */
            uint64_t rs = 0, re = 0, roff = 0;
            const char *rpath = NULL;
            if (mem_region_next(addr, &rs, &re, &rpath, &roff) && rs <= addr && addr < re)
                fprintf(stderr, "xiaoji:   data 0x%llx: prot %u, region 0x%llx-0x%llx %s off 0x%llx\n",
                        (unsigned long long)addr, mem_prot_at(addr), (unsigned long long)rs,
                        (unsigned long long)re, rpath ? rpath : "(anon)", (unsigned long long)roff);
            else
                fprintf(stderr, "xiaoji:   data 0x%llx: not mapped\n", (unsigned long long)addr);
        }
    }
    if (setup_frame(c, sig, code, 1, addr, 0) < 0)
        sig_segv_fatal(c->sp, c->pc);
    return 1;
}

#define SIGSYS_N 31
#define SYS_SECCOMP 1 /* si_code */

void sig_seccomp_trap(cpu_t *c, int32_t nr, uint32_t data, uint32_t arch)
{
    if (!catchable_now(c, SIGSYS_N)) {
        die_by_signal(SIGSYS_N);
        return;
    }
    /* sync: si_call_addr (offset 16) is the address after the svc, and fault_address stays 0 */
    if (setup_frame(c, SIGSYS_N, SYS_SECCOMP, 1, c->pc, 0) < 0) {
        sig_segv_fatal(c->sp, c->pc);
        return;
    }
    uint8_t w[4];
    put32(w, data); /* si_errno: SECCOMP_RET_DATA */
    uint8_t u[8];
    put32(u, (uint32_t)nr); /* si_syscall */
    put32(u + 4, arch);     /* si_arch */
    if (mem_copy_in(c->sp + 4, w, sizeof w) < 0 || mem_copy_in(c->sp + 24, u, sizeof u) < 0)
        sig_segv_fatal(c->sp, c->pc);
}

void sig_rt_sigreturn(cpu_t *c)
{
    uint8_t f[FRAME_SIZE];
    c->sig_restore = 1;
    if (mem_copy_out(f, c->sp, sizeof f) < 0) {
        sig_segv_fatal(c->sp, c->pc);
        return;
    }
    for (int i = 0; i < 31; i++)
        c->x[i] = get64(f + MC_REGS_OFF + 8 * i);
    c->sp = get64(f + MC_SP_OFF);
    c->pc = get64(f + MC_PC_OFF);
    c->nzcv = (uint32_t)get64(f + MC_PSTATE_OFF) & 0xF0000000u;
    c->sig_mask = get64(f + UC_SIGMASK_OFF) & ~UNBLOCKABLE;
    c->alt_sp = get64(f + UC_STACK_OFF);
    c->alt_flags = (get32(f + UC_STACK_OFF + 8) & SS_DISABLE) ? SS_DISABLE : 0;
    c->alt_size = get64(f + UC_STACK_OFF + 16);
    c->excl_valid = 0;
    for (size_t p = RES_OFF; p + 8 <= FRAME_SIZE;) {
        uint32_t magic = get32(f + p), size = get32(f + p + 4);
        if (magic == 0 || size < 8 || size > FRAME_SIZE - p)
            break;
        if (magic == FPSIMD_MAGIC && size == FPSIMD_SIZE) {
            c->fpsr = get32(f + p + 8);
            c->fpcr = get32(f + p + 12);
            for (int n = 0; n < 32; n++) {
                c->v[n][0] = get64(f + p + 16 + 16 * n);
                c->v[n][1] = get64(f + p + 24 + 16 * n);
            }
        }
        p += size;
    }
}

/* ---------- syscalls ---------- */

int64_t sig_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oact, uint64_t sigsetsize)
{
    if (sigsetsize != SIGSET_SIZE || sig < 1 || sig > SIG_MAX)
        return -LE_INVAL;
    if (act && (sig == SIGKILL_N || sig == SIGSTOP_N))
        return -LE_INVAL;
    if (oact && mem_copy_in(oact, &cur_sig->act[sig], SA_SIZE) < 0)
        return -LE_FAULT;
    if (act) {
        guest_sigaction n;
        if (mem_copy_out(&n, act, SA_SIZE) < 0)
            return -LE_FAULT;
        cur_sig->act[sig] = n;
    }
    return 0;
}

int64_t sig_rt_sigprocmask(cpu_t *c, uint64_t how, uint64_t set, uint64_t oset, uint64_t sigsetsize)
{
    if (sigsetsize != SIGSET_SIZE)
        return -LE_INVAL;
    uint64_t old = c->sig_mask, now = old;
    if (set) {
        uint64_t s;
        if (mem_copy_out(&s, set, sizeof s) < 0)
            return -LE_FAULT;
        switch (how) {
        case 0: now = old | s; break;  /* SIG_BLOCK */
        case 1: now = old & ~s; break; /* SIG_UNBLOCK */
        case 2: now = s; break;        /* SIG_SETMASK */
        default: return -LE_INVAL;
        }
        now &= ~UNBLOCKABLE;
    }
    if (oset && mem_copy_in(oset, &old, sizeof old) < 0)
        return -LE_FAULT;
    c->sig_mask = now; /* a signal unblocked here is delivered at the next instruction */
    return 0;
}

int64_t sig_sigaltstack(cpu_t *c, uint64_t ss, uint64_t oss)
{
    if (oss) {
        uint8_t b[STACK_T_SIZE];
        memset(b, 0, sizeof b);
        put64(b, c->alt_sp);
        put32(b + 8, altstack_flags(c));
        put64(b + 16, c->alt_size);
        if (mem_copy_in(oss, b, sizeof b) < 0)
            return -LE_FAULT;
    }
    if (ss) {
        uint8_t b[STACK_T_SIZE];
        if (mem_copy_out(b, ss, sizeof b) < 0)
            return -LE_FAULT;
        uint64_t sp = get64(b), size = get64(b + 16);
        uint32_t flags = get32(b + 8);
        if (on_altstack(c, c->sp))
            return -LE_PERM;
        if (flags & ~SS_DISABLE)
            return -LE_INVAL;
        if (flags & SS_DISABLE) {
            c->alt_sp = 0;
            c->alt_size = 0;
            c->alt_flags = SS_DISABLE;
        } else {
            c->alt_sp = sp;
            c->alt_size = size;
            c->alt_flags = 0;
        }
    }
    return 0;
}

/* siginfo for a signal that was waited for or delivered without a frame. */
static int write_siginfo(uint64_t va, int sig, int code, uint32_t pid, uint64_t value)
{
    uint8_t b[128];
    memset(b, 0, sizeof b);
    put32(b, (uint32_t)sig);
    put32(b + 8, (uint32_t)code);
    put32(b + 16, pid);
    put64(b + 24, value);
    return mem_copy_in(va, b, sizeof b);
}

/* Raises sig for thread t. A thread waiting in rt_sigtimedwait for sig takes it
 * at once; otherwise it becomes pending, and an interruptible wait ends with EINTR.
 * SIGKILL on the running thread ends its process now. On another thread it is pending
 * (it cannot be blocked or caught) and the thread is woken to die. */
static void raise_for(cpu_t *t, int sig, int code, uint32_t pid, uint64_t value)
{
    if (sig == SIGKILL_N && t == sched_cur_cpu()) {
        die_by_signal(sig);
        return;
    }
    if (sig != SIGKILL_N && ignored_for(t, sig))
        return;
    uint64_t b = bit(sig);
    if (sched_wait_kind(t) == SW_SIGWAIT && (t->sig_wait_set & b)) {
        if (t->sig_wait_info)
            (void)write_siginfo(t->sig_wait_info, sig, code, pid, value);
        t->sig_wait_set = 0;
        sched_wake(t, sig);
        return;
    }
    t->sig_pending |= b;
    t->sig_code[sig] = code;
    t->sig_pid[sig] = pid;
    t->sig_value[sig] = value;
    if (!(t->sig_mask & b) && (sig == SIGKILL_N || sched_wait_kind(t) != SW_NONE))
        sched_wake(t, -(int64_t)LE_INTR);
}

/* The thread of process p that takes a process-directed signal: one that waits for it,
 * else the first that does not block it, else the first thread (where it stays pending). */
static cpu_t *process_target(proc_t *p, int sig)
{
    cpu_t *list[p->nthreads ? p->nthreads : 1]; /* every thread of the process: no fixed cap (B16) */
    int n = sched_threads_of(p, list, p->nthreads ? p->nthreads : 1);
    uint64_t b = bit(sig);
    for (int i = 0; i < n; i++)
        if (sched_wait_kind(list[i]) == SW_SIGWAIT && (list[i]->sig_wait_set & b))
            return list[i];
    for (int i = 0; i < n; i++)
        if (!(list[i]->sig_mask & b))
            return list[i];
    return n ? list[0] : NULL;
}

/* A process-directed signal to process p. SIGKILL reaches every thread of it. */
static void signal_process(proc_t *p, int sig, int code, uint32_t sender, uint64_t value)
{
    if (sig == SIGKILL_N) {
        cpu_t *list[p->nthreads ? p->nthreads : 1]; /* every thread of the process: no fixed cap (B16) */
        int n = sched_threads_of(p, list, p->nthreads ? p->nthreads : 1);
        for (int i = 0; i < n; i++)
            raise_for(list[i], sig, code, sender, value);
        return;
    }
    cpu_t *t = process_target(p, sig);
    if (t)
        raise_for(t, sig, code, sender, value);
}

/* SIGCHLD (or another child-state signal) for a parent: code is CLD_EXITED or CLD_KILLED. */
void sig_child_event(proc_t *parent, int sig, int code, uint32_t pid, int status)
{
    cpu_t *t = process_target(parent, sig);
    if (!t)
        return;
    raise_for(t, sig, code, pid, (uint64_t)(uint32_t)status);
}

/* kill(2): pid > 0 is one process; 0 the caller's process group; -1 every process but
 * the initial one; less than -1 the group -pid. Sig 0 only checks that the target exists. */
int64_t sig_kill(uint64_t pid64, uint64_t sig)
{
    int32_t pid = (int32_t)pid64;
    if (sig > SIG_MAX)
        return -LE_INVAL;
    proc_t *me = proc_cur();
    if (pid > 0) {
        proc_t *p = proc_find(pid);
        if (!p || p->zombie || p->exited)
            return -LE_SRCH;
        if (sig) {
            if (sig_debug_faults())
                fprintf(stderr, "xiaoji: kill: pid %d sends signal %d to pid %d\n", me->pid, (int)sig, p->pid);
            signal_process(p, (int)sig, SI_USER, (uint32_t)me->pid, 0);
        }
        return 0;
    }
    int64_t group = pid == 0 ? me->pgid : -(int64_t)pid;
    int any = 0;
    for (proc_t *p = proc_first(); p; p = proc_next(p)) {
        if (p->zombie || p->exited)
            continue;
        if (pid == -1 ? proc_is_initial(p) : p->pgid != group)
            continue;
        any = 1;
        if (sig)
            signal_process(p, (int)sig, SI_USER, (uint32_t)me->pid, 0);
    }
    return any ? 0 : -LE_SRCH;
}

static int64_t thread_kill(uint64_t tid, uint64_t sig)
{
    if (sig > SIG_MAX)
        return -LE_INVAL;
    cpu_t *t = sched_find_thread((uint64_t)(uint32_t)tid);
    if (!t)
        return -LE_SRCH;
    if (sig == 0)
        return 0;
    raise_for(t, (int)sig, SI_TKILL, proc_cur()->pid, 0);
    return 0;
}

int64_t sig_tkill(uint64_t tid, uint64_t sig) { return thread_kill(tid, sig); }

/* A timer's signal (timers.c): to process p, or to thread tid of p when tid is not 0. Returns 0, or -1
 * when there is no such target. si_pid holds the timer id for SI_TIMER and si_value the sigevent value. */
int sig_timer_raise(proc_t *p, int tid, int sig, int code, uint32_t id, uint64_t value)
{
    if (!tid) {
        if (p->zombie || p->exited)
            return -1;
        signal_process(p, sig, code, id, value);
        return 0;
    }
    cpu_t *list[p->nthreads ? p->nthreads : 1]; /* every thread of the process: no fixed cap (B16) */
    int n = sched_threads_of(p, list, p->nthreads ? p->nthreads : 1);
    for (int i = 0; i < n; i++)
        if ((int)list[i]->tid == tid) {
            raise_for(list[i], sig, code, id, value);
            return 0;
        }
    return -1;
}

/* Whether sig is pending for the target of a timer (the same target as sig_timer_raise). */
int sig_timer_pending(proc_t *p, int tid, int sig)
{
    cpu_t *list[p->nthreads ? p->nthreads : 1]; /* every thread of the process: no fixed cap (B16) */
    int n = sched_threads_of(p, list, p->nthreads ? p->nthreads : 1);
    for (int i = 0; i < n; i++) {
        if (tid && (int)list[i]->tid != tid)
            continue;
        if (list[i]->sig_pending & bit(sig))
            return 1;
    }
    return 0;
}

/* Raises sig for the running thread (SIGPIPE from a write to a pipe with no reader). */
void sig_raise_current(int sig)
{
    cpu_t *c = sched_cur_cpu();
    if (c)
        raise_for(c, sig, SI_USER, (uint32_t)proc_cur()->pid, 0);
}

int64_t sig_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig)
{
    if ((int32_t)tgid != proc_cur()->pid)
        return -LE_SRCH;
    return thread_kill(tid, sig);
}

/* rt_sigqueueinfo and rt_tgsigqueueinfo. The guest's siginfo gives si_code and si_value;
 * the sender is the caller. Linux (do_rt_sigqueueinfo) refuses a non-negative si_code, and
 * SI_TKILL, unless the target is the caller's own process: the kernel's own kinds of signal
 * (kill(2), tgkill) cannot be forged for another process. */
static int64_t sigqueue_args(uint64_t sig, uint64_t uinfo, int32_t *code, uint64_t *value)
{
    uint8_t b[128];
    if (mem_copy_out(b, uinfo, sizeof b) < 0)
        return -LE_FAULT;
    if (sig > SIG_MAX)
        return -LE_INVAL;
    *code = (int32_t)get32(b + 8);
    *value = get64(b + 24);
    return 0;
}

static int forges_sender(int32_t code)
{
    return code >= 0 || code == SI_TKILL;
}

int64_t sig_rt_sigqueueinfo(uint64_t pid, uint64_t sig, uint64_t uinfo)
{
    int32_t code;
    uint64_t value;
    int64_t rc = sigqueue_args(sig, uinfo, &code, &value);
    if (rc < 0)
        return rc;
    proc_t *me = proc_cur();
    if ((int32_t)pid != me->pid && forges_sender(code))
        return -LE_PERM;
    proc_t *p = proc_find((int32_t)pid);
    if (!p || p->zombie || p->exited)
        return -LE_SRCH;
    if (sig)
        signal_process(p, (int)sig, code, (uint32_t)me->pid, value);
    return 0;
}

int64_t sig_rt_tgsigqueueinfo(uint64_t tgid, uint64_t tid, uint64_t sig, uint64_t uinfo)
{
    int32_t code;
    uint64_t value;
    int64_t rc = sigqueue_args(sig, uinfo, &code, &value);
    if (rc < 0)
        return rc;
    proc_t *me = proc_cur();
    if ((int32_t)tgid != me->pid && forges_sender(code))
        return -LE_PERM;
    proc_t *p = proc_find((int32_t)tgid);
    if (!p || p->zombie || p->exited)
        return -LE_SRCH;
    cpu_t *want = sched_find_thread((uint32_t)tid);
    cpu_t *list[p->nthreads ? p->nthreads : 1]; /* every thread of the process: no fixed cap (B16) */
    int n = sched_threads_of(p, list, p->nthreads ? p->nthreads : 1);
    int member = 0;
    for (int i = 0; i < n && want; i++)
        if (list[i] == want)
            member = 1;
    if (!want || !member)
        return -LE_SRCH;
    if (sig)
        raise_for(want, (int)sig, code, (uint32_t)me->pid, value);
    return 0;
}

/* rt_sigsuspend: replaces the mask with set and waits for a signal that is caught (or
 * that the default action ends the process for). The handler's frame restores the mask
 * from before the call, and the call returns -EINTR, as on Linux. */
int64_t sig_sigsuspend(cpu_t *c, uint64_t set, uint64_t sigsetsize)
{
    if (sigsetsize != SIGSET_SIZE)
        return -LE_INVAL;
    c->sig_saved = c->sig_mask;
    c->sig_suspended = 1;
    c->sig_mask = set & ~UNBLOCKABLE;
    for (int sig = 1; sig <= SIG_MAX; sig++) {
        if (!(c->sig_pending & bit(sig)) || (c->sig_mask & bit(sig)))
            continue;
        if (tab_of(c)->act[sig].handler != SIG_IGN_H && !(default_ignored(sig) && tab_of(c)->act[sig].handler == SIG_DFL_H))
            return -LE_INTR; /* delivered at the next instruction boundary */
    }
    sched_block_suspend(); /* sched_wake (raise_for) ends the wait with -EINTR */
    return 0;
}

int64_t sig_timedwait(cpu_t *c, uint64_t wanted, uint64_t info, int poll, int has_deadline, uint64_t deadline)
{
    wanted &= ~UNBLOCKABLE;
    uint64_t ready = c->sig_pending & wanted;
    if (ready) {
        int sig = __builtin_ctzll(ready) + 1;
        c->sig_pending &= ~bit(sig);
        if (info && write_siginfo(info, sig, c->sig_code[sig], c->sig_pid[sig], c->sig_value[sig]) < 0)
            return -LE_FAULT;
        return sig;
    }
    if (poll)
        return -LE_AGAIN;
    c->sig_wait_set = wanted;
    c->sig_wait_info = info;
    sched_block_sigwait(has_deadline, deadline); /* x0 is set when the wait ends */
    return 0;
}
