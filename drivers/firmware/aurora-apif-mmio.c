// SPDX-License-Identifier: GPL-2.0-only
/* Aurora Platform Interface over Memory-Mapped I/O. */
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/soc/aurora/apif-mmio.h>

#define APIF_MAGIC 0x4f494d4d46495041ULL
#define APIF_VERSION 2
#define APIF_REG_MAGIC 0x000
#define APIF_REG_VERSION 0x008
#define APIF_REG_MAX_OPS 0x010
#define APIF_REG_FEATURES 0x018
#define APIF_REG_DOORBELL 0x100
#define APIF_FEAT_NATIVE BIT_ULL(0)
#define APIF_BATCH_BYTES SZ_16K

struct apif_batch {
	u32 count, flags;
	s32 status;
	u32 done;
	struct aurora_apif_op ops[];
};

#define APIF_BATCH_MAX                                    \
	((APIF_BATCH_BYTES - sizeof(struct apif_batch)) / \
	 sizeof(struct aurora_apif_op))

struct apif_provider {
	void __iomem *regs;
	u32 max_ops;
};

struct aurora_apif {
	struct kref refs;
	raw_spinlock_t lock;
	bool live;
	void __iomem *doorbell;
	struct apif_batch *batch;
	u32 max_ops;
};

static struct platform_driver apif_driver;
static_assert(sizeof(struct apif_batch) == 16);
static_assert(sizeof(struct aurora_apif_op) == 64);
static_assert(sizeof(struct aurora_apif_boot_iommu) == 64);

/* SPTM redispatch restores SP_EL1 from its exception-stack contract. Keep the
 * Linux stack in a preserved GPR across the trapping store. The monitor must
 * preserve all GPRs, including x9, and return synchronously after the batch.
 */
static void apif_ring(struct aurora_apif *apif)
{
	u64 pa = virt_to_phys(apif->batch);

	/* Publish the request before its synchronous doorbell. */
	wmb();
	asm volatile("mov x9, sp\n"
		     "str %x[pa], [%[db]]\n"
		     "mov sp, x9\n"
		     :
		     : [pa] "r"(pa), [db] "r"(apif->doorbell)
		     : "x9", "memory");
	/* Observe completion and returned fields after redispatch. */
	rmb();
}

int aurora_apif_submit(struct aurora_apif *apif, struct aurora_apif_op *ops,
		       u32 count, u32 *done)
{
	unsigned long flags;
	u32 total = 0;
	int ret = 0;

	if (done)
		*done = 0;
	if (!apif || (count && !ops))
		return -EINVAL;
	raw_spin_lock_irqsave(&apif->lock, flags);
	if (!apif->live) {
		ret = -ENODEV;
		goto out;
	}
	while (total < count) {
		u32 n = min(count - total, apif->max_ops);
		u32 reported;

		apif->batch->count = n;
		apif->batch->flags = 0;
		apif->batch->status = -EINPROGRESS;
		apif->batch->done = 0;
		memcpy(apif->batch->ops, ops + total, n * sizeof(*ops));
		apif_ring(apif);
		reported = READ_ONCE(apif->batch->done);
		if (reported > n) {
			ret = -EIO;
			break;
		}
		memcpy(ops + total, apif->batch->ops, n * sizeof(*ops));
		total += reported;
		ret = READ_ONCE(apif->batch->status);
		if (ret || reported != n) {
			if (ret >= 0 || ret == -EINPROGRESS)
				ret = -EIO;
			break;
		}
	}
out:
	raw_spin_unlock_irqrestore(&apif->lock, flags);
	if (done)
		*done = total;
	return ret;
}
EXPORT_SYMBOL_GPL(aurora_apif_submit);

static void apif_free(struct kref *refs)
{
	kfree(container_of(refs, struct aurora_apif, refs));
}

void aurora_apif_put(struct aurora_apif *apif)
{
	kref_put(&apif->refs, apif_free);
}
EXPORT_SYMBOL_GPL(aurora_apif_put);

static void apif_revoke(void *data)
{
	struct aurora_apif *apif = data;
	unsigned long flags;

	raw_spin_lock_irqsave(&apif->lock, flags);
	apif->live = false;
	apif->doorbell = NULL;
	apif->batch = NULL;
	raw_spin_unlock_irqrestore(&apif->lock, flags);
	aurora_apif_put(apif);
}

struct aurora_apif *aurora_apif_get(struct device *dev)
{
	struct device_node *node;
	struct platform_device *supplier;
	struct apif_provider *provider;
	struct aurora_apif *apif;
	struct device_link *link;
	int ret;

	node = of_parse_phandle(dev->of_node, "aurora,apif", 0);
	if (!node)
		return ERR_PTR(-ENODEV);
	supplier = of_find_device_by_node(node);
	of_node_put(node);
	if (!supplier)
		return ERR_PTR(-EPROBE_DEFER);
	if (supplier->dev.driver != &apif_driver.driver) {
		ret = -EPROBE_DEFER;
		goto put_device;
	}
	link = device_link_add(dev, &supplier->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		ret = -ENOMEM;
		goto put_device;
	}
	provider = platform_get_drvdata(supplier);
	if (!provider) {
		ret = -EPROBE_DEFER;
		goto remove_link;
	}
	apif = kzalloc(sizeof(*apif), GFP_KERNEL);
	if (!apif) {
		ret = -ENOMEM;
		goto remove_link;
	}
	kref_init(&apif->refs);
	raw_spin_lock_init(&apif->lock);
	apif->batch = devm_kzalloc(dev, APIF_BATCH_BYTES, GFP_KERNEL);
	if (!apif->batch) {
		ret = -ENOMEM;
		aurora_apif_put(apif);
		goto remove_link;
	}
	apif->max_ops = provider->max_ops;
	apif->doorbell = provider->regs + APIF_REG_DOORBELL;
	apif->live = true;
	kref_get(&apif->refs);
	ret = devm_add_action_or_reset(dev, apif_revoke, apif);
	if (ret) {
		aurora_apif_put(apif);
		goto remove_link;
	}
	put_device(&supplier->dev);
	return apif;
remove_link:
	device_link_del(link);
put_device:
	put_device(&supplier->dev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(aurora_apif_get);

static int apif_probe(struct platform_device *pdev)
{
	struct apif_provider *provider;
	struct resource *resource;
	u64 max_ops;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource ||
	    resource_size(resource) < APIF_REG_DOORBELL + sizeof(u64))
		return -EINVAL;
	provider = devm_kzalloc(&pdev->dev, sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return -ENOMEM;
	provider->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(provider->regs))
		return PTR_ERR(provider->regs);
	if (readq(provider->regs + APIF_REG_MAGIC) != APIF_MAGIC ||
	    readq(provider->regs + APIF_REG_VERSION) != APIF_VERSION ||
	    !(readq(provider->regs + APIF_REG_FEATURES) & APIF_FEAT_NATIVE))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "unsupported APIF interface\n");
	max_ops = readq(provider->regs + APIF_REG_MAX_OPS);
	if (!max_ops)
		return -EINVAL;
	provider->max_ops = min_t(u64, max_ops, APIF_BATCH_MAX);
	platform_set_drvdata(pdev, provider);
	return 0;
}

static const struct of_device_id apif_of_match[] = {
	{ .compatible = "aurora,apif-mmio" },
	{}
};
MODULE_DEVICE_TABLE(of, apif_of_match);

static struct platform_driver apif_driver = {
	.driver = {
		.name = "aurora-apif-mmio",
		.of_match_table = apif_of_match,
	},
	.probe = apif_probe,
};
module_platform_driver(apif_driver);

MODULE_DESCRIPTION("Aurora Platform Interface over Memory-Mapped I/O");
MODULE_LICENSE("GPL");
