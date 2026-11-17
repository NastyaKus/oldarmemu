/*
 * Qualcomm Adreno 200 ("Yamato", a2xx) GPU of the QSD8250.
 *
 * The guest runs its own KGSL kernel driver and the ROM's closed
 * libGLES*_adreno200 user space; we model the hardware they talk to:
 *
 *  - the register file at 0xA0000000 (dword offsets as in freedreno's
 *    a2xx.xml / KGSL yamato_reg.h), RBBM identification and status,
 *  - the MH MMU: one flat page table, PTE = phys | RV(1) | WV(2),
 *  - the command processor: ring buffer, indirect buffers and the PM4
 *    packets KGSL and the GL driver use (timestamps, interrupts,
 *    constants, shader instruction loads, register writes, draws),
 *  - INT_GRAPHICS.
 *
 * Commands execute synchronously when the ring's write pointer moves, so
 * the GPU is idle whenever the CPU looks.  Drawing is in adreno_draw.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "adreno.h"

/* ---- registers (dword offsets) ---- */
#define REG_RBBM_PATCH_RELEASE  0x0001
#define REG_MH_MMU_CONFIG       0x0040
#define REG_MH_MMU_VA_RANGE     0x0041
#define REG_MH_MMU_PT_BASE      0x0042
#define REG_MH_MMU_PAGE_FAULT   0x0043
#define REG_MH_MMU_INVALIDATE   0x0045
#define REG_CP_RB_BASE          0x01c0
#define REG_CP_RB_CNTL          0x01c1
#define REG_CP_RB_RPTR_ADDR     0x01c3
#define REG_CP_RB_RPTR          0x01c4
#define REG_CP_RB_WPTR          0x01c5
#define REG_CP_RB_WPTR_BASE     0x01c8
#define REG_SCRATCH_UMSK        0x01dc
#define REG_SCRATCH_ADDR        0x01dd
#define REG_CP_INT_CNTL         0x01f2
#define REG_CP_INT_STATUS       0x01f3
#define REG_CP_INT_ACK          0x01f4
#define REG_CP_ME_CNTL          0x01f6
#define REG_CP_ME_STATUS        0x01f7
#define REG_CP_ME_RAM_WADDR     0x01f8
#define REG_CP_ME_RAM_RADDR     0x01f9
#define REG_CP_ME_RAM_DATA      0x01fa
#define REG_RBBM_INT_CNTL       0x03b4
#define REG_RBBM_INT_STATUS     0x03b5
#define REG_RBBM_INT_ACK        0x03b6
#define REG_MASTER_INT_SIGNAL   0x03b7
#define REG_RBBM_PERIPHID1      0x03f9
#define REG_RBBM_PERIPHID2      0x03fa
#define REG_CP_STAT             0x047f
#define REG_SCRATCH_REG0        0x0578
#define REG_RBBM_STATUS         0x05d0
#define REG_MH_INTERRUPT_MASK   0x0a42
#define REG_MH_INTERRUPT_STATUS 0x0a43
#define REG_MH_INTERRUPT_CLEAR  0x0a44
#define REG_SQ_INT_CNTL         0x0d34
#define REG_SQ_INT_STATUS       0x0d35
#define REG_SQ_INT_ACK          0x0d36

#define RB_CNTL_NO_UPDATE       (1u << 27)

/* PM4 */
#define CP_NOP                  0x10
#define CP_REG_RMW              0x21
#define CP_DRAW_INDX            0x22
#define CP_VIZ_QUERY            0x23
#define CP_SET_STATE            0x25
#define CP_WAIT_FOR_IDLE        0x26
#define CP_IM_LOAD              0x27
#define CP_IM_LOAD_IMMEDIATE    0x2b
#define CP_IM_STORE             0x2c
#define CP_SET_CONSTANT         0x2d
#define CP_LOAD_CONSTANT_CONTEXT 0x2e
#define CP_DRAW_INDX_BIN        0x34
#define CP_DRAW_INDX_2_BIN      0x35
#define CP_DRAW_INDX_2          0x36
#define CP_INDIRECT_BUFFER_PFD  0x37
#define CP_INVALIDATE_STATE     0x3b
#define CP_WAIT_REG_MEM         0x3c
#define CP_MEM_WRITE            0x3d
#define CP_REG_TO_MEM           0x3e
#define CP_INDIRECT_BUFFER      0x3f
#define CP_INTERRUPT            0x40
#define CP_MEM_TO_REG           0x42
#define CP_COND_EXEC            0x44
#define CP_COND_WRITE           0x45
#define CP_EVENT_WRITE          0x46
#define CP_ME_INIT              0x48
#define CP_SET_SHADER_BASES     0x4a
#define CP_SET_DRAW_INIT_FLAGS  0x4b
#define CP_MEM_WRITE_CNTR       0x4f
#define CP_SET_BIN_MASK         0x50
#define CP_SET_BIN_SELECT       0x51
#define CP_EVENT_WRITE_SHD      0x58
#define CP_EVENT_WRITE_CFL      0x59
#define CP_EVENT_WRITE_ZPD      0x5b
#define CP_SET_PROTECTED_MODE   0x5f
#define CP_SET_BIN_MASK_LO      0x60
#define CP_SET_BIN_MASK_HI      0x61
#define CP_SET_BIN_SELECT_LO    0x62
#define CP_SET_BIN_SELECT_HI    0x63

#define MAX_IB_DWORDS           (1 << 20)

int adreno_trace;
long adreno_trace_from;
long adreno_trace_len = 40;

/* ---------------- interrupts ---------------- */

static uint32_t adreno_irq_signal(AdrenoState *s)
{
    uint32_t sig = 0;

    if (s->regs[REG_MH_INTERRUPT_STATUS] & s->regs[REG_MH_INTERRUPT_MASK]) {
        sig |= 1u << 5;
    }
    if (s->regs[REG_SQ_INT_STATUS] & s->regs[REG_SQ_INT_CNTL]) {
        sig |= 1u << 26;
    }
    if (s->regs[REG_CP_INT_STATUS] & s->regs[REG_CP_INT_CNTL]) {
        sig |= 1u << 30;
    }
    if (s->regs[REG_RBBM_INT_STATUS] & s->regs[REG_RBBM_INT_CNTL]) {
        sig |= 1u << 31;
    }
    s->regs[REG_MASTER_INT_SIGNAL] = sig;
    return sig;
}

/* caller holds s->lock; from the GPU thread the line is set by a BH */
static void adreno_update_irq(AdrenoState *s)
{
    uint32_t sig = adreno_irq_signal(s);

    if (qemu_thread_is_self(&s->thread)) {
        qemu_bh_schedule(s->irq_bh);
    } else {
        qemu_set_irq(s->irq, sig != 0);
    }
}

static void adreno_irq_bh(void *opaque)
{
    AdrenoState *s = opaque;
    uint32_t sig;

    qemu_mutex_lock(&s->lock);
    sig = adreno_irq_signal(s);
    qemu_mutex_unlock(&s->lock);
    qemu_set_irq(s->irq, sig != 0);
}

/* ---------------- GPU memory (MH + MMU) ---------------- */

/* GPU virtual -> physical; false (and an MMU fault) when unmapped */
static void adreno_tlb_flush(AdrenoState *s)
{
    s->tlb_valid = false;
    memset(s->hp_tag, 0, sizeof(s->hp_tag));
}

bool adreno_xlate(AdrenoState *s, uint32_t va, hwaddr *pa)
{
    uint32_t cfg = s->regs[REG_MH_MMU_CONFIG];
    uint32_t range = s->regs[REG_MH_MMU_VA_RANGE];
    uint32_t base = range & 0xfffff000;
    uint64_t size = (uint64_t)(range & 0xfff) << 16;

    if (!(cfg & 1) || va < base || va - base >= size) {
        *pa = va;                       /* untranslated (MPU) region */
        return true;
    }
    uint32_t idx = (va - base) >> 12;
    if (idx != s->tlb_idx || !s->tlb_valid) {
        uint32_t pte = ldl_le_phys(&address_space_memory,
                                   s->regs[REG_MH_MMU_PT_BASE] + idx * 4);
        if (!(pte & 3)) {
            if (!s->fault_reported) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "adreno: MMU fault at gpu va 0x%08x\n", va);
                s->fault_reported = true;
            }
            s->regs[REG_MH_MMU_PAGE_FAULT] = va | 1;
            return false;
        }
        s->tlb_idx = idx;
        s->tlb_pte = pte;
        s->tlb_valid = true;
    }
    *pa = (s->tlb_pte & 0xfffff000) | (va & 0xfff);
    return true;
}

static uint64_t adreno_watch;             /* debug: ADRENO_WATCH=<pa> */

static void watch_check(AdrenoState *s, hwaddr pa, const uint8_t *p,
                        uint32_t n, const char *what)
{
    if (adreno_watch && adreno_watch >= pa && adreno_watch + 4 <= pa + n) {
        qemu_log("adreno: watch %s pa %08" PRIx64 " = %08x draw %" PRIu64
                 " mode %u\n", what, (uint64_t)adreno_watch,
                 ldl_le_p(p + (adreno_watch - pa)), s->draws,
                 s->regs[0x2208 /* RB_MODECONTROL */] & 7);
        qemu_log("   dest %08x pitch %x info %08x off %08x surf %08x"
                 " cinfo %08x pt %08x\n", s->regs[0x2319], s->regs[0x231a],
                 s->regs[0x231b], s->regs[0x231c], s->regs[0x2000],
                 s->regs[0x2001], s->regs[REG_MH_MMU_PT_BASE]);
    }
}

uint8_t *adreno_host_page_slow(AdrenoState *s, uint32_t va)
{
    uint32_t pg = va >> 12, i = pg & (ADRENO_HP_ENTRIES - 1);
    hwaddr pa, xlat, len = 4096;
    MemoryRegion *mr;
    uint8_t *p = NULL;

    if (!adreno_xlate(s, va & ~0xfffu, &pa)) {
        return NULL;
    }
    RCU_READ_LOCK_GUARD();
    mr = address_space_translate(&address_space_memory, pa, &xlat, &len,
                                 false, MEMTXATTRS_UNSPECIFIED);
    if (memory_region_is_ram(mr) && len >= 4096) {
        p = (uint8_t *)memory_region_get_ram_ptr(mr) + xlat;
        s->hp_tag[i] = pg + 1;
        s->hp_ptr[i] = p;
    }
    return p;
}

/* Mark GPU pages [va, va+len) written through host pointers as dirty. */
void adreno_mark_dirty(AdrenoState *s, uint32_t va, uint32_t len)
{
    uint32_t end = va + len;

    for (va &= ~0xfffu; va < end; va += 4096) {
        hwaddr pa, xlat, l = 4096;
        if (!adreno_xlate(s, va, &pa)) {
            continue;
        }
        RCU_READ_LOCK_GUARD();
        MemoryRegion *mr = address_space_translate(&address_space_memory, pa,
                                                   &xlat, &l, true,
                                                   MEMTXATTRS_UNSPECIFIED);
        if (memory_region_is_ram(mr)) {
            memory_region_set_dirty(mr, xlat, MIN(l, 4096));
        }
    }
}

uint32_t adreno_rd32(AdrenoState *s, uint32_t va)
{
    hwaddr pa;

    if (!adreno_xlate(s, va, &pa)) {
        return 0;
    }
    return ldl_le_phys(&address_space_memory, pa);
}

void adreno_wr32(AdrenoState *s, uint32_t va, uint32_t val)
{
    hwaddr pa;

    if (adreno_xlate(s, va, &pa)) {
        /*
         * CP writes (timestamps, read pointer, scratch shadows) tell the
         * CPU that earlier GPU output is ready: on weakly ordered hosts
         * (ARM64 phones) the pixels must be visible first.
         */
        smp_wmb();
        stl_le_phys(&address_space_memory, pa, val);
        if (adreno_watch) {
            uint8_t b[4];
            stl_le_p(b, val);
            watch_check(s, pa, b, 4, "wr32");
        }
    }
}

/* Copy GPU memory, page by page. */
void adreno_read(AdrenoState *s, uint32_t va, void *buf, uint32_t len)
{
    uint8_t *p = buf;

    while (len) {
        uint32_t n = MIN(len, 4096 - (va & 4095));
        hwaddr pa;
        if (adreno_xlate(s, va, &pa)) {
            address_space_read(&address_space_memory, pa,
                               MEMTXATTRS_UNSPECIFIED, p, n);
        } else {
            memset(p, 0, n);
        }
        va += n;
        p += n;
        len -= n;
    }
}

void adreno_write(AdrenoState *s, uint32_t va, const void *buf, uint32_t len)
{
    const uint8_t *p = buf;

    smp_wmb();                          /* see adreno_wr32 */
    while (len) {
        uint32_t n = MIN(len, 4096 - (va & 4095));
        hwaddr pa;
        if (adreno_xlate(s, va, &pa)) {
            address_space_write(&address_space_memory, pa,
                                MEMTXATTRS_UNSPECIFIED, p, n);
            watch_check(s, pa, p, n, "write");
        }
        va += n;
        p += n;
        len -= n;
    }
}

/* ---------------- register writes from the CP ---------------- */

/* shadow type and index of a register, -1 if not shadowed */
static int shadow_slot(uint32_t reg, uint32_t *idx)
{
    if (reg >= 0x2000 && reg < 0x2400) {
        *idx = reg - 0x2000;
        return 4;
    }
    if (reg >= ADRENO_REG_ALU && reg < ADRENO_REG_ALU + 2048) {
        *idx = reg - ADRENO_REG_ALU;
        return 0;
    }
    if (reg >= ADRENO_REG_FETCH && reg < ADRENO_REG_FETCH + ADRENO_FETCH_CONSTS) {
        *idx = reg - ADRENO_REG_FETCH;
        return 1;
    }
    return -1;
}

static void gpu_reg_write(AdrenoState *s, uint32_t reg, uint32_t val)
{
    uint32_t idx;
    int t;

    if (reg >= ADRENO_NREGS) {
        return;
    }
    if (reg >= REG_SCRATCH_REG0 && reg < REG_SCRATCH_REG0 + 8) {
        /* polled by the CPU without the lock (KGSL timestamps) */
        qatomic_store_release(&s->regs[reg], val);
    } else {
        s->regs[reg] = val;
    }
    if (reg >= REG_MH_MMU_CONFIG && reg <= REG_MH_MMU_INVALIDATE) {
        /* per-process page table switch from the ring (KGSL setstate) */
        adreno_tlb_flush(s);
        s->fault_reported = false;
    }
    if (reg >= REG_SCRATCH_REG0 && reg < REG_SCRATCH_REG0 + 8) {
        unsigned i = reg - REG_SCRATCH_REG0;
        if (s->regs[REG_SCRATCH_UMSK] & (1u << i)) {
            adreno_wr32(s, s->regs[REG_SCRATCH_ADDR] + i * 4, val);
        }
    }
    t = shadow_slot(reg, &idx);
    if (t >= 0 && s->shadow_en[t]) {
        adreno_wr32(s, s->shadow_base[t] + idx * 4, val);
    }
    adreno_reg_written(s, reg, val);
}

/* ---------------- command processor ---------------- */

static void cp_exec(AdrenoState *s, const uint32_t *cmd, uint32_t n, int level);

static void cp_indirect(AdrenoState *s, uint32_t addr, uint32_t len, int level)
{
    if (level >= 3 || len > MAX_IB_DWORDS) {
        qemu_log_mask(LOG_GUEST_ERROR, "adreno: bad IB 0x%08x+%u level %d\n",
                      addr, len, level);
        return;
    }
    g_autofree uint32_t *buf = g_new(uint32_t, len ? len : 1);
    adreno_read(s, addr, buf, len * 4);
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = le32_to_cpu(buf[i]);
    }
    cp_exec(s, buf, len, level + 1);
}

static const uint32_t const_base[5] = {
    ADRENO_REG_ALU, ADRENO_REG_FETCH, ADRENO_REG_BOOL, ADRENO_REG_LOOP, 0x2000,
};

static void cp_set_constant(AdrenoState *s, const uint32_t *d, uint32_t n)
{
    uint32_t type = (d[0] >> 16) & 7, off = d[0] & 0x7ff;

    if (type > 4) {
        return;
    }
    for (uint32_t i = 1; i < n; i++, off++) {
        gpu_reg_write(s, const_base[type] + off, d[i]);
    }
}

/*
 * LOAD_CONSTANT_CONTEXT: d0 shadow address (bit 0: force reload),
 * d1 (shadow enable << 24 | type << 16 | start), then for registers
 * (type 4) pairs of (4 << 16 | offset, count), else a count.  Loads the
 * listed values from the shadow and (de)activates shadowing of later
 * writes to that memory.
 */
static void cp_load_constant_context(AdrenoState *s, const uint32_t *d,
                                     uint32_t n)
{
    uint32_t base = d[0] & ~0x1fffu, type = (d[1] >> 16) & 0xff;
    bool en = (d[1] >> 24) & 1;
    uint32_t start = d[1] & 0xffff;

    if (type > 4) {
        return;
    }
    s->shadow_base[type] = base;
    s->shadow_en[type] = false;            /* no echo while loading */
    if (type == 4) {
        for (uint32_t i = 2; i + 1 < n; i += 2) {
            uint32_t off = d[i] & 0xffff, cnt = d[i + 1];
            for (uint32_t k = 0; k < cnt && off + k < 0x400; k++) {
                gpu_reg_write(s, 0x2000 + off + k,
                              adreno_rd32(s, base + (off + k) * 4));
            }
        }
    } else if (n >= 3) {
        uint32_t cnt = d[2];
        for (uint32_t k = 0; k < cnt && k < 2048; k++) {
            gpu_reg_write(s, const_base[type] + start + k,
                          adreno_rd32(s, base + (start + k) * 4));
        }
    }
    s->shadow_en[type] = en;
}

/* instruction index of IM_LOAD start @start of type 0 vtx / 1 pix / 2 shared */
static uint32_t im_where(AdrenoState *s, uint32_t type, uint32_t start)
{
    uint32_t part = s->regs[ADRENO_REG_INST_STORE];
    return start + (type == 0 ? (part >> 16) & 0xfff :
                    type == 1 ? (part & 0xfff) : 0);
}

static void cp_im_load(AdrenoState *s, uint32_t type, uint32_t start,
                       uint32_t size, const uint32_t *data, uint32_t addr)
{
    uint32_t at = im_where(s, type, start) * 3, dw = size * 3;

    if (at + dw > ADRENO_ISTORE_DWORDS) {
        qemu_log_mask(LOG_GUEST_ERROR, "adreno: IM_LOAD out of range\n");
        return;
    }
    if (data) {
        memcpy(&s->istore[at], data, dw * 4);
    } else {
        adreno_read(s, addr, &s->istore[at], dw * 4);
        for (uint32_t i = 0; i < dw; i++) {
            s->istore[at + i] = le32_to_cpu(s->istore[at + i]);
        }
    }
    if (type <= 2) {
        s->im_start[type] = start;
        s->im_size[type] = size;
    }
}

static void cp_type3(AdrenoState *s, uint32_t op, const uint32_t *d, uint32_t n,
                     int level, uint32_t *skip)
{
    switch (op) {
    case CP_NOP:
    case 0x52:                      /* WAIT_REG_EQ */
    case 0x53:                      /* WAIT_REG_GTE */
    case 0x5c:                      /* WAIT_UNTIL_READ */
    case 0x5d:                      /* WAIT_IB_PFD_COMPLETE */
    case 0x5e:                      /* CONTEXT_UPDATE */
    case CP_ME_INIT:
    case CP_WAIT_FOR_IDLE:
    case CP_WAIT_REG_MEM:
    case CP_INVALIDATE_STATE:
    case CP_SET_PROTECTED_MODE:
    case CP_SET_STATE:
        break;
    case CP_INDIRECT_BUFFER:
    case CP_INDIRECT_BUFFER_PFD:
        if (n >= 2) {
            cp_indirect(s, d[0], d[1] & 0xfffff, level);
        }
        break;
    case CP_REG_RMW:
        if (n >= 3) {
            uint32_t r = d[0] & 0x7fff;
            if (r < ADRENO_NREGS) {
                gpu_reg_write(s, r, (s->regs[r] & d[1]) | d[2]);
            }
        }
        break;
    case CP_REG_TO_MEM:
        if (n >= 2) {
            uint32_t r = d[0] & 0x7fff;
            adreno_wr32(s, d[1], r < ADRENO_NREGS ? s->regs[r] : 0);
        }
        break;
    case CP_MEM_TO_REG:
        if (n >= 2) {
            gpu_reg_write(s, d[0] & 0x7fff, adreno_rd32(s, d[1]));
        }
        break;
    case CP_MEM_WRITE:
        for (uint32_t i = 1; i < n; i++) {
            adreno_wr32(s, d[0] + (i - 1) * 4, d[i]);
        }
        break;
    case CP_MEM_WRITE_CNTR:
        if (n >= 1) {
            adreno_wr32(s, d[0], 0);
        }
        break;
    case CP_EVENT_WRITE:
    case CP_EVENT_WRITE_SHD:
    case CP_EVENT_WRITE_CFL:
    case CP_EVENT_WRITE_ZPD:
        if (n >= 1) {
            adreno_event(s, d[0] & 0x3f);
        }
        if (n >= 3) {
            adreno_wr32(s, d[1], d[2]);  /* *_TS events: timestamp */
        }
        break;
    case CP_INTERRUPT:
        if (n >= 1) {
            s->regs[REG_CP_INT_STATUS] |= d[0];
            adreno_update_irq(s);
        }
        break;
    case CP_COND_EXEC:
        /* execute the next d[3] dwords if *a0 != 0 && *a1 <= ref */
        if (n >= 4) {
            uint32_t a0 = adreno_rd32(s, d[0] << 2);
            uint32_t a1 = adreno_rd32(s, d[1] << 2);
            if (!(a0 != 0 && (int32_t)(a1 - d[2]) <= 0)) {
                *skip = d[3];
            }
        }
        break;
    case CP_COND_WRITE:
        /* d0: function | mem space; d1 poll; d2 ref; d3 mask; d4 dst; d5 data */
        if (n >= 6) {
            uint32_t v = (d[0] & 0x10) ? adreno_rd32(s, d[1])
                         : (d[1] < ADRENO_NREGS ? s->regs[d[1]] : 0);
            uint32_t ref = d[2], m = d[3];
            bool ok;
            v &= m;
            switch (d[0] & 7) {
            case 0: ok = true; break;
            case 1: ok = v < ref; break;
            case 2: ok = v <= ref; break;
            case 3: ok = v == ref; break;
            case 4: ok = v != ref; break;
            case 5: ok = v >= ref; break;
            case 6: ok = v > ref; break;
            default: ok = false; break;
            }
            if (ok) {
                if (d[0] & 0x100) {
                    adreno_wr32(s, d[4], d[5]);
                } else {
                    gpu_reg_write(s, d[4] & 0x7fff, d[5]);
                }
            }
        }
        break;
    case CP_SET_CONSTANT:
        if (n >= 2) {
            cp_set_constant(s, d, n);
        }
        break;
    case CP_LOAD_CONSTANT_CONTEXT:
        if (n >= 2) {
            cp_load_constant_context(s, d, n);
        }
        break;
    case CP_IM_LOAD:
        /* d0: address | type, d1: start << 16 | size */
        if (n >= 2) {
            cp_im_load(s, d[0] & 3, d[1] >> 16, d[1] & 0xffff, NULL,
                       d[0] & ~3u);
        }
        break;
    case CP_IM_LOAD_IMMEDIATE:
        if (n >= 2) {
            uint32_t size = MIN(d[1] & 0xffff, (n - 2) / 3);
            cp_im_load(s, d[0] & 3, d[1] >> 16, size, d + 2, 0);
        }
        break;
    case CP_IM_STORE:
        /*
         * d0: address | type; store the type's program there and its
         * start << 16 | size at d1 (for a later IM_LOAD).
         */
        if (n >= 2) {
            uint32_t t = d[0] & 3;
            uint32_t st = t <= 2 ? s->im_start[t] : 0;
            uint32_t sz = t <= 2 ? s->im_size[t] : 0;
            uint32_t at = im_where(s, t, st) * 3, dw = sz * 3;
            for (uint32_t i = 0; i < dw && at + i < ADRENO_ISTORE_DWORDS; i++) {
                adreno_wr32(s, (d[0] & ~3u) + i * 4, s->istore[at + i]);
            }
            adreno_wr32(s, d[1], (st << 16) | sz);
        }
        break;
    case CP_SET_SHADER_BASES:
        if (n >= 1) {
            gpu_reg_write(s, ADRENO_REG_INST_STORE, d[0]);
        }
        break;
    case CP_SET_BIN_MASK:
    case CP_SET_BIN_MASK_LO:
    case CP_SET_BIN_SELECT:
    case CP_SET_BIN_SELECT_LO:
        if (n >= 1) {
            if (op == CP_SET_BIN_MASK || op == CP_SET_BIN_MASK_LO) {
                s->bin_mask = d[0];
            } else {
                s->bin_select = d[0];
            }
        }
        break;
    case CP_SET_BIN_MASK_HI:
    case CP_SET_BIN_SELECT_HI:
    case CP_SET_DRAW_INIT_FLAGS:
    case CP_VIZ_QUERY:
        break;
    case CP_DRAW_INDX:
    case CP_DRAW_INDX_2:
    case CP_DRAW_INDX_BIN:
    case CP_DRAW_INDX_2_BIN:
        adreno_draw(s, op, d, n);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "adreno: PM4 type3 op 0x%02x (%u dwords)\n",
                      op, n);
        break;
    }
}

static void cp_exec(AdrenoState *s, const uint32_t *cmd, uint32_t n, int level)
{
    uint32_t i = 0;

    while (i < n) {
        uint32_t hdr = cmd[i++];
        uint32_t skip = 0;

        switch (hdr >> 30) {
        case 0: {
            uint32_t reg = hdr & 0x7fff, cnt = ((hdr >> 16) & 0x3fff) + 1;
            bool one = hdr & 0x8000;
            for (uint32_t k = 0; k < cnt && i < n; k++, i++) {
                gpu_reg_write(s, one ? reg : reg + k, cmd[i]);
            }
            break;
        }
        case 1:
            if (i + 2 <= n) {
                gpu_reg_write(s, hdr & 0x7ff, cmd[i]);
                gpu_reg_write(s, (hdr >> 11) & 0x7ff, cmd[i + 1]);
            }
            i += 2;
            break;
        case 2:
            break;
        case 3: {
            uint32_t op = (hdr >> 8) & 0xff, cnt = ((hdr >> 16) & 0x3fff) + 1;
            if (i + cnt > n) {
                cnt = n - i;
            }
            if (adreno_trace > 1) {
                qemu_log("adreno: L%d op %02x n %u:", level, op, cnt);
                for (uint32_t k = 0; k < MIN(cnt, 8u); k++) {
                    qemu_log(" %08x", cmd[i + k]);
                }
                qemu_log("\n");
            }
            cp_type3(s, op, &cmd[i], cnt, level, &skip);
            i += cnt;
            i += skip;
            break;
        }
        }
    }
}

/* Run the ring from the read to the write pointer. */
static void cp_run(AdrenoState *s)
{
    uint32_t base = s->regs[REG_CP_RB_BASE];
    uint32_t size = 2u << (s->regs[REG_CP_RB_CNTL] & 0x3f);   /* dwords */
    uint32_t mask = size - 1;
    /* pairs with the release in adreno_mmio_write: ring contents first */
    uint32_t wptr = qatomic_load_acquire(&s->regs[REG_CP_RB_WPTR]) & mask;
    uint32_t rptr = s->regs[REG_CP_RB_RPTR] & mask;

    if (s->regs[REG_CP_ME_CNTL] & 0x10000000 || size > (1u << 22)) {
        return;                         /* micro engine halted */
    }
    if (rptr != wptr) {
        uint32_t len = (wptr - rptr) & mask;
        g_autofree uint32_t *buf = g_new(uint32_t, len);
        for (uint32_t i = 0; i < len; i++) {
            buf[i] = adreno_rd32(s, base + ((rptr + i) & mask) * 4);
        }
        /*
         * Like the CP, only take whole packets: KGSL publishes the header
         * of the NOP that pads the ring up to its end before the wrapped
         * commands, and the NOP body must not be parsed as packets.
         */
        uint32_t whole = 0;
        while (whole < len) {
            uint32_t hdr = buf[whole], sz;
            switch (hdr >> 30) {
            case 0: sz = 1 + ((hdr >> 16) & 0x3fff) + 1; break;
            case 1: sz = 3; break;
            case 2: sz = 1; break;
            default: sz = 1 + ((hdr >> 16) & 0x3fff) + 1; break;
            }
            if (whole + sz > len) {
                break;
            }
            whole += sz;
        }
        if (!whole) {
            return;                     /* wait for the rest of the packet */
        }
        len = whole;
        wptr = (rptr + whole) & mask;
        static int ringlog = -1;
        if (ringlog < 0) {
            ringlog = getenv("ADRENO_RINGLOG") != NULL;
        }
        if (ringlog) {
            qemu_log("adreno: ring %04x-%04x:", rptr, wptr);
            for (uint32_t i = 0; i < len; i++) {
                qemu_log(" %08x", buf[i]);
            }
            qemu_log("\n");
        }
        cp_exec(s, buf, len, 0);
        /* consumed: publish the read pointer once the work is done */
        qatomic_store_release(&s->regs[REG_CP_RB_RPTR], wptr);
        if (ringlog) {
            qemu_log("adreno: ring done ts %08x\n", s->regs[REG_SCRATCH_REG0]);
        }
    }
    if (!(s->regs[REG_CP_RB_CNTL] & RB_CNTL_NO_UPDATE) &&
        s->regs[REG_CP_RB_RPTR_ADDR]) {
        adreno_wr32(s, s->regs[REG_CP_RB_RPTR_ADDR] & ~3u,
                    s->regs[REG_CP_RB_RPTR]);
    }
}

/* ---------------- MMIO ---------------- */

static uint64_t adreno_mmio_read_locked(AdrenoState *s, uint32_t r);

static uint64_t adreno_mmio_read(void *opaque, hwaddr off, unsigned size)
{
    AdrenoState *s = opaque;
    uint32_t r = off >> 2;
    uint64_t v;

    switch (r) {
    case REG_RBBM_STATUS:
        /* GUI_ACTIVE while the command processor has work */
        return 0x110 | (qatomic_read(&s->regs[REG_CP_RB_RPTR]) !=
                        qatomic_read(&s->regs[REG_CP_RB_WPTR]) ? 0x80000000u : 0);
    case REG_CP_RB_RPTR:
    case REG_CP_RB_WPTR:
    case REG_SCRATCH_REG0 ... REG_SCRATCH_REG0 + 7:
        /* polled: never wait; acquire pairs with the GPU thread's release */
        return qatomic_load_acquire(&s->regs[r]);
    }
    qemu_mutex_lock(&s->lock);
    v = adreno_mmio_read_locked(s, r);
    qemu_mutex_unlock(&s->lock);
    return v;
}

static uint64_t adreno_mmio_read_locked(AdrenoState *s, uint32_t r)
{
    switch (r) {
    case REG_RBBM_PERIPHID1:
        return 0x00;                    /* core 0 */
    case REG_RBBM_PERIPHID2:
        return 0x20;                    /* major 2: Adreno 200 */
    case REG_RBBM_PATCH_RELEASE:
        return 0x00010001;              /* Yamato DX 2.1.1 (QSD8x50) */
    case REG_CP_ME_STATUS:
    case REG_CP_STAT:
        return 0;
    default:
        return r < ADRENO_NREGS ? s->regs[r] : 0;
    }
}

static void adreno_mmio_write_locked(AdrenoState *s, uint32_t r,
                                     uint64_t val);

static void adreno_mmio_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    AdrenoState *s = opaque;
    uint32_t r = off >> 2;

    if (r >= ADRENO_NREGS) {
        return;
    }
    if (r == REG_CP_RB_WPTR) {
        /*
         * New commands: hand them to the GPU thread, do not wait.  The
         * guest's ring and IB stores are plain host stores (single-threaded
         * TCG drops guest barriers), so order them before the new write
         * pointer here; without it an ARM64 host lets the GPU thread see
         * the pointer before the commands and replay stale ones.
         */
        smp_mb();
        qatomic_store_release(&s->regs[r], (uint32_t)val);
        qemu_event_set(&s->kick);
        return;
    }
    qemu_mutex_lock(&s->lock);
    adreno_mmio_write_locked(s, r, val);
    qemu_mutex_unlock(&s->lock);
}

static void adreno_mmio_write_locked(AdrenoState *s, uint32_t r, uint64_t val)
{
    static int ringlog = -1;
    if (ringlog < 0) {
        ringlog = getenv("ADRENO_RINGLOG") != NULL;
    }
    if (ringlog && (r == REG_CP_RB_BASE || r == REG_CP_RB_CNTL ||
        r == REG_CP_ME_CNTL || r == REG_CP_RB_WPTR || r == REG_CP_RB_RPTR ||
        (r >= 0x1c0 && r < 0x1d0) || r == 0x3b /* RBBM_CNTL */ ||
        r == 0x3c /* SOFT_RESET */)) {
        qemu_log("adreno: mmio w %03x = %08x (rptr %x wptr %x me %x)\n", r,
                 (uint32_t)val, s->regs[REG_CP_RB_RPTR], s->regs[REG_CP_RB_WPTR],
                 s->regs[REG_CP_ME_CNTL]);
    }
    switch (r) {
    case REG_CP_INT_ACK:
        s->regs[REG_CP_INT_STATUS] &= ~val;
        adreno_update_irq(s);
        return;
    case REG_RBBM_INT_ACK:
        s->regs[REG_RBBM_INT_STATUS] &= ~val;
        adreno_update_irq(s);
        return;
    case REG_MH_INTERRUPT_CLEAR:
        s->regs[REG_MH_INTERRUPT_STATUS] &= ~val;
        adreno_update_irq(s);
        return;
    case REG_SQ_INT_ACK:
        s->regs[REG_SQ_INT_STATUS] &= ~val;
        adreno_update_irq(s);
        return;
    case REG_CP_INT_CNTL:
    case REG_RBBM_INT_CNTL:
    case REG_MH_INTERRUPT_MASK:
    case REG_SQ_INT_CNTL:
        s->regs[r] = val;
        adreno_update_irq(s);
        return;
    case REG_MH_MMU_INVALIDATE:
    case REG_MH_MMU_PT_BASE:
    case REG_MH_MMU_VA_RANGE:
    case REG_MH_MMU_CONFIG:
        s->regs[r] = val;
        adreno_tlb_flush(s);
        s->fault_reported = false;
        return;
    case REG_CP_ME_RAM_DATA:
        return;                         /* PM4 microcode: emulated in C */
    case REG_CP_RB_BASE:
    case REG_CP_RB_CNTL:
        s->regs[r] = val;
        s->regs[REG_CP_RB_RPTR] = 0;
        return;
    case REG_CP_ME_CNTL:
        s->regs[r] = val;
        if (!(val & 0x10000000)) {
            qemu_event_set(&s->kick);
        }
        return;
    default:
        gpu_reg_write(s, r, val);
        return;
    }
}

static const MemoryRegionOps adreno_ops = {
    .read = adreno_mmio_read,
    .write = adreno_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void *adreno_thread(void *opaque)
{
    AdrenoState *s = opaque;

    rcu_register_thread();
    for (;;) {
        qemu_event_reset(&s->kick);
        if (qatomic_read(&s->quit)) {
            break;
        }
        qemu_mutex_lock(&s->lock);
        cp_run(s);
        bool idle = qatomic_read(&s->regs[REG_CP_RB_RPTR]) ==
                    (qatomic_read(&s->regs[REG_CP_RB_WPTR]) &
                     ((2u << (s->regs[REG_CP_RB_CNTL] & 0x3f)) - 1)) ||
                    (s->regs[REG_CP_ME_CNTL] & 0x10000000);
        qemu_mutex_unlock(&s->lock);
        if (idle) {
            qemu_event_wait(&s->kick);
        }
    }
    rcu_unregister_thread();
    return NULL;
}

static void adreno_reset(DeviceState *dev)
{
    AdrenoState *s = ADRENO(dev);

    qemu_mutex_lock(&s->lock);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->istore, 0, sizeof(s->istore));
    memset(s->shadow_en, 0, sizeof(s->shadow_en));
    adreno_tlb_flush(s);
    s->regs[REG_CP_ME_CNTL] = 0x10000000;   /* halted until KGSL starts it */
    adreno_draw_reset(s);
    adreno_irq_signal(s);
    qemu_mutex_unlock(&s->lock);
    qemu_set_irq(s->irq, 0);
}

static void adreno_realize(DeviceState *dev, Error **errp)
{
    AdrenoState *s = ADRENO(dev);

    qemu_mutex_init(&s->lock);
    qemu_event_init(&s->kick, false);
    s->irq_bh = qemu_bh_new(adreno_irq_bh, s);
    qemu_thread_create(&s->thread, "adreno-cp", adreno_thread, s,
                       QEMU_THREAD_JOINABLE);
}

static void adreno_init(Object *obj)
{
    AdrenoState *s = ADRENO(obj);

    memory_region_init_io(&s->iomem, obj, &adreno_ops, s, "adreno",
                          ADRENO_NREGS * 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    adreno_trace = getenv("ADRENO_TRACE") != NULL;
    adreno_trace_from = adreno_trace ? atol(getenv("ADRENO_TRACE")) : 0;
    if (getenv("ADRENO_TRACE_LEN")) {
        adreno_trace_len = atol(getenv("ADRENO_TRACE_LEN"));
    }
    if (getenv("ADRENO_WATCH")) {
        adreno_watch = strtoull(getenv("ADRENO_WATCH"), NULL, 0);
    }
}

static void adreno_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, adreno_reset);
    dc->realize = adreno_realize;
    dc->desc = "Adreno 200 GPU";
}

static const TypeInfo adreno_info = {
    .name = TYPE_ADRENO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AdrenoState),
    .instance_init = adreno_init,
    .class_init = adreno_class_init,
};

static void adreno_register_types(void)
{
    type_register_static(&adreno_info);
}

type_init(adreno_register_types)
