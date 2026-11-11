// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple PCIe PIODMA dmaengine provider
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../pci/controller/pcie-apple-t8140-bootstrap.h"
#include "virt-dma.h"

#define APPLE_PIODMA_ARENA_SIZE		SZ_16K
#define APPLE_PIODMA_COMPLETION_OFFSET	SZ_4K
#define APPLE_PIODMA_IDLE_TIMEOUT_US	50000

struct apple_piodma_hw_slot {
	__le32 packet[4];
	u8 reserved[APPLE_PIODMA_COMPLETION_OFFSET - sizeof(__le32) * 4];
	__le32 completion[4];
};

struct apple_piodma_desc {
	struct virt_dma_desc vd;
	dma_addr_t dst;
	dma_addr_t src;
	size_t len;
	size_t done;
};

struct apple_piodma {
	struct dma_device dma;
	struct virt_dma_chan vc;
	struct device *dev;
	void __iomem *engine;
	void __iomem *fabric;
	struct apple_piodma_hw_slot *slot;
	dma_addr_t slot_dma;
	struct apple_piodma_desc *active;
	u8 tag;
	int irq;
	bool allocated;
	bool stopping;
};

static inline struct apple_piodma *to_apple_piodma(struct dma_chan *chan)
{
	return container_of(chan->device, struct apple_piodma, dma);
}

static inline struct apple_piodma_desc *to_apple_piodma_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct apple_piodma_desc, vd);
}

static u32 apple_piodma_packet0(dma_addr_t src)
{
	return FIELD_PREP(APPLE_PIODMA_PACKET0_FRAME_LO,
			  APPLE_PIODMA_PACKET0_FRAME_LO_VALUE) |
	       FIELD_PREP(APPLE_PIODMA_PACKET0_FRAME_HI,
			  APPLE_PIODMA_PACKET0_FRAME_HI_VALUE) |
	       FIELD_PREP(APPLE_PIODMA_PACKET0_SOURCE_LO, src);
}

static int apple_piodma_wait_idle(struct apple_piodma *piodma)
{
	u32 value;
	int ret;

	ret = readl_poll_timeout(piodma->engine + APPLE_PIODMA_REG_COMMAND,
				 value, !(value & APPLE_PIODMA_COMMAND_RUN), 10,
				 APPLE_PIODMA_IDLE_TIMEOUT_US);
	if (ret)
		return ret;

	return readl_poll_timeout(piodma->engine + APPLE_PIODMA_REG_FIFO_STATUS,
				  value,
				  FIELD_GET(APPLE_PIODMA_FIFO_AVAILABLE, value) ==
					APPLE_PIODMA_FIFO_DEPTH,
				  10, APPLE_PIODMA_IDLE_TIMEOUT_US);
}

static void apple_piodma_ack_irqs(struct apple_piodma *piodma)
{
	u32 value;

	value = readl(piodma->fabric + APPLE_PIODMA_FABRIC_REG_IRQ_STATUS);
	writel(value, piodma->fabric + APPLE_PIODMA_FABRIC_REG_IRQ_STATUS);
	value = readl(piodma->engine + APPLE_PIODMA_REG_IRQ_STATUS);
	writel(value, piodma->engine + APPLE_PIODMA_REG_IRQ_STATUS);
}

static int apple_piodma_hw_init(struct apple_piodma *piodma)
{
	static const struct {
		u32 reg;
		u32 mask;
		u32 class;
	} tunings[] = {
		{ APPLE_PIODMA_REG_TUNE0, APPLE_PIODMA_TUNE_WIDE_MASK,
		  APPLE_PIODMA_TUNE_CLASS_REQUEST },
		{ APPLE_PIODMA_REG_TUNE1, APPLE_PIODMA_TUNE_WIDE_MASK,
		  APPLE_PIODMA_TUNE_CLASS_REQUEST },
		{ APPLE_PIODMA_REG_TUNE2, APPLE_PIODMA_TUNE_WIDE_MASK,
		  APPLE_PIODMA_TUNE_CLASS_COMPLETION },
		{ APPLE_PIODMA_REG_TUNE3, APPLE_PIODMA_TUNE_NARROW_MASK,
		  APPLE_PIODMA_TUNE_CLASS_NONE },
		{ APPLE_PIODMA_REG_TUNE4, APPLE_PIODMA_TUNE_NARROW_MASK,
		  APPLE_PIODMA_TUNE_CLASS_NONE },
	};
	u32 value;
	int i;

	apple_piodma_ack_irqs(piodma);
	writel(APPLE_PIODMA_IRQ_ALL,
	       piodma->engine + APPLE_PIODMA_REG_IRQ_ENABLE);
	writel(0, piodma->fabric + APPLE_PIODMA_FABRIC_REG_IRQ_ENABLE);

	for (i = 0; i < ARRAY_SIZE(tunings); i++) {
		value = readl(piodma->engine + tunings[i].reg);
		value &= ~tunings[i].mask;
		value |= APPLE_PIODMA_TUNE_WINDOWS |
			 FIELD_PREP(APPLE_PIODMA_TUNE_CLASS, tunings[i].class);
		writel(value, piodma->engine + tunings[i].reg);
	}

	value = readl(piodma->engine + APPLE_PIODMA_REG_CONTROL);
	value &= ~APPLE_PIODMA_CONTROL_ENABLE;
	writel(value, piodma->engine + APPLE_PIODMA_REG_CONTROL);

	for (i = 0; i < APPLE_PIODMA_ROUTE_COUNT; i++) {
		writel(0, piodma->engine + APPLE_PIODMA_REG_ROUTE_BASE +
		       i * APPLE_PIODMA_REG_ROUTE_STRIDE);
		writel(i == APPLE_PIODMA_MAP_DESCRIPTOR_INDEX ?
		       APPLE_PIODMA_MAP_DESCRIPTOR : 0,
		       piodma->engine + APPLE_PIODMA_REG_MAP_BASE +
		       i * APPLE_PIODMA_REG_MAP_STRIDE);
	}

	/* Make every route and tuning write visible before the idle readback. */
	wmb();
	return apple_piodma_wait_idle(piodma);
}

static void apple_piodma_start_locked(struct apple_piodma *piodma)
{
	struct apple_piodma_desc *desc = piodma->active;
	dma_addr_t descriptor, completion, src, dst;
	size_t len;
	u32 packet1;

	lockdep_assert_held(&piodma->vc.lock);

	if (!desc || piodma->stopping)
		return;

	src = desc->src + desc->done;
	dst = desc->dst + desc->done;
	len = min_t(size_t, desc->len - desc->done,
		    APPLE_PIODMA_MAX_TRANSFER);

	piodma->slot->packet[0] = cpu_to_le32(apple_piodma_packet0(src));
	packet1 = FIELD_PREP(APPLE_PIODMA_PACKET1_SOURCE_HI, src >> 8) |
		  FIELD_PREP(APPLE_PIODMA_PACKET1_DEST_LO, dst);
	piodma->slot->packet[1] = cpu_to_le32(packet1);
	piodma->slot->packet[2] = cpu_to_le32(dst >> 4);
	piodma->slot->packet[3] = cpu_to_le32(len);
	memset(piodma->slot->completion, 0,
	       sizeof(piodma->slot->completion));
	dma_wmb();

	piodma->tag++;
	if (!piodma->tag)
		piodma->tag++;
	descriptor = piodma->slot_dma + APPLE_PIODMA_POINTER_PREFIX;
	completion = descriptor + APPLE_PIODMA_COMPLETION_OFFSET;

	writel(FIELD_PREP(APPLE_PIODMA_TAG, piodma->tag),
	       piodma->engine + APPLE_PIODMA_REG_TAG);
	writel(FIELD_GET(APPLE_PIODMA_POINTER_HI, descriptor >> 32),
	       piodma->engine + APPLE_PIODMA_REG_DESCRIPTOR_HI);
	writel(lower_32_bits(descriptor),
	       piodma->engine + APPLE_PIODMA_REG_DESCRIPTOR_LO);
	writel(FIELD_GET(APPLE_PIODMA_POINTER_HI, completion >> 32),
	       piodma->engine + APPLE_PIODMA_REG_COMPLETION_HI);
	writel(lower_32_bits(completion),
	       piodma->engine + APPLE_PIODMA_REG_COMPLETION_LO);
	writel(APPLE_PIODMA_COMMAND_DESCRIPTOR | APPLE_PIODMA_COMMAND_IRQ |
	       APPLE_PIODMA_COMMAND_RUN,
	       piodma->engine + APPLE_PIODMA_REG_COMMAND);
}

static void apple_piodma_start_next_locked(struct apple_piodma *piodma)
{
	struct virt_dma_desc *vd;

	lockdep_assert_held(&piodma->vc.lock);

	if (piodma->active || piodma->stopping)
		return;
	vd = vchan_next_desc(&piodma->vc);
	if (!vd)
		return;
	piodma->active = to_apple_piodma_desc(vd);
	apple_piodma_start_locked(piodma);
}

static irqreturn_t apple_piodma_irq(int irq, void *data)
{
	struct apple_piodma *piodma = data;
	struct apple_piodma_desc *desc;
	unsigned long flags;
	u32 fabric, status, tag;
	bool error;
	int ret;

	fabric = readl(piodma->fabric + APPLE_PIODMA_FABRIC_REG_IRQ_STATUS);
	writel(fabric, piodma->fabric + APPLE_PIODMA_FABRIC_REG_IRQ_STATUS);
	status = readl(piodma->engine + APPLE_PIODMA_REG_IRQ_STATUS);
	writel(status, piodma->engine + APPLE_PIODMA_REG_IRQ_STATUS);
	if (!fabric && !status)
		return IRQ_NONE;

	tag = FIELD_GET(APPLE_PIODMA_TAG,
			readl(piodma->engine + APPLE_PIODMA_REG_COMPLETED_TAG));
	error = status & ~APPLE_PIODMA_IRQ_COMPLETION;
	error |= !(status & APPLE_PIODMA_IRQ_COMPLETION);
	error |= tag != piodma->tag;
	if (error)
		writel(0, piodma->engine + APPLE_PIODMA_REG_COMMAND);
	ret = apple_piodma_wait_idle(piodma);
	if (error)
		ret = -EIO;

	spin_lock_irqsave(&piodma->vc.lock, flags);
	desc = piodma->active;
	if (!desc || piodma->stopping) {
		spin_unlock_irqrestore(&piodma->vc.lock, flags);
		return IRQ_HANDLED;
	}

	if (ret || !(status & APPLE_PIODMA_IRQ_COMPLETION)) {
		desc->vd.tx_result.result = DMA_TRANS_ABORTED;
		desc->vd.tx_result.residue = desc->len - desc->done;
	} else {
		desc->done += min_t(size_t, desc->len - desc->done,
				   APPLE_PIODMA_MAX_TRANSFER);
		if (desc->done != desc->len) {
			apple_piodma_start_locked(piodma);
			spin_unlock_irqrestore(&piodma->vc.lock, flags);
			return IRQ_HANDLED;
		}
	}

	list_del(&desc->vd.node);
	piodma->active = NULL;
	vchan_cookie_complete(&desc->vd);
	apple_piodma_start_next_locked(piodma);
	spin_unlock_irqrestore(&piodma->vc.lock, flags);

	return IRQ_HANDLED;
}

static struct dma_async_tx_descriptor *
apple_piodma_prep_memcpy(struct dma_chan *chan, dma_addr_t dst,
			 dma_addr_t src, size_t len, unsigned long flags)
{
	struct apple_piodma *piodma = to_apple_piodma(chan);
	struct apple_piodma_desc *desc;
	dma_addr_t last;

	if (!len || !IS_ALIGNED(src | dst | len, APPLE_PIODMA_COPY_ALIGN) ||
	    check_add_overflow(src, len - 1, &last) ||
	    last > APPLE_PIODMA_PACKET_MASK ||
	    check_add_overflow(dst, len - 1, &last) ||
	    last > APPLE_PIODMA_PACKET_MASK)
		return NULL;

	desc = kzalloc_obj(*desc, GFP_NOWAIT);
	if (!desc)
		return NULL;
	desc->src = src;
	desc->dst = dst;
	desc->len = len;

	return vchan_tx_prep(&piodma->vc, &desc->vd, flags);
}

static void apple_piodma_issue_pending(struct dma_chan *chan)
{
	struct apple_piodma *piodma = to_apple_piodma(chan);
	unsigned long flags;

	spin_lock_irqsave(&piodma->vc.lock, flags);
	if (vchan_issue_pending(&piodma->vc))
		apple_piodma_start_next_locked(piodma);
	spin_unlock_irqrestore(&piodma->vc.lock, flags);
}

static int apple_piodma_terminate_all(struct dma_chan *chan)
{
	struct apple_piodma *piodma = to_apple_piodma(chan);
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irqsave(&piodma->vc.lock, flags);
	piodma->stopping = true;
	writel(0, piodma->engine + APPLE_PIODMA_REG_COMMAND);
	piodma->active = NULL;
	vchan_get_all_descriptors(&piodma->vc, &head);
	list_splice_tail_init(&head, &piodma->vc.desc_terminated);
	spin_unlock_irqrestore(&piodma->vc.lock, flags);

	synchronize_irq(piodma->irq);
	if (apple_piodma_wait_idle(piodma))
		dev_warn(piodma->dev, "request engine did not stop cleanly\n");
	apple_piodma_ack_irqs(piodma);

	spin_lock_irqsave(&piodma->vc.lock, flags);
	piodma->stopping = false;
	spin_unlock_irqrestore(&piodma->vc.lock, flags);

	return 0;
}

static void apple_piodma_synchronize(struct dma_chan *chan)
{
	struct apple_piodma *piodma = to_apple_piodma(chan);

	synchronize_irq(piodma->irq);
	vchan_synchronize(&piodma->vc);
}

static int apple_piodma_alloc_chan_resources(struct dma_chan *chan)
{
	struct apple_piodma *piodma = to_apple_piodma(chan);
	int ret;

	if (piodma->allocated)
		return 0;

	ret = pm_runtime_resume_and_get(piodma->dev);
	if (ret < 0)
		return ret;
	ret = apple_piodma_hw_init(piodma);
	if (ret) {
		pm_runtime_put_sync(piodma->dev);
		return ret;
	}
	piodma->allocated = true;
	return 0;
}

static void apple_piodma_free_chan_resources(struct dma_chan *chan)
{
	struct apple_piodma *piodma = to_apple_piodma(chan);

	if (!piodma->allocated) {
		vchan_free_chan_resources(&piodma->vc);
		return;
	}

	apple_piodma_terminate_all(chan);
	apple_piodma_synchronize(chan);
	vchan_free_chan_resources(&piodma->vc);
	if (piodma->allocated) {
		piodma->allocated = false;
		pm_runtime_mark_last_busy(piodma->dev);
		pm_runtime_put_autosuspend(piodma->dev);
	}
}

static void apple_piodma_desc_free(struct virt_dma_desc *vd)
{
	kfree(to_apple_piodma_desc(vd));
}

static int apple_piodma_runtime_resume(struct device *dev)
{
	struct apple_piodma *piodma = dev_get_drvdata(dev);

	return apple_piodma_hw_init(piodma);
}

static int apple_piodma_runtime_suspend(struct device *dev)
{
	struct apple_piodma *piodma = dev_get_drvdata(dev);

	writel(0, piodma->engine + APPLE_PIODMA_REG_IRQ_ENABLE);
	apple_piodma_ack_irqs(piodma);
	return 0;
}

static int apple_piodma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_piodma *piodma;
	u64 pointer_last;
	int ret;

	piodma = devm_kzalloc(dev, sizeof(*piodma), GFP_KERNEL);
	if (!piodma)
		return -ENOMEM;
	piodma->dev = dev;
	piodma->engine = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(piodma->engine))
		return PTR_ERR(piodma->engine);
	piodma->fabric = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(piodma->fabric))
		return PTR_ERR(piodma->fabric);
	piodma->irq = platform_get_irq(pdev, 0);
	if (piodma->irq < 0)
		return piodma->irq;

	ret = dma_set_mask_and_coherent(dev, APPLE_PIODMA_PACKET_MASK);
	if (ret)
		return ret;
	piodma->slot = dmam_alloc_coherent(dev, APPLE_PIODMA_ARENA_SIZE,
					   &piodma->slot_dma, GFP_KERNEL);
	if (!piodma->slot)
		return -ENOMEM;
	if (!IS_ALIGNED(piodma->slot_dma, APPLE_PIODMA_ARENA_SIZE) ||
	    check_add_overflow((u64)piodma->slot_dma,
			       APPLE_PIODMA_ARENA_SIZE - 1, &pointer_last) ||
	    pointer_last > APPLE_PIODMA_PACKET_MASK ||
	    check_add_overflow(pointer_last, APPLE_PIODMA_POINTER_PREFIX,
			       &pointer_last) ||
	    pointer_last > APPLE_PIODMA_POINTER_MASK)
		return -ERANGE;

	ret = devm_request_threaded_irq(dev, piodma->irq, NULL,
					apple_piodma_irq, IRQF_ONESHOT,
					dev_name(dev), piodma);
	if (ret)
		return ret;

	piodma->vc.desc_free = apple_piodma_desc_free;
	piodma->dma.dev = dev;
	INIT_LIST_HEAD(&piodma->dma.channels);
	dma_cap_set(DMA_MEMCPY, piodma->dma.cap_mask);
	piodma->dma.device_alloc_chan_resources =
		apple_piodma_alloc_chan_resources;
	piodma->dma.device_free_chan_resources =
		apple_piodma_free_chan_resources;
	piodma->dma.device_prep_dma_memcpy = apple_piodma_prep_memcpy;
	piodma->dma.device_issue_pending = apple_piodma_issue_pending;
	piodma->dma.device_tx_status = dma_cookie_status;
	piodma->dma.device_terminate_all = apple_piodma_terminate_all;
	piodma->dma.device_synchronize = apple_piodma_synchronize;
	piodma->dma.directions = BIT(DMA_MEM_TO_MEM);
	piodma->dma.copy_align = DMAENGINE_ALIGN_4_BYTES;
	piodma->dma.residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;
	vchan_init(&piodma->vc, &piodma->dma);

	platform_set_drvdata(pdev, piodma);
	pm_runtime_set_autosuspend_delay(dev, 100);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);

	ret = dma_async_device_register(&piodma->dma);
	if (ret) {
		pm_runtime_disable(dev);
		tasklet_kill(&piodma->vc.task);
		return ret;
	}
	pm_runtime_idle(dev);

	dev_info(dev, "registered memory-copy DMA channel\n");
	return 0;
}

static void apple_piodma_remove(struct platform_device *pdev)
{
	struct apple_piodma *piodma = platform_get_drvdata(pdev);
	int ret;

	ret = pm_runtime_resume_and_get(&pdev->dev);
	dma_async_device_unregister(&piodma->dma);
	apple_piodma_free_chan_resources(&piodma->vc.chan);
	if (ret >= 0) {
		writel(0, piodma->engine + APPLE_PIODMA_REG_IRQ_ENABLE);
		apple_piodma_ack_irqs(piodma);
		pm_runtime_put_noidle(&pdev->dev);
	}
	pm_runtime_barrier(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	tasklet_kill(&piodma->vc.task);
}

static const struct of_device_id apple_piodma_of_match[] = {
	{ .compatible = "apple,pcie-piodma" },
	{ .compatible = "apple,pciec-piodma" },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_piodma_of_match);

static const struct dev_pm_ops apple_piodma_pm_ops = {
	RUNTIME_PM_OPS(apple_piodma_runtime_suspend,
		       apple_piodma_runtime_resume, NULL)
};

static struct platform_driver apple_piodma_driver = {
	.probe = apple_piodma_probe,
	.remove = apple_piodma_remove,
	.driver = {
		.name = "apple-piodma",
		.of_match_table = apple_piodma_of_match,
		.pm = pm_ptr(&apple_piodma_pm_ops),
	},
};
module_platform_driver(apple_piodma_driver);

MODULE_DESCRIPTION("Apple PCIe PIODMA engine driver");
MODULE_LICENSE("GPL");
