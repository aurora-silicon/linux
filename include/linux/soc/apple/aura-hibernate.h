/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_APPLE_AURA_HIBERNATE_H
#define __LINUX_SOC_APPLE_AURA_HIBERNATE_H

#include <linux/errno.h>
#include <linux/types.h>

struct device;
struct device_node;

/* A firmware memory-region of a device as this boot's device tree gives it. */
struct aura_fw_region {
	u64 phys;
	u64 iova;
	u64 size;
};

/*
 * Declared unconditionally so that Rust drivers can bind to them; without
 * CONFIG_APPLE_AURA_HIBERNATE they must not call them.
 */
int aura_hibernate_boot_fw_regions(struct device_node *np, struct aura_fw_region *out,
				   unsigned int max);
int aura_hibernate_reprobe_after_thaw(struct device *dev);

#ifdef CONFIG_APPLE_AURA_HIBERNATE
int aura_hibernate_boot_chosen_u64(const char *name, u64 *val);
#else
static inline int aura_hibernate_boot_chosen_u64(const char *name, u64 *val) { return -ENODEV; }
#endif

#endif
