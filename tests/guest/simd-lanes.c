/* simd-lanes (B17): the AdvSIMD load/store single structure group. system_server's UI thread died with SIGILL on
 * `st4 {v0.s-v3.s}[0], [x8]` (0x0d20a100, libhwui) because the engine had only LD1/ST1 lanes and LD1R. This covers
 * LD1-LD4 and ST1-ST4 lanes for B, H, S and D elements (low and high lanes, Q 0 and 1 encodings), LD1R-LD4R in the 64-
 * and 128-bit forms (the 64-bit form clears the high half), register wrap-around (v31 to v0), and both post-index
 * forms (the immediate, which is selem times the element size, and a register).
 *
 * Each line is v0..v3 (or the registers named) as low and high 64-bit halves, or the bytes of the memory written,
 * after one instruction. The native run (the builder VM's CPU) is the oracle. */
#include "sys.h"

static u8 src[64];
static u8 dst[64];

static void hex64(u64 v) { putx(v); }

static void show_regs(const char *tag, const u64 r[][2], int n)
{
    put(tag);
    for (int i = 0; i < n; i++) {
        put(" ");
        hex64(r[i][0]);
        put(":");
        hex64(r[i][1]);
    }
    put("\n");
}

static void show_mem(const char *tag, int n)
{
    put(tag);
    put(" mem");
    for (int i = 0; i < n; i++) {
        static const char h[] = "0123456789abcdef";
        char b[3] = {h[dst[i] >> 4], h[dst[i] & 15], 0};
        put(" ");
        put(b);
    }
    put("\n");
}

/* Fills v0-v3 (and v31) with a known pattern, then runs one load and stores v0-v3 into out. */
#define FILL                                                                                  \
    "movi v0.16b, #0xa0\n\tmovi v1.16b, #0xa1\n\tmovi v2.16b, #0xa2\n\tmovi v3.16b, #0xa3\n\t" \
    "movi v31.16b, #0xbf\n\t"

#define LOAD(insn, tag)                                                                           \
    do {                                                                                          \
        u64 r[4][2];                                                                              \
        __asm__ volatile(FILL "mov x8, %1\n\t" insn "\n\t"                                       \
                         "st1 {v0.2d, v1.2d, v2.2d, v3.2d}, [%0]"                                 \
                         :                                                                        \
                         : "r"(r), "r"(src)                                                       \
                         : "x8", "v0", "v1", "v2", "v3", "v31", "memory");                        \
        show_regs(tag, (const u64(*)[2])r, 4);                                                    \
    } while (0)

/* Loads v0-v3 from src as whole registers, clears dst, runs one store to dst. */
#define STORE(insn, tag, n)                                                                       \
    do {                                                                                          \
        memset(dst, 0, sizeof dst);                                                               \
        __asm__ volatile("ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%1]\n\t"                         \
                         "mov x8, %0\n\t" insn                                                    \
                         :                                                                        \
                         : "r"(dst), "r"(src)                                                     \
                         : "x8", "v0", "v1", "v2", "v3", "memory");                               \
        show_mem(tag, n);                                                                         \
    } while (0)

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (int i = 0; i < 64; i++)
        src[i] = (u8)(0x10 + i * 3);

    /* Lane loads: the other lanes keep the FILL pattern. */
    LOAD("ld1 {v0.b}[0], [x8]", "ld1.b[0]");
    LOAD("ld1 {v0.b}[15], [x8]", "ld1.b[15]");
    LOAD("ld2 {v0.b, v1.b}[9], [x8]", "ld2.b[9]");
    LOAD("ld3 {v0.b, v1.b, v2.b}[3], [x8]", "ld3.b[3]");
    LOAD("ld4 {v0.b, v1.b, v2.b, v3.b}[14], [x8]", "ld4.b[14]");
    LOAD("ld1 {v0.h}[7], [x8]", "ld1.h[7]");
    LOAD("ld2 {v0.h, v1.h}[2], [x8]", "ld2.h[2]");
    LOAD("ld3 {v0.h, v1.h, v2.h}[5], [x8]", "ld3.h[5]");
    LOAD("ld4 {v0.h, v1.h, v2.h, v3.h}[0], [x8]", "ld4.h[0]");
    LOAD("ld1 {v0.s}[3], [x8]", "ld1.s[3]");
    LOAD("ld2 {v0.s, v1.s}[1], [x8]", "ld2.s[1]");
    LOAD("ld3 {v0.s, v1.s, v2.s}[2], [x8]", "ld3.s[2]");
    LOAD("ld4 {v0.s, v1.s, v2.s, v3.s}[0], [x8]", "ld4.s[0]");
    LOAD("ld4 {v0.s, v1.s, v2.s, v3.s}[3], [x8]", "ld4.s[3]");
    LOAD("ld1 {v0.d}[1], [x8]", "ld1.d[1]");
    LOAD("ld2 {v0.d, v1.d}[0], [x8]", "ld2.d[0]");
    LOAD("ld3 {v0.d, v1.d, v2.d}[1], [x8]", "ld3.d[1]");
    LOAD("ld4 {v0.d, v1.d, v2.d, v3.d}[1], [x8]", "ld4.d[1]");

    /* Replicate: Q 0 clears the high half. */
    LOAD("ld1r {v0.8b}, [x8]", "ld1r.8b");
    LOAD("ld1r {v0.4s}, [x8]", "ld1r.4s");
    LOAD("ld2r {v0.4h, v1.4h}, [x8]", "ld2r.4h");
    LOAD("ld2r {v0.2d, v1.2d}, [x8]", "ld2r.2d");
    LOAD("ld3r {v0.16b, v1.16b, v2.16b}, [x8]", "ld3r.16b");
    LOAD("ld3r {v0.2s, v1.2s, v2.2s}, [x8]", "ld3r.2s");
    LOAD("ld4r {v0.8h, v1.8h, v2.8h, v3.8h}, [x8]", "ld4r.8h");
    LOAD("ld4r {v0.1d, v1.1d, v2.1d, v3.1d}, [x8]", "ld4r.1d");

    /* Lane stores: only selem elements are written. */
    STORE("st1 {v0.b}[13], [x8]", "st1.b[13]", 4);
    STORE("st2 {v0.b, v1.b}[1], [x8]", "st2.b[1]", 4);
    STORE("st3 {v0.b, v1.b, v2.b}[15], [x8]", "st3.b[15]", 4);
    STORE("st4 {v0.b, v1.b, v2.b, v3.b}[6], [x8]", "st4.b[6]", 6);
    STORE("st1 {v0.h}[3], [x8]", "st1.h[3]", 4);
    STORE("st2 {v0.h, v1.h}[6], [x8]", "st2.h[6]", 6);
    STORE("st3 {v0.h, v1.h, v2.h}[1], [x8]", "st3.h[1]", 8);
    STORE("st4 {v0.h, v1.h, v2.h, v3.h}[7], [x8]", "st4.h[7]", 10);
    STORE("st1 {v0.s}[2], [x8]", "st1.s[2]", 6);
    STORE("st2 {v0.s, v1.s}[3], [x8]", "st2.s[3]", 10);
    STORE("st3 {v0.s, v1.s, v2.s}[0], [x8]", "st3.s[0]", 14);
    STORE("st4 {v0.s, v1.s, v2.s, v3.s}[0], [x8]", "st4.s[0]", 18);
    STORE("st4 {v0.s, v1.s, v2.s, v3.s}[2], [x8]", "st4.s[2]", 18);
    STORE("st1 {v0.d}[1], [x8]", "st1.d[1]", 10);
    STORE("st2 {v0.d, v1.d}[0], [x8]", "st2.d[0]", 18);
    STORE("st3 {v0.d, v1.d, v2.d}[1], [x8]", "st3.d[1]", 26);
    STORE("st4 {v0.d, v1.d, v2.d, v3.d}[1], [x8]", "st4.d[1]", 34);

    /* Register wrap-around: v31, v0, v1. */
    {
        u64 r[4][2];
        __asm__ volatile(FILL "mov x8, %1\n\t"
                         "ld3 {v31.s, v0.s, v1.s}[1], [x8]\n\t"
                         "mov v2.16b, v31.16b\n\t"
                         "st1 {v0.2d, v1.2d, v2.2d}, [%0]"
                         :
                         : "r"(r), "r"(src)
                         : "x8", "v0", "v1", "v2", "v3", "v31", "memory");
        show_regs("ld3.s[1] v31,v0,v1 (v0 v1 v31)", (const u64(*)[2])r, 3);
    }

    /* Post-index: the immediate (selem times the element size) and a register. */
    {
        u64 a = (u64)src, b = (u64)src, c2 = (u64)dst, d = (u64)src, step = 5;
        __asm__ volatile("ld4 {v0.s, v1.s, v2.s, v3.s}[1], [%0], #16\n\t"
                         "ld2 {v0.h, v1.h}[3], [%1], %4\n\t"
                         "st3 {v0.b, v1.b, v2.b}[2], [%2], #3\n\t"
                         "ld3r {v0.8h, v1.8h, v2.8h}, [%3], #6"
                         : "+r"(a), "+r"(b), "+r"(c2), "+r"(d)
                         : "r"(step)
                         : "v0", "v1", "v2", "v3", "memory");
        put("post ld4.s ");
        putx(a - (u64)src);
        put(" ld2.h reg ");
        putx(b - (u64)src);
        put(" st3.b ");
        putx(c2 - (u64)dst);
        put(" ld3r.8h ");
        putx(d - (u64)src);
        put("\n");
    }
    return 0;
}
