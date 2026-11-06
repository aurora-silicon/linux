// SPDX-License-Identifier: GPL-2.0-only
/*
 * TI CD2E22 eUSB2 to USB 2.0 repeater
 *
 * Copyright (C) 2026 Ryan Murray
 */

#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define CD2E22_TUNABLE_ENTRY_SIZE 4
#define CD2E22_PORT_DISABLE_REG 0x82
#define CD2E22_PORT_DISABLE(port) BIT((port) + 5)

struct cd2e22 {
	struct device *dev;
	struct regmap *regmap;
	struct phy *phy;
	/* Serialize PHY initialization with system resume. */
	struct mutex lock;
	u8 *tunable;
	size_t tunable_len;
	u32 port;
	bool initialized;
};

static int cd2e22_apply_tunable_locked(struct cd2e22 *cd2e22)
{
	int ret;

	lockdep_assert_held(&cd2e22->lock);

	for (size_t i = 0; i < cd2e22->tunable_len;
	     i += CD2E22_TUNABLE_ENTRY_SIZE) {
		unsigned int readback;
		u8 reg = cd2e22->tunable[i];
		u8 mask = cd2e22->tunable[i + 1];
		u8 value = cd2e22->tunable[i + 2];

		ret = regmap_update_bits(cd2e22->regmap, reg, mask, value);
		if (ret)
			return dev_err_probe(cd2e22->dev, ret,
					     "failed to tune register %#02x\n",
					     reg);

		ret = regmap_read(cd2e22->regmap, reg, &readback);
		if (ret)
			return dev_err_probe(cd2e22->dev, ret,
					     "failed to verify register %#02x\n", reg);
		if ((readback & mask) != (value & mask))
			return dev_err_probe(cd2e22->dev, -EIO,
					     "register %#02x did not retain tuning\n", reg);
		dev_dbg(cd2e22->dev, "register %#02x = %#02x\n", reg, readback);
	}

	/* Firmware tuning leaves the channel disabled until its user opens it. */
	ret = regmap_clear_bits(cd2e22->regmap, CD2E22_PORT_DISABLE_REG,
				CD2E22_PORT_DISABLE(cd2e22->port));
	if (ret)
		return dev_err_probe(cd2e22->dev, ret,
				     "failed to enable repeater port %u\n",
				     cd2e22->port);

	return 0;
}

static int cd2e22_init(struct phy *phy)
{
	struct cd2e22 *cd2e22 = phy_get_drvdata(phy);

	guard(mutex)(&cd2e22->lock);
	cd2e22->initialized = true;
	if (!cd2e22->tunable)
		return 0;

	return cd2e22_apply_tunable_locked(cd2e22);
}

static const struct phy_ops cd2e22_phy_ops = {
	.init = cd2e22_init,
	.owner = THIS_MODULE,
};

static int cd2e22_resume(struct device *dev)
{
	struct cd2e22 *cd2e22 = dev_get_drvdata(dev);

	guard(mutex)(&cd2e22->lock);
	if (!cd2e22->initialized || !cd2e22->tunable)
		return 0;

	return cd2e22_apply_tunable_locked(cd2e22);
}

static DEFINE_SIMPLE_DEV_PM_OPS(cd2e22_pm_ops, NULL, cd2e22_resume);

static const struct regmap_config cd2e22_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

static int cd2e22_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct phy_provider *provider;
	struct cd2e22 *cd2e22;
	int count;
	int ret;

	cd2e22 = devm_kzalloc(dev, sizeof(*cd2e22), GFP_KERNEL);
	if (!cd2e22)
		return -ENOMEM;

	cd2e22->dev = dev;
	mutex_init(&cd2e22->lock);
	ret = device_property_read_u32(dev, "ti,port", &cd2e22->port);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read ti,port\n");
	if (cd2e22->port > 1)
		return dev_err_probe(dev, -EINVAL, "invalid repeater port %u\n",
				     cd2e22->port);

	if (!device_property_present(dev, "apple,tunable")) {
		dev_dbg(dev, "no firmware tuning data\n");
	} else {
		if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
			return dev_err_probe(dev, -EOPNOTSUPP,
					     "adapter does not support raw I2C transfers\n");

		count = device_property_count_u8(dev, "apple,tunable");
		if (count < CD2E22_TUNABLE_ENTRY_SIZE ||
		    count % CD2E22_TUNABLE_ENTRY_SIZE)
			return dev_err_probe(dev, -EINVAL,
					     "apple,tunable must contain four-byte entries\n");

		cd2e22->tunable_len = count;
		cd2e22->tunable = devm_kmalloc(dev, count, GFP_KERNEL);
		if (!cd2e22->tunable)
			return -ENOMEM;

		ret = device_property_read_u8_array(dev, "apple,tunable",
						    cd2e22->tunable, count);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to read apple,tunable\n");

		for (size_t i = 0; i < cd2e22->tunable_len;
		     i += CD2E22_TUNABLE_ENTRY_SIZE) {
			if (cd2e22->tunable[i + 3])
				return dev_err_probe(dev, -EINVAL,
						     "apple,tunable entry %zu has nonzero reserved byte\n",
						     i / CD2E22_TUNABLE_ENTRY_SIZE);
		}

		cd2e22->regmap = devm_regmap_init_i2c(client,
						      &cd2e22_regmap_config);
		if (IS_ERR(cd2e22->regmap))
			return dev_err_probe(dev, PTR_ERR(cd2e22->regmap),
					     "failed to allocate register map\n");
	}

	cd2e22->phy = devm_phy_create(dev, dev->of_node, &cd2e22_phy_ops);
	if (IS_ERR(cd2e22->phy))
		return dev_err_probe(dev, PTR_ERR(cd2e22->phy),
				     "failed to create PHY\n");

	phy_set_drvdata(cd2e22->phy, cd2e22);
	i2c_set_clientdata(client, cd2e22);

	/*
	 * Tune as soon as the repeater is reachable, as the firmware does; the
	 * PHY init and system resume reapply the same list.
	 */
	if (cd2e22->tunable) {
		guard(mutex)(&cd2e22->lock);
		ret = cd2e22_apply_tunable_locked(cd2e22);
		if (ret)
			return ret;
		cd2e22->initialized = true;
	}

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id cd2e22_of_match[] = {
	{ .compatible = "ti,cd2e22" },
	{ }
};
MODULE_DEVICE_TABLE(of, cd2e22_of_match);

static const struct i2c_device_id cd2e22_i2c_ids[] = {
	{ "cd2e22" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cd2e22_i2c_ids);

static struct i2c_driver cd2e22_driver = {
	.driver = {
		.name = "ti-cd2e22",
		.of_match_table = cd2e22_of_match,
		.pm = pm_sleep_ptr(&cd2e22_pm_ops),
	},
	.probe = cd2e22_probe,
	.id_table = cd2e22_i2c_ids,
};
module_i2c_driver(cd2e22_driver);

MODULE_AUTHOR("Ryan Murray <81541646+RyanTheTide@users.noreply.github.com>");
MODULE_DESCRIPTION("TI CD2E22 eUSB2 repeater driver");
MODULE_LICENSE("GPL");
