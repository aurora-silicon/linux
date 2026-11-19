// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
#include <kunit/test.h>
#include <linux/soc/apple/dp-tunnel.h>

#include "atc-tunnel.h"

/* Only the per-DP IN clock PHY, and only on a connector wired to both DP INs. */
static void atc_tunnel_dpin_clock_gate(struct kunit *test)
{
	const struct {
		bool dpin_clocks;
		bool dual_stream;
		int expected;
	} cases[] = {
		{ true, true, 0 },
		{ true, false, -EOPNOTSUPP },
		{ false, true, -EOPNOTSUPP },
		{ false, false, -EOPNOTSUPP },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		KUNIT_EXPECT_EQ(test,
				apple_atc_dpin_clock_gate(cases[i].dpin_clocks,
							  cases[i].dual_stream),
				cases[i].expected);
}

/* Which tunnel clock a PHY has follows its compatible's match data. */
static void atc_tunnel_clock_by_compatible(struct kunit *test)
{
	const struct {
		const char *compatible;
		bool t8103_clock;
		bool dpin_clocks;
		bool t8122_clock;
	} cases[] = {
		{ "apple,t8103-atcphy", true, false, false },
		{ "apple,t6000-atcphy", true, false, false },
		{ "apple,t6020-atcphy", false, true, false },
		{ "apple,t6030-atcphy", false, false, true },
		{ "apple,t8122-atcphy", false, false, false },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct of_device_id *id;
		const struct atcphy_hw *hw = NULL;

		for (id = atcphy_match; id->compatible[0]; id++) {
			if (!strcmp(id->compatible, cases[i].compatible)) {
				hw = id->data;
				break;
			}
		}
		KUNIT_ASSERT_NOT_NULL_MSG(test, hw, "%s", cases[i].compatible);
		KUNIT_EXPECT_EQ_MSG(test, hw->tunnel_t8103_clock, cases[i].t8103_clock,
				    "%s", cases[i].compatible);
		KUNIT_EXPECT_EQ_MSG(test, hw->tunnel_dpin_clocks, cases[i].dpin_clocks,
				    "%s", cases[i].compatible);
		KUNIT_EXPECT_EQ_MSG(test, hw->t8122_tunnel_clock, cases[i].t8122_clock,
				    "%s", cases[i].compatible);
	}
}

static void atc_tunnel_t6030_clock_sources(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t6030_pclk(0, 0), 0);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t6030_pclk(2, 0), 0);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t6030_pclk(0, 1), 1);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t6030_pclk(2, 1), 1);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t6030_pclk(1, 0), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t6030_pclk(4, 0), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t6030_pclk(0, 2), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t6030_pclk(~0U, 0), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t6030_pclk(0, ~0U), -EOPNOTSUPP);
}

static void atc_tunnel_t8132_clock_sources(struct kunit *test)
{
	/* dcpext0 (2) and dcpext1 (4): PCLK1 for DP IN 0, PCLK2 for DP IN 1 */
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_pclk(2, 0), 0);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_pclk(4, 0), 0);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_pclk(2, 1), 1);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_pclk(4, 1), 1);
	/* No other source, and no third DP IN */
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_pclk(0, 0), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_pclk(3, 0), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_pclk(2, 2), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_pclk(~0U, 0), -EOPNOTSUPP);
}

static void atc_tunnel_pclk_gates(struct kunit *test)
{
	/* DP IN 0 alone: PCLK1 */
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_pclks(BIT(0)), APPLE_DP_TUNNEL_PCLK1);
	/* DP IN 1 alone: PCLK2, and PCLK1 at its rate */
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_pclks(BIT(1)),
			APPLE_DP_TUNNEL_PCLK1 | APPLE_DP_TUNNEL_PCLK2);
	/* both: both */
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_pclks(BIT(0) | BIT(1)),
			APPLE_DP_TUNNEL_PCLK1 | APPLE_DP_TUNNEL_PCLK2);
	/* DP IN 0 released with DP IN 1 up: PCLK1 stays */
	KUNIT_EXPECT_TRUE(test, apple_dp_tunnel_pclks((BIT(0) | BIT(1)) & ~BIT(0)) &
			  APPLE_DP_TUNNEL_PCLK1);
	/* DP IN 1 released with DP IN 0 up: PCLK2 goes, PCLK1 stays */
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_pclks((BIT(0) | BIT(1)) & ~BIT(1)),
			APPLE_DP_TUNNEL_PCLK1);
	/* all released: neither */
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_pclks(0), 0U);
}

static void atc_tunnel_t8132_one_rate(struct kunit *test)
{
	/* HBR2 (0x14) next to HBR2, or alone: allowed */
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_rate_ok(0x14, 0x14), 0);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_rate_ok(0x14, 0), 0);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_rate_ok(0x0a, 0), 0);
	/* a stop is never refused */
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_rate_ok(0, 0x14), 0);
	/* another rate next to a running stream: refused */
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_rate_ok(0x0a, 0x14), -EBUSY);
	KUNIT_EXPECT_EQ(test, apple_dp_tunnel_t8132_rate_ok(0x1e, 0x0a), -EBUSY);
}

static struct kunit_case atc_tunnel_cases[] = {
	KUNIT_CASE(atc_tunnel_t6030_clock_sources),
	KUNIT_CASE(atc_tunnel_t8132_clock_sources),
	KUNIT_CASE(atc_tunnel_t8132_one_rate),
	KUNIT_CASE(atc_tunnel_pclk_gates),
	KUNIT_CASE(atc_tunnel_dpin_clock_gate),
	KUNIT_CASE(atc_tunnel_clock_by_compatible),
	{},
};

static struct kunit_suite atc_tunnel_suite = {
	.name = "apple-atc-tunnel",
	.test_cases = atc_tunnel_cases,
};

kunit_test_suite(atc_tunnel_suite);
