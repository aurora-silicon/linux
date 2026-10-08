/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CD321X_SETUP_H__
#define __CD321X_SETUP_H__

#include <linux/errno.h>
#include "cd321x-pm.h"

struct cd321x_setup_ops {
	int (*read_state)(void *ctx, u8 *state);
	int (*read_mask)(void *ctx, u64 *mask);
	int (*set_s0)(void *ctx);
	int (*set_mask)(void *ctx, u64 mask);
};

/* Caller holds tps->lock. A reset gets at most three setup attempts. */
static inline int cd321x_setup_run(struct cd321x_pm_state *pm, u64 event,
				   u64 expected_mask,
				   const struct cd321x_setup_ops *ops, void *ctx)
{
	u64 mask;
	u8 state;
	int ret;
	bool changed;

	if (pm->phase == CD321X_PM_INITIALIZING ||
	    pm->phase == CD321X_PM_PREPARED || pm->phase == CD321X_PM_REMOVED)
		return 0;
	if (!pm->setup_pending) {
		if (!(event & ~expected_mask))
			return 0;
		pm->setup_pending = true;
		pm->setup_attempts_left = CD321X_RESUME_ATTEMPTS;
	}
	if (!pm->setup_attempts_left)
		return -EIO;
	pm->setup_attempts_left--;
	ret = ops->read_state(ctx, &state);
	if (ret)
		return ret;
	ret = ops->read_mask(ctx, &mask);
	if (ret)
		return ret;
	changed = state || mask != expected_mask;
	if (changed) {
		ret = ops->set_s0(ctx);
		if (ret)
			return ret;
		ret = ops->set_mask(ctx, expected_mask);
		if (ret)
			return ret;
	}
	pm->setup_pending = false;
	pm->setup_attempts_left = 0;
	return changed;
}

#endif /* __CD321X_SETUP_H__ */
