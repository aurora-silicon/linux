/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_RTKIT_HELPER_H
#define _LINUX_SOC_APPLE_RTKIT_HELPER_H

#include <linux/types.h>

struct device;

/* The caller holds a device link to the bound helper supplier. */
bool apple_rtkit_helper_preserves_input(struct device *dev);

/*
 * After a resume from disk: the helper took over, in its .restore callback,
 * the session that the boot kernel started and left running. Valid from the
 * helper's .restore until its next .freeze.
 */
bool apple_rtkit_helper_readopted(struct device *dev);

/*
 * After a resume from disk: the session this helper had started itself was
 * found with its co-processor halted (the boot loader stopped it and the boot
 * kernel did not start it). Nothing answers until the helper is probed again,
 * which starts the co-processor. Valid from the helper's .restore until its
 * next .freeze.
 */
bool apple_rtkit_helper_halted_underneath(struct device *dev);

#endif
