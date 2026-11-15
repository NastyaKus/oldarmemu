/*
 * HTC HD2 boot helpers: a high level stand-in for the cLK boot loader.
 *
 * cLK loads an Android boot image (kernel + ramdisk + cmdline) from the
 * "boot" or "recovery" NAND partition and starts it with ATAGs that
 * carry the NAND partition table.  Here we accept the same boot image
 * either as the -kernel file or from NAND, unpack it and hand the pieces
 * to QEMU's generic ARM Linux loader.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/boards.h"
#include "leo_boot.h"

#define BOOT_MAGIC          "ANDROID!"
#define BOOT_MAGIC_SIZE     8
#define BOOT_NAME_SIZE      16
#define BOOT_ARGS_SIZE      512

typedef struct QEMU_PACKED AndroidBootHdr {
    uint8_t  magic[BOOT_MAGIC_SIZE];
    uint32_t kernel_size;
    uint32_t kernel_addr;
    uint32_t ramdisk_size;
    uint32_t ramdisk_addr;
    uint32_t second_size;
    uint32_t second_addr;
    uint32_t tags_addr;
    uint32_t page_size;
    uint32_t unused[2];
    uint8_t  name[BOOT_NAME_SIZE];
    uint8_t  cmdline[BOOT_ARGS_SIZE];
    uint32_t id[8];
} AndroidBootHdr;

bool leo_is_android_bootimg(const uint8_t *buf, size_t len)
{
    return len >= sizeof(AndroidBootHdr) &&
           !memcmp(buf, BOOT_MAGIC, BOOT_MAGIC_SIZE);
}

static char *write_tmp(const uint8_t *data, size_t len, const char *tmpl,
                       Error **errp)
{
    GError *gerr = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp(tmpl, &path, &gerr);

    if (fd < 0) {
        error_setg(errp, "htc-leo: cannot create temp file: %s", gerr->message);
        g_error_free(gerr);
        return NULL;
    }
    if (qemu_write_full(fd, data, len) != len) {
        error_setg_errno(errp, errno, "htc-leo: cannot write %s", path);
        close(fd);
        unlink(path);
        g_free(path);
        return NULL;
    }
    close(fd);
    return path;
}

static void set_cmdline(MachineState *m, const char *from_image)
{
    const char *user = m->kernel_cmdline ? m->kernel_cmdline : "";
    char *cl = g_strstrip(g_strdup_printf("%s %s", from_image, user));

    g_free(m->kernel_cmdline);
    m->kernel_cmdline = cl;
}

bool leo_use_kernel_initrd(MachineState *m, const uint8_t *kernel,
                           size_t ksize, const uint8_t *ramdisk,
                           size_t rsize, const char *cmdline, Error **errp)
{
    char *kpath = write_tmp(kernel, ksize, "leo-kernel-XXXXXX", errp);
    if (!kpath) {
        return false;
    }
    g_free(m->kernel_filename);
    m->kernel_filename = kpath;

    if (ramdisk && rsize) {
        char *rpath = write_tmp(ramdisk, rsize, "leo-ramdisk-XXXXXX", errp);
        if (!rpath) {
            return false;
        }
        g_free(m->initrd_filename);
        m->initrd_filename = rpath;
    }
    set_cmdline(m, cmdline ? cmdline : "");
    return true;
}

bool leo_use_android_bootimg(MachineState *m, const uint8_t *img, size_t len,
                             Error **errp)
{
    const AndroidBootHdr *h = (const AndroidBootHdr *)img;
    uint32_t page, kpages, kofs, rofs;
    char cmdline[BOOT_ARGS_SIZE + 1];

    if (!leo_is_android_bootimg(img, len)) {
        error_setg(errp, "htc-leo: not an Android boot image");
        return false;
    }
    page = le32_to_cpu(h->page_size);
    if (page < 2048 || page > 16384 || (page & (page - 1))) {
        page = 2048;
    }
    kpages = DIV_ROUND_UP(le32_to_cpu(h->kernel_size), page);
    kofs = page;
    rofs = page * (1 + kpages);
    if ((uint64_t)kofs + le32_to_cpu(h->kernel_size) > len ||
        (uint64_t)rofs + le32_to_cpu(h->ramdisk_size) > len) {
        error_setg(errp, "htc-leo: truncated boot image");
        return false;
    }
    memcpy(cmdline, h->cmdline, BOOT_ARGS_SIZE);
    cmdline[BOOT_ARGS_SIZE] = 0;

    info_report("htc-leo: boot image \"%.16s\": kernel %u bytes, ramdisk %u "
                "bytes, cmdline \"%s\"", (const char *)h->name,
                le32_to_cpu(h->kernel_size), le32_to_cpu(h->ramdisk_size),
                cmdline);
    return leo_use_kernel_initrd(m, img + kofs, le32_to_cpu(h->kernel_size),
                                 img + rofs, le32_to_cpu(h->ramdisk_size),
                                 cmdline, errp);
}

void leo_cleanup_tmp(MachineState *m)
{
    /* the generic loader has copied everything into ROM blobs */
    if (m->kernel_filename && strstr(m->kernel_filename, "leo-kernel-")) {
        unlink(m->kernel_filename);
    }
    if (m->initrd_filename && strstr(m->initrd_filename, "leo-ramdisk-")) {
        unlink(m->initrd_filename);
    }
}
