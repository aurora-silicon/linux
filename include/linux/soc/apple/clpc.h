/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_CLPC_H
#define _LINUX_SOC_APPLE_CLPC_H

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/property.h>

struct apple_clpc_gpu;
struct apple_clpc_ane;

#if IS_REACHABLE(CONFIG_APPLE_CLPC)
int apple_clpc_gpu_get(struct device *dev, struct apple_clpc_gpu **out);
void apple_clpc_gpu_set_active(struct apple_clpc_gpu *gpu, bool active);
void apple_clpc_gpu_put(struct apple_clpc_gpu *gpu);
/* Optional, single-publisher input: success with NULL means no provider. */
int apple_clpc_ane_get(struct device *dev, struct apple_clpc_ane **out);
void apple_clpc_ane_set_active(struct apple_clpc_ane *ane, bool active);
void apple_clpc_ane_put(struct apple_clpc_ane *ane);
#else
static inline int apple_clpc_gpu_get(struct device *dev, struct apple_clpc_gpu **out)
{
	*out = NULL;
	return device_property_present(dev, "apple,clpc") ? -ENODEV : 0;
}
static inline void apple_clpc_gpu_set_active(struct apple_clpc_gpu *gpu, bool active) {}
static inline void apple_clpc_gpu_put(struct apple_clpc_gpu *gpu) {}

/* ANE remains usable when the optional floor provider is not reachable. */
static inline int apple_clpc_ane_get(struct device *dev, struct apple_clpc_ane **out)
{
	*out = NULL;
	return 0;
}

static inline void apple_clpc_ane_set_active(struct apple_clpc_ane *ane, bool active) {}

static inline void apple_clpc_ane_put(struct apple_clpc_ane *ane) {}
#endif
#endif
