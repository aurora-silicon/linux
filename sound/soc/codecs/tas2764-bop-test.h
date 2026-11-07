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

/* Every register the driver or its quirk sequences write is writeable */
static void tas2764_writeable_covers_every_write(struct kunit *test)
{
	unsigned int i, j, reg;

	for (i = 0; i < ARRAY_SIZE(tas2764_quirk_init_sequences); i++) {
		const struct tas2764_quirk_init_sequence *q = &tas2764_quirk_init_sequences[i];

		for (j = 0; q->seq && j < q->len; j++) {
			reg = q->seq[j].reg;
			KUNIT_EXPECT_TRUE_MSG(test, tas2764_writeable_register(NULL, reg),
					      "quirk %u reg 0x%x", i, reg);
		}
	}
	for (i = 0; i < ARRAY_SIZE(tas2764_pre_shutdown_seq); i++) {
		reg = tas2764_pre_shutdown_seq[i].reg;
		KUNIT_EXPECT_TRUE(test, tas2764_writeable_register(NULL, reg));
	}
	for (i = 0; i < ARRAY_SIZE(tas2764_post_shutdown_seq); i++) {
		reg = tas2764_post_shutdown_seq[i].reg;
		KUNIT_EXPECT_TRUE(test, tas2764_writeable_register(NULL, reg));
	}
	for (i = 0; i < TAS2764_BOP_CFG_LEN; i++)
		KUNIT_EXPECT_TRUE(test, tas2764_writeable_register(NULL, TAS2764_BOP_CFG0 + i));
	KUNIT_EXPECT_TRUE(test, tas2764_writeable_register(NULL, TAS2764_PAGE));
	KUNIT_EXPECT_TRUE(test, tas2764_writeable_register(NULL, TAS2764_PWR_CTRL));
	KUNIT_EXPECT_TRUE(test, tas2764_writeable_register(NULL, TAS2764_DVC));
	KUNIT_EXPECT_TRUE(test, tas2764_writeable_register(NULL, TAS2764_SDOUT_HIZ_9));
}

/*
 * What only a debugfs dump caches is never written back: the reserved pages
 * between the page-1 quirk registers and the page-6 ones (the 0xb3..0x313 raw
 * block that stuck the speaker buses at resume), and the rest of pages 1 and 6.
 */
static void tas2764_sync_skips_read_only_cache(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, tas2764_writeable_register(NULL, TAS2764_REG(0x1, 0x34)));
	KUNIT_EXPECT_FALSE(test, tas2764_writeable_register(NULL, TAS2764_REG(0x1, 0x7f)));
	KUNIT_EXPECT_FALSE(test, tas2764_writeable_register(NULL, TAS2764_REG(0x2, 0x00)));
	KUNIT_EXPECT_FALSE(test, tas2764_writeable_register(NULL, TAS2764_REG(0x4, 0x40)));
	KUNIT_EXPECT_FALSE(test, tas2764_writeable_register(NULL, TAS2764_REG(0x6, 0x13)));
	KUNIT_EXPECT_FALSE(test, tas2764_writeable_register(NULL, TAS2764_REG(0x6, 0x20)));
	KUNIT_EXPECT_FALSE(test, tas2764_writeable_register(NULL, TAS2764_REG(0x10, 0x00)));
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
	KUNIT_CASE(tas2764_writeable_covers_every_write),
	KUNIT_CASE(tas2764_sync_skips_read_only_cache),
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
