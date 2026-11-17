/*
 * Broadcom BCM4329 Wi-Fi as seen by the HD2's dhd driver (bcm4329.ko).
 *
 * The real chip is an SDIO dongle running its own firmware.  The guest
 * driver downloads that firmware, then talks to it through three SDIO
 * functions:
 *   F0  CCCR/FBR/CIS (card setup),
 *   F1  the SDIO core's misc registers plus a 32 KiB window onto the chip's
 *       backplane (chipcommon, ARM, SOCRAM, SDIO core registers, RAM),
 *   F2  SDPCM frames: ioctls (CDC) on channel 0, firmware events on 1,
 *       802.3 data with a BDC header on 2.
 * Interrupts reach the host on an out-of-band GPIO (152, active high).
 *
 * This model keeps the backplane as plain register storage (enough for the
 * driver's core enumeration, reset and download sequences), accepts and
 * ignores the firmware, and answers the ioctls itself: one open access
 * point ("HD2-Emulator") is always in range and joining it connects the
 * guest to a QEMU network backend (normally -netdev user,id=wifi: NAT
 * through the host's own connection).
 *
 * References: drivers/net/wireless/bcm4329 in the leo kernel (dhd_sdio.c,
 * dhd_cdc.c, siutils.c, sbutils.c, wl_iw.c and include/).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "qemu/main-loop.h"
#include "qemu/cutils.h"
#include "hw/irq.h"
#include "net/net.h"
#include "msm.h"
#include "leo_bcm4329.h"

/* ---- chip layout ---- */
#define SI_ENUM_BASE        0x18000000u
#define CORE_SIZE           0x1000u
#define RAM_SIZE            (288 * 1024)        /* SOCRAM rev 3: 9 x 32 KiB */
#define CHIPID              (0x4329u | (3u << 16) | (5u << 24)) /* rev 3, 5 cores, SB */

enum { CORE_CC, CORE_D11, CORE_ARM, CORE_SDIOD, CORE_SOCRAM, NCORES };
static const struct { uint16_t id; uint8_t rev; } cores[NCORES] = {
    [CORE_CC]     = { 0x800, 20 },  /* chipcommon */
    [CORE_D11]    = { 0x812, 16 },  /* 802.11 MAC */
    [CORE_ARM]    = { 0x82a, 1 },   /* ARM Cortex-M3 */
    [CORE_SDIOD]  = { 0x829, 4 },   /* SDIO device */
    [CORE_SOCRAM] = { 0x80e, 3 },   /* on-chip RAM */
};

/* sbconfig registers, at the top of each core's 4 KiB */
#define SBTMSTATELOW        0xf98
#define SBTMSTATEHIGH       0xf9c
#define SBIDLOW             0xff8
#define SBIDHIGH            0xffc
#define SBTML_RESET         0x1
#define SBTML_CLK_EN        0x10000

/* chipcommon */
#define CC_CHIPID           0x000
#define CC_CAPABILITIES     0x004
#define CC_CAP_PMU          0x10000000
#define CC_PMUCAPS          0x604
#define CC_PMUSTATUS        0x608

/* SDIO device core */
#define SD_INTSTATUS        0x020
#define SD_HOSTINTMASK      0x024
#define SD_TOSBMAILBOX      0x040
#define SD_TOHOSTMAILBOX    0x044
#define SD_TOSBMAILBOXDATA  0x048
#define SD_TOHOSTMBDATA     0x04c
#define I_HMB_FRAME_IND     (1u << 6)
#define I_HMB_HOST_INT      (1u << 7)

/* F1 misc registers */
#define F1_WATERMARK        0x10008
#define F1_DEVCTL           0x10009
#define F1_SBADDRLOW        0x1000a
#define F1_SBADDRMID        0x1000b
#define F1_SBADDRHIGH       0x1000c
#define F1_FRAMECTRL        0x1000d
#define F1_CHIPCLKCSR       0x1000e
#define F1_PULLUP           0x1000f
#define SFC_RF_TERM         0x01
#define SFC_WF_TERM         0x02
#define CLK_FORCE_ALP       0x01
#define CLK_FORCE_HT        0x02
#define CLK_ALP_REQ         0x08
#define CLK_HT_REQ          0x10
#define CLK_ALP_AVAIL       0x40
#define CLK_HT_AVAIL        0x80

/* F0 */
#define CCCR_IOEN           0x02
#define CCCR_IORDY          0x03
#define CCCR_INTEN          0x04
#define CCCR_INTPEND        0x05
#define CCCR_ABORT          0x06
#define CIS_COMMON          0x1000
#define CIS_F1              0x1100
#define CIS_F2              0x1200

/* SDPCM */
#define SDPCM_HDRLEN        12
#define CHAN_CONTROL        0
#define CHAN_EVENT          1
#define CHAN_DATA           2
#define TX_WINDOW           32

/* wl ioctls (wlioctl.h) */
#define WLC_GET_MAGIC       0
#define WLC_GET_VERSION     1
#define WLC_UP              2
#define WLC_DOWN            3
#define WLC_GET_RATE        12
#define WLC_GET_INSTANCE    14
#define WLC_GET_INFRA       19
#define WLC_GET_AUTH        21
#define WLC_GET_BSSID       23
#define WLC_GET_SSID        25
#define WLC_SET_SSID        26
#define WLC_GET_CHANNEL     29
#define WLC_GET_SRL         31
#define WLC_GET_LRL         33
#define WLC_GET_RADIO       37
#define WLC_GET_PHYTYPE     39
#define WLC_SCAN            50
#define WLC_SCAN_RESULTS    51
#define WLC_DISASSOC        52
#define WLC_REASSOC         53
#define WLC_GET_PM          85
#define WLC_SET_PM          86
#define WLC_GET_REVINFO     98
#define WLC_GET_CURR_RATESET 114
#define WLC_GET_AP          117
#define WLC_GET_RSSI        127
#define WLC_GET_WSEC        133
#define WLC_GET_PHY_NOISE   135
#define WLC_GET_BSS_INFO    136
#define WLC_GET_BAND        141
#define WLC_GET_ASSOCLIST   159
#define WLC_GET_VALID_CHANNELS 217
#define WLC_GET_KEY_PRIMARY 235
#define WLC_GET_VAR         262
#define WLC_SET_VAR         263
#define BCME_NOTASSOCIATED  (-17)
#define BCME_UNSUPPORTED    (-23)

/* events (bcmevent.h) */
#define WLC_E_SET_SSID      0
#define WLC_E_JOIN          1
#define WLC_E_AUTH          3
#define WLC_E_DISASSOC      11
#define WLC_E_ASSOC         7
#define WLC_E_LINK          16
#define WLC_E_SCAN_COMPLETE 26
#define WLC_E_STATUS_SUCCESS 0
#define WLC_E_STATUS_NO_NETWORKS 3
#define WLC_EVENT_MSG_LINK  0x01

#define AP_SSID             "HD2-Emulator"
#define AP_CHANNEL          6
static const uint8_t ap_bssid[6] = { 0x02, 0x1a, 0x11, 0x48, 0x44, 0x32 };
static const uint8_t ap_rates[12] = {
    0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c
};

struct LeoWifi {
    qemu_irq oob;
    bool powered;

    /* SDIO */
    uint8_t ioen, inten, bus_if, speed, sepint;
    uint16_t blksz[3];

    /* CMD53 in progress */
    int xfn;
    bool xwrite;
    uint32_t xaddr, xlen, xpos;
    uint8_t *xbuf;              /* F1 data, either direction */

    /* F1 */
    uint32_t sbwin;
    uint8_t clkcsr, devctl, watermark, pullup;

    /* backplane */
    GHashTable *regs;           /* address -> value, everything not special */
    uint8_t *ram;
    bool fw_running;
    uint32_t intstatus, hostintmask;

    /* F2 */
    GQueue rxq;                 /* frames for the host (GByteArray) */
    GByteArray *rxcur;
    uint32_t rxpos;
    GByteArray *txacc;          /* host frame being written */
    uint8_t rx_seq;             /* next sequence number we send */
    uint8_t host_seq;           /* next sequence number the host sends */
    uint8_t window_sent;        /* host_seq when our last frame left */

    /* wl state */
    uint8_t mac[6];
    bool up, associated;
    uint8_t eventmask[16];
    GHashTable *iovars;         /* name -> int value, for unknown iovars */
    int scan_pending;           /* 1: iscan, 2: legacy scan */
    QEMUTimer *timer;
    QEMUTimer *join_timer;

    NICState *nic;
    NICConf conf;
    MemReentrancyGuard guard;
};

static int trace = -1;
#define TRACE(...) do { if (trace > 0) qemu_log("bcm4329: " __VA_ARGS__); } while (0)

/* ---------------- interrupts ---------------- */

static void wifi_update_irq(LeoWifi *s)
{
    qemu_set_irq(s->oob, s->powered && (s->intstatus & s->hostintmask) != 0);
}

static void wifi_frame_ready(LeoWifi *s)
{
    s->intstatus |= I_HMB_FRAME_IND;
    wifi_update_irq(s);
}

/* ---------------- frames to the host ---------------- */

static void queue_frame(LeoWifi *s, int chan, const void *hdr, size_t hlen,
                        const void *body, size_t blen)
{
    GByteArray *f = g_byte_array_sized_new(SDPCM_HDRLEN + hlen + blen);
    uint16_t len = SDPCM_HDRLEN + hlen + blen;
    uint8_t h[SDPCM_HDRLEN] = { 0 };

    stw_le_p(&h[0], len);
    stw_le_p(&h[2], ~len);
    h[4] = s->rx_seq++;
    h[5] = chan;
    h[7] = SDPCM_HDRLEN;                /* data offset */
    h[9] = s->host_seq + TX_WINDOW;     /* credit: host may send this far */
    s->window_sent = s->host_seq;
    g_byte_array_append(f, h, sizeof(h));
    if (hlen) {
        g_byte_array_append(f, hdr, hlen);
    }
    if (blen) {
        g_byte_array_append(f, body, blen);
    }
    g_queue_push_tail(&s->rxq, f);
    wifi_frame_ready(s);
}

/* BDC header + Ethernet frame on the event or data channel */
static void queue_eth(LeoWifi *s, int chan, const uint8_t *eth, size_t len)
{
    static const uint8_t bdc[4] = { 1 << 4, 0, 0, 0 };   /* version 1, if 0 */

    queue_frame(s, chan, bdc, sizeof(bdc), eth, len);
}

static void send_event(LeoWifi *s, uint32_t type, uint32_t status,
                       uint32_t reason, uint16_t flags,
                       const void *data, uint32_t dlen)
{
    uint8_t ev[14 + 10 + 48];
    uint8_t *p = ev;

    if (!(s->eventmask[type / 8] & (1 << (type % 8)))) {
        return;
    }
    /* Ethernet header: to us, from the dongle, type BRCM */
    memcpy(p, s->mac, 6);
    memcpy(p + 6, s->mac, 6);
    stw_be_p(p + 12, 0x886c);
    p += 14;
    /* bcmeth_hdr_t: vendor long subtype, BRCM OUI, event user subtype */
    stw_be_p(p, 0x8001);
    stw_be_p(p + 2, 10 + 48 + dlen);
    p[4] = 0;
    p[5] = 0x00; p[6] = 0x10; p[7] = 0x18;
    stw_be_p(p + 8, 1);
    p += 10;
    /* wl_event_msg_t, network order */
    memset(p, 0, 48);
    stw_be_p(p, 1);
    stw_be_p(p + 2, flags);
    stl_be_p(p + 4, type);
    stl_be_p(p + 8, status);
    stl_be_p(p + 12, reason);
    stl_be_p(p + 16, 0);                /* auth type: open */
    stl_be_p(p + 20, dlen);
    memcpy(p + 24, s->associated || type == WLC_E_SET_SSID ? ap_bssid : s->mac, 6);
    pstrcpy((char *)p + 30, 16, "wl0");

    g_autofree uint8_t *buf = g_malloc(sizeof(ev) + dlen);
    memcpy(buf, ev, sizeof(ev));
    if (dlen) {
        memcpy(buf + sizeof(ev), data, dlen);
    }
    TRACE("event %u status %u flags %x\n", type, status, flags);
    queue_eth(s, CHAN_EVENT, buf, sizeof(ev) + dlen);
}

/* ---------------- the access point ---------------- */

/* wl_bss_info_t (version 108) followed by its IEs; returns the length */
static size_t put_bss_info(uint8_t *b)
{
    uint8_t *ie;
    size_t ielen;

    memset(b, 0, 124);
    stl_le_p(b + 0, 108);               /* version */
    memcpy(b + 8, ap_bssid, 6);
    stw_le_p(b + 14, 100);              /* beacon period */
    stw_le_p(b + 16, 0x0421);           /* ESS, short preamble, short slot */
    b[18] = strlen(AP_SSID);
    memcpy(b + 19, AP_SSID, strlen(AP_SSID));
    stl_le_p(b + 52, sizeof(ap_rates));
    memcpy(b + 56, ap_rates, sizeof(ap_rates));
    stw_le_p(b + 72, 0x2000 | 0x0800 | 0x0300 | AP_CHANNEL);   /* chanspec */
    b[76] = 1;                          /* dtim */
    stw_le_p(b + 78, (uint16_t)-45);    /* RSSI */
    b[80] = (uint8_t)-92;               /* noise */
    b[88] = AP_CHANNEL;                 /* control channel */

    ie = b + 124;
    ie[0] = 0; ie[1] = strlen(AP_SSID);
    memcpy(ie + 2, AP_SSID, ie[1]);
    ie += 2 + ie[1];
    ie[0] = 1; ie[1] = 8;
    memcpy(ie + 2, ap_rates, 8);
    ie += 10;
    ie[0] = 3; ie[1] = 1; ie[2] = AP_CHANNEL;
    ie += 3;
    ie[0] = 50; ie[1] = 4;
    memcpy(ie + 2, ap_rates + 8, 4);
    ie += 6;
    ielen = ie - (b + 124);
    stw_le_p(b + 116, 124);             /* ie_offset */
    stl_le_p(b + 120, ielen);
    size_t len = ROUND_UP(124 + ielen, 4);
    stl_le_p(b + 4, len);
    return len;
}

/* wl_scan_results_t with our one BSS */
static size_t put_scan_results(uint8_t *b, size_t room)
{
    if (room < 12 + 256) {
        return 0;
    }
    size_t n = put_bss_info(b + 12);
    stl_le_p(b + 0, 12 + n);            /* buflen */
    stl_le_p(b + 4, 108);               /* version */
    stl_le_p(b + 8, 1);                 /* count */
    return 12 + n;
}

static void wifi_timer(void *opaque)
{
    LeoWifi *s = opaque;

    if (s->scan_pending) {
        s->scan_pending = 0;
        send_event(s, WLC_E_SCAN_COMPLETE, WLC_E_STATUS_SUCCESS, 0, 0, NULL, 0);
    }
}

static void wifi_join(void *opaque)
{
    LeoWifi *s = opaque;
    uint8_t ssid[4 + 32] = { 0 };

    s->associated = true;
    send_event(s, WLC_E_AUTH, WLC_E_STATUS_SUCCESS, 0, 0, NULL, 0);
    send_event(s, WLC_E_ASSOC, WLC_E_STATUS_SUCCESS, 0, 0, NULL, 0);
    send_event(s, WLC_E_JOIN, WLC_E_STATUS_SUCCESS, 0, 0, NULL, 0);
    send_event(s, WLC_E_LINK, WLC_E_STATUS_SUCCESS, 0, WLC_EVENT_MSG_LINK,
               NULL, 0);
    stl_le_p(ssid, strlen(AP_SSID));
    memcpy(ssid + 4, AP_SSID, strlen(AP_SSID));
    send_event(s, WLC_E_SET_SSID, WLC_E_STATUS_SUCCESS, 0, 0, ssid, 4 + 32);
    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

static void wifi_leave(LeoWifi *s)
{
    if (s->associated) {
        send_event(s, WLC_E_DISASSOC, WLC_E_STATUS_SUCCESS, 8, 0, NULL, 0);
        s->associated = false;
        send_event(s, WLC_E_LINK, WLC_E_STATUS_SUCCESS, 0, 0, NULL, 0);
    }
}

/* ---------------- ioctls ---------------- */

static int iovar_get_int(LeoWifi *s, const char *name, int def)
{
    gpointer v;

    if (g_hash_table_lookup_extended(s->iovars, name, NULL, &v)) {
        return GPOINTER_TO_INT(v);
    }
    return def;
}

/*
 * Handle one ioctl.  "buf" holds the request data and receives the reply
 * (up to "outlen" bytes, zero filled).  Returns a BCME_* status.
 */
static int wl_ioctl(LeoWifi *s, uint32_t cmd, bool set, uint8_t *buf,
                    uint32_t inlen, uint32_t outlen)
{
    g_autofree uint8_t *in = g_memdup2(buf, inlen);

    if (!set) {
        memset(buf, 0, outlen);
    }
#define PUT32(v) do { if (outlen >= 4) stl_le_p(buf, (v)); } while (0)

    switch (cmd) {
    case WLC_GET_MAGIC:     PUT32(0x14e46c77); return 0;
    case WLC_GET_VERSION:   PUT32(1); return 0;
    case WLC_UP:            s->up = true; return 0;
    case WLC_DOWN:
        wifi_leave(s);
        s->up = false;
        return 0;
    case WLC_GET_RATE:      PUT32(108); return 0;      /* 54 Mbit/s */
    case WLC_GET_INSTANCE:  PUT32(0); return 0;
    case WLC_GET_INFRA:     PUT32(1); return 0;
    case WLC_GET_AUTH:      PUT32(0); return 0;
    case WLC_GET_RADIO:     PUT32(0); return 0;        /* not disabled */
    case WLC_GET_PHYTYPE:   PUT32(2); return 0;        /* G */
    case WLC_GET_SRL:       PUT32(7); return 0;
    case WLC_GET_LRL:       PUT32(4); return 0;
    case WLC_GET_WSEC:      PUT32(iovar_get_int(s, "wsec", 0)); return 0;
    case WLC_GET_PM:        PUT32(iovar_get_int(s, "PM", 0)); return 0;
    case WLC_SET_PM:
        g_hash_table_insert(s->iovars, g_strdup("PM"),
                            GINT_TO_POINTER(inlen >= 4 ? ldl_le_p(in) : 0));
        return 0;
    case WLC_GET_PHY_NOISE: PUT32((uint32_t)-92); return 0;
    case WLC_GET_BAND:      PUT32(0); return 0;        /* auto */
    case WLC_GET_AP:        PUT32(0); return 0;
    case WLC_GET_KEY_PRIMARY: PUT32(0); return 0;
    case WLC_GET_ASSOCLIST: PUT32(0); return 0;
    case WLC_GET_RSSI:                  /* 0 while not associated */
        PUT32(s->associated ? (uint32_t)-45 : 0);
        return 0;
    case WLC_GET_CHANNEL:
        if (outlen >= 12) {
            stl_le_p(buf, AP_CHANNEL);
            stl_le_p(buf + 4, AP_CHANNEL);
            stl_le_p(buf + 8, 0);
        }
        return 0;
    case WLC_GET_CURR_RATESET:
        if (outlen >= 4 + sizeof(ap_rates)) {
            stl_le_p(buf, sizeof(ap_rates));
            memcpy(buf + 4, ap_rates, sizeof(ap_rates));
        }
        return 0;
    case WLC_GET_VALID_CHANNELS:
        if (outlen >= 4 + 11 * 4) {
            stl_le_p(buf, 11);
            for (int i = 0; i < 11; i++) {
                stl_le_p(buf + 4 + i * 4, i + 1);
            }
        }
        return 0;
    case WLC_GET_REVINFO:
        if (outlen >= 64) {
            stl_le_p(buf + 0, 0x14e4);     /* vendorid */
            stl_le_p(buf + 4, 0x4329);     /* deviceid */
            stl_le_p(buf + 8, 0);          /* radiorev */
            stl_le_p(buf + 12, 3);         /* chiprev */
            stl_le_p(buf + 16, 16);        /* corerev */
            stl_le_p(buf + 32, 0x04ae);    /* driverrev */
            stl_le_p(buf + 40, 0x4329);    /* chipnum */
            stl_le_p(buf + 52, 2);         /* phytype */
        }
        return 0;
    case WLC_GET_BSSID:
        if (!s->associated) {
            return BCME_NOTASSOCIATED;
        }
        if (outlen >= 6) {
            memcpy(buf, ap_bssid, 6);
        }
        return 0;
    case WLC_GET_SSID:
        if (s->associated && outlen >= 4 + 32) {
            stl_le_p(buf, strlen(AP_SSID));
            memcpy(buf + 4, AP_SSID, strlen(AP_SSID));
        }
        return 0;
    case WLC_GET_BSS_INFO:
        if (!s->associated) {
            return BCME_NOTASSOCIATED;
        }
        if (outlen >= 4 + 256) {
            stl_le_p(buf, outlen);
            put_bss_info(buf + 4);
        }
        return 0;
    case WLC_SET_SSID:
    case WLC_REASSOC: {
        uint32_t len = inlen >= 4 ? MIN(ldl_le_p(in), 32) : 0;
        bool ours = len == strlen(AP_SSID) && !memcmp(in + 4, AP_SSID, len);

        TRACE("join \"%.*s\"\n", (int)len, (const char *)in + 4);
        if (cmd == WLC_SET_SSID && s->associated) {
            wifi_leave(s);
        }
        if (len == 0) {
            return 0;                   /* just disassociate */
        }
        if (ours) {
            timer_mod(s->join_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
        } else {
            send_event(s, WLC_E_SET_SSID, WLC_E_STATUS_NO_NETWORKS, 0, 0,
                       NULL, 0);
        }
        return 0;
    }
    case WLC_DISASSOC:
        timer_del(s->join_timer);
        wifi_leave(s);
        return 0;
    case WLC_SCAN:
        s->scan_pending = 2;
        timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 300);
        return 0;
    case WLC_SCAN_RESULTS:
        put_scan_results(buf, outlen);
        return 0;
    case WLC_GET_VAR:
    case WLC_SET_VAR: {
        size_t nlen = strnlen((char *)in, inlen);
        g_autofree char *name = g_strndup((char *)in, nlen);
        const uint8_t *val = in + MIN(nlen + 1, inlen);
        uint32_t vlen = inlen - (val - in);

        if (cmd == WLC_SET_VAR) {
            TRACE("set %s (%u bytes)\n", name, vlen);
            if (!strcmp(name, "cur_etheraddr") && vlen >= 6) {
                memcpy(s->mac, val, 6);
            } else if (!strcmp(name, "event_msgs") && vlen >= 16) {
                memcpy(s->eventmask, val, 16);
            } else if (!strcmp(name, "iscan")) {
                /* wl_iscan_params_t: version, action (1 start, 2 continue) */
                s->scan_pending = 1;
                timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 300);
            } else if (vlen >= 4) {
                g_hash_table_insert(s->iovars, g_strdup(name),
                                    GINT_TO_POINTER(ldl_le_p(val)));
            }
            return 0;
        }
        TRACE("get %s\n", name);
        if (!strcmp(name, "cur_etheraddr")) {
            memcpy(buf, s->mac, MIN(6, outlen));
        } else if (!strcmp(name, "ver")) {
            pstrcpy((char *)buf, outlen,
                    "wl0: Nov 17 2026 HD2 emulator version 4.218.248.18\n");
        } else if (!strcmp(name, "event_msgs")) {
            memcpy(buf, s->eventmask, MIN(16, outlen));
        } else if (!strcmp(name, "iscanresults")) {
            if (outlen >= 4) {
                stl_le_p(buf, 0);       /* WL_SCAN_RESULTS_SUCCESS */
                put_scan_results(buf + 4, outlen - 4);
            }
        } else if (!strcmp(name, "country")) {
            /* wl_country_t: abbrev[4], rev, ccode[4] */
            if (outlen >= 12) {
                memcpy(buf, "US", 3);
                stl_le_p(buf + 4, 0);
                memcpy(buf + 8, "US", 3);
            }
        } else if (!strcmp(name, "rtsthresh")) {
            PUT32(iovar_get_int(s, name, 2347));
        } else if (!strcmp(name, "fragthresh")) {
            PUT32(iovar_get_int(s, name, 2346));
        } else if (!strcmp(name, "qtxpower")) {
            PUT32(iovar_get_int(s, name, 72));
        } else if (!strcmp(name, "nmode")) {
            PUT32(iovar_get_int(s, name, 0));
        } else if (!strcmp(name, "mpc")) {
            PUT32(iovar_get_int(s, name, 1));
        } else {
            PUT32(iovar_get_int(s, name, 0));
        }
        return 0;
    }
    default:
        TRACE("ioctl %u (%s) ignored\n", cmd, set ? "set" : "get");
        return 0;
    }
#undef PUT32
}

/* a CDC request on the control channel */
static void handle_control(LeoWifi *s, const uint8_t *p, uint32_t len)
{
    if (len < 16) {
        return;
    }
    uint32_t cmd = ldl_le_p(p), mlen = ldl_le_p(p + 4);
    uint32_t flags = ldl_le_p(p + 8);
    uint32_t outlen = mlen & 0xffff;
    uint32_t inlen = MIN(len - 16, outlen);
    bool set = flags & 2;
    g_autofree uint8_t *buf = g_malloc0(MAX(outlen, inlen) + 1);
    uint8_t hdr[16];
    int status;

    memcpy(buf, p + 16, inlen);
    status = wl_ioctl(s, cmd, set, buf, inlen, outlen);

    stl_le_p(hdr, cmd);
    stl_le_p(hdr + 4, outlen);
    stl_le_p(hdr + 8, (flags & 0xfffff002) | (status ? 1 : 0));
    stl_le_p(hdr + 12, status);
    /* answers to sets carry nothing worth copying back */
    queue_frame(s, CHAN_CONTROL, hdr, 16, buf, set ? MIN(outlen, 64) : outlen);
}

static void handle_host_frame(LeoWifi *s, const uint8_t *f, uint32_t len)
{
    uint8_t chan = f[5] & 0x0f, doff = f[7];

    s->host_seq = f[4] + 1;
    if (doff < SDPCM_HDRLEN || doff > len) {
        return;
    }
    switch (chan) {
    case CHAN_CONTROL:
        handle_control(s, f + doff, len - doff);
        return;
    case CHAN_DATA: {
        const uint8_t *bdc = f + doff;
        uint32_t blen = len - doff;
        if (blen < 4 || blen < 4 + bdc[3] * 4u) {
            return;
        }
        uint32_t off = 4 + bdc[3] * 4;
        if (s->associated && s->up) {
            qemu_send_packet(qemu_get_queue(s->nic), bdc + off, blen - off);
        }
        break;
    }
    default:
        break;
    }
    /* keep the host's credit window open even when we have nothing to say */
    if ((uint8_t)(s->host_seq - s->window_sent) >= TX_WINDOW / 2) {
        queue_frame(s, CHAN_DATA, NULL, 0, NULL, 0);
    }
}

/* ---------------- network backend ---------------- */

static bool wifi_can_receive(NetClientState *nc)
{
    LeoWifi *s = qemu_get_nic_opaque(nc);

    return s->powered && s->up && s->associated && g_queue_get_length(&s->rxq) < 64;
}

static ssize_t wifi_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    LeoWifi *s = qemu_get_nic_opaque(nc);

    if (!s->powered || !s->associated || size > 1514 + 4) {
        return size;
    }
    queue_eth(s, CHAN_DATA, buf, size);
    return size;
}

static NetClientInfo wifi_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = wifi_can_receive,
    .receive = wifi_receive,
};

/* ---------------- backplane ---------------- */

static uint32_t bp_reg(LeoWifi *s, uint32_t a)
{
    return GPOINTER_TO_UINT(g_hash_table_lookup(s->regs, GUINT_TO_POINTER(a)));
}

static void bp_set(LeoWifi *s, uint32_t a, uint32_t v)
{
    g_hash_table_insert(s->regs, GUINT_TO_POINTER(a), GUINT_TO_POINTER(v));
}

static uint32_t bp_read32(LeoWifi *s, uint32_t a)
{
    a &= ~3u;
    if (a < RAM_SIZE) {
        return ldl_le_p(s->ram + a);
    }
    if (a >= SI_ENUM_BASE && a < SI_ENUM_BASE + NCORES * CORE_SIZE) {
        int core = (a - SI_ENUM_BASE) / CORE_SIZE;
        uint32_t off = a & (CORE_SIZE - 1);

        switch (off) {
        case SBIDHIGH:
            return 0x42430000u | ((cores[core].rev & 0x70) << 8) |
                   ((cores[core].id & 0x8ff) << 4) | (cores[core].rev & 0xf);
        case SBIDLOW:
            return 0;
        case SBTMSTATEHIGH:
            return 0;                   /* never busy */
        }
        switch (core) {
        case CORE_CC:
            switch (off) {
            case CC_CHIPID:        return CHIPID;
            case CC_CAPABILITIES:  return CC_CAP_PMU | 0x00040000;
            case CC_PMUCAPS:       return 0x00000001;   /* PMU rev 1 */
            case CC_PMUSTATUS:     return 0x4;          /* HT available */
            }
            break;
        case CORE_SDIOD:
            switch (off) {
            case SD_INTSTATUS:     return s->intstatus;
            case SD_HOSTINTMASK:   return s->hostintmask;
            }
            break;
        case CORE_SOCRAM:
            if (off == 0) {
                return (9 << 4) | 1;    /* coreinfo: 9 banks of 32 KiB */
            }
            break;
        }
    }
    return bp_reg(s, a);
}

static void bp_write32(LeoWifi *s, uint32_t a, uint32_t v)
{
    a &= ~3u;
    if (a < RAM_SIZE) {
        stl_le_p(s->ram + a, v);
        return;
    }
    if (a >= SI_ENUM_BASE && a < SI_ENUM_BASE + NCORES * CORE_SIZE) {
        int core = (a - SI_ENUM_BASE) / CORE_SIZE;
        uint32_t off = a & (CORE_SIZE - 1);

        if (core == CORE_ARM && off == SBTMSTATELOW) {
            bool run = (v & SBTML_CLK_EN) && !(v & SBTML_RESET);
            if (run != s->fw_running) {
                TRACE("ARM %s\n", run ? "released: firmware running" : "halted");
            }
            s->fw_running = run;
        }
        if (core == CORE_SDIOD) {
            switch (off) {
            case SD_INTSTATUS:
                s->intstatus &= ~v;
                /* frames still queued: indicate them again */
                if (!g_queue_is_empty(&s->rxq) ||
                    (s->rxcur && s->rxpos < s->rxcur->len)) {
                    s->intstatus |= I_HMB_FRAME_IND;
                }
                wifi_update_irq(s);
                return;
            case SD_HOSTINTMASK:
                s->hostintmask = v;
                wifi_update_irq(s);
                return;
            }
        }
    }
    bp_set(s, a, v);
}

static uint8_t bp_read8(LeoWifi *s, uint32_t a)
{
    if (a < RAM_SIZE) {
        return s->ram[a];
    }
    return bp_read32(s, a) >> ((a & 3) * 8);
}

static void bp_write8(LeoWifi *s, uint32_t a, uint8_t v)
{
    if (a < RAM_SIZE) {
        s->ram[a] = v;
        return;
    }
    uint32_t w = bp_read32(s, a), sh = (a & 3) * 8;
    bp_write32(s, a, (w & ~(0xffu << sh)) | ((uint32_t)v << sh));
}

/* ---------------- SDIO functions ---------------- */

static const uint8_t cis_common[] = {
    0x20, 4, 0xd0, 0x02, 0x29, 0x43,            /* MANFID: Broadcom 4329 */
    0x21, 2, 0x0c, 0x00,                        /* FUNCID: SDIO */
    0x22, 4, 0x00, 0x40, 0x00, 0x32,            /* FUNCE: 64-byte blocks, 25 MHz */
    0xff,
};

static int cis_func(int fn, uint8_t *out)
{
    uint8_t *p = out;
    uint16_t blk = fn == 1 ? 64 : 512;

    /*
     * MANFID in each function's CIS: the leo board hands the MMC core an
     * all-zero "embedded" common CIS, so this is where the driver match
     * (vendor 0x02d0, device 0x4329) comes from, as on the real chip.
     */
    *p++ = 0x20; *p++ = 4; *p++ = 0xd0; *p++ = 0x02; *p++ = 0x29; *p++ = 0x43;
    *p++ = 0x21; *p++ = 2; *p++ = 0x0c; *p++ = 0x00;
    *p++ = 0x22; *p++ = 42;                     /* FUNCE (function, v1.1) */
    memset(p, 0, 42);
    p[0] = 1;
    p[12] = blk & 0xff;
    p[13] = blk >> 8;
    p[28] = 100;                                /* enable timeout: 1 s */
    p += 42;
    *p++ = 0xff;
    return p - out;
}

static uint8_t f0_read(LeoWifi *s, uint32_t a)
{
    if (a >= CIS_COMMON && a < CIS_COMMON + sizeof(cis_common)) {
        return cis_common[a - CIS_COMMON];
    }
    if ((a >= CIS_F1 && a < CIS_F1 + 0x100) || (a >= CIS_F2 && a < CIS_F2 + 0x100)) {
        uint8_t cis[64];
        int n = cis_func(a >= CIS_F2 ? 2 : 1, cis);
        uint32_t i = a & 0xff;
        return i < n ? cis[i] : 0xff;
    }
    if (a >= 0x100 && a < 0x300) {
        int fn = a >> 8;
        switch (a & 0xff) {
        case 0x09: return (fn == 1 ? CIS_F1 : CIS_F2) & 0xff;
        case 0x0a: return (fn == 1 ? CIS_F1 : CIS_F2) >> 8;
        case 0x0b: return 0;
        case 0x10: return s->blksz[fn] & 0xff;
        case 0x11: return s->blksz[fn] >> 8;
        default:   return 0;
        }
    }
    switch (a) {
    case 0x00: return 0x32;             /* CCCR 1.20, SDIO 2.00 */
    case 0x01: return 0x02;
    case CCCR_IOEN:  return s->ioen;
    case CCCR_IORDY: return s->ioen & (2 | (s->fw_running ? 4 : 0));
    case CCCR_INTEN: return s->inten;
    case CCCR_INTPEND:
        return (s->intstatus & s->hostintmask) ? 2 : 0;
    case 0x07: return s->bus_if;
    case 0x08: return 0x17;             /* SMB, SRW, S4MI, SDC */
    case 0x09: return CIS_COMMON & 0xff;
    case 0x0a: return CIS_COMMON >> 8;
    case 0x0b: return 0;
    case 0x10: return s->blksz[0] & 0xff;
    case 0x11: return s->blksz[0] >> 8;
    case 0x12: return 0x01;             /* SMPC */
    case 0x13: return 0x01 | s->speed;  /* SHS */
    case 0xf2: return s->sepint;
    default:   return 0;
    }
}

static void f2_abort(LeoWifi *s)
{
    g_byte_array_set_size(s->txacc, 0);
}

static void f0_write(LeoWifi *s, uint32_t a, uint8_t v)
{
    if (a >= 0x100 && a < 0x300) {
        int fn = a >> 8;
        if ((a & 0xff) == 0x10) {
            s->blksz[fn] = (s->blksz[fn] & 0xff00) | v;
        } else if ((a & 0xff) == 0x11) {
            s->blksz[fn] = (s->blksz[fn] & 0x00ff) | (v << 8);
        }
        return;
    }
    switch (a) {
    case CCCR_IOEN:
        s->ioen = v & 6;
        TRACE("IOEN %x\n", v);
        break;
    case CCCR_INTEN:  s->inten = v; break;
    case CCCR_ABORT:
        if ((v & 7) == 2) {
            f2_abort(s);
        }
        break;
    case 0x07: s->bus_if = v; break;
    case 0x10: s->blksz[0] = (s->blksz[0] & 0xff00) | v; break;
    case 0x11: s->blksz[0] = (s->blksz[0] & 0x00ff) | (v << 8); break;
    case 0x13: s->speed = v & 2; break;
    case 0xf2: s->sepint = v; break;
    default: break;
    }
}

static uint8_t clkcsr_read(LeoWifi *s)
{
    uint8_t v = s->clkcsr & 0x3f;

    if (v & (CLK_HT_REQ | CLK_FORCE_HT)) {
        v |= CLK_ALP_AVAIL | CLK_HT_AVAIL;
    } else if (v & (CLK_ALP_REQ | CLK_FORCE_ALP)) {
        v |= CLK_ALP_AVAIL;
    }
    return v;
}

static uint8_t f1_read8(LeoWifi *s, uint32_t a)
{
    if (a >= 0x10000) {
        switch (a) {
        case F1_WATERMARK:   return s->watermark;
        case F1_DEVCTL:      return s->devctl;
        case F1_SBADDRLOW:   return (s->sbwin >> 8) & 0x80;
        case F1_SBADDRMID:   return s->sbwin >> 16;
        case F1_SBADDRHIGH:  return s->sbwin >> 24;
        case F1_CHIPCLKCSR:  return clkcsr_read(s);
        case F1_PULLUP:      return s->pullup;
        default:             return 0;   /* frame byte counts etc. */
        }
    }
    return bp_read8(s, (s->sbwin & 0xffff8000u) | (a & 0x7fff));
}

static void f1_write8(LeoWifi *s, uint32_t a, uint8_t v)
{
    if (a >= 0x10000) {
        switch (a) {
        case F1_WATERMARK:  s->watermark = v; break;
        case F1_DEVCTL:     s->devctl = v; break;
        case F1_SBADDRLOW:  s->sbwin = (s->sbwin & 0xffff0000u) | ((v & 0x80) << 8); break;
        case F1_SBADDRMID:  s->sbwin = (s->sbwin & 0xff00ffffu) | (v << 16); break;
        case F1_SBADDRHIGH: s->sbwin = (s->sbwin & 0x00ffffffu) | (v << 24); break;
        case F1_FRAMECTRL:
            if (v & SFC_RF_TERM) {          /* drop the rest of this frame */
                if (s->rxcur) {
                    s->rxpos = s->rxcur->len;
                }
            }
            if (v & SFC_WF_TERM) {
                f2_abort(s);
            }
            break;
        case F1_CHIPCLKCSR: s->clkcsr = v; break;
        case F1_PULLUP:     s->pullup = v; break;
        default: break;
        }
        return;
    }
    bp_write8(s, (s->sbwin & 0xffff8000u) | (a & 0x7fff), v);
}

/* F1 CMD53: 4-byte register accesses or byte streams into RAM */
static void f1_read_block(LeoWifi *s, uint32_t a, uint8_t *buf, uint32_t len)
{
    uint32_t bp = (s->sbwin & 0xffff8000u) | (a & 0x7fff);

    if (a < 0x10000 && len == 4 && !(bp & 3)) {
        stl_le_p(buf, bp_read32(s, bp));
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = f1_read8(s, a + i);
    }
}

static void f1_write_block(LeoWifi *s, uint32_t a, const uint8_t *buf,
                           uint32_t len)
{
    uint32_t bp = (s->sbwin & 0xffff8000u) | (a & 0x7fff);

    if (a < 0x10000 && len == 4 && !(bp & 3)) {
        bp_write32(s, bp, ldl_le_p(buf));
        return;
    }
    if (a < 0x10000 && bp + len <= RAM_SIZE) {
        memcpy(s->ram + bp, buf, len);
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        f1_write8(s, a + i, buf[i]);
    }
}

/* F2 read: stream the current host-bound frame, zeros past its end */
static void f2_start_read(LeoWifi *s)
{
    if (!s->rxcur || s->rxpos >= s->rxcur->len) {
        if (s->rxcur) {
            g_byte_array_unref(s->rxcur);
        }
        s->rxcur = g_queue_pop_head(&s->rxq);
        s->rxpos = 0;
        if (!s->rxcur) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
    }
}

static void f2_end_write(LeoWifi *s)
{
    GByteArray *a = s->txacc;

    if (a->len < 4) {
        return;
    }
    uint16_t len = lduw_le_p(a->data), chk = lduw_le_p(a->data + 2);
    if ((uint16_t)(len ^ chk) != 0xffff || len < SDPCM_HDRLEN) {
        TRACE("bad host frame tag %04x/%04x\n", len, chk);
        g_byte_array_set_size(a, 0);
        return;
    }
    if (a->len < len) {
        return;                         /* rest follows in another CMD53 */
    }
    handle_host_frame(s, a->data, len);
    g_byte_array_set_size(a, 0);        /* the padding goes with it */
}

/* ---------------- SDIO commands ---------------- */

static void wifi_reset_card(LeoWifi *s)
{
    s->ioen = s->inten = s->bus_if = s->speed = s->sepint = 0;
    s->blksz[0] = s->blksz[1] = s->blksz[2] = 0;
    s->xlen = s->xpos = 0;
    s->sbwin = SI_ENUM_BASE;
    s->clkcsr = s->devctl = s->watermark = s->pullup = 0;
    s->fw_running = false;
    s->intstatus = s->hostintmask = 0;
    s->up = s->associated = false;
    s->scan_pending = 0;
    timer_del(s->timer);
    timer_del(s->join_timer);
    while (!g_queue_is_empty(&s->rxq)) {
        g_byte_array_unref(g_queue_pop_head(&s->rxq));
    }
    if (s->rxcur) {
        g_byte_array_unref(s->rxcur);
        s->rxcur = NULL;
    }
    g_byte_array_set_size(s->txacc, 0);
    s->rx_seq = 0;
    s->host_seq = s->window_sent = 0xff;
    memset(s->eventmask, 0xff, sizeof(s->eventmask));
    g_hash_table_remove_all(s->regs);
    g_hash_table_remove_all(s->iovars);
    wifi_update_irq(s);
}

static bool wifi_command(void *opaque, uint8_t cmd, uint32_t arg, uint32_t *resp)
{
    LeoWifi *s = opaque;

    if (!s->powered) {
        return false;
    }
    switch (cmd) {
    case 0:                             /* GO_IDLE: no response */
        return true;
    case 5:                             /* IO_SEND_OP_COND: R4 */
        *resp = 0x80000000u | (2u << 28) | 0x00ff8000u;
        return true;
    case 3:                             /* SEND_RELATIVE_ADDR: R6 */
        *resp = 0x00010000u;
        return true;
    case 7:                             /* SELECT_CARD */
        *resp = 0x00001e00u;
        return true;
    case 52: {                          /* IO_RW_DIRECT */
        bool wr = arg >> 31;
        int fn = (arg >> 28) & 7;
        bool raw = arg & (1u << 27);
        uint32_t a = (arg >> 9) & 0x1ffff;
        uint8_t v = arg & 0xff;

        if (fn > 2) {
            *resp = 0x1000 | 0x200;     /* FUNCTION_NUMBER error */
            return true;
        }
        if (wr) {
            if (fn == 0) {
                f0_write(s, a, v);
            } else if (fn == 1) {
                f1_write8(s, a, v);
            }
        }
        if (!wr || raw) {
            v = fn == 0 ? f0_read(s, a) : fn == 1 ? f1_read8(s, a) : 0;
        }
        if (trace > 1 || (trace > 0 && (fn == 0 || a >= 0x10000))) {
            qemu_log("bcm4329: cmd52 %s f%d %05x = %02x\n", wr ? "wr" : "rd",
                     fn, a, v);
        }
        *resp = 0x1000 | v;
        return true;
    }
    case 53: {                          /* IO_RW_EXTENDED */
        int fn = (arg >> 28) & 7;
        bool block = arg & (1u << 27);
        uint32_t count = arg & 0x1ff;

        s->xwrite = arg >> 31;
        s->xfn = fn;
        s->xaddr = (arg >> 9) & 0x1ffff;
        if (block) {
            s->xlen = (count ? count : 512) * MAX(s->blksz[fn], 1);
        } else {
            s->xlen = count ? count : 512;
        }
        s->xpos = 0;
        s->xbuf = g_realloc(s->xbuf, s->xlen);
        TRACE("cmd53 %s f%d %05x len %u (win %08x)\n", s->xwrite ? "wr" : "rd",
              fn, s->xaddr, s->xlen, s->sbwin);
        if (fn == 1 && !s->xwrite) {
            f1_read_block(s, s->xaddr, s->xbuf, s->xlen);
        } else if (fn == 2 && !s->xwrite) {
            f2_start_read(s);
        }
        *resp = 0x1000;
        return true;
    }
    default:
        return false;                   /* not an SD memory card */
    }
}

static void wifi_read(void *opaque, uint8_t *buf, unsigned len)
{
    LeoWifi *s = opaque;

    for (unsigned i = 0; i < len; i++) {
        uint8_t v = 0;
        if (s->xpos < s->xlen) {
            if (s->xfn == 1) {
                v = s->xbuf[s->xpos];
            } else if (s->xfn == 2 && s->rxcur && s->rxpos < s->rxcur->len) {
                v = s->rxcur->data[s->rxpos++];
            } else if (s->xfn == 0) {
                v = f0_read(s, s->xaddr + s->xpos);
            }
            s->xpos++;
        }
        buf[i] = v;
    }
}

static void wifi_write(void *opaque, const uint8_t *buf, unsigned len)
{
    LeoWifi *s = opaque;
    unsigned n = MIN(len, s->xlen - s->xpos);

    if (!n) {
        return;
    }
    if (s->xfn == 1) {
        memcpy(s->xbuf + s->xpos, buf, n);
    } else if (s->xfn == 2) {
        g_byte_array_append(s->txacc, buf, n);
    }
    s->xpos += n;
    if (s->xpos == s->xlen) {
        if (s->xfn == 1) {
            f1_write_block(s, s->xaddr, s->xbuf, s->xlen);
        } else if (s->xfn == 2) {
            f2_end_write(s);
        }
    }
}

static const MsmSdioOps wifi_sdio_ops = {
    .command = wifi_command,
    .read = wifi_read,
    .write = wifi_write,
};

/* WIFI_SHUTDOWN_N: the card exists only while powered */
static void wifi_power(void *opaque, int n, int level)
{
    LeoWifi *s = opaque;

    if (trace < 0) {
        trace = getenv("BCM4329_TRACE") ? atoi(getenv("BCM4329_TRACE")) : 0;
    }
    if (!!level != s->powered) {
        TRACE("power %s\n", level ? "on" : "off");
        s->powered = level;
        wifi_reset_card(s);
    }
}

LeoWifi *leo_bcm4329_init(DeviceState *sdcc, qemu_irq oob, qemu_irq *power_in,
                          NetClientState *peer)
{
    LeoWifi *s = g_new0(LeoWifi, 1);
    static const uint8_t mac[6] = { 0x00, 0x23, 0x76, 0x9e, 0x9e, 0x9e };

    s->oob = oob;
    s->regs = g_hash_table_new(NULL, NULL);
    s->iovars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s->ram = g_malloc0(RAM_SIZE);
    s->txacc = g_byte_array_new();
    g_queue_init(&s->rxq);
    s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, wifi_timer, s);
    s->join_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, wifi_join, s);
    memcpy(s->mac, mac, 6);
    memcpy(s->conf.macaddr.a, mac, 6);
    s->conf.peers.ncs[0] = peer;
    s->conf.peers.queues = 1;
    s->nic = qemu_new_nic(&wifi_net_info, &s->conf, "bcm4329", "wifi0",
                          &s->guard, s);
    wifi_reset_card(s);
    msm_sdcc_attach_sdio(sdcc, &wifi_sdio_ops, s);
    *power_in = qemu_allocate_irq(wifi_power, s, 0);
    return s;
}
