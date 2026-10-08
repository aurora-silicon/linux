// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC PMP power state reporting driver
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/debugfs.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/seq_file.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/rculist.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/soc/apple/pmp-report.h>

#define PMP_REPORT_READY 0x1

/**
 * struct apple_pmp_report_offsets - per-SoC layout of the report region
 * @tgt_read: power state request, read side
 * @tgt_write: power state request, write side
 * @actual: power state acknowledgment
 * @status: PMP status; bit 0 is set while the PMP is running
 * @fast_die: 32-bit word holding the fast die-temperature effort of each
 *	CPU cluster, one byte per lane; 0 if the SoC has none
 * @fast_die_lanes: number of lanes at @fast_die, at most 4
 */
struct apple_pmp_report_offsets {
	u32 tgt_read;
	u32 tgt_write;
	u32 actual;
	u32 status;
	u32 fast_die;
	u32 fast_die_lanes;
};

struct apple_pmp_report;

static int apple_pmp_dvfs_create(struct apple_pmp_report *report);

struct apple_pmp_report {
	struct device *dev;
	const struct apple_pmp_report_offsets *offsets;
	void __iomem *base;
	spinlock_t lock;
	struct list_head list;
	struct dentry *debugfs;
};

/* Bound report regions, protected by RCU until managed teardown drains readers. */
static LIST_HEAD(apple_pmp_reports);
static DEFINE_MUTEX(apple_pmp_reports_lock);
static struct dentry *apple_pmp_report_debugfs;

/**
 * apple_pmp_report_fast_die_effort() - read the fast die-temperature effort
 *	of a CPU cluster
 * @np: device tree node of the report region
 * @lane: lane of the cluster in the fast die-temperature report
 * @effort: set to the effort on success
 *
 * The PMP of some SoCs runs a control loop on its own die-temperature sensors
 * and publishes an 8-bit effort for each CPU cluster: 0 requests no limit,
 * and the effort rises towards 255 as the die exceeds the firmware's target.
 * The effort is only valid while the PMP reports that it is running; a
 * stopped PMP reads 0.
 *
 * Context: Any context.
 * Return: 0 on success, -EPROBE_DEFER if the region is not bound (yet),
 * -EOPNOTSUPP if the SoC's PMP publishes no such effort, -EINVAL if @lane is
 * out of range, or -EAGAIN if the PMP does not report that it is running.
 */
int apple_pmp_report_fast_die_effort(const struct device_node *np,
				     unsigned int lane, u8 *effort)
{
	const struct apple_pmp_report_offsets *offs;
	const struct apple_pmp_report *rep;
	int ret = -EPROBE_DEFER;

	rcu_read_lock();
	list_for_each_entry_rcu(rep, &apple_pmp_reports, list) {
		if (rep->dev->of_node != np)
			continue;

		offs = rep->offsets;
		if (!offs->fast_die) {
			ret = -EOPNOTSUPP;
		} else if (lane >= offs->fast_die_lanes) {
			ret = -EINVAL;
		} else if (!(readl(rep->base + offs->status) & PMP_REPORT_READY)) {
			ret = -EAGAIN;
		} else {
			*effort = readl(rep->base + offs->fast_die) >>
				  (lane * BITS_PER_BYTE);
			ret = 0;
		}
		break;
	}
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(apple_pmp_report_fast_die_effort);

/* Read only the documented status and fast-die entries, never the aperture. */
static int apple_pmp_report_status_show(struct seq_file *s, void *unused)
{
	struct apple_pmp_report *rep = s->private;
	const struct apple_pmp_report_offsets *offs = rep->offsets;
	u32 status = readl(rep->base + offs->status);
	unsigned int lane;
	u32 effort;

	seq_printf(s, "running: %u\n", !!(status & PMP_REPORT_READY));
	if (!(status & PMP_REPORT_READY) || !offs->fast_die)
		return 0;

	effort = readl(rep->base + offs->fast_die);
	for (lane = 0; lane < offs->fast_die_lanes; lane++)
		seq_printf(s, "lane%u: %u\n", lane,
			   (effort >> (lane * BITS_PER_BYTE)) & 0xff);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_pmp_report_status);

static void apple_pmp_report_release(void *data)
{
	struct apple_pmp_report *rep = data;

	/* Withdraw child providers while their power callbacks can still use MMIO. */
	of_platform_depopulate(rep->dev);
	mutex_lock(&apple_pmp_reports_lock);
	if (!list_empty(&rep->list))
		list_del_rcu(&rep->list);
	mutex_unlock(&apple_pmp_reports_lock);
	synchronize_rcu();
	/* The debugfs proxy drains active reads and rejects subsequent accesses. */
	debugfs_remove(rep->debugfs);
}

static int apple_pmp_report_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct apple_pmp_report *rep;
	int ret;

	rep = devm_kzalloc(dev, sizeof(*rep), GFP_KERNEL);
	if (!rep)
		return -ENOMEM;

	rep->dev = dev;
	spin_lock_init(&rep->lock);
	rep->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rep->base))
		return PTR_ERR(rep->base);
	rep->offsets = of_device_get_match_data(dev);
	dev_set_drvdata(dev, rep);
	ret = apple_pmp_dvfs_create(rep);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&rep->list);
	ret = devm_add_action_or_reset(dev, apple_pmp_report_release, rep);
	if (ret)
		return ret;

	mutex_lock(&apple_pmp_reports_lock);
	list_add_tail_rcu(&rep->list, &apple_pmp_reports);
	mutex_unlock(&apple_pmp_reports_lock);

	if (rep->offsets->fast_die)
		rep->debugfs = debugfs_create_file(dev_name(dev), 0400,
						   apple_pmp_report_debugfs, rep,
						   &apple_pmp_report_status_fops);

	/* Publish the region before child power callbacks start looking it up. */
	ret = of_platform_populate(np, NULL, NULL, dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to create child devices\n");

	return 0;
}

static const struct apple_pmp_report_offsets apple_pmp_offsets_t600x = {
	.tgt_read = 0xf80,
	.tgt_write = 0x107c0,
	.actual = 0x1000,
	.status = 0x10,
};

static const struct apple_pmp_report_offsets apple_pmp_offsets_t602x = {
	.tgt_read = 0x2000,
	.tgt_write = 0x11000,
	.actual = 0x2080,
	.status = 0x10,
};

static const struct apple_pmp_report_offsets apple_pmp_offsets_t8112 = {
	.tgt_read = 0xa00,
	.tgt_write = 0x10500,
	.actual = 0xa40,
	.status = 0x10,
};

/* The fast die-temperature report is the low two bytes of entry 0x1c1. */
static const struct apple_pmp_report_offsets apple_pmp_offsets_t8140 = {
	.tgt_read = 0x1880,
	.tgt_write = 0x10c40,
	.actual = 0x18c0,
	.status = 0x10,
	.fast_die = 0x1c10,
	.fast_die_lanes = 2,
};

static const struct of_device_id apple_pmp_report_of_match[] = {
	{ .compatible = "apple,t6000-pmp-v2-report", .data = &apple_pmp_offsets_t600x },
	{ .compatible = "apple,t6020-pmp-v2-report", .data = &apple_pmp_offsets_t602x },
	{ .compatible = "apple,t8112-pmp-v2-report", .data = &apple_pmp_offsets_t8112 },
	{ .compatible = "apple,t8140-pmp-v2-report", .data = &apple_pmp_offsets_t8140 },
	{}
};

static struct platform_driver apple_pmp_report_driver = {
	.probe = apple_pmp_report_probe,
	.driver = {
		.name = "apple-pmp-report",
		.of_match_table = apple_pmp_report_of_match,
		/* Power-domain providers are not intended for manual unbinding. */
		.suppress_bind_attrs = true,
	},
};

struct apple_pmp_report_entry {
	struct device_node *node;
	struct device_node *report_node;
	struct generic_pm_domain genpd;
	u32 id;
	bool removed;
};

#define genpd_to_apple_pmp_report_entry(_genpd) \
	container_of(_genpd, struct apple_pmp_report_entry, genpd)

static int apple_pmp_report_set_state(struct apple_pmp_report *rep, u32 id,
				      bool enable)
{
	u64 bit_val = BIT_ULL(id);
	u64 val;
	unsigned long flags;

	spin_lock_irqsave(&rep->lock, flags);
	val = readq(rep->base + rep->offsets->tgt_read);
	val &= ~bit_val;
	if (enable)
		val |= bit_val;
	writeq(val, rep->base + rep->offsets->tgt_write);
	spin_unlock_irqrestore(&rep->lock, flags);
	val = readq(rep->base + rep->offsets->status);
	if ((val & PMP_REPORT_READY) == 0)
		return 0;
	return readq_poll_timeout_atomic(
		rep->base + rep->offsets->actual,
		val,
		!!(val & bit_val) == !!enable,
		100,
		50000);
}

static int apple_pmp_report_entry_set_state(struct generic_pm_domain *genpd, bool enable)
{
	struct apple_pmp_report_entry *ent = genpd_to_apple_pmp_report_entry(genpd);
	struct apple_pmp_report *rep;
	int ret = -ENODEV;

	if (READ_ONCE(ent->removed))
		return ret;

	rcu_read_lock();
	list_for_each_entry_rcu(rep, &apple_pmp_reports, list) {
		if (rep->dev->of_node == ent->report_node) {
			ret = apple_pmp_report_set_state(rep, ent->id, enable);
			break;
		}
	}
	rcu_read_unlock();
	return ret;
}

static int apple_pmp_report_entry_power_on(struct generic_pm_domain *genpd)
{
	return apple_pmp_report_entry_set_state(genpd, true);
}

static int apple_pmp_report_entry_power_off(struct generic_pm_domain *genpd)
{
	return apple_pmp_report_entry_set_state(genpd, false);
}

/* ADT DVFS-STATE: four 16-byte entries beginning at entry 8. */
#define APPLE_PMP_DVFS_OFFSET	0x80
#define APPLE_PMP_DVFS_STRIDE	16
#define APPLE_PMP_DVFS_COUNT	4

static int apple_pmp_dvfs_show(struct seq_file *seq, void *unused)
{
	struct apple_pmp_report *report = seq->private;
	u64 values[APPLE_PMP_DVFS_COUNT][2];
	unsigned int index;

	if (!(readl(report->base + report->offsets->status) & PMP_REPORT_READY))
		return -EAGAIN;

	for (index = 0; index < ARRAY_SIZE(values); index++) {
		void __iomem *entry = report->base + APPLE_PMP_DVFS_OFFSET +
				     index * APPLE_PMP_DVFS_STRIDE;

		values[index][0] = readq(entry);
		values[index][1] = readq(entry + sizeof(values[index][0]));
	}

	if (!(readl(report->base + report->offsets->status) & PMP_REPORT_READY))
		return -EAGAIN;

	for (index = 0; index < ARRAY_SIZE(values); index++)
		seq_printf(seq, "entry%u: 0x%016llx 0x%016llx\n", index + 8,
			   values[index][0], values[index][1]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_pmp_dvfs);

static void apple_pmp_dvfs_remove(void *directory)
{
	debugfs_remove_recursive(directory);
}

static int apple_pmp_dvfs_create(struct apple_pmp_report *report)
{
	struct device *dev = report->dev;
	struct resource *resource;
	struct dentry *directory;
	char *name;
	int ret;

	if (!of_device_is_compatible(dev->of_node, "apple,t8140-pmp-v2-report"))
		return 0;

	resource = platform_get_resource(to_platform_device(dev), IORESOURCE_MEM, 0);
	if (resource_size(resource) < APPLE_PMP_DVFS_OFFSET +
				      APPLE_PMP_DVFS_COUNT * APPLE_PMP_DVFS_STRIDE)
		return dev_err_probe(dev, -EINVAL, "DVFS report exceeds resource\n");

	name = devm_kasprintf(dev, GFP_KERNEL, "apple-pmp-%s", dev_name(dev));
	if (!name)
		return -ENOMEM;

	directory = debugfs_create_dir(name, NULL);
	if (IS_ERR(directory))
		return 0;

	ret = devm_add_action_or_reset(dev, apple_pmp_dvfs_remove, directory);
	if (ret)
		return ret;

	debugfs_create_file("dvfs", 0400, directory, report, &apple_pmp_dvfs_fops);
	return 0;
}

static void apple_pmp_report_entry_free(struct apple_pmp_report_entry *ent)
{
	of_node_put(ent->report_node);
	of_node_put(ent->node);
	kfree(ent->genpd.name);
	kfree(ent);
}

static void apple_pmp_report_entry_remove(struct platform_device *pdev)
{
	struct apple_pmp_report_entry *ent = platform_get_drvdata(pdev);

	WRITE_ONCE(ent->removed, true);
	of_genpd_del_provider(ent->node);
	if (pm_genpd_remove(&ent->genpd)) {
		/* The core still owns this domain. Keep its now inert storage alive. */
		dev_warn(&pdev->dev, "retaining busy power domain %s\n", ent->genpd.name);
		return;
	}
	apple_pmp_report_entry_free(ent);
}

static int apple_pmp_report_entry_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct apple_pmp_report_entry *ent;
	int ret;
	const char *name;
	struct of_phandle_iterator it;

	ent = kzalloc_obj(*ent);
	if (!ent)
		return -ENOMEM;

	ent->node = of_node_get(node);
	ent->report_node = of_node_get(dev->parent->of_node);

	ret = of_property_read_u32(node, "reg", &ent->id);
	if (ret) {
		ret = dev_err_probe(dev, ret, "missing reg property\n");
		goto err_free;
	}
	if (ent->id >= 64) {
		ret = -EINVAL;
		goto err_free;
	}

	ret = of_property_read_string(node, "label", &name);
	if (ret < 0) {
		ret = dev_err_probe(dev, ret, "missing label property\n");
		goto err_free;
	}
	ent->genpd.name = kstrdup(name, GFP_KERNEL);
	if (!ent->genpd.name) {
		ret = -ENOMEM;
		goto err_free;
	}

	if (of_property_read_bool(node, "apple,always-on")) {
		ent->genpd.flags |= GENPD_FLAG_ACTIVE_WAKEUP;
		apple_pmp_report_entry_set_state(&ent->genpd, true);
	}

	ent->genpd.power_on = apple_pmp_report_entry_power_on;
	ent->genpd.power_off = apple_pmp_report_entry_power_off;

	ret = pm_genpd_init(&ent->genpd, NULL, true);
	if (ret) {
		ret = dev_err_probe(dev, ret, "pm_genpd_init failed\n");
		goto err_free;
	}

	ret = of_genpd_add_provider_simple(node, &ent->genpd);
	if (ret) {
		ret = dev_err_probe(dev, ret, "of_genpd_add_provider_simple failed\n");
		goto err_remove_domain;
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

	pm_genpd_remove_device(dev);
	platform_set_drvdata(pdev, ent);

	return 0;
err_remove:
	of_genpd_del_provider(node);
err_remove_domain:
	WRITE_ONCE(ent->removed, true);
	if (pm_genpd_remove(&ent->genpd)) {
		dev_warn(dev, "retaining busy power domain %s\n", ent->genpd.name);
		return ret;
	}
err_free:
	apple_pmp_report_entry_free(ent);
	return ret;
}

static const struct of_device_id apple_pmp_report_entry_of_match[] = {
	{ .compatible = "apple,t6000-pmp-v2-report-entry" },
	{}
};

static struct platform_driver apple_pmp_report_entry_driver = {
	.probe = apple_pmp_report_entry_probe,
	.remove = apple_pmp_report_entry_remove,
	.driver = {
		.name = "apple-pmp-report-entry",
		.of_match_table = apple_pmp_report_entry_of_match,
	},
};

MODULE_DEVICE_TABLE(of, apple_pmp_report_of_match);
MODULE_DEVICE_TABLE(of, apple_pmp_report_entry_of_match);

static int __init apple_pmp_report_init(void)
{
	apple_pmp_report_debugfs = debugfs_create_dir("apple-pmp-report", NULL);
	platform_driver_register(&apple_pmp_report_entry_driver);
	platform_driver_register(&apple_pmp_report_driver);
	return 0;
}

static void __exit apple_pmp_report_exit(void)
{
	debugfs_remove_recursive(apple_pmp_report_debugfs);
	platform_driver_unregister(&apple_pmp_report_entry_driver);
	platform_driver_unregister(&apple_pmp_report_driver);
}

module_init(apple_pmp_report_init);
module_exit(apple_pmp_report_exit);

MODULE_DESCRIPTION("PMP power state reporting driver for Apple SoCs");
MODULE_LICENSE("Dual MIT/GPL");
