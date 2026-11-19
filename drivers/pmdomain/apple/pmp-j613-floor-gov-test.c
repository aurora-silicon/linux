// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit cases for the J613 CPU memory-floor governor logic
 * (pmp-j613-floor-gov.h). The host harness tools/asahi/pmp-j613-floor-tests.py
 * covers the same header in more depth, with mutants.
 */
#include <kunit/test.h>

#include "pmp-j613-floor-gov.h"

#define MS PMP_FLOOR_NS_PER_MS

static const struct pmp_floor_gov_cfg defaults = {
	.cpu_busy_pct = 80, .cpu_sum_pct = 90, .gpu_busy_pct = 40, .raise_samples = 2,
	.hold_ms = 500, .min_interval_ms = 100,
};

static void floor_enablement(struct kunit *test)
{
	unsigned int board, native, allowed;

	for (board = 0; board <= 1; board++)
		for (native = 0; native <= 1; native++)
			for (allowed = 0; allowed <= 1; allowed++)
				KUNIT_EXPECT_EQ(test,
					pmp_floor_enabled(board, native, allowed),
					(bool)(board && native && allowed));
}

static void floor_words(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, pmp_floor_word(4, 0), 0x2004000000000000ULL);
	KUNIT_EXPECT_EQ(test, pmp_floor_word(0, 0), PMP_FLOOR_VALID);
	KUNIT_EXPECT_EQ(test, pmp_floor_word(4, 2), 0x2004000200000000ULL);
	KUNIT_EXPECT_TRUE(test, pmp_floor_initial_ok(0));
	KUNIT_EXPECT_TRUE(test, pmp_floor_initial_ok(PMP_FLOOR_VALID));
	KUNIT_EXPECT_FALSE(test, pmp_floor_initial_ok(pmp_floor_word(4, 0)));
	KUNIT_EXPECT_TRUE(test, pmp_floor_raise_confirmed(0x3204, 4, 0));
	KUNIT_EXPECT_FALSE(test, pmp_floor_raise_confirmed(0x3200, 4, 0));
	KUNIT_EXPECT_TRUE(test, pmp_floor_drop_confirmed(0x3200, 0x3204));
	/* J613 ADT: SOC-DEV-DVFS at PTD 0x110, DVFS-STATE at 8. */
	{
		struct pmp_floor_layout l = { .cpu_first = 0x110, .state_first = 8 };

		KUNIT_EXPECT_EQ(test, pmp_floor_req_update(&l), 0x10888ULL);
		KUNIT_EXPECT_EQ(test, pmp_floor_req_read(&l), 0x1110ULL);
		KUNIT_EXPECT_EQ(test, pmp_floor_state_read(&l, 0), 0x80ULL);
		KUNIT_EXPECT_EQ(test, pmp_floor_layout_end(&l), 0x10890ULL);
	}
}

static void floor_busy(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, pmp_floor_cpu_pct(100, 1000, 106, 1030), 80U);
	KUNIT_EXPECT_EQ(test, pmp_floor_cpu_pct(100, 1000, 130, 1030), 0U);
	KUNIT_EXPECT_EQ(test, pmp_floor_cpu_pct(100, 1000, 90, 1030), 0U);
	KUNIT_EXPECT_EQ(test, pmp_floor_busy_pct(20, 10), 100U);
	KUNIT_EXPECT_TRUE(test, pmp_floor_cpu_demand(&defaults, 50, 95));
	KUNIT_EXPECT_FALSE(test, pmp_floor_cpu_demand(&defaults, 20, 80));
}

static void floor_raise_hold_drop(struct kunit *test)
{
	struct pmp_floor_gov g = {};
	unsigned long long t = 30 * MS;
	unsigned int i;

	KUNIT_EXPECT_EQ(test, pmp_floor_gov_step(&g, &defaults, t, true), PMP_FLOOR_KEEP);
	t += 30 * MS;
	KUNIT_EXPECT_EQ(test, pmp_floor_gov_step(&g, &defaults, t, true), PMP_FLOOR_RAISE);
	for (i = 0; i < 16; i++) {
		t += 30 * MS;
		KUNIT_EXPECT_EQ(test, pmp_floor_gov_step(&g, &defaults, t, false), PMP_FLOOR_KEEP);
	}
	t += 30 * MS;
	KUNIT_EXPECT_EQ(test, pmp_floor_gov_step(&g, &defaults, t, false), PMP_FLOOR_DROP);
	KUNIT_EXPECT_FALSE(test, g.raised);
}

static void floor_rate_limit(struct kunit *test)
{
	struct pmp_floor_gov_cfg c = defaults;
	struct pmp_floor_gov g = {};
	unsigned long long t = 0;

	c.raise_samples = 1;
	c.hold_ms = 100;
	c.min_interval_ms = 300;
	t += 10 * MS;
	KUNIT_EXPECT_EQ(test, pmp_floor_gov_step(&g, &c, t, true), PMP_FLOOR_RAISE);
	t += 200 * MS;	/* hold passed, interval not */
	KUNIT_EXPECT_EQ(test, pmp_floor_gov_step(&g, &c, t, false), PMP_FLOOR_KEEP);
	KUNIT_EXPECT_EQ(test, g.rate_limited, 1ULL);
	t += 100 * MS;
	KUNIT_EXPECT_EQ(test, pmp_floor_gov_step(&g, &c, t, false), PMP_FLOOR_DROP);
	pmp_floor_gov_reset(&g, t);
	KUNIT_EXPECT_FALSE(test, g.raised);
}

static void floor_shared_release(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
		pmp_floor_drop_timeout(2000 * MS, 1000 * MS, 0, false, 1000));
	KUNIT_EXPECT_FALSE(test,
		pmp_floor_drop_timeout(9000 * MS, 1000 * MS, 1500 * MS, true, 1000));
	KUNIT_EXPECT_FALSE(test,
		pmp_floor_drop_timeout(2500 * MS, 1000 * MS, 2000 * MS, false, 1000));
	KUNIT_EXPECT_TRUE(test,
		pmp_floor_drop_timeout(3000 * MS, 1000 * MS, 2000 * MS, false, 1000));
	KUNIT_EXPECT_TRUE(test,
		pmp_floor_drop_timeout(2000 * MS, 1000 * MS, 500 * MS, false, 1000));
	KUNIT_EXPECT_FALSE(test,
		pmp_floor_drop_timeout(2000 * MS, 1000 * MS, 2001 * MS, false, 1000));
}

static struct kunit_case pmp_j613_floor_cases[] = {
	KUNIT_CASE(floor_enablement),
	KUNIT_CASE(floor_shared_release),
	KUNIT_CASE(floor_words),
	KUNIT_CASE(floor_busy),
	KUNIT_CASE(floor_raise_hold_drop),
	KUNIT_CASE(floor_rate_limit),
	{}
};

static struct kunit_suite pmp_j613_floor_suite = {
	.name = "pmp-j613-floor-gov",
	.test_cases = pmp_j613_floor_cases,
};
kunit_test_suite(pmp_j613_floor_suite);

MODULE_DESCRIPTION("KUnit tests for the J613 CPU memory-floor governor");
MODULE_LICENSE("Dual MIT/GPL");
