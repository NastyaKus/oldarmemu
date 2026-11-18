/*
 * leojit back end: IR -> AArch64 host code.
 *
 * Host register use (guest state stays in host registers across chained
 * blocks; env holds it only on slow paths and outside the JIT):
 *   x19        env
 *   x20..x28   guest r0..r8        x29  guest r13 (sp)
 *   x9..x13    guest r9..r12, r14
 *   x0..x8,x14 IR temporaries      x15..x17  scratch
 *   NZCV       the guest's NZCV (same semantics in AArch32 and AArch64)
 * Slow paths (TLB miss, MMIO, helpers) store all of it to env first, so a
 * guest exception can longjmp straight out of the generated code.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "cpu.h"
#include "exec/target_page.h"
#include "exec/tlb-common.h"
#include "accel/tcg/cpu-ldst.h"
#include "exec/helper-proto.h"
#include "lir.h"
#include "leojit-backend.h"

#if defined(__aarch64__)

#include <sys/mman.h>

#define CODE_SIZE   (64 << 20)

static uint8_t *code_buf, *code_ptr, *code_end;
static void *tramp_enter, *tramp_exit, *tramp_sync_out, *tramp_sync_in;
static int off_icount, off_tlbf;        /* negative offsets from env */

/* ---------------- instruction encoders ---------------- */

typedef struct Asm {
    uint32_t *p, *start, *end;
    bool overflow;
} Asm;

static void I(Asm *a, uint32_t insn)
{
    if (a->p >= a->end) {
        a->overflow = true;
        return;
    }
    *a->p++ = insn;
}

enum { XZR = 31, SP = 31, X16 = 16, X17 = 17, X15 = 15, X19 = 19, X30 = 30 };

static void movw32(Asm *a, int rd, uint32_t v)
{
    I(a, 0x52800000 | (v & 0xffff) << 5 | rd);                 /* movz wd */
    if (v >> 16) {
        I(a, 0x72a00000 | (v >> 16) << 5 | rd);                /* movk wd, lsl 16 */
    }
}

static void movx64(Asm *a, int rd, uint64_t v)
{
    I(a, 0xd2800000 | (v & 0xffff) << 5 | rd);
    for (int s = 16; s < 64; s += 16) {
        if ((v >> s) & 0xffff) {
            I(a, 0xf2800000 | (s / 16) << 21 | ((v >> s) & 0xffff) << 5 | rd);
        }
    }
}

static void mov32(Asm *a, int rd, int rm)
{
    if (rd != rm) {
        I(a, 0x2a0003e0 | rm << 16 | rd);
    }
}

/* data processing, shifted register: base | rm | rn | rd */
static void dp3(Asm *a, uint32_t base, int rd, int rn, int rm)
{
    I(a, base | rm << 16 | rn << 5 | rd);
}

#define A_ADD   0x0b000000
#define A_ADDS  0x2b000000
#define A_SUB   0x4b000000
#define A_SUBS  0x6b000000
#define A_AND   0x0a000000
#define A_BIC   0x0a200000
#define A_ORR   0x2a000000
#define A_ORN   0x2a200000
#define A_EOR   0x4a000000
#define A_ANDS  0x6a000000
#define A_ADC   0x1a000000
#define A_ADCS  0x3a000000
#define A_SBC   0x5a000000
#define A_SBCS  0x7a000000
#define A_LSLV  0x1ac02000
#define A_LSRV  0x1ac02400
#define A_ASRV  0x1ac02800
#define A_RORV  0x1ac02c00
#define A_MUL   0x1b007c00

static void ubfm(Asm *a, int rd, int rn, int immr, int imms)
{
    I(a, 0x53000000 | immr << 16 | imms << 10 | rn << 5 | rd);
}

static void sbfm(Asm *a, int rd, int rn, int immr, int imms)
{
    I(a, 0x13000000 | immr << 16 | imms << 10 | rn << 5 | rd);
}

static void bfm(Asm *a, int rd, int rn, int immr, int imms)
{
    I(a, 0x33000000 | immr << 16 | imms << 10 | rn << 5 | rd);
}

static void lsli(Asm *a, int rd, int rn, int s)
{
    ubfm(a, rd, rn, (32 - s) & 31, 31 - s);
}

static void lsri(Asm *a, int rd, int rn, int s)
{
    ubfm(a, rd, rn, s, 31);
}

static void asri(Asm *a, int rd, int rn, int s)
{
    sbfm(a, rd, rn, s, 31);
}

static void rori(Asm *a, int rd, int rn, int s)
{
    I(a, 0x13800000 | rn << 16 | s << 10 | rn << 5 | rd);       /* extr */
}

/* bitmask immediate encoding (N:immr:imms), false if not encodable */
static bool logic_imm(uint64_t imm, int width, uint32_t *enc)
{
    if (width == 32) {
        imm = (imm & 0xffffffffull) | (imm << 32);
    }
    if (imm == 0 || imm == ~0ull) {
        return false;
    }
    int size = 64;
    while (size > 2) {
        int h = size / 2;
        uint64_t mask = (1ull << h) - 1;
        if ((imm & mask) != ((imm >> h) & mask)) {
            break;
        }
        size = h;
    }
    uint64_t m = size == 64 ? ~0ull : (1ull << size) - 1;
    uint64_t v = imm & m;
    int ones = ctpop64(v);
    int rot;
    for (rot = 0; rot < size; rot++) {
        uint64_t r = ((v >> rot) | (v << (size - rot))) & m;
        if (r == ((1ull << ones) - 1)) {
            break;
        }
    }
    if (rot == size) {
        return false;
    }
    int immr = (size - rot) % size;
    int imms = ((~(size - 1) << 1) | (ones - 1)) & 0x3f;
    int n = size == 64;
    *enc = n << 22 | immr << 16 | imms << 10;
    return true;
}

/* rd = rn OP imm (32-bit), via x17 if not encodable */
static void logici(Asm *a, uint32_t opbase_imm, uint32_t opbase_reg, int rd, int rn,
                   uint32_t imm)
{
    uint32_t enc;
    if (logic_imm(imm, 32, &enc)) {
        I(a, opbase_imm | enc | rn << 5 | rd);
    } else {
        movw32(a, X17, imm);
        dp3(a, opbase_reg, rd, rn, X17);
    }
}
#define ANDI(a, rd, rn, imm) logici(a, 0x12000000, A_AND, rd, rn, imm)
#define ORRI(a, rd, rn, imm) logici(a, 0x32000000, A_ORR, rd, rn, imm)

static void addi(Asm *a, int rd, int rn, int32_t imm)
{
    if (imm >= 0 && imm < 4096) {
        I(a, 0x11000000 | imm << 10 | rn << 5 | rd);
    } else if (imm < 0 && imm > -4096) {
        I(a, 0x51000000 | (-imm) << 10 | rn << 5 | rd);
    } else {
        movw32(a, X17, imm);
        dp3(a, A_ADD, rd, rn, X17);
    }
}

/* 64-bit rd = rn + imm (for env-relative addresses) */
static void addx(Asm *a, int rd, int rn, int64_t imm)
{
    if (imm >= 0 && imm < 4096) {
        I(a, 0x91000000 | imm << 10 | rn << 5 | rd);
    } else if (imm < 0 && imm > -4096) {
        I(a, 0xd1000000 | (-imm) << 10 | rn << 5 | rd);
    } else {
        movx64(a, X17, imm);
        I(a, 0x8b000000 | X17 << 16 | rn << 5 | rd);
    }
}

/* ldr/str w at [x19 + off] */
static void env_ldr(Asm *a, int rt, int off)
{
    g_assert(off >= 0 && off < 16384 && !(off & 3));
    I(a, 0xb9400000 | (off / 4) << 10 | X19 << 5 | rt);
}

static void env_str(Asm *a, int rt, int off)
{
    g_assert(off >= 0 && off < 16384 && !(off & 3));
    I(a, 0xb9000000 | (off / 4) << 10 | X19 << 5 | rt);
}

/* env->thumb is a bool in some QEMU versions: store it with its own size */
static void env_str_thumb(Asm *a, int rt)
{
    int off = offsetof(CPUARMState, thumb);
    if (sizeof(((CPUARMState *)0)->thumb) == 1) {
        I(a, 0x39000000 | off << 10 | X19 << 5 | rt);      /* strb */
    } else {
        env_str(a, rt, off);
    }
}

static void mrs_nzcv(Asm *a, int rt)
{
    I(a, 0xd53b4200 | rt);
}

static void msr_nzcv(Asm *a, int rt)
{
    I(a, 0xd51b4200 | rt);
}

/* branches, patched later */
static uint32_t *b_here(Asm *a)
{
    uint32_t *at = a->p;
    I(a, 0x14000000);
    return at;
}

static void set_b(uint32_t *at, void *target)
{
    ptrdiff_t d = ((uint8_t *)target - (uint8_t *)at) / 4;
    *at = (*at & 0xfc000000) | (d & 0x3ffffff);
}

static uint32_t *bcond_here(Asm *a, int cond)
{
    uint32_t *at = a->p;
    I(a, 0x54000000 | cond);
    return at;
}

static uint32_t *cbz_here(Asm *a, bool nz, int rt)
{
    uint32_t *at = a->p;
    I(a, (nz ? 0x35000000 : 0x34000000) | rt);
    return at;
}

static uint32_t *tbnz_here(Asm *a, int rt, int bit)
{
    uint32_t *at = a->p;
    I(a, 0x37000000 | (bit & 31) << 19 | rt);
    return at;
}

/* fix a b.cond / cbz / tbnz at 'at' to jump to 'target' */
static void set_bc(uint32_t *at, void *target)
{
    ptrdiff_t d = ((uint8_t *)target - (uint8_t *)at) / 4;
    if ((*at & 0x7e000000) == 0x36000000) {         /* tbz/tbnz: imm14 */
        *at = (*at & ~(0x3fffu << 5)) | (d & 0x3fff) << 5;
    } else {                                        /* b.cond, cbz: imm19 */
        *at = (*at & ~(0x7ffffu << 5)) | (d & 0x7ffff) << 5;
    }
}

static void bl_to(Asm *a, void *target)
{
    ptrdiff_t d = ((uint8_t *)target - (uint8_t *)a->p) / 4;
    if (d >= -(1 << 25) && d < (1 << 25)) {
        I(a, 0x94000000 | (d & 0x3ffffff));
    } else {
        movx64(a, X16, (uintptr_t)target);
        I(a, 0xd63f0000 | X16 << 5);                    /* blr x16 */
    }
}

static void b_to(Asm *a, void *target)
{
    ptrdiff_t d = ((uint8_t *)target - (uint8_t *)a->p) / 4;
    I(a, 0x14000000 | (d & 0x3ffffff));
}

/* ---------------- register mapping ---------------- */

static const int guest_host[15] = {
    20, 21, 22, 23, 24, 25, 26, 27, 28,     /* r0..r8 */
    9, 10, 11, 12,                          /* r9..r12 */
    29,                                     /* r13 */
    13,                                     /* r14 */
};
static const int temp_pool[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 14 };
#define NPOOL ((int)ARRAY_SIZE(temp_pool))

/* ---------------- trampolines ---------------- */

/*
 * sync_out: guest registers and flags -> env (clobbers x16, x17; keeps
 * x0..x15 and the flags).  sync_in: env -> registers and flags.
 */
static void gen_sync_out(Asm *a)
{
    for (int r = 0; r < 15; r++) {
        env_str(a, guest_host[r], offsetof(CPUARMState, regs[r]));
    }
    mrs_nzcv(a, X16);
    env_str(a, X16, offsetof(CPUARMState, NF));                 /* N is bit 31 */
    ANDI(a, X17, X16, 0x40000000);
    logici(a, 0x52000000, A_EOR, X17, X17, 0x40000000);        /* ZF = !Z */
    env_str(a, X17, offsetof(CPUARMState, ZF));
    ubfm(a, X17, X16, 29, 29);                                  /* C */
    env_str(a, X17, offsetof(CPUARMState, CF));
    lsli(a, X17, X16, 3);                                       /* V to bit 31 */
    env_str(a, X17, offsetof(CPUARMState, VF));
    I(a, 0xd65f03c0);                                           /* ret */
}

static void gen_sync_in(Asm *a)
{
    for (int r = 0; r < 15; r++) {
        env_ldr(a, guest_host[r], offsetof(CPUARMState, regs[r]));
    }
    env_ldr(a, X16, offsetof(CPUARMState, NF));
    ANDI(a, X16, X16, 0x80000000);
    env_ldr(a, X17, offsetof(CPUARMState, ZF));
    I(a, 0x7100001f | X17 << 5);                                /* cmp w17, #0 */
    I(a, 0x1a9f17e0 | X17);                                     /* cset w17, eq */
    dp3(a, A_ORR | 30 << 10, X16, X16, X17);                    /* orr w16, w16, w17, lsl 30 */
    env_ldr(a, X17, offsetof(CPUARMState, CF));
    dp3(a, A_ORR | 29 << 10, X16, X16, X17);
    env_ldr(a, X17, offsetof(CPUARMState, VF));
    lsri(a, X17, X17, 31);
    dp3(a, A_ORR | 28 << 10, X16, X16, X17);
    msr_nzcv(a, X16);
    I(a, 0xd65f03c0);
}

/*
 * enter(env, code): save callee-saved registers, load guest state, run.
 * exit (x0 = exit record or NULL): store guest state, restore, return x0.
 */
static void gen_enter(Asm *a)
{
    I(a, 0xa9ba7bfd);                       /* stp x29, x30, [sp, #-96]! */
    I(a, 0xa90153f3);                       /* stp x19, x20, [sp, #16] */
    I(a, 0xa9025bf5);                       /* stp x21, x22, [sp, #32] */
    I(a, 0xa90363f7);                       /* stp x23, x24, [sp, #48] */
    I(a, 0xa9046bf9);                       /* stp x25, x26, [sp, #64] */
    I(a, 0xa90573fb);                       /* stp x27, x28, [sp, #80] */
    I(a, 0xaa0003f3);                       /* mov x19, x0 */
    I(a, 0xaa0103f0);                       /* mov x16, x1 */
    I(a, 0xaa1003ef);                       /* mov x15, x16 */
    bl_to(a, tramp_sync_in);
    I(a, 0xd61f0000 | X15 << 5);            /* br x15 */
}

static void gen_exit(Asm *a)
{
    bl_to(a, tramp_sync_out);
    I(a, 0xa94153f3);                       /* ldp x19, x20, [sp, #16] */
    I(a, 0xa9425bf5);
    I(a, 0xa94363f7);
    I(a, 0xa9446bf9);
    I(a, 0xa94573fb);
    I(a, 0xa8c67bfd);                       /* ldp x29, x30, [sp], #96 */
    I(a, 0xd65f03c0);
}

typedef uintptr_t (*EnterFn)(CPUARMState *env, void *code);

/* ---------------- block compilation ---------------- */

typedef struct Fix {
    uint32_t *at;
    int label;
} Fix;

typedef struct Comp {
    Asm a;
    LBlock *b;
    int host[LR_NREGS];                     /* temp -> host reg, -1 */
    int lastuse[LR_NREGS];
    bool busy[32];
    uint32_t insn_pc, condexec;
    bool pc_written;                        /* this insn already set env's PC */
    Fix fix[256];
    int nfix;
    uint32_t *label_at[256];
    /* slow paths emitted after the body */
    struct Slow {
        uint32_t *jump, *resume;
        int op, d, addr, val;
        MemOpIdx oi;
        uint32_t pc, condexec;
        uint32_t live;                      /* temps (host regs) to keep */
    } slow[128];
    int nslow;
    struct Ex {
        uint32_t *jump;
        uint32_t pc;
        bool chain;
    } exits[16];
    int nexit;
    bool fail;
} Comp;

static int hreg(Comp *c, int r)
{
    if (r < LR_TEMP0) {
        g_assert(r < 15);
        return guest_host[r];
    }
    if (c->host[r] < 0) {
        for (int i = 0; i < NPOOL; i++) {
            if (!c->busy[temp_pool[i]]) {
                c->busy[temp_pool[i]] = true;
                c->host[r] = temp_pool[i];
                return c->host[r];
            }
        }
        c->fail = true;
        return X15;
    }
    return c->host[r];
}

static void release(Comp *c, int opi, const LOp *o)
{
    int regs[4] = { o->d, o->a, o->b, o->c };
    for (int k = 0; k < 4; k++) {
        int r = regs[k];
        if (r >= LR_TEMP0 && c->host[r] >= 0 && c->lastuse[r] <= opi) {
            c->busy[c->host[r]] = false;
            c->host[r] = -1;
        }
    }
}

static uint32_t live_temps(Comp *c)
{
    uint32_t m = 0;
    for (int i = 0; i < NPOOL; i++) {
        if (c->busy[temp_pool[i]]) {
            m |= 1u << temp_pool[i];
        }
    }
    return m;
}

/*
 * store PC/condexec of the current insn and all guest state to env; the
 * PC is left alone once the insn has written its new value (the call is
 * then e.g. the CPSR write of an exception return)
 */
static void sync_for_call(Comp *c, uint32_t pc, uint32_t condexec)
{
    Asm *a = &c->a;
    if (!c->pc_written) {
        movw32(a, X16, pc);
        env_str(a, X16, offsetof(CPUARMState, regs[15]));
    }
    movw32(a, X16, condexec);
    env_str(a, X16, offsetof(CPUARMState, condexec_bits));
    bl_to(a, tramp_sync_out);
}

/* save/restore temps (by host reg mask) around a C call: 16 bytes each */
static int push_regs(Asm *a, uint32_t mask, bool push)
{
    int n = 0, regs[16];
    for (int r = 0; r < 32; r++) {
        if (mask & (1u << r)) {
            regs[n++] = r;
        }
    }
    if (n & 1) {
        regs[n++] = X16;                    /* pad (x15 carries results) */
    }
    if (push) {
        for (int i = 0; i < n; i += 2) {    /* stp xa, xb, [sp, #-16]! */
            I(a, 0xa9bf0000 | regs[i + 1] << 10 | SP << 5 | regs[i]);
        }
    } else {
        for (int i = n - 2; i >= 0; i -= 2) {   /* ldp xa, xb, [sp], #16 */
            I(a, 0xa8c10000 | regs[i + 1] << 10 | SP << 5 | regs[i]);
        }
    }
    return n;
}

static void *ld_fn(MemOp size)
{
    return size == MO_8 ? (void *)cpu_ldb_mmu : size == MO_16 ? (void *)cpu_ldw_mmu
                                                              : (void *)cpu_ldl_mmu;
}

static void *st_fn(MemOp size)
{
    return size == MO_8 ? (void *)cpu_stb_mmu : size == MO_16 ? (void *)cpu_stw_mmu
                                                              : (void *)cpu_stl_mmu;
}

/* inline TLB lookup; jumps to a slow path on a miss.  Returns host addr in x17 */
static bool gen_tlb(Comp *c, int addr, MemOpIdx oi, bool store, int opi, const LOp *o)
{
    Asm *a = &c->a;
    MemOp mop = get_memop(oi);
    int mmu = get_mmuidx(oi);
    int size = 1 << (mop & MO_SIZE);
    int abits = memop_alignment_bits(mop);
    int pbits = qemu_target_page_bits();
    int fast = off_tlbf + mmu * (int)sizeof(CPUTLBDescFast);
    int cmp_off = store ? offsetof(CPUTLBEntry, addr_write) : offsetof(CPUTLBEntry, addr_read);

    if (c->nslow == ARRAY_SIZE(c->slow)) {
        c->fail = true;
        return false;
    }
    /* the tag to compare: page of the last byte, plus alignment bits */
    uint32_t pmask = (uint32_t)TARGET_PAGE_MASK;
    if (abits < (mop & MO_SIZE) && size > 1) {
        addi(a, X15, addr, size - 1);
        ANDI(a, X15, X15, pmask | ((1u << abits) - 1));
    } else {
        ANDI(a, X15, addr, pmask | (size - 1));
    }
    addx(a, X16, X19, fast);
    I(a, 0xa9400000 | X17 << 10 | X16 << 5 | X16);            /* ldp x16, x17, [x16] */
    /* and x16, x16, x(addr), lsr #(pbits - 5) */
    I(a, 0x8a400000 | addr << 16 | (pbits - CPU_TLB_ENTRY_BITS) << 10 | X16 << 5 | X16);
    I(a, 0x8b000000 | X16 << 16 | X17 << 5 | X16);            /* add x16, x17, x16 */
    I(a, 0xf9400000 | (cmp_off / 8) << 10 | X16 << 5 | X17);  /* ldr x17, [x16, #cmp] */
    I(a, 0xca000000 | X17 << 16 | X15 << 5 | X15);            /* eor x15, x15, x17 */
    struct Slow *sl = &c->slow[c->nslow++];
    sl->jump = cbz_here(a, true, X15);
    I(a, 0xf9400000 | (offsetof(CPUTLBEntry, addend) / 8) << 10 | X16 << 5 | X17);
    sl->op = o->op;
    sl->oi = oi;
    sl->pc = c->insn_pc;
    sl->condexec = c->condexec;
    return true;
}

static void gen_mem(Comp *c, int opi, const LOp *o)
{
    Asm *a = &c->a;
    bool store = o->op == L_ST8 || o->op == L_ST16 || o->op == L_ST32;
    int addr = hreg(c, o->a);
    int val = store ? hreg(c, o->b) : -1;
    int d = store ? -1 : hreg(c, o->d);

    if (!gen_tlb(c, addr, o->imm, store, opi, o)) {
        return;
    }
    struct Slow *sl = &c->slow[c->nslow - 1];
    sl->addr = addr;
    sl->val = val;
    sl->d = d;
    sl->live = live_temps(c) & ~(d >= 0 ? 1u << d : 0);
    /* register offset access: x17 + x(addr) */
    switch (o->op) {
    case L_LD8U: I(a, 0x38606800 | addr << 16 | X17 << 5 | d); break;
    case L_LD8S: I(a, 0x38e06800 | addr << 16 | X17 << 5 | d); break;
    case L_LD16U: I(a, 0x78606800 | addr << 16 | X17 << 5 | d); break;
    case L_LD16S: I(a, 0x78e06800 | addr << 16 | X17 << 5 | d); break;
    case L_LD32: I(a, 0xb8606800 | addr << 16 | X17 << 5 | d); break;
    case L_ST8: I(a, 0x38206800 | addr << 16 | X17 << 5 | val); break;
    case L_ST16: I(a, 0x78206800 | addr << 16 | X17 << 5 | val); break;
    case L_ST32: I(a, 0xb8206800 | addr << 16 | X17 << 5 | val); break;
    }
    sl->resume = a->p;
}

static void gen_slow(Comp *c, struct Slow *sl)
{
    Asm *a = &c->a;
    bool store = sl->op == L_ST8 || sl->op == L_ST16 || sl->op == L_ST32;
    MemOp size = get_memop(sl->oi) & MO_SIZE;

    set_bc(sl->jump, a->p);
    push_regs(a, sl->live | 1u << sl->addr | (store ? 1u << sl->val : 0), true);
    sync_for_call(c, sl->pc, sl->condexec);
    /* x0 env, w1 addr, (w2 val), oi, ra = 0: stage first, x0..x2 may hold them */
    mov32(a, X15, sl->addr);
    if (store) {
        mov32(a, X17, sl->val);
    }
    I(a, 0xaa1303e0);                                   /* mov x0, x19 */
    mov32(a, 1, X15);
    if (store) {
        mov32(a, 2, X17);
        movw32(a, 3, sl->oi);
        I(a, 0xd2800004);                               /* mov x4, #0 */
        bl_to(a, st_fn(size));
    } else {
        movw32(a, 2, sl->oi);
        I(a, 0xd2800003);                               /* mov x3, #0 */
        bl_to(a, ld_fn(size));
        mov32(a, X15, 0);
    }
    bl_to(a, tramp_sync_in);
    push_regs(a, sl->live | 1u << sl->addr | (store ? 1u << sl->val : 0), false);
    if (!store) {
        switch (sl->op) {
        case L_LD8S: sbfm(a, sl->d, X15, 0, 7); break;
        case L_LD16S: sbfm(a, sl->d, X15, 0, 15); break;
        default: mov32(a, sl->d, X15); break;
        }
    }
    b_to(a, sl->resume);
}

/* NZCV <- N, Z from w(n) and w(z) (z: value whose zero-ness is Z), keep C, V */
static void set_nz(Comp *c, int nreg, int zreg)
{
    Asm *a = &c->a;
    mrs_nzcv(a, X16);
    ANDI(a, X16, X16, 0x30000000);
    ANDI(a, X15, nreg, 0x80000000);
    dp3(a, A_ORR, X16, X16, X15);
    I(a, 0x7100001f | zreg << 5);                       /* cmp wz, #0 */
    I(a, 0x1a9f17e0 | X15);                             /* cset w15, eq */
    dp3(a, A_ORR | 30 << 10, X16, X16, X15);
    msr_nzcv(a, X16);
}

/* C <- bit 0 of w(src) */
static void set_c(Comp *c, int src)
{
    Asm *a = &c->a;
    mrs_nzcv(a, X16);
    bfm(a, X16, src, 3, 0);                             /* bfi w16, ws, #29, #1 */
    msr_nzcv(a, X16);
}

/* ARM register shift (amount = b & 0xff) without touching the flags */
static void gen_shift_reg(Comp *c, int op, int d, int a_, int b_)
{
    Asm *a = &c->a;
    ANDI(a, X16, b_, 0xff);
    switch (op) {
    case L_SHL:
    case L_SHR:
        dp3(a, op == L_SHL ? A_LSLV : A_LSRV, X15, a_, X16);
        lsri(a, X16, X16, 5);
        {
            uint32_t *j = cbz_here(a, false, X16);
            mov32(a, X15, XZR);
            set_bc(j, a->p);
        }
        mov32(a, d, X15);
        break;
    case L_SAR:
        lsri(a, X17, X16, 5);
        {
            uint32_t *j = cbz_here(a, false, X17);
            movw32(a, X16, 31);
            set_bc(j, a->p);
        }
        dp3(a, A_ASRV, d, a_, X16);
        break;
    case L_ROR:
        dp3(a, A_RORV, d, a_, b_);
        break;
    }
}

/* shifts with carry out (rare): a call to the C helpers, flags via env */
static void gen_shift_cc(Comp *c, const LOp *o)
{
    Asm *a = &c->a;
    void *fn = o->op == L_SHLC ? (void *)helper_shl_cc : o->op == L_SHRC ?
               (void *)helper_shr_cc : o->op == L_SARC ? (void *)helper_sar_cc
                                                       : (void *)helper_ror_cc;
    int ra = hreg(c, o->a), rb = hreg(c, o->b);
    uint32_t live = live_temps(c);
    push_regs(a, live, true);
    sync_for_call(c, c->insn_pc, c->condexec);
    mov32(a, X16, ra);
    mov32(a, X17, rb);
    I(a, 0xaa1303e0);
    mov32(a, 1, X16);
    mov32(a, 2, X17);
    bl_to(a, fn);
    mov32(a, X15, 0);
    bl_to(a, tramp_sync_in);
    push_regs(a, live, false);
    mov32(a, hreg(c, o->d), X15);
}

static void gen_call(Comp *c, const LOp *o)
{
    Asm *a = &c->a;
    int ra = hreg(c, o->a), rb = hreg(c, o->b), rc = hreg(c, o->c);
    uint32_t live = live_temps(c);
    push_regs(a, live, true);
    sync_for_call(c, c->insn_pc, c->condexec);
    mov32(a, X15, ra);
    mov32(a, X16, rb);
    mov32(a, X17, rc);
    I(a, 0xaa1303e0);
    mov32(a, 1, X15);
    mov32(a, 2, X16);
    mov32(a, 3, X17);
    bl_to(a, (void *)c->b->helpers[o->imm]);
    mov32(a, X15, 0);
    bl_to(a, tramp_sync_in);
    push_regs(a, live, false);
    mov32(a, hreg(c, o->d), X15);
}

/*
 * Indirect exit of a block without helper calls (the mode cannot have
 * changed): look the new PC up in leo_jc and jump straight there.
 */
static void gen_jc_lookup(Comp *c)
{
    Asm *a = &c->a;
    LBlock *b = c->b;
    uint32_t *miss[5];

    env_ldr(a, X16, offsetof(CPUARMState, condexec_bits));
    miss[4] = cbz_here(a, true, X16);                   /* inside an IT block */
    env_ldr(a, X16, offsetof(CPUARMState, regs[15]));
    if (sizeof(((CPUARMState *)0)->thumb) == 1) {
        I(a, 0x39400000 | offsetof(CPUARMState, thumb) << 10 | X19 << 5 | X17);
    } else {
        env_ldr(a, X17, offsetof(CPUARMState, thumb));
    }
    dp3(a, A_ORR, X16, X16, X17);                       /* key */
    ubfm(a, X17, X16, 1, LEO_JC_BITS);                  /* index */
    movx64(a, X15, (uintptr_t)leo_jc);
    I(a, 0x8b001400 | X17 << 16 | X15 << 5 | X15);      /* add x15, x15, x17, lsl 5 */
    I(a, 0xb9400000 | X15 << 5 | X17);                  /* ldr w17, [x15] */
    dp3(a, A_EOR, X17, X17, X16);
    miss[0] = cbz_here(a, true, X17);
    I(a, 0xb9400400 | X15 << 5 | X17);                  /* ldr w17, [x15, #4] */
    movw32(a, X16, b->flags & LEO_FLAGS_MASK);
    dp3(a, A_EOR, X17, X17, X16);
    miss[1] = cbz_here(a, true, X17);
    I(a, 0xf9400400 | X15 << 5 | X17);                  /* ldr x17, [x15, #8] */
    movx64(a, X16, b->cs_base);
    I(a, 0xca000000 | X16 << 16 | X17 << 5 | X17);      /* eor x17, x17, x16 */
    miss[2] = cbz_here(a, true, X17);
    I(a, 0xf9400800 | X15 << 5 | X15);                  /* ldr x15, [x15, #16] */
    miss[3] = a->p;
    I(a, 0xb4000000 | X15);                             /* cbz x15, miss */
    I(a, 0xd61f0000 | X15 << 5);                        /* br x15 */
    for (int i = 0; i < 5; i++) {
        set_bc(miss[i], a->p);
    }
}

/* leave: PC (and thumb, condexec) are in env already */
static void gen_exit_op(Comp *c, bool chain, uint32_t pc)
{
    Asm *a = &c->a;
    if (!chain && !c->b->impure) {
        gen_jc_lookup(c);
    }
    if (c->nexit == ARRAY_SIZE(c->exits)) {
        c->fail = true;
        return;
    }
    struct Ex *e = &c->exits[c->nexit++];
    e->pc = pc;
    e->chain = chain;
    e->jump = b_here(a);                /* -> exit stub (later: chained block) */
}

static void compile_body(Comp *c)
{
    LBlock *b = c->b;
    Asm *a = &c->a;
    int known_pc = -1;
    uint32_t pc_val = 0;

    for (int i = 0; i < b->nops && !c->fail; i++) {
        const LOp *o = &b->ops[i];
        int d, x, y;

        switch (o->op) {
        case L_NOP:
            break;
        case L_LABEL:
            c->label_at[o->imm] = a->p;
            known_pc = -1;
            break;
        case L_INSN:
            c->insn_pc = o->imm;
            c->condexec = o->b;
            c->pc_written = false;
            break;
        case L_MOVI:
            movw32(a, hreg(c, o->d), o->imm);
            break;
        case L_MOV:
            mov32(a, hreg(c, o->d), hreg(c, o->a));
            break;
        case L_ADD: case L_SUB: case L_AND: case L_OR: case L_XOR: case L_BIC:
        case L_ORN: case L_MUL: case L_ADC: case L_SBC:
        case L_ADDS: case L_SUBS: case L_ADCS: case L_SBCS: {
            static const uint32_t base[] = {
                [L_ADD] = A_ADD, [L_SUB] = A_SUB, [L_AND] = A_AND, [L_OR] = A_ORR,
                [L_XOR] = A_EOR, [L_BIC] = A_BIC, [L_ORN] = A_ORN, [L_MUL] = A_MUL,
                [L_ADC] = A_ADC, [L_SBC] = A_SBC, [L_ADDS] = A_ADDS, [L_SUBS] = A_SUBS,
                [L_ADCS] = A_ADCS, [L_SBCS] = A_SBCS,
            };
            x = hreg(c, o->a);
            y = hreg(c, o->b);
            d = hreg(c, o->d);
            dp3(a, base[o->op], d, x, y);
            break;
        }
        case L_ADDI:
            x = hreg(c, o->a);
            addi(a, hreg(c, o->d), x, o->imm);
            break;
        case L_NOT:
            x = hreg(c, o->a);
            dp3(a, A_ORN, hreg(c, o->d), XZR, x);
            break;
        case L_LOGICCC:
            x = hreg(c, o->a);
            set_nz(c, x, x);
            break;
        case L_LOGICQCC:
            x = hreg(c, o->a);
            y = hreg(c, o->b);
            dp3(a, A_ORR, X17, x, y);
            set_nz(c, y, X17);
            break;
        case L_SETCI:
            mrs_nzcv(a, X16);
            if (o->imm) {
                ORRI(a, X16, X16, 0x20000000);
            } else {
                ANDI(a, X16, X16, 0xdfffffff);
            }
            msr_nzcv(a, X16);
            break;
        case L_SETCBIT:
            x = hreg(c, o->a);
            lsri(a, X17, x, o->imm);
            set_c(c, X17);
            break;
        case L_SHLI: x = hreg(c, o->a); lsli(a, hreg(c, o->d), x, o->imm); break;
        case L_SHRI: x = hreg(c, o->a); lsri(a, hreg(c, o->d), x, o->imm); break;
        case L_SARI: x = hreg(c, o->a); asri(a, hreg(c, o->d), x, o->imm); break;
        case L_RORI: x = hreg(c, o->a); rori(a, hreg(c, o->d), x, o->imm); break;
        case L_RRX:
            x = hreg(c, o->a);
            d = hreg(c, o->d);
            I(a, 0x1a9f37e0 | X17);                     /* cset w17, cs */
            lsri(a, X16, x, 1);
            dp3(a, A_ORR | 31 << 10, d, X16, X17);      /* orr wd, w16, w17, lsl 31 */
            break;
        case L_SHL: case L_SHR: case L_SAR: case L_ROR:
            x = hreg(c, o->a);
            y = hreg(c, o->b);
            gen_shift_reg(c, o->op, hreg(c, o->d), x, y);
            break;
        case L_SHLC: case L_SHRC: case L_SARC: case L_RORC:
            gen_shift_cc(c, o);
            break;
        case L_CLZ: x = hreg(c, o->a); I(a, 0x5ac01000 | x << 5 | hreg(c, o->d)); break;
        case L_REV: x = hreg(c, o->a); I(a, 0x5ac00800 | x << 5 | hreg(c, o->d)); break;
        case L_REV16: x = hreg(c, o->a); I(a, 0x5ac00400 | x << 5 | hreg(c, o->d)); break;
        case L_RBIT: x = hreg(c, o->a); I(a, 0x5ac00000 | x << 5 | hreg(c, o->d)); break;
        case L_REVSH:
            x = hreg(c, o->a);
            d = hreg(c, o->d);
            I(a, 0x5ac00400 | x << 5 | d);
            sbfm(a, d, d, 0, 15);
            break;
        case L_EXT8U: x = hreg(c, o->a); ubfm(a, hreg(c, o->d), x, 0, 7); break;
        case L_EXT16U: x = hreg(c, o->a); ubfm(a, hreg(c, o->d), x, 0, 15); break;
        case L_EXT8S: x = hreg(c, o->a); sbfm(a, hreg(c, o->d), x, 0, 7); break;
        case L_EXT16S: x = hreg(c, o->a); sbfm(a, hreg(c, o->d), x, 0, 15); break;
        case L_UBFX:
            x = hreg(c, o->a);
            ubfm(a, hreg(c, o->d), x, o->imm & 0xff, (o->imm & 0xff) + (o->imm >> 8) - 1);
            break;
        case L_SBFX:
            x = hreg(c, o->a);
            sbfm(a, hreg(c, o->d), x, o->imm & 0xff, (o->imm & 0xff) + (o->imm >> 8) - 1);
            break;
        case L_BFI:
            x = hreg(c, o->a);
            d = hreg(c, o->d);
            bfm(a, d, x, (32 - (o->imm & 0xff)) & 31, (o->imm >> 8) - 1);
            break;
        case L_MULLU: case L_MULLS:
            x = hreg(c, o->a);
            y = hreg(c, o->b);
            I(a, (o->op == L_MULLU ? 0x9ba07c00 : 0x9b207c00) | y << 16 | x << 5 | X16);
            mov32(a, hreg(c, o->d), X16);
            I(a, 0xd360fc00 | X16 << 5 | hreg(c, o->c));     /* lsr x, x16, #32 */
            break;
        case L_MLALU: case L_MLALS:
            x = hreg(c, o->a);
            y = hreg(c, o->b);
            d = hreg(c, o->d);
            {
                int hi = hreg(c, o->c);
                mov32(a, X16, d);                               /* zero-extends */
                I(a, 0xaa008000 | hi << 16 | X16 << 5 | X16);   /* orr x16, x16, xhi, lsl 32 */
                I(a, (o->op == L_MLALU ? 0x9ba00000 : 0x9b200000) | y << 16 |
                  X16 << 10 | x << 5 | X16);
                mov32(a, d, X16);
                I(a, 0xd360fc00 | X16 << 5 | hi);
            }
            break;
        case L_LD8U: case L_LD8S: case L_LD16U: case L_LD16S: case L_LD32:
        case L_ST8: case L_ST16: case L_ST32:
            gen_mem(c, i, o);
            break;
        case L_BRCOND:
            if (c->nfix < (int)ARRAY_SIZE(c->fix)) {
                c->fix[c->nfix++] = (Fix){ bcond_here(a, (o->imm & 0xf) ^ 1), o->imm >> 8 };
            } else {
                c->fail = true;
            }
            break;
        case L_BRZ: case L_BRNZ:
            x = hreg(c, o->a);
            if (c->nfix < (int)ARRAY_SIZE(c->fix)) {
                c->fix[c->nfix++] = (Fix){ cbz_here(a, o->op == L_BRNZ, x), o->imm >> 8 };
            } else {
                c->fail = true;
            }
            break;
        case L_SETPC:
            x = hreg(c, o->a);
            env_str(a, x, offsetof(CPUARMState, regs[15]));
            known_pc = -1;
            c->pc_written = true;
            break;
        case L_SETPCI:
            movw32(a, X16, o->imm);
            env_str(a, X16, offsetof(CPUARMState, regs[15]));
            known_pc = 1;
            pc_val = o->imm;
            c->pc_written = true;
            break;
        case L_BX:
            x = hreg(c, o->a);
            ANDI(a, X16, x, 1);
            env_str_thumb(a, X16);
            ANDI(a, X16, x, 0xfffffffe);
            env_str(a, X16, offsetof(CPUARMState, regs[15]));
            known_pc = -1;
            c->pc_written = true;
            break;
        case L_SETTHUMB:
            movw32(a, X16, o->imm);
            env_str_thumb(a, X16);
            known_pc = -1;                      /* mode change: through the loop */
            break;
        case L_SETCOND:
            movw32(a, X16, o->imm);
            env_str(a, X16, offsetof(CPUARMState, condexec_bits));
            if (o->imm) {
                known_pc = -1;
            }
            break;
        case L_EXIT:
            gen_exit_op(c, known_pc > 0, pc_val);
            known_pc = -1;
            break;
        case L_CALL:
            gen_call(c, o);
            known_pc = -1;
            break;
        default:
            c->fail = true;
            break;
        }
        release(c, i, o);
    }
}


void *leo_backend_compile(LBlock *b)
{
    Comp *c = g_new0(Comp, 1);
    void *ret = NULL;

    if (!code_buf) {
        goto out;
    }
    memset(c->host, -1, sizeof(c->host));
    for (int i = 0; i < b->nops; i++) {
        const LOp *o = &b->ops[i];
        int regs[4] = { o->d, o->a, o->b, o->c };
        for (int k = 0; k < 4; k++) {
            if (regs[k] >= LR_TEMP0) {
                c->lastuse[regs[k]] = i;
            }
        }
    }
    c->b = b;
    c->a.start = c->a.p = (uint32_t *)code_ptr;
    c->a.end = (uint32_t *)code_end;
    Asm *a = &c->a;

    /* entry: exit request pending (IRQ) -> back to the loop, PC = this block */
    addx(a, X16, X19, off_icount);
    I(a, 0xb9400000 | X16 << 5 | X16);                  /* ldr w16, [x16] */
    uint32_t *irq = tbnz_here(a, X16, 31);
    compile_body(c);
    if (c->fail || a->overflow) {
        goto out;
    }
    for (int i = 0; i < c->nfix; i++) {
        if (!c->label_at[c->fix[i].label]) {
            goto out;
        }
        set_bc(c->fix[i].at, c->label_at[c->fix[i].label]);
    }
    c->pc_written = false;              /* memory faults report the insn's PC */
    for (int i = 0; i < c->nslow; i++) {
        gen_slow(c, &c->slow[i]);
    }
    /* IRQ stub: PC := block start */
    set_bc(irq, a->p);
    movw32(a, X16, (uint32_t)b->pc);
    env_str(a, X16, offsetof(CPUARMState, regs[15]));
    I(a, 0xd2800000);                                   /* mov x0, #0 */
    b_to(a, tramp_exit);
    /* exit stubs */
    LExitRec *recs = g_new0(LExitRec, c->nexit);
    for (int i = 0; i < c->nexit; i++) {
        set_b(c->exits[i].jump, a->p);
        recs[i].site = c->exits[i].jump;
        recs[i].orig = *c->exits[i].jump;
        recs[i].pc = c->exits[i].pc;
        recs[i].chain = c->exits[i].chain;
        recs[i].from = b;
        movx64(a, 0, (uintptr_t)&recs[i]);
        b_to(a, tramp_exit);
    }
    if (a->overflow) {
        g_free(recs);
        goto out;
    }
    b->exits = recs;
    b->nexits = c->nexit;
    ret = code_ptr;
    code_ptr = (uint8_t *)QEMU_ALIGN_PTR_UP(a->p, 16);
    __builtin___clear_cache((char *)ret, (char *)a->p);
out:
    g_free(c);
    return ret;
}

bool leo_backend_full(void)
{
    return code_buf && code_end - code_ptr < (1 << 20);
}

void leo_backend_reset(void)
{
    if (code_buf) {
        code_ptr = (uint8_t *)tramp_sync_in + 4096;     /* keep the trampolines */
    }
}

uintptr_t leo_backend_run(CPUARMState *env, void *code)
{
    return ((EnterFn)tramp_enter)(env, code);
}

/* point an exit at another block's code (or back at its stub) */
void leo_backend_patch(uint32_t *site, void *target, uint32_t orig)
{
    uint32_t insn = orig;
    if (target) {
        ptrdiff_t d = ((uint8_t *)target - (uint8_t *)site) / 4;
        insn = 0x14000000 | (d & 0x3ffffff);
    }
    qatomic_set(site, insn);
    __builtin___clear_cache((char *)site, (char *)(site + 1));
}

bool leo_backend_init(CPUARMState *env)
{
    CPUState *cs = env_cpu(env);

    code_buf = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code_buf == MAP_FAILED) {
        code_buf = NULL;
        warn_report("leojit: no executable memory, interpreting");
        return false;
    }
    code_ptr = code_buf;
    code_end = code_buf + CODE_SIZE;
    off_icount = (int)((uintptr_t)&cs->neg.icount_decr.u32 - (uintptr_t)env);
    off_tlbf = (int)((uintptr_t)&cs->neg.tlb.f[0] - (uintptr_t)env);

    Asm a = { .p = (uint32_t *)code_buf, .start = (uint32_t *)code_buf,
              .end = (uint32_t *)(code_buf + 4096) };
    tramp_sync_out = a.p;
    gen_sync_out(&a);
    tramp_sync_in = a.p;
    gen_sync_in(&a);
    tramp_exit = a.p;
    gen_exit(&a);
    tramp_enter = a.p;
    gen_enter(&a);
    code_ptr = (uint8_t *)tramp_sync_in + 4096;
    __builtin___clear_cache((char *)code_buf, (char *)a.p);
    info_report("leojit: AArch64 code generator ready (env: pc %zu NF %zu thumb %zu "
                "condexec %zu aarch64 %zu, icount %d tlb %d)",
                offsetof(CPUARMState, regs[15]), offsetof(CPUARMState, NF),
                offsetof(CPUARMState, thumb), offsetof(CPUARMState, condexec_bits),
                offsetof(CPUARMState, aarch64), off_icount, off_tlbf);
    return true;
}

#else   /* !__aarch64__ */

bool leo_backend_init(CPUARMState *env)
{
    return false;
}

void *leo_backend_compile(LBlock *b)
{
    return NULL;
}

bool leo_backend_full(void)
{
    return false;
}

void leo_backend_reset(void)
{
}

uintptr_t leo_backend_run(CPUARMState *env, void *code)
{
    g_assert_not_reached();
}

void leo_backend_patch(uint32_t *site, void *target, uint32_t orig)
{
}

#endif
