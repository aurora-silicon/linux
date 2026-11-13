// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The PM notifiers of the GPU: the hibernation gate, and per G17 device the drain and thaw
 * of its firmware around a hibernation and the statistics epoch around every system sleep.
 *
 * Order in the PM notifier chain (higher priority runs first on PREPARE and POST):
 *
 *	30	hibernation gate: refuses before any drain or release
 *	20	G17 drain; ahead of the display (10, -5) and PMP (10) notifiers
 *	-10	statistics epoch and the thaw; below the PMP restore (10), which must run first
 */
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/notifier.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/suspend.h>

#include "hibernate.h"

#define ASAHI_PM_PRIO_GATE	30
#define ASAHI_PM_PRIO_DRAIN	20
#define ASAHI_PM_PRIO_THAW	-10

struct asahi_pm_hooks {
	const void *snapshot;
	asahi_stats_suspend_fn suspend;
	asahi_stats_hibernate_fn hibernate;
	struct notifier_block pm_notifier;
	/* Drain at PM_HIBERNATION_PREPARE ahead of the display and PMP notifiers. */
	struct notifier_block hib_notifier;
	/* pm_notifier saw PM_HIBERNATION_PREPARE: it owns the thaw at PM_POST_HIBERNATION. */
	bool hib_late_prepared;
};

#ifdef CONFIG_HIBERNATION
/*
 * Hibernation gate. Native graphics owns firmware DMA (GPU page tables,
 * queues, firmware heap) that a hibernation image cannot recreate: after a
 * resume from disk the GPU is the instance the boot loader left halted, not
 * the one this driver programmed.
 *
 * A backend with a cold restore path (G17: it registers a hibernate
 * callback) drains its GPU at PM_HIBERNATION_PREPARE and re-boots the
 * firmware against the restored host state at PM_POST_HIBERNATION; while
 * only such devices are bound, hibernation is allowed. Any other bound GPU
 * refuses it.
 *
 * Restoring an image is always refused while a GPU is bound here: the kernel
 * that restores must not own a GPU firmware session of its own, since the
 * image's driver would then find a running firmware it did not program.
 *
 * The notifier is in the chain from module load to unload and does nothing
 * while no device is bound.
 */
static atomic_t asahi_hibernate_devices = ATOMIC_INIT(0);
/* Bound devices without a hibernate callback. */
static atomic_t asahi_hibernate_unrestorable = ATOMIC_INIT(0);

static int asahi_hibernate_notify(struct notifier_block *nb, unsigned long action,
				  void *unused)
{
	int devices = atomic_read(&asahi_hibernate_devices);

	switch (action) {
	case PM_HIBERNATION_PREPARE:
		if (devices && atomic_read(&asahi_hibernate_unrestorable)) {
			pr_warn("asahi: refusing hibernation: a bound GPU has no restore path\n");
			return notifier_from_errno(-EBUSY);
		}
		break;
	case PM_RESTORE_PREPARE:
		/*
		 * A restorable (G17) device bound here would leave a firmware running
		 * with this kernel's graph under the image's driver, which then takes it
		 * for its own, and a backend without a restore path cannot be restored
		 * at all: always refuse.
		 */
		if (devices) {
			pr_warn("asahi: refusing to restore an image: this kernel owns a GPU firmware session\n");
			return notifier_from_errno(-EBUSY);
		}
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block asahi_hibernate_nb = {
	.notifier_call = asahi_hibernate_notify,
	/* Ahead of the G17 drain: a refusal costs no drain. */
	.priority = ASAHI_PM_PRIO_GATE,
};

int asahi_hibernate_gate_init(void)
{
	return register_pm_notifier(&asahi_hibernate_nb);
}

void asahi_hibernate_gate_exit(void)
{
	unregister_pm_notifier(&asahi_hibernate_nb);
}

void asahi_hibernate_device_bound(bool restorable)
{
	if (!restorable)
		atomic_inc(&asahi_hibernate_unrestorable);
	atomic_inc(&asahi_hibernate_devices);
}

void asahi_hibernate_device_unbound(bool restorable)
{
	atomic_dec(&asahi_hibernate_devices);
	if (!restorable)
		atomic_dec(&asahi_hibernate_unrestorable);
}
#else
int asahi_hibernate_gate_init(void) { return 0; }
void asahi_hibernate_gate_exit(void) { }
void asahi_hibernate_device_bound(bool restorable) { }
void asahi_hibernate_device_unbound(bool restorable) { }
#endif

static int asahi_pm_notify(struct notifier_block *nb, unsigned long action, void *unused)
{
	struct asahi_pm_hooks *hooks = container_of(nb, struct asahi_pm_hooks, pm_notifier);

	switch (action) {
	case PM_HIBERNATION_PREPARE:
		/* The drain already ran in asahi_pm_drain_notify(). */
		hooks->suspend(hooks->snapshot, true);
		WRITE_ONCE(hooks->hib_late_prepared, true);
		break;
	case PM_SUSPEND_PREPARE:
	case PM_RESTORE_PREPARE:
		hooks->suspend(hooks->snapshot, true);
		break;
	case PM_POST_HIBERNATION:
		/*
		 * After a resume from disk the boot loader has cold-restarted the GPU
		 * firmware while the host state was restored from the image; after an
		 * aborted hibernation the firmware kept running. The callback tells
		 * them apart, re-boots a cold firmware against the restored state and
		 * reopens submission. This notifier has a lower priority than the PMP
		 * restore, which has already run.
		 */
		if (hooks->hibernate)
			hooks->hibernate(hooks->snapshot, ASAHI_HIBERNATE_THAW);
		hooks->suspend(hooks->snapshot, false);
		WRITE_ONCE(hooks->hib_late_prepared, false);
		break;
	case PM_POST_SUSPEND:
	case PM_POST_RESTORE:
		hooks->suspend(hooks->snapshot, false);
		break;
	}
	return NOTIFY_OK;
}

/*
 * G17 drain, at a priority above the display (DCP) and PMP notifiers: a GPU that cannot be
 * drained refuses the hibernation before any other driver has given anything up (the DCP
 * releases its display links at PM_HIBERNATION_PREPARE). A refusal here reaches no later
 * notifier, and the chain sends PM_POST_HIBERNATION only to the notifiers before it.
 *
 * The thaw stays with asahi_pm_notify(), below the PMP restore that a cold firmware
 * needs. Only when the transition failed before that notifier saw PM_HIBERNATION_PREPARE
 * (a later notifier refused it) does it never get PM_POST_HIBERNATION; the firmware then
 * kept running, and the thaw runs here. After a resume from disk the flag is set in the
 * image, so the restored kernel leaves the cold republish to asahi_pm_notify().
 */
static int asahi_pm_drain_notify(struct notifier_block *nb, unsigned long action, void *unused)
{
	struct asahi_pm_hooks *hooks = container_of(nb, struct asahi_pm_hooks, hib_notifier);
	int ret;

	switch (action) {
	case PM_HIBERNATION_PREPARE:
		WRITE_ONCE(hooks->hib_late_prepared, false);
		ret = hooks->hibernate(hooks->snapshot, ASAHI_HIBERNATE_FREEZE);
		if (ret)
			return notifier_from_errno(ret);
		break;
	case PM_POST_HIBERNATION:
		if (!READ_ONCE(hooks->hib_late_prepared))
			hooks->hibernate(hooks->snapshot, ASAHI_HIBERNATE_THAW);
		break;
	}
	return NOTIFY_OK;
}

void *asahi_pm_hooks_register(const void *snapshot, asahi_stats_suspend_fn suspend,
			      asahi_stats_hibernate_fn hibernate)
{
	struct asahi_pm_hooks *hooks;
	int ret;

	if (!suspend)
		return NULL;
	hooks = kzalloc_obj(*hooks);
	if (!hooks)
		return ERR_PTR(-ENOMEM);
	hooks->snapshot = snapshot;
	hooks->suspend = suspend;
	hooks->hibernate = hibernate;
	hooks->pm_notifier.notifier_call = asahi_pm_notify;
	hooks->pm_notifier.priority = ASAHI_PM_PRIO_THAW;
	ret = register_pm_notifier(&hooks->pm_notifier);
	if (ret) {
		kfree(hooks);
		return ERR_PTR(ret);
	}
	if (hibernate) {
		hooks->hib_notifier.notifier_call = asahi_pm_drain_notify;
		hooks->hib_notifier.priority = ASAHI_PM_PRIO_DRAIN;
		ret = register_pm_notifier(&hooks->hib_notifier);
		if (ret) {
			unregister_pm_notifier(&hooks->pm_notifier);
			kfree(hooks);
			return ERR_PTR(ret);
		}
	}
	return hooks;
}

void asahi_pm_hooks_unregister(void *handle)
{
	struct asahi_pm_hooks *hooks = handle;

	if (!hooks)
		return;
	if (hooks->hibernate)
		unregister_pm_notifier(&hooks->hib_notifier);
	unregister_pm_notifier(&hooks->pm_notifier);
	kfree(hooks);
}
