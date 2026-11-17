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
#include <sys/resource.h>
#include "qemu/timer.h"
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
/* little cores by MIDR part number (ARM: A53, A55, A35, A510, A520) */
static bool leo_little_part(unsigned part)
{
    return part == 0xd03 || part == 0xd05 || part == 0xd04 ||
           part == 0xd46 || part == 0xd80;
}

/*
 * A relative speed for each CPU: cpufreq maximum, else the scheduler's
 * capacity, else "little or not" from /proc/cpuinfo.  Android often denies
 * apps the first two, which used to leave the emulator unpinned and, as
 * seen on a Snapdragon 8 Gen 2, running at a third of its speed.
 */
static int leo_cpu_speeds(long *speed, int max_cpus, const char **how)
{
    int n = 0;

    for (int pass = 0; pass < 2 && !n; pass++) {
        for (n = 0; n < max_cpus; n++) {
            char path[96], *txt = NULL;
            snprintf(path, sizeof(path), pass == 0 ?
                     "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq" :
                     "/sys/devices/system/cpu/cpu%d/cpu_capacity", n);
            if (!g_file_get_contents(path, &txt, NULL, NULL)) {
                break;
            }
            speed[n] = strtol(txt, NULL, 10);
            g_free(txt);
        }
        *how = pass == 0 ? "cpufreq" : "cpu_capacity";
    }
    if (n >= 2) {
        return n;
    }

    char *info = NULL;
    n = 0;
    if (g_file_get_contents("/proc/cpuinfo", &info, NULL, NULL)) {
        int cur = -1;
        for (char *l = strtok(info, "\n"); l; l = strtok(NULL, "\n")) {
            unsigned v;
            if (sscanf(l, "processor : %u", &v) == 1 ||
                sscanf(l, "processor\t: %u", &v) == 1) {
                cur = v;
                if (cur < max_cpus) {
                    speed[cur] = 2;
                    n = MAX(n, cur + 1);
                }
            } else if (cur >= 0 && cur < max_cpus &&
                       sscanf(l, "CPU part\t: %x", &v) == 1) {
                speed[cur] = leo_little_part(v) ? 1 : 2;
            }
        }
        g_free(info);
    }
    *how = "cpuinfo";
    return n;
}

#ifdef __linux__
static cpu_set_t leo_fast_set;
static bool leo_fast_valid;

/* the cgroup cpuset Android keeps this process in, and what it allows */
static void leo_log_cpuset(const char *when)
{
    char *cg = NULL, *st = NULL, *allowed = NULL;

    g_file_get_contents("/proc/self/cpuset", &cg, NULL, NULL);
    if (g_file_get_contents("/proc/self/status", &st, NULL, NULL)) {
        char *p = strstr(st, "Cpus_allowed_list:");
        if (p) {
            allowed = g_strndup(p + 18, strcspn(p + 18, "\n"));
            g_strstrip(allowed);
        }
    }
    info_report("htc-leo: %s: cpuset %s, allowed CPUs %s", when,
                cg ? g_strstrip(cg) : "?", allowed ? allowed : "?");
    g_free(cg);
    g_free(st);
    g_free(allowed);
}

/*
 * Android moves an app's child processes between cpusets (top-app,
 * foreground, background...), and moving a task resets its affinity to
 * the new cpuset: put every thread back on the fast cores.
 */
static int leo_repin_threads(void)
{
    GDir *d = g_dir_open("/proc/self/task", 0, NULL);
    const char *e;
    int ok = 0;

    if (!d) {
        return 0;
    }
    while ((e = g_dir_read_name(d))) {
        if (sched_setaffinity(atoi(e), sizeof(leo_fast_set), &leo_fast_set) == 0) {
            ok++;
        }
    }
    g_dir_close(d);
    return ok;
}
#endif

void leo_pin_fast_cpus(void)
{
#ifdef __linux__
    long max = 0, speed[256];
    const char *how = "";
    int n = leo_cpu_speeds(speed, 256, &how);
    cpu_set_t set;
    GString *list = g_string_new("");

    /* raise the emulator's priority where allowed (threads inherit it) */
    if (setpriority(PRIO_PROCESS, 0, -10) < 0) {
        setpriority(PRIO_PROCESS, 0, -4);
    }
    for (int i = 0; i < n; i++) {
        max = MAX(max, speed[i]);
    }
    if (n < 2 || !max) {
        info_report("htc-leo: CPU pinning: no CPU information (%s)", how);
        g_string_free(list, true);
        return;
    }
    CPU_ZERO(&set);
    for (int i = 0; i < n; i++) {
        if (speed[i] * 4 >= max * 3) {
            CPU_SET(i, &set);
            g_string_append_printf(list, " %d", i);
        }
    }
    leo_fast_set = set;
    leo_fast_valid = true;
    leo_log_cpuset("start");
    if (sched_setaffinity(0, sizeof(set), &set) < 0) {
        warn_report("htc-leo: cannot pin to fast CPUs: %s", strerror(errno));
    } else {
        info_report("htc-leo: pinned to fast CPUs%s (by %s), nice %d", list->str,
                    how, getpriority(PRIO_PROCESS, 0));
    }
    g_string_free(list, true);
#endif
}

/*
 * Every 10 s, log where the busiest emulator thread ran and how much CPU
 * it got: tells from a user's qemu.log whether it sat on a slow core.
 */
void leo_cpu_report(void)
{
#ifdef __linux__
    static int64_t last;
    static unsigned long prev_ticks[64];
    static int prev_tid[64];
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    GDir *d;
    const char *e;
    int best_tid = 0, best_cpu = -1, k = 0;
    unsigned long best = 0;
    int tids[64];
    unsigned long ticks[64];

    static int64_t last_log;
    if (now - last < 2000) {
        return;
    }
    d = g_dir_open("/proc/self/task", 0, NULL);
    if (!d) {
        return;
    }
    while ((e = g_dir_read_name(d)) && k < 64) {
        char path[64], *txt = NULL, *p;
        snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e);
        if (!g_file_get_contents(path, &txt, NULL, NULL)) {
            continue;
        }
        p = strrchr(txt, ')');
        unsigned long ut = 0, st = 0;
        int cpu = -1;
        if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                        "%lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %*d %*u %*u "
                        "%*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u "
                        "%*d %d", &ut, &st, &cpu) == 3) {
            int tid = atoi(e);
            unsigned long delta = ut + st;
            for (int i = 0; i < 64; i++) {
                if (prev_tid[i] == tid) {
                    delta -= prev_ticks[i];
                }
            }
            tids[k] = tid;
            ticks[k] = ut + st;
            k++;
            if (delta > best) {
                best = delta;
                best_tid = tid;
                best_cpu = cpu;
            }
        }
        g_free(txt);
    }
    g_dir_close(d);
    bool off = leo_fast_valid && best_cpu >= 0 && !CPU_ISSET(best_cpu, &leo_fast_set);
    static int repins;
    if (off) {
        int n = leo_repin_threads();
        if (repins++ < 3) {
            leo_log_cpuset("emulator found on a slow core");
            info_report("htc-leo: re-pinned %d threads to the fast cores", n);
        }
    }
    if (last && best_cpu >= 0 && now - last_log >= 10000) {
        last_log = now;
        char path[96], *f = NULL;
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", best_cpu);
        g_file_get_contents(path, &f, NULL, NULL);
        info_report("htc-leo: busiest thread %d on CPU %d, %lu%% of a core%s%s",
                    best_tid, best_cpu,
                    best * 100 / (sysconf(_SC_CLK_TCK) * (now - last) / 1000 + 1),
                    f ? ", MHz " : "", f ? g_strstrip(f) : "");
        g_free(f);
    }
    memset(prev_tid, 0, sizeof(prev_tid));
    for (int i = 0; i < k; i++) {
        prev_tid[i] = tids[i];
        prev_ticks[i] = ticks[i];
    }
    last = now;
#endif
}
