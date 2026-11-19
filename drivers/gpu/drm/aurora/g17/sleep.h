/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef AURORA_G17_SLEEP_H
#define AURORA_G17_SLEEP_H
#include <linux/types.h>

struct apple_mbox;
struct apple_rtkit;
struct aurora_g17_commit;
struct device;

int aurora_g17_pmp_request(struct device *dev, bool enable);
struct aurora_g17_commit *aurora_g17_commit_prepare(struct device *dev,
						  struct apple_rtkit *primary_rtk,
						  struct apple_mbox *primary_mbox,
						  struct apple_rtkit *secondary_rtk,
						  struct apple_mbox *secondary_mbox, bool bridge);
struct aurora_g17_commit *aurora_g16_commit_prepare(struct device *dev, struct apple_rtkit *rtk);
bool aurora_g17_commit_finish(struct aurora_g17_commit *c, u32 *saved);
bool aurora_g17_commit_required(void);
int aurora_g17_power_auto_off(struct device *dev, u32 *saved);
int aurora_g17_power_lower(struct device *dev);
int aurora_g17_power_raise(struct device *dev, u32 saved);
int aurora_g17_power_auto_on(struct device *dev, u32 saved);
#endif
