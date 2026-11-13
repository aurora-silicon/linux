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
#include <linux/sizes.h>
#include <linux/unaligned.h>

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

/* T6041 per-DSID self-clearing deprioritize-action table. */
#define MCC_DSID_DEPRIO_ACTION_BASE		0x13ffc
#define MCC_DSID_DEPRIO_ACTION_DEPRIORITIZE	2
#define MCC_DSID_DEPRIO_ACTION_STRIDE		sizeof(u32)
#define MCC_T6041_PLANE_SIZE			SZ_256K
#define MCC_DSID_DEPRIO_MAX_MAPPED					\
	((MCC_T6041_PLANE_SIZE - MCC_DSID_DEPRIO_ACTION_BASE - sizeof(u32)) / \
	 MCC_DSID_DEPRIO_ACTION_STRIDE)

#define MCC_MCACHE_STREAM_PROPERTY	"apple,tunable-mcache-stream"
#define MCC_MCACHE_STREAM_RECORD_WORDS	12
#define MCC_GPU_PARAMETER_BUFFER_STREAM_ID	14
#define MCC_MCACHE_POLICY_PMP_DYNAMIC_QUOTA	2
#define MCC_MCACHE_DEFAULT_REQUESTED_BYTES	0

#define MCC_MAX_PLANES		16
#define MCC_FLUSH_TIMEOUT_US	100000

struct apple_mcc {
	struct device *dev;
	void __iomem *planes[MCC_MAX_PLANES];
	unsigned int nr_planes;
	bool enabled;
	bool retained;
};

/* Little-endian records copied verbatim from the PMP device tree node. */
struct apple_mcc_mcache_stream_record {
	__le32 stream_id;
	__le32 driver_instance; /* Owner slot, not a record count. */
	__le32 priority;
	__le32 first_quota_group;
	__le32 last_quota_group;
	__le32 first_dsid;
	__le32 last_dsid;
	__le32 allocation_policy;
	__le32 default_assigned_bytes;
	__le32 default_requested_bytes;
	__le32 persistence;
	__le32 flags;
};

static_assert(sizeof(struct apple_mcc_mcache_stream_record) ==
	      MCC_MCACHE_STREAM_RECORD_WORDS * sizeof(__le32));

static u32 apple_mcc_stream_field(const u8 *record, size_t offset)
{
	return get_unaligned_le32(record + offset);
}

#define MCC_STREAM_FIELD(record, field)					\
	apple_mcc_stream_field((record),					\
		offsetof(struct apple_mcc_mcache_stream_record, field))

static int apple_mcc_parse_stream_dsid(const u8 *records, int size, u32 *dsid)
{
	bool found = false;
	int offset;

	if (!records || size <= 0 ||
	    size % sizeof(struct apple_mcc_mcache_stream_record))
		return -EINVAL;

	for (offset = 0; offset < size;
	     offset += sizeof(struct apple_mcc_mcache_stream_record)) {
		const u8 *record = records + offset;
		u32 first_group, last_group, first_dsid, last_dsid;
		u32 policy, requested;

		if (MCC_STREAM_FIELD(record, stream_id) !=
		    MCC_GPU_PARAMETER_BUFFER_STREAM_ID)
			continue;
		if (found)
			return -EINVAL;
		found = true;

		first_group = MCC_STREAM_FIELD(record, first_quota_group);
		last_group = MCC_STREAM_FIELD(record, last_quota_group);
		first_dsid = MCC_STREAM_FIELD(record, first_dsid);
		last_dsid = MCC_STREAM_FIELD(record, last_dsid);
		policy = MCC_STREAM_FIELD(record, allocation_policy);
		requested = MCC_STREAM_FIELD(record, default_requested_bytes);

		if (first_group != last_group || first_dsid != last_dsid ||
		    policy != MCC_MCACHE_POLICY_PMP_DYNAMIC_QUOTA ||
		    requested != MCC_MCACHE_DEFAULT_REQUESTED_BYTES ||
		    first_dsid > MCC_DSID_DEPRIO_MAX_MAPPED)
			return -EINVAL;
		*dsid = first_dsid;
	}

	return found ? 0 : -ENOENT;
}

static int apple_mcc_t6041_stream_dsid(struct apple_mcc *mcc, u32 *dsid)
{
	struct device_node *pmp;
	const u8 *records;
	int size, ret;

	pmp = of_parse_phandle(mcc->dev->of_node, "apple,pmp", 0);
	if (!pmp)
		return -ENOENT;
	records = of_get_property(pmp, MCC_MCACHE_STREAM_PROPERTY, &size);
	ret = apple_mcc_parse_stream_dsid(records, records ? size : 0, dsid);
	of_node_put(pmp);

	return ret;
}

static void apple_mcc_writel(struct apple_mcc *mcc, unsigned int plane,
			     u32 value, u32 offset)
{
#if IS_ENABLED(CONFIG_APPLE_MCC_KUNIT_TEST)
	KUNIT_STATIC_STUB_REDIRECT(apple_mcc_writel, mcc, plane, value, offset);
#endif
	writel(value, mcc->planes[plane] + offset);
}

static int apple_mcc_release_t6041_stream(struct apple_mcc *mcc)
{
	unsigned int i;
	u32 dsid;
	int ret;

	ret = apple_mcc_t6041_stream_dsid(mcc, &dsid);
	if (ret)
		return ret;

	for (i = 0; i < mcc->nr_planes; i++)
		apple_mcc_writel(mcc, i, MCC_DSID_DEPRIO_ACTION_DEPRIORITIZE,
				 MCC_DSID_DEPRIO_ACTION_BASE +
				 dsid * MCC_DSID_DEPRIO_ACTION_STRIDE);

	/* The action self-clears, so a read-back cannot verify the write. */
	dev_info(mcc->dev, "released SLC residency for stream %u DSID %u\n",
		 MCC_GPU_PARAMETER_BUFFER_STREAM_ID, dsid);
	return 0;
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

static int apple_mcc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_mcc *mcc;
	bool t6041;
	int ret;

	mcc = devm_kzalloc(dev, sizeof(*mcc), GFP_KERNEL);
	if (!mcc)
		return -ENOMEM;

	mcc->dev = dev;
	t6041 = of_device_is_compatible(dev->of_node, "apple,t6041-mcc");
	mcc->retained = of_device_is_compatible(dev->of_node, "apple,t8132-mcc");
	while (platform_get_resource(pdev, IORESOURCE_MEM, mcc->nr_planes)) {
		struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, mcc->nr_planes);

		if (mcc->nr_planes == MCC_MAX_PLANES)
			return dev_err_probe(dev, -EINVAL, "too many planes\n");
		if (resource_size(res) < (mcc->retained ? 0x1c1c04 :
					  t6041 ? MCC_T6041_PLANE_SIZE : 0x4000))
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

	if (t6041) {
		ret = apple_mcc_release_t6041_stream(mcc);
		if (ret == -ENOENT)
			/* No PMP stream record to act on (e.g. a DTB or SoC
			 * variant that does not carry one): nothing to release.
			 */
			dev_dbg(dev, "no GPU parameter-buffer stream to release\n");
		else if (ret)
			dev_warn(dev, "GPU parameter-buffer stream not released: %pe\n",
				 ERR_PTR(ret));
	}

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

static const struct of_device_id apple_mcc_of_match[] = {
	{ .compatible = "apple,t8132-mcc" },
	{ .compatible = "apple,t8140-mcc" },
	{}
};
MODULE_DEVICE_TABLE(of, apple_mcc_of_match);

static struct platform_driver apple_mcc_driver = {
	.driver = {
		.name = "apple-mcc",
		.of_match_table = apple_mcc_of_match,
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
