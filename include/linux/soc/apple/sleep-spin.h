/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_APPLE_SLEEP_SPIN_H
#define _LINUX_SOC_APPLE_SLEEP_SPIN_H

#include <linux/errno.h>
#include <linux/io.h>
#include <linux/time64.h>

#ifdef CONFIG_ARM64
#include <asm/arch_timer.h>
#endif

/* No WFx is permitted before the platform's final sleep instruction. */
static inline int apple_sleep_spin(void __iomem *reg, u32 mask, u32 expected, unsigned int usec)
{
#ifdef CONFIG_ARM64
	u64 start = arch_timer_read_counter();
	/* CNTFRQ_EL0, the arch timer's rate: arch_timer_get_rate() isn't exported to modules. */
	u64 ticks = ((u64)arch_timer_get_cntfrq() * usec + USEC_PER_SEC - 1) / USEC_PER_SEC;

	if (!ticks)
		return -EINVAL;
	/* Bound a stopped counter as well as a peripheral that never responds. */
	for (unsigned int i = 0; i < 20000000; i++) {
		u64 now;

		if (reg && (readl(reg) & mask) == expected)
			return 0;
		now = arch_timer_read_counter();
		if (now < start)
			return -EIO;
		/* One more look: a stall after the read mustn't make a done wait time out. */
		if (now - start >= ticks)
			return reg && (readl(reg) & mask) != expected ? -ETIMEDOUT : 0;
		cpu_relax();
	}
	return -ETIMEDOUT;
#else
	return -EOPNOTSUPP;
#endif
}

#endif
