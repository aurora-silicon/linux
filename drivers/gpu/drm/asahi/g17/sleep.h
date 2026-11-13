/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ASAHI_G17_SLEEP_H
#define ASAHI_G17_SLEEP_H
#include <linux/types.h>

struct apple_mbox;
struct apple_rtkit;
struct asahi_g17_commit;
struct device;

int asahi_g17_pmp_request(struct device *dev, bool enable);
struct asahi_g17_commit *asahi_g17_commit_prepare(struct device *dev,
						  struct apple_rtkit *primary_rtk,
						  struct apple_mbox *primary_mbox,
						  struct apple_rtkit *secondary_rtk,
						  struct apple_mbox *secondary_mbox, bool bridge);
bool asahi_g17_commit_finish(struct asahi_g17_commit *c, u32 *saved);
bool asahi_g17_commit_required(void);
bool asahi_g17_commit_stop(void);
int asahi_g17_power_auto_off(struct device *dev, u32 *saved);
int asahi_g17_power_lower(struct device *dev);
int asahi_g17_power_raise(struct device *dev, u32 saved);
int asahi_g17_power_auto_on(struct device *dev, u32 saved);
#endif
