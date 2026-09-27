// SPDX-License-Identifier: GPL-2.0

#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

__rust_helper int rust_helper_pm_runtime_get_sync(struct device *dev)
{
	return pm_runtime_get_sync(dev);
}

__rust_helper int rust_helper_pm_runtime_put_sync(struct device *dev)
{
	return pm_runtime_put_sync(dev);
}

__rust_helper void rust_helper_pm_runtime_put_noidle(struct device *dev)
{
	pm_runtime_put_noidle(dev);
}
