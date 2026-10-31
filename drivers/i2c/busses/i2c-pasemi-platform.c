// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 The Asahi Linux Contributors
 *
 * PA Semi PWRficient SMBus host driver for Apple SoCs
 */

#include <linux/clk.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/types.h>

#include "i2c-pasemi-core.h"

struct pasemi_platform_i2c_data {
	struct pasemi_smbus smbus;
	struct clk *clk_ref;
	int irq;
};

static int pasemi_platform_i2c_runtime_suspend(struct device *dev)
{
	struct pasemi_platform_i2c_data *data = dev_get_drvdata(dev);

	if (data->smbus.use_irq)
		disable_irq(data->irq);
	clk_disable_unprepare(data->clk_ref);

	return 0;
}

static int pasemi_platform_i2c_runtime_resume(struct device *dev)
{
	struct pasemi_platform_i2c_data *data = dev_get_drvdata(dev);
	int error;

	error = clk_prepare_enable(data->clk_ref);
	if (error)
		return error;

	/* Restore the divider, enable bit and FIFOs after domain power loss. */
	pasemi_i2c_init(&data->smbus);
	if (data->smbus.use_irq)
		enable_irq(data->irq);

	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(pasemi_platform_i2c_pm_ops,
				 pasemi_platform_i2c_runtime_suspend,
				 pasemi_platform_i2c_runtime_resume, NULL);

static void pasemi_platform_i2c_disable_pm(void *arg)
{
	struct device *dev = arg;

	/* Registered before the adapter, after the IRQ: clients are gone now. */
	pm_runtime_disable(dev);
	pm_runtime_dont_use_autosuspend(dev);
	if (!pm_runtime_status_suspended(dev))
		pasemi_platform_i2c_runtime_suspend(dev);
	pm_runtime_set_suspended(dev);
}

static int
pasemi_platform_i2c_calc_clk_div(struct pasemi_platform_i2c_data *data,
				 u32 frequency)
{
	unsigned long clk_rate = clk_get_rate(data->clk_ref);

	if (!clk_rate)
		return -EINVAL;

	data->smbus.clk_div = DIV_ROUND_UP(clk_rate, 16 * frequency);
	if (data->smbus.clk_div < 4)
		return dev_err_probe(data->smbus.dev, -EINVAL,
				     "Bus frequency %d is too fast.\n",
				     frequency);
	if (data->smbus.clk_div > 0xff)
		return dev_err_probe(data->smbus.dev, -EINVAL,
				     "Bus frequency %d is too slow.\n",
				     frequency);

	return 0;
}

static int pasemi_platform_i2c_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pasemi_platform_i2c_data *data;
	struct pasemi_smbus *smbus;
	u32 frequency;
	int error;
	int irq_num;

	data = devm_kzalloc(dev, sizeof(struct pasemi_platform_i2c_data),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	smbus = &data->smbus;
	smbus->dev = dev;

	smbus->ioaddr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(smbus->ioaddr))
		return PTR_ERR(smbus->ioaddr);

	if (of_property_read_u32(dev->of_node, "clock-frequency", &frequency))
		frequency = I2C_MAX_STANDARD_MODE_FREQ;

	data->clk_ref = devm_clk_get(dev, NULL);
	if (IS_ERR(data->clk_ref))
		return PTR_ERR(data->clk_ref);

	error = pasemi_platform_i2c_calc_clk_div(data, frequency);
	if (error)
		return error;

	platform_set_drvdata(pdev, data);
	error = clk_prepare_enable(data->clk_ref);
	if (error)
		return error;

	pasemi_i2c_init(smbus);
	irq_num = platform_get_irq_optional(pdev, 0);
	if (irq_num < 0 && irq_num != -ENXIO) {
		error = irq_num;
		goto disable_clk;
	}
	if (irq_num > 0) {
		error = devm_request_irq(dev, irq_num, pasemi_irq_handler, 0,
					 "pasemi_apple_i2c", smbus);
		if (error)
			goto disable_clk;
		smbus->use_irq = 1;
		data->irq = irq_num;
	}

	pm_runtime_get_noresume(dev);
	error = pm_runtime_set_active(dev);
	if (error) {
		pm_runtime_put_noidle(dev);
		goto disable_clk;
	}
	pm_runtime_set_autosuspend_delay(dev, 100);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	error = devm_add_action_or_reset(dev, pasemi_platform_i2c_disable_pm, dev);
	if (error) {
		pm_runtime_put_noidle(dev);
		return error;
	}

	smbus->runtime_pm = true;
	smbus->adapter.dev.of_node = dev->of_node;
	error = pasemi_i2c_register_adapter(smbus);
	pm_runtime_put_autosuspend(dev);
	return error;

disable_clk:
	if (smbus->use_irq)
		devm_free_irq(dev, data->irq, smbus);
	clk_disable_unprepare(data->clk_ref);
	return error;
}

static const struct of_device_id pasemi_platform_i2c_of_match[] = {
	{ .compatible = "apple,t8103-i2c" },
	{ .compatible = "apple,i2c" },
	{},
};
MODULE_DEVICE_TABLE(of, pasemi_platform_i2c_of_match);

static struct platform_driver pasemi_platform_i2c_driver = {
	.driver	= {
		.name			= "i2c-apple",
		.of_match_table		= pasemi_platform_i2c_of_match,
		.pm			= pm_ptr(&pasemi_platform_i2c_pm_ops),
	},
	.probe	= pasemi_platform_i2c_probe,
};
module_platform_driver(pasemi_platform_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_DESCRIPTION("Apple/PASemi SMBus platform driver");
