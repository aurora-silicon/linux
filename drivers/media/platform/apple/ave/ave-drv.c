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
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#include "ave.h"
#include "ave-regs.h"

#define AVE_AUTOSUSPEND_MS	2000

static const struct ave_coproc_regs ave_coproc_v40 = {
	.vcbar = AVE_COPROC_VCBAR,
	.timebase_freq = AVE_COPROC_TIMEBASE_FREQ,
	.timebase = AVE_COPROC_TIMEBASE,
	.ctrl = AVE_COPROC_CTRL,
	.status = AVE_COPROC_STATUS,
	.boot_arg = AVE_COPROC_BOOT_ARG,
	.keep_mappings = AVE_COPROC_KEEP_MAPPINGS,
};

/* Pre-M4 wire layouts are shared; their command constants still need traces. */
static const struct ave_variant ave_t6000 = {
	.regs = &ave_coproc_v40,
	.fw_text_size = 0xd0000,
	.fw_data_size = 0x128000,
	.boot_device_id = 11,
	.device_type = 9,
	.lfs_ref_scale = 1,
};

static const struct ave_variant ave_t8112 = {
	.regs = &ave_coproc_v40,
	.fw_text_size = 0xd0000,
	.fw_data_size = 0x128000,
	.boot_device_id = 15,
	.device_type = 13,
	.lfs_ref_scale = 1,
};

/* CRC, boot-config words and coprocessor start registers remain unqualified. */
static const struct ave_variant ave_t6041 = {
	.fw_text_size = 0x14c000,
	.fw_data_size = 0x12c000,
	.lfs_ref_scale = 2,
};

static const struct ave_variant ave_t8140 = {
	.regs = &ave_coproc_v40,
	.fw_text_size = 0x14c000,
	.fw_data_size = 0x130000,
	.boot_device_id = 29,
	.device_type = 27,
	.lfs_ref_scale = 2,
	.boot_qualified = true,
};

/*
 * debugfs: apple-ave/last_start_{h264,hevc} hold the last Start built and
 * last_process0_{h264,hevc} the first Process of the last session, so the
 * generated commands can be compared with reference captures.
 */
void ave_debugfs_save(struct ave_dev *ave, enum ave_codec codec, enum ave_image kind,
		      const void *cmd, size_t len)
{
	void *dst = ave->dump[codec - 1][kind];

	if (ave->layout && dst && len == ave->layout->bytes[codec - 1][kind])
		memcpy(dst, cmd, len);
}

static void ave_debugfs_init(struct ave_dev *ave)
{
	static const char * const names[2][AVE_IMAGE_COUNT] = {
		{ "last_start_h264", "last_process0_h264" },
		{ "last_start_hevc", "last_process0_hevc" },
	};

	if (!IS_ENABLED(CONFIG_DEBUG_FS) || !ave->layout)
		return;
	ave->debugfs = debugfs_create_dir("apple-ave", NULL);
	for (unsigned int c = 0; c < 2; c++)
		for (unsigned int k = 0; k < AVE_IMAGE_COUNT; k++) {
			struct debugfs_blob_wrapper *blob;
			size_t size = ave->layout->bytes[c][k];

			blob = devm_kzalloc(ave->dev, sizeof(*blob), GFP_KERNEL);
			ave->dump[c][k] = devm_kzalloc(ave->dev, size, GFP_KERNEL);
			if (!blob || !ave->dump[c][k]) {
				ave->dump[c][k] = NULL;
				continue;
			}
			blob->data = ave->dump[c][k];
			blob->size = ave->layout->bytes[c][k];
			debugfs_create_blob(names[c][k], 0400, ave->debugfs, blob);
		}
}

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
	ave->variant = of_device_get_match_data(dev);
	if (!ave->variant)
		return -ENODEV;
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

	ret = ave_fw_probe(ave);
	if (ret)
		return ret;

	/*
	 * Domain attachment took active supplier references, but the driver
	 * starts firmware only on first use and begins suspended. Reconcile that
	 * state before enabling runtime PM, otherwise those references survive
	 * until the first session has resumed and suspended the encoder.
	 */
	ret = pm_runtime_set_suspended(dev);
	if (ret)
		goto err_fw;

	pm_runtime_set_autosuspend_delay(dev, AVE_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		goto err_pm;

	ret = ave_v4l2_register(ave);
	if (ret)
		goto err_pm;
	ave_debugfs_init(ave);
	return 0;

err_pm:
	pm_runtime_dont_use_autosuspend(dev);
err_fw:
	ave_fw_remove(ave);
	return ret;
}

static void ave_remove(struct platform_device *pdev)
{
	struct ave_dev *ave = platform_get_drvdata(pdev);

	debugfs_remove_recursive(ave->debugfs);
	ave_v4l2_unregister(ave);

	/*
	 * The coprocessor memory can only go once the firmware never ran or
	 * was reset by a power cycle. A halted coprocessor may keep power
	 * (its domain is shared with the DARTs), so its memory stays.
	 */
	pm_runtime_dont_use_autosuspend(ave->dev);
	mutex_lock(&ave->hw_lock);
	if (ave->fw_state != AVE_FW_OFF) {
		mutex_unlock(&ave->hw_lock);
		dev_warn(ave->dev, "firmware was started, keeping its memory\n");
		return;
	}
	mutex_unlock(&ave->hw_lock);
	ave_fw_remove(ave);
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
	int ret;

	/* Let a running job finish; the m2m core holds back new ones. */
	v4l2_m2m_suspend(ave->m2m_dev);

	/*
	 * The firmware does not survive the power going down. Drain and close
	 * every open session and halt the firmware, as at the end of a stream;
	 * each session reopens in a cold-started firmware on its next frame.
	 */
	mutex_lock(&ave->hw_lock);
	ave_v4l2_suspend(ave);
	ret = ave_fw_shutdown(ave);
	mutex_unlock(&ave->hw_lock);
	if (ret) {
		/*
		 * genpd may gate the encoder and its DARTs after this callback.
		 * A stuck firmware may still DMA, so abort before that happens.
		 * PM does not resume a device whose suspend callback failed.
		 */
		dev_err(dev, "firmware cannot be stopped for sleep: %d\n", ret);
		v4l2_m2m_resume(ave->m2m_dev);
	}
	return ret;
}

static int ave_resume(struct device *dev)
{
	struct ave_dev *ave = dev_get_drvdata(dev);

	/* The next boot checks the coprocessor state itself. */
	v4l2_m2m_resume(ave->m2m_dev);
	return 0;
}

static const struct dev_pm_ops ave_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(ave_suspend, ave_resume)
	RUNTIME_PM_OPS(ave_runtime_suspend, ave_runtime_resume, NULL)
};

static const struct of_device_id ave_of_match[] = {
	{ .compatible = "apple,t6000-ave", .data = &ave_t6000 },
	{ .compatible = "apple,t6041-ave", .data = &ave_t6041 },
	{ .compatible = "apple,t8112-ave", .data = &ave_t8112 },
	{ .compatible = "apple,t8140-ave", .data = &ave_t8140 },
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
		/* A started coprocessor keeps its memory; do not unbind at will. */
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ave_driver);

MODULE_DESCRIPTION("Apple AVE video encoder");
MODULE_LICENSE("GPL");
