/*
 * Qualcomm MSM (QSD8x50 "Scorpion" flavour) vectored interrupt controller.
 *
 * Register layout from arch/arm/mach-msm/irq.c (CONFIG_ARCH_MSM_SCORPION).
 * 64 interrupt sources in two 32-bit banks.  Each source can be level or
 * edge triggered and routed to IRQ or FIQ.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "msm.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmVicState, MSM_VIC)

#define VIC_INT_SELECT0     0x000   /* 1: FIQ, 0: IRQ */
#define VIC_INT_EN0         0x010
#define VIC_INT_ENCLEAR0    0x020
#define VIC_INT_ENSET0      0x030
#define VIC_INT_TYPE0       0x040   /* 1: EDGE, 0: LEVEL */
#define VIC_INT_POLARITY0   0x050   /* 1: NEG, 0: POS */
#define VIC_NO_PEND_VAL     0x060
#define VIC_NO_PEND_VAL_FIQ 0x064
#define VIC_INT_MASTEREN    0x068   /* 1: IRQ, 2: FIQ */
#define VIC_CONFIG          0x06C
#define VIC_IRQ_STATUS0     0x080
#define VIC_FIQ_STATUS0     0x090
#define VIC_RAW_STATUS0     0x0A0
#define VIC_INT_CLEAR0      0x0B0
#define VIC_SOFTINT0        0x0C0
#define VIC_IRQ_VEC_RD      0x0D0
#define VIC_IRQ_VEC_PEND_RD 0x0D4
#define VIC_IRQ_VEC_WR      0x0D8
#define VIC_FIQ_VEC_RD      0x0DC
#define VIC_FIQ_VEC_PEND_RD 0x0E0
#define VIC_FIQ_VEC_WR      0x0E4
#define VIC_IRQ_IN_SERVICE  0x0E8
#define VIC_IRQ_IN_STACK    0x0EC
#define VIC_FIQ_IN_SERVICE  0x0F0
#define VIC_FIQ_IN_STACK    0x0F4
#define VIC_TEST_BUS_SEL    0x0F8
#define VIC_IRQ_CTRL_CONFIG 0x0FC
#define VIC_VECTPRIORITY0   0x200
#define VIC_VECTADDR0       0x400

struct MsmVicState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq fiq;
    qemu_irq wake_req;      /* to the fake modem: apps needs waking */

    uint32_t select[2];
    uint32_t enable[2];
    uint32_t type[2];
    uint32_t polarity[2];
    uint32_t level[2];      /* current input line levels */
    uint32_t latched[2];    /* latched edge events */
    uint32_t softint[2];
    uint32_t no_pend_val;
    uint32_t no_pend_val_fiq;
    uint32_t masteren;
    uint32_t config;
    uint32_t last_irq;
    uint32_t vectaddr[MSM_VIC_NUM_IRQS];
    uint32_t vectprio[MSM_VIC_NUM_IRQS];
};

static uint32_t vic_raw(MsmVicState *s, int b)
{
    uint32_t lvl = s->level[b] ^ (s->polarity[b] & ~s->type[b]);
    return (lvl & ~s->type[b]) | (s->latched[b] & s->type[b]) | s->softint[b];
}

static uint32_t vic_pending(MsmVicState *s, int b)
{
    return vic_raw(s, b) & s->enable[b];
}

static int vic_first(MsmVicState *s, bool fiq)
{
    for (int b = 0; b < 2; b++) {
        uint32_t p = vic_pending(s, b) & (fiq ? s->select[b] : ~s->select[b]);
        if (p) {
            return b * 32 + ctz32(p);
        }
    }
    return -1;
}

static void vic_update(MsmVicState *s)
{
    bool irq = false, fiq = false;
    for (int b = 0; b < 2; b++) {
        uint32_t p = vic_pending(s, b);
        irq |= (p & ~s->select[b]) != 0;
        fiq |= (p & s->select[b]) != 0;
    }
    qemu_set_irq(s->irq, irq && (s->masteren & 1));
    qemu_set_irq(s->fiq, fiq && (s->masteren & 2));
}

/*
 * In deep sleep HD2 kernels mask everything but INT_A9_M2A_6 and let the
 * modem watch the wake-up sources.  Tell our fake modem when one of them
 * fires while the apps CPU waits like that.
 */
static bool vic_apps_in_modem_sleep(MsmVicState *s)
{
    return s->enable[0] == (1u << INT_A9_M2A_6) && s->enable[1] == 0;
}

static bool vic_is_wake_source(int n)
{
    return n == INT_SIRC_0 || n == INT_SIRC_1 || n == INT_GP_TIMER_EXP ||
           n == INT_DEBUG_TIMER_EXP || n == INT_SDC2_0;
}

static void vic_set_irq(void *opaque, int n, int level)
{
    MsmVicState *s = opaque;
    int b = n >> 5;
    uint32_t m = 1u << (n & 31);
    bool old = s->level[b] & m;

    if (level && !old && vic_is_wake_source(n) && vic_apps_in_modem_sleep(s)) {
        qemu_irq_pulse(s->wake_req);
    }

    if (level) {
        s->level[b] |= m;
    } else {
        s->level[b] &= ~m;
    }
    /* edge detection: rising edge (or falling edge if polarity is NEG) */
    if (s->type[b] & m) {
        bool neg = s->polarity[b] & m;
        if ((!neg && !old && level) || (neg && old && !level)) {
            s->latched[b] |= m;
        }
    }
    vic_update(s);
}

/* LEO_IRQSTATS=1: interrupts taken per source, every 5 s (on stderr) */
static void vic_count(int n)
{
    static int on = -1;
    static unsigned cnt[MSM_VIC_NUM_IRQS];
    static int64_t last;
    int64_t now;

    if (on < 0) {
        on = getenv("LEO_IRQSTATS") != NULL;
    }
    if (!on || n < 0 || n >= MSM_VIC_NUM_IRQS) {
        return;
    }
    cnt[n]++;
    now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (now - last >= 5000) {
        fprintf(stderr, "irqstats:");
        for (int i = 0; i < MSM_VIC_NUM_IRQS; i++) {
            if (cnt[i]) {
                fprintf(stderr, " %d:%u", i, cnt[i]);
            }
        }
        fprintf(stderr, "\n");
        memset(cnt, 0, sizeof(cnt));
        last = now;
    }
}

static uint64_t vic_read(void *opaque, hwaddr off, unsigned size)
{
    MsmVicState *s = opaque;
    int b = (off >> 2) & 1;
    int n;

    if (off >= VIC_VECTADDR0 && off < VIC_VECTADDR0 + 4 * MSM_VIC_NUM_IRQS) {
        return s->vectaddr[(off - VIC_VECTADDR0) >> 2];
    }
    if (off >= VIC_VECTPRIORITY0 &&
        off < VIC_VECTPRIORITY0 + 4 * MSM_VIC_NUM_IRQS) {
        return s->vectprio[(off - VIC_VECTPRIORITY0) >> 2];
    }

    switch (off) {
    case VIC_INT_SELECT0: case VIC_INT_SELECT0 + 4:
        return s->select[b];
    case VIC_INT_EN0: case VIC_INT_EN0 + 4:
    case VIC_INT_ENSET0: case VIC_INT_ENSET0 + 4:
        return s->enable[b];
    case VIC_INT_TYPE0: case VIC_INT_TYPE0 + 4:
        return s->type[b];
    case VIC_INT_POLARITY0: case VIC_INT_POLARITY0 + 4:
        return s->polarity[b];
    case VIC_NO_PEND_VAL:
        return s->no_pend_val;
    case VIC_NO_PEND_VAL_FIQ:
        return s->no_pend_val_fiq;
    case VIC_INT_MASTEREN:
        return s->masteren;
    case VIC_CONFIG:
        return s->config;
    case VIC_IRQ_STATUS0: case VIC_IRQ_STATUS0 + 4:
        return vic_pending(s, b) & ~s->select[b];
    case VIC_FIQ_STATUS0: case VIC_FIQ_STATUS0 + 4:
        return vic_pending(s, b) & s->select[b];
    case VIC_RAW_STATUS0: case VIC_RAW_STATUS0 + 4:
        return vic_raw(s, b);
    case VIC_SOFTINT0: case VIC_SOFTINT0 + 4:
        return s->softint[b];
    case VIC_IRQ_VEC_RD:
        /* returns the irq number of the interrupt being serviced */
        n = vic_first(s, false);
        if (n >= 0) {
            s->last_irq = n;
        }
        return s->last_irq;
    case VIC_IRQ_VEC_PEND_RD:
        n = vic_first(s, false);
        vic_count(n);
        return n >= 0 ? n : s->no_pend_val;
    case VIC_FIQ_VEC_RD:
    case VIC_FIQ_VEC_PEND_RD:
        n = vic_first(s, true);
        return n >= 0 ? n : s->no_pend_val_fiq;
    case VIC_IRQ_IN_SERVICE:
    case VIC_IRQ_IN_STACK:
    case VIC_FIQ_IN_SERVICE:
    case VIC_FIQ_IN_STACK:
    case VIC_TEST_BUS_SEL:
    case VIC_IRQ_CTRL_CONFIG:
    case VIC_IRQ_VEC_WR:
    case VIC_FIQ_VEC_WR:
        return 0;
    }
    qemu_log_mask(LOG_UNIMP, "msm-vic: read of unknown reg 0x%03x\n",
                  (unsigned)off);
    return 0;
}

static void vic_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmVicState *s = opaque;
    int b = (off >> 2) & 1;

    if (off >= VIC_VECTADDR0 && off < VIC_VECTADDR0 + 4 * MSM_VIC_NUM_IRQS) {
        s->vectaddr[(off - VIC_VECTADDR0) >> 2] = val;
        return;
    }
    if (off >= VIC_VECTPRIORITY0 &&
        off < VIC_VECTPRIORITY0 + 4 * MSM_VIC_NUM_IRQS) {
        s->vectprio[(off - VIC_VECTPRIORITY0) >> 2] = val;
        return;
    }

    switch (off) {
    case VIC_INT_SELECT0: case VIC_INT_SELECT0 + 4:
        s->select[b] = val;
        break;
    case VIC_INT_EN0: case VIC_INT_EN0 + 4:
        s->enable[b] = val;
        break;
    case VIC_INT_ENCLEAR0: case VIC_INT_ENCLEAR0 + 4:
        s->enable[b] &= ~val;
        break;
    case VIC_INT_ENSET0: case VIC_INT_ENSET0 + 4:
        s->enable[b] |= val;
        break;
    case VIC_INT_TYPE0: case VIC_INT_TYPE0 + 4:
        s->type[b] = val;
        break;
    case VIC_INT_POLARITY0: case VIC_INT_POLARITY0 + 4:
        s->polarity[b] = val;
        break;
    case VIC_NO_PEND_VAL:
        s->no_pend_val = val;
        break;
    case VIC_NO_PEND_VAL_FIQ:
        s->no_pend_val_fiq = val;
        break;
    case VIC_INT_MASTEREN:
        s->masteren = val;
        /* the kernel has just initialised its interrupt layer */
        leo_apply_kernel_quirks();
        break;
    case VIC_CONFIG:
        s->config = val;
        break;
    case VIC_INT_CLEAR0: case VIC_INT_CLEAR0 + 4:
        s->latched[b] &= ~val;
        s->softint[b] &= ~val;
        break;
    case VIC_SOFTINT0: case VIC_SOFTINT0 + 4:
        s->softint[b] |= val;
        break;
    case VIC_IRQ_VEC_WR:
    case VIC_FIQ_VEC_WR:
    case VIC_IRQ_IN_SERVICE:
    case VIC_IRQ_IN_STACK:
    case VIC_FIQ_IN_SERVICE:
    case VIC_FIQ_IN_STACK:
    case VIC_TEST_BUS_SEL:
    case VIC_IRQ_CTRL_CONFIG:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "msm-vic: write 0x%08x to unknown reg 0x%03x\n",
                      (unsigned)val, (unsigned)off);
        return;
    }
    vic_update(s);
}

static const MemoryRegionOps vic_ops = {
    .read = vic_read,
    .write = vic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void vic_reset(DeviceState *dev)
{
    MsmVicState *s = MSM_VIC(dev);
    uint32_t lvl0 = s->level[0], lvl1 = s->level[1];

    memset(&s->select, 0, sizeof(*s) - offsetof(MsmVicState, select));
    s->level[0] = lvl0;
    s->level[1] = lvl1;
    leo_reset_kernel_quirks();
    s->no_pend_val = 0xffffffff;
    s->no_pend_val_fiq = 0xffffffff;
    s->last_irq = 0xffffffff;
}

static void vic_init(Object *obj)
{
    MsmVicState *s = MSM_VIC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &vic_ops, s, "msm-vic", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->fiq);
    qdev_init_gpio_in(DEVICE(obj), vic_set_irq, MSM_VIC_NUM_IRQS);
    qdev_init_gpio_out_named(DEVICE(obj), &s->wake_req, "wake-req", 1);
}

static const VMStateDescription vmstate_msm_vic = {
    .name = "msm-vic",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(select, MsmVicState, 2),
        VMSTATE_UINT32_ARRAY(enable, MsmVicState, 2),
        VMSTATE_UINT32_ARRAY(type, MsmVicState, 2),
        VMSTATE_UINT32_ARRAY(polarity, MsmVicState, 2),
        VMSTATE_UINT32_ARRAY(level, MsmVicState, 2),
        VMSTATE_UINT32_ARRAY(latched, MsmVicState, 2),
        VMSTATE_UINT32_ARRAY(softint, MsmVicState, 2),
        VMSTATE_UINT32(no_pend_val, MsmVicState),
        VMSTATE_UINT32(no_pend_val_fiq, MsmVicState),
        VMSTATE_UINT32(masteren, MsmVicState),
        VMSTATE_UINT32(config, MsmVicState),
        VMSTATE_UINT32(last_irq, MsmVicState),
        VMSTATE_UINT32_ARRAY(vectaddr, MsmVicState, MSM_VIC_NUM_IRQS),
        VMSTATE_UINT32_ARRAY(vectprio, MsmVicState, MSM_VIC_NUM_IRQS),
        VMSTATE_END_OF_LIST()
    }
};

static void vic_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, vic_reset);
    dc->vmsd = &vmstate_msm_vic;
}

static const TypeInfo vic_info = {
    .name = TYPE_MSM_VIC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmVicState),
    .instance_init = vic_init,
    .class_init = vic_class_init,
};

static void vic_register_types(void)
{
    type_register_static(&vic_info);
}

type_init(vic_register_types)
