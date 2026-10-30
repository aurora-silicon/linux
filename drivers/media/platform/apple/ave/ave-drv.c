// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE video encoder
 *
 * The AVE block runs an encoder firmware on its own coprocessor, which the
 * bootloader loads but does not start. This driver starts it on first use,
 * talks to it over two shared-memory rings and exposes it as a V4L2
 * stateful encoder.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#include "ave.h"

#define AVE_AUTOSUSPEND_MS	2000

static const char * const ave_pd_names[] = { "dma", "pipe4", "pipe5", "me0", "me1" };

static int ave_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct dev_pm_domain_attach_data pd_data = {
		.pd_names = ave_pd_names,
		.num_pd_names = ARRAY_SIZE(ave_pd_names),
		.pd_flags = PD_FLAG_DEV_LINK_ON,
	};
	struct ave_dev *ave;
	int ret;

	ave = devm_kzalloc(dev, sizeof(*ave), GFP_KERNEL);
	if (!ave)
		return -ENOMEM;
	ave->dev = dev;
	mutex_init(&ave->hw_lock);
	platform_set_drvdata(pdev, ave);

	ave->codec = devm_platform_ioremap_resource_byname(pdev, "codec");
	if (IS_ERR(ave->codec))
		return PTR_ERR(ave->codec);
	ave->mbox = devm_platform_ioremap_resource_byname(pdev, "mailbox");
	if (IS_ERR(ave->mbox))
		return PTR_ERR(ave->mbox);
	ave->coproc = devm_platform_ioremap_resource_byname(pdev, "coproc");
	if (IS_ERR(ave->coproc))
		return PTR_ERR(ave->coproc);

	ave->domain = iommu_get_domain_for_dev(dev);
	if (!ave->domain || !(ave->domain->type & __IOMMU_DOMAIN_PAGING))
		return dev_err_probe(dev, -ENODEV, "needs a translating IOMMU\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42));
	if (ret)
		return ret;

	/* The encoder pipes, the DMA block and the motion engines. */
	ret = devm_pm_domain_attach_list(dev, &pd_data, &ave->pd_list);
	if (ret < 0)
		return dev_err_probe(dev, ret, "cannot attach power domains\n");

	pm_runtime_set_autosuspend_delay(dev, AVE_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = ave_fw_probe(ave);
	if (ret)
		return ret;

	ret = ave_v4l2_register(ave);
	if (ret) {
		ave_fw_remove(ave);
		return ret;
	}
	return 0;
}

static void ave_remove(struct platform_device *pdev)
{
	struct ave_dev *ave = platform_get_drvdata(pdev);

	ave_v4l2_unregister(ave);

	/*
	 * Make sure the coprocessor really loses power before its memory is
	 * freed: drop to runtime suspend synchronously and check.
	 */
	pm_runtime_dont_use_autosuspend(ave->dev);
	if (pm_runtime_suspended(ave->dev) || pm_runtime_status_suspended(ave->dev)) {
		mutex_lock(&ave->hw_lock);
		ave_fw_lost(ave);
		mutex_unlock(&ave->hw_lock);
		ave_fw_remove(ave);
	} else {
		/* The firmware still owns its buffers; leave them allocated. */
		dev_warn(ave->dev, "firmware still running, keeping its memory\n");
	}
}

static int ave_runtime_suspend(struct device *dev)
{
	struct ave_dev *ave = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&ave->hw_lock);
	ret = ave_fw_shutdown(ave);
	mutex_unlock(&ave->hw_lock);
	return ret;
}

static int ave_runtime_resume(struct device *dev)
{
	return 0;
}

static int ave_suspend(struct device *dev)
{
	struct ave_dev *ave = dev_get_drvdata(dev);

	/* Let a running frame finish; the m2m core holds back new jobs. */
	v4l2_m2m_suspend(ave->m2m_dev);

	mutex_lock(&ave->hw_lock);
	if (ave->fw_state != AVE_FW_OFF && ave_fw_shutdown(ave))
		dev_dbg(dev, "firmware will restart after resume\n");
	mutex_unlock(&ave->hw_lock);
	return 0;
}

static int ave_resume(struct device *dev)
{
	struct ave_dev *ave = dev_get_drvdata(dev);

	/*
	 * If the power domains went down with the system, the coprocessor is
	 * back in reset: open sessions fail their next frame and a new session
	 * boots the firmware again from its saved state.
	 */
	mutex_lock(&ave->hw_lock);
	ave_fw_resume(ave);
	mutex_unlock(&ave->hw_lock);

	v4l2_m2m_resume(ave->m2m_dev);
	return 0;
}

static const struct dev_pm_ops ave_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(ave_suspend, ave_resume)
	RUNTIME_PM_OPS(ave_runtime_suspend, ave_runtime_resume, NULL)
};

static const struct of_device_id ave_of_match[] = {
	{ .compatible = "apple,t8140-ave" },
	{ }
};
MODULE_DEVICE_TABLE(of, ave_of_match);

static struct platform_driver ave_driver = {
	.probe = ave_probe,
	.remove = ave_remove,
	.driver = {
		.name = "apple-ave",
		.of_match_table = ave_of_match,
		.pm = pm_ptr(&ave_pm_ops),
		/* The firmware cannot be stopped yet, so neither can the driver. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ave_driver);

MODULE_DESCRIPTION("Apple AVE video encoder");
MODULE_LICENSE("GPL");
