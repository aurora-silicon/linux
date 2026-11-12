// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The GPU's power state word across a system sleep. The domain is always on as far
 * as genpd knows, so nothing else lowers it. The word and its parent come from the
 * device tree.
 */
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/soc/apple/mailbox.h>
#include <linux/soc/apple/pmp-report.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/soc/apple/sleep.h>

#include "sleep.h"

#define PS_AUTO_ENABLE		BIT(28)
#define PS_AUTO			GENMASK(27, 24)
#define PS_BUSY			BIT(11)
#define PS_FLAGS		GENMASK(9, 8)
#define PS_ACTUAL		GENMASK(7, 4)
#define PS_TARGET		GENMASK(3, 0)
#define PS_ACTIVE		0xf
#define PS_POLL_US		100
#define PS_TIMEOUT_US		50000

/* In the "sgx" range, as regs.rs has them. */
#define SGX_GATING		0xd06030
#define SGX_GATING_HOLD		BIT(2)
#define SGX_BRIDGE		0x1000104
#define SGX_BRIDGE_WORDS	2
#define SGX_BRIDGE_ENABLE	BIT(0)

#define ASC_CPU_STATUS		0x4
/* Where the ASC's own registers hold the CPU's control: G16. */
#define ASC_CPU_CONTROL		0x44
#define ASC_CPU_CONTROL_RUN	BIT(4)

/*
 * The GPU's PMP report entry: the one apple,pmp-report-entry names, or the GPU's
 * power domain where that is the entry itself (T6040: pmp-gfx).
 */
static struct device_node *asahi_g17_pmp_entry(struct device *dev, bool *domain)
{
	struct device_node *np;

	if (domain)
		*domain = false;
	np = of_parse_phandle(dev->of_node, "apple,pmp-report-entry", 0);
	if (np)
		return np;
	np = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (np && of_device_is_compatible(np, "apple,t6000-pmp-v2-report-entry")) {
		if (domain)
			*domain = true;
		return np;
	}
	of_node_put(np);

	return NULL;
}

/* The PMP's request for the GPU, where the device tree names one. */
int asahi_g17_pmp_request(struct device *dev, bool enable)
{
	struct device_node *np;
	int ret;

	np = asahi_g17_pmp_entry(dev, NULL);
	if (!np)
		return 0;
	ret = apple_pmp_report_entry_request(np, enable);
	of_node_put(np);

	return ret;
}

static bool asahi_g17_is_pwrstate(struct device_node *np)
{
	return of_device_is_compatible(np, "apple,t8103-pmgr-pwrstate") ||
	       of_device_is_compatible(np, "apple,pmgr-pwrstate");
}

/*
 * The GPU's PMGR state word. Where a PMP report entry stands in front of it (T6040:
 * power-domains = pmp-gfx, whose own domain is the PMGR's gfx), follow that entry's
 * domain down to the word.
 */
static struct device_node *asahi_g17_ps_node(struct device *dev)
{
	struct device_node *np, *next;
	int hops;

	np = of_parse_phandle(dev->of_node, "power-domains", 0);
	for (hops = 0; np && hops < 2 && !asahi_g17_is_pwrstate(np); hops++) {
		next = of_parse_phandle(np, "power-domains", 0);
		of_node_put(np);
		np = next;
	}
	if (!np)
		return ERR_PTR(-EINVAL);
	if (!asahi_g17_is_pwrstate(np)) {
		of_node_put(np);
		return ERR_PTR(-ENODEV);
	}

	return np;
}

static int asahi_g17_ps(struct device *dev, struct regmap **regmap, u32 *offset)
{
	struct device_node *np = asahi_g17_ps_node(dev);
	int ret;

	if (IS_ERR(np))
		return PTR_ERR(np);
	ret = of_property_read_u32(np, "reg", offset);
	if (!ret) {
		*regmap = np->parent ? syscon_node_to_regmap(np->parent) : ERR_PTR(-EINVAL);
		ret = PTR_ERR_OR_ZERO(*regmap);
	}
	of_node_put(np);

	return ret;
}

/* Automatic power management off. Returns the word as it was in @saved. */
int asahi_g17_power_auto_off(struct device *dev, u32 *saved)
{
	struct regmap *regmap;
	u32 offset;
	int ret;

	ret = asahi_g17_ps(dev, &regmap, &offset);
	if (ret)
		return ret;
	ret = regmap_read(regmap, offset, saved);
	if (ret)
		return ret;

	return regmap_write(regmap, offset, *saved & ~(PS_AUTO_ENABLE | PS_BUSY | PS_FLAGS));
}

/* The target off, then wait for the domain to finish going off. */
int asahi_g17_power_lower(struct device *dev)
{
	struct regmap *regmap;
	u32 offset, val, cur;
	int ret;

	ret = asahi_g17_ps(dev, &regmap, &offset);
	if (ret)
		return ret;
	ret = regmap_read(regmap, offset, &val);
	if (ret)
		return ret;

	regmap_write(regmap, offset,
		     (val & ~(PS_AUTO_ENABLE | PS_BUSY | PS_FLAGS | PS_TARGET)) | PS_FLAGS);

	ret = regmap_read_poll_timeout(regmap, offset, cur,
				       !FIELD_GET(PS_ACTUAL, cur) && !(cur & PS_BUSY),
				       PS_POLL_US, PS_TIMEOUT_US);
	dev_dbg(dev, "GPU domain %#x, now %#x\n", val, cur);

	return ret;
}

static u32 asahi_g17_ps_on(u32 val, u32 saved)
{
	val &= ~(PS_AUTO_ENABLE | PS_AUTO | PS_BUSY | PS_FLAGS | PS_ACTUAL | PS_TARGET);

	return val | (saved & PS_AUTO) | FIELD_PREP(PS_TARGET, PS_ACTIVE);
}

/* The target on, and wait for it. */
int asahi_g17_power_raise(struct device *dev, u32 saved)
{
	struct regmap *regmap;
	u32 offset, val, cur;
	int ret;

	ret = asahi_g17_ps(dev, &regmap, &offset);
	if (ret)
		return ret;
	ret = regmap_read(regmap, offset, &val);
	if (ret)
		return ret;
	dev_dbg(dev, "GPU domain before raising it: %#x\n", val);

	regmap_write(regmap, offset, asahi_g17_ps_on(val, saved));

	return regmap_read_poll_timeout(regmap, offset, cur,
					FIELD_GET(PS_ACTUAL, cur) == PS_ACTIVE,
					PS_POLL_US, PS_TIMEOUT_US);
}

/* Automatic power management as it was. */
int asahi_g17_power_auto_on(struct device *dev, u32 saved)
{
	struct regmap *regmap;
	u32 offset, val;
	int ret;

	ret = asahi_g17_ps(dev, &regmap, &offset);
	if (ret)
		return ret;
	ret = regmap_read(regmap, offset, &val);
	if (ret)
		return ret;

	return regmap_write(regmap, offset, asahi_g17_ps_on(val, saved) | (saved & PS_AUTO_ENABLE));
}

/*
 * What the sleep's commit needs to stop the GPU with interrupts off, taken in .prepare, and
 * what it saw, for the log after the sleep.
 */
struct asahi_g17_commit {
	struct device *dev;
	unsigned int count;
	struct apple_rtkit *rtk[2];
	struct apple_mbox *mbox[2];
	void __iomem *cpu[2];
	void __iomem *gating;
	void __iomem *bridge;
	void __iomem *ps;
	struct device_node *request;
	/* The power domain is the PMP's entry: the sleep controller lowers the word behind it. */
	bool entry_domain;

	bool ran;
	int idle[2];
	unsigned int idle_us[2];
	u32 status[2];
	u32 words[SGX_BRIDGE_WORDS + 1];
	int request_ret;
	u32 saved;
	u32 lowered;
	int lower_ret;
};

/*
 * down() at the commit, every refusal behind it: both instances idled by polling, mailboxes
 * and run bits off, the gating hold, the PMP request withdrawn while the PMP still runs, then
 * the bridge and the domain. Told to idle, the firmware can't carry on, so the sleep goes on
 * whatever it answers.
 */
static int asahi_g17_commit_stop(void *data)
{
	struct asahi_g17_commit *c = data;
	unsigned int i;
	u32 val;

	c->ran = true;
	for (i = 0; i < c->count; i++)
		c->idle[i] = apple_rtkit_sleep_idle_polled(c->rtk[i], c->cpu[i], &c->idle_us[i]);
	for (i = 0; i < c->count; i++) {
		c->status[i] = readl(c->cpu[i] + ASC_CPU_STATUS);
		apple_rtkit_mailbox_recv_off(c->rtk[i]);
		if (c->mbox[i])
			apple_mbox_ascwrap_v6_stop_cpu_atomic(c->mbox[i]);
		else
			writel(readl(c->cpu[i]) & ~ASC_CPU_CONTROL_RUN, c->cpu[i]);
		if (!i && c->bridge)
			writel(readl(c->gating) | SGX_GATING_HOLD, c->gating);
	}
	c->request_ret = c->request ? apple_pmp_report_entry_withdraw_polled(c->request) : 0;
	if (c->entry_domain)
		return (c->idle[0] || c->idle[1] || c->request_ret) ? 1 : 0;

	c->saved = readl(c->ps);
	writel(c->saved & ~(PS_AUTO_ENABLE | PS_BUSY | PS_FLAGS), c->ps);
	if (c->bridge) {
		for (i = 0; i < SGX_BRIDGE_WORDS; i++)
			c->words[i] = readl(c->bridge + 4 * i);
		c->words[i] = readl(c->gating);
		for (i = 0; i < SGX_BRIDGE_WORDS; i++)
			writel(c->words[i] & ~SGX_BRIDGE_ENABLE, c->bridge + 4 * i);
	}
	val = readl(c->ps);
	writel((val & ~(PS_AUTO_ENABLE | PS_BUSY | PS_FLAGS | PS_TARGET)) | PS_FLAGS, c->ps);
	c->lower_ret = readl_poll_timeout_atomic(c->ps, c->lowered,
						 !FIELD_GET(PS_ACTUAL, c->lowered) &&
						 !(c->lowered & PS_BUSY),
						 10, PS_TIMEOUT_US);

	return (c->idle[0] || c->idle[1] || c->request_ret || c->lower_ret) ? 1 : 0;
}

static void __iomem *asahi_g17_map(struct device_node *np, const char *name, int index,
				   u32 offset, u32 size)
{
	struct resource res;

	if (name)
		index = of_property_match_string(np, "reg-names", name);
	if (index < 0 || of_address_to_resource(np, index, &res) ||
	    offset + size > resource_size(&res))
		return NULL;

	return ioremap(res.start + offset, size);
}

static void asahi_g17_commit_free(struct asahi_g17_commit *c)
{
	if (c->request) {
		apple_pmp_report_entry_commit_owned(c->request, false);
		of_node_put(c->request);
	}
	/* The mapping is only this structure's on G16: G17's belongs to its mailbox. */
	if (!c->mbox[0] && c->cpu[0])
		iounmap(c->cpu[0]);
	if (c->ps)
		iounmap(c->ps);
	if (c->bridge)
		iounmap(c->bridge);
	if (c->gating)
		iounmap(c->gating);
	kfree(c);
}

/*
 * Leaves the GPU's whole stop to the sleep's commit: maps what the stop touches, keeps the
 * PMP request for it, and registers it with the sleep driver, ready. Nothing is stopped
 * here. @secondary_rtk is NULL on a single instance, and @bridge false on a target that runs
 * without the bridge.
 */
struct asahi_g17_commit *asahi_g17_commit_prepare(struct device *dev,
						  struct apple_rtkit *primary_rtk,
						  struct apple_mbox *primary_mbox,
						  struct apple_rtkit *secondary_rtk,
						  struct apple_mbox *secondary_mbox, bool bridge)
{
	struct asahi_g17_commit *c;
	struct device_node *np;
	unsigned int i;
	u32 offset;
	int ret;

	c = kzalloc_obj(*c);
	if (!c)
		return ERR_PTR(-ENOMEM);
	c->dev = dev;
	c->rtk[0] = primary_rtk;
	c->mbox[0] = primary_mbox;
	c->rtk[1] = secondary_rtk;
	c->mbox[1] = secondary_mbox;
	c->count = secondary_rtk ? 2 : 1;

	ret = -EOPNOTSUPP;
	for (i = 0; i < c->count; i++) {
		c->cpu[i] = apple_mbox_ascwrap_v6_cpu_control(c->mbox[i]);
		if (!c->rtk[i] || !c->cpu[i])
			goto err;
	}

	np = asahi_g17_ps_node(dev);
	if (IS_ERR(np)) {
		ret = PTR_ERR(np);
		goto err;
	}
	ret = -ENODEV;
	if (!of_property_read_u32(np, "reg", &offset) && np->parent)
		c->ps = asahi_g17_map(np->parent, NULL, 0, offset, 4);
	of_node_put(np);
	c->gating = asahi_g17_map(dev->of_node, "sgx", 0, SGX_GATING, 4);
	if (bridge)
		c->bridge = asahi_g17_map(dev->of_node, "sgx", 0, SGX_BRIDGE,
					  4 * SGX_BRIDGE_WORDS);
	if (!c->ps || !c->gating || (bridge && !c->bridge))
		goto err;
	/* The word as it is before the sleep, for the raise after it if the stop leaves it. */
	c->saved = readl(c->ps);

	c->request = asahi_g17_pmp_entry(dev, &c->entry_domain);
	if (c->request)
		apple_pmp_report_entry_commit_owned(c->request, true);

	ret = apple_sleep_coproc_register_first(dev, asahi_g17_commit_stop, c);
	if (ret)
		goto err;
	apple_sleep_coproc_set_ready(dev, true);

	return c;

err:
	asahi_g17_commit_free(c);
	return ERR_PTR(ret);
}

/* Whether only the sleep's commit may stop the GPU: then nothing else may. */
bool asahi_g17_commit_required(void)
{
	return apple_sleep_commit_stops_coprocs();
}

/*
 * The same stop for G16: one firmware instance, whose control registers are the ASC's own, no
 * bridge and no PMP request to withdraw, as its vote stays through the sleep.
 */
struct asahi_g17_commit *asahi_g16_commit_prepare(struct device *dev, struct apple_rtkit *rtk)
{
	struct asahi_g17_commit *c;
	struct device_node *np;
	u32 offset;
	int ret;

	c = kzalloc_obj(*c);
	if (!c)
		return ERR_PTR(-ENOMEM);
	c->dev = dev;
	c->rtk[0] = rtk;
	c->count = 1;

	ret = -ENODEV;
	c->cpu[0] = asahi_g17_map(dev->of_node, "asc", 0, ASC_CPU_CONTROL, 8);
	np = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (!np)
		goto err;
	if (!of_property_read_u32(np, "reg", &offset) && np->parent)
		c->ps = asahi_g17_map(np->parent, NULL, 0, offset, 4);
	of_node_put(np);
	if (!c->cpu[0] || !c->ps)
		goto err;

	ret = apple_sleep_coproc_register_first(dev, asahi_g17_commit_stop, c);
	if (ret)
		goto err;
	apple_sleep_coproc_set_ready(dev, true);

	return c;

err:
	asahi_g17_commit_free(c);
	return ERR_PTR(ret);
}

/*
 * After the sleep, refused or not: whether the commit stopped the GPU, and in @saved the
 * domain's word from before, if it did. Frees @c.
 */
bool asahi_g17_commit_finish(struct asahi_g17_commit *c, u32 *saved)
{
	bool stopped = apple_sleep_coproc_stopped(c->dev) || c->ran;

	apple_sleep_coproc_unregister(c->dev);
	if (c->ran) {
		if (c->idle[0] || c->idle[1] || c->request_ret || c->lower_ret)
			dev_warn(c->dev, "GPU stop at the commit: idle %d %d, PMP request %d, domain %#x (%d)\n",
				 c->idle[0], c->idle[1], c->request_ret, c->lowered, c->lower_ret);
		dev_dbg(c->dev, "GPU stopped at the commit: idle in %u %u us, status %#x %#x\n",
			c->idle_us[0], c->idle_us[1], c->status[0], c->status[1]);
		dev_dbg(c->dev, "GPU at the commit: bridge %#x %#x gating %#x, domain %#x now %#x\n",
			c->words[0], c->words[1], c->words[2], c->saved, c->lowered);
		*saved = c->saved;
	}
	asahi_g17_commit_free(c);

	return stopped;
}
