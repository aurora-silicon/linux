/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * The Apple SPMI controller's reply FIFO, for the one user that cannot go through the SPMI
 * core: the sleep controller's PMU commands with interrupts off.
 */
#ifndef _LINUX_SOC_APPLE_SPMI_FIFO_H_
#define _LINUX_SOC_APPLE_SPMI_FIFO_H_

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

#if IS_REACHABLE(CONFIG_SPMI_APPLE)
/*
 * Take the controller driver's exchange lock without waiting. 0 with the lock held, -EBUSY if
 * an exchange is under way, -ENODEV if the driver is not bound to @dev.
 */
int apple_spmi_fifo_trylock(struct device *dev);
void apple_spmi_fifo_unlock(struct device *dev);
bool apple_spmi_irq_wake_armed(struct device *dev, unsigned int hwirq);
#else
static inline int apple_spmi_fifo_trylock(struct device *dev)
{
	return -ENODEV;
}

static inline void apple_spmi_fifo_unlock(struct device *dev)
{
}

static inline bool apple_spmi_irq_wake_armed(struct device *dev, unsigned int hwirq)
{
	return false;
}
#endif

#endif
