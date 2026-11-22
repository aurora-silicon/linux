// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T8140 memory cache controller: system level cache allocation
 *
 * The boot firmware enables and powers the cache ways of every plane but
 * leaves allocation disabled, so the cache holds no data. This driver allows
 * allocation once the devices that took over DMA from the bootloader are
 * bound, and disables allocation and flushes the cache again on shutdown.
 */

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
#define MCC_CACHE_TAGS_POWERED	GENMASK(14, 8)
#define MCC_CACHE_FLUSH		0x2844
#define MCC_CACHE_FLUSH_BUSY	BIT(0)

#define MCC_MAX_PLANES		32
#define MCC_FLUSH_TIMEOUT_US	100000

struct apple_mcc {
	struct device *dev;
	void __iomem *planes[MCC_MAX_PLANES];
	unsigned int nr_planes;
	bool enabled;
	bool retained;
	/*
	 * T8142: a deep sleep also turns the cache's ways off, as AppleT8142MCC's sleep action
	 * does, and the wake puts the boot's way count back.
	 */
	bool ways_off;
	u32 ways;
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

/*
 * As traced on macOS: both multicast apertures to 0, then each plane's status counts down to 0
 * while it writes back. Nothing allocates from here on, so whatever runs after this goes straight
 * to memory. A write-back that doesn't finish refuses the sleep, as the data would be lost.
 */
static int apple_mcc_disable_t8132(struct apple_mcc *mcc)
{
	u32 first, status;
	ktime_t start;
	unsigned int p;

	start = ktime_get();
	first = readl(mcc->planes[0] + 0x1c04);
	writel(0, mcc->planes[0] + 0x1c1c00);
	writel(0, mcc->planes[1] + 0x1c1c00);
	/* Complete both multicast requests before observing per-plane status. */
	mb();
	for (p = 0; p < 4; p++) {
		void __iomem *plane = mcc->planes[p / 2] + (p % 2) * 0x40000;

		if (readl_poll_timeout_atomic(plane + 0x1c04, status, !status, 10,
					      MCC_FLUSH_TIMEOUT_US)) {
			dev_err(mcc->dev, "plane %u write-back timed out: status %#x\n", p, status);
			return -ETIMEDOUT;
		}
	}
	dev_dbg(mcc->dev, "cache written back in %lld us, status was %#x\n",
		 ktime_us_delta(ktime_get(), start), first);
	return 0;
}

/*
 * T8132 powers the whole cache through two multicast apertures: 1 turns it on,
 * and 0 writes it back and turns it off, see apple_mcc_disable_t8132().
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
		if (ret) {
			/* Not confirmed on: off again, so that no sleep finds it allocating. */
			apple_mcc_disable_t8132(mcc);
			return ret;
		}
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

/*
 * AppleT8142MCC's mccEnableCacheMode and aura's cold-boot enable: every plane's way count,
 * then each plane's data and tag ways (0x2804 [30:24] and [14:8]) waited for until both
 * read it. macOS waits without a limit.
 */
static int apple_mcc_set_ways(struct apple_mcc *mcc, u32 ways)
{
	unsigned int i;
	u32 val;
	int ret;

	for (i = 0; i < mcc->nr_planes; i++)
		apple_mcc_writel(mcc, i, ways, MCC_CACHE_WAYS);

	for (i = 0; i < mcc->nr_planes; i++) {
		ret = readl_poll_timeout_atomic(mcc->planes[i] + MCC_CACHE_POWER, val,
						FIELD_GET(MCC_CACHE_WAYS_POWERED, val) == ways &&
						FIELD_GET(MCC_CACHE_TAGS_POWERED, val) == ways,
						10, MCC_FLUSH_TIMEOUT_US);
		if (ret) {
			dev_err(mcc->dev, "plane %u ways did not reach %u: %#x\n", i, ways, val);
			return ret;
		}
	}

	return 0;
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

static int apple_mcc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_mcc *mcc;
	int ret;

	mcc = devm_kzalloc(dev, sizeof(*mcc), GFP_KERNEL);
	if (!mcc)
		return -ENOMEM;

	mcc->dev = dev;
	mcc->retained = of_device_is_compatible(dev->of_node, "apple,t8132-mcc");
	mcc->ways_off = of_device_is_compatible(dev->of_node, "apple,t8142-mcc");
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
	if (mcc->ways_off)
		mcc->ways = FIELD_GET(MCC_CACHE_WAYS_ENABLED,
				      readl(mcc->planes[0] + MCC_CACHE_WAYS));
	dev_info(dev, "system level cache allocation enabled on %u planes\n", mcc->nr_planes);
	return 0;
}

static void apple_mcc_remove(struct platform_device *pdev)
{
	struct apple_mcc *mcc = platform_get_drvdata(pdev);

	/* Without this driver nothing writes T8132's cache back before a deep sleep. */
	if (mcc->retained) {
		apple_mcc_disable_t8132(mcc);
		mcc->enabled = false;
		return;
	}
	apple_mcc_quiesce(mcc);
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
	int ret;

	if (!pm_suspend_via_firmware())
		return 0;
	/* T8132's cache is written back even if probe left it alone: nothing else does. */
	if (mcc->retained) {
		ret = apple_mcc_disable_t8132(mcc);
		if (ret)
			return ret;
		mcc->sleep_disabled = mcc->enabled;
		mcc->enabled = false;
		return 0;
	}
	if (!mcc->enabled)
		return 0;
	apple_mcc_quiesce(mcc);
	/*
	 * T8142 has no skip-mcc-off-to-s2r (only T8140's J700 does), so macOS turns the ways
	 * off before the sleep: way count 0. The flush above (the kext's own cacheFlush) has
	 * written everything back already, so a timeout here loses nothing and only leaves
	 * the ways as they were.
	 */
	if (mcc->ways_off && mcc->ways)
		apple_mcc_set_ways(mcc, 0);
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
	/* The way count the boot set, back (it reads 0 after the power loss, or after the above). */
	if (mcc->ways_off && mcc->ways &&
	    FIELD_GET(MCC_CACHE_WAYS_ENABLED, readl(mcc->planes[0] + MCC_CACHE_WAYS)) != mcc->ways)
		apple_mcc_set_ways(mcc, mcc->ways);
	/* Only a performance setting: a cache not ready after the wake stays off. */
	ret = mcc->retained ? apple_mcc_enable_t8132(mcc) : apple_mcc_enable(mcc);
	mcc->enabled = !ret;
	if (ret)
		dev_warn(dev, "system level cache allocation stays off after the sleep: %d\n", ret);

	return 0;
}

static const struct dev_pm_ops apple_mcc_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(apple_mcc_suspend_noirq, apple_mcc_resume_noirq)
};

static const struct of_device_id apple_mcc_of_match[] = {
	{ .compatible = "apple,t8132-mcc" },
	{ .compatible = "apple,t8140-mcc" },
	{ .compatible = "apple,t8142-mcc" },
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
MODULE_DESCRIPTION("Apple T8140 system level cache allocation");
MODULE_LICENSE("Dual MIT/GPL");

#if IS_ENABLED(CONFIG_APPLE_MCC_KUNIT_TEST)
#include "mcc-test.c"
#endif
