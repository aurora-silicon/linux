/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * T600x ANE level sequence, based on the public omarchy-ane implementation
 * by Joshua Warren (ane/src/ane_dvfs.c, e20d25defb78).
 * The report provider owns both windows and serializes request-word updates.
 */
#ifndef __APPLE_PMP_ANE_H__
#define __APPLE_PMP_ANE_H__

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>

#define PMP_ANE_DVFS_ON		0x2000
#define PMP_ANE_DVFS_CMD		0xa00
#define PMP_ANE_LEVEL_READ	0x10c0
#define PMP_ANE_LEVEL_WRITE	0x10860
#define PMP_ANE_LEVEL_TAG		0x2000000000000000ULL
#define PMP_ANE_MAX_LEVEL		5
#define PMP_ANE_ID		10

struct apple_pmp_ane {
	void __iomem *dvfs;
	void __iomem *report;
	unsigned int level;
	unsigned int timeouts;
	bool owned;
	bool reserved;
};

static void apple_pmp_ane_cmd(struct apple_pmp_ane *ane, unsigned int old,
			      unsigned int new)
{
	writel(0x80000000U | old << 4 | new, ane->dvfs + PMP_ANE_DVFS_CMD);
	usleep_range(1000, 2000);
}

/* Caller serializes transitions and pins the ANE and report power domains. */
static int apple_pmp_ane_set(struct apple_pmp_ane *ane, unsigned int level,
			     int (*request)(void *ctx, bool enable), void *ctx)
{
	u64 word;
	int ret = 0;

	if (!ane->dvfs)
		return -EOPNOTSUPP;
	if (level > PMP_ANE_MAX_LEVEL)
		return -EINVAL;
	if (level == ane->level && (level || !ane->owned))
		return 0;
	if (!level && !ane->level)
		goto release;
	if (level && !ane->level && ane->owned)
		return -EBUSY;
	if (level && ane->timeouts >= 3)
		return -ETIMEDOUT;

	if (!ane->level) {
		if (readl(ane->dvfs + PMP_ANE_DVFS_ON))
			return -EBUSY;
		word = readq(ane->report + PMP_ANE_LEVEL_READ);
		if (word && (word < PMP_ANE_LEVEL_TAG ||
			     word > (PMP_ANE_LEVEL_TAG | PMP_ANE_MAX_LEVEL)))
			return -EBUSY;

		writel(1, ane->dvfs + PMP_ANE_DVFS_ON);
		usleep_range(1000, 2000);
		apple_pmp_ane_cmd(ane, 0, 0);
		ret = request(ctx, true);
		if (ret) {
			if (ret == -ETIMEDOUT)
				ane->timeouts++;
			goto release;
		}
	}

	apple_pmp_ane_cmd(ane, ane->level, level);
	writeq(PMP_ANE_LEVEL_TAG | level, ane->report + PMP_ANE_LEVEL_WRITE);
	ane->level = level;
	if (level)
		return 0;
	usleep_range(1000, 2000);

release:
	if (ane->owned) {
		int err = request(ctx, false);

		if (!ret)
			ret = err;
	}
	apple_pmp_ane_cmd(ane, 0, 0);
	writel(0, ane->dvfs + PMP_ANE_DVFS_ON);
	return ret;
}

#endif
