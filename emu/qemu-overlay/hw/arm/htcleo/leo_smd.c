/*
 * Remote end of the Qualcomm SMD (shared memory driver) and of the DAL
 * RPC protocol, as needed by HTC HD2 kernels.
 *
 * On the HD2 (AMSS 1550, WinCE radio) the QDSP6 audio services are
 * reached through SMD channels owned by the modem: the apps side rings
 * A2M doorbell 0 and gets INT_A9_M2A_0 back.  We create the channels in
 * the SMEM allocation table, run the open handshake, and answer DAL
 * calls (arch/arm/mach-msm/qdsp6_1550/{dal,q6audio}.c):
 *
 *  - ATTACH       -> status 0 + service name, our handle as "from"
 *  - DDI calls    -> status 0, empty output buffer
 *  - audio INIT   -> remember event cookie/session for the client
 *  - audio OPEN   -> async ADSP_AUDIO_EVT_STATUS_OPEN
 *  - audio WRITE  -> async BUF_DONE after the buffer's playback time
 *  - audio READ   -> async BUF_DONE after the buffer's capture time
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/cutils.h"
#include "hw/irq.h"
#include "system/rtc.h"
#include "system/address-spaces.h"
#include "msm_a9.h"

/* SMEM item ids (CONFIG_MSM_N_WAY_SMD numbering) */
#define SMEM_CHANNEL_ALLOC_TBL  13
#define SMEM_SMD_BASE_ID        14
#define SMEM_SMD_FIFO_BASE_ID   338

#define SMD_TYPE_APPS_MODEM     0x000
#define SMD_KIND_STREAM         0x100
#define SMD_KIND_PACKET         0x200
#define SMD_FIFO_SIZE           16384   /* both directions, power of two */

/* half channel layout */
#define HC_STATE    0
#define HC_FDSR     4
#define HC_FCTS     5
#define HC_FCD      6
#define HC_FHEAD    8
#define HC_FTAIL    9
#define HC_FSTATE   10
#define HC_TAIL     12
#define HC_HEAD     16
#define HC_SIZE     20

#define SMD_SS_CLOSED       0
#define SMD_SS_OPENING      1
#define SMD_SS_OPENED       2
#define SMD_SS_CLOSING      4

/* DAL */
#define DAL_HDR_SIZE        16
#define DAL_VERSION         0x11
#define DAL_MSGID_DDI       0x00
#define DAL_MSGID_ATTACH    0x01
#define DAL_MSGID_DETACH    0x02
#define DAL_MSGID_ASYNCH    0xC0
#define DAL_MSGID_REPLY     0x80
#define DAL_HANDLE_BASE     0x5A1D0000

#define AUDIO_DAL_DEVICE    0x02000028
#define DAL_OP_FIRST_DEVICE_API 9
enum {
    AUDIO_OP_OPEN = DAL_OP_FIRST_DEVICE_API,
    AUDIO_OP_WRITE,
    AUDIO_OP_READ,
    AUDIO_OP_IOCTL,
    AUDIO_OP_INIT,
    AUDIO_OP_CLOSE,
    AUDIO_OP_FLUSH,
};

#define ADSP_AUDIO_EVT_STATUS_OPEN      0x0108c0d6
#define ADSP_AUDIO_EVT_STATUS_CLOSE     0x0108c0d7
#define ADSP_AUDIO_EVT_STATUS_BUF_DONE  0x0108c0d8

/* PCM format when the OPEN command did not say: 44.1 kHz, stereo, s16 */
#define PCM_DEFAULT_RATE    44100
#define PCM_DEFAULT_CH      2
#define ADSP_AUDIO_FORMAT_PCM           0x0103d2fd
#define ADSP_AUDIO_OPCODE_OPEN_WRITE    0x02

/*
 * The kernel names a channel "SMD_<name>" (type APPS_MODEM) or
 * "DSP_<name>" (type APPS_DSP); q6audio opens DSP_DAL_AQ_AUD and the
 * ACDB/ADIE services live on SMD_DAL00.
 */
#define SMD_TYPE_APPS_DSP       0x001
static const struct {
    const char *name;
    uint32_t cid;
    uint32_t type;
    uint32_t kind;
    bool dal;
} a9_channels[] = {
    { "DAL_AQ_AUD", 1, SMD_TYPE_APPS_MODEM, SMD_KIND_STREAM, true },
    { "DAL00",      2, SMD_TYPE_APPS_MODEM, SMD_KIND_STREAM, true },
    /* radio channels: opened by rild / QMI, kept silent (no modem) */
    { "DS",         3, SMD_TYPE_APPS_MODEM, SMD_KIND_STREAM, false },
    { "CONTROL",    4, SMD_TYPE_APPS_MODEM, SMD_KIND_PACKET, false },
    /* ONCRPC router: PMIC, power, USB, HTC battery/acoustic services */
    { "RPCCALL",    5, SMD_TYPE_APPS_MODEM, SMD_KIND_STREAM, false },
};

/* ---------------- ONCRPC router (smd_rpcrouter.c) ---------------- */

#define RR_VERSION              1
#define RR_HDR_SIZE             32
#define RR_ROUTER_ADDRESS       0xfffffffe
#define RR_PID_REMOTE           0
#define RR_CMD_DATA             1
#define RR_CMD_HELLO            2
#define RR_CMD_NEW_SERVER       4
#define RR_CMD_RESUME_TX        7

#define RPC_MSG_CALL            0
#define RPC_MSG_REPLY           1
#define RPC_SERVER_CID_BASE     0x100

/* services the HD2 modem offers to the apps processor */
static const struct {
    uint32_t prog, vers;
} a9_rpc_servers[] = {
    { 0x30000061, 0x00010001 },     /* PMIC / PM_LIB */
    { 0x30000060, 0x00010001 },     /* PM global */
    { 0x30000064, 0x00010001 },     /* HSUSB API */
    { 0x30100001, 0x00000000 },     /* HTC battery (APP_BATT) */
    { 0x30100002, 0x00000000 },     /* HTC acoustic */
    { 0x30100003, 0x00000000 },     /* HTC acoustic (QSD) */
    { 0x30000048, 0x00010001 },     /* TIMEREMOTE: the RTC */
};

#define TIMEREMOTE_PROG                 0x30000048
#define TIMEREMOTE_SET_JULIAN           6
#define TIMEREMOTE_GET_JULIAN           7

/* modem RTC = QEMU's RTC (host clock, -rtc base=...) + what the guest set */
static time_t rtc_offset;

/*
 * Fill the results of a call; returns their size.  args/argn: the XDR
 * arguments after the call header.
 */
static uint32_t rpc_results(uint32_t prog, uint32_t proc, const uint8_t *args,
                            uint32_t argn, uint8_t *res)
{
    if (prog == TIMEREMOTE_PROG && proc == TIMEREMOTE_GET_JULIAN) {
        struct tm tm;

        qemu_get_timedate(&tm, rtc_offset);
        stl_be_p(res + 0, 1);                   /* opt_arg: data follows */
        stl_be_p(res + 4, tm.tm_year + 1900);
        stl_be_p(res + 8, tm.tm_mon + 1);
        stl_be_p(res + 12, tm.tm_mday);
        stl_be_p(res + 16, tm.tm_hour);
        stl_be_p(res + 20, tm.tm_min);
        stl_be_p(res + 24, tm.tm_sec);
        stl_be_p(res + 28, tm.tm_wday);
        return 32;
    }
    if (prog == TIMEREMOTE_PROG && proc == TIMEREMOTE_SET_JULIAN &&
        argn >= 32 && ldl_be_p(args)) {
        struct tm tm = {
            .tm_year = ldl_be_p(args + 4) - 1900,
            .tm_mon = ldl_be_p(args + 8) - 1,
            .tm_mday = ldl_be_p(args + 12),
            .tm_hour = ldl_be_p(args + 16),
            .tm_min = ldl_be_p(args + 20),
            .tm_sec = ldl_be_p(args + 24),
        };
        rtc_offset = qemu_timedate_diff(&tm);
    }
    return 0;
}

static void rr_send(A9SmdChan *c, uint32_t type, uint32_t src_cid,
                    uint32_t dst_pid, uint32_t dst_cid,
                    const void *payload, uint32_t len)
{
    uint8_t h[RR_HDR_SIZE];

    stl_le_p(h + 0, RR_VERSION);
    stl_le_p(h + 4, type);
    stl_le_p(h + 8, RR_PID_REMOTE);
    stl_le_p(h + 12, src_cid);
    stl_le_p(h + 16, 0);                /* confirm_rx */
    stl_le_p(h + 20, len);
    stl_le_p(h + 24, dst_pid);
    stl_le_p(h + 28, dst_cid);
    g_byte_array_append(c->tx, h, sizeof(h));
    g_byte_array_append(c->tx, payload, len);
}

static void rr_send_ctrl(A9SmdChan *c, uint32_t cmd, uint32_t a, uint32_t b,
                         uint32_t pid, uint32_t cid)
{
    uint8_t m[20];

    stl_le_p(m + 0, cmd);
    stl_le_p(m + 4, a);
    stl_le_p(m + 8, b);
    stl_le_p(m + 12, pid);
    stl_le_p(m + 16, cid);
    /* control messages go router to router */
    rr_send(c, cmd, RR_ROUTER_ADDRESS, RR_PID_REMOTE, RR_ROUTER_ADDRESS,
            m, sizeof(m));
}

static void rr_announce(A9SmdChan *c)
{
    rr_send_ctrl(c, RR_CMD_HELLO, 0, 0, 0, 0);
    for (int i = 0; i < ARRAY_SIZE(a9_rpc_servers); i++) {
        rr_send_ctrl(c, RR_CMD_NEW_SERVER, a9_rpc_servers[i].prog,
                     a9_rpc_servers[i].vers, RR_PID_REMOTE,
                     RPC_SERVER_CID_BASE + i);
    }
}

/* answer any call with "accepted, success" and zeroed results */
static void rr_handle_data(A9SmdChan *c, const uint8_t *h, const uint8_t *p,
                           uint32_t len)
{
    uint32_t src_pid = ldl_le_p(h + 8), src_cid = ldl_le_p(h + 12);
    uint32_t confirm_rx = ldl_le_p(h + 16), dst_cid = ldl_le_p(h + 28);
    uint32_t pm, xid, mtype, prog = 0, proc = 0;
    uint8_t reply[4 + 6 * 4 + 32];

    if (confirm_rx) {
        /* cli = { pid, cid } of the endpoint the sender may resume to */
        rr_send_ctrl(c, RR_CMD_RESUME_TX, RR_PID_REMOTE, dst_cid, 0, 0);
    }
    if (len < 4 + 8) {
        return;
    }
    pm = ldl_le_p(p);
    xid = ldl_be_p(p + 4);
    mtype = ldl_be_p(p + 8);
    if (mtype != RPC_MSG_CALL) {
        return;     /* replies to our (non-existent) calls */
    }
    if (len >= 4 + 24) {
        prog = ldl_be_p(p + 16);
        proc = ldl_be_p(p + 24);
        qemu_log_mask(LOG_UNIMP, "leo-rpc: call prog 0x%08x vers 0x%08x "
                      "proc %u\n", prog, ldl_be_p(p + 20), proc);
    }
    memset(reply, 0, sizeof(reply));
    /* call header: pacmark + 10 words (xid ... verifier) */
    if (len >= 4 + 40) {
        rpc_results(prog, proc, p + 44, len - 44, reply + 4 + 24);
    }
    stl_le_p(reply, (sizeof(reply) - 4) | (((pm >> 16) & 0xff) << 16) |
                    (1u << 30) | (1u << 31));
    stl_be_p(reply + 4, xid);
    stl_be_p(reply + 8, RPC_MSG_REPLY);
    stl_be_p(reply + 12, 0);            /* MSG_ACCEPTED */
    stl_be_p(reply + 16, 0);            /* verifier: AUTH_NULL */
    stl_be_p(reply + 20, 0);
    stl_be_p(reply + 24, 0);            /* SUCCESS */
    rr_send(c, RR_CMD_DATA, dst_cid, src_pid, src_cid, reply, sizeof(reply));
}

static void rr_parse_rx(A9SmdChan *c)
{
    while (c->rx->len >= RR_HDR_SIZE) {
        const uint8_t *h = c->rx->data;
        uint32_t size = ldl_le_p(h + 20);

        if (ldl_le_p(h) != RR_VERSION || size > 512) {
            g_byte_array_set_size(c->rx, 0);    /* resync */
            return;
        }
        if (c->rx->len < RR_HDR_SIZE + size) {
            return;
        }
        if (ldl_le_p(h + 28) == RR_ROUTER_ADDRESS) {
            /* HELLO, NEW_SERVER for apps servers, ...: nothing to do */
        } else {
            rr_handle_data(c, h, h + RR_HDR_SIZE, size);
        }
        g_byte_array_remove_range(c->rx, 0, RR_HDR_SIZE + size);
    }
}

static void hc_set_state(MsmA9State *s, A9SmdChan *c, uint32_t st)
{
    uint32_t h = c->hdr + HC_SIZE;      /* our half: ch1 */
    uint8_t on = st == SMD_SS_OPENED;

    smem_wr(s, h + HC_STATE, st);
    s->smem[h + HC_FDSR] = on;
    s->smem[h + HC_FCTS] = on;
    s->smem[h + HC_FCD] = on;
    s->smem[h + HC_FSTATE] = 1;
}

/*
 * Kernels without CONFIG_QDSP6 only look at DSP-type channels from
 * smd_sleep_exit(), and only if some *open* channel has a pending flag;
 * the real modem keeps its channels busy.  Raise a flag on an open,
 * otherwise idle modem channel so the next idle exit services the DSP
 * list too.  No interrupt: the modem IRQ handler would clear the flag
 * before the CPU goes idle.
 */
static void a9_poke_dsp_service(MsmA9State *s)
{
    for (int i = 0; i < s->nsmd; i++) {
        A9SmdChan *c = &s->smd[i];
        if (!c->dsp && !c->dal && !c->rpc &&
            smem_rd(s, c->hdr + HC_STATE) == SMD_SS_OPENED &&
            smem_rd(s, c->hdr + HC_SIZE + HC_STATE) == SMD_SS_OPENED) {
            s->smem[c->hdr + HC_SIZE + HC_FSTATE] = 1;
            return;
        }
    }
}

static void a9_notify_apps(MsmA9State *s)
{
    qemu_irq_pulse(s->m2a[0]);
}

void a9_smd_init(MsmA9State *s)
{
    uint32_t tbl = smem_alloc_item(s, SMEM_CHANNEL_ALLOC_TBL, 64 * 32);

    s->nsmd = 0;
    for (int i = 0; i < ARRAY_SIZE(a9_channels); i++) {
        A9SmdChan *c = &s->smd[s->nsmd++];
        uint32_t e = tbl + a9_channels[i].cid * 32;

        c->name = a9_channels[i].name;
        c->cid = a9_channels[i].cid;
        c->hdr = smem_alloc_item(s, SMEM_SMD_BASE_ID + c->cid, 2 * HC_SIZE);
        c->fifo = smem_alloc_item(s, SMEM_SMD_FIFO_BASE_ID + c->cid,
                                  SMD_FIFO_SIZE);
        c->half = SMD_FIFO_SIZE / 2;
        if (!c->rx) {
            c->rx = g_byte_array_new();
            c->tx = g_byte_array_new();
        }
        g_byte_array_set_size(c->rx, 0);
        g_byte_array_set_size(c->tx, 0);

        pstrcpy((char *)s->smem + e, 20, c->name);
        smem_wr(s, e + 20, c->cid);
        smem_wr(s, e + 24, a9_channels[i].type | a9_channels[i].kind);
        c->dsp = a9_channels[i].type == SMD_TYPE_APPS_DSP;
        c->dal = a9_channels[i].dal;
        c->rpc = !strcmp(c->name, "RPCCALL");
        smem_wr(s, e + 28, 1);                 /* ref_count */
        hc_set_state(s, c, SMD_SS_CLOSED);
        s->smem[c->hdr + HC_SIZE + HC_FSTATE] = 0;
    }
}

void a9_smd_reset(MsmA9State *s)
{
    s->smd_renamed = false;
    memset(s->dal, 0, sizeof(s->dal));
    if (!s->events) {
        s->events = g_array_new(false, false, sizeof(A9PendingEvt));
    }
    g_array_set_size(s->events, 0);
    timer_del(s->smd_timer);
}

/* ---------------- outgoing data ---------------- */

static bool a9_flush_tx(MsmA9State *s, A9SmdChan *c)
{
    uint32_t h = c->hdr + HC_SIZE;
    uint32_t mask = c->half - 1;
    uint32_t head = smem_rd(s, h + HC_HEAD);
    uint32_t tail = smem_rd(s, h + HC_TAIL);
    uint32_t space = mask - ((head - tail) & mask);
    uint32_t n = MIN(space, c->tx->len);

    if (!n) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        s->smem[c->fifo + c->half + ((head + i) & mask)] = c->tx->data[i];
    }
    g_byte_array_remove_range(c->tx, 0, n);
    smem_wr(s, h + HC_HEAD, (head + n) & mask);
    s->smem[h + HC_FHEAD] = 1;
    return true;
}

static void dal_send(MsmA9State *s, A9SmdChan *c, uint32_t msgid,
                     uint32_t ddi, uint32_t proto, uint32_t from, uint32_t to,
                     const void *data, uint32_t len)
{
    uint8_t hdr[DAL_HDR_SIZE];

    stl_le_p(hdr + 0, (DAL_HDR_SIZE + len) | (DAL_VERSION << 16));
    stl_le_p(hdr + 4, (ddi & 0xffff) | ((proto & 0xff) << 16) |
                      ((msgid & 0xff) << 24));
    stl_le_p(hdr + 8, from);
    stl_le_p(hdr + 12, to);
    g_byte_array_append(c->tx, hdr, sizeof(hdr));
    g_byte_array_append(c->tx, data, len);
}

/* ---------------- audio service ---------------- */

static void audio_event(MsmA9State *s, int ci, uint32_t event_id)
{
    A9DalClient *cl = &s->dal[ci];
    uint8_t ev[68];

    memset(ev, 0, sizeof(ev));
    stl_le_p(ev + 0, cl->cb_evt);
    stl_le_p(ev + 4, cl->loc_evt);
    stl_le_p(ev + 8, sizeof(ev) - 12);
    /* struct adsp_audio_event: hdr{context,data}, session, event, status */
    stl_le_p(ev + 12, cl->session);
    stl_le_p(ev + 20, cl->session);
    stl_le_p(ev + 24, event_id);
    stl_le_p(ev + 28, 0);
    dal_send(s, &s->smd[cl->chan], DAL_MSGID_ASYNCH, 0, 0,
             DAL_HANDLE_BASE + ci, cl->apps_ptr, ev, sizeof(ev));
}

static void queue_event(MsmA9State *s, int ci, uint32_t event_id,
                        int64_t when)
{
    A9PendingEvt e = { .when = when, .client = ci, .event_id = event_id };
    guint i = 0;

    while (i < s->events->len &&
           g_array_index(s->events, A9PendingEvt, i).when <= when) {
        i++;
    }
    g_array_insert_val(s->events, i, e);
    timer_mod(s->smd_timer, g_array_index(s->events, A9PendingEvt, 0).when);
}

void a9_smd_timer_cb(void *opaque)
{
    MsmA9State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool sent = false;

    while (s->events->len &&
           g_array_index(s->events, A9PendingEvt, 0).when <= now) {
        A9PendingEvt e = g_array_index(s->events, A9PendingEvt, 0);
        g_array_remove_index(s->events, 0);
        if (s->dal[e.client].used) {
            audio_event(s, e.client, e.event_id);
            sent = true;
        }
    }
    if (s->events->len) {
        timer_mod(s->smd_timer, g_array_index(s->events, A9PendingEvt, 0).when);
    }
    if (sent) {
        for (int i = 0; i < s->nsmd; i++) {
            a9_flush_tx(s, &s->smd[i]);
        }
        a9_poke_dsp_service(s);
        a9_notify_apps(s);
    }
}

/* f5 calls carry [ilen][ibuf...] */
static void audio_ddi(MsmA9State *s, int ci, uint32_t ddi,
                      const uint8_t *p, uint32_t len)
{
    A9DalClient *cl = &s->dal[ci];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    switch (ddi) {
    case AUDIO_OP_INIT:
        if (len >= 20) {
            cl->cb_evt = ldl_le_p(p + 4);
            cl->loc_evt = ldl_le_p(p + 8);
            cl->session = ldl_le_p(p + 12);
        }
        break;
    case AUDIO_OP_OPEN:
        /*
         * [ilen] hdr{context,data} opcode numdev dev[4] stream_context
         * format pblock blocklen ...; pblock -> adsp_audio_standard_format
         * { u16 channels, u16 bits, u32 rate, ... } in guest memory
         */
        cl->rate = PCM_DEFAULT_RATE;
        cl->channels = PCM_DEFAULT_CH;
        cl->playback = false;
        if (len >= 4 + 8 + 40) {
            uint32_t opcode = ldl_le_p(p + 12);
            uint32_t format = ldl_le_p(p + 40);
            uint32_t pblock = ldl_le_p(p + 44);
            uint8_t fmt[8];

            if (format == ADSP_AUDIO_FORMAT_PCM && pblock &&
                address_space_read(&address_space_memory, pblock,
                                   MEMTXATTRS_UNSPECIFIED, fmt, 8) == MEMTX_OK &&
                lduw_le_p(fmt) >= 1 && lduw_le_p(fmt) <= 2 &&
                lduw_le_p(fmt + 2) == 16 && ldl_le_p(fmt + 4) >= 8000 &&
                ldl_le_p(fmt + 4) <= 48000) {
                cl->channels = lduw_le_p(fmt);
                cl->rate = ldl_le_p(fmt + 4);
            }
            cl->playback = opcode == ADSP_AUDIO_OPCODE_OPEN_WRITE &&
                           format == ADSP_AUDIO_FORMAT_PCM;
            if (cl->playback) {
                leo_hostlink_audio_format(cl->rate, cl->channels);
            }
        }
        queue_event(s, ci, ADSP_AUDIO_EVT_STATUS_OPEN, now + 1000000);
        break;
    case AUDIO_OP_WRITE:
    case AUDIO_OP_READ: {
        /*
         * [ilen] hdr{context,data} buffer{addr,max_size,actual_size,
         * offset,...}
         */
        uint32_t bytes = len >= 24 ? ldl_le_p(p + 4 + 8 + 8) : 0;
        if (ddi == AUDIO_OP_READ && len >= 24) {
            bytes = ldl_le_p(p + 4 + 8 + 4);
        }
        if (ddi == AUDIO_OP_WRITE && cl->playback && bytes &&
            bytes <= 256 * 1024 && len >= 4 + 8 + 16) {
            uint32_t addr = ldl_le_p(p + 12) + ldl_le_p(p + 24);
            g_autofree uint8_t *pcm = g_malloc(bytes);

            if (address_space_read(&address_space_memory, addr,
                                   MEMTXATTRS_UNSPECIFIED, pcm,
                                   bytes) == MEMTX_OK) {
                leo_hostlink_audio_write(pcm, bytes);
            }
        }
        uint32_t bps = (cl->rate ? cl->rate : PCM_DEFAULT_RATE) * 2 *
                       (cl->channels ? cl->channels : PCM_DEFAULT_CH);
        int64_t dur = muldiv64(bytes ? bytes : 4096, NANOSECONDS_PER_SECOND,
                               bps);
        cl->busy_until = MAX(cl->busy_until, now) + dur;
        queue_event(s, ci, ADSP_AUDIO_EVT_STATUS_BUF_DONE, cl->busy_until);
        break;
    }
    case AUDIO_OP_CLOSE:
        cl->busy_until = 0;
        queue_event(s, ci, ADSP_AUDIO_EVT_STATUS_CLOSE, now + 1000000);
        break;
    default:
        break;
    }
}

/* ---------------- incoming DAL messages ---------------- */

static void dal_handle(MsmA9State *s, int chan, const uint8_t *m, uint32_t len)
{
    A9SmdChan *c = &s->smd[chan];
    uint32_t w1 = ldl_le_p(m + 4);
    uint32_t ddi = w1 & 0xffff, proto = (w1 >> 16) & 0xff;
    uint32_t msgid = (w1 >> 24) & 0xff;
    uint32_t from = ldl_le_p(m + 8), to = ldl_le_p(m + 12);
    const uint8_t *p = m + DAL_HDR_SIZE;
    uint32_t plen = len - DAL_HDR_SIZE;
    uint8_t reply[68];
    int ci;

    memset(reply, 0, sizeof(reply));
    switch (msgid) {
    case DAL_MSGID_ATTACH:
        for (ci = 0; ci < A9_DAL_MAX_CLIENTS && s->dal[ci].used; ci++) {
            /* find a free slot */
        }
        if (ci == A9_DAL_MAX_CLIENTS) {
            stl_le_p(reply, 1);         /* failure */
            dal_send(s, c, msgid | DAL_MSGID_REPLY, ddi, proto, 0, from,
                     reply, 68);
            return;
        }
        memset(&s->dal[ci], 0, sizeof(s->dal[ci]));
        s->dal[ci].used = true;
        s->dal[ci].chan = chan;
        s->dal[ci].apps_ptr = from;
        s->dal[ci].device_id = plen >= 4 ? ldl_le_p(p) : 0;
        pstrcpy((char *)reply + 4, 64, "qemu-leo-dsp");
        qemu_log_mask(LOG_UNIMP, "leo-dsp: %s attach device 0x%08x -> "
                      "handle %d\n", c->name, s->dal[ci].device_id, ci);
        dal_send(s, c, msgid | DAL_MSGID_REPLY, ddi, proto,
                 DAL_HANDLE_BASE + ci, from, reply, 68);
        return;

    case DAL_MSGID_DETACH:
        ci = to - DAL_HANDLE_BASE;
        if (ci >= 0 && ci < A9_DAL_MAX_CLIENTS) {
            s->dal[ci].used = false;
        }
        dal_send(s, c, msgid | DAL_MSGID_REPLY, ddi, proto, to, from,
                 reply, 4);
        return;

    case DAL_MSGID_DDI:
        ci = to - DAL_HANDLE_BASE;
        if (ci >= 0 && ci < A9_DAL_MAX_CLIENTS && s->dal[ci].used &&
            s->dal[ci].device_id == AUDIO_DAL_DEVICE) {
            audio_ddi(s, ci, ddi, p, plen);
        }
        /* status 0, no output data: [status][olen=0] */
        dal_send(s, c, msgid | DAL_MSGID_REPLY, ddi, proto, to, from,
                 reply, 8);
        return;

    default:
        qemu_log_mask(LOG_UNIMP, "leo-dsp: unhandled DAL msgid 0x%x\n", msgid);
        return;
    }
}

static void a9_parse_rx(MsmA9State *s, int chan)
{
    A9SmdChan *c = &s->smd[chan];

    while (c->rx->len >= DAL_HDR_SIZE) {
        uint32_t len = ldl_le_p(c->rx->data) & 0xffff;
        if (len < DAL_HDR_SIZE) {
            g_byte_array_set_size(c->rx, 0);    /* resync */
            return;
        }
        if (c->rx->len < len) {
            return;
        }
        dal_handle(s, chan, c->rx->data, len);
        g_byte_array_remove_range(c->rx, 0, len);
    }
}

/* ---------------- doorbell ---------------- */

void a9_smd_kick(MsmA9State *s)
{
    bool notify = false, dsp_work = false;

    if (!s->smd_renamed) {
        s->smd_renamed = leo_quirk_rename_smd_channel("SMD_DAL_AQ_AUD",
                                                      "DSP_DAL_AQ_AUD");
    }

    for (int i = 0; i < s->nsmd; i++) {
        A9SmdChan *c = &s->smd[i];
        uint32_t apps = c->hdr, ours = c->hdr + HC_SIZE;
        uint32_t ast = smem_rd(s, apps + HC_STATE);
        uint32_t ost = smem_rd(s, ours + HC_STATE);

        /* open / close handshake */
        if (ast == SMD_SS_OPENING && ost != SMD_SS_OPENED) {
            smem_wr(s, ours + HC_TAIL, 0);
            smem_wr(s, ours + HC_HEAD, 0);
            g_byte_array_set_size(c->rx, 0);
            g_byte_array_set_size(c->tx, 0);
            hc_set_state(s, c, SMD_SS_OPENED);
            notify = true;
            dsp_work |= c->dsp;
            if (c->rpc) {
                rr_announce(c);
            }
            qemu_log_mask(LOG_UNIMP, "leo-dsp: channel %s opened\n", c->name);
        } else if ((ast == SMD_SS_CLOSING || ast == SMD_SS_CLOSED) &&
                   ost == SMD_SS_OPENED) {
            hc_set_state(s, c, SMD_SS_CLOSED);
            notify = true;
        }
        /*
         * Accept data as soon as our side is open: the apps side may
         * still be in OPENING (it only notices our state change when it
         * services the channel list).
         */
        if ((ast != SMD_SS_OPENED && ast != SMD_SS_OPENING) ||
            smem_rd(s, ours + HC_STATE) != SMD_SS_OPENED) {
            continue;
        }

        /* drain apps -> remote FIFO */
        uint32_t mask = c->half - 1;
        uint32_t head = smem_rd(s, apps + HC_HEAD) & mask;
        uint32_t tail = smem_rd(s, apps + HC_TAIL) & mask;
        if (head != tail) {
            while (tail != head) {
                g_byte_array_append(c->rx, &s->smem[c->fifo + tail], 1);
                tail = (tail + 1) & mask;
            }
            smem_wr(s, apps + HC_TAIL, tail);
            s->smem[ours + HC_FTAIL] = 1;
            notify = true;
            if (c->dal) {
                a9_parse_rx(s, i);
            } else if (c->rpc) {
                rr_parse_rx(c);
            } else {
                g_byte_array_set_size(c->rx, 0);    /* no radio: drop */
            }
        }
        if (a9_flush_tx(s, c)) {
            notify = true;
            dsp_work |= c->dsp;
        }
    }
    if (dsp_work) {
        a9_poke_dsp_service(s);
    }
    if (notify) {
        a9_notify_apps(s);
    }
}
