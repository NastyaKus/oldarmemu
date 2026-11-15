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
#include "msm.h"

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

/* ------------------------------------------------------------------ */
/* Booting from a NAND partition                                       */
/* ------------------------------------------------------------------ */

#define NAND_PAGE           2048
#define NAND_PAGES_PER_BLK  64

/* yaffs2 "tags only" layout in the 16 free OOB bytes */
#define YAFFS_EXTRA_HEADER_INFO_FLAG    0x80000000u
#define YAFFS_ALL_EXTRA_FLAGS           0xF0000000u
#define YAFFS_OBJ_ROOT                  1
#define YAFFS_OBJECT_TYPE_FILE          1
#define YAFFS_HDR_NAME_OFF              10
#define YAFFS_HDR_SIZE_OFF              292

typedef struct YaffsChunk {
    uint32_t seq;
    uint32_t page;
    uint32_t nbytes;
} YaffsChunk;

typedef struct YaffsObj {
    uint32_t seq;           /* sequence of the newest header */
    uint32_t parent;
    uint32_t type;
    uint32_t size;
    char name[256];
    GHashTable *chunks;     /* chunk id -> YaffsChunk* */
} YaffsObj;

static void yaffs_obj_free(gpointer p)
{
    YaffsObj *o = p;
    g_hash_table_destroy(o->chunks);
    g_free(o);
}

static YaffsObj *yaffs_obj(GHashTable *objs, uint32_t id)
{
    YaffsObj *o = g_hash_table_lookup(objs, GUINT_TO_POINTER(id));
    if (!o) {
        o = g_new0(YaffsObj, 1);
        o->chunks = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                          NULL, g_free);
        g_hash_table_insert(objs, GUINT_TO_POINTER(id), o);
    }
    return o;
}

/* Scan a yaffs2 partition; returns object table keyed by object id. */
static GHashTable *yaffs_scan(DeviceState *nand, uint32_t first_page,
                              uint32_t npages)
{
    GHashTable *objs = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                             NULL, yaffs_obj_free);
    uint8_t data[NAND_PAGE], oob[16];

    for (uint32_t p = 0; p < npages; p++) {
        uint32_t page = first_page + p;
        if (!msm_nand_read_page(nand, page, data, oob)) {
            break;
        }
        uint32_t seq = ldl_le_p(oob), obj = ldl_le_p(oob + 4);
        uint32_t chunk = ldl_le_p(oob + 8), nbytes = ldl_le_p(oob + 12);

        if (seq == 0 || seq == 0xffffffff) {
            continue;       /* erased or unused */
        }
        if (chunk & YAFFS_EXTRA_HEADER_INFO_FLAG) {
            chunk = 0;
        }
        obj &= ~YAFFS_ALL_EXTRA_FLAGS;

        YaffsObj *o = yaffs_obj(objs, obj);
        if (chunk == 0) {
            if (seq >= o->seq) {
                o->seq = seq;
                o->type = ldl_le_p(data);
                o->parent = ldl_le_p(data + 4);
                memcpy(o->name, data + YAFFS_HDR_NAME_OFF, 255);
                o->name[255] = 0;
                o->size = ldl_le_p(data + YAFFS_HDR_SIZE_OFF);
            }
        } else {
            YaffsChunk *c = g_hash_table_lookup(o->chunks,
                                                GUINT_TO_POINTER(chunk));
            if (!c) {
                c = g_new0(YaffsChunk, 1);
                g_hash_table_insert(o->chunks, GUINT_TO_POINTER(chunk), c);
            }
            if (seq >= c->seq) {
                c->seq = seq;
                c->page = page;
                c->nbytes = nbytes;
            }
        }
    }
    return objs;
}

static uint8_t *yaffs_read_file(DeviceState *nand, GHashTable *objs,
                                const char *name, size_t *len)
{
    GHashTableIter it;
    gpointer k, v;

    g_hash_table_iter_init(&it, objs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        YaffsObj *o = v;
        if (o->type != YAFFS_OBJECT_TYPE_FILE || o->parent != YAFFS_OBJ_ROOT ||
            strcmp(o->name, name)) {
            continue;
        }
        uint8_t *buf = g_malloc0(o->size ? o->size : 1);
        for (uint32_t off = 0; off < o->size; off += NAND_PAGE) {
            YaffsChunk *c = g_hash_table_lookup(
                o->chunks, GUINT_TO_POINTER(off / NAND_PAGE + 1));
            uint8_t page[NAND_PAGE];
            if (!c || !msm_nand_read_page(nand, c->page, page, NULL)) {
                g_free(buf);
                return NULL;
            }
            memcpy(buf + off, page, MIN(NAND_PAGE, o->size - off));
        }
        *len = o->size;
        return buf;
    }
    return NULL;
}

bool leo_boot_from_nand(MachineState *m, DeviceState *nand,
                        uint32_t start_block, uint32_t nblocks,
                        const char *what, Error **errp)
{
    uint32_t first = start_block * NAND_PAGES_PER_BLK;
    uint32_t npages = nblocks * NAND_PAGES_PER_BLK;
    uint8_t page[NAND_PAGE];

    if (!msm_nand_read_page(nand, first, page, NULL)) {
        error_setg(errp, "htc-leo: cannot read the %s partition", what);
        return false;
    }

    /* raw Android boot image, as written by fastboot / flash_image */
    if (leo_is_android_bootimg(page, sizeof(page))) {
        size_t len = (size_t)npages * NAND_PAGE;
        g_autofree uint8_t *img = g_malloc(len);
        for (uint32_t p = 0; p < npages; p++) {
            if (!msm_nand_read_page(nand, first + p, img + p * NAND_PAGE,
                                    NULL)) {
                error_setg(errp, "htc-leo: NAND read error in %s", what);
                return false;
            }
        }
        info_report("htc-leo: booting Android boot image from NAND %s", what);
        return leo_use_android_bootimg(m, img, len, errp);
    }

    /* yaffs2 partition holding zImage + initrd.gz (MAGLDR / cLK style) */
    {
        g_autoptr(GHashTable) objs = yaffs_scan(nand, first, npages);
        size_t klen = 0, rlen = 0;
        g_autofree uint8_t *k = yaffs_read_file(nand, objs, "zImage", &klen);
        g_autofree uint8_t *r = yaffs_read_file(nand, objs, "initrd.gz", &rlen);

        if (k) {
            info_report("htc-leo: booting zImage (%zu bytes) + initrd.gz "
                        "(%zu bytes) from yaffs2 %s", klen, rlen, what);
            return leo_use_kernel_initrd(m, k, klen, r, rlen, "", errp);
        }
    }
    error_setg(errp, "htc-leo: nothing bootable in the NAND %s partition "
               "(flash a ROM or recovery first)", what);
    return false;
}
