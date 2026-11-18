/*
 * Adreno 200 (a2xx) GPU model, see adreno.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_HTCLEO_ADRENO_H
#define HW_ARM_HTCLEO_ADRENO_H

#include "hw/sysbus.h"
#include "qom/object.h"
#include "qemu/thread.h"

#define TYPE_ADRENO "msm-adreno"
OBJECT_DECLARE_SIMPLE_TYPE(AdrenoState, ADRENO)

#define ADRENO_NREGS            0x8000          /* 128 KiB register window */
#define ADRENO_ISTORE_DWORDS    (4096 * 3)      /* shader instruction store */
#define ADRENO_ALU_CONSTS       512             /* float4 constants */
#define ADRENO_FETCH_CONSTS     (32 * 6)        /* 32 x 6-dword fetch consts */
#define ADRENO_GMEM_SIZE        (256 * 1024)

/* MMU translation and host page pointers cached by one thread */
#define ADRENO_HP_ENTRIES 256
typedef struct AdrenoMemCache {
    uint32_t gen;
    bool tlb_valid;
    uint32_t tlb_idx, tlb_pte;
    uint32_t hp_tag[ADRENO_HP_ENTRIES];     /* page + 1, 0 = empty */
    uint8_t *hp_ptr[ADRENO_HP_ENTRIES];
} AdrenoMemCache;

struct AdrenoState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[ADRENO_NREGS];

    bool fault_reported;
    /*
     * Cached GPU translations: the GPU thread's below, the draw workers'
     * in adreno_draw.c.  Bumping mc_gen (MMU or page table change)
     * invalidates all of them.
     */
    uint32_t mc_gen;
    AdrenoMemCache mc;

    /*
     * Shader instruction store; constants live in the register file
     * (ALU 0x4000, fetch 0x4800, bool 0x4900, loop 0x4908) like on the
     * chip, where KGSL reads them back with REG_TO_MEM.
     */
    uint32_t istore[ADRENO_ISTORE_DWORDS];
    uint32_t im_start[3], im_size[3];       /* last IM_LOAD per type */
    uint32_t bin_mask, bin_select;

    /* LOAD_CONSTANT_CONTEXT register shadowing, per constant type */
    uint32_t shadow_base[5];
    bool shadow_en[5];

    /* on-chip tile memory the RB renders into */
    uint8_t gmem[ADRENO_GMEM_SIZE];

    /* statistics */
    uint64_t draws;

    /*
     * The command processor runs in its own thread, like the GPU next to
     * the CPU: WPTR writes only wake it.  "lock" guards the register file
     * and all GPU state; the thread never takes the BQL (interrupts are
     * raised from a bottom half).
     */
    QemuThread thread;
    QemuMutex lock;
    QemuEvent kick;
    QEMUBH *irq_bh;
    bool quit;
    bool paused;                /* the VM is stopped: the GPU stops too */
    /*
     * The GPU thread holds the lock while it runs the ring, but lets
     * waiting CPU accesses (IRQ status/ack...) in between packets: in_run
     * marks a ring pass in progress, mmio_waiting counts CPU accesses
     * waiting for the lock, run_done is signalled at the end of a pass.
     */
    bool in_run;
    int mmio_waiting;
    QemuCond run_done;
};

#define ADRENO_REG_ALU          0x4000
#define ADRENO_REG_FETCH        0x4800
#define ADRENO_REG_BOOL         0x4900
#define ADRENO_REG_LOOP         0x4908
#define ADRENO_REG_INST_STORE   0x0d02          /* SQ_INST_STORE_MANAGMENT */
#define ADRENO_REG_PS_PROGRAM   0x21f6
#define ADRENO_REG_VS_PROGRAM   0x21f7

/* first instruction of the current vertex (0) / pixel (1) program */
static inline uint32_t adreno_prog_base(AdrenoState *s, int type)
{
    uint32_t part = s->regs[ADRENO_REG_INST_STORE];
    uint32_t prog = s->regs[type ? ADRENO_REG_PS_PROGRAM : ADRENO_REG_VS_PROGRAM];
    return (type ? (part & 0xfff) : ((part >> 16) & 0xfff)) + (prog & 0xfff);
}

extern int adreno_trace;
extern long adreno_trace_from;
extern long adreno_trace_len;

/*
 * GPU memory access.  The _c variants take the calling thread's cache
 * (draw workers); the plain ones use the GPU thread's.
 */
bool adreno_xlate_c(AdrenoState *s, AdrenoMemCache *c, uint32_t va, hwaddr *pa);
void adreno_read_c(AdrenoState *s, AdrenoMemCache *c, uint32_t va, void *buf,
                   uint32_t len);
void adreno_write_c(AdrenoState *s, AdrenoMemCache *c, uint32_t va,
                    const void *buf, uint32_t len);
uint8_t *adreno_host_page_slow(AdrenoState *s, AdrenoMemCache *c, uint32_t va);
uint32_t adreno_rd32(AdrenoState *s, uint32_t va);
void adreno_wr32(AdrenoState *s, uint32_t va, uint32_t val);
void adreno_mark_dirty(AdrenoState *s, uint32_t va, uint32_t len);
void adreno_watch_draw(AdrenoState *s, uint32_t va, uint32_t len, const char *what);

static inline bool adreno_xlate(AdrenoState *s, uint32_t va, hwaddr *pa)
{
    return adreno_xlate_c(s, &s->mc, va, pa);
}

static inline void adreno_read(AdrenoState *s, uint32_t va, void *buf, uint32_t len)
{
    adreno_read_c(s, &s->mc, va, buf, len);
}

static inline void adreno_write(AdrenoState *s, uint32_t va, const void *buf,
                                uint32_t len)
{
    adreno_write_c(s, &s->mc, va, buf, len);
}

/* host pointer to the RAM page holding GPU address va, NULL if not RAM */
static inline uint8_t *adreno_host_page_c(AdrenoState *s, AdrenoMemCache *c,
                                          uint32_t va)
{
    uint32_t pg = va >> 12, i = pg & (ADRENO_HP_ENTRIES - 1);

    if (c->hp_tag[i] == pg + 1 && c->gen == s->mc_gen) {
        return c->hp_ptr[i];
    }
    return adreno_host_page_slow(s, c, va);
}

static inline uint8_t *adreno_host_page(AdrenoState *s, uint32_t va)
{
    return adreno_host_page_c(s, &s->mc, va);
}

/* adreno_draw.c */
void adreno_draw_reset(AdrenoState *s);
void adreno_draw_post_load(AdrenoState *s);
void adreno_reg_written(AdrenoState *s, uint32_t reg, uint32_t val);
void adreno_event(AdrenoState *s, uint32_t event);
void adreno_draw(AdrenoState *s, uint32_t op, const uint32_t *d, uint32_t n);

#endif
