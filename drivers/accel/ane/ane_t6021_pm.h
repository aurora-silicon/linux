/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_T6021_PM_H__
#define __ANE_T6021_PM_H__

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/pm.h>

/* State transitions and submission checks hold the firmware mutex. */
enum ane_t6021_pm_state {
	ANE_T6021_PM_ACTIVE,
	ANE_T6021_PM_SUSPENDED,
	ANE_T6021_PM_FAILED,
};

/*
 * Adapters to the shared H14 lifetime implementation, not another firmware
 * restart path. stop must complete protocol shutdown and prove ASC STOPPED
 * with RUN clear before system PM may remove power. A failed stop retains
 * the power claim, DMA mappings and module pin. restart must use the same
 * cold-start/handshake/program-replay path as timeout recovery, returning
 * success only once existing public IDs are usable again.
 *
 * These callbacks hold the existing firmware mutex. Neither callback may use
 * runtime-PM force/cycle helpers while system PM has disabled runtime PM.
 * Supplier and DART system callbacks perform power removal/restoration around
 * these consumer callbacks. RX shutdown and restoration belong to the shared
 * lifetime implementation along with the firmware state.
 */
struct ane_t6021_pm_ops {
	int (*stop)(void *ctx);
	int (*restart)(void *ctx);
};

struct ane_t6021_pm {
	/* Existing firmware command mutex, shared with submitters. */
	struct mutex *fw_lock;
	const struct ane_t6021_pm_ops *ops;
	void *ctx;
	enum ane_t6021_pm_state state;
};

static inline int ane_t6021_pm_submit_error(enum ane_t6021_pm_state state)
{
	return state == ANE_T6021_PM_ACTIVE ? 0 : -EHOSTDOWN;
}

static inline int ane_t6021_pm_suspend(struct ane_t6021_pm *pm)
{
	int ret = -EHOSTDOWN;

	/* Drains the current synchronous command and its finish event, then
	 * fences queued submitters before handing off to the shared stop path.
	 */
	mutex_lock(pm->fw_lock);
	if (pm->state != ANE_T6021_PM_ACTIVE)
		goto out;
	pm->state = ANE_T6021_PM_FAILED;
	ret = pm->ops->stop(pm->ctx);
	if (!ret)
		pm->state = ANE_T6021_PM_SUSPENDED;
out:
	mutex_unlock(pm->fw_lock);
	return ret;
}

static inline int ane_t6021_pm_resume(struct ane_t6021_pm *pm)
{
	int ret = -EHOSTDOWN;

	mutex_lock(pm->fw_lock);
	/* PM core may unwind before it reached this device's suspend. */
	if (pm->state == ANE_T6021_PM_ACTIVE) {
		ret = 0;
		goto out;
	}
	if (pm->state != ANE_T6021_PM_SUSPENDED)
		goto out;
	pm->state = ANE_T6021_PM_FAILED;
	ret = pm->ops->restart(pm->ctx);
	if (!ret)
		pm->state = ANE_T6021_PM_ACTIVE;
out:
	mutex_unlock(pm->fw_lock);
	return ret;
}

/* The getter preserves the driver's own drvdata layout and returns its PM
 * adapter, initialized once at probe with the shared lifetime callbacks.
 */
#define DEFINE_ANE_T6021_SYSTEM_PM_OPS(name, get_pm) \
static int __maybe_unused name##_suspend(struct device *dev) \
{ \
	return ane_t6021_pm_suspend(get_pm(dev)); \
} \
static int __maybe_unused name##_resume(struct device *dev) \
{ \
	return ane_t6021_pm_resume(get_pm(dev)); \
} \
static const struct dev_pm_ops __maybe_unused name = { \
	SYSTEM_SLEEP_PM_OPS(name##_suspend, name##_resume) \
}

#endif /* __ANE_T6021_PM_H__ */
