/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple SoC PMP report region
 * Copyright (C) The Asahi Linux Contributors
 */

#ifndef _LINUX_SOC_APPLE_PMP_REPORT_H_
#define _LINUX_SOC_APPLE_PMP_REPORT_H_

#include <linux/errno.h>
#include <linux/types.h>

struct device_node;
struct device;

#if IS_ENABLED(CONFIG_APPLE_PMP_REPORT)
int apple_pmp_report_wait_supplier_ready(struct device *supplier, unsigned int timeout_ms);
int apple_pmp_report_wait_ready(struct device_node *entry, unsigned long timeout);
int apple_pmp_report_fast_die_effort(const struct device_node *np,
				     unsigned int lane, u8 *effort);
int apple_pmp_report_gpu_effort(u32 phandle, u32 *effort);
#else
static inline int apple_pmp_report_wait_supplier_ready(struct device *supplier,
						unsigned int timeout_ms)
{
	return -ENODEV;
}

static inline int apple_pmp_report_wait_ready(struct device_node *entry,
					      unsigned long timeout)
{
	return -ENODEV;
}

static inline int apple_pmp_report_fast_die_effort(const struct device_node *np,
						   unsigned int lane, u8 *effort)
{
	return -ENODEV;
}

static inline int apple_pmp_report_gpu_effort(u32 phandle, u32 *effort)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_SOC_APPLE_PMP_REPORT_H_ */
