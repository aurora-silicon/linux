/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_RTKIT_HELPER_H
#define _LINUX_SOC_APPLE_RTKIT_HELPER_H

#include <linux/types.h>

struct device;

/* The caller holds a device link to the bound helper supplier. */
bool apple_rtkit_helper_preserves_input(struct device *dev);
/* Its adopted session didn't survive a sleep: nothing may be sent to the firmware. */
bool apple_rtkit_helper_session_lost(struct device *dev);
/* The AP on notice ending a firmware-driven sleep, before the consumer's host state. */
int apple_rtkit_helper_ap_wake(struct device *dev);

#endif
