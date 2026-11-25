/* seccomp (B19): strict mode and classic BPF filters, as Linux's kernel/seccomp.c and net/core/filter.c have them.
 *
 * A filter is checked when it is installed (seccomp_check_filter's opcode list and its rewrites of the length loads,
 * then bpf_check_classic's jump, division, shift, scratch-memory and last-instruction rules), and run by the
 * interpreter below on every system call of a thread with filters: each filter of the thread's chain, newest first,
 * gives a 32-bit result, and the most restrictive action wins (the lowest action as a signed number; for equal
 * actions the newest filter's data). The actions follow __seccomp_filter: ERRNO (data clamped to 4095), TRAP (a forced
 * SIGSYS, si_code SYS_SECCOMP), TRACE and USER_NOTIF (no tracer, no listener: ENOSYS), LOG and ALLOW (the call runs),
 * KILL_THREAD (the thread ends; the process when it is the last thread) and KILL_PROCESS and any unknown action (the
 * process dies by SIGSYS). Strict mode allows read, write, exit and rt_sigreturn and ends the thread with SIGKILL
 * otherwise.
 *
 * The verdict cache: when a run of the chain for syscall nr reads only nr and arch from seccomp_data (arch is always
 * AUDIT_ARCH_AARCH64), its result is the same for every call with that nr, whatever the arguments and the
 * instruction pointer (the interpreter is deterministic in what it reads, and the checker has made sure the scratch
 * memory is written before it is read). The result is then kept with the chain's newest filter, per nr below 512.
 * A chain never changes (a new filter makes a new head), so a cached result is never stale.
 *
 * Not modelled: the user-notification listener (SECCOMP_FILTER_FLAG_NEW_LISTENER is refused with EINVAL; the
 * USER_NOTIF action itself behaves as Linux's does without a listener), ptrace (TRACE has no tracer), the audit log
 * (LOG and the kill actions write a line on stderr instead), and compat (32-bit) system calls. */
#include "seccomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cred.h"
#include "linux.h"
#include "mem.h"
#include "proc.h"
#include "sched.h"
#include "signals.h"

#define SC_RET_KILL_PROCESS 0x80000000u
#define SC_RET_KILL_THREAD 0x00000000u
#define SC_RET_TRAP 0x00030000u
#define SC_RET_ERRNO 0x00050000u
#define SC_RET_USER_NOTIF 0x7fc00000u
#define SC_RET_TRACE 0x7ff00000u
#define SC_RET_LOG 0x7ffc0000u
#define SC_RET_ALLOW 0x7fff0000u
#define SC_RET_ACTION_FULL 0xffff0000u
#define SC_RET_DATA 0x0000ffffu

#define SC_SET_MODE_STRICT 0
#define SC_SET_MODE_FILTER 1
#define SC_GET_ACTION_AVAIL 2
#define SC_GET_NOTIF_SIZES 3

#define SC_FLAG_TSYNC 1u
#define SC_FLAG_LOG 2u
#define SC_FLAG_SPEC_ALLOW 4u
#define SC_FLAG_NEW_LISTENER 8u
#define SC_FLAG_TSYNC_ESRCH 16u
#define SC_FLAG_WAIT_KILLABLE_RECV 32u
#define SC_FLAG_MASK 63u

#define BPF_MAXINSNS 4096
#define MAX_INSNS_PER_PATH 32768
#define BPF_MEMWORDS 16
#define SECCOMP_DATA_SIZE 64
#define MAX_ERRNO 4095
#define AUDIT_ARCH_AARCH64 0xC00000B7u
#define CACHE_NR 512

/* Classic BPF opcodes. */
#define BPF_LD 0x00
#define BPF_LDX 0x01
#define BPF_ST 0x02
#define BPF_STX 0x03
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_RET 0x06
#define BPF_MISC 0x07
#define BPF_W 0x00
#define BPF_IMM 0x00
#define BPF_ABS 0x20
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
    uint16_t code;
    uint8_t jt, jf;
    uint32_t k;
} bpf_insn;

/* Cache states, per nr. */
#define CACHE_UNKNOWN 0
#define CACHE_FIXED 1
#define CACHE_VARIES 2

struct sc_filter {
    int refs;
    int len;
    int log;                 /* SECCOMP_FILTER_FLAG_LOG */
    struct sc_filter *prev;  /* the older filters of the chain (this filter holds a reference) */
    uint8_t *cache_state;    /* per nr below CACHE_NR, for the chain from here down: NULL until first used */
    uint32_t *cache_val;
    bpf_insn insns[];
};
typedef struct sc_filter sc_filter;

static void filter_put(sc_filter *f)
{
    while (f && --f->refs == 0) {
        sc_filter *prev = f->prev;
        free(f->cache_state);
        free(f->cache_val);
        free(f);
        f = prev;
    }
}

static void filter_get(sc_filter *f)
{
    if (f)
        f->refs++;
}

void seccomp_thread_copy(cpu_t *c) { filter_get(c->sc_filter); }

void seccomp_thread_release(cpu_t *c)
{
    filter_put(c->sc_filter);
    c->sc_filter = NULL;
}

/* ---------- the checker ---------- */

/* seccomp_check_filter: the opcodes seccomp accepts, with its rewrites (a length load becomes an immediate 64). */
static int seccomp_check(bpf_insn *f, int n)
{
    for (int pc = 0; pc < n; pc++) {
        bpf_insn *i = &f[pc];
        switch (i->code) {
        case BPF_LD | BPF_W | BPF_ABS:
            if (i->k >= SECCOMP_DATA_SIZE || (i->k & 3))
                return -LE_INVAL;
            continue;
        case BPF_LD | BPF_W | BPF_LEN:
            i->code = BPF_LD | BPF_IMM;
            i->k = SECCOMP_DATA_SIZE;
            continue;
        case BPF_LDX | BPF_W | BPF_LEN:
            i->code = BPF_LDX | BPF_IMM;
            i->k = SECCOMP_DATA_SIZE;
            continue;
        case BPF_RET | BPF_K:
        case BPF_RET | BPF_A:
        case BPF_ALU | BPF_ADD | BPF_K:
        case BPF_ALU | BPF_ADD | BPF_X:
        case BPF_ALU | BPF_SUB | BPF_K:
        case BPF_ALU | BPF_SUB | BPF_X:
        case BPF_ALU | BPF_MUL | BPF_K:
        case BPF_ALU | BPF_MUL | BPF_X:
        case BPF_ALU | BPF_DIV | BPF_K:
        case BPF_ALU | BPF_DIV | BPF_X:
        case BPF_ALU | BPF_AND | BPF_K:
        case BPF_ALU | BPF_AND | BPF_X:
        case BPF_ALU | BPF_OR | BPF_K:
        case BPF_ALU | BPF_OR | BPF_X:
        case BPF_ALU | BPF_XOR | BPF_K:
        case BPF_ALU | BPF_XOR | BPF_X:
        case BPF_ALU | BPF_LSH | BPF_K:
        case BPF_ALU | BPF_LSH | BPF_X:
        case BPF_ALU | BPF_RSH | BPF_K:
        case BPF_ALU | BPF_RSH | BPF_X:
        case BPF_ALU | BPF_NEG:
        case BPF_LD | BPF_IMM:
        case BPF_LDX | BPF_IMM:
        case BPF_MISC | BPF_TAX:
        case BPF_MISC | BPF_TXA:
        case BPF_LD | BPF_MEM:
        case BPF_LDX | BPF_MEM:
        case BPF_ST:
        case BPF_STX:
        case BPF_JMP | BPF_JA:
        case BPF_JMP | BPF_JEQ | BPF_K:
        case BPF_JMP | BPF_JEQ | BPF_X:
        case BPF_JMP | BPF_JGE | BPF_K:
        case BPF_JMP | BPF_JGE | BPF_X:
        case BPF_JMP | BPF_JGT | BPF_K:
        case BPF_JMP | BPF_JGT | BPF_X:
        case BPF_JMP | BPF_JSET | BPF_K:
        case BPF_JMP | BPF_JSET | BPF_X:
            continue;
        default:
            return -LE_INVAL;
        }
    }
    return 0;
}

static int is_cond_jump(uint16_t code)
{
    switch (code) {
    case BPF_JMP | BPF_JEQ | BPF_K:
    case BPF_JMP | BPF_JEQ | BPF_X:
    case BPF_JMP | BPF_JGE | BPF_K:
    case BPF_JMP | BPF_JGE | BPF_X:
    case BPF_JMP | BPF_JGT | BPF_K:
    case BPF_JMP | BPF_JGT | BPF_X:
    case BPF_JMP | BPF_JSET | BPF_K:
    case BPF_JMP | BPF_JSET | BPF_X:
        return 1;
    }
    return 0;
}

/* check_load_and_stores: a scratch word is read only after a store on every path to the read. */
static int check_mem(const bpf_insn *f, int n)
{
    uint16_t *masks = malloc((size_t)n * sizeof *masks);
    if (!masks)
        return -LE_NOMEM;
    memset(masks, 0xff, (size_t)n * sizeof *masks);
    uint16_t valid = 0;
    int ret = 0;
    for (int pc = 0; pc < n; pc++) {
        valid &= masks[pc];
        uint16_t code = f[pc].code;
        if (code == BPF_ST || code == BPF_STX) {
            valid |= (uint16_t)(1u << f[pc].k);
        } else if (code == (BPF_LD | BPF_MEM) || code == (BPF_LDX | BPF_MEM)) {
            if (!(valid & (1u << f[pc].k))) {
                ret = -LE_INVAL;
                break;
            }
        } else if (code == (BPF_JMP | BPF_JA)) {
            masks[pc + 1 + f[pc].k] &= valid;
            valid = 0xffff;
        } else if (is_cond_jump(code)) {
            masks[pc + 1 + f[pc].jt] &= valid;
            masks[pc + 1 + f[pc].jf] &= valid;
            valid = 0xffff;
        }
    }
    free(masks);
    return ret;
}

/* bpf_check_classic, for the opcodes seccomp_check let through. */
static int classic_check(const bpf_insn *f, int n)
{
    for (int pc = 0; pc < n; pc++) {
        const bpf_insn *i = &f[pc];
        switch (i->code) {
        case BPF_ALU | BPF_DIV | BPF_K:
            if (i->k == 0)
                return -LE_INVAL;
            break;
        case BPF_ALU | BPF_LSH | BPF_K:
        case BPF_ALU | BPF_RSH | BPF_K:
            if (i->k >= 32)
                return -LE_INVAL;
            break;
        case BPF_LD | BPF_MEM:
        case BPF_LDX | BPF_MEM:
        case BPF_ST:
        case BPF_STX:
            if (i->k >= BPF_MEMWORDS)
                return -LE_INVAL;
            break;
        case BPF_JMP | BPF_JA:
            if (i->k >= (uint32_t)(n - pc - 1))
                return -LE_INVAL;
            break;
        default:
            if (is_cond_jump(i->code) && (pc + i->jt + 1 >= n || pc + i->jf + 1 >= n))
                return -LE_INVAL;
            break;
        }
    }
    uint16_t last = f[n - 1].code;
    if (last != (BPF_RET | BPF_K) && last != (BPF_RET | BPF_A))
        return -LE_INVAL;
    return check_mem(f, n);
}

/* ---------- the interpreter ---------- */

/* Runs one checked program on data (struct seccomp_data, 16 words). *other is set when the program read a word
 * other than nr and arch. */
static uint32_t run(const sc_filter *flt, const uint32_t *data, int *other)
{
    uint32_t A = 0, X = 0, M[BPF_MEMWORDS];
    const bpf_insn *f = flt->insns;
    for (int pc = 0;; pc++) {
        const bpf_insn *i = &f[pc];
        uint32_t k = i->k;
        switch (i->code) {
        case BPF_LD | BPF_W | BPF_ABS:
            if (k > 4)
                *other = 1;
            A = data[k >> 2];
            break;
        case BPF_LD | BPF_IMM: A = k; break;
        case BPF_LDX | BPF_IMM: X = k; break;
        case BPF_LD | BPF_MEM: A = M[k]; break;
        case BPF_LDX | BPF_MEM: X = M[k]; break;
        case BPF_ST: M[k] = A; break;
        case BPF_STX: M[k] = X; break;
        case BPF_MISC | BPF_TAX: X = A; break;
        case BPF_MISC | BPF_TXA: A = X; break;
        case BPF_ALU | BPF_ADD | BPF_K: A += k; break;
        case BPF_ALU | BPF_ADD | BPF_X: A += X; break;
        case BPF_ALU | BPF_SUB | BPF_K: A -= k; break;
        case BPF_ALU | BPF_SUB | BPF_X: A -= X; break;
        case BPF_ALU | BPF_MUL | BPF_K: A *= k; break;
        case BPF_ALU | BPF_MUL | BPF_X: A *= X; break;
        case BPF_ALU | BPF_DIV | BPF_K: A /= k; break;
        case BPF_ALU | BPF_DIV | BPF_X:
            if (X == 0)
                return 0; /* as the classic-to-eBPF conversion does: the program returns 0 */
            A /= X;
            break;
        case BPF_ALU | BPF_AND | BPF_K: A &= k; break;
        case BPF_ALU | BPF_AND | BPF_X: A &= X; break;
        case BPF_ALU | BPF_OR | BPF_K: A |= k; break;
        case BPF_ALU | BPF_OR | BPF_X: A |= X; break;
        case BPF_ALU | BPF_XOR | BPF_K: A ^= k; break;
        case BPF_ALU | BPF_XOR | BPF_X: A ^= X; break;
        case BPF_ALU | BPF_LSH | BPF_K: A <<= k; break;
        case BPF_ALU | BPF_LSH | BPF_X: A <<= (X & 31); break; /* eBPF's 32-bit shift by a register */
        case BPF_ALU | BPF_RSH | BPF_K: A >>= k; break;
        case BPF_ALU | BPF_RSH | BPF_X: A >>= (X & 31); break;
        case BPF_ALU | BPF_NEG: A = 0u - A; break;
        case BPF_JMP | BPF_JA: pc += (int)k; break;
        case BPF_JMP | BPF_JEQ | BPF_K: pc += A == k ? i->jt : i->jf; break;
        case BPF_JMP | BPF_JEQ | BPF_X: pc += A == X ? i->jt : i->jf; break;
        case BPF_JMP | BPF_JGT | BPF_K: pc += A > k ? i->jt : i->jf; break;
        case BPF_JMP | BPF_JGT | BPF_X: pc += A > X ? i->jt : i->jf; break;
        case BPF_JMP | BPF_JGE | BPF_K: pc += A >= k ? i->jt : i->jf; break;
        case BPF_JMP | BPF_JGE | BPF_X: pc += A >= X ? i->jt : i->jf; break;
        case BPF_JMP | BPF_JSET | BPF_K: pc += (A & k) ? i->jt : i->jf; break;
        case BPF_JMP | BPF_JSET | BPF_X: pc += (A & X) ? i->jt : i->jf; break;
        case BPF_RET | BPF_K: return k;
        case BPF_RET | BPF_A: return A;
        default: return 0; /* not reached: the checker refused anything else */
        }
    }
}

static int32_t action_of(uint32_t ret) { return (int32_t)(ret & SC_RET_ACTION_FULL); }

/* seccomp_run_filters: the chain, newest first; the lowest action wins, the first (newest) of equals. */
static uint32_t run_chain(const sc_filter *head, const uint32_t *data, int *other)
{
    uint32_t ret = SC_RET_ALLOW;
    for (const sc_filter *f = head; f; f = f->prev) {
        uint32_t cur = run(f, data, other);
        if (action_of(cur) < action_of(ret))
            ret = cur;
    }
    return ret;
}

static uint32_t evaluate(sc_filter *head, uint64_t nr, const uint64_t a[6], uint64_t ip)
{
    uint32_t nr32 = (uint32_t)nr;
    if (nr32 < CACHE_NR && head->cache_state && head->cache_state[nr32] == CACHE_FIXED)
        return head->cache_val[nr32];
    uint32_t data[16];
    data[0] = nr32;
    data[1] = AUDIT_ARCH_AARCH64;
    data[2] = (uint32_t)ip;
    data[3] = (uint32_t)(ip >> 32);
    for (int i = 0; i < 6; i++) {
        data[4 + 2 * i] = (uint32_t)a[i];
        data[5 + 2 * i] = (uint32_t)(a[i] >> 32);
    }
    int other = 0;
    uint32_t ret = run_chain(head, data, &other);
    if (nr32 < CACHE_NR) {
        if (!head->cache_state) {
            head->cache_state = calloc(CACHE_NR, 1);
            head->cache_val = calloc(CACHE_NR, sizeof *head->cache_val);
            if (!head->cache_state || !head->cache_val) {
                free(head->cache_state);
                free(head->cache_val);
                head->cache_state = NULL;
                head->cache_val = NULL;
                return ret;
            }
        }
        if (head->cache_state[nr32] == CACHE_UNKNOWN) {
            head->cache_state[nr32] = other ? CACHE_VARIES : CACHE_FIXED;
            head->cache_val[nr32] = ret;
        }
    }
    return ret;
}

/* ---------- the actions ---------- */

static int live_threads(const proc_t *p)
{
    int max = p->nthreads ? p->nthreads : 1;
    cpu_t *list[max];
    int n = sched_threads_of(p, list, max), live = 0;
    for (int i = 0; i < n; i++)
        live += !list[i]->exited;
    return live;
}

/* The thread ends, or the process when the thread is its last (sig: its fatal signal then). */
static void end_thread(cpu_t *c, int whole, int sig)
{
    proc_t *p = sched_proc_of(c);
    if (whole || live_threads(p) <= 1) {
        fflush(stdout);
        fprintf(stderr, "xiaoji: killed by signal %d\n", sig);
        sched_exit_signal(sig);
    } else {
        sched_thread_exit(c, (uint64_t)sig);
    }
}

static void note(const cpu_t *c, const char *what, uint64_t nr)
{
    proc_t *p = sched_proc_of(c);
    fprintf(stderr, "xiaoji: seccomp: %s: pid %d tid %llu syscall %llu at pc 0x%llx\n", what, p->pid,
            (unsigned long long)c->tid, (unsigned long long)nr, (unsigned long long)c->pc);
}

int seccomp_enter(cpu_t *c, uint64_t nr, const uint64_t a[6], int64_t *ret)
{
    if (c->sc_mode == SECCOMP_MODE_STRICT) {
        if (nr == 63 || nr == 64 || nr == 93 || nr == 139) /* read, write, exit, rt_sigreturn */
            return SECCOMP_RUN;
        note(c, "strict mode kill", nr);
        end_thread(c, 0, 9);
        return SECCOMP_GONE;
    }
    if (!c->sc_filter)
        return SECCOMP_RUN;
    uint32_t r = evaluate(c->sc_filter, nr, a, c->pc);
    uint32_t data = r & SC_RET_DATA;
    switch (r & SC_RET_ACTION_FULL) {
    case SC_RET_ALLOW:
        return SECCOMP_RUN;
    case SC_RET_LOG:
        if (sig_debug_faults())
            note(c, "log", nr);
        return SECCOMP_RUN;
    case SC_RET_ERRNO:
        *ret = -(int64_t)(data > MAX_ERRNO ? MAX_ERRNO : data);
        return SECCOMP_SKIP;
    case SC_RET_TRACE:      /* no tracer */
    case SC_RET_USER_NOTIF: /* no listener */
        *ret = -LE_NOSYS;
        return SECCOMP_SKIP;
    case SC_RET_TRAP:
        if (sig_debug_faults())
            note(c, "trap", nr);
        sig_seccomp_trap(c, (int32_t)nr, data, AUDIT_ARCH_AARCH64);
        return SECCOMP_GONE;
    case SC_RET_KILL_THREAD:
        note(c, "kill thread", nr);
        end_thread(c, 0, 31);
        return SECCOMP_GONE;
    default: /* KILL_PROCESS and unknown actions */
        note(c, "kill process", nr);
        end_thread(c, 1, 31);
        return SECCOMP_GONE;
    }
}

/* ---------- installing ---------- */

static int may_assign(const cpu_t *c, int mode) { return c->sc_mode == SECCOMP_MODE_DISABLED || c->sc_mode == mode; }

static int64_t set_strict(cpu_t *c, uint64_t flags, uint64_t uargs)
{
    if (flags || uargs)
        return -LE_INVAL;
    if (!may_assign(c, SECCOMP_MODE_STRICT))
        return -LE_INVAL;
    c->sc_mode = SECCOMP_MODE_STRICT;
    return 0;
}

static int is_ancestor(const sc_filter *parent, const sc_filter *child)
{
    if (!parent)
        return 1;
    for (; child; child = child->prev)
        if (child == parent)
            return 1;
    return 0;
}

static int64_t set_filter(cpu_t *c, uint64_t flags, uint64_t uargs)
{
    if (flags & ~(uint64_t)SC_FLAG_MASK)
        return -LE_INVAL;
    if ((flags & SC_FLAG_TSYNC) && (flags & SC_FLAG_NEW_LISTENER) && !(flags & SC_FLAG_TSYNC_ESRCH))
        return -LE_INVAL;
    if ((flags & SC_FLAG_WAIT_KILLABLE_RECV) && !(flags & SC_FLAG_NEW_LISTENER))
        return -LE_INVAL;

    /* seccomp_prepare_user_filter: struct sock_fprog { unsigned short len; struct sock_filter *filter; } */
    uint8_t fprog[16];
    if (mem_copy_out(fprog, uargs, sizeof fprog) < 0)
        return -LE_FAULT;
    unsigned len = (unsigned)fprog[0] | (unsigned)fprog[1] << 8;
    uint64_t insns = 0;
    for (int i = 0; i < 8; i++)
        insns |= (uint64_t)fprog[8 + i] << (8 * i);
    if (len == 0 || len > BPF_MAXINSNS)
        return -LE_INVAL;
    proc_t *p = sched_proc_of(c);
    if (!c->nnp && !cred_has_cap(p, LCAP_SYS_ADMIN))
        return -LE_ACCES;
    sc_filter *f = calloc(1, sizeof *f + len * sizeof(bpf_insn));
    if (!f)
        return -LE_NOMEM;
    f->refs = 1;
    f->len = (int)len;
    uint8_t *raw = malloc(len * 8u);
    if (!raw) {
        free(f);
        return -LE_NOMEM;
    }
    if (mem_copy_out(raw, insns, len * 8u) < 0) {
        free(raw);
        free(f);
        return -LE_FAULT;
    }
    for (unsigned i = 0; i < len; i++) {
        const uint8_t *r = raw + 8 * i;
        f->insns[i].code = (uint16_t)(r[0] | r[1] << 8);
        f->insns[i].jt = r[2];
        f->insns[i].jf = r[3];
        f->insns[i].k = (uint32_t)r[4] | (uint32_t)r[5] << 8 | (uint32_t)r[6] << 16 | (uint32_t)r[7] << 24;
    }
    free(raw);
    int64_t err = seccomp_check(f->insns, f->len);
    if (!err)
        err = classic_check(f->insns, f->len);
    if (!err && (flags & SC_FLAG_NEW_LISTENER))
        err = -LE_INVAL; /* no user-notification listener (see the top of the file) */
    if (!err && !may_assign(c, SECCOMP_MODE_FILTER))
        err = -LE_INVAL;
    if (err) {
        free(f);
        return err;
    }

    /* seccomp_attach_filter: the length of the chain, then TSYNC's check, then the attach and the sync. */
    long total = f->len;
    for (const sc_filter *w = c->sc_filter; w; w = w->prev)
        total += w->len + 4;
    if (total > MAX_INSNS_PER_PATH) {
        free(f);
        return -LE_NOMEM;
    }
    int max = p->nthreads ? p->nthreads : 1;
    cpu_t *list[max];
    int n = sched_threads_of(p, list, max);
    if (flags & SC_FLAG_TSYNC) {
        for (int i = 0; i < n; i++) {
            cpu_t *t = list[i];
            if (t == c || t->exited)
                continue;
            if (t->sc_mode == SECCOMP_MODE_DISABLED ||
                (t->sc_mode == SECCOMP_MODE_FILTER && is_ancestor(t->sc_filter, c->sc_filter)))
                continue;
            free(f);
            return (flags & SC_FLAG_TSYNC_ESRCH) ? -LE_SRCH : (int64_t)t->tid;
        }
    }
    f->log = (flags & SC_FLAG_LOG) != 0;
    f->prev = c->sc_filter; /* the caller's reference moves to the new filter */
    c->sc_filter = f;
    c->sc_mode = SECCOMP_MODE_FILTER;
    if (flags & SC_FLAG_TSYNC) {
        for (int i = 0; i < n; i++) {
            cpu_t *t = list[i];
            if (t == c || t->exited)
                continue;
            filter_get(f);
            filter_put(t->sc_filter);
            t->sc_filter = f;
            if (c->nnp)
                t->nnp = 1;
            if (t->sc_mode == SECCOMP_MODE_DISABLED)
                t->sc_mode = SECCOMP_MODE_FILTER;
        }
    }
    return 0;
}

static int64_t action_avail(uint64_t flags, uint64_t uargs)
{
    if (flags)
        return -LE_INVAL;
    uint8_t b[4];
    if (mem_copy_out(b, uargs, sizeof b) < 0)
        return -LE_FAULT;
    uint32_t act = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    switch (act) {
    case SC_RET_KILL_PROCESS:
    case SC_RET_KILL_THREAD:
    case SC_RET_TRAP:
    case SC_RET_ERRNO:
    case SC_RET_USER_NOTIF:
    case SC_RET_TRACE:
    case SC_RET_LOG:
    case SC_RET_ALLOW:
        return 0;
    default:
        return -LE_NOTSUP;
    }
}

int64_t seccomp_syscall(cpu_t *c, uint64_t op, uint64_t flags, uint64_t uargs)
{
    switch (op) {
    case SC_SET_MODE_STRICT:
        return set_strict(c, flags, uargs);
    case SC_SET_MODE_FILTER:
        return set_filter(c, flags, uargs);
    case SC_GET_ACTION_AVAIL:
        return action_avail(flags, uargs);
    case SC_GET_NOTIF_SIZES: {
        if (flags)
            return -LE_INVAL;
        /* struct seccomp_notif_sizes: seccomp_notif 80, seccomp_notif_resp 24, seccomp_data 64 (u16 each) */
        const uint8_t sizes[6] = {80, 0, 24, 0, 64, 0};
        return mem_copy_in(uargs, sizes, sizeof sizes) < 0 ? -LE_FAULT : 0;
    }
    default:
        return -LE_INVAL;
    }
}

int64_t seccomp_prctl_set(cpu_t *c, uint64_t mode, uint64_t filter)
{
    switch (mode) {
    case SECCOMP_MODE_STRICT:
        return set_strict(c, 0, 0); /* the filter argument is ignored, as Linux does */
    case SECCOMP_MODE_FILTER:
        return set_filter(c, 0, filter);
    default:
        return -LE_INVAL;
    }
}

int64_t seccomp_prctl_get(const cpu_t *c) { return c->sc_mode; }

int seccomp_chain_length(const cpu_t *c)
{
    int n = 0;
    for (const sc_filter *f = c->sc_filter; f; f = f->prev)
        n++;
    return n;
}
