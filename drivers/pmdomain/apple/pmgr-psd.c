// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T8152 PMGR2 PSD services as power domains.
 *
 * Each service has a 0x100 byte register block: the target state at +0x90,
 * the acknowledge in bit 0 of +0xbc, and the current state in bits 2:0 of
 * +0x80 (0 powered, 6 cold). Reading the state consumes the acknowledge, so
 * the state is read only once the acknowledge is set.
 *
 * The consumers include IRQ-safe devices (DARTs), so the domains are IRQ-safe
 * and a transition is polled atomically, as for the PMGR power states.
 */
#include <linux/bits.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>

#define APPLE_PSD_STATE		0x80
#define APPLE_PSD_STATE_CUR	GENMASK(2, 0)
#define APPLE_PSD_TARGET	0x90
#define APPLE_PSD_ACK		0xbc
#define APPLE_PSD_ACK_DONE	BIT(0)

#define APPLE_PSD_ON		0
#define APPLE_PSD_COLD		6

#define APPLE_PSD_TIMEOUT_US	10000

struct apple_psd {
	struct generic_pm_domain genpd;
	struct device *dev;
	void __iomem *regs;
};

static u32 apple_psd_state(struct apple_psd *psd)
{
	return readl(psd->regs + APPLE_PSD_STATE) & APPLE_PSD_STATE_CUR;
}

static bool apple_psd_reached(struct apple_psd *psd, u32 target, u32 *ack,
			      u32 *state)
{
	*ack = readl(psd->regs + APPLE_PSD_ACK);
	if (!(*ack & APPLE_PSD_ACK_DONE))
		return false;
	*state = apple_psd_state(psd);
	return *state == target;
}

static int apple_psd_set_state(struct generic_pm_domain *genpd, u32 target)
{
	struct apple_psd *psd = container_of(genpd, struct apple_psd, genpd);
	u32 ack = 0, state;
	bool done;
	int ret;

	state = apple_psd_state(psd);
	if (state == target)
		return 0;
	writel(target, psd->regs + APPLE_PSD_TARGET);
	ret = read_poll_timeout_atomic(apple_psd_reached, done, done, 1,
				       APPLE_PSD_TIMEOUT_US, false, psd, target,
				       &ack, &state);
	if (ret) {
		dev_err(psd->dev, "%s: state %u not reached: ack %#x, state %u\n",
			genpd->name, target, ack, state);
		return ret;
	}
	return 0;
}

static int apple_psd_power_on(struct generic_pm_domain *genpd)
{
	return apple_psd_set_state(genpd, APPLE_PSD_ON);
}

static int apple_psd_power_off(struct generic_pm_domain *genpd)
{
	return apple_psd_set_state(genpd, APPLE_PSD_COLD);
}

static void apple_psd_remove_provider(void *data)
{
	struct apple_psd *psd = data;

	of_genpd_del_provider(psd->dev->of_node);
	pm_genpd_remove(&psd->genpd);
}

static int apple_psd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct apple_psd *psd;
	bool is_off;
	int ret;

	psd = devm_kzalloc(dev, sizeof(*psd), GFP_KERNEL);
	if (!psd)
		return -ENOMEM;
	psd->dev = dev;
	psd->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(psd->regs))
		return PTR_ERR(psd->regs);

	if (of_property_read_string(np, "label", &psd->genpd.name))
		psd->genpd.name = dev_name(dev);
	psd->genpd.power_on = apple_psd_power_on;
	psd->genpd.power_off = apple_psd_power_off;
	psd->genpd.flags = GENPD_FLAG_IRQ_SAFE;

	if (of_property_read_bool(np, "apple,always-on")) {
		psd->genpd.flags |= GENPD_FLAG_ALWAYS_ON;
		ret = apple_psd_power_on(&psd->genpd);
		if (ret)
			return ret;
	}
	is_off = apple_psd_state(psd) != APPLE_PSD_ON;

	ret = pm_genpd_init(&psd->genpd, NULL, is_off);
	if (ret)
		return ret;
	ret = of_genpd_add_provider_simple(np, &psd->genpd);
	if (ret) {
		pm_genpd_remove(&psd->genpd);
		return ret;
	}
	return devm_add_action_or_reset(dev, apple_psd_remove_provider, psd);
}

static const struct of_device_id apple_psd_match[] = {
	{ .compatible = "apple,t8152-psd" },
	{}
};
MODULE_DEVICE_TABLE(of, apple_psd_match);

static struct platform_driver apple_psd_driver = {
	.probe = apple_psd_probe,
	.driver = {
		.name = "apple-pmgr-psd",
		.of_match_table = apple_psd_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(apple_psd_driver);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple T8152 PMGR2 PSD power domains");
