/*
 * leojit: our own ARMv7-A execution engine, used instead of TCG for the
 * guest code it understands (TCG runs the rest).
 *
 * Guest instructions are decoded with QEMU's decodetree descriptions
 * (a32/t32/t16.decode, trans functions prefixed leo_) into a small
 * register IR, one block per guest basic block.  The IR is run by an
 * interpreter (portable, the reference) or compiled to host code.  All
 * guest state lives in CPUARMState, in the same format TCG uses, so the
 * two engines can hand over at any block boundary.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TARGET_ARM_LEOJIT_LIR_H
#define TARGET_ARM_LEOJIT_LIR_H

#include "cpu.h"
#include "exec/memopidx.h"

/* IR registers: 0..15 the guest's r0..r15 (r15 is never read: the
 * decoder turns reads of the PC into constants), 16.. temporaries */
#define LR_TEMP0    16
#define LR_NREGS    160

enum {
    L_NOP,
    L_INSN,         /* start of a guest insn: imm pc, b condexec bits */
    L_MOVI,         /* d = imm */
    L_MOV,          /* d = a */
    L_ADD, L_SUB, L_AND, L_OR, L_XOR, L_BIC, L_ORN, L_MUL,  /* d = a op b */
    L_ADDI,         /* d = a + imm */
    L_NOT,          /* d = ~a */
    L_ADC,          /* d = a + b + C */
    L_SBC,          /* d = a - b + C - 1 */
    L_ADDS, L_SUBS, L_ADCS, L_SBCS,     /* as above, setting NZCV */
    L_LOGICCC,      /* N, Z from a */
    L_LOGICQCC,     /* N from b (high), Z from a | b */
    L_SETCI,        /* C = imm */
    L_SETCBIT,      /* C = (a >> imm) & 1 */
    L_SHLI, L_SHRI, L_SARI, L_RORI,     /* d = a op imm (1..31) */
    L_RRX,          /* d = C:a >> 1 */
    L_SHL, L_SHR, L_SAR, L_ROR,         /* by register b, ARM semantics */
    L_SHLC, L_SHRC, L_SARC, L_RORC,     /* same, setting C as ARM does */
    L_CLZ, L_REV, L_REV16, L_REVSH, L_RBIT,
    L_EXT8U, L_EXT16U, L_EXT8S, L_EXT16S,
    L_UBFX, L_SBFX,  /* d = field of a at imm & 0xff, width imm >> 8 */
    L_BFI,           /* d = d with a's low bits at imm & 0xff, width imm >> 8 */
    L_MULLU, L_MULLS,   /* {c:d} = a * b, 64-bit */
    L_MLALU, L_MLALS,   /* {c:d} += a * b */
    L_LD8U, L_LD8S, L_LD16U, L_LD16S, L_LD32,   /* d = [a], imm = memop idx */
    L_ST8, L_ST16, L_ST32,                       /* [a] = b */
    L_BRCOND,       /* if !cond(imm & 15) goto label (imm >> 8) */
    L_BRZ,          /* if a == 0 goto label (imm >> 8) */
    L_BRNZ,         /* if a != 0 goto label (imm >> 8) */
    L_LABEL,        /* label imm (resolved to an op index) */
    L_SETPC,        /* pc = a */
    L_SETPCI,       /* pc = imm */
    L_BX,           /* thumb = a & 1, pc = a & ~1 */
    L_SETTHUMB,     /* thumb = imm */
    L_SETCOND,      /* condexec_bits = imm */
    L_EXIT,         /* leave the block (pc set) */
    L_CALL,         /* d = helper imm (env, a, b, c) */
    L_NOPS
};

typedef struct LOp {
    uint8_t op, d, a, b, c;
    uint32_t imm;
} LOp;

/* helpers callable from IR: uint32_t fn(env, a, b, c) */
typedef uint32_t LHelper(CPUARMState *env, uint32_t a, uint32_t b, uint32_t c);

typedef struct LBlock LBlock;

/* a block exit in generated code: patched to jump straight to the next block */
typedef struct LExitRec {
    uint32_t *site;                 /* the b instruction */
    uint32_t orig;                  /* its encoding towards the exit stub */
    uint32_t pc;                    /* target pc, when known */
    bool chain;
    LBlock *from;
} LExitRec;

typedef struct LIncoming {
    uint32_t *site;
    uint32_t orig;
} LIncoming;

struct LBlock {
    vaddr pc;
    uint64_t cs_base;
    uint32_t flags;                     /* TB flags (cs_base holds flags2) */
    ram_addr_t page0, page1;            /* code pages (page1 -1 if one) */
    uint32_t ninsn;                     /* guest instructions */
    uint32_t nops;
    LOp *ops;
    LHelper **helpers;                  /* helper table index -> function */
    struct LBlock *hnext;               /* hash chain */
    struct LBlock *pnext;               /* chain of blocks of a code page */
    bool dead;
    bool impure;                        /* calls with side effects: not replayable */
    uint32_t nverified;
    void *code;                         /* host code, when compiled */
    bool nojit;                         /* the back end could not compile it */
    LExitRec *exits;
    int nexits;
    GArray *incoming;                   /* LIncoming: exits patched to us */
};

/*
 * Indirect branch cache, read by generated code: key = pc | thumb, flags
 * and flags2 (cs_base) without the THUMB/CONDEXEC bits, and the code.
 */
typedef struct LJmp {
    uint32_t key, flags;
    uint64_t flags2;
    void *code;
    uint64_t pad;
} LJmp;
#define LEO_JC_BITS 12
extern LJmp leo_jc[1 << LEO_JC_BITS];
void leo_jc_clear(void);
#define LEO_FLAGS_MASK (~(uint32_t)(R_TBFLAG_AM32_THUMB_MASK | R_TBFLAG_AM32_CONDEXEC_MASK))

/* interp.c */
void leo_interp(CPUARMState *env, LBlock *b);

/* verification: the interpreter logs its stores and notes MMIO accesses */
typedef struct LeoWrite {
    uint32_t addr, old, new;
    MemOpIdx oi;
} LeoWrite;
extern bool leo_verify_log;
extern LeoWrite leo_wlog[];
extern int leo_nwlog;
extern bool leo_mmio_seen;
#define LEO_WLOG_MAX 256
uint32_t leo_mem_ld(CPUARMState *env, uint32_t addr, MemOpIdx oi);
void leo_mem_st(CPUARMState *env, uint32_t addr, uint32_t val, MemOpIdx oi);

/* frontend.c */
LBlock *leo_translate(CPUARMState *env, vaddr pc, uint64_t cs_base,
                      uint32_t flags, int max_insns);

/* helpers.c: functions the IR calls */
extern LHelper *const leo_helpers[];
enum {
    LH_CPSR_READ, LH_CPSR_WRITE, LH_SPSR_READ, LH_SPSR_WRITE,
    LH_CPSR_WRITE_ERET, LH_EXCEPTION, LH_CP_READ, LH_CP_WRITE,
    LH_CP_CHECK, LH_SET_NZCV, LH_USER_GET, LH_USER_SET,
    LH_LDREX, LH_STREX, LH_CLREX, LH_REBUILD_HFLAGS,
    LH_NHELPERS
};

#endif
