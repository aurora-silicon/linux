// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple silicon scaler and colour-space converter (MSR).
 *
 * Every Apple silicon SoC has one or more MSR engines: memory-to-memory
 * scalers with a programmable colour-space converter, chroma resampling,
 * flip/rotate and alpha fill. Hardware-version 10 (M1) through 22 (A18 Pro)
 * are programmed directly through MMIO and report completion with an
 * interrupt; later generations front the engine with firmware and are not
 * handled here.
 *
 * This version only brings the engine up: it attaches the power domains,
 * checks the IOMMU, identifies the hardware, takes the completion
 * interrupt and keeps the engine idle. Jobs and the V4L2 mem2mem device
 * come in later patches.
 */
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include "apple-scaler-regs.h"

#define ASC_NAME		"apple-scaler"
#define ASC_MIN_REG_SIZE	0x28000

/**
 * struct apple_scaler_variant - per-SoC engine description
 * @hw_version: MSR hardware-version of this SoC (from the hardware description)
 * @num_reg: number of register apertures the binding describes
 */
struct apple_scaler_variant {
	u32 hw_version;
	unsigned int num_reg;
};

struct apple_scaler {
	struct device *dev;
	void __iomem *regs;
	const struct apple_scaler_variant *variant;
	struct dev_pm_domain_list *pd_list;
	struct reset_control *rst;
	int irq;
	u32 id;
};

static u32 asc_read(struct apple_scaler *asc, u32 reg)
{
	return readl_relaxed(asc->regs + reg);
}

static void asc_write(struct apple_scaler *asc, u32 reg, u32 val)
{
	writel_relaxed(val, asc->regs + reg);
}

/*
 * No job is ever started yet, so any interrupt is unexpected: acknowledge
 * it and report it. The handler only touches the engine while it is
 * runtime-active, because a powered-down MSR must not be read.
 */
static irqreturn_t asc_irq(int irq, void *data)
{
	struct apple_scaler *asc = data;
	u32 status;

	if (pm_runtime_get_if_active(asc->dev) <= 0)
		return IRQ_NONE;

	status = asc_read(asc, ASC_IRQ_STATUS);
	if (status) {
		asc_write(asc, ASC_IRQ_STATUS, status);
		dev_warn_ratelimited(asc->dev, "unexpected interrupt, status %#x\n",
				     status);
	}
	pm_runtime_put_autosuspend(asc->dev);
	return status ? IRQ_HANDLED : IRQ_NONE;
}

static int asc_identify(struct apple_scaler *asc)
{
	u32 running, status;
	int ret;

	ret = pm_runtime_resume_and_get(asc->dev);
	if (ret < 0)
		return dev_err_probe(asc->dev, ret, "failed to power up\n");

	asc->id = asc_read(asc, ASC_HW_VERSION);
	running = asc_read(asc, ASC_IS_RUNNING);
	status = asc_read(asc, ASC_IRQ_STATUS);
	/* Nothing may be enabled until a job path owns the engine. */
	asc_write(asc, ASC_IRQ_ENABLE, 0);
	if (status)
		asc_write(asc, ASC_IRQ_STATUS, status);

	pm_runtime_put_sync_suspend(asc->dev);

	if (running)
		dev_warn(asc->dev, "engine busy at probe (%#x)\n", running);
	dev_info(asc->dev, "MSR hardware-version %u, id %#x, stale status %#x\n",
		 asc->variant->hw_version, asc->id, status);
	return 0;
}

static int asc_runtime_suspend(struct device *dev)
{
	return 0;
}

static int asc_runtime_resume(struct device *dev)
{
	/* The engine loses its state when powered down; jobs program it fully. */
	return 0;
}

static const struct dev_pm_ops asc_pm_ops = {
	RUNTIME_PM_OPS(asc_runtime_suspend, asc_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static int asc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_scaler *asc;
	struct resource *res;
	int ret;

	asc = devm_kzalloc(dev, sizeof(*asc), GFP_KERNEL);
	if (!asc)
		return -ENOMEM;
	asc->dev = dev;
	asc->variant = of_device_get_match_data(dev);
	if (!asc->variant)
		return -ENODEV;
	platform_set_drvdata(pdev, asc);

	if (!device_iommu_mapped(dev))
		return dev_err_probe(dev, -ENODEV, "no IOMMU\n");

	/*
	 * MSR-V and the ASE core domain below it; link both so they are on
	 * exactly while the engine is runtime-active.
	 */
	if (of_count_phandle_with_args(dev->of_node, "power-domains",
				       "#power-domain-cells") > 1) {
		static const struct dev_pm_domain_attach_data pd_data = {
			.pd_flags = PD_FLAG_DEV_LINK_ON,
		};

		ret = devm_pm_domain_attach_list(dev, &pd_data, &asc->pd_list);
		if (ret < 0)
			return dev_err_probe(dev, ret, "failed to attach power domains\n");
	} else if (!dev->pm_domain) {
		return dev_err_probe(dev, -ENODEV, "requires a power domain\n");
	}

	if (platform_get_resource(pdev, IORESOURCE_MEM, asc->variant->num_reg - 1) == NULL)
		return dev_err_probe(dev, -EINVAL, "expected %u register ranges\n",
				     asc->variant->num_reg);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (resource_size(res) < ASC_MIN_REG_SIZE)
		return dev_err_probe(dev, -EINVAL, "MSR aperture too small\n");
	asc->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(asc->regs))
		return PTR_ERR(asc->regs);

	asc->rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(asc->rst))
		return dev_err_probe(dev, PTR_ERR(asc->rst), "failed to get reset\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(dev, 100);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = asc_identify(asc);
	if (ret)
		return ret;

	/* Interrupt 0 is the MSR (completion/error); 1 is the PIODMA engine. */
	asc->irq = platform_get_irq(pdev, 0);
	if (asc->irq < 0)
		return asc->irq;
	return devm_request_irq(dev, asc->irq, asc_irq, 0, dev_name(dev), asc);
}

static const struct apple_scaler_variant apple_scaler_t6041 = {
	.hw_version = 21,
	.num_reg = 4,
};

static const struct apple_scaler_variant apple_scaler_t8140 = {
	.hw_version = 22,
	.num_reg = 4,
};

static const struct of_device_id asc_of_match[] = {
	{ .compatible = "apple,t6041-scaler", .data = &apple_scaler_t6041 },
	{ .compatible = "apple,t8140-scaler", .data = &apple_scaler_t8140 },
	{ }
};
MODULE_DEVICE_TABLE(of, asc_of_match);

static struct platform_driver asc_driver = {
	.probe = asc_probe,
	.driver = {
		.name = ASC_NAME,
		.of_match_table = asc_of_match,
		.pm = pm_ptr(&asc_pm_ops),
	},
};
module_platform_driver(asc_driver);

MODULE_DESCRIPTION("Apple silicon scaler and colour-space converter");
MODULE_LICENSE("GPL");
