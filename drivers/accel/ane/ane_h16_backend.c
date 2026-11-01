// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Staged H16 backend, using the accepted H16 cold-boot facts, round 5. */
#include <linux/debugfs.h>
#include <linux/cpumask.h>
#include <linux/irq.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/sched/cputime.h>
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
#include <linux/scatterlist.h>
#include <linux/timex.h>
#include <linux/vmalloc.h>

#include "ane_h16_boot.h"
#include "ane_fw_irq_wait.h"
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

#define ANE_H16_CONSUME_TRACE_MAX 256
struct ane_h16_consume_trace {
	struct ane_fw_consume_trace transport;
	u64 begin_counter, end_counter, uuid;
	u32 token, command_id, state, begin_cpu, end_cpu, irq_before, irq_after;
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
	bool job_irq_mode;
	struct ane_h16_consume_trace *consume_trace;
	u32 consume_trace_count, consume_trace_dropped;
	struct ane_fw_irq_event irq_event;
	atomic_t irq_calls, irq_notifications, irq_empty, irq_empty_run, irq_disabled;
	atomic_t irq_waits, irq_woken, irq_timeouts;
	atomic_t irq_status_count[ANE_FW_CHANNELS];
	cpumask_t irq_seen_cpus;
	u32 irq_job_base[6];
	u32 mailbox_rx_count;
	struct ane_fw_rtk rtk;
	struct {
		u64 word0;
		u64 word1;
	} tx[ANE_H16_TX_WORDS];
	u32 tx_read;
	u32 tx_write;
	bool rtk_failed;
	u64 shared_next;
	bool startup_tracing;
	u32 startup_trace_count, startup_trace_dropped;
	struct {
		u64 ns, counter;
		u32 type, value, shared_count;
	} startup_trace[128];
};

static bool h16_job_irq;
module_param(h16_job_irq, bool, 0444);
MODULE_PARM_DESC(h16_job_irq, "Diagnostic IRQ completion after startup, default0; incompatible with quiet probes");

static bool h16_job_trace;
module_param(h16_job_trace, bool, 0444);
MODULE_PARM_DESC(h16_job_trace, "Bounded consume-stage RAM timestamps, max256 reports/default0; perturbs timing");

static bool h16_map_batch;
module_param(h16_map_batch, bool, 0444);
MODULE_PARM_DESC(h16_map_batch, "Diagnostic private surface DART mapping in one SG transaction, default0");

static DEFINE_MUTEX(ane_h16_lock);
static bool ane_h16_released;

/* Diagnostic, workspace ane: observe an unknown resident image. */
static unsigned int h16_startup_poll_us = 1000;
module_param(h16_startup_poll_us, uint, 0444);
MODULE_PARM_DESC(h16_startup_poll_us, "Startup wait service cadence10..1000us, default1000");
static unsigned int h16_startup_drain_poll_us = 10;
module_param(h16_startup_drain_poll_us, uint, 0444);
MODULE_PARM_DESC(h16_startup_drain_poll_us, "Pre-IO service cadence10..1000us, default10");
static bool h16_startup_trace;
module_param(h16_startup_trace, bool, 0444);
MODULE_PARM_DESC(h16_startup_trace, "Record bounded startup allocation/doorbell timestamps, print only after probe");

static char *h16_identity;
module_param(h16_identity, charp, 0444);
MODULE_PARM_DESC(h16_identity, "observe: accept an unknown resident image for preflight only and publish it in debugfs ane_h16/");

static const char *const ane_h16_build_names[ANE_H16_PRELOAD_BUILDS] = {
	"H16 27.0 26A428", "H16 26.5.2 25F84",
};

static struct debugfs_blob_wrapper ane_h16_dump[2];
static struct dentry *ane_h16_debugfs;
/* Failure-only copies; firmware remains live, so these are not atomic dumps. */
static struct debugfs_blob_wrapper ane_h16_failure[4 + ANE_FW_SHARED_MAX];

static bool ane_h16_observing(void)
{
	return h16_identity && !strcmp(h16_identity, "observe");
}

static struct ane_h16_device *ane_h16_job_dev;
static struct debugfs_blob_wrapper ane_h16_job_result;

void ane_h16_backend_exit(void)
{
	unsigned int i;

	debugfs_remove_recursive(ane_h16_debugfs);
	ane_h16_debugfs = NULL;
	vfree(ane_h16_dump[0].data);
	vfree(ane_h16_dump[1].data);
	vfree(ane_h16_job_result.data);
	for (i = 0; i < ARRAY_SIZE(ane_h16_failure); i++)
		vfree(ane_h16_failure[i].data);
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
		h->mailbox_rx_count++;
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
	/* One provider TLB synchronization for the complete surface, retaining
	 * identical pages/protection/DVA. iommu_map_sgtable rolls back on error.
	 */
	if (h16_map_batch) {
		struct sg_table sgt;
		ssize_t mapped;

		ret = sg_alloc_table_from_pages(&sgt, m->pages, size / PAGE_SIZE,
						0, size, GFP_KERNEL);
		if (ret)
			return ret;
		mapped = iommu_map_sgtable(h->domain, dva, &sgt,
					  IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE);
		sg_free_table(&sgt);
		if (mapped < 0)
			return mapped;
		m->mapped = mapped;
		if ((size_t)mapped != size)
			return -EIO;
	} else {
		/* Every backing page was allocated with __GFP_ZERO above. */
		for (i = 0; i < m->blocks; i++) {
			ret = iommu_map(h->domain, dva + i * SZ_16K,
					page_to_phys(m->pages[i * pages_per_block]), SZ_16K,
					IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, GFP_KERNEL);
			if (ret)
				return ret;
			m->mapped += SZ_16K;
		}
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

static void ane_h16_startup_record(struct ane_h16_device *h, u32 type, u32 value)
{
	u32 i;

	if (!h->startup_tracing)
		return;
	i = h->startup_trace_count;
	if (i == ARRAY_SIZE(h->startup_trace)) {
		h->startup_trace_dropped++;
		return;
	}
	h->startup_trace[i].ns = ktime_get_ns();
	h->startup_trace[i].counter = get_cycles();
	h->startup_trace[i].type = type;
	h->startup_trace[i].value = value;
	h->startup_trace[i].shared_count = h->boot.transport.shared_count;
	h->startup_trace_count++;
}

static void ane_h16_engine_write(void *ctx, u32 offset, u32 value)
{
	struct ane_h16_device *h = ctx;

	ane_h16_mmio_write32(ctx, ANE_H16_ENGINE, offset, value);
	if (offset == h->boot.soc->doorbell)
		ane_h16_startup_record(h, 1, value);
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
	ane_h16_delay(h, clamp(h16_startup_poll_us, 10U, 1000U));
}

static int ane_h16_alloc_shared(void *ctx, size_t size, u32 tag, struct ane_fw_surface *out)
{
	struct ane_h16_device *h = ctx;
	int ret;

	ane_h16_startup_record(h, 2, tag);
	ret = ane_h16_alloc_at(h, h->shared_next, size, out);

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
	kfree(h->consume_trace);
	kfree(h);
}

/* Only this handler owns AP channel-status acknowledgment in IRQ mode.
 * All transport/ring/allocation state remains in the process-context worker.
 */
static void ane_h16_irq_fault(struct ane_h16_device *h, int error)
{
	if (!atomic_cmpxchg(&h->irq_disabled, 0, 1))
		disable_irq_nosync(h->irq);
	ane_fw_irq_event_notify(&h->irq_event, error);
}

static irqreturn_t ane_h16_channel_irq(int irq, void *data)
{
	struct ane_h16_device *h = data;
	u32 value;
	unsigned int i;

	if (!READ_ONCE(h->job_irq_mode))
		return IRQ_NONE;
	cpumask_set_cpu(raw_smp_processor_id(), &h->irq_seen_cpus);
	atomic_inc(&h->irq_calls);
	if (atomic_read(&h->irq_disabled))
		return IRQ_HANDLED;
	value = ane_h16_engine_read(h, h->boot.soc->irq_status);
	if (value & ~GENMASK(7, 0)) {
		ane_h16_irq_fault(h, -EPROTO);
		return IRQ_HANDLED;
	}
	if (!value) {
		atomic_inc(&h->irq_empty);
		/* A wrong source/clear mapping must not storm on this level line. */
		if (atomic_inc_return(&h->irq_empty_run) >= 32) {
			ane_h16_irq_fault(h, -EIO);
			return IRQ_HANDLED;
		}
		return IRQ_NONE;
	}
	atomic_set(&h->irq_empty_run, 0);
	ane_h16_engine_write(h, h->boot.soc->irq_ack, value);
	/* engine_write completes the ACK barrier BEFORE sequence publication. */
	for (i = 0; i < ANE_FW_CHANNELS; i++)
		if (value & BIT(i))
			atomic_inc(&h->irq_status_count[i]);
	atomic_inc(&h->irq_notifications);
	ane_fw_irq_event_notify(&h->irq_event, 0);
	return IRQ_HANDLED;
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
	ane_fw_irq_event_init(&h->irq_event);
	cpumask_clear(&h->irq_seen_cpus);
	atomic_set(&h->irq_calls, 0);
	atomic_set(&h->irq_notifications, 0);
	atomic_set(&h->irq_empty, 0);
	atomic_set(&h->irq_empty_run, 0);
	atomic_set(&h->irq_disabled, 0);
	atomic_set(&h->irq_waits, 0);
	atomic_set(&h->irq_woken, 0);
	atomic_set(&h->irq_timeouts, 0);
	for (i = 0; i < ANE_FW_CHANNELS; i++)
		atomic_set(&h->irq_status_count[i], 0);
	ret = request_irq(irq, ane_h16_channel_irq, IRQF_NO_AUTOEN, "ane-h16-channel", h);
	if (ret)
		return ret;
	h->irq = irq;
	dev_info(h->dev, "H16 startup polling; channel IRQ held masked until startup completes\n");
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
 * {u32 op, s32 status, u64 ns, u32 length, u32 trailer, data} per job record. The
 * host builds every firmware command; the kernel maps, sends and waits.
 */
static unsigned int h16_startup_drain_ms;
module_param(h16_startup_drain_ms, uint, 0444);
MODULE_PARM_DESC(h16_startup_drain_ms, "Service startup allocation/log channels before first IO, max1000ms, default0");

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
	ANE_H16_JOB_QUIET,	/* result only: UUID, handles, requested us, busy, CPU; two counter anchors */
	ANE_H16_JOB_GAP,	/* result only: UUID, requested us, CPU; two gap counter anchors */
};

#define ANE_H16_JOB_FILL_PROGRAM BIT(0)	/* u32 program handle at +8 */
#define ANE_H16_JOB_FILL_PROCESS BIT(1)	/* u32 process handle at +0xc */
#define ANE_H16_JOB_RESULT_MAX SZ_64M
static unsigned int h16_job_result_mb = 64;
module_param(h16_job_result_mb, uint, 0444);
MODULE_PARM_DESC(h16_job_result_mb, "Diagnostic result allocation1..64MiB, default64");

#define ANE_H16_JOB_DONE 64

struct ane_h16_job {
	u8 *out;
	size_t used, size;
	u32 program, process;
	u64 published_ns;
	/* Terminal events consumed while waiting for another UUID (pipelined). */
	struct {
		u64 uuid;
		u32 state;
	} done[ANE_H16_JOB_DONE];
	unsigned int done_next;
	/* Pool offset where job commands start, and EXECUTEs not yet terminal. */
	size_t pool_mark;
	bool marked;
	unsigned int outstanding;
	u64 last_exec_uuid;
	u32 last_exec_handles[3], quiet_warm_handles[3];
	bool quiet_warmed, quiet_batch_pipelined;
};

static void *ane_h16_job_cpu(struct ane_h16_device *h, u64 dva, u64 size)
{
	struct ane_h16_mapping *m;

	list_for_each_entry(m, &h->mappings, node)
		if (m->cpu && dva >= m->dva && size <= m->size && dva - m->dva <= m->size - size)
			return (u8 *)m->cpu + (dva - m->dva);
	return NULL;
}

/* @trailer counts host architectural-counter values after @data; they count
 * in the length. Decode with the host arch_timer frequency, independently of
 * the firmware event clock (M4 Linux1GHz host versus24MHz firmware).
 */
static int ane_h16_job_emit_counters(struct ane_h16_job *j, u32 op, int status, u64 ns,
				     const void *data, size_t length,
				     const u64 *counters, u32 trailer)
{
	size_t total = length + trailer * sizeof(u64);
	u8 *r;
	u32 i;

	if (j->size - j->used < 24 || total > j->size - j->used - 24)
		return -ENOSPC;
	r = j->out + j->used;
	put_unaligned_le32(op, r);
	put_unaligned_le32(status, r + 4);
	put_unaligned_le64(ns, r + 8);
	put_unaligned_le32(total, r + 16);
	put_unaligned_le32(trailer, r + 20);
	if (length)
		memcpy(r + 24, data, length);
	for (i = 0; i < trailer; i++)
		put_unaligned_le64(counters[i], r + 24 + length + i * sizeof(u64));
	j->used += 24 + total;
	return 0;
}

static int ane_h16_job_emit(struct ane_h16_job *j, u32 op, int status, u64 ns,
			    const void *data, size_t length)
{
	return ane_h16_job_emit_counters(j, op, status, ns, data, length, NULL, 0);
}

/* Optional short-request spin before sleeping; default preserves polled mode.
 * Bound CPU cost to 1 ms per wait even if a larger diagnostic value is supplied.
 */
static unsigned int h16_job_spin_us;
module_param(h16_job_spin_us, uint, 0444);
MODULE_PARM_DESC(h16_job_spin_us, "Spin up to this many us before sleeping in job waits (max 1000, default 0)");

/* Mechanism probe, not a latency optimization: after the EXECUTE slot ACK,
 * withhold transport/MMIO service briefly while firmware runs. At most 10 ms;
 * the existing completion deadline and post-window servicing remain in force.
 * Default zero leaves the diagnostic polling path unchanged.
 */
static unsigned int h16_job_quiet_us;
module_param(h16_job_quiet_us, uint, 0444);
MODULE_PARM_DESC(h16_job_quiet_us, "Diagnostic no-poll interval before event drain (0..10000 us, default 0)");
static bool h16_job_quiet_busy;
module_param(h16_job_quiet_busy, bool, 0444);
MODULE_PARM_DESC(h16_job_quiet_busy, "Use private busy work instead of one sleep during no-poll interval");

static unsigned int h16_job_quiet_wake_us;
module_param(h16_job_quiet_wake_us, uint, 0444);
MODULE_PARM_DESC(h16_job_quiet_wake_us, "Optional private periodic wakes inside quiet window (10..1000 us,0 disables)");
static unsigned int h16_job_quiet_work_ns;
module_param(h16_job_quiet_work_ns, uint, 0444);
MODULE_PARM_DESC(h16_job_quiet_work_ns, "Private work after each quiet wake, at most10000ns");
static unsigned int h16_job_gap_us;
module_param(h16_job_gap_us, uint, 0444);
MODULE_PARM_DESC(h16_job_gap_us, "Diagnostic recovery gap before warmed serial EXECUTE, max100us/default0; included in throughput");
static unsigned int h16_job_poll_us = 10;
module_param(h16_job_poll_us, uint, 0444);
MODULE_PARM_DESC(h16_job_poll_us, "Diagnostic job polling sleep10..1000us, default10");

static int ane_h16_job_quiet(struct ane_h16_job *j, u64 uuid)
{
	unsigned int us = min_t(unsigned int, h16_job_quiet_us, 10000);
	u64 until, start, counters[2], user_before, sys_before, user_after, sys_after;
	u32 wake_us = h16_job_quiet_wake_us ? clamp(h16_job_quiet_wake_us, 10U, 1000U) : 0;
	u32 work_ns = min(h16_job_quiet_work_ns, 10000U), wakes = 0, active_ns = 0;
	u8 data[56];

	/* Only a warmed, single outstanding EXEC with the exact same identity.
	 * Cached/pipelined completions must not masquerade as quiet execution.
	 */
	if (!us || !uuid || j->outstanding != 1 || j->quiet_batch_pipelined ||
	    uuid != j->last_exec_uuid ||
	    !j->quiet_warmed || memcmp(j->last_exec_handles,
				     j->quiet_warm_handles, sizeof(j->last_exec_handles)))
		return 0;
	put_unaligned_le64(uuid, data);
	memcpy(data + 8, j->last_exec_handles, sizeof(j->last_exec_handles));
	put_unaligned_le32(us, data + 20);
	put_unaligned_le32(h16_job_quiet_busy, data + 24);
	put_unaligned_le32(raw_smp_processor_id(), data + 28);
	task_cputime_adjusted(current, &user_before, &sys_before);
	start = ktime_get_ns();
	counters[0] = get_cycles();
	if (wake_us) {
		until = start + (u64)us * NSEC_PER_USEC;
		while (ktime_get_ns() < until) {
			u64 began, work_until;

			usleep_range(wake_us, wake_us + 10);
			began = ktime_get_ns();
			if (began >= until)
				break;
			work_until = min(began + work_ns, until);
			while (ktime_get_ns() < work_until)
				cpu_relax();
			active_ns += ktime_get_ns() - began;
			wakes++;
		}
	} else if (!h16_job_quiet_busy) {
		usleep_range(us, us + 50);
	} else {
		until = start + (u64)us * NSEC_PER_USEC;
		while (ktime_get_ns() < until)
			cpu_relax();
	}
	counters[1] = get_cycles();
	task_cputime_adjusted(current, &user_after, &sys_after);
	put_unaligned_le32(wake_us, data + 32);
	put_unaligned_le32(work_ns, data + 36);
	put_unaligned_le32(wakes, data + 40);
	/* Completed private blocks exclude the final wake beyond the deadline.
	 * active_ns is block wall time, including any preemption/IRQ service.
	 * Scheduler-accounted task CPU delta has its own resolution limits.
	 */
	put_unaligned_le32(active_ns, data + 44);
	put_unaligned_le64(user_after + sys_after - user_before - sys_before, data + 48);
	return ane_h16_job_emit_counters(j, ANE_H16_JOB_QUIET, 0,
				       ktime_get_ns() - start, data, sizeof(data), counters, 2);
}

static u64 ane_h16_job_spin_deadline(void)
{
	if (!h16_job_spin_us)
		return 0;
	return ktime_get_ns() + (u64)min_t(unsigned int, h16_job_spin_us, 1000) * NSEC_PER_USEC;
}

static void ane_h16_job_relax(u64 spin_deadline)
{
	if (spin_deadline && ktime_get_ns() < spin_deadline)
		cpu_relax();
	else {
		unsigned int us = clamp(h16_job_poll_us, 10U, 1000U);

		usleep_range(us, us + 10);
	}
}

static int ane_h16_job_irq_error(struct ane_h16_device *h)
{
	int error = h->job_irq_mode ? ane_fw_irq_event_error(&h->irq_event) : 0;

	return error ? ane_fw_transport_fail(&h->boot.transport, error) : 0;
}

/* All coalesced auxiliary requests must be drained before blocking. */
static int ane_h16_job_irq_service(struct ane_h16_device *h, bool *progress)
{
	struct ane_fw_transport *t = &h->boot.transport;
	u64 terminal_before = t->terminal_messages;
	u32 rx_before = h->mailbox_rx_count, tx_before = h->tx_read;
	int ret = ane_fw_transport_shared_drain(t, progress);

	if (ret)
		return ret;
	ret = ane_fw_transport_terminal(t);
	if (ret)
		return ret;
	ane_h16_mailbox(h);
	if (terminal_before != t->terminal_messages || rx_before != h->mailbox_rx_count ||
	    tx_before != h->tx_read)
		*progress = true;
	ret = ane_h16_faults(&h->boot, false);
	return ret ? ret : ane_h16_job_irq_error(h);
}

static int ane_h16_job_irq_pause(struct ane_h16_device *h, unsigned int sequence,
				 u64 deadline, u64 spin_deadline)
{
	u64 now = ktime_get_ns(), remaining;
	int ret;

	if (now >= deadline)
		return ane_fw_transport_fail(&h->boot.transport, -ETIMEDOUT);
	if (spin_deadline && now < spin_deadline) {
		cpu_relax();
		return ane_h16_job_irq_error(h);
	}
	remaining = min_t(u64, deadline - now, 10 * NSEC_PER_MSEC);
	atomic_inc(&h->irq_waits);
	ret = ane_fw_irq_event_wait(&h->irq_event, sequence, ns_to_ktime(remaining));
	if (ret < 0)
		return ane_fw_transport_fail(&h->boot.transport, ret);
	if (!ret)
		atomic_inc(&h->irq_timeouts);
	else
		atomic_inc(&h->irq_woken);
	return ane_h16_job_irq_error(h);
}

/* ane_fw_transport_wait with a 10-20 us poll, so job timings are not 1 ms steps. */
static int ane_h16_job_wait(struct ane_h16_device *h, unsigned int ch)
{
	struct ane_fw_transport *t = &h->boot.transport;
	u32 pending;
	u64 spin_deadline = ane_h16_job_spin_deadline();
	int ret;

	for (;;) {
		unsigned int sequence = ane_fw_irq_event_sequence(&h->irq_event);
		bool progress;

		ret = ane_h16_job_irq_error(h);
		if (ret)
			return ret;
		ret = ane_fw_transport_poll(t, ch);
		if (ret != -EAGAIN)
			return ane_h16_job_irq_error(h) ?: ret;
		if (h->job_irq_mode) {
			ret = ane_h16_job_irq_service(h, &progress);
			if (ret)
				return ane_fw_transport_fail(t, ret);
			if (ktime_get_ns() >= t->pending[ch].deadline_ns)
				return ane_fw_transport_fail(t, -ETIMEDOUT);
			ane_fw_transport_rering(t, ch);
			if (progress)
				continue;
			ret = ane_h16_job_irq_pause(h, sequence, t->pending[ch].deadline_ns,
						     spin_deadline);
			if (ret)
				return ret;
			continue;
		}
		ret = ane_fw_transport_irq(t, &pending);
		if (ret)
			return ret;
		ret = ane_fw_transport_shared(t);
		if (ret && ret != -EAGAIN)
			return ret;
		ret = ane_fw_transport_terminal(t);
		if (ret)
			return ret;
		if (ktime_get_ns() >= t->pending[ch].deadline_ns)
			return ane_fw_transport_fail(t, -ETIMEDOUT);
		ane_fw_transport_rering(t, ch);
		ane_h16_mailbox(h);
		ret = ane_h16_faults(&h->boot, false);
		if (ret)
			return ane_fw_transport_fail(t, ret);
		ane_h16_job_relax(spin_deadline);
	}
}

static int ane_h16_job_cmd(struct ane_h16_device *h, struct ane_h16_job *j,
			   const u8 *p, u32 length, u64 *ns)
{
	struct ane_fw_transport *t = &h->boot.transport;
	u32 channel, fill;
	u8 *command;
	u64 start, counters[2];
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
	/*
	 * The transport appends commands to its pool and never reuses it. Wrap to
	 * the job's first command once nothing is outstanding.
	 */
	if (!j->marked) {
		j->pool_mark = t->pool_used;
		j->marked = true;
	}
	if (ALIGN(t->pool_used, ANE_FW_SLOT_SIZE) + length > t->pool.size && !j->outstanding)
		t->pool_used = j->pool_mark;
	/* A diagnostic recovery interval for RPC report retirement. Never hold
	 * back an already-pipelined request or the first call of a new identity.
	 * Its separate record keeps the interval in complete cycle accounting.
	 */
	if (h16_job_gap_us && length >= 0x28 &&
	    get_unaligned_le16(command + 4) == ANE_H16_EXECUTE &&
	    !j->outstanding && j->quiet_warmed &&
	    !memcmp(command + 8, j->quiet_warm_handles, sizeof(j->quiet_warm_handles))) {
		u8 gap[16];
		u64 gap_counters[2], began;
		u32 us = min(h16_job_gap_us, 100U);

		put_unaligned_le64(get_unaligned_le64(command + 0x20), gap);
		put_unaligned_le32(us, gap + 8);
		put_unaligned_le32(raw_smp_processor_id(), gap + 12);
		began = ktime_get_ns();
		gap_counters[0] = get_cycles();
		udelay(us);
		gap_counters[1] = get_cycles();
		ret = ane_h16_job_emit_counters(j, ANE_H16_JOB_GAP, 0,
					      ktime_get_ns() - began, gap, sizeof(gap), gap_counters, 2);
		if (ret) {
			kfree(command);
			return ret;
		}
	}
	start = ktime_get_ns();
	counters[0] = get_cycles();
	ret = ane_fw_transport_publish(t, channel, command, length,
				       start + h->boot.build->command_timeout_ns);
	if (!ret) {
		j->published_ns = start;
		if (get_unaligned_le16(command + 4) == ANE_H16_EXECUTE) {
			if (j->outstanding)
				j->quiet_batch_pipelined = true;
			j->outstanding++;
			j->last_exec_uuid = 0;
			if (length >= 0x28) {
				j->last_exec_uuid = get_unaligned_le64(command + 0x20);
				memcpy(j->last_exec_handles, command + 8,
				       sizeof(j->last_exec_handles));
			}
		}
		ret = ane_h16_job_wait(h, channel);
	}
	counters[1] = get_cycles();
	*ns = ktime_get_ns() - start;
	if (t->pending[channel].rerings)
		dev_info(h->dev, "H16 job command %x on ch %u: %u doorbell re-rings, %d\n",
			 get_unaligned_le16(command + 4), channel, t->pending[channel].rerings, ret);
	if (!ret || ret == -EREMOTEIO) {
		const u8 *reply = (u8 *)t->pool.cpu + (t->pending[channel].dva - t->pool.dva);
		u16 id = get_unaligned_le16(reply + 4);

		if (!ret && id == ANE_H16_LOAD_PROGRAM && length >= 0x1bc)
			j->program = get_unaligned_le32(reply + 0x1b8);
		if (!ret && id == ANE_H16_CREATE_PROCESS && length >= 0x10)
			j->process = get_unaligned_le32(reply + 0xc);
		memcpy(command, reply, length);
	}
	ret = ane_h16_job_emit_counters(j, ANE_H16_JOB_CMD, ret, *ns, command, length,
					counters, 2) ?: ret;
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
	u64 spin_deadline = ane_h16_job_spin_deadline();
	int ret = -ETIMEDOUT;
	unsigned int i;

	ret = ane_h16_job_irq_error(h);
	if (ret)
		return ret;
	for (i = 0; i < ANE_H16_JOB_DONE; i++) {
		if (j->done[i].uuid != uuid || !uuid)
			continue;
		j->done[i].uuid = 0;
		return ane_h16_job_irq_error(h) ?: (j->done[i].state == ANE_H16_EVENT_SUCCESS ? 0 : -EIO);
	}
	ret = ane_h16_job_quiet(j, uuid);
	if (ret)
		return ret;
	while (ktime_get_ns() < deadline) {
		size_t size = 0;
		u32 pending;
		unsigned int sequence = ane_fw_irq_event_sequence(&h->irq_event);
		bool progress;
		int r = ane_h16_job_irq_error(h);
		struct ane_h16_consume_trace trace;
		bool tracing = h->consume_trace &&
			h->consume_trace_count < ANE_H16_CONSUME_TRACE_MAX;

		if (r)
			return r;
		if (tracing) {
			trace.begin_cpu = raw_smp_processor_id();
			trace.irq_before = atomic_read(&h->irq_calls);
			trace.begin_counter = get_cycles();
		}
		r = ane_fw_transport_consume_traced(t, report, sizeof(report), &size,
						tracing ? &trace.transport : NULL);
		if (!r) {
			u64 ns = ktime_get_ns() - j->published_ns;
			u64 counter = get_cycles();

			/* Preserve the original EVENT endpoint before trace bookkeeping. */
			if (tracing) {
				trace.end_counter = counter;
				trace.end_cpu = raw_smp_processor_id();
				trace.irq_after = atomic_read(&h->irq_calls);
				trace.token = get_unaligned_le32(report);
				trace.command_id = get_unaligned_le16(report + 4);
				trace.uuid = 0;
				trace.state = U32_MAX;
				if (trace.command_id == ANE_H16_EVENT_REPORT && size >= 0x20) {
					trace.uuid = get_unaligned_le64(report + 8);
					trace.state = get_unaligned_le32(report + 0x1c);
				}
				h->consume_trace[h->consume_trace_count++] = trace;
			} else if (h->consume_trace) {
				h->consume_trace_dropped++;
			}
			r = ane_h16_job_emit_counters(j, ANE_H16_JOB_EVENT, 0, ns, report, size,
						      &counter, 1);
			if (r)
				return r;
			if (size >= 0x20 && get_unaligned_le16(report + 4) == ANE_H16_EVENT_REPORT) {
				u64 id = get_unaligned_le64(report + 8);
				u32 state = get_unaligned_le32(report + 0x1c);
				bool terminal = state == ANE_H16_EVENT_SUCCESS ||
						state == ANE_H16_EVENT_FAILURE;

				if (terminal && j->outstanding) {
					j->outstanding--;
					if (!j->outstanding)
						j->quiet_batch_pipelined = false;
				}
				if (id == uuid && terminal) {
					if (state == ANE_H16_EVENT_SUCCESS && size >= 0x30 &&
					    id == j->last_exec_uuid &&
					    !memcmp(report + 0x10, j->last_exec_handles,
						    sizeof(j->last_exec_handles))) {
						memcpy(j->quiet_warm_handles, j->last_exec_handles,
						       sizeof(j->quiet_warm_handles));
						j->quiet_warmed = true;
					}
					return ane_h16_job_irq_error(h) ?: (state == ANE_H16_EVENT_SUCCESS ? 0 : -EIO);
				}
				if (terminal) {
					j->done[j->done_next].uuid = id;
					j->done[j->done_next].state = state;
					j->done_next = (j->done_next + 1) % ANE_H16_JOB_DONE;
				}
			}
			continue;
		}
		if (r != -EAGAIN)
			return r;
		if (h->job_irq_mode) {
			r = ane_h16_job_irq_service(h, &progress);
			if (r)
				return ane_fw_transport_fail(t, r);
			if (progress)
				continue;
			r = ane_h16_job_irq_pause(h, sequence, deadline, spin_deadline);
			if (r)
				return r;
			ret = -ETIMEDOUT;
			continue;
		}
		r = ane_fw_transport_irq(t, &pending);
		if (r)
			return r;
		r = ane_fw_transport_shared(t);
		if (r && r != -EAGAIN)
			return r;
		r = ane_fw_transport_terminal(t);
		if (r)
			return r;
		ane_h16_mailbox(h);
		ret = ane_h16_faults(&h->boot, false);
		if (ret)
			return ret;
		ret = -ETIMEDOUT;
		ane_h16_job_relax(spin_deadline);
	}
	/* A quiet/progress path may leave ret==0; expiry is never success. */
	return ane_h16_job_irq_error(h) ?: -ETIMEDOUT;
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
			ns = ktime_get_ns();
			ret = ane_h16_alloc_at(h, dva, ALIGN(bytes, SZ_16K), &s);
			if (!ret)
				memcpy(s.cpu, a + 16, length - 16);
			ns = ktime_get_ns() - ns;
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
			if (op != ANE_H16_JOB_PEEK)
				ns = ktime_get_ns();
			if (op == ANE_H16_JOB_WRITE)
				memcpy(cpu, a + 8, bytes);
			else if (op == ANE_H16_JOB_FILL)
				memset(cpu, length >= 20 ? a[16] : 0, bytes);
			else
				ret = ane_h16_job_emit(j, op, 0, 0, cpu, bytes);
			if (op != ANE_H16_JOB_PEEK)
				ns = ktime_get_ns() - ns;
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

static void ane_h16_irq_job_begin(struct ane_h16_device *h)
{
	h->irq_job_base[0] = atomic_read(&h->irq_calls);
	h->irq_job_base[1] = atomic_read(&h->irq_notifications);
	h->irq_job_base[2] = atomic_read(&h->irq_empty);
	h->irq_job_base[3] = atomic_read(&h->irq_waits);
	h->irq_job_base[4] = atomic_read(&h->irq_woken);
	h->irq_job_base[5] = atomic_read(&h->irq_timeouts);
}

static int ane_h16_irq_stats_show(struct seq_file *s, void *unused)
{
	struct ane_h16_device *h = s->private;
	const struct cpumask *effective = irq_get_effective_affinity_mask(h->irq);
	u32 calls = (u32)atomic_read(&h->irq_calls) - h->irq_job_base[0];
	u32 notifications = (u32)atomic_read(&h->irq_notifications) - h->irq_job_base[1];
	u32 empty = (u32)atomic_read(&h->irq_empty) - h->irq_job_base[2];
	u32 waits = (u32)atomic_read(&h->irq_waits) - h->irq_job_base[3];
	u32 woken = (u32)atomic_read(&h->irq_woken) - h->irq_job_base[4];
	u32 timeouts = (u32)atomic_read(&h->irq_timeouts) - h->irq_job_base[5];
	int error = ane_fw_irq_event_error(&h->irq_event);
	unsigned int i;

	if (!effective)
		effective = cpu_possible_mask;
	seq_printf(s, "{\"available\":true,\"mode\":%s,\"active\":%s,\"linux_irq\":%d,",
		   h->job_irq_mode ? "true" : "false",
		   h->job_irq_mode && !atomic_read(&h->irq_disabled) ? "true" : "false", h->irq);
	seq_printf(s, "\"calls\":%u,\"notifications\":%u,\"empty\":%u,\"waits\":%u,\"woken\":%u,\"fallbacks\":%u,",
		   calls, notifications, empty, waits, woken, timeouts);
	seq_printf(s, "\"delivered\":%s,\"wait_notified\":%s,\"error\":%d,\"ok\":%s,",
		   notifications ? "true" : "false", woken ? "true" : "false", error,
		   !error && !atomic_read(&h->irq_disabled) ? "true" : "false");
	seq_printf(s, "\"handler_cpus_since_enable\":\"%*pbl\",\"effective_affinity\":\"%*pbl\",\"status_counts_since_enable\":[",
		   cpumask_pr_args(&h->irq_seen_cpus), cpumask_pr_args(effective));
	for (i = 0; i < ANE_FW_CHANNELS; i++)
		seq_printf(s, "%s%u", i ? "," : "", (u32)atomic_read(&h->irq_status_count[i]));
	seq_puts(s, "]}\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ane_h16_irq_stats);

static ssize_t ane_h16_job_write(struct file *file, const char __user *buf, size_t count,
				 loff_t *ppos)
{
	struct ane_h16_job j = {};
	u8 *in;
	int ret;

	if (*ppos || count > SZ_256M || !h16_job_result_mb || h16_job_result_mb > 64)
		return -EINVAL;
	in = vmemdup_user(buf, count);
	if (IS_ERR(in))
		return PTR_ERR(in);
	j.size = (size_t)h16_job_result_mb * SZ_1M;
	j.out = vzalloc(j.size);
	if (!j.out) {
		kvfree(in);
		return -ENOMEM;
	}
	mutex_lock(&ane_h16_lock);
	if (!ane_h16_job_dev) {
		ret = -ENODEV;
	} else {
		ane_h16_job_dev->consume_trace_count = 0;
		ane_h16_job_dev->consume_trace_dropped = 0;
		ane_h16_irq_job_begin(ane_h16_job_dev);
		ret = ane_h16_job_irq_error(ane_h16_job_dev);
		if (!ret)
			ret = ane_h16_job_run(ane_h16_job_dev, &j, in, count);
		ret = ane_h16_job_irq_error(ane_h16_job_dev) ?: ret;
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

/*
 * Diagnostic, workspace ane: macOS 25F84 ends ANE_Init with these three engine
 * writes after its startup commands (re/25F84-kext.md, epilogue 0x92dfbf0).
 */
static bool h16_postboot;
module_param(h16_postboot, bool, 0444);
MODULE_PARM_DESC(h16_postboot, "After startup, do macOS's post-boot writes 0x1880058=1, 0x1880060=0, 0x196c000|=1");

static void ane_h16_postboot(struct ane_h16_device *h)
{
	void __iomem *e = h->window[ANE_H16_ENGINE];
	u32 a = readl(e + 0x1880058), b = readl(e + 0x1880060), c = readl(e + 0x196c000);

	writel(1, e + 0x1880058);
	writel(0, e + 0x1880060);
	writel(c | 1, e + 0x196c000);
	dev_info(h->dev, "H16 post-boot writes: 0x1880058 %x->%x 0x1880060 %x->%x 0x196c000 %x->%x\n",
		 a, readl(e + 0x1880058), b, readl(e + 0x1880060), c, readl(e + 0x196c000));
}

static int ane_h16_consume_trace_show(struct seq_file *m, void *unused)
{
	struct ane_h16_device *h = m->private;
	u32 i, k;

	mutex_lock(&ane_h16_lock);
	seq_printf(m, "{\"schema\":1,\"enabled\":%s,\"count\":%u,\"dropped\":%u,\"records\":[",
		   h->consume_trace ? "true" : "false", h->consume_trace_count,
		   h->consume_trace_dropped);
	for (i = 0; i < h->consume_trace_count; i++) {
		struct ane_h16_consume_trace *r = &h->consume_trace[i];

		seq_printf(m, "%s{\"index\":%u,\"dva\":%llu,\"length\":%llu,\"slot\":%u,\"token\":%u,\"command_id\":%u,\"uuid\":%llu,\"state\":%u,",
			   i ? "," : "", i, r->transport.dva, r->transport.length,
			   r->transport.slot, r->token, r->command_id, r->uuid, r->state);
		seq_printf(m, "\"begin_counter\":%llu,\"end_counter\":%llu,\"begin_cpu\":%u,\"end_cpu\":%u,\"irq_before\":%u,\"irq_after\":%u,\"ns\":[",
			   r->begin_counter, r->end_counter, r->begin_cpu, r->end_cpu,
			   r->irq_before, r->irq_after);
		for (k = 0; k < ARRAY_SIZE(r->transport.ns); k++)
			seq_printf(m, "%s%llu", k ? "," : "", r->transport.ns[k]);
		seq_puts(m, "]}");
	}
	seq_puts(m, "]}\n");
	mutex_unlock(&ane_h16_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ane_h16_consume_trace);

static void ane_h16_job_publish(struct ane_h16_device *h)
{
	if (!h16_job || ane_h16_job_dev)
		return;
	if (!ane_h16_debugfs)
		ane_h16_debugfs = debugfs_create_dir("ane_h16", NULL);
	if (h16_job_trace) {
		h->consume_trace = kcalloc(ANE_H16_CONSUME_TRACE_MAX,
					       sizeof(*h->consume_trace), GFP_KERNEL);
		if (!h->consume_trace)
			return;
	}
	if (h16_job_irq) {
		u32 pending;
		int ret = ane_fw_transport_irq(&h->boot.transport, &pending);

		if (ret) {
			dev_err(h->dev, "H16 IRQ preflight refused: %d\n", ret);
			return;
		}
		WRITE_ONCE(h->job_irq_mode, true);
		enable_irq(h->irq);
		dev_info(h->dev, "H16 job IRQ mode enabled on Linux IRQ %d\n", h->irq);
	}
	ane_h16_job_dev = h;
	debugfs_create_file("consume_trace", 0400, ane_h16_debugfs, h, &ane_h16_consume_trace_fops);
	debugfs_create_file("irq_stats", 0400, ane_h16_debugfs, h, &ane_h16_irq_stats_fops);
	debugfs_create_file("job", 0200, ane_h16_debugfs, NULL, &ane_h16_job_fops);
	debugfs_create_blob("result", 0400, ane_h16_debugfs, &ane_h16_job_result);
	dev_info(h->dev, "H16 job interface in debugfs ane_h16/\n");
}

/* Never alter DMA ownership or reset the firmware while observing a failure. */
static void ane_h16_failure_copy(struct ane_h16_device *h, unsigned int index,
				const char *name, const void *cpu, size_t size, u64 dva)
{
	struct debugfs_blob_wrapper *blob = &ane_h16_failure[index];

	if (!cpu || !size || blob->data)
		return;
	blob->data = vmalloc(size);
	if (!blob->data)
		return;
	dma_rmb();
	memcpy(blob->data, cpu, size);
	blob->size = size;
	debugfs_create_blob(name, 0400, ane_h16_debugfs, blob);
	dev_info(h->dev, "H16 failure snapshot %s DVA=%llx bytes=%zx\n", name, dva, size);
}

static void ane_h16_failure_publish(struct ane_h16_device *h)
{
	struct ane_h16_boot *b = &h->boot;
	struct ane_fw_transport *t = &b->transport;
	void *live;
	unsigned int i;

	/* 25F84 fw IRQ receiver reads this pending register (firmware 0x8c90).
	 * Read only: no inferred clear/ack semantics are used here.
	 */
	if (b->soc->doorbell == 0x1884000) {
		u32 idle = readl(h->window[ANE_H16_ENGINE] + 0x1600048);

		dev_info(h->dev, "H16 failure firmware IRQ pending=%x\n",
			 readl(h->window[ANE_H16_ENGINE] + 0x1888000));
		/* 25F84 ANE_deInit reads32 and calls mask3 nonzero CPU-in-WFI. */
		dev_info(h->dev, "H16 failure ASCWRAP_IDLE_STATUS=%x AppleWFI=%u\n",
			 idle, !!(idle & 3));
	}
	if (!ane_h16_debugfs)
		ane_h16_debugfs = debugfs_create_dir("ane_h16", NULL);
	live = memremap(h->preload_phys[1], b->soc->preload_size[1], MEMREMAP_WB);
	if (live) {
		ane_h16_failure_copy(h, 0, "failure-data", live,
				     b->soc->preload_size[1], b->soc->preload_dva[1]);
		memunmap(live);
	}
	ane_h16_failure_copy(h, 1, "failure-ipc", b->ipc.cpu, b->ipc.size, b->ipc.dva);
	ane_h16_failure_copy(h, 2, "failure-pool", b->pool.cpu, b->pool.size, b->pool.dva);
	ane_h16_failure_copy(h, 3, "failure-heap", b->heap.cpu, b->heap.size, b->heap.dva);
	for (i = 0; i < t->shared_count; i++) {
		char name[32];

		snprintf(name, sizeof(name), "failure-shared-%u", i);
		ane_h16_failure_copy(h, 4 + i, name, t->shared[i].cpu,
				     t->shared[i].size, t->shared[i].dva);
	}
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
	if (h16_job_irq && h16_job_quiet_us) {
		dev_err(&pdev->dev, "H16 IRQ and quiet probes cannot be combined\n");
		ret = -EINVAL;
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
	h->boot.startup_drain_ms = min(h16_startup_drain_ms, 1000U);
	h->boot.startup_drain_poll_us = clamp(h16_startup_drain_poll_us, 10U, 1000U);
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
	h->startup_tracing = h16_startup_trace;
	ret = ane_h16_run(&h->boot, stage);
	h->startup_tracing = false;
	for (i = 0; i < h->startup_trace_count; i++)
		dev_info(h->dev, "H16 STARTUP_TRACE %u type=%u value=%x shared=%u ns=%llu cnt=%llu\n",
			 i, h->startup_trace[i].type, h->startup_trace[i].value,
			 h->startup_trace[i].shared_count, h->startup_trace[i].ns,
			 h->startup_trace[i].counter);
	if (h->startup_trace_dropped)
		dev_info(h->dev, "H16 STARTUP_TRACE dropped=%u\n", h->startup_trace_dropped);
	if (ret && !h->boot.ran)
		goto cleanup;
	/* Bind even on a post-RUN error. devres teardown must never reclaim DMA. */
	platform_set_drvdata(pdev, h);
	if (ret && h16_job)
		ane_h16_failure_publish(h);
	if (!ret) {
		dev_info(h->dev, "H16 stage %u complete%s\n", stage,
			 h->boot.ran ? "; held until reboot" : "");
		if (stage == ANE_H16_STARTUP) {
			print_hex_dump(KERN_INFO, "H16 RESOURCE_INFO ", DUMP_PREFIX_OFFSET,
				       16, 1, h->boot.resource_info,
				       sizeof(h->boot.resource_info), false);
			if (h16_postboot)
				ane_h16_postboot(h);
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
