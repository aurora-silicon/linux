/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_APPLE_DART_APIF_H
#define _LINUX_APPLE_DART_APIF_H
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#if IS_REACHABLE(CONFIG_APPLE_DART_APIF)
/* Caller must quiesce every device on this DART before powering it down.
 * Tables and mappings survive the native SPTM power transition.
 */
int apple_dart_apif_set_power(struct device *dev, bool on);
#else
static inline int apple_dart_apif_set_power(struct device *dev, bool on)
{
	return -EOPNOTSUPP;
}
#endif
#endif
