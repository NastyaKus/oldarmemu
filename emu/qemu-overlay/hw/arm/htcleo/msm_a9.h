/*
 * Fake ARM9/DSP shared definitions (msm_a9.c, leo_smd.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_HTCLEO_MSM_A9_H
#define HW_ARM_HTCLEO_MSM_A9_H

#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "msm.h"

OBJECT_DECLARE_SIMPLE_TYPE(MsmA9State, MSM_A9)

#define SMD_MODEM_DOORBELL      0
#define SMD_DSP_DOORBELL        8
#define A9_SMD_MAX_CH           12
#define A9_DAL_MAX_CLIENTS      16

typedef struct A9SmdChan {
    const char *name;
    uint32_t cid;
    uint32_t hdr;           /* offset of the two half-channel headers */
    uint32_t fifo;          /* offset of the FIFO area (2 halves) */
    uint32_t half;          /* size of each direction's FIFO */
    GByteArray *rx;         /* apps -> remote, not yet parsed */
    GByteArray *tx;         /* remote -> apps, not yet in the FIFO */
    bool dsp;               /* APPS_DSP type: serviced from smd_sleep_exit */
    bool dal;               /* carries DAL RPC */
    bool rpc;               /* ONCRPC router */
} A9SmdChan;

typedef struct A9DalClient {
    bool used;
    uint32_t chan;          /* index into smd[] */
    uint32_t apps_ptr;      /* the apps side dal_client pointer */
    uint32_t device_id;
    uint32_t cb_evt, loc_evt, session;
    int64_t busy_until;     /* playback clock for buffer pacing */
} A9DalClient;

typedef struct A9PendingEvt {
    int64_t when;
    int client;
    uint32_t event_id;
} A9PendingEvt;

struct MsmA9State {
    SysBusDevice parent_obj;
    MemoryRegion shared_ram;
    uint8_t *smem;
    qemu_irq m2a[7];
    uint32_t heap_free;
    uint32_t dex_count;

    /* SMD / DAL */
    A9SmdChan smd[A9_SMD_MAX_CH];
    int nsmd;
    A9DalClient dal[A9_DAL_MAX_CLIENTS];
    GArray *events;         /* A9PendingEvt, sorted by time */
    QEMUTimer *smd_timer;
    bool smd_renamed;
};

static inline uint32_t smem_rd(MsmA9State *s, uint32_t off)
{
    return ldl_le_p(s->smem + off);
}

static inline void smem_wr(MsmA9State *s, uint32_t off, uint32_t v)
{
    stl_le_p(s->smem + off, v);
}

uint32_t smem_alloc_item(MsmA9State *s, unsigned id, uint32_t size);

void a9_smd_init(MsmA9State *s);
void a9_smd_reset(MsmA9State *s);
void a9_smd_kick(MsmA9State *s);
void a9_smd_timer_cb(void *opaque);

#endif
