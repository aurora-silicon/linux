/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_PM_H__
#define __ANE_PM_H__

#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

/*
 * The caller serializes PM entries and holds exactly one consumer reference.
 * Its suspend callback must permit recovery and its resume callback must defer
 * engine access until the caller verifies power-on. verify_off may only read
 * always-accessible provider registers, never the powered-down engine.
 *
 * Kept independent of the H13 task manager: a firmware client can use the same
 * PM transaction after stopping its RX worker and fencing submissions.
 */
static inline int ane_pm_cycle(struct device *dev, struct device **domains,
			       int count, int (*verify_off)(void *), void *ctx)
{
	int err, resume_err;

	if (count > 1) {
		for (int i = 0; i < count; i++)
			dev_pm_genpd_synced_poweroff(domains[i]);
	} else {
		dev_pm_genpd_synced_poweroff(dev);
	}

	/* Normal runtime PM releases the consumer's supplier references. */
	err = pm_runtime_put_sync_suspend(dev);
	if (err < 0)
		goto resume;

	/* Finish asynchronous supplier idle work, then suspend leaves first.
	 * Runtime PM stays enabled so genpd can also suspend their parents.
	 */
	for (int i = count - 1; i >= 0 && count > 1; i--) {
		pm_runtime_barrier(domains[i]);
		err = pm_runtime_suspend(domains[i]);
		if (err < 0)
			goto resume;
	}
	err = verify_off(ctx);

resume:
	/* Restore the caller's reference even if resume itself fails. */
	resume_err = pm_runtime_get_sync(dev);
	return err < 0 ? err : (resume_err < 0 ? resume_err : 0);
}

#endif /* __ANE_PM_H__ */
