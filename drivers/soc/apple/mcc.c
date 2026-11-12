// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple memory cache controller
 *
 * The boot firmware enables and powers the cache ways of every plane but
 * leaves allocation disabled, so the cache holds no data. This driver allows
 * allocation once the devices that took over DMA from the bootloader are
 * bound, and disables allocation and flushes the cache again on shutdown.
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>

#if IS_ENABLED(CONFIG_APPLE_MCC_KUNIT_TEST)
#include <kunit/static_stub.h>
#endif

#define MCC_ALLOC_DISABLE	0x82c
#define MCC_CACHE_WAYS		0x2800
#define MCC_CACHE_WAYS_ENABLED	GENMASK(4, 0)
#define MCC_CACHE_POWER		0x2804
#define MCC_CACHE_WAYS_POWERED	GENMASK(30, 24)
#define MCC_CACHE_FLUSH		0x2844
#define MCC_CACHE_FLUSH_BUSY	BIT(0)

#define MCC_MAX_PLANES		32
#define MCC_FLUSH_TIMEOUT_US	100000

/* Public m1n1 T8103/T8112 DCS layout names these DCC_DRAMCFG0/1. */
#define T8112_DCS_RESOURCE	2
#define T8112_DCS_CHANNELS	8
#define T8112_DCS_STRIDE		0x40000
#define T8112_DCS_DRAM_CONFIG_0	0xdc4
#define T8112_DCS_DRAM_CONFIG_1	0xdbc
#define T8112_PMP_POLL_MS	100

static bool restore_dram_config;
module_param(restore_dram_config, bool, 0444);
MODULE_PARM_DESC(restore_dram_config,
		 "Restore the pre-PMP T8112 DCS DRAM configuration after PMP startup");

struct t8112_dram_config {
	u32 config0;
	u32 config1;
};

struct apple_mcc {
	struct device *dev;
	void __iomem *planes[MCC_MAX_PLANES];
	unsigned int nr_planes;
	bool enabled;
	bool retained;
	/* Turned off for a deep sleep, to be turned back on after it. */
	bool sleep_disabled;
	void __iomem *dcs;
	struct t8112_dram_config boot_config[T8112_DCS_CHANNELS];
	struct t8112_dram_config pmp_config[T8112_DCS_CHANNELS];
	struct delayed_work pmp_work;
	struct mutex config_lock; /* protects snapshots and restore state */
	bool captured_before_pmp;
	bool pmp_sampled;
	bool config_restored;
};

static struct t8112_dram_config apple_mcc_t8112_read_config(struct apple_mcc *mcc,
							    unsigned int channel)
{
	void __iomem *base;

#if IS_ENABLED(CONFIG_APPLE_MCC_KUNIT_TEST)
	KUNIT_STATIC_STUB_REDIRECT(apple_mcc_t8112_read_config, mcc, channel);
#endif
	base = mcc->dcs + channel * T8112_DCS_STRIDE;
	return (struct t8112_dram_config) {
		.config0 = readl(base + T8112_DCS_DRAM_CONFIG_0),
		.config1 = readl(base + T8112_DCS_DRAM_CONFIG_1),
	};
}

static void apple_mcc_t8112_write_config(struct apple_mcc *mcc, unsigned int channel,
					 const struct t8112_dram_config *config)
{
	void __iomem *base;

#if IS_ENABLED(CONFIG_APPLE_MCC_KUNIT_TEST)
	KUNIT_STATIC_STUB_REDIRECT(apple_mcc_t8112_write_config, mcc, channel, config);
#endif
	base = mcc->dcs + channel * T8112_DCS_STRIDE;
	writel(config->config0, base + T8112_DCS_DRAM_CONFIG_0);
	writel(config->config1, base + T8112_DCS_DRAM_CONFIG_1);
}

static bool apple_mcc_t8112_config_equal(const struct t8112_dram_config *a,
					 const struct t8112_dram_config *b)
{
	return a->config0 == b->config0 && a->config1 == b->config1;
}

static bool apple_mcc_t8112_pmp_bound(struct device *dev)
{
	struct platform_device *pdev;
	struct device_node *node;
	bool bound;

	node = of_parse_phandle(dev->of_node, "apple,pmp", 0);
	if (!node)
		return false;
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	bound = pdev && device_is_bound(&pdev->dev);
	if (pdev)
		put_device(&pdev->dev);
	return bound;
}

static int apple_mcc_t8112_restore_config(struct apple_mcc *mcc)
{
	unsigned int channel;

	for (channel = 0; channel < T8112_DCS_CHANNELS; channel++)
		apple_mcc_t8112_write_config(mcc, channel, &mcc->boot_config[channel]);
	/* Complete every channel's pair before validating the restored set. */
	mb();
	for (channel = 0; channel < T8112_DCS_CHANNELS; channel++) {
		struct t8112_dram_config live =
			apple_mcc_t8112_read_config(mcc, channel);

		if (!apple_mcc_t8112_config_equal(&live, &mcc->boot_config[channel]))
			goto rollback;
	}
	return 0;

rollback:
	/* A partial performance restore is worse than the PMP's coherent set. */
	for (channel = 0; channel < T8112_DCS_CHANNELS; channel++)
		apple_mcc_t8112_write_config(mcc, channel, &mcc->pmp_config[channel]);
	/* Complete the rollback before reporting failure to the caller. */
	mb();
	return -EIO;
}

static void apple_mcc_t8112_pmp_work(struct work_struct *work)
{
	struct apple_mcc *mcc = container_of(to_delayed_work(work), struct apple_mcc,
					     pmp_work);
	unsigned int channel, changed = 0;
	int ret;

	if (!apple_mcc_t8112_pmp_bound(mcc->dev)) {
		schedule_delayed_work(&mcc->pmp_work, msecs_to_jiffies(T8112_PMP_POLL_MS));
		return;
	}

	mutex_lock(&mcc->config_lock);
	for (channel = 0; channel < T8112_DCS_CHANNELS; channel++) {
		mcc->pmp_config[channel] = apple_mcc_t8112_read_config(mcc, channel);
		changed += !apple_mcc_t8112_config_equal(&mcc->boot_config[channel],
							 &mcc->pmp_config[channel]);
	}
	mcc->pmp_sampled = true;
	dev_info(mcc->dev, "PMP changed DCS DRAM configuration on %u/%u channels%s\n",
		 changed, T8112_DCS_CHANNELS,
		 restore_dram_config ? "; restore requested" : "; read-only observation");

	if (!restore_dram_config || !changed)
		goto out;
	if (!mcc->captured_before_pmp) {
		dev_err(mcc->dev, "refusing DCS DRAM restore: no pre-PMP snapshot\n");
		goto out;
	}
	ret = apple_mcc_t8112_restore_config(mcc);
	if (ret) {
		dev_err(mcc->dev, "DCS DRAM configuration restore failed; PMP values restored\n");
	} else {
		mcc->config_restored = true;
		dev_info(mcc->dev, "pre-PMP DCS DRAM configuration restored on %u channels\n",
			 T8112_DCS_CHANNELS);
	}
out:
	mutex_unlock(&mcc->config_lock);
}

static ssize_t dram_config_snapshot_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	struct apple_mcc *mcc = dev_get_drvdata(dev);
	ssize_t len;
	unsigned int channel;

	mutex_lock(&mcc->config_lock);
	len = sysfs_emit(buf,
			 "captured_before_pmp=%u pmp_sampled=%u restore_requested=%u restored=%u\n",
			 mcc->captured_before_pmp, mcc->pmp_sampled,
			 restore_dram_config, mcc->config_restored);
	for (channel = 0; channel < T8112_DCS_CHANNELS; channel++) {
		struct t8112_dram_config live =
			apple_mcc_t8112_read_config(mcc, channel);

		len += sysfs_emit_at(buf, len,
			"channel%u boot=%#010x/%#010x pmp=%#010x/%#010x current=%#010x/%#010x\n",
			channel, mcc->boot_config[channel].config0,
			mcc->boot_config[channel].config1,
			mcc->pmp_config[channel].config0,
			mcc->pmp_config[channel].config1,
			live.config0, live.config1);
	}
	mutex_unlock(&mcc->config_lock);
	return len;
}
static DEVICE_ATTR_RO(dram_config_snapshot);

static void apple_mcc_writel(struct apple_mcc *mcc, unsigned int plane,
			     u32 value, u32 offset)
{
#if IS_ENABLED(CONFIG_APPLE_MCC_KUNIT_TEST)
	KUNIT_STATIC_STUB_REDIRECT(apple_mcc_writel, mcc, plane, value, offset);
#endif
	writel(value, mcc->planes[plane] + offset);
}

/* T8132 powers the whole cache through two multicast apertures. Unlike
 * T8140 allocation control, its dirty-data drain is not qualified yet.
 */
static int apple_mcc_enable_t8132(struct apple_mcc *mcc)
{
	u32 control[4], status[4];
	bool off = true, on = true;
	unsigned int p;
	int ret;

	if (mcc->nr_planes != 2)
		return -EINVAL;
	for (p = 0; p < 4; p++) {
		void __iomem *plane = mcc->planes[p / 2] + (p % 2) * 0x40000;

		control[p] = readl(plane + 0x1c00);
		status[p] = readl(plane + 0x1c04);
		off &= !control[p] && !status[p];
		on &= control[p] == 1 && (status[p] & 0x3ff0) == 0x2100;
	}
	if (on)
		return 0;
	if (!off)
		return -EBUSY;
	writel(1, mcc->planes[0] + 0x1c1c00);
	writel(1, mcc->planes[1] + 0x1c1c00);
	/* Complete both multicast requests before observing per-plane status. */
	mb();
	for (p = 0; p < 4; p++) {
		void __iomem *plane = mcc->planes[p / 2] + (p % 2) * 0x40000;

		ret = readl_poll_timeout(plane + 0x1c04, status[p],
					(status[p] & 0x3ff0) == 0x2100, 100, 10000);
		if (ret)
			return ret;
	}
	return 0;
}

static int apple_mcc_wait_boot_dma(struct device *dev)
{
	struct of_phandle_iterator it;
	int ret;

	of_for_each_phandle(&it, ret, dev->of_node, "apple,boot-dma-devices", NULL, 0) {
		struct platform_device *pdev = of_find_device_by_node(it.node);
		bool bound = pdev && device_is_bound(&pdev->dev);

		if (pdev)
			put_device(&pdev->dev);
		if (!bound) {
			ret = dev_err_probe(dev, -EPROBE_DEFER, "waiting for %pOF to bind\n",
					    it.node);
			of_node_put(it.node);
			return ret;
		}
	}

	return ret == -ENOENT ? 0 : ret;
}

static bool apple_mcc_plane_ready(struct apple_mcc *mcc, unsigned int plane)
{
	void __iomem *regs = mcc->planes[plane];
	u32 ways = FIELD_GET(MCC_CACHE_WAYS_ENABLED, readl(regs + MCC_CACHE_WAYS));
	u32 powered = FIELD_GET(MCC_CACHE_WAYS_POWERED, readl(regs + MCC_CACHE_POWER));
	u32 flush = readl(regs + MCC_CACHE_FLUSH);

	if (ways && powered == ways && !(flush & MCC_CACHE_FLUSH_BUSY))
		return true;

	dev_warn(mcc->dev, "plane %u not ready: %u ways enabled, %u powered, flush %#x\n",
		 plane, ways, powered, flush);
	return false;
}

/* Stop allocating on the first @count planes, then write back and invalidate them. */
static void apple_mcc_disable_planes(struct apple_mcc *mcc, unsigned int count)
{
	unsigned int i;
	u32 val;

	for (i = 0; i < count; i++) {
		apple_mcc_writel(mcc, i, 1, MCC_ALLOC_DISABLE);
		apple_mcc_writel(mcc, i, 1, MCC_CACHE_FLUSH);
	}

	for (i = 0; i < count; i++)
		if (readl_poll_timeout_atomic(mcc->planes[i] + MCC_CACHE_FLUSH, val,
					      !(val & MCC_CACHE_FLUSH_BUSY), 10,
					      MCC_FLUSH_TIMEOUT_US))
			dev_err(mcc->dev, "plane %u flush timed out\n", i);
}

static int apple_mcc_enable(struct apple_mcc *mcc)
{
	unsigned int i;

	for (i = 0; i < mcc->nr_planes; i++)
		if (!apple_mcc_plane_ready(mcc, i))
			return -EBUSY;

	for (i = 0; i < mcc->nr_planes; i++) {
		apple_mcc_writel(mcc, i, 0, MCC_ALLOC_DISABLE);
		if (readl(mcc->planes[i] + MCC_ALLOC_DISABLE)) {
			dev_err(mcc->dev, "plane %u did not enable allocation\n", i);
			/*
			 * The driver will not bind, so nothing disables these
			 * planes on shutdown: disable the ones enabled so far,
			 * and this one, whose state is unknown, now.
			 */
			apple_mcc_disable_planes(mcc, i + 1);
			return -EIO;
		}
	}

	return 0;
}

static void apple_mcc_quiesce(struct apple_mcc *mcc)
{
	if (!mcc->enabled || mcc->retained)
		return;

	apple_mcc_disable_planes(mcc, mcc->nr_planes);
	mcc->enabled = false;
}

static int apple_mcc_probe_t8112(struct platform_device *pdev, struct apple_mcc *mcc)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	unsigned int channel;
	int ret;

	if (!devm_auxiliary_device_create(dev, "regview", NULL))
		dev_warn(dev, "register view unavailable\n");
	platform_set_drvdata(pdev, mcc);

	/* Default DTs have no PMP link and retain the bootloader configuration. */
	if (!of_property_present(dev->of_node, "apple,pmp"))
		return 0;

	res = platform_get_resource(pdev, IORESOURCE_MEM, T8112_DCS_RESOURCE);
	if (!res || resource_size(res) <
		    (T8112_DCS_CHANNELS - 1) * T8112_DCS_STRIDE +
		    T8112_DCS_DRAM_CONFIG_0 + sizeof(u32))
		return dev_err_probe(dev, -EINVAL, "DCS aperture too small\n");
	mcc->dcs = devm_platform_ioremap_resource(pdev, T8112_DCS_RESOURCE);
	if (IS_ERR(mcc->dcs))
		return PTR_ERR(mcc->dcs);

	mutex_init(&mcc->config_lock);
	INIT_DELAYED_WORK(&mcc->pmp_work, apple_mcc_t8112_pmp_work);
	mcc->captured_before_pmp = !apple_mcc_t8112_pmp_bound(dev);
	for (channel = 0; channel < T8112_DCS_CHANNELS; channel++)
		mcc->boot_config[channel] = apple_mcc_t8112_read_config(mcc, channel);
	/* A PMP which bound during the snapshot makes its provenance ambiguous. */
	if (apple_mcc_t8112_pmp_bound(dev))
		mcc->captured_before_pmp = false;

	ret = device_create_file(dev, &dev_attr_dram_config_snapshot);
	if (ret)
		return dev_err_probe(dev, ret, "failed to publish DCS DRAM snapshot\n");
	schedule_delayed_work(&mcc->pmp_work, 0);
	dev_info(dev, "captured %u-channel DCS DRAM configuration before PMP: %s\n",
		 T8112_DCS_CHANNELS, mcc->captured_before_pmp ? "yes" : "no");
	return 0;
}

static int apple_mcc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_mcc *mcc;
	int ret;

	mcc = devm_kzalloc(dev, sizeof(*mcc), GFP_KERNEL);
	if (!mcc)
		return -ENOMEM;

	mcc->dev = dev;
	if (of_device_is_compatible(dev->of_node, "apple,t8112-mcc"))
		return apple_mcc_probe_t8112(pdev, mcc);
	mcc->retained = of_device_is_compatible(dev->of_node, "apple,t8132-mcc");
	while (platform_get_resource(pdev, IORESOURCE_MEM, mcc->nr_planes)) {
		struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, mcc->nr_planes);

		if (mcc->nr_planes == MCC_MAX_PLANES)
			return dev_err_probe(dev, -EINVAL, "too many planes\n");
		if (resource_size(res) < (mcc->retained ? 0x1c1c04 : 0x4000))
			return dev_err_probe(dev, -EINVAL, "cache register aperture too small\n");
		mcc->planes[mcc->nr_planes] = devm_platform_ioremap_resource(pdev, mcc->nr_planes);
		if (IS_ERR(mcc->planes[mcc->nr_planes]))
			return PTR_ERR(mcc->planes[mcc->nr_planes]);
		mcc->nr_planes++;
	}
	if (!mcc->nr_planes)
		return dev_err_probe(dev, -EINVAL, "no planes\n");

	ret = apple_mcc_wait_boot_dma(dev);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, mcc);

	/* Allocation is only a performance setting: leave a cache that is not ready alone. */
	ret = mcc->retained ? apple_mcc_enable_t8132(mcc) : apple_mcc_enable(mcc);
	if (ret == -EBUSY)
		return 0;
	if (ret)
		return ret;

	mcc->enabled = true;
	dev_info(dev, "system level cache allocation enabled on %u planes\n", mcc->nr_planes);
	return 0;
}

static void apple_mcc_remove(struct platform_device *pdev)
{
	struct apple_mcc *mcc = platform_get_drvdata(pdev);

	if (mcc && mcc->dcs) {
		cancel_delayed_work_sync(&mcc->pmp_work);
		device_remove_file(&pdev->dev, &dev_attr_dram_config_snapshot);
	}
	apple_mcc_quiesce(mcc);
}

static void apple_mcc_shutdown(struct platform_device *pdev)
{
	struct apple_mcc *mcc = platform_get_drvdata(pdev);

	if (mcc) {
		if (mcc->dcs)
			cancel_delayed_work_sync(&mcc->pmp_work);
		apple_mcc_quiesce(mcc);
	}
}

/*
 * A deep sleep takes the cache's power with the rest of the SoC: stop allocating and write it
 * back first, as at shutdown, and allocate again after. Suspend-to-idle keeps it.
 */
static int apple_mcc_suspend_noirq(struct device *dev)
{
	struct apple_mcc *mcc = dev_get_drvdata(dev);

	if (!pm_suspend_via_firmware() || !mcc->enabled || mcc->retained)
		return 0;
	apple_mcc_quiesce(mcc);
	mcc->sleep_disabled = true;

	return 0;
}

static int apple_mcc_resume_noirq(struct device *dev)
{
	struct apple_mcc *mcc = dev_get_drvdata(dev);
	int ret;

	if (!mcc->sleep_disabled)
		return 0;
	mcc->sleep_disabled = false;
	/* Only a performance setting: a cache not ready after the wake stays off. */
	ret = apple_mcc_enable(mcc);
	mcc->enabled = !ret;
	if (ret)
		dev_warn(dev, "system level cache allocation stays off after the sleep: %d\n", ret);

	return 0;
}

static const struct dev_pm_ops apple_mcc_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(apple_mcc_suspend_noirq, apple_mcc_resume_noirq)
};

static const struct of_device_id apple_mcc_of_match[] = {
	{ .compatible = "apple,t8112-mcc" },
	{ .compatible = "apple,t8132-mcc" },
	{ .compatible = "apple,t8140-mcc" },
	{}
};
MODULE_DEVICE_TABLE(of, apple_mcc_of_match);

static struct platform_driver apple_mcc_driver = {
	.driver = {
		.name = "apple-mcc",
		.of_match_table = apple_mcc_of_match,
		.pm = pm_sleep_ptr(&apple_mcc_pm_ops),
	},
	.probe = apple_mcc_probe,
	.remove = apple_mcc_remove,
	.shutdown = apple_mcc_shutdown,
};
module_platform_driver(apple_mcc_driver);

MODULE_AUTHOR("Ryan Murray <ryan@aurorasilicon.org>");
MODULE_DESCRIPTION("Apple memory-controller performance handoff");
MODULE_LICENSE("Dual MIT/GPL");

#if IS_ENABLED(CONFIG_APPLE_MCC_KUNIT_TEST)
#include "mcc-test.c"
#endif
