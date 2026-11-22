/* Vector floating-point and by-element forms (B4a, for zygote's ART code): each form is run
 * through inline assembly on fixed 128-bit inputs (signed zeros, infinities, NaNs, ties and
 * negatives), and the raw 128-bit result is printed. Native and engine output must be identical,
 * so the engine's arithmetic is checked bit for bit. The inputs are used as floats for the
 * single forms and as doubles for the 2D forms (the same bytes). */
#include "sys.h"

typedef struct {
    u64 lo, hi;
} v128;

static const u32 in_n[4] = {0x3fc00000u, 0x80000000u, 0x00000000u, 0x7f800000u}; /* 1.5 -0 +0 inf */
static const u32 in_m[4] = {0xc0200000u, 0x00000000u, 0x7fc00000u, 0xff800000u}; /* -2.5 0 NaN -inf */
static const u32 in_d[4] = {0x3e800000u, 0xc0400000u, 0x40e00000u, 0x80000000u}; /* 0.25 -3 7 -0 */
static const u32 in_i[4] = {0x0000000fu, 0xfffffff1u, 0x7fffffffu, 0x80000000u};  /* ints */

#define LOAD3 "ldr q0, [%0]\n ldr q1, [%1]\n ldr q2, [%2]\n "
#define STORE "str q2, [%3]\n "
#define ARGS "r"(n), "r"(m), "r"(d), "r"(out)
#define CLOB "memory", "v0", "v1", "v2"

/* One operation: its name, and the assembly that writes v2 from v0 (Vn) and v1 (Vm). */
#define OP(name, insn)                                                              \
    static void name(const void *n, const void *m, const void *d, void *out)       \
    {                                                                               \
        __asm__ volatile(LOAD3 insn "\n " STORE : : ARGS : CLOB);                  \
    }
/* An operation that reads the integer inputs (v0 holds them; v2 is written). */
#define OPI(name, insn)                                                             \
    static void name(const void *n, const void *m, const void *d, void *out)       \
    {                                                                               \
        __asm__ volatile(LOAD3 insn "\n " STORE : : ARGS : CLOB);                  \
    }

/* Single precision, 2S and 4S; double precision, 2D. */
OP(fadd_2s, "fadd v2.2s, v0.2s, v1.2s")
OP(fadd_4s, "fadd v2.4s, v0.4s, v1.4s")
OP(fsub_4s, "fsub v2.4s, v0.4s, v1.4s")
OP(fmul_4s, "fmul v2.4s, v0.4s, v1.4s")
OP(fdiv_4s, "fdiv v2.4s, v0.4s, v1.4s")
OP(fmax_4s, "fmax v2.4s, v0.4s, v1.4s")
OP(fmin_4s, "fmin v2.4s, v0.4s, v1.4s")
OP(fmaxnm_4s, "fmaxnm v2.4s, v0.4s, v1.4s")
OP(fminnm_4s, "fminnm v2.4s, v0.4s, v1.4s")
OP(fmax_2d, "fmax v2.2d, v0.2d, v1.2d")
OP(fmin_2d, "fmin v2.2d, v0.2d, v1.2d")
OP(fmaxnm_2d, "fmaxnm v2.2d, v0.2d, v1.2d")
OP(fminnm_2d, "fminnm v2.2d, v0.2d, v1.2d")
OP(fmla_4s, "fmla v2.4s, v0.4s, v1.4s")
OP(fmls_4s, "fmls v2.4s, v0.4s, v1.4s")
OP(fmla_2d, "fmla v2.2d, v0.2d, v1.2d")
OP(fmulx_4s, "fmulx v2.4s, v0.4s, v1.4s")
OP(fabd_4s, "fabd v2.4s, v0.4s, v1.4s")
OP(fcmeq_4s, "fcmeq v2.4s, v0.4s, v1.4s")
OP(fcmge_4s, "fcmge v2.4s, v0.4s, v1.4s")
OP(fcmgt_4s, "fcmgt v2.4s, v0.4s, v1.4s")
OP(facge_4s, "facge v2.4s, v0.4s, v1.4s")
OP(facgt_4s, "facgt v2.4s, v0.4s, v1.4s")
OP(frecps_4s, "frecps v2.4s, v0.4s, v1.4s")
OP(frsqrts_4s, "frsqrts v2.4s, v0.4s, v1.4s")
OP(faddp_4s, "faddp v2.4s, v0.4s, v1.4s")
OP(fmaxp_4s, "fmaxp v2.4s, v0.4s, v1.4s")
OP(fminp_2d, "fminp v2.2d, v0.2d, v1.2d")
OP(fmaxnmp_2s, "fmaxnmp v2.2s, v0.2s, v1.2s")
OP(fabs_4s, "fabs v2.4s, v0.4s")
OP(fneg_4s, "fneg v2.4s, v0.4s")
OP(fsqrt_4s, "fsqrt v2.4s, v0.4s")
OP(fsqrt_2d, "fsqrt v2.2d, v0.2d")
OP(frintn_4s, "frintn v2.4s, v0.4s")
OP(frintm_4s, "frintm v2.4s, v0.4s")
OP(frintp_4s, "frintp v2.4s, v0.4s")
OP(frintz_4s, "frintz v2.4s, v0.4s")
OP(frinta_4s, "frinta v2.4s, v0.4s")
OP(frintx_4s, "frintx v2.4s, v0.4s")
OP(frinti_4s, "frinti v2.4s, v0.4s")
OP(fcvtns_4s, "fcvtns v2.4s, v0.4s")
OP(fcvtms_4s, "fcvtms v2.4s, v0.4s")
OP(fcvtzs_4s, "fcvtzs v2.4s, v0.4s")
OP(fcvtzu_4s, "fcvtzu v2.4s, v0.4s")
OP(fcvtas_4s, "fcvtas v2.4s, v0.4s")
OP(fcvtau_4s, "fcvtau v2.4s, v0.4s")
OP(fcvtzs_2d, "fcvtzs v2.2d, v0.2d")
OP(scvtf_4s, "scvtf v2.4s, v0.4s")
OP(ucvtf_4s, "ucvtf v2.4s, v0.4s")
OP(scvtf_2d, "scvtf v2.2d, v0.2d")
OP(fmla_el_4s, "fmla v2.4s, v0.4s, v1.s[1]")
OP(fmls_el_4s, "fmls v2.4s, v0.4s, v1.s[2]")
OP(fmul_el_4s, "fmul v2.4s, v0.4s, v1.s[3]")
OP(fmulx_el_4s, "fmulx v2.4s, v0.4s, v1.s[0]")
OP(fmla_el_2d, "fmla v2.2d, v0.2d, v1.d[1]")
OP(fmul_el_2s, "fmul v2.2s, v0.2s, v1.s[1]")
OP(mul_el_4s, "mul v2.4s, v0.4s, v1.s[2]")
OP(mla_el_4s, "mla v2.4s, v0.4s, v1.s[1]")
OP(mls_el_4s, "mls v2.4s, v0.4s, v1.s[3]")
OP(mul_el_8h, "mul v2.8h, v0.8h, v1.h[5]")
OP(mla_el_8h, "mla v2.8h, v0.8h, v1.h[7]")
OP(fcvtas_s, "fcvtas s2, s0")
OP(fcvtau_d, "fcvtau d2, d0")
OP(faddp_s, "faddp s2, v0.2s")
OP(fmaxnmp_s, "fmaxnmp s2, v0.2s")
OP(fminnmp_s, "fminnmp s2, v0.2s")
OP(fmaxp_d, "fmaxp d2, v0.2d")
OP(fminp_d, "fminp d2, v0.2d")
OP(dup_s, "dup s2, v0.s[1]")
OP(dup_d, "dup d2, v0.d[1]")
OPI(scvtf_s, "scvtf s2, s0")

struct op {
    const char *name;
    void (*fn)(const void *, const void *, const void *, void *);
};

#define ENTRY(f) {#f, f}

static const struct op ops[] = {
    ENTRY(fadd_2s), ENTRY(fadd_4s), ENTRY(fsub_4s), ENTRY(fmul_4s), ENTRY(fdiv_4s),
    ENTRY(fmax_4s), ENTRY(fmin_4s), ENTRY(fmaxnm_4s), ENTRY(fminnm_4s),
    ENTRY(fmax_2d), ENTRY(fmin_2d), ENTRY(fmaxnm_2d), ENTRY(fminnm_2d),
    ENTRY(fmla_4s), ENTRY(fmls_4s), ENTRY(fmla_2d), ENTRY(fmulx_4s), ENTRY(fabd_4s),
    ENTRY(fcmeq_4s), ENTRY(fcmge_4s), ENTRY(fcmgt_4s), ENTRY(facge_4s), ENTRY(facgt_4s),
    ENTRY(frecps_4s), ENTRY(frsqrts_4s), ENTRY(faddp_4s), ENTRY(fmaxp_4s), ENTRY(fminp_2d),
    ENTRY(fmaxnmp_2s), ENTRY(fabs_4s), ENTRY(fneg_4s), ENTRY(fsqrt_4s), ENTRY(fsqrt_2d),
    ENTRY(frintn_4s), ENTRY(frintm_4s), ENTRY(frintp_4s), ENTRY(frintz_4s), ENTRY(frinta_4s),
    ENTRY(frintx_4s), ENTRY(frinti_4s), ENTRY(fcvtns_4s), ENTRY(fcvtms_4s), ENTRY(fcvtzs_4s),
    ENTRY(fcvtzu_4s), ENTRY(fcvtas_4s), ENTRY(fcvtau_4s), ENTRY(fcvtzs_2d), ENTRY(scvtf_4s),
    ENTRY(ucvtf_4s), ENTRY(scvtf_2d), ENTRY(fmla_el_4s), ENTRY(fmls_el_4s), ENTRY(fmul_el_4s),
    ENTRY(fmulx_el_4s), ENTRY(fmla_el_2d), ENTRY(fmul_el_2s), ENTRY(mul_el_4s), ENTRY(mla_el_4s),
    ENTRY(mls_el_4s), ENTRY(mul_el_8h), ENTRY(mla_el_8h), ENTRY(fcvtas_s), ENTRY(fcvtau_d),
    ENTRY(faddp_s), ENTRY(fmaxnmp_s), ENTRY(fminnmp_s), ENTRY(fmaxp_d), ENTRY(fminp_d),
    ENTRY(dup_s), ENTRY(dup_d), ENTRY(scvtf_s),
};

static void pack(v128 *v, const u32 *w)
{
    v->lo = (u64)w[0] | (u64)w[1] << 32;
    v->hi = (u64)w[2] | (u64)w[3] << 32;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    v128 n, m, d, r;
    pack(&n, in_n);
    pack(&m, in_m);
    pack(&d, in_d);
    /* The integer forms read the integer inputs in Vn. */
    v128 ni;
    pack(&ni, in_i);
    for (unsigned k = 0; k < sizeof ops / sizeof ops[0]; k++) {
        const void *nn = &n;
        if (ops[k].fn == scvtf_s || ops[k].fn == dup_s || ops[k].fn == dup_d)
            nn = &ni;
        if (ops[k].fn == scvtf_s)
            nn = &ni;
        ops[k].fn(nn, &m, &d, &r);
        put(ops[k].name);
        put(" ");
        putx(r.hi);
        put(" ");
        putx(r.lo);
        put("\n");
    }
    return 0;
}
