/* simd-sat (B17): the saturating vector add and subtract, SQADD, UQADD, SQSUB and UQSUB, in every arrangement, and the
 * cumulative saturation bit (FPSR.QC). The launcher's loader thread died with SIGILL on `uqsub v0.8b, v0.8b, v1.8b`
 * (0x2e212c00, libhwui) in a browser boot. Each line is the result (low and high halves) and QC after one instruction,
 * which starts with QC clear. The operands mix values that saturate and values that do not. The native run (the builder
 * VM's CPU) is the oracle. */
#include "sys.h"

static const u64 A[2] = {0x7f80017ffe0280ffull, 0x8000000000000001ull};
static const u64 B[2] = {0x0181ff80027f80ffull, 0x7fffffffffffffffull};

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

#define RUN(insn)                                                                        \
    do {                                                                                 \
        u64 lo, hi, f;                                                                   \
        __asm__ volatile("msr fpsr, xzr\n\tldr q0, [%3]\n\tldr q1, [%4]\n\t" insn "\n\t" \
                         "umov %0, v2.d[0]\n\tumov %1, v2.d[1]\n\tmrs %2, fpsr"          \
                         : "=r"(lo), "=r"(hi), "=r"(f)                                   \
                         : "r"(A), "r"(B)                                                \
                         : "v0", "v1", "v2", "memory");                                  \
        show(insn, lo, hi, f);                                                           \
    } while (0)

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN("uqsub v2.8b, v0.8b, v1.8b");
    RUN("uqsub v2.16b, v0.16b, v1.16b");
    RUN("uqsub v2.4h, v1.4h, v0.4h");
    RUN("uqsub v2.4s, v0.4s, v1.4s");
    RUN("uqsub v2.2d, v0.2d, v1.2d");
    RUN("uqadd v2.8b, v0.8b, v1.8b");
    RUN("uqadd v2.8h, v0.8h, v1.8h");
    RUN("uqadd v2.2s, v0.2s, v1.2s");
    RUN("uqadd v2.2d, v0.2d, v1.2d");
    RUN("sqadd v2.16b, v0.16b, v1.16b");
    RUN("sqadd v2.4h, v0.4h, v1.4h");
    RUN("sqadd v2.4s, v0.4s, v1.4s");
    RUN("sqadd v2.2d, v0.2d, v1.2d");
    RUN("sqsub v2.8b, v0.8b, v1.8b");
    RUN("sqsub v2.8h, v1.8h, v0.8h");
    RUN("sqsub v2.2s, v0.2s, v1.2s");
    RUN("sqsub v2.2d, v0.2d, v1.2d");
    RUN("sqsub v2.2d, v1.2d, v0.2d");
    RUN("uqadd v2.4h, v0.4h, v0.4h");
    return 0;
}
