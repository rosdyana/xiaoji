/* fmov-vec-imm (B4f): FMOV (vector, immediate) in its single-precision (4S, 2S) and double-precision
 * (2D) forms. The boot's SIGILL in system_server was `fmov v0.4s, #0.5` (0x4f03f400) in the ART boot
 * image. The engine did not implement cmode 1111 of the modified-immediate group.
 *
 * Each line is the register after one instruction: its low and high 64-bit halves, in hex. A 2S
 * instruction must clear the high half. The native run (the builder VM's CPU) is the oracle. */
#include "sys.h"

static void show(const char *tag, u64 lo, u64 hi)
{
    put(tag);
    put(" lo=");
    putx(lo);
    put(" hi=");
    putx(hi);
    put("\n");
}

#define CASE(form, imm, sfx)                                  \
    do {                                                      \
        u64 lo, hi;                                           \
        __asm__ volatile("fmov v0." form ", #" imm "\n\t"     \
                         "umov %0, v0.d[0]\n\t"               \
                         "umov %1, v0.d[1]"                   \
                         : "=r"(lo), "=r"(hi)::"v0");         \
        show(form " #" imm sfx, lo, hi);                      \
    } while (0)

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    CASE("4s", "0.5", "");
    CASE("4s", "1.0", "");
    CASE("4s", "2.0", "");
    CASE("4s", "-1.0", "");
    CASE("4s", "0.125", "");
    CASE("4s", "31.0", "");
    CASE("4s", "1.9375", "");
    CASE("4s", "-0.5", "");
    CASE("2s", "0.5", " (high half cleared)");
    CASE("2s", "-2.0", " (high half cleared)");
    CASE("2d", "0.5", "");
    CASE("2d", "-1.0", "");
    CASE("2d", "31.0", "");
    return 0;
}
