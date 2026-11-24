/* simd-hwui (B17): the AdvSIMD and FP forms libhwui uses that the engine lacked, found by running each distinct
 * encoding of the product's libhwui.so natively and on the engine (docs/boot.md, B17): FCVTN and FCVTL with half
 * precision and the scalar FCVT to and from half, FRECPE, UMULL, UMLAL and SMULL by element (and the 2 forms), SQDMULH and
 * SQRDMULH (vector and by element), and SMAXV and SMINV. The operands include the special values (zero, infinity, NaN,
 * subnormals, rounding ties, and the saturating minimum products). Each line is the destination as low and high halves,
 * and QC. The native run (the builder VM's CPU) is the oracle. */
#include "sys.h"

static const u64 F[2] = {0x3f8000003eaaaaabull, 0x7f800000ff800001ull};   /* 1.0, 1/3; +inf, a signalling NaN */
static const u64 G[2] = {0x0000000180000000ull, 0x387fc00047800000ull};   /* a subnormal, -0; near the half
                                                                              subnormal edge, 65536 (overflows half) */
static const u64 T[2] = {0x3f80100040001000ull, 0x33800001c7ffefffull};   /* half ties: 1+2^-11 and 2+2^-11; tiny; a
                                                                              value just under -65520 */
static const u64 H[2] = {0x7c00fc007e017d00ull, 0x000103ff3c00bc00ull};   /* halves: inf, -inf, qNaN, sNaN; subnormals, 1, -1 */
static const u64 D[2] = {0x3ff0000000000000ull, 0x0000000000000001ull};   /* 1.0, the smallest double subnormal */
static const u64 S[2] = {0x80007fff00017ffeull, 0x8000000080000000ull};   /* int16 and int32 extremes */
static const u64 W[2] = {0xfffe0003fff0000full, 0x7fffffff00000005ull};

static void show(const char *tag, u64 lo, u64 hi, u64 fpsr)
{
    put(tag);
    put(" ");
    putx(lo);
    put(":");
    putx(hi);
    put(" qc ");
    putu((fpsr >> 27) & 1);
    put("\n");
}

#define RUN(src0, src1, insn)                                                                    \
    do {                                                                                         \
        u64 lo, hi, f;                                                                           \
        __asm__ volatile("msr fpsr, xzr\n\tldr q0, [%3]\n\tldr q1, [%4]\n\tmovi v2.16b, #0x55\n\t" \
                         insn "\n\tumov %0, v2.d[0]\n\tumov %1, v2.d[1]\n\tmrs %2, fpsr"           \
                         : "=r"(lo), "=r"(hi), "=r"(f)                                           \
                         : "r"(src0), "r"(src1)                                                  \
                         : "v0", "v1", "v2", "memory");                                          \
        show(#src0 "," #src1 " " insn, lo, hi, f);                                               \
    } while (0)

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(F, G, "fcvtn v2.4h, v0.4s");
    RUN(F, G, "fcvtn2 v2.8h, v1.4s");
    RUN(T, G, "fcvtn v2.4h, v0.4s");
    RUN(H, G, "fcvtl v2.4s, v0.4h");
    RUN(H, G, "fcvtl2 v2.4s, v0.8h");
    RUN(H, G, "fcvt s2, h0");
    RUN(H, G, "mov v0.h[0], v0.h[5]\n\tfcvt d2, h0");
    RUN(F, G, "fcvt h2, s0");
    RUN(T, G, "fcvt h2, s0");
    RUN(D, G, "fcvt h2, d0");
    RUN(D, G, "mov v0.d[0], v0.d[1]\n\tfcvt h2, d0");
    RUN(F, G, "frecpe v2.4s, v0.4s");
    RUN(G, F, "frecpe v2.4s, v0.4s");
    RUN(T, F, "frecpe v2.2s, v0.2s");
    RUN(D, F, "frecpe v2.2d, v0.2d");
    RUN(S, W, "umull v2.4s, v0.4h, v1.h[3]");
    RUN(S, W, "umull2 v2.4s, v0.8h, v1.h[6]");
    RUN(S, W, "umull v2.2d, v0.2s, v1.s[1]");
    RUN(S, W, "umlal v2.4s, v0.4h, v1.h[0]");
    RUN(S, W, "umlal2 v2.2d, v0.4s, v1.s[3]");
    RUN(S, W, "smull v2.4s, v0.4h, v1.h[1]");
    RUN(S, W, "smlsl2 v2.2d, v0.4s, v1.s[2]");
    RUN(S, W, "sqdmulh v2.8h, v0.8h, v1.8h");
    RUN(S, S, "sqdmulh v2.8h, v0.8h, v1.8h");
    RUN(S, S, "sqdmulh v2.4s, v0.4s, v1.4s");
    RUN(S, W, "sqrdmulh v2.4h, v0.4h, v1.4h");
    RUN(S, S, "sqrdmulh v2.4s, v0.4s, v1.4s");
    RUN(S, W, "sqdmulh v2.8h, v0.8h, v1.h[2]");
    RUN(S, S, "sqdmulh v2.4s, v0.4s, v1.s[3]");
    RUN(S, W, "sqrdmulh v2.4s, v0.4s, v1.s[0]");
    RUN(S, W, "smaxv s2, v0.4s");
    RUN(S, W, "sminv s2, v1.4s");
    RUN(S, W, "smaxv h2, v0.8h");
    RUN(S, W, "sminv b2, v1.16b");
    return 0;
}
