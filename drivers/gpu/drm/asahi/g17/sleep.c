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
#include <linux/mm.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/soc/apple/dcp-v27-sleep.h>
#include <linux/soc/apple/mailbox.h>
#include <linux/soc/apple/pmp-report.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/soc/apple/sleep.h>

#include "sleep.h"

#define PS_AUTO_ENABLE		BIT(28)
#define PS_AUTO			GENMASK(27, 24)
#define PS_MIN			GENMASK(19, 16)
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

/* Only the M5 boards with a deep sleep need a wrapped provider translation here: those whose
 * device tree describes it. Other G17 clients retain their established direct-domain methods
 * below.
 */
static bool asahi_g17_m5_provider(struct device *dev)
{
	return apple_t6050_sleep_described() &&
	       of_device_is_compatible(dev->of_node, "apple,agx-g17s");
}

static bool asahi_g17_m5_zero_cells(struct device_node *np)
{
	u32 cells;

	return of_device_is_available(np) &&
	       of_property_count_u32_elems(np, "#power-domain-cells") == 1 &&
	       !of_property_read_u32(np, "#power-domain-cells", &cells) && !cells;
}

static int asahi_g17_m5_dependency(struct device_node *np, struct device_node **out)
{
	struct of_phandle_args args;
	int ret;

	if (of_count_phandle_with_args(np, "power-domains", "#power-domain-cells") != 1)
		return -EINVAL;
	ret = of_parse_phandle_with_args(np, "power-domains", "#power-domain-cells", 0, &args);
	if (ret)
		return ret;
	if (args.args_count || !asahi_g17_m5_zero_cells(args.np)) {
		of_node_put(args.np);
		return -EINVAL;
	}
	*out = args.np;
	return 0;
}

/* Report reg is an entry identity. Its ONLY dependency supplies the power word. */
static int asahi_g17_m5_report_power(struct device_node *np, struct device_node **power)
{
	struct device_node *parent;
	u32 id;
	int ret = -EINVAL;

	if (!of_device_is_compatible(np, "apple,t6050-pmp-v2-report-entry") ||
	    !asahi_g17_m5_zero_cells(np) ||
	    of_property_count_u32_elems(np, "reg") != 1 ||
	    of_property_read_u32(np, "reg", &id) || id >= 64)
		return -EINVAL;
	parent = of_get_parent(np);
	if (parent && of_device_is_available(parent) &&
	    of_device_is_compatible(parent, "apple,t6050-pmp-v2-report"))
		ret = asahi_g17_m5_dependency(np, power);
	of_node_put(parent);
	return ret;
}

/* Referenced output nodes; metadata only, no request or power transition. */
static int asahi_g17_m5_nodes(struct device *dev, struct device_node **power,
			    struct device_node **request, u32 *offset)
{
	struct device_node *np = NULL, *report = NULL, *parent = NULL;
	struct device_node *explicit = NULL, *dep = NULL;
	struct resource res;
	u32 reg[2];
	int ret;

	*power = *request = NULL;
	ret = asahi_g17_m5_dependency(dev->of_node, &np);
	if (ret)
		return ret;
	if (of_device_is_compatible(np, "apple,t6050-pmp-v2-report-entry")) {
		report = np;
		np = NULL;
		ret = asahi_g17_m5_report_power(report, &np);
		if (ret)
			goto out;
	}
	ret = -EINVAL;
	if (!of_device_is_compatible(np, "apple,t6050-pmgr-pwrstate") ||
	    !asahi_g17_m5_zero_cells(np) ||
	    of_property_count_u32_elems(np, "reg") != 2 ||
	    of_property_read_u32_array(np, "reg", reg, 2) ||
	    reg[1] != sizeof(u32) || (reg[0] & 3))
		goto out;
	parent = of_get_parent(np);
	if (!parent || !of_device_is_available(parent) ||
	    !of_device_is_compatible(parent, "apple,t6050-pmgr") ||
	    !of_device_is_compatible(parent, "syscon") ||
	    of_address_to_resource(parent, 0, &res) || !(res.flags & IORESOURCE_MEM) ||
	    res.end < res.start || resource_size(&res) < sizeof(u32) ||
	    reg[0] > resource_size(&res) - sizeof(u32))
		goto out;
	if (of_property_present(dev->of_node, "apple,pmp-report-entry")) {
		if (of_property_count_u32_elems(dev->of_node, "apple,pmp-report-entry") != 1)
			goto out;
		explicit = of_parse_phandle(dev->of_node, "apple,pmp-report-entry", 0);
		if (!explicit || (report && explicit != report))
			goto out;
		ret = asahi_g17_m5_report_power(explicit, &dep);
		if (ret)
			goto out;
		ret = -EINVAL;
		if (dep != np)
			goto out;
		if (!report)
			report = of_node_get(explicit);
	}
	*offset = reg[0];
	*power = np;
	*request = report;
	np = report = NULL;
	ret = 0;
out:
	of_node_put(np);
	of_node_put(report);
	of_node_put(parent);
	of_node_put(explicit);
	of_node_put(dep);
	return ret;
}

/* The PMP's request for the GPU, where the device tree names one. */
int asahi_g17_pmp_request(struct device *dev, bool enable)
{
	struct device_node *np;
	int ret;

	if (asahi_g17_m5_provider(dev)) {
		struct device_node *power;
		u32 offset;

		ret = asahi_g17_m5_nodes(dev, &power, &np, &offset);
		if (ret)
			return ret;
		of_node_put(power);
	} else {
		np = of_parse_phandle(dev->of_node, "apple,pmp-report-entry", 0);
	}
	if (!np)
		return 0;
	ret = apple_pmp_report_entry_request(np, enable);
	of_node_put(np);

	return ret;
}

/*
 * The node of the GPU's power state word. Where the GPU's first power domain is its PMP
 * report entry (a small index for a reg, under the PMP), as in the J616C device tree the
 * bring-up runs, the word is that entry's own domain.
 */
static struct device_node *asahi_g17_ps_node(struct device *dev)
{
	struct device_node *np, *next;

	np = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (np && of_device_is_compatible(np, "apple,t6000-pmp-v2-report-entry")) {
		next = of_parse_phandle(np, "power-domains", 0);
		of_node_put(np);
		np = next;
	}
	return np;
}

static int asahi_g17_ps(struct device *dev, struct regmap **regmap, u32 *offset)
{
	struct device_node *np;
	int ret;

	if (asahi_g17_m5_provider(dev)) {
		struct device_node *request, *parent;

		ret = asahi_g17_m5_nodes(dev, &np, &request, offset);
		if (ret)
			return ret;
		of_node_put(request);
		parent = of_get_parent(np);
		*regmap = parent ? syscon_node_to_regmap(parent) : ERR_PTR(-EINVAL);
		ret = PTR_ERR_OR_ZERO(*regmap);
		of_node_put(parent);
		of_node_put(np);
		return ret;
	}
	np = asahi_g17_ps_node(dev);
	if (!np)
		return -EINVAL;
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

/*
 * The automatic state to raise with: apple,sleep-ps-auto if the DT has it, else the saved one.
 *
 * The floor (bits 19:16) as well, from apple,sleep-ps-min if the DT has it. The sleep
 * controller's wake replay writes the GFX word whole, with a zero floor, and the raise keeps
 * whatever floor it finds. The boot's word has floor 4, and the domain idles at state 4
 * between jobs. With floor 0 it goes fully off between jobs after every wake instead, and a
 * long-lived client's compute kick then occasionally never starts. Without the property the
 * floor stays as found.
 */
static u32 asahi_g17_ps_on(struct device *dev, u32 val, u32 saved)
{
	u32 ps_auto, ps_min;

	val &= ~(PS_AUTO_ENABLE | PS_AUTO | PS_BUSY | PS_FLAGS | PS_ACTUAL | PS_TARGET);
	if (!of_property_read_u32(dev->of_node, "apple,sleep-ps-min", &ps_min))
		val = (val & ~PS_MIN) | FIELD_PREP(PS_MIN, ps_min & 0xf);

	if (!of_property_read_u32(dev->of_node, "apple,sleep-ps-auto", &ps_auto))
		return val | FIELD_PREP(PS_AUTO, ps_auto) | FIELD_PREP(PS_TARGET, PS_ACTIVE);
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

	regmap_write(regmap, offset, asahi_g17_ps_on(dev, val, saved));

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

	ret = regmap_write(regmap, offset,
			   asahi_g17_ps_on(dev, val, saved) | (saved & PS_AUTO_ENABLE));
	/* The word as the wake leaves it, its floor included (bits 19:16). */
	if (!ret && !regmap_read(regmap, offset, &val))
		dev_info(dev, "GPU domain after the wake: %#x (floor %lu, saved %#x)\n", val,
			 FIELD_GET(PS_MIN, val), saved);
	return ret;
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

	bool ran, request_owned;
	int idle[2];
	unsigned int idle_us[2];
	u32 status[2];
	u32 words[SGX_BRIDGE_WORDS + 1];
	int request_ret;
	u32 saved;
	u32 lowered;
	int lower_ret;
	/* The domain at the start of the stop, and the raise to full power before any ASC access */
	u32 entry;
	int raise_ret;
	/* Whether the stop raises the domain first (asahi_g17_commit_raises()) */
	bool raise;
};

/*
 * Whether the commit's stop raises the GPU's domain before it touches the coprocessor: on the
 * SoCs the raise was made for, the T8142 (J815: the GPU gates itself between frames and its
 * registers may not answer below full power). Elsewhere the stop goes straight
 * to the idle poll, as before the raise. On the T8140 (J700) the domain reads its target on
 * and nothing powered under automatic power at the commit (0x1000230f); a raise from there
 * times out with BUSY latched, the PMP's AP IDLE then goes unanswered, the sleep takes the
 * deep path and the PMP and GPU firmware don't come back from it.
 */
static bool asahi_g17_commit_raises(struct device *dev)
{
	return of_device_is_compatible(dev->of_node, "apple,agx-t8142");
}

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
	apple_sleep_note_sub(1);
	/*
	 * The GPU gates itself between frames (~95 times a second on J815), and at the commit its
	 * domain read 0x244 (actual 4, J815 boot 12505746): its coprocessor's and mailbox's
	 * registers may not answer there, and with interrupts off a stalled read ends at WD2 (J815
	 * boot 3c93d5a2: PMU note 0x44, the first commit stop). Bring it to full power first, as
	 * asahi_g17_power_raise() does, and keep the word from before for the resume. Only where
	 * asahi_g17_commit_raises() says so.
	 */
	if (c->raise) {
		c->entry = readl(c->ps);
		c->saved = c->entry;
		if (FIELD_GET(PS_ACTUAL, c->entry) != PS_ACTIVE) {
			writel(asahi_g17_ps_on(c->dev, c->entry, c->entry), c->ps);
			c->raise_ret = readl_poll_timeout_atomic(c->ps, val,
								 FIELD_GET(PS_ACTUAL, val) ==
								 PS_ACTIVE, 10, PS_TIMEOUT_US);
		}
	}
	apple_sleep_note_sub(2);
	if (c->raise_ret) {
		/* Its registers can't be trusted to answer: lower the domain only. */
		c->idle[0] = c->raise_ret;
		goto lower;
	}
	for (i = 0; i < c->count; i++)
		c->idle[i] = apple_rtkit_sleep_idle_polled(c->rtk[i], c->cpu[i], &c->idle_us[i]);
	apple_sleep_note_sub(3);
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
	apple_sleep_note_sub(4);
	c->request_ret = c->request ? apple_pmp_report_entry_withdraw_polled(c->request) : 0;
	apple_sleep_note_sub(5);

	/*
	 * Off automatic management, for the bridge (still at full power after a raise). Without
	 * the raise, the word for the resume is the one found here, after the idle.
	 */
	val = readl(c->ps);
	if (!c->raise)
		c->saved = val;
	writel(val & ~(PS_AUTO_ENABLE | PS_BUSY | PS_FLAGS), c->ps);
	if (c->bridge) {
		for (i = 0; i < SGX_BRIDGE_WORDS; i++)
			c->words[i] = readl(c->bridge + 4 * i);
		c->words[i] = readl(c->gating);
		for (i = 0; i < SGX_BRIDGE_WORDS; i++)
			writel(c->words[i] & ~SGX_BRIDGE_ENABLE, c->bridge + 4 * i);
	}
	apple_sleep_note_sub(6);
lower:
	val = readl(c->ps);
	writel((val & ~(PS_AUTO_ENABLE | PS_BUSY | PS_FLAGS | PS_TARGET)) | PS_FLAGS, c->ps);
	c->lower_ret = readl_poll_timeout_atomic(c->ps, c->lowered,
						 !FIELD_GET(PS_ACTUAL, c->lowered) &&
						 !(c->lowered & PS_BUSY),
						 10, PS_TIMEOUT_US);
	apple_sleep_note_sub(7);

	return (c->idle[0] || c->idle[1] || c->request_ret || c->lower_ret) ? 1 : 0;
}

/*
 * The M5 Pro's stop, made in the sleep's last steps, before the commit, and nothing else. GFX
 * then GFX1, each whole: AP IDLE, IOP IDLE and its ack, receiving off, the CPU's run bit off.
 * Then GFX's power word down. No PMP request, gating or bridge write.
 *
 * After a first sleep the domain runs under automatic power as it does under macOS: between
 * jobs its word has its target on, automatic power enabled and nothing powered, where the
 * boot's word never reads that low. That is an idle GPU whose firmware still runs, not a stopped
 * one: a stop that sends it no IDLE leaves firmware the wake cannot restart. macOS stops a
 * domain that is up, so the domain is taken out of automatic power and waited on first, and
 * the stop is the same from there.
 */
static int asahi_g17_m5_commit_stop(void *data)
{
	struct asahi_g17_commit *c = data;
	unsigned int i;
	u32 val, automatic;

	c->ran = true;
	/*
	 * With its domain off the first mailbox read here takes a synchronous external abort.
	 * With GFX off there is nothing to idle: report it and only lower the word.
	 */
	c->saved = readl(c->ps);
	dev_info(c->dev, "commit stop: GFX %#x\n", c->saved);
	automatic = c->saved & PS_AUTO_ENABLE;
	if (!FIELD_GET(PS_ACTUAL, c->saved) && automatic && FIELD_GET(PS_TARGET, c->saved)) {
		int ret;

		writel((c->saved & ~(PS_AUTO_ENABLE | PS_AUTO | PS_BUSY | PS_FLAGS | PS_ACTUAL |
				     PS_TARGET)) |
		       FIELD_PREP(PS_AUTO, PS_ACTIVE) | FIELD_PREP(PS_TARGET, PS_ACTIVE), c->ps);
		ret = readl_poll_timeout_atomic(c->ps, val,
						FIELD_GET(PS_ACTUAL, val) == PS_ACTIVE,
						10, PS_TIMEOUT_US);
		dev_info(c->dev, "commit stop: GFX idle under automatic power, raised for the stop: %#x (%d)\n",
			 val, ret);
		c->saved = val;
	}
	if (FIELD_GET(PS_ACTUAL, c->saved) != PS_ACTIVE &&
	    (!FIELD_GET(PS_ACTUAL, c->saved) || !automatic)) {
		/* Off, or not raised: its mailboxes can't be read. The wake will not restart it. */
		dev_warn(c->dev, "commit stop: GFX off (%#x), no idle sent: the GPU's firmware is not stopped for this sleep\n",
			 c->saved);
		c->idle[0] = c->idle[1] = -ENODEV;
	} else {
		for (i = 0; i < c->count; i++) {
			c->idle[i] = apple_rtkit_sleep_idle_polled(c->rtk[i], c->cpu[i],
								   &c->idle_us[i]);
			c->status[i] = readl(c->cpu[i] + ASC_CPU_STATUS);
			apple_rtkit_mailbox_recv_off(c->rtk[i]);
			apple_mbox_ascwrap_v6_stop_cpu_atomic(c->mbox[i]);
		}
		c->saved = readl(c->ps);
	}
	/* The wake puts automatic power back as the stop found it. */
	c->saved = (c->saved & ~PS_AUTO_ENABLE) | automatic;
	/*
	 * The GPU's PMP request (AGX, PTD request bit 15) goes before GFX does, as in the Neo's stop
	 * above: before the sleep the only PTD request left is ANS's. Left on here, the PMP's
	 * firmware crashes before its own stop (AP IDLE never echoed, a crashlog message instead).
	 */
	c->request_ret = c->request ? apple_pmp_report_entry_withdraw_polled(c->request) : 0;
	dev_info(c->dev, "commit stop: PMP request withdrawn: %d\n", c->request_ret);
	writel(c->saved & ~(PS_AUTO_ENABLE | PS_BUSY | PS_FLAGS), c->ps);
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

	/*
	 * As the bus says (nonposted-mmio): on T6030 a posted write to the GPU's ASC page or its
	 * PMGR word took the SoC down (J516s: PMU notes 0x21, 0x23, 0x24 at those writes).
	 */
	if (res.flags & IORESOURCE_MEM_NONPOSTED)
		return ioremap_np(res.start + offset, size);
	return ioremap(res.start + offset, size);
}

static void asahi_g17_commit_free(struct asahi_g17_commit *c)
{
	if (c->request) {
		if (c->request_owned)
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
	c->raise = asahi_g17_commit_raises(dev);

	ret = -EOPNOTSUPP;
	for (i = 0; i < c->count; i++) {
		c->cpu[i] = apple_mbox_ascwrap_v6_cpu_control(c->mbox[i]);
		if (!c->rtk[i] || !c->cpu[i])
			goto err;
	}

	ret = -ENODEV;
	if (asahi_g17_m5_provider(dev)) {
		struct device_node *parent;

		ret = asahi_g17_m5_nodes(dev, &np, &c->request, &offset);
		if (ret)
			goto err;
		/* A failed mapping must remain an error, not ERR_PTR(0). */
		ret = -ENODEV;
		parent = of_get_parent(np);
		if (parent)
			c->ps = asahi_g17_map(parent, NULL, 0, offset, 4);
		of_node_put(parent);
		of_node_put(np);
	} else {
		np = asahi_g17_ps_node(dev);
		if (!np)
			goto err;
		if (!of_property_read_u32(np, "reg", &offset) && np->parent)
			c->ps = asahi_g17_map(np->parent, NULL, 0, offset, 4);
		of_node_put(np);
	}
	c->gating = asahi_g17_map(dev->of_node, "sgx", 0, SGX_GATING, 4);
	if (bridge)
		c->bridge = asahi_g17_map(dev->of_node, "sgx", 0, SGX_BRIDGE,
					  4 * SGX_BRIDGE_WORDS);
	if (!c->ps || !c->gating || (bridge && !c->bridge))
		goto err;

	if (!asahi_g17_m5_provider(dev))
		c->request = of_parse_phandle(dev->of_node, "apple,pmp-report-entry", 0);
	if (c->request) {
		ret = apple_pmp_report_entry_commit_owned(c->request, true);
		if (ret && asahi_g17_m5_provider(dev))
			goto err;
		/* Legacy release semantics are retained, even for its ignored error. */
		c->request_owned = true;
	}

	ret = apple_sleep_coproc_register_first(dev, asahi_g17_m5_provider(dev) ?
						asahi_g17_m5_commit_stop : asahi_g17_commit_stop, c);
	if (ret)
		goto err;
	apple_sleep_coproc_set_ready(dev, true);

	return c;

err:
	asahi_g17_commit_free(c);
	return ERR_PTR(ret);
}

/* The M5 Pro: its GPU firmware is stopped by the sleep's last steps, not by .suspend. */
bool asahi_g17_commit_stops_firmware(void)
{
	return apple_sleep_v27_late();
}

/* Whether only the sleep's commit may stop the GPU: then nothing else may. */
bool asahi_g17_commit_required(void)
{
	return apple_sleep_commit_stops_coprocs();
}

/*
 * What a G16 stop touches: one firmware instance, whose control registers are the ASC's own,
 * no bridge, the GFX word, and the PMP's request for the GPU where the device tree names one
 * (apple,pmp-report-entry). Without one the vote stays through the sleep, as on T8132.
 */
static struct asahi_g17_commit *asahi_g16_commit_alloc(struct device *dev,
						       struct apple_rtkit *rtk)
{
	struct asahi_g17_commit *c;
	struct device_node *np;
	u32 offset;

	c = kzalloc_obj(*c);
	if (!c)
		return ERR_PTR(-ENOMEM);
	c->dev = dev;
	c->rtk[0] = rtk;
	c->count = 1;
	c->raise = asahi_g17_commit_raises(dev);

	c->cpu[0] = asahi_g17_map(dev->of_node, "asc", 0, ASC_CPU_CONTROL, 8);
	np = of_parse_phandle(dev->of_node, "power-domains", 0);
	if (np && !of_property_read_u32(np, "reg", &offset) && np->parent)
		c->ps = asahi_g17_map(np->parent, NULL, 0, offset, 4);
	of_node_put(np);
	if (!c->cpu[0] || !c->ps) {
		asahi_g17_commit_free(c);
		return ERR_PTR(-ENODEV);
	}
	c->request = of_parse_phandle(dev->of_node, "apple,pmp-report-entry", 0);

	return c;
}

/*
 * DIAGNOSTIC (T6030 sleep bring-up): a G16 sleep step in PMU scratch +4 (0x830c), which a reset
 * keeps, through the SPMI nvmem and its locking: from .prepare and .complete, while the SPMI
 * driver runs. Not the sleep driver's raw notes, which only its commit may write.
 */
void asahi_g16_sleep_note(u8 step)
{
	static struct nvmem_device *nv;

	if (!nv) {
		nv = nvmem_device_find((void *)"spmi_nvmem0", device_match_name);
		if (IS_ERR(nv)) {
			nv = NULL;
			return;
		}
	}
	nvmem_device_write(nv, 0x830c, 1, &step);
}

/*
 * G15S's stop at the commit, as far as Linux can make it (J516s, pm_test=devices): AP IDLE and
 * IOP IDLE, the mailbox's receive off, the PMP request withdrawn. After the idle the ASC's own
 * page doesn't answer, and every AP write taking GFX out of automatic power reset the SoC, so
 * neither is touched; the sleep controller takes AFR, GFX's parent, down. An idled G15S
 * firmware answers neither IOP INIT nor IOP ON without a reset, so only the commit, which
 * only a real wake follows, may idle it.
 */
static int asahi_g16_commit_stop_quiet(void *data)
{
	struct asahi_g17_commit *c = data;

	c->ran = true;
	apple_sleep_note_sub(1);
	c->entry = readl(c->ps);
	c->saved = c->entry;
	c->idle[0] = apple_rtkit_sleep_idle_polled(c->rtk[0], NULL, &c->idle_us[0]);
	apple_sleep_note_sub(3);
	apple_rtkit_mailbox_recv_off(c->rtk[0]);
	c->words[0] = apple_rtkit_mailbox_status(c->rtk[0]);
	apple_sleep_note_sub(4);
	c->request_ret = c->request ? apple_pmp_report_entry_withdraw_polled(c->request) : 0;
	apple_sleep_note_sub(5);
	c->lowered = readl(c->ps);

	/* Which step failed, as the sleep driver notes it (+4 = -result): 0xff the idle, 0xfe the request */
	return (c->idle[0] ? 1 : 0) | (c->request_ret ? 2 : 0);
}

/*
 * The same stop for G16, made by the sleep's commit. @quiet: G15S's, which touches neither the
 * ASC's page nor GFX. The mailbox's interrupt stays live until the commit, which turns its
 * receive off (J516s: masked from .prepare instead, the firmware's messages went unanswered
 * through the suspend and its stop failed at the commit, PMU +4 0xff).
 */
struct asahi_g17_commit *asahi_g16_commit_prepare(struct device *dev, struct apple_rtkit *rtk,
						  bool quiet)
{
	struct asahi_g17_commit *c;
	int ret;

	c = asahi_g16_commit_alloc(dev, rtk);
	if (IS_ERR(c))
		return c;
	if (c->request) {
		ret = apple_pmp_report_entry_commit_owned(c->request, true);
		if (ret)
			goto err;
		c->request_owned = true;
	}

	ret = apple_sleep_coproc_register_first(dev, quiet ? asahi_g16_commit_stop_quiet :
							     asahi_g17_commit_stop, c);
	if (ret)
		goto err;
	apple_sleep_coproc_set_ready(dev, true);

	return c;

err:
	asahi_g17_commit_free(c);
	return ERR_PTR(ret);
}

/*
 * The SRAM a G15S stop saves while its firmware still runs and its restart writes back before the
 * run bit, as macOS does (lane A, mac-gpustop-1): one buffer, the GPU being one device.
 */
static u64 *asahi_g16_sram;
static void __iomem *asahi_g16_sram_io;
static size_t asahi_g16_sram_save_len, asahi_g16_sram_restore_len;

static int asahi_g16_sram_save(struct device *dev, u64 offset, size_t save_len, size_t restore_len)
{
	size_t i;

	if (!save_len || restore_len > save_len || (save_len | restore_len | offset) & 7)
		return -EINVAL;
	if (!asahi_g16_sram) {
		asahi_g16_sram = kvmalloc(save_len, GFP_KERNEL);
		asahi_g16_sram_io = asahi_g17_map(dev->of_node, "sgx", 0, offset, save_len);
		if (!asahi_g16_sram || !asahi_g16_sram_io) {
			kvfree(asahi_g16_sram);
			if (asahi_g16_sram_io)
				iounmap(asahi_g16_sram_io);
			asahi_g16_sram = NULL;
			asahi_g16_sram_io = NULL;
			return -ENOMEM;
		}
		asahi_g16_sram_save_len = save_len;
	}
	if (save_len != asahi_g16_sram_save_len)
		return -EINVAL;
	/* In order, 64 bits at a time, as macOS reads it */
	for (i = 0; i < save_len / 8; i++)
		asahi_g16_sram[i] = readq(asahi_g16_sram_io + 8 * i);
	asahi_g16_sram_restore_len = restore_len;
	return 0;
}

/* The restart's write-back, with GFX on and before the run bit: the first @restore_len only. */
int asahi_g16_sram_restore(void)
{
	size_t i;

	if (!asahi_g16_sram || !asahi_g16_sram_restore_len)
		return -ENODATA;
	for (i = 0; i < asahi_g16_sram_restore_len / 8; i++)
		writeq(asahi_g16_sram[i], asahi_g16_sram_io + 8 * i);
	asahi_g16_sram_restore_len = 0;
	return 0;
}

/*
 * DIAGNOSTIC (T6030 sleep bring-up): what a G15S GFX power cycle loses that iBoot set at boot.
 * Read-only snapshots, nonposted, 32 bits at a time, of the windows macOS's stop and wake capture
 * (s2r-trace ref/t6030-macos-gpu-stop-asc-sgx.tsv) didn't hook and the firmware maps (PLAN U4), plus
 * hooked ones as controls: #0 before the stop (firmware running, GFX on), #1 at the restart (GFX
 * back on, PMP request made, SRAM written back, before the run bit), #2 after the handshake. #1 is
 * diffed against #0 and #2 against #1, and #0 is searched for the boot loader's handoff addresses
 * (the reserved regions the GPU node names) in the forms an address register may hold them.
 * aurora.g16_sleep_snap: the windows to take (bit i = window i); aurora.g16_sleep_snap_restore:
 * the windows whose changed words #0's values are written back to, in order, before the run bit.
 * PAs: J516s ADT (gfx-asc reg[0] 0x292400000, reg[1] 0x292050000; sgx reg[0] 0x290000000+0x32a4000,
 * reg[1] 0x290d00000) and the firmware's register map in the live dump (hv-dump-live).
 */
struct asahi_g16_snap_win {
	const char *name;
	u64 pa;
	u32 size;
	bool no_restore;
};

static const struct asahi_g16_snap_win asahi_g16_snap_wins[] = {
	{ "gfx-asc reg[1]", 0x292050000, 0x6c000 },
	{ "asc cpu", 0x292400000, 0x8000 },
	{ "asc above mbox", 0x29240c000, 0x14000 },
	{ "sgx +0", 0x290000000, 0x20000 },
	{ "sgx +0xd04000", 0x290d04000, 0x8000 },
	{ "sgx +0xd10000", 0x290d10000, 0x4000 },
	{ "sgx +0xd40000", 0x290d40000, 0x8000 },
	{ "sgx +0xd4c000", 0x290d4c000, 0xc000 },
	{ "sgx +0xe00000", 0x290e00000, 0x4000 },
	{ "sgx +0xe08000", 0x290e08000, 0x8000 },
	{ "sgx +0xe40000", 0x290e40000, 0x4000 },
	{ "sgx +0xe5c000", 0x290e5c000, 0x8000 },
	{ "sgx +0x1000000", 0x291000000, 0x4000 },
	{ "gpu pmgr ps", 0x290e80000, 0x4000, true },
	{ "gpu pmgr +0x10000", 0x290e90000, 0x6000, true },
	{ "sgx +0x3000000", 0x293000000, 0x400000 },
};

#define G16_SNAPS	3
#define G16_SNAP_WINS	ARRAY_SIZE(asahi_g16_snap_wins)

static u32 *asahi_g16_snap_buf[G16_SNAPS][G16_SNAP_WINS];
static bool asahi_g16_snap_have[G16_SNAPS];
/*
 * Bit 31 of the mask: #0 once the stop has halted the ASC, before GFX goes off, not with the
 * firmware running (J516s: reading the first windows with it running crashed it, FAR in its TEXT).
 */
#define G16_SNAP_LATE	BIT(31)
static u32 asahi_g16_snap_late;

/* Never read: CPU_STATUS (macOS never does). Never written back: CPU_CONTROL, the GPU counters. */
static bool asahi_g16_snap_skip(const struct asahi_g16_snap_win *w, u32 off, bool write)
{
	if (w->pa == 0x292400000 && off == 0x48)
		return true;
	if (!write)
		return false;
	if (w->pa == 0x292400000 && off == 0x44)
		return true;
	return w->pa == 0x290d10000 && off >= 0x1000 && off < 0x1020;
}

static void asahi_g16_snap_needles(struct device *dev, unsigned int n)
{
	static const char *const names[] = { "ttbs", "pagetables", "handoff", "shared-l2",
					     "fw-text", "fw-data" };
	unsigned int r, k, found = 0;
	struct resource res;
	u64 base[ARRAY_SIZE(names)] = { 0 };

	for (r = 0; r < ARRAY_SIZE(names); r++)
		if (!of_reserved_mem_region_to_resource_byname(dev->of_node, names[r], &res))
			base[r] = res.start;

	for (k = 0; k < G16_SNAP_WINS; k++) {
		const struct asahi_g16_snap_win *w = &asahi_g16_snap_wins[k];
		u32 *b = asahi_g16_snap_buf[n][k];
		u32 i;

		if (!b)
			continue;
		for (i = 0; i < w->size / 4; i++) {
			u32 v = b[i];

			if (!v)
				continue;
			for (r = 0; r < ARRAY_SIZE(names); r++) {
				const char *form = NULL;

				if (!base[r])
					continue;
				if (v == lower_32_bits(base[r]))
					form = "lo";
				else if (v == lower_32_bits(base[r] >> 12))
					form = ">>12";
				else if (v == lower_32_bits(base[r] >> 14))
					form = ">>14";
				else if (v == lower_32_bits(base[r] >> 2))
					form = ">>2";
				if (form && found++ < 64)
					dev_info(dev, "G16 snap %u: %s +%#x = %#x is %s %#llx %s\n", n,
						 w->name, 4 * i, v, names[r], base[r], form);
			}
		}
	}
	dev_info(dev, "G16 snap %u: %u words hold a handoff address\n", n, found);
}

static void asahi_g16_snap_take(struct device *dev, unsigned int n, u32 mask)
{
	unsigned int k;

	for (k = 0; k < G16_SNAP_WINS; k++) {
		const struct asahi_g16_snap_win *w = &asahi_g16_snap_wins[k];
		void __iomem *io;
		u32 *b, i;

		if (!(mask & BIT(k)))
			continue;
		b = asahi_g16_snap_buf[n][k];
		if (!b) {
			b = kvzalloc(w->size, GFP_KERNEL);
			if (!b)
				continue;
			asahi_g16_snap_buf[n][k] = b;
		}
		io = ioremap_np(w->pa, w->size);
		if (!io)
			continue;
		/* Where a read took the SoC down, in PMU scratch */
		asahi_g16_sleep_note(0x40 + 0x10 * n + k);
		for (i = 0; i < w->size / 4; i++)
			b[i] = asahi_g16_snap_skip(w, 4 * i, false) ? 0 : readl(io + 4 * i);
		iounmap(io);
	}
	asahi_g16_snap_have[n] = true;
}

static void asahi_g16_snap_diff(struct device *dev, unsigned int a, unsigned int n, u32 mask)
{
	unsigned int k;

	for (k = 0; k < G16_SNAP_WINS; k++) {
		const struct asahi_g16_snap_win *w = &asahi_g16_snap_wins[k];
		u32 *x = asahi_g16_snap_buf[a][k], *y = asahi_g16_snap_buf[n][k];
		u32 i, changed = 0, nonzero = 0;

		if (!(mask & BIT(k)) || !x || !y)
			continue;
		for (i = 0; i < w->size / 4; i++) {
			nonzero += !!y[i];
			if (x[i] == y[i])
				continue;
			if (changed++ < 256)
				dev_info(dev, "G16 snap %u-%u: %s +%#x (%#llx): %#x -> %#x\n", a, n,
					 w->name, 4 * i, w->pa + 4 * i, x[i], y[i]);
		}
		dev_info(dev, "G16 snap %u-%u: %s: %u words changed, %u of %u non-zero\n", a, n,
			 w->name, changed, nonzero, w->size / 4);
	}
}

static void asahi_g16_snap_write_back(struct device *dev, u32 mask)
{
	unsigned int k;

	for (k = 0; k < G16_SNAP_WINS; k++) {
		const struct asahi_g16_snap_win *w = &asahi_g16_snap_wins[k];
		u32 *x = asahi_g16_snap_buf[0][k], *y = asahi_g16_snap_buf[1][k];
		void __iomem *io;
		u32 i, written = 0;

		if (!(mask & BIT(k)) || w->no_restore || !x || !y)
			continue;
		io = ioremap_np(w->pa, w->size);
		if (!io)
			continue;
		asahi_g16_sleep_note(0x70 + k);
		for (i = 0; i < w->size / 4; i++) {
			if (x[i] == y[i] || asahi_g16_snap_skip(w, 4 * i, true))
				continue;
			writel(x[i], io + 4 * i);
			written++;
		}
		/* What the write-back left, for the log */
		for (i = 0; i < w->size / 4; i++)
			if (x[i] != y[i] && !asahi_g16_snap_skip(w, 4 * i, true))
				y[i] = readl(io + 4 * i);
		iounmap(io);
		dev_info(dev, "G16 snap: %s: %u words written back\n", w->name, written);
	}
}

/*
 * @when 0: before the stop; 1: at the restart, before the run bit (diffs against 0, then writes
 * back @restore's windows); 2: after the handshake (diffs against 1).
 */
void asahi_g16_snap(struct device *dev, unsigned int when, u32 mask, u32 restore)
{
	if (!mask || when >= G16_SNAPS)
		return;
	if (!when && (mask & G16_SNAP_LATE)) {
		asahi_g16_snap_late = mask & ~G16_SNAP_LATE;
		return;
	}
	if (when && !asahi_g16_snap_have[when - 1])
		return;
	asahi_g16_snap_take(dev, when, mask);
	if (!when) {
		asahi_g16_snap_needles(dev, 0);
		return;
	}
	asahi_g16_snap_diff(dev, when - 1, when, mask);
	if (when == 1 && restore) {
		asahi_g16_snap_write_back(dev, restore & mask);
		asahi_g16_snap_diff(dev, 0, 1, restore & mask);
	}
	asahi_g16_snap_have[when - 1] = false;
	if (when == 2)
		asahi_g16_snap_have[2] = false;
}

/* The mailbox +0x0 bit macOS waits for after the receive goes off (0x2c -> 0x2e) */
#define G16_MBOX_STOPPED	BIT(1)

/*
 * The G15S stop as macOS 27.2 makes it on J516s, in its device sleep before any display request
 * goes, about 4.2 s before the commit (lane A's mac-gpustop-1 with the ASC page and sgx hooked,
 * s2r-trace ref/t6030-macos-gpu-stop-asc-sgx.tsv; ms before the commit):
 *  1. -4398 to -4240: the SRAM in sgx reg[1] read in order, 64 bits at a time (@save_len at
 *     @sram_offset in "sgx"), with the firmware running;
 *  2. -4239.87: AP IDLE (0x201), echoed; IOP IDLE, acknowledged at -4239.58;
 *  3. -4239.56: the mailbox's receive interrupt off (I2A control 0x22201 -> 0x22200), then its
 *     +0x0 polled until bit 1 (0x2c -> 0x2e, 1.26 ms);
 *  4. -4238.29: ASC CPU_CONTROL read (0x10), written 0, read, written 0; CPU_STATUS is never read;
 *  5. -4238.27: the PMP's AGX request (PTD request bit 4) withdrawn;
 *  6. -4238.04: the GFX word out of automatic power with its target on (0x0f0000ff), then its
 *     target off (0x0f0003f0); it reads 0x0f000100.
 * The restart is the reverse (asahi_g16_sram_restore() and the driver's warm restart): GFX on,
 * automatic power, the AGX request, the SRAM's first @restore_len written back, the receive on, the
 * run bit, and the firmware's own HELLO.
 *
 * Interrupts on, in .prepare, before anything else suspends. Told to idle, the firmware can't
 * carry on, so the stop goes on whatever it answers, and only the restart brings it back.
 */
struct asahi_g17_commit *asahi_g16_stop_now(struct device *dev, struct apple_rtkit *rtk,
					    u64 sram_offset, size_t save_len, size_t restore_len)
{
	struct asahi_g17_commit *c;
	unsigned int i;
	u32 val;

	c = asahi_g16_commit_alloc(dev, rtk);
	if (IS_ERR(c))
		return c;

	/* The step reached, in PMU scratch, which a reset keeps */
	c->ran = true;
	asahi_g16_sleep_note(0x21);
	c->raise_ret = save_len ? asahi_g16_sram_save(dev, sram_offset, save_len, restore_len) : 0;
	asahi_g16_sleep_note(0x28);
	c->entry = readl(c->ps);
	c->saved = c->entry;
	c->idle[0] = apple_rtkit_sleep_idle_polled(c->rtk[0], NULL, &c->idle_us[0]);
	asahi_g16_sleep_note(0x22);
	apple_rtkit_mailbox_recv_off(c->rtk[0]);
	for (i = 0; i < 1000; i++) {
		c->words[0] = apple_rtkit_mailbox_status(c->rtk[0]);
		if (c->words[0] & G16_MBOX_STOPPED)
			break;
		udelay(10);
	}
	apple_rtkit_hold_rx(c->rtk[0]);
	asahi_g16_sleep_note(0x23);
	c->status[0] = readl(c->cpu[0]);
	writel(0, c->cpu[0]);
	readl(c->cpu[0]);
	writel(0, c->cpu[0]);
	asahi_g16_sleep_note(0x29);
	if (asahi_g16_snap_late) {
		asahi_g16_snap_take(dev, 0, asahi_g16_snap_late);
		asahi_g16_snap_needles(dev, 0);
		asahi_g16_snap_late = 0;
	}
	c->request_ret = c->request ? apple_pmp_report_entry_request(c->request, false) : 0;
	asahi_g16_sleep_note(0x24);

	val = readl(c->ps);
	writel(val & ~(PS_AUTO_ENABLE | PS_BUSY | PS_FLAGS), c->ps);
	val = readl(c->ps);
	c->words[1] = val;
	asahi_g16_sleep_note(0x25);
	writel((val & ~(PS_AUTO_ENABLE | PS_BUSY | PS_FLAGS | PS_TARGET)) | PS_FLAGS, c->ps);
	asahi_g16_sleep_note(0x26);
	c->lower_ret = readl_poll_timeout(c->ps, c->lowered,
					  !FIELD_GET(PS_ACTUAL, c->lowered) &&
					  !(c->lowered & PS_BUSY),
					  10, PS_TIMEOUT_US);
	asahi_g16_sleep_note(0x27);

	dev_info(dev, "GPU stopped for the sleep: SRAM %d, idle %d in %u us, mailbox %#x, control %#x, PMP request %d, GFX %#x, %#x then %#x (%d)\n",
		 c->raise_ret, c->idle[0], c->idle_us[0], c->words[0], c->status[0],
		 c->request_ret, c->entry, c->words[1], c->lowered, c->lower_ret);

	return c;
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
		if (c->raise)
			dev_info(c->dev, "GPU domain at the commit stop: %#x, raise to full %d\n",
				 c->entry, c->raise_ret);
		*saved = c->saved;
	}
	asahi_g17_commit_free(c);

	return stopped;
}
