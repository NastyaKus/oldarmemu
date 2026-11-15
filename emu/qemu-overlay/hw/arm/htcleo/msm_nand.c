/*
 * Qualcomm MSM NAND controller (NANDc, 0xA0A00000) with an attached
 * large-page NAND chip (2048 + 64 byte pages, 64 pages per block).
 *
 * The backing image stores the *physical* flash contents: per page
 * 4 codewords of 528 bytes (2112 bytes), bit-inverted so that erased
 * flash (0xFF) is all zeroes on disk: images are sparse and an empty
 * file is a fully erased chip.  A codeword written in ECC mode
 * holds 516 user bytes + 10 parity bytes + spare.  As on the real
 * controller the user byte(s) that would land on the factory bad block
 * marker column of the last codeword are relocated to the spare area,
 * so the marker column keeps reading 0xFF for good blocks.
 *
 * Every EXEC_CMD transfers exactly one codeword between the flash and
 * the 528 byte FLASH_BUFFER; software (usually via the data mover)
 * moves the buffer contents.  See drivers/mtd/devices/msm_nand.[ch].
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "system/block-backend.h"
#include "migration/vmstate.h"
#include "msm.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmNandState, MSM_NAND)

#define NAND_FLASH_CMD          0x000
#define NAND_ADDR0              0x004
#define NAND_ADDR1              0x008
#define NAND_FLASH_CHIP_SELECT  0x00C
#define NAND_EXEC_CMD           0x010
#define NAND_FLASH_STATUS       0x014
#define NAND_BUFFER_STATUS      0x018
#define NAND_DEV0_CFG0          0x020
#define NAND_DEV0_CFG1          0x024
#define NAND_DEV1_CFG0          0x030
#define NAND_DEV1_CFG1          0x034
#define NAND_READ_ID            0x040
#define NAND_READ_STATUS        0x044
#define NAND_CONFIG_DATA        0x050
#define NAND_CONFIG             0x054
#define NAND_CONFIG_MODE        0x058
#define NAND_CONFIG_STATUS      0x060
#define NAND_MACRO1_REG         0x064
#define NAND_DEV_CMD0           0x0A0
#define NAND_DEV_CMD1           0x0A4
#define NAND_DEV_CMD2           0x0A8
#define NAND_DEV_CMD_VLD        0x0AC
#define NAND_EBI2_ECC_BUF_CFG   0x0F0
#define NAND_FLASH_BUFFER       0x100
#define NAND_FLASH_BUFFER_END   0x400   /* window size in the register map */

#define CMD_SOFT_RESET          0x01
#define CMD_SEQ_PAGE_READ       0x15
#define CMD_PAGE_READ           0x32
#define CMD_PAGE_READ_ECC       0x33
#define CMD_PAGE_READ_ALL       0x34
#define CMD_PRG_PAGE            0x36
#define CMD_PRG_PAGE_ECC        0x37
#define CMD_PRG_PAGE_ALL        0x39
#define CMD_BLOCK_ERASE         0x3A
#define CMD_FETCH_ID            0x0B
#define CMD_STATUS              0x0C
#define CMD_RESET               0x0D

#define CFG1_ECC_DISABLE        (1u << 0)
#define CFG1_WIDE_FLASH         (1u << 1)
#define CFG1_BAD_BLOCK_IN_SPARE (1u << 16)

#define FLASH_STATUS_OK         0x000000A0  /* ready, not write protected */
#define FLASH_STATUS_OP_ERR     0x00000010

#define CW_SIZE                 528
#define CW_USER                 516
#define CW_PER_PAGE             4
#define PAGE_DATA               2048
#define PAGE_RAW                (CW_SIZE * CW_PER_PAGE)      /* 2112 */
#define PAGES_PER_BLOCK         64

struct MsmNandState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq_op;
    BlockBackend *blk;
    uint32_t size_mb;
    uint32_t flash_id;

    uint64_t npages;
    uint8_t *ram;           /* used when no drive is attached */

    uint32_t cmd, addr0, addr1, chip_sel;
    uint32_t cfg0, cfg1, dev1_cfg0, dev1_cfg1;
    uint32_t flash_status, buffer_status;
    uint32_t read_id, read_status;
    uint32_t ecc_buf_cfg;
    uint32_t regs[0x100 / 4];     /* misc registers, plain storage */
    uint32_t cur_page;
    uint32_t cur_cw;
    uint8_t buf[CW_SIZE];
};

/* ---- backing store ---- */

static void invert(uint8_t *p, int len)
{
    for (int i = 0; i < len; i++) {
        p[i] = ~p[i];
    }
}

static bool nand_load(MsmNandState *s, uint64_t off, void *buf, int len)
{
    if (s->blk) {
        if (blk_pread(s->blk, off, len, buf, 0) < 0) {
            return false;
        }
    } else {
        memcpy(buf, s->ram + off, len);
    }
    invert(buf, len);
    return true;
}

static bool nand_store(MsmNandState *s, uint64_t off, const void *buf, int len)
{
    g_autofree uint8_t *tmp = g_memdup2(buf, len);

    invert(tmp, len);
    if (s->blk) {
        return blk_pwrite(s->blk, off, len, tmp, 0) >= 0;
    }
    memcpy(s->ram + off, tmp, len);
    return true;
}

static uint64_t cw_offset(uint32_t page, uint32_t cw)
{
    return (uint64_t)page * PAGE_RAW + (uint64_t)cw * CW_SIZE;
}

/* bad block marker column inside the last codeword, and its width */
static int nand_bbm_pos(MsmNandState *s)
{
    int pos = (int)((s->cfg1 >> 6) & 0x3ff) - 1;
    if (pos < 0 || pos >= CW_SIZE - 2) {
        pos = PAGE_DATA - CW_SIZE * (CW_PER_PAGE - 1);   /* 464 */
    }
    return pos;
}

static int nand_bbm_width(MsmNandState *s)
{
    return (s->cfg1 & CFG1_WIDE_FLASH) ? 2 : 1;
}

static bool nand_ecc_mode(MsmNandState *s)
{
    return !(s->cfg1 & CFG1_ECC_DISABLE);
}

/*
 * When the marker column lies in the user data of the last codeword
 * (CFG1 bit 16 clear), the controller keeps the factory marker intact by
 * storing the user byte(s) of that column in the last bytes of the
 * codeword instead.
 */
static bool nand_relocate_bbm(MsmNandState *s, uint32_t cw)
{
    return nand_ecc_mode(s) && cw == CW_PER_PAGE - 1 &&
           !(s->cfg1 & CFG1_BAD_BLOCK_IN_SPARE);
}

#define BBM_SWAP_OFF(w)     (CW_SIZE - (w))

/* codeword geometry from CFG0: user data, parity and spare byte counts */
static void nand_cw_geometry(MsmNandState *s, int *ud, int *ecc, int *spare)
{
    *ud = (s->cfg0 >> 9) & 0x3ff;
    *ecc = (s->cfg0 >> 19) & 0xf;
    *spare = (s->cfg0 >> 23) & 0xf;
    if (*ud == 0 || *ud + *ecc + *spare > CW_SIZE) {
        *ud = 516;
        *ecc = 10;
        *spare = 0;
    }
}

/*
 * Reading in ECC mode the buffer mirrors the physical codeword
 * [user data][parity][spare][...]; the controller would correct bit
 * errors, which we never have.
 */
static void nand_read_cw(MsmNandState *s)
{
    uint8_t phys[CW_SIZE];

    s->flash_status = FLASH_STATUS_OK;
    s->buffer_status = 0;
    if (s->cur_page >= s->npages || s->cur_cw >= CW_PER_PAGE ||
        !nand_load(s, cw_offset(s->cur_page, s->cur_cw), phys, CW_SIZE)) {
        s->flash_status |= FLASH_STATUS_OP_ERR;
        memset(s->buf, 0xff, sizeof(s->buf));
        return;
    }
    memcpy(s->buf, phys, CW_SIZE);
    if (nand_relocate_bbm(s, s->cur_cw)) {
        int pos = nand_bbm_pos(s), w = nand_bbm_width(s);
        memcpy(s->buf + pos, phys + BBM_SWAP_OFF(w), w);
        memset(s->buf + BBM_SWAP_OFF(w), 0xff, w);
    }
}

/*
 * Programming in ECC mode software supplies [user data][spare] and the
 * controller inserts the parity bytes in between.
 */
static void nand_program_cw(MsmNandState *s)
{
    uint8_t phys[CW_SIZE], data[CW_SIZE];

    s->flash_status = FLASH_STATUS_OK;
    s->buffer_status = 0;
    if (s->cur_page >= s->npages || s->cur_cw >= CW_PER_PAGE ||
        !nand_load(s, cw_offset(s->cur_page, s->cur_cw), phys, CW_SIZE)) {
        s->flash_status |= FLASH_STATUS_OP_ERR;
        return;
    }
    memset(data, 0xff, sizeof(data));
    if (nand_ecc_mode(s)) {
        int ud, ecc, spare;
        nand_cw_geometry(s, &ud, &ecc, &spare);
        memcpy(data, s->buf, ud);
        memset(data + ud, 0x00, ecc);           /* fake parity: "programmed" */
        memcpy(data + ud + ecc, s->buf + ud, spare);
        if (nand_relocate_bbm(s, s->cur_cw)) {
            int pos = nand_bbm_pos(s), w = nand_bbm_width(s);
            memcpy(data + BBM_SWAP_OFF(w), s->buf + pos, w);
            memset(data + pos, 0xff, w);
        }
    } else {
        memcpy(data, s->buf, CW_SIZE);
    }
    /* NAND programming can only clear bits */
    for (int i = 0; i < CW_SIZE; i++) {
        phys[i] &= data[i];
    }
    if (!nand_store(s, cw_offset(s->cur_page, s->cur_cw), phys, CW_SIZE)) {
        s->flash_status |= FLASH_STATUS_OP_ERR;
    }
}

static void nand_erase_block(MsmNandState *s, uint32_t page)
{
    uint32_t first = page & ~(PAGES_PER_BLOCK - 1);
    g_autofree uint8_t *ff = g_malloc(PAGE_RAW * PAGES_PER_BLOCK);

    s->flash_status = FLASH_STATUS_OK;
    if (first >= s->npages) {
        s->flash_status |= FLASH_STATUS_OP_ERR;
        return;
    }
    memset(ff, 0xff, PAGE_RAW * PAGES_PER_BLOCK);
    if (!nand_store(s, (uint64_t)first * PAGE_RAW, ff,
                    PAGE_RAW * PAGES_PER_BLOCK)) {
        s->flash_status |= FLASH_STATUS_OP_ERR;
    }
}

static void nand_set_address(MsmNandState *s)
{
    uint32_t col = s->addr0 & 0xffff;

    if (s->cfg1 & CFG1_WIDE_FLASH) {
        col <<= 1;
    }
    s->cur_page = (s->addr0 >> 16) | ((s->addr1 & 0xff) << 16);
    s->cur_cw = col / CW_SIZE;
}

static bool nand_trace;

static void nand_exec(MsmNandState *s)
{
    if (nand_trace) {
        fprintf(stderr, "nand: cmd %02x page %u cw %u cfg0 %08x cfg1 %08x\n",
                s->cmd & 0xff, s->cur_page, s->cur_cw, s->cfg0, s->cfg1);
    }
    switch (s->cmd & 0xff) {
    case CMD_SOFT_RESET:
    case CMD_RESET:
        s->flash_status = FLASH_STATUS_OK;
        break;
    case CMD_FETCH_ID:
        s->read_id = s->flash_id;
        s->flash_status = FLASH_STATUS_OK;
        break;
    case CMD_STATUS:
        s->read_status = 0xE0;  /* ready, not protected */
        s->flash_status = FLASH_STATUS_OK;
        break;
    case CMD_PAGE_READ:
    case CMD_PAGE_READ_ECC:
    case CMD_PAGE_READ_ALL:
    case CMD_SEQ_PAGE_READ:
        nand_read_cw(s);
        s->cur_cw++;
        break;
    case CMD_PRG_PAGE:
    case CMD_PRG_PAGE_ECC:
    case CMD_PRG_PAGE_ALL:
        nand_program_cw(s);
        s->cur_cw++;
        break;
    case CMD_BLOCK_ERASE:
        nand_erase_block(s, s->addr0);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "msm-nand: unknown command 0x%x\n", s->cmd);
        s->flash_status = FLASH_STATUS_OK | FLASH_STATUS_OP_ERR;
        break;
    }
    qemu_irq_pulse(s->irq_op);
}

static uint64_t nand_read(void *opaque, hwaddr off, unsigned size)
{
    MsmNandState *s = opaque;

    if (off >= NAND_FLASH_BUFFER && off < NAND_FLASH_BUFFER_END) {
        uint32_t o = off - NAND_FLASH_BUFFER;
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++) {
            uint8_t b = (o + i < CW_SIZE) ? s->buf[o + i] : 0xff;
            v |= (uint64_t)b << (8 * i);
        }
        return v;
    }
    switch (off) {
    case NAND_FLASH_CMD:        return s->cmd;
    case NAND_ADDR0:            return s->addr0;
    case NAND_ADDR1:            return s->addr1;
    case NAND_FLASH_CHIP_SELECT: return s->chip_sel;
    case NAND_EXEC_CMD:         return 0;
    case NAND_FLASH_STATUS:     return s->flash_status;
    case NAND_BUFFER_STATUS:    return s->buffer_status;
    case NAND_DEV0_CFG0:        return s->cfg0;
    case NAND_DEV0_CFG1:        return s->cfg1;
    case NAND_DEV1_CFG0:        return s->dev1_cfg0;
    case NAND_DEV1_CFG1:        return s->dev1_cfg1;
    case NAND_READ_ID:          return s->read_id;
    case NAND_READ_STATUS:      return s->read_status;
    case NAND_EBI2_ECC_BUF_CFG: return s->ecc_buf_cfg;
    default:
        return off < 0x100 ? s->regs[off >> 2] : 0;
    }
}

static void nand_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MsmNandState *s = opaque;

    if (off >= NAND_FLASH_BUFFER && off < NAND_FLASH_BUFFER_END) {
        uint32_t o = off - NAND_FLASH_BUFFER;
        for (unsigned i = 0; i < size; i++) {
            if (o + i < CW_SIZE) {
                s->buf[o + i] = val >> (8 * i);
            }
        }
        return;
    }
    switch (off) {
    case NAND_FLASH_CMD:        s->cmd = val; break;
    case NAND_ADDR0:            s->addr0 = val; nand_set_address(s); break;
    case NAND_ADDR1:            s->addr1 = val; nand_set_address(s); break;
    case NAND_FLASH_CHIP_SELECT: s->chip_sel = val; break;
    case NAND_EXEC_CMD:
        if (val & 1) {
            nand_exec(s);
        }
        break;
    case NAND_FLASH_STATUS:     s->flash_status = val; break;
    case NAND_BUFFER_STATUS:    s->buffer_status = val; break;
    case NAND_DEV0_CFG0:        s->cfg0 = val; break;
    case NAND_DEV0_CFG1:        s->cfg1 = val; break;
    case NAND_DEV1_CFG0:        s->dev1_cfg0 = val; break;
    case NAND_DEV1_CFG1:        s->dev1_cfg1 = val; break;
    case NAND_READ_ID:          break;
    case NAND_READ_STATUS:      break;
    case NAND_EBI2_ECC_BUF_CFG: s->ecc_buf_cfg = val; break;
    default:
        if (off < 0x100) {
            s->regs[off >> 2] = val;
        }
        break;
    }
}

static const MemoryRegionOps nand_ops = {
    .read = nand_read,
    .write = nand_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * Read one page as the HD2 kernels (htcleo_nand) and cLK lay it out:
 * per codeword 512 data + 10 parity + 4 spare + 2 marker bytes; the
 * 16 free OOB bytes are the four 4-byte spare areas.  Used by the board
 * code to boot straight from NAND.
 */
bool msm_nand_read_page(DeviceState *dev, uint32_t page, uint8_t *data,
                        uint8_t *oob)
{
    MsmNandState *s = MSM_NAND(dev);
    const int pos = PAGE_DATA - CW_SIZE * (CW_PER_PAGE - 1);   /* 464 */

    if (page >= s->npages) {
        return false;
    }
    for (int cw = 0; cw < CW_PER_PAGE; cw++) {
        uint8_t phys[CW_SIZE];
        if (!nand_load(s, cw_offset(page, cw), phys, CW_SIZE)) {
            return false;
        }
        if (cw == CW_PER_PAGE - 1) {
            memcpy(phys + pos, phys + BBM_SWAP_OFF(2), 2);
        }
        if (data) {
            memcpy(data + cw * 512, phys, 512);
        }
        if (oob) {
            memcpy(oob + cw * 4, phys + 512 + 10, 4);
        }
    }
    return true;
}

static void nand_reset(DeviceState *dev)
{
    MsmNandState *s = MSM_NAND(dev);

    s->cmd = s->addr0 = s->addr1 = s->chip_sel = 0;
    /* values left behind by the boot loader / ARM9 */
    s->cfg0 = 0xAA5400C0;
    s->cfg1 = 0x0004745C;
    s->dev1_cfg0 = s->dev1_cfg1 = 0;
    s->flash_status = FLASH_STATUS_OK;
    s->buffer_status = 0;
    s->read_id = s->flash_id;
    s->read_status = 0xE0;
    s->ecc_buf_cfg = 0x1FF;
    memset(s->regs, 0, sizeof(s->regs));
    s->cur_page = s->cur_cw = 0;
    memset(s->buf, 0xff, sizeof(s->buf));
}

static void nand_realize(DeviceState *dev, Error **errp)
{
    MsmNandState *s = MSM_NAND(dev);
    uint64_t want;

    switch (s->size_mb) {
    case 512:
        s->flash_id = 0x5510bcec;   /* Samsung 4Gbit x16 */
        break;
    case 1024:
        s->flash_id = 0x5510b3ec;   /* Samsung 8Gbit x16 */
        break;
    default:
        error_setg(errp, "msm-nand: size must be 512 or 1024 (MiB)");
        return;
    }
    s->npages = (uint64_t)s->size_mb * MiB / PAGE_DATA;
    want = s->npages * PAGE_RAW;

    if (s->blk) {
        int64_t len = blk_getlength(s->blk);
        bool rw = blk_supports_write_perm(s->blk);
        uint64_t perm = BLK_PERM_CONSISTENT_READ |
                        (rw ? BLK_PERM_WRITE | BLK_PERM_RESIZE : 0);
        if (blk_set_perm(s->blk, perm, BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (len >= 0 && (uint64_t)len < want && rw) {
            /* grow (e.g. a fresh empty file) into an erased chip */
            if (blk_truncate(s->blk, want, false, PREALLOC_MODE_OFF, 0,
                             errp) < 0) {
                return;
            }
            len = want;
        }
        if (len < 0 || (uint64_t)len < want) {
            error_setg(errp, "msm-nand: image must be at least %" PRIu64
                       " bytes (%u MiB in 2112-byte pages)", want, s->size_mb);
            return;
        }
    } else {
        s->ram = g_malloc0(want);   /* inverted storage: zero == erased */
    }
    nand_trace = getenv("LEO_NAND_TRACE") != NULL;
    memory_region_init_io(&s->iomem, OBJECT(s), &nand_ops, s, "msm-nand",
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq_op);
}

static const Property nand_props[] = {
    DEFINE_PROP_DRIVE("drive", MsmNandState, blk),
    DEFINE_PROP_UINT32("size-mb", MsmNandState, size_mb, 512),
};

static const VMStateDescription vmstate_msm_nand = {
    .name = "msm-nand",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cmd, MsmNandState),
        VMSTATE_UINT32(addr0, MsmNandState),
        VMSTATE_UINT32(addr1, MsmNandState),
        VMSTATE_UINT32(cfg0, MsmNandState),
        VMSTATE_UINT32(cfg1, MsmNandState),
        VMSTATE_UINT32(flash_status, MsmNandState),
        VMSTATE_UINT32(cur_page, MsmNandState),
        VMSTATE_UINT32(cur_cw, MsmNandState),
        VMSTATE_UINT8_ARRAY(buf, MsmNandState, CW_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static void nand_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = nand_realize;
    device_class_set_legacy_reset(dc, nand_reset);
    device_class_set_props(dc, nand_props);
    dc->vmsd = &vmstate_msm_nand;
}

static const TypeInfo nand_info = {
    .name = TYPE_MSM_NAND,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmNandState),
    .class_init = nand_class_init,
};

static void nand_register_types(void)
{
    type_register_static(&nand_info);
}

type_init(nand_register_types)
