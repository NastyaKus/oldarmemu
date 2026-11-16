/*
 * Adreno 200 rendering in software: shaders, rasterizer, GMEM, resolves.
 *
 * a2xx renders into on-chip GMEM tiles (RB_MODECONTROL = COLOR_DEPTH)
 * and copies them out to memory with "resolve" draws (EDRAM_COPY).  The
 * unified shader ISA is the one of the Xbox 360's Xenos; instruction and
 * fetch constant layouts follow freedreno (instr-a2xx.h, a2xx.xml) and
 * Xenia's documentation of the microcode.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include <math.h>
#include "qemu/timer.h"
#include "adreno.h"
#include "adreno_regs.h"

typedef struct V4 { float f[4]; } V4;

void adreno_etc1_texel(const uint8_t *blk, int px, int py, float *rgba);

/* ---------------- decoded shader ---------------- */

enum { ST_VS = 0, ST_PS = 1 };

typedef struct Thread {
    int stage;
    V4 r[64];
    V4 exp[64];
    uint64_t exp_mask;
    float prev;             /* previous scalar result */
    bool pred;
    int a0;                 /* address register */
    int aL;                 /* loop index */
    bool kill;
} Thread;

static inline float f_of(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static inline uint32_t u_of(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static inline float sat(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : (v != v ? 0.0f : v);
}

/* D3D9 rule: 0 * anything = 0 */
static inline float mulz(float a, float b)
{
    return (a == 0.0f || b == 0.0f) ? 0.0f : a * b;
}

static uint32_t reg(AdrenoState *s, uint32_t r)
{
    return s->regs[r];
}

/* float constant c[idx] of the thread's stage */
static V4 get_const(AdrenoState *s, Thread *t, int idx)
{
    uint32_t c = reg(s, t->stage == ST_VS ? A2XX_SQ_VS_CONST : A2XX_SQ_PS_CONST);
    uint32_t base = c & 0x1ff, size = (c >> 12) & 0x1ff;
    V4 v = { { 0, 0, 0, 0 } };

    if (idx < 0 || (uint32_t)idx > size || base + idx >= ADRENO_ALU_CONSTS) {
        return v;
    }
    for (int i = 0; i < 4; i++) {
        v.f[i] = f_of(s->regs[ADRENO_REG_ALU + (base + idx) * 4 + i]);
    }
    return v;
}

static inline V4 *tmp(Thread *t, uint32_t r, bool rel)
{
    int i = (r & 0x3f) + (rel ? t->aL : 0);
    return &t->r[i & 63];
}

/* ---------------- ALU ---------------- */

static void alu_exec(AdrenoState *s, Thread *t, const uint32_t *w)
{
    uint32_t d0 = w[0], d1 = w[1], d2 = w[2];
    uint32_t vdst = d0 & 0x3f, vdst_rel = (d0 >> 6) & 1, abs_c = (d0 >> 7) & 1;
    uint32_t sdst = (d0 >> 8) & 0x3f, sdst_rel = (d0 >> 14) & 1;
    bool exp = (d0 >> 15) & 1;
    uint32_t vmask = (d0 >> 16) & 0xf, smask = (d0 >> 20) & 0xf;
    bool vclamp = (d0 >> 24) & 1, sclamp = (d0 >> 25) & 1;
    uint32_t sop = (d0 >> 26) & 0x3f;
    uint32_t swz[3] = { (d1 >> 16) & 0xff, (d1 >> 8) & 0xff, d1 & 0xff };
    bool neg[3] = { (d1 >> 26) & 1, (d1 >> 25) & 1, (d1 >> 24) & 1 };
    bool pred_cond = (d1 >> 27) & 1, predicated = (d1 >> 28) & 1;
    bool c_a0 = (d1 >> 29) & 1, c1_addr = (d1 >> 30) & 1, c0_addr = (d1 >> 31) & 1;
    uint32_t sreg[3] = { (d2 >> 16) & 0xff, (d2 >> 8) & 0xff, d2 & 0xff };
    uint32_t vop = (d2 >> 24) & 0x1f;
    bool istemp[3] = { (d2 >> 31) & 1, (d2 >> 30) & 1, (d2 >> 29) & 1 };
    V4 src[3];
    V4 vr = { { 0, 0, 0, 0 } };
    bool vrep = false;

    if (predicated && pred_cond != t->pred) {
        return;
    }

    /* operands */
    for (int i = 0; i < 3; i++) {
        V4 raw;
        bool absv;
        if (istemp[i]) {
            raw = *tmp(t, sreg[i], sreg[i] & 0x40);
            absv = sreg[i] & 0x80;
        } else {
            bool addressed = i == 0 ? c0_addr :
                             i == 1 ? (istemp[0] ? c0_addr : c1_addr) :
                             ((istemp[0] && istemp[1]) ? c0_addr : c1_addr);
            int idx = sreg[i] + (addressed ? (c_a0 ? t->a0 : t->aL) : 0);
            raw = get_const(s, t, idx);
            absv = abs_c;
        }
        for (int j = 0; j < 4; j++) {
            float v = raw.f[((swz[i] >> (2 * j)) + j) & 3];
            if (absv) {
                v = fabsf(v);
            }
            src[i].f[j] = neg[i] ? -v : v;
        }
    }
    V4 *a = &src[0], *b = &src[1], *c = &src[2];

    uint32_t vwm = vmask, swm = smask;
    if (exp) {
        vwm &= ~smask;
        swm &= ~vmask;
    }

    if (vwm || vop >= 20) {
        switch (vop) {
        case 0: for (int i = 0; i < 4; i++) vr.f[i] = a->f[i] + b->f[i]; break;
        case 1: for (int i = 0; i < 4; i++) vr.f[i] = mulz(a->f[i], b->f[i]); break;
        case 2: for (int i = 0; i < 4; i++) vr.f[i] = a->f[i] >= b->f[i] ? a->f[i] : b->f[i]; break;
        case 3: for (int i = 0; i < 4; i++) vr.f[i] = a->f[i] < b->f[i] ? a->f[i] : b->f[i]; break;
        case 4: for (int i = 0; i < 4; i++) vr.f[i] = a->f[i] == b->f[i]; break;
        case 5: for (int i = 0; i < 4; i++) vr.f[i] = a->f[i] > b->f[i]; break;
        case 6: for (int i = 0; i < 4; i++) vr.f[i] = a->f[i] >= b->f[i]; break;
        case 7: for (int i = 0; i < 4; i++) vr.f[i] = a->f[i] != b->f[i]; break;
        case 8: for (int i = 0; i < 4; i++) vr.f[i] = a->f[i] - floorf(a->f[i]); break;
        case 9: for (int i = 0; i < 4; i++) vr.f[i] = truncf(a->f[i]); break;
        case 10: for (int i = 0; i < 4; i++) vr.f[i] = floorf(a->f[i]); break;
        case 11: for (int i = 0; i < 4; i++) vr.f[i] = mulz(a->f[i], b->f[i]) + c->f[i]; break;
        case 12: for (int i = 0; i < 4; i++) vr.f[i] = a->f[i] == 0.0f ? b->f[i] : c->f[i]; break;
        case 13: for (int i = 0; i < 4; i++) vr.f[i] = a->f[i] >= 0.0f ? b->f[i] : c->f[i]; break;
        case 14: for (int i = 0; i < 4; i++) vr.f[i] = a->f[i] > 0.0f ? b->f[i] : c->f[i]; break;
        case 15:
            vr.f[0] = mulz(a->f[0], b->f[0]) + mulz(a->f[1], b->f[1]) +
                      mulz(a->f[2], b->f[2]) + mulz(a->f[3], b->f[3]);
            vrep = true;
            break;
        case 16:
            vr.f[0] = mulz(a->f[0], b->f[0]) + mulz(a->f[1], b->f[1]) +
                      mulz(a->f[2], b->f[2]);
            vrep = true;
            break;
        case 17:
            vr.f[0] = mulz(a->f[0], b->f[0]) + mulz(a->f[1], b->f[1]) + c->f[0];
            vrep = true;
            break;
        case 18: {          /* CUBE: operand .z_xy */
            float x = a->f[2], y = a->f[3], z = a->f[0];
            float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
            if (az >= ax && az >= ay) {
                vr.f[0] = -y; vr.f[1] = z < 0 ? -x : x; vr.f[2] = z;
                vr.f[3] = z < 0 ? 5 : 4;
            } else if (ay >= ax) {
                vr.f[0] = y < 0 ? -z : z; vr.f[1] = x; vr.f[2] = y;
                vr.f[3] = y < 0 ? 3 : 2;
            } else {
                vr.f[0] = -y; vr.f[1] = x < 0 ? z : -z; vr.f[2] = x;
                vr.f[3] = x < 0 ? 1 : 0;
            }
            vr.f[2] *= 2.0f;
            break;
        }
        case 19: {
            float m = a->f[0];
            for (int i = 1; i < 4; i++) {
                m = a->f[i] > m ? a->f[i] : m;
            }
            vr.f[0] = m;
            vrep = true;
            break;
        }
        case 20: case 21: case 22: case 23: {   /* PRED_SET*_PUSH */
            bool c3, c0;
            float bw = b->f[3], bx = b->f[0];
            switch (vop) {
            case 20: c3 = bw == 0; c0 = bx == 0; break;
            case 21: c3 = bw != 0; c0 = bx != 0; break;
            case 22: c3 = bw > 0; c0 = bx > 0; break;
            default: c3 = bw >= 0; c0 = bx >= 0; break;
            }
            t->pred = a->f[3] == 0.0f && c3;
            vr.f[0] = (a->f[0] == 0.0f && c0) ? 0.0f : a->f[0] + 1.0f;
            vrep = true;
            break;
        }
        case 24: case 25: case 26: case 27: {  /* KILL* */
            bool k = false;
            for (int i = 0; i < 4; i++) {
                switch (vop) {
                case 24: k |= a->f[i] == b->f[i]; break;
                case 25: k |= a->f[i] > b->f[i]; break;
                case 26: k |= a->f[i] >= b->f[i]; break;
                default: k |= a->f[i] != b->f[i]; break;
                }
            }
            if (k) {
                t->kill = true;
            }
            vr.f[0] = k;
            vrep = true;
            break;
        }
        case 28:
            vr.f[0] = 1.0f; vr.f[1] = mulz(a->f[1], b->f[1]);
            vr.f[2] = a->f[2]; vr.f[3] = b->f[3];
            break;
        case 29: {          /* MAXA */
            float aw = a->f[3] < -256 ? -256 : a->f[3] > 255 ? 255 : a->f[3];
            t->a0 = (int)floorf(aw + 0.5f);
            for (int i = 0; i < 4; i++) vr.f[i] = a->f[i] >= b->f[i] ? a->f[i] : b->f[i];
            break;
        }
        default:
            break;
        }
        if (vrep) {
            vr.f[1] = vr.f[2] = vr.f[3] = vr.f[0];
        }
    }

    /* scalar: src3 (raw, before the vector swizzle above re-applied) */
    float so[2] = { 0, 0 };
    {
        uint32_t sw = swz[2];
        if (sop >= 42 && sop <= 47) {
            /* MUL/ADD/SUB_CONST: c.w op r.x, r from scalar opc & src3 bits */
            int cidx = sreg[2] + (c1_addr ? (c_a0 ? t->a0 : t->aL) : 0);
            V4 cv = get_const(s, t, cidx);
            uint32_t rr = (sop & 1) | (istemp[2] << 1) | (sw & 0x3c);
            so[0] = cv.f[((sw >> 6) + 3) & 3];
            so[1] = t->r[rr & 63].f[(sw + 0) & 3];
            if (abs_c) {
                so[0] = fabsf(so[0]);
            }
            if (neg[2]) {
                so[0] = -so[0];
                so[1] = -so[1];
            }
        } else {
            so[0] = c->f[3];
            so[1] = c->f[0];
        }
    }
    float x = so[0], y = so[1], p = t->prev;
    switch (sop) {
    case 0: case 44: case 45: p = x + y; break;
    case 1: p = x + p; break;
    case 2: case 42: case 43: p = mulz(x, y); break;
    case 3: p = mulz(x, p); break;
    case 4:
        p = (p == -FLT_MAX || !isfinite(p) || !isfinite(y) || y <= 0) ?
            -FLT_MAX : mulz(x, p);
        break;
    case 5: p = x >= y ? x : y; break;
    case 6: p = x < y ? x : y; break;
    case 7: p = x == 0.0f; break;
    case 8: p = x > 0.0f; break;
    case 9: p = x >= 0.0f; break;
    case 10: p = x != 0.0f; break;
    case 11: p = x - floorf(x); break;
    case 12: p = truncf(x); break;
    case 13: p = floorf(x); break;
    case 14: p = exp2f(x); break;
    case 15: p = log2f(x); if (p == -INFINITY) p = -FLT_MAX; break;
    case 16: p = log2f(x); break;
    case 17: p = 1.0f / x; if (isinf(p)) p = p < 0 ? -FLT_MAX : FLT_MAX; break;
    case 18: p = 1.0f / x; if (isinf(p)) p = p < 0 ? -0.0f : 0.0f; break;
    case 19: p = 1.0f / x; break;
    case 20: p = 1.0f / sqrtf(x); if (isinf(p)) p = p < 0 ? -FLT_MAX : FLT_MAX; break;
    case 21: p = 1.0f / sqrtf(x); if (isinf(p)) p = 0.0f; break;
    case 22: p = 1.0f / sqrtf(x); break;
    case 23: case 24: {
        float cx = x < -256 ? -256 : x > 255 ? 255 : x;
        t->a0 = (int)floorf(sop == 23 ? cx + 0.5f : cx);
        p = x >= y ? x : y;
        break;
    }
    case 25: case 46: case 47: p = x - y; break;
    case 26: p = x - p; break;
    case 27: t->pred = x == 0.0f; p = !t->pred; break;
    case 28: t->pred = x != 0.0f; p = !t->pred; break;
    case 29: t->pred = x > 0.0f; p = !t->pred; break;
    case 30: t->pred = x >= 0.0f; p = !t->pred; break;
    case 31: t->pred = x == 1.0f; p = t->pred ? 0.0f : (x == 0.0f ? 1.0f : x); break;
    case 32: { float n = x - 1.0f; t->pred = n <= 0.0f; p = t->pred ? 0.0f : n; break; }
    case 33: t->pred = false; p = FLT_MAX; break;
    case 34: t->pred = x == 0.0f; p = t->pred ? 0.0f : x; break;
    case 35: p = x == 0.0f; if (p != 0) t->kill = true; break;
    case 36: p = x > 0.0f; if (p != 0) t->kill = true; break;
    case 37: p = x >= 0.0f; if (p != 0) t->kill = true; break;
    case 38: p = x != 0.0f; if (p != 0) t->kill = true; break;
    case 39: p = x == 1.0f; if (p != 0) t->kill = true; break;
    case 40: p = sqrtf(x); break;
    case 48: p = sinf(x); break;
    case 49: p = cosf(x); break;
    case 50: break;
    default: break;
    }
    t->prev = p;

    if (vclamp) {
        for (int i = 0; i < 4; i++) {
            vr.f[i] = sat(vr.f[i]);
        }
    }
    float sr = sclamp ? sat(p) : p;

    if (exp) {
        V4 *e = &t->exp[vdst & 63];
        uint32_t one = vmask & smask;
        uint32_t zero = sdst_rel ? (0xf & ~(vmask | smask)) : 0;
        for (int i = 0; i < 4; i++) {
            uint32_t bit = 1u << i;
            if (vwm & bit) {
                e->f[i] = vr.f[i];
            } else if (swm & bit) {
                e->f[i] = sr;
            } else if (one & bit) {
                e->f[i] = 1.0f;
            } else if (zero & bit) {
                e->f[i] = 0.0f;
            }
        }
        if (vmask | smask | zero) {
            t->exp_mask |= 1ull << (vdst & 63);
        }
    } else {
        if (vwm) {
            V4 *d = tmp(t, vdst, vdst_rel);
            for (int i = 0; i < 4; i++) {
                if (vwm & (1u << i)) {
                    d->f[i] = vr.f[i];
                }
            }
        }
        if (swm) {
            V4 *d = tmp(t, sdst, sdst_rel);
            for (int i = 0; i < 4; i++) {
                if (swm & (1u << i)) {
                    d->f[i] = sr;
                }
            }
        }
    }
}

/* ---------------- fetches ---------------- */

static float half_to_float(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v;
    if (e == 0) {
        v = ldexpf(m, -24);
    } else if (e == 31) {
        v = m ? NAN : INFINITY;
    } else {
        v = ldexpf(m | 0x400, (int)e - 25);
    }
    return s ? -v : v;
}

static void store_fetch(Thread *t, uint32_t dst, bool rel, uint32_t dswz,
                        const float *v)
{
    V4 *d = tmp(t, dst, rel);
    for (int i = 0; i < 4; i++) {
        uint32_t c = (dswz >> (3 * i)) & 7;
        switch (c) {
        case 0: case 1: case 2: case 3: d->f[i] = v[c]; break;
        case 4: d->f[i] = 0.0f; break;
        case 5: d->f[i] = 1.0f; break;
        case 7: break;
        default: d->f[i] = 0.0f; break;
        }
    }
}

/* unpack packed integer components: widths/offsets in bits */
static void unpack_bits(const uint32_t *dw, int n, const int *wid,
                        const int *off, bool sign, bool norm, float *out)
{
    for (int i = 0; i < n; i++) {
        uint32_t word = dw[off[i] >> 5];
        int o = off[i] & 31, w = wid[i];
        uint32_t m = w == 32 ? 0xffffffffu : ((1u << w) - 1);
        uint32_t v = (word >> o) & m;
        if (sign) {
            int32_t sv = (int32_t)(v << (32 - w)) >> (32 - w);
            out[i] = sv;
            if (norm) {
                out[i] = out[i] / (float)((1u << (w - 1)) - 1);
                if (out[i] < -1.0f) {
                    out[i] = -1.0f;
                }
            }
        } else {
            out[i] = v;
            if (norm) {
                out[i] = out[i] / (float)m;
            }
        }
    }
}

/* Decode one element of surface format @fmt from @dw into RGBA floats. */
static float unorm8[256];

static void __attribute__((constructor)) unorm8_init(void)
{
    for (int i = 0; i < 256; i++) {
        unorm8[i] = i / 255.0f;
    }
}

static bool decode_format(uint32_t fmt, const uint32_t *dw, bool sign,
                          bool norm, float *v)
{
    static const int w8888[] = { 8, 8, 8, 8 }, o8888[] = { 0, 8, 16, 24 };
    static const int w565[] = { 5, 6, 5 }, o565[] = { 0, 5, 11 };
    static const int w4444[] = { 4, 4, 4, 4 }, o4444[] = { 0, 4, 8, 12 };
    static const int w1555[] = { 5, 5, 5, 1 }, o1555[] = { 0, 5, 10, 15 };
    static const int w5551[] = { 1, 5, 5, 5 }, o5551[] = { 0, 1, 6, 11 };
    static const int w1616[] = { 16, 16, 16, 16 }, o1616[] = { 0, 16, 32, 48 };
    static const int w2101010[] = { 10, 10, 10, 2 }, o2101010[] = { 0, 10, 20, 30 };
    static const int w88[] = { 8, 8 }, o88[] = { 0, 8 };
    static const int w8[] = { 8 }, o8[] = { 0 };

    if (!sign && norm) {
        uint32_t x = dw[0];
        switch (fmt) {
        case 6: case 14:
            v[0] = unorm8[x & 0xff];
            v[1] = unorm8[(x >> 8) & 0xff];
            v[2] = unorm8[(x >> 16) & 0xff];
            v[3] = unorm8[x >> 24];
            return true;
        case 4:
            v[0] = (x & 31) * (1.0f / 31);
            v[1] = ((x >> 5) & 63) * (1.0f / 63);
            v[2] = ((x >> 11) & 31) * (1.0f / 31);
            v[3] = 1.0f;
            return true;
        }
    }
    v[0] = v[1] = v[2] = 0.0f;
    v[3] = 1.0f;
    switch (fmt) {
    case 2: case 8: case 9:             /* 8, 8_A, 8_B */
        unpack_bits(dw, 1, w8, o8, sign, norm, v);
        return true;
    case 3: unpack_bits(dw, 4, w1555, o1555, sign, norm, v); return true;
    case 13: unpack_bits(dw, 4, w5551, o5551, sign, norm, v); return true;
    case 4: unpack_bits(dw, 3, w565, o565, sign, norm, v); return true;
    case 6: case 14:
        unpack_bits(dw, 4, w8888, o8888, sign, norm, v);
        return true;
    case 7: unpack_bits(dw, 4, w2101010, o2101010, sign, norm, v); return true;
    case 10: unpack_bits(dw, 2, w88, o88, sign, norm, v); return true;
    case 15: unpack_bits(dw, 4, w4444, o4444, sign, norm, v); return true;
    case 25: unpack_bits(dw, 2, w1616, o1616, sign, norm, v); return true;
    case 26: unpack_bits(dw, 4, w1616, o1616, sign, norm, v); return true;
    case 31:
        v[0] = half_to_float(dw[0]);
        v[1] = half_to_float(dw[0] >> 16);
        return true;
    case 32:
        v[0] = half_to_float(dw[0]);
        v[1] = half_to_float(dw[0] >> 16);
        v[2] = half_to_float(dw[1]);
        v[3] = half_to_float(dw[1] >> 16);
        return true;
    case 33: case 34: case 35: {
        int n = fmt - 32;
        for (int i = 0; i < n; i++) {
            if (sign) {
                v[i] = (float)(int32_t)dw[i];
                if (norm) v[i] /= 2147483647.0f;
            } else {
                v[i] = (float)dw[i];
                if (norm) v[i] /= 4294967295.0f;
            }
        }
        return true;
    }
    case 36: case 37: case 38: case 57: {
        int n = fmt == 36 ? 1 : fmt == 37 ? 2 : fmt == 38 ? 4 : 3;
        for (int i = 0; i < n; i++) {
            v[i] = f_of(dw[i]);
        }
        return true;
    }
    default:
        return false;
    }
}

static int format_bytes(uint32_t fmt)
{
    switch (fmt) {
    case 2: case 8: case 9: return 1;
    case 3: case 4: case 5: case 10: case 13: case 15: return 2;
    case 6: case 7: case 14: case 25: case 31: case 33: case 36: return 4;
    case 26: case 32: case 34: case 37: return 8;
    case 57: return 12;
    case 35: case 38: return 16;
    default: return 0;
    }
}

static void vtx_fetch(AdrenoState *s, Thread *t, const uint32_t *w,
                      uint32_t *last_addr)
{
    uint32_t d0 = w[0], d1 = w[1], d2 = w[2];
    uint32_t src = (d0 >> 5) & 0x3f, src_rel = (d0 >> 11) & 1;
    uint32_t dst = (d0 >> 12) & 0x3f, dst_rel = (d0 >> 18) & 1;
    uint32_t cidx = ((d0 >> 20) & 0x1f) * 3 + ((d0 >> 25) & 3);
    uint32_t sswz = (d0 >> 30) & 3;
    uint32_t dswz = d1 & 0xfff;
    bool sign = (d1 >> 12) & 1, norm = !((d1 >> 13) & 1);
    bool rounded = (d1 >> 15) & 1;
    uint32_t fmt = (d1 >> 16) & 0x3f;
    int exp_adj = ((int32_t)(d1 << 2)) >> 26;      /* bits 24-29 */
    /*
     * Bit 30 is the Xenos "mini fetch" flag, but the a2xx compilers set it
     * on every fetch after the first, each with its own constant: always
     * compute the address.
     */
    uint32_t stride = d2 & 0xff;                  /* bytes on a2xx */
    uint32_t offset = (d2 >> 8) & 0x3fffff;       /* bytes */
    uint32_t fc0 = s->regs[ADRENO_REG_FETCH + (cidx * 2) % ADRENO_FETCH_CONSTS];
    float v[4];

    {
        float iv = tmp(t, src, src_rel)->f[sswz];
        uint32_t index = (uint32_t)(int32_t)floorf(iv + (rounded ? 0.5f : 0.0f));
        *last_addr = (fc0 & ~3u) + stride * index;
    }
    uint32_t addr = *last_addr + offset;
    uint32_t dw[4] = { 0, 0, 0, 0 };
    int nb = format_bytes(fmt);
    adreno_read(s, addr, dw, nb > 16 || nb <= 0 ? 16 : ((nb + 3) & ~3));
    for (int i = 0; i < 4; i++) {
        dw[i] = le32_to_cpu(dw[i]);
    }
    if (!decode_format(fmt, dw, sign, norm, v)) {
        qemu_log_mask(LOG_UNIMP, "adreno: vertex format %u\n", fmt);
    }
    if (exp_adj) {
        float f = ldexpf(1.0f, exp_adj);
        for (int i = 0; i < 4; i++) {
            v[i] *= f;
        }
    }
    store_fetch(t, dst, dst_rel, dswz, v);
}

/* ---- textures ---- */

typedef struct TexInfo {
    uint32_t base, pitch, w, h, fmt, bpp;
    uint32_t clamp_x, clamp_y;
    uint32_t swz[4];
    bool tiled, sign, norm;
    int mag, min;
    int exp_adj;
} TexInfo;

/*
 * Byte offset of block (x, y) in a tiled 2D surface: the Xenos/Yamato
 * layout (32x32 macro tiles, bank/pipe swizzle), pitch in blocks.
 */
static uint32_t tiled_offset(uint32_t x, uint32_t y, uint32_t pitch,
                             uint32_t bpp)
{
    uint32_t log2b = bpp >= 16 ? 4 : bpp >= 8 ? 3 : bpp >= 4 ? 2 : bpp >= 2 ? 1 : 0;
    uint32_t pa = (pitch + 31) & ~31u;
    uint32_t outer = ((y >> 5) * (pa >> 5) + (x >> 5)) << 6;
    uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
    uint32_t oi = (outer | inner) << log2b;
    uint32_t bank = (y >> 4) & 1;
    uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);

    return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oi & 15) |
           (((oi >> 4) & 1) << 5) | (((oi >> 5) & 7) << 8) | ((oi >> 8) << 12);
}

static void tex_info(AdrenoState *s, uint32_t idx, TexInfo *ti)
{
    const uint32_t *c = &s->regs[ADRENO_REG_FETCH + (idx * 6) % ADRENO_FETCH_CONSTS];

    ti->clamp_x = (c[0] >> 10) & 7;
    ti->clamp_y = (c[0] >> 13) & 7;
    ti->pitch = ((c[0] >> 22) & 0x1ff) << 5;
    ti->tiled = c[0] >> 31;
    ti->sign = ((c[0] >> 2) & 3) == 1;
    ti->fmt = c[1] & 0x3f;
    ti->base = c[1] & 0xfffff000;
    ti->w = (c[2] & 0x1fff) + 1;
    ti->h = ((c[2] >> 13) & 0x1fff) + 1;
    ti->norm = !(c[3] & 1);
    for (int i = 0; i < 4; i++) {
        ti->swz[i] = (c[3] >> (1 + 3 * i)) & 7;
    }
    ti->exp_adj = ((int32_t)(c[3] << 13)) >> 26;
    ti->mag = (c[3] >> 19) & 3;
    ti->min = (c[3] >> 21) & 3;
    ti->bpp = format_bytes(ti->fmt);
    if (!ti->pitch) {
        ti->pitch = ti->w;
    }
}

static int wrap_coord(int i, int n, uint32_t mode)
{
    switch (mode) {
    case 0:                             /* wrap */
        i %= n;
        return i < 0 ? i + n : i;
    case 1: {                           /* mirror */
        int p = 2 * n;
        i %= p;
        if (i < 0) {
            i += p;
        }
        return i < n ? i : p - 1 - i;
    }
    case 3: case 5: case 7:             /* mirror once */
        if (i < 0) {
            i = -1 - i;
        }
        return i >= n ? n - 1 : i;
    default:                            /* clamp */
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    }
}

static void texel(AdrenoState *s, const TexInfo *ti, int x, int y, float *v)
{
    uint32_t dw[4] = { 0, 0, 0, 0 };

    x = wrap_coord(x, ti->w, ti->clamp_x);
    y = wrap_coord(y, ti->h, ti->clamp_y);
    if (ti->fmt == 47 || ti->fmt == 48 || ti->fmt == 46) {
        /* ETC1: 4x4 blocks of 8 bytes */
        uint32_t bw = (ti->pitch + 3) / 4;
        uint32_t blk = ((y / 4) * bw + x / 4) * 8;
        uint8_t b[8];
        adreno_read(s, ti->base + blk, b, 8);
        adreno_etc1_texel(b, x & 3, y & 3, v);
        return;
    }
    uint32_t off = ti->tiled ? tiled_offset(x, y, ti->pitch, ti->bpp)
                             : (y * ti->pitch + x) * ti->bpp;
    uint32_t va = ti->base + off, nb = ti->bpp <= 16 ? ti->bpp : 16;
    uint8_t *hp = adreno_host_page(s, va);
    if (hp && (va & 4095) + nb <= 4096) {
        memcpy(dw, hp + (va & 4095), nb);
    } else {
        adreno_read(s, va, dw, nb);
    }
    for (int i = 0; i < 4; i++) {
        dw[i] = le32_to_cpu(dw[i]);
    }
    decode_format(ti->fmt, dw, ti->sign, ti->norm, v);
}

/* row cache: avoid one guest-memory translation per texel */
static void tex_sample(AdrenoState *s, const TexInfo *ti, float u, float v,
                       bool denorm, int filter, float *out)
{
    float raw[4];

    if (!denorm) {
        u *= ti->w;
        v *= ti->h;
    }
    if (filter == 1) {                  /* bilinear */
        float fu = u - 0.5f, fv = v - 0.5f;
        int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
        float ax = fu - x0, ay = fv - y0;
        float t00[4], t10[4], t01[4], t11[4];
        /* 1:1 composition samples texel centres: one fetch is enough */
        const float eps = 1.0f / 512;
        if ((ax < eps || ax > 1 - eps) && (ay < eps || ay > 1 - eps)) {
            texel(s, ti, x0 + (ax > 0.5f), y0 + (ay > 0.5f), raw);
            goto filtered;
        }
        texel(s, ti, x0, y0, t00);
        texel(s, ti, x0 + 1, y0, t10);
        texel(s, ti, x0, y0 + 1, t01);
        texel(s, ti, x0 + 1, y0 + 1, t11);
        for (int i = 0; i < 4; i++) {
            float top = t00[i] + (t10[i] - t00[i]) * ax;
            float bot = t01[i] + (t11[i] - t01[i]) * ax;
            raw[i] = top + (bot - top) * ay;
        }
    } else {
        texel(s, ti, (int)floorf(u), (int)floorf(v), raw);
    }
filtered:
    if (ti->exp_adj) {
        float f = ldexpf(1.0f, ti->exp_adj);
        for (int i = 0; i < 4; i++) {
            raw[i] *= f;
        }
    }
    for (int i = 0; i < 4; i++) {
        uint32_t sw = ti->swz[i];
        out[i] = sw < 4 ? raw[sw] : sw == 5 ? 1.0f : 0.0f;
    }
}

static uint64_t stat_fast;              /* draws on the fast path */

/* texture constants decoded once per draw */
static TexInfo tic[32];
static uint32_t tic_valid;

static void tex_fetch(AdrenoState *s, Thread *t, const uint32_t *w)
{
    uint32_t d0 = w[0], d1 = w[1], d2 = w[2];
    uint32_t opc = d0 & 0x1f;
    uint32_t src = (d0 >> 5) & 0x3f, src_rel = (d0 >> 11) & 1;
    uint32_t dst = (d0 >> 12) & 0x3f, dst_rel = (d0 >> 18) & 1;
    uint32_t cidx = (d0 >> 20) & 0x1f;
    bool denorm = (d0 >> 25) & 1;
    uint32_t sswz = (d0 >> 26) & 0x3f;
    uint32_t dswz = d1 & 0xfff;
    int mag = (d1 >> 12) & 3;
    float v[4] = { 0, 0, 0, 1 };
    TexInfo ti;

    (void)d2;
    if (opc != 1) {                     /* only TEX_FETCH */
        store_fetch(t, dst, dst_rel, dswz, v);
        return;
    }
    if (!(tic_valid & (1u << cidx))) {
        tex_info(s, cidx, &tic[cidx]);
        tic_valid |= 1u << cidx;
    }
    ti = tic[cidx];
    V4 *c = tmp(t, src, src_rel);
    float u = c->f[sswz & 3], vv = c->f[(sswz >> 2) & 3];
    int filter = mag == 3 ? ti.mag : mag;
    if (!ti.bpp && !(ti.fmt >= 46 && ti.fmt <= 48)) {
        static bool warned[64];
        if (!warned[ti.fmt]) {
            warned[ti.fmt] = true;
            qemu_log_mask(LOG_UNIMP, "adreno: texture format %u\n", ti.fmt);
        }
    } else {
        tex_sample(s, &ti, u, vv, denorm, filter, v);
    }
    store_fetch(t, dst, dst_rel, dswz, v);
}

/* ---------------- control flow ---------------- */

static void cf_get(const uint32_t *prog, uint32_t i, uint32_t *lo, uint32_t *hi)
{
    const uint32_t *p = &prog[3 * (i >> 1)];
    if (i & 1) {
        *lo = (p[1] >> 16) | (p[2] << 16);
        *hi = p[2] >> 16;
    } else {
        *lo = p[0];
        *hi = p[1] & 0xffff;
    }
}

static bool bool_const(AdrenoState *s, uint32_t a)
{
    return (s->regs[ADRENO_REG_BOOL + ((a >> 5) & 7)] >> (a & 31)) & 1;
}

static void run_shader(AdrenoState *s, Thread *t, uint32_t base)
{
    const uint32_t *prog = &s->istore[base * 3];
    uint32_t limit = ADRENO_ISTORE_DWORDS / 3 - base;
    uint32_t vaddr = 0;
    int call_stack[4], call_depth = 0;
    int loop_cnt[4], loop_depth = 0;

    for (uint32_t pc = 0, steps = 0; pc < 512 && steps < 4096; steps++) {
        uint32_t lo, hi;
        cf_get(prog, pc, &lo, &hi);
        uint64_t cf = lo | ((uint64_t)hi << 32);
        uint32_t opc = (cf >> 44) & 0xf;
        uint32_t next = pc + 1;

        switch (opc) {
        case 0: case 12: case 15:       /* NOP, ALLOC, MARK_VS_FETCH_DONE */
            break;
        case 1: case 2: case 3: case 4: case 5: case 6: case 13: case 14: {
            uint32_t addr = cf & 0x1ff, count = (cf >> 12) & 7;
            uint32_t serial = (cf >> 16) & 0xfff;
            uint32_t baddr = (cf >> 34) & 0xff;
            bool cond = (cf >> 42) & 1;
            bool run = true;
            if (opc == 3 || opc == 4 || opc == 13 || opc == 14) {
                run = bool_const(s, baddr) == cond;
            } else if (opc == 5 || opc == 6) {
                run = t->pred == cond;
            }
            if (run) {
                for (uint32_t i = 0; i < count && addr + i < limit; i++) {
                    const uint32_t *w = &prog[3 * (addr + i)];
                    if ((serial >> (2 * i)) & 1) {
                        bool pselect = (w[1] >> 31) & 1;
                        bool pcond = (w[2] >> 31) & 1;
                        if (pselect && pcond != t->pred) {
                            continue;
                        }
                        if ((w[0] & 0x1f) == 0) {
                            vtx_fetch(s, t, w, &vaddr);
                        } else {
                            tex_fetch(s, t, w);
                        }
                    } else {
                        alu_exec(s, t, w);
                    }
                }
            }
            if (opc == 2 || opc == 4 || opc == 6 || opc == 14) {
                return;
            }
            break;
        }
        case 7: {                       /* LOOP_START */
            uint32_t id = (cf >> 16) & 0x1f;
            uint32_t lc = s->regs[ADRENO_REG_LOOP + (id & 31)];
            int count = lc & 0xff;
            if (loop_depth >= 4 || count == 0) {
                next = cf & 0x3ff;
                break;
            }
            loop_cnt[loop_depth++] = count;
            t->aL = (lc >> 8) & 0xff;
            break;
        }
        case 8: {                       /* LOOP_END */
            uint32_t id = (cf >> 16) & 0x1f;
            uint32_t lc = s->regs[ADRENO_REG_LOOP + (id & 31)];
            if (!loop_depth) {
                break;
            }
            t->aL += (int8_t)(lc >> 16);
            if (--loop_cnt[loop_depth - 1] > 0) {
                next = cf & 0x3ff;
            } else {
                loop_depth--;
            }
            break;
        }
        case 9: case 11: {              /* COND_CALL, COND_JMP */
            bool force = (cf >> 13) & 1, predj = (cf >> 14) & 1;
            bool cond = (cf >> 42) & 1;
            uint32_t baddr = (cf >> 34) & 0xff;
            bool take = force ||
                        (predj ? t->pred == cond : bool_const(s, baddr) == cond);
            if (take) {
                if (opc == 9) {
                    if (call_depth >= 4) {
                        break;
                    }
                    call_stack[call_depth++] = pc + 1;
                }
                next = cf & 0x3ff;
            }
            break;
        }
        case 10:                        /* RETURN */
            if (call_depth) {
                next = call_stack[--call_depth];
            }
            break;
        default:
            return;
        }
        pc = next;
    }
}

/* ---------------- pipeline state ---------------- */

typedef struct PVtx {
    float x, y, z, rw;      /* window coordinates, 1/w */
    V4 p[16];
    float clipw;
} PVtx;

/*
 * Pixel shader fast path.  The program is executed once symbolically per
 * draw; every register component is tracked as b + a * T[k] + c * P[m]:
 * T the (single) texture fetch result, P the interpolated inputs (m =
 * reg * 4 + comp).  Branches on boolean constants are resolved on the
 * way.  Composition shaders (texture copy, modulate by a constant, text,
 * solid fill) reduce to such a formula and are evaluated per pixel
 * without the interpreter.
 */
#define SYM_NONE    (-1)
#define SYM_BAD     (-2)

typedef struct Sym { float a, b, c; int k, m; } Sym;

typedef struct FastPS {
    bool ok, has_tex, denorm, uses_params;
    uint32_t tcidx, param, uc, vc;
    int mag;
    Sym out[4];
} FastPS;

typedef struct DrawState {
    AdrenoState *s;
    uint32_t mode;          /* EDRAM mode */
    int sx0, sy0, sx1, sy1; /* scissor (inclusive-exclusive) in GMEM pixels */
    uint32_t nparams;
    bool persp;
    Thread pt;                          /* pixel thread, reused */
    FastPS fast;
    /* render target */
    uint32_t cbase, cfmt, cswap, pitch;
    bool sysmem, clinear;               /* rendering straight to memory */
    uint32_t dirty_lo, dirty_hi;        /* memory written, for dirty bits */
    uint32_t samples;                   /* MSAA samples per pixel */
    uint32_t zbase, zfmt;
    uint32_t zctl, cctl, blend, cmask;
    float alpha_ref;
    uint32_t ps_base;
    uint32_t ps_regs;
    bool param_gen;
    uint32_t param_gen_pos;
    /* resolve */
    uint32_t cp_dest, cp_pitch, cp_info, cp_dx, cp_dy, cp_ctl;
    uint64_t pixels;
} DrawState;

static int fmt_bpp_rb(uint32_t f)
{
    switch (f) {
    case 0: case 1: case 2: case 4: case 7: return 2;
    case 3: case 13: return 1;
    case 5: case 6: case 8: case 10: return 4;
    case 9: case 11: return 8;
    case 12: return 16;
    case 14: return 3;
    default: return 4;
    }
}

/* RB color format <-> RGBA float */
static void rb_unpack(uint32_t fmt, uint32_t swap, const uint8_t *p, float *c)
{
    uint32_t v;
    if (fmt == 5 && swap == 0) {
        c[0] = unorm8[p[0]];
        c[1] = unorm8[p[1]];
        c[2] = unorm8[p[2]];
        c[3] = unorm8[p[3]];
        return;
    }
    switch (fmt) {
    case 2:                             /* 5_6_5 */
        v = p[0] | (p[1] << 8);
        c[0] = ((v >> 11) & 31) / 31.0f;
        c[1] = ((v >> 5) & 63) / 63.0f;
        c[2] = (v & 31) / 31.0f;
        c[3] = 1.0f;
        break;
    case 0:                             /* 4_4_4_4 */
        v = p[0] | (p[1] << 8);
        c[0] = ((v >> 12) & 15) / 15.0f;
        c[1] = ((v >> 8) & 15) / 15.0f;
        c[2] = ((v >> 4) & 15) / 15.0f;
        c[3] = (v & 15) / 15.0f;
        break;
    case 1:                             /* 1_5_5_5 */
        v = p[0] | (p[1] << 8);
        c[0] = ((v >> 10) & 31) / 31.0f;
        c[1] = ((v >> 5) & 31) / 31.0f;
        c[2] = (v & 31) / 31.0f;
        c[3] = (v >> 15) & 1;
        break;
    case 3:
        c[0] = c[1] = c[2] = p[0] / 255.0f;
        c[3] = 1.0f;
        break;
    case 4:
        c[0] = p[0] / 255.0f;
        c[1] = p[1] / 255.0f;
        c[2] = 0;
        c[3] = 1.0f;
        break;
    default:                            /* 8_8_8_8 */
        c[0] = p[0] / 255.0f;
        c[1] = p[1] / 255.0f;
        c[2] = p[2] / 255.0f;
        c[3] = p[3] / 255.0f;
        break;
    }
    if (swap == 1) {
        float tt = c[0];
        c[0] = c[2];
        c[2] = tt;
    }
}

static inline uint32_t to_unorm(float v, int max)
{
    return (uint32_t)(sat(v) * max + 0.5f);
}

static void rb_pack(uint32_t fmt, uint32_t swap, const float *cin, uint8_t *p,
                    uint32_t wmask)
{
    float c[4] = { cin[0], cin[1], cin[2], cin[3] };
    uint32_t v;

    if (fmt == 5 && swap == 0 && wmask == 0xf) {
        for (int i = 0; i < 4; i++) {
            float f = c[i] * 255.0f + 0.5f;
            p[i] = f >= 255.0f ? 255 : f > 0.0f ? (uint8_t)f : 0;
        }
        return;
    }
    if (swap == 1) {
        c[0] = cin[2];
        c[2] = cin[0];
    }
    switch (fmt) {
    case 2:
        v = (to_unorm(c[0], 31) << 11) | (to_unorm(c[1], 63) << 5) |
            to_unorm(c[2], 31);
        p[0] = v;
        p[1] = v >> 8;
        break;
    case 0:
        v = (to_unorm(c[0], 15) << 12) | (to_unorm(c[1], 15) << 8) |
            (to_unorm(c[2], 15) << 4) | to_unorm(c[3], 15);
        p[0] = v;
        p[1] = v >> 8;
        break;
    case 1:
        v = (to_unorm(c[0], 31) << 10) | (to_unorm(c[1], 31) << 5) |
            to_unorm(c[2], 31) | (to_unorm(c[3], 1) << 15);
        p[0] = v;
        p[1] = v >> 8;
        break;
    case 3:
        p[0] = to_unorm(c[0], 255);
        break;
    case 4:
        p[0] = to_unorm(c[0], 255);
        p[1] = to_unorm(c[1], 255);
        break;
    default:
        for (int i = 0; i < 4; i++) {
            if (wmask & (1u << i)) {
                p[i] = to_unorm(c[i], 255);
            }
        }
        break;
    }
}

static float blend_factor(uint32_t f, const float *src, const float *dst,
                          const float *k, int ch)
{
    switch (f) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 4: return src[ch];
    case 5: return 1.0f - src[ch];
    case 6: return src[3];
    case 7: return 1.0f - src[3];
    case 8: return dst[ch];
    case 9: return 1.0f - dst[ch];
    case 10: return dst[3];
    case 11: return 1.0f - dst[3];
    case 12: return k[ch];
    case 13: return 1.0f - k[ch];
    case 14: return k[3];
    case 15: return 1.0f - k[3];
    case 16: {
        float a = src[3], b = 1.0f - dst[3];
        return ch == 3 ? 1.0f : (a < b ? a : b);
    }
    default: return 1.0f;
    }
}

static float blend_op(uint32_t op, float s, float d)
{
    switch (op) {
    case 0: return s + d;
    case 1: return s - d;
    case 2: return s < d ? s : d;
    case 3: return s > d ? s : d;
    case 4: return d - s;
    default: return s + d;
    }
}

static bool compare(uint32_t func, float a, float b)
{
    switch (func) {
    case 0: return false;
    case 1: return a < b;
    case 2: return a == b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a != b;
    case 6: return a >= b;
    default: return true;
    }
}

static inline uint8_t *gmem_at(AdrenoState *s, uint32_t off)
{
    /*
     * GMEM addresses wrap: JB's SurfaceFlinger sets the surface pitch to
     * the screen width (480) for 128-wide bins, which only fits because
     * rows past 256 KiB land in the unused columns of earlier rows.
     */
    return &s->gmem[off & (ADRENO_GMEM_SIZE - 1)];
}

/* ---------------- pixels ---------------- */

static inline Sym sym_c(float b)
{
    Sym r = { 0.0f, b, 0.0f, SYM_NONE, SYM_NONE };
    return r;
}

static inline Sym sym_bad(void)
{
    Sym r = { 0.0f, 0.0f, 0.0f, SYM_BAD, SYM_NONE };
    return r;
}

static inline bool sym_ok(Sym x)
{
    return x.k != SYM_BAD;
}

static inline bool sym_const(Sym x)
{
    return x.k == SYM_NONE && x.m == SYM_NONE;
}

static Sym sym_add(Sym x, Sym y)
{
    if (!sym_ok(x) || !sym_ok(y) ||
        (x.k != SYM_NONE && y.k != SYM_NONE && x.k != y.k) ||
        (x.m != SYM_NONE && y.m != SYM_NONE && x.m != y.m)) {
        return sym_bad();
    }
    Sym r = { x.a + y.a, x.b + y.b, x.c + y.c,
              x.k != SYM_NONE ? x.k : y.k, x.m != SYM_NONE ? x.m : y.m };
    return r;
}

static Sym sym_scale(Sym x, float f)
{
    if (f == 0.0f) {
        return sym_c(0.0f);
    }
    x.a *= f;
    x.b *= f;
    x.c *= f;
    return x;
}

static Sym sym_mul(Sym x, Sym y)
{
    if (!sym_ok(x) || !sym_ok(y)) {
        return sym_bad();
    }
    if (sym_const(x) && sym_const(y)) {
        return sym_c(mulz(x.b, y.b));
    }
    if (sym_const(x)) {
        return sym_scale(y, x.b);
    }
    if (sym_const(y)) {
        return sym_scale(x, y.b);
    }
    return sym_bad();
}

static inline bool sym_same(Sym x, Sym y)
{
    return x.k == y.k && x.m == y.m && x.a == y.a && x.b == y.b && x.c == y.c;
}

static Sym sym_minmax(Sym x, Sym y, bool max)
{
    if (!sym_ok(x) || !sym_ok(y)) {
        return sym_bad();
    }
    if (sym_same(x, y)) {
        return x;
    }
    if (sym_const(x) && sym_const(y)) {
        return sym_c(max ? (x.b >= y.b ? x.b : y.b) : (x.b < y.b ? x.b : y.b));
    }
    return sym_bad();
}

/* x within [lo, hi] for every texel in [0, 1] (unsigned formats) */
static bool sym_in_range(Sym x, bool unsigned_tex, float lo, float hi)
{
    if (x.m != SYM_NONE) {
        return false;
    }
    if (x.k == SYM_NONE) {
        return x.b >= lo && x.b <= hi;
    }
    float e0 = x.b, e1 = x.b + x.a;
    return unsigned_tex && MIN(e0, e1) >= lo && MAX(e0, e1) <= hi;
}

static Sym sym_sat(Sym x, bool unsigned_tex)
{
    if (sym_const(x)) {
        return sym_c(sat(x.b));
    }
    return sym_ok(x) && sym_in_range(x, unsigned_tex, 0.0f, 1.0f) ? x
                                                                   : sym_bad();
}

static void ps_analyze(AdrenoState *s, DrawState *ds)
{
    FastPS *f = &ds->fast;
    const uint32_t *prog = &s->istore[ds->ps_base * 3];
    uint32_t limit = ADRENO_ISTORE_DWORDS / 3 - ds->ps_base;
    static Sym R[64][4];
    Sym E[4], prev = sym_c(0.0f);
    Thread ct;
    bool unsigned_tex = true;

    memset(f, 0, sizeof(*f));
    ct.stage = ST_PS;
    for (int i = 0; i < 64; i++) {
        for (int c = 0; c < 4; c++) {
            if ((uint32_t)i < ds->nparams) {
                Sym p = { 0.0f, 0.0f, 1.0f, SYM_NONE, i * 4 + c };
                R[i][c] = p;
            } else {
                R[i][c] = sym_bad();
            }
        }
    }
    if (ds->param_gen) {
        for (int c = 0; c < 4; c++) {
            R[ds->param_gen_pos & 63][c] = sym_bad();
        }
    }
    for (int c = 0; c < 4; c++) {
        E[c] = sym_bad();
    }

    for (uint32_t pc = 0, steps = 0; pc < 64 && steps < 128; steps++) {
        uint32_t lo, hi;
        cf_get(prog, pc, &lo, &hi);
        uint64_t cf = lo | ((uint64_t)hi << 32);
        uint32_t opc = (cf >> 44) & 0xf;
        uint32_t next = pc + 1;
        bool end = false;

        switch (opc) {
        case 0: case 12: case 15:       /* NOP, ALLOC, MARK_VS_FETCH_DONE */
            pc = next;
            continue;
        case 1: case 2:                 /* EXEC, EXEC_END */
            break;
        case 3: case 4: case 13: case 14: {   /* COND_EXEC* on a bool */
            uint32_t baddr = (cf >> 34) & 0xff;
            bool cond = (cf >> 42) & 1;
            if (bool_const(s, baddr) != cond) {
                if (opc == 4 || opc == 14) {
                    end = true;
                    goto finish;
                }
                pc = next;
                continue;
            }
            break;
        }
        case 11: {                      /* COND_JMP */
            bool force = (cf >> 13) & 1, predj = (cf >> 14) & 1;
            bool cond = (cf >> 42) & 1;
            if (predj) {
                return;
            }
            if (force || bool_const(s, (cf >> 34) & 0xff) == cond) {
                next = cf & 0x3ff;
            }
            pc = next;
            continue;
        }
        default:
            return;                     /* loops, calls, predicates */
        }
        end = opc == 2 || opc == 4 || opc == 14;
        uint32_t addr = cf & 0x1ff, count = (cf >> 12) & 7;
        uint32_t serial = (cf >> 16) & 0xfff;
        for (uint32_t i = 0; i < count; i++) {
            if (addr + i >= limit) {
                return;
            }
            const uint32_t *w = &prog[3 * (addr + i)];
            uint32_t d0 = w[0], d1 = w[1], d2 = w[2];
            if ((serial >> (2 * i)) & 1) {
                /* texture fetch from an interpolated input */
                if (((d1 >> 31) & 1) || (d0 & 0x1f) != 1 || f->has_tex) {
                    return;
                }
                uint32_t src = (d0 >> 5) & 0x3f, dst = (d0 >> 12) & 0x3f;
                uint32_t sswz = (d0 >> 26) & 0x3f, dswz = d1 & 0xfff;
                if (((d0 >> 11) & 1) || ((d0 >> 18) & 1)) {
                    return;
                }
                Sym u = R[src][sswz & 3], v = R[src][(sswz >> 2) & 3];
                if (u.k != SYM_NONE || v.k != SYM_NONE || u.m < 0 ||
                    v.m < 0 || u.c != 1.0f || v.c != 1.0f || u.b != 0.0f ||
                    v.b != 0.0f || u.m / 4 != v.m / 4) {
                    return;
                }
                f->has_tex = true;
                f->tcidx = (d0 >> 20) & 0x1f;
                f->denorm = (d0 >> 25) & 1;
                f->mag = (d1 >> 12) & 3;
                f->param = u.m / 4;
                f->uc = u.m & 3;
                f->vc = v.m & 3;
                TexInfo ti;
                tex_info(s, f->tcidx, &ti);
                unsigned_tex = !ti.sign;
                for (int c = 0; c < 4; c++) {
                    uint32_t sel = (dswz >> (3 * c)) & 7;
                    if (sel < 4) {
                        Sym t = { 1.0f, 0.0f, 0.0f, (int)sel, SYM_NONE };
                        R[dst][c] = t;
                    } else if (sel == 5) {
                        R[dst][c] = sym_c(1.0f);
                    } else if (sel != 7) {
                        R[dst][c] = sym_c(0.0f);
                    }
                }
                continue;
            }

            /* ALU */
            uint32_t vdst = d0 & 0x3f, abs_c = (d0 >> 7) & 1;
            uint32_t sdst = (d0 >> 8) & 0x3f, sdst_rel = (d0 >> 14) & 1;
            bool exp = (d0 >> 15) & 1;
            uint32_t vmask = (d0 >> 16) & 0xf, smask = (d0 >> 20) & 0xf;
            bool vclamp = (d0 >> 24) & 1, sclamp = (d0 >> 25) & 1;
            uint32_t sop = (d0 >> 26) & 0x3f;
            uint32_t swz[3] = { (d1 >> 16) & 0xff, (d1 >> 8) & 0xff, d1 & 0xff };
            bool neg[3] = { (d1 >> 26) & 1, (d1 >> 25) & 1, (d1 >> 24) & 1 };
            uint32_t sreg[3] = { (d2 >> 16) & 0xff, (d2 >> 8) & 0xff, d2 & 0xff };
            uint32_t vop = (d2 >> 24) & 0x1f;
            bool istemp[3] = { (d2 >> 31) & 1, (d2 >> 30) & 1, (d2 >> 29) & 1 };
            Sym src[3][4];

            if (((d1 >> 28) & 1) || ((d1 >> 30) & 1) || ((d1 >> 31) & 1) ||
                ((d0 >> 6) & 1) || vop >= 20 || (sop >= 23 && sop != 25 &&
                sop != 26 && sop != 40 && sop != 48 && sop != 49 &&
                sop != 50) || (!exp && sdst_rel)) {
                return;                 /* side effects or addressing */
            }
            for (int k = 0; k < 3; k++) {
                Sym raw[4];
                bool absv;
                if (istemp[k]) {
                    if (sreg[k] & 0x40) {
                        return;
                    }
                    memcpy(raw, R[sreg[k] & 63], sizeof(raw));
                    absv = sreg[k] & 0x80;
                } else {
                    V4 cv = get_const(s, &ct, sreg[k]);
                    for (int c = 0; c < 4; c++) {
                        raw[c] = sym_c(cv.f[c]);
                    }
                    absv = abs_c;
                }
                for (int j = 0; j < 4; j++) {
                    Sym v = raw[((swz[k] >> (2 * j)) + j) & 3];
                    if (absv && sym_ok(v)) {
                        if (sym_const(v)) {
                            v.b = fabsf(v.b);
                        } else if (!sym_in_range(v, unsigned_tex, 0.0f,
                                                 FLT_MAX)) {
                            v = sym_bad();
                        }
                    }
                    if (neg[k]) {
                        v = sym_scale(v, -1.0f);
                        if (sym_const(v) && v.b == 0.0f) {
                            v.b = -0.0f;
                        }
                    }
                    src[k][j] = v;
                }
            }
            uint32_t vwm = vmask, swm = smask;
            if (exp) {
                vwm &= ~smask;
                swm &= ~vmask;
            }
            Sym vr[4];
            for (int c = 0; c < 4; c++) {
                switch (vop) {
                case 0: vr[c] = sym_add(src[0][c], src[1][c]); break;
                case 1: vr[c] = sym_mul(src[0][c], src[1][c]); break;
                case 2: vr[c] = sym_minmax(src[0][c], src[1][c], true); break;
                case 3: vr[c] = sym_minmax(src[0][c], src[1][c], false); break;
                case 11:
                    vr[c] = sym_add(sym_mul(src[0][c], src[1][c]), src[2][c]);
                    break;
                default: vr[c] = sym_bad(); break;
                }
                if (vclamp) {
                    vr[c] = sym_sat(vr[c], unsigned_tex);
                }
            }
            if (vop == 15 || vop == 16) {      /* DOT4 / DOT3 */
                Sym d = sym_c(0.0f);
                for (int c = 0; c < (vop == 15 ? 4 : 3); c++) {
                    d = sym_add(d, sym_mul(src[0][c], src[1][c]));
                }
                if (vclamp) {
                    d = sym_sat(d, unsigned_tex);
                }
                vr[0] = vr[1] = vr[2] = vr[3] = d;
            }
            Sym x = src[2][3], y = src[2][0], p;
            switch (sop) {
            case 0: p = sym_add(x, y); break;
            case 1: p = sym_add(x, prev); break;
            case 2: p = sym_mul(x, y); break;
            case 3: p = sym_mul(x, prev); break;
            case 5: p = sym_minmax(x, y, true); break;
            case 6: p = sym_minmax(x, y, false); break;
            case 50: p = prev; break;
            default: p = sym_bad(); break;
            }
            prev = p;
            Sym sr = sclamp ? sym_sat(p, unsigned_tex) : p;

            if (exp) {
                if (vdst != 0) {
                    return;             /* only the colour export */
                }
                uint32_t one = vmask & smask;
                uint32_t zero = sdst_rel ? (0xf & ~(vmask | smask)) : 0;
                for (int c = 0; c < 4; c++) {
                    uint32_t bit = 1u << c;
                    if (vwm & bit) {
                        E[c] = vr[c];
                    } else if (swm & bit) {
                        E[c] = sr;
                    } else if (one & bit) {
                        E[c] = sym_c(1.0f);
                    } else if (zero & bit) {
                        E[c] = sym_c(0.0f);
                    }
                }
            } else {
                for (int c = 0; c < 4; c++) {
                    if (vwm & (1u << c)) {
                        R[vdst][c] = vr[c];
                    }
                }
                for (int c = 0; c < 4; c++) {
                    if (swm & (1u << c)) {
                        R[sdst][c] = sr;
                    }
                }
            }
        }
finish:
        if (end) {
            for (int c = 0; c < 4; c++) {
                if (!sym_ok(E[c])) {
                    return;
                }
                f->out[c] = E[c];
                f->uses_params |= E[c].m != SYM_NONE;
            }
            f->ok = true;
            return;
        }
        pc = next;
    }
}

/* value of a fast-path output for texel tx and inputs in */
static inline float sym_eval(const Sym *o, const float *tx, const V4 *in)
{
    float v = o->b;
    if (o->k >= 0) {
        v += o->a * tx[o->k];
    }
    if (o->m >= 0) {
        v += o->c * in[o->m >> 2].f[o->m & 3];
    }
    return v;
}

/* storage of colour pixel (x, y): GMEM or, for sysmem targets, guest RAM */
static uint8_t *rt_color(DrawState *ds, int x, int y, int bpp, uint32_t ns)
{
    AdrenoState *s = ds->s;

    if (!ds->sysmem) {
        uint8_t *p = gmem_at(s, ds->cbase + (y * ds->pitch + x) * ns * bpp);
        return p && p + bpp * ns <= s->gmem + ADRENO_GMEM_SIZE ? p : NULL;
    }
    uint32_t off = ds->clinear ? (y * ds->pitch + x) * bpp
                               : tiled_offset(x, y, ds->pitch, bpp);
    uint32_t va = ds->cbase + off;
    uint8_t *hp = adreno_host_page(s, va);
    if (!hp || (va & 4095) + bpp > 4096) {
        return NULL;
    }
    ds->dirty_lo = MIN(ds->dirty_lo, va);
    ds->dirty_hi = MAX(ds->dirty_hi, va + bpp);
    return hp + (va & 4095);
}

static void shade_pixel(DrawState *ds, int x, int y, float z, const V4 *in)
{
    AdrenoState *s = ds->s;
    Thread *tp = &ds->pt;
    int bpp = fmt_bpp_rb(ds->cfmt);
    uint32_t ns = ds->sysmem ? 1 : ds->samples;
    uint8_t *cp = rt_color(ds, x, y, bpp, ns);

    if (!cp) {
        return;
    }

    /* depth test first (early-Z style; shaders here do not write depth) */
    uint8_t *zp = NULL;
    uint32_t zold = 0, znew = 0;
    bool ztest = ds->zctl & 2, zwrite = (ds->zctl >> 2) & 1;
    if (ztest) {
        int zb = ds->zfmt ? 4 : 2;
        zp = gmem_at(s, ds->zbase + (y * ds->pitch + x) * ns * zb);
        if (zp && zp + zb * ns <= s->gmem + ADRENO_GMEM_SIZE) {
            if (ds->zfmt) {
                zold = ldl_le_p(zp) >> 8;
                znew = (uint32_t)(sat(z) * 16777215.0f);
            } else {
                zold = lduw_le_p(zp);
                znew = (uint32_t)(sat(z) * 65535.0f);
            }
            if (!compare((ds->zctl >> 4) & 7, znew, zold)) {
                return;
            }
        }
    }

    /*
     * The draw's thread is reused: temporaries keep the previous pixel's
     * values (undefined on the chip too), so only the control state is
     * reset instead of clearing 2 KiB per pixel.
     */
    if (ds->fast.ok) {
        FastPS *f = &ds->fast;
        float tx[4] = { 0, 0, 0, 0 };
        if (f->has_tex) {
            if (!(tic_valid & (1u << f->tcidx))) {
                tex_info(s, f->tcidx, &tic[f->tcidx]);
                tic_valid |= 1u << f->tcidx;
            }
            const TexInfo *ti = &tic[f->tcidx];
            tex_sample(s, ti, in[f->param].f[f->uc], in[f->param].f[f->vc],
                       f->denorm, f->mag == 3 ? ti->mag : f->mag, tx);
        }
        for (int c = 0; c < 4; c++) {
            tp->exp[0].f[c] = sym_eval(&f->out[c], tx, in);
        }
        goto shaded;
    }
    tp->exp_mask = 0;
    tp->prev = 0;
    tp->pred = false;
    tp->a0 = tp->aL = 0;
    tp->kill = false;
    memcpy(tp->r, in, ds->nparams * sizeof(V4));
    if (ds->param_gen) {
        V4 *g = &tp->r[ds->param_gen_pos & 63];
        g->f[0] = x + 0.5f;
        g->f[1] = y + 0.5f;
        g->f[2] = 0;
        g->f[3] = 0;
    }
    run_shader(s, tp, ds->ps_base);
    if (tp->kill) {
        return;
    }
shaded:;
    float src[4];
    memcpy(src, tp->exp[0].f, sizeof(src));

    /* alpha test */
    if ((ds->cctl >> 3) & 1) {
        if (!compare(ds->cctl & 7, src[3], ds->alpha_ref)) {
            return;
        }
    }
    if (zp && zwrite) {
        for (uint32_t i = 0; i < ns; i++) {
            if (ds->zfmt) {
                uint8_t *q = zp + i * 4;
                stl_le_p(q, (ldl_le_p(q) & 0xff) | (znew << 8));
            } else {
                stw_le_p(zp + i * 2, znew);
            }
        }
    }

    float out[4];
    bool blend = !((ds->cctl >> 5) & 1) ||
                 ((ds->blend >> 29) & 1);   /* BLEND_FORCE_ENABLE */
    if (blend && ds->blend == 0x07010701) {
        /* premultiplied source-over: the composition blend */
        float dst[4], ia = 1.0f - src[3];
        rb_unpack(ds->cfmt, ds->cswap, cp, dst);
        for (int i = 0; i < 4; i++) {
            out[i] = src[i] + dst[i] * ia;
        }
    } else if (blend && ds->blend == 0x07060706) {
        float dst[4], ia = 1.0f - src[3];
        rb_unpack(ds->cfmt, ds->cswap, cp, dst);
        for (int i = 0; i < 4; i++) {
            out[i] = src[i] * src[3] + dst[i] * ia;
        }
    } else if (blend && ds->blend != 0x00010001) {
        float dst[4], k[4];
        rb_unpack(ds->cfmt, ds->cswap, cp, dst);
        for (int i = 0; i < 4; i++) {
            k[i] = f_of(s->regs[A2XX_RB_BLEND_RED + i]);
        }
        uint32_t b = ds->blend;
        for (int i = 0; i < 3; i++) {
            float sf = blend_factor(b & 0x1f, src, dst, k, i);
            float df = blend_factor((b >> 8) & 0x1f, src, dst, k, i);
            out[i] = blend_op((b >> 5) & 7, src[i] * sf, dst[i] * df);
        }
        float sa = blend_factor((b >> 16) & 0x1f, src, dst, k, 3);
        float da = blend_factor((b >> 24) & 0x1f, src, dst, k, 3);
        out[3] = blend_op((b >> 21) & 7, src[3] * sa, dst[3] * da);
    } else {
        memcpy(out, src, sizeof(out));
    }
    rb_pack(ds->cfmt, ds->cswap, out, cp, ds->cmask);
    for (uint32_t i = 1; i < ns; i++) {
        memcpy(cp + i * bpp, cp, bpp);
    }
    ds->pixels++;
}

/* EDRAM_COPY: GMEM pixel (x, y) -> memory */
/* convert GMEM pixel (x, y) into dst (which holds the old value) */
static inline void resolve_px(DrawState *ds, int x, int y, uint8_t *dst)
{
    AdrenoState *s = ds->s;
    int sbpp = fmt_bpp_rb(ds->cfmt);
    uint8_t *sp = gmem_at(s, ds->cbase + (y * ds->pitch + x) * sbpp);
    uint32_t dfmt = (ds->cp_info >> 4) & 0xf, dswap = (ds->cp_info >> 8) & 3;
    uint32_t wmask = (ds->cp_info >> 14) & 0xf;
    int dbpp = fmt_bpp_rb(dfmt);
    float c[4];

    if (!sp || sp + sbpp > s->gmem + ADRENO_GMEM_SIZE) {
        return;
    }
    if (dfmt == ds->cfmt && dswap == ds->cswap && wmask == 0xf) {
        memcpy(dst, sp, dbpp);
    } else {
        rb_unpack(ds->cfmt, ds->cswap, sp, c);
        rb_pack(dfmt, dswap, c, dst, wmask);
    }
    ds->pixels++;

    /* copy-and-clear */
    if (ds->cp_ctl & 0xf0) {
        uint32_t cc = s->regs[A2XX_CLEAR_COLOR];
        if (sbpp == 4) {
            stl_le_p(sp, cc);
        } else {
            memset(sp, 0, sbpp);
        }
    }
}

/* EDRAM_COPY of the GMEM pixels x0..x1-1 of row y to memory */
static void resolve_span(DrawState *ds, int x0, int x1, int y)
{
    AdrenoState *s = ds->s;
    uint32_t dfmt = (ds->cp_info >> 4) & 0xf;
    uint32_t wmask = (ds->cp_info >> 14) & 0xf;
    bool linear = (ds->cp_info >> 3) & 1;
    int dbpp = fmt_bpp_rb(dfmt);
    uint32_t dy = y + ds->cp_dy;
    uint8_t buf[256 * 16];

    uint32_t dswap = (ds->cp_info >> 8) & 3;
    if (linear && dfmt == ds->cfmt && dswap == ds->cswap && wmask == 0xf &&
        !(ds->cp_ctl & 0xf0)) {
        /* same format: copy the GMEM row as is */
        uint32_t so = (ds->cbase + (y * ds->pitch + x0) * dbpp) &
                      (ADRENO_GMEM_SIZE - 1);
        uint32_t len = (x1 - x0) * dbpp;
        if (so + len <= ADRENO_GMEM_SIZE) {
            adreno_write(s, ds->cp_dest +
                         (dy * ds->cp_pitch + x0 + ds->cp_dx) * dbpp,
                         &s->gmem[so], len);
            ds->pixels += x1 - x0;
            return;
        }
        /* the row wraps around the end of GMEM: per pixel below */
    }
    while (x0 < x1) {
        int n = MIN(x1 - x0, 256);
        uint32_t dx = x0 + ds->cp_dx;
        if (linear) {
            uint32_t addr = ds->cp_dest + (dy * ds->cp_pitch + dx) * dbpp;
            if (wmask != 0xf) {
                adreno_read(s, addr, buf, n * dbpp);
            }
            for (int i = 0; i < n; i++) {
                resolve_px(ds, x0 + i, y, buf + i * dbpp);
            }
            adreno_write(s, addr, buf, n * dbpp);
        } else {
            for (int i = 0; i < n; i++) {
                uint32_t addr = ds->cp_dest +
                    tiled_offset(dx + i, dy, ds->cp_pitch, dbpp);
                if (wmask != 0xf) {
                    adreno_read(s, addr, buf, dbpp);
                }
                resolve_px(ds, x0 + i, y, buf);
                adreno_write(s, addr, buf, dbpp);
            }
        }
        x0 += n;
    }
}

/*
 * Span loop for the common composition case: fast-path shader, no depth
 * or alpha test, RGBA8888 target, all channels written, opaque or
 * premultiplied/straight source-over blending.  u/v advance linearly.
 */
static bool fast_span_ok(const DrawState *ds, bool persp)
{
    bool blend = !((ds->cctl >> 5) & 1) || ((ds->blend >> 29) & 1);

    return ds->fast.ok && !ds->fast.uses_params && !persp && ds->mode == 4 &&
           !ds->sysmem &&
           !(ds->zctl & 6) &&
           !((ds->cctl >> 3) & 1) && ds->samples == 1 && ds->cfmt == 5 &&
           ds->cswap == 0 && ds->cmask == 0xf &&
           (!blend || ds->blend == 0x00010001 || ds->blend == 0x07010701 ||
            ds->blend == 0x07060706);
}

static void fast_span(DrawState *ds, int xa, int xb, int y, float u, float v,
                      float du, float dv)
{
    AdrenoState *s = ds->s;
    const FastPS *f = &ds->fast;
    bool blend = !((ds->cctl >> 5) & 1) || ((ds->blend >> 29) & 1);
    int bmode = !blend || ds->blend == 0x00010001 ? 0 :
                ds->blend == 0x07010701 ? 1 : 2;
    uint32_t off = ds->cbase + (y * ds->pitch + xa) * 4;
    const TexInfo *ti = NULL;
    float tx[4] = { 0, 0, 0, 0 };
    int filter = 0;

    off &= ADRENO_GMEM_SIZE - 1;
    if (off + (uint32_t)(xb - xa + 1) * 4 > ADRENO_GMEM_SIZE) {
        /* the row wraps around the end of GMEM: split it */
        int first = (ADRENO_GMEM_SIZE - off) / 4;
        float k = (float)first;
        fast_span(ds, xa, xa + first - 1, y, u, v, du, dv);
        fast_span(ds, xa + first, xb, y, u + du * k, v + dv * k, du, dv);
        return;
    }
    uint8_t *cp = &s->gmem[off];
    if (f->has_tex) {
        if (!(tic_valid & (1u << f->tcidx))) {
            tex_info(s, f->tcidx, &tic[f->tcidx]);
            tic_valid |= 1u << f->tcidx;
        }
        ti = &tic[f->tcidx];
        filter = f->mag == 3 ? ti->mag : f->mag;
    }
    /*
     * 1:1 copy of a texture row (the usual composition): walk the texels
     * in memory instead of sampling.  Texel centres must line up.
     */
    int tx0 = 0, ty = 0;
    uint32_t va = 0;
    uint8_t *hp = NULL;
    bool walk = false;
    if (ti && !ti->tiled && !ti->sign && !ti->exp_adj && dv == 0.0f &&
        (ti->fmt == 6 || ti->fmt == 4)) {
        float su = f->denorm ? u : u * ti->w, sv = f->denorm ? v : v * ti->h;
        float sdu = f->denorm ? du : du * ti->w;
        float fu = su - 0.5f, fv = sv - 0.5f;
        tx0 = (int)floorf(fu + 0.5f);
        ty = (int)floorf(fv + 0.5f);
        if (fabsf(sdu - 1.0f) < 1e-4f && fabsf(fu - tx0) < 1.0f / 512 &&
            fabsf(fv - ty) < 1.0f / 512 && tx0 >= 0 && ty >= 0 &&
            ty < (int)ti->h && tx0 + (xb - xa) < (int)ti->w) {
            walk = true;
            va = ti->base + (ty * ti->pitch + tx0) * ti->bpp;
        }
    }
    for (int x = xa; x <= xb; x++, cp += 4, u += du, v += dv) {
        float c[4];
        if (walk) {
            float raw[4];
            if (!hp || !(va & 4095)) {
                hp = adreno_host_page(s, va);
                if (!hp) {
                    walk = false;
                    goto sample;
                }
            }
            const uint8_t *q = hp + (va & 4095);
            if (ti->fmt == 6) {
                raw[0] = unorm8[q[0]];
                raw[1] = unorm8[q[1]];
                raw[2] = unorm8[q[2]];
                raw[3] = unorm8[q[3]];
            } else {
                uint32_t w16 = q[0] | (q[1] << 8);
                raw[0] = (w16 & 31) * (1.0f / 31);
                raw[1] = ((w16 >> 5) & 63) * (1.0f / 63);
                raw[2] = ((w16 >> 11) & 31) * (1.0f / 31);
                raw[3] = 1.0f;
            }
            va += ti->bpp;
            for (int i = 0; i < 4; i++) {
                uint32_t sw = ti->swz[i];
                tx[i] = sw < 4 ? raw[sw] : sw == 5 ? 1.0f : 0.0f;
            }
        } else if (ti) {
sample:
            tex_sample(s, ti, u, v, f->denorm, filter, tx);
        }
        for (int i = 0; i < 4; i++) {
            c[i] = sym_eval(&f->out[i], tx, NULL);
        }
        if (bmode) {
            float ia = 1.0f - c[3];
            if (bmode == 2) {
                c[0] *= c[3];
                c[1] *= c[3];
                c[2] *= c[3];
                c[3] *= c[3];
            }
            for (int i = 0; i < 4; i++) {
                c[i] += unorm8[cp[i]] * ia;
            }
        }
        for (int i = 0; i < 4; i++) {
            float q = c[i] * 255.0f + 0.5f;
            cp[i] = q >= 255.0f ? 255 : q > 0.0f ? (uint8_t)q : 0;
        }
        ds->pixels++;
    }
}

/* ---------------- rasterizer ---------------- */

static inline float edge(float ax, float ay, float bx, float by, float px,
                         float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void raster_tri(DrawState *ds, const PVtx *v0, const PVtx *v1,
                       const PVtx *v2)
{
    float area = edge(v0->x, v0->y, v1->x, v1->y, v2->x, v2->y);
    if (adreno_trace && ds->s->draws >= (uint64_t)adreno_trace_from &&
        ds->s->draws < (uint64_t)adreno_trace_from + adreno_trace_len) {
        qemu_log("   tri (%g,%g) (%g,%g) (%g,%g) area %g sc %d,%d-%d,%d\n",
                 v0->x, v0->y, v1->x, v1->y, v2->x, v2->y, area,
                 ds->sx0, ds->sy0, ds->sx1, ds->sy1);
    }
    if (area == 0.0f || !isfinite(area)) {
        return;
    }
    /* culling: window y grows downwards */
    uint32_t sc = ds->s->regs[A2XX_PA_SU_SC_MODE_CNTL];
    bool ccw_front = !((sc >> 2) & 1);
    bool is_front = (area < 0) == ccw_front;
    if ((is_front && (sc & 1)) || (!is_front && (sc & 2))) {
        return;
    }
    if (area < 0) {
        const PVtx *t = v1;
        v1 = v2;
        v2 = t;
        area = -area;
    }

    int minx = (int)floorf(MIN(MIN(v0->x, v1->x), v2->x));
    int maxx = (int)ceilf(MAX(MAX(v0->x, v1->x), v2->x));
    int miny = (int)floorf(MIN(MIN(v0->y, v1->y), v2->y));
    int maxy = (int)ceilf(MAX(MAX(v0->y, v1->y), v2->y));
    minx = MAX(minx, ds->sx0);
    miny = MAX(miny, ds->sy0);
    maxx = MIN(maxx, ds->sx1 - 1);
    maxy = MIN(maxy, ds->sy1 - 1);
    if (minx > maxx || miny > maxy) {
        return;
    }

    if (adreno_trace && ds->s->draws >= (uint64_t)adreno_trace_from &&
        ds->s->draws < (uint64_t)adreno_trace_from + adreno_trace_len) {
        qemu_log("   tri box %d,%d-%d,%d fast %d\n", minx, miny, maxx, maxy,
                 fast_span_ok(ds, ds->persp && !(v0->rw == v1->rw && v1->rw == v2->rw)));
    }
    float inv = 1.0f / area;
    /* top-left rule bias */
    bool tl0 = (v1->y == v2->y && v2->x < v1->x) || v2->y < v1->y;
    bool tl1 = (v2->y == v0->y && v0->x < v2->x) || v0->y < v2->y;
    bool tl2 = (v0->y == v1->y && v1->x < v0->x) || v1->y < v0->y;
    V4 in[16];
    uint32_t np = ds->nparams;
    /* d(edge)/dx of the three edge functions */
    float s0 = v1->y - v2->y, s1 = v2->y - v0->y, s2 = v0->y - v1->y;
    bool persp = ds->persp &&
                 !(v0->rw == v1->rw && v1->rw == v2->rw);
    bool fast = fast_span_ok(ds, persp);

    for (int y = miny; y <= maxy; y++) {
        float py = y + 0.5f, px0 = minx + 0.5f;
        float e0 = edge(v1->x, v1->y, v2->x, v2->y, px0, py);
        float e1 = edge(v2->x, v2->y, v0->x, v0->y, px0, py);
        float e2 = edge(v0->x, v0->y, v1->x, v1->y, px0, py);
        /* covered x range of this row from the edge planes (+-1 slack) */
        float lo = 0.0f, hi = (float)(maxx - minx);
        const float es[3] = { e0, e1, e2 }, ss[3] = { s0, s1, s2 };
        for (int k = 0; k < 3; k++) {
            if (ss[k] > 0) {
                lo = MAX(lo, -es[k] / ss[k]);
            } else if (ss[k] < 0) {
                hi = MIN(hi, es[k] / -ss[k]);
            } else if (es[k] < 0) {
                hi = -1.0f;
            }
        }
        if (hi < lo - 1.0f) {
            continue;
        }
        int xa = minx + MAX(0, (int)floorf(lo) - 1);
        int xb = minx + MIN(maxx - minx, (int)ceilf(hi) + 1);
        int run = -1;

        if (fast) {
            /* convex: find the first and last covered pixel, shade between */
#define INSIDE(xx) ({ float fx_ = (float)((xx) - minx); \
            float w0_ = e0 + s0 * fx_, w1_ = e1 + s1 * fx_, w2_ = e2 + s2 * fx_; \
            !(w0_ < 0 || w1_ < 0 || w2_ < 0) && \
            !((w0_ == 0 && !tl0) || (w1_ == 0 && !tl1) || (w2_ == 0 && !tl2)); })
            while (xa <= xb && !INSIDE(xa)) {
                xa++;
            }
            while (xb >= xa && !INSIDE(xb)) {
                xb--;
            }
#undef INSIDE
            if (xa > xb) {
                continue;
            }
            float fx = (float)(xa - minx);
            float c0 = (e0 + s0 * fx) * inv, c1 = (e1 + s1 * fx) * inv;
            float c2 = (e2 + s2 * fx) * inv;
            float u = 0, vv = 0, du = 0, dv = 0;
            if (ds->fast.has_tex) {
                const V4 *p0 = &v0->p[ds->fast.param], *p1 = &v1->p[ds->fast.param];
                const V4 *p2 = &v2->p[ds->fast.param];
                uint32_t uc = ds->fast.uc, vc = ds->fast.vc;
                u = c0 * p0->f[uc] + c1 * p1->f[uc] + c2 * p2->f[uc];
                vv = c0 * p0->f[vc] + c1 * p1->f[vc] + c2 * p2->f[vc];
                du = (s0 * p0->f[uc] + s1 * p1->f[uc] + s2 * p2->f[uc]) * inv;
                dv = (s0 * p0->f[vc] + s1 * p1->f[vc] + s2 * p2->f[vc]) * inv;
            }
            fast_span(ds, xa, xb, y, u, vv, du, dv);
            continue;
        }

        for (int x = xa; x <= xb; x++) {
            float fx = (float)(x - minx);
            float w0 = e0 + s0 * fx, w1 = e1 + s1 * fx, w2 = e2 + s2 * fx;
            bool inside = !(w0 < 0 || w1 < 0 || w2 < 0) &&
                          !((w0 == 0 && !tl0) || (w1 == 0 && !tl1) ||
                            (w2 == 0 && !tl2));
            if (ds->mode == 6) {
                if (inside && run < 0) {
                    run = x;
                } else if (!inside && run >= 0) {
                    resolve_span(ds, run, x, y);
                    run = -1;
                }
                continue;
            }
            if (!inside) {
                continue;
            }
            float b0 = w0 * inv, b1 = w1 * inv, b2 = w2 * inv;
            float z = b0 * v0->z + b1 * v1->z + b2 * v2->z;
            float c0 = b0, c1 = b1, c2 = b2;
            if (persp) {
                float q0 = b0 * v0->rw, q1 = b1 * v1->rw, q2 = b2 * v2->rw;
                float qs = q0 + q1 + q2;
                if (qs != 0.0f) {
                    c0 = q0 / qs;
                    c1 = q1 / qs;
                    c2 = q2 / qs;
                }
            }
            if (ds->fast.ok && !ds->fast.uses_params) {
                if (ds->fast.has_tex) {
                    uint32_t i = ds->fast.param;
                    for (int k = 0; k < 4; k++) {
                        in[i].f[k] = c0 * v0->p[i].f[k] + c1 * v1->p[i].f[k] +
                                     c2 * v2->p[i].f[k];
                    }
                }
            } else {
                for (uint32_t i = 0; i < np; i++) {
                    for (int k = 0; k < 4; k++) {
                        in[i].f[k] = c0 * v0->p[i].f[k] + c1 * v1->p[i].f[k] +
                                     c2 * v2->p[i].f[k];
                    }
                }
            }
            shade_pixel(ds, x, y, z, in);
        }
        if (run >= 0) {
            resolve_span(ds, run, xb + 1, y);
        }
    }
}

/* rectangle from 3 corners: find the right-angle corner, add the fourth */
static void raster_rect(DrawState *ds, const PVtx *v)
{
    for (int c = 0; c < 3; c++) {
        const PVtx *o = &v[c], *a = &v[(c + 1) % 3], *b = &v[(c + 2) % 3];
        bool right = (fabsf(a->x - o->x) < 1e-3f && fabsf(b->y - o->y) < 1e-3f) ||
                     (fabsf(a->y - o->y) < 1e-3f && fabsf(b->x - o->x) < 1e-3f);
        if (right) {
            PVtx d = *a;
            d.x = a->x + b->x - o->x;
            d.y = a->y + b->y - o->y;
            d.z = a->z + b->z - o->z;
            d.rw = a->rw + b->rw - o->rw;
            for (uint32_t i = 0; i < ds->nparams; i++) {
                for (int k = 0; k < 4; k++) {
                    d.p[i].f[k] = a->p[i].f[k] + b->p[i].f[k] - o->p[i].f[k];
                }
            }
            /* draw both halves regardless of winding */
            uint32_t sc = ds->s->regs[A2XX_PA_SU_SC_MODE_CNTL];
            ds->s->regs[A2XX_PA_SU_SC_MODE_CNTL] = sc & ~3u;
            raster_tri(ds, o, a, b);
            raster_tri(ds, a, &d, b);
            ds->s->regs[A2XX_PA_SU_SC_MODE_CNTL] = sc;
            return;
        }
    }
    raster_tri(ds, &v[0], &v[1], &v[2]);
}

/* ---------------- vertices ---------------- */

static void run_vs(AdrenoState *s, DrawState *ds, uint32_t index, PVtx *out)
{
    Thread t;
    uint32_t vte = s->regs[A2XX_PA_CL_VTE_CNTL];

    memset(&t, 0, sizeof(t));
    t.stage = ST_VS;
    t.r[0].f[0] = (float)index;
    run_shader(s, &t, adreno_prog_base(s, 0));

    V4 pos = t.exp[62];
    float x = pos.f[0], y = pos.f[1], z = pos.f[2], w = pos.f[3];
    if (!(t.exp_mask & (1ull << 62))) {
        w = 1.0f;
    }
    out->clipw = w;
    float rw = 1.0f;
    if (!((vte >> 8) & 1) || !((vte >> 9) & 1)) {
        rw = (w != 0.0f) ? 1.0f / w : 0.0f;
    }
    if (!((vte >> 8) & 1)) {
        x *= rw;
        y *= rw;
    }
    if (!((vte >> 9) & 1)) {
        z *= rw;
    }
    if (vte & 1) x *= f_of(s->regs[A2XX_PA_CL_VPORT_XSCALE]);
    if (vte & 2) x += f_of(s->regs[A2XX_PA_CL_VPORT_XOFFSET]);
    if (vte & 4) y *= f_of(s->regs[A2XX_PA_CL_VPORT_YSCALE]);
    if (vte & 8) y += f_of(s->regs[A2XX_PA_CL_VPORT_YOFFSET]);
    if (vte & 16) z *= f_of(s->regs[A2XX_PA_CL_VPORT_ZSCALE]);
    if (vte & 32) z += f_of(s->regs[A2XX_PA_CL_VPORT_ZOFFSET]);
    if ((s->regs[A2XX_PA_SU_SC_MODE_CNTL] >> 16) & 1) {
        uint32_t wo = s->regs[A2XX_PA_SC_WINDOW_OFFSET];
        x += ((int32_t)(wo << 17)) >> 17;
        y += ((int32_t)(wo << 1)) >> 17;
    }
    out->x = x;
    out->y = y;
    out->z = z;
    out->rw = rw;
    for (uint32_t i = 0; i < 16; i++) {
        out->p[i] = t.exp[i];
    }
}

static void setup_draw(AdrenoState *s, DrawState *ds)
{
    uint32_t r;

    memset(ds, 0, sizeof(*ds));
    ds->pt.stage = ST_PS;
    ds->s = s;
    ds->mode = s->regs[A2XX_RB_MODECONTROL] & 7;
    r = s->regs[A2XX_RB_COLOR_INFO];
    ds->cfmt = r & 0xf;
    ds->cswap = (r >> 9) & 3;
    ds->cbase = r & 0xfffff000;
    /*
     * a20x can render to system memory: the colour base is then a GPU
     * address instead of a GMEM offset (KGSL blobs use it for FBOs).
     */
    ds->sysmem = ds->cbase >= ADRENO_GMEM_SIZE;
    ds->clinear = (r >> 6) & 1;
    ds->dirty_lo = UINT32_MAX;
    ds->dirty_hi = 0;
    ds->pitch = s->regs[A2XX_RB_SURFACE_INFO] & 0x3fff;
    /*
     * MSAA_SAMPLES: the blob's fast clear uses 4x with a quarter pitch so
     * that one shaded pixel fills four GMEM slots (samples stored side by
     * side).
     */
    ds->samples = 1u << MIN((s->regs[A2XX_RB_SURFACE_INFO] >> 14) & 3, 2u);
    r = s->regs[A2XX_RB_DEPTH_INFO];
    ds->zfmt = r & 1;
    ds->zbase = r & 0xfffff000;
    ds->zctl = s->regs[A2XX_RB_DEPTHCONTROL];
    ds->cctl = s->regs[A2XX_RB_COLORCONTROL];
    ds->blend = s->regs[A2XX_RB_BLEND_CONTROL];
    ds->cmask = s->regs[A2XX_RB_COLOR_MASK] & 0xf;
    ds->alpha_ref = f_of(s->regs[A2XX_RB_ALPHA_REF]);
    ds->ps_base = adreno_prog_base(s, 1);
    ds->persp = !((s->regs[A2XX_PA_SU_SC_MODE_CNTL] >> 20) & 1);
    r = s->regs[A2XX_SQ_PROGRAM_CNTL];
    ds->param_gen = (r >> 18) & 1;
    ds->param_gen_pos = (s->regs[A2XX_SQ_CONTEXT_MISC] >> 8) & 0xff;
    ds->nparams = MIN(16u, ((r >> 20) & 0xf) + 1);
    static int nofast = -1;
    if (nofast < 0) {
        nofast = getenv("ADRENO_NOFAST") != NULL;
    }
    if (ds->mode == 4 && !nofast) {
        ps_analyze(s, ds);
        stat_fast += ds->fast.ok;
        if (!ds->fast.ok) {
            /* name each slow pixel shader once, for fast-path work */
            static uint32_t seen[32];
            static int nseen;
            uint32_t h = 2166136261u;
            for (uint32_t i = 0; i < 48; i++) {
                h = (h ^ s->istore[ds->ps_base * 3 + i]) * 16777619u;
            }
            bool known = false;
            for (int i = 0; i < nseen; i++) {
                known |= seen[i] == h;
            }
            if (!known && nseen < 32) {
                seen[nseen++] = h;
                qemu_log_mask(LOG_UNIMP, "adreno: slow pixel shader %08x:", h);
                for (uint32_t i = 0; i < 48; i++) {
                    qemu_log_mask(LOG_UNIMP, " %08x",
                                  s->istore[ds->ps_base * 3 + i]);
                }
                qemu_log_mask(LOG_UNIMP, "\n");
            }
        }
    }
    ds->cp_dest = s->regs[A2XX_RB_COPY_DEST_BASE];
    ds->cp_pitch = (s->regs[A2XX_RB_COPY_DEST_PITCH] & 0x1ff) << 5;
    ds->cp_info = s->regs[A2XX_RB_COPY_DEST_INFO];
    ds->cp_dx = s->regs[A2XX_RB_COPY_DEST_OFFSET] & 0x1fff;
    ds->cp_dy = (s->regs[A2XX_RB_COPY_DEST_OFFSET] >> 13) & 0x1fff;
    ds->cp_ctl = s->regs[A2XX_RB_COPY_CONTROL];

    /* scissors: window scissor (+ window offset), screen scissor */
    uint32_t wo = s->regs[A2XX_PA_SC_WINDOW_OFFSET];
    int ox = ((int32_t)(wo << 17)) >> 17, oy = ((int32_t)(wo << 1)) >> 17;
    uint32_t wtl = s->regs[A2XX_PA_SC_WINDOW_SCISSOR_TL];
    uint32_t wbr = s->regs[A2XX_PA_SC_WINDOW_SCISSOR_BR];
    int wx0 = wtl & 0x7fff, wy0 = (wtl >> 16) & 0x7fff;
    int wx1 = wbr & 0x7fff, wy1 = (wbr >> 16) & 0x7fff;
    if (!(wtl >> 31)) {
        wx0 += ox; wx1 += ox; wy0 += oy; wy1 += oy;
    }
    uint32_t stl = s->regs[A2XX_PA_SC_SCREEN_SCISSOR_TL];
    uint32_t sbr = s->regs[A2XX_PA_SC_SCREEN_SCISSOR_BR];
    int sx0 = stl & 0x7fff, sy0 = (stl >> 16) & 0x7fff;
    int sx1 = sbr & 0x7fff, sy1 = (sbr >> 16) & 0x7fff;
    ds->sx0 = MAX(MAX(wx0, sx0), 0);
    ds->sy0 = MAX(MAX(wy0, sy0), 0);
    ds->sx1 = MIN(wx1, sx1);
    ds->sy1 = MIN(wy1, sy1);
}

/* ---------------- entry points ---------------- */

void adreno_draw_reset(AdrenoState *s)
{
    s->draws = 0;
    memset(s->gmem, 0, sizeof(s->gmem));
}

void adreno_reg_written(AdrenoState *s, uint32_t reg, uint32_t val)
{
}

void adreno_event(AdrenoState *s, uint32_t event)
{
}

static void dump_state(AdrenoState *s, uint32_t op, const uint32_t *d,
                       uint32_t n, DrawState *ds)
{
    qemu_log("adreno: DRAW #%" PRIu64 " op %02x:", s->draws - 1, op);
    for (uint32_t i = 0; i < MIN(n, 6u); i++) {
        qemu_log(" %08x", d[i]);
    }
    qemu_log(" mode %u scissor %d,%d-%d,%d cfmt %u pitch %u pixels %" PRIu64
             " vte %x sc %x\n", ds->mode, ds->sx0, ds->sy0, ds->sx1, ds->sy1,
             ds->cfmt, ds->pitch, ds->pixels, s->regs[A2XX_PA_CL_VTE_CNTL],
             s->regs[A2XX_PA_SU_SC_MODE_CNTL]);
    uint32_t vc = s->regs[A2XX_SQ_VS_CONST];
    {
        uint8_t *gp = gmem_at(s, ds->cbase + (200 * ds->pitch + 64) * 4);
        uint32_t pc = s->regs[A2XX_SQ_PS_CONST] & 0x1ff;
        qemu_log("   cbase %x cinfo %08x zctl %08x zbase %x zfmt %u surf %08x samples %u fast %d/%d\n",
                 ds->cbase, s->regs[A2XX_RB_COLOR_INFO],
                 ds->zctl, ds->zbase, ds->zfmt, s->regs[A2XX_RB_SURFACE_INFO],
                 ds->samples, ds->fast.ok, fast_span_ok(ds, ds->persp));
        qemu_log("   blend %08x cctl %08x cmask %x gmem(64,200) %08x psc0 %g %g %g %g\n",
                 ds->blend, ds->cctl, ds->cmask, gp ? ldl_le_p(gp) : 0,
                 f_of(s->regs[ADRENO_REG_ALU + pc * 4]),
                 f_of(s->regs[ADRENO_REG_ALU + pc * 4 + 1]),
                 f_of(s->regs[ADRENO_REG_ALU + pc * 4 + 2]),
                 f_of(s->regs[ADRENO_REG_ALU + pc * 4 + 3]));
    }
    {
        TexInfo ti;
        hwaddr pa = 0;
        uint32_t px[4];
        tex_info(s, 0, &ti);
        uint32_t a = ti.base + ((ti.h / 2) * ti.pitch + ti.w / 4) * ti.bpp;
        adreno_xlate(s, a, &pa);
        adreno_read(s, a, px, 16);
        qemu_log("   tex0 %ux%u fmt %u base %08x mid@%08x(pa %08" PRIx64 "): %08x %08x %08x %08x\n",
                 ti.w, ti.h, ti.fmt, ti.base, a, (uint64_t)pa, px[0], px[1], px[2], px[3]);
    }
    if (ds->mode == 6) {
        qemu_log("   copy dest %08x pitch %u info %08x off %u,%u ctl %x cbase %x"
                 " surf %08x wscissor %08x %08x\n", ds->cp_dest, ds->cp_pitch,
                 ds->cp_info, ds->cp_dx, ds->cp_dy, ds->cp_ctl, ds->cbase,
                 s->regs[A2XX_RB_SURFACE_INFO],
                 s->regs[A2XX_PA_SC_WINDOW_SCISSOR_TL],
                 s->regs[A2XX_PA_SC_WINDOW_SCISSOR_BR]);
        static const int rows[] = { 0, 50, 103, 104, 105, 200, 415 };
        qemu_log("   rows:");
        for (int i = 0; i < 7; i++) {
            hwaddr pa = 0;
            uint32_t va = ds->cp_dest + ((rows[i] + ds->cp_dy) * ds->cp_pitch + ds->cp_dx) * 4;
            bool ok = adreno_xlate(s, va, &pa);
            qemu_log(" %d:%08x->%s%08" PRIx64, rows[i], va, ok ? "" : "FAULT", (uint64_t)pa);
        }
        qemu_log("\n");
    }
    qemu_log("   sq_vs_const %08x sq_ps_const %08x vport %g %g %g %g woff %08x\n"
             "   vsconst:", vc, s->regs[A2XX_SQ_PS_CONST],
             f_of(s->regs[A2XX_PA_CL_VPORT_XSCALE]),
             f_of(s->regs[A2XX_PA_CL_VPORT_XOFFSET]),
             f_of(s->regs[A2XX_PA_CL_VPORT_YSCALE]),
             f_of(s->regs[A2XX_PA_CL_VPORT_YOFFSET]),
             s->regs[A2XX_PA_SC_WINDOW_OFFSET]);
    for (int i = 0; i < 16; i++) {
        qemu_log(" %g", f_of(s->regs[ADRENO_REG_ALU + (vc & 0x1ff) * 4 + i]));
    }
    qemu_log("\n");
}

#define MAX_VERTS 65536

static void draw_one(AdrenoState *s, uint32_t op, const uint32_t *d, uint32_t n)
{
    DrawState ds;
    uint32_t init, count, prim, src;
    uint32_t ibase = 0, isize = 0;
    const uint32_t *imm = NULL;

    tic_valid = 0;

    if (n < 2) {
        return;
    }
    init = d[1];
    prim = init & 0x3f;
    src = (init >> 6) & 3;
    count = init >> 16;
    switch (op) {
    case 0x22:                          /* DRAW_INDX: viz, init, base, size */
        if (n >= 4) {
            ibase = d[2];
            isize = d[3];
        }
        break;
    case 0x34:                          /* DRAW_INDX_BIN: viz, init, bin
                                         * base, bin size, index base/size */
        if (n >= 6) {
            ibase = d[4];
            isize = d[5];
        }
        break;
    case 0x36:                          /* DRAW_INDX_2: inline indices */
        imm = &d[2];
        isize = (n - 2) * 4;
        src = 1;
        break;
    case 0x35:                          /* DRAW_INDX_2_BIN */
        imm = &d[4];
        isize = n > 4 ? (n - 4) * 4 : 0;
        src = 1;
        break;
    }
    s->draws++;
    if (!(s->draws % 64) && !access("/tmp/adreno-trace", F_OK)) {
        adreno_trace = 1;
        unlink("/tmp/adreno-trace");
        adreno_trace_from = s->draws + 1;
    }
    if (adreno_trace && !(s->draws % 5000)) {
        qemu_log("adreno: %" PRIu64 " draws\n", s->draws);
    }
    setup_draw(s, &ds);
    if (ds.mode != 4 && ds.mode != 6) {
        return;
    }
    if (!count || count > MAX_VERTS || ds.sx0 >= ds.sx1 || ds.sy0 >= ds.sy1) {
        return;
    }

    /* indices */
    bool idx32 = (init >> 11) & 1, idx8 = (init >> 13) & 1;
    uint32_t ioff = s->regs[A2XX_VGT_INDX_OFFSET];
    g_autofree uint32_t *idx = g_new(uint32_t, count);
    if (src == 2) {
        for (uint32_t i = 0; i < count; i++) {
            idx[i] = i;
        }
    } else {
        uint32_t esz = idx32 ? 4 : idx8 ? 1 : 2;
        uint32_t bytes = MIN(count * esz, isize ? isize : count * esz);
        g_autofree uint8_t *raw = g_malloc0(count * esz);
        if (imm) {
            memcpy(raw, imm, MIN(bytes, isize));
        } else {
            adreno_read(s, ibase, raw, bytes);
        }
        for (uint32_t i = 0; i < count; i++) {
            idx[i] = esz == 4 ? ldl_le_p(raw + 4 * i) :
                     esz == 2 ? lduw_le_p(raw + 2 * i) : raw[i];
        }
    }

    /* vertex shading, cached per index */
    GHashTable *cache = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, g_free);
    g_autofree PVtx **vs = g_new(PVtx *, count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t key = idx[i] + ioff;
        PVtx *v = g_hash_table_lookup(cache, GUINT_TO_POINTER(key + 1));
        if (!v) {
            v = g_new(PVtx, 1);
            run_vs(s, &ds, key, v);
            g_hash_table_insert(cache, GUINT_TO_POINTER(key + 1), v);
        }
        vs[i] = v;
    }

    /* primitives */
    switch (prim) {
    case 4:                             /* TRILIST */
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            raster_tri(&ds, vs[i], vs[i + 1], vs[i + 2]);
        }
        break;
    case 5:                             /* TRIFAN */
        for (uint32_t i = 1; i + 1 < count; i++) {
            raster_tri(&ds, vs[0], vs[i], vs[i + 1]);
        }
        break;
    case 6:                             /* TRISTRIP */
        for (uint32_t i = 0; i + 2 < count; i++) {
            if (i & 1) {
                raster_tri(&ds, vs[i + 1], vs[i], vs[i + 2]);
            } else {
                raster_tri(&ds, vs[i], vs[i + 1], vs[i + 2]);
            }
        }
        break;
    case 8:                             /* RECTLIST */
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            PVtx r3[3] = { *vs[i], *vs[i + 1], *vs[i + 2] };
            raster_rect(&ds, r3);
        }
        break;
    default: {
        static bool warned[64];
        if (!warned[prim]) {
            warned[prim] = true;
            qemu_log_mask(LOG_UNIMP, "adreno: primitive type %u\n", prim);
        }
        break;
    }
    }
    if (adreno_trace && s->draws >= (uint64_t)adreno_trace_from &&
        s->draws < (uint64_t)adreno_trace_from + adreno_trace_len) {
        for (uint32_t i = 0; i < MIN(count, 4u); i++) {
            qemu_log("   v%u idx %u: %f %f %f rw %f p0 %f %f %f %f\n", i,
                     idx[i], vs[i]->x, vs[i]->y, vs[i]->z, vs[i]->rw,
                     vs[i]->p[0].f[0], vs[i]->p[0].f[1], vs[i]->p[0].f[2],
                     vs[i]->p[0].f[3]);
        }
        uint8_t ib[16];
        adreno_read(s, ibase, ib, 16);
        qemu_log("   ib@%08x:", ibase);
        for (int i = 0; i < 16; i++) {
            qemu_log(" %02x", ib[i]);
        }
        qemu_log("\n   fc:");
        for (int i = 0; i < ADRENO_FETCH_CONSTS; i++) {
            if (s->regs[ADRENO_REG_FETCH + i]) {
                qemu_log(" [%d]=%08x", i, s->regs[ADRENO_REG_FETCH + i]);
            }
        }
        uint32_t vb = adreno_prog_base(s, 0);
        qemu_log("\n   vs@%u:", vb);
        for (uint32_t i = 0; i < 60; i++) {
            qemu_log(" %08x", s->istore[vb * 3 + i]);
        }
        qemu_log("\n");
    }
    g_hash_table_destroy(cache);
    if (ds.sysmem && ds.dirty_hi > ds.dirty_lo) {
        adreno_mark_dirty(s, ds.dirty_lo, ds.dirty_hi - ds.dirty_lo);
    }

    if (adreno_trace && s->draws >= (uint64_t)adreno_trace_from &&
        s->draws < (uint64_t)adreno_trace_from + adreno_trace_len) {
        dump_state(s, op, d, n, &ds);
        if (ds.mode == 6) {
            uint32_t va = ds.cp_dest + ((200 + ds.cp_dy) * ds.cp_pitch + ds.cp_dx + 64) * 4;
            uint8_t *gp = gmem_at(s, ds.cbase + (200 * ds.pitch + 64) * 4);
            qemu_log("   after resolve: mem(64,200) %08x gmem %08x\n",
                     adreno_rd32(s, va), gp ? ldl_le_p(gp) : 0);
        }
        qemu_log("   ps@%u:", ds.ps_base);
        for (uint32_t i = 0; i < 60; i++) {
            qemu_log(" %08x", s->istore[ds.ps_base * 3 + i]);
        }
        uint32_t pc = s->regs[A2XX_SQ_PS_CONST] & 0x1ff;
        qemu_log("\n   psconst:");
        for (int i = 0; i < 16; i++) {
            qemu_log(" %g", f_of(s->regs[ADRENO_REG_ALU + pc * 4 + i]));
        }
        qemu_log("\n");
    }
}

/* ADRENO_STATS=1: log the host time spent drawing every 5 s */
void adreno_draw(AdrenoState *s, uint32_t op, const uint32_t *d, uint32_t n)
{
    static int stats = -1;
    static int64_t t_start, t_draw[8], n_draw[8];

    if (stats < 0) {
        stats = getenv("ADRENO_STATS") != NULL;
    }
    if (!stats) {
        draw_one(s, op, d, n);
        return;
    }
    int64_t t0 = get_clock();
    uint32_t mode = s->regs[A2XX_RB_MODECONTROL] & 7;
    draw_one(s, op, d, n);
    int64_t t1 = get_clock();
    t_draw[mode] += t1 - t0;
    n_draw[mode]++;
    if (!t_start) {
        t_start = t1;
    } else if (t1 - t_start > 5000000000LL) {
        qemu_log("adreno: stats %.1fs: render %" PRId64 " draws (%" PRIu64
                 " fast) %.0f ms,"
                 " resolve %" PRId64 " %.0f ms, other %.0f ms\n",
                 (t1 - t_start) / 1e9, n_draw[4], stat_fast, t_draw[4] / 1e6,
                 n_draw[6],
                 t_draw[6] / 1e6,
                 (t_draw[0] + t_draw[1] + t_draw[2] + t_draw[3] + t_draw[5] +
                  t_draw[7]) / 1e6);
        memset(t_draw, 0, sizeof(t_draw));
        memset(n_draw, 0, sizeof(n_draw));
        stat_fast = 0;
        t_start = t1;
    }
}
