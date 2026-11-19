// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/soc/apple/pmp-cooling.h>
#include <linux/soc/apple/pmp-power-policy.h>
#include "apple-pmp-confirm.h"

static void apple_pmp_test_fault_recovery(struct kunit *test)
{
	struct apple_pmp_cooling state;
	unsigned int i;

	apple_pmp_cooling_init(&state);
	for (i = 0; i < 100; i++)
		apple_pmp_cooling_sample(&state, 20000000, 50000, 0);
	KUNIT_EXPECT_TRUE(test, state.hard);
	KUNIT_EXPECT_EQ(test, state.good_reads, 1U);
	for (i = 2; i <= 10; i++)
		apple_pmp_cooling_sample(&state, i * 20000000ULL, 50000, 0);
	KUNIT_EXPECT_FALSE(test, state.hard);
	KUNIT_EXPECT_EQ(test, state.level, 8U);
	apple_pmp_cooling_sample(&state, 400000000, 50000, 0);
	KUNIT_EXPECT_EQ(test, state.level, 7U);
	apple_pmp_cooling_sample(&state, 405000000, 50000, -EIO);
	KUNIT_EXPECT_TRUE(test, state.hard);
	KUNIT_EXPECT_EQ(test, state.level, 8U);
}

static void apple_pmp_test_poll_cadence(struct kunit *test)
{
	struct apple_pmp_cooling state;
	unsigned int i;

	apple_pmp_cooling_init(&state);
	KUNIT_EXPECT_TRUE(test, apple_pmp_cooling_fast(&state, 50000, 0));
	for (i = 1; i <= 10; i++)
		apple_pmp_cooling_sample(&state, i * 20000000ULL, 50000, 0);
	KUNIT_EXPECT_FALSE(test, state.hard);
	/* Recovery still holds a cooling level until it has been released. */
	KUNIT_EXPECT_TRUE(test, apple_pmp_cooling_fast(&state, 50000, 0));
	for (i = 0; i < 40 && state.level; i++)
		apple_pmp_cooling_sample(&state, 400000000ULL + i * 100000000ULL,
					 50000, 0);
	KUNIT_EXPECT_EQ(test, state.level, 0U);
	KUNIT_EXPECT_FALSE(test, apple_pmp_cooling_fast(&state, 50000, 0));
	KUNIT_EXPECT_FALSE(test, apple_pmp_cooling_fast(&state, APPLE_PMP_FAST_POLL_MC - 1, 0));
	KUNIT_EXPECT_TRUE(test, apple_pmp_cooling_fast(&state, APPLE_PMP_FAST_POLL_MC, 0));
	KUNIT_EXPECT_TRUE(test, apple_pmp_cooling_fast(&state, 50000, -EIO));
}

static void apple_pmp_test_critical_confirmation(struct kunit *test)
{
	struct apple_pmp_confirmation state = {};
	unsigned int i;

	for (i = 0; i < 100; i++)
		apple_pmp_confirmation_sample(&state, APPLE_PMP_CRITICAL_MC, 0, 10000000);
	KUNIT_EXPECT_EQ(test, state.count, 1U);
	KUNIT_EXPECT_FALSE(test, state.confirmed);
	apple_pmp_confirmation_sample(&state, APPLE_PMP_CRITICAL_MC, -EIO, 15000000);
	KUNIT_EXPECT_EQ(test, state.count, 0U);
	for (i = 0; i < 3; i++)
		apple_pmp_confirmation_sample(&state, APPLE_PMP_CRITICAL_MC, 0,
					      20000000 + i * 5000000ULL);
	KUNIT_EXPECT_TRUE(test, state.confirmed);
	apple_pmp_confirmation_sample(&state, 0, -EIO, 40000000);
	KUNIT_EXPECT_TRUE(test, state.confirmed);
}

static void apple_pmp_test_power_validation(struct kunit *test)
{
	struct apple_pmp_power_policy state;
	u32 mw;
	u32 rails[3] = { 4000, 1000, 0 };

	KUNIT_EXPECT_TRUE(test, apple_pmp_power_decode(0x3f800000, &mw));
	KUNIT_EXPECT_EQ(test, mw, 1000U);
	KUNIT_EXPECT_FALSE(test, apple_pmp_power_decode(0x7f800000, &mw));
	KUNIT_EXPECT_FALSE(test, apple_pmp_power_decode(0xbf800000, &mw));
	KUNIT_EXPECT_FALSE(test, apple_pmp_power_decode(0x42500000, &mw));
	apple_pmp_power_init(&state);
	apple_pmp_power_sample(&state, 500000000, rails, 60000, -EIO);
	KUNIT_EXPECT_TRUE(test, state.fault);
	KUNIT_EXPECT_EQ(test, state.level[0], 8U);
	KUNIT_EXPECT_EQ(test, state.level[1], 8U);
}

static void apple_pmp_test_gpu_budget_freshness(struct kunit *test)
{
	u64 now = 10000000000ULL;
	u64 publication = apple_pmp_gpu_budget_pack(3, now);

	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(0, now), 8U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(publication, now), 3U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(publication,
				 now + APPLE_PMP_POWER_STALE_NS), 3U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(publication,
				 now + APPLE_PMP_POWER_STALE_NS + 1000000), 8U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(publication, now - 1000000), 8U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(publication | 15, now), 8U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(
			 apple_pmp_gpu_budget_pack(0, now), now), 0U);
	KUNIT_EXPECT_EQ(test, apple_pmp_gpu_budget_level(
			 apple_pmp_gpu_budget_pack(100, now), now), 8U);
}

static void apple_pmp_test_shared_power_arbitration(struct kunit *test)
{
	struct apple_pmp_power_policy state;
	u32 rails[3] = {2000, 500, 12000};
	u32 idle[3] = {2000, 500, 500};

	apple_pmp_power_init(&state);
	/* A confirmed, warm, exhausted-burst policy with all domains released. */
	for (unsigned int i = 1; i <= 3; i++)
		apple_pmp_power_sample(&state, i * 500000000ULL, idle, 85000, 0);
	state.level[0] = state.level[1] = state.level[2] = 0;
	apple_pmp_power_sample(&state, 2500000000ULL, rails, 85000, 0);
	KUNIT_EXPECT_EQ(test, state.total_mw, 14500);
	/* GPU is the largest measured consumer even with its existing 4x weight. */
	KUNIT_EXPECT_EQ(test, state.level[0], 0U);
	KUNIT_EXPECT_EQ(test, state.level[1], 0U);
	KUNIT_EXPECT_EQ(test, state.level[2], 1U);
	/* Repeated observations cannot buy another control step. */
	apple_pmp_power_sample(&state, 2500000000ULL, idle, 85000, 0);
	KUNIT_EXPECT_EQ(test, state.level[2], 1U);
	apple_pmp_power_sample(&state, 3500000000ULL, idle, 85000, 0);
	KUNIT_EXPECT_EQ(test, state.level[2], 0U);
	apple_pmp_power_sample(&state, 4000000000ULL, idle, 85000, -EIO);
	KUNIT_EXPECT_TRUE(test, state.fault);
	KUNIT_EXPECT_EQ(test, state.level[2], 8U);
}

static void apple_pmp_test_gpu_attachment(struct kunit *test)
{
	struct apple_pmp_power_policy state;
	u32 rails[3] = {2000, 500, 12000};

	apple_pmp_power_init(&state);
	apple_pmp_power_set_active(&state, 3);
	KUNIT_EXPECT_EQ(test, state.level[2], 0U);
	for (unsigned int i = 1; i <= 3; i++)
		apple_pmp_power_sample(&state, i * 500000000ULL, rails, 85000, 0);
	KUNIT_EXPECT_EQ(test, state.total_mw, 2500);
	KUNIT_EXPECT_EQ(test, state.level[2], 0U);
	apple_pmp_power_set_active(&state, 7);
	KUNIT_EXPECT_EQ(test, state.level[2], 8U);
	apple_pmp_power_sample(&state, 2000000000ULL, rails, 85000, 0);
	KUNIT_EXPECT_EQ(test, state.total_mw, 14500);
	apple_pmp_power_set_active(&state, 3);
	apple_pmp_power_sample(&state, 2500000000ULL, rails, 85000, -EIO);
	KUNIT_EXPECT_EQ(test, state.level[0], 8U);
	KUNIT_EXPECT_EQ(test, state.level[1], 8U);
	KUNIT_EXPECT_EQ(test, state.level[2], 0U);
}

static struct kunit_case apple_pmp_policy_cases[] = {
	KUNIT_CASE(apple_pmp_test_fault_recovery),
	KUNIT_CASE(apple_pmp_test_poll_cadence),
	KUNIT_CASE(apple_pmp_test_critical_confirmation),
	KUNIT_CASE(apple_pmp_test_power_validation),
	KUNIT_CASE(apple_pmp_test_gpu_budget_freshness),
	KUNIT_CASE(apple_pmp_test_shared_power_arbitration),
	KUNIT_CASE(apple_pmp_test_gpu_attachment),
	{},
};

static struct kunit_suite apple_pmp_policy_suite = {
	.name = "apple-pmp-policy",
	.test_cases = apple_pmp_policy_cases,
};
kunit_test_suite(apple_pmp_policy_suite);
MODULE_DESCRIPTION("KUnit tests for the Apple PMP thermal policy");
MODULE_LICENSE("Dual MIT/GPL");
