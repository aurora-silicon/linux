// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
#include <kunit/test.h>
#include <linux/module.h>

#include "atc-t8122-dp.h"

static void atc_t8122_dp_test_rates(struct kunit *test)
{
	static const struct {
		unsigned int link_rate;
		u32 count_target;
		bool div2;
	} cases[] = {
		{ 1620, 0x21c, true },
		{ 2700, 0x1c2, false },
		{ 5400, 0x1c2, false },
		{ 8100, 0x2a3, false },
	};
	static const unsigned int unsupported[] = { 0, 1000, 2160, 2430, 3240, 4320, 10000 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct atc_t8122_dp_rate *rate = atc_t8122_dp_rate(cases[i].link_rate);

		KUNIT_ASSERT_NOT_NULL(test, rate);
		KUNIT_EXPECT_EQ(test, rate->link_rate, cases[i].link_rate);
		/* frequency counter target in FREQ_DESC_A[9:0] */
		KUNIT_EXPECT_EQ(test, rate->freq_desc[0] & 0x3ff, cases[i].count_target);
		/* loop coefficients KI 8/3, KP 8/7 in FREQ_DESC_A[29:14] */
		KUNIT_EXPECT_EQ(test, rate->freq_desc[0] & GENMASK(29, 14),
				(8u << 14) | (3u << 18) | (8u << 22) | (7u << 26));
		KUNIT_EXPECT_EQ(test, rate->div2, cases[i].div2);
	}
	/* RBR uses the same descriptor as the T6030 Thunderbolt tunnel clock */
	KUNIT_EXPECT_EQ(test, atc_t8122_dp_rate(1620)->freq_desc[0], 0x1e0e021c);
	KUNIT_EXPECT_EQ(test, atc_t8122_dp_rate(1620)->freq_desc[1], 0);

	for (i = 0; i < ARRAY_SIZE(unsupported); i++)
		KUNIT_EXPECT_NULL(test, atc_t8122_dp_rate(unsupported[i]));
}

static void atc_t8122_dp_test_presets_valid(struct kunit *test)
{
	unsigned int voltage, pre;

	for (voltage = 0; voltage < 5; voltage++) {
		for (pre = 0; pre < 5; pre++) {
			u32 preset = 0xdeadbeef;
			int ret = atc_t8122_dp_preset(voltage, pre, &preset);

			if (voltage > 3 || pre > 3 || voltage + pre > 3) {
				KUNIT_EXPECT_EQ(test, ret, -EINVAL);
				KUNIT_EXPECT_EQ(test, preset, 0xdeadbeef);
			} else {
				KUNIT_EXPECT_EQ(test, ret, 0);
				KUNIT_EXPECT_NE(test, preset, 0);
				KUNIT_EXPECT_EQ(test, preset & ~GENMASK(23, 0), 0);
			}
		}
	}
}

static void atc_t8122_dp_test_presets_decode(struct kunit *test)
{
	u32 preset;

	KUNIT_ASSERT_EQ(test, atc_t8122_dp_preset(0, 0, &preset), 0);
	/* swing 9, swing LSBs 0 */
	KUNIT_EXPECT_EQ(test, atc_t8122_dp_preset_swing(preset), 9);
	/* the lowest level matches the RX-as-TX block's initial de-emphasis */
	KUNIT_EXPECT_EQ(test, atc_t8122_dp_preset_deemph(preset), ATC_T8122_RX_EQ17_INIT);

	KUNIT_ASSERT_EQ(test, atc_t8122_dp_preset(0, 1, &preset), 0);
	KUNIT_EXPECT_EQ(test, atc_t8122_dp_preset_swing(preset), (3u << 14) | 4);
	KUNIT_EXPECT_EQ(test, atc_t8122_dp_preset_deemph(preset), (0x8dc0u << 1) | 1);

	/* every decoded value fits its register field */
	for (unsigned int v = 0; v < 4; v++) {
		for (unsigned int p = 0; v + p < 4; p++) {
			KUNIT_ASSERT_EQ(test, atc_t8122_dp_preset(v, p, &preset), 0);
			KUNIT_EXPECT_EQ(test, atc_t8122_dp_preset_swing(preset) &
					~(ATC_T8122_EQ1_SWING | ATC_T8122_EQ1_SWING_LSB), 0);
			KUNIT_EXPECT_EQ(test, atc_t8122_dp_preset_deemph(preset) &
					~ATC_T8122_EQ17_DEEMPH, 0);
		}
	}
}

static void atc_t8122_dp_test_lanes(struct kunit *test)
{
	static const struct {
		u8 pairs;
		unsigned int lane;
		int ret;
		unsigned int pair;
		bool tx;
	} cases[] = {
		/* pin D, normal orientation: DP on lane pair 1 */
		{ 2, 0, 0, 1, false },
		{ 2, 1, 0, 1, true },
		{ 2, 2, -EINVAL },
		/* pin D, flipped: DP on lane pair 0 */
		{ 1, 0, 0, 0, false },
		{ 1, 1, 0, 0, true },
		{ 1, 2, -EINVAL },
		/* pin C/E and the HDMI converter: four lanes over both pairs */
		{ 3, 0, 0, 0, false },
		{ 3, 1, 0, 0, true },
		{ 3, 2, 0, 1, false },
		{ 3, 3, 0, 1, true },
		{ 3, 4, -EINVAL },
		/* no DP lanes */
		{ 0, 0, -EINVAL },
		{ 4, 0, -EINVAL },
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
		unsigned int pair = 99;
		bool tx = false;
		int ret = atc_t8122_dp_lane_map(cases[i].pairs, cases[i].lane, &pair, &tx);

		KUNIT_EXPECT_EQ_MSG(test, ret, cases[i].ret, "case %u", i);
		if (ret)
			continue;
		KUNIT_EXPECT_EQ_MSG(test, pair, cases[i].pair, "case %u", i);
		KUNIT_EXPECT_EQ_MSG(test, tx, cases[i].tx, "case %u", i);
	}
}

static struct kunit_case atc_t8122_dp_test_cases[] = {
	KUNIT_CASE(atc_t8122_dp_test_rates),
	KUNIT_CASE(atc_t8122_dp_test_presets_valid),
	KUNIT_CASE(atc_t8122_dp_test_presets_decode),
	KUNIT_CASE(atc_t8122_dp_test_lanes),
	{}
};

static struct kunit_suite atc_t8122_dp_test_suite = {
	.name = "apple-atc-t8122-dp",
	.test_cases = atc_t8122_dp_test_cases,
};
kunit_test_suite(atc_t8122_dp_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple ATC T8122 DisplayPort helpers");
MODULE_LICENSE("Dual BSD/GPL");
