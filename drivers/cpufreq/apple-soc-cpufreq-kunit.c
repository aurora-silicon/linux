// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "apple-soc-cpufreq-internal.h"

static void apple_cpufreq_ps2_preserve_test(struct kunit *test)
{
	u64 reg = 7ULL << 12;
	u64 got;

	got = apple_soc_cpufreq_command(reg, GENMASK_ULL(4, 0), 0, 15, false);
	KUNIT_EXPECT_EQ(test, got & GENMASK_ULL(4, 0), 15ULL);
	KUNIT_EXPECT_EQ(test, got & GENMASK_ULL(15, 12), 7ULL << 12);
	KUNIT_EXPECT_TRUE(test, got & BIT_ULL(25));
}

static void apple_cpufreq_ps2_update_test(struct kunit *test)
{
	u64 reg = 7ULL << 12;
	u64 got;

	got = apple_soc_cpufreq_command(reg, GENMASK_ULL(4, 0), 0, 12, true);
	KUNIT_EXPECT_EQ(test, got & GENMASK_ULL(4, 0), 12ULL);
	KUNIT_EXPECT_EQ(test, got & GENMASK_ULL(15, 12), 12ULL << 12);
}

static void apple_cpufreq_t8112_apsc_rows_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_apsc_row(0x58000, 0x40, 1),
			0x58040U);
	KUNIT_EXPECT_EQ(test, apple_soc_cpufreq_apsc_row(0x58000, 0x40, 14),
			0x58380U);
}

static struct kunit_case apple_cpufreq_cases[] = {
	KUNIT_CASE(apple_cpufreq_ps2_preserve_test),
	KUNIT_CASE(apple_cpufreq_ps2_update_test),
	KUNIT_CASE(apple_cpufreq_t8112_apsc_rows_test),
	{}
};

static struct kunit_suite apple_cpufreq_suite = {
	.name = "apple-soc-cpufreq",
	.test_cases = apple_cpufreq_cases,
};

kunit_test_suite(apple_cpufreq_suite);

MODULE_DESCRIPTION("KUnit tests for Apple SoC CPU DVFS requests");
MODULE_LICENSE("GPL");
