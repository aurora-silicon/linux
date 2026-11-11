// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC read-only register views
 *
 * Exposes fixed, per-block lists of 32-bit registers through debugfs so that
 * blocks without a functional driver can be inspected. The driver never
 * writes to the hardware. Each list names only registers that are known to
 * be safe to read:
 *
 *  - memory cache controller (T8112, T6000): the carveout bounds and enables
 *    of plane 0 and the cache status of every plane. The boot loader reads
 *    exactly these registers on every boot of these SoCs.
 *  - SPMI sideband (T8112): the bus and queue fault counters. These are
 *    plain 32-bit counters; the interrupt status and acknowledge words of
 *    the window are left out, since clearing them takes a write.
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

/* One run of 32-bit registers inside one DT reg entry. */
struct apple_regview_range {
	u8 reg;		/* index into the node's reg property */
	u32 offset;	/* byte offset from the start of that entry */
	u32 count;	/* number of 32-bit registers */
	u32 stride;	/* byte distance between registers; 0 means 4 */
	const char *name;
};

struct apple_regview_hw {
	const struct apple_regview_range *ranges;
	unsigned int nr_ranges;
	unsigned int nr_regs;	/* reg entries the node must have */
};

struct apple_regview_map {
	void __iomem *base;
	resource_size_t size;
};

static struct dentry *apple_regview_root;

struct apple_regview {
	struct device *dev;
	const struct apple_regview_hw *hw;
	struct apple_regview_map *maps;
	struct dentry *dir;
};

#define MCC_PLANE_STRIDE	0x40000
#define MCC_PLANE_CACHE_STATUS	0x1c04
#define MCC_TZ_BASE		0x6a0
#define MCC_TZ_STRIDE		0x10
#define MCC_TZ_REGIONS		4

/*
 * Plane 0 carveout start, end and enable of each region, then the cache
 * status of each plane.
 */
static const struct apple_regview_range apple_regview_t8112_mcc[] = {
	/* T8112: reg[1] holds the four planes. */
	{ 1, MCC_TZ_BASE + 0x0, MCC_TZ_REGIONS, MCC_TZ_STRIDE, "tz-start" },
	{ 1, MCC_TZ_BASE + 0x4, MCC_TZ_REGIONS, MCC_TZ_STRIDE, "tz-end" },
	{ 1, MCC_TZ_BASE + 0x8, MCC_TZ_REGIONS, MCC_TZ_STRIDE, "tz-enable" },
	{ 1, MCC_PLANE_CACHE_STATUS, 4, MCC_PLANE_STRIDE, "cache-status" },
};

static const struct apple_regview_hw apple_regview_t8112_mcc_hw = {
	.ranges = apple_regview_t8112_mcc,
	.nr_ranges = ARRAY_SIZE(apple_regview_t8112_mcc),
	.nr_regs = 2,
};

/* T6000: one reg entry per controller, each with four planes at offset 0. */
static const struct apple_regview_range apple_regview_t6000_mcc[] = {
	{ 0, MCC_TZ_BASE + 0x0, MCC_TZ_REGIONS, MCC_TZ_STRIDE, "tz-start" },
	{ 0, MCC_TZ_BASE + 0x4, MCC_TZ_REGIONS, MCC_TZ_STRIDE, "tz-end" },
	{ 0, MCC_TZ_BASE + 0x8, MCC_TZ_REGIONS, MCC_TZ_STRIDE, "tz-enable" },
	{ 0, MCC_PLANE_CACHE_STATUS, 4, MCC_PLANE_STRIDE, "cache-status" },
	{ 1, MCC_PLANE_CACHE_STATUS, 4, MCC_PLANE_STRIDE, "cache-status" },
	{ 2, MCC_PLANE_CACHE_STATUS, 4, MCC_PLANE_STRIDE, "cache-status" },
	{ 3, MCC_PLANE_CACHE_STATUS, 4, MCC_PLANE_STRIDE, "cache-status" },
};

static const struct apple_regview_hw apple_regview_t6000_mcc_hw = {
	.ranges = apple_regview_t6000_mcc,
	.nr_ranges = ARRAY_SIZE(apple_regview_t6000_mcc),
	.nr_regs = 4,
};

/*
 * SPMI sideband window (reg[0]): ten bus fault counters (unmapped writes,
 * unsupported reads, missing responses, NACKs, parity and command errors,
 * collisions) followed by seven queue and filter fault counters. Only the
 * counters are listed; how they wrap is not part of the interface.
 */
#define SPMI_SB_BUS_FAULTS	0x200
#define SPMI_SB_NR_BUS_FAULTS	10
#define SPMI_SB_QUEUE_FAULTS	0x25c
#define SPMI_SB_NR_QUEUE_FAULTS	7

static const struct apple_regview_range apple_regview_t8112_spmi_sb[] = {
	{ 0, SPMI_SB_BUS_FAULTS, SPMI_SB_NR_BUS_FAULTS, 0, "bus-fault-count" },
	{ 0, SPMI_SB_QUEUE_FAULTS, SPMI_SB_NR_QUEUE_FAULTS, 0, "queue-fault-count" },
};

static const struct apple_regview_hw apple_regview_t8112_spmi_sb_hw = {
	.ranges = apple_regview_t8112_spmi_sb,
	.nr_ranges = ARRAY_SIZE(apple_regview_t8112_spmi_sb),
	.nr_regs = 1,
};

static u32 apple_regview_stride(const struct apple_regview_range *r)
{
	return r->stride ? r->stride : 4;
}

static int apple_regview_check(const struct apple_regview *rv)
{
	const struct apple_regview_hw *hw = rv->hw;
	unsigned int i;

	for (i = 0; i < hw->nr_ranges; i++) {
		const struct apple_regview_range *r = &hw->ranges[i];
		u64 last;

		if (r->reg >= hw->nr_regs || !r->count)
			return -EINVAL;
		last = r->offset + (u64)(r->count - 1) * apple_regview_stride(r) + 4;
		if (last > rv->maps[r->reg].size)
			return -EINVAL;
	}

	return 0;
}

static int apple_regview_show(struct seq_file *s, void *unused)
{
	struct apple_regview *rv = s->private;
	const struct apple_regview_hw *hw = rv->hw;
	unsigned int i, j;

	for (i = 0; i < hw->nr_ranges; i++) {
		const struct apple_regview_range *r = &hw->ranges[i];
		const struct apple_regview_map *map = &rv->maps[r->reg];
		u32 stride = apple_regview_stride(r);

		for (j = 0; j < r->count; j++) {
			u32 off = r->offset + j * stride;

			seq_printf(s, "%u+0x%07x %s[%u]: 0x%08x\n",
				   r->reg, off, r->name, j,
				   readl_relaxed(map->base + off));
		}
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_regview);

static void apple_regview_remove_debugfs(void *data)
{
	debugfs_remove_recursive(data);
}

static int apple_regview_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_regview *rv;
	unsigned int i;
	int ret;

	rv = devm_kzalloc(dev, sizeof(*rv), GFP_KERNEL);
	if (!rv)
		return -ENOMEM;

	rv->dev = dev;
	rv->hw = of_device_get_match_data(dev);
	if (!rv->hw)
		return -ENODEV;

	rv->maps = devm_kcalloc(dev, rv->hw->nr_regs, sizeof(*rv->maps),
				GFP_KERNEL);
	if (!rv->maps)
		return -ENOMEM;

	for (i = 0; i < rv->hw->nr_regs; i++) {
		struct resource res;

		ret = of_address_to_resource(dev->of_node, i, &res);
		if (ret)
			return dev_err_probe(dev, ret, "missing reg %u\n", i);
		if (resource_size(&res) & 3)
			return dev_err_probe(dev, -EINVAL, "reg %u not word sized\n", i);

		/*
		 * Mapping does not touch the hardware. Other drivers may own
		 * parts of these windows, so they are not requested.
		 */
		rv->maps[i].base = devm_ioremap(dev, res.start, resource_size(&res));
		if (!rv->maps[i].base)
			return -ENOMEM;
		rv->maps[i].size = resource_size(&res);
	}

	ret = apple_regview_check(rv);
	if (ret)
		return dev_err_probe(dev, ret, "register list exceeds the apertures\n");

	rv->dir = debugfs_create_dir(dev_name(dev), apple_regview_root);
	debugfs_create_file("regs", 0400, rv->dir, rv, &apple_regview_fops);

	return devm_add_action_or_reset(dev, apple_regview_remove_debugfs,
					rv->dir);
}

static const struct of_device_id apple_regview_of_match[] = {
	{ .compatible = "apple,t8112-mcc", .data = &apple_regview_t8112_mcc_hw },
	{ .compatible = "apple,t6000-mcc", .data = &apple_regview_t6000_mcc_hw },
	{ .compatible = "apple,t8112-spmi-sideband",
	  .data = &apple_regview_t8112_spmi_sb_hw },
	{}
};
MODULE_DEVICE_TABLE(of, apple_regview_of_match);

static struct platform_driver apple_regview_driver = {
	.driver = {
		.name = "apple-regview",
		.of_match_table = apple_regview_of_match,
	},
	.probe = apple_regview_probe,
};

static int __init apple_regview_init(void)
{
	int ret;

	apple_regview_root = debugfs_create_dir("apple-regview", NULL);
	ret = platform_driver_register(&apple_regview_driver);
	if (ret)
		debugfs_remove_recursive(apple_regview_root);
	return ret;
}
module_init(apple_regview_init);

static void __exit apple_regview_exit(void)
{
	platform_driver_unregister(&apple_regview_driver);
	debugfs_remove_recursive(apple_regview_root);
}
module_exit(apple_regview_exit);

MODULE_DESCRIPTION("Apple SoC read-only register views");
MODULE_LICENSE("Dual MIT/GPL");
