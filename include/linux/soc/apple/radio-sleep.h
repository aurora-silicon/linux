/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_APPLE_RADIO_SLEEP_H
#define _LINUX_SOC_APPLE_RADIO_SLEEP_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

/* Endpoint hooks are no-ops outside an exact active managed J713 transaction. */
#if IS_REACHABLE(CONFIG_PCIE_APPLE)
bool apple_radio_sleep_owned(struct device *dev);
int apple_radio_sleep_endpoint_prepare(struct device *dev, void (*finish)(struct device *dev));
int apple_radio_sleep_endpoint_suspended(struct device *dev);
int apple_radio_sleep_endpoint_resume(struct device *dev);
void apple_radio_sleep_endpoint_resumed(struct device *dev, int result);
#else
static inline bool apple_radio_sleep_owned(struct device *dev)
{
	return false;
}

static inline int apple_radio_sleep_endpoint_prepare(struct device *dev,
						     void (*finish)(struct device *dev))
{
	return 0;
}

static inline int apple_radio_sleep_endpoint_suspended(struct device *dev)
{
	return 0;
}

static inline int apple_radio_sleep_endpoint_resume(struct device *dev)
{
	return 0;
}

static inline void apple_radio_sleep_endpoint_resumed(struct device *dev, int result)
{
}
#endif

#endif
