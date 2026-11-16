/*
 * "Hostlink": a shared memory file through which a front end (the
 * Android app) gets the HD2 screen and sound without a display backend.
 *
 * Layout (little endian), see android/.../Hostlink.kt:
 *   0x00 magic "HD2L"          0x04 version
 *   0x08 fb width              0x0c fb height
 *   0x10 fb sequence (bumped after each changed frame)
 *   0x14 lcd on
 *   0x18 audio rate            0x1c audio channels
 *   0x20 audio write count     0x24 audio read count (written by the app)
 *   0x28 audio ring size       0x2c audio format sequence
 *   0x1000 frame buffer, 480x800 RGBA
 *   then   audio ring (s16 interleaved PCM)
 *
 * Counters are free running u32; the ring holds count % size.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include <sys/mman.h>
#include "msm.h"

#define HL_MAGIC        0x4c324448      /* "HD2L" */
#define HL_VERSION      1
#define HL_HDR_SIZE     0x1000
#define HL_FB_W         480
#define HL_FB_H         800
#define HL_FB_SIZE      (HL_FB_W * HL_FB_H * 4)
#define HL_AUDIO_SIZE   (256 * 1024)
#define HL_TOTAL        (HL_HDR_SIZE + HL_FB_SIZE + HL_AUDIO_SIZE)

enum {
    HL_W = 0x08, HL_H = 0x0c, HL_FB_SEQ = 0x10, HL_LCD_ON = 0x14,
    HL_A_RATE = 0x18, HL_A_CH = 0x1c, HL_A_WR = 0x20, HL_A_RD = 0x24,
    HL_A_SIZE = 0x28, HL_A_SEQ = 0x2c,
};

static uint8_t *hl;

static inline uint32_t *hl_word(unsigned off)
{
    return (uint32_t *)(hl + off);
}

bool leo_hostlink_open(const char *path, Error **errp)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    void *p;

    if (fd < 0 || ftruncate(fd, HL_TOTAL) < 0) {
        error_setg_errno(errp, errno, "hostlink: cannot create %s", path);
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    p = mmap(NULL, HL_TOTAL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        error_setg_errno(errp, errno, "hostlink: cannot map %s", path);
        return false;
    }
    hl = p;
    *hl_word(0x04) = HL_VERSION;
    *hl_word(HL_W) = HL_FB_W;
    *hl_word(HL_H) = HL_FB_H;
    *hl_word(HL_A_RATE) = 44100;
    *hl_word(HL_A_CH) = 2;
    *hl_word(HL_A_SIZE) = HL_AUDIO_SIZE;
    qatomic_store_release(hl_word(0), HL_MAGIC);
    return true;
}

uint8_t *leo_hostlink_fb(void)
{
    return hl ? hl + HL_HDR_SIZE : NULL;
}

void leo_hostlink_fb_done(int w, int h, bool lcd_on)
{
    if (!hl) {
        return;
    }
    *hl_word(HL_W) = w;
    *hl_word(HL_H) = h;
    *hl_word(HL_LCD_ON) = lcd_on;
    qatomic_store_release(hl_word(HL_FB_SEQ), *hl_word(HL_FB_SEQ) + 1);
}

void leo_hostlink_audio_format(uint32_t rate, uint32_t channels)
{
    if (!hl || !rate || !channels || channels > 2) {
        return;
    }
    if (*hl_word(HL_A_RATE) != rate || *hl_word(HL_A_CH) != channels) {
        *hl_word(HL_A_RATE) = rate;
        *hl_word(HL_A_CH) = channels;
        qatomic_store_release(hl_word(HL_A_SEQ), *hl_word(HL_A_SEQ) + 1);
    }
}

/* Queue PCM for the front end; dropped when nobody keeps up. */
void leo_hostlink_audio_write(const uint8_t *pcm, uint32_t len)
{
    uint8_t *ring;
    uint32_t wr, rd, n;

    if (!hl) {
        return;
    }
    ring = hl + HL_HDR_SIZE + HL_FB_SIZE;
    wr = *hl_word(HL_A_WR);
    rd = qatomic_load_acquire(hl_word(HL_A_RD));
    if (wr - rd > HL_AUDIO_SIZE) {
        rd = wr - HL_AUDIO_SIZE;            /* reader never started */
    }
    n = MIN(len, HL_AUDIO_SIZE - (wr - rd));
    n &= ~3u;
    for (uint32_t done = 0; done < n;) {
        uint32_t at = (wr + done) % HL_AUDIO_SIZE;
        uint32_t chunk = MIN(n - done, HL_AUDIO_SIZE - at);
        memcpy(ring + at, pcm + done, chunk);
        done += chunk;
    }
    qatomic_store_release(hl_word(HL_A_WR), wr + n);
}

/*
 * Keep the emulator on the fastest cores (the cluster within 25% of the
 * top frequency): TCG is single threaded per guest CPU and suffers badly
 * when the scheduler parks it on a little core.
 */
void leo_pin_fast_cpus(void)
{
#ifdef __linux__
    long max = 0, freq[256];
    int n = 0;
    cpu_set_t set;

    for (; n < 256; n++) {
        char path[96], *txt = NULL;
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", n);
        if (!g_file_get_contents(path, &txt, NULL, NULL)) {
            break;
        }
        freq[n] = strtol(txt, NULL, 10);
        g_free(txt);
        max = MAX(max, freq[n]);
    }
    if (n < 2 || !max) {
        return;
    }
    CPU_ZERO(&set);
    for (int i = 0; i < n; i++) {
        if (freq[i] * 4 >= max * 3) {
            CPU_SET(i, &set);
        }
    }
    if (sched_setaffinity(0, sizeof(set), &set) < 0) {
        warn_report("htc-leo: cannot pin to fast CPUs: %s", strerror(errno));
    }
#endif
}
