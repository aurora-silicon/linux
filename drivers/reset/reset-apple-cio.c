// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC CIO (USB4/Thunderbolt) block reset driver
 *
 * Copyright The Asahi Linux Contributors
 *
 * The CIO blocks have a reset inside the power manager (PMGR) that
 * has to be deasserted before their co-processor can be booted. On
 * t8103 each port comes with a dedicated register page while t600x
 * uses a single register shared by all ports of a die inside the PMGR
 * MMIO region.
 *
 * Only the deassert direction is implemented: a request to assert returns
 * immediately on every SoC, hence the missing .assert op here. The register
 * layout is not shared between generations:
 *
 *	SoC	regmap	offset		request		busy
 *	t8103	0	0x84000 + 0x4000 * port	BIT(0), rmw	BIT(1)
 *	t8112	0x63	0x8000 / 0x4000	BIT(0), rmw	BIT(1)
 *	t8122	0x62	0x8		BIT(slot)	BIT(16 + slot)
 *	t6000	0	0x1a034		BIT(port)	BIT(16 + port)
 *	t6020	0	0xa02c		BIT(port)	BIT(16 + port)
 *	t6030	0x62	0x8		BIT(slot)	BIT(16 + slot)
 *	t6031	?	0x2028		BIT(port)	BIT(16 + port)
 *	t6041	?	0x2024		BIT(slot)	BIT(16 + slot)
 *	t6050	?	0x20060		BIT(2 * port)	BIT(2 * port + 1)
 *
 * t6000, t6020, t6031 and t6050 split slot into a die index and a port with
 * "port = slot % 4", which the device tree expresses as one controller node
 * per die instead. The others index by slot directly.
 *
 * "regmap" is an ApplePMGR::RegMap enum rather than an address. Only 0 is
 * pinned down, as the PMGR block itself: t8103 resolves to 0x23b784000 and
 * t6020 to the pmgr node plus 0xa02c, both of which match this driver. The
 * non-zero selectors are unresolved, so t8122 and t6030 cannot be wired up
 * yet even though their offset and bit layout are known.
 *
 * t8132 and t8142 have a different shape again, indexing a per-port table out
 * of a driver-private structure rather than forming an offset, and have not
 * been decoded.
 *
 * Every implementation polls the busy bits, stores the request, then polls
 * again, with a 192 ms budget. The first poll's result is discarded.
 */

#include <linux/bits.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#define APPLE_CIO_RESET_POLL_US 100
#define APPLE_CIO_RESET_TIMEOUT_US 100000

#define T8103_CIO_CTRL_STRIDE 0x4000
#define T8103_CIO_CTRL_INIT_REQ BIT(0)
#define T8103_CIO_CTRL_INIT_BUSY BIT(1)
#define T8103_CIO_CTRL_INIT_DONE BIT(2)

#define T6000_CIO_CTRL_INIT_REQ(port) BIT(port)
#define T6000_CIO_CTRL_INIT_BUSY(port) BIT(16 + (port))

struct apple_cio_reset;

struct apple_cio_reset_variant {
	unsigned int nr_resets;
	bool pmgr_child;
	int (*deassert)(struct apple_cio_reset *priv, unsigned long id);
};

struct apple_cio_reset {
	struct reset_controller_dev rcdev;
	const struct apple_cio_reset_variant *variant;
	struct device *dev;
	struct regmap *regmap;
	u32 offset;
	struct mutex lock;
};

static int t8103_cio_deassert(struct apple_cio_reset *priv, unsigned long id)
{
	u32 offset = priv->offset + id * T8103_CIO_CTRL_STRIDE;
	u32 val;
	int ret;

	/*
	 * Wait for a reconfiguration already in flight, as Apple does, and like Apple
	 * carry on regardless of the result. See the t6000 variant for why this is only
	 * a warning.
	 *
	 * Apple also sets the request bit with a read-modify-write rather than a plain
	 * store. That is deliberately not copied here: the poll below waits for the
	 * whole register to read exactly INIT_DONE, which works on every machine this
	 * has run on, and preserving unknown bits could stop that value ever appearing.
	 */
	ret = regmap_read_poll_timeout(priv->regmap, offset, val,
				       !(val & T8103_CIO_CTRL_INIT_BUSY),
				       APPLE_CIO_RESET_POLL_US,
				       APPLE_CIO_RESET_TIMEOUT_US);
	if (ret)
		dev_warn(priv->dev, "CIO %lu still busy before reconfigure request\n", id);

	ret = regmap_write(priv->regmap, offset, T8103_CIO_CTRL_INIT_REQ);
	if (ret)
		return ret;

	return regmap_read_poll_timeout(priv->regmap, offset, val,
					val == T8103_CIO_CTRL_INIT_DONE,
					APPLE_CIO_RESET_POLL_US,
					APPLE_CIO_RESET_TIMEOUT_US);
}

static int t6000_cio_deassert(struct apple_cio_reset *priv, unsigned long id)
{
	u32 val;
	int ret;

	guard(mutex)(&priv->lock);

	/*
	 * Every port on the die shares this register, so a reconfiguration requested for
	 * another port can still be running. The busy bits must clear before the request
	 * is stored, and the result of that wait is advisory, so only
	 * warn here: waiting can only help, while failing would turn a slow reconfigure
	 * into a probe failure.
	 */
	ret = regmap_read_poll_timeout(priv->regmap, priv->offset, val,
				       !(val & T6000_CIO_CTRL_INIT_BUSY(id)),
				       APPLE_CIO_RESET_POLL_US,
				       APPLE_CIO_RESET_TIMEOUT_US);
	if (ret)
		dev_warn(priv->dev, "CIO %lu still busy before reconfigure request\n", id);

	ret = regmap_write(priv->regmap, priv->offset, T6000_CIO_CTRL_INIT_REQ(id));
	if (ret)
		return ret;

	return regmap_read_poll_timeout(priv->regmap, priv->offset, val,
					!(val & T6000_CIO_CTRL_INIT_BUSY(id)),
					APPLE_CIO_RESET_POLL_US,
					APPLE_CIO_RESET_TIMEOUT_US);
}

static const struct apple_cio_reset_variant apple_t8103_cio_reset = {
	.nr_resets = 2,
	.deassert = t8103_cio_deassert,
};

static const struct apple_cio_reset_variant apple_t6000_cio_reset = {
	.nr_resets = 4,
	.pmgr_child = true,
	.deassert = t6000_cio_deassert,
};

static int apple_cio_reset_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct apple_cio_reset *priv =
		container_of(rcdev, struct apple_cio_reset, rcdev);

	return priv->variant->deassert(priv, id);
}

static const struct reset_control_ops apple_cio_reset_ops = {
	.deassert = apple_cio_reset_deassert,
};

static const struct regmap_config apple_cio_reset_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
};

static int apple_cio_reset_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_cio_reset *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->variant = of_device_get_match_data(dev);
	priv->dev = dev;

	ret = devm_mutex_init(dev, &priv->lock);
	if (ret)
		return ret;

	if (priv->variant->pmgr_child) {
		priv->regmap = syscon_node_to_regmap(dev->of_node->parent);
		if (IS_ERR(priv->regmap))
			return dev_err_probe(dev, PTR_ERR(priv->regmap),
					     "Failed to get parent regmap");

		ret = of_property_read_u32(dev->of_node, "reg", &priv->offset);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to read reg offset");
	} else {
		void __iomem *base;

		base = devm_platform_ioremap_resource(pdev, 0);
		if (IS_ERR(base))
			return PTR_ERR(base);

		priv->regmap = devm_regmap_init_mmio(dev, base,
						     &apple_cio_reset_regmap_config);
		if (IS_ERR(priv->regmap))
			return dev_err_probe(dev, PTR_ERR(priv->regmap),
					     "Failed to init MMIO regmap");
	}

	priv->rcdev.owner = THIS_MODULE;
	priv->rcdev.ops = &apple_cio_reset_ops;
	priv->rcdev.of_node = dev->of_node;
	priv->rcdev.nr_resets = priv->variant->nr_resets;

	return devm_reset_controller_register(dev, &priv->rcdev);
}

static const struct of_device_id apple_cio_reset_match[] = {
	{
		.compatible = "apple,t8103-cio-reset",
		.data = &apple_t8103_cio_reset,
	},
	{
		.compatible = "apple,t6000-cio-reset",
		.data = &apple_t6000_cio_reset,
	},
	{},
};
MODULE_DEVICE_TABLE(of, apple_cio_reset_match);

static struct platform_driver apple_cio_reset_driver = {
	.driver = {
		.name = "apple-cio-reset",
		.of_match_table = apple_cio_reset_match,
	},
	.probe = apple_cio_reset_probe,
};
module_platform_driver(apple_cio_reset_driver);

MODULE_AUTHOR("Sven Peter <sven@kernel.org>");
MODULE_DESCRIPTION("Apple SoC CIO block reset driver");
MODULE_LICENSE("Dual MIT/GPL");
