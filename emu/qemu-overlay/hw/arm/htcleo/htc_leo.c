/*
 * HTC HD2 (codename "Leo", HTCLEO, mach id 2524) machine.
 *
 * SoC: Qualcomm QSD8250 (Scorpion ARMv7 @1GHz, Adreno 200, ARM9 modem)
 * RAM: 64 MiB SMI at 0x00000000 + 512 MiB EBI1 at 0x10000000
 *      (kernel is linked at 0x11800000, CONFIG_PHYS_OFFSET)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qapi/visitor.h"
#include "qemu/cutils.h"
#include "hw/boards.h"
#include "hw/arm/boot.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/sd/sd.h"
#include "system/blockdev.h"
#include "hw/qdev-properties-system.h"
#include "chardev/char-fe.h"
#include "ui/input.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/address-spaces.h"
#include "target/arm/cpu-qom.h"
#include "cpu.h"
#include "msm.h"
#include "migration/vmstate.h"
#include "adreno.h"
#include "leo_boot.h"
#include "leo_bcm4329.h"

#define HTCLEO_MACH_ID          2524
#define LEO_EBI1_DEFAULT_SIZE   (512 * MiB)

#define RAM_CONSOLE_SIG         0x43474244  /* "DBGC" */
#define RAM_CONSOLE_POLL_MS     20

struct HtcLeoMachineState {
    MachineState parent;

    ARMCPU *cpu;
    MemoryRegion smi;
    DeviceState *vic;
    DeviceState *csr;
    DeviceState *a9;
    DeviceState *sirc;
    DeviceState *gpio;
    DeviceState *dmov;
    DeviceState *nand;
    uint32_t nand_mb;
    uint32_t system_mb;
    uint32_t cache_mb;
    char *boot_mode;
    char *reboot_file;
    char *hostlink;
    bool pin_cpu;

    /* keypad matrix (3 driven columns x 3 sensed rows) + power key */
    uint32_t keys_down;         /* bit = col * 3 + row; bit 9 = power */
    bool kp_col_level[3];
    qemu_irq *kp_col_in;

    /* ram_console tap */
    QEMUTimer *rc_timer;
    CharBackend rc_chr;
    bool rc_have_chr;
    uint32_t rc_last;
    bool rc_seen;
};

#define TYPE_HTC_LEO_MACHINE MACHINE_TYPE_NAME("htc-leo")
OBJECT_DECLARE_SIMPLE_TYPE(HtcLeoMachineState, HTC_LEO_MACHINE)

static struct arm_boot_info leo_binfo;

/*
 * NAND partition layout, as cLK builds it (lk/target/htcleo/init.c):
 * apps partitions start at block 0x219; userdata takes what is left up
 * to (blocks - 1 - 186 - 4).  Lengths are in MiB, 128 KiB per block.
 */
#define LEO_FLASH_OFFSET        0x219
#define LEO_BLOCKS_PER_MB       8

typedef struct LeoPartition {
    const char *name;
    uint32_t start;     /* in blocks */
    uint32_t length;    /* in blocks */
} LeoPartition;

static int leo_build_ptable(HtcLeoMachineState *s, LeoPartition *p)
{
    uint32_t nblocks = (s->nand_mb ? s->nand_mb : 512) * LEO_BLOCKS_PER_MB;
    uint32_t sys_mb = s->system_mb ? s->system_mb : 150;
    uint32_t cache_mb = s->cache_mb ? s->cache_mb : 44;
    uint32_t start = LEO_FLASH_OFFSET;
    uint32_t end = nblocks - 1 - 186 - 4;
    const struct { const char *name; uint32_t mb; } def[] = {
        { "misc", 1 }, { "recovery", 5 }, { "boot", 5 },
        { "system", sys_mb }, { "cache", cache_mb }, { "userdata", 0 },
    };
    int n = 0;

    for (int i = 0; i < ARRAY_SIZE(def); i++) {
        uint32_t len = def[i].mb ? def[i].mb * LEO_BLOCKS_PER_MB : end - start;
        if (start + len > end) {
            error_report("htc-leo: partitions do not fit in %u MiB NAND",
                         nblocks / LEO_BLOCKS_PER_MB);
            exit(EXIT_FAILURE);
        }
        p[n].name = def[i].name;
        p[n].start = start;
        p[n].length = len;
        start += len;
        n++;
    }
    return n;
}

#define ATAG_MSM_PARTITION      0x4d534d70
#define ATAG_MAGLDR_BOOT        0x4c47414d      /* "MAGL" */
#define CLK_SIGNATURE           0x004b4c63      /* "cLK" */

static int leo_atag_board(const struct arm_boot_info *info, void *p)
{
    HtcLeoMachineState *s = HTC_LEO_MACHINE(qdev_get_machine());
    LeoPartition parts[8];
    int n = leo_build_ptable(s, parts);
    uint32_t *w = p;

    w[0] = cpu_to_le32(2 + n * 7);
    w[1] = cpu_to_le32(ATAG_MSM_PARTITION);
    w += 2;
    for (int i = 0; i < n; i++) {
        char name[16] = { 0 };
        pstrcpy(name, sizeof(name), parts[i].name);
        memcpy(w, name, 16);
        w[4] = cpu_to_le32(parts[i].start);
        w[5] = cpu_to_le32(parts[i].length);
        w[6] = 0;
        w += 7;
    }
    /*
     * Like cLK (target/htcleo/atags.c): tell the kernel it was started
     * from NAND by cLK.  Without it HD2 kernels assume a haret boot from
     * WinMo: they start the RPC router themselves (and break when the
     * modem says HELLO too) and avoid power collapse.
     */
    w[0] = cpu_to_le32(4);
    w[1] = cpu_to_le32(ATAG_MAGLDR_BOOT);
    w[2] = cpu_to_le32(CLK_SIGNATURE);
    w[3] = cpu_to_le32(13);                 /* cLK version */
    return (2 + n * 7 + 4) * 4;
}

/*
 * The HD2 has no accessible UART; kernels log into the Android
 * ram_console at the top of EBI1.  Mirror that buffer to serial0 (or
 * stderr) so we can see the boot log live.
 */
static void leo_rc_emit(HtcLeoMachineState *s, const uint8_t *buf, int len)
{
    if (len <= 0) {
        return;
    }
    if (s->rc_have_chr) {
        qemu_chr_fe_write_all(&s->rc_chr, buf, len);
    } else {
        fwrite(buf, 1, len, stderr);
        fflush(stderr);
    }
}

static void leo_rc_poll(void *opaque)
{
    leo_cpu_report();
    HtcLeoMachineState *s = opaque;
    uint32_t hdr[3];
    const uint32_t data_size = LEO_RAM_CONSOLE_SIZE - 12;

    timer_mod(s->rc_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + RAM_CONSOLE_POLL_MS);

    if (address_space_read(&address_space_memory, LEO_RAM_CONSOLE_PHYS,
                           MEMTXATTRS_UNSPECIFIED, hdr, sizeof(hdr))) {
        return;
    }
    uint32_t sig = le32_to_cpu(hdr[0]);
    uint32_t start = le32_to_cpu(hdr[1]);
    if (sig != RAM_CONSOLE_SIG || start >= data_size) {
        return;
    }
    if (!s->rc_seen) {
        s->rc_seen = true;
        s->rc_last = 0;
    }
    while (s->rc_last != start) {
        uint32_t end = start > s->rc_last ? start : data_size;
        uint32_t len = MIN(end - s->rc_last, 4096);
        uint8_t buf[4096];

        address_space_read(&address_space_memory,
                           LEO_RAM_CONSOLE_PHYS + 12 + s->rc_last,
                           MEMTXATTRS_UNSPECIFIED, buf, len);
        leo_rc_emit(s, buf, len);
        s->rc_last += len;
        if (s->rc_last >= data_size) {
            s->rc_last = 0;
        }
    }
}

static void leo_rc_reset(void *opaque)
{
    HtcLeoMachineState *s = opaque;
    s->rc_seen = false;
    s->rc_last = 0;
}

/*
 * Values the HD2 SPL leaves in SMI RAM for the OS.  The kernel reads the
 * LCD panel vendor from MSM_PANEL_PHYS (0x80000) + 0x1034:
 *   1 = AUO, 10 = SHARP.
 */
#define LEO_SPL_PANEL_BOARD_ID  0x81030
#define LEO_SPL_PANEL_TYPE      0x81034
#define LEO_PANEL_AUO           1

static void leo_bootstate_reset(void *opaque)
{
    HtcLeoMachineState *s = opaque;
    uint8_t *smi = memory_region_get_ram_ptr(&s->smi);

    stl_le_p(smi + LEO_SPL_PANEL_BOARD_ID, 0);
    stl_le_p(smi + LEO_SPL_PANEL_TYPE, LEO_PANEL_AUO);
}

static void leo_map_stubs(void)
{
    /* catch-all for the whole peripheral window, lowest priority */
    msm_stub_create("periph", 0xA0000000, 0x10000000, -1000);
    msm_stub_create("l2cc", 0xC0400000, 0x1000, -1000);

    /* named stubs for blocks we know about, so the logs are readable */
    static const struct { const char *name; hwaddr base; uint64_t size; } st[] = {
        { "uartdm1",   MSM_UART1DM_PHYS,   0x1000 },
        { "sdc3",      MSM_SDC3_PHYS,      0x1000 },
        { "sdc4",      MSM_SDC4_PHYS,      0x1000 },
        { "hsusb",     MSM_HSUSB_PHYS,     0x1000 },
        { "ssbi",      MSM_SSBI_PHYS,      0x1000 },
        { "clk_ctl",   MSM_CLK_CTL_PHYS,   0x1000 },
        { "tcsr",      MSM_TCSR_PHYS,      0x1000 },
        { "scpll",     MSM_SCPLL_PHYS,     0x1000 },
        { "gpiocfg1",  MSM_GPIOCFG1_PHYS,  0x1000 },
        { "gpiocfg2",  MSM_GPIOCFG2_PHYS,  0x1000 },
        { "uart1",     MSM_UART1_PHYS,     0x1000 },
        { "uart2",     MSM_UART2_PHYS,     0x1000 },
        { "uart3",     MSM_UART3_PHYS,     0x1000 },
        { "clk_sh2",   0xABA01000,         0x1000 },
    };
    /*
     * Register state the real bootloader (SPL/cLK) and the ARM9 leave
     * behind when the kernel starts.
     */
    static const struct { const char *name; hwaddr off; uint32_t val; } pre[] = {
        /* SCPLL running at L=20 (20 * 38.4 MHz = 768 MHz) */
        { "scpll",   0x10,  20 << 3 },
        /* PLL0 (MPLL) on: MODE, L, M, N  -> 19.2 * 12.8 = 245.76 MHz */
        { "clk_ctl", 0x300, 0x7 },
        { "clk_ctl", 0x304, 12 },
        { "clk_ctl", 0x308, 4 },
        { "clk_ctl", 0x30C, 5 },
        /* PLL1 (GPLL) on */
        { "clk_ctl", 0x320, 0x7 },
        { "clk_ctl", 0x324, 40 },
        { "clk_ctl", 0x328, 0 },
        { "clk_ctl", 0x32C, 1 },
        /* PLL4/PLL5 off but with sane N to avoid div by zero in dumps */
        { "clk_ctl", 0x38C, 1 },
        { "clk_ctl", 0x3AC, 1 },
    };

    /*
     * Busy bits that complete at once: the HS-USB endpoint flush/prime and
     * status, the controller reset and the ULPI viewport "run" bit.  The
     * gadget driver otherwise spins in flush_endpoint_hw() and ulpi_*()
     * until their timeouts, a large share of boot time.
     */
    static const struct { const char *name; hwaddr off; uint32_t mask; } sc[] = {
        { "hsusb", 0x140, 1u << 1 },        /* USBCMD.RST */
        { "hsusb", 0x170, 1u << 30 },       /* ULPI_VIEWPORT.RUN */
        { "hsusb", 0x1b0, 0xffffffff },     /* ENDPTPRIME */
        { "hsusb", 0x1b4, 0xffffffff },     /* ENDPTFLUSH */
        { "hsusb", 0x1b8, 0xffffffff },     /* ENDPTSTAT */
    };

    for (int i = 0; i < ARRAY_SIZE(st); i++) {
        DeviceState *d = msm_stub_create(st[i].name, st[i].base, st[i].size, 0);
        for (int j = 0; j < ARRAY_SIZE(pre); j++) {
            if (!strcmp(pre[j].name, st[i].name)) {
                msm_stub_preset(d, pre[j].off, pre[j].val);
            }
        }
        for (int j = 0; j < ARRAY_SIZE(sc); j++) {
            if (!strcmp(sc[j].name, st[i].name)) {
                msm_stub_selfclear(d, sc[j].off, sc[j].mask);
            }
        }
    }
}

/* HD2 board GPIOs (board-htcleo.h) */
#define LEO_GPIO_TP_INT_N       92
#define LEO_GPIO_POWER_KEY      94
#define LEO_GPIO_SD_STATUS      153
#define LEO_GPIO_WIFI_SHUTDOWN_N 129
#define LEO_GPIO_WIFI_IRQ       152
#define LEO_GPIO_HDS_MIC        35
#define LEO_GPIO_UP_INT_N       90
#define LEO_GPIO_TS_POWER       160
#define LEO_GPIO_COMPASS_INT_N  39
#define LEO_GPIO_HDS_DET        145
#define LEO_GPIO_KP_MPIN0       42
#define LEO_GPIO_KP_MPIN1       41
#define LEO_GPIO_KP_MPIN2       40
#define LEO_GPIO_KP_MKOUT0      33
#define LEO_GPIO_KP_MKOUT1      32
#define LEO_GPIO_KP_MKOUT2      31

/* HD2 hardware buttons: index = col * 3 + row (board-htcleo-keypad.c) */
enum {
    LEO_KEY_VOLUP = 0,      /* col 0 row 0 */
    LEO_KEY_VOLDOWN = 1,    /* col 0 row 1 */
    LEO_KEY_WINDOWS = 3,    /* col 1 row 0, KEY_MENU */
    LEO_KEY_CALL = 4,       /* col 1 row 1, KEY_SEND */
    LEO_KEY_END = 5,        /* col 1 row 2 */
    LEO_KEY_BACK = 6,       /* col 2 row 0 */
    LEO_KEY_HOME = 7,       /* col 2 row 1 */
    LEO_KEY_POWER = 9,      /* GPIO 94 */
};

/* idle levels of input pins as set by board pull-ups */
static DeviceState *leo_gpio_dev;

/* board state that is not in a device: ram_console mirror, buttons */
static const VMStateDescription vmstate_htc_leo = {
    .name = "htc-leo-board",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(keys_down, HtcLeoMachineState),
        VMSTATE_BOOL_ARRAY(kp_col_level, HtcLeoMachineState, 3),
        VMSTATE_UINT32(rc_last, HtcLeoMachineState),
        VMSTATE_BOOL(rc_seen, HtcLeoMachineState),
        VMSTATE_END_OF_LIST()
    }
};

/* TLMM setup from the kernel: only the Wi-Fi power pin needs a direction */
static void leo_tlmm(uint32_t cfg)
{
    int gpio = (cfg >> 4) & 0x3ff, func = cfg & 0xf, dir = (cfg >> 14) & 1;

    if (gpio == LEO_GPIO_WIFI_SHUTDOWN_N && func == 0 && leo_gpio_dev) {
        msm_gpio_set_direction(leo_gpio_dev, gpio, dir);
    }
}

static void leo_gpio_defaults(DeviceState *gpio)
{
    bool have_sd = drive_get(IF_SD, 0, 0) != NULL;

    msm_gpio_set_default(gpio, LEO_GPIO_POWER_KEY, 1);   /* active low */
    /* card detect is active low */
    msm_gpio_set_default(gpio, LEO_GPIO_SD_STATUS, !have_sd);
    msm_gpio_set_default(gpio, LEO_GPIO_HDS_MIC, 1);     /* no headset mic */
    msm_gpio_set_default(gpio, LEO_GPIO_HDS_DET, 1);     /* no headset */
    msm_gpio_set_default(gpio, LEO_GPIO_UP_INT_N, 1);    /* microp idle */
    msm_gpio_set_default(gpio, LEO_GPIO_TP_INT_N, 1);    /* touch idle */
    msm_gpio_set_default(gpio, LEO_GPIO_KP_MPIN0, 1);    /* keypad matrix */
    msm_gpio_set_default(gpio, LEO_GPIO_KP_MPIN1, 1);    /* inputs pulled */
    msm_gpio_set_default(gpio, LEO_GPIO_KP_MPIN2, 1);    /* up */
    msm_gpio_set_default(gpio, LEO_GPIO_KP_MKOUT0, 1);   /* columns are */
    msm_gpio_set_default(gpio, LEO_GPIO_KP_MKOUT1, 1);   /* pulled up when */
    msm_gpio_set_default(gpio, LEO_GPIO_KP_MKOUT2, 1);   /* not scanned */
}

static const int leo_kp_row_gpio[3] = {
    LEO_GPIO_KP_MPIN0, LEO_GPIO_KP_MPIN1, LEO_GPIO_KP_MPIN2
};

/* recompute the sensed rows from driven columns and pressed keys */
static void leo_keypad_update(HtcLeoMachineState *s)
{
    for (int row = 0; row < 3; row++) {
        bool level = true;
        for (int col = 0; col < 3; col++) {
            if ((s->keys_down & (1u << (col * 3 + row))) &&
                !s->kp_col_level[col]) {
                level = false;
            }
        }
        qemu_set_irq(qdev_get_gpio_in(s->gpio, leo_kp_row_gpio[row]), level);
    }
    qemu_set_irq(qdev_get_gpio_in(s->gpio, LEO_GPIO_POWER_KEY),
                 !(s->keys_down & (1u << LEO_KEY_POWER)));
}

static void leo_kp_col_changed(void *opaque, int col, int level)
{
    HtcLeoMachineState *s = opaque;
    s->kp_col_level[col] = level;
    leo_keypad_update(s);
}

void leo_set_key(int key, bool down)
{
    HtcLeoMachineState *s = HTC_LEO_MACHINE(qdev_get_machine());
    if (down) {
        s->keys_down |= 1u << key;
    } else {
        s->keys_down &= ~(1u << key);
    }
    leo_keypad_update(s);
}

/* host keyboard -> HD2 buttons */
static int leo_qcode_to_key(int qcode)
{
    switch (qcode) {
    case Q_KEY_CODE_UP:
    case Q_KEY_CODE_PGUP:       return LEO_KEY_VOLUP;
    case Q_KEY_CODE_DOWN:
    case Q_KEY_CODE_PGDN:       return LEO_KEY_VOLDOWN;
    case Q_KEY_CODE_F1:
    case Q_KEY_CODE_M:          return LEO_KEY_WINDOWS;
    case Q_KEY_CODE_F2:
    case Q_KEY_CODE_RET:        return LEO_KEY_CALL;
    case Q_KEY_CODE_F3:
    case Q_KEY_CODE_END:        return LEO_KEY_END;
    case Q_KEY_CODE_ESC:
    case Q_KEY_CODE_BACKSPACE:  return LEO_KEY_BACK;
    case Q_KEY_CODE_HOME:
    case Q_KEY_CODE_H:          return LEO_KEY_HOME;
    case Q_KEY_CODE_F4:
    case Q_KEY_CODE_P:          return LEO_KEY_POWER;
    default:                    return -1;
    }
}

static void leo_kbd_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    InputKeyEvent *key = evt->u.key.data;
    int k = leo_qcode_to_key(qemu_input_key_value_to_qcode(key->key));
    if (k >= 0) {
        leo_set_key(k, key->down);
    }
}

static const QemuInputHandler leo_kbd_handler = {
    .name  = "HTC HD2 buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = leo_kbd_event,
};

#define LEO_REBOOT_RECOVERY     0x77665502

/*
 * cLK picks the boot image from the restart reason.  QEMU cannot switch
 * kernels on reset, so when a reboot-file is configured we record the
 * wanted mode ("android", "recovery" or "off") and exit; the front end
 * starts the emulator again in that mode.  Without one: plain reset.
 */
static void leo_finish(const char *mode)
{
    HtcLeoMachineState *s = HTC_LEO_MACHINE(qdev_get_machine());

    info_report("htc-leo: guest requested %s", mode);
    if (s->reboot_file) {
        g_file_set_contents(s->reboot_file, mode, -1, NULL);
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    } else if (!strcmp(mode, "off")) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    } else {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

void leo_request_reboot(uint32_t reason)
{
    leo_finish(reason == LEO_REBOOT_RECOVERY ? "recovery" : "android");
}

void leo_request_power_off(void)
{
    leo_finish("off");
}

static void htc_leo_init(MachineState *machine)
{
    HtcLeoMachineState *s = HTC_LEO_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    Chardev *chr;

    if (machine->ram_size > 768 * MiB) {
        error_report("htc-leo: at most 768 MiB of EBI1 RAM is supported");
        exit(EXIT_FAILURE);
    }

    if (s->pin_cpu) {
        leo_pin_fast_cpus();        /* before the vCPU thread exists */
    }
    if (s->hostlink) {
        leo_hostlink_open(s->hostlink, &error_fatal);
    }

    s->cpu = ARM_CPU(cpu_create(machine->cpu_type));

    /* memory */
    memory_region_init_ram(&s->smi, NULL, "leo.smi", MSM_SMI_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, MSM_SMI_BASE, &s->smi);
    memory_region_add_subregion(sysmem, MSM_EBI1_BASE, machine->ram);
    leo_quirks_set_ram((uint8_t *)memory_region_get_ram_ptr(machine->ram) +
                       (LEO_KERNEL_BASE - MSM_EBI1_BASE),
                       machine->ram_size - (LEO_KERNEL_BASE - MSM_EBI1_BASE));

    /* interrupt controller */
    s->vic = qdev_new(TYPE_MSM_VIC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->vic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->vic), 0, MSM_VIC_PHYS);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->vic), 0,
                       qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->vic), 1,
                       qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ));

    /* CSR: GPT + DGT timers, A2M doorbells */
    s->csr = qdev_new(TYPE_MSM_CSR);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->csr), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->csr), 0, MSM_CSR_PHYS);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->csr), 0,
                       qdev_get_gpio_in(s->vic, INT_GP_TIMER_EXP));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->csr), 1,
                       qdev_get_gpio_in(s->vic, INT_DEBUG_TIMER_EXP));

    /* fake modem processor, owns shared RAM */
    s->a9 = qdev_new(TYPE_MSM_A9);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->a9), &error_fatal);
    memory_region_add_subregion_overlap(sysmem, MSM_SHARED_RAM_PHYS,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(s->a9), 0), 10);
    for (int i = 0; i < 7; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(s->a9), i,
                           qdev_get_gpio_in(s->vic, INT_A9_M2A_0 + i));
    }
    for (int i = 0; i < MSM_A2M_NUM; i++) {
        qdev_connect_gpio_out_named(s->csr, "a2m", i,
                                    qdev_get_gpio_in_named(s->a9, "a2m", i));
    }
    qdev_connect_gpio_out_named(s->vic, "wake-req", 0,
                                qdev_get_gpio_in_named(s->a9, "wake", 0));

    /* secondary interrupt controller, cascaded into the VIC */
    s->sirc = qdev_new(TYPE_MSM_SIRC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->sirc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->sirc), 0, MSM_SIRC_PHYS);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->sirc), 0,
                       qdev_get_gpio_in(s->vic, INT_SIRC_0));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->sirc), 1,
                       qdev_get_gpio_in(s->vic, INT_SIRC_1));

    /* GPIO */
    s->gpio = qdev_new(TYPE_MSM_GPIO);
    leo_gpio_defaults(s->gpio);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->gpio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gpio), 0, MSM_GPIO1_PHYS);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gpio), 1, MSM_GPIO2_PHYS);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->gpio), 0,
                       qdev_get_gpio_in(s->sirc, SIRC_INT_GPIO_GROUP1));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->gpio), 1,
                       qdev_get_gpio_in(s->sirc, SIRC_INT_GPIO_GROUP2));

    /* data mover (DMA) */
    s->dmov = qdev_new(TYPE_MSM_DMOV);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->dmov), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->dmov), 0, MSM_DMOV_PHYS);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->dmov), 0,
                       qdev_get_gpio_in(s->vic, INT_ADM_AARM));

    /* NAND flash: -drive if=mtd,format=raw,file=nand.img */
    s->nand = qdev_new(TYPE_MSM_NAND);
    {
        DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);
        if (dinfo) {
            qdev_prop_set_drive_err(s->nand, "drive",
                                    blk_by_legacy_dinfo(dinfo), &error_fatal);
        }
    }
    qdev_prop_set_uint32(s->nand, "size-mb", s->nand_mb ? s->nand_mb : 512);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->nand), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->nand), 0, MSM_NAND_PHYS);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->nand), 0,
                       qdev_get_gpio_in(s->vic, INT_NAND_OP_DONE));

    /* I2C bus 0 and the devices on it (board-htcleo.c base_i2c_devices) */
    {
        DeviceState *i2c = qdev_new(TYPE_MSM_I2C);
        I2CBus *bus;
        I2CSlave *ts;

        sysbus_realize_and_unref(SYS_BUS_DEVICE(i2c), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(i2c), 0, MSM_I2C_PHYS);
        sysbus_connect_irq(SYS_BUS_DEVICE(i2c), 0,
                           qdev_get_gpio_in(s->vic, INT_PWB_I2C));
        bus = I2C_BUS(qdev_get_child_bus(i2c, "i2c"));

        ts = i2c_slave_create_simple(bus, TYPE_LEO_TOUCH, 0x2A >> 1);
        qdev_connect_gpio_out_named(DEVICE(ts), "irq-n", 0,
                                    qdev_get_gpio_in(s->gpio, LEO_GPIO_TP_INT_N));
        qdev_connect_gpio_out_named(s->gpio, "out", LEO_GPIO_TS_POWER,
                                    qdev_get_gpio_in_named(DEVICE(ts), "power", 0));
        i2c_slave_create_simple(bus, TYPE_HTC_MICROP, 0xCC >> 1);
        i2c_slave_create_simple(bus, TYPE_I2C_REGFILE, 0x48);   /* tps65023 */
        i2c_slave_create_simple(bus, TYPE_I2C_REGFILE, 0x1C);   /* akm8973 */
        i2c_slave_create_simple(bus, "leo-ds2746", 0x26);       /* fuel gauge */
    }

    /* Adreno 200 GPU */
    {
        DeviceState *gpu = qdev_new(TYPE_ADRENO);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(gpu), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(gpu), 0, MSM_GPU_REG_PHYS);
        sysbus_connect_irq(SYS_BUS_DEVICE(gpu), 0,
                           qdev_get_gpio_in(s->vic, INT_GRAPHICS));
    }

    /* display controller (MDP 3.1 + LCDC panel) */
    {
        DeviceState *mdp = qdev_new(TYPE_MSM_MDP);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(mdp), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(mdp), 0, MSM_MDP_PHYS);
        sysbus_connect_irq(SYS_BUS_DEVICE(mdp), 0,
                           qdev_get_gpio_in(s->vic, INT_MDP));
    }

    /* SDC1: Wi-Fi SDIO slot (BCM4329), SDC2: microSD slot */
    {
        static const struct { hwaddr base; int irq0, irq1; } sdc[] = {
            { MSM_SDC1_PHYS, INT_SDC1_0, INT_SDC1_1 },
            { MSM_SDC2_PHYS, INT_SDC2_0, INT_SDC2_1 },
        };
        for (int i = 0; i < ARRAY_SIZE(sdc); i++) {
            DeviceState *d = qdev_new(TYPE_MSM_SDCC);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(d), 0, sdc[i].base);
            sysbus_connect_irq(SYS_BUS_DEVICE(d), 0,
                               qdev_get_gpio_in(s->vic, sdc[i].irq0));
            sysbus_connect_irq(SYS_BUS_DEVICE(d), 1,
                               qdev_get_gpio_in(s->vic, sdc[i].irq1));
            if (i == 0) {
                qemu_irq power;
                leo_gpio_dev = s->gpio;
                leo_tlmm_config = leo_tlmm;
                leo_bcm4329_init(d, qdev_get_gpio_in(s->gpio, LEO_GPIO_WIFI_IRQ),
                                 &power, qemu_find_netdev("wifi"));
                qdev_connect_gpio_out_named(s->gpio, "out",
                                            LEO_GPIO_WIFI_SHUTDOWN_N, power);
            }
            if (i == 1) {
                DriveInfo *di = drive_get(IF_SD, 0, 0);
                if (di) {
                    DeviceState *card = qdev_new(TYPE_SD_CARD);
                    qdev_prop_set_drive_err(card, "drive",
                                            blk_by_legacy_dinfo(di),
                                            &error_fatal);
                    qdev_realize_and_unref(card,
                                           qdev_get_child_bus(d, "sd-bus"),
                                           &error_fatal);
                }
            }
        }
    }

    /* hardware buttons */
    s->kp_col_in = qemu_allocate_irqs(leo_kp_col_changed, s, 3);
    s->kp_col_level[0] = s->kp_col_level[1] = s->kp_col_level[2] = true;
    qdev_connect_gpio_out_named(s->gpio, "out", LEO_GPIO_KP_MKOUT0, s->kp_col_in[0]);
    qdev_connect_gpio_out_named(s->gpio, "out", LEO_GPIO_KP_MKOUT1, s->kp_col_in[1]);
    qdev_connect_gpio_out_named(s->gpio, "out", LEO_GPIO_KP_MKOUT2, s->kp_col_in[2]);
    qemu_input_handler_activate(
        qemu_input_handler_register(DEVICE(s->gpio), &leo_kbd_handler));

    leo_map_stubs();
    vmstate_register(NULL, 0, &vmstate_htc_leo, s);

    /* ram_console mirror */
    chr = serial_hd(0);
    if (chr) {
        qemu_chr_fe_init(&s->rc_chr, chr, &error_abort);
        s->rc_have_chr = true;
    }
    s->rc_timer = timer_new_ms(QEMU_CLOCK_REALTIME, leo_rc_poll, s);
    timer_mod(s->rc_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
    qemu_register_reset(leo_rc_reset, s);
    qemu_register_reset(leo_bootstate_reset, s);

    if (!machine->kernel_filename) {
        /* like cLK: boot (or recovery) partition from NAND */
        LeoPartition parts[8];
        int n = leo_build_ptable(s, parts);
        const char *want = s->boot_mode && !strcmp(s->boot_mode, "recovery")
                           ? "recovery" : "boot";
        for (int i = 0; i < n; i++) {
            if (!strcmp(parts[i].name, want)) {
                leo_boot_from_nand(machine, s->nand, parts[i].start,
                                   parts[i].length, want, &error_fatal);
            }
        }
    }

    /* -kernel may be an Android boot image (boot.img / recovery.img) */
    {
        g_autofree uint8_t *img = NULL;
        gsize len = 0;

        if (g_file_get_contents(machine->kernel_filename, (gchar **)&img,
                                &len, NULL) &&
            leo_is_android_bootimg(img, len)) {
            leo_use_android_bootimg(machine, img, len, &error_fatal);
        }
    }

    leo_binfo.loader_start = LEO_KERNEL_BASE;
    leo_binfo.ram_size = machine->ram_size - (LEO_KERNEL_BASE - MSM_EBI1_BASE);
    leo_binfo.board_id = HTCLEO_MACH_ID;
    leo_binfo.atag_board = leo_atag_board;
    leo_binfo.atag_board_raw = true;
    arm_load_kernel(s->cpu, machine, &leo_binfo);
    leo_cleanup_tmp(machine);
}

static void leo_get_nand_size(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    HtcLeoMachineState *s = HTC_LEO_MACHINE(obj);
    uint32_t val = s->nand_mb ? s->nand_mb : 512;
    visit_type_uint32(v, name, &val, errp);
}

static void leo_set_nand_size(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    HtcLeoMachineState *s = HTC_LEO_MACHINE(obj);
    uint32_t val;
    if (visit_type_uint32(v, name, &val, errp)) {
        s->nand_mb = val;
    }
}

static void leo_prop_get_u32(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    uint32_t *field = (uint32_t *)((char *)obj + (uintptr_t)opaque);
    visit_type_uint32(v, name, field, errp);
}

static void leo_prop_set_u32(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    uint32_t *field = (uint32_t *)((char *)obj + (uintptr_t)opaque);
    visit_type_uint32(v, name, field, errp);
}

static char *leo_get_boot_mode(Object *obj, Error **errp)
{
    return g_strdup(HTC_LEO_MACHINE(obj)->boot_mode ?: "boot");
}

static void leo_set_boot_mode(Object *obj, const char *v, Error **errp)
{
    HtcLeoMachineState *s = HTC_LEO_MACHINE(obj);
    if (strcmp(v, "boot") && strcmp(v, "recovery")) {
        error_setg(errp, "boot-mode must be 'boot' or 'recovery'");
        return;
    }
    g_free(s->boot_mode);
    s->boot_mode = g_strdup(v);
}

static char *leo_get_reboot_file(Object *obj, Error **errp)
{
    return g_strdup(HTC_LEO_MACHINE(obj)->reboot_file ?: "");
}

static void leo_set_reboot_file(Object *obj, const char *v, Error **errp)
{
    HtcLeoMachineState *s = HTC_LEO_MACHINE(obj);
    g_free(s->reboot_file);
    s->reboot_file = *v ? g_strdup(v) : NULL;
}

static char *leo_get_hostlink(Object *obj, Error **errp)
{
    return g_strdup(HTC_LEO_MACHINE(obj)->hostlink ?: "");
}

static void leo_set_hostlink(Object *obj, const char *v, Error **errp)
{
    HtcLeoMachineState *s = HTC_LEO_MACHINE(obj);
    g_free(s->hostlink);
    s->hostlink = *v ? g_strdup(v) : NULL;
}

static bool leo_get_pin_cpu(Object *obj, Error **errp)
{
    return HTC_LEO_MACHINE(obj)->pin_cpu;
}

static void leo_set_pin_cpu(Object *obj, bool v, Error **errp)
{
    HTC_LEO_MACHINE(obj)->pin_cpu = v;
}

static void htc_leo_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "hostlink", leo_get_hostlink,
                                  leo_set_hostlink);
    object_class_property_set_description(oc, "hostlink",
        "shared memory file carrying the screen and sound to a front end");
    object_class_property_add_bool(oc, "pin-cpu", leo_get_pin_cpu,
                                   leo_set_pin_cpu);
    object_class_property_set_description(oc, "pin-cpu",
        "keep the emulator on the host's fastest CPU cores");

    object_class_property_add_str(oc, "reboot-file", leo_get_reboot_file,
                                  leo_set_reboot_file);
    object_class_property_set_description(oc, "reboot-file",
        "on guest reboot/power off write the next mode (android, recovery, "
        "off) to this file and exit instead of resetting");

    object_class_property_add_str(oc, "boot-mode", leo_get_boot_mode,
                                  leo_set_boot_mode);
    object_class_property_set_description(oc, "boot-mode",
        "without -kernel: boot from the NAND 'boot' (default) or "
        "'recovery' partition");
    object_class_property_add(oc, "system-size", "uint32", leo_prop_get_u32,
                              leo_prop_set_u32, NULL,
                              (void *)offsetof(HtcLeoMachineState, system_mb));
    object_class_property_set_description(oc, "system-size",
        "system partition size in MiB (cLK default 150)");
    object_class_property_add(oc, "cache-size", "uint32", leo_prop_get_u32,
                              leo_prop_set_u32, NULL,
                              (void *)offsetof(HtcLeoMachineState, cache_mb));
    object_class_property_set_description(oc, "cache-size",
        "cache partition size in MiB (cLK default 44)");

    object_class_property_add(oc, "nand-size", "uint32", leo_get_nand_size,
                              leo_set_nand_size, NULL, NULL);
    object_class_property_set_description(oc, "nand-size",
        "NAND size in MiB: 512 (EU HD2) or 1024 (T-Mobile US HD2)");
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("scorpion"),
        ARM_CPU_TYPE_NAME("cortex-a8"),
        NULL
    };

    mc->desc = "HTC HD2 / Leo (Qualcomm QSD8250)";
    mc->init = htc_leo_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("scorpion");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = LEO_EBI1_DEFAULT_SIZE;
    mc->default_ram_id = "leo.ebi1";
    mc->ignore_memory_transaction_failures = true;
}

static const TypeInfo htc_leo_info = {
    .name = TYPE_HTC_LEO_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(HtcLeoMachineState),
    .class_init = htc_leo_class_init,
};

static void htc_leo_register_types(void)
{
    type_register_static(&htc_leo_info);
}

type_init(htc_leo_register_types)
