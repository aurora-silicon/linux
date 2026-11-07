/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * KUnit tests of tas2764_parse_bop_config(), tas2764_parse_dcblocker_config()
 * and tas2764_parse_tx_zd_config(): the SN012776 brownout block, DC blockers
 * and idle slots come from the device tree's apple,bop-config,
 * apple,amp-dcblocker-config and apple,amp-tx-zd-config, which the boot
 * loader copies from the Mac's ADT, and from nowhere else.
 */
#include <kunit/of.h>
#include <kunit/test.h>

/* A device on the test node @node, and a driver state not from the device tree */
static struct tas2764_priv *tas2764_test_priv(struct kunit *test, const char *node,
					      enum tas2764_devid devid,
					      struct device **devp)
{
	struct tas2764_priv *tas2764;
	struct device *dev;

	dev = kunit_kzalloc(test, sizeof(*dev), GFP_KERNEL);
	tas2764 = kunit_kzalloc(test, sizeof(*tas2764), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	KUNIT_ASSERT_NOT_NULL(test, tas2764);

	dev->of_node = of_find_node_by_name(NULL, node);
	KUNIT_ASSERT_NOT_NULL(test, dev->of_node);
	of_node_put_kunit(test, dev->of_node);

	tas2764->devid = devid;
	/* A value that did not come from the device tree must not survive */
	tas2764->have_bop_config = true;
	memset(tas2764->bop_config, 0xee, sizeof(tas2764->bop_config));
	tas2764->have_dc_blk = true;
	memset(tas2764->dc_blk, 0xee, sizeof(tas2764->dc_blk));
	tas2764->have_tx_zd = true;
	tas2764->tx_zd_mask = 0xeeeeeeee;

	*devp = dev;
	return tas2764;
}

static struct tas2764_priv *tas2764_bop_test_parse(struct kunit *test,
						   const char *node,
						   enum tas2764_devid devid,
						   int *ret)
{
	struct tas2764_priv *tas2764;
	struct device *dev;

	tas2764 = tas2764_test_priv(test, node, devid, &dev);
	*ret = tas2764_parse_bop_config(dev, tas2764);
	return tas2764;
}

/* The block is the property, byte for byte and in register order */
static void tas2764_bop_from_dt(struct kunit *test)
{
	struct tas2764_priv *tas2764;
	int ret, i;

	tas2764 = tas2764_bop_test_parse(test, "tas2764-kunit-bop-good",
					 DEVID_SN012776, &ret);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, tas2764->have_bop_config);
	for (i = 0; i < TAS2764_BOP_CFG_LEN; i++)
		KUNIT_EXPECT_EQ(test, tas2764->bop_config[i], i);
}

/* No property: no block, and no value from anywhere else */
static void tas2764_bop_absent(struct kunit *test)
{
	struct tas2764_priv *tas2764;
	int ret;

	tas2764 = tas2764_bop_test_parse(test, "tas2764-kunit-bop-none",
					 DEVID_SN012776, &ret);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, tas2764->have_bop_config);
}

/* A block of the wrong length is a broken device tree: the probe fails */
static void tas2764_bop_wrong_length(struct kunit *test)
{
	struct tas2764_priv *tas2764;
	int ret;

	tas2764 = tas2764_bop_test_parse(test, "tas2764-kunit-bop-short",
					 DEVID_SN012776, &ret);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_FALSE(test, tas2764->have_bop_config);

	tas2764 = tas2764_bop_test_parse(test, "tas2764-kunit-bop-long",
					 DEVID_SN012776, &ret);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_FALSE(test, tas2764->have_bop_config);
}

/* The TAS2764 itself has no such block: the property is not its */
static void tas2764_bop_not_sn012776(struct kunit *test)
{
	struct tas2764_priv *tas2764;
	int ret;

	tas2764 = tas2764_bop_test_parse(test, "tas2764-kunit-bop-good",
					 DEVID_TAS2764, &ret);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, tas2764->have_bop_config);
}

/* As tas2764_bop_test_parse(), for the DC blockers */
static struct tas2764_priv *tas2764_dcb_test_parse(struct kunit *test,
						   const char *node,
						   enum tas2764_devid devid,
						   int *ret)
{
	struct tas2764_priv *tas2764;
	struct device *dev;

	tas2764 = tas2764_test_priv(test, node, devid, &dev);
	*ret = tas2764_parse_dcblocker_config(dev, tas2764);
	return tas2764;
}

/* DC_BLK0 and DC_BLK1 are the property's first two bytes; the rest is padding */
static void tas2764_dcb_from_dt(struct kunit *test)
{
	struct tas2764_priv *tas2764;
	int ret;

	tas2764 = tas2764_dcb_test_parse(test, "tas2764-kunit-bop-good",
					 DEVID_SN012776, &ret);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, tas2764->have_dc_blk);
	KUNIT_EXPECT_EQ(test, tas2764->dc_blk[0], 0x5a);
	KUNIT_EXPECT_EQ(test, tas2764->dc_blk[1], 0xa5);
}

/* No property: explicitly program the SN012776 fallback on every amp. */
static void tas2764_dcb_absent(struct kunit *test)
{
	struct tas2764_priv *tas2764;
	int ret;

	tas2764 = tas2764_dcb_test_parse(test, "tas2764-kunit-bop-none",
					 DEVID_SN012776, &ret);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, tas2764->have_dc_blk);
	KUNIT_EXPECT_EQ(test, tas2764->dc_blk[0], 0x01);
	KUNIT_EXPECT_EQ(test, tas2764->dc_blk[1], 0x00);
}

/* One byte cannot hold both registers: the probe fails */
static void tas2764_dcb_short(struct kunit *test)
{
	struct tas2764_priv *tas2764;
	int ret;

	tas2764 = tas2764_dcb_test_parse(test, "tas2764-kunit-bop-short",
					 DEVID_SN012776, &ret);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_FALSE(test, tas2764->have_dc_blk);
}

/* Not the TAS2764's */
static void tas2764_dcb_not_sn012776(struct kunit *test)
{
	struct tas2764_priv *tas2764;
	int ret;

	tas2764 = tas2764_dcb_test_parse(test, "tas2764-kunit-bop-good",
					 DEVID_TAS2764, &ret);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, tas2764->have_dc_blk);
}

/* The idle slot mask is the row's first four bytes, slot 0 first */
static void tas2764_tx_zd(struct kunit *test)
{
	struct tas2764_priv *tas2764;
	struct device *dev;

	tas2764 = tas2764_test_priv(test, "tas2764-kunit-bop-good", DEVID_SN012776, &dev);
	KUNIT_EXPECT_EQ(test, tas2764_parse_tx_zd_config(dev, tas2764), 0);
	KUNIT_EXPECT_TRUE(test, tas2764->have_tx_zd);
	KUNIT_EXPECT_EQ(test, tas2764->tx_zd_mask, 0x5aa50ff0);

	/* Absent: the sound card's own idle masks apply */
	tas2764 = tas2764_test_priv(test, "tas2764-kunit-bop-none", DEVID_SN012776, &dev);
	KUNIT_EXPECT_EQ(test, tas2764_parse_tx_zd_config(dev, tas2764), 0);
	KUNIT_EXPECT_FALSE(test, tas2764->have_tx_zd);
	KUNIT_EXPECT_EQ(test, tas2764->tx_zd_mask, 0);

	/* Not a whole row: a broken device tree */
	tas2764 = tas2764_test_priv(test, "tas2764-kunit-bop-short", DEVID_SN012776, &dev);
	KUNIT_EXPECT_EQ(test, tas2764_parse_tx_zd_config(dev, tas2764), -EINVAL);
	KUNIT_EXPECT_FALSE(test, tas2764->have_tx_zd);

	/* Not the TAS2764's */
	tas2764 = tas2764_test_priv(test, "tas2764-kunit-bop-good", DEVID_TAS2764, &dev);
	KUNIT_EXPECT_EQ(test, tas2764_parse_tx_zd_config(dev, tas2764), 0);
	KUNIT_EXPECT_FALSE(test, tas2764->have_tx_zd);
}

static struct kunit_case tas2764_bop_test_cases[] = {
	KUNIT_CASE(tas2764_bop_from_dt),
	KUNIT_CASE(tas2764_bop_absent),
	KUNIT_CASE(tas2764_bop_wrong_length),
	KUNIT_CASE(tas2764_bop_not_sn012776),
	KUNIT_CASE(tas2764_dcb_from_dt),
	KUNIT_CASE(tas2764_dcb_absent),
	KUNIT_CASE(tas2764_dcb_short),
	KUNIT_CASE(tas2764_dcb_not_sn012776),
	KUNIT_CASE(tas2764_tx_zd),
	{}
};

/* The test nodes, for each case; the overlay goes again at the case's end */
static int tas2764_bop_test_init(struct kunit *test)
{
	return of_overlay_apply_kunit(test, tas2764_kunit);
}

static struct kunit_suite tas2764_bop_test_suite = {
	.name = "tas2764-bop",
	.init = tas2764_bop_test_init,
	.test_cases = tas2764_bop_test_cases,
};

kunit_test_suite(tas2764_bop_test_suite);
