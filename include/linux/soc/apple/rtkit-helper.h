/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_RTKIT_HELPER_H
#define _LINUX_SOC_APPLE_RTKIT_HELPER_H

#include <linux/types.h>

struct device;

/* The caller holds a device link to the bound helper supplier. */
bool apple_rtkit_helper_preserves_input(struct device *dev);

#endif
