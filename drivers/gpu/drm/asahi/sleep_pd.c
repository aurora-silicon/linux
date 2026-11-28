// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hold the GPU's attached power domain across suspend-to-idle.
 *
 * When a system sleep does not hand control to platform firmware
 * (!pm_suspend_via_firmware()), the GPU driver leaves its firmware running.
 * genpd would otherwise power the GPU's domain (such as ps_gfx) off in
 * genpd_suspend_noirq(), cutting power under the live coprocessor.
 *
 * Register a temporary GENPD_NOTIFY_PRE_OFF veto notifier on the GPU device's
 * own genpd for the noirq sleep window, matching pcie-apple's consumer-side
 * hold without changing any other power domain.
 */

#include <linux/bug.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/pm_domain.h>
#include <linux/soc/apple/sleep.h>
#include <linux/suspend.h>
#include <linux/types.h>

#include "sleep_pd.h"

struct asahi_sleep_pd {
	struct notifier_block nb;
	bool active;
};

/* IRQ-safe domains invoke power notifiers in atomic context. */
static int asahi_sleep_pd_notify(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	if (action != GENPD_NOTIFY_PRE_OFF)
		return NOTIFY_OK;
	return notifier_from_errno(-EBUSY);
}

static void asahi_sleep_pd_release(struct device *dev, void *res)
{
	struct asahi_sleep_pd *hold = res;

	if (hold->active) {
		dev_pm_genpd_remove_notifier(dev);
		hold->active = false;
	}
}

int asahi_sleep_pd_init(struct device *dev)
{
	struct asahi_sleep_pd *hold;

	hold = devres_alloc(asahi_sleep_pd_release, sizeof(*hold), GFP_KERNEL);
	if (!hold)
		return -ENOMEM;

	hold->nb.notifier_call = asahi_sleep_pd_notify;
	devres_add(dev, hold);
	return 0;
}

int asahi_sleep_pd_suspend_noirq_hold(struct device *dev, bool hold_firmware)
{
	struct asahi_sleep_pd *hold;
	int ret;

	if (pm_suspend_via_firmware() && !hold_firmware)
		return 0;

	/*
	 * Every current GPU node attaches a single power domain (M2 attaches
	 * pmp_report_gfx with APPLE_USE_PMP, which keeps ps_gfx up as its
	 * parent), so dev->pm_domain is a single genpd that
	 * dev_pm_genpd_is_on() and dev_pm_genpd_add_notifier() target
	 * directly. For a multi-domain GPU node, genpd_dev_pm_attach() leaves
	 * dev->pm_domain unset, so dev_pm_genpd_is_on() would return false and
	 * genpd would power those domains off under live GPU firmware; refuse
	 * sleep with -EBUSY rather than failing open.
	 */
	if (!dev->pm_domain) {
		if (of_count_phandle_with_args(dev->of_node, "power-domains",
					       "#power-domain-cells") > 1) {
			dev_warn_once(dev,
				      "GPU has multiple power domains; sleep hold only supports a single genpd, refusing sleep\n");
			return -EBUSY;
		}
		return 0;
	}

	if (!dev_pm_genpd_is_on(dev))
		return 0;

	hold = devres_find(dev, asahi_sleep_pd_release, NULL, NULL);
	if (WARN_ON_ONCE(!hold))
		return -EBUSY;

	if (hold->active)
		return 0;

	ret = dev_pm_genpd_add_notifier(dev, &hold->nb);
	if (ret == -EEXIST) {
		dev_warn(dev, "genpd already has a power notifier; refusing sleep\n");
		return -EBUSY;
	}
	if (ret)
		return ret;

	hold->active = true;
	return 0;
}

int asahi_sleep_pd_suspend_noirq(struct device *dev)
{
	return asahi_sleep_pd_suspend_noirq_hold(dev, false);
}

int asahi_sleep_pd_resume_noirq(struct device *dev)
{
	struct asahi_sleep_pd *hold;

	hold = devres_find(dev, asahi_sleep_pd_release, NULL, NULL);
	if (!hold || !hold->active)
		return 0;

	dev_pm_genpd_remove_notifier(dev);
	hold->active = false;
	return 0;
}

int asahi_sleep_pd_domain_set(struct device *dev, bool on)
{
	struct device_node *np;
	int ret;

	np = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (!np)
		return -ENODEV;
	ret = apple_sleep_domain_set(np, on);
	of_node_put(np);
	return ret;
}
