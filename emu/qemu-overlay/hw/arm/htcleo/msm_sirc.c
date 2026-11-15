/*
 * Qualcomm QSD8x50 secondary interrupt controller (SIRC, 0xAC200000).
 *
 * 23 sources (irqs 64..86 in Linux numbering), cascaded into the VIC as
 * INT_SIRC_0 (IRQ path) and INT_SIRC_1 (FIQ path).  arch/arm/mach-msm/sirc.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "msm.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmSircState, MSM_SIRC)

#define SIRC_MASK               0x007FFFFF
#define SIRC_INT_SELECT         0x00
#define SIRC_INT_ENABLE         0x04
#define SIRC_INT_ENABLE_CLEAR   0x08
#define SIRC_INT_ENABLE_SET     0x0C
#define SIRC_INT_TYPE           0x10
#define SIRC_INT_POLARITY       0x14
#define SIRC_SECURITY           0x18
#define SIRC_IRQ_STATUS         0x1C
#define SIRC_IRQ1_STATUS        0x20
#define SIRC_RAW_STATUS         0x24
#define SIRC_INT_CLEAR          0x28
#define SIRC_SOFT_INT           0x2C

struct MsmSircState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq out_irq;
    qemu_irq out_fiq;

    uint32_t select, enable, type, polarity, security;
    uint32_t level, latched, softint;
};

static uint32_t sirc_raw(MsmSircState *s)
{
    uint32_t lvl = s->level ^ (s->polarity & ~s->type);
    return ((lvl & ~s->type) | (s->latched & s->type) | s->softint) & SIRC_MASK;
}

static void sirc_update(MsmSircState *s)
{
    uint32_t p = sirc_raw(s) & s->enable;
    qemu_set_irq(s->out_irq, (p & ~s->select) != 0);
    qemu_set_irq(s->out_fiq, (p & s->select) != 0);
}

static void sirc_set_irq(void *opaque, int n, int level)
{
    MsmSircState *s = opaque;
    uint32_t m = 1u << n;
    bool old = s->level & m;

    if (level) {
        s->level |= m;
    } else {
        s->level &= ~m;
    }
    if (s->type & m) {
        bool neg = s->polarity & m;
        if ((!neg && !old && level) || (neg && old && !level)) {
            s->latched |= m;
        }
    }
    sirc_update(s);
}

static uint64_t sirc_read(void *opaque, hwaddr off, unsigned size)
{
    MsmSircState *s = opaque;

    switch (off) {
    case SIRC_INT_SELECT:   return s->select;
    case SIRC_INT_ENABLE:
    case SIRC_INT_ENABLE_SET:
        return s->enable;
    case SIRC_INT_TYPE:     return s->type;
    case SIRC_INT_POLARITY: return s->polarity;
    case SIRC_SECURITY:     return s->security;
    case SIRC_IRQ_STATUS:   return sirc_raw(s) & s->enable & ~s->select;
    case SIRC_IRQ1_STATUS:  return sirc_raw(s) & s->enable & s->select;
    case SIRC_RAW_STATUS:   return sirc_raw(s);
    case SIRC_SOFT_INT:     return s->softint;
    default:
        return 0;
    }
}

static void sirc_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    MsmSircState *s = opaque;

    switch (off) {
    case SIRC_INT_SELECT:       s->select = v; break;
    case SIRC_INT_ENABLE:       s->enable = v; break;
    case SIRC_INT_ENABLE_CLEAR: s->enable &= ~v; break;
    case SIRC_INT_ENABLE_SET:   s->enable |= v; break;
    case SIRC_INT_TYPE:         s->type = v; break;
    case SIRC_INT_POLARITY:     s->polarity = v; break;
    case SIRC_SECURITY:         s->security = v; break;
    case SIRC_INT_CLEAR:
        s->latched &= ~v;
        s->softint &= ~v;
        break;
    case SIRC_SOFT_INT:         s->softint |= v; break;
    default:
        qemu_log_mask(LOG_UNIMP, "msm-sirc: write 0x%x to reg 0x%02x\n",
                      (unsigned)v, (unsigned)off);
        return;
    }
    sirc_update(s);
}

static const MemoryRegionOps sirc_ops = {
    .read = sirc_read,
    .write = sirc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void sirc_reset(DeviceState *dev)
{
    MsmSircState *s = MSM_SIRC(dev);
    s->select = s->enable = s->type = s->polarity = s->security = 0;
    s->latched = s->softint = 0;
}

static void sirc_init(Object *obj)
{
    MsmSircState *s = MSM_SIRC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sirc_ops, s, "msm-sirc", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->out_irq);
    sysbus_init_irq(sbd, &s->out_fiq);
    qdev_init_gpio_in(DEVICE(obj), sirc_set_irq, MSM_SIRC_NUM_IRQS);
}

static const VMStateDescription vmstate_msm_sirc = {
    .name = "msm-sirc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(select, MsmSircState),
        VMSTATE_UINT32(enable, MsmSircState),
        VMSTATE_UINT32(type, MsmSircState),
        VMSTATE_UINT32(polarity, MsmSircState),
        VMSTATE_UINT32(security, MsmSircState),
        VMSTATE_UINT32(level, MsmSircState),
        VMSTATE_UINT32(latched, MsmSircState),
        VMSTATE_UINT32(softint, MsmSircState),
        VMSTATE_END_OF_LIST()
    }
};

static void sirc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, sirc_reset);
    dc->vmsd = &vmstate_msm_sirc;
}

static const TypeInfo sirc_info = {
    .name = TYPE_MSM_SIRC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmSircState),
    .instance_init = sirc_init,
    .class_init = sirc_class_init,
};

static void sirc_register_types(void)
{
    type_register_static(&sirc_info);
}

type_init(sirc_register_types)
