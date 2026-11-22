/* A64 -> WebAssembly block translator, wasm builds only (the native build leaves this
 * file out; see the Makefile).
 *
 * The interpreter loop (cpu_a64.c, run_loop) asks jit_dispatch for a block whenever
 * the pc arrives somewhere other than the next sequential instruction. A block is a
 * straight run of translatable instructions, at most one page and 64 instructions,
 * ending at a branch, at an instruction it does not translate, or at the limit. Each
 * block is one wasm function (i32 cpu) -> i32 that returns the number of instructions
 * it ran. Guest registers are loaded into wasm locals on entry and stored back on
 * exit, and before every helper call that can fault, so a fault reports the right
 * state.
 *
 * Blocks are keyed by the host address of their first instruction together with the
 * guest VA page, so one host page mapped at two VAs (ART's dual mapping of a memfd)
 * gives two blocks, and a block on a host page is dropped whenever that page changes
 * through any mapping (mem.c calls jit_page_changed). Blocks found in a batch of hot
 * entries are compiled together into one wasm module, and the JS side (jit_lib.js)
 * installs the functions in the Emscripten table.
 *
 * Memory accesses go through the C helpers jit_ld and jit_st, which set the faulting
 * pc and do the access with the interpreter's own code. An inline TLB probe is a
 * later step. */
#include <emscripten/emscripten.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_a64.h"
#include "jit.h"
#include "mem.h"
#include "prof.h"

#ifndef JIT_HOT
#define JIT_HOT 1000 /* block entries before a block is queued for translation */
#endif
#define JIT_MAX_INSNS 64
#define JIT_BATCH 64
#define JIT_MAX_BLOCKS 16384
#define JIT_BHASH 8192 /* block hash buckets, power of two */
#define JIT_PAGES 8192 /* host page table slots, power of two */
#define JIT_PAGE_LIMIT (JIT_PAGES * 3 / 4)
#define JIT_CNT 4096 /* lossy entry counters, power of two */
#define JIT_TICK 8192 /* dispatch misses between flushes of a partial batch */

/* Imports of the generated modules, in the order of their function indices. */
#define IMP_LD 0
#define IMP_ST 1
#define IMP_PURE 2
#define N_IMPORTS 3

/* Types of the generated functions. */
#define TY_BLOCK 0 /* (i32) -> i32 */
#define TY_LD 1    /* (i64 va, i64 pc, i32 size) -> i64 */
#define TY_ST 2    /* (i64 va, i64 v, i64 pc, i32 size) -> i32 (1: a code page changed) */
#define TY_PURE 3  /* (i32 op, i64 a, i64 b, i32 sf) -> i64 */

/* Wasm locals of a block: 0 is the cpu pointer. */
#define L_X(r) (1 + (r)) /* x0..x30: locals 1..31 */
#define L_SP 32
#define L_T0 33
#define L_T1 34
#define L_T2 35
#define L_T3 36
#define L_P 37    /* i64: the address of the access being probed */
#define L_V 38    /* i64: the value of a store */
#define L_NZCV 39 /* i32 */
#define L_I0 40   /* i32: set when a store changed a code page */
#define L_I1 41   /* i32: the TLB entry of the access being probed */
#define N_LOCALS_I64 38
#define N_LOCALS_I32 3

#define OFF_X(r) ((uint32_t)(offsetof(cpu_t, x) + 8 * (r)))
#define OFF_SP ((uint32_t)offsetof(cpu_t, sp))
#define OFF_PC ((uint32_t)offsetof(cpu_t, pc))
#define OFF_NZCV ((uint32_t)offsetof(cpu_t, nzcv))
#define OFF_ICOUNT ((uint32_t)offsetof(cpu_t, icount))
#define OFF_STOP ((uint32_t)offsetof(cpu_t, stop))
#define OFF_SIG_PENDING ((uint32_t)offsetof(cpu_t, sig_pending))
#define OFF_SIG_MASK ((uint32_t)offsetof(cpu_t, sig_mask))

/* Operations of jit_pure. */
enum {
    P_RBIT = 1,
    P_REV16,
    P_REV32,
    P_REV64,
    P_CLS,
    P_UDIV,
    P_SDIV,
    P_UMULH,
    P_SMULH,
    P_CRC = 0x100, /* | opcode & 7 */
};

/* Wasm opcodes used by the generator. */
enum {
    W_UNREACHABLE = 0x00,
    W_IF = 0x04,
    W_ELSE = 0x05,
    W_END = 0x0B,
    W_RETURN = 0x0F,
    W_CALL = 0x10,
    W_RETURN_CALL = 0x12,
    W_RETURN_CALL_INDIRECT = 0x13,
    W_DROP = 0x1A,
    W_SELECT = 0x1B,
    W_LGET = 0x20,
    W_LSET = 0x21,
    W_I32_LOAD = 0x28,
    W_I64_LOAD = 0x29,
    W_I64_LOAD8_U = 0x31,
    W_I64_LOAD16_U = 0x33,
    W_I64_LOAD32_U = 0x35,
    W_I32_STORE = 0x36,
    W_I64_STORE = 0x37,
    W_I64_STORE8 = 0x3C,
    W_I64_STORE16 = 0x3D,
    W_I64_STORE32 = 0x3E,
    W_I32_CONST = 0x41,
    W_I64_CONST = 0x42,
    W_I32_EQZ = 0x45,
    W_I64_TYPE = 0x7E, /* the value type i64, as a block type */
    W_I32_ADD = 0x6A,
    W_I32_MUL = 0x6C,
    W_I32_AND = 0x71,
    W_I32_OR = 0x72,
    W_I32_XOR = 0x73,
    W_I32_SHL = 0x74,
    W_I32_SHR_U = 0x76,
    W_I64_EQZ = 0x50,
    W_I64_EQ = 0x51,
    W_I64_NE = 0x52,
    W_I64_LT_U = 0x54,
    W_I64_LE_S = 0x57,
    W_I64_LE_U = 0x58,
    W_I64_CLZ = 0x79,
    W_I64_ADD = 0x7C,
    W_I64_SUB = 0x7D,
    W_I64_MUL = 0x7E,
    W_I64_AND = 0x83,
    W_I64_OR = 0x84,
    W_I64_XOR = 0x85,
    W_I64_SHL = 0x86,
    W_I64_SHR_S = 0x87,
    W_I64_SHR_U = 0x88,
    W_I64_ROTR = 0x8A,
    W_I32_WRAP_I64 = 0xA7,
    W_I64_EXTEND_U_I32 = 0xAD,
    W_I64_EXTEND8_S = 0xC2,
    W_I64_EXTEND16_S = 0xC3,
    W_I64_EXTEND32_S = 0xC4,
};

/* ---------- byte buffers ---------- */

typedef struct {
    uint8_t *p;
    size_t n, cap;
} Buf;

static void die_oom(void)
{
    fputs("xiaoji: out of host memory\n", stderr);
    exit(1);
}

static void buf_reserve(Buf *b, size_t extra)
{
    if (b->n + extra <= b->cap)
        return;
    size_t nc = b->cap ? b->cap : 1024;
    while (nc < b->n + extra)
        nc *= 2;
    uint8_t *np = realloc(b->p, nc);
    if (!np)
        die_oom();
    b->p = np;
    b->cap = nc;
}

static void buf_put(Buf *b, uint8_t x)
{
    buf_reserve(b, 1);
    b->p[b->n++] = x;
}

static void buf_bytes(Buf *b, const void *s, size_t k)
{
    buf_reserve(b, k);
    memcpy(b->p + b->n, s, k);
    b->n += k;
}

static void buf_uleb(Buf *b, uint64_t v)
{
    do {
        uint8_t x = v & 0x7F;
        v >>= 7;
        if (v)
            x |= 0x80;
        buf_put(b, x);
    } while (v);
}

static void buf_sleb(Buf *b, int64_t v)
{
    int more = 1;
    while (more) {
        uint8_t x = v & 0x7F;
        v >>= 7;
        if ((v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40)))
            more = 0;
        else
            x |= 0x80;
        buf_put(b, x);
    }
}

static void buf_name(Buf *b, const char *s)
{
    size_t k = strlen(s);
    buf_uleb(b, k);
    buf_bytes(b, s, k);
}

/* ---------- state ---------- */

typedef struct {
    uintptr_t host;    /* host address of the first instruction */
    uint64_t vpage;    /* guest VA page of the first instruction */
    int32_t fn;        /* table index of the function, or -1: the interpreter runs it */
    int32_t next_hash; /* next block in the hash bucket, -1 = end */
    int32_t next_page; /* next block on the same host page, -1 = end */
    uint8_t valid;     /* 0 once its page changed */
} Blk;

typedef struct {
    uintptr_t page; /* host page address */
    int32_t head;   /* first block on the page, -1 = none */
    uint8_t used, code;
} PageEnt;

typedef struct {
    uintptr_t host;
    uint64_t vpage;
    uint64_t pc;
} Pend;

static int jit_on = -1; /* -1 until read from XIAOJI_JIT */
static Blk blks[JIT_MAX_BLOCKS];
static int nblks;
static int32_t bhash[JIT_BHASH];
static PageEnt pages[JIT_PAGES];
static int npages;
static uint16_t cnt[JIT_CNT];
static Pend queue[JIT_BATCH];
static int qlen;
static uint64_t tick;

/* Set while a block runs: the pc it started at, for the fault path. */
static uint64_t run_start;
static int running;

static uint32_t inval_seq; /* bumped whenever a page with code is dropped */

/* The interpreter's TLBs, which the generated code probes (see mem_jit_tlb). */
static uint32_t tlb_rd_base, tlb_wr_base, tlb_stride, tlb_page_off;

static uint64_t st_dispatch, st_block_runs, st_insns, st_blocks, st_untranslatable,
    st_refused, st_modules, st_compile_fail, st_inv_pages, st_inv_blocks, st_faults, st_smc_exits;

/* Block chaining. A block whose exit is known to the generated code tail-calls chain_fn
 * (a function of its module), which looks the next pc up in fast[] and tail-calls that
 * block, without returning to C. Entries hold pc + 1, so zeroed memory is empty; only
 * untagged, aligned pcs are cached. The slice budget and the hit count are plain statics
 * at fixed addresses, which the generated code reads and writes. */
#define JIT_FAST 4096
typedef struct {
    uint64_t key; /* pc + 1 of a block's first instruction; 0 = empty */
    int32_t fn;   /* its table index */
    int32_t pad;
} FastEnt;
static FastEnt fast[JIT_FAST];
static int64_t jit_budget; /* instructions left in the slice, while translated code runs */
static uint64_t st_chain_hits;
static int chain_on = -1; /* -1 until decided; wasm builds with tail calls only */

/* Compiles the module at buf (len bytes) and installs its nfuncs functions in the
 * table. Returns the first table index, or -1 if the module did not compile. */
int js_jit_compile(const uint8_t *buf, int len, int nfuncs);

/* 1 if this browser validates a module with return_call (the tail-call proposal).
 * Chaining needs it; without it, blocks return to C as before. */
EM_JS(int, js_tail_ok, (void), {
    try {
        return WebAssembly.validate(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, 1, 5, 1, 96, 0, 1, 127,
                                                    3, 2, 1, 0, 10, 6, 1, 4, 0, 18, 0, 11])) ? 1 : 0;
    } catch (e) {
        return 0;
    }
});

static int jit_enabled(void)
{
    if (jit_on < 0) {
        const char *e = getenv("XIAOJI_JIT");
        jit_on = !(e && e[0] == '0');
        for (int i = 0; i < JIT_BHASH; i++)
            bhash[i] = -1;
        uintptr_t rd, wr;
        mem_jit_tlb(&rd, &wr, &tlb_stride, &tlb_page_off);
        tlb_rd_base = (uint32_t)rd;
        tlb_wr_base = (uint32_t)wr;
        e = getenv("XIAOJI_CHAIN");
        chain_on = !(e && e[0] == '0') && js_tail_ok();
    }
    return jit_on;
}

/* Empties the chain cache. Called by the memory layer on every change of the mapping
 * (tlb_flush), so no cached pc outlives the host page it was mapped to. */
void jit_chain_flush(void) { memset(fast, 0, sizeof fast); }

static uint32_t fast_idx(uint64_t pc) { return (uint32_t)(pc >> 2) & (JIT_FAST - 1); }

static void fast_fill(uint64_t pc, int32_t fn)
{
    if (pc & ~(uint64_t)MEM_UNTAG_MASK)
        return; /* tagged: the chain code compares untagged pcs only */
    FastEnt *e = &fast[fast_idx(pc)];
    e->key = pc + 1;
    e->fn = fn;
}

static void fast_drop(uint64_t pc, int32_t fn)
{
    FastEnt *e = &fast[fast_idx(pc)];
    if (e->key == pc + 1 && e->fn == fn)
        e->key = 0;
}

static uint32_t blk_hash(uintptr_t host, uint64_t vpage)
{
    uint32_t h = (uint32_t)host * 2654435761u ^ (uint32_t)(vpage * 0x9E3779B1u);
    return (h >> 8) & (JIT_BHASH - 1);
}

static uint32_t cnt_hash(uintptr_t host, uint64_t vpage)
{
    uint32_t h = (uint32_t)host * 2246822519u ^ (uint32_t)(vpage * 0x85EBCA77u);
    return (h >> 9) & (JIT_CNT - 1);
}

static PageEnt *page_find(uintptr_t page, int create)
{
    uint32_t i = ((uint32_t)(page >> 12) * 2654435761u) >> 19;
    for (;;) {
        PageEnt *e = &pages[i & (JIT_PAGES - 1)];
        if (!e->used) {
            if (!create || npages >= JIT_PAGE_LIMIT)
                return NULL;
            e->used = 1;
            e->page = page;
            e->head = -1;
            npages++;
            return e;
        }
        if (e->page == page)
            return e;
        i++;
    }
}

static Blk *blk_find(uintptr_t host, uint64_t vpage)
{
    for (int32_t i = bhash[blk_hash(host, vpage)]; i >= 0; i = blks[i].next_hash) {
        Blk *b = &blks[i];
        if (b->valid && b->host == host && b->vpage == vpage)
            return b;
    }
    return NULL;
}

static Blk *blk_new(uintptr_t host, uint64_t vpage, PageEnt *pe)
{
    if (nblks >= JIT_MAX_BLOCKS)
        return NULL;
    int32_t idx = nblks++;
    Blk *b = &blks[idx];
    uint32_t h = blk_hash(host, vpage);
    b->host = host;
    b->vpage = vpage;
    b->fn = -1;
    b->valid = 1;
    b->next_hash = bhash[h];
    bhash[h] = idx;
    b->next_page = pe->head;
    pe->head = idx;
    return b;
}

void jit_page_changed(const uint8_t *host_page)
{
    PageEnt *pe = page_find((uintptr_t)host_page, 0);
    if (!pe || !pe->code)
        return;
    for (int32_t i = pe->head; i >= 0; i = blks[i].next_page) {
        if (blks[i].fn >= 0)
            fast_drop(((uint64_t)blks[i].vpage << 12) | (blks[i].host & 0xFFF), blks[i].fn);
        if (blks[i].valid) {
            blks[i].valid = 0;
            st_inv_blocks++;
        }
    }
    pe->code = 0;
    pe->head = -1;
    st_inv_pages++;
    inval_seq++;
}

int jit_page_is_code(const uint8_t *host_page)
{
    PageEnt *pe = page_find((uintptr_t)host_page, 0);
    return pe && pe->code;
}

/* ---------- code generator ---------- */

typedef struct {
    Buf ops; /* the block's instructions */
    Buf pro; /* the entry loads */
    Buf *o;  /* where emitters write: ops, or pro while building the entry */
    uint64_t pc;   /* pc of the instruction being translated */
    unsigned cur_n; /* instructions up to and including it */
    uint32_t touched, written; /* bit r: x_r (r < 31); bit 31: sp */
    int nzcv_touched, nzcv_written;
    int chain; /* the module has chain_fn, and exits tail-call it */
} Gen;

static void o8(Gen *g, uint8_t op) { buf_put(g->o, op); }
static void lget(Gen *g, uint32_t l)
{
    o8(g, W_LGET);
    buf_uleb(g->o, l);
}
static void lset(Gen *g, uint32_t l)
{
    o8(g, W_LSET);
    buf_uleb(g->o, l);
}
static void i64c(Gen *g, int64_t v)
{
    o8(g, W_I64_CONST);
    buf_sleb(g->o, v);
}
static void i32c(Gen *g, int32_t v)
{
    o8(g, W_I32_CONST);
    buf_sleb(g->o, v);
}
static void memop(Gen *g, uint8_t op, uint32_t align, uint32_t off)
{
    o8(g, op);
    buf_uleb(g->o, align);
    buf_uleb(g->o, off);
}
static void calli(Gen *g, uint32_t fn)
{
    o8(g, W_CALL);
    buf_uleb(g->o, fn);
}

/* Stores the registers in regs (and nzcv if asked) back to the cpu struct. */
static void flush(Gen *g, uint32_t regs, int nzcv)
{
    for (unsigned r = 0; r < 31; r++) {
        if (regs & (1u << r)) {
            lget(g, 0);
            lget(g, L_X(r));
            memop(g, W_I64_STORE, 3, OFF_X(r));
        }
    }
    if (regs & (1u << 31)) {
        lget(g, 0);
        lget(g, L_SP);
        memop(g, W_I64_STORE, 3, OFF_SP);
    }
    if (nzcv) {
        lget(g, 0);
        lget(g, L_NZCV);
        memop(g, W_I32_STORE, 2, OFF_NZCV);
    }
}

/* Exit kinds, returned in bits 8 and up of the block's result (the instruction count
 * is in bits 0..7). NEXT: the pc is the next instruction to run (an untranslated one,
 * or the length limit). JUMP: a branch, taken or not, or an indirect branch. SMC: a
 * store changed translated code; the pc is the instruction after the store. */
enum { EXIT_NEXT = 0, EXIT_JUMP = 1, EXIT_SMC = 2, EXIT_BAIL = 3 /* chain_fn: n = 0 */ };

/* The module's chain function is its first function, after the imports. */
#define CHAIN_FN N_IMPORTS
/* Addresses of the statics that generated code reads and writes. */
#define ADDR_BUDGET ((int32_t)(uintptr_t)&jit_budget)
#define ADDR_RUN_START ((int32_t)(uintptr_t)&run_start)
#define ADDR_HITS ((int32_t)(uintptr_t)&st_chain_hits)
#define ADDR_FAST ((int32_t)(uintptr_t)fast)

/* The block's n instructions are done: charge them to the cpu and the slice budget,
 * then leave. A chained exit tail-calls chain_fn, which hops to the next block or
 * returns with n = 0 (the charge above already counted this block). Otherwise the
 * block returns n | kind. SMC always returns to C, so the interpreter takes the next
 * instruction. */
static void exit_finish(Gen *g, unsigned n, unsigned kind)
{
    lget(g, 0);
    lget(g, 0);
    memop(g, W_I64_LOAD, 3, OFF_ICOUNT);
    i64c(g, n);
    o8(g, W_I64_ADD);
    memop(g, W_I64_STORE, 3, OFF_ICOUNT);
    i32c(g, ADDR_BUDGET);
    i32c(g, ADDR_BUDGET);
    memop(g, W_I64_LOAD, 3, 0);
    i64c(g, n);
    o8(g, W_I64_SUB);
    memop(g, W_I64_STORE, 3, 0);
    if (g->chain && kind != EXIT_SMC) {
        lget(g, 0);
        o8(g, W_RETURN_CALL);
        buf_uleb(g->o, CHAIN_FN);
        return;
    }
    i32c(g, (int32_t)(n | kind << 8));
    o8(g, W_RETURN);
}

/* Exits with pc = the constant pc, having run n instructions. */
static void exit_kind(Gen *g, uint64_t pc, unsigned n, unsigned kind)
{
    flush(g, g->written, g->nzcv_written);
    lget(g, 0);
    i64c(g, (int64_t)pc);
    memop(g, W_I64_STORE, 3, OFF_PC);
    exit_finish(g, n, kind);
}

static void exit_to(Gen *g, uint64_t pc, unsigned n) { exit_kind(g, pc, n, EXIT_NEXT); }

/* Exits with pc = the value in T0. */
static void exit_dyn(Gen *g, unsigned n)
{
    flush(g, g->written, g->nzcv_written);
    lget(g, 0);
    lget(g, L_T0);
    memop(g, W_I64_STORE, 3, OFF_PC);
    exit_finish(g, n, EXIT_JUMP);
}

/* Exits to taken or fall, depending on the i32 condition on the stack. */
static void exit_cond(Gen *g, uint64_t taken, uint64_t fall, unsigned n)
{
    o8(g, W_IF);
    buf_put(g->o, 0x40);
    exit_kind(g, taken, n, EXIT_JUMP);
    o8(g, W_ELSE);
    exit_kind(g, fall, n, EXIT_JUMP);
    o8(g, W_END);
}

/* ---------- registers ---------- */

static void touch(Gen *g, unsigned r) { g->touched |= 1u << r; }

/* Pushes the value of register r as an i64, masked to sf. sp selects SP for r = 31;
 * otherwise 31 reads as zero. */
static void rd_(Gen *g, unsigned r, unsigned sf, unsigned sp)
{
    if (r == 31) {
        if (sp) {
            touch(g, 31);
            lget(g, L_SP);
        } else {
            i64c(g, 0);
        }
    } else {
        touch(g, r);
        lget(g, L_X(r));
    }
    if (!sf) {
        i64c(g, 0xFFFFFFFFll);
        o8(g, W_I64_AND);
    }
}

/* Writes the i64 on the stack to register r, masked to sf. sp selects SP for r = 31;
 * otherwise writes to 31 are dropped. */
static void wr(Gen *g, unsigned r, unsigned sf, unsigned sp)
{
    if (!sf) {
        i64c(g, 0xFFFFFFFFll);
        o8(g, W_I64_AND);
    }
    if (r == 31) {
        if (!sp) {
            o8(g, W_DROP);
            return;
        }
        touch(g, 31);
        g->written |= 1u << 31;
        lset(g, L_SP);
        return;
    }
    touch(g, r);
    g->written |= 1u << r;
    lset(g, L_X(r));
}

static void mask_t2(Gen *g, unsigned sf)
{
    if (!sf) {
        lget(g, L_T2);
        i64c(g, 0xFFFFFFFFll);
        o8(g, W_I64_AND);
        lset(g, L_T2);
    }
}

/* ---------- flags ---------- */

static void flags_touch(Gen *g)
{
    g->nzcv_touched = 1;
    g->nzcv_written = 1;
}

/* Pushes N<<31 | Z<<30 of the w-bit result in T2. */
static void push_nz(Gen *g, unsigned w)
{
    lget(g, L_T2);
    i64c(g, w - 1);
    o8(g, W_I64_SHR_U);
    i64c(g, 1);
    o8(g, W_I64_AND);
    o8(g, W_I32_WRAP_I64);
    i32c(g, 31);
    o8(g, W_I32_SHL);
    lget(g, L_T2);
    o8(g, W_I64_EQZ);
    i32c(g, 30);
    o8(g, W_I32_SHL);
    o8(g, W_I32_OR);
}

/* Pushes C<<29 of an addition: T0 + T1 + T3 (T2 holds the unmasked sum for sf=0). */
static void push_c(Gen *g, unsigned sf)
{
    if (sf) {
        lget(g, L_T2);
        lget(g, L_T0);
        o8(g, W_I64_LT_U);
        lget(g, L_T2);
        lget(g, L_T0);
        o8(g, W_I64_EQ);
        lget(g, L_T3);
        o8(g, W_I32_WRAP_I64);
        o8(g, W_I32_AND);
        o8(g, W_I32_OR);
    } else {
        lget(g, L_T2);
        i64c(g, 32);
        o8(g, W_I64_SHR_U);
        o8(g, W_I32_WRAP_I64);
        i32c(g, 1);
        o8(g, W_I32_AND);
    }
    i32c(g, 29);
    o8(g, W_I32_SHL);
}

/* Pushes V<<28 of an addition T0 + T1 with the masked result in T2. */
static void push_v(Gen *g, unsigned w)
{
    lget(g, L_T0);
    lget(g, L_T2);
    o8(g, W_I64_XOR);
    lget(g, L_T1);
    lget(g, L_T2);
    o8(g, W_I64_XOR);
    o8(g, W_I64_AND);
    i64c(g, w - 1);
    o8(g, W_I64_SHR_U);
    i64c(g, 1);
    o8(g, W_I64_AND);
    o8(g, W_I32_WRAP_I64);
    i32c(g, 28);
    o8(g, W_I32_SHL);
}

/* T2 = T0 + T1 + T3 (T0, T1 masked to sf first). Sets NZCV from it if flags. Leaves
 * the masked result in T2. T1 must already be inverted for a subtraction. */
static void addc(Gen *g, unsigned sf, int flags)
{
    unsigned w = sf ? 64 : 32;
    if (!sf) {
        lget(g, L_T0);
        i64c(g, 0xFFFFFFFFll);
        o8(g, W_I64_AND);
        lset(g, L_T0);
        lget(g, L_T1);
        i64c(g, 0xFFFFFFFFll);
        o8(g, W_I64_AND);
        lset(g, L_T1);
    }
    lget(g, L_T0);
    lget(g, L_T1);
    o8(g, W_I64_ADD);
    lget(g, L_T3);
    o8(g, W_I64_ADD);
    lset(g, L_T2);
    if (flags) {
        flags_touch(g);
        push_c(g, sf);
        mask_t2(g, sf);
        push_nz(g, w);
        o8(g, W_I32_OR);
        push_v(g, w);
        o8(g, W_I32_OR);
        lset(g, L_NZCV);
    } else {
        mask_t2(g, sf);
    }
}

/* Sets NZCV from a logical result in T2 (C = V = 0). */
static void logic_flags(Gen *g, unsigned sf)
{
    flags_touch(g);
    push_nz(g, sf ? 64 : 32);
    lset(g, L_NZCV);
}

/* Pushes the i32 bit (N, Z, C or V) at shift of NZCV. */
static void flagbit(Gen *g, uint32_t shift)
{
    lget(g, L_NZCV);
    i32c(g, (int32_t)shift);
    o8(g, W_I32_SHR_U);
    i32c(g, 1);
    o8(g, W_I32_AND);
}

/* Pushes cond_true(NZCV, cond) as an i32 0 or 1. */
static void cond_push(Gen *g, unsigned cond)
{
    g->nzcv_touched = 1;
    switch (cond >> 1) {
    case 0: flagbit(g, 30); break;               /* EQ */
    case 1: flagbit(g, 29); break;               /* CS */
    case 2: flagbit(g, 31); break;               /* MI */
    case 3: flagbit(g, 28); break;               /* VS */
    case 4:                                      /* HI: C && !Z */
        flagbit(g, 29);
        flagbit(g, 30);
        o8(g, W_I32_EQZ);
        o8(g, W_I32_AND);
        break;
    case 5:                                      /* GE: N == V */
        flagbit(g, 31);
        flagbit(g, 28);
        o8(g, W_I32_XOR);
        o8(g, W_I32_EQZ);
        break;
    case 6:                                      /* GT: N == V && !Z */
        flagbit(g, 31);
        flagbit(g, 28);
        o8(g, W_I32_XOR);
        o8(g, W_I32_EQZ);
        flagbit(g, 30);
        o8(g, W_I32_EQZ);
        o8(g, W_I32_AND);
        break;
    default: /* AL and NV */
        i32c(g, 1);
        return;
    }
    if (cond & 1)
        o8(g, W_I32_EQZ);
}

/* ---------- shifts and extension (operate on the i64 on the stack) ---------- */

/* shift_op of cpu_a64.c, type 0 LSL, 1 LSR, 2 ASR, 3 ROR. The operand is masked. */
static void emit_shift(Gen *g, unsigned sf, unsigned type, unsigned amt)
{
    unsigned w = sf ? 64 : 32;
    amt %= w;
    switch (type & 3) {
    case 0: /* LSL */
        if (amt) {
            i64c(g, amt);
            o8(g, W_I64_SHL);
        }
        if (!sf) {
            i64c(g, 0xFFFFFFFFll);
            o8(g, W_I64_AND);
        }
        break;
    case 1: /* LSR */
        if (amt) {
            i64c(g, amt);
            o8(g, W_I64_SHR_U);
        }
        break;
    case 2: /* ASR */
        if (!sf)
            o8(g, W_I64_EXTEND32_S);
        if (amt) {
            i64c(g, amt);
            o8(g, W_I64_SHR_S);
        }
        if (!sf) {
            i64c(g, 0xFFFFFFFFll);
            o8(g, W_I64_AND);
        }
        break;
    default: /* ROR */
        if (amt == 0)
            break;
        if (sf) {
            i64c(g, amt);
            o8(g, W_I64_ROTR);
        } else {
            lset(g, L_T3);
            lget(g, L_T3);
            i64c(g, amt);
            o8(g, W_I64_SHR_U);
            lget(g, L_T3);
            i64c(g, 32 - amt);
            o8(g, W_I64_SHL);
            o8(g, W_I64_OR);
            i64c(g, 0xFFFFFFFFll);
            o8(g, W_I64_AND);
        }
        break;
    }
}

/* extend_op of cpu_a64.c, applied to the i64 on the stack. */
static void emit_extend(Gen *g, unsigned option, unsigned sh)
{
    switch (option & 7) {
    case 0: i64c(g, 0xFF); o8(g, W_I64_AND); break;
    case 1: i64c(g, 0xFFFF); o8(g, W_I64_AND); break;
    case 2: i64c(g, 0xFFFFFFFFll); o8(g, W_I64_AND); break;
    case 4: o8(g, W_I64_EXTEND8_S); break;
    case 5: o8(g, W_I64_EXTEND16_S); break;
    case 6: o8(g, W_I64_EXTEND32_S); break;
    default: break;
    }
    if (sh) {
        i64c(g, sh);
        o8(g, W_I64_SHL);
    }
}

/* ---------- helpers that can fault, and pure helpers ---------- */

/* Probes the TLB at base (the read or the write array) for the untagged address in
 * P. Leaves an i32 on the stack that is 1 when the access is within one page and the
 * page is in the TLB, and I1 holds the entry. */
static void tlb_probe(Gen *g, uint32_t base, uint32_t size)
{
    /* I1 = base + ((P >> 12) & 255) * stride */
    lget(g, L_P);
    i64c(g, 12);
    o8(g, W_I64_SHR_U);
    o8(g, W_I32_WRAP_I64);
    i32c(g, 255);
    o8(g, W_I32_AND);
    i32c(g, (int32_t)tlb_stride);
    o8(g, W_I32_MUL);
    i32c(g, (int32_t)base);
    o8(g, W_I32_ADD);
    lset(g, L_I1);
    /* tag == (P >> 12) + 1 */
    lget(g, L_I1);
    memop(g, W_I64_LOAD, 3, 0);
    lget(g, L_P);
    i64c(g, 12);
    o8(g, W_I64_SHR_U);
    i64c(g, 1);
    o8(g, W_I64_ADD);
    o8(g, W_I64_EQ);
    /* the access does not cross the page: (P & 0xFFF) <= 4096 - size */
    lget(g, L_P);
    i64c(g, 0xFFF);
    o8(g, W_I64_AND);
    i64c(g, (int64_t)(4096 - size));
    o8(g, W_I64_LE_U);
    o8(g, W_I32_AND);
}

/* Pushes the host address of the access in P (after tlb_probe found its page). */
static void host_addr(Gen *g)
{
    lget(g, L_I1);
    memop(g, W_I32_LOAD, 2, tlb_page_off);
    lget(g, L_P);
    i64c(g, 0xFFF);
    o8(g, W_I64_AND);
    o8(g, W_I32_WRAP_I64);
    o8(g, W_I32_ADD);
}

/* stack: va. Pushes the loaded value (zero-extended to 64 bits). The inline probe hits
 * the TLB for an access within one page; otherwise jit_ld does it, after the registers
 * are stored, so a fault sees the state before this instruction. */
static void untag(Gen *g)
{
    lget(g, L_P);
    i64c(g, (int64_t)MEM_UNTAG_MASK);
    o8(g, W_I64_AND);
    lset(g, L_P);
}

static void mem_ld(Gen *g, uint32_t size)
{
    lset(g, L_P);
    untag(g);
    tlb_probe(g, tlb_rd_base, size);
    o8(g, W_IF);
    buf_put(g->o, W_I64_TYPE);
    host_addr(g);
    memop(g, size == 1 ? W_I64_LOAD8_U : size == 2 ? W_I64_LOAD16_U : size == 4 ? W_I64_LOAD32_U : W_I64_LOAD, 0, 0);
    o8(g, W_ELSE);
    flush(g, g->written, g->nzcv_written);
    lget(g, L_P);
    i64c(g, (int64_t)g->pc);
    i32c(g, (int32_t)size);
    calli(g, IMP_LD);
    o8(g, W_END);
}

/* stack: va, value. Stores size bytes. The inline probe uses the write TLB, which never
 * holds a page with code, so only the helper can change code; it records that in I0,
 * and the block leaves after the instruction completes. */
static void mem_st(Gen *g, uint32_t size)
{
    lset(g, L_V);
    lset(g, L_P);
    untag(g);
    tlb_probe(g, tlb_wr_base, size);
    o8(g, W_IF);
    buf_put(g->o, 0x40);
    host_addr(g);
    lget(g, L_V);
    memop(g, size == 1 ? W_I64_STORE8 : size == 2 ? W_I64_STORE16 : size == 4 ? W_I64_STORE32 : W_I64_STORE, 0, 0);
    o8(g, W_ELSE);
    flush(g, g->written, g->nzcv_written);
    lget(g, L_P);
    lget(g, L_V);
    i64c(g, (int64_t)g->pc);
    i32c(g, (int32_t)size);
    calli(g, IMP_ST);
    lget(g, L_I0);
    o8(g, W_I32_OR);
    lset(g, L_I0);
    o8(g, W_END);
}

/* After an instruction that stored: if a store changed code, leave now (pc = next). */
static void smc_check(Gen *g)
{
    lget(g, L_I0);
    o8(g, W_IF);
    buf_put(g->o, 0x40);
    exit_kind(g, g->pc + 4, g->cur_n, EXIT_SMC);
    o8(g, W_END);
}

/* stack: a, b (i64). Replaces them with pure(op, a, b, sf) computed by jit_pure. */
static void pure_call(Gen *g, uint32_t op, unsigned sf)
{
    /* The op must sit below a and b on the wasm stack, so they go through T0 and T1. */
    lset(g, L_T1);
    lset(g, L_T0);
    i32c(g, (int32_t)op);
    lget(g, L_T0);
    lget(g, L_T1);
    i32c(g, (int32_t)sf);
    calli(g, IMP_PURE);
}

/* ---------- instruction translation ---------- */
/* Each returns 0 if the instruction is not translated (nothing emitted: the block ends
 * before it), 1 if it was translated, 2 if it was translated and ended the block. */

static uint64_t sext(uint64_t v, unsigned n)
{
    uint64_t m = n >= 64 ? ~0ull : ((1ull << n) - 1);
    uint64_t s = 1ull << (n - 1);
    v &= m;
    return ((v ^ s) - s);
}

static int decode_bitmask(unsigned sf, unsigned N, unsigned imms, unsigned immr, int immediate,
                          uint64_t *wmask, uint64_t *tmask)
{
    unsigned v = (N << 6) | (~imms & 0x3F);
    if (!v)
        return 0;
    int len = 31 - __builtin_clz(v);
    if (len < 1)
        return 0;
    if (!sf && N)
        return 0;
    unsigned esize = 1u << len;
    unsigned levels = esize - 1;
    unsigned S = imms & levels, R = immr & levels;
    if (immediate && S == levels)
        return 0;
    uint64_t emask = esize == 64 ? ~0ull : ((1ull << esize) - 1);
    uint64_t welem = (S + 1 == 64) ? ~0ull : ((1ull << (S + 1)) - 1);
    unsigned diff = (S - R) & levels;
    uint64_t telem = (diff + 1 == 64) ? ~0ull : ((1ull << (diff + 1)) - 1);
    uint64_t w = R ? (((welem >> R) | (welem << (esize - R))) & emask) : welem;
    uint64_t rep = w;
    for (unsigned e = esize; e < 64; e *= 2)
        rep |= rep << e;
    uint64_t trep = telem;
    for (unsigned e = esize; e < 64; e *= 2)
        trep |= trep << e;
    *wmask = rep;
    *tmask = trep;
    return 1;
}

/* Logical results: T2 holds the result (unmasked for sf=0), wr it. Rd = 31 is SP only
 * in the logical-immediate forms (sp); the shifted-register forms always write ZR. */
static void logic_store(Gen *g, unsigned sf, unsigned rd, int setflags, int sp)
{
    lset(g, L_T2);
    mask_t2(g, sf);
    if (setflags)
        logic_flags(g, sf);
    lget(g, L_T2);
    wr(g, rd, sf, sp);
}

static int tr_dp_imm(Gen *g, uint32_t insn)
{
    unsigned sf = insn >> 31, rd = insn & 31;
    uint64_t pc = g->pc;
    switch ((insn >> 23) & 7) {
    case 0:
    case 1: { /* ADR / ADRP */
        uint64_t imm = sext((((insn >> 5) & 0x7FFFF) << 2) | ((insn >> 29) & 3), 21);
        uint64_t v = (insn >> 31) ? ((pc & ~0xFFFull) + (imm << 12)) : (pc + imm);
        i64c(g, (int64_t)v);
        wr(g, rd, 1, 0);
        return 1;
    }
    case 2: { /* ADD / SUB immediate */
        unsigned op = (insn >> 30) & 1, S = (insn >> 29) & 1, sh = (insn >> 22) & 1;
        uint64_t imm = (uint64_t)((insn >> 10) & 0xFFF) << (sh ? 12 : 0);
        unsigned rn = (insn >> 5) & 31;
        rd_(g, rn, sf, 1);
        lset(g, L_T0);
        i64c(g, (int64_t)(op ? ~imm : imm));
        lset(g, L_T1);
        i64c(g, op);
        lset(g, L_T3);
        addc(g, sf, S);
        lget(g, L_T2);
        wr(g, rd, sf, !S);
        return 1;
    }
    case 4: { /* logical immediate */
        unsigned opc = (insn >> 29) & 3, N = (insn >> 22) & 1;
        unsigned immr = (insn >> 16) & 0x3F, imms = (insn >> 10) & 0x3F;
        unsigned rn = (insn >> 5) & 31;
        uint64_t wm, tm;
        if (!decode_bitmask(sf, N, imms, immr, 1, &wm, &tm))
            return 0;
        rd_(g, rn, sf, 0);
        i64c(g, (int64_t)wm);
        o8(g, opc == 1 ? W_I64_OR : opc == 2 ? W_I64_XOR : W_I64_AND);
        logic_store(g, sf, rd, opc == 3, opc != 3);
        return 1;
    }
    case 5: { /* move wide */
        unsigned opc = (insn >> 29) & 3, hw = (insn >> 21) & 3;
        uint64_t imm16 = (insn >> 5) & 0xFFFF;
        if (opc == 1 || (!sf && hw >= 2))
            return 0;
        unsigned pos = hw * 16;
        uint64_t m = sf ? ~0ull : 0xFFFFFFFFull;
        if (opc == 0) {
            i64c(g, (int64_t)(~(imm16 << pos) & m));
        } else if (opc == 2) {
            i64c(g, (int64_t)((imm16 << pos) & m));
        } else {
            rd_(g, rd, sf, 0);
            i64c(g, (int64_t)~(0xFFFFull << pos));
            o8(g, W_I64_AND);
            i64c(g, (int64_t)(imm16 << pos));
            o8(g, W_I64_OR);
        }
        wr(g, rd, sf, 0);
        return 1;
    }
    case 6: { /* SBFM, BFM, UBFM */
        unsigned opc = (insn >> 29) & 3, N = (insn >> 22) & 1;
        unsigned immr = (insn >> 16) & 0x3F, imms = (insn >> 10) & 0x3F;
        unsigned rn = (insn >> 5) & 31;
        if (opc == 3 || N != sf)
            return 0;
        uint64_t wm, tm;
        if (!decode_bitmask(sf, N, imms, immr, 0, &wm, &tm))
            return 0;
        uint64_t m = sf ? ~0ull : 0xFFFFFFFFull;
        rd_(g, rn, sf, 0);
        lset(g, L_T0);
        /* bot = ror(src, immr) & wm */
        lget(g, L_T0);
        emit_shift(g, sf, 3, immr);
        i64c(g, (int64_t)wm);
        o8(g, W_I64_AND);
        lset(g, L_T1);
        if (opc == 1) { /* BFM: bot keeps the destination's other bits */
            rd_(g, rd, sf, 0);
            i64c(g, (int64_t)(~wm & m));
            o8(g, W_I64_AND);
            lget(g, L_T1);
            o8(g, W_I64_OR);
            lset(g, L_T1);
            rd_(g, rd, sf, 0);
            lset(g, L_T2);
        } else if (opc == 0) { /* SBFM: top is all ones if the sign bit is set */
            i64c(g, 0);
            lget(g, L_T0);
            i64c(g, imms);
            o8(g, W_I64_SHR_U);
            i64c(g, 1);
            o8(g, W_I64_AND);
            o8(g, W_I64_SUB);
            lset(g, L_T2);
        } else { /* UBFM */
            i64c(g, 0);
            lset(g, L_T2);
        }
        /* r = ((top & ~tm) | (bot & tm)) & m */
        lget(g, L_T2);
        i64c(g, (int64_t)~tm);
        o8(g, W_I64_AND);
        lget(g, L_T1);
        i64c(g, (int64_t)tm);
        o8(g, W_I64_AND);
        o8(g, W_I64_OR);
        i64c(g, (int64_t)m);
        o8(g, W_I64_AND);
        wr(g, rd, sf, 0);
        return 1;
    }
    case 7: { /* EXTR */
        unsigned op21 = (insn >> 29) & 3, N = (insn >> 22) & 1, o0 = (insn >> 21) & 1;
        unsigned rm = (insn >> 16) & 31, imms = (insn >> 10) & 0x3F, rn = (insn >> 5) & 31;
        if (op21 || o0 || N != sf)
            return 0;
        if (!sf && imms >= 32)
            return 0;
        if (sf) {
            if (imms == 0) {
                rd_(g, rm, 1, 0);
            } else {
                rd_(g, rn, 1, 0);
                i64c(g, 64 - imms);
                o8(g, W_I64_SHL);
                rd_(g, rm, 1, 0);
                i64c(g, imms);
                o8(g, W_I64_SHR_U);
                o8(g, W_I64_OR);
            }
        } else {
            rd_(g, rn, 0, 0);
            i64c(g, 32);
            o8(g, W_I64_SHL);
            rd_(g, rm, 0, 0);
            o8(g, W_I64_OR);
            i64c(g, imms);
            o8(g, W_I64_SHR_U);
            i64c(g, 0xFFFFFFFFll);
            o8(g, W_I64_AND);
        }
        wr(g, rd, sf, 0);
        return 1;
    }
    default:
        return 0;
    }
}

/* a + b (+ cin) into rd: T0 = a, T1 = b (not inverted), cin_mode: 0 = the subtraction
 * carry (1 for SUB), 1 = the carry flag (ADC/SBC). */
static void addsub_core(Gen *g, unsigned sf, unsigned sub, unsigned S, unsigned carry_in,
                        unsigned rd, unsigned sp_dst)
{
    if (sub) {
        lget(g, L_T1);
        i64c(g, -1);
        o8(g, W_I64_XOR);
        lset(g, L_T1);
    }
    if (carry_in) {
        lget(g, L_NZCV);
        i32c(g, 29);
        o8(g, W_I32_SHR_U);
        i32c(g, 1);
        o8(g, W_I32_AND);
        o8(g, W_I64_EXTEND_U_I32);
        lset(g, L_T3);
        g->nzcv_touched = 1;
    } else {
        i64c(g, sub ? 1 : 0);
        lset(g, L_T3);
    }
    addc(g, sf, S);
    lget(g, L_T2);
    wr(g, rd, sf, sp_dst);
}

static int tr_logical_shifted(Gen *g, uint32_t insn)
{
    unsigned sf = insn >> 31, opc = (insn >> 29) & 3, shift = (insn >> 22) & 3, N = (insn >> 21) & 1;
    unsigned rm = (insn >> 16) & 31, imm6 = (insn >> 10) & 0x3F, rn = (insn >> 5) & 31, rd = insn & 31;
    if (!sf && imm6 >= 32)
        return 0;
    rd_(g, rm, sf, 0);
    emit_shift(g, sf, shift, imm6);
    if (N) {
        i64c(g, -1);
        o8(g, W_I64_XOR);
        if (!sf) { /* the inverted operand stays 32-bit */
            i64c(g, 0xFFFFFFFFll);
            o8(g, W_I64_AND);
        }
    }
    lset(g, L_T1);
    rd_(g, rn, sf, 0);
    lset(g, L_T0);
    lget(g, L_T0);
    lget(g, L_T1);
    o8(g, opc == 1 ? W_I64_OR : opc == 2 ? W_I64_XOR : W_I64_AND);
    logic_store(g, sf, rd, opc == 3, 0);
    return 1;
}

static int tr_addsub_shifted(Gen *g, uint32_t insn)
{
    unsigned sf = insn >> 31, op = (insn >> 30) & 1, S = (insn >> 29) & 1, shift = (insn >> 22) & 3;
    unsigned rm = (insn >> 16) & 31, imm6 = (insn >> 10) & 0x3F, rn = (insn >> 5) & 31, rd = insn & 31;
    if (shift == 3 || (!sf && imm6 >= 32))
        return 0;
    rd_(g, rm, sf, 0);
    emit_shift(g, sf, shift, imm6);
    lset(g, L_T1);
    rd_(g, rn, sf, 0);
    lset(g, L_T0);
    addsub_core(g, sf, op, S, 0, rd, 0);
    return 1;
}

static int tr_addsub_ext(Gen *g, uint32_t insn)
{
    unsigned sf = insn >> 31, op = (insn >> 30) & 1, S = (insn >> 29) & 1;
    unsigned option = (insn >> 13) & 7, imm3 = (insn >> 10) & 7;
    unsigned rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd = insn & 31;
    if (((insn >> 22) & 3) || imm3 > 4)
        return 0;
    rd_(g, rm, 1, 0);
    emit_extend(g, option, imm3);
    lset(g, L_T1);
    rd_(g, rn, sf, 1);
    lset(g, L_T0);
    addsub_core(g, sf, op, S, 0, rd, !S);
    return 1;
}

static int tr_adc_sbc(Gen *g, uint32_t insn)
{
    unsigned sf = insn >> 31, op = (insn >> 30) & 1, S = (insn >> 29) & 1;
    unsigned rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd = insn & 31;
    if ((insn >> 10) & 0x3F)
        return 0;
    rd_(g, rn, sf, 0);
    lset(g, L_T0);
    rd_(g, rm, sf, 0);
    lset(g, L_T1);
    addsub_core(g, sf, op, S, 1, rd, 0);
    return 1;
}

static int tr_ccmp(Gen *g, uint32_t insn)
{
    unsigned sf = insn >> 31, op = (insn >> 30) & 1, S = (insn >> 29) & 1, imm = (insn >> 11) & 1;
    unsigned o2 = (insn >> 10) & 1, o3 = (insn >> 4) & 1;
    unsigned cond = (insn >> 12) & 0xF, rn = (insn >> 5) & 31, nzcv = insn & 0xF;
    if (!S || o2 || o3)
        return 0;
    cond_push(g, cond);
    o8(g, W_IF);
    buf_put(g->o, 0x40);
    rd_(g, rn, sf, 0);
    lset(g, L_T0);
    if (imm) {
        i64c(g, (insn >> 16) & 0x1F);
    } else {
        rd_(g, (insn >> 16) & 31, sf, 0);
    }
    lset(g, L_T1);
    i64c(g, op);
    lset(g, L_T3);
    /* CCMP subtracts (op = 1, carry-in 1); CCMN adds (carry-in 0). */
    if (op) {
        lget(g, L_T1);
        i64c(g, -1);
        o8(g, W_I64_XOR);
        lset(g, L_T1);
    }
    addc(g, sf, 1);
    o8(g, W_ELSE);
    i32c(g, (int32_t)(nzcv << 28));
    lset(g, L_NZCV);
    o8(g, W_END);
    flags_touch(g);
    return 1;
}

static int tr_csel(Gen *g, uint32_t insn)
{
    unsigned sf = insn >> 31, op = (insn >> 30) & 1, S = (insn >> 29) & 1;
    unsigned cond = (insn >> 12) & 0xF, op2 = (insn >> 10) & 3;
    unsigned rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd = insn & 31;
    if (S || op2 >= 2)
        return 0;
    rd_(g, rn, sf, 0);
    lset(g, L_T0);
    rd_(g, rm, sf, 0);
    lset(g, L_T1);
    if (op) {
        lget(g, L_T1);
        i64c(g, -1);
        o8(g, W_I64_XOR);
        lset(g, L_T1);
    }
    if (op2) {
        lget(g, L_T1);
        i64c(g, 1);
        o8(g, W_I64_ADD);
        lset(g, L_T1);
    }
    lget(g, L_T0);
    lget(g, L_T1);
    cond_push(g, cond);
    o8(g, W_SELECT);
    wr(g, rd, sf, 0);
    return 1;
}

static int tr_dp_1src(Gen *g, uint32_t insn)
{
    unsigned sf = insn >> 31, S = (insn >> 29) & 1, opcode = (insn >> 10) & 0x3F;
    unsigned rn = (insn >> 5) & 31, rd = insn & 31;
    if (S || ((insn >> 16) & 0x1F))
        return 0;
    switch (opcode) {
    case 0: /* RBIT */
        rd_(g, rn, sf, 0);
        i64c(g, 0);
        pure_call(g, P_RBIT, sf);
        break;
    case 1: /* REV16 */
        rd_(g, rn, sf, 0);
        i64c(g, 0);
        pure_call(g, P_REV16, sf);
        break;
    case 2: /* REV32 (64-bit) or REV (32-bit) */
        rd_(g, rn, sf, 0);
        i64c(g, 0);
        pure_call(g, P_REV32, sf);
        break;
    case 3: /* REV (64-bit) */
        if (!sf)
            return 0;
        rd_(g, rn, 1, 0);
        i64c(g, 0);
        pure_call(g, P_REV64, sf);
        break;
    case 4: /* CLZ */
        rd_(g, rn, sf, 0);
        o8(g, W_I64_CLZ);
        if (!sf) {
            i64c(g, 32);
            o8(g, W_I64_SUB);
        }
        break;
    case 5: /* CLS */
        rd_(g, rn, sf, 0);
        i64c(g, 0);
        pure_call(g, P_CLS, sf);
        break;
    default:
        return 0;
    }
    wr(g, rd, sf, 0);
    return 1;
}

static int tr_dp_2src(Gen *g, uint32_t insn)
{
    unsigned sf = insn >> 31, S = (insn >> 29) & 1, opcode = (insn >> 10) & 0x3F;
    unsigned rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd = insn & 31;
    if (S)
        return 0;
    if ((opcode >> 3) == 2) { /* CRC32 and CRC32C */
        unsigned sz = opcode & 3;
        if ((sf && sz != 3) || (!sf && sz == 3))
            return 0;
        rd_(g, rn, 0, 0);
        rd_(g, rm, sf, 0);
        pure_call(g, P_CRC | (opcode & 7), sf);
        wr(g, rd, 0, 0);
        return 1;
    }
    unsigned w = sf ? 64 : 32;
    switch (opcode) {
    case 2: /* UDIV */
        rd_(g, rn, sf, 0);
        rd_(g, rm, sf, 0);
        pure_call(g, P_UDIV, sf);
        break;
    case 3: /* SDIV */
        rd_(g, rn, sf, 0);
        rd_(g, rm, sf, 0);
        pure_call(g, P_SDIV, sf);
        break;
    case 8: /* LSLV */
        rd_(g, rn, sf, 0);
        rd_(g, rm, sf, 0);
        i64c(g, w - 1);
        o8(g, W_I64_AND);
        o8(g, W_I64_SHL);
        break;
    case 9: /* LSRV */
        rd_(g, rn, sf, 0);
        rd_(g, rm, sf, 0);
        i64c(g, w - 1);
        o8(g, W_I64_AND);
        o8(g, W_I64_SHR_U);
        break;
    case 10: /* ASRV */
        rd_(g, rn, sf, 0);
        if (!sf)
            o8(g, W_I64_EXTEND32_S);
        rd_(g, rm, sf, 0);
        i64c(g, w - 1);
        o8(g, W_I64_AND);
        o8(g, W_I64_SHR_S);
        break;
    case 11: /* RORV */
        if (sf) {
            rd_(g, rn, 1, 0);
            rd_(g, rm, 1, 0);
            i64c(g, 63);
            o8(g, W_I64_AND);
            o8(g, W_I64_ROTR);
        } else {
            rd_(g, rn, 0, 0);
            lset(g, L_T0);
            rd_(g, rm, 0, 0);
            i64c(g, 31);
            o8(g, W_I64_AND);
            lset(g, L_T1);
            /* ((x >> s) | (x << (32 - s))) & mask, with s in T1 and x in T0 */
            lget(g, L_T0);
            lget(g, L_T1);
            o8(g, W_I64_SHR_U);
            lget(g, L_T0);
            i64c(g, 32);
            lget(g, L_T1);
            o8(g, W_I64_SUB);
            o8(g, W_I64_SHL);
            o8(g, W_I64_OR);
            i64c(g, 0xFFFFFFFFll);
            o8(g, W_I64_AND);
        }
        break;
    default:
        return 0;
    }
    wr(g, rd, sf, 0);
    return 1;
}

static int tr_dp_3src(Gen *g, uint32_t insn)
{
    unsigned sf = insn >> 31, op54 = (insn >> 29) & 3, op31 = (insn >> 21) & 7, o0 = (insn >> 15) & 1;
    unsigned rm = (insn >> 16) & 31, ra = (insn >> 10) & 31, rn = (insn >> 5) & 31, rd = insn & 31;
    if (op54)
        return 0;
    switch (op31) {
    case 0: /* MADD / MSUB */
    case 1: /* SMADDL / SMSUBL */
    case 5: /* UMADDL / UMSUBL */
        if (op31 != 0 && !sf)
            return 0;
        if (op31 == 0) {
            rd_(g, rn, sf, 0);
            rd_(g, rm, sf, 0);
        } else if (op31 == 1) {
            rd_(g, rn, 0, 0);
            o8(g, W_I64_EXTEND32_S);
            rd_(g, rm, 0, 0);
            o8(g, W_I64_EXTEND32_S);
        } else {
            rd_(g, rn, 0, 0);
            rd_(g, rm, 0, 0);
        }
        o8(g, W_I64_MUL);
        lset(g, L_T0);
        rd_(g, ra, op31 == 0 ? sf : 1, 0);
        lset(g, L_T1);
        lget(g, L_T1);
        lget(g, L_T0);
        o8(g, o0 ? W_I64_SUB : W_I64_ADD);
        wr(g, rd, sf, 0);
        return 1;
    case 2: /* SMULH */
        if (!sf || o0)
            return 0;
        rd_(g, rn, 1, 0);
        rd_(g, rm, 1, 0);
        pure_call(g, P_SMULH, 1);
        wr(g, rd, 1, 0);
        return 1;
    case 6: /* UMULH */
        if (!sf || o0)
            return 0;
        rd_(g, rn, 1, 0);
        rd_(g, rm, 1, 0);
        pure_call(g, P_UMULH, 1);
        wr(g, rd, 1, 0);
        return 1;
    default:
        return 0;
    }
}

static int tr_dp_reg(Gen *g, uint32_t insn)
{
    if (!((insn >> 28) & 1)) {
        if (!((insn >> 24) & 1))
            return tr_logical_shifted(g, insn);
        if ((insn >> 21) & 1)
            return tr_addsub_ext(g, insn);
        return tr_addsub_shifted(g, insn);
    }
    if ((insn >> 24) & 1)
        return tr_dp_3src(g, insn);
    switch ((insn >> 21) & 7) {
    case 0: return tr_adc_sbc(g, insn);
    case 2: return tr_ccmp(g, insn);
    case 4: return tr_csel(g, insn);
    case 6: return ((insn >> 30) & 1) ? tr_dp_1src(g, insn) : tr_dp_2src(g, insn);
    default: return 0;
    }
}

static int tr_branch(Gen *g, uint32_t insn)
{
    uint64_t pc = g->pc;
    if ((insn >> 26) == 0x05) { /* B */
        exit_kind(g, pc + (sext(insn & 0x3FFFFFF, 26) << 2), g->cur_n, EXIT_JUMP);
        return 2;
    }
    if ((insn >> 26) == 0x25) { /* BL */
        i64c(g, (int64_t)(pc + 4));
        wr(g, 30, 1, 0);
        exit_kind(g, pc + (sext(insn & 0x3FFFFFF, 26) << 2), g->cur_n, EXIT_JUMP);
        return 2;
    }
    if (((insn >> 25) & 0x3F) == 0x1A) { /* CBZ / CBNZ */
        unsigned sf = insn >> 31, op = (insn >> 24) & 1, rt = insn & 31;
        rd_(g, rt, sf, 0);
        o8(g, W_I64_EQZ);
        if (op)
            o8(g, W_I32_EQZ);
        exit_cond(g, pc + (sext((insn >> 5) & 0x7FFFF, 19) << 2), pc + 4, g->cur_n);
        return 2;
    }
    if (((insn >> 25) & 0x3F) == 0x1B) { /* TBZ / TBNZ */
        unsigned b5 = insn >> 31, op = (insn >> 24) & 1, b40 = (insn >> 19) & 0x1F, rt = insn & 31;
        unsigned bit = (b5 << 5) | b40;
        rd_(g, rt, 1, 0);
        i64c(g, (int64_t)(1ull << bit));
        o8(g, W_I64_AND);
        o8(g, W_I64_EQZ);
        if (op)
            o8(g, W_I32_EQZ);
        exit_cond(g, pc + (sext((insn >> 5) & 0x3FFF, 14) << 2), pc + 4, g->cur_n);
        return 2;
    }
    if (((insn >> 24) & 0xFF) == 0x54) { /* B.cond */
        if (insn & 0x10)
            return 0;
        cond_push(g, insn & 0xF);
        exit_cond(g, pc + (sext((insn >> 5) & 0x7FFFF, 19) << 2), pc + 4, g->cur_n);
        return 2;
    }
    if (((insn >> 25) & 0x7F) == 0x6B) { /* BR / BLR / RET */
        unsigned opc = (insn >> 21) & 0xF, rn = (insn >> 5) & 31;
        if (((insn >> 16) & 0x1F) != 0x1F || ((insn >> 10) & 0x3F) != 0 || (insn & 0x1F) != 0)
            return 0;
        if (opc == 0 || opc == 2) {
            rd_(g, rn, 1, 0);
            lset(g, L_T0);
            exit_dyn(g, g->cur_n);
            return 2;
        }
        if (opc == 1) { /* BLR: the target is read before x30 is written */
            rd_(g, rn, 1, 0);
            lset(g, L_T0);
            i64c(g, (int64_t)(pc + 4));
            wr(g, 30, 1, 0);
            exit_dyn(g, g->cur_n);
            return 2;
        }
        return 0;
    }
    return 0; /* SVC, system instructions and everything else */
}

static int tr_ldst_literal(Gen *g, uint32_t insn)
{
    unsigned opc = insn >> 30, V = (insn >> 26) & 1, rt = insn & 31;
    if (V)
        return 0;
    uint64_t addr = g->pc + (sext((insn >> 5) & 0x7FFFF, 19) << 2);
    switch (opc) {
    case 0:
        i64c(g, (int64_t)addr);
        mem_ld(g, 4);
        wr(g, rt, 0, 0);
        return 1;
    case 1:
        i64c(g, (int64_t)addr);
        mem_ld(g, 8);
        wr(g, rt, 1, 0);
        return 1;
    case 2:
        i64c(g, (int64_t)addr);
        mem_ld(g, 4);
        o8(g, W_I64_EXTEND32_S);
        wr(g, rt, 1, 0);
        return 1;
    default: /* PRFM literal: a hint */
        return 1;
    }
}

static int tr_ldst_pair(Gen *g, uint32_t insn)
{
    unsigned opc = insn >> 30, V = (insn >> 26) & 1, mode = (insn >> 23) & 7, L = (insn >> 22) & 1;
    unsigned rt = insn & 31, rt2 = (insn >> 10) & 31, rn = (insn >> 5) & 31;
    int64_t imm7 = (int64_t)sext((insn >> 15) & 0x7F, 7);
    unsigned bytes, dsf = 1, sign = 0;
    if (mode > 3 || V)
        return 0;
    if (opc == 0) {
        bytes = 4;
        dsf = 0;
    } else if (opc == 2) {
        bytes = 8;
    } else if (opc == 1 && L) {
        bytes = 4;
        sign = 1;
    } else {
        return 0;
    }
    int64_t off = imm7 * (int64_t)bytes;
    rd_(g, rn, 1, 1);
    lset(g, L_T0); /* base */
    lget(g, L_T0);
    if (mode != 1) {
        i64c(g, off);
        o8(g, W_I64_ADD);
    }
    lset(g, L_T1); /* address of the first access */
    if (L) {
        lget(g, L_T1);
        mem_ld(g, bytes);
        lset(g, L_T2);
        lget(g, L_T1);
        i64c(g, bytes);
        o8(g, W_I64_ADD);
        mem_ld(g, bytes);
        lset(g, L_T3);
        if (sign) {
            lget(g, L_T2);
            o8(g, W_I64_EXTEND32_S);
            lset(g, L_T2);
            lget(g, L_T3);
            o8(g, W_I64_EXTEND32_S);
            lset(g, L_T3);
        }
        lget(g, L_T2);
        wr(g, rt, dsf, 0);
        lget(g, L_T3);
        wr(g, rt2, dsf, 0);
    } else {
        lget(g, L_T1);
        rd_(g, rt, dsf, 0);
        mem_st(g, bytes);
        lget(g, L_T1);
        i64c(g, bytes);
        o8(g, W_I64_ADD);
        rd_(g, rt2, dsf, 0);
        mem_st(g, bytes);
    }
    if (mode == 1 || mode == 3) {
        lget(g, L_T0);
        i64c(g, off);
        o8(g, W_I64_ADD);
        wr(g, rn, 1, 1);
    }
    if (!L)
        smc_check(g);
    return 1;
}

static int tr_ldst_single(Gen *g, uint32_t insn)
{
    unsigned size = insn >> 30, V = (insn >> 26) & 1, opc = (insn >> 22) & 3;
    unsigned mode = (insn >> 24) & 3, bit21 = (insn >> 21) & 1, sub = (insn >> 10) & 3;
    unsigned rt = insn & 31, rn = (insn >> 5) & 31;
    if (V)
        return 0;
    if ((insn & 0x3FFFFC00u) == 0x38BFC000u) { /* LDAPR and variants: a plain load */
        rd_(g, rn, 1, 1);
        mem_ld(g, 1u << size);
        wr(g, rt, size == 3, 0);
        return 1;
    }
    if (size == 3 && opc == 2) { /* PRFM forms: hints */
        if (mode == 1)
            return 1;
        if (mode == 0 && !bit21 && sub == 0)
            return 1;
        if (mode == 0 && bit21 && sub == 2)
            return 1;
    }
    if (opc == 3 && size >= 2)
        return 0;
    unsigned scale = size, bytes = 1u << scale;
    int is_load = opc != 0;
    int sign_ext = opc >= 2;
    unsigned dsf = (opc == 2) || (opc == 1 && size == 3);
    int wb = 0;
    int64_t wb_imm = 0;

    rd_(g, rn, 1, 1);
    lset(g, L_T0); /* base */
    if (mode == 1) { /* unsigned immediate */
        lget(g, L_T0);
        i64c(g, (int64_t)(((insn >> 10) & 0xFFF) << scale));
        o8(g, W_I64_ADD);
        lset(g, L_T1);
    } else if (mode == 0 && bit21) { /* register offset */
        if (sub != 2)
            return 0;
        unsigned option = (insn >> 13) & 7, S = (insn >> 12) & 1, rm = (insn >> 16) & 31;
        if (option != 2 && option != 3 && option != 6 && option != 7)
            return 0;
        lget(g, L_T0);
        rd_(g, rm, 1, 0);
        emit_extend(g, option, S ? scale : 0);
        o8(g, W_I64_ADD);
        lset(g, L_T1);
    } else if (mode == 0) { /* unscaled, post-index, pre-index */
        int64_t imm = (int64_t)sext((insn >> 12) & 0x1FF, 9);
        switch (sub) {
        case 0: /* LDUR / STUR */
            lget(g, L_T0);
            i64c(g, imm);
            o8(g, W_I64_ADD);
            lset(g, L_T1);
            break;
        case 1: /* post-index: the access is at base */
            lget(g, L_T0);
            lset(g, L_T1);
            wb = 1;
            wb_imm = imm;
            break;
        case 3: /* pre-index */
            lget(g, L_T0);
            i64c(g, imm);
            o8(g, W_I64_ADD);
            lset(g, L_T1);
            wb = 2;
            break;
        default: /* LDTR / STTR */
            return 0;
        }
    } else {
        return 0;
    }

    if (is_load) {
        lget(g, L_T1);
        mem_ld(g, bytes);
        if (sign_ext) {
            if (bytes == 1)
                o8(g, W_I64_EXTEND8_S);
            else if (bytes == 2)
                o8(g, W_I64_EXTEND16_S);
            else if (bytes == 4)
                o8(g, W_I64_EXTEND32_S);
        }
        wr(g, rt, dsf, 0);
    } else {
        lget(g, L_T1);
        rd_(g, rt, 1, 0);
        mem_st(g, bytes);
    }
    if (wb == 1) {
        lget(g, L_T0);
        i64c(g, wb_imm);
        o8(g, W_I64_ADD);
        wr(g, rn, 1, 1);
    } else if (wb == 2) {
        lget(g, L_T1);
        wr(g, rn, 1, 1);
    }
    if (!is_load)
        smc_check(g);
    return 1;
}

static int tr_ldst(Gen *g, uint32_t insn)
{
    if (((insn >> 24) & 0x3F) == 0x08) /* exclusives and ordered accesses */
        return 0;
    if (((insn >> 27) & 7) == 3 && ((insn >> 24) & 3) == 0)
        return tr_ldst_literal(g, insn);
    if (((insn >> 27) & 7) == 5)
        return tr_ldst_pair(g, insn);
    if (((insn >> 27) & 7) == 7)
        return tr_ldst_single(g, insn);
    return 0; /* SIMD&FP and multi-structure accesses */
}

static int tr_insn(Gen *g, uint32_t insn)
{
    unsigned op0 = (insn >> 25) & 0xF;
    if ((op0 & 0xE) == 0x8)
        return tr_dp_imm(g, insn);
    if ((op0 & 0xE) == 0xA)
        return tr_branch(g, insn);
    if ((op0 & 0x5) == 0x4)
        return tr_ldst(g, insn);
    if ((op0 & 0x7) == 0x5)
        return tr_dp_reg(g, insn);
    return 0; /* SIMD&FP and reserved */
}

/* ---------- blocks ---------- */

/* Translates the block at pc0 (host bytes at page + off). Returns its instruction
 * count, or 0 if the first instruction is not translated. The code is in g->ops. */
static unsigned gen_block(Gen *g, uint64_t pc0, const uint8_t *page, uint32_t off)
{
    unsigned maxn = (4096 - off) / 4;
    if (maxn > JIT_MAX_INSNS)
        maxn = JIT_MAX_INSNS;
    unsigned n = 0;
    int term = 0;
    for (unsigned i = 0; i < maxn; i++) {
        uint32_t insn;
        memcpy(&insn, page + off + 4 * i, 4);
        g->pc = pc0 + 4 * i;
        g->cur_n = i + 1;
        int r = tr_insn(g, insn);
        if (r == 0)
            break;
        n = i + 1;
        if (r == 2) {
            term = 1;
            break;
        }
    }
    if (n == 0)
        return 0;
    if (!term)
        exit_to(g, pc0 + 4 * n, n);
    return n;
}

/* Builds the function body: local declarations, the entry loads, then the code. */
static void finish_body(Gen *g, Buf *out)
{
    out->n = 0;
    buf_uleb(out, 2);
    buf_uleb(out, N_LOCALS_I64);
    buf_put(out, 0x7E);
    buf_uleb(out, N_LOCALS_I32);
    buf_put(out, 0x7F);
    /* The entry loads are built in pro; the code itself is already in ops. */
    g->pro.n = 0;
    g->o = &g->pro;
    for (unsigned r = 0; r < 31; r++) {
        if (g->touched & (1u << r)) {
            lget(g, 0);
            memop(g, W_I64_LOAD, 3, OFF_X(r));
            lset(g, L_X(r));
        }
    }
    if (g->touched & (1u << 31)) {
        lget(g, 0);
        memop(g, W_I64_LOAD, 3, OFF_SP);
        lset(g, L_SP);
    }
    if (g->nzcv_touched) {
        lget(g, 0);
        memop(g, W_I32_LOAD, 2, OFF_NZCV);
        lset(g, L_NZCV);
    }
    buf_bytes(out, g->pro.p, g->pro.n);
    buf_bytes(out, g->ops.p, g->ops.n);
    buf_put(out, W_UNREACHABLE); /* the code ends in a return; this keeps the body valid */
    buf_put(out, W_END);
}

/* ---------- modules ---------- */

static void section(Buf *m, uint8_t id, const Buf *content)
{
    buf_put(m, id);
    buf_uleb(m, content->n);
    buf_bytes(m, content->p, content->n);
}

static void type_entry(Buf *b, int np, const uint8_t *params, int nr, uint8_t result)
{
    buf_put(b, 0x60);
    buf_uleb(b, np);
    buf_bytes(b, params, np);
    buf_uleb(b, nr);
    if (nr)
        buf_put(b, result);
}

static Buf mod_buf, sec_buf, fn_bodies[JIT_BATCH];

/* Ends the current if-arm with an early return of EXIT_BAIL (n = 0), which the caller
 * takes as "already charged, return to the dispatcher". */
static void bail_if(Gen *g)
{
    o8(g, W_IF);
    buf_put(g->o, 0x40);
    i32c(g, EXIT_BAIL << 8);
    o8(g, W_RETURN);
    o8(g, W_END);
}

/* The chain function, the module's first function: (cpu) -> i32. Its locals are 1 (i64)
 * pc and 2 (i32) the cache entry. It bails unless the budget is positive, no stop and no
 * signal is pending, and the pc is cached; then it counts the hit, sets the block's
 * start pc for jit_fault, and tail-calls the cached block. The cache check is the whole
 * validity check: jit_page_changed and tlb_flush drop every entry they make stale. */
static void chain_body(Buf *out)
{
    Gen g;
    Buf code = {0};
    memset(&g, 0, sizeof g);
    g.o = &code;
    i32c(&g, ADDR_BUDGET);
    memop(&g, W_I64_LOAD, 3, 0);
    i64c(&g, 0);
    o8(&g, W_I64_LE_S);
    bail_if(&g);
    lget(&g, 0);
    memop(&g, W_I32_LOAD, 2, OFF_STOP);
    bail_if(&g);
    lget(&g, 0);
    memop(&g, W_I64_LOAD, 3, OFF_SIG_PENDING);
    lget(&g, 0);
    memop(&g, W_I64_LOAD, 3, OFF_SIG_MASK);
    i64c(&g, -1);
    o8(&g, W_I64_XOR);
    o8(&g, W_I64_AND);
    i64c(&g, 0);
    o8(&g, W_I64_NE);
    bail_if(&g);
    lget(&g, 0);
    memop(&g, W_I64_LOAD, 3, OFF_PC);
    lset(&g, 1);
    /* entry = fast + ((pc >> 2) & (JIT_FAST - 1)) * sizeof(FastEnt) */
    lget(&g, 1);
    i64c(&g, 2);
    o8(&g, W_I64_SHR_U);
    o8(&g, W_I32_WRAP_I64);
    i32c(&g, JIT_FAST - 1);
    o8(&g, W_I32_AND);
    i32c(&g, (int32_t)sizeof(FastEnt));
    o8(&g, W_I32_MUL);
    i32c(&g, ADDR_FAST);
    o8(&g, W_I32_ADD);
    lset(&g, 2);
    lget(&g, 2);
    memop(&g, W_I64_LOAD, 3, offsetof(FastEnt, key));
    lget(&g, 1);
    i64c(&g, 1);
    o8(&g, W_I64_ADD);
    o8(&g, W_I64_NE);
    bail_if(&g);
    /* the hop */
    i32c(&g, ADDR_HITS);
    i32c(&g, ADDR_HITS);
    memop(&g, W_I64_LOAD, 3, 0);
    i64c(&g, 1);
    o8(&g, W_I64_ADD);
    memop(&g, W_I64_STORE, 3, 0);
    i32c(&g, ADDR_RUN_START);
    lget(&g, 1);
    memop(&g, W_I64_STORE, 3, 0);
    lget(&g, 0);
    lget(&g, 2);
    memop(&g, W_I32_LOAD, 2, offsetof(FastEnt, fn));
    o8(&g, W_RETURN_CALL_INDIRECT);
    buf_uleb(&code, TY_BLOCK);
    buf_uleb(&code, 0); /* table 0: the imported table */
    i32c(&g, EXIT_BAIL << 8);
    o8(&g, W_RETURN);

    out->n = 0;
    buf_uleb(out, 2);
    buf_uleb(out, 1);
    buf_put(out, 0x7E);
    buf_uleb(out, 1);
    buf_put(out, 0x7F);
    buf_bytes(out, code.p, code.n);
    buf_put(out, W_END);
    free(code.p);
}

static Buf chain_buf;

/* Builds the module of nf functions in fn_bodies. With chain, it also has the table
 * import and the chain function (CHAIN_FN), and the blocks follow it. */
static void build_module(int nf, int chain)
{
    static const uint8_t i32t = 0x7F, i64t = 0x7E;
    Buf *s = &sec_buf;
    mod_buf.n = 0;
    buf_bytes(&mod_buf, "\0asm\x01\0\0\0", 8);
    if (chain) {
        chain_buf.n = 0;
        chain_body(&chain_buf);
    }

    s->n = 0;
    buf_uleb(s, 4);
    type_entry(s, 1, &i32t, 1, i32t);
    {
        const uint8_t p[] = {i64t, i64t, i32t};
        type_entry(s, 3, p, 1, i64t);
    }
    {
        const uint8_t p[] = {i64t, i64t, i64t, i32t};
        type_entry(s, 4, p, 1, i32t);
    }
    {
        const uint8_t p[] = {i32t, i64t, i64t, i32t};
        type_entry(s, 4, p, 1, i64t);
    }
    section(&mod_buf, 1, s);

    s->n = 0;
    buf_uleb(s, N_IMPORTS + 1 + (chain ? 1 : 0));
    buf_name(s, "e");
    buf_name(s, "m");
    buf_put(s, 0x02); /* memory */
    buf_put(s, 0x00); /* no maximum */
    buf_uleb(s, 0);   /* minimum: the engine's memory is larger */
    buf_name(s, "e");
    buf_name(s, "ld");
    buf_put(s, 0x00);
    buf_uleb(s, TY_LD);
    buf_name(s, "e");
    buf_name(s, "st");
    buf_put(s, 0x00);
    buf_uleb(s, TY_ST);
    buf_name(s, "e");
    buf_name(s, "pure");
    buf_put(s, 0x00);
    buf_uleb(s, TY_PURE);
    if (chain) { /* the table the chain tail-calls through */
        buf_name(s, "e");
        buf_name(s, "t");
        buf_put(s, 0x01); /* table */
        buf_put(s, 0x70); /* funcref */
        buf_put(s, 0x00); /* no maximum */
        buf_uleb(s, 0);   /* minimum: the engine's table is larger */
    }
    section(&mod_buf, 2, s);

    int nfn = nf + (chain ? 1 : 0); /* chain_fn, then the blocks */
    s->n = 0;
    buf_uleb(s, nfn);
    for (int i = 0; i < nfn; i++)
        buf_uleb(s, TY_BLOCK);
    section(&mod_buf, 3, s);

    s->n = 0;
    buf_uleb(s, nf);
    for (int i = 0; i < nf; i++) {
        char name[16];
        snprintf(name, sizeof name, "f%d", i);
        buf_name(s, name);
        buf_put(s, 0x00);
        buf_uleb(s, N_IMPORTS + (chain ? 1 : 0) + i);
    }
    section(&mod_buf, 7, s);

    s->n = 0;
    buf_uleb(s, nfn);
    if (chain) {
        buf_uleb(s, chain_buf.n);
        buf_bytes(s, chain_buf.p, chain_buf.n);
    }
    for (int i = 0; i < nf; i++) {
        buf_uleb(s, fn_bodies[i].n);
        buf_bytes(s, fn_bodies[i].p, fn_bodies[i].n);
    }
    section(&mod_buf, 10, s);
}

/* ---------- batches ---------- */

static Gen gen; /* reused for every block: its buffers keep their memory */

static void translate_batch(void)
{
    int nf = 0;
    int fidx[JIT_BATCH];
    int new_code = 0;
    for (int q = 0; q < qlen; q++) {
        Pend *p = &queue[q];
        if (blk_find(p->host, p->vpage))
            continue;
        /* The page may have been unmapped or moved since the block was queued. */
        uint8_t *now = mem_exec_page(p->pc);
        if (!now || (uintptr_t)now + (p->pc & 0xFFF) != p->host)
            continue;
        uintptr_t page = p->host & ~(uintptr_t)0xFFF;
        PageEnt *pe = page_find(page, 1);
        if (!pe) {
            st_refused++;
            continue;
        }
        if (!pe->code) {
            pe->code = 1;
            new_code = 1;
        }
        Blk *b = blk_new(p->host, p->vpage, pe);
        if (!b) {
            st_refused++;
            continue;
        }
        Gen *g = &gen;
        g->ops.n = 0;
        g->pro.n = 0;
        g->o = &g->ops;
        g->touched = g->written = 0;
        g->nzcv_touched = g->nzcv_written = 0;
        g->chain = chain_on;
        unsigned n = gen_block(g, p->pc, (const uint8_t *)page, (uint32_t)(p->host - page));
        if (n == 0) {
            st_untranslatable++;
            continue;
        }
        finish_body(g, &fn_bodies[nf]);
        fidx[nf] = (int)(b - blks);
        nf++;
    }
    if (new_code)
        mem_flush_write_tlb();
    if (nf) {
        build_module(nf, chain_on);
        st_modules++;
        int base = js_jit_compile(mod_buf.p, (int)mod_buf.n, nf);
        if (base < 0) {
            st_compile_fail++;
        } else {
            for (int i = 0; i < nf; i++) {
                blks[fidx[i]].fn = base + i;
                st_blocks++;
            }
        }
    }
    qlen = 0;
}

/* ---------- dispatch ---------- */

/* Runs the block at table index fn, and any blocks it chains to. The blocks charge
 * their instructions to c->icount and jit_budget as they exit, so this only sets the
 * budget in and copies it out. */
static void run_block(cpu_t *c, int32_t fn, uint64_t *budget)
{
    run_start = c->pc;
    running = 1;
    jit_budget = (int64_t)*budget;
    uint64_t ic0 = c->icount;
    int32_t r = ((int32_t (*)(cpu_t *))(uintptr_t)(uint32_t)fn)(c);
    running = 0;
    if ((r >> 8) == EXIT_SMC)
        st_smc_exits++;
    st_insns += c->icount - ic0;
    st_block_runs++;
    *budget = jit_budget > 0 ? (uint64_t)jit_budget : 0;
}

int jit_dispatch(cpu_t *c, uint64_t *budget)
{
    if (!jit_enabled() || prof_on || (c->pc & 3))
        return 0;
    uint64_t pc = c->pc;
    uint8_t *pg = mem_exec_page(pc);
    if (!pg)
        return 0;
    uintptr_t host = (uintptr_t)pg + (pc & 0xFFF);
    uint64_t vpage = (pc & 0x00FFFFFFFFFFFFFFull) >> 12;
    st_dispatch++;
    Blk *b = blk_find(host, vpage);
    if (b) {
        if (b->fn < 0)
            return 2; /* not translatable: the interpreter runs it, then may re-enter */
        fast_fill(pc, b->fn);
        run_block(c, b->fn, budget);
        return 1;
    }
    uint16_t *k = &cnt[cnt_hash(host, vpage)];
    if (++*k >= JIT_HOT) { /* hot: queue it; batches are translated together */
        *k = 0;
        queue[qlen].host = host;
        queue[qlen].vpage = vpage;
        queue[qlen].pc = pc;
        qlen++;
        if (qlen == JIT_BATCH)
            translate_batch();
    } else if (qlen && (++tick % JIT_TICK) == 0) {
        translate_batch();
    } else {
        return 0;
    }
    /* A batch may have translated this pc just now. Run it here, rather than
     * interpreting the instruction and leaving the block for the next entry. */
    b = blk_find(host, vpage);
    if (b && b->fn >= 0) {
        fast_fill(pc, b->fn);
        run_block(c, b->fn, budget);
        return 1;
    }
    return 0;
}

void jit_fault(cpu_t *c, uint64_t fault_pc)
{
    if (!running)
        return;
    running = 0;
    st_faults++;
    uint64_t n = fault_pc >= run_start ? (fault_pc - run_start) / 4 + 1 : 1;
    c->icount += n;
    st_insns += n;
}

void jit_print_stats(uint64_t total_insns)
{
    jit_enabled();
    fprintf(stderr,
            "xiaoji: jit %s: %llu of %llu instructions in translated code (%.1f%%); "
            "%llu blocks in %llu modules (%llu refused, %llu not translatable, %llu compile failures); "
            "%llu block runs of %llu dispatches, %llu chain hits; %llu faults in translated code; "
            "%llu self-modifying exits; %llu pages and %llu blocks invalidated\n",
            jit_on ? "on" : "off", (unsigned long long)st_insns, (unsigned long long)total_insns,
            total_insns ? 100.0 * (double)st_insns / (double)total_insns : 0.0,
            (unsigned long long)st_blocks, (unsigned long long)st_modules,
            (unsigned long long)st_refused, (unsigned long long)st_untranslatable,
            (unsigned long long)st_compile_fail, (unsigned long long)st_block_runs,
            (unsigned long long)st_dispatch, (unsigned long long)st_chain_hits, (unsigned long long)st_faults,
            (unsigned long long)st_smc_exits, (unsigned long long)st_inv_pages,
            (unsigned long long)st_inv_blocks);
}

/* ---------- helpers called from generated code ---------- */

EMSCRIPTEN_KEEPALIVE uint64_t jit_ld(uint64_t va, uint64_t pc, uint32_t size)
{
    mem_set_pc(pc);
    switch (size) {
    case 1: return mem_read8(va);
    case 2: return mem_read16(va);
    case 4: return mem_read32(va);
    default: return mem_read64(va);
    }
}

EMSCRIPTEN_KEEPALIVE int32_t jit_st(uint64_t va, uint64_t v, uint64_t pc, uint32_t size)
{
    uint32_t before = inval_seq;
    mem_set_pc(pc);
    switch (size) {
    case 1: mem_write8(va, (uint8_t)v); break;
    case 2: mem_write16(va, (uint16_t)v); break;
    case 4: mem_write32(va, (uint32_t)v); break;
    default: mem_write64(va, v); break;
    }
    return inval_seq != before;
}

EMSCRIPTEN_KEEPALIVE uint64_t jit_pure(uint32_t op, uint64_t a, uint64_t b, uint32_t sf)
{
    unsigned w = sf ? 64 : 32;
    uint64_t m = sf ? ~0ull : 0xFFFFFFFFull;
    switch (op) {
    case P_RBIT: {
        uint64_t r = 0;
        for (unsigned i = 0; i < w; i++)
            r |= ((a >> i) & 1) << (w - 1 - i);
        return r;
    }
    case P_REV16: {
        uint64_t r = 0;
        for (unsigned k = 0; k < w / 16; k++) {
            uint64_t chunk = (a >> (16 * k)) & 0xFFFF;
            r |= (((chunk & 0xFF) << 8) | (chunk >> 8)) << (16 * k);
        }
        return r;
    }
    case P_REV32:
        if (sf)
            return (uint64_t)__builtin_bswap32((uint32_t)a) |
                   ((uint64_t)__builtin_bswap32((uint32_t)(a >> 32)) << 32);
        return __builtin_bswap32((uint32_t)a);
    case P_REV64:
        return __builtin_bswap64(a);
    case P_CLS: {
        unsigned sign = (unsigned)((a >> (w - 1)) & 1);
        uint64_t x = sign ? (~a & m) : a;
        return (x ? (uint64_t)(__builtin_clzll(x) - (64 - w)) : w) - 1;
    }
    case P_UDIV:
        return b ? a / b : 0;
    case P_SDIV: {
        int64_t minv = (w == 64) ? INT64_MIN : -2147483648LL;
        int64_t sa = (int64_t)sext(a, w), sb = (int64_t)sext(b, w);
        if (sb == 0)
            return 0;
        if (sb == -1 && sa == minv)
            return (uint64_t)sa;
        return (uint64_t)(sa / sb);
    }
    case P_UMULH:
        return (uint64_t)(((unsigned __int128)a * b) >> 64);
    case P_SMULH:
        return (uint64_t)(((__int128)(int64_t)a * (__int128)(int64_t)b) >> 64);
    default: { /* P_CRC | opcode: a is the accumulator, b the data */
        unsigned sz = op & 3, cast = (op >> 2) & 1;
        uint32_t poly = cast ? 0x82F63B78u : 0xEDB88320u;
        uint32_t crc = (uint32_t)a;
        for (unsigned i = 0; i < (1u << sz); i++) {
            crc ^= (uint8_t)(b >> (8 * i));
            for (unsigned k = 0; k < 8; k++)
                crc = (crc & 1) ? (crc >> 1) ^ poly : crc >> 1;
        }
        return crc;
    }
    }
}
