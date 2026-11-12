// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple memory cache controller: system level cache allocation
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
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/suspend.h>

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

/*
 * T8112 exposes four cache planes through its second MCC aperture. These are
 * the cache controls the m1n1 boot loader programs on every T8112 boot.
 */
#define T8112_MCC_PLANES		4
#define T8112_MCC_PLANE_STRIDE	0x40000
#define T8112_MCC_RESOURCE	1
#define T8112_CACHE_ENABLE	0x1c00
#define T8112_CACHE_STATUS	0x1c04
#define T8112_CACHE_STATUS_DATA	GENMASK(14, 10)
#define T8112_CACHE_STATUS_TAG	GENMASK(9, 5)
#define T8112_CACHE_WAYS		16
#define T8112_CACHE_STATUS_READY	(FIELD_PREP(T8112_CACHE_STATUS_DATA, T8112_CACHE_WAYS) | \
				 FIELD_PREP(T8112_CACHE_STATUS_TAG, T8112_CACHE_WAYS))
#define T8112_CACHE_DISABLE	0x424
#define T8112_CACHE_TIMEOUT_US	10000

#define MCC_MAX_PLANES		32
#define MCC_FLUSH_TIMEOUT_US	100000

struct apple_mcc {
	struct device *dev;
	void __iomem *planes[MCC_MAX_PLANES];
	unsigned int nr_planes;
	bool enabled;
	bool retained;
	bool leave_enabled;
	/* Turned off for a deep sleep, to be turned back on after it. */
	bool sleep_disabled;
};

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

/*
 * The boot loader enables T8112's cache before Linux. Starting the PMP can
 * re-establish the memory-controller mode, so restore the same named cache
 * controls only after the PMP driver has completed its startup.
 */
static int apple_mcc_enable_t8112(struct apple_mcc *mcc)
{
	const u32 mask = T8112_CACHE_STATUS_DATA | T8112_CACHE_STATUS_TAG;
	unsigned int p;
	u32 status;
	int ret;

	if (mcc->nr_planes != T8112_MCC_PLANES)
		return -EINVAL;

	for (p = 0; p < mcc->nr_planes; p++)
		apple_mcc_writel(mcc, p, T8112_CACHE_WAYS, T8112_CACHE_ENABLE);
	/* Submit every plane's request before observing any completion. */
	mb();

	for (p = 0; p < mcc->nr_planes; p++) {
		ret = readl_poll_timeout(mcc->planes[p] + T8112_CACHE_STATUS, status,
					 (status & mask) == T8112_CACHE_STATUS_READY,
					 10, T8112_CACHE_TIMEOUT_US);
		if (ret) {
			dev_err(mcc->dev, "plane %u cache enable timed out: %#x\n", p, status);
			return ret;
		}
	}

	for (p = 0; p < mcc->nr_planes; p++)
		apple_mcc_writel(mcc, p, 0, T8112_CACHE_DISABLE);
	/* Enable every plane's allocation before checking read-back. */
	mb();
	for (p = 0; p < mcc->nr_planes; p++) {
		if (readl(mcc->planes[p] + T8112_CACHE_DISABLE)) {
			dev_err(mcc->dev, "plane %u cache allocation stayed disabled\n", p);
			return -EIO;
		}
	}

	return 0;
}

static int apple_mcc_wait_pmp(struct device *dev)
{
	struct platform_device *pdev;
	struct device_node *node;
	bool bound;

	node = of_parse_phandle(dev->of_node, "apple,pmp", 0);
	if (!node)
		return -ENODEV;
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	bound = pdev && device_is_bound(&pdev->dev);
	if (pdev)
		put_device(&pdev->dev);

	return bound ? 0 : dev_err_probe(dev, -EPROBE_DEFER,
					  "waiting for the PMP to start\n");
}

static int apple_mcc_init_t8112(struct platform_device *pdev, struct apple_mcc *mcc)
{
	const u32 mask = T8112_CACHE_STATUS_DATA | T8112_CACHE_STATUS_TAG;
	struct resource *res;
	void __iomem *base;
	unsigned int p;

	res = platform_get_resource(pdev, IORESOURCE_MEM, T8112_MCC_RESOURCE);
	if (!res || resource_size(res) <
		    (T8112_MCC_PLANES - 1) * T8112_MCC_PLANE_STRIDE + T8112_CACHE_STATUS + 4)
		return dev_err_probe(&pdev->dev, -EINVAL, "cache plane aperture too small\n");
	base = devm_platform_ioremap_resource(pdev, T8112_MCC_RESOURCE);
	if (IS_ERR(base))
		return PTR_ERR(base);
	for (p = 0; p < T8112_MCC_PLANES; p++)
		mcc->planes[p] = base + p * T8112_MCC_PLANE_STRIDE;
	mcc->nr_planes = T8112_MCC_PLANES;
	mcc->leave_enabled = true;

	for (p = 0; p < mcc->nr_planes; p++) {
		u32 status = readl(mcc->planes[p] + T8112_CACHE_STATUS);

		if ((status & mask) != T8112_CACHE_STATUS_READY)
			dev_info(&pdev->dev, "plane %u cache not ready after PMP startup: %#x\n",
				 p, status);
	}

	return apple_mcc_enable_t8112(mcc);
}

/*
 * T8112 has one MCC node for both the read-only register view and the cache
 * restore, so this driver owns the node and hands the view to apple-regview
 * through an auxiliary device. Without a PMP the boot loader's cache setup
 * stands and nothing is written.
 */
static int apple_mcc_probe_t8112(struct platform_device *pdev, struct apple_mcc *mcc)
{
	struct device *dev = &pdev->dev;
	bool pmp = of_property_present(dev->of_node, "apple,pmp");
	int ret;

	if (pmp) {
		ret = apple_mcc_wait_pmp(dev);
		if (ret)
			return ret;
	}

	if (!devm_auxiliary_device_create(dev, "regview", NULL))
		dev_warn(dev, "register view unavailable\n");

	platform_set_drvdata(pdev, mcc);
	if (!pmp)
		return 0;

	/* The cache is only a performance setting: stay bound if it fails. */
	ret = apple_mcc_init_t8112(pdev, mcc);
	if (ret) {
		dev_err(dev, "system level cache not restored after PMP startup: %d\n", ret);
		return 0;
	}
	mcc->enabled = true;
	dev_info(dev, "system level cache restored after PMP startup on %u planes\n",
		 mcc->nr_planes);
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
	if (!mcc->enabled || mcc->retained || mcc->leave_enabled)
		return;

	apple_mcc_disable_planes(mcc, mcc->nr_planes);
	mcc->enabled = false;
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
	apple_mcc_quiesce(platform_get_drvdata(pdev));
}

static void apple_mcc_shutdown(struct platform_device *pdev)
{
	struct apple_mcc *mcc = platform_get_drvdata(pdev);

	if (mcc)
		apple_mcc_quiesce(mcc);
}

/*
 * A deep sleep takes the cache's power with the rest of the SoC: stop allocating and write it
 * back first, as at shutdown, and allocate again after. Suspend-to-idle keeps it.
 */
static int apple_mcc_suspend_noirq(struct device *dev)
{
	struct apple_mcc *mcc = dev_get_drvdata(dev);

	if (!pm_suspend_via_firmware() || !mcc->enabled || mcc->retained ||
	    mcc->leave_enabled)
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
MODULE_DESCRIPTION("Apple system level cache allocation");
MODULE_LICENSE("Dual MIT/GPL");

#if IS_ENABLED(CONFIG_APPLE_MCC_KUNIT_TEST)
#include "mcc-test.c"
#endif
