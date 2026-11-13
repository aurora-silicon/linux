/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _LINUX_SOC_APPLE_SLEEP_ANS_H_
#define _LINUX_SOC_APPLE_SLEEP_ANS_H_
#include <linux/compiler_types.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
struct device;
/*
 * ANS's sleep owner on T6050, whose final stop the sleep makes in its last steps.
 * check is read-only/noirq. stop runs only after all returnable checks; its error
 * cannot cancel the commit. The sleep owns the RUN clear and the domains.
 * domains_ready is a real wake's receipt, never a request to raise power. The owner
 * registers from its own .suspend only.
 */
#if IS_ENABLED(CONFIG_APPLE_SOC_SLEEP)
bool apple_sleep_ans_late(void);
int apple_sleep_ans_m5_register(struct device *dev, void __iomem *asc,
	int (*check)(void *), int (*stop)(void *), void *data);
int apple_sleep_ans_m5_preflight(struct device *dev);
int apple_sleep_ans_m5_domains_ready(struct device *dev);
#else
static inline bool apple_sleep_ans_late(void)
{
	return false;
}
static inline int apple_sleep_ans_m5_register(struct device *dev, void __iomem *asc,
	int (*check)(void *), int (*stop)(void *), void *data)
{
	return -EOPNOTSUPP;
}
static inline int apple_sleep_ans_m5_preflight(struct device *dev)
{
	return -EOPNOTSUPP;
}
static inline int apple_sleep_ans_m5_domains_ready(struct device *dev)
{
	return -EOPNOTSUPP;
}
#endif
#endif
