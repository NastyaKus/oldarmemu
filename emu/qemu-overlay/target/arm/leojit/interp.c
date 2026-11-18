/*
 * leojit: the IR interpreter (the reference engine) and the helpers the
 * IR calls.  Guest state is CPUARMState itself; flags are kept in QEMU's
 * format (N = NF bit 31, Z = ZF == 0, C = CF 0/1, V = VF bit 31).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/host-utils.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "internals.h"
#include "cpregs.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/probe.h"
#include "exec/tlb-flags.h"
#include "lir.h"

/* ---------------- helpers ---------------- */

static uint32_t h_cpsr_read(CPUARMState *env, uint32_t a, uint32_t b, uint32_t c)
{
    return helper_cpsr_read(env);
}

static uint32_t h_cpsr_write(CPUARMState *env, uint32_t val, uint32_t mask, uint32_t c)
{
    helper_cpsr_write(env, val, mask);
    return 0;
}

static uint32_t h_spsr_read(CPUARMState *env, uint32_t a, uint32_t b, uint32_t c)
{
    return env->spsr;
}

static uint32_t h_spsr_write(CPUARMState *env, uint32_t val, uint32_t mask, uint32_t c)
{
    env->spsr = (env->spsr & ~mask) | (val & mask);
    return 0;
}

static uint32_t h_cpsr_write_eret(CPUARMState *env, uint32_t val, uint32_t b, uint32_t c)
{
    helper_cpsr_write_eret(env, val);
    return 0;
}

static uint32_t h_exception(CPUARMState *env, uint32_t excp, uint32_t syn, uint32_t c)
{
    helper_exception_with_syndrome(env, excp, syn);
    return 0;
}

static const ARMCPRegInfo *cp_ri(CPUARMState *env, uint32_t key)
{
    return get_arm_cp_reginfo(env_archcpu(env)->cp_regs, key);
}

static uint32_t h_cp_read(CPUARMState *env, uint32_t key, uint32_t b, uint32_t c)
{
    const ARMCPRegInfo *ri = cp_ri(env, key);

    if (ri->readfn) {
        return helper_get_cp_reg(env, ri);
    }
    return *(uint32_t *)((char *)env + ri->fieldoffset);
}

static uint32_t h_cp_write(CPUARMState *env, uint32_t key, uint32_t val, uint32_t c)
{
    const ARMCPRegInfo *ri = cp_ri(env, key);

    if (ri->writefn) {
        helper_set_cp_reg(env, ri, val);
    } else {
        *(uint32_t *)((char *)env + ri->fieldoffset) = val;
    }
    return 0;
}

static uint32_t h_cp_check(CPUARMState *env, uint32_t key, uint32_t syn, uint32_t isread)
{
    helper_access_check_cp_reg(env, key, syn, isread);
    return 0;
}

static uint32_t h_set_nzcv(CPUARMState *env, uint32_t v, uint32_t b, uint32_t c)
{
    env->NF = v;
    env->ZF = (v & (1u << 30)) ^ (1u << 30);
    env->CF = (v >> 29) & 1;
    env->VF = v << 3;
    return 0;
}

static uint32_t h_user_get(CPUARMState *env, uint32_t regno, uint32_t b, uint32_t c)
{
    return helper_get_user_reg(env, regno);
}

static uint32_t h_user_set(CPUARMState *env, uint32_t regno, uint32_t val, uint32_t c)
{
    helper_set_user_reg(env, regno, val);
    return 0;
}

#define mem_ld leo_mem_ld
#define mem_st leo_mem_st

uint32_t mem_ld(CPUARMState *env, uint32_t addr, MemOpIdx oi)
{
    switch (get_memop(oi) & MO_SIZE) {
    case MO_8: return cpu_ldb_mmu(env, addr, oi, 0);
    case MO_16: return cpu_ldw_mmu(env, addr, oi, 0);
    default: return cpu_ldl_mmu(env, addr, oi, 0);
    }
}

void mem_st(CPUARMState *env, uint32_t addr, uint32_t val, MemOpIdx oi)
{
    switch (get_memop(oi) & MO_SIZE) {
    case MO_8: cpu_stb_mmu(env, addr, val, oi, 0); break;
    case MO_16: cpu_stw_mmu(env, addr, val, oi, 0); break;
    default: cpu_stl_mmu(env, addr, val, oi, 0); break;
    }
}

static uint32_t h_ldrex(CPUARMState *env, uint32_t addr, uint32_t oi, uint32_t c)
{
    uint32_t v = mem_ld(env, addr, oi);

    env->exclusive_val = v;
    env->exclusive_addr = addr;
    return v;
}

static uint32_t h_strex(CPUARMState *env, uint32_t addr, uint32_t val, uint32_t oi)
{
    uint32_t r = 1;

    if (env->exclusive_addr == addr) {
        uint32_t old = mem_ld(env, addr, oi);
        if (old == (uint32_t)env->exclusive_val) {
            mem_st(env, addr, val, oi);
            r = 0;
        }
    }
    env->exclusive_addr = -1;
    return r;
}

static uint32_t h_clrex(CPUARMState *env, uint32_t a, uint32_t b, uint32_t c)
{
    env->exclusive_addr = -1;
    return 0;
}

static uint32_t h_rebuild_hflags(CPUARMState *env, uint32_t newel, uint32_t el, uint32_t c)
{
    if (newel) {
        helper_rebuild_hflags_a32_newel(env);
    } else {
        helper_rebuild_hflags_a32(env, el);
    }
    return 0;
}

LHelper *const leo_helpers[LH_NHELPERS] = {
    [LH_CPSR_READ] = h_cpsr_read,
    [LH_CPSR_WRITE] = h_cpsr_write,
    [LH_SPSR_READ] = h_spsr_read,
    [LH_SPSR_WRITE] = h_spsr_write,
    [LH_CPSR_WRITE_ERET] = h_cpsr_write_eret,
    [LH_EXCEPTION] = h_exception,
    [LH_CP_READ] = h_cp_read,
    [LH_CP_WRITE] = h_cp_write,
    [LH_CP_CHECK] = h_cp_check,
    [LH_SET_NZCV] = h_set_nzcv,
    [LH_USER_GET] = h_user_get,
    [LH_USER_SET] = h_user_set,
    [LH_LDREX] = h_ldrex,
    [LH_STREX] = h_strex,
    [LH_CLREX] = h_clrex,
    [LH_REBUILD_HFLAGS] = h_rebuild_hflags,
};

/* ---------------- verification support ---------------- */

bool leo_verify_log;
LeoWrite leo_wlog[LEO_WLOG_MAX];
int leo_nwlog;
bool leo_mmio_seen;

/* note an access that is not plain RAM (device registers have side effects) */
static void v_probe(CPUARMState *env, uint32_t addr, MemOpIdx oi, MMUAccessType t)
{
    void *host = NULL;
    int size = 1 << (get_memop(oi) & MO_SIZE);
    int fl = probe_access_flags(env, addr, size, t, get_mmuidx(oi), true, &host, 0);

    if ((fl & TLB_INVALID_MASK) || !host || (addr & 4095) + size > 4096) {
        leo_mmio_seen = true;
    }
}

static uint32_t v_ld(CPUARMState *env, uint32_t addr, MemOpIdx oi)
{
    v_probe(env, addr, oi, MMU_DATA_LOAD);
    return mem_ld(env, addr, oi);
}

static void v_st(CPUARMState *env, uint32_t addr, uint32_t val, MemOpIdx oi)
{
    v_probe(env, addr, oi, MMU_DATA_STORE);
    if (leo_mmio_seen || leo_nwlog == LEO_WLOG_MAX) {
        leo_mmio_seen = true;
        mem_st(env, addr, val, oi);
        return;
    }
    uint32_t old = mem_ld(env, addr, oi);
    mem_st(env, addr, val, oi);
    switch (get_memop(oi) & MO_SIZE) {
    case MO_8: val = (uint8_t)val; break;
    case MO_16: val = (uint16_t)val; break;
    default: break;
    }
    leo_wlog[leo_nwlog++] = (LeoWrite){ addr, old, val, oi };
}

/* ---------------- interpreter ---------------- */

static bool cond_holds(CPUARMState *env, uint32_t cc)
{
    bool r;

    switch (cc >> 1) {
    case 0: r = env->ZF == 0; break;
    case 1: r = env->CF; break;
    case 2: r = (int32_t)env->NF < 0; break;
    case 3: r = (int32_t)env->VF < 0; break;
    case 4: r = env->CF && env->ZF; break;
    case 5: r = (int32_t)(env->VF ^ env->NF) >= 0; break;
    case 6: r = env->ZF && (int32_t)(env->VF ^ env->NF) >= 0; break;
    default: return true;
    }
    return (cc & 1) ? !r : r;
}

void leo_interp(CPUARMState *env, LBlock *blk)
{
    uint32_t t[LR_NREGS - LR_TEMP0];
    uint32_t *g = env->regs;
    const LOp *ops = blk->ops;

#define R(i)   (*((i) < LR_TEMP0 ? &g[(i)] : &t[(i) - LR_TEMP0]))

    for (uint32_t pc = 0;;) {
        const LOp *o = &ops[pc++];
        uint32_t a, b, r;
        uint64_t m;

        switch (o->op) {
        case L_NOP:
        case L_LABEL:
            break;
        case L_INSN:
            g[15] = o->imm;
            env->condexec_bits = o->b;
            break;
        case L_MOVI: R(o->d) = o->imm; break;
        case L_MOV: R(o->d) = R(o->a); break;
        case L_ADD: R(o->d) = R(o->a) + R(o->b); break;
        case L_SUB: R(o->d) = R(o->a) - R(o->b); break;
        case L_AND: R(o->d) = R(o->a) & R(o->b); break;
        case L_OR: R(o->d) = R(o->a) | R(o->b); break;
        case L_XOR: R(o->d) = R(o->a) ^ R(o->b); break;
        case L_BIC: R(o->d) = R(o->a) & ~R(o->b); break;
        case L_ORN: R(o->d) = R(o->a) | ~R(o->b); break;
        case L_MUL: R(o->d) = R(o->a) * R(o->b); break;
        case L_ADDI: R(o->d) = R(o->a) + o->imm; break;
        case L_NOT: R(o->d) = ~R(o->a); break;
        case L_ADC: R(o->d) = R(o->a) + R(o->b) + env->CF; break;
        case L_SBC: R(o->d) = R(o->a) - R(o->b) + env->CF - 1; break;
        case L_ADDS:
            a = R(o->a), b = R(o->b), r = a + b;
            env->CF = r < a;
            env->VF = (r ^ a) & ~(a ^ b);
            env->NF = env->ZF = r;
            R(o->d) = r;
            break;
        case L_SUBS:
            a = R(o->a), b = R(o->b), r = a - b;
            env->CF = a >= b;
            env->VF = (r ^ a) & (a ^ b);
            env->NF = env->ZF = r;
            R(o->d) = r;
            break;
        case L_ADCS:
        case L_SBCS:
            a = R(o->a), b = R(o->b);
            if (o->op == L_SBCS) {
                b = ~b;
            }
            m = (uint64_t)a + b + env->CF;
            r = m;
            env->CF = m >> 32;
            env->VF = (r ^ a) & ~(a ^ b);
            env->NF = env->ZF = r;
            R(o->d) = r;
            break;
        case L_LOGICCC: env->NF = env->ZF = R(o->a); break;
        case L_LOGICQCC:
            env->NF = R(o->b);
            env->ZF = R(o->a) | R(o->b);
            break;
        case L_SETCI: env->CF = o->imm; break;
        case L_SETCBIT: env->CF = (R(o->a) >> o->imm) & 1; break;
        case L_SHLI: R(o->d) = R(o->a) << o->imm; break;
        case L_SHRI: R(o->d) = R(o->a) >> o->imm; break;
        case L_SARI: R(o->d) = (int32_t)R(o->a) >> o->imm; break;
        case L_RORI: R(o->d) = ror32(R(o->a), o->imm); break;
        case L_RRX: R(o->d) = (R(o->a) >> 1) | (env->CF << 31); break;
        case L_SHL:
            b = R(o->b) & 0xff;
            R(o->d) = b >= 32 ? 0 : R(o->a) << b;
            break;
        case L_SHR:
            b = R(o->b) & 0xff;
            R(o->d) = b >= 32 ? 0 : R(o->a) >> b;
            break;
        case L_SAR:
            b = MIN(R(o->b) & 0xff, 31u);
            R(o->d) = (int32_t)R(o->a) >> b;
            break;
        case L_ROR: R(o->d) = ror32(R(o->a), R(o->b) & 0x1f); break;
        case L_SHLC: R(o->d) = helper_shl_cc(env, R(o->a), R(o->b)); break;
        case L_SHRC: R(o->d) = helper_shr_cc(env, R(o->a), R(o->b)); break;
        case L_SARC: R(o->d) = helper_sar_cc(env, R(o->a), R(o->b)); break;
        case L_RORC: R(o->d) = helper_ror_cc(env, R(o->a), R(o->b)); break;
        case L_CLZ: R(o->d) = clz32(R(o->a)); break;
        case L_REV: R(o->d) = bswap32(R(o->a)); break;
        case L_REV16:
            a = R(o->a);
            R(o->d) = ((a & 0x00ff00ff) << 8) | ((a >> 8) & 0x00ff00ff);
            break;
        case L_REVSH: R(o->d) = (int16_t)bswap16(R(o->a)); break;
        case L_RBIT: R(o->d) = revbit32(R(o->a)); break;
        case L_EXT8U: R(o->d) = (uint8_t)R(o->a); break;
        case L_EXT16U: R(o->d) = (uint16_t)R(o->a); break;
        case L_EXT8S: R(o->d) = (int8_t)R(o->a); break;
        case L_EXT16S: R(o->d) = (int16_t)R(o->a); break;
        case L_UBFX: R(o->d) = extract32(R(o->a), o->imm & 0xff, o->imm >> 8); break;
        case L_SBFX: R(o->d) = sextract32(R(o->a), o->imm & 0xff, o->imm >> 8); break;
        case L_BFI: R(o->d) = deposit32(R(o->d), o->imm & 0xff, o->imm >> 8, R(o->a)); break;
        case L_MULLU:
            m = (uint64_t)R(o->a) * R(o->b);
            R(o->d) = m;
            R(o->c) = m >> 32;
            break;
        case L_MULLS:
            m = (int64_t)(int32_t)R(o->a) * (int32_t)R(o->b);
            R(o->d) = m;
            R(o->c) = m >> 32;
            break;
        case L_MLALU:
        case L_MLALS:
            m = o->op == L_MLALU ? (uint64_t)R(o->a) * R(o->b)
                                 : (uint64_t)((int64_t)(int32_t)R(o->a) * (int32_t)R(o->b));
            m += ((uint64_t)R(o->c) << 32) | R(o->d);
            R(o->d) = m;
            R(o->c) = m >> 32;
            break;
        case L_LD8U:
            r = unlikely(leo_verify_log) ? v_ld(env, R(o->a), o->imm)
                                         : cpu_ldb_mmu(env, R(o->a), o->imm, 0);
            R(o->d) = r;
            break;
        case L_LD8S:
            r = unlikely(leo_verify_log) ? v_ld(env, R(o->a), o->imm)
                                         : cpu_ldb_mmu(env, R(o->a), o->imm, 0);
            R(o->d) = (int8_t)r;
            break;
        case L_LD16U:
            r = unlikely(leo_verify_log) ? v_ld(env, R(o->a), o->imm)
                                         : cpu_ldw_mmu(env, R(o->a), o->imm, 0);
            R(o->d) = r;
            break;
        case L_LD16S:
            r = unlikely(leo_verify_log) ? v_ld(env, R(o->a), o->imm)
                                         : cpu_ldw_mmu(env, R(o->a), o->imm, 0);
            R(o->d) = (int16_t)r;
            break;
        case L_LD32:
            r = unlikely(leo_verify_log) ? v_ld(env, R(o->a), o->imm)
                                         : cpu_ldl_mmu(env, R(o->a), o->imm, 0);
            R(o->d) = r;
            break;
        case L_ST8:
            if (unlikely(leo_verify_log)) {
                v_st(env, R(o->a), R(o->b), o->imm);
            } else {
                cpu_stb_mmu(env, R(o->a), R(o->b), o->imm, 0);
            }
            break;
        case L_ST16:
            if (unlikely(leo_verify_log)) {
                v_st(env, R(o->a), R(o->b), o->imm);
            } else {
                cpu_stw_mmu(env, R(o->a), R(o->b), o->imm, 0);
            }
            break;
        case L_ST32:
            if (unlikely(leo_verify_log)) {
                v_st(env, R(o->a), R(o->b), o->imm);
            } else {
                cpu_stl_mmu(env, R(o->a), R(o->b), o->imm, 0);
            }
            break;
        case L_BRCOND:
            if (!cond_holds(env, o->imm & 0xf)) {
                pc = o->imm >> 8;
            }
            break;
        case L_BRZ:
            if (R(o->a) == 0) {
                pc = o->imm >> 8;
            }
            break;
        case L_BRNZ:
            if (R(o->a) != 0) {
                pc = o->imm >> 8;
            }
            break;
        case L_SETPC: g[15] = R(o->a); break;
        case L_SETPCI: g[15] = o->imm; break;
        case L_BX:
            a = R(o->a);
            env->thumb = a & 1;
            g[15] = a & ~1u;
            break;
        case L_SETTHUMB: env->thumb = o->imm; break;
        case L_SETCOND: env->condexec_bits = o->imm; break;
        case L_EXIT:
            return;
        case L_CALL:
            R(o->d) = blk->helpers[o->imm](env, R(o->a), R(o->b), R(o->c));
            break;
        default:
            g_assert_not_reached();
        }
    }
#undef R
}
