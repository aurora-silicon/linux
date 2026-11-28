/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_APPLE_IDLE_H
#define __ASM_APPLE_IDLE_H

/* WFI that preserves x18-x30 on affected Apple cores. */
void apple_cpu_wfi(void);

#ifdef CONFIG_ARCH_APPLE
#include <linux/percpu.h>

/* Called from smp_prepare_boot_cpu(): before the boot CPU's first idle. */
void apple_wfi_quirk_init(void);

/* Set by the Apple CPU PMU driver while events are counting on this CPU. */
DECLARE_PER_CPU(bool, apple_pmu_wfi_freeze);
#else
static inline void apple_wfi_quirk_init(void) { }
#endif

#endif
