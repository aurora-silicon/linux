// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T6041 CIO cluster reconfiguration controller
 *
 * The three CIO ports share one PMGR register. Each port has a request bit
 * and a busy bit sixteen positions above it. Firmware waits before and after
 * the read/modify/write request, with a 192 ms budget for each wait.
 */

#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#define T6041_CIO_RECONFIG_PORTS	3
#define T6041_CIO_RECONFIG_POLL_US	100
#define T6041_CIO_RECONFIG_TIMEOUT_US	192000

struct t6041_cio_reconfig {
	struct reset_controller_dev rcdev;
	struct regmap *regmap;
	struct mutex lock; /* serializes the shared request register */
	u32 offset;
};

static int t6041_cio_reconfig_deassert(struct reset_controller_dev *rcdev,
				       unsigned long id)
{
	struct t6041_cio_reconfig *reconfig =
		container_of(rcdev, struct t6041_cio_reconfig, rcdev);
	u32 busy = BIT(16 + id);
	u32 request = BIT(id);
	u32 value;
	int ret;

	guard(mutex)(&reconfig->lock);

	ret = regmap_read_poll_timeout(reconfig->regmap, reconfig->offset, value,
				       !(value & busy), T6041_CIO_RECONFIG_POLL_US,
				       T6041_CIO_RECONFIG_TIMEOUT_US);
	if (ret)
		return ret;

	/* Force the RMW write even if a stale request bit still reads as one. */
	ret = regmap_write_bits(reconfig->regmap, reconfig->offset, request, request);
	if (ret)
		return ret;

	return regmap_read_poll_timeout(reconfig->regmap, reconfig->offset, value,
					!(value & busy), T6041_CIO_RECONFIG_POLL_US,
					T6041_CIO_RECONFIG_TIMEOUT_US);
}

static const struct reset_control_ops t6041_cio_reconfig_ops = {
	.deassert = t6041_cio_reconfig_deassert,
};

static int t6041_cio_reconfig_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct t6041_cio_reconfig *reconfig;
	int ret;

	reconfig = devm_kzalloc(dev, sizeof(*reconfig), GFP_KERNEL);
	if (!reconfig)
		return -ENOMEM;

	ret = devm_mutex_init(dev, &reconfig->lock);
	if (ret)
		return ret;

	reconfig->regmap = syscon_node_to_regmap(dev->of_node->parent);
	if (IS_ERR(reconfig->regmap))
		return dev_err_probe(dev, PTR_ERR(reconfig->regmap),
				     "failed to get parent PMGR regmap\n");

	ret = of_property_read_u32(dev->of_node, "reg", &reconfig->offset);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read reconfiguration offset\n");

	reconfig->rcdev.owner = THIS_MODULE;
	reconfig->rcdev.ops = &t6041_cio_reconfig_ops;
	reconfig->rcdev.of_node = dev->of_node;
	reconfig->rcdev.nr_resets = T6041_CIO_RECONFIG_PORTS;

	return devm_reset_controller_register(dev, &reconfig->rcdev);
}

static const struct of_device_id t6041_cio_reconfig_of_match[] = {
	{ .compatible = "apple,t6041-cio-reset" },
	{}
};
MODULE_DEVICE_TABLE(of, t6041_cio_reconfig_of_match);

static struct platform_driver t6041_cio_reconfig_driver = {
	.probe = t6041_cio_reconfig_probe,
	.driver = {
		.name = "apple-t6041-cio-reconfig",
		.of_match_table = t6041_cio_reconfig_of_match,
	},
};
module_platform_driver(t6041_cio_reconfig_driver);

MODULE_AUTHOR("The Asahi Linux Contributors");
MODULE_DESCRIPTION("Apple T6041 CIO cluster reconfiguration controller");
MODULE_LICENSE("Dual MIT/GPL");
