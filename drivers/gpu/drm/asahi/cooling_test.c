// SPDX-License-Identifier: GPL-2.0-only OR MIT

/* Build this same production-policy suite with ASAHI_HOST_TEST or KUnit. */
#ifdef ASAHI_HOST_TEST
#include <assert.h>
#include <limits.h>
#include <stdio.h>
struct kunit { int unused; };
#define KUNIT_EXPECT_EQ(test, a, b) do { (void)(test); assert((a) == (b)); } while (0)
#define KUNIT_EXPECT_LT(test, a, b) do { (void)(test); assert((a) < (b)); } while (0)
#else
#include <kunit/test.h>
#include <linux/limits.h>
#include <linux/module.h>
#endif
#include "cooling_policy.h"

struct fake_actuator {
	unsigned int region_c_mw;
	unsigned int ppm_target_mw;
	unsigned int ppm_budget_mw;
	unsigned int calls;
	int error;
};

static int fake_publish(void *context, unsigned int mw)
{
	struct fake_actuator *fake = context;

	fake->calls++;
	if (fake->error)
		return fake->error; /* Fail before either store. */
	fake->region_c_mw = mw;
	fake->ppm_target_mw = mw < fake->ppm_budget_mw ? mw : fake->ppm_budget_mw;
	return 0;
}

static void cap_mapping(struct kunit *test)
{
	struct asahi_gpu_cap cap;
	struct fake_actuator fake = { .ppm_budget_mw = 9750 };
	unsigned long state;
	unsigned int previous = 9550;

	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_init(&cap, 9550), 0);
	for (state = 0; state <= 100; state++) {
		unsigned int expected = 9550 * (100 - state) / 100;

		if (expected < 2387)
			expected = 2387;
		KUNIT_EXPECT_EQ(test, asahi_gpu_cap_update(&cap, state, fake_publish, &fake), 0);
		KUNIT_EXPECT_EQ(test, cap.applied_mw, expected);
		KUNIT_EXPECT_EQ(test, fake.region_c_mw, expected);
		KUNIT_EXPECT_EQ(test, fake.ppm_target_mw, expected);
		KUNIT_EXPECT_EQ(test, cap.applied_mw <= previous, 1);
		previous = cap.applied_mw;
	}
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_percent(100), 0);
	KUNIT_EXPECT_LT(test, asahi_gpu_cap_percent(101), 0);
	KUNIT_EXPECT_LT(test, asahi_gpu_cap_percent(ULONG_MAX), 0);
}

static void cap_composition(struct kunit *test)
{
	struct asahi_gpu_cap cap;
	struct fake_actuator fake = { .ppm_budget_mw = 10000 };
	unsigned int a, b, source;

	/* Every demand pair, for every non-thermal requester, in both orders. */
	for (source = ASAHI_GPU_CAP_FAST_DIE; source < ASAHI_GPU_CAP_SOURCES; source++) {
		for (a = 25; a <= 100; a++) {
			for (b = 25; b <= 100; b++) {
				unsigned int expected = (a < b ? a : b) * 100;

				asahi_gpu_cap_init(&cap, 10000);
				KUNIT_EXPECT_EQ(test, asahi_gpu_cap_request(&cap, source, b * 100, fake_publish, &fake), 0);
				KUNIT_EXPECT_EQ(test, cap.applied_mw, 2500U);
				KUNIT_EXPECT_EQ(test, asahi_gpu_cap_update(&cap, 100 - a, fake_publish, &fake), 0);
				KUNIT_EXPECT_EQ(test, cap.applied_mw, expected);
				asahi_gpu_cap_init(&cap, 10000);
				asahi_gpu_cap_update(&cap, 100 - a, fake_publish, &fake);
				asahi_gpu_cap_request(&cap, source, b * 100, fake_publish, &fake);
				KUNIT_EXPECT_EQ(test, cap.applied_mw, expected);
			}
		}
	}
	/* All four owners remain independent when one relaxes. */
	asahi_gpu_cap_init(&cap, 10000);
	asahi_gpu_cap_update(&cap, 0, fake_publish, &fake);
	asahi_gpu_cap_request(&cap, ASAHI_GPU_CAP_FAST_DIE, 3000, fake_publish, &fake);
	asahi_gpu_cap_request(&cap, ASAHI_GPU_CAP_SYSTEM, 5000, fake_publish, &fake);
	asahi_gpu_cap_request(&cap, ASAHI_GPU_CAP_MANUAL, 4000, fake_publish, &fake);
	KUNIT_EXPECT_EQ(test, cap.applied_mw, 3000U);
	asahi_gpu_cap_request(&cap, ASAHI_GPU_CAP_FAST_DIE, 10000, fake_publish, &fake);
	KUNIT_EXPECT_EQ(test, cap.applied_mw, 4000U);
	asahi_gpu_cap_update(&cap, 1, fake_publish, &fake);
	KUNIT_EXPECT_EQ(test, cap.applied_mw, 4000U);
	asahi_gpu_cap_request(&cap, ASAHI_GPU_CAP_MANUAL, 10000, fake_publish, &fake);
	KUNIT_EXPECT_EQ(test, cap.applied_mw, 5000U);
}

static void cap_startup_and_ppm(struct kunit *test)
{
	struct asahi_gpu_cap cap;
	struct fake_actuator fake = { .ppm_budget_mw = 7000 };

	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_init(&cap, 9550), 0);
	KUNIT_EXPECT_EQ(test, cap.applied_mw, 0U);
	KUNIT_EXPECT_EQ(test, cap.requested_state, 100U);
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_update(&cap, 100, fake_publish, &fake), 0);
	KUNIT_EXPECT_EQ(test, fake.region_c_mw, 2387U);
	asahi_gpu_cap_update(&cap, 0, fake_publish, &fake);
	KUNIT_EXPECT_EQ(test, fake.region_c_mw, 9550U);
	KUNIT_EXPECT_EQ(test, fake.ppm_target_mw, 7000U);
	KUNIT_EXPECT_EQ(test, fake.ppm_budget_mw, 7000U);
}

static void cap_failure_and_invalid(struct kunit *test)
{
	struct asahi_gpu_cap cap;
	struct fake_actuator fake = { .ppm_budget_mw = 10000 };
	unsigned int calls;

	asahi_gpu_cap_init(&cap, 10000);
	asahi_gpu_cap_update(&cap, 70, fake_publish, &fake);
	fake.error = -5;
	/* Failed relaxation and failed tightening both keep the old restriction. */
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_update(&cap, 0, fake_publish, &fake), -5);
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_update(&cap, 100, fake_publish, &fake), -5);
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_request(&cap, ASAHI_GPU_CAP_MANUAL, 2500, fake_publish, &fake), -5);
	KUNIT_EXPECT_EQ(test, cap.requested_state, 70U);
	KUNIT_EXPECT_EQ(test, cap.applied_mw, 3000U);
	KUNIT_EXPECT_EQ(test, cap.requests[ASAHI_GPU_CAP_MANUAL], 10000U);
	KUNIT_EXPECT_EQ(test, fake.region_c_mw, 3000U);
	KUNIT_EXPECT_EQ(test, fake.ppm_target_mw, 3000U);
	calls = fake.calls;
	KUNIT_EXPECT_LT(test, asahi_gpu_cap_update(&cap, ULONG_MAX, fake_publish, &fake), 0);
	KUNIT_EXPECT_LT(test, asahi_gpu_cap_request(&cap, ASAHI_GPU_CAP_SOURCES, 3000, fake_publish, &fake), 0);
	KUNIT_EXPECT_LT(test, asahi_gpu_cap_request(&cap, ASAHI_GPU_CAP_SYSTEM, 0, fake_publish, &fake), 0);
	KUNIT_EXPECT_LT(test, asahi_gpu_cap_request(&cap, ASAHI_GPU_CAP_SYSTEM, 2499, fake_publish, &fake), 0);
	KUNIT_EXPECT_LT(test, asahi_gpu_cap_request(&cap, ASAHI_GPU_CAP_SYSTEM, 10001, fake_publish, &fake), 0);
	KUNIT_EXPECT_EQ(test, fake.calls, calls);
	KUNIT_EXPECT_EQ(test, cap.applied_mw, 3000U);
}

static void cap_recovery_and_limits(struct kunit *test)
{
	struct asahi_gpu_cap cap;
	struct fake_actuator fake = { .ppm_budget_mw = UINT_MAX };

	KUNIT_EXPECT_LT(test, asahi_gpu_cap_init(&cap, 0), 0);
	asahi_gpu_cap_init(&cap, 1);
	asahi_gpu_cap_update(&cap, 100, fake_publish, &fake);
	KUNIT_EXPECT_EQ(test, cap.applied_mw, 1U);
	asahi_gpu_cap_init(&cap, UINT_MAX);
	asahi_gpu_cap_update(&cap, 0, fake_publish, &fake);
	KUNIT_EXPECT_EQ(test, cap.applied_mw, UINT_MAX);
	asahi_gpu_cap_init(&cap, 10000);
	asahi_gpu_cap_update(&cap, 0, fake_publish, &fake);
	asahi_gpu_cap_request(&cap, ASAHI_GPU_CAP_MANUAL, 3000, fake_publish, &fake);
	fake.region_c_mw = 10000;
	fake.ppm_target_mw = 10000;
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_rearm(&cap, fake_publish, &fake), 0);
	KUNIT_EXPECT_EQ(test, fake.region_c_mw, 3000U);
	KUNIT_EXPECT_EQ(test, fake.ppm_target_mw, 3000U);
	KUNIT_EXPECT_EQ(test, cap.requested_state, 0U);
}

#ifdef ASAHI_HOST_TEST
int main(void)
{
	struct kunit test = {};

	cap_mapping(&test);
	cap_composition(&test);
	cap_startup_and_ppm(&test);
	cap_failure_and_invalid(&test);
	cap_recovery_and_limits(&test);
	puts("PASS: mW mapping, all-owner composition, startup, separate PPM, failure, recovery, limits");
	return 0;
}
#else
static struct kunit_case asahi_gpu_cooling_cases[] = {
	KUNIT_CASE(cap_mapping),
	KUNIT_CASE(cap_composition),
	KUNIT_CASE(cap_startup_and_ppm),
	KUNIT_CASE(cap_failure_and_invalid),
	KUNIT_CASE(cap_recovery_and_limits),
	{}
};
static struct kunit_suite asahi_gpu_cooling_suite = {
	.name = "asahi-gpu-cooling",
	.test_cases = asahi_gpu_cooling_cases,
};
kunit_test_suite(asahi_gpu_cooling_suite);
MODULE_LICENSE("Dual MIT/GPL");
#endif
