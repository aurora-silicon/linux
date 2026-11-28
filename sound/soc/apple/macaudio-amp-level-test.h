/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * KUnit tests of the SN012776 and TAS5770L amp levels: each amp's level is the second byte
 * of its apple,speaker-config, which the boot loader copies from the Mac's
 * ADT, and the speakers stay off when any amp lacks it, or under a fixed
 * ceiling when any amp has another level than the ceiling was computed for.
 */
#include <kunit/of.h>
#include <kunit/test.h>

static struct device_node *macaudio_level_test_node(struct kunit *test, const char *name)
{
	struct device_node *np = of_find_node_by_name(NULL, name);

	KUNIT_ASSERT_NOT_NULL(test, np);
	of_node_put_kunit(test, np);
	return np;
}

/* Whether the speakers of the codec node @name would run under @cfg */
static bool macaudio_level_test_present(struct kunit *test, struct device *dev,
					const struct macaudio_platform_cfg *cfg,
					const char *name)
{
	return macaudio_amp_level_count(dev, cfg, macaudio_level_test_node(test, name)) > 0;
}

/* The level is the row's second byte, its low five bits */
static void macaudio_level_from_row(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, macaudio_sn012776_level(
		macaudio_level_test_node(test, "macaudio-kunit-amp-a")), 0x0f);
	KUNIT_EXPECT_EQ(test, macaudio_sn012776_level(
		macaudio_level_test_node(test, "macaudio-kunit-amp-b")), 0x07);
}

/* A level above the amp's range, or no full row, is no level */
static void macaudio_level_rejects(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, macaudio_sn012776_level(
		macaudio_level_test_node(test, "macaudio-kunit-amp-high")), -ERANGE);
	KUNIT_EXPECT_EQ(test, macaudio_sn012776_level(
		macaudio_level_test_node(test, "macaudio-kunit-amp-short")), -ENOENT);
	KUNIT_EXPECT_EQ(test, macaudio_sn012776_level(
		macaudio_level_test_node(test, "macaudio-kunit-amp-none")), -ENOENT);
}

/* The speakers run only when every amp of the link has its level */
static void macaudio_levels_every_amp(struct kunit *test)
{
	struct device *dev = kunit_kzalloc(test, sizeof(*dev), GFP_KERNEL);

	const struct macaudio_platform_cfg *cfg = &macaudio_j316_cfg;

	KUNIT_ASSERT_NOT_NULL(test, dev);
	KUNIT_EXPECT_TRUE(test, macaudio_level_test_present(test, dev, cfg,
							    "macaudio-kunit-codec-good"));
	KUNIT_EXPECT_FALSE(test, macaudio_level_test_present(test, dev, cfg,
							     "macaudio-kunit-codec-one-missing"));
	KUNIT_EXPECT_FALSE(test, macaudio_level_test_present(test, dev, cfg,
							     "macaudio-kunit-codec-one-high"));
}

/* A fixed ceiling, here computed at level 15, holds only at that level */
static const struct macaudio_platform_cfg macaudio_level_ceiling_cfg = {
	.amp = AMP_SN012776,
	.spk_fixed_ceiling = true,
	.spk_ceiling_level = 15,
};

static void macaudio_levels_fixed_ceiling(struct kunit *test)
{
	struct device *dev = kunit_kzalloc(test, sizeof(*dev), GFP_KERNEL);

	const struct macaudio_platform_cfg *cfg = &macaudio_level_ceiling_cfg;

	KUNIT_ASSERT_NOT_NULL(test, dev);
	/* One amp at 0x0f */
	KUNIT_EXPECT_TRUE(test, macaudio_level_test_present(test, dev, cfg,
							    "macaudio-kunit-codec-level-15"));
	/* 0x0f and 0x07: the second amp's level is not the ceiling's */
	KUNIT_EXPECT_FALSE(test, macaudio_level_test_present(test, dev, cfg,
							     "macaudio-kunit-codec-good"));
	KUNIT_EXPECT_FALSE(test, macaudio_level_test_present(test, dev, cfg,
							     "macaudio-kunit-codec-one-missing"));
	KUNIT_EXPECT_FALSE(test, macaudio_amp_level_allowed(cfg, 14));
	KUNIT_EXPECT_FALSE(test, macaudio_amp_level_allowed(cfg, 16));
}

static void macaudio_tas_level_from_row(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, macaudio_tas5770_level(
		macaudio_level_test_node(test, "macaudio-kunit-tas-a")), 4);
	KUNIT_EXPECT_EQ(test, macaudio_tas5770_level(
		macaudio_level_test_node(test, "macaudio-kunit-tas-b")), 14);
}

static void macaudio_tas_level_rejects(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, macaudio_tas5770_level(
		macaudio_level_test_node(test, "macaudio-kunit-tas-zero")), -ERANGE);
	KUNIT_EXPECT_EQ(test, macaudio_tas5770_level(
		macaudio_level_test_node(test, "macaudio-kunit-tas-high")), -ERANGE);
	KUNIT_EXPECT_EQ(test, macaudio_tas5770_level(
		macaudio_level_test_node(test, "macaudio-kunit-tas-short")), -ENOENT);
	KUNIT_EXPECT_EQ(test, macaudio_tas5770_level(
		macaudio_level_test_node(test, "macaudio-kunit-tas-none")), -ENOENT);
}

static void macaudio_tas_levels_every_amp(struct kunit *test)
{
	struct device *dev = kunit_kzalloc(test, sizeof(*dev), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dev);
	KUNIT_EXPECT_TRUE(test, macaudio_level_test_present(test, dev, &macaudio_j313_cfg,
							    "macaudio-kunit-tas-codec-good"));
	KUNIT_EXPECT_FALSE(test, macaudio_level_test_present(test, dev, &macaudio_j313_cfg,
							     "macaudio-kunit-tas-codec-missing"));
	KUNIT_EXPECT_FALSE(test, macaudio_level_test_present(test, dev, &macaudio_j313_cfg,
							     "macaudio-kunit-tas-codec-wrong"));
}

static int macaudio_level_test_init(struct kunit *test)
{
	return of_overlay_apply_kunit(test, macaudio_kunit);
}

static struct kunit_case macaudio_level_cases[] = {
	KUNIT_CASE(macaudio_level_from_row),
	KUNIT_CASE(macaudio_level_rejects),
	KUNIT_CASE(macaudio_levels_every_amp),
	KUNIT_CASE(macaudio_levels_fixed_ceiling),
	KUNIT_CASE(macaudio_tas_level_from_row),
	KUNIT_CASE(macaudio_tas_level_rejects),
	KUNIT_CASE(macaudio_tas_levels_every_amp),
	{}
};

static struct kunit_suite macaudio_level_suite = {
	.name = "apple-speaker-amp-level",
	.init = macaudio_level_test_init,
	.test_cases = macaudio_level_cases,
};

kunit_test_suite(macaudio_level_suite);
