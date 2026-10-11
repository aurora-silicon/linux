// SPDX-License-Identifier: GPL-2.0-only
/* T6050 fixed four-lane DisplayPort PHY (HDMI bridge path). */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/soc/apple/tunable.h>
#include <linux/soc/apple/dp-tunnel.h>

enum atc_operation { ATC_MASK, ATC_TUNABLE, ATC_DELAY, ATC_POLL, ATC_POWER };
struct atc_op {
	u8 op, bank;
	u16 offset;
	u32 mask, value, timeout;
};
struct atc_rate_op {
	struct atc_op op;
	u8 rates;
	u32 values[4];
};
#include "atc-t6050-dp-data.h"

static const struct { u8 bank; u32 offset; } core_banks[] = {
	{2, 0}, {3, 0x400}, {4, 0x800}, {5, 0xa00}, {6, 0x1000},
	{7, 0x3000}, {8, 0x3200}, {9, 0x3800}, {10, 0x3a00},
	{14, 0x7000}, {16, 0x9000}, {18, 0xb000}, {19, 0xc000},
	{20, 0xd000}, {21, 0xe000}, {22, 0xf000}, {25, 0x19000},
	{27, 0x1b000}, {28, 0x1c000}, {29, 0x1d000}, {30, 0x1e000},
	{31, 0x1f000}, {33, 0x28000}, {34, 0x28400},
};
static const struct { u8 bank; const char *name; } extra_banks[] = {
	{39, "reset"}, {40, "fabric"}, {41, "axi2af"},
	{42, "lioa"}, {43, "common-cfg"},
};
static const struct { u8 bank; const char *name; } tunable_names[] = {
	{41, "apple,tunable-axi2af"}, {40, "apple,tunable-fabric"},
	{43, "apple,tunable-common-cfg"}, {42, "apple,tunable-lioa"},
	{20, "apple,tunable-lane0-rx-clock"}, {29, "apple,tunable-lane1-rx-clock"},
	{18, "apple,tunable-lane0-rx-eq"}, {27, "apple,tunable-lane1-rx-eq"},
	{8, "apple,tunable-auspll"},
};
static const unsigned int link_rates[] = {1620, 2700, 5400, 8100};
static const char * const training_names[] = {
	"apple,dp-training-table-rbr", "apple,dp-training-table-hbr",
	"apple,dp-training-table-hbr2", "apple,dp-training-table-hbr3",
};
struct atc_domain {
	struct device *dev;
	bool active;
};
struct t6050_dp {
	struct device *dev;
	struct mutex lock;
	void __iomem *banks[44];
	struct resource resources[44];
	struct apple_tunable *tunables[ARRAY_SIZE(tunable_names)];
	struct atc_domain domain[3]; /* USB AON, common, wrapper: native order. */
	u32 training[4][16];
	u32 voltage[4], pre[4];
	int rate;
	bool enabled, faulted;
};

static int atc_domain_set(struct atc_domain *pd, bool active)
{
	int ret;

	if (pd->active == active)
		return 0;
	if (active) {
		ret = pm_runtime_resume_and_get(pd->dev);
		if (ret < 0)
			return ret;
	} else {
		ret = pm_runtime_put_sync_suspend(pd->dev);
		if (ret < 0) {
			/* put decremented usage even when suspend failed. */
			pm_runtime_get_noresume(pd->dev);
			return ret;
		}
	}
	pd->active = active;
	return 0;
}

static void atc_domain_release(void *data)
{
	struct atc_domain *pd = data;

	if (pd->active)
		pm_runtime_put_sync_suspend(pd->dev);
	dev_pm_domain_detach(pd->dev, true);
}

static int atc_validate_step(struct t6050_dp *dp, const struct atc_op *s)
{
	if (s->op == ATC_DELAY)
		return s->value <= 1000 ? 0 : -EINVAL;
	if (s->op == ATC_POWER)
		return s->value <= 1 ? 0 : -EINVAL;
	if (s->bank >= ARRAY_SIZE(dp->banks) || !dp->banks[s->bank] ||
	    s->offset % 4 || s->offset + 4 > resource_size(&dp->resources[s->bank]))
		return -EINVAL;
	if (s->op == ATC_TUNABLE)
		return s->value < ARRAY_SIZE(tunable_names) &&
		       tunable_names[s->value].bank == s->bank ? 0 : -EINVAL;
	if (s->op == ATC_MASK || s->op == ATC_POLL)
		return s->value & ~s->mask ? -EINVAL : 0;
	return -EINVAL;
}

static int atc_step(struct t6050_dp *dp, const struct atc_op *s)
{
	void __iomem *reg;
	u32 value;
	int ret;

	if (s->op == ATC_DELAY) {
		udelay(s->value);
		return 0;
	}
	if (s->op == ATC_POWER)
		return atc_domain_set(&dp->domain[2], s->value);
	if (s->bank >= ARRAY_SIZE(dp->banks) || !dp->banks[s->bank] ||
	    s->offset + 4 > resource_size(&dp->resources[s->bank]))
		return -EINVAL;
	reg = dp->banks[s->bank] + s->offset;
	switch (s->op) {
	case ATC_MASK:
		value = readl(reg);
		writel((value & ~s->mask) | s->value, reg);
		return 0;
	case ATC_TUNABLE:
		if (s->value >= ARRAY_SIZE(dp->tunables))
			return -EINVAL;
		apple_tunable_apply(dp->banks[s->bank], dp->tunables[s->value]);
		return 0;
	case ATC_POLL:
		ret = readl_poll_timeout(reg, value, (value & s->mask) == s->value,
					s->timeout > 1000 ? 1000 : 1, s->timeout);
		if (ret)
			dev_err(dp->dev, "bank%u+%x timeout: %x & %x != %x\n",
				s->bank, s->offset, value, s->mask, s->value);
		return ret;
	default:
		return -EINVAL;
	}
}

static int atc_sequence(struct t6050_dp *dp, const struct atc_op *s, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		int ret = atc_step(dp, &s[i]);

		if (ret) {
			dp->faulted = true;
			return ret;
		}
	}
	return 0;
}
#define ATC_SEQUENCE(dp, seq) atc_sequence(dp, seq, ARRAY_SIZE(seq))

static int atc_link_rate(struct t6050_dp *dp, int rate)
{
	int ret;

	if (dp->rate == rate)
		return 0;
	if (dp->rate >= 0) {
		ret = ATC_SEQUENCE(dp, dp_link_off);
		if (ret)
			return ret;
		dp->rate = -1;
	}
	if (rate < 0)
		return 0;
	for (size_t i = 0; i < ARRAY_SIZE(dp_link_on); i++) {
		struct atc_op op = dp_link_on[i].op;

		if (!(dp_link_on[i].rates & BIT(rate)))
			continue;
		op.value = dp_link_on[i].values[rate];
		ret = atc_step(dp, &op);
		if (ret) {
			dp->faulted = true;
			return ret;
		}
	}
	dp->rate = rate;
	memset(dp->voltage, 0, sizeof(dp->voltage));
	memset(dp->pre, 0, sizeof(dp->pre));
	return 0;
}

static int atc_start(struct t6050_dp *dp)
{
	int ret;

	if (dp->faulted)
		return -EIO;
	if (dp->enabled)
		return 0;
	ret = atc_domain_set(&dp->domain[0], true);
	if (ret)
		return ret;
	ret = atc_domain_set(&dp->domain[1], true);
	if (ret) {
		atc_domain_set(&dp->domain[0], false);
		return ret;
	}
	ret = ATC_SEQUENCE(dp, dp_init);
	if (!ret)
		ret = ATC_SEQUENCE(dp, aux_on);
	if (ret)
		return ret;
	dp->enabled = true;
	return 0;
}

static int atc_stop(struct t6050_dp *dp)
{
	int ret;

	if (dp->faulted)
		return -EIO;
	if (!dp->enabled)
		return 0;
	ret = atc_link_rate(dp, -1);
	if (!ret)
		ret = ATC_SEQUENCE(dp, aux_off);
	if (!ret)
		ret = ATC_SEQUENCE(dp, dp_poweroff);
	if (ret)
		return ret;
	dp->enabled = false;
	ret = atc_domain_set(&dp->domain[1], false);
	if (!ret)
		ret = atc_domain_set(&dp->domain[0], false);
	return ret;
}

static void atc_mask(void __iomem *reg, u32 mask, u32 value)
{
	writel((readl(reg) & ~mask) | value, reg);
}

static void atc_drive(struct t6050_dp *dp, unsigned int lane, u32 preset)
{
	/* Fixed normal DP orientation: group1 RX/TX, then group0 RX/TX. */
	unsigned int group = lane < 2 ? 1 : 0;
	bool tx = lane & 1;
	void __iomem *base = dp->banks[(tx ? 21 : 20) + 9 * group];
	u32 ctrl = ((preset >> 2) & 0xf) | ((preset & 3) << 14);
	u32 eq = ((preset >> 5) & 0x7fffe) | 1;

	if (tx)
		atc_mask(base + 0x100, BIT(20), BIT(20));
	atc_mask(base + (tx ? 0x58 : 0x68), 0xc00f, ctrl);
	atc_mask(base + (tx ? 0x104 : 0xac), BIT(29), BIT(29));
	atc_mask(base + (tx ? 0x100 : 0xa8), 0x7ffff, eq);
}

static int t6050_dp_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct t6050_dp *dp = phy_get_drvdata(phy);

	guard(mutex)(&dp->lock);
	if (mode == PHY_MODE_DP)
		return atc_start(dp);
	if (mode == PHY_MODE_INVALID)
		return atc_stop(dp);
	return -EINVAL;
}

static int t6050_dp_validate(struct phy *phy, enum phy_mode mode, int submode,
			    union phy_configure_opts *opts)
{
	if (mode != PHY_MODE_DP)
		return -EINVAL;
	memset(opts, 0, sizeof(*opts));
	opts->dp.lanes = 4;
	opts->dp.link_rate = 8100;
	opts->dp.ssc = 0;
	return 0;
}

static int t6050_dp_configure(struct phy *phy, union phy_configure_opts *opts)
{
	struct t6050_dp *dp = phy_get_drvdata(phy);
	struct phy_configure_opts_dp *cfg = &opts->dp;
	int rate = -1, ret;

	/* Validate the whole request before altering a running transmitter. */
	if (cfg->set_rate && cfg->link_rate) {
		for (unsigned int i = 0; i < ARRAY_SIZE(link_rates); i++)
			if (cfg->link_rate == link_rates[i])
				rate = i;
		if (rate < 0 || cfg->ssc)
			return -EINVAL;
	}
	if ((cfg->set_lanes || cfg->set_voltages) &&
	    cfg->lanes != 0 && cfg->lanes != 1 && cfg->lanes != 2 && cfg->lanes != 4)
		return -EINVAL;
	if (cfg->set_voltages)
		for (unsigned int i = 0; i < cfg->lanes; i++)
			if (cfg->voltage[i] > 3 || cfg->pre[i] > 3 ||
			    cfg->voltage[i] + cfg->pre[i] > 3)
				return -EINVAL;
	if (cfg->set_voltages && cfg->set_rate && rate < 0 && cfg->lanes)
		return -EINVAL;
	guard(mutex)(&dp->lock);
	if (dp->faulted)
		return -EIO;
	if (!dp->enabled)
		return -EPIPE;
	if (cfg->set_rate) {
		ret = atc_link_rate(dp, rate);
		if (ret)
			return ret;
	}
	/* The display controller owns the active lane count. */
	if (cfg->set_voltages) {
		if (dp->rate < 0)
			return -EPIPE;
		for (unsigned int i = 0; i < cfg->lanes; i++) {
			u32 preset = dp->training[dp->rate][4 * cfg->voltage[i] + cfg->pre[i]];

			atc_drive(dp, i, preset);
			dp->voltage[i] = cfg->voltage[i];
			dp->pre[i] = cfg->pre[i];
		}
	}
	return 0;
}

static const struct phy_ops t6050_dp_ops = {
	.set_mode = t6050_dp_set_mode,
	.validate = t6050_dp_validate,
	.configure = t6050_dp_configure,
	.owner = THIS_MODULE,
};

int apple_dp_phy_link_config(struct phy *phy, unsigned int *pclk, bool *uhbr)
{
	struct t6050_dp *dp;

	if (!phy || phy->ops != &t6050_dp_ops)
		return -EOPNOTSUPP;
	dp = phy_get_drvdata(phy);
	guard(mutex)(&dp->lock);
	if (dp->faulted)
		return -EIO;
	if (!dp->enabled || dp->rate < 0)
		return -ENOLINK;
	/* Native direct DPPHY uses logical clock0, mapped to xbar PCLK1.
	 * The other four logical clocks are separately managed tunnel clocks. */
	*pclk = 1;
	*uhbr = false;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dp_phy_link_config);

static int t6050_dp_probe(struct platform_device *pdev)
{
	static const char * const domains[] = {"usb-aon", "common", "wrapper"};
	struct device *dev = &pdev->dev;
	struct t6050_dp *dp;
	struct phy_provider *provider;
	struct phy *phy;
	struct resource *core;
	void __iomem *base;
	int ret;

	dp = devm_kzalloc(dev, sizeof(*dp), GFP_KERNEL);
	if (!dp)
		return -ENOMEM;
	dp->dev = dev;
	dp->rate = -1;
	mutex_init(&dp->lock);
	platform_set_drvdata(pdev, dp);
	core = platform_get_resource_byname(pdev, IORESOURCE_MEM, "core");
	if (!core || resource_size(core) < 0x2c400)
		return -EINVAL;
	base = devm_ioremap_resource(dev, core);
	if (IS_ERR(base))
		return PTR_ERR(base);
	for (unsigned int i = 0; i < ARRAY_SIZE(core_banks); i++) {
		u8 bank = core_banks[i].bank;

		dp->banks[bank] = base + core_banks[i].offset;
		dp->resources[bank] = *core;
		dp->resources[bank].start += core_banks[i].offset;
		dp->resources[bank].end = dp->resources[bank].start + 0x4000 - 1;
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(extra_banks); i++) {
		u8 bank = extra_banks[i].bank;
		struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
								   extra_banks[i].name);

		if (!res || resource_size(res) < 4)
			return -EINVAL;
		dp->banks[bank] = devm_ioremap_resource(dev, res);
		if (IS_ERR(dp->banks[bank]))
			return PTR_ERR(dp->banks[bank]);
		dp->resources[bank] = *res;
	}
	/* Check every register step before any domain is enabled. */
	{
		static const struct { const struct atc_op *ops; size_t count; } sequences[] = {
			{ dp_init, ARRAY_SIZE(dp_init) },
			{ aux_on, ARRAY_SIZE(aux_on) },
			{ aux_off, ARRAY_SIZE(aux_off) },
			{ dp_link_off, ARRAY_SIZE(dp_link_off) },
			{ dp_poweroff, ARRAY_SIZE(dp_poweroff) },
		};

		for (unsigned int i = 0; i < ARRAY_SIZE(sequences); i++)
			for (size_t j = 0; j < sequences[i].count; j++) {
				ret = atc_validate_step(dp, &sequences[i].ops[j]);
				if (ret)
					return ret;
			}
		for (unsigned int i = 0; i < ARRAY_SIZE(dp_link_on); i++)
			for (unsigned int rate = 0; rate < ARRAY_SIZE(link_rates); rate++) {
				struct atc_op op = dp_link_on[i].op;

				if (!(dp_link_on[i].rates & BIT(rate)))
					continue;
				op.value = dp_link_on[i].values[rate];
				ret = atc_validate_step(dp, &op);
				if (ret)
					return ret;
			}
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(tunable_names); i++) {
		dp->tunables[i] = devm_apple_tunable_parse(dev, dev->of_node,
				 tunable_names[i].name, &dp->resources[tunable_names[i].bank]);
		if (IS_ERR(dp->tunables[i]))
			return dev_err_probe(dev, PTR_ERR(dp->tunables[i]),
					     "missing or invalid %s\n", tunable_names[i].name);
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(training_names); i++) {
		const char *name = training_names[i];

		if (!of_property_present(dev->of_node, name))
			name = "apple,dp-training-table";
		if (!of_property_present(dev->of_node, name)) {
			memcpy(dp->training[i], dp_training_default, sizeof(dp_training_default));
			continue;
		}
		if (of_property_count_u32_elems(dev->of_node, name) != 16)
			return -EINVAL;
		ret = of_property_read_u32_array(dev->of_node, name, dp->training[i], 16);
		if (ret)
			return ret;
	}
	/* Three named domains prevent automatic wrapper enable at probe time. */
	if (of_count_phandle_with_args(dev->of_node, "power-domains", "#power-domain-cells") != 3)
		return -EINVAL;
	for (unsigned int i = 0; i < ARRAY_SIZE(domains); i++) {
		dp->domain[i].dev = dev_pm_domain_attach_by_name(dev, domains[i]);
		if (IS_ERR_OR_NULL(dp->domain[i].dev))
			return dev_err_probe(dev, dp->domain[i].dev ? PTR_ERR(dp->domain[i].dev) : -ENODEV,
					     "cannot attach %s domain\n", domains[i]);
		ret = devm_add_action_or_reset(dev, atc_domain_release, &dp->domain[i]);
		if (ret)
			return ret;
	}
	phy = devm_phy_create(dev, NULL, &t6050_dp_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, dp);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static void t6050_dp_shutdown(struct platform_device *pdev)
{
	struct t6050_dp *dp = platform_get_drvdata(pdev);

	guard(mutex)(&dp->lock);
	if (atc_stop(dp))
		dev_warn(dp->dev, "PHY shutdown incomplete after an earlier error\n");
}

static const struct of_device_id t6050_dp_match[] = {
	{ .compatible = "apple,t6050-dp-phy" },
	{},
};
MODULE_DEVICE_TABLE(of, t6050_dp_match);
static struct platform_driver t6050_dp_driver = {
	.probe = t6050_dp_probe,
	.remove = t6050_dp_shutdown,
	.shutdown = t6050_dp_shutdown,
	.driver = {
		.name = "apple-t6050-dp-phy",
		.of_match_table = t6050_dp_match,
	},
};
module_platform_driver(t6050_dp_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple T6050 fixed DisplayPort PHY");
