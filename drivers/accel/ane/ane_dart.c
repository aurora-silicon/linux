// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Joshua Warren */

#include <kunit/visibility.h>
#include <linux/interrupt.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "ane.h"

/*
 * T8103 DART is the T8020 register block (apple_dart_hw_t8103 in
 * drivers/iommu/apple-dart.c), pagesized 0x4000. Containment policy:
 * during a job the apple-dart fault IRQ is masked so the latch survives
 * for this driver to read, the completion poll checks that latch before
 * every engine access (engine reads hang the SoC once a fault has
 * landed), and a faulted IOVA is handed a throwaway page so the retried
 * transaction completes and the engine drains. Mapping itself stays
 * provider-owned; the page tables are never rewritten here.
 */
#define DART_STREAM_COMMAND		0x20
#define DART_STREAM_COMMAND_BUSY	BIT(2)
#define DART_STREAM_COMMAND_INVALIDATE	BIT(20)
#define DART_STREAM_SELECT		0x34
#define DART_ERROR			0x40
#define DART_ERROR_FLAG			BIT(31)
#define DART_ERROR_ADDR_LO		0x50
#define DART_ERROR_ADDR_HI		0x54
#define DART_STREAMS_ENABLE		0xfc
#define DART_TCR			0x100
#define DART_TTBR			0x200
#define DART_TTBR_COUNT			4

#define DART_TCR_SID(sid) (DART_TCR + ((sid) << 2))
#define DART_TTBR_SID(sid, idx) \
	(DART_TTBR + ((((sid) * DART_TTBR_COUNT) + (idx)) << 2))

static bool dart_contain = true;
module_param(dart_contain, bool, 0444);
MODULE_PARM_DESC(dart_contain,
		 "Mask ANE DART fault IRQs during a job and restore the stream after a fault (default: on)");

static int ane_dart_invalidate(struct ane_device *ane, struct ane_dart *dart)
{
	void __iomem *regs = dart->regs;
	u32 cmd;
	int ret;

	if (dart->sid >= 32)
		return -EINVAL;
	writel(BIT(dart->sid), regs + DART_STREAM_SELECT);
	writel(DART_STREAM_COMMAND_INVALIDATE, regs + DART_STREAM_COMMAND);
	ret = readl_poll_timeout(regs + DART_STREAM_COMMAND, cmd,
				 !(cmd & DART_STREAM_COMMAND_BUSY), 1, 100);
	if (ret)
		dev_err(ane->dev, "DART sid %u TLB invalidate timed out\n",
			dart->sid);
	return ret;
}

int ane_dart_init(struct ane_device *ane)
{
	struct device_node *np = ane->dev->of_node;
	int count, i;

	ane->dart_count = 0;
	if (!dart_contain)
		return 0;
	count = of_count_phandle_with_args(np, "iommus", "#iommu-cells");
	if (count < 0)
		return 0;
	for (i = 0; i < count && ane->dart_count < ANE_DART_MAX; i++) {
		struct of_phandle_args args;
		struct platform_device *pdev;
		struct resource *res;
		struct ane_dart *dart;
		void __iomem *regs;
		int irq;

		if (of_parse_phandle_with_args(np, "iommus", "#iommu-cells",
					       i, &args))
			break;
		if (!of_device_is_compatible(args.np, "apple,t8103-dart")) {
			of_node_put(args.np);
			continue;
		}
		pdev = of_find_device_by_node(args.np);
		if (!pdev) {
			of_node_put(args.np);
			continue;
		}
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		irq = platform_get_irq(pdev, 0);
		if (!res || irq < 0) {
			put_device(&pdev->dev);
			of_node_put(args.np);
			continue;
		}
		regs = devm_ioremap(ane->dev, res->start, resource_size(res));
		if (!regs) {
			put_device(&pdev->dev);
			of_node_put(args.np);
			return -ENOMEM;
		}
		dart = &ane->darts[ane->dart_count++];
		dart->regs = regs;
		dart->irq = irq;
		dart->sid = args.args_count ? args.args[0] : 0;
		dart->masked = false;
		dev_dbg(ane->dev, "DART containment: %pOFn sid %u irq %d\n",
			args.np, dart->sid, irq);
		put_device(&pdev->dev);
		of_node_put(args.np);
	}
	dev_info(ane->dev, "DART containment armed: %d\n", ane->dart_count);
	return 0;
}

void ane_dart_mask(struct ane_device *ane)
{
	int i;

	for (i = 0; i < ane->dart_count; i++) {
		struct ane_dart *dart = &ane->darts[i];

		if (dart->masked)
			continue;
		disable_irq(dart->irq);
		dart->masked = true;
	}
}

void ane_dart_unmask(struct ane_device *ane)
{
	int i;

	for (i = 0; i < ane->dart_count; i++) {
		struct ane_dart *dart = &ane->darts[i];

		if (!dart->masked)
			continue;
		enable_irq(dart->irq);
		dart->masked = false;
	}
}

bool ane_dart_faulted(struct ane_device *ane, u64 *iova, u32 *status)
{
	int i;

	for (i = 0; i < ane->dart_count; i++) {
		void __iomem *regs = ane->darts[i].regs;
		u32 err = readl(regs + DART_ERROR);

		if (!(err & DART_ERROR_FLAG))
			continue;
		if (status)
			*status = err;
		if (iova)
			*iova = readl(regs + DART_ERROR_ADDR_LO) |
				((u64)readl(regs + DART_ERROR_ADDR_HI) << 32);
		return true;
	}
	return false;
}

/*
 * The scratch page is mapped read-write for the job that faulted, and
 * the job's output can carry what it reads from there: hand it out
 * zeroed.
 */
VISIBLE_IF_KUNIT struct page *ane_dart_alloc_scratch_page(void)
{
	return alloc_page(GFP_KERNEL | __GFP_ZERO);
}
EXPORT_SYMBOL_IF_KUNIT(ane_dart_alloc_scratch_page);

/*
 * Where may a fault-drain page go? A fault is a missing PTE in one of two
 * places. In a hole of the allocator (a wild address), the drain reserves
 * a node for the page (@host stays NULL). Or in the range of a BO that is
 * not in the DART: ane_submit() takes every BO that the job does not name
 * out of the DART, so an access past the end of a job's buffer, or a
 * descriptor that points at another buffer of this or another file, lands
 * there; the ranges are packed, so this is the common case. The page then
 * goes into that BO's range and @host is the BO's node. That BO cannot get
 * its pages back into the DART, nor its range back to the allocator, while
 * the page is there (ane_dart_scratch_in()), so no scratch PTE outlives
 * the drain into a mapped BO. Anything else (a mapped BO, a scratch page,
 * a wedge-preserved range) has PTEs and cannot fault: refuse it.
 *
 * Callers hold iommu_lock.
 */
VISIBLE_IF_KUNIT int ane_dart_scratch_slot(struct ane_device *ane, u64 iova,
					   u64 size, struct drm_mm_node **host)
{
	struct drm_mm *mm = &ane->mm;
	struct drm_mm_node *node;

	*host = NULL;
	if (iova + size < iova)
		return -EINVAL;
	/* A drain in an unmapped BO has no separate drm_mm node. */
	for (int i = 0; i < ane->scratch_pages; i++)
		if (ane->scratch[i].iova == iova)
			return -EBUSY;
	drm_mm_for_each_node_in_range(node, mm, iova, iova + size) {
		/* Past the end of the range that the allocator manages. */
		if (node == &mm->head_node)
			return -ERANGE;
		if (node->color != ANE_MM_BO_UNMAPPED ||
		    node->start > iova ||
		    node->start + node->size < iova + size)
			return -EBUSY;
		*host = node;
		return 0;
	}
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(ane_dart_scratch_slot);

/* Does a drain page sit in the range of the BO that owns @host? */
bool ane_dart_scratch_in(struct ane_device *ane, struct drm_mm_node *host)
{
	for (int i = 0; i < ane->scratch_pages; i++)
		if (host && ane->scratch[i].host == host)
			return true;
	return false;
}
EXPORT_SYMBOL_IF_KUNIT(ane_dart_scratch_in);

/*
 * A fault is a missing PTE, nothing more: the page tables are provider
 * owned and unchanged, so the stream-restore apple-dart uses at reset
 * time is the wrong tool mid-job (register surgery while the faulting
 * transaction is retried takes the machine down). Instead the faulted
 * IOVA is handed a throwaway DART page, so the retried transaction
 * completes and the engine drains on its own; the scratch pages are
 * unmapped once the request has been failed cleanly.
 *
 * The drm_mm allocator knows nothing about a raw iommu_map, so a scratch
 * PTE is always inside a node: one the drain reserves in a hole, or the
 * node of the unmapped BO it landed in (ane_dart_scratch_slot()). A
 * node-tracked mapping can never collide with a later BO_INIT, and a
 * scratch whose release is skipped degrades into a bounded, benign leak
 * instead of a poisoned address space. A page that no drain may take
 * is refused, and the caller wedges and recovers, fail closed.
 *
 * Callers hold engine_lock, which keeps the BOs' DART state still.
 */
int ane_dart_drain_fault(struct ane_device *ane, u64 fault_iova)
{
	struct ane_dart_scratch *scratch;
	struct drm_mm_node *node = NULL, *host;
	struct page *page;
	u64 iova;
	int ret, i;

	lockdep_assert_held(&ane->engine_lock);
	page = ane_dart_alloc_scratch_page();
	if (!page)
		return -ENOMEM;
	iova = fault_iova & ~((1ULL << ane->shift) - 1);

	mutex_lock(&ane->iommu_lock);
	ret = ane_dart_scratch_slot(ane, iova, BIT(ane->shift), &host);
	if (ret)
		goto refuse;
	if (!host) {
		node = kzalloc_obj(*node);
		if (!node) {
			ret = -ENOMEM;
			goto refuse;
		}
		node->start = iova;
		node->size = BIT(ane->shift);
		node->color = ANE_MM_SCRATCH;
		ret = drm_mm_reserve_node(&ane->mm, node);
		if (ret) {
			kfree(node);
			goto refuse;
		}
	}

	ret = iommu_map(ane->domain, iova, page_to_phys(page),
			BIT(ane->shift), IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (ret) {
		if (node) {
			drm_mm_remove_node(node);
			kfree(node);
		}
		goto refuse;
	}
	mutex_unlock(&ane->iommu_lock);
	dev_dbg(ane->dev, "fault drain: page at %#llx in %s\n", iova,
		host ? "an unmapped buffer's range" : "a hole");
	scratch = &ane->scratch[ane->scratch_pages++];
	scratch->iova = iova;
	scratch->page = page;
	scratch->node = node;
	scratch->host = host;
	for (i = 0; i < ane->dart_count; i++)
		ane_dart_invalidate(ane, &ane->darts[i]);
	return 0;

refuse:
	mutex_unlock(&ane->iommu_lock);
	__free_page(page);
	dev_dbg(ane->dev, "fault drain refused at %#llx: %d\n", iova, ret);
	return ret;
}

void ane_dart_release_scratch(struct ane_device *ane,
			      struct ane_dart_scratch *scratch, int pages)
{
	int i, d;

	mutex_lock(&ane->iommu_lock);
	for (i = 0; i < pages; i++) {
		size_t unmapped;

		unmapped = iommu_unmap(ane->domain, scratch[i].iova,
				       BIT(ane->shift));
		if (unmapped != BIT(ane->shift))
			dev_err(ane->dev,
				"scratch unmap short at %#llx: %zu\n",
				scratch[i].iova, unmapped);
		if (scratch[i].node) {
			drm_mm_remove_node(scratch[i].node);
			kfree(scratch[i].node);
		}
		scratch[i].node = NULL;
		scratch[i].host = NULL;
		__free_page(scratch[i].page);
		scratch[i].page = NULL;
	}
	mutex_unlock(&ane->iommu_lock);
	if (pages)
		for (d = 0; d < ane->dart_count; d++)
			ane_dart_invalidate(ane, &ane->darts[d]);
}
