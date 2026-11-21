// SPDX-License-Identifier: GPL-2.0-only
/* Apple AVE auxiliary DMA fault reporting. */

#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#define AVE_DMA_ERROR_STATUS		0x40
#define AVE_DMA_ERROR_PENDING		BIT(31)
#define AVE_DMA_ERROR_ACK_MASK		0x27c
#define AVE_DMA_ERROR_INFO0		0x44
#define AVE_DMA_ERROR_INFO1		0x48
#define AVE_DMA_ERROR_INFO2		0x4c
#define AVE_DMA_ERROR_ADDRESS		0x50
#define AVE_DMA_ERROR_INFO3		0x58
#define AVE_DMA_ERROR_INFO4		0x60
#define AVE_DMA_ERROR_INFO5		0x64

struct apple_ave_dma {
	struct device *dev;
	void __iomem *base;
};

static irqreturn_t apple_ave_dma_irq(int irq, void *data)
{
	struct apple_ave_dma *dma = data;
	u32 status = readl(dma->base + AVE_DMA_ERROR_STATUS);

	if (!(status & AVE_DMA_ERROR_PENDING))
		return IRQ_NONE;

	dev_err_ratelimited(dma->dev,
			    "DMA fault: status %08x info %08x %08x %08x address %08x info %08x %08x %08x\n",
			    status, readl(dma->base + AVE_DMA_ERROR_INFO0),
			    readl(dma->base + AVE_DMA_ERROR_INFO1),
			    readl(dma->base + AVE_DMA_ERROR_INFO2),
			    readl(dma->base + AVE_DMA_ERROR_ADDRESS),
			    readl(dma->base + AVE_DMA_ERROR_INFO3),
			    readl(dma->base + AVE_DMA_ERROR_INFO4),
			    readl(dma->base + AVE_DMA_ERROR_INFO5));

	/* The implemented error bits are write-one-to-clear. */
	writel(status & AVE_DMA_ERROR_ACK_MASK,
	       dma->base + AVE_DMA_ERROR_STATUS);
	readl(dma->base + AVE_DMA_ERROR_STATUS);
	return IRQ_HANDLED;
}

static int apple_ave_dma_probe(struct platform_device *pdev)
{
	struct apple_ave_dma *dma;
	int irq, ret;

	dma = devm_kzalloc(&pdev->dev, sizeof(*dma), GFP_KERNEL);
	if (!dma)
		return -ENOMEM;

	dma->dev = &pdev->dev;
	dma->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dma->base))
		return PTR_ERR(dma->base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	/*
	 * This reporter sits in the venc_sys power domain, which the encoder
	 * and its DARTs gate whenever the codec is idle. Enable runtime PM and
	 * leave it runtime-suspended so it does not vote the domain on; without
	 * this genpd treats the bound device as active and pins ps_venc_sys
	 * forever, defeating the encoder's idle power saving. A DMA fault can
	 * only be raised - and the interrupt it shares with the codec DARTs
	 * only asserted - while the encoder runs, so the handler always touches
	 * registers in a powered domain. The domain is live at bind (the codec
	 * DARTs are already up), so start active and drop the vote once the
	 * handler is in place.
	 */
	pm_runtime_get_noresume(&pdev->dev);
	pm_runtime_set_active(&pdev->dev);
	ret = devm_pm_runtime_enable(&pdev->dev);
	if (ret) {
		pm_runtime_put_noidle(&pdev->dev);
		return ret;
	}

	ret = devm_request_irq(&pdev->dev, irq, apple_ave_dma_irq,
			       IRQF_SHARED, dev_name(&pdev->dev), dma);
	if (ret) {
		pm_runtime_put_noidle(&pdev->dev);
		return ret;
	}

	pm_runtime_put(&pdev->dev);
	return 0;
}

static const struct of_device_id apple_ave_dma_of_match[] = {
	{ .compatible = "apple,t6000-ave-dma" },
	{}
};
MODULE_DEVICE_TABLE(of, apple_ave_dma_of_match);

static struct platform_driver apple_ave_dma_driver = {
	.probe = apple_ave_dma_probe,
	.driver = {
		.name = "apple-ave-dma",
		.of_match_table = apple_ave_dma_of_match,
	},
};
module_platform_driver(apple_ave_dma_driver);

MODULE_DESCRIPTION("Apple AVE auxiliary DMA fault handler");
MODULE_LICENSE("GPL");
