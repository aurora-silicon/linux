// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC PMC voter interface driver
 *
 * The PMC arbitrates the performance state of shared rails, such as the
 * DRAM controller (DCS) and the SoC fabric, between voter agents: the PMP,
 * the host performance controller (CLPC), the GPU and the application
 * processor. Each agent owns a perf-state floor word holding one 4-bit
 * state per rail, and each rail has an interface enable word with one bit
 * per agent. The PMC only honours the floor of an agent whose interface is
 * enabled on that rail.
 *
 * At startup macOS enables the static agents (PMP, CLPC and AP) on their
 * rails, and enables a dynamic agent such as the GPU only while the PMGR
 * device it belongs to is powered on: after the device has been powered
 * on, and before it is powered off. This driver does the former at probe
 * and models each dynamic agent as a power domain that sits between the
 * device and its PMGR power state, which gives the same ordering.
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/bits.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/soc/apple/pmc.h>
#include <linux/spinlock.h>

#define APPLE_PMC_MAX_RAILS	8
#define APPLE_PMC_MAX_AGENTS	32

/**
 * struct apple_pmc_hw - per-SoC layout of the PMC voter interface
 * @agent_stride: distance between the floor words of consecutive agents
 *	in the "voters" region
 * @agents: number of voter agents
 * @rails: number of rails
 * @agent_state: offset of the per-agent state words in the "rails" region;
 *	each holds the agent's floor as the PMC honours it: masked by the
 *	agent's interface enables and clamped to each rail's highest state
 * @rail_enable: offset of the per-rail interface enable words in the
 *	"rails" region
 */
struct apple_pmc_hw {
	u32 agent_stride;
	u32 agents;
	u32 rails;
	u32 agent_state;
	u32 rail_enable;
};

struct apple_pmc {
	struct device *dev;
	const struct apple_pmc_hw *hw;
	void __iomem *voters;
	void __iomem *rails;
	/* Serializes read-modify-write cycles on enable and floor words. */
	raw_spinlock_t lock;
	u32 saved_enable[APPLE_PMC_MAX_RAILS];
	u32 static_agents;
	struct list_head domains;
	struct dentry *debugfs;
};

struct apple_pmc_domain {
	struct generic_pm_domain genpd;
	struct apple_pmc *pmc;
	struct device_node *np;
	struct list_head list;
	unsigned int agent;
	bool provider;
};

#define genpd_to_apple_pmc_domain(_genpd) \
	container_of(_genpd, struct apple_pmc_domain, genpd)

static struct dentry *apple_pmc_debugfs_root;

static u32 apple_pmc_enable_read(struct apple_pmc *pmc, unsigned int rail)
{
	return readl(pmc->rails + pmc->hw->rail_enable + 4 * rail);
}

/* Caller holds pmc->lock. Every write is read back, as macOS does. */
static int apple_pmc_enable_update(struct apple_pmc *pmc, unsigned int rail,
				   u32 set, u32 clear)
{
	void __iomem *reg = pmc->rails + pmc->hw->rail_enable + 4 * rail;
	u32 old = readl(reg);
	u32 val = (old | set) & ~clear;

	if (val == old)
		return 0;

	writel(val, reg);
	if (readl(reg) != val) {
		dev_err_ratelimited(pmc->dev,
				    "rail %u enable %#x -> %#x did not stick\n",
				    rail, old, val);
		return -EIO;
	}

	return 0;
}

static int apple_pmc_agent_enable(struct apple_pmc *pmc, unsigned int agent,
				  bool enable)
{
	unsigned long flags;
	unsigned int rail;
	int ret = 0;

	raw_spin_lock_irqsave(&pmc->lock, flags);
	for (rail = 0; rail < pmc->hw->rails && !ret; rail++)
		ret = apple_pmc_enable_update(pmc, rail,
					      enable ? BIT(agent) : 0,
					      enable ? 0 : BIT(agent));
	raw_spin_unlock_irqrestore(&pmc->lock, flags);

	return ret;
}

static struct apple_pmc *apple_pmc_from_dev(struct device *dev)
{
	return dev ? dev_get_drvdata(dev) : NULL;
}

/**
 * apple_pmc_rail_count() - number of rails of a PMC
 * @dev: the PMC device
 *
 * Return: the number of rails, or 0 if @dev is not a bound PMC.
 */
unsigned int apple_pmc_rail_count(struct device *dev)
{
	struct apple_pmc *pmc = apple_pmc_from_dev(dev);

	return pmc ? pmc->hw->rails : 0;
}
EXPORT_SYMBOL_GPL(apple_pmc_rail_count);

/**
 * apple_pmc_voter_enabled() - whether the PMC honours an agent on a rail
 * @dev: the PMC device
 * @agent: voter agent
 * @rail: rail
 *
 * Context: Any context.
 */
bool apple_pmc_voter_enabled(struct device *dev, unsigned int agent,
			     unsigned int rail)
{
	struct apple_pmc *pmc = apple_pmc_from_dev(dev);

	if (!pmc || agent >= pmc->hw->agents || rail >= pmc->hw->rails)
		return false;

	return apple_pmc_enable_read(pmc, rail) & BIT(agent);
}
EXPORT_SYMBOL_GPL(apple_pmc_voter_enabled);

/**
 * apple_pmc_floor_read() - read the perf-state floor word of an agent
 * @dev: the PMC device
 * @agent: voter agent
 *
 * Context: Any context.
 * Return: the floor word, or 0 if @agent is out of range.
 */
u32 apple_pmc_floor_read(struct device *dev, unsigned int agent)
{
	struct apple_pmc *pmc = apple_pmc_from_dev(dev);

	if (!pmc || agent >= pmc->hw->agents)
		return 0;

	return readl(pmc->voters + agent * pmc->hw->agent_stride);
}
EXPORT_SYMBOL_GPL(apple_pmc_floor_read);

/**
 * apple_pmc_floor_update() - update the perf-state floor of an agent
 * @dev: the PMC device
 * @agent: voter agent
 * @mask: bits of the floor word to change, see APPLE_PMC_RAIL_MASK()
 * @value: new value of the bits in @mask
 *
 * The floor only takes effect on rails where the agent's interface is
 * enabled.
 *
 * Context: Any context.
 * Return: 0 on success, -EINVAL if @agent is out of range, or -EIO if the
 * PMC did not accept the new floor.
 */
int apple_pmc_floor_update(struct device *dev, unsigned int agent, u32 mask,
			   u32 value)
{
	struct apple_pmc *pmc = apple_pmc_from_dev(dev);
	void __iomem *reg;
	unsigned long flags;
	u32 old, val;
	int ret = 0;

	if (!pmc || agent >= pmc->hw->agents)
		return -EINVAL;

	reg = pmc->voters + agent * pmc->hw->agent_stride;
	raw_spin_lock_irqsave(&pmc->lock, flags);
	old = readl(reg);
	val = (old & ~mask) | (value & mask);
	if (val != old) {
		writel(val, reg);
		if ((readl(reg) & mask) != (value & mask))
			ret = -EIO;
	}
	raw_spin_unlock_irqrestore(&pmc->lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(apple_pmc_floor_update);

static int apple_pmc_domain_power_on(struct generic_pm_domain *genpd)
{
	struct apple_pmc_domain *dom = genpd_to_apple_pmc_domain(genpd);

	return apple_pmc_agent_enable(dom->pmc, dom->agent, true);
}

static int apple_pmc_domain_power_off(struct generic_pm_domain *genpd)
{
	struct apple_pmc_domain *dom = genpd_to_apple_pmc_domain(genpd);

	return apple_pmc_agent_enable(dom->pmc, dom->agent, false);
}

static void apple_pmc_domain_free(struct apple_pmc_domain *dom)
{
	if (dom->provider)
		of_genpd_del_provider(dom->np);
	if (pm_genpd_remove(&dom->genpd)) {
		/* The core still owns this domain. Keep its storage alive. */
		dev_warn(dom->pmc->dev, "retaining busy power domain %s\n",
			 dom->genpd.name);
		return;
	}
	of_node_put(dom->np);
	kfree(dom->genpd.name);
	kfree(dom);
}

static int apple_pmc_domain_add(struct apple_pmc *pmc, struct device_node *np)
{
	struct device *dev = pmc->dev;
	struct apple_pmc_domain *dom;
	struct of_phandle_args parent;
	const char *label;
	u32 agent;
	int i, ret;

	ret = of_property_read_u32(np, "reg", &agent);
	if (ret)
		return dev_err_probe(dev, ret, "%pOF: missing reg\n", np);
	if (agent >= pmc->hw->agents)
		return dev_err_probe(dev, -EINVAL, "%pOF: invalid agent %u\n",
				     np, agent);
	if (pmc->static_agents & BIT(agent))
		return dev_err_probe(dev, -EINVAL,
				     "%pOF: agent %u is also static\n", np,
				     agent);

	dom = kzalloc_obj(*dom);
	if (!dom)
		return -ENOMEM;

	dom->pmc = pmc;
	dom->np = of_node_get(np);
	dom->agent = agent;
	if (of_property_read_string(np, "label", &label))
		label = np->name;
	dom->genpd.name = kasprintf(GFP_KERNEL, "pmc-%s", label);
	if (!dom->genpd.name) {
		of_node_put(dom->np);
		kfree(dom);
		return -ENOMEM;
	}
	dom->genpd.power_on = apple_pmc_domain_power_on;
	dom->genpd.power_off = apple_pmc_domain_power_off;

	/*
	 * Start from the hardware state, so that an agent enabled by the
	 * bootloader is not disabled under a device that is still running.
	 */
	ret = pm_genpd_init(&dom->genpd, NULL,
			    !(apple_pmc_enable_read(pmc, 0) & BIT(agent)));
	if (ret) {
		of_node_put(dom->np);
		kfree(dom->genpd.name);
		kfree(dom);
		return dev_err_probe(dev, ret, "%pOF: genpd init failed\n", np);
	}
	list_add_tail(&dom->list, &pmc->domains);

	ret = of_genpd_add_provider_simple(np, &dom->genpd);
	if (ret)
		return dev_err_probe(dev, ret, "%pOF: provider failed\n", np);
	dom->provider = true;

	for (i = 0; !of_parse_phandle_with_args(np, "power-domains",
						"#power-domain-cells", i,
						&parent); i++) {
		struct of_phandle_args child = { .np = np };

		ret = of_genpd_add_subdomain(&parent, &child);
		of_node_put(parent.np);
		if (ret)
			return dev_err_probe(dev, ret,
					     "%pOF: failed to join parent domain\n",
					     np);
	}

	return 0;
}

static int apple_pmc_status_show(struct seq_file *s, void *unused)
{
	struct apple_pmc *pmc = s->private;
	const struct apple_pmc_hw *hw = pmc->hw;
	unsigned int i;

	seq_printf(s, "static agents: %#x\n", pmc->static_agents);
	for (i = 0; i < hw->agents; i++)
		seq_printf(s, "agent%u floor: %#010x honoured: %#010x\n", i,
			   readl(pmc->voters + i * hw->agent_stride),
			   readl(pmc->rails + hw->agent_state + 4 * i));
	for (i = 0; i < hw->rails; i++)
		seq_printf(s, "rail%u enable: %#010x (saved %#010x)\n",
			   i, apple_pmc_enable_read(pmc, i), pmc->saved_enable[i]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_pmc_status);

static void apple_pmc_release(void *data)
{
	struct apple_pmc *pmc = data;
	struct apple_pmc_domain *dom, *tmp;
	unsigned long flags;
	unsigned int rail;

	of_platform_depopulate(pmc->dev);
	debugfs_remove_recursive(pmc->debugfs);
	list_for_each_entry_safe_reverse(dom, tmp, &pmc->domains, list) {
		list_del(&dom->list);
		apple_pmc_domain_free(dom);
	}

	raw_spin_lock_irqsave(&pmc->lock, flags);
	for (rail = 0; rail < pmc->hw->rails; rail++)
		writel(pmc->saved_enable[rail],
		       pmc->rails + pmc->hw->rail_enable + 4 * rail);
	raw_spin_unlock_irqrestore(&pmc->lock, flags);
}

static int apple_pmc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	u32 agents[APPLE_PMC_MAX_AGENTS];
	struct device_node *child;
	struct apple_pmc *pmc;
	unsigned int rail;
	int count, i, ret;

	pmc = devm_kzalloc(dev, sizeof(*pmc), GFP_KERNEL);
	if (!pmc)
		return -ENOMEM;

	pmc->dev = dev;
	pmc->hw = of_device_get_match_data(dev);
	raw_spin_lock_init(&pmc->lock);
	INIT_LIST_HEAD(&pmc->domains);

	if (pmc->hw->rails > APPLE_PMC_MAX_RAILS ||
	    pmc->hw->agents > APPLE_PMC_MAX_AGENTS)
		return -EINVAL;

	pmc->voters = devm_platform_ioremap_resource_byname(pdev, "voters");
	if (IS_ERR(pmc->voters))
		return PTR_ERR(pmc->voters);
	pmc->rails = devm_platform_ioremap_resource_byname(pdev, "rails");
	if (IS_ERR(pmc->rails))
		return PTR_ERR(pmc->rails);

	/* An unpowered PMC reads all-ones; do not write to it. */
	for (rail = 0; rail < pmc->hw->rails; rail++) {
		pmc->saved_enable[rail] = apple_pmc_enable_read(pmc, rail);
		if (pmc->saved_enable[rail] == ~0U)
			return dev_err_probe(dev, -ENODEV,
					     "PMC is not responding\n");
	}

	count = of_property_count_u32_elems(np, "apple,static-voters");
	if (count > 0) {
		if (count > ARRAY_SIZE(agents))
			return -EINVAL;
		ret = of_property_read_u32_array(np, "apple,static-voters",
						 agents, count);
		if (ret)
			return ret;
		for (i = 0; i < count; i++) {
			if (agents[i] >= pmc->hw->agents)
				return dev_err_probe(dev, -EINVAL,
						     "invalid static voter %u\n",
						     agents[i]);
			pmc->static_agents |= BIT(agents[i]);
		}
	}

	platform_set_drvdata(pdev, pmc);
	ret = devm_add_action_or_reset(dev, apple_pmc_release, pmc);
	if (ret)
		return ret;

	for (i = 0; i < pmc->hw->agents; i++) {
		if (!(pmc->static_agents & BIT(i)))
			continue;
		ret = apple_pmc_agent_enable(pmc, i, true);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to enable static voter %d\n",
					     i);
	}

	for_each_available_child_of_node(np, child) {
		if (!of_property_present(child, "#power-domain-cells"))
			continue;
		ret = apple_pmc_domain_add(pmc, child);
		if (ret) {
			of_node_put(child);
			return ret;
		}
	}

	pmc->debugfs = debugfs_create_dir(dev_name(dev), apple_pmc_debugfs_root);
	debugfs_create_file("status", 0400, pmc->debugfs, pmc,
			    &apple_pmc_status_fops);

	ret = of_platform_populate(np, NULL, NULL, dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to create child devices\n");

	return 0;
}

static const struct apple_pmc_hw apple_pmc_hw_t8140 = {
	.agent_stride = 0x4000,
	.agents = 4,
	.rails = 4,
	.agent_state = 0x0,
	.rail_enable = 0x2000,
};

static const struct of_device_id apple_pmc_of_match[] = {
	{ .compatible = "apple,t8140-pmc", .data = &apple_pmc_hw_t8140 },
	{}
};
MODULE_DEVICE_TABLE(of, apple_pmc_of_match);

static struct platform_driver apple_pmc_driver = {
	.probe = apple_pmc_probe,
	.driver = {
		.name = "apple-pmc",
		.of_match_table = apple_pmc_of_match,
		/* Power-domain providers are not intended for manual unbinding. */
		.suppress_bind_attrs = true,
	},
};

static int __init apple_pmc_init(void)
{
	apple_pmc_debugfs_root = debugfs_create_dir("apple-pmc", NULL);
	return platform_driver_register(&apple_pmc_driver);
}
module_init(apple_pmc_init);

MODULE_DESCRIPTION("Apple SoC PMC voter interface driver");
MODULE_LICENSE("Dual MIT/GPL");
