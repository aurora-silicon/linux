// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
#include <kunit/test.h>
#include <linux/soc/apple/dp-tunnel.h>

#include "atc-tunnel.h"

static void atc_tunnel_soc_qualification(struct kunit *test)
{
	const struct {
		const char *soc;
		bool qualified;
	} cases[] = {
		{ "apple,t6020", true },
		{ "apple,t6021", true },
		{ "apple,t6022", false },
		{ "apple,t8103", false },
		{ "apple,t6000", false },
		{ "apple,t6001", false },
		{ "apple,t6002", false },
		{ "apple,t8112", false },
		{ "apple,t6030", false },
		{ "apple,t6031", false },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct property compatible = {
			.name = "compatible", .value = (void *)cases[i].soc,
			.length = strlen(cases[i].soc) + 1,
		};
		struct device_node root = { .properties = &compatible };

		KUNIT_EXPECT_EQ(test, apple_atc_t602x_qualified(&root), cases[i].qualified);
	}
	KUNIT_EXPECT_FALSE(test, apple_atc_t602x_qualified(NULL));
}

static void atc_tunnel_core_qualification(struct kunit *test)
{
	const struct {
		const char *compatible;
		u64 base;
		u64 size;
		bool valid;
	} cases[] = {
		{ "apple,t6020-atcphy", 0x703000000ULL, 0x7048, true },
		{ "apple,t6020-atcphy", 0xb03000000ULL, 0x7048, true },
		{ "apple,t6020-atcphy", 0xf03000000ULL, 0x7048, true },
		{ "apple,t6020-atcphy", 0x703000000ULL, 0x7047, false },
		{ "apple,t6020-atcphy", 0x1303000000ULL, 0x4c000, false },
		{ "apple,t6020-atcphy", 0x2703000000ULL, 0x4c000, false },
		{ "apple,t8103-atcphy", 0x703000000ULL, 0x4c000, false },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct property compatible = {
			.name = "compatible", .value = (void *)cases[i].compatible,
			.length = strlen(cases[i].compatible) + 1,
		};
		struct device_node np = { .properties = &compatible };

		KUNIT_EXPECT_EQ(test,
				apple_atc_t602x_core_valid(&np, cases[i].base, cases[i].size),
				cases[i].valid);
	}
	KUNIT_EXPECT_FALSE(test, apple_atc_t602x_core_valid(NULL, 0x703000000ULL, 0x7048));
}

static void atc_tunnel_wiring_gate(struct kunit *test)
{
	const struct {
		bool own;
		bool present;
		bool valid;
		int expected;
	} cases[] = {
		{ true, true, true, 0 },
		{ false, true, true, -EOPNOTSUPP },
		{ false, true, false, -EINVAL },
		{ true, true, false, -EINVAL },
		{ false, false, true, -EOPNOTSUPP },
		{ false, false, false, -EOPNOTSUPP },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		KUNIT_EXPECT_EQ(test,
				apple_atc_t602x_gate(cases[i].own, cases[i].present,
						     cases[i].valid),
				cases[i].expected);
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
	KUNIT_CASE(atc_tunnel_soc_qualification),
	KUNIT_CASE(atc_tunnel_core_qualification),
	KUNIT_CASE(atc_tunnel_wiring_gate),
	{},
};

static struct kunit_suite atc_tunnel_suite = {
	.name = "apple-atc-tunnel",
	.test_cases = atc_tunnel_cases,
};

kunit_test_suite(atc_tunnel_suite);
