/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_APPLE_IDLE_H
#define __ASM_APPLE_IDLE_H

#include <asm/cputype.h>

static inline bool apple_needs_wait_save(void)
{
	/* T8140 (A18 Pro), E and P core models measured on J700. */
	u32 model;

	if (!IS_ENABLED(CONFIG_ARCH_APPLE))
		return false;
	model = read_cpuid_id() & MIDR_CPU_MODEL_MASK;

	return model == MIDR_APPLE_T8140_E || model == MIDR_APPLE_T8140_P;
}

void apple_cpu_wfi(void);

#endif
