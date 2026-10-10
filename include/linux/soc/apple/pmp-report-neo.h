/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple SoC PMP report region
 * Copyright (C) The Asahi Linux Contributors
 */

#ifndef _LINUX_SOC_APPLE_PMP_REPORT_NEO_H_
#define _LINUX_SOC_APPLE_PMP_REPORT_NEO_H_

#include <linux/errno.h>
#include <linux/types.h>

struct device_node;

#if IS_ENABLED(CONFIG_APPLE_PMP_REPORT_NEO)
int apple_neo_pmp_report_fast_die_effort(const struct device_node *np,
				     unsigned int lane, u8 *effort);
#else
static inline int apple_neo_pmp_report_fast_die_effort(const struct device_node *np,
						   unsigned int lane, u8 *effort)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_SOC_APPLE_PMP_REPORT_NEO_H_ */
