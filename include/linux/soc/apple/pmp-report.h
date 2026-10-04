/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple SoC PMP power state reporting
 *
 * Copyright The Asahi Linux Contributors
 */

#ifndef _LINUX_SOC_APPLE_PMP_REPORT_H_
#define _LINUX_SOC_APPLE_PMP_REPORT_H_

#include <linux/errno.h>

struct device_node;

#if IS_ENABLED(CONFIG_APPLE_PMP_REPORT)
int apple_pmp_report_wait_ready(struct device_node *entry, unsigned long timeout);
#else
static inline int apple_pmp_report_wait_ready(struct device_node *entry,
					      unsigned long timeout)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_SOC_APPLE_PMP_REPORT_H_ */
