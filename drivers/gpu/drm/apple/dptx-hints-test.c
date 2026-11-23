// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>

#include "dcp-peer-v13_5.h"

static void t602x_no_peer_reply(struct kunit *test)
{
	struct dcp_display_hints_v13_5 request;
	u8 expected[sizeof(request) + 1], response[sizeof(expected)];
	unsigned int enabled;

	for (enabled = 0; enabled <= 1; enabled++) {
		memset(&request, 0x5a, sizeof(request));
		request.hints[0] = enabled;
		memset(response, 0xcc, sizeof(response));

		/* Firmware owns the flags, timing and padding, even on disable. */
		memcpy(expected, &request, sizeof(request));
		memset(expected + 0x60, 0, 20);
		memset(expected + 0x80, 0, 4);
		expected[sizeof(request)] = 0xcc;
		KUNIT_ASSERT_EQ(test, dcp_no_peer_hints_v13_5(&request, response,
							      sizeof(response)), 0);
		KUNIT_EXPECT_MEMEQ(test, response, expected, sizeof(expected));
	}
}

static void t602x_short_reply(struct kunit *test)
{
	struct dcp_display_hints_v13_5 request = {};
	u8 response[sizeof(request) - 1];

	memset(response, 0xcc, sizeof(response));
	KUNIT_EXPECT_EQ(test, dcp_no_peer_hints_v13_5(&request, response,
						      sizeof(response)), -EINVAL);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(response, 0xcc, sizeof(response)), NULL);
}

static void t602x_pipe_bandwidth(struct kunit *test)
{
	struct dcp_dptx_caps caps = dcp_t602x_limits(&(struct dcp_dptx_caps){});

	KUNIT_EXPECT_TRUE(test, dcp_t602x_mode_fits(caps, 2560, 1440, 2720, 1619, 1056750, 1));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_mode_fits(caps, 2560, 1440, 2720, 1726, 1690000, 1));
	KUNIT_EXPECT_TRUE(test, dcp_t602x_mode_fits(caps, 2560, 1440, 2720, 1726, 1690000, 2));
	/* Reduced blanking fits the clock limit but exceeds active-pixel throughput. */
	KUNIT_EXPECT_FALSE(test, dcp_t602x_mode_fits(caps, 2560, 1440, 2640, 1450, 1360000, 1));
	KUNIT_EXPECT_TRUE(test, dcp_t602x_mode_fits(caps, 2560, 1440, 2640, 1450, 1360000, 2));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_mode_fits(caps, 2560, 1440, 0, 1726, 1690000, 2));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_mode_fits(caps, 2560, 1440, 2720, 1726, 3000000, 2));
}

static void t602x_catalogue_clock(struct kunit *test)
{
	u8 timing[T602X_TIMING_SIZE] = {};

	put_unaligned_le32(1690000001, timing + T602X_CLOCK_OFFSET);
	KUNIT_EXPECT_EQ(test, dcp_t602x_wire_clock(timing, sizeof(timing)), 1690000001U);
	KUNIT_EXPECT_EQ(test, dcp_t602x_wire_clock(timing, sizeof(timing) - 1), 0U);
	KUNIT_EXPECT_EQ(test, dcp_t602x_wire_clock(timing, sizeof(timing) + 1), 0U);
}

static void t602x_physical_clock_limits(struct kunit *test)
{
	struct dcp_dptx_caps caps = dcp_t602x_limits(&(struct dcp_dptx_caps){});

	KUNIT_EXPECT_TRUE(test, dcp_t602x_rate_fits(caps, 6144, 1440, 6304, 1500, 567360000, 1));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_rate_fits(caps, 6145, 1440, 6305, 1500, 567450000, 1));
	/* A 1 Hz overflow would be lost in a DRM clock rounded to kHz. */
	KUNIT_EXPECT_TRUE(test, dcp_t602x_rate_fits(caps, 2560, 1440, 4000, 4000,
						    2720000000ULL, 2));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_rate_fits(caps, 2560, 1440, 4000, 4000,
						     2720000001ULL, 2));
	KUNIT_EXPECT_TRUE(test, dcp_t602x_rate_fits(caps, 6144, 3456, 6144, 3456,
						    1274019840ULL, 1));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_rate_fits(caps, 6144, 3456, 6144, 3456,
						     1274019841ULL, 1));
}

static void t602x_reported_limits(struct kunit *test)
{
	struct dcp_dptx_caps caps = {
		.width = 4096, .total_rate = U32_MAX, .active_rate = U32_MAX,
	};

	caps = dcp_t602x_limits(&caps);
	KUNIT_EXPECT_EQ(test, caps.width, 4096U);
	KUNIT_EXPECT_TRUE(test, dcp_t602x_rate_fits(caps, 4096, 2000, 65535, 65535, U32_MAX, 2));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_rate_fits(caps, 4097, 2000, 65535, 65535, U32_MAX, 1));
}

static void t602x_directional_routes(struct kunit *test)
{
	__be32 peers[] = { cpu_to_be32(7), cpu_to_be32(9) };
	struct property routes = {
		.name = "apple,scanout-peers",
		.length = sizeof(peers),
		.value = peers,
	};
	struct device_node source = { .phandle = 2, .properties = &routes };
	struct device_node target = { .phandle = 9 };

	KUNIT_EXPECT_TRUE(test, dcp_t602x_is_peer(&source, &target));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_is_peer(&target, &source));
	target.phandle = 8;
	KUNIT_EXPECT_FALSE(test, dcp_t602x_is_peer(&source, &target));
	target.phandle = 0;
	KUNIT_EXPECT_FALSE(test, dcp_t602x_is_peer(&source, &target));
	source.phandle = 7;
	KUNIT_EXPECT_FALSE(test, dcp_t602x_is_peer(&source, &source));
}

static void t602x_checked_peer(struct kunit *test)
{
	struct dcp_t602x_assignment assignment = { .generation = 9, .peer_index = 3 };

	KUNIT_EXPECT_TRUE(test, dcp_t602x_peer_matches(&assignment, 3, 9));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_peer_matches(&assignment, 4, 9));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_peer_matches(&assignment, 3, 10));
	assignment.peer_index = 0;
	KUNIT_EXPECT_FALSE(test, dcp_t602x_peer_matches(&assignment, 0, 9));
}

static void t602x_topology_match(struct kunit *test)
{
	u32 options[] = { BIT(2) | BIT(3), BIT(2) };
	u32 choice[ARRAY_SIZE(options)] = {};

	/* A greedy allocation would give CRTC 2 to the first output. */
	KUNIT_ASSERT_EQ(test, dcp_t602x_match(options, choice, ARRAY_SIZE(options)), 0);
	KUNIT_EXPECT_EQ(test, choice[0], (u32)BIT(3));
	KUNIT_EXPECT_EQ(test, choice[1], (u32)BIT(2));

	memset(choice, 0, sizeof(choice));
	options[0] = BIT(2);
	KUNIT_EXPECT_EQ(test, dcp_t602x_match(options, choice, ARRAY_SIZE(options)), -ENOSPC);
	KUNIT_EXPECT_EQ(test, choice[0], (u32)0);
	KUNIT_EXPECT_EQ(test, choice[1], (u32)0);

	/* Distinct CRTCs on separate dies can carry the same hardware index. */
	options[0] = BIT(31);
	KUNIT_ASSERT_EQ(test, dcp_t602x_match(options, choice, ARRAY_SIZE(options)), 0);
	KUNIT_EXPECT_EQ(test, choice[0], (u32)BIT(31));
	KUNIT_EXPECT_EQ(test, choice[1], (u32)BIT(2));
}

static void t602x_equal_half_geometry(struct kunit *test)
{
	struct dcp_dptx_caps caps = dcp_t602x_limits(&(struct dcp_dptx_caps){});

	KUNIT_EXPECT_TRUE(test, dcp_t602x_even_horizontal(2560, 2608, 2640, 2720));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_even_horizontal(2561, 2608, 2640, 2720));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_even_horizontal(2560, 2609, 2640, 2720));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_even_horizontal(2560, 2608, 2641, 2720));
	KUNIT_EXPECT_FALSE(test, dcp_t602x_even_horizontal(2560, 2608, 2640, 2721));
	/* Independent odd porches can add up to an even total. */
	KUNIT_EXPECT_FALSE(test, dcp_t602x_even_horizontal(2560, 2609, 2641, 2720));
	/* An odd kHz clock is still independent from horizontal geometry. */
	KUNIT_EXPECT_TRUE(test, dcp_t602x_mode_fits(caps, 2560, 1440, 2720, 1726, 1690001, 2));
}

static struct kunit_case dptx_hints_cases[] = {
	KUNIT_CASE(t602x_topology_match),
	KUNIT_CASE(t602x_checked_peer),
	KUNIT_CASE(t602x_equal_half_geometry),
	KUNIT_CASE(t602x_pipe_bandwidth),
	KUNIT_CASE(t602x_physical_clock_limits),
	KUNIT_CASE(t602x_reported_limits),
	KUNIT_CASE(t602x_catalogue_clock),
	KUNIT_CASE(t602x_directional_routes),
	KUNIT_CASE(t602x_no_peer_reply),
	KUNIT_CASE(t602x_short_reply),
	{}
};

static struct kunit_suite dptx_hints_suite = {
	.name = "apple-dptx-hints",
	.test_cases = dptx_hints_cases,
};

kunit_test_suite(dptx_hints_suite);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Tests for Apple DPTX display allocation replies");
