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
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/vmalloc.h>

#include "ave.h"
#include "ave-regs.h"
#include "ave-mctf.h"

#define AVE_AUTOSUSPEND_MS 2000

static const struct ave_coproc_regs ave_coproc_v40 = {
	.vcbar = AVE_COPROC_VCBAR,
	.timebase_freq = AVE_COPROC_TIMEBASE_FREQ,
	.timebase = AVE_COPROC_TIMEBASE,
	.ctrl = AVE_COPROC_CTRL,
	.status = AVE_COPROC_STATUS,
	.boot_arg = AVE_COPROC_BOOT_ARG,
	.keep_mappings = AVE_COPROC_KEEP_MAPPINGS,
};

/* Panda/Castor register family used by 6070.11.1. */
static const struct ave_coproc_regs ave_coproc_6070 = {
	.vcbar = 0x50000,
	.timebase_freq = 0x160020,
	.timebase = 0x178000,
	.ctrl = 0x400044,
	.status = 0x400048,
	.boot_arg = 0x400400,
	.keep_mappings = 0x400808,
};

const struct ave_variant ave_t6000 = {
	.regs = &ave_coproc_6070,
	.fw_text_size = 0xd0000,
	.fw_data_size = 0x128000,
	.boot_device_id = 11,
	.device_type = 9,
	.lfs_ref_scale = 1,
	.text_crc = 0x460c7a7c,
	.boot_arg_value = 0x400,
	.timebase_multiplier = 1000000,
	.vcbar_mask = 0x0000000ffffff800ULL,
	.vcbar_flags = 0x0102000000000000ULL,

	.extra0_size = 0x24,
	.boot_qualified = true,
};

const struct ave_variant ave_t8112 = {
	.regs = &ave_coproc_6070,
	.fw_text_size = 0xd0000,
	.fw_data_size = 0x128000,
	.boot_device_id = 15,
	.device_type = 13,
	.lfs_ref_scale = 1,
	.text_crc = 0x25bfd039,
	.boot_arg_value = 0x10000,
	.timebase_multiplier = 1,
	.vcbar_mask = 0x000003fffffff800ULL,
	.vcbar_flags = 0x0102000000000000ULL,

	.extra0_size = 0x4000,
	.boot_qualified = true,
};

/*
 * The M4 Pro (T6040 in the ADT, apple,t6041 in the DT) starts its encoder
 * like the A18 Pro: same coprocessor registers and boot sequence, its own
 * device ID and type.
 */
static const struct ave_variant ave_t6041 = {
	.regs = &ave_coproc_v40,
	.fw_text_size = 0x14c000,
	.fw_data_size = 0x12c000,
	.boot_device_id = 27,
	.boot_arg_value = AVE_COPROC_BOOT_ARG_VALUE,
	.timebase_multiplier = 1,
	.vcbar_mask = U64_MAX,
	.vcbar_flags = AVE_COPROC_VCBAR_VALID,
	.device_type = 25,
	.lfs_ref_scale = 2,
	.boot_qualified = true,
};

static const struct ave_variant ave_t8140 = {
	.regs = &ave_coproc_v40,
	.fw_text_size = 0x14c000,
	.fw_data_size = 0x130000,
	.boot_device_id = 29,
	.boot_arg_value = AVE_COPROC_BOOT_ARG_VALUE,
	.timebase_multiplier = 1,
	.vcbar_mask = U64_MAX,
	.vcbar_flags = AVE_COPROC_VCBAR_VALID,
	.device_type = 27,
	.lfs_ref_scale = 2,
	.boot_qualified = true,
};

/*
 * debugfs: apple-ave/last_start_{h264,hevc} hold the last Start built and
 * last_process0_{h264,hevc} the first Process of the last session, so the
 * generated commands can be compared with reference captures.
 */
void ave_debugfs_save(struct ave_dev *ave, enum ave_codec codec,
		      enum ave_image kind, const void *cmd, size_t len)
{
	void *dst = ave->dump[codec - 1][kind];

	if (ave->layout && dst && len == ave->layout->bytes[codec - 1][kind])
		memcpy(dst, cmd, len);
}

/* The last MCTF Start and the first Process of the last MCTF session */
void ave_debugfs_save_mctf(struct ave_dev *ave, enum ave_image kind,
			   const void *cmd, size_t len)
{
	const struct ave_mctf_layout *l = ave_mctf_layout_for_crc(ave->text_crc);
	void *dst = ave->mctf_dump[kind];

	if (l && dst &&
	    len == (kind == AVE_IMAGE_START ? l->start_bytes : l->process_bytes))
		memcpy(dst, cmd, len);
}

static void ave_vfree(void *p)
{
	vfree(p);
}

/*
 * last_mctf_in and last_mctf_out: with mctf_capture set, the raw compressed
 * input surface (as the driver wrote it) and output surface (as the firmware
 * wrote it) of the last delivered frame, a known-plaintext pair.
 */
static void ave_debugfs_mctf_surfaces(struct ave_dev *ave,
				      const struct ave_mctf_layout *l)
{
	static const char *const names[2] = { "last_mctf_in", "last_mctf_out" };

	for (unsigned int k = 0; k < 2; k++) {
		struct debugfs_blob_wrapper *blob;
		void *data = vzalloc(l->form.bytes);

		if (!data || devm_add_action_or_reset(ave->dev, ave_vfree, data))
			continue;
		blob = devm_kzalloc(ave->dev, sizeof(*blob), GFP_KERNEL);
		if (!blob)
			continue;
		ave->mctf_surf_dump[k] = data;
		blob->data = data;
		blob->size = l->form.bytes;
		debugfs_create_blob(names[k], 0400, ave->debugfs, blob);
	}
	debugfs_create_bool("mctf_capture", 0600, ave->debugfs, &ave->mctf_capture);
}

static void ave_debugfs_mctf(struct ave_dev *ave)
{
	static const char *const names[AVE_IMAGE_COUNT] = {
		"last_start_mctf", "last_process0_mctf",
	};
	const struct ave_mctf_layout *l = ave_mctf_qualified(ave->text_crc);

	if (!l)
		return;
	for (unsigned int k = 0; k < AVE_IMAGE_COUNT; k++) {
		struct debugfs_blob_wrapper *blob;
		size_t size = k == AVE_IMAGE_START ? l->start_bytes :
						     l->process_bytes;

		blob = devm_kzalloc(ave->dev, sizeof(*blob), GFP_KERNEL);
		ave->mctf_dump[k] = devm_kzalloc(ave->dev, size, GFP_KERNEL);
		if (!blob || !ave->mctf_dump[k]) {
			ave->mctf_dump[k] = NULL;
			continue;
		}
		blob->data = ave->mctf_dump[k];
		blob->size = size;
		debugfs_create_blob(names[k], 0400, ave->debugfs, blob);
	}
	ave_debugfs_mctf_surfaces(ave, l);
}

static ssize_t ave_lifecycle_write(struct file *file, const char __user *user,
				   size_t count, loff_t *ppos)
{
	struct ave_dev *ave = file->private_data;
	unsigned int codec;
	int ret = kstrtouint_from_user(user, count, 0, &codec);

	if (ret)
		return ret;
	if (codec != AVE_CODEC_H264 && codec != AVE_CODEC_HEVC)
		return -EINVAL;
	ret = ave_fw_lifecycle_test(ave, codec);
	return ret ?: count;
}

static const struct file_operations ave_lifecycle_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = ave_lifecycle_write,
};

/* Root-only software timeout injection; does not send malformed firmware commands. */
static int ave_inject_wedge(void *data, u64 value)
{
	struct ave_dev *ave = data;
	int ret = 0;

	if (value != 1)
		return -EINVAL;
	mutex_lock(&ave->hw_lock);
	if (ave->fw_state != AVE_FW_RUNNING) {
		ret = -EBUSY;
	} else {
		ave->inject_wedge = true;
		ave_fw_mark_broken(ave, -ETIMEDOUT, "debugfs injection");
	}
	mutex_unlock(&ave->hw_lock);
	return ret;
}
DEFINE_DEBUGFS_ATTRIBUTE(ave_inject_wedge_fops, NULL, ave_inject_wedge, "%llu\n");

static void ave_debugfs_init(struct ave_dev *ave)
{
	static const char *const names[2][AVE_IMAGE_COUNT] = {
		{ "last_start_h264", "last_process0_h264" },
		{ "last_start_hevc", "last_process0_hevc" },
	};

	if (!IS_ENABLED(CONFIG_DEBUG_FS) || !ave->layout)
		return;
	ave->debugfs = debugfs_create_dir("apple-ave", NULL);
	if (ave->layout->abi == &ave_abi_6070) {
		debugfs_create_file("lifecycle_test", 0200, ave->debugfs, ave,
				    &ave_lifecycle_fops);
		return;
	}
	debugfs_create_file("inject_wedge", 0200, ave->debugfs, ave,
			    &ave_inject_wedge_fops);
	for (unsigned int c = 0; c < 2; c++)
		for (unsigned int k = 0; k < AVE_IMAGE_COUNT; k++) {
			struct debugfs_blob_wrapper *blob;
			size_t size = ave->layout->bytes[c][k];

			blob = devm_kzalloc(ave->dev, sizeof(*blob),
					    GFP_KERNEL);
			ave->dump[c][k] =
				devm_kzalloc(ave->dev, size, GFP_KERNEL);
			if (!blob || !ave->dump[c][k]) {
				ave->dump[c][k] = NULL;
				continue;
			}
			blob->data = ave->dump[c][k];
			blob->size = ave->layout->bytes[c][k];
			debugfs_create_blob(names[c][k], 0400, ave->debugfs,
					    blob);
		}
	ave_debugfs_mctf(ave);
}

/*
 * The encoder pipes, the DMA block and the motion engines, then optionally
 * "perf": a performance floor the encoder needs to run at full speed, such
 * as the SoC rail floor on the M4 Pro. It is held while the encoder is
 * powered, like the other domains, so it ends with the last session.
 */
static const char *const ave_pd_names[] = { "dma", "pipe4", "pipe5", "me0",
					    "me1", "perf" };

static int ave_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	bool perf = of_property_match_string(dev->of_node, "power-domain-names",
					     "perf") >= 0;
	const struct dev_pm_domain_attach_data pd_data = {
		.pd_names = ave_pd_names,
		.num_pd_names = ARRAY_SIZE(ave_pd_names) - !perf,
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
		dev_warn(ave->dev,
			 "firmware was started, keeping its memory\n");
		return;
	}
	/* OFF also follows a proven power loss; free the pool only if it never ran. */
	if (!ave->fw_ever_ran)
		ave_mctf_pool_free(ave);
	mutex_unlock(&ave->hw_lock);
	ave_fw_remove(ave);
}

static int ave_runtime_suspend(struct device *dev)
{
	struct ave_dev *ave = dev_get_drvdata(dev);
	int ret;

	/*
	 * The IPC, heap and log buffers stay mapped: runtime suspend gives no
	 * reset proof, and the next boot reuses them at the same addresses.
	 */
	mutex_lock(&ave->hw_lock);
	ret = ave_fw_runtime_suspend(ave);
	mutex_unlock(&ave->hw_lock);
	return ret;
}

static int ave_runtime_resume(struct device *dev)
{
	struct ave_dev *ave = dev_get_drvdata(dev);

	mutex_lock(&ave->hw_lock);
	if (ave->layout && ave->layout->abi->transport->scratch_halt &&
	    ave->fw_state == AVE_FW_HALTED)
		ave_fw_lost(ave);
	mutex_unlock(&ave->hw_lock);
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
	if (!ret && ave->layout && ave->layout->abi->transport->scratch_halt)
		ave_fw_release_boot(ave);
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
	ave_runtime_resume(dev);
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
	{}
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
MODULE_IMPORT_NS("DMA_BUF");
