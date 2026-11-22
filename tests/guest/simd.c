/* AdvSIMD checks. Each case loads fixed inputs into v0..v3 and x5/x6, runs one
 * instruction (or a short sequence), and prints every output register in hex.
 * The output must match the native run exactly, so a wrong encoding or lane
 * semantics shows up as a diff. Two input sets per case; the second one has
 * sign bits and zero lanes set to exercise signed compares and edge cases.
 *
 * Families covered: LD1/ST1 (multiple structures, post-index), DUP (element and
 * general), MOVI/MVNI/ORR/BIC (immediate), CMEQ/CMGT/CMGE/CMHI/CMHS/CMTST (register
 * and zero), AND/BIC/ORR/ORN/EOR/BSL/BIT/BIF, ADD/SUB, ADDP/UMAXP/UMINP,
 * ADDV/UMAXV/UMINV/UADDLV, SHRN/SHRN2, USHR/SSHR/SHL, XTN/XTN2, UZP/TRN/ZIP,
 * EXT, TBL/TBX, REV64, CNT, NOT, INS/UMOV/SMOV, FMOV (general and vector lanes). */
#include "sys.h"

#define NSETS 2

/* v0..v3 as 8 words, then x5 and x6. */
static const u64 sets[NSETS][10] __attribute__((aligned(16))) = {
    {0x0123456789abcdefull, 0xfedcba9876543210ull, 0x80ff7f0100ff00aaull, 0x5577991133557799ull,
     0xf0e1d2c3b4a59687ull, 0x0f1e2d3c4b5a6978ull, 0x3c3cc3c3ffff0000ull, 0x01020304050607f8ull,
     0x8899aabbccddeeffull, 0x1122334455667788ull},
    {0xffffffffffffffffull, 0x0000000000000000ull, 0x8000000000000000ull, 0x7fffffffffffffffull,
     0x00ff00ff00ff00ffull, 0xff00ff00ff00ff00ull, 0x0000000000000001ull, 0xfffffffffffffffeull,
     0x0000000080000000ull, 0x00000000ffffffffull},
};


#define NFSETS 6

/* Floating point inputs. Layout per row: v0, v1, v2, v3 (each a register's
 * low word, then its high word, which is zero), then x5 and x6. Single precision
 * reads the low 32 bits of each low word; double precision reads the whole word.
 * Rows cover NaN (quiet and signalling), infinities, zeros (both signs), denormals,
 * negative fractions, ties (2.5), values at or above 2^63 and 3e9 (above the W
 * saturation point), and equal pairs (v1 == v2 in row 2, +0 == -0 in row 5). */
static const u64 fsets_s[NFSETS][10] __attribute__((aligned(16))) = {
    {0x000000003f800000ull, 0, 0x000000007fc00000ull, 0, 0x000000007f800000ull, 0, 0x0000000000000000ull, 0, 0x8000000000000000ull, 0xffffffffffffffffull},
    {0x00000000c0200000ull, 0, 0x00000000bf000000ull, 0, 0x0000000040200000ull, 0, 0x00000000ff800000ull, 0, 0x0000000000000001ull, 0x7fffffffffffffffull},
    {0x000000003f000000ull, 0, 0x000000004f32d05eull, 0, 0x000000004f32d05eull, 0, 0x0000000080000000ull, 0, 0xfffffffffffffffeull, 0x0000000080000000ull},
    {0x0000000000000001ull, 0, 0x00000000ff800000ull, 0, 0x000000007f800001ull, 0, 0x000000003f800000ull, 0, 0x0000000100000000ull, 0x00000000ffffffffull},
    {0x000000005f000000ull, 0, 0x000000005f000000ull, 0, 0x00000000df000000ull, 0, 0x0000000040600000ull, 0, 0x7fffffffffffffffull, 0x0000000080000001ull},
    {0x00000000bf800000ull, 0, 0x0000000000000000ull, 0, 0x0000000080000000ull, 0, 0x0000000080000001ull, 0, 0x0000000000000000ull, 0x0000000000000001ull},
};

static const u64 fsets_d[NFSETS][10] __attribute__((aligned(16))) = {
    {0x3ff0000000000000ull, 0, 0x7ff8000000000000ull, 0, 0x7ff0000000000000ull, 0, 0x0000000000000000ull, 0, 0x8000000000000000ull, 0xffffffffffffffffull},
    {0xc004000000000000ull, 0, 0xbfe0000000000000ull, 0, 0x4004000000000000ull, 0, 0xfff0000000000000ull, 0, 0x0000000000000001ull, 0x7fffffffffffffffull},
    {0x3fe0000000000000ull, 0, 0x41e65a0bc0000000ull, 0, 0x41e65a0bc0000000ull, 0, 0x8000000000000000ull, 0, 0xfffffffffffffffeull, 0x0000000080000000ull},
    {0x0000000000000001ull, 0, 0xfff0000000000000ull, 0, 0x7ff0000000000001ull, 0, 0x3ff0000000000000ull, 0, 0x0000000100000000ull, 0x00000000ffffffffull},
    {0x43e0000000000000ull, 0, 0x43e0000000000000ull, 0, 0xc3e0000000000000ull, 0, 0x400c000000000000ull, 0, 0x7fffffffffffffffull, 0x0000000080000001ull},
    {0xbff0000000000000ull, 0, 0x0000000000000000ull, 0, 0x8000000000000000ull, 0, 0x8000000000000001ull, 0, 0x0000000000000000ull, 0x0000000000000001ull},
};

static u8 mbuf[64] __attribute__((aligned(16)));

#define LOADV "ldp q0, q1, [%[in]]\n ldp q2, q3, [%[in], #32]\n"
#define LOADG "ldr x5, [%[in], #64]\n ldr x6, [%[in], #72]\n"
#define STOREV "stp q0, q1, [%[out]]\n stp q2, q3, [%[out], #32]\n str x5, [%[out], #64]\n str x6, [%[out], #72]\n"

static void report(const char *name, int s, const u64 *o, int n)
{
    put(name);
    put(" s");
    putu((u64)s);
    for (int i = 0; i < n; i++) {
        put(" ");
        putx(o[i]);
    }
    put("\n");
}

/* One instruction; inputs v0..v3, x5, x6 from one set of the table; outputs all
 * of them. */
#define CASE_N(name, insn, tbl, n)                                                    \
    do {                                                                              \
        for (int s = 0; s < (n); s++) {                                               \
            u64 o[10];                                                                \
            __asm__ volatile(LOADV LOADG insn "\n" STOREV                              \
                             :                                                        \
                             : [in] "r"(tbl[s]), [out] "r"(o)                         \
                             : "memory", "v0", "v1", "v2", "v3", "x5", "x6");        \
            report(name, s, o, 10);                                                   \
        }                                                                             \
    } while (0)
#define CASE(name, insn) CASE_N(name, insn, sets, NSETS)
/* Floating-point cases: single precision reads the low 32 bits of each word,
 * double precision the whole word. */
#define FPS(name, insn) CASE_N(name, insn, fsets_s, NFSETS)
#define FPD(name, insn) CASE_N(name, insn, fsets_d, NFSETS)

/* A memory instruction: v0..v3 from the set, x5 points at mbuf, and mbuf is
 * printed afterwards. x5 is reported relative to mbuf (addresses differ between
 * the native run and the engine). */
#define MEMCASE(name, insn)                                                           \
    do {                                                                              \
        for (int s = 0; s < NSETS; s++) {                                             \
            u64 o[10];                                                                \
            for (int i = 0; i < 64; i++)                                              \
                mbuf[i] = (u8)(i * 7 + 3 + s * 0x55);                                 \
            __asm__ volatile(LOADV "mov x5, %[m]\n" insn "\n"                         \
                             "stp q0, q1, [%[out]]\n stp q2, q3, [%[out], #32]\n"     \
                             "sub x5, x5, %[m]\n str x5, [%[out], #64]\n"             \
                             :                                                        \
                             : [in] "r"(sets[s]), [out] "r"(o), [m] "r"(mbuf)         \
                             : "memory", "v0", "v1", "v2", "v3", "x5", "x6");        \
            report(name, s, o, 9);                                                    \
            u64 mw[8];                                                                \
            for (int i = 0; i < 8; i++) {                                             \
                mw[i] = 0;                                                            \
                for (int b = 0; b < 8; b++)                                           \
                    mw[i] |= (u64)mbuf[i * 8 + b] << (8 * b);                         \
            }                                                                         \
            report(name, s + 100, mw, 8);                                             \
        }                                                                             \
    } while (0)

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* MOVI, MVNI, ORR and BIC (vector, immediate). */
    CASE("movi-8b", "movi v0.8b, #0x12");
    CASE("movi-16b", "movi v0.16b, #0x5a");
    CASE("movi-4s", "movi v0.4s, #0x34, lsl #8");
    CASE("movi-4s-lsl24", "movi v0.4s, #0xab, lsl #24");
    CASE("movi-8h", "movi v0.8h, #0xab, lsl #8");
    CASE("movi-4s-msl8", "movi v0.4s, #0xcd, msl #8");
    CASE("movi-4s-msl16", "movi v0.4s, #0xab, msl #16");
    CASE("movi-2d", "movi v0.2d, #0xff00ff0000ffff00");
    CASE("mvni-4s", "mvni v0.4s, #0x12, lsl #16");
    CASE("mvni-8h", "mvni v0.8h, #0x34");
    CASE("orr-4s", "orr v0.4s, #0x81, lsl #8");
    CASE("orr-4h", "orr v0.4h, #0x07");
    CASE("bic-8h", "bic v0.8h, #0x3c");
    CASE("bic-2s", "bic v0.2s, #0xff, lsl #24");

    /* DUP and INS, UMOV and SMOV, FMOV. */
    CASE("dup-b8", "dup v0.8b, v1.b[3]");
    CASE("dup-b16", "dup v0.16b, v1.b[15]");
    CASE("dup-h4", "dup v0.4h, v1.h[2]");
    CASE("dup-h8", "dup v0.8h, v1.h[7]");
    CASE("dup-s2", "dup v0.2s, v1.s[1]");
    CASE("dup-s4", "dup v0.4s, v1.s[3]");
    CASE("dup-d2", "dup v0.2d, v1.d[1]");
    CASE("dup-gen-b8", "dup v0.8b, w5");
    CASE("dup-gen-b16", "dup v0.16b, w5");
    CASE("dup-gen-h8", "dup v0.8h, w5");
    CASE("dup-gen-s4", "dup v0.4s, w5");
    CASE("dup-gen-d2", "dup v0.2d, x5");
    CASE("ins-elem-b", "ins v0.b[3], v1.b[9]");
    CASE("ins-elem-h", "ins v0.h[2], v1.h[5]");
    CASE("ins-elem-s", "ins v0.s[1], v1.s[2]");
    CASE("ins-elem-d", "ins v0.d[1], v1.d[0]");
    CASE("ins-gen-b", "ins v0.b[5], w5");
    CASE("ins-gen-h", "ins v0.h[1], w5");
    CASE("ins-gen-s", "ins v0.s[2], w5");
    CASE("ins-gen-d", "ins v0.d[1], x5");
    CASE("umov-b", "umov w5, v1.b[7]");
    CASE("umov-h", "umov w5, v1.h[3]");
    CASE("umov-s", "umov w5, v1.s[2]");
    CASE("umov-d", "umov x5, v1.d[1]");
    CASE("smov-b-w", "smov w5, v1.b[7]");
    CASE("smov-b-x", "smov x5, v1.b[15]");
    CASE("smov-h-w", "smov w5, v1.h[1]");
    CASE("smov-h-x", "smov x5, v1.h[6]");
    CASE("smov-s-x", "smov x5, v1.s[3]");
    CASE("fmov-s-w", "fmov s0, w5");
    CASE("fmov-w-s", "fmov w5, s1");
    CASE("fmov-d-x", "fmov d0, x5");
    CASE("fmov-x-d", "fmov x5, d1");
    CASE("fmov-vd1-x", "fmov v0.d[1], x5");
    CASE("fmov-x-vd1", "fmov x5, v1.d[1]");

    /* Bitwise logic on vectors. */
    CASE("and-16b", "and v0.16b, v1.16b, v2.16b");
    CASE("and-8b", "and v0.8b, v1.8b, v2.8b");
    CASE("bic-16b", "bic v0.16b, v1.16b, v2.16b");
    CASE("orr-16b", "orr v0.16b, v1.16b, v2.16b");
    CASE("orn-8b", "orn v0.8b, v1.8b, v2.8b");
    CASE("eor-16b", "eor v0.16b, v1.16b, v2.16b");
    CASE("bsl-16b", "bsl v0.16b, v1.16b, v2.16b");
    CASE("bsl-8b", "bsl v0.8b, v1.8b, v2.8b");
    CASE("bit-16b", "bit v0.16b, v1.16b, v2.16b");
    CASE("bif-16b", "bif v0.16b, v1.16b, v2.16b");
    CASE("bif-8b", "bif v0.8b, v1.8b, v2.8b");

    /* ADD and SUB. */
    CASE("add-16b", "add v0.16b, v1.16b, v2.16b");
    CASE("add-8b", "add v0.8b, v1.8b, v2.8b");
    CASE("add-8h", "add v0.8h, v1.8h, v2.8h");
    CASE("add-4h", "add v0.4h, v1.4h, v2.4h");
    CASE("add-4s", "add v0.4s, v1.4s, v2.4s");
    CASE("add-2s", "add v0.2s, v1.2s, v2.2s");
    CASE("add-2d", "add v0.2d, v1.2d, v2.2d");
    CASE("sub-16b", "sub v0.16b, v1.16b, v2.16b");
    CASE("sub-8h", "sub v0.8h, v1.8h, v2.8h");
    CASE("sub-4s", "sub v0.4s, v1.4s, v2.4s");
    CASE("sub-2d", "sub v0.2d, v1.2d, v2.2d");

    /* Compares, register form. */
    CASE("cmeq-16b", "cmeq v0.16b, v1.16b, v2.16b");
    CASE("cmeq-8h", "cmeq v0.8h, v1.8h, v2.8h");
    CASE("cmeq-4s", "cmeq v0.4s, v1.4s, v2.4s");
    CASE("cmeq-2d", "cmeq v0.2d, v1.2d, v2.2d");
    CASE("cmgt-16b", "cmgt v0.16b, v1.16b, v2.16b");
    CASE("cmgt-8b", "cmgt v0.8b, v1.8b, v2.8b");
    CASE("cmgt-8h", "cmgt v0.8h, v1.8h, v2.8h");
    CASE("cmgt-4s", "cmgt v0.4s, v1.4s, v2.4s");
    CASE("cmgt-2d", "cmgt v0.2d, v1.2d, v2.2d");
    CASE("cmge-16b", "cmge v0.16b, v1.16b, v2.16b");
    CASE("cmge-8h", "cmge v0.8h, v1.8h, v2.8h");
    CASE("cmge-4s", "cmge v0.4s, v1.4s, v2.4s");
    CASE("cmge-2d", "cmge v0.2d, v1.2d, v2.2d");
    CASE("cmhi-16b", "cmhi v0.16b, v1.16b, v2.16b");
    CASE("cmhi-8h", "cmhi v0.8h, v1.8h, v2.8h");
    CASE("cmhi-4s", "cmhi v0.4s, v1.4s, v2.4s");
    CASE("cmhi-2d", "cmhi v0.2d, v1.2d, v2.2d");
    CASE("cmhs-16b", "cmhs v0.16b, v1.16b, v2.16b");
    CASE("cmhs-8h", "cmhs v0.8h, v1.8h, v2.8h");
    CASE("cmhs-4s", "cmhs v0.4s, v1.4s, v2.4s");
    CASE("cmhs-2d", "cmhs v0.2d, v1.2d, v2.2d");
    CASE("cmtst-16b", "cmtst v0.16b, v1.16b, v2.16b");
    CASE("cmtst-8h", "cmtst v0.8h, v1.8h, v2.8h");
    CASE("cmtst-4s", "cmtst v0.4s, v1.4s, v2.4s");
    CASE("cmtst-2d", "cmtst v0.2d, v1.2d, v2.2d");

    /* Compares against zero. */
    CASE("cmeq0-16b", "cmeq v0.16b, v1.16b, #0");
    CASE("cmeq0-4s", "cmeq v0.4s, v1.4s, #0");
    CASE("cmge0-16b", "cmge v0.16b, v1.16b, #0");
    CASE("cmge0-8h", "cmge v0.8h, v1.8h, #0");
    CASE("cmgt0-16b", "cmgt v0.16b, v1.16b, #0");
    CASE("cmgt0-2d", "cmgt v0.2d, v1.2d, #0");
    CASE("cmle0-16b", "cmle v0.16b, v1.16b, #0");
    CASE("cmle0-4s", "cmle v0.4s, v1.4s, #0");
    CASE("cmlt0-16b", "cmlt v0.16b, v1.16b, #0");
    CASE("cmlt0-8h", "cmlt v0.8h, v1.8h, #0");

    /* Pairwise: ADDP, UMAXP, UMINP. */
    CASE("addp-16b", "addp v0.16b, v1.16b, v2.16b");
    CASE("addp-8h", "addp v0.8h, v1.8h, v2.8h");
    CASE("addp-4s", "addp v0.4s, v1.4s, v2.4s");
    CASE("addp-2d", "addp v0.2d, v1.2d, v2.2d");
    CASE("addp-8b", "addp v0.8b, v1.8b, v2.8b");
    CASE("umaxp-16b", "umaxp v0.16b, v1.16b, v2.16b");
    CASE("umaxp-4h", "umaxp v0.4h, v1.4h, v2.4h");
    CASE("umaxp-4s", "umaxp v0.4s, v1.4s, v2.4s");
    CASE("uminp-16b", "uminp v0.16b, v1.16b, v2.16b");
    CASE("uminp-8h", "uminp v0.8h, v1.8h, v2.8h");
    CASE("uminp-2s", "uminp v0.2s, v1.2s, v2.2s");

    /* Across lanes. */
    CASE("umaxv-b16", "umaxv b0, v1.16b");
    CASE("umaxv-b8", "umaxv b0, v1.8b");
    CASE("umaxv-h8", "umaxv h0, v1.8h");
    CASE("umaxv-h4", "umaxv h0, v1.4h");
    CASE("umaxv-s4", "umaxv s0, v1.4s");
    CASE("uminv-b16", "uminv b0, v1.16b");
    CASE("uminv-h8", "uminv h0, v1.8h");
    CASE("uminv-s4", "uminv s0, v1.4s");
    CASE("addv-b16", "addv b0, v1.16b");
    CASE("addv-b8", "addv b0, v1.8b");
    CASE("addv-h8", "addv h0, v1.8h");
    CASE("addv-h4", "addv h0, v1.4h");
    CASE("addv-s4", "addv s0, v1.4s");
    CASE("uaddlv-b8", "uaddlv h0, v1.8b");
    CASE("uaddlv-b16", "uaddlv h0, v1.16b");
    CASE("uaddlv-h8", "uaddlv s0, v1.8h");
    CASE("uaddlv-s4", "uaddlv d0, v1.4s");

    /* Shifts by immediate, and narrowing. */
    CASE("ushr-16b", "ushr v0.16b, v1.16b, #3");
    CASE("sshr-16b", "sshr v0.16b, v1.16b, #3");
    CASE("ushr-8b", "ushr v0.8b, v1.8b, #8");
    CASE("sshr-8h", "sshr v0.8h, v1.8h, #15");
    CASE("ushr-4s", "ushr v0.4s, v1.4s, #31");
    CASE("sshr-4s", "sshr v0.4s, v1.4s, #32");
    CASE("ushr-2d", "ushr v0.2d, v1.2d, #64");
    CASE("sshr-2d", "sshr v0.2d, v1.2d, #1");
    CASE("shl-16b", "shl v0.16b, v1.16b, #7");
    CASE("shl-8h", "shl v0.8h, v1.8h, #15");
    CASE("shl-4s", "shl v0.4s, v1.4s, #31");
    CASE("shl-2d", "shl v0.2d, v1.2d, #63");
    CASE("shrn-8b", "shrn v0.8b, v1.8h, #3");
    CASE("shrn2-16b", "shrn2 v0.16b, v1.8h, #5");
    CASE("shrn-4h", "shrn v0.4h, v1.4s, #7");
    CASE("shrn2-8h", "shrn2 v0.8h, v1.4s, #16");
    CASE("shrn-2s", "shrn v0.2s, v1.2d, #20");
    CASE("shrn2-4s", "shrn2 v0.4s, v1.2d, #32");
    CASE("xtn-8b", "xtn v0.8b, v1.8h");
    CASE("xtn2-16b", "xtn2 v0.16b, v1.8h");
    CASE("xtn-4h", "xtn v0.4h, v1.4s");
    CASE("xtn2-8h", "xtn2 v0.8h, v1.4s");
    CASE("xtn-2s", "xtn v0.2s, v1.2d");
    CASE("xtn2-4s", "xtn2 v0.4s, v1.2d");

    /* Permutes. */
    CASE("uzp1-16b", "uzp1 v0.16b, v1.16b, v2.16b");
    CASE("uzp2-16b", "uzp2 v0.16b, v1.16b, v2.16b");
    CASE("uzp1-8b", "uzp1 v0.8b, v1.8b, v2.8b");
    CASE("uzp2-8h", "uzp2 v0.8h, v1.8h, v2.8h");
    CASE("uzp1-4s", "uzp1 v0.4s, v1.4s, v2.4s");
    CASE("uzp2-2d", "uzp2 v0.2d, v1.2d, v2.2d");
    CASE("trn1-16b", "trn1 v0.16b, v1.16b, v2.16b");
    CASE("trn2-16b", "trn2 v0.16b, v1.16b, v2.16b");
    CASE("trn1-8h", "trn1 v0.8h, v1.8h, v2.8h");
    CASE("trn2-4s", "trn2 v0.4s, v1.4s, v2.4s");
    CASE("trn1-2d", "trn1 v0.2d, v1.2d, v2.2d");
    CASE("zip1-16b", "zip1 v0.16b, v1.16b, v2.16b");
    CASE("zip2-16b", "zip2 v0.16b, v1.16b, v2.16b");
    CASE("zip1-8b", "zip1 v0.8b, v1.8b, v2.8b");
    CASE("zip2-8h", "zip2 v0.8h, v1.8h, v2.8h");
    CASE("zip1-4s", "zip1 v0.4s, v1.4s, v2.4s");
    CASE("zip2-2d", "zip2 v0.2d, v1.2d, v2.2d");
    CASE("ext-16b-5", "ext v0.16b, v1.16b, v2.16b, #5");
    CASE("ext-16b-0", "ext v0.16b, v1.16b, v2.16b, #0");
    CASE("ext-16b-15", "ext v0.16b, v1.16b, v2.16b, #15");
    CASE("ext-8b-3", "ext v0.8b, v1.8b, v2.8b, #3");

    /* Table lookup. */
    CASE("tbl-1r-16b", "tbl v0.16b, {v1.16b}, v3.16b");
    CASE("tbl-1r-8b", "tbl v0.8b, {v1.16b}, v3.8b");
    CASE("tbl-2r-16b", "tbl v0.16b, {v1.16b, v2.16b}, v3.16b");
    CASE("tbl-2r-8b", "tbl v0.8b, {v1.16b, v2.16b}, v3.8b");
    CASE("tbx-1r-16b", "tbx v0.16b, {v1.16b}, v3.16b");
    CASE("tbx-2r-8b", "tbx v0.8b, {v1.16b, v2.16b}, v3.8b");

    /* Reverse, count and not. */
    CASE("rev64-16b", "rev64 v0.16b, v1.16b");
    CASE("rev64-8b", "rev64 v0.8b, v1.8b");
    CASE("rev64-8h", "rev64 v0.8h, v1.8h");
    CASE("rev64-4h", "rev64 v0.4h, v1.4h");
    CASE("rev64-4s", "rev64 v0.4s, v1.4s");
    CASE("rev64-2s", "rev64 v0.2s, v1.2s");
    CASE("cnt-16b", "cnt v0.16b, v1.16b");
    CASE("cnt-8b", "cnt v0.8b, v1.8b");
    CASE("not-16b", "not v0.16b, v1.16b");
    CASE("not-8b", "not v0.8b, v1.8b");

    /* Loads and stores, multiple structures, with and without post-index. */
    MEMCASE("ld1-1r-16b", "ld1 {v0.16b}, [x5]");
    MEMCASE("ld1-3r-8b", "ld1 {v0.8b, v1.8b, v2.8b}, [x5]");
    MEMCASE("ld1-2r-post-imm", "ld1 {v0.16b, v1.16b}, [x5], #32");
    MEMCASE("ld1-4r-post-imm", "ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x5], #64");
    MEMCASE("ld1-2r-post-reg", "mov x6, #16\n ld1 {v0.8h, v1.8h}, [x5], x6");
    MEMCASE("st1-1r-16b", "st1 {v0.16b}, [x5]");
    MEMCASE("st1-3r-8b", "st1 {v0.8b, v1.8b, v2.8b}, [x5]");
    MEMCASE("st1-2r-post-imm", "st1 {v0.16b, v1.16b}, [x5], #32");
    MEMCASE("st1-4r-post-imm", "st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [x5], #64");


    /* Scalar floating point: arithmetic with NaN, infinity, zero and denormal
     * inputs, in single and double precision. */
    FPS("fmul-s", "fmul s0, s1, s2");
    FPS("fdiv-s", "fdiv s0, s1, s2");
    FPS("fdiv-s-rev", "fdiv s0, s2, s1");
    FPS("fadd-s", "fadd s0, s1, s2");
    FPS("fsub-s", "fsub s0, s1, s2");
    FPD("fmul-d", "fmul d0, d1, d2");
    FPD("fdiv-d", "fdiv d0, d1, d2");
    FPD("fdiv-d-rev", "fdiv d0, d2, d1");
    FPD("fadd-d", "fadd d0, d1, d2");
    FPD("fsub-d", "fsub d0, d1, d2");

    /* Integer to floating point (SCVTF, UCVTF), round to nearest. */
    CASE("ucvtf-s-w", "ucvtf s0, w5");
    CASE("ucvtf-s-x", "ucvtf s0, x5");
    CASE("scvtf-s-w", "scvtf s0, w5");
    CASE("scvtf-s-x", "scvtf s0, x5");
    CASE("ucvtf-d-w", "ucvtf d0, w5");
    CASE("ucvtf-d-x", "ucvtf d0, x5");
    CASE("scvtf-d-w", "scvtf d0, w5");
    CASE("scvtf-d-x", "scvtf d0, x5");

    /* Floating point to integer, each rounding mode, saturating. */
    FPS("fcvtns-w", "fcvtns w5, s1");
    FPS("fcvtnu-x", "fcvtnu x5, s1");
    FPS("fcvtps-x", "fcvtps x5, s1");
    FPS("fcvtpu-x", "fcvtpu x5, s1");
    FPS("fcvtpu-w", "fcvtpu w5, s1");
    FPS("fcvtms-w", "fcvtms w5, s1");
    FPS("fcvtmu-x", "fcvtmu x5, s1");
    FPS("fcvtzs-w", "fcvtzs w5, s1");
    FPS("fcvtzs-x", "fcvtzs x5, s1");
    FPS("fcvtzu-w", "fcvtzu w5, s1");
    FPS("fcvtzu-x", "fcvtzu x5, s1");
    FPD("fcvtns-d-x", "fcvtns x5, d1");
    FPD("fcvtnu-d-w", "fcvtnu w5, d1");
    FPD("fcvtps-d-w", "fcvtps w5, d1");
    FPD("fcvtpu-d-x", "fcvtpu x5, d1");
    FPD("fcvtms-d-x", "fcvtms x5, d1");
    FPD("fcvtmu-d-w", "fcvtmu w5, d1");
    FPD("fcvtzs-d-w", "fcvtzs w5, d1");
    FPD("fcvtzu-d-x", "fcvtzu x5, d1");

    /* Scalar compares: the NZCV result is read back into x6. */
    FPS("fcmp-s", "fcmp s1, s2\n mrs x6, nzcv");
    FPS("fcmp-s-zero", "fcmp s1, #0.0\n mrs x6, nzcv");
    FPD("fcmp-d", "fcmp d1, d2\n mrs x6, nzcv");
    FPD("fcmp-d-zero", "fcmp d1, #0.0\n mrs x6, nzcv");
    FPS("fcmpe-s", "fcmpe s1, s2\n mrs x6, nzcv");
    FPS("fcmpe-s-zero", "fcmpe s1, #0.0\n mrs x6, nzcv");
    FPD("fcmpe-d", "fcmpe d1, d2\n mrs x6, nzcv");
    FPD("fcmpe-d-zero", "fcmpe d1, #0.0\n mrs x6, nzcv");

    /* Shift left long: UXTL, SXTL and their 2 forms. */
    CASE("ushll-8h", "ushll v0.8h, v1.8b, #0");
    CASE("uxtl-4s", "uxtl v0.4s, v1.4h");
    CASE("uxtl2-2d", "uxtl2 v0.2d, v1.4s");
    CASE("sxtl-2d", "sxtl v0.2d, v1.2s");
    CASE("sxtl2-8h", "sxtl2 v0.8h, v1.16b");
    CASE("ushll2-4s", "ushll2 v0.4s, v1.8h, #3");
    CASE("sshll-4s", "sshll v0.4s, v1.4h, #7");

    /* Multiply, multiply-accumulate and widening multiply-accumulate. */
    CASE("mul-16b", "mul v0.16b, v1.16b, v2.16b");
    CASE("mul-8b", "mul v0.8b, v1.8b, v2.8b");
    CASE("mul-8h", "mul v0.8h, v1.8h, v2.8h");
    CASE("mul-4s", "mul v0.4s, v1.4s, v2.4s");
    CASE("mla-4s", "mla v0.4s, v1.4s, v2.4s");
    CASE("mla-4h", "mla v0.4h, v1.4h, v2.4h");
    CASE("mls-16b", "mls v0.16b, v1.16b, v2.16b");
    CASE("umlal-8h", "umlal v0.8h, v1.8b, v2.8b");
    CASE("umlal2-8h", "umlal2 v0.8h, v1.16b, v2.16b");
    CASE("umlal-4s", "umlal v0.4s, v1.4h, v2.4h");
    CASE("umlal2-4s", "umlal2 v0.4s, v1.8h, v2.8h");
    CASE("umlal-2d", "umlal v0.2d, v1.2s, v2.2s");
    CASE("umlal2-2d", "umlal2 v0.2d, v1.4s, v2.4s");
    CASE("pmull-8h", "pmull v0.8h, v1.8b, v2.8b");
    CASE("pmull2-8h", "pmull2 v0.8h, v1.16b, v2.16b");

    /* Pairwise add long. */
    CASE("uaddlp-8h", "uaddlp v0.8h, v1.16b");
    CASE("uaddlp-4h", "uaddlp v0.4h, v1.8b");
    CASE("uaddlp-2d", "uaddlp v0.2d, v1.4s");
    CASE("saddlp-8h", "saddlp v0.8h, v1.16b");
    CASE("saddlp-4s", "saddlp v0.4s, v1.8h");

    /* Signed and unsigned maximum and minimum. */
    CASE("smax-16b", "smax v0.16b, v1.16b, v2.16b");
    CASE("smin-8h", "smin v0.8h, v1.8h, v2.8h");
    CASE("umax-4s", "umax v0.4s, v1.4s, v2.4s");
    CASE("umin-8b", "umin v0.8b, v1.8b, v2.8b");

    /* CRC32 and CRC32C, all widths. */
    CASE("crc32b", ".arch_extension crc\n crc32b w5, w5, w6");
    CASE("crc32h", ".arch_extension crc\n crc32h w5, w5, w6");
    CASE("crc32w", ".arch_extension crc\n crc32w w5, w5, w6");
    CASE("crc32x", ".arch_extension crc\n crc32x w5, w5, x6");
    CASE("crc32cb", ".arch_extension crc\n crc32cb w5, w5, w6");
    CASE("crc32ch", ".arch_extension crc\n crc32ch w5, w5, w6");
    CASE("crc32cw", ".arch_extension crc\n crc32cw w5, w5, w6");
    CASE("crc32cx", ".arch_extension crc\n crc32cx w5, w5, x6");

    /* LD1 and ST1 of one lane, and LD1R (replicate), with post-index. */
    MEMCASE("ld1-lane-s", "ld1 {v0.s}[0], [x5]");
    MEMCASE("ld1-lane-b", "ld1 {v0.b}[5], [x5]");
    MEMCASE("ld1-lane-h-post", "ld1 {v0.h}[3], [x5], #2");
    MEMCASE("ld1-lane-d", "ld1 {v0.d}[1], [x5]");
    MEMCASE("st1-lane-s", "st1 {v0.s}[1], [x5]");
    MEMCASE("st1-lane-b", "st1 {v0.b}[9], [x5]");
    MEMCASE("ld1r-8b", "ld1r {v0.8b}, [x5]");
    MEMCASE("ld1r-4s", "ld1r {v0.4s}, [x5]");
    MEMCASE("ld1r-2d", "ld1r {v0.2d}, [x5]");
    MEMCASE("ld1r-16b-post", "ld1r {v0.16b}, [x5], #1");
    MEMCASE("ld1r-8h-post", "ld1r {v0.8h}, [x5], #2");

    put("simd done\n");
    return 0;
}
