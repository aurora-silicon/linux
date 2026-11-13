/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple silicon camera-in-use indicator broker
 *
 * On machines whose camera has no indicator light on the sensor supply, the
 * camera-in-use indicator is drawn on the internal panel. The display driver
 * provides it, the camera driver asks for it while it streams. This is a software indicator kept
 * by the kernel: it is not a hardware privacy guarantee.
 */

#ifndef _LINUX_SOC_APPLE_CAMERA_INDICATOR_H_
#define _LINUX_SOC_APPLE_CAMERA_INDICATOR_H_

#include <linux/errno.h>
#include <linux/types.h>

struct device;

struct apple_cam_indicator_ops {
	/*
	 * Show (@on) or remove the indicator. Process context, called with
	 * the broker lock held: must not call back into the broker except
	 * apple_cam_indicator_set_shown().
	 */
	void (*set)(void *priv, bool on);
};

#if IS_ENABLED(CONFIG_APPLE_CAMERA_INDICATOR)
/* Provider (display driver): one internal panel per machine. */
int apple_cam_indicator_register(struct device *dev,
				 const struct apple_cam_indicator_ops *ops,
				 void *priv);
void apple_cam_indicator_unregister(struct device *dev);
/* The indicator is (or is no longer) on the panel. Any context. */
void apple_cam_indicator_set_shown(bool shown);

/* Consumer (camera driver). */
int apple_cam_indicator_request(void);
int apple_cam_indicator_acquire(unsigned int timeout_ms);
void apple_cam_indicator_release(void);
/* Lockless, any context: is the indicator on the panel right now? */
bool apple_cam_indicator_shown(void);
#else
static inline int
apple_cam_indicator_register(struct device *dev,
			     const struct apple_cam_indicator_ops *ops,
			     void *priv)
{
	return 0;
}

static inline void apple_cam_indicator_unregister(struct device *dev) { }
static inline void apple_cam_indicator_set_shown(bool shown) { }

static inline int apple_cam_indicator_request(void)
{
	return -ENODEV;
}

static inline int apple_cam_indicator_acquire(unsigned int timeout_ms)
{
	return -ENODEV;
}

static inline void apple_cam_indicator_release(void) { }

static inline bool apple_cam_indicator_shown(void)
{
	return false;
}
#endif

#endif /* _LINUX_SOC_APPLE_CAMERA_INDICATOR_H_ */
