// SPDX-License-Identifier: GPL-2.0-only
/*
 * Probe-time diagnostics for the Apple S8000-family SIO AES engine.
 * Only the readiness word and key-disable state are read. In particular,
 * do not initialise the engine, acknowledge interrupts or touch the FIFO.
 */

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#define APPLE_SIO_AES_READINESS	0x0c

struct apple_sio_aes_data {
	u32 revision;
	/* Revision 3 has a shared word; revision 5 has per-slot words. */
	bool shared_disable;
	u32 uid_offset;
	u32 gid0_offset;
	u32 gid1_offset;
};

struct apple_sio_aes {
	const struct apple_sio_aes_data *data;
	u32 readiness;
	u32 disable;
	u32 uid_disable;
	u32 gid0_disable;
	u32 gid1_disable;
};

static int apple_sio_aes_state_show(struct seq_file *s, void *unused)
{
	const struct apple_sio_aes *aes = s->private;

	seq_puts(s, "snapshot: probe\n");
	seq_printf(s, "revision_from_compatible: %u\n", aes->data->revision);
	seq_printf(s, "readiness_raw: %#010x\n", aes->readiness);
	seq_printf(s, "disable_raw: %#010x\n", aes->disable);
	if (aes->data->shared_disable) {
		seq_puts(s, "uid_gid_disable: shared word, slot bits undecoded\n");
	} else {
		seq_printf(s, "uid_disable_raw: %#010x\n", aes->uid_disable);
		seq_printf(s, "gid0_disable_raw: %#010x\n", aes->gid0_disable);
		seq_printf(s, "gid1_disable_raw: %#010x\n", aes->gid1_disable);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_sio_aes_state);

static void apple_sio_aes_debugfs_remove(void *dir)
{
	debugfs_remove_recursive(dir);
}

static void __iomem *apple_sio_aes_map(struct platform_device *pdev,
				       unsigned int index, resource_size_t min_size)
{
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, index);
	if (!res || resource_size(res) < min_size)
		return IOMEM_ERR_PTR(-EINVAL);

	return devm_ioremap_resource(&pdev->dev, res);
}

static int apple_sio_aes_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct apple_sio_aes_data *data;
	void __iomem *engine, *disable;
	struct apple_sio_aes *aes;
	struct dentry *dir;
	char *name;
	int ret;

	data = of_device_get_match_data(dev);
	if (!data)
		return -ENODEV;

	/* The platform bus attaches the single power domain and SIO DART. */
	if (!dev->pm_domain || !of_property_present(dev->of_node, "power-domains"))
		return dev_err_probe(dev, -EINVAL, "Missing AES power domain\n");
	if (!of_property_present(dev->of_node, "iommus") ||
	    !iommu_get_domain_for_dev(dev))
		return dev_err_probe(dev, -ENODEV, "Missing SIO DART domain\n");

	aes = devm_kzalloc(dev, sizeof(*aes), GFP_KERNEL);
	if (!aes)
		return -ENOMEM;
	aes->data = data;

	/* apple,aes orders the banks as engine, key-disable, then optional DKEY. */
	engine = apple_sio_aes_map(pdev, 0, APPLE_SIO_AES_READINESS + 4);
	if (IS_ERR(engine))
		return dev_err_probe(dev, PTR_ERR(engine), "Invalid engine aperture\n");
	disable = apple_sio_aes_map(pdev, 1, data->gid1_offset + 4);
	if (IS_ERR(disable))
		return dev_err_probe(dev, PTR_ERR(disable), "Invalid key-disable aperture\n");

	/* The DKEY aperture and IRQ are described by DT but never accessed. */
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to power AES for snapshot\n");

	/* No polling or decoding of unverified readiness/disable bitfields. */
	aes->readiness = readl(engine + APPLE_SIO_AES_READINESS);
	aes->disable = readl(disable);
	if (!data->shared_disable) {
		aes->uid_disable = readl(disable + data->uid_offset);
		aes->gid0_disable = readl(disable + data->gid0_offset);
		aes->gid1_disable = readl(disable + data->gid1_offset);
	}

	ret = pm_runtime_put_sync_suspend(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to release AES power\n");

	if (data->shared_disable)
		dev_info(dev,
			 "probe snapshot: revision %u (compatible), readiness %#010x, shared UID/GID disable %#010x (undecoded)\n",
			 data->revision, aes->readiness, aes->disable);
	else
		dev_info(dev,
			 "probe snapshot: revision %u (compatible), readiness %#010x, disable %#010x, UID %#010x, GID0 %#010x, GID1 %#010x (raw)\n",
			 data->revision, aes->readiness, aes->disable,
			 aes->uid_disable, aes->gid0_disable, aes->gid1_disable);

	name = devm_kasprintf(dev, GFP_KERNEL, "apple-sio-aes-%s", dev_name(dev));
	if (!name)
		return -ENOMEM;
	dir = debugfs_create_dir(name, NULL);
	if (IS_ERR(dir))
		return 0;
	ret = devm_add_action_or_reset(dev, apple_sio_aes_debugfs_remove, dir);
	if (ret)
		return ret;
	debugfs_create_file("state", 0400, dir, aes, &apple_sio_aes_state_fops);

	return 0;
}

static const struct apple_sio_aes_data apple_sio_aes_v3 = {
	.revision = 3,
	.shared_disable = true,
};

static const struct apple_sio_aes_data apple_sio_aes_t6041 = {
	.revision = 5,
	.uid_offset = 0x04,
	.gid0_offset = 0x08,
	.gid1_offset = 0x0c,
};

static const struct apple_sio_aes_data apple_sio_aes_t8140 = {
	.revision = 5,
	.uid_offset = 0x40,
	.gid0_offset = 0x44,
	.gid1_offset = 0x48,
};

static const struct of_device_id apple_sio_aes_of_match[] = {
	{ .compatible = "apple,t6000-aes", .data = &apple_sio_aes_v3 },
	{ .compatible = "apple,t8112-aes", .data = &apple_sio_aes_v3 },
	{ .compatible = "apple,t6041-aes", .data = &apple_sio_aes_t6041 },
	{ .compatible = "apple,t8140-aes", .data = &apple_sio_aes_t8140 },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_sio_aes_of_match);

static struct platform_driver apple_sio_aes_driver = {
	.probe = apple_sio_aes_probe,
	.driver = {
		.name = "apple-sio-aes",
		.of_match_table = apple_sio_aes_of_match,
	},
};
module_platform_driver(apple_sio_aes_driver);

MODULE_DESCRIPTION("Apple SIO AES probe-time read-only diagnostics");
MODULE_AUTHOR("Ryan Murray <81541646+RyanTheTide@users.noreply.github.com>");
MODULE_LICENSE("GPL");
