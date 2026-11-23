/* fabd-scalar (G1): FABD (scalar, single and double precision), the instruction SurfaceFlinger's boot executed
 * (`fabd s0, s0, s1`, 0x7ea1d400, in its onComposerHalHotplug path) and the engine did not implement (SIGILL).
 * The scalar floating-point three-same group is the same arithmetic as the vector form on lane 0, with the rest
 * of the register zeroed. Each line is the result's bits, and the native run (the builder VM's CPU) is the oracle. */
#include "sys.h"

static u64 fabd_s(float a, float b)
{
    u64 out;
    float ra = a, rb = b;
    __asm__ volatile("fmov s0, %w1\n\t"
                     "fmov s1, %w2\n\t"
                     "fabd s0, s0, s1\n\t"
                     "fmov %w0, s0"
                     : "=r"(out)
                     : "r"(*(u32 *)&ra), "r"(*(u32 *)&rb)
                     : "v0", "v1");
    return out & 0xffffffffu;
}

static u64 fabd_d(double a, double b)
{
    u64 out, ra = *(u64 *)&a, rb = *(u64 *)&b;
    __asm__ volatile("fmov d0, %1\n\t"
                     "fmov d1, %2\n\t"
                     "fabd d0, d0, d1\n\t"
                     "fmov %0, d0"
                     : "=r"(out)
                     : "r"(ra), "r"(rb)
                     : "v0", "v1");
    return out;
}

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
    line("s 1.5 -2.25", fabd_s(1.5f, -2.25f));
    line("s -2.25 1.5", fabd_s(-2.25f, 1.5f));
    line("s 7 7", fabd_s(7.0f, 7.0f));
    line("s -0 0", fabd_s(-0.0f, 0.0f));
    line("s 1e-30 -1e30", fabd_s(1e-30f, -1e30f));
    line("d 1.5 -2.25", fabd_d(1.5, -2.25));
    line("d -2.25 1.5", fabd_d(-2.25, 1.5));
    line("d 3 3", fabd_d(3.0, 3.0));
    line("d -0 0", fabd_d(-0.0, 0.0));
    line("d 1e-300 -1e300", fabd_d(1e-300, -1e300));
    return 0;
}
