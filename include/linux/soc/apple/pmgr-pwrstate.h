/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __LINUX_SOC_APPLE_PMGR_PWRSTATE_H
#define __LINUX_SOC_APPLE_PMGR_PWRSTATE_H

#include <linux/errno.h>

struct device;
struct device_node;

#if IS_ENABLED(CONFIG_APPLE_PMGR_PWRSTATE)
int apple_pmgr_ps_verify_off(struct device *consumer);
int apple_pmgr_ps_cycle(struct device_node *np);
/*
 * Power off (child first) or on (parent first) the chain of
 * apple,consumer-switched power states that starts at @consumer's power
 * domain. The consumer has stopped its coprocessor before power-off and
 * starts it again after power-on. The check says, changing nothing, whether
 * the power-off can run now.
 */
int apple_pmgr_ps_consumer_check(struct device *consumer);
int apple_pmgr_ps_consumer_off(struct device *consumer);
int apple_pmgr_ps_consumer_on(struct device *consumer);
#else
static inline int apple_pmgr_ps_verify_off(struct device *consumer) { return -EOPNOTSUPP; }
static inline int apple_pmgr_ps_cycle(struct device_node *np) { return -EOPNOTSUPP; }
static inline int apple_pmgr_ps_consumer_check(struct device *consumer) { return -EOPNOTSUPP; }
static inline int apple_pmgr_ps_consumer_off(struct device *consumer) { return -EOPNOTSUPP; }
static inline int apple_pmgr_ps_consumer_on(struct device *consumer) { return -EOPNOTSUPP; }
#endif

#endif
