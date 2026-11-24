/* simd-insert (B17): SLI (shift left and insert) in every element size, 64- and 128-bit, and the scalar 64-bit forms of
 * SLI, SRI, SSRA, USRA, SRSHR, URSHR, SRSRA and URSRA. The launcher's loader thread died with SIGILL on
 * `sli v0.2d, v1.2d, #32` (0x6f605420, libhwui) because the engine had SHL only, and the scalar group took only SSHR, USHR
 * and SHL. Each line is the destination (low and high halves) after one instruction, which starts with a known value.
 * The native run (the builder VM's CPU) is the oracle. */
#include "sys.h"

static const u64 A[2] = {0x0123456789abcdefull, 0xfedcba9876543210ull};
static const u64 B[2] = {0x8899aabbccddeeffull, 0x1122334455667788ull};

static void show(const char *tag, u64 lo, u64 hi)
{
    put(tag);
    put(" ");
    putx(lo);
    put(":");
    putx(hi);
    put("\n");
}

#define RUN(insn)                                                                     \
    do {                                                                              \
        u64 lo, hi;                                                                   \
        __asm__ volatile("ldr q0, [%2]\n\tldr q1, [%3]\n\t" insn "\n\t"             \
                         "umov %0, v0.d[0]\n\tumov %1, v0.d[1]"                       \
                         : "=r"(lo), "=r"(hi)                                         \
                         : "r"(B), "r"(A)                                             \
                         : "v0", "v1", "memory");                                     \
        show(insn, lo, hi);                                                           \
    } while (0)

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN("sli v0.2d, v1.2d, #32");
    RUN("sli v0.2d, v1.2d, #0");
    RUN("sli v0.2d, v1.2d, #63");
    RUN("sli v0.4s, v1.4s, #5");
    RUN("sli v0.2s, v1.2s, #31");
    RUN("sli v0.8h, v1.8h, #9");
    RUN("sli v0.4h, v1.4h, #1");
    RUN("sli v0.16b, v1.16b, #3");
    RUN("sli v0.8b, v1.8b, #7");
    RUN("sli d0, d1, #12");
    RUN("sri d0, d1, #12");
    RUN("sri d0, d1, #64");
    RUN("ssra d0, d1, #3");
    RUN("usra d0, d1, #63");
    RUN("srshr d0, d1, #5");
    RUN("urshr d0, d1, #64");
    RUN("srsra d0, d1, #17");
    RUN("ursra d0, d1, #1");
    return 0;
}
