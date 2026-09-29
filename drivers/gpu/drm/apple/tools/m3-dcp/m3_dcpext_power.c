// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Explicit, guarded DCPEXT0 power-domain acquisition through Linux genpd.
 * Does not start/reset firmware, change DARTs or access the HDMI PHY.
 * Keep the domain reference until reboot, including after a failed resume.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#ifndef DCPEXT_INSTANCE
#define DCPEXT_INSTANCE 0
#endif

static struct platform_device *owner;
static struct device *domain;
static int result = -EINPROGRESS;
module_param(result, int, 0400);

static int __init acquire(void)
{
	struct device_node *node;
	u32 version;
	int ret;

	if (!of_machine_is_compatible("apple,j514s") ||
	    !of_machine_is_compatible("apple,t6030"))
		return -ENODEV;
	node = of_find_node_by_path(DCPEXT_INSTANCE ? "dcpext1" : "dcpext0");
	if (!node)
		return -ENODEV;
	if (of_device_is_available(node) ||
	    of_property_read_u32(node, "apple,j514s-dcpext-reservations", &version) ||
	    version != 1) {
		of_node_put(node);
		return -EINVAL;
	}
	owner = platform_device_alloc(DCPEXT_INSTANCE ? "m3-dcpext1-power" : "m3-dcpext-power", PLATFORM_DEVID_NONE);
	if (!owner) {
		of_node_put(node);
		return -ENOMEM;
	}
	device_set_node(&owner->dev, of_fwnode_handle(node));
	ret = platform_device_add(owner);
	if (ret) {
		platform_device_put(owner);
		of_node_put(node);
		return ret;
	}
	domain = dev_pm_domain_attach_by_id(&owner->dev, 0);
	if (IS_ERR_OR_NULL(domain)) {
		ret = domain ? PTR_ERR(domain) : -ENODEV;
		platform_device_unregister(owner);
		of_node_put(node);
		return ret;
	}
	pr_info("M3 DCPEXT%u acquiring CPU/FE/SYS domains via genpd\n", DCPEXT_INSTANCE);
	result = pm_runtime_resume_and_get(domain);
	pr_info("M3 DCPEXT%u power acquisition result=%d; retained until reboot\n", DCPEXT_INSTANCE, result);
	return 0;
}

module_init(acquire);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J514S guarded external display power-domain acquisition");
