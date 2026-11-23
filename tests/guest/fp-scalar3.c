/* fp-scalar3 (G1): the other scalar floating-point three-same instructions the engine decodes (engine/simd.c,
 * scalar_fp3_allocated): FMULX, FCMEQ, FCMGE, FCMGT, FACGE, FACGT, FRECPS and FRSQRTS, single precision.
 * Each line is the result's bits. The native run (the builder VM's CPU) is the oracle. */
#include "sys.h"

#define OP2(name, insn)                                                   \
    static u64 name(float a, float b)                                     \
    {                                                                     \
        u64 out;                                                          \
        u32 ra = *(u32 *)&a, rb = *(u32 *)&b;                             \
        __asm__ volatile("fmov s0, %w1\n\t"                               \
                         "fmov s1, %w2\n\t" insn "\n\t"                   \
                         "fmov %w0, s0"                                   \
                         : "=r"(out)                                      \
                         : "r"(ra), "r"(rb)                               \
                         : "v0", "v1");                                   \
        return out & 0xffffffffu;                                         \
    }

OP2(fmulx_s, "fmulx s0, s0, s1")
OP2(fcmeq_s, "fcmeq s0, s0, s1")
OP2(fcmge_s, "fcmge s0, s0, s1")
OP2(fcmgt_s, "fcmgt s0, s0, s1")
OP2(facge_s, "facge s0, s0, s1")
OP2(facgt_s, "facgt s0, s0, s1")
OP2(frecps_s, "frecps s0, s0, s1")
OP2(frsqrts_s, "frsqrts s0, s0, s1")

static void line(const char *tag, u64 bits)
{
    put(tag);
    put(" ");
    putx(bits);
    put("\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    float inf = 1.0f / 0.0f, zero = 0.0f;
    line("fmulx 2 3", fmulx_s(2.0f, 3.0f));
    line("fmulx 0 inf", fmulx_s(zero, inf));
    line("fmulx -0 inf", fmulx_s(-zero, inf));
    line("fcmeq 2 2", fcmeq_s(2.0f, 2.0f));
    line("fcmeq 2 3", fcmeq_s(2.0f, 3.0f));
    line("fcmge 3 2", fcmge_s(3.0f, 2.0f));
    line("fcmge 2 3", fcmge_s(2.0f, 3.0f));
    line("fcmgt 3 2", fcmgt_s(3.0f, 2.0f));
    line("fcmgt 2 2", fcmgt_s(2.0f, 2.0f));
    line("facge -3 2", facge_s(-3.0f, 2.0f));
    line("facge 2 -3", facge_s(2.0f, -3.0f));
    line("facgt -3 2", facgt_s(-3.0f, 2.0f));
    line("facgt 2 2", facgt_s(2.0f, 2.0f));
    line("frecps 2 3", frecps_s(2.0f, 3.0f));
    line("frsqrts 2 3", frsqrts_s(2.0f, 3.0f));
    return 0;
}
