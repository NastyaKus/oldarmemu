/*
 * Tiny serializer for device state that does not fit VMState fields
 * (queues, hash tables): pre_save packs it into a byte buffer saved with
 * VMSTATE_VBUFFER_ALLOC_UINT32, post_load unpacks it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_HTCLEO_VMBLOB_H
#define HW_ARM_HTCLEO_VMBLOB_H

#include "qemu/bswap.h"

/* writer */
static inline void blob_u32(GByteArray *b, uint32_t v)
{
    uint8_t t[4];
    stl_le_p(t, v);
    g_byte_array_append(b, t, 4);
}

static inline void blob_u64(GByteArray *b, uint64_t v)
{
    blob_u32(b, v);
    blob_u32(b, v >> 32);
}

static inline void blob_bytes(GByteArray *b, const void *p, uint32_t n)
{
    blob_u32(b, n);
    if (n) {
        g_byte_array_append(b, p, n);
    }
}

/* turn a writer into the (len, buf) pair VMState saves */
static inline void blob_finish(GByteArray *b, uint32_t *len, uint8_t **buf)
{
    g_free(*buf);
    *len = b->len;
    *buf = (uint8_t *)g_byte_array_free(b, false);
}

/* reader: never runs past the end, returns zeros instead */
typedef struct BlobReader {
    const uint8_t *p;
    uint32_t left;
    bool bad;
} BlobReader;

static inline uint32_t blob_get_u32(BlobReader *r)
{
    uint32_t v;

    if (r->left < 4) {
        r->bad = true;
        return 0;
    }
    v = ldl_le_p(r->p);
    r->p += 4;
    r->left -= 4;
    return v;
}

static inline uint64_t blob_get_u64(BlobReader *r)
{
    uint64_t lo = blob_get_u32(r);
    return lo | ((uint64_t)blob_get_u32(r) << 32);
}

/* returns a pointer into the buffer, *n set to the length */
static inline const uint8_t *blob_get_bytes(BlobReader *r, uint32_t *n)
{
    const uint8_t *p;

    *n = blob_get_u32(r);
    if (*n > r->left) {
        r->bad = true;
        *n = 0;
        return NULL;
    }
    p = r->p;
    r->p += *n;
    r->left -= *n;
    return p;
}

#endif
