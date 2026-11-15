/*
 * Run-time fix-ups applied to HD2 kernels for emulation.
 *
 * msm_irq_to_smsm[] (arch/arm/mach-msm/irq.c) tells which interrupts can
 * wake the apps CPU from idle power collapse.  INT_A9_M2A_4 (the WinCE
 * "DEX" callback used by HD2 kernels) is missing from it, so once that
 * interrupt is enabled msm_irq_idle_sleep_allowed() is always false and
 * the kernel never runs msm_sleep() from idle.  But msm_sleep() ->
 * smd_sleep_exit() is the only place these kernels service SMD channels
 * of the DSP type (CONFIG_QDSP6 is off), which q6audio needs at boot.
 * We mark M2A_4 as a (fake) wake-up source before it gets enabled.
 *
 * Applied when the kernel first programs the interrupt controller, i.e.
 * after decompression and before board init.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"
#include "msm.h"

#define SCAN_BASE   LEO_KERNEL_BASE
#define SCAN_SIZE   (24 * 1024 * 1024)
#define SMSM_FAKE_IRQ 0xff

static bool leo_quirks_done;

/* signature of the QSD8x50 msm_irq_to_smsm[] table */
static bool is_irq_to_smsm(const uint8_t *t)
{
    return t[0] == 0xff && t[1] == 0xff && t[5] == 0xff &&     /* M2A_0,1,5 */
           t[7] == 0xff && t[8] == 0xff && t[9] == 0xff &&     /* GPT,DGT,SIRC */
           t[15] == 4 && t[16] == 2 && t[17] == 1 && t[18] == 3 && /* USB/MDDI */
           t[24] == 6 && t[25] == 7 && t[26] == 8 && t[27] == 9;   /* SDCx */
}

void leo_apply_kernel_quirks(void)
{
    g_autofree uint8_t *buf = NULL;

    if (leo_quirks_done) {
        return;
    }
    leo_quirks_done = true;

    buf = g_malloc(SCAN_SIZE);
    if (address_space_read(&address_space_memory, SCAN_BASE,
                           MEMTXATTRS_UNSPECIFIED, buf, SCAN_SIZE)) {
        warn_report("htc-leo: kernel quirks: cannot read guest RAM");
        return;
    }
    for (uint32_t i = 0; i + 64 < SCAN_SIZE; i++) {
        if (is_irq_to_smsm(buf + i)) {
            uint8_t v = SMSM_FAKE_IRQ;
            if (buf[i + INT_A9_M2A_4] == 0) {
                address_space_write(&address_space_memory,
                                    SCAN_BASE + i + INT_A9_M2A_4,
                                    MEMTXATTRS_UNSPECIFIED, &v, 1);
                info_report("htc-leo: kernel quirk: INT_A9_M2A_4 marked as "
                            "wake-up irq (msm_irq_to_smsm at 0x%x)",
                            SCAN_BASE + i);
            }
            return;
        }
    }
    warn_report("htc-leo: kernel quirks: msm_irq_to_smsm not found");
}

void leo_reset_kernel_quirks(void)
{
    leo_quirks_done = false;
}

/*
 * The q6audio DAL channel is declared as a modem channel so that it is
 * serviced from the SMD modem interrupt, which works with any CPU load.
 * The kernel names modem channels "SMD_<name>" though, while q6audio
 * opens "DSP_DAL_AQ_AUD"; once the channel exists, rename it in the
 * kernel's smd_channel structure.
 */
static uint8_t *leo_ram;
static uint64_t leo_ram_size;

void leo_quirks_set_ram(void *host, uint64_t size)
{
    leo_ram = host;
    leo_ram_size = size;
}

bool leo_quirk_rename_smd_channel(const char *from, const char *to)
{
    size_t n = strlen(from) + 1;
    uint8_t *p;

    if (!leo_ram || strlen(to) + 1 != n) {
        return false;
    }
    /* lowmem only: kmalloc'ed structures live there */
    p = memmem(leo_ram, MIN(leo_ram_size, 256 * 1024 * 1024), from, n);
    if (!p) {
        return false;
    }
    memcpy(p, to, n);
    info_report("htc-leo: kernel quirk: SMD channel %s renamed to %s",
                from, to);
    return true;
}
