// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Staged H16 backend, using the accepted H16 cold-boot facts, round 5. */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-map-ops.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "ane_h16_boot.h"
#include "ane_h16_execute.h"
#include "ane_h16_preload.h"

#define ANE_H16_TX_WORDS 272

struct ane_h16_mapping {
	struct list_head node;
	u64 dva;
	size_t size;
	size_t mapped;
	struct page **pages;
	u32 blocks;
	void *cpu;
};

struct ane_h16_device {
	struct ane_h16_boot boot;
	struct device *dev;
	void __iomem *window[ANE_H16_WINDOWS];
	struct device *power[5];
	struct device *dart[3];
	u32 powered;
	u32 darts_powered;
	u64 preload_phys[2];
	struct list_head mappings;
	struct iommu_domain *domain;
	struct iommu_group *group;
	bool claimed;
	bool attached;
	bool pinned;
	int irq;
	struct ane_fw_rtk rtk;
	struct {
		u64 word0;
		u64 word1;
	} tx[ANE_H16_TX_WORDS];
	u32 tx_read;
	u32 tx_write;
	bool rtk_failed;
	u64 shared_next;
};

static DEFINE_MUTEX(ane_h16_lock);
static bool ane_h16_released;

/* Diagnostic, workspace ane: observe an unknown resident image. */
static char *h16_identity;
module_param(h16_identity, charp, 0444);
MODULE_PARM_DESC(h16_identity, "observe: accept an unknown resident image for preflight only and publish it in debugfs ane_h16/");

static const char *const ane_h16_build_names[ANE_H16_PRELOAD_BUILDS] = {
	"H16 27.0 26A428", "H16 26.5.2 25F84",
};

static struct debugfs_blob_wrapper ane_h16_dump[2];
static struct dentry *ane_h16_debugfs;

static bool ane_h16_observing(void)
{
	return h16_identity && !strcmp(h16_identity, "observe");
}

static struct ane_h16_device *ane_h16_job_dev;
static struct debugfs_blob_wrapper ane_h16_job_result;

void ane_h16_backend_exit(void)
{
	debugfs_remove_recursive(ane_h16_debugfs);
	ane_h16_debugfs = NULL;
	vfree(ane_h16_dump[0].data);
	vfree(ane_h16_dump[1].data);
	vfree(ane_h16_job_result.data);
	ane_h16_job_dev = NULL;
}

static u32 ane_h16_mmio_read32(void *ctx, enum ane_h16_window w, u32 off)
{
	struct ane_h16_device *h = ctx;

	return readl(h->window[w] + off);
}

static u64 ane_h16_mmio_read64(void *ctx, enum ane_h16_window w, u32 off)
{
	struct ane_h16_device *h = ctx;

	return readq(h->window[w] + off);
}

static void ane_h16_mmio_write32(void *ctx, enum ane_h16_window w, u32 off, u32 value)
{
	struct ane_h16_device *h = ctx;

	writel(value, h->window[w] + off);
}

static void ane_h16_mmio_write64(void *ctx, enum ane_h16_window w, u32 off, u64 value)
{
	struct ane_h16_device *h = ctx;

	writeq(value, h->window[w] + off);
}

static void ane_h16_delay(void *ctx, unsigned int us)
{
	if (us < 100)
		udelay(us);
	else
		usleep_range(us, us + 50);
}

static void ane_h16_log(void *ctx, const char *item, u64 a, u64 b)
{
	struct ane_h16_device *h = ctx;

	dev_info(h->dev, "H16 %s %llx %llx\n", item, a, b);
}

static void ane_h16_quarantine(void *ctx, int error)
{
	struct ane_h16_device *h = ctx;

	dev_err(h->dev, "H16 quarantine error %d; mappings, power and module held until reboot\n",
		error);
}

static int ane_h16_pin(void *ctx)
{
	struct ane_h16_device *h = ctx;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	h->pinned = true;
	ane_h16_released = true;
	return 0;
}

/* Nonblocking transmit queue. RTKit progress never gates scratch progress. */
static int ane_h16_mailbox_send(void *ctx, u64 word0, u64 word1)
{
	struct ane_h16_device *h = ctx;

	if (h->tx_write - h->tx_read == ARRAY_SIZE(h->tx))
		return -ENOSPC;
	h->tx[h->tx_write % ARRAY_SIZE(h->tx)].word0 = word0;
	h->tx[h->tx_write % ARRAY_SIZE(h->tx)].word1 = word1;
	h->tx_write++;
	return 0;
}

static void ane_h16_mailbox(void *ctx)
{
	struct ane_h16_device *h = ctx;
	void __iomem *mb = h->window[ANE_H16_ENGINE] + h->boot.soc->mailbox;
	unsigned int i;

	for (i = 0; i < 16; i++) {
		u64 word0, word1;
		int ret;

		if (readl(mb + 0x114) & BIT(17))
			break;
		/* Two 64-bit accesses. Reading the second word pops the entry. */
		word0 = readq(mb + 0x830);
		word1 = readq(mb + 0x838);
		dev_info(h->dev, "H16 ASC RX %016llx %016llx\n", word0, word1);
		if (h->rtk_failed)
			continue;
		ret = ane_fw_rtk_receive(&h->rtk, word0, word1, h, ane_h16_mailbox_send);
		if (ret) {
			dev_info(h->dev,
				 "H16 RTKit message declined %d; scratch remains independent\n",
				 ret);
			if (ret != -EOPNOTSUPP)
				h->rtk_failed = true;
		}
	}
	for (i = 0; i < 16 && h->tx_read != h->tx_write; i++) {
		u32 index = h->tx_read % ARRAY_SIZE(h->tx);

		if (readl(mb + 0x110) & BIT(16))
			break;
		writeq(h->tx[index].word0, mb + 0x800);
		/* Publish word 0 before the word-1 FIFO commit. */
		wmb();
		writeq(h->tx[index].word1, mb + 0x808);
		dev_info(h->dev, "H16 ASC TX %016llx %016llx\n",
			 h->tx[index].word0, h->tx[index].word1);
		h->tx_read++;
	}
}

static int ane_h16_map_range(struct ane_h16_device *h, u64 dva, u64 pa,
			     size_t size, int prot)
{
	struct ane_h16_mapping *m;
	int ret;

	m = kzalloc_obj(*m);
	if (!m)
		return -ENOMEM;
	m->dva = dva;
	m->size = size;
	list_add_tail(&m->node, &h->mappings);
	ret = iommu_map(h->domain, dva, pa, size, prot, GFP_KERNEL);
	if (!ret)
		m->mapped = size;
	return ret;
}

static int ane_h16_count_group(struct device *dev, void *data)
{
	unsigned int *count = data;

	(*count)++;
	return 0;
}

static int ane_h16_map(void *ctx)
{
	struct ane_h16_device *h = ctx;
	struct ane_h16_boot *b = &h->boot;
	unsigned int count = 0, i;
	int ret;

	if (of_count_phandle_with_args(h->dev->of_node, "iommus", "#iommu-cells") != 3) {
		dev_err(h->dev, "H16 map requires only SID 0 on three DARTs; refusing extra streams\n");
		return -EOPNOTSUPP;
	}
	h->group = iommu_group_get(h->dev);
	if (!h->group)
		return -ENODEV;
	ret = iommu_group_for_each_dev(h->group, &count, ane_h16_count_group);
	if (ret || count != 1)
		return ret ?: -EBUSY;
	ret = iommu_device_claim_dma_owner(h->dev, h);
	if (ret)
		return ret;
	h->claimed = true;
	h->domain = iommu_paging_domain_alloc(h->dev);
	if (IS_ERR_OR_NULL(h->domain)) {
		ret = h->domain ? PTR_ERR(h->domain) : -ENOMEM;
		h->domain = NULL;
		return ret;
	}
	if (h->domain->type != IOMMU_DOMAIN_UNMANAGED ||
	    !(h->domain->pgsize_bitmap & SZ_16K))
		return -EOPNOTSUPP;
	/* Never widen a finalized DART aperture by assigning geometry fields. */
	for (i = 0; i < 4; i++) {
		u64 page = round_down(b->ranges[i][0], SZ_16K);

		if (!ane_fw_contains(h->domain->geometry.aperture_start,
				     h->domain->geometry.aperture_end -
				     h->domain->geometry.aperture_start + 1,
				     page, SZ_16K))
			return -ERANGE;
	}
	if (h->domain->geometry.aperture_end < b->build->shared_dva + ANE_FW_SHARED_TOTAL)
		return -ERANGE;
	ret = iommu_attach_device(h->domain, h->dev);
	if (ret)
		return ret;
	h->attached = true;
	for (i = 0; i < 2; i++) {
		ret = ane_h16_map_range(h, b->soc->preload_dva[i], h->preload_phys[i],
					b->soc->preload_size[i],
					IOMMU_READ | IOMMU_CACHE | (i ? IOMMU_WRITE : 0));
		if (ret)
			return ret;
		dev_info(h->dev, "H16 preload %s mapped at %llx bytes %llx\n",
			 i ? "DATA" : "TEXT", b->soc->preload_dva[i], b->soc->preload_size[i]);
	}
	for (i = 0; i < 4; i++) {
		u64 page = round_down(b->ranges[i][0], SZ_16K);

		ret = ane_h16_map_range(h, page, page, SZ_16K,
					IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO);
		if (ret)
			return ret;
		dev_info(h->dev, "H16 identity MMIO %llx mapped\n", page);
	}
	return 0;
}

static int ane_h16_alloc_at(void *ctx, u64 dva, size_t size, struct ane_fw_surface *out)
{
	struct ane_h16_device *h = ctx;
	struct ane_h16_mapping *m;
	u32 i, j, pages_per_block = SZ_16K / PAGE_SIZE;
	unsigned int order = get_order(SZ_16K);
	int ret;

	if (!dev_is_dma_coherent(h->dev)) {
		dev_err(h->dev, "H16 private surfaces require a coherent client\n");
		return -EOPNOTSUPP;
	}
	if (!size || !IS_ALIGNED(size, SZ_16K) || !IS_ALIGNED(dva, SZ_16K) ||
	    PAGE_SIZE > SZ_16K)
		return -EINVAL;
	m = kzalloc_obj(*m);
	if (!m)
		return -ENOMEM;
	m->size = size;
	m->dva = dva;
	list_add_tail(&m->node, &h->mappings);
	m->pages = kvcalloc(size / PAGE_SIZE, sizeof(*m->pages), GFP_KERNEL);
	if (!m->pages)
		return -ENOMEM;
	for (i = 0; i < size / SZ_16K; i++) {
		struct page *page = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);

		if (!page)
			return -ENOMEM;
		for (j = 0; j < pages_per_block; j++)
			m->pages[i * pages_per_block + j] = page + j;
		m->blocks++;
	}
	/* The coherent-client gate permits matching cached CPU/device aliases. */
	m->cpu = vmap(m->pages, size / PAGE_SIZE, VM_MAP, PAGE_KERNEL);
	if (!m->cpu)
		return -ENOMEM;
	memset(m->cpu, 0, size);
	for (i = 0; i < m->blocks; i++) {
		ret = iommu_map(h->domain, dva + i * SZ_16K,
				page_to_phys(m->pages[i * pages_per_block]), SZ_16K,
				IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, GFP_KERNEL);
		if (ret)
			return ret;
		m->mapped += SZ_16K;
	}
	*out = (struct ane_fw_surface) { .cpu = m->cpu, .dva = dva, .size = size };
	return 0;
}

static u64 ane_h16_load64(void *ctx, const void *p)
{
	return le64_to_cpu(READ_ONCE(*(const __le64 *)p));
}

static void ane_h16_store64(void *ctx, void *p, u64 value)
{
	WRITE_ONCE(*(__le64 *)p, cpu_to_le64(value));
}

static void ane_h16_dma_read_barrier(void *ctx)
{
	dma_rmb();
}

static void ane_h16_dma_write_barrier(void *ctx)
{
	dma_wmb();
}

static u32 ane_h16_engine_read(void *ctx, u32 offset)
{
	return ane_h16_mmio_read32(ctx, ANE_H16_ENGINE, offset);
}

static void ane_h16_engine_write(void *ctx, u32 offset, u32 value)
{
	struct ane_h16_device *h = ctx;

	ane_h16_mmio_write32(ctx, ANE_H16_ENGINE, offset, value);
	if (offset == h->boot.soc->irq_ack) {
		/* Complete the snapshot acknowledgment before ring reads (r5). */
		mb();
	}
}

static u64 ane_h16_now(void *ctx)
{
	return ktime_get_ns();
}

static void ane_h16_relax(void *ctx)
{
	struct ane_h16_device *h = ctx;
	int ret;

	ane_h16_mailbox(h);
	ret = ane_h16_faults(&h->boot, false);
	if (ret)
		ane_fw_transport_fail(&h->boot.transport, ret);
	usleep_range(1000, 1050);
}

static int ane_h16_alloc_shared(void *ctx, size_t size, u32 tag, struct ane_fw_surface *out)
{
	struct ane_h16_device *h = ctx;
	int ret = ane_h16_alloc_at(h, h->shared_next, size, out);

	if (!ret) {
		h->shared_next += size;
		dev_info(h->dev, "H16 SHAREDMALLOC bytes %zx tag %08x DVA %llx\n",
			 size, tag, out->dva);
	}
	return ret;
}

static void ane_h16_refused(void *ctx, u64 w0, u64 w1, u64 w2, int error)
{
	struct ane_h16_device *h = ctx;

	dev_err(h->dev, "H16 SHAREDMALLOC refused %llx %llx %llx error %d\n",
		w0, w1, w2, error);
}

static const struct ane_h16_boot_ops ane_h16_ops = {
	.read32 = ane_h16_mmio_read32,
	.read64 = ane_h16_mmio_read64,
	.write32 = ane_h16_mmio_write32,
	.write64 = ane_h16_mmio_write64,
	.delay_us = ane_h16_delay,
	.map = ane_h16_map,
	.alloc_at = ane_h16_alloc_at,
	.pin = ane_h16_pin,
	.mailbox = ane_h16_mailbox,
	.log = ane_h16_log,
	.quarantine = ane_h16_quarantine,
};

static const struct ane_fw_transport_ops ane_h16_transport_ops = {
	.load64 = ane_h16_load64,
	.store64 = ane_h16_store64,
	.read_barrier = ane_h16_dma_read_barrier,
	.write_barrier = ane_h16_dma_write_barrier,
	.read32 = ane_h16_engine_read,
	.write32 = ane_h16_engine_write,
	.now_ns = ane_h16_now,
	.relax = ane_h16_relax,
	.alloc = ane_h16_alloc_shared,
	.refused = ane_h16_refused,
};

static void ane_h16_cleanup(struct ane_h16_device *h)
{
	struct ane_h16_mapping *m, *next;
	unsigned int i, pages_per_block = SZ_16K / PAGE_SIZE;

	if (h->boot.ran || h->pinned)
		return;
	list_for_each_entry_safe_reverse(m, next, &h->mappings, node) {
		if (m->mapped)
			iommu_unmap(h->domain, m->dva, m->mapped);
		if (m->cpu)
			vunmap(m->cpu);
		for (i = 0; i < m->blocks; i++)
			__free_pages(m->pages[i * pages_per_block], get_order(SZ_16K));
		kvfree(m->pages);
		list_del(&m->node);
		kfree(m);
	}
	if (h->attached)
		iommu_detach_device(h->domain, h->dev);
	if (h->domain)
		iommu_domain_free(h->domain);
	if (h->claimed)
		iommu_device_release_dma_owner(h->dev);
	if (h->group)
		iommu_group_put(h->group);
	if (h->irq >= 0)
		free_irq(h->irq, h);
	for (i = ANE_H16_WINDOWS; i-- > 0; )
		if (h->window[i])
			iounmap(h->window[i]);
	while (h->darts_powered)
		pm_runtime_put_sync(h->dart[--h->darts_powered]);
	for (i = 0; i < 3; i++)
		if (h->dart[i])
			put_device(h->dart[i]);
	while (h->powered) {
		struct device *pd = h->power[--h->powered];

		pm_runtime_put_sync(pd);
		dev_pm_domain_detach(pd, true);
	}
	dev_info(h->dev, "H16 pre-RUN rollback complete; no CPU release\n");
	kfree(h);
}

static irqreturn_t ane_h16_masked_irq(int irq, void *data)
{
	return IRQ_NONE;
}

static int ane_h16_acquire(struct platform_device *pdev, struct ane_h16_device *h)
{
	struct device_node *np = pdev->dev.of_node;
	static const char * const names[] = { "engine", "pmgr", "aux-3" };
	unsigned int i, j;
	int ret, irq;

	if (of_count_phandle_with_args(np, "power-domains", "#power-domain-cells") != 5)
		return -EINVAL;
	for (i = 0; i < 5; i++) {
		struct device *pd = dev_pm_domain_attach_by_id(h->dev, i);

		if (IS_ERR_OR_NULL(pd))
			return pd ? PTR_ERR(pd) : -ENODEV;
		ret = pm_runtime_resume_and_get(pd);
		if (ret < 0) {
			dev_pm_domain_detach(pd, true);
			return ret;
		}
		h->power[h->powered++] = pd;
	}
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		struct resource *r = platform_get_resource_byname(pdev, IORESOURCE_MEM, names[i]);

		if (!r)
			return -ENODEV;
		h->window[i] = ioremap_np(r->start, resource_size(r));
		if (!h->window[i])
			return -ENOMEM;
	}
	for (i = 0; i < 3; i++) {
		struct of_phandle_args args;
		struct platform_device *dart;
		struct resource r;

		ret = of_parse_phandle_with_args(np, "iommus", "#iommu-cells", i, &args);
		if (ret)
			return ret;
		if (args.args_count != 1 || args.args[0] != 0 ||
		    !of_device_is_compatible(args.np, "apple,t8110-dart") ||
		    !of_device_is_available(args.np)) {
			of_node_put(args.np);
			return -EINVAL;
		}
		dart = of_find_device_by_node(args.np);
		ret = of_address_to_resource(args.np, 0, &r);
		of_node_put(args.np);
		if (!dart)
			return -EPROBE_DEFER;
		h->dart[i] = &dart->dev;
		for (j = 0; j < i; j++)
			if (h->dart[j] == h->dart[i])
				return -EINVAL;
		if (ret)
			return ret;
		ret = pm_runtime_resume_and_get(h->dart[i]);
		if (ret < 0)
			return ret;
		h->darts_powered++;
		h->window[ANE_H16_DART0 + i] = ioremap_np(r.start, resource_size(&r));
		h->window[ANE_H16_DAPF0 + i] = ioremap_np(r.start + 0x10000, SZ_16K);
		if (!h->window[ANE_H16_DART0 + i] || !h->window[ANE_H16_DAPF0 + i])
			return -ENOMEM;
	}
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = request_irq(irq, ane_h16_masked_irq, IRQF_NO_AUTOEN, "ane-h16-polled", h);
	if (ret)
		return ret;
	h->irq = irq;
	dev_info(h->dev, "H16 polled mode; channel IRQ remains masked\n");
	return 0;
}

static int ane_h16_preload_identity(struct ane_h16_device *h)
{
	struct ane_h16_boot *b = &h->boot;
	void *snap[2] = {}, *second = NULL;
	unsigned int i;
	int ret = -ENOMEM;

	for (i = 0; i < 2; i++) {
		struct device_node *np = of_parse_phandle(h->dev->of_node, "memory-region", i);
		struct reserved_mem *rmem;
		void *live;

		if (!np) {
			ret = -ENODATA;
			goto out;
		}
		rmem = of_reserved_mem_lookup(np);
		of_node_put(np);
		if (!rmem || rmem->size != b->soc->preload_size[i]) {
			ret = -EINVAL;
			goto out;
		}
		h->preload_phys[i] = rmem->base;
		snap[i] = vzalloc(rmem->size);
		second = vzalloc(rmem->size);
		if (!snap[i] || !second)
			goto out;
		live = memremap(rmem->base, rmem->size, MEMREMAP_WB);
		if (!live)
			goto out;
		memcpy(snap[i], live, rmem->size);
		/* Separate the two observations used to reject an unstable preload. */
		rmb();
		memcpy(second, live, rmem->size);
		memunmap(live);
		if (memcmp(snap[i], second, rmem->size)) {
			ret = -EBUSY;
			goto out;
		}
		vfree(second);
		second = NULL;
	}
	ret = ane_h16_preload_identify(snap[0], b->soc->preload_size[0],
				       snap[1], b->soc->preload_size[1]);
	if (ret >= 0) {
		if (b->soc->h16_builds)
			b->build = b->soc->h16_builds[ret];
		h->shared_next = b->build->shared_dva;
		dev_info(h->dev, "H16 preload identity %s accepted (boot values %s)\n",
			 ane_h16_build_names[ret], b->build->identity);
		ret = 0;
	} else if (ane_h16_observing() && !ane_h16_debugfs) {
		dev_warn(h->dev, "H16 preload identity refused (%d); observing, image in debugfs ane_h16/\n",
			 ret);
		for (i = 0; i < 2; i++) {
			ane_h16_dump[i].data = snap[i];
			ane_h16_dump[i].size = b->soc->preload_size[i];
			snap[i] = NULL;
		}
		ane_h16_debugfs = debugfs_create_dir("ane_h16", NULL);
		debugfs_create_blob("text", 0400, ane_h16_debugfs, &ane_h16_dump[0]);
		debugfs_create_blob("data", 0400, ane_h16_debugfs, &ane_h16_dump[1]);
		ret = 0;
	}
out:
	vfree(second);
	vfree(snap[0]);
	vfree(snap[1]);
	return ret;
}

/*
 * Diagnostic job interface (workspace ane). With h16_job=1 and a completed
 * startup stage, one write to debugfs ane_h16/job runs a sequence of records
 * {u32 op, u32 length, payload} in order; ane_h16/result then holds one record
 * {u32 op, s32 status, u64 ns, u32 length, u32 0, data} per job record. The
 * host builds every firmware command; the kernel maps, sends and waits.
 */
static bool h16_job;
module_param(h16_job, bool, 0444);
MODULE_PARM_DESC(h16_job, "Diagnostic: after startup, run host-built jobs from debugfs ane_h16/job");

enum {
	ANE_H16_JOB_MAP = 1,	/* u64 dva, u64 size, data: zero-filled 16K-aligned mapping */
	ANE_H16_JOB_WRITE,	/* u64 dva, data */
	ANE_H16_JOB_FILL,	/* u64 dva, u64 size, u32 byte */
	ANE_H16_JOB_CMD,	/* u32 channel, u32 fill, command; result: the reply */
	ANE_H16_JOB_EVENT,	/* u64 uuid, u32 timeout ms; result: reports up to the terminal one */
	ANE_H16_JOB_PEEK,	/* u64 dva, u64 size; result: the bytes */
};

#define ANE_H16_JOB_FILL_PROGRAM BIT(0)	/* u32 program handle at +8 */
#define ANE_H16_JOB_FILL_PROCESS BIT(1)	/* u32 process handle at +0xc */
#define ANE_H16_JOB_RESULT_MAX SZ_64M

struct ane_h16_job {
	u8 *out;
	size_t used, size;
	u32 program, process;
	u64 published_ns;
};

static void *ane_h16_job_cpu(struct ane_h16_device *h, u64 dva, u64 size)
{
	struct ane_h16_mapping *m;

	list_for_each_entry(m, &h->mappings, node)
		if (m->cpu && dva >= m->dva && size <= m->size && dva - m->dva <= m->size - size)
			return (u8 *)m->cpu + (dva - m->dva);
	return NULL;
}

static int ane_h16_job_emit(struct ane_h16_job *j, u32 op, int status, u64 ns,
			    const void *data, size_t length)
{
	u8 *r;

	if (j->size - j->used < 24 || length > j->size - j->used - 24)
		return -ENOSPC;
	r = j->out + j->used;
	put_unaligned_le32(op, r);
	put_unaligned_le32(status, r + 4);
	put_unaligned_le64(ns, r + 8);
	put_unaligned_le32(length, r + 16);
	put_unaligned_le32(0, r + 20);
	if (length)
		memcpy(r + 24, data, length);
	j->used += 24 + length;
	return 0;
}

/* ane_fw_transport_wait with a 10-20 us poll, so job timings are not 1 ms steps. */
static int ane_h16_job_wait(struct ane_h16_device *h, unsigned int ch)
{
	struct ane_fw_transport *t = &h->boot.transport;
	u32 pending;
	int ret;

	for (;;) {
		ret = ane_fw_transport_poll(t, ch);
		if (ret != -EAGAIN)
			return ret;
		ret = ane_fw_transport_irq(t, &pending);
		if (ret)
			return ret;
		ret = ane_fw_transport_shared(t);
		if (ret && ret != -EAGAIN)
			return ret;
		if (ktime_get_ns() >= t->pending[ch].deadline_ns)
			return ane_fw_transport_fail(t, -ETIMEDOUT);
		ane_h16_mailbox(h);
		ret = ane_h16_faults(&h->boot, false);
		if (ret)
			return ane_fw_transport_fail(t, ret);
		usleep_range(10, 20);
	}
}

static int ane_h16_job_cmd(struct ane_h16_device *h, struct ane_h16_job *j,
			   const u8 *p, u32 length, u64 *ns)
{
	struct ane_fw_transport *t = &h->boot.transport;
	u32 channel, fill;
	u8 *command;
	u64 start;
	int ret;

	if (length < 16)
		return -EINVAL;
	channel = get_unaligned_le32(p);
	fill = get_unaligned_le32(p + 4);
	length -= 8;
	command = kmemdup(p + 8, length, GFP_KERNEL);
	if (!command)
		return -ENOMEM;
	if (fill & ANE_H16_JOB_FILL_PROGRAM)
		put_unaligned_le32(j->program, command + 8);
	if ((fill & ANE_H16_JOB_FILL_PROCESS) && length >= 16)
		put_unaligned_le32(j->process, command + 0xc);
	start = ktime_get_ns();
	ret = ane_fw_transport_publish(t, channel, command, length,
				       start + h->boot.build->command_timeout_ns);
	if (!ret) {
		j->published_ns = start;
		ret = ane_h16_job_wait(h, channel);
	}
	*ns = ktime_get_ns() - start;
	if (!ret || ret == -EREMOTEIO) {
		const u8 *reply = (u8 *)t->pool.cpu + (t->pending[channel].dva - t->pool.dva);
		u16 id = get_unaligned_le16(reply + 4);

		if (!ret && id == ANE_H16_LOAD_PROGRAM && length >= 0x1bc)
			j->program = get_unaligned_le32(reply + 0x1b8);
		if (!ret && id == ANE_H16_CREATE_PROCESS && length >= 0x10)
			j->process = get_unaligned_le32(reply + 0xc);
		memcpy(command, reply, length);
	}
	ret = ane_h16_job_emit(j, ANE_H16_JOB_CMD, ret, *ns, command, length) ?: ret;
	kfree(command);
	return ret;
}

/* Collect IO_T2H reports until the terminal event of @uuid or the timeout. */
static int ane_h16_job_event(struct ane_h16_device *h, struct ane_h16_job *j,
			     u64 uuid, u32 timeout_ms)
{
	struct ane_fw_transport *t = &h->boot.transport;
	u64 deadline = ktime_get_ns() + (u64)timeout_ms * NSEC_PER_MSEC;
	u8 report[0x80];
	int ret = -ETIMEDOUT;

	while (ktime_get_ns() < deadline) {
		size_t size = 0;
		u32 pending;
		int r = ane_fw_transport_consume(t, report, sizeof(report), &size);

		if (!r) {
			u64 ns = ktime_get_ns() - j->published_ns;

			r = ane_h16_job_emit(j, ANE_H16_JOB_EVENT, 0, ns, report, size);
			if (r)
				return r;
			if (size >= 0x20 && get_unaligned_le16(report + 4) == ANE_H16_EVENT_REPORT &&
			    get_unaligned_le64(report + 8) == uuid) {
				u32 state = get_unaligned_le32(report + 0x1c);

				if (state == ANE_H16_EVENT_SUCCESS)
					return 0;
				if (state == ANE_H16_EVENT_FAILURE)
					return -EIO;
			}
			continue;
		}
		if (r != -EAGAIN)
			return r;
		r = ane_fw_transport_irq(t, &pending);
		if (r)
			return r;
		r = ane_fw_transport_shared(t);
		if (r && r != -EAGAIN)
			return r;
		ane_h16_mailbox(h);
		ret = ane_h16_faults(&h->boot, false);
		if (ret)
			return ret;
		ret = -ETIMEDOUT;
		usleep_range(10, 20);
	}
	return ret;
}

static int ane_h16_job_run(struct ane_h16_device *h, struct ane_h16_job *j,
			   const u8 *p, size_t size)
{
	while (size >= 8) {
		u32 op = get_unaligned_le32(p), length = get_unaligned_le32(p + 4);
		const u8 *a = p + 8;
		u64 ns = 0;
		int ret = 0;

		if (length > size - 8)
			return -EINVAL;
		switch (op) {
		case ANE_H16_JOB_MAP: {
			struct ane_fw_surface s;
			u64 dva, bytes;

			if (length < 16)
				return -EINVAL;
			dva = get_unaligned_le64(a);
			bytes = get_unaligned_le64(a + 8);
			if (length - 16 > bytes)
				return -EINVAL;
			ret = ane_h16_alloc_at(h, dva, ALIGN(bytes, SZ_16K), &s);
			if (!ret)
				memcpy(s.cpu, a + 16, length - 16);
			break;
		}
		case ANE_H16_JOB_WRITE:
		case ANE_H16_JOB_FILL:
		case ANE_H16_JOB_PEEK: {
			u64 dva, bytes;
			u8 *cpu;

			if (length < (op == ANE_H16_JOB_WRITE ? 8 : 16))
				return -EINVAL;
			dva = get_unaligned_le64(a);
			bytes = op == ANE_H16_JOB_WRITE ? length - 8 : get_unaligned_le64(a + 8);
			cpu = ane_h16_job_cpu(h, dva, bytes);
			if (!cpu) {
				ret = -ENOENT;
				break;
			}
			if (op == ANE_H16_JOB_WRITE)
				memcpy(cpu, a + 8, bytes);
			else if (op == ANE_H16_JOB_FILL)
				memset(cpu, length >= 20 ? a[16] : 0, bytes);
			else
				ret = ane_h16_job_emit(j, op, 0, 0, cpu, bytes);
			break;
		}
		case ANE_H16_JOB_CMD:
			ret = ane_h16_job_cmd(h, j, a, length, &ns);
			if (ret)
				return ret;
			break;
		case ANE_H16_JOB_EVENT:
			if (length < 12)
				return -EINVAL;
			ret = ane_h16_job_event(h, j, get_unaligned_le64(a), get_unaligned_le32(a + 8));
			ns = ktime_get_ns() - j->published_ns;
			break;
		default:
			return -EINVAL;
		}
		if (op != ANE_H16_JOB_PEEK && op != ANE_H16_JOB_CMD)
			ane_h16_job_emit(j, op, ret, ns, NULL, 0);
		if (ret)
			return ret;
		p += 8 + length;
		size -= 8 + length;
	}
	return size ? -EINVAL : 0;
}

static ssize_t ane_h16_job_write(struct file *file, const char __user *buf, size_t count,
				 loff_t *ppos)
{
	struct ane_h16_job j = {};
	u8 *in;
	int ret;

	if (*ppos || count > SZ_256M)
		return -EINVAL;
	in = vmemdup_user(buf, count);
	if (IS_ERR(in))
		return PTR_ERR(in);
	j.size = ANE_H16_JOB_RESULT_MAX;
	j.out = vzalloc(j.size);
	if (!j.out) {
		kvfree(in);
		return -ENOMEM;
	}
	mutex_lock(&ane_h16_lock);
	if (!ane_h16_job_dev) {
		ret = -ENODEV;
	} else {
		ret = ane_h16_job_run(ane_h16_job_dev, &j, in, count);
		dev_info(ane_h16_job_dev->dev, "H16 job %zu bytes: %d, result %zu bytes\n",
			 count, ret, j.used);
	}
	vfree(ane_h16_job_result.data);
	ane_h16_job_result.data = j.out;
	ane_h16_job_result.size = j.used;
	mutex_unlock(&ane_h16_lock);
	kvfree(in);
	return ret ? ret : count;
}

static const struct file_operations ane_h16_job_fops = {
	.owner = THIS_MODULE,
	.write = ane_h16_job_write,
};

static void ane_h16_job_publish(struct ane_h16_device *h)
{
	if (!h16_job || ane_h16_job_dev)
		return;
	if (!ane_h16_debugfs)
		ane_h16_debugfs = debugfs_create_dir("ane_h16", NULL);
	ane_h16_job_dev = h;
	debugfs_create_file("job", 0200, ane_h16_debugfs, NULL, &ane_h16_job_fops);
	debugfs_create_blob("result", 0400, ane_h16_debugfs, &ane_h16_job_result);
	dev_info(h->dev, "H16 job interface in debugfs ane_h16/\n");
}

int ane_h16_backend_probe(struct platform_device *pdev, const struct ane_fw_platform *soc,
			  unsigned int stage)
{
	struct ane_h16_device *h;
	unsigned int i, j;
	int ret;

	mutex_lock(&ane_h16_lock);
	if (ane_h16_released) {
		ret = -EBUSY;
		goto unlock;
	}
	h = kzalloc_obj(*h);
	if (!h) {
		ret = -ENOMEM;
		goto unlock;
	}
	h->dev = &pdev->dev;
	h->irq = -1;
	INIT_LIST_HEAD(&h->mappings);
	h->boot.soc = soc;
	h->boot.build = soc->h16_boot;
	h->boot.ctx = h;
	h->boot.ops = &ane_h16_ops;
	h->boot.transport_ops = &ane_h16_transport_ops;
	h->shared_next = soc->h16_boot->shared_dva;
	ret = of_property_read_u64_array(h->dev->of_node, "apple,dapf-ranges",
					 &h->boot.ranges[0][0], 8);
	if (ret)
		goto cleanup;
	for (i = 0; i < 4; i++) {
		u64 start = h->boot.ranges[i][0], end = h->boot.ranges[i][1];

		if (!start || end < start || round_down(start, SZ_16K) != round_down(end, SZ_16K)) {
			ret = -EINVAL;
			goto cleanup;
		}
		for (j = 0; j < i; j++)
			if (round_down(start, SZ_16K) == round_down(h->boot.ranges[j][0], SZ_16K)) {
				ret = -EINVAL;
				goto cleanup;
			}
	}
	if (ane_h16_observing() && stage != ANE_H16_PREFLIGHT) {
		ret = -EPERM;
		goto cleanup;
	}
	ret = ane_h16_preload_identity(h);
	if (ret)
		goto cleanup;
	ret = ane_h16_acquire(pdev, h);
	if (ret)
		goto cleanup;
	ret = ane_h16_run(&h->boot, stage);
	if (ret && !h->boot.ran)
		goto cleanup;
	/* Bind even on a post-RUN error. devres teardown must never reclaim DMA. */
	platform_set_drvdata(pdev, h);
	if (!ret) {
		dev_info(h->dev, "H16 stage %u complete%s\n", stage,
			 h->boot.ran ? "; held until reboot" : "");
		if (stage == ANE_H16_STARTUP) {
			print_hex_dump(KERN_INFO, "H16 RESOURCE_INFO ", DUMP_PREFIX_OFFSET,
				       16, 1, h->boot.resource_info,
				       sizeof(h->boot.resource_info), false);
			ane_h16_job_publish(h);
		}
	}
	ret = 0;
	goto unlock;
cleanup:
	dev_err(h->dev, "H16 stage %u refused before RUN, error %d\n", stage, ret);
	ane_h16_cleanup(h);
unlock:
	mutex_unlock(&ane_h16_lock);
	return ret;
}

void ane_h16_backend_remove(struct platform_device *pdev)
{
	struct ane_h16_device *h = platform_get_drvdata(pdev);

	mutex_lock(&ane_h16_lock);
	if (h) {
		if (h->boot.ran)
			dev_warn(h->dev, "H16 remove held; reboot reclaims firmware DMA\n");
		else
			ane_h16_cleanup(h);
	}
	mutex_unlock(&ane_h16_lock);
}
