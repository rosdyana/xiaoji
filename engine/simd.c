/* AdvSIMD and FP/SIMD register instructions. Each family follows the Arm ARM
 * encoding tables and pseudocode; tests/guest/simd.c checks them against native
 * hardware. Vector registers are kept as raw bytes (v[n][0] is bytes 0..7,
 * little-endian), so element access is a memcpy at i * esize.
 *
 * Register conventions: a D-form (64-bit) result writes zeros to bits 127:64;
 * a Q-form writes all 128 bits. Scalar results zero the bits above the element. */
#include "simd.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "mem.h"

/* ---------- helpers ---------- */

static void rd_v(const cpu_t *c, unsigned n, uint8_t b[16]) { memcpy(b, c->v[n], 16); }

/* Writes datasize bytes (8 or 16); an 8-byte result zeroes the upper half. */
static void wr_v(cpu_t *c, unsigned n, const uint8_t b[16], unsigned datasize)
{
    memcpy(c->v[n], b, 16);
    if (datasize == 8)
        c->v[n][1] = 0;
}

static uint64_t getel(const uint8_t *b, unsigned esz, unsigned i)
{
    uint64_t v = 0;
    memcpy(&v, b + (size_t)i * esz, esz);
    return v;
}

static void putel(uint8_t *b, unsigned esz, unsigned i, uint64_t v)
{
    memcpy(b + (size_t)i * esz, &v, esz);
}

static uint64_t emask(unsigned esz) { return esz == 8 ? ~0ull : ((1ull << (8 * esz)) - 1); }

/* Sign-extends the low esz bytes of v. */
static int64_t sext_el(uint64_t v, unsigned esz)
{
    unsigned sh = 64 - 8 * esz;
    return (int64_t)(v << sh) >> sh;
}

/* Right shift of a sign- or zero-extended element, with shift counts >= 64 handled. */
static uint64_t shr_el(uint64_t v, unsigned esz, unsigned sh, int is_signed)
{
    if (is_signed) {
        int64_t s = sext_el(v, esz);
        if (sh >= 64)
            return s < 0 ? ~0ull : 0;
        return (uint64_t)(s >> sh);
    }
    if (sh >= 64)
        return 0;
    return v >> sh;
}

static uint64_t gpr_r(const cpu_t *c, unsigned r) { return r == 31 ? 0 : c->x[r]; }

static void gpr_w(cpu_t *c, unsigned r, uint64_t v)
{
    if (r != 31)
        c->x[r] = v;
}

/* Element size in bytes from the lowest set bit of imm5 (B, H, S or D), and the
 * element index. Returns 0 if imm5 is zero. */
static unsigned imm5_size(unsigned imm5, unsigned *log2)
{
    if (!imm5)
        return 0;
    if (imm5 & 1) { *log2 = 0; return 1; }
    if (imm5 & 2) { *log2 = 1; return 2; }
    if (imm5 & 4) { *log2 = 2; return 4; }
    if (imm5 & 8) { *log2 = 3; return 8; }
    return 0; /* only 16 is set: no element size */
}


/* ---------- scalar floating point (default FPCR only) ----------
 * Only the default modes are implemented: round to nearest, no flush-to-zero,
 * NaN propagation without default-NaN mode (DN=0), and no alternative half
 * precision. A non-default FPCR returns -1 (unimplemented) rather than guessing.
 * FPSR cumulative exception flags are not updated. */
#define FPCR_NONDEFAULT ((3ull << 22) | (1ull << 24) | (1ull << 25) | (1ull << 26)) /* RMode, FZ, DN, AHP */
#define FP_QUIET_S 0x00400000u
#define FP_QUIET_D 0x0008000000000000ull
#define FP_DEFAULT_NAN_S 0x7FC00000u
#define FP_DEFAULT_NAN_D 0x7FF8000000000000ull

static int fp_default(const cpu_t *c) { return (c->fpcr & FPCR_NONDEFAULT) == 0; }

static int nan_s(uint32_t b) { return ((b >> 23) & 0xFF) == 0xFF && (b & 0x007FFFFFu); }
static int nan_d(uint64_t b) { return ((b >> 52) & 0x7FF) == 0x7FF && (b & 0x000FFFFFFFFFFFFFull); }

static float as_f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint32_t f_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static double as_d(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static uint64_t d_bits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }

/* Scalar write: the bits above the element are zeroed. */
static void put_fs(cpu_t *c, unsigned r, uint32_t bits) { c->v[r][0] = bits; c->v[r][1] = 0; }
static void put_fd(cpu_t *c, unsigned r, uint64_t bits) { c->v[r][0] = bits; c->v[r][1] = 0; }

/* Two-source arithmetic: FMUL (0), FDIV (1), FADD (2), FSUB (3). A NaN operand
 * propagates (the first NaN, quieted); a NaN from non-NaN operands is the
 * default NaN. */
static int fp_arith2(cpu_t *c, uint32_t insn)
{
    unsigned type = (insn >> 22) & 3, rm = (insn >> 16) & 31, opc = (insn >> 12) & 0xF;
    unsigned rn = (insn >> 5) & 31, rd = insn & 31;
    if (opc > 3 || type > 1 || !fp_default(c))
        return -1;
    if (type == 0) {
        uint32_t a = (uint32_t)c->v[rn][0], b = (uint32_t)c->v[rm][0], r;
        if (nan_s(a) || nan_s(b)) {
            r = nan_s(a) ? (a | FP_QUIET_S) : (b | FP_QUIET_S);
        } else {
            float fa = as_f(a), fb = as_f(b), fr;
            fr = opc == 0 ? fa * fb : opc == 1 ? fa / fb : opc == 2 ? fa + fb : fa - fb;
            r = nan_s(f_bits(fr)) ? FP_DEFAULT_NAN_S : f_bits(fr);
        }
        put_fs(c, rd, r);
        return 0;
    }
    uint64_t a = c->v[rn][0], b = c->v[rm][0], r;
    if (nan_d(a) || nan_d(b)) {
        r = nan_d(a) ? (a | FP_QUIET_D) : (b | FP_QUIET_D);
    } else {
        double da = as_d(a), db = as_d(b), dr;
        dr = opc == 0 ? da * db : opc == 1 ? da / db : opc == 2 ? da + db : da - db;
        r = nan_d(d_bits(dr)) ? FP_DEFAULT_NAN_D : d_bits(dr);
    }
    put_fd(c, rd, r);
    return 0;
}

/* FCMP and FCMPE (register, or against zero when the opc bit 3 is set). The
 * signalling variant only differs in the exception flags, which are not kept. */
static int fp_cmp(cpu_t *c, uint32_t insn)
{
    unsigned type = (insn >> 22) & 3, rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, opc = insn & 0x1F;
    if (type > 1 || !fp_default(c) || (opc & 0x7))
        return -1; /* bit 4 selects FCMPE, which sets the same flags here */
    int zero = (opc >> 3) & 1;
    if (zero && rm != 0)
        return -1;
    double a, b;
    if (type == 0) {
        a = as_f((uint32_t)c->v[rn][0]);
        b = zero ? 0.0 : as_f((uint32_t)c->v[rm][0]);
    } else {
        a = as_d(c->v[rn][0]);
        b = zero ? 0.0 : as_d(c->v[rm][0]);
    }
    uint32_t nzcv;
    if (a != a || b != b)
        nzcv = 0x30000000u; /* unordered: C and V */
    else if (a == b)
        nzcv = 0x60000000u; /* equal: Z and C */
    else if (a < b)
        nzcv = 0x80000000u; /* less: N */
    else
        nzcv = 0x20000000u; /* greater: C */
    c->nzcv = nzcv;
    return 0;
}

/* SCVTF and UCVTF (integer to floating point), round to nearest. */
static int fp_from_int(cpu_t *c, uint32_t insn)
{
    unsigned sf = insn >> 31, type = (insn >> 22) & 3, opc = (insn >> 16) & 7, rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned rmode = (insn >> 19) & 3;
    if (type > 1 || rmode != 0 || !fp_default(c))
        return -1;
    int uns = opc == 3;
    uint64_t x = gpr_r(c, rn);
    if (!sf)
        x = uns ? (uint64_t)(uint32_t)x : (uint64_t)(int64_t)(int32_t)(uint32_t)x;
    if (type == 0)
        put_fs(c, rd, f_bits(uns ? (float)x : (float)(int64_t)x));
    else
        put_fd(c, rd, d_bits(uns ? (double)x : (double)(int64_t)x));
    return 0;
}

/* FCVTNS/FCVTNU, FCVTPS/FCVTPU, FCVTMS/FCVTMU, FCVTZS/FCVTZU: to integer with
 * rounding mode rmode (0 nearest, 1 up, 2 down, 3 toward zero). Saturates;
 * NaN gives 0. */
static int fp_to_int(cpu_t *c, uint32_t insn)
{
    unsigned sf = insn >> 31, type = (insn >> 22) & 3, rmode = (insn >> 19) & 3, uns = (insn >> 16) & 1;
    unsigned rn = (insn >> 5) & 31, rd = insn & 31;
    if (type > 1 || !fp_default(c))
        return -1;
    double x = type == 0 ? (double)as_f((uint32_t)c->v[rn][0]) : as_d(c->v[rn][0]);
    double r = rmode == 0 ? rint(x) : rmode == 1 ? ceil(x) : rmode == 2 ? floor(x) : trunc(x);
    uint64_t out;
    if (x != x) {
        out = 0;
    } else if (uns) {
        if (r <= 0)
            out = 0;
        else if (sf)
            out = r >= 18446744073709551616.0 ? ~0ull : (uint64_t)r;
        else
            out = r >= 4294967296.0 ? 0xFFFFFFFFull : (uint64_t)r;
    } else if (sf) {
        if (r >= 9223372036854775808.0)
            out = (uint64_t)INT64_MAX;
        else if (r < -9223372036854775808.0)
            out = (uint64_t)INT64_MIN;
        else
            out = (uint64_t)(int64_t)r;
    } else {
        if (r >= 2147483648.0)
            out = 0x7FFFFFFFull;
        else if (r < -2147483648.0)
            out = 0x80000000ull;
        else
            out = (uint64_t)(uint32_t)(int32_t)r;
    }
    gpr_w(c, rd, sf ? out : (uint32_t)out);
    return 0;
}

/* ---------- FMOV between general and SIMD registers ---------- */

static int fmov_gen(cpu_t *c, uint32_t insn)
{
    unsigned sf = insn >> 31, ftype = (insn >> 22) & 3, rmode = (insn >> 19) & 3, opc = (insn >> 16) & 7;
    unsigned rn = (insn >> 5) & 31, rd = insn & 31;
    if (!sf && ftype == 0 && rmode == 0 && opc == 7) { /* FMOV Sd, Wn */
        c->v[rd][0] = (uint32_t)gpr_r(c, rn);
        c->v[rd][1] = 0;
        return 0;
    }
    if (!sf && ftype == 0 && rmode == 0 && opc == 6) { /* FMOV Wd, Sn */
        gpr_w(c, rd, (uint32_t)c->v[rn][0]);
        return 0;
    }
    if (sf && ftype == 1 && rmode == 0 && opc == 7) { /* FMOV Dd, Xn */
        c->v[rd][0] = gpr_r(c, rn);
        c->v[rd][1] = 0;
        return 0;
    }
    if (sf && ftype == 1 && rmode == 0 && opc == 6) { /* FMOV Xd, Dn */
        gpr_w(c, rd, c->v[rn][0]);
        return 0;
    }
    if (sf && ftype == 2 && rmode == 1 && opc == 7) { /* FMOV Vd.D[1], Xn */
        c->v[rd][1] = gpr_r(c, rn);
        return 0;
    }
    if (sf && ftype == 2 && rmode == 1 && opc == 6) { /* FMOV Xd, Vn.D[1] */
        gpr_w(c, rd, c->v[rn][1]);
        return 0;
    }
    return -1;
}

/* ---------- modified immediate: MOVI, MVNI, ORR and BIC (vector, immediate) ---------- */

static int modified_imm(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, op = (insn >> 29) & 1;
    unsigned abc = (insn >> 16) & 7, cmode = (insn >> 12) & 0xF, defgh = (insn >> 5) & 0x1F;
    unsigned rd = insn & 31;
    unsigned imm8 = (abc << 5) | defgh;
    unsigned dsz = q ? 16 : 8;
    uint8_t d[16], r[16];
    rd_v(c, rd, d);

    uint64_t val = 0;
    int invert = 0, logic = 0; /* logic: ORR (op 0) / BIC (op 1) applied to Vd */
    unsigned esz = 4;          /* element size of the replicated immediate */
    if (cmode == 0xF)
        return -1; /* FMOV (vector, immediate) is not in M1 */
    if (cmode < 8) { /* 32-bit shifted immediate */
        unsigned sh = (cmode >> 1) & 3;
        val = (uint64_t)imm8 << (8 * sh);
        if (cmode & 1)
            logic = 1;
        else
            invert = op;
        esz = 4;
    } else if (cmode < 12) { /* 16-bit shifted immediate */
        unsigned sh = (cmode >> 1) & 1;
        val = (uint64_t)imm8 << (8 * sh);
        if (cmode & 1)
            logic = 1;
        else
            invert = op;
        esz = 2;
    } else if (cmode < 14) { /* shifting ones: MOVI / MVNI */
        val = (cmode & 1) ? (((uint64_t)imm8 << 16) | 0xFFFF) : (((uint64_t)imm8 << 8) | 0xFF);
        invert = op;
        esz = 4;
    } else { /* cmode 1110: 8-bit MOVI, or 64-bit MOVI (op 1) */
        if (!op) {
            val = (uint64_t)imm8 * 0x0101010101010101ull;
            esz = 1;
        } else {
            uint64_t v = 0;
            for (unsigned i = 0; i < 8; i++)
                if (imm8 & (1u << i))
                    v |= 0xFFull << (8 * i);
            val = v;
            esz = 8;
        }
    }
    if (invert)
        val = ~val;
    /* Replicate the element (esz bytes) across one 64-bit half. */
    uint64_t w;
    switch (esz) {
    case 1: w = (val & 0xFFull) * 0x0101010101010101ull; break;
    case 2: w = (val & 0xFFFFull) * 0x0001000100010001ull; break;
    case 4: w = (val & 0xFFFFFFFFull) * 0x0000000100000001ull; break;
    default: w = val; break;
    }
    memcpy(r, &w, 8);
    memcpy(r + 8, &w, 8);
    if (logic) {
        for (unsigned i = 0; i < dsz; i++)
            r[i] = op ? (uint8_t)(d[i] & ~r[i]) : (uint8_t)(d[i] | r[i]);
    }
    wr_v(c, rd, r, dsz);
    return 0;
}

/* ---------- shift by immediate: SSHR, USHR, SHL, SHRN, SHRN2 ---------- */

static int shift_imm(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, u = (insn >> 29) & 1;
    unsigned immh = (insn >> 19) & 0xF, immhb = (insn >> 16) & 0x7F;
    unsigned opc = (insn >> 11) & 0x1F, rn = (insn >> 5) & 31, rd = insn & 31;
    if (!immh)
        return -1;
    unsigned hb = 31 - (unsigned)__builtin_clz(immh); /* highest set bit of immh */
    unsigned esz = 1u << hb;                          /* element size in bytes */
    unsigned dsz = q ? 16 : 8;
    unsigned lanes = dsz / esz;
    uint8_t n[16], r[16], d[16];
    rd_v(c, rn, n);
    switch (opc) {
    case 0x00: { /* SSHR (u=0), USHR (u=1) */
        unsigned sh = 2 * esz * 8 - immhb;
        for (unsigned i = 0; i < lanes; i++)
            putel(r, esz, i, shr_el(getel(n, esz, i), esz, sh, !u) & emask(esz));
        wr_v(c, rd, r, dsz);
        return 0;
    }
    case 0x0A: { /* SHL (u=0) */
        if (u)
            return -1; /* SLI is not in M1 */
        unsigned sh = immhb - esz * 8;
        for (unsigned i = 0; i < lanes; i++)
            putel(r, esz, i, (getel(n, esz, i) << sh) & emask(esz));
        wr_v(c, rd, r, dsz);
        return 0;
    }
    case 0x10: { /* SHRN (u=0): narrow the 128-bit source into 64 bits */
        if (u || esz > 4)
            return -1;
        unsigned src_esz = 2 * esz;
        unsigned sh = 2 * esz * 8 - immhb;
        unsigned src_lanes = 16 / src_esz;
        uint8_t half[8];
        memset(half, 0, sizeof half);
        for (unsigned i = 0; i < src_lanes; i++)
            putel(half, esz, i, shr_el(getel(n, src_esz, i), src_esz, sh, 0) & emask(esz));
        if (q) {
            rd_v(c, rd, d);
            memcpy(r, d, 8);
            memcpy(r + 8, half, 8);
            wr_v(c, rd, r, 16);
        } else {
            memset(r, 0, sizeof r);
            memcpy(r, half, 8);
            wr_v(c, rd, r, 8);
        }
        return 0;
    }
    case 0x14: { /* SSHLL (u=0), USHLL (u=1); shift 0 is SXTL and UXTL */
        if (esz > 4)
            return -1; /* immh 1xxx would need 128-bit source elements */
        unsigned sh = immhb - esz * 8;
        unsigned dst_esz = 2 * esz, half_lanes = 8 / esz;
        unsigned half_off = q ? 8 : 0;
        for (unsigned i = 0; i < half_lanes; i++) {
            uint64_t x = getel(n + half_off, esz, i);
            int64_t v = u ? (int64_t)x : sext_el(x, esz);
            putel(r, dst_esz, i, ((uint64_t)v << sh) & emask(dst_esz));
        }
        wr_v(c, rd, r, 16);
        return 0;
    }
    default:
        return -1;
    }
}

/* ---------- three registers of the same type ---------- */

static int three_same(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, u = (insn >> 29) & 1, size = (insn >> 22) & 3;
    unsigned rm = (insn >> 16) & 31, opc = (insn >> 11) & 0x1F, rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned dsz = q ? 16 : 8, esz = 1u << size, lanes = dsz / esz;
    uint8_t n[16], m[16], d[16], r[16];
    rd_v(c, rn, n);
    rd_v(c, rm, m);
    rd_v(c, rd, d);
    memset(r, 0, sizeof r);

    if (opc == 0x03) { /* AND, BIC, ORR, ORN (u=0); EOR, BSL, BIT, BIF (u=1) */
        for (unsigned i = 0; i < dsz; i++) {
            uint8_t a = n[i], b = m[i], x = d[i];
            switch ((u << 2) | size) {
            case 0: r[i] = a & b; break;                      /* AND */
            case 1: r[i] = a & (uint8_t)~b; break;            /* BIC */
            case 2: r[i] = a | b; break;                      /* ORR */
            case 3: r[i] = a | (uint8_t)~b; break;            /* ORN */
            case 4: r[i] = a ^ b; break;                      /* EOR */
            case 5: r[i] = (x & a) | ((uint8_t)~x & b); break; /* BSL: x selects a, else b */
            case 6: r[i] = (x & (uint8_t)~b) | (a & b); break; /* BIT: insert a where b is 1 */
            default: r[i] = (x & b) | (a & (uint8_t)~b); break; /* BIF: insert a where b is 0 */
            }
        }
        wr_v(c, rd, r, dsz);
        return 0;
    }

    switch (opc) {
    case 0x10: /* ADD (u=0), SUB (u=1) */
        for (unsigned i = 0; i < lanes; i++) {
            uint64_t a = getel(n, esz, i), b = getel(m, esz, i);
            putel(r, esz, i, (u ? a - b : a + b) & emask(esz));
        }
        break;
    case 0x11: /* CMTST (u=0), CMEQ (u=1) */
        for (unsigned i = 0; i < lanes; i++) {
            uint64_t a = getel(n, esz, i), b = getel(m, esz, i);
            int t = u ? (a == b) : ((a & b) != 0);
            putel(r, esz, i, t ? emask(esz) : 0);
        }
        break;
    case 0x06: /* CMGT (u=0), CMHI (u=1) */
    case 0x07: /* CMGE (u=0), CMHS (u=1) */
        for (unsigned i = 0; i < lanes; i++) {
            uint64_t a = getel(n, esz, i), b = getel(m, esz, i);
            int t;
            if (opc == 0x06)
                t = u ? (a > b) : (sext_el(a, esz) > sext_el(b, esz));
            else
                t = u ? (a >= b) : (sext_el(a, esz) >= sext_el(b, esz));
            putel(r, esz, i, t ? emask(esz) : 0);
        }
        break;
    case 0x12: /* MLA (u=0), MLS (u=1): accumulate into Vd, modulo the element size */
        if (size == 3)
            return -1;
        for (unsigned i = 0; i < lanes; i++) {
            uint64_t prod = getel(n, esz, i) * getel(m, esz, i);
            uint64_t acc = getel(d, esz, i);
            putel(r, esz, i, (u ? acc - prod : acc + prod) & emask(esz));
        }
        break;
    case 0x13: /* MUL (u=0): low bits of the product; PMUL (u=1) is not in M1 */
        if (u || size == 3)
            return -1;
        for (unsigned i = 0; i < lanes; i++)
            putel(r, esz, i, (getel(n, esz, i) * getel(m, esz, i)) & emask(esz));
        break;
    case 0x0C: /* SMAX (u=0), UMAX (u=1) */
    case 0x0D: /* SMIN (u=0), UMIN (u=1) */
        for (unsigned i = 0; i < lanes; i++) {
            uint64_t a = getel(n, esz, i), b = getel(m, esz, i);
            int a_gt_b = u ? (a > b) : (sext_el(a, esz) > sext_el(b, esz));
            int pick_a = (opc == 0x0C) ? a_gt_b : !a_gt_b;
            putel(r, esz, i, pick_a ? a : b);
        }
        break;
    case 0x14: /* UMAXP (u=1) and UMINP (u=1, opc 0x15): pairwise over n:m */
    case 0x15:
    case 0x17: /* ADDP (u=0) */
        if (opc == 0x14 && !u)
            return -1; /* SMAXP is not in M1 */
        if (opc == 0x15 && !u)
            return -1; /* SMINP is not in M1 */
        if (opc == 0x17 && u)
            return -1;
        for (unsigned i = 0; i < lanes / 2; i++) {
            uint64_t a0 = getel(n, esz, 2 * i), a1 = getel(n, esz, 2 * i + 1);
            uint64_t b0 = getel(m, esz, 2 * i), b1 = getel(m, esz, 2 * i + 1);
            uint64_t lo, hi;
            if (opc == 0x17) {
                lo = (a0 + a1) & emask(esz);
                hi = (b0 + b1) & emask(esz);
            } else if (opc == 0x14) {
                lo = a0 > a1 ? a0 : a1;
                hi = b0 > b1 ? b0 : b1;
            } else {
                lo = a0 < a1 ? a0 : a1;
                hi = b0 < b1 ? b0 : b1;
            }
            putel(r, esz, i, lo);
            putel(r, esz, lanes / 2 + i, hi);
        }
        break;
    default:
        return -1;
    }
    wr_v(c, rd, r, dsz);
    return 0;
}

/* ---------- three registers, different widths ---------- */

static int three_diff(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, u = (insn >> 29) & 1, size = (insn >> 22) & 3;
    unsigned rm = (insn >> 16) & 31, opc = (insn >> 12) & 0xF, rn = (insn >> 5) & 31, rd = insn & 31;
    if (!u && opc == 0xE && size == 0) { /* PMULL and PMULL2 (8-bit polynomial multiply) */
        unsigned half8 = q ? 8 : 0;
        uint8_t n[16], m[16], r[16];
        rd_v(c, rn, n);
        rd_v(c, rm, m);
        for (unsigned i = 0; i < 8; i++) {
            uint16_t acc = 0; /* carry-less product of two 8-bit polynomials */
            for (unsigned k = 0; k < 8; k++)
                if ((m[half8 + i] >> k) & 1)
                    acc ^= (uint16_t)((uint16_t)n[half8 + i] << k);
            r[2 * i] = (uint8_t)acc;
            r[2 * i + 1] = (uint8_t)(acc >> 8);
        }
        wr_v(c, rd, r, 16);
        return 0;
    }
    if (size == 3 || !(u && opc == 0x8)) /* only UMLAL, UMLAL2 and 8-bit PMULL are in M1 */
        return -1;
    unsigned esz = 1u << size, dst_esz = 2 * esz, lanes = 8 / esz;
    unsigned half = q ? 8 : 0;
    uint8_t n[16], m[16], d[16], r[16];
    rd_v(c, rn, n);
    rd_v(c, rm, m);
    rd_v(c, rd, d);
    for (unsigned i = 0; i < lanes; i++) {
        uint64_t prod = getel(n + half, esz, i) * getel(m + half, esz, i);
        putel(r, dst_esz, i, (getel(d, dst_esz, i) + prod) & emask(dst_esz));
    }
    wr_v(c, rd, r, 16);
    return 0;
}

/* ---------- two registers, miscellaneous ---------- */

static int two_misc(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, u = (insn >> 29) & 1, size = (insn >> 22) & 3;
    unsigned opc = (insn >> 12) & 0x1F, rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned dsz = q ? 16 : 8, esz = 1u << size, lanes = dsz / esz;
    uint8_t n[16], r[16], d[16];
    rd_v(c, rn, n);
    memset(r, 0, sizeof r);

    switch (opc) {
    case 0x00: { /* REV64 (u=0): reverse the elements inside each 64-bit doubleword */
        if (u || size == 3)
            return -1;
        unsigned per = 8 / esz;
        for (unsigned i = 0; i < lanes; i++) {
            unsigned base = (i / per) * per;
            unsigned j = base + (per - 1 - (i - base));
            putel(r, esz, i, getel(n, esz, j));
        }
        break;
    }
    case 0x02: { /* SADDLP (u=0), UADDLP (u=1): pairwise sums into elements twice as wide */
        if (size == 3)
            return -1;
        unsigned dst_esz = 2 * esz;
        for (unsigned i = 0; i < lanes / 2; i++) {
            uint64_t a = getel(n, esz, 2 * i), b = getel(n, esz, 2 * i + 1);
            uint64_t sum = u ? a + b : (uint64_t)(sext_el(a, esz) + sext_el(b, esz));
            putel(r, dst_esz, i, sum & emask(dst_esz));
        }
        wr_v(c, rd, r, dsz);
        return 0;
    }
    case 0x05: /* CNT (u=0, size 0) and NOT (u=1, size 0) */
        if (size != 0)
            return -1;
        for (unsigned i = 0; i < dsz; i++)
            r[i] = u ? (uint8_t)~n[i] : (uint8_t)__builtin_popcount(n[i]);
        break;
    case 0x08: /* CMGT zero (u=0), CMGE zero (u=1) */
    case 0x09: /* CMEQ zero (u=0), CMLE zero (u=1) */
    case 0x0A: /* CMLT zero (u=0) */
        if (opc == 0x0A && u)
            return -1;
        for (unsigned i = 0; i < lanes; i++) {
            int64_t s = sext_el(getel(n, esz, i), esz);
            int t;
            if (opc == 0x08)
                t = u ? (s >= 0) : (s > 0);
            else if (opc == 0x09)
                t = u ? (s <= 0) : (s == 0);
            else
                t = s < 0;
            putel(r, esz, i, t ? emask(esz) : 0);
        }
        break;
    case 0x12: { /* XTN (u=0), XTN2 when q: narrow the source's low elements */
        if (u || size == 3)
            return -1;
        unsigned src_esz = 2u << size; /* H, S or D source */
        unsigned src_lanes = 16 / src_esz;
        uint8_t half[8];
        memset(half, 0, sizeof half);
        for (unsigned i = 0; i < src_lanes; i++)
            putel(half, esz, i, getel(n, src_esz, i) & emask(esz));
        if (q) {
            rd_v(c, rd, d);
            memcpy(r, d, 8);
            memcpy(r + 8, half, 8);
            wr_v(c, rd, r, 16);
        } else {
            memcpy(r, half, 8);
            wr_v(c, rd, r, 8);
        }
        return 0;
    }
    default:
        return -1;
    }
    wr_v(c, rd, r, dsz);
    return 0;
}

/* ---------- across lanes ---------- */

static int across_lanes(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, u = (insn >> 29) & 1, size = (insn >> 22) & 3;
    unsigned opc = (insn >> 12) & 0x1F, rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned dsz = q ? 16 : 8, esz = 1u << size;
    if (size == 3 || (size == 2 && !q))
        return -1;
    unsigned lanes = dsz / esz;
    uint8_t n[16], r[16];
    rd_v(c, rn, n);
    memset(r, 0, sizeof r);

    switch (opc) {
    case 0x1B: { /* ADDV (u=0) */
        if (u)
            return -1;
        uint64_t sum = 0;
        for (unsigned i = 0; i < lanes; i++)
            sum += getel(n, esz, i);
        putel(r, esz, 0, sum & emask(esz));
        break;
    }
    case 0x03: { /* UADDLV (u=1): widened sum, result element is twice the size */
        if (!u)
            return -1;
        uint64_t sum = 0;
        for (unsigned i = 0; i < lanes; i++)
            sum += getel(n, esz, i);
        unsigned rsz = 2 * esz;
        putel(r, rsz, 0, sum & emask(rsz));
        break;
    }
    case 0x0A: /* UMAXV (u=1) */
    case 0x1A: /* UMINV (u=1) */
    {
        if (!u)
            return -1;
        uint64_t best = getel(n, esz, 0);
        for (unsigned i = 1; i < lanes; i++) {
            uint64_t x = getel(n, esz, i);
            if (opc == 0x0A ? x > best : x < best)
                best = x;
        }
        putel(r, esz, 0, best);
        break;
    }
    default:
        return -1;
    }
    wr_v(c, rd, r, 16); /* scalar results zero the rest of the register */
    return 0;
}

/* ---------- permute: UZP, TRN and ZIP ---------- */

static int permute(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, size = (insn >> 22) & 3, rm = (insn >> 16) & 31;
    unsigned opc = (insn >> 12) & 7, rn = (insn >> 5) & 31, rd = insn & 31;
    if (size == 3 && !q)
        return -1; /* 1D permutes do not exist */
    unsigned dsz = q ? 16 : 8, esz = 1u << size, lanes = dsz / esz;
    uint8_t n[16], m[16], r[16];
    rd_v(c, rn, n);
    rd_v(c, rm, m);
    memset(r, 0, sizeof r);
    unsigned half = lanes / 2;
    unsigned part = (opc & 4) ? 1 : 0; /* 1: the "2" forms (odd or upper elements) */
    switch (opc & 3) {
    case 1: /* UZP: de-interleave; UZP1 takes even elements, UZP2 odd */
        for (unsigned i = 0; i < half; i++) {
            putel(r, esz, i, getel(n, esz, 2 * i + part));
            putel(r, esz, half + i, getel(m, esz, 2 * i + part));
        }
        break;
    case 2: /* TRN: transpose pairs; TRN1 takes elements 2i, TRN2 2i+1 */
        for (unsigned i = 0; i < half; i++) {
            putel(r, esz, 2 * i, getel(n, esz, 2 * i + part));
            putel(r, esz, 2 * i + 1, getel(m, esz, 2 * i + part));
        }
        break;
    case 3: /* ZIP: interleave the low (ZIP1) or high (ZIP2) halves */
        for (unsigned i = 0; i < half; i++) {
            putel(r, esz, 2 * i, getel(n, esz, half * part + i));
            putel(r, esz, 2 * i + 1, getel(m, esz, half * part + i));
        }
        break;
    default:
        return -1;
    }
    wr_v(c, rd, r, dsz);
    return 0;
}

/* ---------- table lookup and extract ---------- */

static int tbl(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, len = (insn >> 13) & 3, op = (insn >> 12) & 1;
    unsigned rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned dsz = q ? 16 : 8, nregs = len + 1;
    uint8_t table[64], idx[16], d[16], r[16];
    for (unsigned t = 0; t < nregs; t++)
        memcpy(table + 16 * t, c->v[(rn + t) & 31], 16);
    rd_v(c, rm, idx);
    rd_v(c, rd, d);
    memset(r, 0, sizeof r);
    for (unsigned i = 0; i < dsz; i++) {
        unsigned k = idx[i];
        if (k < 16 * nregs)
            r[i] = table[k];
        else
            r[i] = op ? d[i] : 0; /* TBX keeps the destination byte */
    }
    wr_v(c, rd, r, dsz);
    return 0;
}

static int ext(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, rm = (insn >> 16) & 31, index = (insn >> 11) & 0xF;
    unsigned rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned dsz = q ? 16 : 8;
    if (!q && index > 7)
        return -1;
    uint8_t n[16], m[16], r[16];
    rd_v(c, rn, n);
    rd_v(c, rm, m);
    memset(r, 0, sizeof r);
    for (unsigned i = 0; i < dsz; i++) {
        unsigned j = index + i;
        r[i] = j < dsz ? n[j] : m[j - dsz];
    }
    wr_v(c, rd, r, dsz);
    return 0;
}

/* ---------- copies: DUP, INS, UMOV and SMOV ---------- */

static int dup_elem(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, imm5 = (insn >> 16) & 0x1F, rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned log2, esz = imm5_size(imm5, &log2);
    if (!esz)
        return -1;
    unsigned index = imm5 >> (log2 + 1);
    unsigned dsz = q ? 16 : 8;
    uint8_t n[16], r[16];
    rd_v(c, rn, n);
    uint64_t v = getel(n, esz, index);
    for (unsigned i = 0; i < dsz / esz; i++)
        putel(r, esz, i, v);
    wr_v(c, rd, r, dsz);
    return 0;
}

static int dup_gen(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, imm5 = (insn >> 16) & 0x1F, rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned log2, esz = imm5_size(imm5, &log2);
    if (!esz || (esz == 8 && !q))
        return -1;
    unsigned dsz = q ? 16 : 8;
    uint8_t r[16];
    uint64_t v = gpr_r(c, rn);
    for (unsigned i = 0; i < dsz / esz; i++)
        putel(r, esz, i, v);
    wr_v(c, rd, r, dsz);
    return 0;
}

static int ins_gen(cpu_t *c, uint32_t insn)
{
    unsigned imm5 = (insn >> 16) & 0x1F, rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned log2, esz = imm5_size(imm5, &log2);
    if (!esz)
        return -1;
    unsigned index = imm5 >> (log2 + 1);
    uint8_t d[16];
    rd_v(c, rd, d);
    putel(d, esz, index, gpr_r(c, rn));
    wr_v(c, rd, d, 16);
    return 0;
}

static int ins_elem(cpu_t *c, uint32_t insn)
{
    unsigned imm5 = (insn >> 16) & 0x1F, imm4 = (insn >> 11) & 0xF, rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned log2, esz = imm5_size(imm5, &log2);
    if (!esz)
        return -1;
    unsigned dst = imm5 >> (log2 + 1), src = imm4 >> log2;
    uint8_t d[16], n[16];
    rd_v(c, rd, d);
    rd_v(c, rn, n);
    putel(d, esz, dst, getel(n, esz, src));
    wr_v(c, rd, d, 16);
    return 0;
}

static int umov_smov(cpu_t *c, uint32_t insn, int is_signed)
{
    unsigned q = (insn >> 30) & 1, imm5 = (insn >> 16) & 0x1F, rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned log2, esz = imm5_size(imm5, &log2);
    if (!esz)
        return -1;
    unsigned index = imm5 >> (log2 + 1);
    uint8_t n[16];
    rd_v(c, rn, n);
    if (!is_signed) {
        if (esz == 8 ? !q : q)
            return -1; /* UMOV: D to X, and B, H, S to W */
        gpr_w(c, rd, getel(n, esz, index));
        return 0;
    }
    if (esz == 8 || (esz == 4 && !q))
        return -1; /* SMOV: B and H to W or X, S only to X */
    uint64_t v = (uint64_t)sext_el(getel(n, esz, index), esz);
    gpr_w(c, rd, q ? v : (uint32_t)v);
    return 0;
}

/* ---------- LD1/ST1 (multiple structures) ---------- */

int simd_ldst_multi(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, post = (insn >> 23) & 1, L = (insn >> 22) & 1;
    unsigned rm = (insn >> 16) & 31, opc = (insn >> 12) & 0xF;
    unsigned rn = (insn >> 5) & 31, rt = insn & 31;
    unsigned nregs;
    switch (opc) {
    case 0x7: nregs = 1; break; /* LD1/ST1 {Vt} */
    case 0xA: nregs = 2; break; /* {Vt, Vt2} */
    case 0x6: nregs = 3; break; /* {Vt, Vt2, Vt3} */
    case 0x2: nregs = 4; break; /* {Vt, Vt2, Vt3, Vt4} */
    default: return -1;         /* LD2, LD3 and LD4 are not in M1 */
    }
    unsigned dsz = q ? 16 : 8;
    uint64_t addr = rn == 31 ? c->sp : c->x[rn];
    for (unsigned t = 0; t < nregs; t++) {
        unsigned reg = (rt + t) & 31;
        uint64_t va = addr + (uint64_t)t * dsz;
        if (L) {
            if (dsz == 16) {
                mem_read128(va, c->v[reg]);
            } else {
                c->v[reg][0] = mem_read64(va);
                c->v[reg][1] = 0;
            }
        } else {
            if (dsz == 16)
                mem_write128(va, c->v[reg]);
            else
                mem_write64(va, c->v[reg][0]);
        }
    }
    if (post) {
        uint64_t inc = rm == 31 ? (uint64_t)nregs * dsz : c->x[rm];
        uint64_t nb = addr + inc;
        if (rn == 31)
            c->sp = nb;
        else
            c->x[rn] = nb;
    }
    return 0;
}

/* ---------- LD1/ST1 single structure and LD1R (replicate) ---------- */

int simd_ldst_single(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, post = (insn >> 23) & 1, L = (insn >> 22) & 1, R = (insn >> 21) & 1;
    unsigned rm = (insn >> 16) & 31, opc = (insn >> 13) & 7, S = (insn >> 12) & 1, size = (insn >> 10) & 3;
    unsigned rn = (insn >> 5) & 31, rt = insn & 31;
    unsigned esz, index;
    if (!post && rm)
        return -1; /* without post-index, Rm is not encoded */
    if (!R && opc != 6) { /* one register, one lane */
        if (opc == 0) {
            esz = 1;
            index = (q << 3) | (S << 2) | size;
        } else if (opc == 2) {
            if (size & 1)
                return -1;
            esz = 2;
            index = (q << 2) | (S << 1) | (size >> 1);
        } else if (opc == 4) {
            if (size == 0) {
                esz = 4;
                index = (q << 1) | S;
            } else if (size == 1 && !S) {
                esz = 8;
                index = q;
            } else {
                return -1;
            }
        } else {
            return -1; /* LD2 to LD4 lanes are not in M1 */
        }
        uint64_t base = rn == 31 ? c->sp : c->x[rn];
        uint8_t reg[16];
        memcpy(reg, c->v[rt], 16);
        if (L) { /* only the addressed lane changes */
            for (unsigned i = 0; i < esz; i++)
                reg[(size_t)index * esz + i] = mem_read8(base + i);
            memcpy(c->v[rt], reg, 16);
        } else {
            for (unsigned i = 0; i < esz; i++)
                mem_write8(base + i, reg[(size_t)index * esz + i]);
        }
        if (post) {
            uint64_t inc = rm == 31 ? esz : c->x[rm];
            if (rn == 31)
                c->sp = base + inc;
            else
                c->x[rn] = base + inc;
        }
        return 0;
    }
    /* LD1R (R clear, opcode 110): load one element and replicate it. R set with
     * opcode 110 is LD2R and up, which are not in M1. */
    if (!L || R || opc != 6)
        return -1;
    esz = 1u << size;
    unsigned dsz = q ? 16 : 8;
    uint64_t base = rn == 31 ? c->sp : c->x[rn];
    uint64_t val = 0;
    for (unsigned i = 0; i < esz; i++)
        val |= (uint64_t)mem_read8(base + i) << (8 * i);
    uint8_t r[16];
    for (unsigned i = 0; i < dsz / esz; i++)
        memcpy(r + (size_t)i * esz, &val, esz);
    wr_v(c, rt, r, dsz);
    if (post) {
        uint64_t inc = rm == 31 ? esz : c->x[rm];
        if (rn == 31)
            c->sp = base + inc;
        else
            c->x[rn] = base + inc;
    }
    return 0;
}

/* ---------- dispatch ---------- */

int simd_exec(cpu_t *c, uint32_t insn)
{
    unsigned top = (insn >> 24) & 0x1F; /* bits 28..24 */
    if (top == 0x1E) { /* scalar floating point: bits 30 and 29 are zero (0x5E and 0x7E are AdvSIMD) */
        if (((insn >> 29) & 3) || !((insn >> 21) & 1))
            return -1;
        unsigned low6 = (insn >> 10) & 0x3F;
        if (low6 == 0) {
            /* opcode 0/1: FCVT{N,P,M,Z}{S,U}; 2/3: SCVTF, UCVTF; 6/7: FMOV general */
            unsigned op = (insn >> 16) & 7;
            if (op == 6 || op == 7)
                return fmov_gen(c, insn);
            if (op < 2)
                return fp_to_int(c, insn);
            if (op < 4)
                return fp_from_int(c, insn);
            return -1;
        }
        if (insn >> 31)
            return -1;
        if ((low6 & 3) == 2 && ((insn >> 12) & 0xF) <= 3)
            return fp_arith2(c, insn);
        if (low6 == 0x08)
            return fp_cmp(c, insn);
        return -1;
    }
    if (insn >> 31)
        return -1;
    if (top == 0x0E) { /* Advanced SIMD (bit 29 is U) */
        unsigned u = (insn >> 29) & 1, b21 = (insn >> 21) & 1, b15 = (insn >> 15) & 1;
        unsigned b11_10 = (insn >> 10) & 3;
        if (b21) {
            if ((insn >> 10) & 1)
                return three_same(c, insn);
            if (b11_10 == 2 && ((insn >> 17) & 0x1F) == 0x10)
                return two_misc(c, insn);
            if (b11_10 == 2 && ((insn >> 17) & 0x1F) == 0x18)
                return across_lanes(c, insn);
            if (b11_10 == 0)
                return three_diff(c, insn);
            return -1;
        }
        if (b15)
            return -1;
        /* Copies, TBL and EXT need bits 23:22 clear; permutes use them as size. */
        unsigned sz = (insn >> 22) & 3;
        if (!u && b11_10 == 2)
            return permute(c, insn);
        if (sz)
            return -1;
        if (u) {
            if (!((insn >> 10) & 1))
                return ext(c, insn);
            return ins_elem(c, insn);
        }
        switch (b11_10) {
        case 0: return tbl(c, insn);
        case 1: return ((insn >> 11) & 0xF) ? -1 : dup_elem(c, insn);
        default:
            switch ((insn >> 11) & 0xF) {
            case 0x1: return dup_gen(c, insn);
            case 0x3: return ins_gen(c, insn);
            case 0x5: return umov_smov(c, insn, 1);
            case 0x7: return umov_smov(c, insn, 0);
            default: return -1;
            }
        }
    }
    if (top == 0x0F) { /* modified immediate, or shift by immediate: both have bit 10 set */
        if (!((insn >> 10) & 1) || ((insn >> 23) & 1))
            return -1;
        if (((insn >> 19) & 0xF) == 0)
            return modified_imm(c, insn);
        return shift_imm(c, insn);
    }
    return -1;
}
