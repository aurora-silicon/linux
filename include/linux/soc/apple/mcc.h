/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_MCC_H_
#define _LINUX_SOC_APPLE_MCC_H_

#include <linux/kconfig.h>
#include <linux/list.h>
#include <linux/types.h>

struct device;
struct device_node;

/* A cache controller with allocation enabled, for the OF detach notifier. */
struct apple_mcc_boot_dma_consumer {
	struct list_head link;
	struct device *dev;
	/* Under the boot DMA lock: turn allocation off. */
	void (*lost)(struct apple_mcc_boot_dma_consumer *consumer);
};

#if IS_ENABLED(CONFIG_APPLE_MCC_BOOT_DMA)
int apple_mcc_boot_scanout_reserve(struct device_node *supplier);
void apple_mcc_boot_scanout_retired(struct device_node *supplier);
int apple_mcc_boot_scanout_ready(struct device *dev, struct device_node *supplier);
void apple_mcc_boot_dma_lock(void);
void apple_mcc_boot_dma_unlock(void);
void apple_mcc_boot_dma_register_locked(struct apple_mcc_boot_dma_consumer *consumer);
void apple_mcc_boot_dma_unregister(struct apple_mcc_boot_dma_consumer *consumer);
#else
static inline int apple_mcc_boot_scanout_reserve(struct device_node *supplier)
{
	return 0;
}

static inline void apple_mcc_boot_scanout_retired(struct device_node *supplier) { }
#endif

#endif
