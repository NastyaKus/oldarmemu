/*
 * High-level emulation of the QSD8250 modem processor (ARM9 / AMSS).
 *
 * The real HD2 runs Windows Mobile era AMSS firmware on the ARM9 side.
 * The apps processor talks to it through:
 *   - shared memory (SMEM) at physical 0x00100000, whose heap TOC the
 *     modem fills before the apps CPU boots,
 *   - "DEX" commands (WinCE flavour of proc_comm) in SMEM + 0xefe00,
 *     doorbell A2M_INT(4),
 *   - standard proc_comm in SMEM + 0x0, doorbell A2M_INT(6),
 *   - SMSM state words and SMD channels (not yet).
 *
 * We do not run AMSS; instead requests are answered synchronously from
 * the doorbell write.  See arch/arm/mach-msm/{dex_comm,proc_comm,smd}.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "system/rtc.h"
#include "migration/vmstate.h"
#include "msm.h"

#include "msm_a9.h"

/* ---- SMEM layout (smd_private.h) ---- */
#define SMEM_HEAP_INFO_OFF      0x0C0
#define SMEM_TOC_OFF            0x0D0
#define SMD_HEAP_SIZE           512
#define SMEM_HEAP_START         0x2100
#define SMEM_HEAP_END           0xE0000   /* keep clear of WinCE areas */

/* item ids (CONFIG_MSM_N_WAY_SMD numbering) */
enum {
    SMEM_PROC_COMM = 0,
    SMEM_HEAP_INFO,
    SMEM_ALLOCATION_TABLE,
    SMEM_VERSION_INFO,
    SMEM_HW_RESET_DETECT,
    SMEM_AARM_WARM_BOOT,
    SMEM_DIAG_ERR_MESSAGE,
    SMEM_SPINLOCK_ARRAY,
    SMEM_MEMORY_BARRIER_LOCATION,
    SMEM_AARM_PARTITION_TABLE,
    SMEM_AARM_BAD_BLOCK_TABLE,
    SMEM_RESERVE_BAD_BLOCKS,
    SMEM_WM_UUID,
    SMEM_CHANNEL_ALLOC_TBL,
    SMEM_SMD_BASE_ID,
    SMEM_SMEM_LOG_IDX = SMEM_SMD_BASE_ID + 64,
    SMEM_SMEM_LOG_EVENTS,
    SMEM_SMEM_STATIC_LOG_IDX,
    SMEM_SMEM_STATIC_LOG_EVENTS,
    SMEM_SMEM_SLOW_CLOCK_SYNC,
    SMEM_SMEM_SLOW_CLOCK_VALUE,
    SMEM_BIO_LED_BUF,
    SMEM_SMSM_SHARED_STATE,
    SMEM_SMSM_INT_INFO,
    SMEM_SMSM_SLEEP_DELAY,
    SMEM_SMSM_LIMIT_SLEEP,
    SMEM_SLEEP_POWER_COLLAPSE_DISABLED,
    SMEM_KEYPAD_KEYS_PRESSED,
    SMEM_KEYPAD_STATE_UPDATED,
    SMEM_KEYPAD_STATE_IDX,
    SMEM_GPIO_INT,
};

/* SMSM state bits */
#define SMSM_INIT               0x00000001
#define SMSM_SMDINIT            0x00000008
#define SMSM_RPCINIT            0x00000020
#define SMSM_RUN                0x00000100
#define SMSM_STATE_APPS         0
#define SMSM_STATE_MODEM        1
#define SMSM_STATE_APPS_DEM     3
#define SMSM_STATE_POWER_MASTER_DEM 6
#define SMSM_STATE_TIME_MASTER_DEM  7

/* time sync with the modem's sleep clock, timer.c (N-way SMSM) */
#define DEM_TIME_MASTER_TIME_PENDING_APPS   0x0001
#define DEM_TIME_SLAVE_TIME_REQUEST         0x0400
#define DEM_TIME_SLAVE_TIME_POLL            0x0800

/* DEM (dynamic environment manager) sleep handshake, pm.c */
#define DEM_SLAVE_SMSM_RUN          0x0001
#define DEM_SLAVE_SMSM_PWRC         0x0002
#define DEM_SLAVE_SMSM_WFPI         0x0010
#define DEM_SLAVE_SMSM_SLEEP        0x0020
#define DEM_SLAVE_SMSM_SLEEP_EXIT   0x0040
#define DEM_SLAVE_SMSM_PWRC_SUSPEND 0x0200
#define DEM_MASTER_SMSM_RUN         0x01
#define DEM_MASTER_SMSM_RSA         0x02
#define DEM_MASTER_SMSM_SLEEP_EXIT  0x08
#define DEM_MASTER_SMSM_READY       0x10
#define DEM_MASTER_SMSM_SLEEP       0x20
#define SMEM_APPS_DEM_SLAVE_DATA    334

#define SMSM_DOORBELL               5

/* ---- standard proc_comm (offset 0 of SMEM) ---- */
#define PC_APP_COMMAND          0x00
#define PC_APP_STATUS           0x04
#define PC_APP_DATA1            0x08
#define PC_APP_DATA2            0x0C
#define PC_MDM_COMMAND          0x10
#define PC_MDM_STATUS           0x14
#define PCOM_CMD_DONE           0x1
#define PCOM_READY              0x1
#define PCOM_CMD_SUCCESS        0x3
#define PCOM_CMD_FAIL           0x4
#define PCOM_RESET_CHIP         3
#define PCOM_POWER_DOWN         10
#define PCOM_RESET_CHIP_IMM     42
#define PCOM_RPC_GPIO_TLMM_CONFIG_EX 37

void (*leo_tlmm_config)(uint32_t cfg);

/* ---- WinCE DEX interface ---- */
#define DEX_BASE                0xEFE00
#define DEX_COMMAND             0x00
#define DEX_STATUS              0x04
#define DEX_SERIAL              0x08
#define DEX_SERIAL_CHECK        0x0C
#define DEX_DATA                0x20
#define DEX_DATA_RESULT         0x24
#define DEX_NOTIFY              0x28
#define DEX_DATA2               0x30
#define DEX_DATA_RESULT2        0x34
#define DEX_READY               0x3C
#define DEX_HAS_DATA            0x100
#define DEX_STATUS_FAIL         0x200

#define AMSS_VERSION_OFF        0xEF230

#define DEX_DOORBELL            4
#define PROC_COMM_DOORBELL      6

/* Pre-allocate an SMEM item the way AMSS would.  Returns its offset. */
uint32_t smem_alloc_item(MsmA9State *s, unsigned id, uint32_t size)
{
    uint32_t toc = SMEM_TOC_OFF + id * 16;

    size = ROUND_UP(size, 8);
    if (smem_rd(s, toc)) {
        return smem_rd(s, toc + 4);
    }
    if (s->heap_free + size > SMEM_HEAP_END) {
        error_report("msm-a9: SMEM heap exhausted (item %u)", id);
        return 0;
    }
    uint32_t off = s->heap_free;
    s->heap_free += size;
    memset(s->smem + off, 0, size);
    smem_wr(s, toc + 0, 1);
    smem_wr(s, toc + 4, off);
    smem_wr(s, toc + 8, size);
    smem_wr(s, SMEM_HEAP_INFO_OFF + 4, s->heap_free);
    smem_wr(s, SMEM_HEAP_INFO_OFF + 8, SMEM_HEAP_END - s->heap_free);
    return off;
}

static uint32_t a9_rtc_seconds(void)
{
    struct tm tm;
    qemu_get_timedate(&tm, 0);
    return (uint32_t)mktimegm(&tm);
}

/* Returns true and fills results when the command produces data. */
static bool a9_dex_exec(MsmA9State *s, uint32_t cmd, uint32_t d1, uint32_t d2,
                        uint32_t *r1, uint32_t *r2, bool *fail)
{
    *fail = false;
    switch (cmd) {
    case 0x81: /* DEX_READ_RTC */
        *r1 = a9_rtc_seconds();
        *r2 = 0;
        return true;
    case 0x82: /* DEX_WRITE_RTC */
    case 0x84: /* DEX_SET_ALARM_RTC */
        return false;
    case 0x8a: /* DEX_GET_BATTERY_DATA */
        *r1 = 0;
        *r2 = 0;
        return true;
    case 0x8b: /* DEX_GET_BATTERY_ID */
        *r1 = 1;
        *r2 = 0;
        return true;
    case 0xa2: /* DEX_GET_SLEEP_CLOCK */
        *r1 = (uint32_t)muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                                 32768, NANOSECONDS_PER_SECOND);
        *r2 = 0;
        return true;
    case 0x14: /* DEX_POWER_OFF */
        leo_request_power_off();
        return false;
    default:
        return false;
    }
}

static void a9_handle_dex(MsmA9State *s)
{
    uint32_t base = DEX_BASE;
    uint32_t command = smem_rd(s, base + DEX_COMMAND);
    uint32_t cmd = command & 0xff;
    uint32_t d1 = smem_rd(s, base + DEX_DATA);
    uint32_t d2 = smem_rd(s, base + DEX_DATA2);
    uint32_t r1 = 0, r2 = 0;
    bool fail;
    bool has = a9_dex_exec(s, cmd, d1, d2, &r1, &r2, &fail);
    uint32_t status = cmd;

    if (fail) {
        status |= DEX_STATUS_FAIL;
    } else if (has) {
        status |= DEX_HAS_DATA;
        smem_wr(s, base + DEX_DATA_RESULT, r1);
        smem_wr(s, base + DEX_DATA_RESULT2, r2);
    }
    s->dex_count++;
    qemu_log_mask(LOG_UNIMP, "msm-a9: DEX cmd 0x%02x data 0x%x 0x%x -> "
                  "status 0x%x res 0x%x 0x%x\n", cmd, d1, d2, status, r1, r2);
    smem_wr(s, base + DEX_STATUS, status);
    /* completion is signalled by echoing the serial number */
    smem_wr(s, base + DEX_SERIAL_CHECK, smem_rd(s, base + DEX_SERIAL));
}

static void a9_handle_proc_comm(MsmA9State *s)
{
    uint32_t cmd = smem_rd(s, PC_APP_COMMAND);
    uint32_t d1 = smem_rd(s, PC_APP_DATA1);
    uint32_t d2 = smem_rd(s, PC_APP_DATA2);

    static uint8_t logged[256];
    static unsigned stats[256];
    static int64_t stats_t;
    static int stats_on = -1;

    if (stats_on < 0) {
        stats_on = getenv("LEO_IRQSTATS") != NULL;
    }
    if (stats_on) {
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        stats[cmd & 0xff]++;
        if (now - stats_t >= 5000) {
            fprintf(stderr, "pcomstats:");
            for (int i = 0; i < 256; i++) {
                if (stats[i]) {
                    fprintf(stderr, " %d:%u", i, stats[i]);
                }
            }
            fprintf(stderr, "\n");
            memset(stats, 0, sizeof(stats));
            stats_t = now;
        }
    }

    /* clock and vreg toggles come by the thousand (KGSL power management) */
    if (logged[cmd & 0xff] < 8) {
        logged[cmd & 0xff]++;
        qemu_log_mask(LOG_UNIMP, "msm-a9: proc_comm cmd %u data 0x%x 0x%x\n",
                      cmd, d1, d2);
    }
    smem_wr(s, PC_APP_STATUS, PCOM_CMD_SUCCESS);
    smem_wr(s, PC_APP_COMMAND, PCOM_CMD_DONE);

    switch (cmd) {
    case PCOM_RESET_CHIP:
    case PCOM_RESET_CHIP_IMM:
        /* data1: restart reason, as also saved for cLK at 0x2FFB0000 */
        leo_request_reboot(d1);
        break;
    case PCOM_POWER_DOWN:
        leo_request_power_off();
        break;
    case PCOM_RPC_GPIO_TLMM_CONFIG_EX:
        if (leo_tlmm_config) {
            leo_tlmm_config(d1);
        }
        break;
    }
}

/*
 * The apps processor announces sleep/wake transitions in its DEM slave
 * state; acknowledge them as the power master (modem) would.
 */
static void a9_handle_smsm(MsmA9State *s)
{
    uint32_t off = smem_rd(s, SMEM_TOC_OFF + SMEM_SMSM_SHARED_STATE * 16 + 4);
    uint32_t apps = smem_rd(s, off + SMSM_STATE_APPS_DEM * 4);
    uint32_t master = DEM_MASTER_SMSM_READY;

    if (apps & (DEM_SLAVE_SMSM_PWRC | DEM_SLAVE_SMSM_PWRC_SUSPEND)) {
        master |= DEM_MASTER_SMSM_RSA;
    } else if (apps & DEM_SLAVE_SMSM_SLEEP) {
        master |= DEM_MASTER_SMSM_SLEEP;
    } else if (apps & DEM_SLAVE_SMSM_SLEEP_EXIT) {
        master |= DEM_MASTER_SMSM_SLEEP_EXIT | DEM_MASTER_SMSM_RUN;
    } else {
        /* RUN, WFPI (wake from power collapse) or anything else */
        master |= DEM_MASTER_SMSM_RUN;
    }
    smem_wr(s, off + SMSM_STATE_POWER_MASTER_DEM * 4, master);

    /*
     * Sleep clock sync (msm_timer_sync_smem_clock): the apps side raises
     * TIME_REQUEST, waits for PENDING_APPS, switches to TIME_POLL and
     * reads SMEM_SMEM_SLOW_CLOCK_VALUE; back at TIME_INIT the master
     * drops PENDING_APPS.  Without an answer every idle entry would spin
     * through two 7.8 ms timeouts.
     */
    uint32_t clk = smem_rd(s, SMEM_TOC_OFF + SMEM_SMEM_SLOW_CLOCK_VALUE * 16 + 4);
    if (!clk) {
        return;
    }
    if (apps & (DEM_TIME_SLAVE_TIME_REQUEST | DEM_TIME_SLAVE_TIME_POLL)) {
        smem_wr(s, clk, msm_csr_gpt_count() | 1);
        smem_wr(s, off + SMSM_STATE_TIME_MASTER_DEM * 4,
                DEM_TIME_MASTER_TIME_PENDING_APPS);
    } else {
        smem_wr(s, off + SMSM_STATE_TIME_MASTER_DEM * 4, 0);
        smem_wr(s, clk, 0);
    }
}

static void a9_doorbell(void *opaque, int n, int level)
{
    MsmA9State *s = opaque;

    if (!level) {
        return;
    }
    switch (n) {
    case DEX_DOORBELL:
        a9_handle_dex(s);
        break;
    case PROC_COMM_DOORBELL:
        a9_handle_proc_comm(s);
        break;
    case SMSM_DOORBELL:
        a9_handle_smsm(s);
        break;
    case SMD_MODEM_DOORBELL:
    case SMD_DSP_DOORBELL:
        a9_smd_kick(s);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "msm-a9: doorbell %d rung\n", n);
        break;
    }
}

static void a9_build_smem(MsmA9State *s)
{
    uint32_t off;

    memset(s->smem, 0, MSM_SHARED_RAM_SIZE);
    s->heap_free = SMEM_HEAP_START;

    /* heap info: initialized */
    smem_wr(s, SMEM_HEAP_INFO_OFF + 0, 1);
    smem_wr(s, SMEM_HEAP_INFO_OFF + 4, s->heap_free);
    smem_wr(s, SMEM_HEAP_INFO_OFF + 8, SMEM_HEAP_END - s->heap_free);

    /* modem proc_comm ready */
    smem_wr(s, PC_MDM_STATUS, PCOM_READY);

    /* SMSM shared state, V1 layout: 8 words */
    off = smem_alloc_item(s, SMEM_SMSM_SHARED_STATE, 8 * 4);
    smem_wr(s, off + SMSM_STATE_MODEM * 4,
            SMSM_INIT | SMSM_SMDINIT | SMSM_RPCINIT | SMSM_RUN);

    smem_wr(s, off + SMSM_STATE_POWER_MASTER_DEM * 4,
            DEM_MASTER_SMSM_RUN | DEM_MASTER_SMSM_READY);

    smem_alloc_item(s, SMEM_SMSM_INT_INFO, 3 * 4);
    /* struct msm_dem_slave_data: enables idle power collapse */
    smem_alloc_item(s, SMEM_APPS_DEM_SLAVE_DATA, 52);
    smem_alloc_item(s, SMEM_SMEM_SLOW_CLOCK_VALUE, 4);
    smem_alloc_item(s, SMEM_SMSM_SLEEP_DELAY, 4);
    smem_alloc_item(s, SMEM_SMSM_LIMIT_SLEEP, 4);
    smem_alloc_item(s, SMEM_SLEEP_POWER_COLLAPSE_DISABLED, 4);

    /* AMSS version string, read by dump_debug_stuff() */
    pstrcpy((char *)s->smem + AMSS_VERSION_OFF, 16, "QEMU-LEO-A9");

    /* DEX ready flag */
    smem_wr(s, DEX_BASE + DEX_READY, 1);

    /* SMD channels served by the fake modem/DSP */
    a9_smd_init(s);
}

static void a9_reset(DeviceState *dev)
{
    MsmA9State *s = MSM_A9(dev);
    a9_smd_reset(s);
    a9_build_smem(s);
    s->dex_count = 0;
}

static void a9_realize(DeviceState *dev, Error **errp)
{
    MsmA9State *s = MSM_A9(dev);

    if (!memory_region_init_ram(&s->shared_ram, OBJECT(dev), "msm.smem",
                                MSM_SHARED_RAM_SIZE, errp)) {
        return;
    }
    s->smem = memory_region_get_ram_ptr(&s->shared_ram);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->shared_ram);
}

/* a wake-up source fired while the apps CPU sleeps: wake it like AMSS */
static void a9_wake_apps(void *opaque, int n, int level)
{
    MsmA9State *s = opaque;
    uint32_t off;

    if (!level) {
        return;
    }
    off = smem_rd(s, SMEM_TOC_OFF + SMEM_SMSM_SHARED_STATE * 16 + 4);
    smem_wr(s, off + SMSM_STATE_POWER_MASTER_DEM * 4,
            DEM_MASTER_SMSM_RUN | DEM_MASTER_SMSM_READY);
    qemu_irq_pulse(s->m2a[6]);
}

static void a9_init(Object *obj)
{
    MsmA9State *s = MSM_A9(obj);

    s->smd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, a9_smd_timer_cb, s);

    for (int i = 0; i < 7; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->m2a[i]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), a9_doorbell, "a2m", MSM_A2M_NUM);
    qdev_init_gpio_in_named(DEVICE(obj), a9_wake_apps, "wake", 1);
}

static int a9_pre_save(void *opaque)
{
    a9_smd_save(opaque);
    return 0;
}

static int a9_post_load(void *opaque, int version_id)
{
    a9_smd_load(opaque);
    return 0;
}

static const VMStateDescription vmstate_msm_a9 = {
    .name = "msm-a9-hle",
    .version_id = 2,
    .minimum_version_id = 2,
    .pre_save = a9_pre_save,
    .post_load = a9_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(heap_free, MsmA9State),
        VMSTATE_UINT32(dex_count, MsmA9State),
        VMSTATE_UINT32(vm_len, MsmA9State),
        VMSTATE_VBUFFER_ALLOC_UINT32(vm_buf, MsmA9State, 0, NULL, vm_len),
        VMSTATE_END_OF_LIST()
    }
};

static void a9_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = a9_realize;
    device_class_set_legacy_reset(dc, a9_reset);
    dc->vmsd = &vmstate_msm_a9;
}

static const TypeInfo a9_info = {
    .name = TYPE_MSM_A9,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MsmA9State),
    .instance_init = a9_init,
    .class_init = a9_class_init,
};

static void a9_register_types(void)
{
    type_register_static(&a9_info);
}

type_init(a9_register_types)
