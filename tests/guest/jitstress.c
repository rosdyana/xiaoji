/* JIT stress: hot loops that the block translator must run and get right, and code
 * that rewrites itself after it has been translated.
 *
 * alu_kernel is one long block of inline assembly: ALU ops in every shifted, extended
 * and immediate form, flag-setting ops, conditional selects and compares, multiplies,
 * divides, bit operations, and every integer load and store form. Its results and the
 * flags are folded into a checksum over thousands of iterations, so the block is
 * translated and then run hot. Then the program rewrites a function it has already
 * run hot in three ways (a store into an RWX page, mprotect RW then RX, and munmap then
 * mmap at the same address) and calls it again. Output must match native. */
#include "sys.h"

#include <stdint.h>

#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20

/* Calls per hot loop. The translator waits for a batch of hot blocks before it
 * compiles them, so each loop is long enough that its code is translated before the
 * rewrite or the fault. */
#define CALLS 12000

static u64 seed = 88172645463325252ull;

static u64 rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

/* r[0..7]: inputs and outputs in x0..x7. r[8..31]: scratch memory for the loads and
 * stores. r[31] receives the flags. */
static void alu_kernel(u64 *r)
{
    register u64 *p asm("x9") = r;
    asm volatile(
        "ldp x0, x1, [x9]\n"
        "ldp x2, x3, [x9, #16]\n"
        "ldp x4, x5, [x9, #32]\n"
        "ldp x6, x7, [x9, #48]\n"
        "adds x0, x0, x1\n"
        "adcs x2, x2, x3\n"
        "sbcs x4, x4, x5\n"
        "subs w6, w6, w7\n"
        "ccmp x0, x2, #5, ne\n"
        "csel x1, x0, x2, lt\n"
        "csinc x3, x4, x6, hi\n"
        "csinv x5, x7, x0, ge\n"
        "csneg x7, x1, x3, le\n"
        "madd x0, x1, x2, x3\n"
        "msub x2, x4, x5, x6\n"
        "smaddl x4, w5, w7, x0\n"
        "umaddl x6, w0, w1, x2\n"
        "umulh x6, x0, x1\n"
        "smulh x7, x2, x3\n"
        "udiv x0, x1, x2\n"
        "sdiv x3, x4, x5\n"
        "lsl x1, x0, x2\n"
        "lsr w2, w3, w4\n"
        "asr x3, x4, x5\n"
        "ror x4, x5, x6\n"
        "ror w5, w6, w7\n"
        "ubfx x5, x6, #3, #17\n"
        "sbfx x6, x7, #5, #9\n"
        "bfi x7, x0, #4, #12\n"
        "extr x0, x1, x2, #13\n"
        "clz x1, x2\n"
        "cls w3, w4\n"
        "rbit x5, x6\n"
        "rev x7, x0\n"
        "rev16 w1, w2\n"
        "rev32 x3, x4\n"
        "and x0, x0, #0xff00ff00ff00ff00\n"
        "orr w1, w1, #0x0000ffff\n"
        "eor x2, x2, x3, lsr #7\n"
        "bics x4, x4, x5, asr #3\n"
        "orn x5, x5, x6\n"
        "eon x6, x6, x7, ror #9\n"
        "add x7, x7, x0, sxtb #2\n"
        "sub x0, x0, w1, uxth #1\n"
        "adds x1, x1, w2, sxtw\n"
        "add x2, x2, x3, lsl #5\n"
        "subs x3, x3, x4, asr #9\n"
        "movz x4, #0x1234, lsl #16\n"
        "movk x5, #0xbeef\n"
        "movn x6, #7\n"
        "tst x7, #0x10\n"
        "cset w8, eq\n"
        "cinc x1, x1, cc\n"
        "eor xzr, x1, x2\n"
        "and wzr, w3, w4\n"
        "orr xzr, x5, x6\n"
        "mrs x8, nzcv\n"
        "str x0, [x9, #64]\n"
        "strb w1, [x9, #72]\n"
        "strh w2, [x9, #74]\n"
        "str w3, [x9, #76]\n"
        "ldrb w4, [x9, #72]\n"
        "ldrsb x5, [x9, #72]\n"
        "ldrh w6, [x9, #74]\n"
        "ldrsh x7, [x9, #74]\n"
        "ldrsw x0, [x9, #76]\n"
        "mov x10, x9\n"
        "str x1, [x10, #80]!\n"
        "ldr x2, [x10], #8\n"
        "stur x3, [x9, #88]\n"
        "ldur x4, [x9, #88]\n"
        "mov x11, #8\n"
        "str x5, [x9, x11, lsl #3]\n"
        "ldr x6, [x9, x11, lsl #3]\n"
        "stp x0, x1, [x9, #96]\n"
        "ldp x2, x3, [x9, #96]\n"
        "ldpsw x4, x5, [x9, #104]\n"
        "stp x6, x7, [x10, #112]!\n"
        "ldp x0, x1, [x10], #-16\n"
        "adr x11, 1f\n"
        "1:\n"
        "adds x2, x2, x11\n"
        "str x8, [x9, #248]\n"
        "stp x0, x1, [x9]\n"
        "stp x2, x3, [x9, #16]\n"
        "stp x4, x5, [x9, #32]\n"
        "stp x6, x7, [x9, #48]\n"
        : "+r"(p)
        :
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x10", "x11", "cc", "memory");
}

static int checks;

/* ---------- a fault inside translated code ---------- */

#define SIGSEGV 11
#define SA_SIGINFO 0x4ull
#define SA_RESTORER 0x04000000ull
#define SYS_RT_SIGACTION 134

typedef struct {
    u64 handler, flags, restorer, mask;
} ksigaction;

typedef struct {
    i32 signo, err, code, pad;
    u64 w[14];
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
    u64 uc_stack[3];
    u64 uc_sigmask;
    u8 unused[127];
    kmcontext uc_mcontext;
} kucontext;

/* A leaf with no frame: one load, then return. The handler resumes it at its caller,
 * so the fault is in a block that is translated after the loop runs hot. */
__asm__(".text\n"
        ".global jitstress_load\n"
        "jitstress_load:\n"
        "  ldr x0, [x0]\n"
        "  ret\n"
        ".global jitstress_restore\n"
        "jitstress_restore:\n"
        "  mov x8, #139\n"
        "  svc #0\n");
extern u64 jitstress_load(const u64 *p);
extern void jitstress_restore(void);

/* A leaf entered through the chain: three ALU ops, then the load at offset 12. The
 * fault is the fourth instruction of its block, so its instruction count depends on
 * where the block starts. The handler sets x0 = 77 and returns to the caller. */
extern u64 jitstress_chain_load(const u64 *p, u64 k);
__asm__(".text\n"
        ".global jitstress_chain_load\n"
        "jitstress_chain_load:\n"
        "  add x2, x1, #3\n"
        "  eor x2, x2, x1, lsl #2\n"
        "  add x2, x2, #1\n"
        "  ldr x0, [x0]\n"
        "  ret\n");

/* jitstress_chain_run(good, bad, n): calls jitstress_chain_load n times from one call
 * site. The pointer is selected with csel, so the last pass (the bad pointer) runs the
 * same blocks as every other pass: the loop head, the call, the leaf and the return
 * point. A C ternary compiles to a separate block that first runs on the last pass, and
 * that block is entered from C, not from a chain hop. Returns the sum of the results. */
extern u64 jitstress_chain_run(const u64 *good, const u64 *bad, u64 n);
__asm__(".text\n"
        ".global jitstress_chain_run\n"
        "jitstress_chain_run:\n"
        "  stp x29, x30, [sp, #-80]!\n"
        "  mov x29, sp\n"
        "  stp x19, x20, [sp, #16]\n"
        "  stp x21, x22, [sp, #32]\n"
        "  stp x23, x24, [sp, #48]\n"
        "  mov x19, x0\n"
        "  mov x20, x1\n"
        "  mov x21, x2\n"
        "  mov x22, #0\n"
        "  mov x23, #0\n"
        "1:\n"
        "  sub x24, x21, #1\n"
        "  cmp x23, x24\n"
        "  csel x0, x20, x19, eq\n"
        "  mov x1, x23\n"
        "  bl jitstress_chain_load\n"
        "  add x22, x22, x0\n"
        "  add x23, x23, #1\n"
        "  cmp x23, x21\n"
        "  b.lo 1b\n"
        "  mov x0, x22\n"
        "  ldp x23, x24, [sp, #48]\n"
        "  ldp x21, x22, [sp, #32]\n"
        "  ldp x19, x20, [sp, #16]\n"
        "  ldp x29, x30, [sp], #80\n"
        "  ret\n");

/* Leaves with a fault in the middle, after registers were written: the handler must
 * see those values (x1..x3) and the faulting pc, and the post-index load must not have
 * written back. */
__asm__(".text\n"
        ".global jitstress_mid_load\n"
        "jitstress_mid_load:\n"
        "  mov x1, #41\n"
        "  add x1, x1, #1\n"
        "  subs x2, x1, #43\n"
        "  mov x3, #0x1234\n"
        "  ldr x4, [x0], #8\n" /* offset 16 */
        "  ret\n"
        ".global jitstress_mid_store\n"
        "jitstress_mid_store:\n"
        "  mov x1, #41\n"
        "  add x1, x1, #1\n"
        "  mov x3, #0x77\n"
        "  str x3, [x0]\n" /* offset 12 */
        "  ret\n");
extern u64 jitstress_mid_load(u64 *p);
extern void jitstress_mid_store(u64 *p);

static volatile int faults;
static volatile u64 snap_pc, snap_addr, snap_x[5], snap_pstate;

static void fault_handler(int sig, kinfo *info, kucontext *uc)
{
    (void)sig;
    (void)info;
    faults++;
    snap_pc = uc->uc_mcontext.pc;
    snap_addr = uc->uc_mcontext.fault_address;
    for (int k = 0; k < 5; k++)
        snap_x[k] = uc->uc_mcontext.regs[k];
    snap_pstate = uc->uc_mcontext.pstate;
    uc->uc_mcontext.regs[0] = 77;
    uc->uc_mcontext.pc = uc->uc_mcontext.regs[30];
}

static void install_segv(void)
{
    ksigaction sa;
    sa.handler = (u64)fault_handler;
    sa.flags = SA_SIGINFO | SA_RESTORER;
    sa.restorer = (u64)jitstress_restore;
    sa.mask = 0;
    sc6(SYS_RT_SIGACTION, SIGSEGV, (i64)&sa, 0, 8, 0, 0);
}

static void hot_fault(u64 *sum)
{
    static const u64 good = 5;
    u64 s = *sum;
    for (u64 i = 0; i < CALLS; i++)
        s += jitstress_load(&good);
    /* The last load faults. The handler makes it return 77 to here. */
    s += jitstress_load((const u64 *)0x40);
    *sum = s;
}

/* ---------- FP and SIMD in hot blocks ---------- */

/* jitstress_fpsimd(in, out, fpcr): one pass of scalar FP (arithmetic, FMA, compare and
 * select, conversions in both directions, FP<->GPR moves, rounding), SIMD (DUP, ADD,
 * CMEQ, ADDP, UMAXP, AND, ORR, EOR, MOVI, INS, UMOV, LDR/STR of Q registers, LD1 and
 * ST1 of several registers) and the NZCV and FPSR the pass sets. The inputs are 6 words
 * (d0, d1, the two halves of v2 and v3, and x6). FPCR is set for the pass and restored.
 * Results go to out[0..51] (word 49 is unused). Only v0..v7 and v16..v31 are used, so no
 * callee-saved state is touched. Only the default FPCR is compared with native: the
 * interpreter does not implement non-default rounding modes, FZ or DN, or the cumulative
 * FPSR flags (simd.c), so this test does not run them. */
extern void jitstress_fpsimd(const u64 *in, u64 *out, u64 fpcr);
__asm__(".text\n"
        ".global jitstress_fpsimd\n"
        "jitstress_fpsimd:\n"
        "  mrs x9, fpcr\n"
        "  msr fpcr, x2\n"
        "  ldr d0, [x0]\n"
        "  ldr d1, [x0, #8]\n"
        "  ldr q2, [x0, #16]\n"
        "  ldr q3, [x0, #32]\n"
        "  ldr x6, [x0, #48]\n"
        "  fadd d4, d0, d1\n"
        "  fsub d5, d0, d1\n"
        "  fmul d6, d0, d1\n"
        "  fdiv d7, d0, d1\n"
        "  fsqrt d16, d0\n"
        "  fmadd d17, d0, d1, d4\n"
        "  fmsub d18, d0, d1, d4\n"
        "  fmax d19, d0, d1\n"
        "  fmin d20, d0, d1\n"
        "  fabs d21, d0\n"
        "  fneg d22, d1\n"
        "  fcmp d0, d1\n"
        "  fcsel d23, d0, d1, lt\n"
        "  fcvt s24, d0\n"
        "  fcvt d25, s24\n"
        "  fcvtzs x7, d0\n"
        "  fcvtzu x8, d1\n"
        "  scvtf d26, x6\n"
        "  ucvtf d27, x6\n"
        "  fmov x10, d4\n"
        "  fmov d28, x6\n"
        "  frintz d29, d0\n"
        "  str d4, [x1, #0]\n"
        "  str d5, [x1, #8]\n"
        "  str d6, [x1, #16]\n"
        "  str d7, [x1, #24]\n"
        "  str d16, [x1, #32]\n"
        "  str d17, [x1, #40]\n"
        "  str d18, [x1, #48]\n"
        "  str d19, [x1, #56]\n"
        "  str d20, [x1, #64]\n"
        "  str d21, [x1, #72]\n"
        "  str d22, [x1, #80]\n"
        "  str d23, [x1, #88]\n"
        "  str d24, [x1, #96]\n"
        "  str d25, [x1, #104]\n"
        "  str d26, [x1, #112]\n"
        "  str d27, [x1, #120]\n"
        "  str d28, [x1, #128]\n"
        "  str d29, [x1, #136]\n"
        "  str x7, [x1, #144]\n"
        "  str x8, [x1, #152]\n"
        "  str x10, [x1, #160]\n"
        "  mrs x11, nzcv\n"
        "  str x11, [x1, #168]\n"
        "  add x14, x1, #176\n"
        "  dup v0.2d, x10\n"
        "  add v1.2d, v2.2d, v3.2d\n"
        "  cmeq v4.16b, v2.16b, v3.16b\n"
        "  addp v5.4s, v2.4s, v3.4s\n"
        "  umaxp v6.4s, v2.4s, v3.4s\n"
        "  and v7.16b, v2.16b, v3.16b\n"
        "  orr v16.16b, v2.16b, v3.16b\n"
        "  eor v17.16b, v2.16b, v3.16b\n"
        "  movi v18.16b, #0x5a\n"
        "  ins v19.d[1], x6\n"
        "  umov x13, v2.d[1]\n"
        "  ldr q21, [x0]\n"
        "  ld1 {v20.2d}, [x0], #16\n"
        "  str q0, [x14, #0]\n"
        "  str q1, [x14, #16]\n"
        "  str q4, [x14, #32]\n"
        "  str q5, [x14, #48]\n"
        "  str q6, [x14, #64]\n"
        "  str q7, [x14, #80]\n"
        "  str q16, [x14, #96]\n"
        "  str q17, [x14, #112]\n"
        "  str q18, [x14, #128]\n"
        "  str q19, [x14, #144]\n"
        "  add x14, x14, #160\n"
        "  st1 {v2.2d, v3.2d}, [x14]\n"
        "  str q20, [x14, #32]\n"
        "  str x13, [x14, #48]\n"
        "  str q21, [x14, #64]\n"
        "  msr fpcr, x9\n"
        "  ret\n");

/* jitstress_vec_load(p): a SIMD load that faults when p is bad; the handler makes the
 * call return 77 (x0 = 77 and pc = the return address). */
extern u64 jitstress_vec_load(const u64 *p);
__asm__(".text\n"
        ".global jitstress_vec_load\n"
        "jitstress_vec_load:\n"
        "  ldr q0, [x0]\n"
        "  fmov x0, d0\n"
        "  ret\n");

/* jitstress_vec_mem(src, dst): SIMD&FP loads and stores of every scalar size (B, H, S, D)
 * and of Q, with unsigned-offset, post-index and pre-index forms; pairs of S, D and Q
 * registers; and a literal load. Returns the movement of src in the low word and of dst
 * (shifted) in the high. */
extern u64 jitstress_vec_mem(const u8 *src, u8 *dst);
__asm__(".text\n"
        ".global jitstress_vec_mem\n"
        "jitstress_vec_mem:\n"
        "  mov x2, x0\n"
        "  mov x3, x1\n"
        "  ldr d0, [x0], #8\n"
        "  ldr q1, [x0], #16\n"
        "  ldr s2, [x0, #8]\n"
        "  ldr h3, [x0, #-8]!\n"
        "  ldr b4, [x0, #1]\n"
        "  str q1, [x1]\n"
        "  str d0, [x1, #16]!\n"
        "  str s2, [x1], #8\n"
        "  str h3, [x1]\n"
        "  str b4, [x1, #2]\n"
        "  ldp s24, s25, [x0, #8]\n"
        "  ldp d26, d27, [x0], #16\n"
        "  ldp q28, q29, [x0, #-16]\n"
        "  ldr q30, 1f\n"
        "  add x5, x3, #64\n"
        "  stp s24, s25, [x5]\n"
        "  str q30, [x5, #16]\n"
        "  stp d26, d27, [x5, #32]\n"
        "  stp q28, q29, [x5, #48]\n"
        "  ldp q16, q17, [x0, #-32]!\n"
        "  stp q16, q17, [x5, #80]\n"
        "  sub x0, x0, x2\n"
        "  sub x1, x1, x3\n"
        "  lsl x1, x1, #32\n"
        "  orr x0, x0, x1\n"
        "  ret\n"
        "  .balign 16\n"
        "1: .quad 0x0123456789ABCDEF, 0xFEDCBA9876543210\n");

/* jitstress_ld1_load(p): a multi-structure SIMD load (LD1), which the translator runs
 * through the interpreter's own code. The call faults when p is bad. */
extern u64 jitstress_ld1_load(const u64 *p);
__asm__(".text\n"
        ".global jitstress_ld1_load\n"
        "jitstress_ld1_load:\n"
        "  ld1 {v0.16b}, [x0]\n"
        "  fmov x0, d0\n"
        "  ret\n");

static u8 vm_src[64] __attribute__((aligned(16)));
static u8 vm_dst[176] __attribute__((aligned(16)));

/* Hot SIMD&FP loads and stores: each call gets new source bytes, and the checksum covers
 * the return value and the destination bytes. */
static void vec_mem_hot(u64 *sum)
{
    u64 s = *sum;
    for (u64 i = 0; i < CALLS; i++) {
        for (int k = 0; k < 48; k++)
            vm_src[k] = (u8)(i * 31 + k * 7 + (i >> 3));
        s += jitstress_vec_mem(vm_src, vm_dst);
        for (int k = 0; k < 176; k++)
            s = (s ^ vm_dst[k]) * 0x100000001B3ull;
    }
    *sum = s;
}

static u64 fp_in[8] __attribute__((aligned(16)));
static u64 fp_out[64] __attribute__((aligned(16)));

/* Inputs: mostly ordinary values, and a share of the special encodings (zeros, infinities,
 * a subnormal, the largest finite value) and random bits. NaN inputs are left out: the
 * interpreter gets the sign of a NaN operand wrong in FMSUB (it negates the NaN; the
 * hardware keeps it), so they cannot be compared with native. NaN results still occur:
 * 0/0 and infinity minus infinity give the default NaN. */
static u64 fp_value(void)
{
    static const u64 specials[] = {
        0x0000000000000000ull, 0x8000000000000000ull, 0x7FF0000000000000ull,
        0xFFF0000000000000ull, 0x0000000000000001ull, 0x7FEFFFFFFFFFFFFFull,
    };
    u64 k = rnd() % 8;
    if (k < 3)
        return specials[rnd() % (sizeof specials / sizeof specials[0])];
    if (k < 6) {
        union {
            double d;
            u64 b;
        } u;
        u.d = (double)(i64)(rnd() % 2001) - 1000.0;
        return u.b;
    }
    u64 r = rnd();
    if (((r >> 52) & 0x7FF) == 0x7FF && (r & 0x000FFFFFFFFFFFFFull))
        r ^= 1ull << 62; /* a NaN becomes a finite value */
    return r;
}

/* Hot FP and SIMD, with the default FPCR. The checksum covers every output word of every
 * pass; the inputs include NaNs, infinities and zeros. */
static void fp_hot(u64 *sum)
{
    u64 s = *sum;
    for (u64 i = 0; i < 2 * CALLS; i++) {
        for (int k = 0; k < 6; k++)
            fp_in[k] = fp_value();
        fp_in[6] = fp_in[7] = 0;
        jitstress_fpsimd(fp_in, fp_out, 0);
        for (int k = 0; k < 52; k++)
            s = (s ^ fp_out[k]) * 0x100000001B3ull;
    }
    *sum = s;
}

/* An unallocated AdvSIMD encoding (bit 31 set), in a hot function. Native raises SIGILL.
 * The handler counts it and skips the instruction, and the function returns 3. The engine
 * must take the same path from translated code: the block leaves at that instruction and
 * the signal is delivered there. */
extern u64 jitstress_undef(void);
__asm__(".text\n"
        ".global jitstress_undef\n"
        "jitstress_undef:\n"
        "  .inst 0x8E000400\n"
        "  mov x0, #3\n"
        "  ret\n");

static volatile int ills;

static void ill_handler(int sig, kinfo *info, kucontext *uc)
{
    (void)sig;
    (void)info;
    ills++;
    uc->uc_mcontext.pc += 4;
}

static void install_ill(void)
{
    ksigaction sa;
    sa.handler = (u64)ill_handler;
    sa.flags = SA_SIGINFO | SA_RESTORER;
    sa.restorer = (u64)jitstress_restore;
    sa.mask = 0;
    sc6(SYS_RT_SIGACTION, 4 /* SIGILL */, (i64)&sa, 0, 8, 0, 0);
}

#define RET_INSN 0xD65F03C0u
static u32 movz_w0(u32 imm) { return 0x52800000u | (imm << 5); }

/* HINT-space NOPs and barriers inside a hot loop. The translator treats them as no-ops
 * and keeps the block going, so the loop runs as translated code. */
static u64 hint_loop(u64 n)
{
    u64 acc = 0;
    for (u64 i = 0; i < n; i++) {
        __asm__ volatile("nop\n\t"
                         "dmb ish\n\t"
                         "dsb sy\n\t"
                         "hint #34\n\t"
                         "isb\n\t"
                         : : : "memory");
        acc += i ^ (acc >> 2);
    }
    return acc;
}

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

/* Makes the instruction at p, just written, visible to fetch: the same maintenance as
 * the self-modify test. A real core keeps stale instruction lines otherwise. */
static void sync_code(void *p)
{
    __asm__ volatile("dc cvau, %0\n\t"
                     "dsb ish\n\t"
                     "ic ivau, %0\n\t"
                     "dsb ish\n\t"
                     "isb"
                     : : "r"(p) : "memory");
}

/* A translated loop that rewrites a function it calls, in the middle of the loop. The
 * store is a block exit, and the calls after it must run the new instruction. */
static void smc_chain(u64 *sum)
{
    typedef u64 (*pure_fn)(void);
    u32 *fn = (u32 *)sys_mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    fn[0] = movz_w0(2);
    fn[1] = RET_INSN;
    sync_code(fn);
    u64 s = *sum;
    for (u64 i = 0; i < CALLS; i++) {
        if (i == CALLS / 2) {
            fn[0] = movz_w0(5);
            sync_code(fn);
        }
        s += ((pure_fn)(uintptr_t)fn)();
    }
    *sum = s;
}

/* A fault reached by a chain: the asm loop calls the leaf on every pass from one call
 * site, and the last pass passes a bad pointer, so the fault is in a block entered by a
 * chain hop from the loop. The handler makes that load return 77. The loop is twice as
 * long as the warm-up, so the blocks are translated and chained when the last pass
 * faults. */
static void chain_fault(u64 *sum)
{
    static const u64 good = 5;
    *sum = jitstress_chain_run(&good, (const u64 *)0x40, 2 * CALLS);
}

static void check(const char *label, int ok)
{
    put(label);
    put(ok ? "=ok\n" : "=FAIL\n");
    if (!ok)
        checks++;
}

/* Branches and calls in C: conditional branches, compare-and-branch, bit tests, and a
 * call through a pointer. */
static u64 leaf(u64 x)
{
    return (x * 0x9E3779B97F4A7C15ull) >> 17;
}

static u64 branchy(u64 x)
{
    u64 acc = 0;
    for (u64 i = 0; i < 16; i++) {
        if (x & (1ull << i))
            acc += leaf(x + i);
        else if (i & 1)
            acc ^= x >> i;
        if (i == 7)
            continue;
        acc += (x >> 2) ? 3 : 5;
    }
    return acc;
}

typedef u64 (*fn_t)(u64);

static void hot_branches(u64 *sum)
{
    fn_t fp = leaf;
    u64 s = *sum;
    for (u64 i = 0; i < 6000; i++) {
        u64 x = rnd();
        s += branchy(x);
        s += fp(x);
        if (!(x & 0x3))
            s ^= 0x55;
    }
    *sum = s;
}

/* Self-modifying code. A function is "movz w0, #imm; ret". */

typedef int (*gen_fn)(void);

typedef int (*smc_fn)(u32 *, u32);

static u64 call_many(void *code, u64 n)
{
    gen_fn f = (gen_fn)code;
    u64 s = 0;
    for (u64 i = 0; i < n; i++)
        s += (u64)(unsigned)f();
    return s;
}

/* __clear_cache, the routine __builtin___clear_cache calls on arm64. -nostdlib has no
 * libgcc, so it is written here: clean the data cache to the point of unification and
 * invalidate the instruction cache over the range. Lines are 64 bytes, at least the real
 * size, so every line in the range is covered. The engine treats these as no-ops, since
 * its guest caches are coherent. */
void __clear_cache(void *begin, void *end)
{
    uintptr_t a = (uintptr_t)begin & ~(uintptr_t)63;
    for (uintptr_t p = a; p < (uintptr_t)end; p += 64)
        asm volatile("dc cvau, %0" : : "r"(p) : "memory");
    asm volatile("dsb ish" : : : "memory");
    for (uintptr_t p = a; p < (uintptr_t)end; p += 64)
        asm volatile("ic ivau, %0" : : "r"(p) : "memory");
    asm volatile("dsb ish\n\tisb" : : : "memory");
}

static void rewrite_code(void *code, u32 imm)
{
    u32 *w = (u32 *)code;
    w[0] = movz_w0(imm);
    w[1] = RET_INSN;
    __builtin___clear_cache((char *)code, (char *)code + 8);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* ALU, flags, shifts, bitfields, multiplies, divides and memory, from random
     * inputs: the checksum covers every output register and the flags. */
    u64 r[32];
    u64 acc = 0;
    for (u64 i = 0; i < 4000; i++) {
        for (int k = 0; k < 8; k++)
            r[k] = rnd();
        for (int k = 8; k < 32; k++)
            r[k] = rnd();
        alu_kernel(r);
        for (int k = 0; k < 8; k++)
            acc = (acc ^ r[k]) * 0x100000001B3ull;
        acc ^= r[8] + r[31];
    }
    put("alu_acc=");
    putx(acc);
    put("\n");

    u64 bsum = 0;
    hot_branches(&bsum);
    put("branch_sum=");
    putx(bsum);
    put("\n");

    /* 1. Rewrite a hot function in an RWX page. The rewrite is an ordinary store to a
     * page that holds translated code. */
    u8 *rwx = (u8 *)sys_mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    rewrite_code(rwx, 11);
    u64 a = call_many(rwx, CALLS);
    rewrite_code(rwx, 42);
    u64 b = call_many(rwx, CALLS);
    check("rwx_rewrite", a == CALLS * 11 && b == CALLS * 42);
    put("rwx_sums=");
    putd((i64)a);
    put(" ");
    putd((i64)b);
    put("\n");

    /* 2. Rewrite through mprotect: RW, write, RX, run. */
    u8 *mp = (u8 *)sys_mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    rewrite_code(mp, 7);
    sys_mprotect((u64)mp, 4096, PROT_READ | PROT_EXEC);
    u64 c = call_many(mp, CALLS);
    sys_mprotect((u64)mp, 4096, PROT_READ | PROT_WRITE);
    rewrite_code(mp, 99);
    sys_mprotect((u64)mp, 4096, PROT_READ | PROT_EXEC);
    u64 d = call_many(mp, CALLS);
    check("mprotect_rewrite", c == CALLS * 7 && d == CALLS * 99);
    put("mprotect_sums=");
    putd((i64)c);
    put(" ");
    putd((i64)d);
    put("\n");

    /* 3. Unmap and map again at the same address, new code. */
    sys_munmap((u64)mp, 4096);
    u8 *mp2 = (u8 *)sys_mmap((u64)mp, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    rewrite_code(mp2, 5);
    u64 e = call_many(mp2, CALLS);
    sys_munmap((u64)mp2, 4096);
    u8 *mp3 = (u8 *)sys_mmap((u64)mp2, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    rewrite_code(mp3, 123);
    u64 f = call_many(mp3, CALLS);
    check("remap_rewrite", e == CALLS * 5 && f == CALLS * 123);
    put("remap_sums=");
    putd((i64)e);
    put(" ");
    putd((i64)f);
    put("\n");

    /* 4. A fault in a hot function: the handler sees the faulting pc, which must be
     * the load itself, and the function returns the value the handler chose. */
    install_segv();
    u64 fs = 0;
    hot_fault(&fs);
    check("fault_in_hot", faults == 1 && snap_pc == (u64)jitstress_load && snap_addr == 0x40 &&
                              fs == CALLS * 5 + 77);
    put("fault_hits=");
    putd(faults);
    put(" pc_off=");
    putd((i64)(snap_pc - (u64)jitstress_load));
    put(" addr=");
    putx(snap_addr);
    put(" sum=");
    putd((i64)fs);
    put("\n");

    /* Faults in the middle of hot blocks: the load and the store each leave the registers
     * written before them, and the pc of the faulting instruction. */
    static u64 buf[16];
    faults = 0;
    for (u64 i = 0; i < CALLS; i++) {
        jitstress_mid_load(&buf[0]);
        jitstress_mid_store(&buf[1]);
    }
    check("hot_mid_store", buf[1] == 0x77 && faults == 0);
    jitstress_mid_load((u64 *)0x40);
    int ok_load = faults == 1 && snap_pc == (u64)jitstress_mid_load + 16 && snap_addr == 0x40 &&
                  snap_x[0] == 0x40 && snap_x[1] == 42 && snap_x[2] == (u64)-1 && snap_x[3] == 0x1234 &&
                  (snap_pstate >> 28) == 8; /* N: 42 - 43 */
    check("fault_mid_load", ok_load);
    put("mid_load=");
    putd((i64)(snap_pc - (u64)jitstress_mid_load));
    put(" x1=");
    putd((i64)snap_x[1]);
    put(" x2=");
    putd((i64)snap_x[2]);
    put(" x3=");
    putx(snap_x[3]);
    put(" nzcv=");
    putx(snap_pstate >> 28);
    put("\n");
    faults = 0;
    jitstress_mid_store((u64 *)0x40);
    int ok_store = faults == 1 && snap_pc == (u64)jitstress_mid_store + 12 && snap_addr == 0x40 &&
                   snap_x[0] == 0x40 && snap_x[1] == 42 && snap_x[3] == 0x77;
    check("fault_mid_store", ok_store);
    put("mid_store=");
    putd((i64)(snap_pc - (u64)jitstress_mid_store));
    put(" x1=");
    putd((i64)snap_x[1]);
    put(" x3=");
    putx(snap_x[3]);
    put("\n");

    // 5. A translated block that stores into its own page. The store overwrites the
    // movz at word 8, which is the instruction that sets the result. The block must
    // leave after the store, so the new movz runs. The store is followed by the cache
    // maintenance a real core needs before it executes modified code.
    u32 *sm = (u32 *)sys_mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    sm[0] = 0xB9002001u; // str w1, [x0, #32]   (word 8)
    sm[1] = 0x91002002u; // add x2, x0, #32
    sm[2] = 0xD50B7B22u; // dc cvau, x2
    sm[3] = 0xD5033B9Fu; // dsb ish
    sm[4] = 0xD50B7522u; // ic ivau, x2
    sm[5] = 0xD5033B9Fu; // dsb ish
    sm[6] = 0xD5033FDFu; // isb
    sm[7] = 0xD503201Fu; // nop
    sm[8] = movz_w0(1);  // overwritten by the store before it runs
    sm[9] = RET_INSN;
    __builtin___clear_cache((char *)sm, (char *)sm + 40);
    smc_fn sfn = (smc_fn)sm;
    u64 smc_bad = 0;
    for (u64 i = 0; i < CALLS; i++) {
        u32 v = (i & 1) ? 3 : 2;
        if ((u64)(unsigned)sfn(sm, movz_w0(v)) != v)
            smc_bad++;
    }
    check("self_modify", smc_bad == 0);
    put("self_modify_bad=");
    putd((i64)smc_bad);
    put("\n");

    // 5b. Barriers and HINT-space NOPs in a hot loop. Output must match native.
    put("hint_loop=");
    putx(hint_loop(CALLS * 4));
    put("\n");

    // 6. Hot loops that chain through blocks by direct, conditional, indirect and return
    // exits. Output must match native.
    put("chain_loop=");
    putx(chain_loop(12345, CALLS * 8));
    put("\n");

    // 7. A rewrite in the middle of a chained loop: the calls after it must run the new
    // instruction. The sum is exact.
    u64 smc_sum = 0;
    smc_chain(&smc_sum);
    check("smc_chain", smc_sum == (u64)(CALLS / 2) * 2 + (u64)(CALLS - CALLS / 2) * 5);

    // 8. A fault reached through a chain of blocks, handled by the signal handler.
    faults = 0;
    u64 cf_sum = 0;
    chain_fault(&cf_sum);
    check("fault_chained", faults == 1 && snap_addr == 0x40 && snap_pc == (u64)jitstress_chain_load + 12 &&
                               cf_sum == (u64)(2 * CALLS - 1) * 5 + 77);
    put("fault_chained_sum=");
    putd((i64)cf_sum);
    put("\n");

    // 8b. FP and SIMD in hot blocks, with the default FPCR. The output must match native.
    u64 fp_sum = 0;
    fp_hot(&fp_sum);
    put("fpsimd_hot=");
    putx(fp_sum);
    put("\n");

    // 8c. A SIMD load that faults inside a hot block: the handler sees the faulting pc
    // (the load itself) and the address, and the call returns the handler's value.
    faults = 0;
    u64 vs = 0;
    static const u64 vgood[2] = {5, 6};
    for (u64 i = 0; i < CALLS; i++)
        vs += jitstress_vec_load(vgood);
    vs += jitstress_vec_load((const u64 *)0x40);
    check("fault_vec_hot", faults == 1 && snap_pc == (u64)jitstress_vec_load && snap_addr == 0x40 &&
                               vs == CALLS * 5 + 77);
    put("fault_vec_sum=");
    putd((i64)vs);
    put("\n");

    // 8c2. A multi-structure SIMD load (LD1) that faults in a hot block: the fault comes
    // out of the interpreter's code, called from the translated block.
    faults = 0;
    u64 ls = 0;
    for (u64 i = 0; i < CALLS; i++)
        ls += jitstress_ld1_load(vgood);
    ls += jitstress_ld1_load((const u64 *)0x40);
    check("fault_ld1_hot", faults == 1 && snap_pc == (u64)jitstress_ld1_load && snap_addr == 0x40 &&
                               ls == CALLS * 5 + 77);
    put("fault_ld1_sum=");
    putd((i64)ls);
    put("\n");

    // 8e. SIMD&FP loads and stores of every size and index form, in a hot block.
    u64 vm_sum = 0;
    vec_mem_hot(&vm_sum);
    put("vec_mem_hot=");
    putx(vm_sum);
    put("\n");

    // 8d. An unimplemented instruction in a hot block: each call raises SIGILL, the
    // handler skips the instruction, and the function returns 3.
    install_ill();
    ills = 0;
    u64 us = 0;
    for (u64 i = 0; i < CALLS; i++)
        us += jitstress_undef();
    check("unimpl_hot", ills == CALLS && us == (u64)CALLS * 3);
    put("unimpl_sum=");
    putd((i64)us);
    put(" ills=");
    putd((i64)ills);
    put("\n");

    put(checks ? "jitstress=FAIL\n" : "jitstress=ok\n");
    return checks ? 1 : 0;
}
