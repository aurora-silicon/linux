// SPDX-License-Identifier: GPL-2.0-only
/*
 * Paravirtualised Apple DART for the x1n1 hypervisor (sptm2mmio transport).
 *
 * On an Apple SoC running under hardware SPTM, a trusted Linux guest cannot
 * touch the DART MMIO registers or own the DART translation tables: SPTM does.
 * The x1n1 hypervisor (EL2) forwards the guest's IOMMU intent to the SPTM DART
 * endpoints. This driver is the guest half: instead of programming an
 * io-pgtable and TTBRs, each map/unmap becomes an entry in an operation batch
 * that x1n1 executes against SPTM.
 *
 * Transport ("sptm2mmio"): x1n1 exposes a synthetic MMIO page whose every
 * access traps to EL2. Reads of the identification registers are emulated
 * inline. A 64-bit write to a per-device doorbell carries the guest-physical
 * address of an operation batch that the guest built in ordinary RAM; x1n1
 * leaves the guest, runs the whole batch against SPTM, and re-enters. One
 * doorbell write therefore costs one trap and one world switch for a batch of
 * up to hundreds of ops.
 *
 * SP_EL1 ABI: SPTM's guest re-dispatch ties the resumed guest's SP_EL1 to the
 * hypervisor's SP_EL2, so a doorbell write clobbers the caller's SP. Every
 * other register survives, so the doorbell is issued with the kernel SP saved
 * in a GPR and restored immediately after, under a local IRQ save.
 *
 * DT: x1n1 presents a paravirt DART as an "apple,dart-x1n1" node with the
 * doorbell page in "reg", a u32 "apple,x1n1-dart-id" (the ADT dart-id SPTM
 * knows) and #iommu-cells = <1> (the stream id).
 */

#include <linux/bitmap.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

/* sptm2mmio register/doorbell map, shared with x1n1 src/sptm.c. */
#define X1N1_MMIO_MAGIC		0x000
#define X1N1_MMIO_VERSION	0x008
#define X1N1_MMIO_MAX_OPS	0x010
#define X1N1_MMIO_FEATURES	0x018
#define X1N1_MMIO_DART_DB	0x800	/* + 8 * dart-id */
#define X1N1_MMIO_SIZE		0x4000

#define X1N1_MAGIC		0x78316e31UL	/* "x1n1" */
#define X1N1_FEAT_DART		BIT(0)

/* DART unit ops. */
#define X1N1_DART_INIT		1	/* ()                       INIT + POWERUP */
#define X1N1_DART_ATTACH	2	/* (sid, dva, level) */
#define X1N1_DART_MAP		3	/* (sid, dva, ipa, size, prot) */
#define X1N1_DART_UNMAP		4	/* (sid, dva, size) */
#define X1N1_DART_ENABLE	5	/* (sid) */
#define X1N1_DART_DISABLE	6	/* (sid) */

struct x1n1_op {
	u32 op;
	u32 flags;
	u64 arg[6];
	u64 ret;
} __packed;

struct x1n1_batch {
	u32 count;
	u32 flags;
	s32 status;
	u32 done;
	struct x1n1_op op[];
} __packed;

#define DART_X1N1_PAGE_SHIFT	14
#define DART_X1N1_PAGE_SIZE	(1UL << DART_X1N1_PAGE_SHIFT)	/* 16 KiB */
/* x1n1's DART MAP forwards one contiguous run within a single 32 MiB leaf-table
 * span, so every MAP op must stay inside one aligned block. */
#define DART_X1N1_BLOCK_SHIFT	25
#define DART_X1N1_BLOCK_SIZE	(1UL << DART_X1N1_BLOCK_SHIFT)	/* 32 MiB */

#define DART_X1N1_MAX_STREAMS	256
#define DART_X1N1_APERTURE_END	(SZ_4G - 1)

/* One batch buffer per DART, sized to hold a full unmap/map of the aperture in
 * per-block chunks; kept in the linear map so its guest PA is stable. A page
 * holds (PAGE_SIZE-16)/64 ops; the callers below chunk to fit. */
#define X1N1_BATCH_MAX_OPS	((PAGE_SIZE - sizeof(struct x1n1_batch)) / \
				 sizeof(struct x1n1_op))

struct apple_dart_x1n1 {
	struct device *dev;
	struct iommu_device iommu;
	void __iomem *mmio;
	u32 dart_id;
	u32 num_streams;
	bool inited;
	struct mutex init_lock;		/* serialises one-time init */

	struct x1n1_batch *batch;	/* linear-map buffer, guest-PA stable */
	phys_addr_t batch_pa;
	spinlock_t batch_lock;		/* protects batch + doorbell */
};

struct apple_dart_x1n1_master {
	struct apple_dart_x1n1 *dart;
	DECLARE_BITMAP(sids, DART_X1N1_MAX_STREAMS);
};

struct apple_dart_x1n1_domain {
	struct iommu_domain domain;
	struct apple_dart_x1n1 *dart;
	DECLARE_BITMAP(sids, DART_X1N1_MAX_STREAMS);
	struct mutex lock;		/* protects sids / dart binding */
};

static struct apple_dart_x1n1_domain *to_x1n1_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct apple_dart_x1n1_domain, domain);
}

/*
 * Ring a doorbell: publish the batch, write its guest-physical address to the
 * doorbell, and return the batch status. The doorbell write world-switches to
 * EL2, which clobbers SP_EL1 (see the file header), so save and restore the
 * kernel SP around it with interrupts masked. The caller holds batch_lock.
 */
static int x1n1_ring(struct apple_dart_x1n1 *dart, u32 db_off)
{
	void __iomem *db = dart->mmio + db_off;
	u64 batch_pa = dart->batch_pa;
	unsigned long flags;

	dart->batch->status = 0;
	dart->batch->done = 0;
	/* Order the batch stores before the doorbell; x1n1 does dsb on its side. */
	wmb();

	local_irq_save(flags);
	asm volatile(
		"mov	x9, sp\n"
		"str	%x[pa], [%[db]]\n"
		"mov	sp, x9\n"
		:
		: [db] "r"(db), [pa] "r"(batch_pa)
		: "x9", "memory");
	local_irq_restore(flags);

	return dart->batch->status;
}

/* Bring the DART up in SPTM exactly once, before any stream is enabled. */
static int apple_dart_x1n1_ensure_init(struct apple_dart_x1n1 *dart)
{
	int ret = 0;

	mutex_lock(&dart->init_lock);
	if (!dart->inited) {
		unsigned long flags;

		spin_lock_irqsave(&dart->batch_lock, flags);
		dart->batch->count = 1;
		dart->batch->op[0] = (struct x1n1_op){ .op = X1N1_DART_INIT };
		ret = x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);
		spin_unlock_irqrestore(&dart->batch_lock, flags);
		if (ret)
			ret = -EIO;
		else
			dart->inited = true;
	}
	mutex_unlock(&dart->init_lock);
	return ret;
}

static int apple_dart_x1n1_attach_dev(struct iommu_domain *domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	struct apple_dart_x1n1_master *master = dev_iommu_priv_get(dev);
	struct apple_dart_x1n1_domain *dom = to_x1n1_domain(domain);
	struct apple_dart_x1n1 *dart;
	unsigned long flags;
	unsigned int sid;
	u32 n;
	int ret;

	if (!master)
		return -ENODEV;
	dart = master->dart;

	ret = apple_dart_x1n1_ensure_init(dart);
	if (ret)
		return ret;

	mutex_lock(&dom->lock);
	if (dom->dart && dom->dart != dart) {
		mutex_unlock(&dom->lock);
		return -EINVAL;
	}
	dom->dart = dart;

	/* For each stream: attach a level-2 table (the 32-bit aperture this
	 * driver advertises needs no level-0/1 root) and enable translation.
	 * x1n1 owns the tables. Build the ops in one batch. */
	spin_lock_irqsave(&dart->batch_lock, flags);
	n = 0;
	for_each_set_bit(sid, master->sids, dart->num_streams) {
		if (test_bit(sid, dom->sids))
			continue;
		if (n + 2 > X1N1_BATCH_MAX_OPS)
			break;
		dart->batch->op[n++] = (struct x1n1_op){
			.op = X1N1_DART_ATTACH,
			.arg = { sid, 0, 2 },
		};
		dart->batch->op[n++] = (struct x1n1_op){
			.op = X1N1_DART_ENABLE,
			.arg = { sid },
		};
	}
	dart->batch->count = n;
	ret = n ? x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id) : 0;
	if (!ret)
		for_each_set_bit(sid, master->sids, dart->num_streams)
			set_bit(sid, dom->sids);
	spin_unlock_irqrestore(&dart->batch_lock, flags);

	mutex_unlock(&dom->lock);
	return ret ? -EIO : 0;
}

static int apple_dart_x1n1_map_pages(struct iommu_domain *domain,
				     unsigned long iova, phys_addr_t paddr,
				     size_t pgsize, size_t pgcount, int prot,
				     gfp_t gfp, size_t *mapped)
{
	struct apple_dart_x1n1_domain *dom = to_x1n1_domain(domain);
	struct apple_dart_x1n1 *dart = dom->dart;
	size_t total = pgsize * pgcount;
	size_t done = 0;
	unsigned long flags;

	if (!dart)
		return -ENODEV;

	/*
	 * Forward the range in per-stream chunks that each stay inside one
	 * aligned 32 MiB block, matching x1n1's contiguous-map constraint.
	 * Pack as many MAP ops as fit into a batch, then ring once.
	 */
	spin_lock_irqsave(&dart->batch_lock, flags);
	while (done < total) {
		u32 n = 0;
		int ret;
		size_t batch_start = done;

		while (done < total && n < X1N1_BATCH_MAX_OPS) {
			u64 cur_iova = iova + done;
			u64 cur_pa = paddr + done;
			size_t block_left = DART_X1N1_BLOCK_SIZE -
					    (cur_iova & (DART_X1N1_BLOCK_SIZE - 1));
			size_t chunk = min_t(size_t, total - done, block_left);
			unsigned int sid;
			bool full = false;

			for_each_set_bit(sid, dom->sids, dart->num_streams) {
				if (n >= X1N1_BATCH_MAX_OPS) {
					full = true;
					break;
				}
				dart->batch->op[n++] = (struct x1n1_op){
					.op = X1N1_DART_MAP,
					.arg = { sid, cur_iova, cur_pa,
						 chunk, prot },
				};
			}
			if (full)
				break;
			done += chunk;
		}

		dart->batch->count = n;
		ret = x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);
		if (ret) {
			/* Report the bytes that fully mapped before this batch. */
			*mapped = batch_start;
			spin_unlock_irqrestore(&dart->batch_lock, flags);
			return -ENOMEM;
		}
	}
	spin_unlock_irqrestore(&dart->batch_lock, flags);

	*mapped = done;
	return 0;
}

static size_t apple_dart_x1n1_unmap_pages(struct iommu_domain *domain,
					  unsigned long iova, size_t pgsize,
					  size_t pgcount,
					  struct iommu_iotlb_gather *gather)
{
	struct apple_dart_x1n1_domain *dom = to_x1n1_domain(domain);
	struct apple_dart_x1n1 *dart = dom->dart;
	size_t total = pgsize * pgcount;
	size_t done = 0;
	unsigned long flags;

	if (!dart)
		return 0;

	spin_lock_irqsave(&dart->batch_lock, flags);
	while (done < total) {
		u32 n = 0;

		while (done < total && n < X1N1_BATCH_MAX_OPS) {
			u64 cur_iova = iova + done;
			size_t block_left = DART_X1N1_BLOCK_SIZE -
					    (cur_iova & (DART_X1N1_BLOCK_SIZE - 1));
			size_t chunk = min_t(size_t, total - done, block_left);
			unsigned int sid;
			bool full = false;

			for_each_set_bit(sid, dom->sids, dart->num_streams) {
				if (n >= X1N1_BATCH_MAX_OPS) {
					full = true;
					break;
				}
				dart->batch->op[n++] = (struct x1n1_op){
					.op = X1N1_DART_UNMAP,
					.arg = { sid, cur_iova, chunk },
				};
			}
			if (full)
				break;
			done += chunk;
		}

		dart->batch->count = n;
		x1n1_ring(dart, X1N1_MMIO_DART_DB + 8 * dart->dart_id);
	}
	spin_unlock_irqrestore(&dart->batch_lock, flags);

	return total;
}

static void apple_dart_x1n1_iotlb_sync(struct iommu_domain *domain,
				       struct iommu_iotlb_gather *gather)
{
	/* x1n1 invalidates inside every MAP/UNMAP it forwards to SPTM, so a
	 * separate flush is a no-op. */
}

static phys_addr_t apple_dart_x1n1_iova_to_phys(struct iommu_domain *domain,
						dma_addr_t iova)
{
	/* SPTM owns the DART tables; the guest does not shadow them, so a
	 * hardware-accurate walk is not available here. The DMA path does not
	 * need it. */
	return 0;
}

static void apple_dart_x1n1_domain_free(struct iommu_domain *domain)
{
	kfree(to_x1n1_domain(domain));
}

static struct iommu_domain *
apple_dart_x1n1_domain_alloc_paging(struct device *dev)
{
	struct apple_dart_x1n1_domain *dom;

	dom = kzalloc(sizeof(*dom), GFP_KERNEL);
	if (!dom)
		return NULL;

	mutex_init(&dom->lock);
	dom->domain.pgsize_bitmap = DART_X1N1_PAGE_SIZE;
	dom->domain.geometry.aperture_start = 0;
	dom->domain.geometry.aperture_end = DART_X1N1_APERTURE_END;
	dom->domain.geometry.force_aperture = true;

	if (dev) {
		struct apple_dart_x1n1_master *master = dev_iommu_priv_get(dev);

		if (master)
			dom->dart = master->dart;
	}

	return &dom->domain;
}

static struct iommu_device *apple_dart_x1n1_probe_device(struct device *dev)
{
	struct apple_dart_x1n1_master *master = dev_iommu_priv_get(dev);

	if (!dev_iommu_fwspec_get(dev) || !master)
		return ERR_PTR(-ENODEV);

	device_link_add(dev, master->dart->dev,
			DL_FLAG_PM_RUNTIME | DL_FLAG_AUTOREMOVE_SUPPLIER);

	return &master->dart->iommu;
}

static void apple_dart_x1n1_release_device(struct device *dev)
{
	kfree(dev_iommu_priv_get(dev));
}

static int apple_dart_x1n1_of_xlate(struct device *dev,
				    const struct of_phandle_args *args)
{
	struct apple_dart_x1n1_master *master = dev_iommu_priv_get(dev);
	struct platform_device *iommu_pdev;
	struct apple_dart_x1n1 *dart;
	u32 sid;

	if (args->args_count != 1)
		return -EINVAL;
	sid = args->args[0];
	if (sid >= DART_X1N1_MAX_STREAMS)
		return -EINVAL;

	iommu_pdev = of_find_device_by_node(args->np);
	if (!iommu_pdev)
		return -ENODEV;
	dart = platform_get_drvdata(iommu_pdev);
	put_device(&iommu_pdev->dev);
	if (!dart)
		return -ENODEV;

	if (!master) {
		master = kzalloc(sizeof(*master), GFP_KERNEL);
		if (!master)
			return -ENOMEM;
		master->dart = dart;
		dev_iommu_priv_set(dev, master);
	} else if (master->dart != dart) {
		/* This driver forwards per (dart-id, sid); a device spanning
		 * two paravirt DARTs is not modelled. */
		return -EINVAL;
	}

	if (sid >= dart->num_streams)
		return -EINVAL;

	set_bit(sid, master->sids);
	return 0;
}

static bool apple_dart_x1n1_capable(struct device *dev, enum iommu_cap cap)
{
	return cap == IOMMU_CAP_CACHE_COHERENCY;
}

static const struct iommu_ops apple_dart_x1n1_ops = {
	.capable		= apple_dart_x1n1_capable,
	.domain_alloc_paging	= apple_dart_x1n1_domain_alloc_paging,
	.probe_device		= apple_dart_x1n1_probe_device,
	.release_device		= apple_dart_x1n1_release_device,
	.device_group		= generic_device_group,
	.of_xlate		= apple_dart_x1n1_of_xlate,
	.owner			= THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev		= apple_dart_x1n1_attach_dev,
		.map_pages		= apple_dart_x1n1_map_pages,
		.unmap_pages		= apple_dart_x1n1_unmap_pages,
		.iotlb_sync		= apple_dart_x1n1_iotlb_sync,
		.iova_to_phys		= apple_dart_x1n1_iova_to_phys,
		.free			= apple_dart_x1n1_domain_free,
	},
};

static int apple_dart_x1n1_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_dart_x1n1 *dart;
	u32 magic, features;
	int ret;

	dart = devm_kzalloc(dev, sizeof(*dart), GFP_KERNEL);
	if (!dart)
		return -ENOMEM;

	dart->dev = dev;
	mutex_init(&dart->init_lock);
	spin_lock_init(&dart->batch_lock);

	dart->mmio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dart->mmio))
		return PTR_ERR(dart->mmio);

	if (of_property_read_u32(dev->of_node, "apple,x1n1-dart-id",
				 &dart->dart_id))
		return dev_err_probe(dev, -EINVAL,
				     "missing apple,x1n1-dart-id\n");
	if (dart->dart_id > 0xff)
		return dev_err_probe(dev, -EINVAL, "dart-id out of range\n");

	dart->num_streams = DART_X1N1_MAX_STREAMS;
	of_property_read_u32(dev->of_node, "apple,x1n1-num-streams",
			     &dart->num_streams);
	if (!dart->num_streams || dart->num_streams > DART_X1N1_MAX_STREAMS)
		return dev_err_probe(dev, -EINVAL, "bad stream count\n");

	/* Confirm we really are under x1n1: the emulated MAGIC register reads
	 * back "x1n1", and the DART feature bit is advertised. */
	magic = readl(dart->mmio + X1N1_MMIO_MAGIC);
	features = readl(dart->mmio + X1N1_MMIO_FEATURES);
	if (magic != (u32)X1N1_MAGIC || !(features & X1N1_FEAT_DART))
		return dev_err_probe(dev, -ENODEV,
				     "x1n1 paravirt IOMMU not present (magic 0x%x)\n",
				     magic);

	/* Batch buffer in the linear map so its guest PA is stable and x1n1
	 * can read it by guest-physical address. */
	dart->batch = (void *)devm_get_free_pages(dev, GFP_KERNEL, 0);
	if (!dart->batch)
		return -ENOMEM;
	dart->batch_pa = virt_to_phys(dart->batch);

	platform_set_drvdata(pdev, dart);

	ret = iommu_device_sysfs_add(&dart->iommu, dev, NULL, "apple-dart-x1n1.%s",
				     dev_name(dev));
	if (ret)
		return ret;

	ret = iommu_device_register(&dart->iommu, &apple_dart_x1n1_ops, dev);
	if (ret)
		goto err_sysfs;

	dev_info(dev, "x1n1 paravirt DART id %u, %u streams (sptm2mmio)\n",
		 dart->dart_id, dart->num_streams);
	return 0;

err_sysfs:
	iommu_device_sysfs_remove(&dart->iommu);
	return ret;
}

static void apple_dart_x1n1_remove(struct platform_device *pdev)
{
	struct apple_dart_x1n1 *dart = platform_get_drvdata(pdev);

	iommu_device_unregister(&dart->iommu);
	iommu_device_sysfs_remove(&dart->iommu);
}

static const struct of_device_id apple_dart_x1n1_of_match[] = {
	{ .compatible = "apple,dart-x1n1" },
	{ },
};
MODULE_DEVICE_TABLE(of, apple_dart_x1n1_of_match);

static struct platform_driver apple_dart_x1n1_driver = {
	.driver = {
		.name = "apple-dart-x1n1",
		.of_match_table = apple_dart_x1n1_of_match,
	},
	.probe = apple_dart_x1n1_probe,
	.remove = apple_dart_x1n1_remove,
};
module_platform_driver(apple_dart_x1n1_driver);

MODULE_DESCRIPTION("Paravirtualised Apple DART for the x1n1 hypervisor");
MODULE_LICENSE("GPL");
