// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC read-only debug register views
 *
 * Root-only debugfs views of register blocks that only debug tools use:
 *
 *  - apple,aic-timebase: the timebase aperture of the interrupt controller
 *  - apple,debug-dock: the debug dock connection status register
 *  - apple,t6000-dwi, apple,t8112-dwi, apple,t8140-dwi: the DWI serial
 *    backlight interface. The panel backlight of these machines is driven by
 *    the display coprocessor and does not use DWI, so these registers only
 *    hold their boot state
 *
 * The driver never writes a device register and requests no interrupt.
 * Registers are read only when a debugfs file is read. A block in a power
 * domain is powered through runtime PM only for the duration of the read.
 */

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <asm/arch_timer.h>

static unsigned int snap_bytes = 0x40;
module_param(snap_bytes, uint, 0444);
MODULE_PARM_DESC(snap_bytes, "Bytes of an MMIO view read by its snapshot file");

struct dbgview {
	struct device *dev;
	void __iomem *base;
	resource_size_t size;
	struct dentry *dir;
};

static struct dentry *dbgview_root;

static u64 dbgview_cntvct(void)
{
	return __arch_counter_get_cntvct_stable();
}

static ssize_t dbgview_regs_read(struct file *file, char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct dbgview *dv = file->private_data;
	loff_t pos = *ppos;
	size_t len, i;
	u32 *buf;
	int ret;

	if (pos < 0 || !IS_ALIGNED(pos, 4))
		return -EINVAL;
	if (pos >= dv->size)
		return 0;

	len = min_t(size_t, count, dv->size - pos);
	len = min_t(size_t, len, PAGE_SIZE);
	len = ALIGN_DOWN(len, 4);
	if (!len)
		return -EINVAL;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = pm_runtime_resume_and_get(dv->dev);
	if (ret) {
		kfree(buf);
		return ret;
	}
	for (i = 0; i < len / 4; i++)
		buf[i] = readl(dv->base + pos + i * 4);
	pm_runtime_put(dv->dev);

	if (copy_to_user(ubuf, buf, len)) {
		kfree(buf);
		return -EFAULT;
	}
	kfree(buf);

	*ppos = pos + len;
	return len;
}

static const struct file_operations dbgview_regs_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = dbgview_regs_read,
	.llseek = default_llseek,
};

static int dbgview_snapshot_show(struct seq_file *s, void *unused)
{
	struct dbgview *dv = s->private;
	size_t words = min_t(size_t, snap_bytes, dv->size) / 4;
	unsigned long flags;
	u64 before, after;
	u32 *val;
	size_t i;
	int ret;

	val = kzalloc_objs(*val, max_t(size_t, words, 1));
	if (!val)
		return -ENOMEM;

	ret = pm_runtime_resume_and_get(dv->dev);
	if (ret) {
		kfree(val);
		return ret;
	}

	/* Bracket the reads by the arch counter with nothing in between. */
	local_irq_save(flags);
	before = dbgview_cntvct();
	for (i = 0; i < words; i++)
		val[i] = readl(dv->base + i * 4);
	after = dbgview_cntvct();
	local_irq_restore(flags);

	pm_runtime_put(dv->dev);

	seq_printf(s, "cntvct_before %016llx\n", before);
	for (i = 0; i < words; i++)
		seq_printf(s, "%04zx %08x\n", i * 4, val[i]);
	seq_printf(s, "cntvct_after %016llx\n", after);
	seq_printf(s, "cntfrq %u\n", arch_timer_get_cntfrq());

	kfree(val);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dbgview_snapshot);

static int dbgview_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct dbgview *dv;
	int ret;

	dv = devm_kzalloc(dev, sizeof(*dv), GFP_KERNEL);
	if (!dv)
		return -ENOMEM;
	dv->dev = dev;

	dv->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(dv->base))
		return PTR_ERR(dv->base);
	dv->size = resource_size(res);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	dv->dir = debugfs_create_dir(dev_name(dev), dbgview_root);

	debugfs_create_file_size("regs", 0400, dv->dir, dv,
				 &dbgview_regs_fops, dv->size);
	debugfs_create_file("snapshot", 0400, dv->dir, dv,
			    &dbgview_snapshot_fops);

	platform_set_drvdata(pdev, dv);
	return 0;
}

static void dbgview_remove(struct platform_device *pdev)
{
	struct dbgview *dv = platform_get_drvdata(pdev);

	debugfs_remove_recursive(dv->dir);
}

static const struct of_device_id dbgview_of_match[] = {
	{ .compatible = "apple,aic-timebase" },
	{ .compatible = "apple,debug-dock" },
	{ .compatible = "apple,t6000-dwi" },
	{ .compatible = "apple,t8112-dwi" },
	{ .compatible = "apple,t8140-dwi" },
	{ }
};
MODULE_DEVICE_TABLE(of, dbgview_of_match);

static struct platform_driver dbgview_driver = {
	.driver = {
		.name = "apple-dbgview",
		.of_match_table = dbgview_of_match,
	},
	.probe = dbgview_probe,
	.remove = dbgview_remove,
};

static int __init dbgview_init(void)
{
	int ret;

	dbgview_root = debugfs_create_dir("apple-dbgview", NULL);
	ret = platform_driver_register(&dbgview_driver);
	if (ret)
		debugfs_remove_recursive(dbgview_root);
	return ret;
}
module_init(dbgview_init);

static void __exit dbgview_exit(void)
{
	platform_driver_unregister(&dbgview_driver);
	debugfs_remove_recursive(dbgview_root);
}
module_exit(dbgview_exit);

MODULE_AUTHOR("Ryan Murray <81541646+RyanTheTide@users.noreply.github.com>");
MODULE_DESCRIPTION("Apple SoC read-only debug register views");
MODULE_LICENSE("Dual MIT/GPL");
