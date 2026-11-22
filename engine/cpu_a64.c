/* A64 integer interpreter (M0 subset, see plan). Correctness first. */
#include "cpu_a64.h"

#include <stdint.h>
#include <stdio.h>

#include "host.h"
#include "mem.h"
#include "syscall.h"

/* ---------- helpers ---------- */

static inline uint64_t mask_of(unsigned sf) { return sf ? ~0ull : 0xFFFFFFFFull; }

/* Sign-extends the low n bits of v to 64 bits. */
static uint64_t sext(uint64_t v, unsigned n)
{
    uint64_t m = n >= 64 ? ~0ull : ((1ull << n) - 1);
    uint64_t s = 1ull << (n - 1);
    v &= m;
    return ((v ^ s) - s);
}

/* Register 31 is ZR here. 32-bit forms read the low 32 bits. */
static inline uint64_t rz(const cpu_t *c, unsigned r, unsigned sf)
{
    uint64_t v = r == 31 ? 0 : c->x[r];
    return sf ? v : (uint32_t)v;
}

/* Register 31 is SP here. */
static inline uint64_t rsp(const cpu_t *c, unsigned r, unsigned sf)
{
    uint64_t v = r == 31 ? c->sp : c->x[r];
    return sf ? v : (uint32_t)v;
}

/* Writes to register 31 are discarded (ZR). 32-bit writes zero-extend. */
static inline void wz(cpu_t *c, unsigned r, uint64_t v, unsigned sf)
{
    if (r != 31)
        c->x[r] = sf ? v : (uint32_t)v;
}

/* Writes to register 31 go to SP. */
static inline void wsp(cpu_t *c, unsigned r, uint64_t v, unsigned sf)
{
    if (r == 31)
        c->sp = sf ? v : (uint32_t)v;
    else
        c->x[r] = sf ? v : (uint32_t)v;
}

/* AddWithCarry. Returns the result and sets NZCV in *flags. */
static uint64_t adc(uint64_t a, uint64_t b, unsigned cin, unsigned sf, uint32_t *flags)
{
    unsigned w = sf ? 64 : 32;
    uint64_t m = mask_of(sf);
    a &= m;
    b &= m;
    unsigned __int128 s = (unsigned __int128)a + b + cin;
    uint64_t r = (uint64_t)s & m;
    uint32_t C = (uint32_t)((s >> w) & 1);
    uint32_t N = (uint32_t)((r >> (w - 1)) & 1);
    uint32_t Z = r == 0;
    uint32_t V = (uint32_t)((((~(a ^ b)) & (a ^ r)) >> (w - 1)) & 1);
    *flags = (N << 31) | (Z << 30) | (C << 29) | (V << 28);
    return r;
}

static uint32_t nz_flags(uint64_t r, unsigned sf)
{
    uint32_t N = (uint32_t)((r >> (sf ? 63 : 31)) & 1);
    uint32_t Z = (r & mask_of(sf)) == 0;
    return (N << 31) | (Z << 30);
}

static int cond_true(uint32_t f, unsigned cond)
{
    unsigned N = (f >> 31) & 1, Z = (f >> 30) & 1, C = (f >> 29) & 1, V = (f >> 28) & 1;
    int r;
    switch (cond >> 1) {
    case 0: r = Z; break;
    case 1: r = C; break;
    case 2: r = N; break;
    case 3: r = V; break;
    case 4: r = C && !Z; break;
    case 5: r = N == V; break;
    case 6: r = (N == V) && !Z; break;
    default: return 1; /* AL, and NV which behaves as AL */
    }
    return (cond & 1) ? !r : r;
}

static uint64_t ror_n(uint64_t x, unsigned r, unsigned w)
{
    uint64_t m = mask_of(w == 64);
    x &= m;
    r %= w;
    if (!r)
        return x;
    return ((x >> r) | (x << (w - r))) & m;
}

/* Shift by an immediate or register amount (amount is taken mod datasize). */
static uint64_t shift_op(uint64_t v, unsigned type, unsigned amt, unsigned sf)
{
    unsigned w = sf ? 64 : 32;
    uint64_t m = mask_of(sf);
    v &= m;
    amt %= w;
    switch (type & 3) {
    case 0: return (v << amt) & m;
    case 1: return v >> amt;
    case 2: return (uint64_t)((int64_t)sext(v, w) >> amt) & m;
    default: return amt ? (((v >> amt) | (v << (w - amt))) & m) : v;
    }
}

static uint64_t extend_op(uint64_t v, unsigned option, unsigned sh)
{
    switch (option & 7) {
    case 0: v &= 0xFF; break;
    case 1: v &= 0xFFFF; break;
    case 2: v &= 0xFFFFFFFFull; break;
    case 3: break;
    case 4: v = sext(v, 8); break;
    case 5: v = sext(v, 16); break;
    case 6: v = sext(v, 32); break;
    default: break;
    }
    return v << sh;
}

static uint64_t replicate(uint64_t x, unsigned esize)
{
    for (unsigned e = esize; e < 64; e *= 2)
        x |= x << e;
    return x;
}

/* DecodeBitMasks for the 64-bit (N=1) and 32-bit (N=0) datasizes. */
static int decode_bitmask(unsigned sf, unsigned N, unsigned imms, unsigned immr, int immediate,
                          uint64_t *wmask, uint64_t *tmask)
{
    unsigned v = (N << 6) | (~imms & 0x3F);
    if (!v)
        return 0;
    int len = 31 - __builtin_clz(v);
    if (len < 1)
        return 0;
    if (!sf && N)
        return 0;
    unsigned esize = 1u << len;
    unsigned levels = esize - 1;
    unsigned S = imms & levels, R = immr & levels;
    if (immediate && S == levels)
        return 0;
    uint64_t emask = esize == 64 ? ~0ull : ((1ull << esize) - 1);
    uint64_t welem = (S + 1 == 64) ? ~0ull : ((1ull << (S + 1)) - 1);
    unsigned diff = (S - R) & levels;
    uint64_t telem = (diff + 1 == 64) ? ~0ull : ((1ull << (diff + 1)) - 1);
    uint64_t w = R ? (((welem >> R) | (welem << (esize - R))) & emask) : welem;
    *wmask = replicate(w, esize);
    *tmask = replicate(telem, esize);
    return 1;
}

static uint64_t ld(uint64_t va, unsigned bytes)
{
    switch (bytes) {
    case 1: return mem_read8(va);
    case 2: return mem_read16(va);
    case 4: return mem_read32(va);
    default: return mem_read64(va);
    }
}

static void st(uint64_t va, uint64_t v, unsigned bytes)
{
    switch (bytes) {
    case 1: mem_write8(va, (uint8_t)v); break;
    case 2: mem_write16(va, (uint16_t)v); break;
    case 4: mem_write32(va, (uint32_t)v); break;
    default: mem_write64(va, v); break;
    }
}

/* ---------- data processing: immediate ---------- */

static int dp_imm(cpu_t *c, uint32_t insn, uint64_t pc)
{
    unsigned sf = insn >> 31;
    unsigned rd = insn & 31;
    switch ((insn >> 23) & 7) {
    case 0:
    case 1: { /* ADR / ADRP */
        uint64_t imm = sext((((insn >> 5) & 0x7FFFF) << 2) | ((insn >> 29) & 3), 21);
        if (insn >> 31)
            wz(c, rd, (pc & ~0xFFFull) + (imm << 12), 1);
        else
            wz(c, rd, pc + imm, 1);
        return 0;
    }
    case 2: { /* ADD/SUB immediate */
        unsigned op = (insn >> 30) & 1, S = (insn >> 29) & 1, sh = (insn >> 22) & 1;
        uint64_t imm = (uint64_t)((insn >> 10) & 0xFFF) << (sh ? 12 : 0);
        unsigned rn = (insn >> 5) & 31;
        uint32_t f = 0;
        uint64_t a = rsp(c, rn, sf);
        uint64_t r = op ? adc(a, ~imm, 1, sf, &f) : adc(a, imm, 0, sf, &f);
        if (S) {
            c->nzcv = f;
            wz(c, rd, r, sf);
        } else {
            wsp(c, rd, r, sf);
        }
        return 0;
    }
    case 4: { /* logical immediate */
        unsigned opc = (insn >> 29) & 3, N = (insn >> 22) & 1;
        unsigned immr = (insn >> 16) & 0x3F, imms = (insn >> 10) & 0x3F;
        unsigned rn = (insn >> 5) & 31;
        uint64_t wm, tm;
        if (!decode_bitmask(sf, N, imms, immr, 1, &wm, &tm))
            return -1;
        uint64_t a = rz(c, rn, sf);
        uint64_t r;
        switch (opc) {
        case 1: r = a | wm; break;
        case 2: r = a ^ wm; break;
        default: r = a & wm; break;
        }
        r &= mask_of(sf);
        if (opc == 3) {
            c->nzcv = nz_flags(r, sf);
            wz(c, rd, r, sf);
        } else {
            wsp(c, rd, r, sf);
        }
        return 0;
    }
    case 5: { /* move wide */
        unsigned opc = (insn >> 29) & 3, hw = (insn >> 21) & 3;
        uint64_t imm16 = (insn >> 5) & 0xFFFF;
        if (opc == 1 || (!sf && hw >= 2))
            return -1;
        unsigned pos = hw * 16;
        uint64_t r;
        if (opc == 0)
            r = ~(imm16 << pos);
        else if (opc == 2)
            r = imm16 << pos;
        else
            r = (rz(c, rd, sf) & ~(0xFFFFull << pos)) | (imm16 << pos);
        wz(c, rd, r & mask_of(sf), sf);
        return 0;
    }
    case 6: { /* bitfield: SBFM, BFM, UBFM */
        unsigned opc = (insn >> 29) & 3, N = (insn >> 22) & 1;
        unsigned immr = (insn >> 16) & 0x3F, imms = (insn >> 10) & 0x3F;
        unsigned rn = (insn >> 5) & 31;
        if (opc == 3 || N != sf)
            return -1;
        uint64_t wm, tm;
        if (!decode_bitmask(sf, N, imms, immr, 0, &wm, &tm))
            return -1;
        unsigned w = sf ? 64 : 32;
        uint64_t m = mask_of(sf);
        uint64_t src = rz(c, rn, sf);
        uint64_t bot = ror_n(src, immr, w) & wm;
        uint64_t top;
        if (opc == 1) { /* BFM */
            uint64_t dst = rz(c, rd, sf);
            bot = ((dst & ~wm) & m) | bot;
            top = dst;
        } else if (opc == 0) { /* SBFM */
            top = ((src >> imms) & 1) ? ~0ull : 0;
        } else { /* UBFM */
            top = 0;
        }
        uint64_t r = ((top & ~tm) | (bot & tm)) & m;
        wz(c, rd, r, sf);
        return 0;
    }
    case 7: { /* EXTR */
        unsigned op21 = (insn >> 29) & 3, N = (insn >> 22) & 1, o0 = (insn >> 21) & 1;
        unsigned rm = (insn >> 16) & 31, imms = (insn >> 10) & 0x3F, rn = (insn >> 5) & 31;
        if (op21 || o0 || N != sf)
            return -1;
        if (!sf && imms >= 32)
            return -1;
        uint64_t r;
        if (sf) {
            unsigned __int128 cat = ((unsigned __int128)rz(c, rn, 1) << 64) | rz(c, rm, 1);
            r = (uint64_t)(cat >> imms);
        } else {
            uint64_t cat = (rz(c, rn, 0) << 32) | rz(c, rm, 0);
            r = (cat >> imms) & 0xFFFFFFFFull;
        }
        wz(c, rd, r, sf);
        return 0;
    }
    default:
        return -1;
    }
}

/* ---------- branches, exceptions and system ---------- */

static int sysreg_read(cpu_t *c, unsigned op1, unsigned crn, unsigned crm, unsigned op2, uint64_t *out)
{
    if (op1 == 3 && crn == 13 && crm == 0 && op2 == 2) { *out = c->tpidr_el0; return 1; }
    if (op1 == 3 && crn == 4 && crm == 2 && op2 == 0) { *out = c->nzcv; return 1; }
    if (op1 == 3 && crn == 4 && crm == 4 && op2 == 0) { *out = c->fpcr; return 1; }
    if (op1 == 3 && crn == 4 && crm == 4 && op2 == 1) { *out = c->fpsr; return 1; }
    /* 24 MHz virtual counter from host monotonic time. */
    if (op1 == 3 && crn == 14 && crm == 0 && op2 == 2) { *out = host_monotonic_ns() * 24 / 1000; return 1; }
    if (op1 == 3 && crn == 14 && crm == 0 && op2 == 0) { *out = 24000000; return 1; }
    if (op1 == 3 && crn == 0 && crm == 0 && op2 == 7) { *out = 0x10; return 1; }
    if (op1 == 3 && crn == 0 && crm == 0 && op2 == 1) { *out = 0x8444C004; return 1; }
    if (op1 == 0 && crn == 0 && crm == 0 && op2 == 0) { *out = 0x410FD034; return 1; }
    return 0;
}

static int sysreg_write(cpu_t *c, unsigned op1, unsigned crn, unsigned crm, unsigned op2, uint64_t v)
{
    if (op1 == 3 && crn == 13 && crm == 0 && op2 == 2) { c->tpidr_el0 = v; return 1; }
    if (op1 == 3 && crn == 4 && crm == 2 && op2 == 0) { c->nzcv = (uint32_t)v & 0xF0000000u; return 1; }
    if (op1 == 3 && crn == 4 && crm == 4 && op2 == 0) { c->fpcr = v; return 1; }
    if (op1 == 3 && crn == 4 && crm == 4 && op2 == 1) { c->fpsr = v; return 1; }
    return 0;
}

static int branch_sys(cpu_t *c, uint32_t insn, uint64_t pc)
{
    if ((insn >> 26) == 0x05) { /* B */
        c->pc = pc + (sext(insn & 0x3FFFFFF, 26) << 2);
        return 0;
    }
    if ((insn >> 26) == 0x25) { /* BL */
        c->x[30] = pc + 4;
        c->pc = pc + (sext(insn & 0x3FFFFFF, 26) << 2);
        return 0;
    }
    if (((insn >> 25) & 0x3F) == 0x1A) { /* CBZ / CBNZ */
        unsigned sf = insn >> 31, op = (insn >> 24) & 1, rt = insn & 31;
        uint64_t v = rz(c, rt, sf);
        if ((v == 0) != (int)op)
            c->pc = pc + (sext((insn >> 5) & 0x7FFFF, 19) << 2);
        return 0;
    }
    if (((insn >> 25) & 0x3F) == 0x1B) { /* TBZ / TBNZ */
        unsigned b5 = insn >> 31, op = (insn >> 24) & 1, b40 = (insn >> 19) & 0x1F, rt = insn & 31;
        unsigned bit = (b5 << 5) | b40;
        unsigned b = (unsigned)((rz(c, rt, 1) >> bit) & 1);
        if (b == op)
            c->pc = pc + (sext((insn >> 5) & 0x3FFF, 14) << 2);
        return 0;
    }
    if (((insn >> 24) & 0xFF) == 0x54) { /* B.cond */
        if (insn & 0x10)
            return -1;
        if (cond_true(c->nzcv, insn & 0xF))
            c->pc = pc + (sext((insn >> 5) & 0x7FFFF, 19) << 2);
        return 0;
    }
    if (((insn >> 25) & 0x7F) == 0x6B) { /* BR / BLR / RET */
        unsigned opc = (insn >> 21) & 0xF, rn = (insn >> 5) & 31;
        if (((insn >> 16) & 0x1F) != 0x1F || ((insn >> 10) & 0x3F) != 0 || (insn & 0x1F) != 0)
            return -1;
        if (opc == 0 || opc == 2) {
            c->pc = rz(c, rn, 1);
            return 0;
        }
        if (opc == 1) {
            uint64_t target = rz(c, rn, 1);
            c->x[30] = pc + 4;
            c->pc = target;
            return 0;
        }
        return -1;
    }
    if ((insn & 0xFFE0001F) == 0xD4000001) { /* SVC #imm16 */
        if ((insn >> 5) & 0xFFFF)
            return -1;
        syscall_dispatch(c);
        return 0;
    }
    if (((insn >> 22) & 0x3FF) == 0x354) { /* system: hints, barriers, MSR/MRS */
        unsigned L = (insn >> 21) & 1, op0 = (insn >> 19) & 3, op1 = (insn >> 16) & 7;
        unsigned crn = (insn >> 12) & 0xF, crm = (insn >> 8) & 0xF, op2 = (insn >> 5) & 7;
        unsigned rt = insn & 31;
        if (op0 == 0) {
            if (L)
                return -1;
            if (crn == 2 && op1 == 3 && rt == 31) /* HINT space: all NOPs */
                return 0;
            if (crn == 3 && op1 == 3 && rt == 31) { /* barriers are NOPs; CLREX clears the monitor */
                if (op2 == 2)
                    c->excl_valid = 0;
                return 0;
            }
            return -1;
        }
        if (op0 < 2)
            return -1;
        if (L) {
            uint64_t v;
            if (op0 != 3 || !sysreg_read(c, op1, crn, crm, op2, &v))
                return -1;
            wz(c, rt, v, 1);
            return 0;
        }
        if (op0 != 3 || !sysreg_write(c, op1, crn, crm, op2, rz(c, rt, 1)))
            return -1;
        return 0;
    }
    return -1;
}

/* ---------- loads and stores ---------- */

static int ldst_literal(cpu_t *c, uint32_t insn, uint64_t pc)
{
    unsigned opc = insn >> 30, V = (insn >> 26) & 1, rt = insn & 31;
    uint64_t addr = pc + (sext((insn >> 5) & 0x7FFFF, 19) << 2);
    if (!V) {
        switch (opc) {
        case 0: wz(c, rt, mem_read32(addr), 0); return 0;
        case 1: wz(c, rt, mem_read64(addr), 1); return 0;
        case 2: wz(c, rt, sext(mem_read32(addr), 32), 1); return 0;
        default: return 0; /* PRFM literal: hint */
        }
    }
    switch (opc) {
    case 0: c->v[rt][0] = mem_read32(addr); c->v[rt][1] = 0; return 0;
    case 1: c->v[rt][0] = mem_read64(addr); c->v[rt][1] = 0; return 0;
    case 2: mem_read128(addr, c->v[rt]); return 0;
    default: return -1;
    }
}

static int ldst_pair(cpu_t *c, uint32_t insn)
{
    unsigned opc = insn >> 30, V = (insn >> 26) & 1, mode = (insn >> 23) & 7, L = (insn >> 22) & 1;
    unsigned rt = insn & 31, rt2 = (insn >> 10) & 31, rn = (insn >> 5) & 31;
    int64_t imm7 = (int64_t)sext((insn >> 15) & 0x7F, 7);
    unsigned bytes, dsf = 1, sign = 0;
    if (mode > 3)
        return -1;
    if (V) {
        if (opc == 0) bytes = 4;
        else if (opc == 1) bytes = 8;
        else if (opc == 2) bytes = 16;
        else return -1;
    } else {
        if (opc == 0) { bytes = 4; dsf = 0; }
        else if (opc == 2) bytes = 8;
        else if (opc == 1 && L) { bytes = 4; sign = 1; } /* LDPSW */
        else return -1;
    }
    uint64_t base = rsp(c, rn, 1);
    uint64_t off = (uint64_t)(imm7 * (int64_t)bytes);
    uint64_t addr = (mode == 1) ? base : base + off;
    uint64_t a2 = addr + bytes;
    if (L) {
        if (V) {
            if (bytes == 16) {
                mem_read128(addr, c->v[rt]);
                mem_read128(a2, c->v[rt2]);
            } else {
                c->v[rt][0] = ld(addr, bytes);
                c->v[rt][1] = 0;
                c->v[rt2][0] = ld(a2, bytes);
                c->v[rt2][1] = 0;
            }
        } else {
            uint64_t v1 = ld(addr, bytes), v2 = ld(a2, bytes);
            if (sign) {
                v1 = sext(v1, 32);
                v2 = sext(v2, 32);
            }
            wz(c, rt, v1, dsf);
            wz(c, rt2, v2, dsf);
        }
    } else {
        if (V) {
            if (bytes == 16) {
                mem_write128(addr, c->v[rt]);
                mem_write128(a2, c->v[rt2]);
            } else {
                st(addr, c->v[rt][0], bytes);
                st(a2, c->v[rt2][0], bytes);
            }
        } else {
            st(addr, rz(c, rt, dsf), bytes);
            st(a2, rz(c, rt2, dsf), bytes);
        }
    }
    if (mode == 1 || mode == 3)
        wsp(c, rn, base + off, 1);
    return 0;
}

static int ldst_single(cpu_t *c, uint32_t insn)
{
    unsigned size = insn >> 30, V = (insn >> 26) & 1, opc = (insn >> 22) & 3;
    unsigned mode = (insn >> 24) & 3, bit21 = (insn >> 21) & 1, sub = (insn >> 10) & 3;
    unsigned rt = insn & 31, rn = (insn >> 5) & 31;
    unsigned scale, bytes;

    if (!V && (insn & 0x3FFFFC00u) == 0x38BFC000u) { /* LDAPR/LDAPRB/LDAPRH: plain load */
        wz(c, rt, ld(rsp(c, rn, 1), 1u << size), size == 3);
        return 0;
    }

    if (!V && size == 3 && opc == 2) { /* PRFM / PRFUM / PRFM register: hints */
        if (mode == 1)
            return 0;
        if (mode == 0 && !bit21 && sub == 0)
            return 0;
        if (mode == 0 && bit21 && sub == 2)
            return 0;
    }
    if (V) {
        if (opc >= 2 && size != 0)
            return -1;
        scale = ((opc >> 1) << 2) | size; /* 0 B, 1 H, 2 S, 3 D, 4 Q */
    } else {
        if (opc == 3 && size >= 2)
            return -1;
        scale = size;
    }
    bytes = 1u << scale;
    int is_load = V ? (int)(opc & 1) : (opc != 0);
    int sign_ext = !V && opc >= 2;
    unsigned dsf = (opc == 2) || (opc == 1 && size == 3);

    uint64_t base = rsp(c, rn, 1);
    uint64_t addr;
    int wb = 0;
    uint64_t newbase = 0;
    if (mode == 1) { /* unsigned immediate */
        addr = base + ((uint64_t)((insn >> 10) & 0xFFF) << scale);
    } else if (mode == 0 && bit21) { /* register offset */
        if (sub != 2)
            return -1; /* atomics (LSE) are not in M0 */
        unsigned option = (insn >> 13) & 7, S = (insn >> 12) & 1, rm = (insn >> 16) & 31;
        if (option != 2 && option != 3 && option != 6 && option != 7)
            return -1;
        addr = base + extend_op(rz(c, rm, 1), option, S ? scale : 0);
    } else if (mode == 0) { /* unscaled, post-index, pre-index */
        uint64_t imm = sext((insn >> 12) & 0x1FF, 9);
        switch (sub) {
        case 0: addr = base + imm; break;                             /* LDUR/STUR */
        case 1: addr = base; wb = 1; newbase = base + imm; break;     /* post-index */
        case 3: addr = base + imm; wb = 1; newbase = addr; break;     /* pre-index */
        default: return -1;                                           /* LDTR/STTR */
        }
    } else {
        return -1;
    }

    if (is_load) {
        if (V) {
            if (scale == 4) {
                mem_read128(addr, c->v[rt]);
            } else {
                c->v[rt][0] = ld(addr, bytes);
                c->v[rt][1] = 0;
            }
        } else {
            uint64_t val = ld(addr, bytes);
            if (sign_ext)
                val = sext(val, bytes * 8);
            wz(c, rt, val, dsf);
        }
    } else {
        if (V) {
            if (scale == 4)
                mem_write128(addr, c->v[rt]);
            else
                st(addr, c->v[rt][0], bytes);
        } else {
            st(addr, rz(c, rt, 1), bytes);
        }
    }
    if (wb)
        wsp(c, rn, newbase, 1);
    return 0;
}

static int ldst_excl(cpu_t *c, uint32_t insn)
{
    unsigned size = insn >> 30, o2 = (insn >> 23) & 1, L = (insn >> 22) & 1, o1 = (insn >> 21) & 1;
    unsigned rs = (insn >> 16) & 31, o0 = (insn >> 15) & 1, rt2 = (insn >> 10) & 31;
    unsigned rn = (insn >> 5) & 31, rt = insn & 31;
    uint64_t addr = rsp(c, rn, 1);

    if (!o2 && o1) { /* LDXP / STXP (pair) */
        if (!(size & 2))
            return -1;
        unsigned bytes = (size & 1) ? 8 : 4;
        if (L) {
            uint64_t v1 = ld(addr, bytes), v2 = ld(addr + bytes, bytes);
            wz(c, rt, v1, bytes == 8);
            wz(c, rt2, v2, bytes == 8);
            c->excl_valid = 1;
            c->excl_addr = addr;
        } else {
            int ok = c->excl_valid && c->excl_addr == addr;
            if (ok) {
                st(addr, rz(c, rt, bytes == 8), bytes);
                st(addr + bytes, rz(c, rt2, bytes == 8), bytes);
            }
            wz(c, rs, ok ? 0 : 1, 0);
            c->excl_valid = 0;
        }
        return 0;
    }
    unsigned bytes = 1u << size;
    if (!o2) { /* LDXR/LDAXR and STXR/STLXR */
        if (o1)
            return -1;
        if (L) {
            wz(c, rt, ld(addr, bytes), size == 3);
            c->excl_valid = 1;
            c->excl_addr = addr;
        } else {
            int ok = c->excl_valid && c->excl_addr == addr;
            if (ok)
                st(addr, rz(c, rt, 1), bytes);
            wz(c, rs, ok ? 0 : 1, 0);
            c->excl_valid = 0;
        }
        return 0;
    }
    /* Ordered: LDAR and STLR (LDLAR/STLLR are not in M0). */
    if (o1 || o0 == 0 || rs != 31 || rt2 != 31)
        return -1;
    if (L)
        wz(c, rt, ld(addr, bytes), size == 3);
    else
        st(addr, rz(c, rt, 1), bytes);
    return 0;
}

static int ldst(cpu_t *c, uint32_t insn, uint64_t pc)
{
    if (((insn >> 24) & 0x3F) == 0x08)
        return ldst_excl(c, insn);
    if (((insn >> 27) & 7) == 3 && ((insn >> 24) & 3) == 0)
        return ldst_literal(c, insn, pc);
    if (((insn >> 27) & 7) == 5)
        return ldst_pair(c, insn);
    if (((insn >> 27) & 7) == 7)
        return ldst_single(c, insn);
    return -1;
}

/* ---------- data processing: register ---------- */

static int logical_shifted(cpu_t *c, uint32_t insn)
{
    unsigned sf = insn >> 31, opc = (insn >> 29) & 3, shift = (insn >> 22) & 3, N = (insn >> 21) & 1;
    unsigned rm = (insn >> 16) & 31, imm6 = (insn >> 10) & 0x3F, rn = (insn >> 5) & 31, rd = insn & 31;
    if (!sf && imm6 >= 32)
        return -1;
    uint64_t b = shift_op(rz(c, rm, sf), shift, imm6, sf);
    if (N)
        b = ~b;
    uint64_t a = rz(c, rn, sf);
    uint64_t r;
    switch (opc) {
    case 1: r = a | b; break;
    case 2: r = a ^ b; break;
    default: r = a & b; break;
    }
    r &= mask_of(sf);
    if (opc == 3)
        c->nzcv = nz_flags(r, sf);
    wz(c, rd, r, sf);
    return 0;
}

static int addsub_shifted(cpu_t *c, uint32_t insn)
{
    unsigned sf = insn >> 31, op = (insn >> 30) & 1, S = (insn >> 29) & 1, shift = (insn >> 22) & 3;
    unsigned rm = (insn >> 16) & 31, imm6 = (insn >> 10) & 0x3F, rn = (insn >> 5) & 31, rd = insn & 31;
    if (shift == 3 || (!sf && imm6 >= 32))
        return -1;
    uint64_t b = shift_op(rz(c, rm, sf), shift, imm6, sf);
    uint64_t a = rz(c, rn, sf);
    uint32_t f = 0;
    uint64_t r = op ? adc(a, ~b, 1, sf, &f) : adc(a, b, 0, sf, &f);
    if (S)
        c->nzcv = f;
    wz(c, rd, r, sf);
    return 0;
}

static int addsub_ext(cpu_t *c, uint32_t insn)
{
    unsigned sf = insn >> 31, op = (insn >> 30) & 1, S = (insn >> 29) & 1;
    unsigned option = (insn >> 13) & 7, imm3 = (insn >> 10) & 7;
    unsigned rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd = insn & 31;
    if (((insn >> 22) & 3) || imm3 > 4)
        return -1;
    uint64_t b = extend_op(rz(c, rm, 1), option, imm3);
    uint64_t a = rsp(c, rn, sf);
    uint32_t f = 0;
    uint64_t r = op ? adc(a, ~b, 1, sf, &f) : adc(a, b, 0, sf, &f);
    if (S) {
        c->nzcv = f;
        wz(c, rd, r, sf);
    } else {
        wsp(c, rd, r, sf);
    }
    return 0;
}

static int adc_sbc(cpu_t *c, uint32_t insn)
{
    unsigned sf = insn >> 31, op = (insn >> 30) & 1, S = (insn >> 29) & 1;
    unsigned rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd = insn & 31;
    if ((insn >> 10) & 0x3F)
        return -1;
    uint64_t a = rz(c, rn, sf), b = rz(c, rm, sf);
    unsigned cin = (c->nzcv >> 29) & 1;
    uint32_t f = 0;
    uint64_t r = op ? adc(a, ~b, cin, sf, &f) : adc(a, b, cin, sf, &f);
    if (S)
        c->nzcv = f;
    wz(c, rd, r, sf);
    return 0;
}

static int ccmp(cpu_t *c, uint32_t insn)
{
    unsigned sf = insn >> 31, op = (insn >> 30) & 1, S = (insn >> 29) & 1, imm = (insn >> 11) & 1;
    unsigned o2 = (insn >> 10) & 1, o3 = (insn >> 4) & 1;
    unsigned cond = (insn >> 12) & 0xF, rn = (insn >> 5) & 31, nzcv = insn & 0xF;
    if (!S || o2 || o3)
        return -1;
    uint64_t a = rz(c, rn, sf);
    uint64_t b = imm ? ((insn >> 16) & 0x1F) : rz(c, (insn >> 16) & 31, sf);
    if (cond_true(c->nzcv, cond)) {
        uint32_t f = 0;
        if (op)
            adc(a, ~b, 1, sf, &f);
        else
            adc(a, b, 0, sf, &f);
        c->nzcv = f;
    } else {
        c->nzcv = nzcv << 28;
    }
    return 0;
}

static int csel(cpu_t *c, uint32_t insn)
{
    unsigned sf = insn >> 31, op = (insn >> 30) & 1, S = (insn >> 29) & 1;
    unsigned cond = (insn >> 12) & 0xF, op2 = (insn >> 10) & 3;
    unsigned rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd = insn & 31;
    if (S || op2 >= 2)
        return -1;
    uint64_t r;
    if (cond_true(c->nzcv, cond)) {
        r = rz(c, rn, sf);
    } else {
        uint64_t m = rz(c, rm, sf);
        if (op)
            m = ~m;
        if (op2)
            m += 1;
        r = m;
    }
    wz(c, rd, r & mask_of(sf), sf);
    return 0;
}

static int dp_2src(cpu_t *c, uint32_t insn)
{
    unsigned sf = insn >> 31, S = (insn >> 29) & 1, opcode = (insn >> 10) & 0x3F;
    unsigned rm = (insn >> 16) & 31, rn = (insn >> 5) & 31, rd = insn & 31;
    if (S)
        return -1;
    unsigned w = sf ? 64 : 32;
    int64_t minv = (w == 64) ? INT64_MIN : -2147483648LL;
    uint64_t a = rz(c, rn, sf), b = rz(c, rm, sf), r;
    switch (opcode) {
    case 2: /* UDIV */
        r = b ? a / b : 0;
        break;
    case 3: { /* SDIV */
        int64_t sa = (int64_t)sext(a, w), sb = (int64_t)sext(b, w);
        if (sb == 0)
            r = 0;
        else if (sb == -1 && sa == minv)
            r = (uint64_t)sa;
        else
            r = (uint64_t)(sa / sb);
        break;
    }
    case 8: r = shift_op(a, 0, (unsigned)(b & (w - 1)), sf); break; /* LSLV */
    case 9: r = shift_op(a, 1, (unsigned)(b & (w - 1)), sf); break; /* LSRV */
    case 10: r = shift_op(a, 2, (unsigned)(b & (w - 1)), sf); break; /* ASRV */
    case 11: r = shift_op(a, 3, (unsigned)(b & (w - 1)), sf); break; /* RORV */
    default: return -1;
    }
    wz(c, rd, r & mask_of(sf), sf);
    return 0;
}

static uint32_t bswap32(uint32_t x) { return __builtin_bswap32(x); }

static int dp_1src(cpu_t *c, uint32_t insn)
{
    unsigned sf = insn >> 31, S = (insn >> 29) & 1, opcode = (insn >> 10) & 0x3F;
    unsigned rn = (insn >> 5) & 31, rd = insn & 31;
    if (S || ((insn >> 16) & 0x1F))
        return -1;
    unsigned w = sf ? 64 : 32;
    uint64_t a = rz(c, rn, sf), r = 0;
    switch (opcode) {
    case 0: /* RBIT */
        for (unsigned i = 0; i < w; i++)
            r |= ((a >> i) & 1) << (w - 1 - i);
        break;
    case 1: /* REV16 */
        for (unsigned k = 0; k < w / 16; k++) {
            uint64_t chunk = (a >> (16 * k)) & 0xFFFF;
            r |= (((chunk & 0xFF) << 8) | (chunk >> 8)) << (16 * k);
        }
        break;
    case 2: /* REV32 (64-bit) or REV (32-bit) */
        if (sf)
            r = (uint64_t)bswap32((uint32_t)a) | ((uint64_t)bswap32((uint32_t)(a >> 32)) << 32);
        else
            r = bswap32((uint32_t)a);
        break;
    case 3: /* REV (64-bit) */
        if (!sf)
            return -1;
        r = __builtin_bswap64(a);
        break;
    case 4: /* CLZ */
        r = a ? (uint64_t)(__builtin_clzll(a) - (64 - w)) : w;
        break;
    case 5: { /* CLS */
        unsigned sign = (unsigned)((a >> (w - 1)) & 1);
        uint64_t x = sign ? (~a & mask_of(sf)) : a;
        r = (x ? (uint64_t)(__builtin_clzll(x) - (64 - w)) : w) - 1;
        break;
    }
    default:
        return -1;
    }
    wz(c, rd, r & mask_of(sf), sf);
    return 0;
}

static int dp_3src(cpu_t *c, uint32_t insn)
{
    unsigned sf = insn >> 31, op54 = (insn >> 29) & 3, op31 = (insn >> 21) & 7, o0 = (insn >> 15) & 1;
    unsigned rm = (insn >> 16) & 31, ra = (insn >> 10) & 31, rn = (insn >> 5) & 31, rd = insn & 31;
    if (op54)
        return -1;
    uint64_t r;
    switch (op31) {
    case 0: { /* MADD / MSUB */
        uint64_t prod = rz(c, rn, sf) * rz(c, rm, sf);
        uint64_t acc = rz(c, ra, sf);
        r = o0 ? acc - prod : acc + prod;
        break;
    }
    case 1: { /* SMADDL / SMSUBL */
        if (!sf)
            return -1;
        uint64_t prod = sext(rz(c, rn, 0), 32) * sext(rz(c, rm, 0), 32);
        uint64_t acc = rz(c, ra, 1);
        r = o0 ? acc - prod : acc + prod;
        break;
    }
    case 2: /* SMULH */
        if (!sf || o0)
            return -1;
        r = (uint64_t)(((__int128)(int64_t)rz(c, rn, 1) * (__int128)(int64_t)rz(c, rm, 1)) >> 64);
        break;
    case 5: { /* UMADDL / UMSUBL */
        if (!sf)
            return -1;
        uint64_t prod = (uint64_t)(uint32_t)rz(c, rn, 0) * (uint64_t)(uint32_t)rz(c, rm, 0);
        uint64_t acc = rz(c, ra, 1);
        r = o0 ? acc - prod : acc + prod;
        break;
    }
    case 6: /* UMULH */
        if (!sf || o0)
            return -1;
        r = (uint64_t)(((unsigned __int128)rz(c, rn, 1) * (unsigned __int128)rz(c, rm, 1)) >> 64);
        break;
    default:
        return -1;
    }
    wz(c, rd, r & mask_of(sf), sf);
    return 0;
}

static int dp_reg(cpu_t *c, uint32_t insn)
{
    if (!((insn >> 28) & 1)) {
        if (!((insn >> 24) & 1))
            return logical_shifted(c, insn);
        if ((insn >> 21) & 1)
            return addsub_ext(c, insn);
        return addsub_shifted(c, insn);
    }
    if ((insn >> 24) & 1)
        return dp_3src(c, insn);
    switch ((insn >> 21) & 7) {
    case 0: return adc_sbc(c, insn);
    case 2: return ccmp(c, insn);
    case 4: return csel(c, insn);
    case 6: return ((insn >> 30) & 1) ? dp_1src(c, insn) : dp_2src(c, insn);
    default: return -1;
    }
}

/* ---------- dispatch and main loop ---------- */

static int exec(cpu_t *c, uint32_t insn, uint64_t pc)
{
    unsigned op0 = (insn >> 25) & 0xF;
    if ((op0 & 0xE) == 0x8)
        return dp_imm(c, insn, pc);
    if ((op0 & 0xE) == 0xA)
        return branch_sys(c, insn, pc);
    if ((op0 & 0x5) == 0x4)
        return ldst(c, insn, pc);
    if ((op0 & 0x7) == 0x5)
        return dp_reg(c, insn);
    return -1; /* reserved, SVE, and FP/SIMD data processing */
}

void cpu_run(cpu_t *c)
{
    while (!c->exited) {
        uint64_t pc = c->pc;
        mem_set_pc(pc);
        uint32_t insn = mem_fetch32(pc);
        c->pc = pc + 4;
        c->icount++;
        if (exec(c, insn, pc) < 0) {
            fprintf(stderr, "xiaoji: unimplemented A64 instruction 0x%08x at pc 0x%llx\n", insn,
                    (unsigned long long)pc);
            c->exited = 1;
            c->exit_code = 132;
        }
    }
}
