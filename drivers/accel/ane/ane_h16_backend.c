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
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "ane_fw_platform.h"
#include "ane_h16_accel.h"
#include "ane_h16_boot.h"
#include "ane_h16_preload.h"

#define ANE_H16_TX_WORDS 272
/* Private program window above the SHAREDMALLOC range; host policy. */
#define ANE_H16_PROGRAM_WINDOW SZ_1G

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
	u64 program_next;
	u64 program_end;
	struct dentry *debugfs;
	bool acquired;
	bool refused;
	bool program_ready;
	struct ane_h16_session session;
	struct work_struct start;
};

static DEFINE_MUTEX(ane_h16_lock);
static bool ane_h16_released;
/* <debugfs>/ane-h16, shared by bound devices; protected by ane_h16_lock. */
static struct dentry *ane_h16_debugfs;
static unsigned int ane_h16_debugfs_users;

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

static int ane_h16_alloc_prot(struct ane_h16_device *h, u64 dva, size_t size, int prot,
			      struct ane_fw_surface *out);

/*
 * The firmware span is config_span bytes from the TEXT DVA. The preload covers
 * TEXT and DATA only; the firmware keeps runtime heap state in the rest, which
 * the host supplies zeroed and device-writable before boot.
 */
static int ane_h16_map_heap_tail(struct ane_h16_device *h)
{
	struct ane_h16_boot *b = &h->boot;
	u64 tail = b->soc->preload_dva[1] + b->soc->preload_size[1];
	u64 end = b->soc->preload_dva[0] + b->build->config_span;
	struct ane_fw_surface s;
	int ret;

	if (tail == end)
		return 0;
	if (tail > end || !IS_ALIGNED(tail, SZ_16K) || !IS_ALIGNED(end, SZ_16K) ||
	    end > b->build->pool_dva)
		return -ERANGE;
	ret = ane_h16_alloc_prot(h, tail, end - tail,
				 IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, &s);
	if (ret)
		return ret;
	dev_info(h->dev, "H16 firmware heap tail mapped at %llx bytes %llx writable\n",
		 tail, end - tail);
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
	if (h->domain->geometry.aperture_end < h->program_end - 1)
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
	ret = ane_h16_map_heap_tail(h);
	if (ret)
		return ret;
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

static int ane_h16_alloc_prot(struct ane_h16_device *h, u64 dva, size_t size, int prot,
			      struct ane_fw_surface *out)
{
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
				prot, GFP_KERNEL);
		if (ret)
			return ret;
		m->mapped += SZ_16K;
	}
	*out = (struct ane_fw_surface) { .cpu = m->cpu, .dva = dva, .size = size };
	return 0;
}

static int ane_h16_alloc_at(void *ctx, u64 dva, size_t size, struct ane_fw_surface *out)
{
	return ane_h16_alloc_prot(ctx, dva, size, IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, out);
}

/* Program sections and surfaces. Only outputs and scratch are device-writable. */
static int ane_h16_program_map(void *ctx, size_t size, bool write, struct ane_fw_surface *out)
{
	struct ane_h16_device *h = ctx;
	u64 dva = h->program_next;
	int ret;

	if (!size || size > h->program_end - dva)
		return -ENOSPC;
	/* A failed mapping keeps its range; DVAs are never reused before reset. */
	h->program_next += size;
	ret = ane_h16_alloc_prot(h, dva, size,
				 IOMMU_READ | IOMMU_CACHE | (write ? IOMMU_WRITE : 0), out);
	if (!ret)
		dev_info(h->dev, "H16 program mapping %llx bytes %zx%s\n", dva, size,
			 write ? " writable" : "");
	return ret;
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

/*
 * The channel IRQ stays masked, so every wait polls. Sleep in short steps
 * first (see ane_fw_poll_sleep_us()): a fixed 1 ms sleep made each of the
 * four waits of an EXECUTE cost at least a millisecond.
 */
static void ane_h16_relax(void *ctx, u64 waited_ns)
{
	struct ane_h16_device *h = ctx;
	unsigned int min_us, max_us;
	int ret;

	ane_h16_mailbox(h);
	ret = ane_h16_faults(&h->boot, false);
	if (ret)
		ane_fw_transport_fail(&h->boot.transport, ret);
	min_us = ane_fw_poll_sleep_us(waited_ns, &max_us);
	usleep_range(min_us, max_us);
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

static const struct ane_h16_session_ops ane_h16_session_ops = {
	.map = ane_h16_program_map,
	.log = ane_h16_log,
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

/* Pre-RUN rollback of everything a stage acquired. h itself stays. */
static void ane_h16_release(struct ane_h16_device *h)
{
	struct ane_h16_mapping *m, *next;
	unsigned int i, pages_per_block = SZ_16K / PAGE_SIZE;

	if (h->boot.ran || h->pinned || !h->acquired)
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
	h->attached = false;
	if (h->domain)
		iommu_domain_free(h->domain);
	h->domain = NULL;
	if (h->claimed)
		iommu_device_release_dma_owner(h->dev);
	h->claimed = false;
	if (h->group)
		iommu_group_put(h->group);
	h->group = NULL;
	if (h->irq >= 0)
		free_irq(h->irq, h);
	h->irq = -1;
	for (i = ANE_H16_WINDOWS; i-- > 0; ) {
		if (h->window[i])
			iounmap(h->window[i]);
		h->window[i] = NULL;
	}
	while (h->darts_powered)
		pm_runtime_put_sync(h->dart[--h->darts_powered]);
	for (i = 0; i < 3; i++) {
		if (h->dart[i])
			put_device(h->dart[i]);
		h->dart[i] = NULL;
	}
	while (h->powered) {
		struct device *pd = h->power[--h->powered];

		pm_runtime_put_sync(pd);
		dev_pm_domain_detach(pd, true);
	}
	h->acquired = false;
	dev_info(h->dev, "H16 pre-RUN rollback complete; no CPU release\n");
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
		if (!h->window[ANE_H16_DART0 + i])
			return -ENOMEM;
		/* Only the instance the platform names has a DAPF bank. */
		if (i != h->boot.soc->dapf_dart)
			continue;
		h->window[ANE_H16_DAPF] = ioremap_np(r.start + h->boot.soc->dapf_offset,
						     h->boot.soc->dapf_size);
		if (!h->window[ANE_H16_DAPF])
			return -ENOMEM;
	}
	if (!h->window[ANE_H16_DAPF])
		return -ENODEV;
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
	const char *identity = NULL;
	unsigned int i;
	int ret = -ENOMEM;

	for (i = 0; i < 2; i++) {
		u64 base, size;
		void *live;

		ret = ane_fw_preload_range(h->dev->of_node, i, &base, &size);
		if (!ret && size != b->soc->preload_size[i])
			ret = -EINVAL;
		if (ret)
			goto out;
		ret = -ENOMEM;
		h->preload_phys[i] = base;
		snap[i] = vzalloc(size);
		second = vzalloc(size);
		if (!snap[i] || !second)
			goto out;
		live = memremap(base, size, MEMREMAP_WB);
		if (!live)
			goto out;
		memcpy(snap[i], live, size);
		/* Separate the two observations used to reject an unstable preload. */
		rmb();
		memcpy(second, live, size);
		memunmap(live);
		if (memcmp(snap[i], second, size)) {
			ret = -EBUSY;
			goto out;
		}
		vfree(second);
		second = NULL;
	}
	ret = -ENOEXEC;
	for (i = 0; i < b->soc->h16_profile_count && ret == -ENOEXEC; i++) {
		const struct ane_h16_profile *p = &b->soc->h16_profiles[i];
		u8 digest[SHA256_DIGEST_SIZE];

		ret = ane_h16_preload_digest(p->preload, snap[0], b->soc->preload_size[0],
					     snap[1], b->soc->preload_size[1], digest);
		if (ret)
			continue;
		if (memcmp(digest, p->preload->digest, sizeof(digest))) {
			/* The layout matched; name the image so a new build can be pinned. */
			dev_info(h->dev, "H16 preload has the %s layout but digest %*phN\n",
				 p->identity, (int)sizeof(digest), digest);
			ret = -ENOEXEC;
			continue;
		}
		b->build = p->build;
		identity = p->identity;
	}
	if (ret) {
		dev_err(h->dev, "H16 preload matches none of %u firmware profiles, error %d\n",
			b->soc->h16_profile_count, ret);
		goto out;
	}
	/* Firmware DVA layout belongs to the build just selected. */
	h->shared_next = b->build->shared_dva;
	h->program_next = b->build->shared_dva + ANE_FW_SHARED_TOTAL;
	h->program_end = h->program_next + ANE_H16_PROGRAM_WINDOW;
	dev_info(h->dev, "H16 preload identity %s accepted\n", identity);
out:
	vfree(second);
	vfree(snap[0]);
	vfree(snap[1]);
	return ret;
}

/* After startup: one program session and the ABI 3 accel node. */
static int ane_h16_program_stage(struct ane_h16_device *h)
{
	int ret;

	ret = ane_h16_session_init(&h->session, &h->boot.transport, &ane_h16_session_ops, h,
				   h->boot.build->command_timeout_ns);
	if (!ret)
		ret = ane_h16_accel_register(h->dev, &h->session);
	if (ret) {
		dev_err(h->dev, "H16 program stage unavailable, error %d; held until reboot\n",
			ret);
		return ret;
	}
	h->program_ready = true;
	dev_info(h->dev, "H16 stage 5 program window %llx..%llx\n", h->program_next,
		 h->program_end - 1);
	return 0;
}

/*
 * Advance the staged bring-up to @stage. Caller holds ane_h16_lock. Before
 * RUN any failure rolls back power and mappings and refuses further stages
 * until the module is reloaded. From RUN on, everything stays held.
 */
static int ane_h16_advance(struct ane_h16_device *h, enum ane_h16_stage stage)
{
	enum ane_h16_stage target = stage > ANE_H16_STARTUP ? ANE_H16_STARTUP : stage;
	int ret;

	if (h->refused || h->boot.failed || h->boot.quarantined)
		return -EBUSY;
	if (h->program_ready || (stage != ANE_H16_PROGRAM && stage <= h->boot.stage))
		return -EALREADY;
	if (!h->acquired) {
		h->acquired = true;
		ret = ane_h16_preload_identity(h);
		if (!ret)
			ret = ane_h16_acquire(to_platform_device(h->dev), h);
		if (ret)
			goto refuse;
	}
	if (target > h->boot.stage) {
		ret = ane_h16_run(&h->boot, target);
		if (ret && !h->boot.ran)
			goto refuse;
		if (ret) {
			dev_err(h->dev, "H16 stage %s failed after RUN, error %d; held until reboot\n",
				ane_h16_stage_name(stage), ret);
			return ret;
		}
		if (h->boot.stage == ANE_H16_STARTUP)
			print_hex_dump(KERN_INFO, "H16 RESOURCE_INFO ", DUMP_PREFIX_OFFSET, 16, 1,
				       h->boot.resource_info, sizeof(h->boot.resource_info),
				       false);
	}
	if (stage == ANE_H16_PROGRAM) {
		ret = ane_h16_program_stage(h);
		if (ret) {
			h->refused = true;
			return ret;
		}
	}
	dev_info(h->dev, "H16 stage %s complete%s\n", ane_h16_stage_name(stage),
		 h->boot.ran ? "; held until reboot" : "");
	return 0;
refuse:
	dev_err(h->dev, "H16 stage %s refused before RUN, error %d\n",
		ane_h16_stage_name(stage), ret);
	h->refused = true;
	ane_h16_release(h);
	return ret;
}

/*
 * The bootloader handed off a preload that passed metadata preflight, so the
 * engine is brought up to the program stage once per boot, off the probe
 * path. A failure before RUN leaves the device refused; after RUN it is held
 * until reboot, as for a stage written to debugfs.
 */
static void ane_h16_start_work(struct work_struct *work)
{
	struct ane_h16_device *h = container_of(work, struct ane_h16_device, start);

	mutex_lock(&ane_h16_lock);
	if (!h->acquired)
		ane_h16_advance(h, ANE_H16_PROGRAM);
	mutex_unlock(&ane_h16_lock);
}

/*
 * Diagnostic control in debugfs: read the stage state, or, before the boot
 * bring-up has run, write a stage name to <debugfs>/ane-h16/<dev>/stage to
 * run every stage up to it.
 */
static ssize_t ane_h16_stage_write(struct file *file, const char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct ane_h16_device *h = file->private_data;
	char buf[16];
	int stage, ret;

	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = 0;
	strim(buf);
	stage = ane_h16_stage_parse(buf);
	if (stage <= ANE_H16_REFUSE)
		return -EINVAL;
	mutex_lock(&ane_h16_lock);
	ret = ane_h16_advance(h, stage);
	mutex_unlock(&ane_h16_lock);
	return ret ? ret : count;
}

static ssize_t ane_h16_stage_read(struct file *file, char __user *ubuf, size_t count,
				  loff_t *ppos)
{
	struct ane_h16_device *h = file->private_data;
	char buf[96];
	int len;

	mutex_lock(&ane_h16_lock);
	len = scnprintf(buf, sizeof(buf), "%s ran=%d refused=%d quarantined=%d error=%d\n",
			h->program_ready ? "program" : ane_h16_stage_name(h->boot.stage),
			h->boot.ran, h->refused || h->boot.failed, h->boot.quarantined,
			h->boot.error);
	mutex_unlock(&ane_h16_lock);
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static const struct file_operations ane_h16_stage_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = ane_h16_stage_read,
	.write = ane_h16_stage_write,
	.llseek = default_llseek,
};

int ane_h16_backend_probe(struct platform_device *pdev, const struct ane_fw_platform *soc)
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
	/* The build, and with it the DVA layout, is chosen from the preload. */
	h->boot.build = NULL;
	h->boot.ctx = h;
	h->boot.ops = &ane_h16_ops;
	h->boot.transport_ops = &ane_h16_transport_ops;
	ret = of_property_read_u64_array(h->dev->of_node, "apple,dapf-ranges",
					 &h->boot.ranges[0][0], 8);
	if (ret)
		goto free;
	for (i = 0; i < 4; i++) {
		u64 start = h->boot.ranges[i][0], end = h->boot.ranges[i][1];

		ret = -EINVAL;
		if (!start || end < start || round_down(start, SZ_16K) != round_down(end, SZ_16K))
			goto free;
		for (j = 0; j < i; j++)
			if (round_down(start, SZ_16K) == round_down(h->boot.ranges[j][0], SZ_16K))
				goto free;
	}
	if (!ane_h16_debugfs_users++)
		ane_h16_debugfs = debugfs_create_dir("ane-h16", NULL);
	h->debugfs = debugfs_create_dir(dev_name(h->dev), ane_h16_debugfs);
	debugfs_create_file("stage", 0600, h->debugfs, h, &ane_h16_stage_fops);
	platform_set_drvdata(pdev, h);
	INIT_WORK(&h->start, ane_h16_start_work);
	schedule_work(&h->start);
	ret = 0;
	goto unlock;
free:
	dev_err(h->dev, "H16 DT description refused, error %d\n", ret);
	kfree(h);
unlock:
	mutex_unlock(&ane_h16_lock);
	return ret;
}

void ane_h16_backend_remove(struct platform_device *pdev)
{
	struct ane_h16_device *h = platform_get_drvdata(pdev);

	if (!h)
		return;
	/* Waits for the boot bring-up and any stage write in progress. */
	cancel_work_sync(&h->start);
	debugfs_remove_recursive(h->debugfs);
	mutex_lock(&ane_h16_lock);
	if (!--ane_h16_debugfs_users) {
		debugfs_remove_recursive(ane_h16_debugfs);
		ane_h16_debugfs = NULL;
	}
	if (h->boot.ran || h->pinned) {
		/* The module reference taken before RUN makes this unreachable by rmmod. */
		dev_warn(h->dev, "H16 remove held; reboot reclaims firmware DMA\n");
	} else {
		ane_h16_release(h);
		kfree(h);
	}
	mutex_unlock(&ane_h16_lock);
}
