/*
 * Qualcomm MSM legacy I2C controller ("PWB I2C", 0xA9900000).
 *
 * Byte oriented: software writes one byte at a time to WRITE_DATA (bit 8
 * marks an address byte = START/repeated START, bit 9 marks the last
 * byte = STOP afterwards).  When reading, the controller pre-fetches the
 * next byte into READ_DATA; writing LAST_BYTE while reading makes the
 * *next* fetched byte the final one.  An interrupt is raised after each
 * byte.  Model of the semantics drivers/i2c/busses/i2c-msm.c relies on.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "msm.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmI2cState, MSM_I2C)

#define I2C_WRITE_DATA          0x00
#define I2C_CLK_CTL             0x04
#define I2C_STATUS              0x08
#define I2C_READ_DATA           0x0c
#define I2C_INTERFACE_SELECT    0x10

#define WD_ADDR_BYTE            (1u << 8)
#define WD_LAST_BYTE            (1u << 9)

#define ST_WR_BUFFER_FULL       (1u << 0)
#define ST_RD_BUFFER_FULL       (1u << 1)
#define ST_PACKET_NACKED        (1u << 3)
#define ST_BUS_ACTIVE           (1u << 8)
#define ST_BUS_MASTER           (1u << 9)

#define IFS_SCL                 (1u << 8)
#define IFS_SDA                 (1u << 9)

#define I2C_BYTE_NS             (20 * 1000)    /* ~400 kHz bus */

enum { BUS_IDLE, BUS_WRITE, BUS_READ };

struct MsmI2cState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    I2CBus *bus;
    QEMUTimer *timer;

    uint32_t clk_ctl;
    uint32_t ifsel;
    uint32_t status;
    uint32_t rx;
    int mode;
    bool last_pending;   /* LAST_BYTE written while reading */
    bool rx_final;       /* the byte in READ_DATA is the final one */
    bool irq_pending;
};

static void i2c_update(MsmI2cState *s)
{
    qemu_set_irq(s->irq, s->irq_pending);
}

/* signal "byte done" after a bus delay */
static void i2c_kick(MsmI2cState *s)
{
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + I2C_BYTE_NS);
}

static void i2c_timer_cb(void *opaque)
{
    MsmI2cState *s = opaque;
    s->irq_pending = true;
    i2c_update(s);
}

static void i2c_stop(MsmI2cState *s)
{
    if (s->mode != BUS_IDLE) {
        i2c_end_transfer(s->bus);
    }
    s->mode = BUS_IDLE;
    s->status &= ~(ST_BUS_ACTIVE | ST_BUS_MASTER);
}

static void i2c_fetch(MsmI2cState *s, bool final)
{
    s->rx = i2c_recv(s->bus);
    s->rx_final = final;
    s->status |= ST_RD_BUFFER_FULL;
}

static void i2c_write_data(MsmI2cState *s, uint32_t v)
{
    s->status &= ~ST_PACKET_NACKED;

    if (v & WD_ADDR_BYTE) {
        uint8_t addr = (v >> 1) & 0x7f;
        bool rd = v & 1;

        if (s->mode != BUS_IDLE) {
            i2c_end_transfer(s->bus);   /* repeated start */
        }
        s->status |= ST_BUS_ACTIVE | ST_BUS_MASTER;
        s->status &= ~ST_RD_BUFFER_FULL;
        s->last_pending = false;
        if (i2c_start_transfer(s->bus, addr, rd)) {
            s->mode = BUS_IDLE;
            s->status |= ST_PACKET_NACKED;
            s->status &= ~(ST_BUS_ACTIVE | ST_BUS_MASTER);
        } else if (rd) {
            s->mode = BUS_READ;
            i2c_fetch(s, (v & WD_LAST_BYTE) != 0);
        } else {
            s->mode = BUS_WRITE;
            if (v & WD_LAST_BYTE) {
                i2c_stop(s);
            }
        }
        i2c_kick(s);
        return;
    }

    switch (s->mode) {
    case BUS_WRITE:
        if (i2c_send(s->bus, v & 0xff)) {
            s->status |= ST_PACKET_NACKED;
            i2c_stop(s);
        } else if (v & WD_LAST_BYTE) {
            i2c_stop(s);
        }
        i2c_kick(s);
        break;
    case BUS_READ:
        if (v & WD_LAST_BYTE) {
            s->last_pending = true;
        }
        break;
    default:
        /* stray STOP request on an idle bus */
        break;
    }
}

static uint32_t i2c_read_data(MsmI2cState *s)
{
    uint32_t v = s->rx;

    if (!(s->status & ST_RD_BUFFER_FULL)) {
        return v;
    }
    s->status &= ~ST_RD_BUFFER_FULL;
    if (s->mode == BUS_READ) {
        if (s->rx_final) {
            i2c_stop(s);
        } else {
            i2c_fetch(s, s->last_pending);
            s->last_pending = false;
            i2c_kick(s);
        }
    }
    return v;
}

static uint64_t i2c_read(void *opaque, hwaddr off, unsigned size)
{
    MsmI2cState *s = opaque;

    switch (off) {
    case I2C_CLK_CTL:
        return s->clk_ctl;
    case I2C_STATUS: {
        uint32_t v = s->status;
        s->irq_pending = false;
        i2c_update(s);
        /* error bits are reported once */
        s->status &= ~ST_PACKET_NACKED;
        return v;
    }
    case I2C_READ_DATA:
        return i2c_read_data(s);
    case I2C_INTERFACE_SELECT:
        /* lines float high when the bus is free */
        return s->ifsel | IFS_SCL | IFS_SDA;
    default:
        return 0;
    }
}

static void i2c_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    MsmI2cState *s = opaque;

    switch (off) {
    case I2C_WRITE_DATA:
        s->irq_pending = false;
        i2c_update(s);
        i2c_write_data(s, v);
        break;
    case I2C_CLK_CTL:
        s->clk_ctl = v;
        break;
    case I2C_INTERFACE_SELECT:
        s->ifsel = v & ~(IFS_SCL | IFS_SDA);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps i2c_ops = {
    .read = i2c_read,
    .write = i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void msm_i2c_reset(DeviceState *dev)
{
    MsmI2cState *s = MSM_I2C(dev);

    timer_del(s->timer);
    if (s->mode != BUS_IDLE) {
        i2c_end_transfer(s->bus);
    }
    s->mode = BUS_IDLE;
    s->status = 0;
    s->clk_ctl = 0;
    s->ifsel = 0;
    s->rx = 0;
    s->last_pending = s->rx_final = s->irq_pending = false;
}

static void msm_i2c_init(Object *obj)
{
    MsmI2cState *s = MSM_I2C(obj);

    memory_region_init_io(&s->iomem, obj, &i2c_ops, s, "msm-i2c", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, i2c_timer_cb, s);
}

static const VMStateDescription vmstate_msm_i2c = {
    .name = "msm-i2c",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, MsmI2cState),
        VMSTATE_UINT32(clk_ctl, MsmI2cState),
        VMSTATE_UINT32(ifsel, MsmI2cState),
        VMSTATE_UINT32(status, MsmI2cState),
        VMSTATE_UINT32(rx, MsmI2cState),
        VMSTATE_INT32(mode, MsmI2cState),
        VMSTATE_BOOL(last_pending, MsmI2cState),
        VMSTATE_BOOL(rx_final, MsmI2cState),
        VMSTATE_BOOL(irq_pending, MsmI2cState),
        VMSTATE_END_OF_LIST()
    }
};

static void msm_i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, msm_i2c_reset);
    dc->vmsd = &vmstate_msm_i2c;
}

static const TypeInfo msm_i2c_info = {
    .name = TYPE_MSM_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmI2cState),
    .instance_init = msm_i2c_init,
    .class_init = msm_i2c_class_init,
};

static void msm_i2c_register_types(void)
{
    type_register_static(&msm_i2c_info);
}

type_init(msm_i2c_register_types)
