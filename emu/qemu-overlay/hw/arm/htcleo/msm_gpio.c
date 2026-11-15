/*
 * Qualcomm QSD8x50 GPIO controller (165 GPIOs in 8 banks).
 *
 * Two register blocks: GPIO1 (0xA9000000, regs at +0x800) hosts banks
 * 0,2,3,4,5,6,7 and GPIO2 (0xA9100000, regs at +0xC00) hosts bank 1.
 * See arch/arm/mach-msm/gpio_hw.h.
 *
 * Board wiring uses qdev GPIOs:
 *   in  [0..164]  external level driven onto an input pin
 *   out "out"[0..164]  level the pin drives when configured as output
 *   sysbus irq 0 / 1   INT_GPIO_GROUP1 / INT_GPIO_GROUP2 (SIRC inputs)
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

OBJECT_DECLARE_SIMPLE_TYPE(MsmGpioState, MSM_GPIO)

#define NBANKS 8

/* first gpio of each bank, plus sentinel */
static const int bank_start[NBANKS + 1] = { 0, 16, 43, 68, 95, 104, 122, 153, 165 };

/* register offset (relative to the block's register window) of each bank */
enum { R_OUT, R_OE, R_IN, R_EDGE, R_POS, R_EN, R_CLEAR, R_STATUS, R_NUM };
static const uint16_t gpio1_reg_base[R_NUM] = {
    0x00, 0x20, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0
};
static const uint16_t gpio2_reg_base[R_NUM] = {
    0x00, 0x08, 0x20, 0x50, 0x58, 0x60, 0x68, 0x70
};
/* bank number -> index within the GPIO1 block's register arrays */
static const int gpio1_bank_index[NBANKS] = { 0, -1, 1, 2, 3, 4, 5, 6 };

struct MsmGpioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem1;
    MemoryRegion iomem2;
    qemu_irq group_irq[2];
    qemu_irq out[MSM_GPIO_NUM];

    uint32_t out_val[NBANKS];
    uint32_t oe[NBANKS];
    uint32_t edge[NBANKS];
    uint32_t pos[NBANKS];
    uint32_t en[NBANKS];
    uint32_t latched[NBANKS];
    uint32_t ext[NBANKS];        /* external input levels */
    uint32_t ext_default[NBANKS];
};

static int bank_width(int b)
{
    return bank_start[b + 1] - bank_start[b];
}

static uint32_t bank_in(MsmGpioState *s, int b)
{
    return (s->out_val[b] & s->oe[b]) | (s->ext[b] & ~s->oe[b]);
}

static uint32_t bank_status(MsmGpioState *s, int b)
{
    uint32_t in = bank_in(s, b);
    uint32_t level_hit = ~s->edge[b] & ~(in ^ s->pos[b]);
    return (s->latched[b] & s->edge[b]) | level_hit;
}

static void gpio_update(MsmGpioState *s)
{
    bool g1 = false, g2 = false;

    for (int b = 0; b < NBANKS; b++) {
        uint32_t mask = (1ull << bank_width(b)) - 1;
        bool hit = (bank_status(s, b) & s->en[b] & mask) != 0;
        if (b == 1) {
            g2 |= hit;
        } else {
            g1 |= hit;
        }
    }
    qemu_set_irq(s->group_irq[0], g1);
    qemu_set_irq(s->group_irq[1], g2);
}

/* detect edges on a pin whose effective level may have changed */
static void gpio_pin_changed(MsmGpioState *s, int b, uint32_t old_in)
{
    uint32_t new_in = bank_in(s, b);
    uint32_t rise = ~old_in & new_in;
    uint32_t fall = old_in & ~new_in;

    s->latched[b] |= (rise & s->pos[b]) | (fall & ~s->pos[b]);
}

static void gpio_drive_outputs(MsmGpioState *s, int b, uint32_t old_out,
                               uint32_t old_oe)
{
    uint32_t old = old_out & old_oe, now = s->out_val[b] & s->oe[b];
    uint32_t diff = old ^ now;

    while (diff) {
        int bit = ctz32(diff);
        diff &= diff - 1;
        if (bank_start[b] + bit < MSM_GPIO_NUM) {
            qemu_set_irq(s->out[bank_start[b] + bit], (now >> bit) & 1);
        }
    }
}

static void gpio_set_input(void *opaque, int n, int level)
{
    MsmGpioState *s = opaque;
    int b = 0;

    while (n >= bank_start[b + 1]) {
        b++;
    }
    uint32_t m = 1u << (n - bank_start[b]);
    uint32_t old_in = bank_in(s, b);

    if (level) {
        s->ext[b] |= m;
    } else {
        s->ext[b] &= ~m;
    }
    gpio_pin_changed(s, b, old_in);
    gpio_update(s);
}

static uint64_t gpio_reg_read(MsmGpioState *s, int b, int r)
{
    switch (r) {
    case R_OUT:    return s->out_val[b];
    case R_OE:     return s->oe[b];
    case R_IN:     return bank_in(s, b);
    case R_EDGE:   return s->edge[b];
    case R_POS:    return s->pos[b];
    case R_EN:     return s->en[b];
    case R_CLEAR:  return 0;
    case R_STATUS: return bank_status(s, b);
    }
    return 0;
}

static void gpio_reg_write(MsmGpioState *s, int b, int r, uint32_t v)
{
    uint32_t old_in = bank_in(s, b);
    uint32_t old_out = s->out_val[b], old_oe = s->oe[b];

    switch (r) {
    case R_OUT:   s->out_val[b] = v; break;
    case R_OE:    s->oe[b] = v; break;
    case R_EDGE:  s->edge[b] = v; break;
    case R_POS:   s->pos[b] = v; break;
    case R_EN:    s->en[b] = v; break;
    case R_CLEAR: s->latched[b] &= ~v; break;
    default:      return;
    }
    gpio_pin_changed(s, b, old_in);
    gpio_drive_outputs(s, b, old_out, old_oe);
    gpio_update(s);
}

/* decode an offset inside a block; returns false if not a bank register */
static bool gpio_decode(hwaddr off, bool block2, int *bank, int *reg)
{
    if (block2) {
        if (off < 0xC00) {
            return false;
        }
        off -= 0xC00;
        for (int r = 0; r < R_NUM; r++) {
            if (off == gpio2_reg_base[r]) {
                *bank = 1;
                *reg = r;
                return true;
            }
        }
        return false;
    }
    if (off < 0x800) {
        return false;
    }
    off -= 0x800;
    for (int r = 0; r < R_NUM; r++) {
        if (off >= gpio1_reg_base[r] && off < gpio1_reg_base[r] + 7 * 4 &&
            !((off - gpio1_reg_base[r]) & 3)) {
            int idx = (off - gpio1_reg_base[r]) >> 2;
            for (int b = 0; b < NBANKS; b++) {
                if (gpio1_bank_index[b] == idx) {
                    *bank = b;
                    *reg = r;
                    return true;
                }
            }
        }
    }
    return false;
}

static uint64_t gpio1_read(void *opaque, hwaddr off, unsigned size)
{
    int b, r;
    if (gpio_decode(off, false, &b, &r)) {
        return gpio_reg_read(opaque, b, r);
    }
    qemu_log_mask(LOG_UNIMP, "msm-gpio1: read of reg 0x%03x\n", (unsigned)off);
    return 0;
}

static void gpio1_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    int b, r;
    if (gpio_decode(off, false, &b, &r)) {
        gpio_reg_write(opaque, b, r, v);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "msm-gpio1: write 0x%x to reg 0x%03x\n",
                  (unsigned)v, (unsigned)off);
}

static uint64_t gpio2_read(void *opaque, hwaddr off, unsigned size)
{
    int b, r;
    if (gpio_decode(off, true, &b, &r)) {
        return gpio_reg_read(opaque, b, r);
    }
    qemu_log_mask(LOG_UNIMP, "msm-gpio2: read of reg 0x%03x\n", (unsigned)off);
    return 0;
}

static void gpio2_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    int b, r;
    if (gpio_decode(off, true, &b, &r)) {
        gpio_reg_write(opaque, b, r, v);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "msm-gpio2: write 0x%x to reg 0x%03x\n",
                  (unsigned)v, (unsigned)off);
}

static const MemoryRegionOps gpio1_ops = {
    .read = gpio1_read,
    .write = gpio1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps gpio2_ops = {
    .read = gpio2_read,
    .write = gpio2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

void msm_gpio_set_default(DeviceState *dev, int n, bool level)
{
    MsmGpioState *s = MSM_GPIO(dev);
    int b = 0;

    while (n >= bank_start[b + 1]) {
        b++;
    }
    uint32_t m = 1u << (n - bank_start[b]);
    if (level) {
        s->ext_default[b] |= m;
        s->ext[b] |= m;
    } else {
        s->ext_default[b] &= ~m;
        s->ext[b] &= ~m;
    }
}

static void gpio_reset(DeviceState *dev)
{
    MsmGpioState *s = MSM_GPIO(dev);

    memset(s->out_val, 0, sizeof(s->out_val));
    memset(s->oe, 0, sizeof(s->oe));
    memset(s->edge, 0, sizeof(s->edge));
    memset(s->pos, 0, sizeof(s->pos));
    memset(s->en, 0, sizeof(s->en));
    memset(s->latched, 0, sizeof(s->latched));
    memcpy(s->ext, s->ext_default, sizeof(s->ext));
}

static void gpio_init(Object *obj)
{
    MsmGpioState *s = MSM_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem1, obj, &gpio1_ops, s, "msm-gpio1", 0x1000);
    memory_region_init_io(&s->iomem2, obj, &gpio2_ops, s, "msm-gpio2", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem1);
    sysbus_init_mmio(sbd, &s->iomem2);
    sysbus_init_irq(sbd, &s->group_irq[0]);
    sysbus_init_irq(sbd, &s->group_irq[1]);
    qdev_init_gpio_in(DEVICE(obj), gpio_set_input, MSM_GPIO_NUM);
    qdev_init_gpio_out_named(DEVICE(obj), s->out, "out", MSM_GPIO_NUM);
}

static const VMStateDescription vmstate_msm_gpio = {
    .name = "msm-gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(out_val, MsmGpioState, NBANKS),
        VMSTATE_UINT32_ARRAY(oe, MsmGpioState, NBANKS),
        VMSTATE_UINT32_ARRAY(edge, MsmGpioState, NBANKS),
        VMSTATE_UINT32_ARRAY(pos, MsmGpioState, NBANKS),
        VMSTATE_UINT32_ARRAY(en, MsmGpioState, NBANKS),
        VMSTATE_UINT32_ARRAY(latched, MsmGpioState, NBANKS),
        VMSTATE_UINT32_ARRAY(ext, MsmGpioState, NBANKS),
        VMSTATE_END_OF_LIST()
    }
};

static void gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, gpio_reset);
    dc->vmsd = &vmstate_msm_gpio;
}

static const TypeInfo gpio_info = {
    .name = TYPE_MSM_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmGpioState),
    .instance_init = gpio_init,
    .class_init = gpio_class_init,
};

static void gpio_register_types(void)
{
    type_register_static(&gpio_info);
}

type_init(gpio_register_types)
