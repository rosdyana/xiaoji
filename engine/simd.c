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

/* Operand classes for the NaN rules (FPProcessNaNs): raw bits of the given precision. */
static int isnan_b(uint64_t v, int dbl) { return dbl ? nan_d(v) : nan_s((uint32_t)v); }
static int isqnan_b(uint64_t v, int dbl)
{
    return isnan_b(v, dbl) && (v & (dbl ? FP_QUIET_D : FP_QUIET_S));
}
static uint64_t quiet_b(uint64_t v, int dbl) { return v | (dbl ? FP_QUIET_D : FP_QUIET_S); }

/* Two NaN operands (at least one is NaN): the first signalling NaN, quieted, else
 * the first quiet NaN. */
static uint64_t nan_pick(uint64_t a, uint64_t b, int dbl)
{
    if (isnan_b(a, dbl) && !isqnan_b(a, dbl))
        return quiet_b(a, dbl);
    if (isnan_b(b, dbl) && !isqnan_b(b, dbl))
        return quiet_b(b, dbl);
    return isqnan_b(a, dbl) ? a : b;
}

/* The NaN rules of a vector FP lane. Host arithmetic gives the x86 sign (negative) to a NaN made by an invalid
 * operation; the Arm default NaN is positive. With no NaN operand, a NaN result is the default NaN. Otherwise a NaN
 * operand propagates, quieted if it is signalling. The order: a signalling NaN outranks a quiet one; within a class
 * the fused forms (nin 3) take the addend first, then the multiplicands x and y, and the other forms take x, then y.
 * This is the order the arm64 hardware gives (checked with fmaf on an arm64 Mac). FMAXNM and FMINNM (nm) return the
 * number when one operand is a NaN, unless that NaN is signalling. */
static uint64_t fp_vec_nan(uint64_t res, int dbl, int nm, int nin, uint64_t x, uint64_t y, uint64_t acc)
{
    uint64_t v[3] = {x, y, acc}, ord[3] = {x, y, acc};
    if (nin == 3) {
        ord[0] = acc;
        ord[1] = x;
        ord[2] = y;
    }
    int any = 0;
    for (int i = 0; i < nin; i++)
        any |= isnan_b(v[i], dbl);
    if (!any)
        return isnan_b(res, dbl) ? (dbl ? FP_DEFAULT_NAN_D : FP_DEFAULT_NAN_S) : res;
    if (nm && nin == 2 && isnan_b(x, dbl) != isnan_b(y, dbl)) {
        int xn = isnan_b(x, dbl);
        uint64_t nanv = xn ? x : y;
        if (!isqnan_b(nanv, dbl))
            return quiet_b(nanv, dbl);
        return xn ? y : x;
    }
    for (int i = 0; i < nin; i++)
        if (isnan_b(ord[i], dbl) && !isqnan_b(ord[i], dbl))
            return quiet_b(ord[i], dbl);
    for (int i = 0; i < nin; i++)
        if (isqnan_b(ord[i], dbl))
            return ord[i];
    return res; /* not reached: some operand is a NaN */
}

/* FMAX/FMIN choice between two non-NaN values, returns 1 for b, 0 for a. Zeros
 * of either sign compare equal, so FMAX prefers +0 and FMIN prefers -0. */
static int pick_minmax(double x, double y, int is_min)
{
    if (x > y)
        return is_min;
    if (y > x)
        return !is_min;
    if (x == 0.0) {
        int xn = signbit(x), yn = signbit(y);
        if (is_min)
            return xn ? 0 : (yn ? 1 : 0);
        return xn ? 1 : 0;
    }
    return 0; /* equal and nonzero: the same value */
}

/* Two-source: FMUL (0), FDIV (1), FADD (2), FSUB (3), FMAX (4), FMIN (5), FMAXNM (6),
 * FMINNM (7), FNMUL (8). A NaN operand propagates (see nan_pick); the NM variants
 * return the other operand when only one is a quiet NaN; a NaN from non-NaN
 * operands is the default NaN. */
static int fp_arith2(cpu_t *c, uint32_t insn)
{
    unsigned type = (insn >> 22) & 3, rm = (insn >> 16) & 31, opc = (insn >> 12) & 0xF;
    unsigned rn = (insn >> 5) & 31, rd = insn & 31;
    if (opc > 8 || type > 1 || !fp_default(c))
        return -1;
    int dbl = type == 1;
    uint64_t a = c->v[rn][0], b = c->v[rm][0], r;
    if (!dbl) {
        a &= 0xFFFFFFFFu;
        b &= 0xFFFFFFFFu;
    }
    int an = isnan_b(a, dbl), bn = isnan_b(b, dbl);
    if (an || bn) {
        if ((opc == 6 || opc == 7) && isqnan_b(a, dbl) && !bn)
            r = b;
        else if ((opc == 6 || opc == 7) && isqnan_b(b, dbl) && !an)
            r = a;
        else
            r = nan_pick(a, b, dbl);
        if (opc == 8) /* FNMUL negates the result, including a NaN (as the hardware does) */
            r ^= dbl ? (1ull << 63) : (1ull << 31);
    } else if (opc >= 4 && opc <= 7) {
        double x = dbl ? as_d(a) : as_f((uint32_t)a);
        double y = dbl ? as_d(b) : as_f((uint32_t)b);
        r = pick_minmax(x, y, opc & 1) ? b : a;
    } else if (dbl) {
        double da = as_d(a), db = as_d(b), dr;
        if (opc == 8)
            dr = -(da * db);
        else
            dr = opc == 0 ? da * db : opc == 1 ? da / db : opc == 2 ? da + db : da - db;
        r = nan_d(d_bits(dr)) ? FP_DEFAULT_NAN_D : d_bits(dr);
    } else {
        float fa = as_f((uint32_t)a), fb = as_f((uint32_t)b), fr;
        if (opc == 8)
            fr = -(fa * fb);
        else
            fr = opc == 0 ? fa * fb : opc == 1 ? fa / fb : opc == 2 ? fa + fb : fa - fb;
        r = nan_s(f_bits(fr)) ? FP_DEFAULT_NAN_S : f_bits(fr);
    }
    if (dbl)
        put_fd(c, rd, r);
    else
        put_fs(c, rd, (uint32_t)r);
    return 0;
}

/* NZCV for a floating-point compare of two raw values of the given precision. */
static uint32_t fcmp_nzcv(unsigned type, uint64_t a, uint64_t b)
{
    double x, y;
    if (type == 0) {
        x = as_f((uint32_t)a);
        y = as_f((uint32_t)b);
    } else {
        x = as_d(a);
        y = as_d(b);
    }
    if (x != x || y != y)
        return 0x30000000u; /* unordered: C and V */
    if (x == y)
        return 0x60000000u; /* equal: Z and C */
    if (x < y)
        return 0x80000000u; /* less: N */
    return 0x20000000u;     /* greater: C */
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
    c->nzcv = fcmp_nzcv(type, c->v[rn][0], zero ? 0 : c->v[rm][0]);
    return 0;
}

/* FCCMP and FCCMPE: compare when cond holds, else set NZCV from the immediate. */
static int fp_ccmp(cpu_t *c, uint32_t insn)
{
    unsigned type = (insn >> 22) & 3, rm = (insn >> 16) & 31, cond = (insn >> 12) & 0xF;
    unsigned rn = (insn >> 5) & 31, nzcv = insn & 0xF;
    if (type > 1 || !fp_default(c))
        return -1;
    if (cond_true(c->nzcv, cond))
        c->nzcv = fcmp_nzcv(type, c->v[rn][0], c->v[rm][0]);
    else
        c->nzcv = nzcv << 28;
    return 0;
}

/* FP 1-source (opcode in bits 20:15): FMOV, FABS, FNEG, FSQRT, FCVT to single or
 * double (half precision is not implemented), and FRINTN/P/M/Z/A/X/I. */
static int fp_1src(cpu_t *c, uint32_t insn)
{
    unsigned type = (insn >> 22) & 3, opcode = (insn >> 15) & 0x3F;
    unsigned rn = (insn >> 5) & 31, rd = insn & 31;
    if (type > 1 || !fp_default(c))
        return -1;
    int dbl = type == 1;
    uint64_t a = c->v[rn][0];
    if (!dbl)
        a &= 0xFFFFFFFFu;
    if (opcode <= 2) { /* FMOV, FABS, FNEG: bit operations, NaNs are not changed */
        uint64_t sign = dbl ? (1ull << 63) : (1ull << 31), r = a;
        if (opcode == 1)
            r = a & ~sign;
        else if (opcode == 2)
            r = a ^ sign;
        if (dbl)
            put_fd(c, rd, r);
        else
            put_fs(c, rd, (uint32_t)r);
        return 0;
    }
    if (opcode == 3) { /* FSQRT: a negative operand gives the default NaN */
        uint64_t r;
        if (isnan_b(a, dbl)) {
            r = quiet_b(a, dbl);
        } else if (dbl) {
            double x = as_d(a);
            r = x < 0 ? FP_DEFAULT_NAN_D : d_bits(sqrt(x));
        } else {
            float x = as_f((uint32_t)a);
            r = x < 0 ? FP_DEFAULT_NAN_S : f_bits(sqrtf(x));
        }
        if (dbl)
            put_fd(c, rd, r);
        else
            put_fs(c, rd, (uint32_t)r);
        return 0;
    }
    if (opcode >= 4 && opcode <= 7) { /* FCVT: the destination type is opcode bits 1:0 */
        unsigned dst = opcode & 3;
        if (dst == 3 || dst == (unsigned)type)
            return -1; /* half precision, or no conversion */
        if (dst == 1) { /* single to double: exact, NaN payload kept at the top of the fraction */
            uint64_t r;
            if (isnan_b(a, 0))
                r = ((a >> 31) << 63) | (0x7FFull << 52) | ((a & 0x7FFFFFu) << 29) | FP_QUIET_D;
            else
                r = d_bits((double)as_f((uint32_t)a));
            put_fd(c, rd, r);
        } else { /* double to single: rounds to nearest, NaN keeps the top fraction bits */
            uint32_t r;
            if (isnan_b(a, 1))
                r = (uint32_t)(((a >> 63) << 31) | (0xFFu << 23) | ((a >> 29) & 0x7FFFFFu) | FP_QUIET_S);
            else
                r = f_bits((float)as_d(a));
            put_fs(c, rd, r);
        }
        return 0;
    }
    if (opcode >= 8 && opcode <= 15 && opcode != 13) { /* FRINT*: round to integral, NaNs quieted */
        uint64_t r;
        if (isnan_b(a, dbl))
            r = quiet_b(a, dbl);
        else if (dbl) {
            double x = as_d(a);
            double y = opcode == 8 || opcode == 14 || opcode == 15 ? rint(x) : opcode == 9 ? ceil(x)
                     : opcode == 10 ? floor(x) : opcode == 11 ? trunc(x) : round(x);
            r = d_bits(y);
        } else {
            float x = as_f((uint32_t)a);
            float y = opcode == 8 || opcode == 14 || opcode == 15 ? rintf(x) : opcode == 9 ? ceilf(x)
                     : opcode == 10 ? floorf(x) : opcode == 11 ? truncf(x) : roundf(x);
            r = f_bits(y);
        }
        if (dbl)
            put_fd(c, rd, r);
        else
            put_fs(c, rd, (uint32_t)r);
        return 0;
    }
    return -1;
}

/* FMADD, FMSUB, FNMADD, FNMSUB: Rd = +-Ra +- Rn*Rm with one rounding (fma). The
 * operands are negated by flipping the sign, which is exact. NaN operands are
 * chosen in the order Rn, Rm, Ra. */
static int fp_3src(cpu_t *c, uint32_t insn)
{
    unsigned type = (insn >> 22) & 3, o1 = (insn >> 21) & 1, rm = (insn >> 16) & 31, o0 = (insn >> 15) & 1;
    unsigned ra = (insn >> 10) & 31, rn = (insn >> 5) & 31, rd = insn & 31;
    if (type > 1 || !fp_default(c))
        return -1;
    int dbl = type == 1;
    uint64_t n = c->v[rn][0], m = c->v[rm][0], a = c->v[ra][0];
    if (!dbl) {
        n &= 0xFFFFFFFFu;
        m &= 0xFFFFFFFFu;
        a &= 0xFFFFFFFFu;
    }
    int prod_neg = (int)(o1 ^ o0), add_neg = (int)o1;
    if (isnan_b(n, dbl) || isnan_b(m, dbl) || isnan_b(a, dbl)) {
        /* The negations apply to the operands before a NaN is chosen, as on hardware. */
        uint64_t sign = dbl ? (1ull << 63) : (1ull << 31);
        uint64_t x[3] = {prod_neg ? n ^ sign : n, m, add_neg ? a ^ sign : a}, r = 0;
        int i;
        for (i = 0; i < 3; i++) {
            if (isnan_b(x[i], dbl) && !isqnan_b(x[i], dbl)) {
                r = quiet_b(x[i], dbl);
                break;
            }
        }
        if (i == 3) {
            for (i = 0; i < 3; i++) {
                if (isqnan_b(x[i], dbl)) {
                    r = x[i];
                    break;
                }
            }
        }
        if (dbl)
            put_fd(c, rd, r);
        else
            put_fs(c, rd, (uint32_t)r);
        return 0;
    }
    if (dbl) {
        double dn = as_d(n), dm = as_d(m), da = as_d(a);
        if (prod_neg)
            dn = -dn;
        if (add_neg)
            da = -da;
        double r = fma(dn, dm, da);
        put_fd(c, rd, nan_d(d_bits(r)) ? FP_DEFAULT_NAN_D : d_bits(r));
    } else {
        float fn = as_f((uint32_t)n), fm = as_f((uint32_t)m), fa = as_f((uint32_t)a);
        if (prod_neg)
            fn = -fn;
        if (add_neg)
            fa = -fa;
        float r = fmaf(fn, fm, fa);
        put_fs(c, rd, nan_s(f_bits(r)) ? FP_DEFAULT_NAN_S : f_bits(r));
    }
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
 * rounding mode rmode (0 nearest, 1 up, 2 down, 3 toward zero). FCVTAS/FCVTAU (opcode 4 and 5,
 * rmode 0) round to nearest with ties away from zero. Saturates; NaN gives 0. */
static int fp_to_int(cpu_t *c, uint32_t insn)
{
    unsigned sf = insn >> 31, type = (insn >> 22) & 3, rmode = (insn >> 19) & 3, uns = (insn >> 16) & 1;
    unsigned op = (insn >> 16) & 7;
    unsigned rn = (insn >> 5) & 31, rd = insn & 31;
    int away = op >= 4; /* FCVTAS, FCVTAU */
    if (type > 1 || !fp_default(c) || (away && rmode != 0))
        return -1;
    double x = type == 0 ? (double)as_f((uint32_t)c->v[rn][0]) : as_d(c->v[rn][0]);
    double r = away ? round(x) : rmode == 0 ? rint(x) : rmode == 1 ? ceil(x) : rmode == 2 ? floor(x) : trunc(x);
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

/* FCSEL: Vd = cond ? Vn : Vm, scalar, single or double. */
static int fp_csel(cpu_t *c, uint32_t insn)
{
    unsigned type = (insn >> 22) & 3, rm = (insn >> 16) & 31, cond = (insn >> 12) & 0xF;
    unsigned rn = (insn >> 5) & 31, rd = insn & 31;
    if (type > 1)
        return -1;
    unsigned src = cond_true(c->nzcv, cond) ? rn : rm;
    if (type == 0)
        put_fs(c, rd, (uint32_t)c->v[src][0]);
    else
        put_fd(c, rd, c->v[src][0]);
    return 0;
}

/* ---------- FMOV (scalar, immediate) ----------
 * VFPExpandImm from the Arm ARM. Single: sign imm8<7>, exponent NOT(b):Replicate(b,5):
 * imm8<5:4> (b = imm8<6>), fraction imm8<3:0>:Zeros(19). Double: exponent
 * NOT(b):Replicate(b,8):imm8<5:4>, fraction imm8<3:0>:Zeros(48). Type 2 is
 * unallocated; type 3 is half precision (FEAT_FP16), not implemented. */
/* VFPExpandImm: the single (32-bit) and double (64-bit) values of an 8-bit FP immediate. */
static uint32_t vfp_expand32(uint32_t imm8)
{
    uint32_t sign = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1, lo = (imm8 >> 4) & 3, frac = imm8 & 0xF;
    uint32_t e = ((b ^ 1) << 7) | (b ? 0x1Fu << 2 : 0) | lo;
    return (sign << 31) | (e << 23) | (frac << 19);
}

static uint64_t vfp_expand64(uint32_t imm8)
{
    uint64_t sign = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1, lo = (imm8 >> 4) & 3, frac = imm8 & 0xF;
    uint64_t e = ((b ^ 1) << 10) | (b ? 0xFFull << 2 : 0) | lo;
    return (sign << 63) | (e << 52) | (frac << 48);
}

static int fmov_imm(cpu_t *c, uint32_t insn)
{
    unsigned type = (insn >> 22) & 3, rd = insn & 31;
    uint32_t imm8 = (insn >> 13) & 0xFF;
    if (type == 0) {
        put_fs(c, rd, vfp_expand32(imm8));
        return 0;
    }
    if (type == 1) {
        put_fd(c, rd, vfp_expand64(imm8));
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
    if (cmode == 0xF) {
        /* FMOV (vector, immediate): op 0 is single precision (2S or 4S), op 1 is double (2D, Q 1
         * only). o2 1 is half precision (FEAT_FP16), not implemented. The value is replicated into
         * every element of the register. */
        if (((insn >> 11) & 1) || (op && !q))
            return -1;
        uint64_t w = op ? vfp_expand64(imm8) : (uint64_t)vfp_expand32(imm8) * 0x0000000100000001ull;
        memcpy(r, &w, 8);
        memcpy(r + 8, &w, 8);
        wr_v(c, rd, r, dsz);
        return 0;
    }
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

/* Lane access for the by-element forms: a lane of esz bytes at byte offset off. */
static uint64_t el_rd(const uint8_t *b, unsigned off, unsigned esz)
{
    uint64_t v = 0;
    memcpy(&v, b + off, esz);
    return v;
}

static void el_wr(uint8_t *b, unsigned off, unsigned esz, uint64_t v)
{
    memcpy(b + off, &v, esz);
}

/* FMULX: like FMUL, except that zero times infinity is +-2.0 (sign: the xor of the signs). */
static float fmulx_f(float x, float y)
{
    if ((x == 0 && isinf(y)) || (isinf(x) && y == 0))
        return (signbit(x) != 0) != (signbit(y) != 0) ? -2.0f : 2.0f;
    return x * y;
}

static double fmulx_d(double x, double y)
{
    if ((x == 0 && isinf(y)) || (isinf(x) && y == 0))
        return (signbit(x) != 0) != (signbit(y) != 0) ? -2.0 : 2.0;
    return x * y;
}

/* FMAX, FMIN and the NM forms on doubles (the float forms are exact in double). A NaN operand
 * propagates for FMAX and FMIN; for the NM forms the other operand is returned when one is a quiet NaN.
 * Zeros of opposite sign: the maximum is +0 and the minimum is -0. */
static double fp_maxmin(double x, double y, int is_min, int nm)
{
    if (isnan(x) || isnan(y)) {
        if (nm && isnan(x) != isnan(y))
            return isnan(x) ? y : x;
        return isnan(x) ? x : y;
    }
    if (x == 0 && y == 0)
        return is_min ? (signbit(x) ? x : y) : (signbit(x) ? y : x);
    return is_min ? (x < y ? x : y) : (x > y ? x : y);
}

/* Vector floating-point three-same (opcodes 0x18 to 0x1F; single with sz=0, double with sz=1,
 * the arrangement 2S, 4S or 2D). The pair of U and a selects the operation (ARM DDI 0487,
 * "Advanced SIMD three same"). Pairwise forms (FADDP, FMAXP, ...) take adjacent pairs of the
 * concatenation Vn:Vm. Compares write all-ones or zero per lane. NaNs: FMAX and FMIN return the
 * first NaN operand; FMAXNM and FMINNM return the other operand when one is a NaN. */
/* The vector form (scalar == 0) and the scalar form (scalar == 1: lane 0 only, the rest of Vd zeroed, no pairs). */
static int fp_three_same_impl(cpu_t *c, uint32_t insn, int scalar)
{
    unsigned q = (insn >> 30) & 1, u = (insn >> 29) & 1, a = (insn >> 23) & 1, sz = (insn >> 22) & 1;
    unsigned rm = (insn >> 16) & 31, opc = (insn >> 11) & 0x1F, rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned esz = sz ? 8 : 4;
    unsigned dsz = scalar ? esz : (q ? 16 : 8), lanes = dsz / esz;
    if ((!scalar && sz && !q) || !fp_default(c))
        return -1;
    enum { OP_NONE, OP_MAXNM, OP_MINNM, OP_MLA, OP_MLS, OP_ADD, OP_SUB, OP_ABD, OP_MULX, OP_MUL,
           OP_CEQ, OP_CGE, OP_CGT, OP_ACGE, OP_ACGT, OP_MAX, OP_MIN, OP_RECPS, OP_RSQRTS, OP_DIV };
    int pair = 0, op = OP_NONE;
    switch (opc) {
    case 0x18: pair = u; op = a ? OP_MINNM : OP_MAXNM; break;
    case 0x19: if (u) return -1; op = a ? OP_MLS : OP_MLA; break;
    case 0x1A: pair = u && !a; op = !u ? (a ? OP_SUB : OP_ADD) : (a ? OP_ABD : OP_ADD); break;
    case 0x1B: if (!u) { op = OP_MULX; } else if (!a) { op = OP_MUL; } else return -1; break;
    case 0x1C: op = !u ? OP_CEQ : a ? OP_CGT : OP_CGE; break;
    case 0x1D: if (!u) return -1; op = a ? OP_ACGT : OP_ACGE; break;
    case 0x1E: pair = u; op = a ? OP_MIN : OP_MAX; break;
    case 0x1F: if (!u) { op = a ? OP_RSQRTS : OP_RECPS; } else { if (a) return -1; op = OP_DIV; } break;
    default: return -1;
    }
    if (pair && !(opc == 0x18 || opc == 0x1E || opc == 0x1A))
        return -1;
    if (opc == 0x1A && u && !a) { op = OP_ADD; pair = 1; }
    if (opc == 0x18 && u) pair = 1;
    if (opc == 0x1E && u) pair = 1;
    if (scalar && pair)
        return -1; /* the pairwise forms are vector-only */
    uint8_t n[16], m[16], d[16], r[16];
    rd_v(c, rn, n);
    rd_v(c, rm, m);
    rd_v(c, rd, d);
    memset(r, 0, sizeof r);
    for (unsigned i = 0; i < lanes; i++) {
        uint64_t x, y, acc = el_rd(d, i * esz, esz), res = 0;
        if (pair) {
            unsigned k0 = 2 * i, k1 = 2 * i + 1;
            x = k0 < lanes ? el_rd(n, k0 * esz, esz) : el_rd(m, (k0 - lanes) * esz, esz);
            y = k1 < lanes ? el_rd(n, k1 * esz, esz) : el_rd(m, (k1 - lanes) * esz, esz);
        } else {
            x = el_rd(n, i * esz, esz);
            y = el_rd(m, i * esz, esz);
        }
        double dx = sz ? as_d(x) : (double)as_f((uint32_t)x), dy = sz ? as_d(y) : (double)as_f((uint32_t)y);
        double dacc = sz ? as_d(acc) : (double)as_f((uint32_t)acc);
        int is_mask = op == OP_CEQ || op == OP_CGE || op == OP_CGT || op == OP_ACGE || op == OP_ACGT;
        if (is_mask) {
            int ok;
            if (op == OP_CEQ) ok = dx == dy;
            else if (op == OP_CGE) ok = dx >= dy;
            else if (op == OP_CGT) ok = dx > dy;
            else if (op == OP_ACGE) ok = fabs(dx) >= fabs(dy);
            else ok = fabs(dx) > fabs(dy);
            res = ok ? (sz ? ~0ull : 0xFFFFFFFFull) : 0;
        } else {
            double v;
            switch (op) {
            case OP_MLA: v = dacc + dx * dy; break; /* fused below */
            case OP_MLS: v = dacc - dx * dy; break;
            case OP_ADD: v = dx + dy; break;
            case OP_SUB: v = dx - dy; break;
            case OP_ABD: v = fabs(dx - dy); break;
            case OP_MULX: v = sz ? fmulx_d(dx, dy) : (double)fmulx_f((float)dx, (float)dy); break;
            case OP_MUL: v = dx * dy; break;
            case OP_DIV: v = dx / dy; break;
            case OP_RECPS: v = 2.0 - dx * dy; break;
            case OP_RSQRTS: v = (3.0 - dx * dy) / 2.0; break;
            case OP_MAX: v = fp_maxmin(dx, dy, 0, 0); break;
            case OP_MIN: v = fp_maxmin(dx, dy, 1, 0); break;
            case OP_MAXNM: v = fp_maxmin(dx, dy, 0, 1); break;
            case OP_MINNM: v = fp_maxmin(dx, dy, 1, 1); break;
            default: return -1;
            }
            if (sz) {
                double out = v;
                if (op == OP_MLA) out = fma(dx, dy, dacc);
                else if (op == OP_MLS) out = fma(-dx, dy, dacc);
                else if (op == OP_MULX) out = fmulx_d(dx, dy);
                res = d_bits(out);
            } else {
                float out = (float)v;
                if (op == OP_MLA) out = fmaf((float)dx, (float)dy, (float)dacc);
                else if (op == OP_MLS) out = fmaf(-(float)dx, (float)dy, (float)dacc);
                else if (op == OP_MULX) out = fmulx_f((float)dx, (float)dy);
                else if (op == OP_RECPS || op == OP_RSQRTS || op == OP_DIV || op == OP_MUL || op == OP_ADD ||
                         op == OP_SUB || op == OP_ABD || op == OP_MAX || op == OP_MIN || op == OP_MAXNM ||
                         op == OP_MINNM)
                    out = (float)v;
                res = f_bits(out);
            }
            /* FMLS negates the product's first operand before a NaN is chosen, as fp_3src does */
            int fused = op == OP_MLA || op == OP_MLS;
            uint64_t sgn = sz ? (1ull << 63) : 0x80000000ull;
            res = fp_vec_nan(res, (int)sz, op == OP_MAXNM || op == OP_MINNM, fused ? 3 : 2,
                             op == OP_MLS ? x ^ sgn : x, y, acc);
        }
        el_wr(r, i * esz, esz, res);
    }
    wr_v(c, rd, r, scalar ? 16 : dsz); /* a scalar result zeroes the rest of the register */
    return 0;
}

static int fp_three_same(cpu_t *c, uint32_t insn)
{
    return fp_three_same_impl(c, insn, 0);
}

static int three_same(cpu_t *c, uint32_t insn)
{
    if (((insn >> 11) & 0x1F) >= 0x18) /* vector floating point */
        return fp_three_same(c, insn);
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
    case 0x00: /* SHADD (u=0), UHADD (u=1): halving add, rounding down */
    case 0x02: /* SRHADD (u=0), URHADD (u=1): halving add, rounding up */
    case 0x04: /* SHSUB (u=0), UHSUB (u=1): halving subtract, rounding down */
        for (unsigned i = 0; i < lanes; i++) {
            uint64_t a = getel(n, esz, i), b = getel(m, esz, i), q;
            if (!u) {
                int64_t x = sext_el(a, esz), y = sext_el(b, esz);
                q = (uint64_t)(opc == 0x00 ? (x >> 1) + (y >> 1) + ((x & y) & 1)
                  : opc == 0x02 ? (x >> 1) + (y >> 1) + ((x | y) & 1)
                  : (x >> 1) - (y >> 1) - ((~x & y) & 1));
            } else {
                q = opc == 0x00 ? (a >> 1) + (b >> 1) + ((a & b) & 1)
                  : opc == 0x02 ? (a >> 1) + (b >> 1) + ((a | b) & 1)
                  : (a >> 1) - (b >> 1) - ((~a & b) & 1);
            }
            putel(r, esz, i, q);
        }
        break;
    case 0x08: /* SSHL (u=0), USHL (u=1): shift by the signed low byte of the shift element */
    case 0x0A: /* SRSHL (u=0), URSHL (u=1): the same, rounding right shifts */
        for (unsigned i = 0; i < lanes; i++) {
            uint64_t a = getel(n, esz, i), res = 0;
            int s = (int8_t)(getel(m, esz, i) & 0xFF);
            unsigned bits = 8 * esz;
            if (s >= 0) {
                res = (unsigned)s >= bits ? 0 : a << s;
            } else {
                unsigned k = (unsigned)-s;
                int64_t sa = sext_el(a, esz);
                if (opc == 0x08) {
                    if (!u)
                        res = (uint64_t)(k >= 64 ? (sa < 0 ? -1 : 0) : (sa >> k));
                    else
                        res = k >= 64 ? 0 : a >> k;
                } else if (k >= 64) {
                    res = 0;
                } else if (!u) {
                    res = (uint64_t)((sa >> k) + ((sa >> (k - 1)) & 1));
                } else {
                    res = (a >> k) + ((a >> (k - 1)) & 1);
                }
            }
            putel(r, esz, i, res);
        }
        break;
    case 0x0E: /* SABD (u=0), UABD (u=1): absolute difference */
    case 0x0F: /* SABA (u=0), UABA (u=1): absolute difference added to Vd */
        for (unsigned i = 0; i < lanes; i++) {
            uint64_t a = getel(n, esz, i), b = getel(m, esz, i), diff;
            if (!u) {
                int64_t x = sext_el(a, esz), y = sext_el(b, esz);
                diff = x > y ? (uint64_t)x - (uint64_t)y : (uint64_t)y - (uint64_t)x;
            } else {
                diff = a > b ? a - b : b - a;
            }
            putel(r, esz, i, opc == 0x0E ? diff : getel(d, esz, i) + diff);
        }
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
    /* Saturating doubling forms (opc 9, 11, 13) need the QC flag, which is not kept;
     * opc 15 and U=1 with PMULL are unallocated. */
    if (opc == 9 || opc == 11 || opc == 13 || opc == 15 || (u && opc == 14))
        return -1;
    uint8_t n[16], m[16], d[16], r[16];
    rd_v(c, rn, n);
    rd_v(c, rm, m);
    rd_v(c, rd, d);
    memset(r, 0, sizeof r);
    int sgn = !u;

    if (opc == 4 || opc == 6) { /* ADDHN, RADDHN (u=1), SUBHN, RSUBHN (u=1): narrowing, the high half */
        /* size is the narrow result's element size (as for XTN): the sources are twice as wide */
        if (size == 3)
            return -1;
        unsigned esz_s = 2u << size, h = 4 * esz_s, lanes = 16 / esz_s;
        unsigned dst_esz = esz_s / 2;
        uint64_t mb = h * 2 == 64 ? ~0ull : ((1ull << (2 * h)) - 1);
        uint64_t mh = (1ull << h) - 1;
        uint64_t rc = u ? (1ull << (h - 1)) : 0; /* rounding constant of the R forms */
        unsigned off = q ? 8 : 0;
        if (q)
            memcpy(r, d, 8); /* the "2" forms keep the low half of Rd */
        for (unsigned i = 0; i < lanes; i++) {
            uint64_t a = getel(n, esz_s, i), b = getel(m, esz_s, i), v;
            v = opc == 4 ? (a + b + rc) & mb : (a - b + rc) & mb;
            putel(r + off, dst_esz, i, (v >> h) & mh);
        }
        wr_v(c, rd, r, 16);
        return 0;
    }

    if (size == 3)
        return -1; /* 2D sources would need a 128-bit result element */
    unsigned esz = 1u << size, dst_esz = 2 * esz, lanes = 8 / esz;
    unsigned half = q ? 8 : 0;
    int wide = opc == 1 || opc == 3; /* SADDW, SSUBW: the first operand is already wide */
    for (unsigned i = 0; i < lanes; i++) {
        uint64_t a, b;
        if (wide)
            a = sgn ? (uint64_t)sext_el(getel(n, dst_esz, i), dst_esz) : getel(n, dst_esz, i);
        else
            a = sgn ? (uint64_t)sext_el(getel(n + half, esz, i), esz) : getel(n + half, esz, i);
        b = sgn ? (uint64_t)sext_el(getel(m + half, esz, i), esz) : getel(m + half, esz, i);
        uint64_t acc = getel(d, dst_esz, i), res = 0;
        uint64_t diff = sgn ? (uint64_t)((int64_t)a >= (int64_t)b ? (int64_t)a - (int64_t)b : (int64_t)b - (int64_t)a)
                            : (a >= b ? a - b : b - a);
        switch (opc) {
        case 0: res = a + b; break;                 /* SADDL, UADDL */
        case 1: res = a + b; break;                 /* SADDW, UADDW */
        case 2: res = a - b; break;                 /* SSUBL, USUBL */
        case 3: res = a - b; break;                 /* SSUBW, USUBW */
        case 5: res = acc + diff; break;            /* SABAL, UABAL */
        case 7: res = diff; break;                  /* SABDL, UABDL */
        case 8: res = acc + a * b; break;           /* SMLAL, UMLAL */
        case 10: res = acc - a * b; break;          /* SMLSL, UMLSL */
        case 12: res = a * b; break;                /* SMULL, UMULL */
        default: return -1;
        }
        putel(r, dst_esz, i, res);
    }
    wr_v(c, rd, r, 16);
    return 0;
}

/* ---------- two registers, miscellaneous ---------- */

static uint64_t to_int_sat(double x, unsigned mode, unsigned bits, int uns);

/* Vector floating-point two-register miscellaneous (0 Q U 01110 a 10000 opcode 10): FABS and FNEG
 * (opcode 01111, a=1), FRINT (11000, 11001), FCVT to integer (11010, 11011, 11100) and SCVTF and
 * UCVTF (11101, a=0), FSQRT (11111, U=1, a=1). Single (sz=0, 2S or 4S) and double (sz=1, 2D). */
static int fp_two_misc(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, u = (insn >> 29) & 1, a = (insn >> 23) & 1, sz = (insn >> 22) & 1;
    unsigned opc = (insn >> 12) & 0x1F, rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned dsz = q ? 16 : 8, esz = sz ? 8 : 4, lanes = dsz / esz;
    if ((sz && !q) || !fp_default(c))
        return -1;
    uint8_t n[16], r[16];
    rd_v(c, rn, n);
    memset(r, 0, sizeof r);
    for (unsigned i = 0; i < lanes; i++) {
        uint64_t xb = el_rd(n, i * esz, esz), out;
        double x = sz ? as_d(xb) : (double)as_f((uint32_t)xb);
        if (opc == 0x0F && a) { /* FABS (U=0), FNEG (U=1) */
            if (sz)
                out = u ? xb ^ (1ull << 63) : xb & ~(1ull << 63);
            else
                out = u ? (xb ^ 0x80000000u) : (xb & 0x7FFFFFFFu);
        } else if (opc == 0x1F && u && a) { /* FSQRT */
            out = fp_vec_nan(sz ? d_bits(sqrt(x)) : f_bits(sqrtf(as_f((uint32_t)xb))), (int)sz, 0, 1, xb, 0, 0);
        } else if (opc == 0x1D && !a) { /* SCVTF (U=0), UCVTF (U=1) from an integer lane */
            if (sz)
                out = d_bits(u ? (double)xb : (double)(int64_t)xb);
            else
                out = f_bits(u ? (float)(uint32_t)xb : (float)(int32_t)(uint32_t)xb);
        } else if (opc == 0x18 || opc == 0x19) { /* FRINT: N, P, A (0x18); M, Z, X, I (0x19) */
            double v;
            if (opc == 0x18 && !u && !a) v = rint(x);      /* FRINTN */
            else if (opc == 0x18 && !u && a) v = ceil(x);  /* FRINTP */
            else if (opc == 0x18 && u && !a) v = round(x); /* FRINTA */
            else if (opc == 0x19 && !u && !a) v = floor(x); /* FRINTM */
            else if (opc == 0x19 && !u && a) v = trunc(x);  /* FRINTZ */
            else if (opc == 0x19 && u) v = rint(x);         /* FRINTX, FRINTI */
            else return -1;
            out = sz ? d_bits(v) : f_bits((float)v);
        } else if (opc == 0x1A || opc == 0x1B || opc == 0x1C) { /* FCVT{N,P}, FCVT{M,Z}, FCVTA{S,U} */
            unsigned mode = opc == 0x1A ? (a ? 2 : 0) : opc == 0x1B ? (a ? 3 : 1) : 4;
            if (opc == 0x1C && a)
                return -1;
            out = to_int_sat(x, mode, esz * 8, (int)u);
        } else {
            return -1;
        }
        el_wr(r, i * esz, esz, out);
    }
    wr_v(c, rd, r, dsz);
    return 0;
}

static int two_misc(cpu_t *c, uint32_t insn)
{
    {
        unsigned fopc = (insn >> 12) & 0x1F;
        if (fopc >= 0x18 || (fopc == 0x0F && ((insn >> 23) & 1)))
            return fp_two_misc(c, insn);
    }
    unsigned q = (insn >> 30) & 1, u = (insn >> 29) & 1, size = (insn >> 22) & 3;
    unsigned opc = (insn >> 12) & 0x1F, rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned dsz = q ? 16 : 8, esz = 1u << size, lanes = dsz / esz;
    uint8_t n[16], r[16], d[16];
    rd_v(c, rn, n);
    memset(r, 0, sizeof r);

    switch (opc) {
    case 0x00: { /* REV64 (u=0), REV32 (u=1): reverse the elements inside each 64- or 32-bit container */
        if (size == 3 || (u && size == 2))
            return -1;
        unsigned per = (u ? 4 : 8) / esz;
        for (unsigned i = 0; i < lanes; i++) {
            unsigned base = (i / per) * per;
            unsigned j = base + (per - 1 - (i - base));
            putel(r, esz, i, getel(n, esz, j));
        }
        break;
    }
    case 0x01: /* REV16 (u=0, size 0): swap the two bytes of each halfword */
        if (u || size != 0)
            return -1;
        for (unsigned i = 0; i < dsz; i += 2) {
            r[i] = n[i + 1];
            r[i + 1] = n[i];
        }
        break;
    case 0x04: { /* CLS (u=0): leading sign bits, not counting the sign bit; CLZ (u=1) */
        if (size == 3)
            return -1;
        unsigned bits = 8 * esz;
        uint64_t mask = emask(esz);
        for (unsigned i = 0; i < lanes; i++) {
            uint64_t v = getel(n, esz, i) & mask;
            int neg = !u && ((v >> (bits - 1)) & 1);
            uint64_t x = neg ? (~v & mask) : v; /* leading zeros of x, then one less for CLS */
            unsigned lz = 0;
            while (lz < bits && !((x >> (bits - 1 - lz)) & 1))
                lz++;
            putel(r, esz, i, u ? lz : lz - 1);
        }
        break;
    }
    case 0x06: { /* SADALP (u=0), UADALP (u=1): pairwise sums added to the wide elements of Rd */
        if (size == 3)
            return -1;
        unsigned dst_esz = 2 * esz;
        rd_v(c, rd, d);
        for (unsigned i = 0; i < lanes / 2; i++) {
            uint64_t a = getel(n, esz, 2 * i), b = getel(n, esz, 2 * i + 1);
            uint64_t sum = u ? a + b : (uint64_t)(sext_el(a, esz) + sext_el(b, esz));
            putel(r, dst_esz, i, getel(d, dst_esz, i) + sum);
        }
        break;
    }
    case 0x0B: /* ABS (u=0), NEG (u=1) */
        for (unsigned i = 0; i < lanes; i++) {
            uint64_t v = getel(n, esz, i);
            int neg = u || sext_el(v, esz) < 0;
            putel(r, esz, i, neg ? 0 - v : v);
        }
        break;
    case 0x13: { /* SHLL (u=1), SHLL2 when q: widen each element and shift left by its width */
        if (!u || size == 3)
            return -1;
        unsigned src_esz = esz, dst_esz = 2 * esz, src_lanes = 8 / esz;
        unsigned half = q ? 8 : 0;
        for (unsigned i = 0; i < src_lanes; i++)
            putel(r, dst_esz, i, getel(n + half, src_esz, i) << (8 * src_esz));
        wr_v(c, rd, r, 16);
        return 0;
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
    case 0x05: /* CNT (u=0, size 0), NOT (u=1, size 0), RBIT (u=1, size 1): bits within each byte */
        if (!u && size == 0) {
            for (unsigned i = 0; i < dsz; i++)
                r[i] = (uint8_t)__builtin_popcount(n[i]);
        } else if (u && size == 0) {
            for (unsigned i = 0; i < dsz; i++)
                r[i] = (uint8_t)~n[i];
        } else if (u && size == 1) {
            for (unsigned i = 0; i < dsz; i++) {
                uint8_t x = n[i], y = 0;
                for (unsigned k = 0; k < 8; k++)
                    y |= (uint8_t)(((x >> k) & 1) << (7 - k));
                r[i] = y;
            }
        } else {
            return -1;
        }
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
    case 0x12: { /* XTN (u=0), XTN2 when q: narrow the source's low elements; SQXTUN (u=1), SQXTUN2 when q: the
                  * signed source saturated to the unsigned range of the narrow element (QC is not kept, see below) */
        if (size == 3)
            return -1;
        unsigned src_esz = 2u << size; /* H, S or D source */
        unsigned src_lanes = 16 / src_esz;
        uint8_t half[8];
        memset(half, 0, sizeof half);
        for (unsigned i = 0; i < src_lanes; i++) {
            if (!u) {
                putel(half, esz, i, getel(n, src_esz, i) & emask(esz));
            } else {
                int64_t s = sext_el(getel(n, src_esz, i), src_esz);
                uint64_t v = s < 0 ? 0 : (uint64_t)s > emask(esz) ? emask(esz) : (uint64_t)s;
                putel(half, esz, i, v);
            }
        }
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
    case 0x14: { /* SQXTN (u=0), UQXTN (u=1), SQXTN2/UQXTN2 when q: the source saturated to the range of the narrow
                  * element (QC is not kept, see below) */
        if (size == 3)
            return -1;
        unsigned src_esz = 2u << size;
        unsigned src_lanes = 16 / src_esz;
        uint8_t half[8];
        memset(half, 0, sizeof half);
        for (unsigned i = 0; i < src_lanes; i++) {
            uint64_t raw = getel(n, src_esz, i), v;
            if (u) {
                v = raw > emask(esz) ? emask(esz) : raw;
            } else {
                int64_t s = sext_el(raw, src_esz);
                int64_t smax = (int64_t)(emask(esz) >> 1), smin = -smax - 1;
                v = (uint64_t)(s > smax ? smax : s < smin ? smin : s) & emask(esz);
            }
            putel(half, esz, i, v);
        }
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

/* Element of esz bytes from or to guest memory, zero-extended. */
static uint64_t mem_rd_el(uint64_t va, unsigned esz)
{
    switch (esz) {
    case 1: return mem_read8(va);
    case 2: return mem_read16(va);
    case 4: return mem_read32(va);
    default: return mem_read64(va);
    }
}

static void mem_wr_el(uint64_t va, unsigned esz, uint64_t v)
{
    switch (esz) {
    case 1: mem_write8(va, (uint8_t)v); break;
    case 2: mem_write16(va, (uint16_t)v); break;
    case 4: mem_write32(va, (uint32_t)v); break;
    default: mem_write64(va, v); break;
    }
}

int simd_ldst_multi(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, post = (insn >> 23) & 1, L = (insn >> 22) & 1;
    unsigned rm = (insn >> 16) & 31, opc = (insn >> 12) & 0xF;
    unsigned rn = (insn >> 5) & 31, rt = insn & 31;
    unsigned nregs;
    int inter = 0; /* LD2/3/4 and ST2/3/4: elements of the registers interleaved in memory */
    switch (opc) {
    case 0x7: nregs = 1; break; /* LD1/ST1 {Vt} */
    case 0xA: nregs = 2; break; /* {Vt, Vt2} */
    case 0x6: nregs = 3; break; /* {Vt, Vt2, Vt3} */
    case 0x2: nregs = 4; break; /* {Vt, Vt2, Vt3, Vt4} */
    case 0x8: nregs = 2; inter = 1; break; /* LD2/ST2 */
    case 0x4: nregs = 3; inter = 1; break; /* LD3/ST3 */
    case 0x0: nregs = 4; inter = 1; break; /* LD4/ST4 */
    default: return -1;
    }
    unsigned dsz = q ? 16 : 8;
    uint64_t addr = rn == 31 ? c->sp : c->x[rn];
    if (inter) {
        unsigned esz = 1u << ((insn >> 10) & 3);
        if (esz == 8 && !q)
            return -1; /* 1D with LD2/3/4 is reserved */
        unsigned elems = dsz / esz;
        if (L) {
            for (unsigned t = 0; t < nregs; t++) {
                unsigned reg = (rt + t) & 31;
                c->v[reg][0] = 0;
                c->v[reg][1] = 0;
            }
        }
        for (unsigned e = 0; e < elems; e++) {
            for (unsigned t = 0; t < nregs; t++) {
                unsigned reg = (rt + t) & 31;
                uint64_t va = addr + (uint64_t)(e * nregs + t) * esz;
                if (L)
                    putel((uint8_t *)c->v[reg], esz, e, mem_rd_el(va, esz));
                else
                    mem_wr_el(va, esz, getel((const uint8_t *)c->v[reg], esz, e));
            }
        }
    } else {
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

/* ---------- scalar AdvSIMD two-register miscellaneous (0x5E and 0x7E) ----------
 * ABS and NEG (64-bit), SCVTF and UCVTF (from a 32- or 64-bit integer in the
 * register), and FCVTNS..FCVTZU to a 32- or 64-bit integer, saturating. */

/* Float to integer with rounding mode (0 nearest, 1 down, 2 up, 3 toward zero,
 * 4 ties away), saturating to 32 or 64 bits. NaN gives 0. */
static uint64_t to_int_sat(double x, unsigned mode, unsigned bits, int uns)
{
    if (x != x)
        return 0;
    double r = mode == 0 ? rint(x) : mode == 1 ? floor(x) : mode == 2 ? ceil(x) : mode == 3 ? trunc(x) : round(x);
    if (uns) {
        if (r <= 0)
            return 0;
        if (r >= (bits == 64 ? 18446744073709551616.0 : 4294967296.0))
            return bits == 64 ? ~0ull : 0xFFFFFFFFull;
        return (uint64_t)r;
    }
    double hi = bits == 64 ? 9223372036854775808.0 : 2147483648.0;
    if (r >= hi)
        return bits == 64 ? (uint64_t)INT64_MAX : 0x7FFFFFFFull;
    if (r < -hi)
        return bits == 64 ? (uint64_t)INT64_MIN : 0x80000000ull;
    return bits == 64 ? (uint64_t)(int64_t)r : (uint64_t)(uint32_t)(int32_t)r;
}

/* ADDP (scalar), size 11, U 0: Dd = Vn.D[0] + Vn.D[1] as 64-bit integers. This is
 * the integer pairwise add, not a floating-point operation: no NaN or zero rules
 * apply, and the result is the sum modulo 2^64. */
static int scalar_addp(cpu_t *c, uint32_t insn)
{
    if (((insn >> 29) & 1) || ((insn >> 22) & 3) != 3)
        return -1;
    unsigned rn = (insn >> 5) & 31, rd = insn & 31;
    put_fd(c, rd, c->v[rn][0] + c->v[rn][1]);
    return 0;
}

/* The opcodes the architecture allocates in the scalar floating-point three-same group (U and a select the
 * operation): FMULX (U 0, opc 11011), FCMEQ (U 0, 11100), FCMGE (U 1, a 0, 11100), FCMGT (U 1, a 1, 11100),
 * FACGE (U 1, a 0, 11101), FACGT (U 1, a 1, 11101), FRECPS (U 0, a 0, 11111), FRSQRTS (U 0, a 1, 11111), and
 * FABD (U 1, a 1, 11010). Anything else in 0x18..0x1F is not an instruction here. */
static int scalar_fp3_allocated(uint32_t insn)
{
    unsigned u = (insn >> 29) & 1, a = (insn >> 23) & 1, opc = (insn >> 11) & 0x1F;
    switch (opc) {
    case 0x1A: return u && a;   /* FABD */
    case 0x1B: return !u;       /* FMULX */
    case 0x1C: return 1;        /* FCMEQ, FCMGE, FCMGT */
    case 0x1D: return u;        /* FACGE, FACGT */
    case 0x1F: return !u;       /* FRECPS, FRSQRTS */
    default: return 0;
    }
}

static int scalar_simd(cpu_t *c, uint32_t insn)
{
    /* Scalar floating-point three same (bit 21 and bit 10 set; the allocated opcodes, scalar_fp3_allocated). */
    if (((insn >> 21) & 1) && ((insn >> 10) & 1) && scalar_fp3_allocated(insn))
        return fp_three_same_impl(c, insn, 1);
    /* DUP (element), scalar: 01 0 11110000 imm5 0000 1 Rn Rd. The element goes to the low bits
     * of Vd and the rest of the register is zeroed. */
    if (((insn >> 21) & 0xFF) == 0xF0 && ((insn >> 10) & 0x1F) == 0x01) {
        unsigned log2, imm5 = (insn >> 16) & 0x1F, rn = (insn >> 5) & 31, rd = insn & 31;
        unsigned esz = imm5_size(imm5, &log2);
        if (!esz)
            return -1;
        uint8_t n[16], r[16];
        rd_v(c, rn, n);
        uint64_t v = getel(n, esz, imm5 >> (log2 + 1));
        memset(r, 0, sizeof r);
        putel(r, esz, 0, v);
        wr_v(c, rd, r, 16);
        return 0;
    }
    /* Pairwise scalar FP (U=1, bits 21:17 = 11000, bits 11:10 = 10): the two lanes of Vn, with
     * opcode 01100 (FMAXNMP, FMINNMP with a=1), 01101 (FADDP) and 01111 (FMAXP, FMINP with a=1). */
    if (((insn >> 29) & 1) && ((insn >> 17) & 0x1F) == 0x18 && ((insn >> 10) & 3) == 2) {
        unsigned a = (insn >> 23) & 1, sz = (insn >> 22) & 1, opc = (insn >> 12) & 0x1F;
        unsigned rn = (insn >> 5) & 31, rd = insn & 31;
        if (!fp_default(c) || a > 1 || !((opc == 0x0C) || (opc == 0x0D && !a) || (opc == 0x0F)))
            return -1;
        if (opc == 0x0D && a)
            return -1;
        unsigned esz = sz ? 8 : 4;
        uint8_t n[16];
        rd_v(c, rn, n);
        uint64_t xb = getel(n, esz, 0), yb = getel(n, esz, 1);
        if (sz) {
            double x = as_d(xb), y = as_d(yb), r;
            if (opc == 0x0D) r = x + y;
            else r = fp_maxmin(x, y, a, opc == 0x0C);
            put_fd(c, rd, d_bits(r));
        } else {
            float x = as_f((uint32_t)xb), y = as_f((uint32_t)yb), r;
            if (opc == 0x0D) r = x + y;
            else r = (float)fp_maxmin((double)x, (double)y, a, opc == 0x0C);
            put_fs(c, rd, f_bits(r));
        }
        return 0;
    }
    /* Pairwise scalar (bits 21:17 = 11000, bits 16:12 = 11011, bits 11:10 = 10): ADDP. */
    if (((insn >> 17) & 0x1F) == 0x18 && ((insn >> 12) & 0x1F) == 0x1B && ((insn >> 10) & 3) == 2)
        return scalar_addp(c, insn);
    /* Only the two-register miscellaneous form (bits 21:17 = 10000, bits 11:10 = 10). */
    if (!((insn >> 21) & 1) || ((insn >> 17) & 0x1F) != 0x10 || ((insn >> 10) & 3) != 2)
        return -1;
    unsigned u = (insn >> 29) & 1, a = (insn >> 23) & 1, sz = (insn >> 22) & 1;
    unsigned opc = (insn >> 12) & 0x1F, rn = (insn >> 5) & 31, rd = insn & 31;
    uint64_t x = c->v[rn][0];
    switch (opc) {
    case 0x0B: /* ABS (u=0), NEG (u=1), 64-bit only */
        if (!a || !sz)
            return -1;
        put_fd(c, rd, u || (int64_t)x < 0 ? 0 - x : x);
        return 0;
    case 0x1D: /* SCVTF (u=0), UCVTF (u=1) from a 32-bit (S) or 64-bit (D) integer */
        if (a)
            return -1;
        if (sz)
            put_fd(c, rd, d_bits(u ? (double)x : (double)(int64_t)x));
        else
            put_fs(c, rd, f_bits(u ? (float)(uint32_t)x : (float)(int32_t)(uint32_t)x));
        return 0;
    case 0x1A: /* FCVTNS/FCVTNU (a=0), FCVTPS/FCVTPU (a=1) */
    case 0x1B: /* FCVTMS/FCVTMU (a=0), FCVTZS/FCVTZU (a=1) */
    case 0x1C: /* FCVTAS/FCVTAU (a=0) */
    {
        if (opc == 0x1C && a)
            return -1;
        unsigned mode = opc == 0x1A ? (a ? 2 : 0) : opc == 0x1B ? (a ? 3 : 1) : 4;
        double v = sz ? as_d(x) : (double)as_f((uint32_t)x);
        unsigned bits = sz ? 64 : 32;
        uint64_t out = to_int_sat(v, mode, bits, (int)u);
        if (sz)
            put_fd(c, rd, out);
        else
            put_fs(c, rd, (uint32_t)out);
        return 0;
    }
    default:
        return -1;
    }
}

/* ---------- dispatch ---------- */

/* Vector x indexed element: 0 Q U 01111 size L M Rm opcode H 0 Rn Rd. Vm supplies one lane,
 * the index being H:L (32- and 64-bit lanes, Rm is M:Rm) or H:L:M (16-bit lanes, Rm is four
 * bits). FP (opcode 1, 5 and 9, with size bit 1 set): FMLA (u=0, opcode 1), FMLS (u=0, opcode 5),
 * FMUL (u=0, opcode 9) and FMULX (u=1, opcode 9). Integer: MUL (u=0, opcode 8), MLA (u=1,
 * opcode 0) and MLS (u=1, opcode 4), on 16- and 32-bit lanes. */
static int by_element(cpu_t *c, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1, u = (insn >> 29) & 1, size = (insn >> 22) & 3;
    unsigned l = (insn >> 21) & 1, m = (insn >> 20) & 1, h = (insn >> 11) & 1;
    unsigned rmlo = (insn >> 16) & 15, opc = (insn >> 12) & 0xF;
    unsigned rn = (insn >> 5) & 31, rd = insn & 31;
    unsigned dsz = q ? 16 : 8;
    uint8_t n[16], d[16], r[16], mv[16];
    rd_v(c, rn, n);
    rd_v(c, rd, d);
    memset(r, 0, sizeof r);
    if ((size & 2) && (opc == 1 || opc == 5 || opc == 9)) { /* FP: size bit 1 is set */
        int fp_op = (u == 0 && (opc == 1 || opc == 5 || opc == 9)) || (u == 1 && opc == 9);
        unsigned sz = size & 1;
        if (!fp_op || !fp_default(c) || (sz && (!q || l)))
            return -1;
        unsigned esz = sz ? 8 : 4, lanes = dsz / esz;
        unsigned idx = sz ? h : (h << 1) | l;
        rd_v(c, (m << 4) | rmlo, mv);
        uint64_t eb = el_rd(mv, idx * esz, esz);
        for (unsigned i = 0; i < lanes; i++) {
            uint64_t xb = el_rd(n, i * esz, esz), db = el_rd(d, i * esz, esz), rb;
            if (sz) {
                double x = as_d(xb), y = as_d(eb), acc = as_d(db);
                double res = opc == 1 ? fma(x, y, acc) : opc == 5 ? fma(-x, y, acc) : u ? fmulx_d(x, y) : x * y;
                rb = fp_vec_nan(d_bits(res), 1, 0, 3, opc == 5 ? xb ^ (1ull << 63) : xb, eb, db);
            } else {
                float x = as_f((uint32_t)xb), y = as_f((uint32_t)eb), acc = as_f((uint32_t)db);
                float res = opc == 1 ? fmaf(x, y, acc) : opc == 5 ? fmaf(-x, y, acc) : u ? fmulx_f(x, y) : x * y;
                rb = fp_vec_nan(f_bits(res), 0, 0, 3, opc == 5 ? xb ^ 0x80000000u : xb, eb, db);
            }
            el_wr(r, i * esz, esz, rb);
        }
        wr_v(c, rd, r, dsz);
        return 0;
    }
    /* Integer forms: 16-bit lanes (size 1) or 32-bit lanes (size 2). */
    if (!((u == 0 && opc == 8) || (u == 1 && (opc == 0 || opc == 4))) || (size != 1 && size != 2))
        return -1;
    unsigned esz = size == 1 ? 2 : 4, lanes = dsz / esz;
    unsigned idx = esz == 2 ? (h << 2) | (l << 1) | m : (h << 1) | l;
    unsigned rm = esz == 2 ? rmlo : (m << 4) | rmlo;
    rd_v(c, rm, mv);
    uint64_t mask = esz == 2 ? 0xFFFFull : 0xFFFFFFFFull;
    uint64_t e = el_rd(mv, idx * esz, esz);
    for (unsigned i = 0; i < lanes; i++) {
        uint64_t x = el_rd(n, i * esz, esz), acc = el_rd(d, i * esz, esz), res;
        if (opc == 8)
            res = x * e;
        else if (opc == 0)
            res = acc + x * e;
        else
            res = acc - x * e;
        el_wr(r, i * esz, esz, res & mask);
    }
    wr_v(c, rd, r, dsz);
    return 0;
}

/* Scalar shift by immediate (01 U 111110 immh immb opcode 1 Rn Rd): the 64-bit element, which is the
 * vector form with Q=0 (the upper half of the register is zeroed). Only SSHR and USHR (opcode 00000),
 * and SHL (opcode 01010, U=0) are allocated here; immh must select the 64-bit element (1xxx). */
static int scalar_shift_imm(cpu_t *c, uint32_t insn)
{
    unsigned immh = (insn >> 19) & 0xF, opc = (insn >> 11) & 0x1F, u = (insn >> 29) & 1;
    if (!((insn >> 10) & 1) || !(immh & 8) || !(opc == 0x00 || (opc == 0x0A && !u)))
        return -1;
    return shift_imm(c, insn & ~(1u << 30));
}

int simd_exec(cpu_t *c, uint32_t insn)
{
    unsigned top = (insn >> 24) & 0x1F; /* bits 28..24 */
    if (top == 0x1E) { /* scalar floating point: bits 30 and 29 are zero (0x5E and 0x7E are AdvSIMD) */
        if ((insn >> 30) & 1) /* scalar AdvSIMD: bit 31 is zero */
            return (insn >> 31) ? -1 : scalar_simd(c, insn);
        if (((insn >> 29) & 3) || !((insn >> 21) & 1))
            return -1;
        unsigned low6 = (insn >> 10) & 0x3F;
        if (low6 == 0) {
            /* opcode 0/1: FCVT{N,P,M,Z}{S,U}; 2/3: SCVTF, UCVTF; 6/7: FMOV general */
            unsigned op = (insn >> 16) & 7;
            if (op == 6 || op == 7)
                return fmov_gen(c, insn);
            if (op < 2 || op == 4 || op == 5)
                return fp_to_int(c, insn);
            if (op < 4)
                return fp_from_int(c, insn);
            return -1;
        }
        if (insn >> 31)
            return -1;
        if (((insn >> 10) & 0x1F) == 0x10) /* bits 14:10 = 10000: 1-source */
            return fp_1src(c, insn);
        if (((insn >> 10) & 7) == 4 && ((insn >> 5) & 0x1F) == 0) /* bits 12:10 = 100, Rn = 0: FMOV imm */
            return fmov_imm(c, insn);
        if ((low6 & 3) == 3)
            return fp_csel(c, insn);
        if ((low6 & 3) == 1)
            return fp_ccmp(c, insn);
        if ((low6 & 3) == 2 && ((insn >> 12) & 0xF) <= 8)
            return fp_arith2(c, insn);
        if (low6 == 0x08)
            return fp_cmp(c, insn);
        return -1;
    }
    if (top == 0x1F && !(insn >> 31) && ((insn >> 30) & 1)) /* scalar shift by immediate: 01 U 111110 */
        return scalar_shift_imm(c, insn);
    if (top == 0x1F) { /* FP 3-source: FMADD and relatives, bit 31 and bits 30:29 zero */
        if ((insn >> 31) || ((insn >> 29) & 3))
            return -1;
        return fp_3src(c, insn);
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
    if (top == 0x0F) { /* by element (bit 10 clear), modified immediate or shift by immediate */
        if (!((insn >> 10) & 1))
            return by_element(c, insn);
        if ((insn >> 23) & 1)
            return -1;
        if (((insn >> 19) & 0xF) == 0)
            return modified_imm(c, insn);
        return shift_imm(c, insn);
    }
    return -1;
}
