/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __LINUX_SOC_APPLE_PMGR_PWRSTATE_H
#define __LINUX_SOC_APPLE_PMGR_PWRSTATE_H

#include <linux/errno.h>

struct device_node;

#if IS_ENABLED(CONFIG_APPLE_PMGR_PWRSTATE)
int apple_pmgr_ps_cycle(struct device_node *np);
#else
static inline int apple_pmgr_ps_cycle(struct device_node *np) { return -EOPNOTSUPP; }
#endif

#endif
