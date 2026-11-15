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
#include "hw/boards.h"
#include "hw/arm/boot.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "chardev/char-fe.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/address-spaces.h"
#include "target/arm/cpu-qom.h"
#include "cpu.h"
#include "msm.h"

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
        { "gpu",       MSM_GPU_REG_PHYS,   0x20000 },
        { "uartdm1",   MSM_UART1DM_PHYS,   0x1000 },
        { "sdc1",      MSM_SDC1_PHYS,      0x1000 },
        { "sdc2",      MSM_SDC2_PHYS,      0x1000 },
        { "sdc3",      MSM_SDC3_PHYS,      0x1000 },
        { "sdc4",      MSM_SDC4_PHYS,      0x1000 },
        { "hsusb",     MSM_HSUSB_PHYS,     0x1000 },
        { "nand",      MSM_NAND_PHYS,      0x1000 },
        { "ssbi",      MSM_SSBI_PHYS,      0x1000 },
        { "clk_ctl",   MSM_CLK_CTL_PHYS,   0x1000 },
        { "tcsr",      MSM_TCSR_PHYS,      0x1000 },
        { "scpll",     MSM_SCPLL_PHYS,     0x1000 },
        { "gpiocfg1",  MSM_GPIOCFG1_PHYS,  0x1000 },
        { "gpiocfg2",  MSM_GPIOCFG2_PHYS,  0x1000 },
        { "dmov",      MSM_DMOV_PHYS,      0x1000 },
        { "i2c",       MSM_I2C_PHYS,       0x1000 },
        { "uart1",     MSM_UART1_PHYS,     0x1000 },
        { "uart2",     MSM_UART2_PHYS,     0x1000 },
        { "uart3",     MSM_UART3_PHYS,     0x1000 },
        { "mdp",       MSM_MDP_PHYS,       0xF0000 },
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

    for (int i = 0; i < ARRAY_SIZE(st); i++) {
        DeviceState *d = msm_stub_create(st[i].name, st[i].base, st[i].size, 0);
        for (int j = 0; j < ARRAY_SIZE(pre); j++) {
            if (!strcmp(pre[j].name, st[i].name)) {
                msm_stub_preset(d, pre[j].off, pre[j].val);
            }
        }
    }
}

/* HD2 board GPIOs (board-htcleo.h) */
#define LEO_GPIO_TP_INT_N       92
#define LEO_GPIO_POWER_KEY      94
#define LEO_GPIO_SD_STATUS      153

/* idle levels of input pins as set by board pull-ups */
static void leo_gpio_defaults(DeviceState *gpio)
{
    msm_gpio_set_default(gpio, LEO_GPIO_POWER_KEY, 1);   /* active low */
    msm_gpio_set_default(gpio, LEO_GPIO_SD_STATUS, 1);   /* no card */
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

    s->cpu = ARM_CPU(cpu_create(machine->cpu_type));

    /* memory */
    memory_region_init_ram(&s->smi, NULL, "leo.smi", MSM_SMI_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, MSM_SMI_BASE, &s->smi);
    memory_region_add_subregion(sysmem, MSM_EBI1_BASE, machine->ram);

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

    leo_map_stubs();

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
        error_report("htc-leo: -kernel is required for now "
                     "(bootloader/NAND boot is not implemented yet)");
        exit(EXIT_FAILURE);
    }

    leo_binfo.loader_start = LEO_KERNEL_BASE;
    leo_binfo.ram_size = machine->ram_size - (LEO_KERNEL_BASE - MSM_EBI1_BASE);
    leo_binfo.board_id = HTCLEO_MACH_ID;
    arm_load_kernel(s->cpu, machine, &leo_binfo);
}

static void htc_leo_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
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
