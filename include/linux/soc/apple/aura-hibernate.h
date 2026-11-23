/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_APPLE_AURA_HIBERNATE_H
#define __LINUX_SOC_APPLE_AURA_HIBERNATE_H

#include <linux/errno.h>
#include <linux/types.h>

#ifdef CONFIG_APPLE_AURA_HIBERNATE
int aura_hibernate_boot_chosen_u64(const char *name, u64 *val);
#else
static inline int aura_hibernate_boot_chosen_u64(const char *name, u64 *val) { return -ENODEV; }
#endif

#endif
