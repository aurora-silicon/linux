/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 Aurora Silicon */
#ifndef PINCTRL_APPLE_IRQ_H
#define PINCTRL_APPLE_IRQ_H

#include <linux/errno.h>
#include <linux/types.h>

/* All pending banks must fit before registering the chained parent IRQs. */
static inline int apple_gpio_irq_layout(u32 pins, u32 first, u32 count,
				       u64 aperture)
{
	u32 size;

	if (!pins || pins > 512 || first > 6 || count > 7 - first)
		return -EINVAL;
	size = pins * 4;
	if (count) {
		u32 last = 0x800 + (first + count - 1) * 0x40 +
			   ((pins - 1) / 32) * 4 + 4;

		if (last > size)
			size = last;
	}
	return aperture < size ? -EINVAL : 0;
}
#endif
