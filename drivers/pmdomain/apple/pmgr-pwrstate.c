// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC PMGR device power state driver
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/bitops.h>
#include <linux/bitfield.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/reset-controller.h>
#include <linux/module.h>

#define APPLE_PMGR_RESET        BIT(31)
#define APPLE_PMGR_AUTO_ENABLE  BIT(28)
#define APPLE_PMGR_PS_AUTO      GENMASK(27, 24)
#define APPLE_PMGR_PS_MIN       GENMASK(19, 16)
#define APPLE_PMGR_PS_RESET     BIT(12)
#define APPLE_PMGR_BUSY         BIT(11)
#define APPLE_PMGR_DEV_DISABLE  BIT(10)
#define APPLE_PMGR_WAS_CLKGATED BIT(9)
#define APPLE_PMGR_WAS_PWRGATED BIT(8)
#define APPLE_PMGR_PS_ACTUAL    GENMASK(7, 4)
#define APPLE_PMGR_PS_TARGET    GENMASK(3, 0)

#define APPLE_PMGR_FLAGS        (APPLE_PMGR_WAS_CLKGATED | APPLE_PMGR_WAS_PWRGATED)

#define APPLE_PMGR_PS_ACTIVE    0xf
#define APPLE_PMGR_PS_CLKGATE   0x4
#define APPLE_PMGR_PS_PWRGATE   0x0

#define APPLE_PMGR_PS_SET_TIMEOUT 100
#define APPLE_PMGR_RESET_TIME 1
#define APPLE_PMGR_T8132_RESET_TIME 10

struct apple_pmgr_ps {
	struct device *dev;
	struct generic_pm_domain genpd;
	struct reset_controller_dev rcdev;
	struct regmap *regmap;
	u32 offset;
	u32 min_state;
	bool force_disable;
	bool force_reset;
	bool externally_clocked;
	bool system_sleep;
	bool firmware_owned;
	/* Never enable automatic power management (apple,no-auto-pm). */
	bool no_auto_pm;
	bool inherited_on;
	bool no_auto_enable;
	bool needs_reset_idle_wait;
	bool strict_state;
	u32 reset_time_us;
};

#define genpd_to_apple_pmgr_ps(_genpd) container_of(_genpd, struct apple_pmgr_ps, genpd)
#define rcdev_to_apple_pmgr_ps(_rcdev) container_of(_rcdev, struct apple_pmgr_ps, rcdev)

static int apple_pmgr_ps_set(struct generic_pm_domain *genpd, u32 pstate, bool auto_enable)
{
	int ret;
	struct apple_pmgr_ps *ps = genpd_to_apple_pmgr_ps(genpd);
	u32 reg, cur;
	u32 timeout = ps->strict_state ? 10000 : APPLE_PMGR_PS_SET_TIMEOUT;

	ret = regmap_read(ps->regmap, ps->offset, &reg);
	if (ret < 0)
		return ret;

	/* Resets are synchronous, and only work if the device is powered and clocked. */
	if (reg & APPLE_PMGR_RESET && pstate != APPLE_PMGR_PS_ACTIVE)
		dev_err_ratelimited(ps->dev, "PS %s: powering off with RESET active\n",
				    genpd->name);

	if (pstate != APPLE_PMGR_PS_ACTIVE && (ps->force_disable || ps->force_reset)) {
		u32 reg_pre = reg & ~(APPLE_PMGR_AUTO_ENABLE | APPLE_PMGR_FLAGS |
				      APPLE_PMGR_BUSY);

		if (ps->force_disable)
			reg_pre |= APPLE_PMGR_DEV_DISABLE;
		if (ps->force_reset)
			reg_pre |= APPLE_PMGR_PS_RESET;

		regmap_write(ps->regmap, ps->offset, reg_pre);

		ret = regmap_read_poll_timeout_atomic(
			ps->regmap, ps->offset, cur,
			(cur & (APPLE_PMGR_DEV_DISABLE | APPLE_PMGR_PS_RESET)) ==
			(reg_pre & (APPLE_PMGR_DEV_DISABLE | APPLE_PMGR_PS_RESET)), 1,
			APPLE_PMGR_PS_SET_TIMEOUT);

		if (ret < 0)
			dev_err_ratelimited(ps->dev,
					    "PS %s: Failed to set reset/disable bits (now: 0x%x)\n",
					    genpd->name, cur);
	}

	/* Do not replay the hardware-owned BUSY bit in this whole-word write. */
	reg &= ~(APPLE_PMGR_DEV_DISABLE | APPLE_PMGR_PS_RESET |
		 APPLE_PMGR_AUTO_ENABLE | APPLE_PMGR_FLAGS | APPLE_PMGR_BUSY |
		 APPLE_PMGR_PS_TARGET);
	reg |= FIELD_PREP(APPLE_PMGR_PS_TARGET, pstate);

	dev_dbg(ps->dev, "PS %s: pwrstate = 0x%x: 0x%x\n", genpd->name, pstate, reg);

	ret = regmap_write(ps->regmap, ps->offset, reg);
	if (ret && ps->strict_state)
		return ret;
	cur = reg;

	if (ps->externally_clocked && pstate == APPLE_PMGR_PS_ACTIVE) {
		/*
		 * If this clock domain requires an external clock, then
		 * consider the "clock gated" state to be good enough.
		 */
		ret = regmap_read_poll_timeout_atomic(
			ps->regmap, ps->offset, cur,
			FIELD_GET(APPLE_PMGR_PS_ACTUAL, cur) >= APPLE_PMGR_PS_CLKGATE, 1,
			APPLE_PMGR_PS_SET_TIMEOUT);
	} else {
		ret = regmap_read_poll_timeout_atomic(
			ps->regmap, ps->offset, cur,
			FIELD_GET(APPLE_PMGR_PS_ACTUAL, cur) == pstate, 1,
			timeout);
	}
	if (ps->strict_state) {
		dev_info(ps->dev, "GFX target=%#x written=%#x actual=%#x error=%d\n",
			 pstate, reg, cur, ret);
		if (ret)
			return ret;
	}

	if (ret < 0)
		dev_err_ratelimited(ps->dev,
				    "PS %s: Failed to reach power state 0x%x (requested 0x%x, now: 0x%x)\n",
				    genpd->name, pstate, reg, cur);

	if (auto_enable) {
		/* Not all devices implement this; this is a no-op where not implemented. */
		reg |= APPLE_PMGR_AUTO_ENABLE;
		if (ps->strict_state)
			return regmap_write(ps->regmap, ps->offset, reg);
		regmap_write(ps->regmap, ps->offset, reg);
	}

	return ret;
}

static int apple_pmgr_ps_is_active(struct apple_pmgr_ps *ps)
{
	u32 reg = 0;
	int ret;

	ret = regmap_read(ps->regmap, ps->offset, &reg);
	if (ret && ps->needs_reset_idle_wait)
		return ret;
	/* The J713 GFX admission needs an actual-state ACK, not target/auto. */
	if (ps->strict_state)
		return FIELD_GET(APPLE_PMGR_PS_ACTUAL, reg) == APPLE_PMGR_PS_ACTIVE;
	/*
	 * We consider domains as active if they are actually on, or if they have auto-PM
	 * enabled and the intended target is on.
	 */
	return (FIELD_GET(APPLE_PMGR_PS_ACTUAL, reg) == APPLE_PMGR_PS_ACTIVE ||
		(FIELD_GET(APPLE_PMGR_PS_TARGET, reg) == APPLE_PMGR_PS_ACTIVE &&
		 reg & APPLE_PMGR_AUTO_ENABLE));
}

/* Verify a retained domain without requesting any hardware transition. */
static int apple_pmgr_ps_check_inherited(struct apple_pmgr_ps *ps)
{
	u32 reg;
	int ret;

	ret = regmap_read(ps->regmap, ps->offset, &reg);
	if (ret)
		return ret;
	if (FIELD_GET(APPLE_PMGR_PS_TARGET, reg) != APPLE_PMGR_PS_ACTIVE ||
	    FIELD_GET(APPLE_PMGR_PS_ACTUAL, reg) != APPLE_PMGR_PS_ACTIVE ||
	    (reg & (APPLE_PMGR_RESET | APPLE_PMGR_AUTO_ENABLE | APPLE_PMGR_PS_RESET |
		    APPLE_PMGR_BUSY | APPLE_PMGR_DEV_DISABLE)))
		return -EBUSY;

	return 0;
}

/*
 * genpd keeps a domain off when ->power_on() fails and on when ->power_off()
 * fails. Follow a failed transition with the opposite one, so the domain is
 * not left requesting a state genpd does not know it is in.
 *
 * The exception is a failed power-on in the noirq and syscore phases of a
 * system sleep transition: genpd ignores the error there and records the
 * domain as on. The PM core runs those phases after the suspend_late callback
 * of every device and before any resume_early callback, so keep requesting
 * the domain on between this device's two callbacks.
 */
static int apple_pmgr_ps_power_on(struct generic_pm_domain *genpd)
{
	struct apple_pmgr_ps *ps = genpd_to_apple_pmgr_ps(genpd);
	int ret;

	/* Its coprocessor's firmware owns the state; Linux only holds a vote. */
	if (ps->firmware_owned)
		return 0;
	if (ps->inherited_on)
		return apple_pmgr_ps_check_inherited(ps);

	ret = apple_pmgr_ps_set(genpd, APPLE_PMGR_PS_ACTIVE,
				!ps->no_auto_enable && !ps->no_auto_pm);
	if (ret && !ps->system_sleep)
		apple_pmgr_ps_set(genpd, APPLE_PMGR_PS_PWRGATE, false);

	return ret;
}

static int apple_pmgr_ps_power_off(struct generic_pm_domain *genpd)
{
	struct apple_pmgr_ps *ps = genpd_to_apple_pmgr_ps(genpd);
	int ret;

	if (ps->firmware_owned)
		return 0;
	if (ps->inherited_on)
		return -EBUSY;

	ret = apple_pmgr_ps_set(genpd, APPLE_PMGR_PS_PWRGATE, false);
	if (ret)
		apple_pmgr_ps_set(genpd, APPLE_PMGR_PS_ACTIVE,
				 !ps->no_auto_enable && !ps->no_auto_pm);

	return ret;
}

static void apple_pmgr_ps_set_system_sleep(struct device *dev, bool sleep)
{
	struct apple_pmgr_ps *ps = dev_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&ps->genpd.slock, flags);
	ps->system_sleep = sleep;
	spin_unlock_irqrestore(&ps->genpd.slock, flags);
}

static int apple_pmgr_ps_suspend_late(struct device *dev)
{
	apple_pmgr_ps_set_system_sleep(dev, true);

	return 0;
}

static int apple_pmgr_ps_resume_early(struct device *dev)
{
	apple_pmgr_ps_set_system_sleep(dev, false);

	return 0;
}

static int apple_pmgr_reset_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct apple_pmgr_ps *ps = rcdev_to_apple_pmgr_ps(rcdev);
	unsigned long flags;
	int ret;
	u32 reg;

	if (ps->firmware_owned || ps->inherited_on)
		return -EPERM;

	spin_lock_irqsave(&ps->genpd.slock, flags);

	if (ps->genpd.status == GENPD_STATE_OFF)
		dev_err_ratelimited(ps->dev,
				    "PS 0x%x: asserting RESET while powered down\n",
				    ps->offset);

	dev_dbg(ps->dev, "PS 0x%x: assert reset\n", ps->offset);
	/* Quiesce device before asserting reset */
	regmap_update_bits(ps->regmap, ps->offset, APPLE_PMGR_FLAGS | APPLE_PMGR_DEV_DISABLE,
			   APPLE_PMGR_DEV_DISABLE);
	if (ps->needs_reset_idle_wait) {
		ret = regmap_read_poll_timeout_atomic(ps->regmap, ps->offset,
						      reg, !(reg & APPLE_PMGR_BUSY),
						      1, APPLE_PMGR_PS_SET_TIMEOUT);
		if (ret) {
			dev_err(ps->dev, "reset quiesce timed out offset=%#x reg=%#x\n",
				ps->offset, reg);
			spin_unlock_irqrestore(&ps->genpd.slock, flags);
			return ret;
		}
	}
	regmap_update_bits(ps->regmap, ps->offset, APPLE_PMGR_FLAGS | APPLE_PMGR_RESET,
			   APPLE_PMGR_RESET);

	spin_unlock_irqrestore(&ps->genpd.slock, flags);

	return 0;
}

static int apple_pmgr_reset_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct apple_pmgr_ps *ps = rcdev_to_apple_pmgr_ps(rcdev);
	unsigned long flags;

	if (ps->firmware_owned || ps->inherited_on)
		return -EPERM;

	if (ps->reset_time_us == APPLE_PMGR_T8132_RESET_TIME)
		usleep_range(ps->reset_time_us, 2 * ps->reset_time_us);

	spin_lock_irqsave(&ps->genpd.slock, flags);

	dev_dbg(ps->dev, "PS 0x%x: deassert reset\n", ps->offset);
	regmap_update_bits(ps->regmap, ps->offset, APPLE_PMGR_FLAGS | APPLE_PMGR_RESET, 0);
	regmap_update_bits(ps->regmap, ps->offset, APPLE_PMGR_FLAGS | APPLE_PMGR_DEV_DISABLE, 0);

	if (ps->genpd.status == GENPD_STATE_OFF)
		dev_err_ratelimited(ps->dev,
				    "PS 0x%x: RESET was deasserted while powered down\n",
				    ps->offset);

	spin_unlock_irqrestore(&ps->genpd.slock, flags);

	return 0;
}

static int apple_pmgr_reset_reset(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct apple_pmgr_ps *ps = rcdev_to_apple_pmgr_ps(rcdev);
	int ret;

	ret = apple_pmgr_reset_assert(rcdev, id);
	if (ret)
		return ret;

	if (ps->reset_time_us != APPLE_PMGR_T8132_RESET_TIME)
		usleep_range(APPLE_PMGR_RESET_TIME, 2 * APPLE_PMGR_RESET_TIME);

	return apple_pmgr_reset_deassert(rcdev, id);
}

static int apple_pmgr_reset_status(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct apple_pmgr_ps *ps = rcdev_to_apple_pmgr_ps(rcdev);
	u32 reg = 0;

	if (ps->firmware_owned || ps->inherited_on)
		return -EPERM;
	regmap_read(ps->regmap, ps->offset, &reg);

	return !!(reg & APPLE_PMGR_RESET);
}

static const struct reset_control_ops apple_pmgr_reset_ops = {
	.assert		= apple_pmgr_reset_assert,
	.deassert	= apple_pmgr_reset_deassert,
	.reset		= apple_pmgr_reset_reset,
	.status		= apple_pmgr_reset_status,
};

static int apple_pmgr_reset_xlate(struct reset_controller_dev *rcdev,
				  const struct of_phandle_args *reset_spec)
{
	return 0;
}

static int apple_pmgr_ps_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct apple_pmgr_ps *ps;
	struct regmap *regmap;
	struct of_phandle_iterator it;
	int ret;
	const char *name;
	bool active, t8132, strict_state = false;
	struct device_node *tvm;

	t8132 = of_device_is_compatible(node, "apple,t8132-pmgr-pwrstate");
	/*
	 * The original J713 loader tree exposes domains Linux cannot access.
	 * The early compatibility adapter supplies the board's explicit status
	 * and ownership before population; fail closed if it was not enabled.
	 * Other T8132 boards keep their own domain descriptions and behaviour.
	 */
	if (t8132 && of_machine_is_compatible("apple,j713") &&
	    !of_property_present(node, "status"))
		return dev_err_probe(dev, -ENODEV, "missing J713 power-domain policy\n");

	tvm = of_parse_phandle(node, "power-domains", 0);
	if (t8132 && tvm)
		strict_state = of_device_is_compatible(tvm, "apple,t8132-gfx-tvm");
	of_node_put(tvm);

	regmap = syscon_node_to_regmap(node->parent);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ps = devm_kzalloc(dev, sizeof(*ps), GFP_KERNEL);
	if (!ps)
		return -ENOMEM;

	ps->dev = dev;
	ps->regmap = regmap;
	platform_set_drvdata(pdev, ps);
	ps->reset_time_us = t8132 ? APPLE_PMGR_T8132_RESET_TIME : APPLE_PMGR_RESET_TIME;
	ps->needs_reset_idle_wait = t8132;
	ps->no_auto_enable = t8132 && !strict_state;
	ps->strict_state = strict_state;

	ret = of_property_read_string(node, "label", &name);
	if (ret < 0) {
		dev_err(dev, "missing label property\n");
		return ret;
	}

	ret = of_property_read_u32(node, "reg", &ps->offset);
	if (ret < 0) {
		dev_err(dev, "missing reg property\n");
		return ret;
	}

	ps->genpd.flags |= GENPD_FLAG_IRQ_SAFE;
	ps->genpd.name = name;
	ps->genpd.power_on = apple_pmgr_ps_power_on;
	ps->genpd.power_off = apple_pmgr_ps_power_off;

	/*
	 * Firmware retains control of this domain across the handoff. Its state
	 * register may not be accessible to the CPU, so represent only an always-on
	 * vote and never read, reset or change the hardware state.
	 */
	ps->firmware_owned = of_property_read_bool(node, "apple,firmware-owned");
	/*
	 * Only T8132 hands over inherited-on domains with this contract. Older
	 * M3 device trees carry the property with another meaning; keep them
	 * as they were.
	 */
	ps->inherited_on = t8132 && of_property_read_bool(node, "apple,inherited-on");
	if (ps->inherited_on &&
	    (ps->firmware_owned || of_property_present(node, "apple,min-state") ||
	     of_property_present(node, "apple,always-on") ||
	     of_property_present(node, "apple,force-disable") ||
	     of_property_present(node, "apple,force-reset") ||
	     of_property_present(node, "apple,externally-clocked")))
		return dev_err_probe(dev, -EINVAL, "conflicting inherited power policy\n");

	ret = of_property_read_u32(node, "apple,min-state", &ps->min_state);
	if (ret == 0 && ps->min_state <= APPLE_PMGR_PS_ACTIVE && !ps->firmware_owned)
		regmap_update_bits(regmap, ps->offset, APPLE_PMGR_FLAGS | APPLE_PMGR_PS_MIN,
				   FIELD_PREP(APPLE_PMGR_PS_MIN, ps->min_state));

	if (of_property_read_bool(node, "apple,force-disable"))
		ps->force_disable = true;

	if (of_property_read_bool(node, "apple,force-reset"))
		ps->force_reset = true;

	if (of_property_read_bool(node, "apple,externally-clocked"))
		ps->externally_clocked = true;

	/* A cold-started coprocessor CPU must not be gated behind its firmware. */
	ps->no_auto_pm = of_property_read_bool(node, "apple,no-auto-pm");

	/*
	 * An inherited-on domain has readable state but no transition ownership.
	 * A firmware-owned domain need not have a CPU-accessible state register;
	 * its availability is supplied by the handoff rather than an MMIO probe.
	 */
	if (ps->inherited_on) {
		ret = apple_pmgr_ps_check_inherited(ps);
		if (ret)
			return dev_err_probe(dev, ret, "domain was not handed over active\n");
		ps->genpd.flags |= GENPD_FLAG_ALWAYS_ON;
		active = true;
	} else if (ps->firmware_owned) {
		ps->genpd.flags |= GENPD_FLAG_ALWAYS_ON;
		active = true;
	} else {
		ret = apple_pmgr_ps_is_active(ps);
		if (ret < 0)
			return dev_err_probe(dev, ret, "cannot read power state\n");
		active = ret;
	}
	if (!ps->firmware_owned && !ps->inherited_on &&
	    of_property_read_bool(node, "apple,always-on")) {
		ps->genpd.flags |= GENPD_FLAG_ALWAYS_ON;
		if (!active) {
			/*
			 * The boot loader need not leave an always-on domain on,
			 * so this is not an error: apple_pmgr_ps_set() reports a
			 * domain that cannot be turned on.
			 */
			dev_info(dev, "always-on domain %s is off at boot, turning it on\n",
				 name);
			/*
			 * Turn it on so pm_genpd_init does not fail. genpd never
			 * powers an always-on domain, so keep requesting it on
			 * even if it does not get there in time.
			 */
			active = apple_pmgr_ps_set(&ps->genpd, APPLE_PMGR_PS_ACTIVE,
						   !ps->no_auto_enable && !ps->no_auto_pm) == 0;
		}
	} else if (active && !ps->firmware_owned && !ps->inherited_on) {
		ps->genpd.flags |= GENPD_FLAG_DEFER_OFF | GENPD_FLAG_ACTIVE_WAKEUP;
	}

	/* Turn on auto-PM if the domain is already on, unless it must stay off */
	if (active && !ps->firmware_owned && !ps->inherited_on && !ps->no_auto_enable)
		regmap_update_bits(regmap, ps->offset, APPLE_PMGR_FLAGS | APPLE_PMGR_AUTO_ENABLE,
				   ps->no_auto_pm ? 0 : APPLE_PMGR_AUTO_ENABLE);

	ret = pm_genpd_init(&ps->genpd, NULL, !active);
	if (ret < 0) {
		dev_err(dev, "pm_genpd_init failed\n");
		return ret;
	}

	ret = of_genpd_add_provider_simple(node, &ps->genpd);
	if (ret < 0) {
		dev_err(dev, "of_genpd_add_provider_simple failed\n");
		return ret;
	}

	of_for_each_phandle(&it, ret, node, "power-domains", "#power-domain-cells", -1) {
		struct of_phandle_args parent, child;

		parent.np = it.node;
		parent.args_count = of_phandle_iterator_args(&it, parent.args, MAX_PHANDLE_ARGS);
		child.np = node;
		child.args_count = 0;
		ret = of_genpd_add_subdomain(&parent, &child);

		if (ret == -EPROBE_DEFER) {
			of_node_put(parent.np);
			goto err_remove;
		} else if (ret < 0) {
			dev_err(dev, "failed to add to parent domain: %d (%s -> %s)\n",
				ret, it.node->name, node->name);
			of_node_put(parent.np);
			goto err_remove;
		}
	}

	/*
	 * Do not participate in regular PM; parent power domains are handled via the
	 * genpd hierarchy.
	 */
	pm_genpd_remove_device(dev);

	ps->rcdev.owner = THIS_MODULE;
	ps->rcdev.nr_resets = 1;
	ps->rcdev.ops = &apple_pmgr_reset_ops;
	ps->rcdev.of_node = dev->of_node;
	ps->rcdev.of_reset_n_cells = 0;
	ps->rcdev.of_xlate = apple_pmgr_reset_xlate;

	ret = devm_reset_controller_register(dev, &ps->rcdev);
	if (ret < 0)
		goto err_remove;

	return 0;
err_remove:
	of_genpd_del_provider(node);
	pm_genpd_remove(&ps->genpd);
	return ret;
}

static const struct of_device_id apple_pmgr_ps_of_match[] = {
	{ .compatible = "apple,t8103-pmgr-pwrstate" },
	{ .compatible = "apple,pmgr-pwrstate" },
	{}
};

MODULE_DEVICE_TABLE(of, apple_pmgr_ps_of_match);

static const struct dev_pm_ops apple_pmgr_ps_pm_ops = {
	LATE_SYSTEM_SLEEP_PM_OPS(apple_pmgr_ps_suspend_late, apple_pmgr_ps_resume_early)
};

static struct platform_driver apple_pmgr_ps_driver = {
	.probe = apple_pmgr_ps_probe,
	.driver = {
		.name = "apple-pmgr-pwrstate",
		.of_match_table = apple_pmgr_ps_of_match,
		.pm = pm_sleep_ptr(&apple_pmgr_ps_pm_ops),
	},
};

MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
MODULE_DESCRIPTION("PMGR power state driver for Apple SoCs");

module_platform_driver(apple_pmgr_ps_driver);

#if IS_ENABLED(CONFIG_APPLE_PMGR_PWRSTATE_KUNIT_TEST)
#include "pmgr-pwrstate-test.c"
#endif
