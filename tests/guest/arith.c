/* Integer ALU, shifts, bit tricks, selects, 128-bit multiplies and unaligned
 * accesses. Operands come from volatile globals so the operations run at
 * runtime. Prints each result and returns a checksum-derived exit code. */
#include "sys.h"

static volatile u64 vu_a = 0x9E3779B97F4A7C15ull;
static volatile u64 vu_b = 0x00000001F00DCAFEull;
static volatile u64 vu_z = 0;
static volatile u32 vw_a = 0xDEADBEEFu;
static volatile u32 vw_b = 0x0000012Bu;
static volatile u32 vw_z = 0;
static volatile i64 vs_a = -1234567890123LL;
static volatile i64 vs_b = 987654321LL;
static volatile i64 vs_min = -9223372036854775807LL - 1;
static volatile i64 vs_m1 = -1;
static volatile i32 vt_a = -2000000000;
static volatile i32 vt_b = 77;
static volatile i32 vt_m1 = -1;
static volatile int vsh = 13;
static volatile int vsh32 = 5;
static volatile int vrot = 17;

static volatile u8 bytes[64];

static u64 acc;

static void pu(const char *label, u64 v)
{
    put(label);
    put("=");
    putx(v);
    put("\n");
    acc ^= v;
    acc = (acc << 1) | (acc >> 63);
}

static void ps(const char *label, i64 v)
{
    put(label);
    put("=");
    putd(v);
    put("\n");
    acc ^= (u64)v;
    acc = (acc << 1) | (acc >> 63);
}

static void fill(volatile u8 *p, int n, u8 seed)
{
    for (int i = 0; i < n; i++)
        p[i] = (u8)(seed + i * 7);
}

struct __attribute__((packed)) unal64 {
    u8 pad;
    u64 v;
};

struct __attribute__((packed)) unal32 {
    u8 pad;
    u32 v;
};

/* Bit-by-bit count with a volatile index, so gcc does not recognise a popcount idiom. */
static u64 popcount_loop(u64 x)
{
    u64 n = 0;
    for (volatile int i = 0; i < 64; i++)
        n += (x >> i) & 1;
    return n;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    u64 a = vu_a, b = vu_b, z = vu_z;
    u32 wa = vw_a, wb = vw_b, wz = vw_z;
    i64 sa = vs_a, sb = vs_b, smin = vs_min, sm1 = vs_m1;
    i32 ta = vt_a, tb = vt_b, tm1 = vt_m1;

    /* 64-bit add, sub, mul, divide and remainder. */
    pu("u64.add", a + b);
    pu("u64.sub", a - b);
    pu("u64.mul", a * b);
    pu("u64.udiv", a / b);
    pu("u64.urem", a % b);
    pu("u64.udiv0", a / z);
    pu("u64.urem0", a % z);
    ps("s64.add", sa + sb);
    ps("s64.sub", sa - sb);
    ps("s64.mul", sa * sb);
    ps("s64.sdiv", sa / sb);
    ps("s64.srem", sa % sb);
    ps("s64.sdiv_min_m1", smin / sm1);
    ps("s64.srem_min_m1", smin % sm1);
    ps("s64.sdiv_neg", sa / -sb);

    /* 32-bit add, sub, mul, divide and remainder. */
    pu("u32.add", (u32)(wa + wb));
    pu("u32.sub", (u32)(wa - wb));
    pu("u32.mul", (u32)(wa * wb));
    pu("u32.udiv", wa / wb);
    pu("u32.urem", wa % wb);
    pu("u32.udiv0", wa / wz);
    ps("s32.sdiv", (i32)ta / (i32)tb);
    ps("s32.srem", (i32)ta % (i32)tb);
    ps("s32.sdiv_m1", (i32)ta / tm1);
    ps("s32.mul", (i32)(ta * tb));

    /* Shifts by variable amounts. */
    int s64 = vsh, s32 = vsh32;
    pu("u64.lsl", a << s64);
    pu("u64.lsr", a >> s64);
    ps("s64.asr", sa >> s64);
    pu("u32.lsl", (u32)(wa << s32));
    pu("u32.lsr", wa >> s32);
    ps("s32.asr", (i32)ta >> s32);

    /* Rotates. */
    int r = vrot;
    pu("u64.ror", (a >> r) | (a << (64 - r)));
    pu("u32.ror", (u32)((wa >> (r & 31)) | (wa << (32 - (r & 31)))));

    /* Bit tricks. */
    pu("popcount", popcount_loop(a));
    pu("clz64", (u64)__builtin_clzll(a));
    pu("clz64_hi", (u64)__builtin_clzll(b));
    pu("bswap64", __builtin_bswap64(a));
    pu("bswap32", __builtin_bswap32(wa));

    /* Conditional selects. */
    pu("csel_u64", a > b ? a : b);
    ps("csel_s64", sa < sb ? sa : sb);
    pu("csel_u32", (wa & 1) ? wa : wb);
    pu("csel_eq", a == b ? 1 : 2);

    /* 128-bit multiplies: UMULH and SMULH. */
    unsigned __int128 prod = (unsigned __int128)a * (unsigned __int128)b;
    pu("umulh", (u64)(prod >> 64));
    pu("mul_lo", (u64)prod);
    __int128 sprod = (__int128)sa * (__int128)sb;
    pu("smulh", (u64)((unsigned __int128)sprod >> 64));
    pu("smul_lo", (u64)sprod);

    /* Unaligned loads and stores through packed structs. */
    fill(bytes, 64, 0x31);
    const struct unal64 *p64 = (const struct unal64 *)(bytes + 3);
    pu("unaligned_ld64", p64->v);
    struct unal64 *q64 = (struct unal64 *)(bytes + 21);
    q64->v = a ^ 0x0123456789ABCDEFull;
    pu("unaligned_st64", ((const struct unal64 *)(bytes + 21))->v);
    const struct unal32 *p32 = (const struct unal32 *)(bytes + 5);
    pu("unaligned_ld32", p32->v);

    /* Sign extension and narrowing through the usual casts. */
    ps("sext8", (i64)(signed char)bytes[7]);
    pu("zext16", (u64)(uint16_t)bytes[9] | ((u64)(uint16_t)bytes[10] << 8));

    put("checksum=");
    putx(acc);
    put("\n");
    return (int)(acc & 0x7F);
}
