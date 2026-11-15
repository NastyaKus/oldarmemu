/*
 * HTC HD2 boot helpers (see leo_boot.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_HTCLEO_LEO_BOOT_H
#define HW_ARM_HTCLEO_LEO_BOOT_H

#include "hw/boards.h"

bool leo_is_android_bootimg(const uint8_t *buf, size_t len);

/* Unpack an Android boot image into machine->kernel/initrd/cmdline. */
bool leo_use_android_bootimg(MachineState *m, const uint8_t *img, size_t len,
                             Error **errp);

/* Same for a separate kernel + ramdisk pair (yaffs2 /boot style). */
bool leo_use_kernel_initrd(MachineState *m, const uint8_t *kernel,
                           size_t ksize, const uint8_t *ramdisk,
                           size_t rsize, const char *cmdline, Error **errp);

void leo_cleanup_tmp(MachineState *m);

/*
 * Load the kernel from a NAND partition (in erase blocks): a raw Android
 * boot image, or a yaffs2 file system with zImage and initrd.gz.
 */
bool leo_boot_from_nand(MachineState *m, DeviceState *nand,
                        uint32_t start_block, uint32_t nblocks,
                        const char *what, Error **errp);

#endif
