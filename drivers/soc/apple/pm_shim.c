// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * System-sleep notifier for the SEP driver. It only forwards "about to sleep"
 * and "back from sleep" to Rust, which ends any Touch ID capture before the
 * system suspends; see pm.rs.
 */
#include <linux/compiler.h>
#include <linux/notifier.h>
#include <linux/suspend.h>

#include "shim.h"

static int (*sep_pm_event)(bool entering);

static int sep_pm_notify(struct notifier_block *nb, unsigned long action,
			 void *data)
{
	int (*event)(bool entering) = READ_ONCE(sep_pm_event);

	if (!event)
		return NOTIFY_DONE;

	switch (action) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		return notifier_from_errno(event(true));
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		event(false);
		return NOTIFY_OK;
	default:
		return NOTIFY_DONE;
	}
}

static struct notifier_block sep_pm_nb = {
	.notifier_call = sep_pm_notify,
};

int sep_pm_register(int (*event)(bool entering))
{
	int ret;

	WRITE_ONCE(sep_pm_event, event);
	ret = register_pm_notifier(&sep_pm_nb);
	if (ret)
		WRITE_ONCE(sep_pm_event, NULL);
	return ret;
}

/* Returns only once no notifier call is still running. */
void sep_pm_unregister(void)
{
	unregister_pm_notifier(&sep_pm_nb);
	WRITE_ONCE(sep_pm_event, NULL);
}
