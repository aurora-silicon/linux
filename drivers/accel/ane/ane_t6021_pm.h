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
 * The firmware may only cold-start on a CPU whose power domain went off
 * after the stop. A suspend that aborts before the domains are powered off
 * (a wake event during the device phase, or pm_test=devices) still runs
 * every resume callback. restart then returns ANE_T6021_PM_RESTART_DEFERRED:
 * the CPU stays stopped, the device stays SUSPENDED (submissions refused)
 * and restart_deferred() later power-cycles the domains through runtime PM
 * and boots, once system PM has released its references.
 *
 * park is optional. It runs when a FAILED device is suspended and must leave
 * its CPU safe to lose power (proven stopped, or already unpowered); an error
 * aborts only that suspend. A FAILED device stays unavailable until reboot
 * but never blocks system sleep by itself.
 *
 * These callbacks hold the existing firmware mutex. stop, restart and park
 * may not use runtime-PM force/cycle helpers while system PM has disabled
 * runtime PM. Supplier and DART system callbacks perform power
 * removal/restoration around these consumer callbacks. RX shutdown and
 * restoration belong to the shared lifetime implementation along with the
 * firmware state.
 */
#define ANE_T6021_PM_RESTART_DEFERRED	1

struct ane_t6021_pm_ops {
	int (*stop)(void *ctx);
	int (*restart)(void *ctx);
	int (*restart_deferred)(void *ctx);
	int (*park)(void *ctx);
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
	int ret = 0;

	/* Drains the current synchronous command and its finish event, then
	 * fences queued submitters before handing off to the shared stop path.
	 */
	mutex_lock(pm->fw_lock);
	switch (pm->state) {
	case ANE_T6021_PM_ACTIVE:
		pm->state = ANE_T6021_PM_FAILED;
		ret = pm->ops->stop(pm->ctx);
		if (!ret)
			pm->state = ANE_T6021_PM_SUSPENDED;
		break;
	case ANE_T6021_PM_SUSPENDED:
		/* Stopped by an earlier suspend; its restart is still deferred. */
		break;
	case ANE_T6021_PM_FAILED:
		if (pm->ops->park)
			ret = pm->ops->park(pm->ctx);
		break;
	}
	mutex_unlock(pm->fw_lock);
	return ret;
}

static inline int ane_t6021_pm_resume(struct ane_t6021_pm *pm)
{
	int ret = 0;

	mutex_lock(pm->fw_lock);
	/* ACTIVE: PM core may unwind before it reached this device's suspend.
	 * FAILED: unavailable until reboot; the failure was reported once.
	 */
	if (pm->state != ANE_T6021_PM_SUSPENDED)
		goto out;
	pm->state = ANE_T6021_PM_FAILED;
	ret = pm->ops->restart(pm->ctx);
	if (ret == ANE_T6021_PM_RESTART_DEFERRED) {
		pm->state = ANE_T6021_PM_SUSPENDED;
		ret = 0;
	} else if (!ret) {
		pm->state = ANE_T6021_PM_ACTIVE;
	}
out:
	mutex_unlock(pm->fw_lock);
	return ret;
}

/* Finish a restart that resume deferred. The caller holds the firmware
 * mutex, runs outside system PM transitions and has runtime PM available.
 */
static inline int ane_t6021_pm_restart_deferred(struct ane_t6021_pm *pm)
{
	int ret;

	lockdep_assert_held(pm->fw_lock);
	if (pm->state != ANE_T6021_PM_SUSPENDED)
		return 0;
	pm->state = ANE_T6021_PM_FAILED;
	ret = pm->ops->restart_deferred(pm->ctx);
	if (!ret)
		pm->state = ANE_T6021_PM_ACTIVE;
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
