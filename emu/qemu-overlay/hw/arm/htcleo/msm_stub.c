/*
 * Placeholder for hardware blocks we do not model yet.
 *
 * Behaves like a sparse register file (reads return the last value
 * written, 0 otherwise) and logs the first accesses to every register
 * under "-d unimp".  This lets a kernel get through probe code that just
 * pokes registers, and tells us exactly which block to implement next
 * when something hangs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "msm.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmStubState, MSM_STUB)

#define STUB_LOG_LIMIT 8

typedef struct StubReg {
    uint32_t val;
    uint32_t nread;
    uint32_t nwrite;
} StubReg;

struct MsmStubState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    char *name;
    uint64_t size;
    uint64_t base;
    GHashTable *regs;
    GHashTable *presets;   /* offset -> reset value */
};

static StubReg *stub_reg(MsmStubState *s, hwaddr off)
{
    StubReg *r = g_hash_table_lookup(s->regs, GUINT_TO_POINTER(off));
    if (!r) {
        r = g_new0(StubReg, 1);
        g_hash_table_insert(s->regs, GUINT_TO_POINTER(off), r);
    }
    return r;
}

static uint64_t stub_read(void *opaque, hwaddr off, unsigned size)
{
    MsmStubState *s = opaque;
    hwaddr aligned = off & ~3ULL;
    StubReg *r = stub_reg(s, aligned);
    uint64_t v = r->val >> ((off & 3) * 8);

    if (size < 4) {
        v &= (1ULL << (size * 8)) - 1;
    }
    if (r->nread++ < STUB_LOG_LIMIT) {
        qemu_log_mask(LOG_UNIMP, "stub %s: read  0x%08" PRIx64 " -> 0x%08x%s\n",
                      s->name, s->base + off, (unsigned)v,
                      r->nread == STUB_LOG_LIMIT ? " (further reads muted)" : "");
    }
    return v;
}

static void stub_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmStubState *s = opaque;
    hwaddr aligned = off & ~3ULL;
    StubReg *r = stub_reg(s, aligned);
    unsigned shift = (off & 3) * 8;
    uint32_t mask = size >= 4 ? 0xffffffffu : (((1u << (size * 8)) - 1) << shift);

    r->val = (r->val & ~mask) | (((uint32_t)val << shift) & mask);
    if (r->nwrite++ < STUB_LOG_LIMIT) {
        qemu_log_mask(LOG_UNIMP, "stub %s: write 0x%08" PRIx64 " <- 0x%08x%s\n",
                      s->name, s->base + off, (unsigned)val,
                      r->nwrite == STUB_LOG_LIMIT ? " (further writes muted)" : "");
    }
}

static const MemoryRegionOps stub_ops = {
    .read = stub_read,
    .write = stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void stub_realize(DeviceState *dev, Error **errp)
{
    MsmStubState *s = MSM_STUB(dev);

    if (!s->name) {
        s->name = g_strdup("unnamed");
    }
    s->regs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    s->presets = g_hash_table_new(g_direct_hash, g_direct_equal);
    memory_region_init_io(&s->iomem, OBJECT(s), &stub_ops, s, s->name, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static void stub_reset(DeviceState *dev)
{
    MsmStubState *s = MSM_STUB(dev);
    GHashTableIter it;
    gpointer k, v;

    if (!s->regs) {
        return;
    }
    g_hash_table_remove_all(s->regs);
    g_hash_table_iter_init(&it, s->presets);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        stub_reg(s, GPOINTER_TO_UINT(k))->val = GPOINTER_TO_UINT(v);
    }
}

void msm_stub_preset(DeviceState *dev, hwaddr off, uint32_t val)
{
    MsmStubState *s = MSM_STUB(dev);

    g_hash_table_insert(s->presets, GUINT_TO_POINTER(off & ~3ULL),
                        GUINT_TO_POINTER(val));
    stub_reg(s, off & ~3ULL)->val = val;
}

static const Property stub_props[] = {
    DEFINE_PROP_STRING("name", MsmStubState, name),
    DEFINE_PROP_UINT64("size", MsmStubState, size, 0x1000),
    DEFINE_PROP_UINT64("base", MsmStubState, base, 0),
};

static void stub_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stub_realize;
    device_class_set_legacy_reset(dc, stub_reset);
    device_class_set_props(dc, stub_props);
    dc->user_creatable = false;
}

static const TypeInfo stub_info = {
    .name = TYPE_MSM_STUB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmStubState),
    .class_init = stub_class_init,
};

static void stub_register_types(void)
{
    type_register_static(&stub_info);
}

type_init(stub_register_types)

DeviceState *msm_stub_create(const char *name, hwaddr base, uint64_t size,
                             int priority)
{
    DeviceState *dev = qdev_new(TYPE_MSM_STUB);

    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint64(dev, "size", size);
    qdev_prop_set_uint64(dev, "base", base);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory_region_add_subregion_overlap(get_system_memory(), base,
                                        sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0),
                                        priority);
    return dev;
}
