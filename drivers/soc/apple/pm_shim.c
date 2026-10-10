// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * System sleep for the SEP driver: the notifier only forwards "about to sleep"
 * and "back from sleep" to Rust, which ends any Touch ID capture before the
 * system suspends (see pm.rs), and the enclave's power domains are kept on
 * across sleep.
 */
#include <linux/compiler.h>
#include <linux/device.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/pm_domain.h>
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

static void sep_pm_release_domains(void *data)
{
	struct dev_pm_domain_list *list = data;
	u32 i;

	for (i = 0; i < list->num_pds; i++)
		dev_pm_syscore_device(list->pd_devs[i], false);
}

/*
 * The enclave runs from boot to shutdown, and on some SoCs it needs power
 * domains besides its own: once one of them is switched off, it stops
 * reading its mailbox for good. A node that lists more than one domain gets
 * all of them attached here, powered on through device links for as long as
 * the driver is bound, and taken out of system sleep, which would otherwise
 * switch a domain off in the noirq phase once its other users had suspended.
 * A node with a single domain is attached by the platform core as before.
 */
int sep_pm_keep_domains(void *ptr)
{
	struct device *dev = ptr;
	struct dev_pm_domain_attach_data data = {
		.pd_flags = PD_FLAG_DEV_LINK_ON,
	};
	struct dev_pm_domain_list *list;
	int count, ret;
	u32 i;

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count <= 1)
		return 0;

	ret = devm_pm_domain_attach_list(dev, &data, &list);
	if (ret < 0)
		return ret;

	for (i = 0; i < list->num_pds; i++)
		dev_pm_syscore_device(list->pd_devs[i], true);

	return devm_add_action_or_reset(dev, sep_pm_release_domains, list);
}
