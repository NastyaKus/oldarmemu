/*
 * leojit host code generator interface (backend.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TARGET_ARM_LEOJIT_BACKEND_H
#define TARGET_ARM_LEOJIT_BACKEND_H

bool leo_backend_init(CPUARMState *env);
void *leo_backend_compile(LBlock *b);
uintptr_t leo_backend_run(CPUARMState *env, void *code);
void leo_backend_patch(uint32_t *site, void *target, uint32_t orig);
bool leo_backend_full(void);
void leo_backend_reset(void);
void *leo_backend_ptr(void);
bool leo_backend_owns(uintptr_t pc);

#endif
