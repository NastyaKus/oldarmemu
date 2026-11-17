/*
 * BCM4329 SDIO Wi-Fi dongle (leo_bcm4329.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_HTCLEO_BCM4329_H
#define HW_ARM_HTCLEO_BCM4329_H

#include "hw/irq.h"
#include "net/net.h"

typedef struct LeoWifi LeoWifi;

/*
 * Attach the dongle to an SDCC slot.  "oob" is the host-wake GPIO input,
 * *power_in receives the line to connect to WIFI_SHUTDOWN_N, and "peer"
 * is the network backend (may be NULL: the access point then has no
 * uplink).
 */
LeoWifi *leo_bcm4329_init(DeviceState *sdcc, qemu_irq oob, qemu_irq *power_in,
                          NetClientState *peer);

#endif
