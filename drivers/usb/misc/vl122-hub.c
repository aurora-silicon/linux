// SPDX-License-Identifier: GPL-2.0-only
/*
 * VIA Labs VL122 USB hub sideband configuration
 *
 * Copyright (C) 2026 Ryan Murray
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define HUB_TUNABLE_ENTRY_SIZE 4
#define HUB_RESET_POLL_US 1000
#define HUB_RESET_TIMEOUT_US 1000000
#define HUB_RELEASE_DELAY_MS 100
/* The management interface needs a moment after reset before it answers. */
#define HUB_ASSERT_SETTLE_US 20000

#define USB_VENDOR_ID_GENESYS_LOGIC 0x05e3
#define USB_VENDOR_ID_VIA_LABS 0x2109

#define VL122_VARIANT_REG 0x2e
#define VL122_VARIANT_VL 0x00
#define VL122_VARIANT_VLF0 0xf0

struct vl122_hub {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	/* Serialize reset-window configuration with system resume. */
	struct mutex lock;
	bool has_tunable;
};

static int vl122_read_vendor(struct vl122_hub *hub, unsigned int *vendor)
{
	unsigned int high;
	unsigned int low;
	int ret;

	ret = regmap_read(hub->regmap, 0, &low);
	if (ret)
		return ret;

	ret = regmap_read(hub->regmap, 1, &high);
	if (ret)
		return ret;

	*vendor = low | high << 8;
	return 0;
}

static int vl122_select_tunable(struct vl122_hub *hub, const char **name)
{
	unsigned int variant;
	unsigned int vendor;
	int ret;

	ret = vl122_read_vendor(hub, &vendor);
	if (ret)
		return ret;

	switch (vendor) {
	case USB_VENDOR_ID_GENESYS_LOGIC:
		*name = "apple,tunable-gl";
		return 0;
	case USB_VENDOR_ID_VIA_LABS:
		ret = regmap_read(hub->regmap, VL122_VARIANT_REG, &variant);
		if (ret)
			return ret;

		switch (variant) {
		case VL122_VARIANT_VL:
			*name = "apple,tunable-vl";
			return 0;
		case VL122_VARIANT_VLF0:
			*name = "apple,tunable-vlf0";
			return 0;
		default:
			return dev_err_probe(hub->dev, -ENODEV,
					     "unsupported VIA variant %#x\n",
					     variant);
		}
	default:
		return dev_err_probe(hub->dev, -ENODEV,
				     "unsupported hub vendor %#06x\n", vendor);
	}
}

static int vl122_apply_tunable(struct vl122_hub *hub, const char *name)
{
	u8 *tunable __free(kfree) = NULL;
	int count;
	int i;
	int ret;

	count = device_property_count_u8(hub->dev, name);
	if (count < HUB_TUNABLE_ENTRY_SIZE || count % HUB_TUNABLE_ENTRY_SIZE)
		return dev_err_probe(hub->dev, -EINVAL,
				     "%s must contain four-byte entries\n",
				     name);

	tunable = kmalloc(count, GFP_KERNEL);
	if (!tunable)
		return -ENOMEM;

	ret = device_property_read_u8_array(hub->dev, name, tunable, count);
	if (ret)
		return dev_err_probe(hub->dev, ret, "failed to read %s\n",
				     name);

	for (i = 0; i < count; i += HUB_TUNABLE_ENTRY_SIZE) {
		unsigned int readback;
		u8 reg = tunable[i];
		u8 mask = tunable[i + 1];
		u8 value = tunable[i + 2];

		if (tunable[i + 3])
			return dev_err_probe(hub->dev, -EINVAL,
					     "%s entry %d has nonzero reserved byte\n",
					     name, i / HUB_TUNABLE_ENTRY_SIZE);

		ret = regmap_update_bits(hub->regmap, reg, mask, value);
		if (ret)
			return dev_err_probe(hub->dev, ret,
					     "failed to tune register %#x\n",
					     reg);

		ret = regmap_read(hub->regmap, reg, &readback);
		if (ret)
			return dev_err_probe(hub->dev, ret,
					     "failed to verify register %#x\n", reg);
		if ((readback & mask) != (value & mask))
			return dev_err_probe(hub->dev, -EIO,
					     "register %#x did not retain tuning\n", reg);
		dev_dbg(hub->dev, "register %#x = %#02x\n", reg, readback);
	}

	return 0;
}

static int vl122_configure(struct vl122_hub *hub)
{
	const char *tunable;
	unsigned int value;
	int err;
	int ret;

	guard(mutex)(&hub->lock);

	/* The sideband registers are available only while reset is held. */
	gpiod_set_value_cansleep(hub->reset_gpio, 1);
	fsleep(HUB_ASSERT_SETTLE_US);

	ret = read_poll_timeout(regmap_read, err, !err, HUB_RESET_POLL_US,
				HUB_RESET_TIMEOUT_US, false, hub->regmap, 0,
				&value);
	if (ret) {
		ret = dev_err_probe(hub->dev, ret,
				    "configuration interface did not become ready\n");
		goto release_reset;
	}

	ret = vl122_select_tunable(hub, &tunable);
	if (ret)
		goto release_reset;

	ret = vl122_apply_tunable(hub, tunable);
	if (!ret)
		dev_dbg(hub->dev, "applied %s\n", tunable);

release_reset:
	gpiod_set_value_cansleep(hub->reset_gpio, 0);
	msleep(HUB_RELEASE_DELAY_MS);
	return ret;
}

static int vl122_resume(struct device *dev)
{
	struct vl122_hub *hub = dev_get_drvdata(dev);

	if (!hub->has_tunable)
		return 0;

	return vl122_configure(hub);
}

static DEFINE_SIMPLE_DEV_PM_OPS(vl122_pm_ops, NULL, vl122_resume);

static const struct regmap_config vl122_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

static int vl122_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct vl122_hub *hub;
	bool has_tunable;

	hub = devm_kzalloc(dev, sizeof(*hub), GFP_KERNEL);
	if (!hub)
		return -ENOMEM;

	hub->dev = dev;
	mutex_init(&hub->lock);
	i2c_set_clientdata(client, hub);

	has_tunable = device_property_present(dev, "apple,tunable-vl") ||
		       device_property_present(dev, "apple,tunable-vlf0") ||
		       device_property_present(dev, "apple,tunable-gl");
	hub->has_tunable = has_tunable;
	if (!hub->has_tunable) {
		dev_dbg(dev, "no firmware tuning data\n");
		return 0;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "adapter does not support raw I2C transfers\n");

	hub->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(hub->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(hub->reset_gpio),
				     "failed to get reset GPIO\n");

	hub->regmap = devm_regmap_init_i2c(client, &vl122_regmap_config);
	if (IS_ERR(hub->regmap))
		return dev_err_probe(dev, PTR_ERR(hub->regmap),
				     "failed to allocate register map\n");

	return vl122_configure(hub);
}

static const struct of_device_id vl122_of_match[] = {
	{ .compatible = "via,vl122" },
	{ }
};
MODULE_DEVICE_TABLE(of, vl122_of_match);

static const struct i2c_device_id vl122_i2c_ids[] = {
	{ "vl122" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, vl122_i2c_ids);

static struct i2c_driver vl122_driver = {
	.driver = {
		.name = "vl122-hub",
		.of_match_table = vl122_of_match,
		.pm = pm_sleep_ptr(&vl122_pm_ops),
	},
	.probe = vl122_probe,
	.id_table = vl122_i2c_ids,
};
module_i2c_driver(vl122_driver);

MODULE_AUTHOR("Ryan Murray <81541646+RyanTheTide@users.noreply.github.com>");
MODULE_DESCRIPTION("VIA Labs VL122 USB hub sideband configuration driver");
MODULE_LICENSE("GPL");
