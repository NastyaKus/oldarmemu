/*
 * Qualcomm MSM SD card controller (SDCC), a PL180 (MMCI) derivative.
 *
 * Differences from the PL181 model in QEMU that matter to the msm_sdcc
 * Linux driver: two interrupt masks (MASK0 -> irq0, MASK1 -> irq1/PIO),
 * a 64 byte FIFO window at 0x80 that is also drained by the data mover
 * in box mode, and DATAEND being raised once DATALENGTH bytes moved.
 *
 * Data is streamed straight between the FIFO window and the SD card;
 * the FIFO never fills or underruns, which suits both PIO and DMA.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/sd/sd.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "msm.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmSdccState, MSM_SDCC)

#define MMCIPOWER           0x000
#define MMCICLOCK           0x004
#define MMCIARGUMENT        0x008
#define MMCICOMMAND         0x00c
#define MMCIRESPCMD         0x010
#define MMCIRESPONSE0       0x014
#define MMCIDATATIMER       0x024
#define MMCIDATALENGTH      0x028
#define MMCIDATACTRL        0x02c
#define MMCIDATACNT         0x030
#define MMCISTATUS          0x034
#define MMCICLEAR           0x038
#define MMCIMASK0           0x03c
#define MMCIMASK1           0x040
#define MMCIFIFOCNT         0x044
#define MMCICCSTIMER        0x058
#define MMCIFIFO            0x080
#define MMCIFIFO_END        0x0c0

#define CMD_INDEX           0x3f
#define CMD_RESPONSE        (1u << 6)
#define CMD_LONGRSP         (1u << 7)
#define CMD_ENABLE          (1u << 10)

#define DPSM_ENABLE         (1u << 0)
#define DPSM_DIRECTION      (1u << 1)   /* 1: card -> host */

#define ST_CMDCRCFAIL       (1u << 0)
#define ST_CMDTIMEOUT       (1u << 2)
#define ST_CMDRESPEND       (1u << 6)
#define ST_CMDSENT          (1u << 7)
#define ST_DATAEND          (1u << 8)
#define ST_DATABLOCKEND     (1u << 10)
#define ST_TXACTIVE         (1u << 12)
#define ST_RXACTIVE         (1u << 13)
#define ST_TXFIFOHALFEMPTY  (1u << 14)
#define ST_RXFIFOHALFFULL   (1u << 15)
#define ST_TXFIFOEMPTY      (1u << 18)
#define ST_RXFIFOEMPTY      (1u << 19)
#define ST_RXDATAAVLBL      (1u << 21)
#define CMD_PROGENA         (1u << 11)
#define ST_PROGDONE         (1u << 23)
#define ST_STATIC_MASK      0x05c007ff  /* bits cleared through MMCICLEAR */

struct MsmSdccState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    SDBus sdbus;
    qemu_irq irq[2];

    uint32_t power, clock, arg, cmd, respcmd;
    uint32_t response[4];
    uint32_t datatimer, datalength, datactrl;
    uint32_t remaining;         /* bytes left in the current transfer */
    uint32_t status;            /* latched (static) status bits */
    uint32_t mask[2];

    /* an SDIO card wired to this slot instead of the SD bus */
    const MsmSdioOps *sdio;
    void *sdio_opaque;
};

void msm_sdcc_attach_sdio(DeviceState *dev, const MsmSdioOps *ops,
                          void *opaque)
{
    MsmSdccState *s = MSM_SDCC(dev);

    s->sdio = ops;
    s->sdio_opaque = opaque;
}

static bool card_data_ready(MsmSdccState *s)
{
    return s->sdio ? true : sdbus_data_ready(&s->sdbus);
}

static int sdcc_trace = -1;

static bool data_active(MsmSdccState *s)
{
    return (s->datactrl & DPSM_ENABLE) && s->remaining;
}

static uint32_t sdcc_status(MsmSdccState *s)
{
    uint32_t st = s->status;

    if (data_active(s)) {
        if (s->datactrl & DPSM_DIRECTION) {
            st |= ST_RXACTIVE;
            if (card_data_ready(s)) {
                st |= ST_RXDATAAVLBL | ST_RXFIFOHALFFULL;
            } else {
                st |= ST_RXFIFOEMPTY;
            }
        } else {
            st |= ST_TXACTIVE | ST_TXFIFOEMPTY | ST_TXFIFOHALFEMPTY;
        }
    } else {
        st |= ST_RXFIFOEMPTY | ST_TXFIFOEMPTY;
    }
    return st;
}

static void sdcc_update(MsmSdccState *s)
{
    uint32_t st = sdcc_status(s);
    qemu_set_irq(s->irq[0], (st & s->mask[0]) != 0);
    qemu_set_irq(s->irq[1], (st & s->mask[1]) != 0);
}

static void sdcc_data_progress(MsmSdccState *s, uint32_t n)
{
    s->remaining = n >= s->remaining ? 0 : s->remaining - n;
    if (!s->remaining) {
        s->status |= ST_DATAEND | ST_DATABLOCKEND;
        if (sdcc_trace > 0) {
            qemu_log("sdcc: dataend (%s)\n", s->datactrl & DPSM_DIRECTION ? "read" : "write");
        }
    }
}

static void sdcc_do_command(MsmSdccState *s)
{
    SDRequest req;
    uint8_t resp[16];
    size_t rlen;

    req.cmd = s->cmd & CMD_INDEX;
    req.arg = s->arg;
    if (s->sdio) {
        uint32_t r = 0;
        rlen = s->sdio->command(s->sdio_opaque, req.cmd, req.arg, &r) ? 4 : 0;
        stl_be_p(resp, r);
    } else {
        rlen = sdbus_do_command(&s->sdbus, &req, resp, sizeof(resp));
    }
    s->respcmd = req.cmd;
    if (sdcc_trace < 0) {
        sdcc_trace = getenv("SDCC_TRACE") != NULL;
    }
    if (sdcc_trace) {
        qemu_log("sdcc: cmd %u arg %08x rlen %zu resp %02x%02x%02x%02x dctrl %x len %u rem %u\n",
                 req.cmd, req.arg, rlen, resp[0], resp[1], resp[2], resp[3],
                 s->datactrl, s->datalength, s->remaining);
    }

    if (s->cmd & CMD_RESPONSE) {
        if (rlen == 0 || (rlen == 4 && (s->cmd & CMD_LONGRSP)) ||
            (rlen != 4 && rlen != 16)) {
            s->status |= ST_CMDTIMEOUT;
            return;
        }
        s->response[0] = ldl_be_p(&resp[0]);
        if (rlen == 4) {
            s->response[1] = s->response[2] = s->response[3] = 0;
        } else {
            s->response[1] = ldl_be_p(&resp[4]);
            s->response[2] = ldl_be_p(&resp[8]);
            s->response[3] = ldl_be_p(&resp[12]) & ~1;
        }
        s->status |= ST_CMDRESPEND;
    } else {
        s->status |= ST_CMDSENT;
    }
}

static uint32_t sdcc_fifo_read(MsmSdccState *s, unsigned size)
{
    uint8_t buf[4] = { 0 };
    uint32_t n = MIN(size, s->remaining);

    if (data_active(s) && (s->datactrl & DPSM_DIRECTION) &&
        card_data_ready(s)) {
        if (s->sdio) {
            s->sdio->read(s->sdio_opaque, buf, n);
        } else {
            sdbus_read_data(&s->sdbus, buf, n);
        }
        sdcc_data_progress(s, n);
    }
    return ldl_le_p(buf);
}

static void sdcc_fifo_write(MsmSdccState *s, uint32_t val, unsigned size)
{
    uint8_t buf[4];
    uint32_t n = MIN(size, s->remaining);

    if (data_active(s) && !(s->datactrl & DPSM_DIRECTION)) {
        stl_le_p(buf, val);
        if (s->sdio) {
            s->sdio->write(s->sdio_opaque, buf, n);
        } else {
            sdbus_write_data(&s->sdbus, buf, n);
        }
        sdcc_data_progress(s, n);
        if (!s->remaining) {
            s->status |= ST_PROGDONE;
        }
    }
}

static uint64_t sdcc_read(void *opaque, hwaddr off, unsigned size)
{
    MsmSdccState *s = opaque;
    uint64_t v;

    if (off >= MMCIFIFO && off < MMCIFIFO_END) {
        v = sdcc_fifo_read(s, size);
        sdcc_update(s);
        return v;
    }
    switch (off) {
    case MMCIPOWER:     return s->power;
    case MMCICLOCK:     return s->clock;
    case MMCIARGUMENT:  return s->arg;
    case MMCICOMMAND:   return s->cmd;
    case MMCIRESPCMD:   return s->respcmd;
    case MMCIRESPONSE0: case MMCIRESPONSE0 + 4:
    case MMCIRESPONSE0 + 8: case MMCIRESPONSE0 + 12:
        return s->response[(off - MMCIRESPONSE0) >> 2];
    case MMCIDATATIMER: return s->datatimer;
    case MMCIDATALENGTH: return s->datalength;
    case MMCIDATACTRL:  return s->datactrl;
    case MMCIDATACNT:   return s->remaining;
    case MMCISTATUS:    return sdcc_status(s);
    case MMCIMASK0:     return s->mask[0];
    case MMCIMASK1:     return s->mask[1];
    case MMCIFIFOCNT:   return DIV_ROUND_UP(s->remaining, 4);
    default:
        return 0;
    }
}

static void sdcc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmSdccState *s = opaque;

    if (off >= MMCIFIFO && off < MMCIFIFO_END) {
        sdcc_fifo_write(s, val, size);
        sdcc_update(s);
        return;
    }
    switch (off) {
    case MMCIPOWER:     s->power = val; break;
    case MMCICLOCK:     s->clock = val; break;
    case MMCIARGUMENT:  s->arg = val; break;
    case MMCICOMMAND:
        s->cmd = val;
        if (val & CMD_ENABLE) {
            sdcc_do_command(s);
            /*
             * PROGENA (R1b commands such as CMD12 after a write): the
             * driver waits for PROGDONE, i.e. for the card to stop
             * being busy.  Our card never is.
             */
            if (val & CMD_PROGENA) {
                s->status |= ST_PROGDONE;
            }
        }
        break;
    case MMCIDATATIMER: s->datatimer = val; break;
    case MMCIDATALENGTH: s->datalength = val; break;
    case MMCIDATACTRL:
        s->datactrl = val;
        if (val & DPSM_ENABLE) {
            s->remaining = s->datalength;
            s->status &= ~(ST_DATAEND | ST_DATABLOCKEND | ST_PROGDONE);
        } else {
            s->remaining = 0;
        }
        break;
    case MMCICLEAR:
        s->status &= ~(val & ST_STATIC_MASK);
        break;
    case MMCIMASK0:     s->mask[0] = val; break;
    case MMCIMASK1:     s->mask[1] = val; break;
    default:
        break;
    }
    sdcc_update(s);
}

static const MemoryRegionOps sdcc_ops = {
    .read = sdcc_read,
    .write = sdcc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void sdcc_reset(DeviceState *dev)
{
    MsmSdccState *s = MSM_SDCC(dev);

    s->power = s->clock = s->arg = s->cmd = s->respcmd = 0;
    memset(s->response, 0, sizeof(s->response));
    s->datatimer = s->datalength = s->datactrl = s->remaining = 0;
    s->status = 0;
    s->mask[0] = s->mask[1] = 0;
}

static void sdcc_init(Object *obj)
{
    MsmSdccState *s = MSM_SDCC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sdcc_ops, s, "msm-sdcc", 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq[0]);
    sysbus_init_irq(sbd, &s->irq[1]);
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_SD_BUS, DEVICE(obj), "sd-bus");
}

static const VMStateDescription vmstate_msm_sdcc = {
    .name = "msm-sdcc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(power, MsmSdccState),
        VMSTATE_UINT32(clock, MsmSdccState),
        VMSTATE_UINT32(arg, MsmSdccState),
        VMSTATE_UINT32(cmd, MsmSdccState),
        VMSTATE_UINT32(respcmd, MsmSdccState),
        VMSTATE_UINT32_ARRAY(response, MsmSdccState, 4),
        VMSTATE_UINT32(datatimer, MsmSdccState),
        VMSTATE_UINT32(datalength, MsmSdccState),
        VMSTATE_UINT32(datactrl, MsmSdccState),
        VMSTATE_UINT32(remaining, MsmSdccState),
        VMSTATE_UINT32(status, MsmSdccState),
        VMSTATE_UINT32_ARRAY(mask, MsmSdccState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void sdcc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, sdcc_reset);
    dc->vmsd = &vmstate_msm_sdcc;
}

static const TypeInfo sdcc_info = {
    .name = TYPE_MSM_SDCC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmSdccState),
    .instance_init = sdcc_init,
    .class_init = sdcc_class_init,
};

static void sdcc_register_types(void)
{
    type_register_static(&sdcc_info);
}

type_init(sdcc_register_types)
