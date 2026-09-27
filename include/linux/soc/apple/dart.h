/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __LINUX_SOC_APPLE_DART_H__
#define __LINUX_SOC_APPLE_DART_H__

#include <linux/kconfig.h>

struct device;

/*
 * The tunneled PCIe IOMMU shares the port clock. Stop issuing commands
 * before that clock is gated, and allow them again once it is back.
 */
#if IS_ENABLED(CONFIG_APPLE_DART)
int apple_dart_quiesce_commands(struct device *dev);
int apple_dart_resume_commands(struct device *dev);
#else
static inline int apple_dart_quiesce_commands(struct device *dev) { return 0; }
static inline int apple_dart_resume_commands(struct device *dev) { return 0; }
#endif

#endif
