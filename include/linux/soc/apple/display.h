/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_DISPLAY_H_
#define _LINUX_SOC_APPLE_DISPLAY_H_

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

/* Initial native display admission, before starting retained GPU firmware. */
int apple_drm_t6030_link_device(struct device *consumer);
int apple_drm_t6031_link_device(struct device *consumer);
int apple_drm_t8132_link_device(struct device *consumer);

/*
 * Software camera indicator (interim, not the secure indicator): a
 * kernel-owned overlay on the internal panel, requested best-effort by the
 * camera driver. set(true) returns 0 once the request is committed, without
 * waiting for the present; a square no present carries within 1 s is
 * withdrawn with a warning. The camera streams regardless of the answer.
 */
#if IS_ENABLED(CONFIG_DRM_APPLE)
int apple_dcp_camera_indicator_set(struct device *dcp, bool on);
#else
static inline int apple_dcp_camera_indicator_set(struct device *dcp, bool on)
{
	return -ENODEV;
}
#endif

#endif
