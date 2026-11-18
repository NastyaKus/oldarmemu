/*
 * leojit front end: ARMv7-A (A32, T32, T16) -> IR.
 *
 * Mirrors target/arm/tcg/translate.c instruction by instruction, but
 * emits our IR.  Anything not handled here (VFP, NEON, rare DSP and
 * system insns, and every case where TCG would raise UNDEF) makes the
 * decoder return false: the block then ends before that instruction and
 * TCG executes it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/host-utils.h"
#include "cpu.h"
#include "internals.h"
#include "cpregs.h"
#include "syndrome.h"
#include "exec/cpu-common.h"
#include "exec/target_page.h"
#include "accel/tcg/internal-common.h"
#include "exec/page-protection.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/getpc.h"
#include "exec/translation-block.h"
#include "semihosting/semihost.h"
#include "lir.h"

enum { J_NEXT, J_JUMP, J_EXIT, J_SWI };

typedef struct DisasContext {
    CPUARMState *env;
    ARMCPU *cpu;
    LOp *ops;
    int nops, cap;
    int ntemp, nlabel;
    uint32_t pc_curr, pc_next, page_start;
    bool thumb;
    int condexec_mask, condexec_cond;
    int condlabel;                      /* skip label of a conditional insn */
    int is_jmp;
    uint32_t svc_imm;
    int mmu_idx;                        /* core mmu index */
    ARMMMUIdx arm_mmu_idx;
    int current_el;
    bool user, align_mem, ns, hstr_active;
} DisasContext;

#define ENABLE_ARCH_4T    arm_feature(s->env, ARM_FEATURE_V4T)
#define ENABLE_ARCH_5     arm_feature(s->env, ARM_FEATURE_V5)
#define ENABLE_ARCH_5TE   arm_feature(s->env, ARM_FEATURE_V5)
#define ENABLE_ARCH_6     arm_feature(s->env, ARM_FEATURE_V6)
#define ENABLE_ARCH_6K    arm_feature(s->env, ARM_FEATURE_V6K)
#define ENABLE_ARCH_6T2   arm_feature(s->env, ARM_FEATURE_THUMB2)
#define ENABLE_ARCH_7     arm_feature(s->env, ARM_FEATURE_V7)
#define IS_USER(s)        ((s)->user)

/* ---------------- IR emission ---------------- */

static void emit(DisasContext *s, int op, int d, int a, int b, int c, uint32_t imm)
{
    if (s->nops == s->cap) {
        s->cap *= 2;
        s->ops = g_renew(LOp, s->ops, s->cap);
    }
    s->ops[s->nops++] = (LOp){ .op = op, .d = d, .a = a, .b = b, .c = c, .imm = imm };
}

static int newtemp(DisasContext *s)
{
    g_assert(s->ntemp < LR_NREGS);
    return s->ntemp++;
}

static int newlabel(DisasContext *s)
{
    return s->nlabel++;
}

static void setlabel(DisasContext *s, int l)
{
    emit(s, L_LABEL, 0, 0, 0, 0, l);
}

static int movi(DisasContext *s, uint32_t v)
{
    int t = newtemp(s);
    emit(s, L_MOVI, t, 0, 0, 0, v);
    return t;
}

static int op2(DisasContext *s, int op, int a, int b)
{
    int t = newtemp(s);
    emit(s, op, t, a, b, 0, 0);
    return t;
}

static int op1i(DisasContext *s, int op, int a, uint32_t imm)
{
    int t = newtemp(s);
    emit(s, op, t, a, 0, 0, imm);
    return t;
}

static int call(DisasContext *s, int h, int a, int b, int c)
{
    int t = newtemp(s);
    emit(s, L_CALL, t, a, b, c, h);
    return t;
}

static int32_t jmp_diff(DisasContext *s, int32_t diff)
{
    return diff + (s->thumb ? 4 : 8);
}

/* a copy of guest register r (the PC reads as the architectural value) */
static int load_reg(DisasContext *s, int r)
{
    if (r == 15) {
        return movi(s, s->pc_curr + jmp_diff(s, 0));
    }
    int t = newtemp(s);
    emit(s, L_MOV, t, r, 0, 0, 0);
    return t;
}

/* reg + ofs, the PC being Align(PC, 4) (literal addressing, ADR) */
static int add_reg_for_lit(DisasContext *s, int reg, int ofs)
{
    if (reg == 15) {
        return movi(s, s->pc_curr + jmp_diff(s, ofs - (s->pc_curr & 3)));
    }
    return op1i(s, L_ADDI, reg, ofs);
}

static void store_reg(DisasContext *s, int reg, int t)
{
    if (reg == 15) {
        int m = op1i(s, L_ADDI, t, 0);
        int k = movi(s, s->thumb ? ~1u : ~3u);
        emit(s, L_AND, m, m, k, 0, 0);
        emit(s, L_SETPC, 0, m, 0, 0, 0);
        s->is_jmp = J_JUMP;
        return;
    }
    emit(s, L_MOV, reg, t, 0, 0, 0);
}

static void gen_bx(DisasContext *s, int t)
{
    emit(s, L_BX, 0, t, 0, 0, 0);
    s->is_jmp = J_JUMP;
}

static void store_reg_bx(DisasContext *s, int reg, int t)
{
    if (reg == 15 && ENABLE_ARCH_7) {
        gen_bx(s, t);
    } else {
        store_reg(s, reg, t);
    }
}

static void store_reg_from_load(DisasContext *s, int reg, int t)
{
    if (reg == 15 && ENABLE_ARCH_5) {
        gen_bx(s, t);
    } else {
        store_reg(s, reg, t);
    }
}

/* PC := next insn and leave: the state changed (TCG's gen_lookup_tb) */
static void gen_lookup_tb(DisasContext *s)
{
    emit(s, L_SETPCI, 0, 0, 0, 0, s->pc_next);
    s->is_jmp = J_EXIT;
}

static void gen_jmp(DisasContext *s, int32_t diff)
{
    emit(s, L_SETPCI, 0, 0, 0, 0, s->pc_curr + diff);
    s->is_jmp = J_JUMP;
}

static void arm_skip_unless(DisasContext *s, uint32_t cond)
{
    s->condlabel = newlabel(s);
    emit(s, L_BRCOND, 0, 0, 0, 0, cond | (s->condlabel << 8));
}

static uint32_t condexec_bits(DisasContext *s)
{
    return s->condexec_mask ? (s->condexec_cond << 4) | (s->condexec_mask >> 1) : 0;
}

/* memory access index for a load/store */
static uint32_t memop_idx(DisasContext *s, MemOp mop, int mmu_idx)
{
    if (s->align_mem && !(mop & MO_AMASK)) {
        mop |= MO_ALIGN;
    }
    return make_memop_idx(mop | MO_LE, mmu_idx);
}

static int get_a32_user_mem_index(DisasContext *s)
{
    switch (s->arm_mmu_idx) {
    case ARMMMUIdx_E3:
    case ARMMMUIdx_E30_0:
    case ARMMMUIdx_E30_3_PAN:
        return arm_to_core_mmu_idx(ARMMMUIdx_E30_0);
    default:
        return arm_to_core_mmu_idx(ARMMMUIdx_E10_0);
    }
}

static int gen_ld(DisasContext *s, int addr, MemOp mop, int mmu_idx)
{
    int op;
    switch (mop & (MO_SIZE | MO_SIGN)) {
    case MO_UB: op = L_LD8U; break;
    case MO_SB: op = L_LD8S; break;
    case MO_UW: op = L_LD16U; break;
    case MO_SW: op = L_LD16S; break;
    default: op = L_LD32; break;
    }
    int t = newtemp(s);
    emit(s, op, t, addr, 0, 0, memop_idx(s, mop & ~MO_SIGN, mmu_idx));
    return t;
}

static void gen_st(DisasContext *s, int addr, int val, MemOp mop, int mmu_idx)
{
    int op = (mop & MO_SIZE) == MO_8 ? L_ST8 : (mop & MO_SIZE) == MO_16 ? L_ST16 : L_ST32;
    emit(s, op, 0, addr, val, 0, memop_idx(s, mop, mmu_idx));
}

/* ---------------- decodetree glue ---------------- */

static int negate(DisasContext *s, int x)
{
    return -x;
}

static int plus_2(DisasContext *s, int x)
{
    return x + 2;
}

static int times_2(DisasContext *s, int x)
{
    return x * 2;
}

static int times_4(DisasContext *s, int x)
{
    return x * 4;
}

static int times_2_plus_1(DisasContext *s, int x)
{
    return x * 2 + 1;
}

static int t32_expandimm_rot(DisasContext *s, int x)
{
    return x & 0xc00 ? extract32(x, 7, 5) : 0;
}

static int t32_expandimm_imm(DisasContext *s, int x)
{
    uint32_t imm = extract32(x, 0, 8);

    switch (extract32(x, 8, 4)) {
    case 0:
        break;
    case 1:
        imm *= 0x00010001;
        break;
    case 2:
        imm *= 0x01000100;
        break;
    case 3:
        imm *= 0x01010101;
        break;
    default:
        imm |= 0x80;
        break;
    }
    return imm;
}

static int t32_branch24(DisasContext *s, int x)
{
    x ^= !(x < 0) * (3 << 21);
    return x << 1;
}

static int t16_setflags(DisasContext *s)
{
    return s->condexec_mask == 0;
}

static int t16_push_list(DisasContext *s, int x)
{
    return (x & 0xff) | (x & 0x100) << (14 - 8);
}

static int t16_pop_list(DisasContext *s, int x)
{
    return (x & 0xff) | (x & 0x100) << (15 - 8);
}

#include "leo-decode-a32.c.inc"
#include "leo-decode-a32-uncond.c.inc"
#include "leo-decode-t32.c.inc"
#include "leo-decode-t16.c.inc"

/* ---------------- shifts and flags ---------------- */

static void shifter_out_im(DisasContext *s, int var, int bit)
{
    emit(s, L_SETCBIT, 0, var, 0, 0, bit);
}

/* var := var shifted by an immediate (ARM encoding), optional carry out */
static int gen_arm_shift_im(DisasContext *s, int var, int shiftop, int shift, int flags)
{
    switch (shiftop) {
    case 0:
        if (shift != 0) {
            if (flags) {
                shifter_out_im(s, var, 32 - shift);
            }
            return op1i(s, L_SHLI, var, shift);
        }
        return var;
    case 1:
        if (shift == 0) {
            if (flags) {
                shifter_out_im(s, var, 31);
            }
            return movi(s, 0);
        }
        if (flags) {
            shifter_out_im(s, var, shift - 1);
        }
        return op1i(s, L_SHRI, var, shift);
    case 2:
        if (shift == 0) {
            shift = 32;
        }
        if (flags) {
            shifter_out_im(s, var, shift - 1);
        }
        return op1i(s, L_SARI, var, shift == 32 ? 31 : shift);
    default:
        if (shift != 0) {
            if (flags) {
                shifter_out_im(s, var, shift - 1);
            }
            return op1i(s, L_RORI, var, shift);
        } else {
            int t = op1i(s, L_RRX, var, 0);       /* reads the old C */
            if (flags) {
                shifter_out_im(s, var, 0);
            }
            return t;
        }
    }
}

static int gen_arm_shift_reg(DisasContext *s, int var, int shiftop, int shift, int flags)
{
    static const int plain[] = { L_SHL, L_SHR, L_SAR, L_ROR };
    static const int cc[] = { L_SHLC, L_SHRC, L_SARC, L_RORC };
    return op2(s, (flags ? cc : plain)[shiftop], var, shift);
}

static void gen_logic_CC(DisasContext *s, int t)
{
    emit(s, L_LOGICCC, 0, t, 0, 0, 0);
}

/* ---------------- data processing ---------------- */

typedef enum { STREG_NONE, STREG_NORMAL, STREG_SP_CHECK, STREG_EXC_RET } StoreRegKind;

enum {
    DP_AND, DP_EOR, DP_ORR, DP_BIC, DP_ORN, DP_RSB, DP_ADC, DP_SBC, DP_RSC,
    DP_ADD, DP_SUB, DP_MOV, DP_MVN,
};

/* result of OP(a, b), setting NZCV for the arithmetic ops when s_ */
static int dp_op(DisasContext *s, int op, bool s_, int a, int b)
{
    switch (op) {
    case DP_AND: return op2(s, L_AND, a, b);
    case DP_EOR: return op2(s, L_XOR, a, b);
    case DP_ORR: return op2(s, L_OR, a, b);
    case DP_BIC: return op2(s, L_BIC, a, b);
    case DP_ORN: return op2(s, L_ORN, a, b);
    case DP_RSB: return op2(s, s_ ? L_SUBS : L_SUB, b, a);
    case DP_ADC: return op2(s, s_ ? L_ADCS : L_ADC, a, b);
    case DP_SBC: return op2(s, s_ ? L_SBCS : L_SBC, a, b);
    case DP_RSC: return op2(s, s_ ? L_SBCS : L_SBC, b, a);
    case DP_ADD: return op2(s, s_ ? L_ADDS : L_ADD, a, b);
    case DP_SUB: return op2(s, s_ ? L_SUBS : L_SUB, a, b);
    case DP_MOV: return b;
    case DP_MVN: return op1i(s, L_NOT, b, 0);
    }
    g_assert_not_reached();
}

static bool is_logic(int op)
{
    return op <= DP_ORN || op == DP_MOV || op == DP_MVN;
}

static void gen_exception_return(DisasContext *s, int pc)
{
    emit(s, L_SETPC, 0, pc, 0, 0, 0);
    int spsr = call(s, LH_SPSR_READ, 0, 0, 0);
    call(s, LH_CPSR_WRITE_ERET, spsr, 0, 0);
    s->is_jmp = J_EXIT;
}

static bool store_reg_kind(DisasContext *s, int rd, int val, StoreRegKind kind)
{
    switch (kind) {
    case STREG_NONE:
        return true;
    case STREG_NORMAL:
        if (s->thumb) {
            store_reg(s, rd, val);
        } else {
            store_reg_bx(s, rd, val);
        }
        return true;
    case STREG_SP_CHECK:
        store_reg(s, 13, val);
        return true;
    case STREG_EXC_RET:
        gen_exception_return(s, val);
        return true;
    }
    g_assert_not_reached();
}

/* the store kind of op with destination rd, false = leave it to TCG */
static bool dp_kind(DisasContext *s, int op, int rd, int rn, int *s_, StoreRegKind *k)
{
    *k = STREG_NORMAL;
    if ((op == DP_SUB || op == DP_MOV) && rd == 15 && *s_) {
        if (IS_USER(s) || s->current_el == 2) {
            return false;               /* UNDEF */
        }
        *s_ = 0;
        *k = STREG_EXC_RET;
    }
    return true;
}

static bool op_dp_shi(DisasContext *s, int op, arg_s_rrr_shi *a, bool cmp)
{
    int sflag = a->s;
    StoreRegKind k = STREG_NONE;
    bool logic;

    if (!cmp && !dp_kind(s, op, a->rd, a->rn, &sflag, &k)) {
        return false;
    }
    logic = is_logic(op) && sflag;
    int t2 = gen_arm_shift_im(s, load_reg(s, a->rm), a->shty, a->shim, logic);
    int t1 = (op == DP_MOV || op == DP_MVN) ? 0 : load_reg(s, a->rn);
    int r = dp_op(s, op, sflag && !logic, t1, t2);
    if (logic) {
        gen_logic_CC(s, r);
    }
    return store_reg_kind(s, a->rd, r, cmp ? STREG_NONE : k);
}

static bool op_dp_shr(DisasContext *s, int op, arg_s_rrr_shr *a, bool cmp)
{
    int sflag = a->s;
    StoreRegKind k = STREG_NONE;
    bool logic;

    if (!cmp && !dp_kind(s, op, a->rd, a->rn, &sflag, &k)) {
        return false;
    }
    logic = is_logic(op) && sflag;
    int sh = load_reg(s, a->rs);
    int t2 = gen_arm_shift_reg(s, load_reg(s, a->rm), a->shty, sh, logic);
    int t1 = (op == DP_MOV || op == DP_MVN) ? 0 : load_reg(s, a->rn);
    int r = dp_op(s, op, sflag && !logic, t1, t2);
    if (logic) {
        gen_logic_CC(s, r);
    }
    return store_reg_kind(s, a->rd, r, cmp ? STREG_NONE : k);
}

static bool op_dp_rot(DisasContext *s, int op, arg_s_rri_rot *a, bool cmp)
{
    int sflag = a->s;
    StoreRegKind k = STREG_NONE;
    bool logic;
    uint32_t imm = ror32(a->imm, a->rot);

    if (!cmp && !dp_kind(s, op, a->rd, a->rn, &sflag, &k)) {
        return false;
    }
    logic = is_logic(op) && sflag;
    if (logic && a->rot) {
        emit(s, L_SETCI, 0, 0, 0, 0, imm >> 31);
    }
    int t2 = movi(s, imm);
    int t1 = (op == DP_MOV || op == DP_MVN) ? 0 : load_reg(s, a->rn);
    int r = dp_op(s, op, sflag && !logic, t1, t2);
    if (logic) {
        gen_logic_CC(s, r);
    }
    return store_reg_kind(s, a->rd, r, cmp ? STREG_NONE : k);
}

#define DO_DP3(NAME, OP)                                                   \
    bool leo_##NAME##_rrri(DisasContext *s, arg_s_rrr_shi *a)              \
    { return op_dp_shi(s, OP, a, false); }                                 \
    bool leo_##NAME##_rrrr(DisasContext *s, arg_s_rrr_shr *a)              \
    { return op_dp_shr(s, OP, a, false); }                                 \
    bool leo_##NAME##_rri(DisasContext *s, arg_s_rri_rot *a)               \
    { return op_dp_rot(s, OP, a, false); }

#define DO_DP2(NAME, OP)                                                   \
    bool leo_##NAME##_rxri(DisasContext *s, arg_s_rrr_shi *a)              \
    { return op_dp_shi(s, OP, a, false); }                                 \
    bool leo_##NAME##_rxrr(DisasContext *s, arg_s_rrr_shr *a)              \
    { return op_dp_shr(s, OP, a, false); }                                 \
    bool leo_##NAME##_rxi(DisasContext *s, arg_s_rri_rot *a)               \
    { return op_dp_rot(s, OP, a, false); }

/* compares: always set flags, no result */
#define DO_CMP(NAME, OP)                                                   \
    bool leo_##NAME##_xrri(DisasContext *s, arg_s_rrr_shi *a)              \
    { a->s = 1; return op_dp_shi(s, OP, a, true); }                        \
    bool leo_##NAME##_xrrr(DisasContext *s, arg_s_rrr_shr *a)              \
    { a->s = 1; return op_dp_shr(s, OP, a, true); }                        \
    bool leo_##NAME##_xri(DisasContext *s, arg_s_rri_rot *a)               \
    { a->s = 1; return op_dp_rot(s, OP, a, true); }

DO_DP3(AND, DP_AND)
DO_DP3(EOR, DP_EOR)
DO_DP3(ORR, DP_ORR)
DO_DP3(BIC, DP_BIC)
DO_DP3(RSB, DP_RSB)
DO_DP3(ADC, DP_ADC)
DO_DP3(SBC, DP_SBC)
DO_DP3(RSC, DP_RSC)
DO_DP3(ADD, DP_ADD)
DO_DP3(SUB, DP_SUB)
DO_DP2(MOV, DP_MOV)
DO_DP2(MVN, DP_MVN)
DO_CMP(TST, DP_AND)
DO_CMP(TEQ, DP_EOR)
DO_CMP(CMN, DP_ADD)
DO_CMP(CMP, DP_SUB)

bool leo_ORN_rrri(DisasContext *s, arg_s_rrr_shi *a)
{
    return op_dp_shi(s, DP_ORN, a, false);
}

bool leo_ORN_rri(DisasContext *s, arg_s_rri_rot *a)
{
    return op_dp_rot(s, DP_ORN, a, false);
}

bool leo_ADR(DisasContext *s, arg_ri *a)
{
    store_reg_bx(s, a->rd, add_reg_for_lit(s, 15, a->imm));
    return true;
}

bool leo_MOVW(DisasContext *s, arg_MOVW *a)
{
    if (!ENABLE_ARCH_6T2) {
        return false;
    }
    store_reg(s, a->rd, movi(s, a->imm));
    return true;
}

bool leo_MOVT(DisasContext *s, arg_MOVW *a)
{
    if (!ENABLE_ARCH_6T2) {
        return false;
    }
    int t = op1i(s, L_EXT16U, load_reg(s, a->rd), 0);
    int r = op2(s, L_OR, t, movi(s, (uint32_t)a->imm << 16));
    store_reg(s, a->rd, r);
    return true;
}

/* ---------------- multiplies ---------------- */

static bool op_mla(DisasContext *s, arg_s_rrrr *a, bool add)
{
    int t = op2(s, L_MUL, load_reg(s, a->rn), load_reg(s, a->rm));
    if (add) {
        t = op2(s, L_ADD, t, load_reg(s, a->ra));
    }
    if (a->s) {
        gen_logic_CC(s, t);
    }
    store_reg(s, a->rd, t);
    return true;
}

bool leo_MUL(DisasContext *s, arg_MUL *a)
{
    return op_mla(s, a, false);
}

bool leo_MLA(DisasContext *s, arg_MLA *a)
{
    return op_mla(s, a, true);
}

bool leo_MLS(DisasContext *s, arg_MLS *a)
{
    if (!ENABLE_ARCH_6T2) {
        return false;
    }
    int t = op2(s, L_MUL, load_reg(s, a->rn), load_reg(s, a->rm));
    store_reg(s, a->rd, op2(s, L_SUB, load_reg(s, a->ra), t));
    return true;
}

static bool op_mlal(DisasContext *s, arg_s_rrrr *a, bool uns, bool add)
{
    int lo = newtemp(s), hi = newtemp(s);
    int rm = load_reg(s, a->rm), rn = load_reg(s, a->rn);

    if (add) {
        emit(s, L_MOV, lo, a->ra, 0, 0, 0);
        emit(s, L_MOV, hi, a->rd, 0, 0, 0);
        emit(s, uns ? L_MLALU : L_MLALS, lo, rm, rn, hi, 0);
    } else {
        emit(s, uns ? L_MULLU : L_MULLS, lo, rm, rn, hi, 0);
    }
    if (a->s) {
        emit(s, L_LOGICQCC, 0, lo, hi, 0, 0);
    }
    store_reg(s, a->ra, lo);
    store_reg(s, a->rd, hi);
    return true;
}

bool leo_UMULL(DisasContext *s, arg_UMULL *a)
{
    return op_mlal(s, a, true, false);
}

bool leo_SMULL(DisasContext *s, arg_SMULL *a)
{
    return op_mlal(s, a, false, false);
}

bool leo_UMLAL(DisasContext *s, arg_UMLAL *a)
{
    return op_mlal(s, a, true, true);
}

bool leo_SMLAL(DisasContext *s, arg_SMLAL *a)
{
    return op_mlal(s, a, false, true);
}

/* ---------------- misc data ---------------- */

bool leo_CLZ(DisasContext *s, arg_CLZ *a)
{
    if (!ENABLE_ARCH_5) {
        return false;
    }
    store_reg(s, a->rd, op1i(s, L_CLZ, load_reg(s, a->rm), 0));
    return true;
}

static bool op_rr(DisasContext *s, arg_rr *a, int op)
{
    if (!ENABLE_ARCH_6) {
        return false;
    }
    store_reg(s, a->rd, op1i(s, op, load_reg(s, a->rm), 0));
    return true;
}

bool leo_REV(DisasContext *s, arg_REV *a)
{
    return op_rr(s, a, L_REV);
}

bool leo_REV16(DisasContext *s, arg_REV16 *a)
{
    return op_rr(s, a, L_REV16);
}

bool leo_REVSH(DisasContext *s, arg_REVSH *a)
{
    return op_rr(s, a, L_REVSH);
}

bool leo_RBIT(DisasContext *s, arg_RBIT *a)
{
    if (!ENABLE_ARCH_6T2) {
        return false;
    }
    store_reg(s, a->rd, op1i(s, L_RBIT, load_reg(s, a->rm), 0));
    return true;
}

static bool op_bfx(DisasContext *s, arg_UBFX *a, bool u)
{
    int width = a->widthm1 + 1, shift = a->lsb;

    if (!ENABLE_ARCH_6T2 || shift + width > 32) {
        return false;
    }
    store_reg(s, a->rd, op1i(s, u ? L_UBFX : L_SBFX, load_reg(s, a->rn),
                             shift | width << 8));
    return true;
}

bool leo_SBFX(DisasContext *s, arg_SBFX *a)
{
    return op_bfx(s, a, false);
}

bool leo_UBFX(DisasContext *s, arg_UBFX *a)
{
    return op_bfx(s, a, true);
}

bool leo_BFCI(DisasContext *s, arg_BFCI *a)
{
    int msb = a->msb, lsb = a->lsb;

    if (!ENABLE_ARCH_6T2 || msb < lsb) {
        return false;
    }
    int width = msb + 1 - lsb;
    int src = a->rn == 15 ? movi(s, 0) : load_reg(s, a->rn);
    int t = load_reg(s, a->rd);
    emit(s, L_BFI, t, src, 0, 0, lsb | width << 8);
    store_reg(s, a->rd, t);
    return true;
}

static bool op_xta(DisasContext *s, arg_rrr_rot *a, int ext)
{
    if (!ENABLE_ARCH_6) {
        return false;
    }
    int t = load_reg(s, a->rm);
    if (a->rot) {
        t = op1i(s, L_RORI, t, a->rot * 8);
    }
    t = op1i(s, ext, t, 0);
    if (a->rn != 15) {
        t = op2(s, L_ADD, t, load_reg(s, a->rn));
    }
    store_reg(s, a->rd, t);
    return true;
}

bool leo_SXTAB(DisasContext *s, arg_SXTAB *a)
{
    return op_xta(s, a, L_EXT8S);
}

bool leo_SXTAH(DisasContext *s, arg_SXTAH *a)
{
    return op_xta(s, a, L_EXT16S);
}

bool leo_UXTAB(DisasContext *s, arg_UXTAB *a)
{
    return op_xta(s, a, L_EXT8U);
}

bool leo_UXTAH(DisasContext *s, arg_UXTAH *a)
{
    return op_xta(s, a, L_EXT16U);
}

/* ---------------- loads and stores ---------------- */

static int op_addr_rr_pre(DisasContext *s, arg_ldst_rr *a)
{
    int addr = load_reg(s, a->rn);

    if (a->p) {
        int ofs = gen_arm_shift_im(s, load_reg(s, a->rm), a->shtype, a->shimm, 0);
        addr = op2(s, a->u ? L_ADD : L_SUB, addr, ofs);
    }
    return addr;
}

static void op_addr_rr_post(DisasContext *s, arg_ldst_rr *a, int addr)
{
    if (!a->p) {
        int ofs = gen_arm_shift_im(s, load_reg(s, a->rm), a->shtype, a->shimm, 0);
        addr = op2(s, a->u ? L_ADD : L_SUB, addr, ofs);
    } else if (!a->w) {
        return;
    }
    store_reg(s, a->rn, addr);
}

static bool op_load_rr(DisasContext *s, arg_ldst_rr *a, MemOp mop, int mem_idx)
{
    int addr = op_addr_rr_pre(s, a);
    int t = gen_ld(s, addr, mop, mem_idx);
    op_addr_rr_post(s, a, addr);
    store_reg_from_load(s, a->rt, t);
    return true;
}

static bool op_store_rr(DisasContext *s, arg_ldst_rr *a, MemOp mop, int mem_idx)
{
    if (s->thumb && a->rn == 15) {
        return false;
    }
    int addr = op_addr_rr_pre(s, a);
    gen_st(s, addr, load_reg(s, a->rt), mop, mem_idx);
    op_addr_rr_post(s, a, addr);
    return true;
}

static int op_addr_ri_pre(DisasContext *s, arg_ldst_ri *a)
{
    int ofs = a->u ? a->imm : -a->imm;
    return add_reg_for_lit(s, a->rn, a->p ? ofs : 0);
}

static void op_addr_ri_post(DisasContext *s, arg_ldst_ri *a, int addr)
{
    int ofs = 0;
    if (!a->p) {
        ofs = a->u ? a->imm : -a->imm;
    } else if (!a->w) {
        return;
    }
    store_reg(s, a->rn, op1i(s, L_ADDI, addr, ofs));
}

static bool op_load_ri(DisasContext *s, arg_ldst_ri *a, MemOp mop, int mem_idx)
{
    int addr = op_addr_ri_pre(s, a);
    int t = gen_ld(s, addr, mop, mem_idx);
    op_addr_ri_post(s, a, addr);
    store_reg_from_load(s, a->rt, t);
    return true;
}

static bool op_store_ri(DisasContext *s, arg_ldst_ri *a, MemOp mop, int mem_idx)
{
    if (s->thumb && a->rn == 15) {
        return false;
    }
    int addr = op_addr_ri_pre(s, a);
    gen_st(s, addr, load_reg(s, a->rt), mop, mem_idx);
    op_addr_ri_post(s, a, addr);
    return true;
}

#define DO_LDST(NAME, WHICH, MEMOP)                                         \
bool leo_##NAME##_ri(DisasContext *s, arg_ldst_ri *a)                       \
{ return op_##WHICH##_ri(s, a, MEMOP, s->mmu_idx); }                        \
bool leo_##NAME##T_ri(DisasContext *s, arg_ldst_ri *a)                      \
{ return op_##WHICH##_ri(s, a, MEMOP, get_a32_user_mem_index(s)); }         \
bool leo_##NAME##_rr(DisasContext *s, arg_ldst_rr *a)                       \
{ return op_##WHICH##_rr(s, a, MEMOP, s->mmu_idx); }                        \
bool leo_##NAME##T_rr(DisasContext *s, arg_ldst_rr *a)                      \
{ return op_##WHICH##_rr(s, a, MEMOP, get_a32_user_mem_index(s)); }

DO_LDST(LDR, load, MO_UL)
DO_LDST(LDRB, load, MO_UB)
DO_LDST(LDRH, load, MO_UW)
DO_LDST(LDRSB, load, MO_SB)
DO_LDST(LDRSH, load, MO_SW)
DO_LDST(STR, store, MO_UL)
DO_LDST(STRB, store, MO_UB)
DO_LDST(STRH, store, MO_UW)

/* LDRD/STRD: two words, word aligned, registers written after both loads */
static void do_ldrd_load(DisasContext *s, int addr, int rt, int rt2)
{
    int a2 = op1i(s, L_ADDI, addr, 4);
    int lo = gen_ld(s, addr, MO_UL | MO_ALIGN, s->mmu_idx);
    int hi = gen_ld(s, a2, MO_UL | MO_ALIGN, s->mmu_idx);
    store_reg(s, rt, lo);
    store_reg(s, rt2, hi);
}

static void do_strd_store(DisasContext *s, int addr, int rt, int rt2)
{
    int a2 = op1i(s, L_ADDI, addr, 4);
    int lo = load_reg(s, rt), hi = load_reg(s, rt2);
    gen_st(s, addr, lo, MO_UL | MO_ALIGN, s->mmu_idx);
    gen_st(s, a2, hi, MO_UL | MO_ALIGN, s->mmu_idx);
}

bool leo_LDRD_rr(DisasContext *s, arg_ldst_rr *a)
{
    if (!ENABLE_ARCH_5TE || (a->rt & 1) || a->rt == 14) {
        return false;
    }
    int addr = op_addr_rr_pre(s, a);
    do_ldrd_load(s, addr, a->rt, a->rt + 1);
    op_addr_rr_post(s, a, addr);
    return true;
}

bool leo_STRD_rr(DisasContext *s, arg_ldst_rr *a)
{
    if (!ENABLE_ARCH_5TE || (a->rt & 1) || a->rt == 14) {
        return false;
    }
    int addr = op_addr_rr_pre(s, a);
    do_strd_store(s, addr, a->rt, a->rt + 1);
    op_addr_rr_post(s, a, addr);
    return true;
}

static bool op_ldrd_ri(DisasContext *s, arg_ldst_ri *a, int rt2)
{
    if (rt2 == 15 || a->rt == 15) {
        return false;
    }
    int addr = op_addr_ri_pre(s, a);
    do_ldrd_load(s, addr, a->rt, rt2);
    op_addr_ri_post(s, a, addr);
    return true;
}

bool leo_LDRD_ri_a32(DisasContext *s, arg_ldst_ri *a)
{
    if (!ENABLE_ARCH_5TE || (a->rt & 1)) {
        return false;
    }
    return op_ldrd_ri(s, a, a->rt + 1);
}

bool leo_LDRD_ri_t32(DisasContext *s, arg_ldst_ri2 *a)
{
    arg_ldst_ri b = { .u = a->u, .w = a->w, .p = a->p,
                      .rn = a->rn, .rt = a->rt, .imm = a->imm };
    return op_ldrd_ri(s, &b, a->rt2);
}

static bool op_strd_ri(DisasContext *s, arg_ldst_ri *a, int rt2)
{
    if (s->thumb && a->rn == 15) {
        return false;
    }
    int addr = op_addr_ri_pre(s, a);
    do_strd_store(s, addr, a->rt, rt2);
    op_addr_ri_post(s, a, addr);
    return true;
}

bool leo_STRD_ri_a32(DisasContext *s, arg_ldst_ri *a)
{
    if (!ENABLE_ARCH_5TE || (a->rt & 1) || a->rt == 14) {
        return false;
    }
    return op_strd_ri(s, a, a->rt + 1);
}

bool leo_STRD_ri_t32(DisasContext *s, arg_ldst_ri2 *a)
{
    arg_ldst_ri b = { .u = a->u, .w = a->w, .p = a->p,
                      .rn = a->rn, .rt = a->rt, .imm = a->imm };
    return op_strd_ri(s, &b, a->rt2);
}

/* exclusives (word, byte, halfword): the monitor lives in the helpers */
static bool op_ldrex(DisasContext *s, arg_LDREX *a, MemOp mop)
{
    if (a->rt == 15 || a->rn == 15) {
        return false;
    }
    int addr = op1i(s, L_ADDI, a->rn, a->imm);
    int oi = movi(s, memop_idx(s, mop | MO_ALIGN, s->mmu_idx));
    int t = call(s, LH_LDREX, addr, oi, 0);
    store_reg(s, a->rt, t);
    return true;
}

static bool op_strex(DisasContext *s, arg_STREX *a, MemOp mop)
{
    if (a->rd == 15 || a->rt == 15 || a->rn == 15 || a->rd == a->rn ||
        a->rd == a->rt) {
        return false;
    }
    int addr = op1i(s, L_ADDI, a->rn, a->imm);
    int oi = movi(s, memop_idx(s, mop | MO_ALIGN, s->mmu_idx));
    int r = call(s, LH_STREX, addr, load_reg(s, a->rt), oi);
    store_reg(s, a->rd, r);
    return true;
}

bool leo_LDREX(DisasContext *s, arg_LDREX *a)
{
    return ENABLE_ARCH_6 && op_ldrex(s, a, MO_UL);
}

bool leo_LDREXB(DisasContext *s, arg_LDREXB *a)
{
    return ENABLE_ARCH_6K && op_ldrex(s, a, MO_UB);
}

bool leo_LDREXH(DisasContext *s, arg_LDREXH *a)
{
    return ENABLE_ARCH_6K && op_ldrex(s, a, MO_UW);
}

bool leo_STREX(DisasContext *s, arg_STREX *a)
{
    return ENABLE_ARCH_6 && op_strex(s, a, MO_UL);
}

bool leo_STREXB(DisasContext *s, arg_STREXB *a)
{
    return ENABLE_ARCH_6K && op_strex(s, a, MO_UB);
}

bool leo_STREXH(DisasContext *s, arg_STREXH *a)
{
    return ENABLE_ARCH_6K && op_strex(s, a, MO_UW);
}

bool leo_CLREX(DisasContext *s, arg_CLREX *a)
{
    if (s->thumb ? !ENABLE_ARCH_7 : !ENABLE_ARCH_6K) {
        return false;
    }
    call(s, LH_CLREX, 0, 0, 0);
    return true;
}

/* ---------------- block transfers ---------------- */

static int op_addr_block_pre(DisasContext *s, arg_ldst_block *a, int n)
{
    int addr = load_reg(s, a->rn);

    if (a->b) {
        addr = op1i(s, L_ADDI, addr, a->i ? 4 : -(n * 4));
    } else if (!a->i && n != 1) {
        addr = op1i(s, L_ADDI, addr, -((n - 1) * 4));
    }
    return addr;
}

static void op_addr_block_post(DisasContext *s, arg_ldst_block *a, int addr, int n)
{
    if (a->w) {
        if (!a->b) {
            addr = op1i(s, L_ADDI, addr, a->i ? 4 : -(n * 4));
        } else if (!a->i && n != 1) {
            addr = op1i(s, L_ADDI, addr, -((n - 1) * 4));
        }
        store_reg(s, a->rn, addr);
    }
}

static bool op_stm(DisasContext *s, arg_ldst_block *a)
{
    bool user = a->u;
    int list = a->list, n = ctpop16(list);

    if ((user && IS_USER(s)) || n < 1 || a->rn == 15) {
        return false;
    }
    int addr = op_addr_block_pre(s, a, n);
    for (int i = 0, j = 0; i < 16; i++) {
        if (!(list & (1 << i))) {
            continue;
        }
        int t = (user && i != 15) ? call(s, LH_USER_GET, movi(s, i), 0, 0)
                                  : load_reg(s, i);
        gen_st(s, addr, t, MO_UL | MO_ALIGN, s->mmu_idx);
        if (++j != n) {
            addr = op1i(s, L_ADDI, addr, 4);
        }
    }
    op_addr_block_post(s, a, addr, n);
    return true;
}

bool leo_STM(DisasContext *s, arg_ldst_block *a)
{
    return op_stm(s, a);
}

bool leo_STM_t32(DisasContext *s, arg_ldst_block *a)
{
    if (a->w && (a->list & (1 << a->rn))) {
        return false;
    }
    return op_stm(s, a);
}

static bool do_ldm(DisasContext *s, arg_ldst_block *a)
{
    bool user = a->u, exc_return = false, loaded_base = false;
    int list = a->list, n = ctpop16(list), loaded_var = 0;

    if (user) {
        if (IS_USER(s)) {
            return false;
        }
        if (extract32(a->list, 15, 1)) {
            exc_return = true;
            user = false;
        } else if (a->w) {
            return false;
        }
    }
    if (n < 1 || a->rn == 15) {
        return false;
    }
    int addr = op_addr_block_pre(s, a, n);
    for (int i = 0, j = 0; i < 16; i++) {
        if (!(list & (1 << i))) {
            continue;
        }
        int t = gen_ld(s, addr, MO_UL | MO_ALIGN, s->mmu_idx);
        if (user) {
            call(s, LH_USER_SET, movi(s, i), t, 0);
        } else if (i == a->rn) {
            loaded_var = t;
            loaded_base = true;
        } else if (i == 15 && exc_return) {
            emit(s, L_SETPC, 0, t, 0, 0, 0);
        } else {
            store_reg_from_load(s, i, t);
        }
        if (++j != n) {
            addr = op1i(s, L_ADDI, addr, 4);
        }
    }
    op_addr_block_post(s, a, addr, n);
    if (loaded_base) {
        store_reg(s, a->rn, loaded_var);
    }
    if (exc_return) {
        int spsr = call(s, LH_SPSR_READ, 0, 0, 0);
        call(s, LH_CPSR_WRITE_ERET, spsr, 0, 0);
        s->is_jmp = J_EXIT;
    }
    return true;
}

bool leo_LDM_a32(DisasContext *s, arg_ldst_block *a)
{
    if (ENABLE_ARCH_7 && a->w && (a->list & (1 << a->rn))) {
        return false;
    }
    return do_ldm(s, a);
}

bool leo_LDM_t32(DisasContext *s, arg_ldst_block *a)
{
    if (a->w && (a->list & (1 << a->rn))) {
        return false;
    }
    return do_ldm(s, a);
}

bool leo_LDM_t16(DisasContext *s, arg_ldst_block *a)
{
    a->w = !(a->list & (1 << a->rn));
    return do_ldm(s, a);
}

/* ---------------- branches ---------------- */

bool leo_B(DisasContext *s, arg_B *a)
{
    gen_jmp(s, jmp_diff(s, a->imm));
    return true;
}

bool leo_B_cond_thumb(DisasContext *s, arg_B_cond_thumb *a)
{
    if (a->cond >= 0xe || s->condexec_mask) {
        return false;
    }
    arm_skip_unless(s, a->cond);
    gen_jmp(s, jmp_diff(s, a->imm));
    return true;
}

bool leo_BL(DisasContext *s, arg_BL *a)
{
    emit(s, L_MOVI, 14, 0, 0, 0, s->pc_next | s->thumb);
    gen_jmp(s, jmp_diff(s, a->imm));
    return true;
}

bool leo_BLX_i(DisasContext *s, arg_BLX_i *a)
{
    if (s->thumb && (a->imm & 2)) {
        return false;
    }
    emit(s, L_MOVI, 14, 0, 0, 0, s->pc_next | s->thumb);
    emit(s, L_SETTHUMB, 0, 0, 0, 0, !s->thumb);
    gen_jmp(s, jmp_diff(s, a->imm - (s->pc_curr & 3)));
    return true;
}

bool leo_BX(DisasContext *s, arg_BX *a)
{
    if (!ENABLE_ARCH_4T) {
        return false;
    }
    gen_bx(s, load_reg(s, a->rm));
    return true;
}

bool leo_BLX_r(DisasContext *s, arg_BLX_r *a)
{
    if (!ENABLE_ARCH_5) {
        return false;
    }
    int t = load_reg(s, a->rm);
    emit(s, L_MOVI, 14, 0, 0, 0, s->pc_next | s->thumb);
    gen_bx(s, t);
    return true;
}

static bool op_tbranch(DisasContext *s, arg_tbranch *a, bool half)
{
    int t = load_reg(s, a->rm);
    if (half) {
        t = op2(s, L_ADD, t, t);
    }
    int addr = op2(s, L_ADD, load_reg(s, a->rn), t);
    int v = gen_ld(s, addr, half ? MO_UW : MO_UB, s->mmu_idx);
    v = op2(s, L_ADD, v, v);
    v = op1i(s, L_ADDI, v, s->pc_curr + jmp_diff(s, 0));
    store_reg(s, 15, v);
    return true;
}

bool leo_TBB(DisasContext *s, arg_TBB *a)
{
    return op_tbranch(s, a, false);
}

bool leo_TBH(DisasContext *s, arg_TBH *a)
{
    return op_tbranch(s, a, true);
}

bool leo_CBZ(DisasContext *s, arg_CBZ *a)
{
    int t = load_reg(s, a->rn);
    s->condlabel = newlabel(s);
    emit(s, a->nz ? L_BRZ : L_BRNZ, 0, t, 0, 0, s->condlabel << 8);
    gen_jmp(s, jmp_diff(s, a->imm));
    return true;
}

bool leo_SVC(DisasContext *s, arg_SVC *a)
{
    const uint32_t semihost_imm = s->thumb ? 0xab : 0x123456;

    if (semihosting_enabled(s->current_el == 0) && a->imm == semihost_imm) {
        return false;
    }
    emit(s, L_SETPCI, 0, 0, 0, 0, s->pc_next);
    s->svc_imm = a->imm;
    s->is_jmp = J_SWI;
    return true;
}

/* ---------------- status registers ---------------- */

static uint32_t msr_mask(DisasContext *s, int flags, int spsr)
{
    uint32_t mask = 0;

    if (flags & 1) {
        mask |= 0xff;
    }
    if (flags & 2) {
        mask |= 0xff00;
    }
    if (flags & 4) {
        mask |= 0xff0000;
    }
    if (flags & 8) {
        mask |= 0xff000000;
    }
    mask &= aarch32_cpsr_valid_mask(s->env->features, &s->cpu->isar);
    if (!spsr) {
        mask &= ~CPSR_EXEC;
    }
    if (IS_USER(s)) {
        mask &= CPSR_USER;
    }
    return mask;
}

static bool gen_set_psr(DisasContext *s, uint32_t mask, int spsr, int t)
{
    if (spsr) {
        if (IS_USER(s)) {
            return false;
        }
        call(s, LH_SPSR_WRITE, t, movi(s, mask), 0);
    } else {
        call(s, LH_CPSR_WRITE, t, movi(s, mask), 0);
    }
    gen_lookup_tb(s);
    return true;
}

bool leo_MSR_imm(DisasContext *s, arg_MSR_imm *a)
{
    uint32_t val = ror32(a->imm, a->rot * 2);
    return gen_set_psr(s, msr_mask(s, a->mask, a->r), a->r, movi(s, val));
}

bool leo_MSR_reg(DisasContext *s, arg_MSR_reg *a)
{
    return gen_set_psr(s, msr_mask(s, a->mask, a->r), a->r, load_reg(s, a->rn));
}

bool leo_MRS_reg(DisasContext *s, arg_MRS_reg *a)
{
    int t;

    if (a->r) {
        if (IS_USER(s)) {
            return false;
        }
        t = call(s, LH_SPSR_READ, 0, 0, 0);
    } else {
        t = call(s, LH_CPSR_READ, 0, 0, 0);
    }
    store_reg(s, a->rd, t);
    return true;
}

bool leo_CPS(DisasContext *s, arg_CPS *a)
{
    uint32_t mask = 0, val = 0;

    if (!ENABLE_ARCH_6) {
        return false;
    }
    if (IS_USER(s)) {
        return true;
    }
    if (a->imod & 2) {
        if (a->A) {
            mask |= CPSR_A;
        }
        if (a->I) {
            mask |= CPSR_I;
        }
        if (a->F) {
            mask |= CPSR_F;
        }
        if (a->imod & 1) {
            val |= mask;
        }
    }
    if (a->M) {
        mask |= CPSR_M;
        val |= a->mode;
    }
    if (mask) {
        return gen_set_psr(s, mask, 0, movi(s, val));
    }
    return true;
}

/* ---------------- coprocessor (cp15) ---------------- */

static bool do_coproc(DisasContext *s, int cpnum, int opc1, int crn, int crm,
                      int opc2, bool isread, int rt)
{
    uint32_t key = ENCODE_CP_REG(cpnum, 0, s->ns, crn, crm, opc1, opc2);
    const ARMCPRegInfo *ri = get_arm_cp_reginfo(s->cpu->cp_regs, key);

    if (cpnum != 15 || s->hstr_active || !ri ||
        !cp_access_ok(s->current_el, ri, isread)) {
        return false;
    }
    if ((ri->type & ARM_CP_SPECIAL_MASK) == ARM_CP_WFI) {
        return false;
    }
    if (ri->accessfn || (ri->type & ARM_CP_RAISES_EXC)) {
        uint32_t syn = syn_cp15_rt_trap(1, 0xe, opc1, opc2, crn, crm, rt,
                                        isread, false);
        call(s, LH_CP_CHECK, movi(s, key), movi(s, syn), movi(s, isread));
    }
    if ((ri->type & ARM_CP_SPECIAL_MASK) == ARM_CP_NOP) {
        return true;
    }
    if (isread) {
        int t = (ri->type & ARM_CP_CONST) ? movi(s, ri->resetvalue)
                                          : call(s, LH_CP_READ, movi(s, key), 0, 0);
        if (rt == 15) {
            call(s, LH_SET_NZCV, t, 0, 0);
        } else {
            store_reg(s, rt, t);
        }
    } else {
        if (ri->type & ARM_CP_CONST) {
            return true;
        }
        call(s, LH_CP_WRITE, movi(s, key), load_reg(s, rt), 0);
        if (!(ri->type & ARM_CP_SUPPRESS_TB_END)) {
            call(s, LH_REBUILD_HFLAGS, movi(s, !!(ri->type & ARM_CP_NEWEL)),
                 movi(s, s->current_el), 0);
            gen_lookup_tb(s);
        }
    }
    return true;
}

bool leo_MCR(DisasContext *s, arg_MCR *a)
{
    return do_coproc(s, a->cp, a->opc1, a->crn, a->crm, a->opc2, false, a->rt);
}

bool leo_MRC(DisasContext *s, arg_MRC *a)
{
    return do_coproc(s, a->cp, a->opc1, a->crn, a->crm, a->opc2, true, a->rt);
}

/* ---------------- hints and barriers ---------------- */

bool leo_NOP(DisasContext *s, arg_NOP *a)
{
    return true;
}

bool leo_DMB(DisasContext *s, arg_DMB *a)
{
    return ENABLE_ARCH_7;
}

bool leo_DSB(DisasContext *s, arg_DSB *a)
{
    return ENABLE_ARCH_7;
}

bool leo_ISB(DisasContext *s, arg_ISB *a)
{
    if (!ENABLE_ARCH_7) {
        return false;
    }
    gen_lookup_tb(s);
    return true;
}

bool leo_PLD(DisasContext *s, arg_PLD *a)
{
    return ENABLE_ARCH_5TE;
}

bool leo_PLI(DisasContext *s, arg_PLI *a)
{
    return ENABLE_ARCH_7;
}

bool leo_IT(DisasContext *s, arg_IT *a)
{
    int cond_mask = a->cond_mask;

    s->condexec_cond = (cond_mask >> 4) & 0xe;
    s->condexec_mask = cond_mask & 0x1f;
    return true;
}

/* everything else: TCG */
#include "leo-unsupported.c.inc"

/* ---------------- the block loop ---------------- */

static bool thumb_insn_is_unconditional(uint32_t insn)
{
    return (insn & 0xffffff00) == 0xbe00;           /* BKPT */
}

static void resolve_labels(DisasContext *s)
{
    g_autofree int *at = g_new0(int, s->nlabel + 1);

    for (int i = 0; i < s->nops; i++) {
        if (s->ops[i].op == L_LABEL) {
            at[s->ops[i].imm] = i;
        }
    }
    for (int i = 0; i < s->nops; i++) {
        LOp *o = &s->ops[i];
        if (o->op == L_BRCOND || o->op == L_BRZ || o->op == L_BRNZ) {
            o->imm = (o->imm & 0xff) | (uint32_t)at[o->imm >> 8] << 8;
        }
    }
}

/* leave the block after the current insn (its PC already set) */
static void emit_exit(DisasContext *s)
{
    emit(s, L_SETCOND, 0, 0, 0, 0, condexec_bits(s));
    if (s->is_jmp == J_SWI) {
        int e = movi(s, EXCP_SWI);
        int syn = movi(s, syn_aa32_svc(s->svc_imm, s->thumb));
        call(s, LH_EXCEPTION, e, syn, 0);
    }
    emit(s, L_EXIT, 0, 0, 0, 0, 0);
}

LBlock *leo_translate(CPUARMState *env, vaddr pc, uint64_t cs_base,
                      uint32_t flags, int max_insns)
{
    CPUARMTBFlags tbf = { .flags = flags, .flags2 = cs_base };
    DisasContext ctx = { 0 }, *s = &ctx;
    void *host = NULL;
    tb_page_addr_t page0;
    uint32_t condexec;
    int n;

    if (EX_TBFLAG_ANY(tbf, AARCH64_STATE) || EX_TBFLAG_ANY(tbf, SS_ACTIVE) ||
        EX_TBFLAG_ANY(tbf, BE_DATA) || EX_TBFLAG_ANY(tbf, PSTATE__IL) ||
        EX_TBFLAG_A32(tbf, SCTLR__B)) {
        return NULL;
    }
    condexec = EX_TBFLAG_AM32(tbf, CONDEXEC);
    if (condexec) {
        return NULL;                    /* starts inside an IT block */
    }
    page0 = get_page_addr_code_hostp(env, pc, &host);
    if (page0 == -1 || !host) {
        return NULL;
    }

    s->env = env;
    s->cpu = env_archcpu(env);
    s->cap = 256;
    s->ops = g_new(LOp, s->cap);
    s->thumb = EX_TBFLAG_AM32(tbf, THUMB);
    s->mmu_idx = EX_TBFLAG_ANY(tbf, MMUIDX);
    s->arm_mmu_idx = core_to_arm_mmu_idx(env, s->mmu_idx);
    s->current_el = arm_mmu_idx_to_el(s->arm_mmu_idx);
    s->user = s->current_el == 0;
    s->align_mem = EX_TBFLAG_ANY(tbf, ALIGN_MEM);
    s->ns = EX_TBFLAG_A32(tbf, NS);
    s->hstr_active = EX_TBFLAG_A32(tbf, HSTR_ACTIVE);
    s->page_start = pc & TARGET_PAGE_MASK;
    s->pc_next = pc;
    s->nlabel = 0;

    const uint8_t *code = (const uint8_t *)host - (pc & ~TARGET_PAGE_MASK);

    for (n = 0; n < max_insns; n++) {
        uint32_t at = s->pc_next, insn;
        bool ok, is16 = false;
        int start = s->nops, labels = s->nlabel;
        int save_mask = s->condexec_mask, save_cond = s->condexec_cond;

        if (at - s->page_start >= TARGET_PAGE_SIZE) {
            break;
        }
        s->pc_curr = at;
        if (s->thumb) {
            insn = lduw_le_p(code + (at - s->page_start));
            is16 = (insn >> 11) < 0x1d;
            if (!is16) {
                if (at + 2 - s->page_start >= TARGET_PAGE_SIZE) {
                    break;
                }
                insn = insn << 16 | lduw_le_p(code + (at + 2 - s->page_start));
            }
            s->pc_next = at + (is16 ? 2 : 4);
        } else {
            if (at & 3) {
                break;
            }
            insn = ldl_le_p(code + (at - s->page_start));
            s->pc_next = at + 4;
        }
        s->ntemp = LR_TEMP0;
        s->condlabel = -1;
        s->is_jmp = J_NEXT;
        emit(s, L_INSN, 0, 0, condexec_bits(s), 0, at);

        if (s->thumb) {
            if (s->condexec_mask && !thumb_insn_is_unconditional(insn) &&
                s->condexec_cond < 0xe) {
                arm_skip_unless(s, s->condexec_cond);
            }
            if (is16) {
                ok = leo_disas_t16(s, insn);
            } else if ((insn & 0xef000000) == 0xef000000 ||
                       (insn & 0xff100000) == 0xf9000000) {
                ok = false;             /* NEON */
            } else {
                ok = leo_disas_t32(s, insn);
            }
        } else {
            uint32_t cond = insn >> 28;
            if (cond == 0xf) {
                ok = leo_disas_a32_uncond(s, insn);
            } else {
                if (cond != 0xe) {
                    arm_skip_unless(s, cond);
                }
                ok = leo_disas_a32(s, insn);
            }
        }
        if (!ok) {
            /* not ours: end the block before it, TCG will run it */
            s->nops = start;
            s->nlabel = labels;
            s->condexec_mask = save_mask;
            s->condexec_cond = save_cond;
            s->pc_next = at;
            break;
        }

        /* advance the IT state */
        if (s->condexec_mask) {
            s->condexec_cond = (s->condexec_cond & 0xe) |
                               ((s->condexec_mask >> 4) & 1);
            s->condexec_mask = (s->condexec_mask << 1) & 0x1f;
            if (s->condexec_mask == 0) {
                s->condexec_cond = 0;
            }
        }
        if (s->is_jmp != J_NEXT) {
            emit_exit(s);
            if (s->condlabel >= 0) {
                setlabel(s, s->condlabel);
                s->is_jmp = J_JUMP;
                emit(s, L_SETPCI, 0, 0, 0, 0, s->pc_next);
                emit(s, L_SETCOND, 0, 0, 0, 0, condexec_bits(s));
                emit(s, L_EXIT, 0, 0, 0, 0, 0);
            }
            n++;
            goto done;
        }
        if (s->condlabel >= 0) {
            setlabel(s, s->condlabel);
        }
    }
    if (n == 0) {
        g_free(s->ops);
        return NULL;
    }
    emit(s, L_SETPCI, 0, 0, 0, 0, s->pc_next);
    emit(s, L_SETCOND, 0, 0, 0, 0, condexec_bits(s));
    emit(s, L_EXIT, 0, 0, 0, 0, 0);
done:
    resolve_labels(s);

    LBlock *b = g_new0(LBlock, 1);
    b->pc = pc;
    b->cs_base = cs_base;
    b->flags = flags;
    b->page0 = page0;
    b->page1 = -1;
    b->ninsn = n;
    b->nops = s->nops;
    b->ops = s->ops;
    b->helpers = (LHelper **)leo_helpers;
    for (int i = 0; i < s->nops; i++) {
        const LOp *o = &s->ops[i];
        if (o->op == L_CALL && o->imm != LH_CPSR_READ && o->imm != LH_SPSR_READ &&
            o->imm != LH_USER_GET && o->imm != LH_SET_NZCV) {
            b->impure = true;           /* state changes or exclusives */
        }
    }
    return b;
}
