// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T6030 PMP device-state (PTD) requests as power domains
 *
 * The power management coprocessor (PMP) of the T6030 serves a device-state
 * table in which the application processor requests SoC devices and the PMP
 * acknowledges them. The external display controllers DISPEXT0/1 share one
 * request entry with the internal display (DISP) and the ANS; their PMGR
 * power chains may only be powered once the PMP has acknowledged them, or
 * the PMP and the PMGR disagree about the controller's power.
 *
 * Each DISPEXT gets a power domain here, the parent of the first domain of
 * its PMGR chain, so whatever powers the chain first (a DART, the mailbox or
 * the coprocessor itself) obtains the acknowledgement first. Powering it on
 * adds the controller, DISP and ANS to the existing request with one 64-bit
 * write and waits for the PMP to acknowledge DISP and the controller (ANS
 * needs no acknowledgement). Only the acknowledgement is polled: a read of
 * the request entry may consume the PMP's change notification, so the
 * request is read again only once acknowledged. The request is retained
 * once acknowledged; the PMP has no qualified release for it.
 *
 * The internal display's DISP|ANS request is seeded before the PMP runs and
 * only its acknowledgement is read later, so these domains are the only
 * runtime writer of the entry.
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/spinlock.h>

#define PTD_STATUS		0x10
#define PTD_STATUS_READY	1
#define PTD_ENTRY(i)		((i) * 16)
#define PTD_SET(i)		(0x10000 + (i) * 8)

/* Display request entry and its acknowledgement (PTD ranges 10 and 11). */
#define PTD_DISPLAY_REQ		280
#define PTD_DISPLAY_ACK		284
#define PTD_DISP		BIT_ULL(7)
#define PTD_ANS			BIT_ULL(16)

/* The specifier cell is the controller's request bit: 8 DISPEXT0, 9 DISPEXT1. */
#define PTD_FIRST_EXT		8
#define PTD_NR_EXT		2
#define PTD_EXT_MASK		GENMASK_ULL(PTD_FIRST_EXT + PTD_NR_EXT - 1, PTD_FIRST_EXT)

#define PTD_READY_TIMEOUT_US	2000000
#define PTD_ACK_POLL_US		10
#define PTD_ACK_TIMEOUT_US	200000

struct apple_pmp_ptd;

struct apple_pmp_ptd_domain {
	struct generic_pm_domain genpd;
	struct apple_pmp_ptd *ptd;
	u64 device;
};

struct apple_pmp_ptd {
	struct device *dev;
	void __iomem *base;
	/* Serializes the request entry between the controllers' domains. */
	spinlock_t lock;
	struct apple_pmp_ptd_domain ext[PTD_NR_EXT];
	struct generic_pm_domain *domains[PTD_FIRST_EXT + PTD_NR_EXT];
	struct genpd_onecell_data onecell;
	/* The external requests withdrawn for system sleep. */
	u64 sleep_ext;
};

#define genpd_to_ptd_domain(g) container_of(g, struct apple_pmp_ptd_domain, genpd)

static int apple_pmp_ptd_power_on(struct generic_pm_domain *genpd)
{
	struct apple_pmp_ptd_domain *d = genpd_to_ptd_domain(genpd);
	struct apple_pmp_ptd *ptd = d->ptd;
	void __iomem *req_reg = ptd->base + PTD_ENTRY(PTD_DISPLAY_REQ);
	void __iomem *ack_reg = ptd->base + PTD_ENTRY(PTD_DISPLAY_ACK);
	u64 sel = PTD_DISP | d->device;
	u64 req, ack = 0, want = 0;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&ptd->lock, flags);
	if (readq(ptd->base + PTD_STATUS) != PTD_STATUS_READY) {
		ret = -ENODEV;
		goto out;
	}
	ack = readq(ack_reg);
	/* Acknowledged before: the request is retained. */
	if ((ack & sel) == sel)
		goto out;
	req = readq(req_reg);
	/*
	 * A disagreement between request and acknowledgement in these bits is
	 * not settled authority, except this controller's own earlier request
	 * still waiting for its acknowledgement.
	 */
	if ((req ^ ack) & sel & ~(req & d->device)) {
		ret = -EBUSY;
		goto out;
	}
	want = req | sel | PTD_ANS;
	if (want != req)
		writeq(want, ptd->base + PTD_SET(PTD_DISPLAY_REQ));
	ret = readq_poll_timeout_atomic(ack_reg, ack, (ack & sel) == sel,
					PTD_ACK_POLL_US, PTD_ACK_TIMEOUT_US);
	if (!ret && (readq(req_reg) & want) != want)
		ret = -EIO;
out:
	spin_unlock_irqrestore(&ptd->lock, flags);
	if (ret)
		dev_err(ptd->dev, "%s request not acknowledged: want %#llx ack %#llx: %d\n",
			genpd->name, want, ack, ret);
	else if (want)
		dev_info(ptd->dev, "%s acknowledged: req %#llx ack %#llx\n",
			 genpd->name, want, ack);
	return ret;
}

/* The request is retained: the PMP has no qualified release for it. */
static int apple_pmp_ptd_power_off(struct generic_pm_domain *genpd)
{
	return 0;
}

/*
 * System sleep. The external controllers' requests keep the PMP's external
 * display state up while nothing is shown (DISP_EXT 3 with both requested, 0
 * without), which costs about 0.25 W in suspend-to-idle. By the noirq phase
 * every display controller has suspended and its displays are off: withdraw
 * the acknowledged external requests with one replacement write, as macOS
 * does when it releases a device, and request them again before the
 * controllers resume. The request is read once before the write, never
 * between the write and its acknowledgement. DISP and ANS, the PMGR chains
 * and the coprocessors stay as they are.
 */
static int apple_pmp_ptd_update_ext(struct apple_pmp_ptd *ptd, u64 mask, bool on)
{
	void __iomem *ack_reg = ptd->base + PTD_ENTRY(PTD_DISPLAY_ACK);
	unsigned long flags;
	u64 req, want, ack;
	int ret;

	spin_lock_irqsave(&ptd->lock, flags);
	req = readq(ptd->base + PTD_ENTRY(PTD_DISPLAY_REQ));
	want = on ? req | mask : req & ~mask;
	writeq(want, ptd->base + PTD_SET(PTD_DISPLAY_REQ));
	ret = readq_poll_timeout_atomic(ack_reg, ack, !((ack ^ want) & mask),
					PTD_ACK_POLL_US, PTD_ACK_TIMEOUT_US);
	spin_unlock_irqrestore(&ptd->lock, flags);
	if (ret)
		dev_err(ptd->dev, "display request %#llx not acknowledged: ack %#llx\n",
			want, ack);
	return ret;
}

static int apple_pmp_ptd_suspend_noirq(struct device *dev)
{
	struct apple_pmp_ptd *ptd = dev_get_drvdata(dev);
	u64 mask;

	if (readq(ptd->base + PTD_STATUS) != PTD_STATUS_READY)
		return 0;
	mask = readq(ptd->base + PTD_ENTRY(PTD_DISPLAY_ACK)) & PTD_EXT_MASK;
	if (!mask)
		return 0;
	/* A failed withdrawal is asserted again on resume all the same. */
	apple_pmp_ptd_update_ext(ptd, mask, false);
	ptd->sleep_ext = mask;
	return 0;
}

static int apple_pmp_ptd_resume_noirq(struct device *dev)
{
	struct apple_pmp_ptd *ptd = dev_get_drvdata(dev);
	u64 mask = ptd->sleep_ext;

	if (!mask)
		return 0;
	ptd->sleep_ext = 0;
	return apple_pmp_ptd_update_ext(ptd, mask, true);
}

static const struct dev_pm_ops apple_pmp_ptd_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(apple_pmp_ptd_suspend_noirq, apple_pmp_ptd_resume_noirq)
};

/* The PMP serves the table once its firmware runs, started by the PMP driver. */
static int apple_pmp_ptd_wait_ready(struct apple_pmp_ptd *ptd)
{
	struct device_node *np;
	struct platform_device *pmp;
	bool bound;
	u64 status;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "apple,t6030-pmp-v2");
	pmp = np ? of_find_device_by_node(np) : NULL;
	of_node_put(np);
	if (!pmp)
		return dev_err_probe(ptd->dev, -EPROBE_DEFER, "waiting for the PMP\n");
	bound = device_is_bound(&pmp->dev) &&
		device_link_add(ptd->dev, &pmp->dev, DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(&pmp->dev);
	if (!bound)
		return dev_err_probe(ptd->dev, -EPROBE_DEFER, "waiting for the PMP driver\n");

	ret = readq_poll_timeout(ptd->base + PTD_STATUS, status, status == PTD_STATUS_READY,
				 1000, PTD_READY_TIMEOUT_US);
	if (ret)
		return dev_err_probe(ptd->dev, ret, "PTD not ready: status %#llx\n", status);
	return 0;
}

static void apple_pmp_ptd_remove_domains(struct apple_pmp_ptd *ptd)
{
	for (unsigned int i = 0; i < PTD_NR_EXT; i++)
		if (ptd->domains[PTD_FIRST_EXT + i])
			pm_genpd_remove(ptd->domains[PTD_FIRST_EXT + i]);
}

static int apple_pmp_ptd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_pmp_ptd *ptd;
	u64 ack;
	int ret;

	ptd = devm_kzalloc(dev, sizeof(*ptd), GFP_KERNEL);
	if (!ptd)
		return -ENOMEM;
	ptd->dev = dev;
	spin_lock_init(&ptd->lock);
	platform_set_drvdata(pdev, ptd);

	ptd->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ptd->base))
		return PTR_ERR(ptd->base);

	ret = apple_pmp_ptd_wait_ready(ptd);
	if (ret)
		return ret;

	ack = readq(ptd->base + PTD_ENTRY(PTD_DISPLAY_ACK));
	for (unsigned int i = 0; i < PTD_NR_EXT; i++) {
		struct apple_pmp_ptd_domain *d = &ptd->ext[i];
		u64 sel;

		d->ptd = ptd;
		d->device = BIT_ULL(PTD_FIRST_EXT + i);
		sel = PTD_DISP | d->device;
		d->genpd.name = devm_kasprintf(dev, GFP_KERNEL, "dispext%u_ptd", i);
		if (!d->genpd.name) {
			ret = -ENOMEM;
			goto err;
		}
		/* The PMGR domains below are IRQ safe, so their parent must be. */
		d->genpd.flags = GENPD_FLAG_IRQ_SAFE;
		d->genpd.power_on = apple_pmp_ptd_power_on;
		d->genpd.power_off = apple_pmp_ptd_power_off;
		ret = pm_genpd_init(&d->genpd, NULL, (ack & sel) != sel);
		if (ret)
			goto err;
		ptd->domains[PTD_FIRST_EXT + i] = &d->genpd;
	}

	ptd->onecell.domains = ptd->domains;
	ptd->onecell.num_domains = ARRAY_SIZE(ptd->domains);
	ret = of_genpd_add_provider_onecell(dev->of_node, &ptd->onecell);
	if (ret)
		goto err;

	dev_info(dev, "PMP device-state requests ready: display ack %#llx\n", ack);
	return 0;
err:
	apple_pmp_ptd_remove_domains(ptd);
	return ret;
}

static const struct of_device_id apple_pmp_ptd_of_match[] = {
	{ .compatible = "apple,t6030-pmp-ptd" },
	{}
};
MODULE_DEVICE_TABLE(of, apple_pmp_ptd_of_match);

static struct platform_driver apple_pmp_ptd_driver = {
	.probe = apple_pmp_ptd_probe,
	.driver = {
		.name = "apple-pmp-ptd",
		.of_match_table = apple_pmp_ptd_of_match,
		.pm = pm_sleep_ptr(&apple_pmp_ptd_pm_ops),
		/* A provider of retained requests has no teardown. */
		.suppress_bind_attrs = true,
	},
};
/*
 * Power domains in use cannot go away: the module has no exit and stays
 * loaded once its provider is registered.
 */
static int __init apple_pmp_ptd_init(void)
{
	return platform_driver_register(&apple_pmp_ptd_driver);
}
module_init(apple_pmp_ptd_init);

MODULE_AUTHOR("Eryk Wieliczko <eryk@wieliczko.ninja>");
MODULE_DESCRIPTION("Apple T6030 PMP device-state power domains");
MODULE_LICENSE("Dual MIT/GPL");
