/*
 * Qualcomm MSM CSR block (0xAC100000 on QSD8x50).
 *
 *  0x000  GPT  (general purpose timer, 32768 Hz)
 *  0x010  DGT  (debug timer, 19.2 MHz / (DGT_CLK_CTL + 1))
 *  0x020  DGT clock divider (htcleo writes 3 -> 4.8 MHz)
 *  0x400  A2M_INT(n): apps -> modem doorbells, write 1 to ring
 *
 * Timer register layout per timer (arch/arm/mach-msm/timer.c):
 *  +0 MATCH_VAL, +4 COUNT_VAL, +8 ENABLE (bit0 EN, bit1 CLR_ON_MATCH),
 *  +C CLEAR (write: count = 0)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "msm.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmCsrState, MSM_CSR)

#define TIMER_MATCH_VAL     0x0
#define TIMER_COUNT_VAL     0x4
#define TIMER_ENABLE        0x8
#define TIMER_CLEAR         0xC
#define TIMER_ENABLE_EN             1
#define TIMER_ENABLE_CLR_ON_MATCH   2

#define CSR_DGT_CLK_CTL     0x020
#define CSR_A2M_BASE        0x400
#define CSR_SPSS_CLK_CNTL   0x100
#define CSR_SPSS_CLK_SEL    0x104

#define GPT_HZ              32768
#define TCXO_HZ             19200000

typedef struct MsmTimer {
    struct MsmCsrState *csr;
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t match;
    uint32_t enable;
    uint32_t count_base;   /* counter value at time 'ns_base' */
    int64_t ns_base;
    uint32_t freq;
} MsmTimer;

struct MsmCsrState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MsmTimer t[2];
    uint32_t dgt_div;
    qemu_irq a2m[MSM_A2M_NUM];
    uint32_t regs[0x1000 / 4];   /* backing store for everything else */
};

static MsmCsrState *csr_instance;

static uint32_t mt_count(MsmTimer *t)
{
    if (!(t->enable & TIMER_ENABLE_EN)) {
        return t->count_base;
    }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t ticks = muldiv64(now - t->ns_base, t->freq, NANOSECONDS_PER_SECOND);
    return t->count_base + (uint32_t)ticks;
}

/* The modem's view of the sleep clock (same 32 kHz source as the GPT). */
uint32_t msm_csr_gpt_count(void)
{
    return csr_instance ? mt_count(&csr_instance->t[0]) : 0;
}

/* Freeze the counter at its current value and re-base the time origin. */
static void mt_rebase(MsmTimer *t)
{
    t->count_base = mt_count(t);
    t->ns_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void mt_rearm(MsmTimer *t)
{
    if (!(t->enable & TIMER_ENABLE_EN)) {
        timer_del(t->timer);
        return;
    }
    uint32_t now = mt_count(t);
    uint32_t delta = t->match - now;
    if (delta == 0) {
        delta = 1;
    }
    /* compute the absolute expiry time from the base so we do not drift */
    uint64_t target_ticks = (uint64_t)(now - t->count_base) + delta;
    int64_t when = t->ns_base +
        muldiv64(target_ticks, NANOSECONDS_PER_SECOND, t->freq);
    /* round up so that the counter really reached match when we fire */
    when += 1;
    timer_mod(t->timer, when);
}

static void mt_expired(void *opaque)
{
    MsmTimer *t = opaque;

    if (!(t->enable & TIMER_ENABLE_EN)) {
        return;
    }
    if (t->enable & TIMER_ENABLE_CLR_ON_MATCH) {
        t->count_base = 0;
        t->ns_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    qemu_irq_pulse(t->irq);
    mt_rearm(t);
}

static void mt_set_freq(MsmTimer *t, uint32_t freq)
{
    mt_rebase(t);
    t->freq = freq;
    mt_rearm(t);
}

static uint64_t mt_read(MsmTimer *t, hwaddr off)
{
    switch (off) {
    case TIMER_MATCH_VAL:
        return t->match;
    case TIMER_COUNT_VAL:
        return mt_count(t);
    case TIMER_ENABLE:
        return t->enable;
    default:
        return 0;
    }
}

static void mt_write(MsmTimer *t, hwaddr off, uint32_t val)
{
    switch (off) {
    case TIMER_MATCH_VAL:
        t->match = val;
        mt_rearm(t);
        break;
    case TIMER_COUNT_VAL:
        t->count_base = val;
        t->ns_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        mt_rearm(t);
        break;
    case TIMER_ENABLE:
        mt_rebase(t);
        t->enable = val & 3;
        t->ns_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        mt_rearm(t);
        break;
    case TIMER_CLEAR:
        t->count_base = 0;
        t->ns_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        mt_rearm(t);
        break;
    }
}

static uint64_t csr_read(void *opaque, hwaddr off, unsigned size)
{
    MsmCsrState *s = opaque;

    if (off < 0x20) {
        return mt_read(&s->t[off >> 4], off & 0xf);
    }
    if (off == CSR_DGT_CLK_CTL) {
        return s->dgt_div;
    }
    return s->regs[off >> 2];
}

static void csr_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmCsrState *s = opaque;

    if (off < 0x20) {
        mt_write(&s->t[off >> 4], off & 0xf, val);
        return;
    }
    if (off == CSR_DGT_CLK_CTL) {
        s->dgt_div = val & 3;
        mt_set_freq(&s->t[1], TCXO_HZ / (s->dgt_div + 1));
        return;
    }
    if (off >= CSR_A2M_BASE && off < CSR_A2M_BASE + 4 * MSM_A2M_NUM) {
        int n = (off - CSR_A2M_BASE) >> 2;
        if (val & 1) {
            qemu_irq_pulse(s->a2m[n]);
        }
        return;
    }
    qemu_log_mask(LOG_UNIMP, "msm-csr: write 0x%08x to reg 0x%03x\n",
                  (unsigned)val, (unsigned)off);
    s->regs[off >> 2] = val;
}

static const MemoryRegionOps csr_ops = {
    .read = csr_read,
    .write = csr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void csr_reset(DeviceState *dev)
{
    MsmCsrState *s = MSM_CSR(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* bootloader leaves the Scorpion clocked from SCPLL (src 1) */
    s->regs[CSR_SPSS_CLK_SEL >> 2] = 1 << 1;
    for (int i = 0; i < 2; i++) {
        MsmTimer *t = &s->t[i];
        timer_del(t->timer);
        t->match = 0;
        t->enable = 0;
        t->count_base = 0;
        t->ns_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    s->t[0].freq = GPT_HZ;
    s->dgt_div = 3;
    s->t[1].freq = TCXO_HZ / 4;
}

static void csr_init(Object *obj)
{
    MsmCsrState *s = MSM_CSR(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    csr_instance = s;
    memory_region_init_io(&s->iomem, obj, &csr_ops, s, "msm-csr", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    for (int i = 0; i < 2; i++) {
        s->t[i].csr = s;
        s->t[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mt_expired, &s->t[i]);
        sysbus_init_irq(sbd, &s->t[i].irq);
    }
    qdev_init_gpio_out_named(DEVICE(obj), s->a2m, "a2m", MSM_A2M_NUM);
}

static const VMStateDescription vmstate_msm_timer = {
    .name = "msm-timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, MsmTimer),
        VMSTATE_UINT32(match, MsmTimer),
        VMSTATE_UINT32(enable, MsmTimer),
        VMSTATE_UINT32(count_base, MsmTimer),
        VMSTATE_INT64(ns_base, MsmTimer),
        VMSTATE_UINT32(freq, MsmTimer),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_msm_csr = {
    .name = "msm-csr",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(t, MsmCsrState, 2, 1, vmstate_msm_timer, MsmTimer),
        VMSTATE_UINT32(dgt_div, MsmCsrState),
        VMSTATE_UINT32_ARRAY(regs, MsmCsrState, 0x1000 / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void csr_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, csr_reset);
    dc->vmsd = &vmstate_msm_csr;
}

static const TypeInfo csr_info = {
    .name = TYPE_MSM_CSR,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmCsrState),
    .instance_init = csr_init,
    .class_init = csr_class_init,
};

static void csr_register_types(void)
{
    type_register_static(&csr_info);
}

type_init(csr_register_types)
