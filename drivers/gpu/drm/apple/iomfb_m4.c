// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple DCP firmware 26.6.2 (T8132/J713).
 *
 * The appledrm component and KMS paths own this controller.  This backend owns
 * only its generation-specific EP37 RPC ABI and retained firmware session.
 * Firmware can keep every published DMA address until an exact successor
 * completion; an uncertain transaction stays pinned until reboot.
 */
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_platform.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/crc32.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/workqueue.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <drm/drm_fourcc.h>

#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/soc/apple/dart.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/soc/apple/pmp-display.h>

#include "dcp.h"
#include "iomfb_m4.h"

#include "dcp-m4-profile.h"
#include "dcp-m4-protocol.h"
#include "dcp-m4-transaction.h"
#include "dcp-m4-timing.h"
#include "dcp-m4-memory.h"
#include "dcp-m4-callback.h"
#include "dcp-m4-property.h"
#include "dcp-m4-frame.h"

struct iomfb_m4 {
	struct apple_dcp *owner;
	struct device *dev;
	struct device *scanout;
	struct iommu_group *scanout_group;
	resource_size_t coproc_phys;
	size_t coproc_size;
	struct work_struct backlight_work;
	void __iomem *coproc;
	struct apple_rtkit *rtkit;
	struct mutex lock;
	spinlock_t state_lock;
	struct completion reply;
	struct dcp_m4_transaction transaction;
	struct platform_device *piodma;
	struct dcp_m4_memory *memory;
	struct dcp_m4_runtime *runtime;
	void *rpc;
	dma_addr_t rpc_dva;
	u64 ready_word;
	size_t allocated;
	int error;
	bool started, init_sent, ready, startup_started;
	bool powered_off; /* setPowerState(0) for system sleep */
	bool display_off; /* setPowerState(0) while blanked, until the next frame */
	u64 display_off_cycles;
	s64 display_off_us, display_off_us_max, display_on_us, display_on_us_max;
	/* Exact completed successor or blank is the only framebuffer retirement. */
	struct drm_framebuffer *active_fb;
	struct drm_framebuffer *uncertain_fb;
	struct dcp_m4_frame active_frame;
	u64 flips, blanks, brightness_swaps;
};

static void dcp_m4_runtime_wake(struct iomfb_m4 *dcp);
static int dcp_m4_set_power_state(struct iomfb_m4 *dcp, bool on);
static void dcp_m4_runtime_receive(struct iomfb_m4 *dcp, u64 word);

static int dcp_m4_dart_check(struct device *dev)
{
	return apple_dart_dcp_check(dev);
}

static int dcp_m4_dart_check_buffer(struct device *dev, dma_addr_t iova, size_t size)
{
	return apple_dart_dcp_check_buffer(dev, iova, size);
}

static void *dcp_m4_dma_alloc(struct device *dev, size_t size, dma_addr_t *dva)
{
	return apple_dart_dcp_alloc(dev, size, dva);
}

static int dcp_m4_pmp_get(struct device *dev)
{
	return apple_pmp_j713_display_get(dev);
}

static int dcp_m4_pmp_check(struct device *dev)
{
	return apple_pmp_j713_display_check(dev);
}

static bool dcp_m4_range(u64 base, u64 length, u64 addr, u64 size)
{
	return length && size && addr >= base && addr - base < length &&
		size <= length - (addr - base);
}

/* Only the consumer's own no-map ASC reservations may back firmware buffers. */
static bool dcp_m4_reserved(struct iomfb_m4 *dcp, u64 pa, size_t size, bool oslog)
{
	struct device_node *node;
	struct resource res;
	bool found = false;

	for (int i = 0; (node = of_parse_phandle(dcp->dev->of_node, "memory-region", i)); i++) {
		struct reserved_mem *rmem = of_reserved_mem_lookup(node);

		if (of_device_is_compatible(node, "apple,asc-mem") &&
		    of_property_read_bool(node, "no-map") &&
		    oslog == of_property_read_bool(node, "apple,dcp-os-log") &&
		    !of_address_to_resource(node, 0, &res) &&
		    rmem && rmem->base == res.start && rmem->size == resource_size(&res) &&
		    dcp_m4_range(res.start, resource_size(&res), pa, size))
			found = true;
		of_node_put(node);
		if (found)
			break;
	}
	return found;
}

static void dcp_m4_failed(struct iomfb_m4 *dcp, int error)
{
	unsigned long flags;
	bool first;

	spin_lock_irqsave(&dcp->state_lock, flags);
	first = !dcp->error;
	if (first)
		dcp->error = error;
	dcp->ready = false;
	WRITE_ONCE(dcp->owner->crashed, true);
	spin_unlock_irqrestore(&dcp->state_lock, flags);
	if (first)
		dev_err(dcp->dev, "Retained DCP session quarantined: %d\n", error);
	complete_all(&dcp->reply);
	dcp_m4_runtime_wake(dcp);
}

static int dcp_m4_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct iomfb_m4 *dcp = cookie;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dcp->dev);
	phys_addr_t pa;
	int ret = -EINVAL;

	if (!bfr->size || bfr->size > SZ_16M || dcp->allocated > SZ_64M - bfr->size ||
	    dcp_m4_dart_check(dcp->dev))
		goto fail;
	if (!bfr->iova) {
		bfr->buffer = dcp_m4_dma_alloc(dcp->dev, bfr->size, &bfr->iova);
		if (IS_ERR(bfr->buffer)) {
			ret = PTR_ERR(bfr->buffer);
			bfr->buffer = NULL;
			goto fail;
		}
		/* Allocation stays pinned even if subsequent transport validation fails. */
		dcp->allocated += bfr->size;
		if (!dcp_m4_range(1ULL << 40, 1ULL << 36, bfr->iova, bfr->size) ||
		    dcp_m4_dart_check(dcp->dev))
			goto fail;
		bfr->is_mapped = false;
		dev_info(dcp->dev, "RTKit allocated %zu bytes at %pad\n", bfr->size, &bfr->iova);
		return 0;
	}
	/* OSLOG is explicitly marked physical; other requests are SID23 DVAs. */
	if (dcp_m4_reserved(dcp, bfr->iova, bfr->size, true)) {
		/* A physical address can also fall in the DVA aperture; refuse ambiguity. */
		for (size_t off = 0; off < bfr->size;) {
			if (iommu_iova_to_phys(domain, bfr->iova + off))
				goto fail;
			off += SZ_16K - ((bfr->iova + off) & (SZ_16K - 1));
		}
		if (dcp_m4_dart_check(dcp->dev))
			goto fail;
		pa = bfr->iova;
	} else {
		if (!dcp_m4_range(1ULL << 40, 1ULL << 36, bfr->iova, bfr->size))
			goto fail;
		ret = dcp_m4_dart_check_buffer(dcp->dev, bfr->iova, bfr->size);
		if (ret)
			goto fail;
		ret = -EINVAL;
		pa = iommu_iova_to_phys(domain, bfr->iova);
		if (!pa || !dcp_m4_reserved(dcp, pa, bfr->size, false))
			goto fail;
		/* Include the initial unaligned span and every crossed 16-KiB page. */
		for (size_t off = SZ_16K - (bfr->iova & (SZ_16K - 1)); off < bfr->size;
		     off += SZ_16K)
			if (iommu_iova_to_phys(domain, bfr->iova + off) != pa + off)
				goto fail;
	}
	/* Firmware payload is Normal-NC, independent of coherent WB table walks. */
	bfr->iomem = ioremap_wc(pa, bfr->size);
	if (!bfr->iomem) {
		ret = -ENOMEM;
		goto fail;
	}
	bfr->is_mapped = true;
	dcp->allocated += bfr->size;
	dev_info(dcp->dev, "RTKit borrowed %zu bytes at %pad -> %pa\n",
		 bfr->size, &bfr->iova, &pa);
	return 0;
fail:
	dcp_m4_failed(dcp, ret);
	return ret;
}

static void dcp_m4_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	/* No qualified ownership-return boundary yet. Never recycle firmware DMA. */
}

static void dcp_m4_crashed(void *cookie, const void *log, size_t size)
{
	struct iomfb_m4 *dcp = cookie;

	dev_err(dcp->dev, "RTKit crashed; retaining session and DMA until reboot\n");
	dcp_m4_failed(dcp, -EIO);
}

static void dcp_m4_receive(void *cookie, u8 endpoint, u64 message)
{
	struct iomfb_m4 *dcp = cookie;
	unsigned long flags;
	int ret;

	dev_dbg(dcp->dev, "RTKit EP%02x word %016llx\n", endpoint, message);
	if (endpoint != DCP_M4_ENDPOINT) {
		dcp_m4_failed(dcp, -EPROTO);
		return;
	}
	if (READ_ONCE(dcp->runtime)) {
		dcp_m4_runtime_receive(dcp, message);
		return;
	}
	spin_lock_irqsave(&dcp->state_lock, flags);
	ret = dcp_m4_ready_notification(message, dcp->init_sent, dcp->ready_word);
	if (ret == 1)
		dcp->ready_word = message;
	spin_unlock_irqrestore(&dcp->state_lock, flags);
	if (ret < 0)
		dcp_m4_failed(dcp, ret);
	else if (ret == 1)
		complete_all(&dcp->reply);
}

static const struct apple_rtkit_ops dcp_m4_rtkit_ops = {
	.recv_message = dcp_m4_receive,
	.crashed = dcp_m4_crashed,
	.shmem_setup = dcp_m4_shmem_setup,
	.shmem_destroy = dcp_m4_shmem_destroy,
};

static int dcp_m4_adopt(struct iomfb_m4 *dcp)
{
	struct device *dev = dcp->dev;
	unsigned long flags;
	u64 init_word;
	int ret;

	if (dcp->started) {
		ret = -EBUSY;
		goto out;
	}
	ret = dcp_m4_dart_check(dev);
	if (ret)
		goto out;
	if (!(readl(dcp->coproc + 0x44) & BIT(4))) {
		ret = -ENODEV;
		goto out;
	}
	/* From the first message onward, neither failure nor unbind may tear down. */
	dcp->started = true;
	dev_info(dev, "Starting warm RTKit adoption; retaining firmware ownership\n");
	ret = apple_rtkit_wake(dcp->rtkit);
	if (ret)
		goto fail;
	if (READ_ONCE(dcp->error)) {
		ret = READ_ONCE(dcp->error);
		goto fail;
	}
	if (!apple_rtkit_has_endpoint(dcp->rtkit, DCP_M4_ENDPOINT)) {
		ret = -ENODEV;
		goto fail;
	}
	dev_info(dev, "RTKit awake; endpoint 0x37 advertised\n");
	dcp->rpc = dcp_m4_dma_alloc(dev, DCP_M4_RPC_SIZE, &dcp->rpc_dva);
	if (IS_ERR(dcp->rpc)) {
		ret = PTR_ERR(dcp->rpc);
		dcp->rpc = NULL;
		goto fail;
	}
	ret = dcp_m4_dart_check(dev);
	if (ret)
		goto fail;
	ret = dcp_m4_init_word(dcp->rpc_dva, &init_word);
	if (ret)
		goto fail;
	if (!dcp_m4_range(1ULL << 40, 1ULL << 36, dcp->rpc_dva, DCP_M4_RPC_SIZE)) {
		ret = -ERANGE;
		goto fail;
	}
	memset(dcp->rpc, 0, DCP_M4_RPC_SIZE);
	put_unaligned_le32(DCP_M4_ALIGNMENT, dcp->rpc + 12);
	dma_wmb();
	ret = apple_rtkit_start_ep(dcp->rtkit, DCP_M4_ENDPOINT);
	if (ret)
		goto fail;
	dev_info(dev, "Sending INIT %016llx RPC=%pad size=%x alignment=%u\n",
		 init_word, &dcp->rpc_dva, DCP_M4_RPC_SIZE, DCP_M4_ALIGNMENT);
	spin_lock_irqsave(&dcp->state_lock, flags);
	dcp->init_sent = true;
	spin_unlock_irqrestore(&dcp->state_lock, flags);
	ret = apple_rtkit_send_message(dcp->rtkit, DCP_M4_ENDPOINT, init_word, NULL, false);
	if (ret)
		goto fail;
	if (!wait_for_completion_timeout(&dcp->reply, msecs_to_jiffies(3000))) {
		ret = -ETIMEDOUT;
		goto fail;
	}
	ret = READ_ONCE(dcp->error);
	if (ret)
		goto fail;
	if (!dcp_m4_ready(dcp->ready_word) || !(readl(dcp->coproc + 0x44) & BIT(4))) {
		ret = -EPROTO;
		goto fail;
	}
	spin_lock_irqsave(&dcp->state_lock, flags);
	ret = dcp->error;
	if (!ret)
		dcp->ready = true;
	spin_unlock_irqrestore(&dcp->state_lock, flags);
	if (ret)
		goto fail;
	dev_info(dev, "DCPLink READY %016llx\n",
		 dcp->ready_word);
	ret = 0;
	goto out;
fail:
	dcp_m4_failed(dcp, ret);
	dev_err(dev, "READY failed: %d; session pinned until guarded reboot\n", ret);
out:
	return ret;
}
static int dcp_m4_shared_alloc(void *cookie, u32 size, u64 *phys, u64 *dva)
{
	struct iomfb_m4 *dcp = cookie;

	return apple_dart_dcp_shared_alloc(dcp->dev, &dcp->piodma->dev, size, phys, dva);
}

static int dcp_m4_shared_map(void *cookie, u64 phys, u64 dva, u32 size)
{
	struct iomfb_m4 *dcp = cookie;

	return apple_dart_dcp_shared_map(dcp->dev, &dcp->piodma->dev, phys, dva, size);
}

static const struct dcp_m4_memory_ops dcp_m4_memory_ops = {
	.alloc = dcp_m4_shared_alloc,
	.map = dcp_m4_shared_map,
};

static int dcp_m4_create_piodma(struct iomfb_m4 *dcp)
{
	struct device_node *node, *alias;
	struct platform_device *existing;
	int ret = -ENODEV;

	node = of_get_child_by_name(dcp->dev->of_node, "piodma");
	alias = of_find_node_by_path("disp0_piodma");
	if (!node || node != alias || !of_device_is_available(node) ||
	    !of_property_read_bool(node, "apple,j713-inherited-mappings"))
		goto out;
	existing = of_find_device_by_node(node);
	if (existing) {
		put_device(&existing->dev);
		ret = -EBUSY;
		goto out;
	}
	dcp->piodma = of_platform_device_create(node, NULL, dcp->dev);
	if (!dcp->piodma)
		goto out;
	/* Keep the child even on failure: retained-domain attachment can have begun. */
	ret = dma_set_mask_and_coherent(&dcp->piodma->dev, DMA_BIT_MASK(42));
	if (!ret)
		ret = of_dma_configure(&dcp->piodma->dev, node, true);
	if (!ret)
		ret = dcp_m4_dart_check(&dcp->piodma->dev);
out:
	of_node_put(alias);
	of_node_put(node);
	return ret;
}

struct dcp_m4_runtime {
	struct iomfb_m4 *dcp;
	struct dcp_m4_properties *properties;
	struct dcp_m4_timing timing;
	struct dcp_m4_swap_state swap;
	struct completion received;
	struct workqueue_struct *wq;
	struct work_struct idle;
	u64 queue[32];
	u32 head, count, callbacks;
	unsigned long deadline;
	bool done;
	bool background_started, background_done;
	u32 background_id;
	bool power_started, power_done;
	bool scanout_started, scanout_done;
	bool qualified; /* KMS startup qualification passed; see dcp_m4_runtime_live() */
	void *scanout_cpu;
	struct dcp_m4_frame scanout_frame;
};

static void dcp_m4_runtime_wake(struct iomfb_m4 *dcp)
{
	struct dcp_m4_runtime *r = smp_load_acquire(&dcp->runtime);

	if (r)
		complete_all(&r->received);
}

static void dcp_m4_runtime_receive(struct iomfb_m4 *dcp, u64 word)
{
	struct dcp_m4_runtime *r = smp_load_acquire(&dcp->runtime);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&dcp->state_lock, flags);
	if (dcp->error)
		ret = dcp->error;
	else if (r->count == ARRAY_SIZE(r->queue)) {
		/* Publish failure before another owner can consume a queued reply. */
		dcp->error = ret = -ENOSPC;
		dcp->ready = false;
	} else {
		r->queue[(r->head + r->count++) % ARRAY_SIZE(r->queue)] = word;
		complete(&r->received);
	}
	spin_unlock_irqrestore(&dcp->state_lock, flags);
	if (ret)
		dcp_m4_failed(dcp, ret);
	else
		queue_work(r->wq, &r->idle);
}

/* Called only while holding dcp->lock, by the synchronous RPC owner or idle worker. */
static int dcp_m4_runtime_next(struct dcp_m4_runtime *r, u64 *word, bool wait)
{
	struct iomfb_m4 *dcp = r->dcp;
	unsigned long flags, now;
	int ret;

	if (READ_ONCE(dcp->error))
		return READ_ONCE(dcp->error);
	if (wait) {
		now = jiffies;
		if (time_after_eq(now, r->deadline) ||
		    !wait_for_completion_timeout(&r->received, r->deadline - now))
			return -ETIMEDOUT;
	} else if (!try_wait_for_completion(&r->received)) {
		return -EAGAIN;
	}
	spin_lock_irqsave(&dcp->state_lock, flags);
	ret = dcp->error ? dcp->error : !r->count ? -EPROTO : 0;
	if (!ret) {
		*word = r->queue[r->head++ % ARRAY_SIZE(r->queue)];
		r->count--;
	}
	spin_unlock_irqrestore(&dcp->state_lock, flags);
	return ret;
}

static int dcp_m4_runtime_health(struct dcp_m4_runtime *r)
{
	struct iomfb_m4 *dcp = r->dcp;
	int ret = READ_ONCE(dcp->error);

	if (!ret && (!(readl(dcp->coproc + 0x44) & BIT(4)) ||
	    dcp_m4_dart_check(dcp->dev) || dcp_m4_dart_check(&dcp->piodma->dev)))
		ret = -EIO;
	if (!ret && r->power_done)
		ret = dcp_m4_pmp_check(dcp->dev);
	if (!ret && dcp->scanout)
		ret = dcp_m4_dart_check(dcp->scanout);
	return ret;
}

/*
 * Full DART/PMP/CPU qualification runs through startup and KMS creation. After
 * that, per-message, per-swap and per-frame checks are only the failure latch;
 * RTKit crashes and protocol errors still latch it asynchronously. The full
 * checks remain available on demand through scanout_probe/pmp_vote reads.
 */
static int dcp_m4_runtime_live(struct dcp_m4_runtime *r)
{
	if (READ_ONCE(r->qualified))
		return READ_ONCE(r->dcp->error);
	return dcp_m4_runtime_health(r);
}

static int dcp_m4_runtime_send(struct dcp_m4_runtime *r, u64 word)
{
	struct iomfb_m4 *dcp = r->dcp;
	int ret = dcp_m4_runtime_live(r);

	if (ret)
		return ret;
	dma_wmb();
	return apple_rtkit_send_message(dcp->rtkit, DCP_M4_ENDPOINT, word, NULL, false);
}

static int dcp_m4_runtime_call(void *cookie, unsigned int method, bool remote,
				const void *input, u32 input_size, void *output, u32 output_size);

static int dcp_m4_runtime_property(void *cookie, unsigned int method,
				    const void *input, void *output)
{
	struct dcp_m4_runtime *r = cookie;

	return dcp_m4_property_callback(r->properties, method, input, output);
}

static int dcp_m4_runtime_memory(void *cookie, unsigned int method,
				  const void *input, void *output)
{
	struct dcp_m4_runtime *r = cookie;
	struct iomfb_m4 *dcp = r->dcp;

	return dcp_m4_memory_callback(dcp->memory, &dcp_m4_memory_ops, dcp, method, input, output);
}

static int dcp_m4_runtime_event(void *cookie, const char *name, const void *dict, size_t size)
{
	struct dcp_m4_runtime *r = cookie;

	dev_info(r->dcp->dev, "DCP event %.*s: %zu bytes CRC32=%08x\n", 64, name, size,
		 dict ? crc32_le(0, dict, size) : 0);
	return 0;
}

static int dcp_m4_runtime_swap_done(void *cookie, u32 id)
{
	struct dcp_m4_runtime *r = cookie;

	return dcp_m4_swap_record_done(&r->swap, id);
}

static const struct dcp_m4_callback_ops dcp_m4_runtime_ops = {
	.call = dcp_m4_runtime_call,
	.property = dcp_m4_runtime_property,
	.memory = dcp_m4_runtime_memory,
	.event = dcp_m4_runtime_event,
	.swap_done = dcp_m4_runtime_swap_done,
};

static void dcp_m4_runtime_bad_word(struct dcp_m4_runtime *r, u64 word, int error)
{
	u32 offset = (word >> 16) & 0xffff;

	dev_err(r->dcp->dev, "Rejected RPC notification %016llx: %d\n", word, error);
	/* Diagnostic bytes only: unknown control bits never grant packet ownership. */
	if ((word & 3) == 2 && !(word & 0x40) && !(offset % DCP_M4_ALIGNMENT) &&
	    offset <= DCP_M4_STREAM_SIZE - DCP_M4_PACKET_HEADER) {
		dma_rmb();
		dev_err(r->dcp->dev, "Candidate callback headers remote0=%12ph remote1=%12ph\n",
			(u8 *)r->dcp->rpc + 0x60000 + offset,
			(u8 *)r->dcp->rpc + 0x40000 + offset);
	}
}

static int dcp_m4_runtime_callback(struct dcp_m4_runtime *r, u64 word)
{
	struct iomfb_m4 *dcp = r->dcp;
	struct dcp_m4_packet packet;
	u8 *input, *snapshot;
	u64 ack;
	const char *stage = "offset";
	int ret, offset;

	dma_rmb();
	ret = dcp_m4_callback_begin(&dcp->transaction, dcp->rpc, DCP_M4_RPC_SIZE, word, &packet);
	if (ret) {
		dcp_m4_runtime_bad_word(r, word, ret);
		return ret;
	}
	offset = dcp_m4_packet_offset(&packet, true);
	if (offset < 0) {
		ret = offset;
		goto failed;
	}
	input = (u8 *)dcp->rpc + offset + DCP_M4_PACKET_HEADER;
	ret = dcp_m4_callback_shape(packet.tag, packet.input_size, packet.output_size);
	if (ret < 0)
		goto failed;
	/* Each nested callback owns its snapshot; validation and property/string
	 * consumers must not fetch shared firmware bytes a second time. Exact
	 * callback shapes bound this allocation to 4168 bytes.
	 */
	snapshot = kmemdup(input, packet.input_size, GFP_KERNEL);
	if (!snapshot && packet.input_size) {
		ret = -ENOMEM;
		goto failed;
	}
	r->callbacks++;
	dev_dbg(dcp->dev, "DCP callback D%03d %u/%u remote=%u flag9=%u\n",
		 dcp_m4_callback_method(packet.tag), packet.input_size, packet.output_size,
		 packet.remote, packet.callback_flag);
	if (packet.callback_flag && packet.input_size > 80 &&
	    packet.tag == get_unaligned_le32("D006"))
		dev_dbg(dcp->dev, "Flagged D006 header=%12ph null-byte=%02x; ACK route=342\n",
			 (u8 *)dcp->rpc + offset, input[80]);
	stage = "dispatch";
	ret = dcp_m4_callback_dispatch(&dcp_m4_runtime_ops, r, packet.tag, packet.remote,
		snapshot, packet.input_size, input + packet.input_size, packet.output_size);
	kfree(snapshot);
	if (ret)
		goto failed;
	stage = "finish";
	ret = dcp_m4_callback_end(&dcp->transaction, dcp->rpc, DCP_M4_RPC_SIZE, &ack);
	if (ret)
		goto failed;
	stage = "ack";
	ret = dcp_m4_runtime_send(r, ack);
	if (!ret)
		return 0;
failed:
	dev_err(dcp->dev,
		"DCP callback D%03d tag=%08x %u/%u remote=%u flag9=%u %s failed: %d\n",
		dcp_m4_callback_method(packet.tag), packet.tag, packet.input_size,
		packet.output_size, packet.remote, packet.callback_flag, stage, ret);
	return ret;
}

static int dcp_m4_runtime_call_stream(struct dcp_m4_runtime *r, unsigned int method,
				      u8 stream, bool remote, const void *input,
				      u32 input_size, void *output, u32 output_size)
{
	struct iomfb_m4 *dcp = r->dcp;
	unsigned long previous = r->deadline;
	struct dcp_m4_packet message;
	u64 word;
	u32 tag;
	int ret;

	if (method > 999)
		return -EINVAL;
	if (!r->deadline)
		r->deadline = jiffies + msecs_to_jiffies(5000);
	if (time_after_eq(jiffies, r->deadline)) {
		ret = -ETIMEDOUT;
		goto finished;
	}
	tag = 'A' | (('0' + method / 100) << 8) |
		(('0' + method / 10 % 10) << 16) | (('0' + method % 10) << 24);
	ret = dcp_m4_call_begin_stream(&dcp->transaction, dcp->rpc, DCP_M4_RPC_SIZE,
				       tag, stream, remote, input, input_size, output_size,
				       &word);
	if (!ret)
		ret = dcp_m4_runtime_send(r, word);
	while (!ret) {
		ret = dcp_m4_runtime_next(r, &word, true);
		if (ret)
			break;
		dma_rmb();
		ret = dcp_m4_transaction_decode(&dcp->transaction, word, &message);
		if (ret) {
			dcp_m4_runtime_bad_word(r, word, ret);
			break;
		}
		if (message.reply) {
			ret = dcp_m4_call_reply(&dcp->transaction, dcp->rpc, DCP_M4_RPC_SIZE,
					word, output, output_size);
			break;
		}
		ret = dcp_m4_runtime_callback(r, word);
	}
finished:
	r->deadline = previous;
	if (ret) {
		dcp_m4_transaction_fail(&dcp->transaction, ret);
		dcp_m4_failed(dcp, ret);
		dev_err(dcp->dev, "DCP A%03u failed: %d; retained session\n", method, ret);
	}
	return ret;
}

static int dcp_m4_runtime_call(void *cookie, unsigned int method, bool remote,
			       const void *input, u32 input_size, void *output, u32 output_size)
{
	return dcp_m4_runtime_call_stream(cookie, method, 0, remote, input, input_size,
					  output, output_size);
}

/* One bounded drain; a positive return asks the idle worker to reschedule. */
static int dcp_m4_runtime_drain(struct dcp_m4_runtime *r, unsigned int budget)
{
	u64 word;
	int ret;

	r->deadline = jiffies + msecs_to_jiffies(5000);
	for (unsigned int handled = 0; handled < budget; handled++) {
		ret = dcp_m4_runtime_next(r, &word, false);
		if (ret == -EAGAIN) {
			ret = 0;
			goto out;
		}
		if (ret)
			goto out;
		if (time_after_eq(jiffies, r->deadline)) {
			ret = -ETIMEDOUT;
			goto out;
		}
		ret = dcp_m4_runtime_callback(r, word);
		if (ret)
			goto out;
	}
	ret = 1;
out:
	r->deadline = 0;
	return ret;
}

static void dcp_m4_runtime_idle(struct work_struct *work)
{
	struct dcp_m4_runtime *r = container_of(work, struct dcp_m4_runtime, idle);
	struct iomfb_m4 *dcp = r->dcp;
	int ret;

	mutex_lock(&dcp->lock);
	ret = dcp_m4_runtime_drain(r, 32);
	if (ret < 0) {
		dcp_m4_transaction_fail(&dcp->transaction, ret);
		dcp_m4_failed(dcp, ret);
	}
	mutex_unlock(&dcp->lock);
	if (ret > 0)
		queue_work(r->wq, &r->idle);
}

/*
 * Serialized owner only; both the A407 reply and its D590 fence must complete.
 * primary/frame/upper/keep as dcp_m4_swap_encode_full().
 */
static int dcp_m4_runtime_swap_full(struct dcp_m4_runtime *r, bool primary,
		const struct dcp_m4_frame *frame, const struct dcp_m4_upper *upper, u32 keep,
		bool brightness_change, u32 nits)
{
	struct iomfb_m4 *dcp = r->dcp;
	u8 start[16] = {0}, started[8], submitted[12];
	u8 *input;
	u32 id;
	u64 word;
	int ret;

	if (((primary && frame) || (upper && upper->frame)) &&
	    (!r->power_done || !r->background_done || !dcp->scanout))
		return -EINVAL;
	if ((primary && frame && dcp_m4_frame_check(frame)) || (upper && dcp_m4_upper_check(upper)))
		return -EINVAL;
	input = kzalloc(DCP_M4_SWAP_SIZE, GFP_KERNEL);
	if (!input) {
		dcp_m4_swap_fail(&r->swap, -ENOMEM);
		dcp_m4_failed(dcp, -ENOMEM);
		return -ENOMEM;
	}
	r->deadline = jiffies + msecs_to_jiffies(5000);
	ret = dcp_m4_runtime_call(r, 406, false, start, sizeof(start), started, sizeof(started));
	if (!ret)
		ret = dcp_m4_swap_start_reply(started, sizeof(started), &id);
	if (!ret)
		ret = dcp_m4_swap_begin(&r->swap, id);
	if (!ret)
		ret = dcp_m4_swap_encode_full(input, DCP_M4_SWAP_SIZE, id, primary, frame, upper,
					      keep, brightness_change, nits, dcp_m4_frame_check);
	if (!ret)
		ret = dcp_m4_runtime_call(r, 407, false, input, DCP_M4_SWAP_SIZE,
					 submitted, sizeof(submitted));
	if (!ret)
		ret = dcp_m4_swap_record_reply(&r->swap, submitted, sizeof(submitted));
	while (!ret && !r->swap.completed) {
		ret = dcp_m4_runtime_next(r, &word, true);
		if (!ret)
			ret = dcp_m4_runtime_callback(r, word);
	}
	if (!ret)
		ret = dcp_m4_runtime_live(r);
	if (!ret && dcp_m4_swap_take_completion(&r->swap) != 1)
		ret = -EPROTO;
	r->deadline = 0;
	/* Only the copied host input is freed; the firmware-visible RPC stays pinned. */
	kfree(input);
	if (ret) {
		dcp_m4_swap_fail(&r->swap, ret);
		dcp_m4_transaction_fail(&dcp->transaction, ret);
		dcp_m4_failed(dcp, ret);
	} else {
		dev_dbg(dcp->dev, "Display swap %u complete: A407 reply and D590\n", r->swap.id);
	}
	return ret;
}

static int dcp_m4_runtime_swap_brightness(struct dcp_m4_runtime *r,
		const struct dcp_m4_frame *frame, bool brightness_change, u32 nits)
{
	return dcp_m4_runtime_swap_full(r, true, frame, NULL, 0, brightness_change, nits);
}

static int dcp_m4_runtime_swap(struct dcp_m4_runtime *r, const struct dcp_m4_frame *frame)
{
	return dcp_m4_runtime_swap_brightness(r, frame, false, 0);
}

static int dcp_m4_runtime_background(struct dcp_m4_runtime *r)
{
	int ret;

	if (!r->done || r->background_started || READ_ONCE(r->dcp->error))
		return -EBUSY;
	r->background_started = true;
	ret = dcp_m4_runtime_swap(r, NULL);
	if (!ret) {
		r->background_id = r->swap.id;
		smp_store_release(&r->background_done, true);
	}
	return ret;
}

/* D101 must report the inherited framebuffer stride, not an invented one. */
static int dcp_m4_boot_framebuffer_check(void)
{
	struct device_node *np;
	u32 width, height, stride;
	int ret = -ENODEV, found = 0;

	for_each_compatible_node(np, NULL, "simple-framebuffer") {
		if (!of_device_is_available(np))
			continue;
		found++;
		/* The inherited console may exclude the notch (64 lines on J713). */
		if (of_property_read_u32(np, "width", &width) ||
		    of_property_read_u32(np, "height", &height) ||
		    of_property_read_u32(np, "stride", &stride) ||
		    width != dcp_m4_pf->panel_width || stride != dcp_m4_pf->boot_stride ||
		    (height != dcp_m4_pf->panel_height &&
		     height != dcp_m4_pf->panel_height - 64)) {
			of_node_put(np);
			return -EINVAL;
		}
		/* The inherited console can exclude the 64-line notch region. */
		ret = 0;
	}
	return found > 1 ? -EINVAL : ret;
}

static int dcp_m4_startup(struct iomfb_m4 *dcp)
{
	struct device *dev = dcp->dev;
	struct dcp_m4_runtime *r;
	unsigned long flags;
	u8 output[4];
	int ret, slot;

	ret = -EBUSY;
	if (!READ_ONCE(dcp->ready) || READ_ONCE(dcp->error) || dcp->startup_started)
		goto out;
	dcp->startup_started = true;
	ret = dcp_m4_boot_framebuffer_check();
	if (ret)
		goto failed;
	ret = dcp_m4_create_piodma(dcp);
	if (ret)
		goto failed;
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	dcp->memory = kzalloc(sizeof(*dcp->memory), GFP_KERNEL);
	if (!r || !dcp->memory) {
		ret = -ENOMEM;
		goto failed;
	}
	r->properties = kzalloc(sizeof(*r->properties), GFP_KERNEL);
	if (!r->properties) {
		ret = -ENOMEM;
		goto failed;
	}
	r->dcp = dcp;
	init_completion(&r->received);
	INIT_WORK(&r->idle, dcp_m4_runtime_idle);
	r->wq = alloc_ordered_workqueue("iomfb-v26-6", WQ_MEM_RECLAIM);
	if (!r->wq) {
		ret = -ENOMEM;
		goto failed;
	}
	spin_lock_irqsave(&dcp->state_lock, flags);
	smp_store_release(&dcp->runtime, r);
	spin_unlock_irqrestore(&dcp->state_lock, flags);
	/* Fresh session only: A401, then the first client open A457. */
	ret = dcp_m4_runtime_call(r, 401, false, NULL, 0, output, sizeof(output));
	if (!ret && get_unaligned_le32(output) != 1)
		ret = -EPROTO;
	if (!ret)
		ret = dcp_m4_runtime_call(r, dcp_m4_pf->open_method, false, NULL, 0, NULL, 0);
	/* A reply may be followed by property callbacks queued for this owner. */
	if (!ret)
		ret = dcp_m4_runtime_drain(r, 256);
	if (ret > 0)
		ret = -E2BIG;
	if (ret)
		goto failed;
	slot = dcp_m4_raw_slot(r->properties, 0, "PreferredTimingElements", false);
	ret = slot < 0 ? slot : dcp_m4_parse_timing(r->properties->raw[slot].data,
					       r->properties->raw[slot].size, &r->timing);
	if (ret)
		goto failed;
	ret = dcp_m4_runtime_health(r);
	if (ret)
		goto failed;
	smp_store_release(&r->done, true);
	dev_info(dev, "IOMFB startup complete: %u callbacks, %ux%u at %u kHz; no modeset\n",
		 r->callbacks, r->timing.hdisplay, r->timing.vdisplay, r->timing.clock_khz);
	ret = 0;
	goto out;
failed:
	dcp_m4_failed(dcp, ret);
	dev_err(dev, "IOMFB startup failed: %d; retained until guarded reboot\n", ret);
out:
	return ret;
}
static int dcp_m4_prepare_scanout(struct iomfb_m4 *dcp)
{
	struct dcp_m4_runtime *r = dcp->runtime;
	dma_addr_t dva;
	int ret;

	if (r->timing.hdisplay != DCP_M4_PANEL_WIDTH ||
	    r->timing.vdisplay != DCP_M4_PANEL_HEIGHT)
		return -EINVAL;
	ret = dcp_m4_dart_check(dcp->scanout);
	if (ret)
		return ret;
	r->scanout_started = true;
	r->scanout_frame = (struct dcp_m4_frame) {
		.width = DCP_M4_PANEL_WIDTH, .height = DCP_M4_PANEL_HEIGHT,
		.stride = DCP_M4_PANEL_WIDTH * 4,
		.backing_size = (u64)DCP_M4_PANEL_WIDTH * 4 * DCP_M4_PANEL_HEIGHT,
		.format = DCP_M4_XRGB8888,
	};
	r->scanout_cpu = apple_dart_dcp_scanout_alloc(dcp->scanout,
						   r->scanout_frame.backing_size, &dva);
	if (IS_ERR(r->scanout_cpu))
		return PTR_ERR(r->scanout_cpu);
	r->scanout_frame.dva = dva;
	ret = dcp_m4_frame_check(&r->scanout_frame);
	if (ret)
		return ret;
	/* Prepare backing while simpledrm still owns the inherited surface. */
	memset(r->scanout_cpu, 0, r->scanout_frame.backing_size);
	dma_wmb();
	return dcp_m4_runtime_health(r);
}

/* Called with all KMS modeset locks after the DRM device is registered. */
static int dcp_m4_initial_scanout(struct iomfb_m4 *dcp)
{
	struct dcp_m4_runtime *r = dcp->runtime;
	int ret;

	if (!r || !r->scanout_cpu)
		return -ENODEV;
	ret = dcp_m4_runtime_health(r);
	if (!ret)
		ret = dcp_m4_runtime_swap(r, &r->scanout_frame);
	if (ret)
		return ret;
	dcp->active_frame = r->scanout_frame;
	smp_store_release(&r->scanout_done, true);
	return 0;
}

/*
 * The common DMA GEM helpers map this object into the pinned SID0 consumer and
 * require one contiguous DMA extent.  A framebuffer reference keeps that DMA
 * mapping stable.  Full DART/PMP validation belongs to startup and health
 * checks, rather than an expensive page-table sweep on every display flip.
 */
static int iomfb_m4_check_frame(const struct dcp_m4_frame *frame)
{
	int ret;

	ret = dcp_m4_direct_extent(frame->dva, frame->backing_size,
				   (u64)frame->stride * frame->height);
	return ret ?: dcp_m4_frame_check(frame);
}

static void iomfb_m4_backlight_work(struct work_struct *work);

/* Once adopted, even driver unbind must preserve the callback cookie and DMA. */
static void iomfb_m4_release(void *cookie)
{
	struct iomfb_m4 *dcp = cookie;

	if (READ_ONCE(dcp->started) || READ_ONCE(dcp->owner->retain_dma))
		return;
	if (dcp->rtkit)
		apple_rtkit_free(dcp->rtkit);
	if (dcp->coproc)
		iounmap(dcp->coproc);
	if (dcp->coproc_size)
		release_mem_region(dcp->coproc_phys, dcp->coproc_size);
	kfree(dcp);
}

/* A UUID identifies the image; both loader version claims must agree with it. */
static bool iomfb_m4_firmware_property(struct device_node *np, const char *name)
{
	u32 version[3];

	return of_property_count_u32_elems(np, name) == ARRAY_SIZE(version) &&
	       !of_property_read_u32_array(np, name, version, ARRAY_SIZE(version)) &&
	       version[0] == 26 && version[1] == 6 && version[2] == 2;
}

int iomfb_m4_init(struct apple_dcp *owner)
{
	struct platform_device *pdev = to_platform_device(owner->dev);
	struct device *dev = owner->dev;
	struct iomfb_m4 *dcp;
	struct resource *res;
	const char *uuid;
	int ret;

	if (!of_machine_is_compatible("apple,j713") ||
	    !of_device_is_compatible(dev->of_node, "apple,t8132-dcp") ||
	    !iomfb_m4_firmware_property(dev->of_node, "apple,firmware-version") ||
	    !iomfb_m4_firmware_property(dev->of_node, "apple,firmware-compat") ||
	    of_property_read_string(dev->of_node, "apple,firmware-uuid", &uuid) ||
	    strcmp(uuid, "81BAF476-679D-3CC0-8C6A-E6B6FA659D5E"))
		return dev_err_probe(dev, -ENODEV, "unqualified firmware 26.6.2 handoff\n");
	ret = dcp_m4_dart_check(dev);
	if (ret)
		return dev_err_probe(dev, ret, "inherited SID23 domain not qualified\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42));
	if (ret)
		return ret;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "coproc");
	if (!res || res->start != 0x30ae00000ULL || resource_size(res) != SZ_16K)
		return -ENODEV;
	/* Resolve the fallible supplier before creating a mailbox owner. */
	ret = apple_pmp_j713_display_prepare(dev);
	if (ret)
		return dev_err_probe(dev, ret, "PMP supplier not ready\n");
	dcp = kzalloc(sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return -ENOMEM;
	dcp->owner = owner;
	dcp->dev = dev;
	mutex_init(&dcp->lock);
	spin_lock_init(&dcp->state_lock);
	init_completion(&dcp->reply);
	INIT_WORK(&dcp->backlight_work, iomfb_m4_backlight_work);
	if (!request_mem_region(res->start, resource_size(res), dev_name(dev))) {
		ret = -EBUSY;
		goto release;
	}
	dcp->coproc_phys = res->start;
	dcp->coproc_size = resource_size(res);
	dcp->coproc = ioremap(res->start, resource_size(res));
	if (!dcp->coproc) {
		ret = -ENOMEM;
		goto release;
	}
	if (!(readl(dcp->coproc + 0x44) & BIT(4))) {
		ret = -ENODEV;
		goto release;
	}
	dcp->rtkit = apple_rtkit_init(dev, dcp, "mbox", 0, &dcp_m4_rtkit_ops);
	if (IS_ERR(dcp->rtkit)) {
		ret = PTR_ERR(dcp->rtkit);
		dcp->rtkit = NULL;
		goto release;
	}
	/* No messages are sent in probe; a failed local probe has no adopted DMA. */
	ret = devm_add_action_or_reset(dev, iomfb_m4_release, dcp);
	if (ret)
		return ret;
	owner->v26_6 = dcp;
	owner->rtk = dcp->rtkit;
	owner->coproc_reg = dcp->coproc;
	pm_runtime_get_noresume(dev);
	dev_info(dev, "firmware 26.6.2 retained DCP backend prepared\n");
	return 0;
release:
	iomfb_m4_release(dcp);
	return dev_err_probe(dev, ret, "cannot prepare retained firmware 26.6.2 backend\n");
}

int iomfb_m4_start(struct apple_dcp *owner, struct device *scanout)
{
	struct iomfb_m4 *dcp = owner->v26_6;
	struct dcp_m4_runtime *r;
	struct iommu_group *group;
	int ret;

	if (!dcp || !scanout)
		return -ENODEV;
	mutex_lock(&dcp->lock);
	if (READ_ONCE(dcp->error)) {
		ret = READ_ONCE(dcp->error);
		goto out;
	}
	if (dcp->started) {
		ret = dcp->error ?: (dcp->runtime && dcp->runtime->scanout_cpu ? 0 : -EBUSY);
		goto out;
	}
	ret = dcp_m4_dart_check(scanout);
	if (ret)
		goto out;
	/* DMA GEM maps in this exact consumer; firmware outlives master unbind. */
	group = iommu_group_get(scanout);
	if (!group) {
		ret = -ENODEV;
		goto out;
	}
	ret = iommu_device_use_default_domain(scanout);
	if (ret) {
		iommu_group_put(group);
		goto out;
	}
	get_device(scanout);
	dcp->scanout = scanout;
	dcp->scanout_group = group;
	pm_runtime_get_noresume(scanout);
	ret = dcp_m4_adopt(dcp);
	if (ret)
		goto fail;
	ret = dcp_m4_startup(dcp);
	if (ret)
		goto fail;
	r = dcp->runtime;
	ret = dcp_m4_prepare_scanout(dcp);
	if (ret)
		goto fail;
	dev_info(dcp->dev, "firmware 26.6.2 prepared for appledrm: %ux%u; boot scanout preserved\n",
		 r->timing.hdisplay, r->timing.vdisplay);
	goto out;
fail:
	dcp_m4_failed(dcp, ret);
	dev_err(dcp->dev, "firmware 26.6.2 startup failed: %d; session retained until reboot\n",
		ret);
out:
	mutex_unlock(&dcp->lock);
	return ret;
}

int iomfb_m4_activate(struct apple_dcp *owner)
{
	struct iomfb_m4 *dcp = owner->v26_6;
	struct dcp_m4_runtime *r;
	int ret;

	if (!dcp || !dcp->runtime)
		return -ENODEV;
	mutex_lock(&dcp->lock);
	ret = READ_ONCE(dcp->error);
	if (ret || READ_ONCE(dcp->runtime->qualified))
		goto out;
	r = dcp->runtime;
	/* Preserve the firmware-qualified blank -> power vote -> surface
	 * sequence, entirely after KMS owns the aperture and is registered.
	 */
	ret = dcp_m4_runtime_background(r);
	if (ret)
		goto failed;
	r->power_started = true;
	ret = dcp_m4_runtime_health(r);
	if (!ret)
		ret = dcp_m4_pmp_get(dcp->dev);
	if (!ret)
		ret = dcp_m4_pmp_check(dcp->dev);
	if (!ret)
		ret = dcp_m4_runtime_health(r);
	if (ret)
		goto failed;
	smp_store_release(&r->power_done, true);
	ret = dcp_m4_initial_scanout(dcp);
	if (!ret)
		ret = dcp_m4_runtime_health(r);
	if (ret)
		goto failed;
	WRITE_ONCE(r->qualified, true);
	dev_info(dcp->dev, "firmware 26.6.2 KMS scanout activated\n");
	goto out;
failed:
	dcp_m4_failed(dcp, ret);
out:
	mutex_unlock(&dcp->lock);
	return ret;
}

int iomfb_m4_get_mode(struct apple_dcp *owner, struct drm_display_mode *mode)
{
	struct iomfb_m4 *dcp = owner->v26_6;
	struct dcp_m4_timing *t;

	if (!dcp || !dcp->runtime || !smp_load_acquire(&dcp->runtime->done))
		return -ENODEV;
	if (READ_ONCE(dcp->error))
		return READ_ONCE(dcp->error);
	t = &dcp->runtime->timing;
	*mode = (struct drm_display_mode) {
		.clock = t->clock_khz,
		.hdisplay = t->hdisplay, .hsync_start = t->hsync_start,
		.hsync_end = t->hsync_end, .htotal = t->htotal,
		.vdisplay = t->vdisplay, .vsync_start = t->vsync_start,
		.vsync_end = t->vsync_end, .vtotal = t->vtotal,
		.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	};
	drm_mode_set_name(mode);
	return 0;
}

int iomfb_m4_error(struct apple_dcp *owner)
{
	return owner->v26_6 ? READ_ONCE(owner->v26_6->error) : -ENODEV;
}

int iomfb_m4_health(struct apple_dcp *owner)
{
	struct iomfb_m4 *dcp = owner->v26_6;
	int ret;

	if (!dcp || !dcp->runtime)
		return -ENODEV;
	mutex_lock(&dcp->lock);
	ret = dcp_m4_runtime_health(dcp->runtime);
	if (ret)
		dcp_m4_failed(dcp, ret);
	mutex_unlock(&dcp->lock);
	return ret;
}

/* dcp->lock is the single owner of swaps, active descriptors and brightness. */
static int iomfb_m4_swap(struct iomfb_m4 *dcp, const struct dcp_m4_frame *frame)
{
	struct dcp_backlight_present present = {};
	int prepared = -ENODATA, ret;

	lockdep_assert_held(&dcp->lock);
	if (dcp_backlight_active(dcp->owner))
		prepared = dcp_backlight_prepare(dcp->owner, true, &present);
	if (prepared && prepared != -EALREADY && prepared != -ENODATA)
		return prepared;
	ret = dcp_m4_runtime_swap_brightness(dcp->runtime, frame,
					    !prepared, present.nits);
	if (!prepared)
		dcp_backlight_complete(dcp->owner, present.sequence, !ret);
	return ret;
}

int iomfb_m4_present(struct apple_dcp *owner, struct drm_framebuffer *fb,
		     dma_addr_t dva, size_t mapped)
{
	struct iomfb_m4 *dcp = owner->v26_6;
	struct drm_framebuffer *retire = NULL;
	struct dcp_m4_frame frame;
	int ret;

	if (!dcp || !dcp->runtime || !fb || fb->format->num_planes != 1 ||
	    fb->offsets[0] || fb->modifier != DRM_FORMAT_MOD_LINEAR ||
	    (fb->format->format != DRM_FORMAT_XRGB8888 &&
	     fb->format->format != DRM_FORMAT_ARGB8888))
		return -EINVAL;
	ret = dcp_m4_direct_frame(&frame, dva, mapped, fb->width, fb->height,
				  fb->pitches[0], fb->format->format == DRM_FORMAT_ARGB8888);
	if (ret)
		return ret;
	mutex_lock(&dcp->lock);
	ret = dcp_m4_runtime_live(dcp->runtime);
	if (!ret && dcp->powered_off)
		ret = -EAGAIN;
	if (!ret)
		ret = iomfb_m4_check_frame(&frame);
	/* The first frame after a blank powers the display back up. */
	if (!ret && dcp->display_off) {
		ktime_t start = ktime_get();

		ret = dcp_m4_set_power_state(dcp, true);
		if (!ret) {
			dcp->display_off = false;
			dcp->display_on_us = ktime_us_delta(ktime_get(), start);
			dcp->display_on_us_max = max(dcp->display_on_us,
						     dcp->display_on_us_max);
		}
	}
	if (ret)
		goto out;
	/* Keep the source before the first work descriptor can become visible. */
	drm_framebuffer_get(fb);
	ret = iomfb_m4_swap(dcp, &frame);
	if (ret) {
		/* Neither the old surface nor this successor has a release receipt. */
		dcp->uncertain_fb = fb;
		dcp_m4_failed(dcp, ret);
		goto out;
	}
	retire = dcp->active_fb;
	dcp->active_fb = fb;
	dcp->active_frame = frame;
	dcp->flips++;
out:
	mutex_unlock(&dcp->lock);
	if (retire)
		drm_framebuffer_put(retire);
	return ret;
}

int iomfb_m4_blank(struct apple_dcp *owner)
{
	struct iomfb_m4 *dcp = owner->v26_6;
	struct drm_framebuffer *retire = NULL;
	int ret;

	if (!dcp || !dcp->runtime)
		return -ENODEV;
	mutex_lock(&dcp->lock);
	ret = dcp_m4_runtime_live(dcp->runtime);
	if (!ret)
		ret = iomfb_m4_swap(dcp, NULL);
	if (ret) {
		dcp_m4_failed(dcp, ret);
	} else {
		retire = dcp->active_fb;
		dcp->active_fb = NULL;
		memset(&dcp->active_frame, 0, sizeof(dcp->active_frame));
		dcp->blanks++;
		/*
		 * Nothing is scanned out until the next frame: power the display
		 * down as system sleep does. A refusal fails the session, as there.
		 */
		if (!dcp->display_off) {
			ktime_t start = ktime_get();

			if (!dcp_m4_set_power_state(dcp, false)) {
				dcp->display_off = true;
				dcp->display_off_cycles++;
				dcp->display_off_us = ktime_us_delta(ktime_get(), start);
				dcp->display_off_us_max = max(dcp->display_off_us,
							      dcp->display_off_us_max);
			}
		}
	}
	mutex_unlock(&dcp->lock);
	if (retire)
		drm_framebuffer_put(retire);
	return ret;
}

/* setPowerState (A473), on the out-of-band stream on IOMFB 26.x. */
struct dcp_m4_power_state_req {
	__le64 state;
	u8 flags[4];
	u8 result_null;
	u8 padding[3];
} __packed;
static_assert(sizeof(struct dcp_m4_power_state_req) == 16);

struct dcp_m4_power_state_reply {
	__le32 value;
	__le32 status;
} __packed;
static_assert(sizeof(struct dcp_m4_power_state_reply) == 8);

static int dcp_m4_set_power_state(struct iomfb_m4 *dcp, bool on)
{
	struct dcp_m4_power_state_req req = { .state = cpu_to_le64(on) };
	struct dcp_m4_power_state_reply reply = {};
	int ret;

	lockdep_assert_held(&dcp->lock);
	ret = dcp_m4_runtime_live(dcp->runtime);
	if (!ret)
		ret = dcp_m4_runtime_call_stream(dcp->runtime, 473, 1, false, &req,
						 sizeof(req), &reply, sizeof(reply));
	if (!ret && reply.status) {
		dev_err(dcp->dev, "display power %s refused: status %#x\n",
			on ? "on" : "off", le32_to_cpu(reply.status));
		ret = -EIO;
		dcp_m4_failed(dcp, ret);
	}
	return ret;
}

/* apple-drm, a consumer of this device, has blanked the panel before this. */
int iomfb_m4_suspend(struct apple_dcp *owner)
{
	struct iomfb_m4 *dcp = owner->v26_6;
	int ret;

	if (!dcp || !dcp->runtime)
		return -ENODEV;
	mutex_lock(&dcp->lock);
	if (dcp->active_fb) {
		dev_warn(dcp->dev, "panel still scanning out: refusing system sleep\n");
		ret = -EBUSY;
	} else if (dcp->display_off) {
		/* Already powered down by the blank; the next frame powers it up. */
		ret = 0;
	} else {
		ret = dcp_m4_set_power_state(dcp, false);
		if (!ret)
			dcp->powered_off = true;
	}
	mutex_unlock(&dcp->lock);
	return ret;
}

int iomfb_m4_resume(struct apple_dcp *owner)
{
	struct iomfb_m4 *dcp = owner->v26_6;
	int ret;

	if (!dcp || !dcp->runtime)
		return -ENODEV;
	mutex_lock(&dcp->lock);
	if (dcp->powered_off) {
		ret = dcp_m4_set_power_state(dcp, true);
		if (!ret)
			dcp->powered_off = false;
	} else {
		ret = dcp_m4_runtime_health(dcp->runtime);
		if (ret)
			dcp_m4_failed(dcp, ret);
	}
	mutex_unlock(&dcp->lock);
	return ret;
}

static void iomfb_m4_backlight_work(struct work_struct *work)
{
	struct iomfb_m4 *dcp = container_of(work, struct iomfb_m4, backlight_work);
	int ret;

	mutex_lock(&dcp->lock);
	if (READ_ONCE(dcp->owner->quiescing) || !dcp->runtime ||
	    !dcp_backlight_pending(dcp->owner) || !dcp->active_frame.dva)
		goto out;
	ret = dcp_m4_runtime_live(dcp->runtime);
	if (!ret)
		ret = iomfb_m4_swap(dcp, &dcp->active_frame);
	if (ret)
		dcp_m4_failed(dcp, ret);
	else
		dcp->brightness_swaps++;
out:
	mutex_unlock(&dcp->lock);
}

static void iomfb_m4_backlight_kick(struct apple_dcp *owner)
{
	struct iomfb_m4 *dcp = owner->v26_6;

	if (dcp && dcp->runtime && !READ_ONCE(owner->quiescing))
		queue_work(dcp->runtime->wq, &dcp->backlight_work);
}

void iomfb_m4_configure_backlight(struct apple_dcp *owner)
{
	int ret;

	if (dcp_backlight_active(owner))
		return;
	ret = dcp_backlight_configure(owner, 500, false, 0, true, 120,
				      iomfb_m4_backlight_kick);
	if (ret && ret != -EBUSY)
		dev_err(owner->dev, "firmware 26.6.2 backlight unavailable: %d\n", ret);
}

void iomfb_m4_quiesce(struct apple_dcp *owner)
{
	struct iomfb_m4 *dcp = owner->v26_6;

	if (!dcp)
		return;
	/* Wake any RPC waiter before waiting for workers; never recycle its DMA. */
	dcp_m4_failed(dcp, -ENODEV);
	cancel_work_sync(&dcp->backlight_work);
	if (dcp->runtime)
		flush_workqueue(dcp->runtime->wq);
}

static int iomfb_m4_status_show(struct seq_file *s, void *unused)
{
	struct apple_dcp *owner = s->private;
	struct iomfb_m4 *dcp = owner->v26_6;
	struct dcp_m4_runtime *r;
	int health;

	if (!dcp)
		return -ENODEV;
	mutex_lock(&dcp->lock);
	r = dcp->runtime;
	health = r ? dcp_m4_runtime_health(r) : -ENODEV;
	if (health && r)
		dcp_m4_failed(dcp, health);
	seq_printf(s, "firmware=26.6.2 ready=%u qualified=%u error=%d run=%u health=%d\n",
		   dcp->ready, r && r->qualified, dcp->error,
		   !!(readl(dcp->coproc + 0x44) & BIT(4)), health);
	seq_printf(s, "startup=%u background=%u power=%u scanout=%u callbacks=%u\n",
		   r && r->done, r && r->background_done, r && r->power_done,
		   r && r->scanout_done, r ? r->callbacks : 0);
	seq_printf(s, "flips=%llu blanks=%llu backlight_swaps=%llu active=%u uncertain=%u\n",
		   dcp->flips, dcp->blanks, dcp->brightness_swaps,
		   !!dcp->active_fb, !!dcp->uncertain_fb);
	seq_printf(s, "display_off=%u off_cycles=%llu off_us=%lld/%lld on_us=%lld/%lld\n",
		   dcp->display_off, dcp->display_off_cycles,
		   dcp->display_off_us, dcp->display_off_us_max,
		   dcp->display_on_us, dcp->display_on_us_max);
	mutex_unlock(&dcp->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(iomfb_m4_status);

void iomfb_m4_debugfs_init(struct apple_dcp *owner, struct dentry *root)
{
	debugfs_create_file("iomfb_m4", 0400, root, owner, &iomfb_m4_status_fops);
}
