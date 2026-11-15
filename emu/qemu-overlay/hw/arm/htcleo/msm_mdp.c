/*
 * Qualcomm MDP 3.1 display processor, LCDC output path (QSD8x50).
 *
 * The HD2 drives its 480x800 panel over the LCDC (parallel RGB)
 * interface: once MDP_LCDC_EN is set, the DMA_P engine continuously
 * scans the frame buffer at DMA_P_IBUF_ADDR.  We present that buffer on
 * a QEMU graphic console and raise LCDC_FRAME_START / DMA_P_DONE at
 * 60 Hz.  The PPP 2D blitter is not modelled yet (DL0_ROI_DONE is
 * signalled immediately so waiters do not hang).
 *
 * Register offsets: drivers/video/msm/mdp_hw.h (CONFIG_MSM_MDP31).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "system/address-spaces.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "migration/vmstate.h"
#include "msm.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmMdpState, MSM_MDP)

#define MDP_INTR_ENABLE         0x00020
#define MDP_INTR_STATUS         0x00024
#define MDP_INTR_CLEAR          0x00028
#define MDP_DMA_P_START         0x00044
#define MDP_DMA_S_START         0x00048
#define MDP_PPP_START           0x00030
#define MDP_DMA_P_CONFIG        0x90000
#define MDP_DMA_P_SIZE          0x90004
#define MDP_DMA_P_IBUF_ADDR     0x90008
#define MDP_DMA_P_IBUF_Y_STRIDE 0x9000c
#define MDP_DMA_P_OUT_XY        0x90010
#define MDP_LCDC_EN             0xe0000

#define INT_DL0_ROI_DONE        (1u << 0)
#define INT_DMA_S_DONE          (1u << 2)
#define INT_DMA_P_DONE          (1u << 14)
#define INT_LCDC_FRAME_START    (1u << 15)

/* MDP 3.1: input buffer format in bits 26:25 */
#define DMA_IBUF_FORMAT_SHIFT   25
#define DMA_IBUF_FORMAT_RGB888  0
#define DMA_IBUF_FORMAT_RGB565  1
#define DMA_IBUF_FORMAT_XRGB8888 2
#define DMA_PACK_PATTERN_SHIFT  8

#define MDP_REGS_SIZE           0xF0000
#define FRAME_NS                (NANOSECONDS_PER_SECOND / 60)

#define LEO_LCD_W               480
#define LEO_LCD_H               800

struct MsmMdpState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QemuConsole *con;
    QEMUTimer *vsync;

    uint32_t intr_enable;
    uint32_t intr_status;
    uint32_t dma_p_config;
    uint32_t dma_p_size;
    uint32_t dma_p_ibuf;
    uint32_t dma_p_stride;
    uint32_t lcdc_en;
    GHashTable *regs;           /* everything else, plain storage */
    uint8_t *line;              /* scratch for one source line */
};

static void mdp_update_irq(MsmMdpState *s)
{
    qemu_set_irq(s->irq, (s->intr_status & s->intr_enable) != 0);
}

static void mdp_raise(MsmMdpState *s, uint32_t bits)
{
    s->intr_status |= bits;
    mdp_update_irq(s);
}

static void mdp_vsync(void *opaque)
{
    MsmMdpState *s = opaque;

    if (s->lcdc_en & 1) {
        mdp_raise(s, INT_LCDC_FRAME_START | INT_DMA_P_DONE);
        timer_mod(s->vsync, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FRAME_NS);
    }
}

/* ---------------- display ---------------- */

static void mdp_gfx_update(void *opaque)
{
    MsmMdpState *s = opaque;
    DisplaySurface *surf = qemu_console_surface(s->con);
    int w = s->dma_p_size & 0xfff;
    int h = (s->dma_p_size >> 16) & 0x7ff;
    int stride = s->dma_p_stride;
    int bpp;
    bool bgr;

    if (!(s->lcdc_en & 1) || !w || !h) {
        w = LEO_LCD_W;
        h = LEO_LCD_H;
    }
    if (!surf || surface_width(surf) != w || surface_height(surf) != h) {
        qemu_console_resize(s->con, w, h);
        surf = qemu_console_surface(s->con);
    }
    uint32_t *dst = (uint32_t *)surface_data(surf);
    int dst_stride = surface_stride(surf) / 4;

    if (!(s->lcdc_en & 1) || !s->dma_p_ibuf) {
        /* panel off: black */
        for (int y = 0; y < h; y++) {
            memset(dst + y * dst_stride, 0, w * 4);
        }
        dpy_gfx_update_full(s->con);
        return;
    }

    switch ((s->dma_p_config >> DMA_IBUF_FORMAT_SHIFT) & 3) {
    case DMA_IBUF_FORMAT_RGB565:
        bpp = 2;
        break;
    case DMA_IBUF_FORMAT_XRGB8888:
        bpp = 4;
        break;
    default:
        /* "RGB888" is also used for 32 bpp buffers with a 4*w stride */
        bpp = (stride && stride / w >= 4) ? 4 : 3;
        break;
    }
    if (!stride) {
        stride = w * bpp;
    }
    /* pack pattern: RGB (0x21) or BGR (0x12) component order */
    bgr = ((s->dma_p_config >> DMA_PACK_PATTERN_SHIFT) & 0x3f) == 0x12;

    s->line = g_realloc(s->line, stride);
    for (int y = 0; y < h; y++) {
        uint8_t *src = s->line;
        uint32_t *d = dst + y * dst_stride;

        address_space_read(&address_space_memory,
                           (hwaddr)s->dma_p_ibuf + (hwaddr)y * stride,
                           MEMTXATTRS_UNSPECIFIED, src, w * bpp);
        for (int x = 0; x < w; x++) {
            unsigned r, g, b;
            if (bpp == 2) {
                uint16_t v = lduw_le_p(src + x * 2);
                r = (v >> 11) & 0x1f;
                g = (v >> 5) & 0x3f;
                b = v & 0x1f;
                r = (r << 3) | (r >> 2);
                g = (g << 2) | (g >> 4);
                b = (b << 3) | (b >> 2);
            } else {
                /* little endian bytes in memory: B G R (A) */
                b = src[x * bpp + 0];
                g = src[x * bpp + 1];
                r = src[x * bpp + 2];
            }
            if (bgr) {
                unsigned t = r;
                r = b;
                b = t;
            }
            d[x] = rgb_to_pixel32(r, g, b);
        }
    }
    dpy_gfx_update_full(s->con);
}

static const GraphicHwOps mdp_gfx_ops = {
    .gfx_update = mdp_gfx_update,
};

/* ---------------- registers ---------------- */

static uint64_t mdp_read(void *opaque, hwaddr off, unsigned size)
{
    MsmMdpState *s = opaque;

    switch (off) {
    case MDP_INTR_ENABLE:       return s->intr_enable;
    case MDP_INTR_STATUS:       return s->intr_status;
    case MDP_DMA_P_CONFIG:      return s->dma_p_config;
    case MDP_DMA_P_SIZE:        return s->dma_p_size;
    case MDP_DMA_P_IBUF_ADDR:   return s->dma_p_ibuf;
    case MDP_DMA_P_IBUF_Y_STRIDE: return s->dma_p_stride;
    case MDP_LCDC_EN:           return s->lcdc_en;
    default:
        return GPOINTER_TO_UINT(g_hash_table_lookup(s->regs,
                                                    GUINT_TO_POINTER(off)));
    }
}

static void mdp_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmMdpState *s = opaque;

    switch (off) {
    case MDP_INTR_ENABLE:
        s->intr_enable = val;
        mdp_update_irq(s);
        return;
    case MDP_INTR_CLEAR:
        s->intr_status &= ~val;
        mdp_update_irq(s);
        return;
    case MDP_DMA_P_CONFIG:      s->dma_p_config = val; return;
    case MDP_DMA_P_SIZE:        s->dma_p_size = val; return;
    case MDP_DMA_P_IBUF_ADDR:   s->dma_p_ibuf = val; return;
    case MDP_DMA_P_IBUF_Y_STRIDE: s->dma_p_stride = val; return;
    case MDP_LCDC_EN:
        s->lcdc_en = val;
        if (val & 1) {
            timer_mod(s->vsync,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FRAME_NS);
        } else {
            timer_del(s->vsync);
        }
        return;
    case MDP_DMA_P_START:
        /* MDDI style one-shot update: completes immediately */
        mdp_raise(s, INT_DMA_P_DONE);
        return;
    case MDP_DMA_S_START:
        mdp_raise(s, INT_DMA_S_DONE);
        return;
    case MDP_PPP_START:
        qemu_log_mask(LOG_UNIMP, "msm-mdp: PPP blit not implemented\n");
        mdp_raise(s, INT_DL0_ROI_DONE);
        return;
    default:
        g_hash_table_insert(s->regs, GUINT_TO_POINTER(off),
                            GUINT_TO_POINTER((uint32_t)val));
        return;
    }
}

static const MemoryRegionOps mdp_ops = {
    .read = mdp_read,
    .write = mdp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void mdp_reset(DeviceState *dev)
{
    MsmMdpState *s = MSM_MDP(dev);

    timer_del(s->vsync);
    s->intr_enable = s->intr_status = 0;
    /* SPL/cLK leave DMA_P in 32 bpp (ARGB8888) mode */
    s->dma_p_config = DMA_IBUF_FORMAT_XRGB8888 << DMA_IBUF_FORMAT_SHIFT;
    s->dma_p_size = (LEO_LCD_H << 16) | LEO_LCD_W;
    s->dma_p_ibuf = 0;
    s->dma_p_stride = LEO_LCD_W * 4;
    s->lcdc_en = 0;
    g_hash_table_remove_all(s->regs);
}

static void mdp_realize(DeviceState *dev, Error **errp)
{
    MsmMdpState *s = MSM_MDP(dev);
    s->con = graphic_console_init(dev, 0, &mdp_gfx_ops, s);
    qemu_console_resize(s->con, LEO_LCD_W, LEO_LCD_H);
}

static void mdp_init(Object *obj)
{
    MsmMdpState *s = MSM_MDP(obj);

    memory_region_init_io(&s->iomem, obj, &mdp_ops, s, "msm-mdp",
                          MDP_REGS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->vsync = timer_new_ns(QEMU_CLOCK_VIRTUAL, mdp_vsync, s);
    s->regs = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static const VMStateDescription vmstate_msm_mdp = {
    .name = "msm-mdp",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(vsync, MsmMdpState),
        VMSTATE_UINT32(intr_enable, MsmMdpState),
        VMSTATE_UINT32(intr_status, MsmMdpState),
        VMSTATE_UINT32(dma_p_config, MsmMdpState),
        VMSTATE_UINT32(dma_p_size, MsmMdpState),
        VMSTATE_UINT32(dma_p_ibuf, MsmMdpState),
        VMSTATE_UINT32(dma_p_stride, MsmMdpState),
        VMSTATE_UINT32(lcdc_en, MsmMdpState),
        VMSTATE_END_OF_LIST()
    }
};

static void mdp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mdp_realize;
    device_class_set_legacy_reset(dc, mdp_reset);
    dc->vmsd = &vmstate_msm_mdp;
}

static const TypeInfo mdp_info = {
    .name = TYPE_MSM_MDP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmMdpState),
    .instance_init = mdp_init,
    .class_init = mdp_class_init,
};

static void mdp_register_types(void)
{
    type_register_static(&mdp_info);
}

type_init(mdp_register_types)
