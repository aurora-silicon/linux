// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple SoC read-only debug register views
 *
 * Root-only debugfs views of register blocks that only debug tools use:
 *
 *  - apple,aic-timebase: the timebase aperture of the interrupt controller
 *  - apple,debug-dock: the debug dock connection status register
 *  - apple,cpu-coresight: the external debug window of one CPU core and
 *    the debug wrapper register that follows its CoreSight regions
 *
 * The driver never writes a device register and requests no interrupt.
 * Registers are read only when a debugfs file is read.
 */

#include <linux/bitfield.h>
#include <linux/cpumask.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/stop_machine.h>
#include <linux/uaccess.h>

#include <asm/arch_timer.h>

/* CoreSight regions of a core, one 64 KiB slot each (ED, CTI, PMU, wrapper). */
#define CS_SLOT_SIZE		0x10000
#define CS_ED_OFFSET		(0 * CS_SLOT_SIZE)
#define CS_WRAP_OFFSET		(3 * CS_SLOT_SIZE)
#define CS_ED_SIZE		0x1000

/* Debug wrapper control and status register. */
#define DBGWRAP			0x0
#define DBGWRAP_DBGACK		BIT_ULL(28)
#define DBGWRAP_DBGHALT		BIT_ULL(31)

/* Arm external debug registers (Arm ARM, external debug register map). */
#define EDSCR			0x088
#define EDPRCR			0x310
#define EDPRSR			0x314
#define EDPRSR_PU		BIT(0)
#define EDPRSR_SPD		BIT(1)
#define EDPRSR_R		BIT(2)
#define EDPRSR_SR		BIT(3)
#define EDPRSR_HALTED		BIT(4)
#define EDPRSR_OSLK		BIT(5)
#define EDPRSR_DLK		BIT(6)
#define EDMIDR			0xd00
#define EDPFR_LO		0xd20
#define EDPFR_HI		0xd24
#define EDDFR_LO		0xd28
#define EDDFR_HI		0xd2c
#define EDLSR			0xfb4
#define EDAUTHSTATUS		0xfb8
#define EDDEVARCH		0xfbc
#define EDDEVID2		0xfc0
#define EDDEVID1		0xfc4
#define EDDEVID			0xfc8
#define EDDEVTYPE		0xfcc
#define EDPIDR4			0xfd0
#define EDPIDR0			0xfe0
#define EDPIDR1			0xfe4
#define EDPIDR2			0xfe8
#define EDPIDR3			0xfec
#define EDCIDR0			0xff0
#define EDCIDR1			0xff4
#define EDCIDR2			0xff8
#define EDCIDR3			0xffc

/* Time the target core waits in the stop_machine() callback. */
#define CS_SPIN_TIMEOUT_US	1000

static unsigned int snap_bytes = 0x40;
module_param(snap_bytes, uint, 0444);
MODULE_PARM_DESC(snap_bytes, "Bytes of an MMIO view read by its snapshot file");

static bool wrapper;
module_param(wrapper, bool, 0444);
MODULE_PARM_DESC(wrapper, "Expose the CPU debug wrapper register (read only)");

enum dbgview_kind {
	DBGVIEW_MMIO,
	DBGVIEW_CORESIGHT,
};

struct dbgview {
	struct device *dev;
	enum dbgview_kind kind;
	void __iomem *base;
	resource_size_t size;
	int cpu;
	struct dentry *dir;
	/* Serializes CoreSight reads, which stop the machine. */
	struct mutex lock;
};

static struct dentry *dbgview_root;

static u64 dbgview_cntvct(void)
{
	return __arch_counter_get_cntvct_stable();
}

/* MMIO views */

static ssize_t dbgview_regs_read(struct file *file, char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct dbgview *dv = file->private_data;
	loff_t pos = *ppos;
	size_t len, i;
	u32 *buf;

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

	for (i = 0; i < len / 4; i++)
		buf[i] = readl(dv->base + pos + i * 4);

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

	val = kzalloc_objs(*val, max_t(size_t, words, 1));
	if (!val)
		return -ENOMEM;

	/* Bracket the reads by the arch counter with nothing in between. */
	local_irq_save(flags);
	before = dbgview_cntvct();
	for (i = 0; i < words; i++)
		val[i] = readl(dv->base + i * 4);
	after = dbgview_cntvct();
	local_irq_restore(flags);

	seq_printf(s, "cntvct_before %016llx\n", before);
	for (i = 0; i < words; i++)
		seq_printf(s, "%04zx %08x\n", i * 4, val[i]);
	seq_printf(s, "cntvct_after %016llx\n", after);
	seq_printf(s, "cntfrq %u\n", arch_timer_get_cntfrq());

	kfree(val);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dbgview_snapshot);

/* CoreSight views */

struct dbgview_cs_ctx {
	struct dbgview *dv;
	unsigned int reader;
	atomic_t arrived;
	atomic_t done;
	bool timeout;
	bool read;
	/* External debug register values, in dbgview_ed_regs[] order. */
	u32 ed[32];
	u64 wrap;
};

static const struct {
	const char *name;
	u32 offset;
} dbgview_ed_regs[] = {
	{ "EDSCR", EDSCR },
	{ "EDPRCR", EDPRCR },
	{ "EDPRSR", EDPRSR },
	{ "EDMIDR", EDMIDR },
	{ "EDPFR_LO", EDPFR_LO },
	{ "EDPFR_HI", EDPFR_HI },
	{ "EDDFR_LO", EDDFR_LO },
	{ "EDDFR_HI", EDDFR_HI },
	{ "EDLSR", EDLSR },
	{ "EDAUTHSTATUS", EDAUTHSTATUS },
	{ "EDDEVARCH", EDDEVARCH },
	{ "EDDEVID2", EDDEVID2 },
	{ "EDDEVID1", EDDEVID1 },
	{ "EDDEVID", EDDEVID },
	{ "EDDEVTYPE", EDDEVTYPE },
	{ "EDPIDR4", EDPIDR4 },
	{ "EDPIDR0", EDPIDR0 },
	{ "EDPIDR1", EDPIDR1 },
	{ "EDPIDR2", EDPIDR2 },
	{ "EDPIDR3", EDPIDR3 },
	{ "EDCIDR0", EDCIDR0 },
	{ "EDCIDR1", EDCIDR1 },
	{ "EDCIDR2", EDCIDR2 },
	{ "EDCIDR3", EDCIDR3 },
};

static_assert(ARRAY_SIZE(dbgview_ed_regs) <= 32);

/*
 * Runs on the reader core and the target core at once. The target core
 * stays in this function, and therefore powered and out of idle, until
 * the reader core has read its registers.
 */
static int dbgview_cs_stop_fn(void *data)
{
	struct dbgview_cs_ctx *ctx = data;
	struct dbgview *dv = ctx->dv;
	unsigned int cpu = smp_processor_id();
	unsigned int i;

	if (cpu == dv->cpu) {
		atomic_set(&ctx->arrived, 1);
		for (i = 0; i < CS_SPIN_TIMEOUT_US && !atomic_read(&ctx->done); i++)
			udelay(1);
		return 0;
	}

	if (cpu != ctx->reader)
		return 0;

	for (i = 0; i < CS_SPIN_TIMEOUT_US && !atomic_read(&ctx->arrived); i++)
		udelay(1);
	if (!atomic_read(&ctx->arrived)) {
		ctx->timeout = true;
		atomic_set(&ctx->done, 1);
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(dbgview_ed_regs); i++)
		ctx->ed[i] = readl(dv->base + CS_ED_OFFSET +
				   dbgview_ed_regs[i].offset);
	if (wrapper)
		ctx->wrap = readq(dv->base + CS_WRAP_OFFSET + DBGWRAP);
	ctx->read = true;

	atomic_set(&ctx->done, 1);
	return 0;
}

static int dbgview_cs_sample(struct dbgview *dv, struct dbgview_cs_ctx *ctx)
{
	cpumask_var_t mask;
	unsigned int reader;
	int ret;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;

	/*
	 * stop_machine() takes the hotplug lock itself. A core that goes
	 * offline after this check does not run the callback, and the read
	 * then fails below instead of touching a powered-down core.
	 */
	reader = cpumask_any_but(cpu_online_mask, dv->cpu);
	if (!cpu_online(dv->cpu) || reader >= nr_cpu_ids) {
		ret = -ENODEV;
		goto out;
	}

	ctx->dv = dv;
	ctx->reader = reader;
	atomic_set(&ctx->arrived, 0);
	atomic_set(&ctx->done, 0);
	ctx->read = false;
	cpumask_set_cpu(dv->cpu, mask);
	cpumask_set_cpu(reader, mask);

	ret = stop_machine(dbgview_cs_stop_fn, ctx, mask);
	if (!ret && ctx->timeout)
		ret = -ETIMEDOUT;
	else if (!ret && !ctx->read)
		ret = -EAGAIN;
out:
	free_cpumask_var(mask);
	return ret;
}

static int dbgview_ed_show(struct seq_file *s, void *unused)
{
	struct dbgview *dv = s->private;
	struct dbgview_cs_ctx *ctx;
	u32 prsr = 0;
	unsigned int i;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;

	mutex_lock(&dv->lock);
	ret = dbgview_cs_sample(dv, ctx);
	mutex_unlock(&dv->lock);
	if (ret)
		goto out;

	seq_printf(s, "cpu %d reader %u\n", dv->cpu, ctx->reader);
	for (i = 0; i < ARRAY_SIZE(dbgview_ed_regs); i++) {
		seq_printf(s, "%-12s %03x %08x\n", dbgview_ed_regs[i].name,
			   dbgview_ed_regs[i].offset, ctx->ed[i]);
		if (dbgview_ed_regs[i].offset == EDPRSR)
			prsr = ctx->ed[i];
	}

	seq_printf(s, "EDPRSR: PU=%d SPD=%d R=%d SR=%d HALTED=%d OSLK=%d DLK=%d\n",
		   !!(prsr & EDPRSR_PU), !!(prsr & EDPRSR_SPD),
		   !!(prsr & EDPRSR_R), !!(prsr & EDPRSR_SR),
		   !!(prsr & EDPRSR_HALTED), !!(prsr & EDPRSR_OSLK),
		   !!(prsr & EDPRSR_DLK));
	if (wrapper)
		seq_printf(s, "DBGWRAP %016llx DBGHALT=%d DBGACK=%d\n", ctx->wrap,
			   !!(ctx->wrap & DBGWRAP_DBGHALT),
			   !!(ctx->wrap & DBGWRAP_DBGACK));
out:
	kfree(ctx);
	return ret;
}
DEFINE_SHOW_ATTRIBUTE(dbgview_ed);

static int dbgview_probe_coresight(struct dbgview *dv)
{
	struct device_node *cpu_np;
	int cpu;

	if (dv->size < CS_WRAP_OFFSET + sizeof(u64) ||
	    dv->size < CS_ED_OFFSET + CS_ED_SIZE)
		return dev_err_probe(dv->dev, -EINVAL, "register window too small\n");

	cpu_np = of_parse_phandle(dv->dev->of_node, "cpu", 0);
	if (!cpu_np)
		return dev_err_probe(dv->dev, -EINVAL, "no cpu phandle\n");

	cpu = of_cpu_node_to_id(cpu_np);
	of_node_put(cpu_np);
	if (cpu < 0)
		return dev_err_probe(dv->dev, -ENODEV, "cpu not present\n");

	dv->cpu = cpu;
	debugfs_create_file("ed", 0400, dv->dir, dv, &dbgview_ed_fops);
	return 0;
}

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
	dv->kind = (enum dbgview_kind)(uintptr_t)of_device_get_match_data(dev);
	ret = devm_mutex_init(dev, &dv->lock);
	if (ret)
		return ret;

	dv->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(dv->base))
		return PTR_ERR(dv->base);
	dv->size = resource_size(res);

	dv->dir = debugfs_create_dir(dev_name(dev), dbgview_root);

	if (dv->kind == DBGVIEW_CORESIGHT) {
		ret = dbgview_probe_coresight(dv);
		if (ret) {
			debugfs_remove_recursive(dv->dir);
			return ret;
		}
	} else {
		debugfs_create_file_size("regs", 0400, dv->dir, dv,
					 &dbgview_regs_fops, dv->size);
		debugfs_create_file("snapshot", 0400, dv->dir, dv,
				    &dbgview_snapshot_fops);
	}

	platform_set_drvdata(pdev, dv);
	return 0;
}

static void dbgview_remove(struct platform_device *pdev)
{
	struct dbgview *dv = platform_get_drvdata(pdev);

	debugfs_remove_recursive(dv->dir);
}

static const struct of_device_id dbgview_of_match[] = {
	{ .compatible = "apple,aic-timebase", .data = (void *)DBGVIEW_MMIO },
	{ .compatible = "apple,debug-dock", .data = (void *)DBGVIEW_MMIO },
	{ .compatible = "apple,cpu-coresight", .data = (void *)DBGVIEW_CORESIGHT },
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
