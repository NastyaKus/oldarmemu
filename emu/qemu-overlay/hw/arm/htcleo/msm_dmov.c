/*
 * Qualcomm MSM "data mover" (ADM) DMA engine, apps security domain.
 *
 * The apps processor uses security domain 3 (offset 0xC00).  Software
 * writes the address of a command (pointer) list into CMD_PTR(ch); the
 * engine walks it and moves data between memory and peripheral FIFOs
 * (NAND, SDCC, UART_DM).  Flow control (CRCI) is irrelevant for us:
 * peripherals are always ready, so commands are executed synchronously
 * at the CMD_PTR write.  See arch/arm/mach-msm/{dma.c,include/mach/dma.h}.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "migration/vmstate.h"
#include "msm.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmDmovState, MSM_DMOV)

#define DMOV_NCH            16
#define DMOV_SD_AARM        0xC00
#define DMOV_CMD_PTR        0x000
#define DMOV_RSLT           0x040
#define DMOV_FLUSH0         0x080
#define DMOV_FLUSH5         0x1C0
#define DMOV_STATUS         0x200
#define DMOV_CONFIG         0x300
#define DMOV_ISR            0x380

#define DMOV_CMD_LIST       0
#define DMOV_CMD_PTR_LIST   1

#define DMOV_RSLT_VALID     (1u << 31)
#define DMOV_RSLT_ERROR     (1u << 3)
#define DMOV_RSLT_FLUSH     (1u << 2)
#define DMOV_RSLT_DONE      (1u << 1)

#define DMOV_STATUS_RSLT_VALID   (1u << 1)
#define DMOV_STATUS_CMD_PTR_RDY  (1u << 0)

#define DMOV_CONFIG_IRQ_EN  (1u << 0)

#define CMD_LC              (1u << 31)
#define CMD_MODE_MASK       3
#define CMD_MODE_SINGLE     0
#define CMD_MODE_SG         1
#define CMD_MODE_IND_SG     2
#define CMD_MODE_BOX        3
#define CMD_PTR_LP          (1u << 31)

#define RSLT_FIFO           8
#define MAX_CMDS            4096    /* guard against runaway lists */

struct MsmDmovState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    AddressSpace *as;

    uint32_t config[DMOV_NCH];
    uint32_t flush[DMOV_NCH][6];
    uint32_t rslt[DMOV_NCH][RSLT_FIFO];
    uint32_t rslt_count[DMOV_NCH];
    uint32_t pending[DMOV_NCH];         /* commands waiting for data */
    uint32_t isr;
};

static uint32_t rd32(MsmDmovState *s, hwaddr a)
{
    uint32_t v = 0;
    address_space_read(s->as, a, MEMTXATTRS_UNSPECIFIED, &v, 4);
    return le32_to_cpu(v);
}

static void dmov_update(MsmDmovState *s)
{
    bool level = false;
    for (int ch = 0; ch < DMOV_NCH; ch++) {
        if ((s->isr & (1u << ch)) && (s->config[ch] & DMOV_CONFIG_IRQ_EN)) {
            level = true;
        }
    }
    qemu_set_irq(s->irq, level);
}

/* Copy len bytes; peripheral registers are accessed one word at a time. */
static void dmov_copy(MsmDmovState *s, hwaddr src, hwaddr dst, uint32_t len)
{
    uint8_t buf[4096];

    while (len) {
        uint32_t n = MIN(len, sizeof(buf));
        address_space_read(s->as, src, MEMTXATTRS_UNSPECIFIED, buf, n);
        address_space_write(s->as, dst, MEMTXATTRS_UNSPECIFIED, buf, n);
        src += n;
        dst += n;
        len -= n;
    }
}

/*
 * Reads from a UART_DM receive FIFO wait for data (CRCI) that our UARTs
 * never produce (Bluetooth on ttyHS0): leave such commands pending until
 * the driver flushes the channel.  Completing them at once makes msm_hs
 * resubmit forever in interrupt context.
 */
static bool dmov_waits_forever(uint32_t src)
{
    return (src >= MSM_UART1DM_PHYS && src < MSM_UART1DM_PHYS + 0x1000) ||
           (src >= MSM_UART2DM_PHYS && src < MSM_UART2DM_PHYS + 0x1000);
}

enum { RUN_BAD, RUN_DONE, RUN_PENDING };

/* Execute an array of commands */
static int dmov_run_cmds(MsmDmovState *s, hwaddr a, int *budget)
{
    for (;;) {
        uint32_t cmd = rd32(s, a);

        if (--(*budget) < 0) {
            return RUN_BAD;
        }
        if ((cmd & CMD_MODE_MASK) == CMD_MODE_SINGLE ||
            (cmd & CMD_MODE_MASK) == CMD_MODE_BOX) {
            if (dmov_waits_forever(rd32(s, a + 4))) {
                return RUN_PENDING;
            }
        }
        switch (cmd & CMD_MODE_MASK) {
        case CMD_MODE_SINGLE: {
            uint32_t src = rd32(s, a + 4);
            uint32_t dst = rd32(s, a + 8);
            uint32_t len = rd32(s, a + 12) & 0xffff;
            dmov_copy(s, src, dst, len);
            a += 16;
            break;
        }
        case CMD_MODE_BOX: {
            uint32_t src = rd32(s, a + 4);
            uint32_t dst = rd32(s, a + 8);
            uint32_t lens = rd32(s, a + 12);
            uint32_t rows = rd32(s, a + 16);
            uint32_t offs = rd32(s, a + 20);
            uint32_t src_len = lens >> 16, dst_len = lens & 0xffff;
            uint32_t nrows = rows >> 16;
            uint32_t src_off = offs >> 16, dst_off = offs & 0xffff;
            uint32_t len = MIN(src_len, dst_len);

            if ((rows & 0xffff) > nrows) {
                nrows = rows & 0xffff;
            }
            for (uint32_t r = 0; r < nrows; r++) {
                dmov_copy(s, src, dst, len);
                src += src_off;
                dst += dst_off;
            }
            a += 24;
            break;
        }
        default:
            qemu_log_mask(LOG_UNIMP, "msm-dmov: command mode %u not "
                          "supported\n", cmd & CMD_MODE_MASK);
            return RUN_BAD;
        }
        if (cmd & CMD_LC) {
            return RUN_DONE;
        }
    }
}

static void dmov_push_result(MsmDmovState *s, int ch, uint32_t r)
{
    if (s->rslt_count[ch] < RSLT_FIFO) {
        s->rslt[ch][s->rslt_count[ch]++] = r;
    }
    s->isr |= 1u << ch;
    dmov_update(s);
}

static int dmov_trace = -1;

static void dmov_start(MsmDmovState *s, int ch, uint32_t val)
{
    hwaddr a = (hwaddr)(val & 0x1fffffff) << 3;
    int budget = MAX_CMDS;
    int r = RUN_DONE;

    /*
     * Type 0 ("command list") is documented as not working, and the
     * kernel's only user (msm_serial_hs) points it at a pointer list
     * anyway: walk both as pointer lists.
     */
    if ((val >> 29) == DMOV_CMD_PTR_LIST || (val >> 29) == DMOV_CMD_LIST) {
        for (;;) {
            uint32_t p = rd32(s, a);
            r = dmov_run_cmds(s, (hwaddr)(p & 0x1fffffff) << 3, &budget);
            if (r != RUN_DONE) {
                break;
            }
            if (p & CMD_PTR_LP) {
                break;
            }
            a += 4;
        }
    } else {
        qemu_log_mask(LOG_UNIMP, "msm-dmov: cmd ptr type %u\n", val >> 29);
        r = RUN_BAD;
    }
    if (dmov_trace < 0) {
        dmov_trace = getenv("DMOV_TRACE") != NULL;
    }
    if (dmov_trace) {
        qemu_log("dmov: ch %d cmdptr %08x -> %s\n", ch, val,
                 r == RUN_PENDING ? "pending" : r == RUN_DONE ? "done" : "error");
    }
    if (r == RUN_PENDING) {
        s->pending[ch]++;               /* completes with the next flush */
        return;
    }
    dmov_push_result(s, ch, DMOV_RSLT_VALID |
                     (r == RUN_DONE ? DMOV_RSLT_DONE : DMOV_RSLT_ERROR));
}

static uint64_t dmov_read(void *opaque, hwaddr off, unsigned size)
{
    MsmDmovState *s = opaque;
    uint32_t v;
    int ch;

    if (off < DMOV_SD_AARM) {
        return 0;           /* other security domains: not ours */
    }
    off -= DMOV_SD_AARM;
    ch = (off >> 2) & (DMOV_NCH - 1);

    if (off == DMOV_ISR) {
        v = s->isr;
        s->isr = 0;
        for (int i = 0; i < DMOV_NCH; i++) {
            if (s->rslt_count[i]) {
                s->isr |= 1u << i;
            }
        }
        dmov_update(s);
        return v;
    }
    switch (off & ~0x3f) {
    case DMOV_CMD_PTR:
        return 0;
    case DMOV_RSLT:
        if (!s->rslt_count[ch]) {
            return 0;
        }
        v = s->rslt[ch][0];
        memmove(&s->rslt[ch][0], &s->rslt[ch][1],
                (RSLT_FIFO - 1) * sizeof(uint32_t));
        s->rslt_count[ch]--;
        return v;
    case DMOV_STATUS:
        v = DMOV_STATUS_CMD_PTR_RDY;
        if (s->rslt_count[ch]) {
            v |= DMOV_STATUS_RSLT_VALID | (MIN(s->rslt_count[ch], 7u) << 29);
        }
        return v;
    case DMOV_CONFIG:
        return s->config[ch];
    }
    if (off >= DMOV_FLUSH0 && off < DMOV_FLUSH5 + 0x40) {
        return s->flush[ch][(off - DMOV_FLUSH0) >> 6];
    }
    return 0;
}

static void dmov_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmDmovState *s = opaque;
    int ch;

    if (off < DMOV_SD_AARM) {
        return;
    }
    off -= DMOV_SD_AARM;
    ch = (off >> 2) & (DMOV_NCH - 1);

    switch (off & ~0x3f) {
    case DMOV_CMD_PTR:
        dmov_start(s, ch, val);
        return;
    case DMOV_CONFIG:
        s->config[ch] = val;
        dmov_update(s);
        return;
    }
    if (off >= DMOV_FLUSH0 && off < DMOV_FLUSH5 + 0x40) {
        int n = (off - DMOV_FLUSH0) >> 6;
        s->flush[ch][n] = val;
        if (n == 0 && (val & (1u << 31))) {
            /*
             * Flush: only a command still waiting for data (UART RX)
             * is in flight and completes with a flush result; otherwise
             * there is nothing to report (a spurious result makes the
             * driver run the callback of a command it never issued).
             */
            if (dmov_trace > 0) {
                qemu_log("dmov: ch %d flush (%s)\n", ch,
                         s->pending[ch] ? "pending" : "idle");
            }
            /* a flush ends every queued command of the channel */
            for (; s->pending[ch]; s->pending[ch]--) {
                dmov_push_result(s, ch, DMOV_RSLT_VALID | DMOV_RSLT_FLUSH);
            }
        }
    }
}

static const MemoryRegionOps dmov_ops = {
    .read = dmov_read,
    .write = dmov_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void dmov_reset(DeviceState *dev)
{
    MsmDmovState *s = MSM_DMOV(dev);
    memset(s->config, 0, sizeof(s->config));
    memset(s->flush, 0, sizeof(s->flush));
    memset(s->rslt, 0, sizeof(s->rslt));
    memset(s->rslt_count, 0, sizeof(s->rslt_count));
    memset(s->pending, 0, sizeof(s->pending));
    s->isr = 0;
}

static void dmov_init(Object *obj)
{
    MsmDmovState *s = MSM_DMOV(obj);

    memory_region_init_io(&s->iomem, obj, &dmov_ops, s, "msm-dmov", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->as = &address_space_memory;
}

static const VMStateDescription vmstate_msm_dmov = {
    .name = "msm-dmov",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(config, MsmDmovState, DMOV_NCH),
        VMSTATE_UINT32_2DARRAY(rslt, MsmDmovState, DMOV_NCH, RSLT_FIFO),
        VMSTATE_UINT32_ARRAY(rslt_count, MsmDmovState, DMOV_NCH),
        VMSTATE_UINT32(isr, MsmDmovState),
        VMSTATE_END_OF_LIST()
    }
};

static void dmov_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, dmov_reset);
    dc->vmsd = &vmstate_msm_dmov;
}

static const TypeInfo dmov_info = {
    .name = TYPE_MSM_DMOV,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmDmovState),
    .instance_init = dmov_init,
    .class_init = dmov_class_init,
};

static void dmov_register_types(void)
{
    type_register_static(&dmov_info);
}

type_init(dmov_register_types)
