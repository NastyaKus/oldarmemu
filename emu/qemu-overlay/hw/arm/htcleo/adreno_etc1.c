/*
 * ETC1 texel decoding for the Adreno 200 model (Khronos ETC1 spec).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

static const int etc1_mod[8][4] = {
    { 2, 8, -2, -8 }, { 5, 17, -5, -17 }, { 9, 29, -9, -29 },
    { 13, 42, -13, -42 }, { 18, 60, -18, -60 }, { 24, 80, -24, -80 },
    { 33, 106, -33, -106 }, { 47, 183, -47, -183 },
};

static inline int clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

void adreno_etc1_texel(const uint8_t *b, int px, int py, float *rgba);

/* b: 8 bytes, big-endian 64-bit block; (px, py) within the 4x4 block */
void adreno_etc1_texel(const uint8_t *b, int px, int py, float *rgba)
{
    uint32_t hi = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
    uint32_t lo = (b[4] << 24) | (b[5] << 16) | (b[6] << 8) | b[7];
    bool diff = (hi >> 1) & 1, flip = hi & 1;
    int sub = flip ? (py >= 2) : (px >= 2);
    int r, g, bl;

    if (diff) {
        int r1 = (hi >> 27) & 31, g1 = (hi >> 19) & 31, b1 = (hi >> 11) & 31;
        if (sub) {
            r1 += ((int32_t)(hi << 5)) >> 29;
            g1 += ((int32_t)(hi << 13)) >> 29;
            b1 += ((int32_t)(hi << 21)) >> 29;
        }
        r = (r1 << 3) | (r1 >> 2);
        g = (g1 << 3) | (g1 >> 2);
        bl = (b1 << 3) | (b1 >> 2);
    } else {
        int sh = sub ? 0 : 4;
        r = ((hi >> (24 + sh)) & 15) * 17;
        g = ((hi >> (16 + sh)) & 15) * 17;
        bl = ((hi >> (8 + sh)) & 15) * 17;
    }
    int table = sub ? (hi >> 2) & 7 : (hi >> 5) & 7;
    int bit = px * 4 + py;
    int idx = (((lo >> (bit + 16)) & 1) << 1) | ((lo >> bit) & 1);
    int m = etc1_mod[table][idx];
    rgba[0] = clamp255(r + m) / 255.0f;
    rgba[1] = clamp255(g + m) / 255.0f;
    rgba[2] = clamp255(bl + m) / 255.0f;
    rgba[3] = 1.0f;
}
