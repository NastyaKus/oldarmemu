/*
 * HTC HD2 (Leo) / Qualcomm QSD8250 emulation - shared definitions.
 *
 * Addresses and IRQ numbers come from the htcleo kernel sources:
 *   arch/arm/mach-msm/include/mach/msm_iomap-8x50.h
 *   arch/arm/mach-msm/include/mach/irqs.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_HTCLEO_MSM_H
#define HW_ARM_HTCLEO_MSM_H

#include "hw/sysbus.h"
#include "qom/object.h"

/* ---- physical memory map ---- */
#define MSM_SMI_BASE            0x00000000
#define MSM_SMI_SIZE            0x04000000      /* 64 MiB stacked SMI */
#define MSM_SHARED_RAM_PHYS     0x00100000      /* SMEM, inside SMI */
#define MSM_SHARED_RAM_SIZE     0x00100000
#define MSM_EBI1_BASE           0x10000000
#define LEO_KERNEL_BASE         0x11800000      /* CONFIG_PHYS_OFFSET */
#define LEO_RAM_CONSOLE_PHYS    0x2FFC0000
#define LEO_RAM_CONSOLE_SIZE    0x00040000

#define MSM_GPU_REG_PHYS        0xA0000000
#define MSM_UART1DM_PHYS        0xA0200000
#define MSM_SDC1_PHYS           0xA0300000
#define MSM_SDC2_PHYS           0xA0400000
#define MSM_SDC3_PHYS           0xA0500000
#define MSM_SDC4_PHYS           0xA0600000
#define MSM_HSUSB_PHYS          0xA0800000
#define MSM_NAND_PHYS           0xA0A00000
#define MSM_SSBI_PHYS           0xA8100000
#define MSM_CLK_CTL_PHYS        0xA8600000
#define MSM_TCSR_PHYS           0xA8700000
#define MSM_SCPLL_PHYS          0xA8800000
#define MSM_GPIOCFG1_PHYS       0xA8E00000
#define MSM_GPIOCFG2_PHYS       0xA8F00000
#define MSM_GPIO1_PHYS          0xA9000000
#define MSM_GPIO2_PHYS          0xA9100000
#define MSM_DMOV_PHYS           0xA9700000
#define MSM_I2C_PHYS            0xA9900000
#define MSM_UART1_PHYS          0xA9A00000
#define MSM_UART2_PHYS          0xA9B00000
#define MSM_UART3_PHYS          0xA9C00000
#define MSM_MDP_PHYS            0xAA200000
#define MSM_VIC_PHYS            0xAC000000
#define MSM_CSR_PHYS            0xAC100000
#define MSM_SIRC_PHYS           0xAC200000

/* ---- VIC interrupt numbers (QSD8x50) ---- */
#define INT_A9_M2A_0            0
#define INT_A9_M2A_4            4
#define INT_A9_M2A_5            5
#define INT_A9_M2A_6            6
#define INT_GP_TIMER_EXP        7
#define INT_DEBUG_TIMER_EXP     8
#define INT_SIRC_0              9
#define INT_MDP                 19
#define INT_GRAPHICS            20
#define INT_ADM_AARM            21
#define INT_SDC1_0              24
#define INT_SDC1_1              25
#define INT_SDC2_0              26
#define INT_SDC2_1              27
#define INT_PWB_I2C             (32 + 2)
#define INT_NAND_WR_ER_DONE     (32 + 4)
#define INT_NAND_OP_DONE        (32 + 5)
#define INT_USB_HS              (32 + 26)
#define INT_SIRC_1              (32 + 31)
#define MSM_VIC_NUM_IRQS        64

/* SIRC sources (Linux irq = 64 + n) */
#define MSM_SIRC_NUM_IRQS       23
#define SIRC_INT_UART1          0
#define SIRC_INT_UART2          1
#define SIRC_INT_UART3          2
#define SIRC_INT_GPIO_GROUP1    9
#define SIRC_INT_GPIO_GROUP2    10

/* GPIO */
#define MSM_GPIO_NUM            165

/* number of apps->modem doorbells in the CSR block */
#define MSM_A2M_NUM             8

/* ---- device type names ---- */
#define TYPE_MSM_VIC    "msm-vic"
#define TYPE_MSM_CSR    "msm-csr"
#define TYPE_MSM_A9     "msm-a9-hle"
#define TYPE_MSM_STUB   "msm-stub"
#define TYPE_MSM_GPIO   "msm-gpio"
#define TYPE_MSM_SIRC   "msm-sirc"
#define TYPE_MSM_DMOV   "msm-dmov"
#define TYPE_MSM_NAND   "msm-nand"
#define TYPE_MSM_I2C    "msm-i2c"
#define TYPE_LEO_TOUCH  "leo-touch"
#define TYPE_HTC_MICROP "htc-microp"
#define TYPE_I2C_REGFILE "i2c-regfile"

/* Create a logging, memory-like placeholder for unmodelled hardware. */
DeviceState *msm_stub_create(const char *name, hwaddr base, uint64_t size,
                             int priority);

/* Set the idle level of an input GPIO pin (pull-up/down on the board). */
void msm_gpio_set_default(DeviceState *dev, int n, bool level);

/* Set the value a stub register holds after reset (bootloader state). */
void msm_stub_preset(DeviceState *dev, hwaddr off, uint32_t val);

#endif
