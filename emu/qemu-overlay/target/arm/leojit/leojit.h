/*
 * leojit: our own ARMv7-A engine, see target/arm/leojit/lir.h.
 * Hooks used by the TCG main loop and code invalidation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TARGET_ARM_LEOJIT_H
#define TARGET_ARM_LEOJIT_H

#include "exec/hwaddr.h"
#include "accel/tcg/tb-cpu-state.h"

bool leo_jit_enabled(void);
bool leo_jit_exec(CPUState *cs, TCGTBCPUState *s);
bool leo_jit_invalidate(ram_addr_t start, ram_addr_t last);
void leo_jit_after_tb(CPUState *cs);

#endif
