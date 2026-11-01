// SPDX-License-Identifier: GPL-2.0-only OR MIT

/* Build this same suite with -DASAHI_HOST_TEST or through KUnit. */
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
	unsigned int words[3];
	unsigned int calls;
	int error;
};

static int fake_publish(void *context, unsigned int percent)
{
	struct fake_actuator *fake = context;
	unsigned int i;

	fake->calls++;
	if (fake->error)
		return fake->error;
	for (i = 0; i < 3; i++)
		fake->words[i] = percent;
	return 0;
}

static void cap_mapping(struct kunit *test)
{
	unsigned long state;

	for (state = 0; state <= 100; state++)
		KUNIT_EXPECT_EQ(test, asahi_gpu_cap_percent(state), (int)(100 - state));
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_percent(25), 75);
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_percent(50), 50);
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_percent(75), 25);
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_percent(99), 1);
	KUNIT_EXPECT_LT(test, asahi_gpu_cap_percent(101), 0);
	KUNIT_EXPECT_LT(test, asahi_gpu_cap_percent(ULONG_MAX), 0);
}

static void cap_composition(struct kunit *test)
{
	unsigned int a, b;

	for (a = 0; a <= 100; a++) {
		for (b = 0; b <= 100; b++) {
			unsigned int expected = 100 - a;

			if (b < expected)
				expected = b;
			if (expected > 25)
				expected = 25;
			KUNIT_EXPECT_EQ(test, asahi_gpu_cap_effective(a, b), expected);
		}
	}
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_effective(ULONG_MAX, 100), 0U);
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_effective(0, 101), 0U);
}

static void cap_startup_and_mirrors(struct kunit *test)
{
	struct asahi_gpu_cap cap;
	struct fake_actuator fake = {};
	unsigned int i;

	asahi_gpu_cap_init(&cap);
	KUNIT_EXPECT_EQ(test, cap.published_percent, ASAHI_GPU_UNPUBLISHED);
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_update(&cap, 0, 100, fake_publish, &fake), 0);
	KUNIT_EXPECT_EQ(test, cap.requested_state, 0U);
	KUNIT_EXPECT_EQ(test, cap.published_percent, 25U);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_EQ(test, fake.words[i], 25U);
}

static void cap_failure_and_invalid(struct kunit *test)
{
	struct asahi_gpu_cap cap;
	struct fake_actuator fake = {};
	unsigned int calls;

	asahi_gpu_cap_init(&cap);
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_update(&cap, 99, 100, fake_publish, &fake), 0);
	fake.error = -5;
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_update(&cap, 0, 100, fake_publish, &fake), -5);
	KUNIT_EXPECT_EQ(test, cap.requested_state, 99U);
	KUNIT_EXPECT_EQ(test, cap.published_percent, 1U);
	KUNIT_EXPECT_EQ(test, fake.words[0], 1U);
	calls = fake.calls;
	KUNIT_EXPECT_LT(test, asahi_gpu_cap_update(&cap, ULONG_MAX, 100, fake_publish, &fake), 0);
	KUNIT_EXPECT_LT(test, asahi_gpu_cap_update(&cap, 0, 101, fake_publish, &fake), 0);
	KUNIT_EXPECT_EQ(test, fake.calls, calls);
	KUNIT_EXPECT_EQ(test, cap.published_percent, 1U);
}

static void cap_recovery(struct kunit *test)
{
	struct asahi_gpu_cap cap;
	struct fake_actuator fake = {};
	unsigned int i;

	asahi_gpu_cap_init(&cap);
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_update(&cap, 99, 100, fake_publish, &fake), 0);
	for (i = 0; i < 3; i++)
		fake.words[i] = 100; /* Firmware reset: rearm before reopening. */
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_update(&cap, cap.requested_state, 100,
						fake_publish, &fake), 0);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_EQ(test, fake.words[i], 1U);
	/* Removing a thermal demand still cannot lift qualification. */
	KUNIT_EXPECT_EQ(test, asahi_gpu_cap_update(&cap, 0, 100, fake_publish, &fake), 0);
	KUNIT_EXPECT_EQ(test, cap.published_percent, 25U);
}

#ifdef ASAHI_HOST_TEST
int main(void)
{
	struct kunit test = {};

	cap_mapping(&test);
	cap_composition(&test);
	cap_startup_and_mirrors(&test);
	cap_failure_and_invalid(&test);
	cap_recovery(&test);
	puts("PASS: cap mapping, composition, startup, mirror transaction, failure, recovery");
	return 0;
}
#else
static struct kunit_case asahi_gpu_cooling_cases[] = {
	KUNIT_CASE(cap_mapping),
	KUNIT_CASE(cap_composition),
	KUNIT_CASE(cap_startup_and_mirrors),
	KUNIT_CASE(cap_failure_and_invalid),
	KUNIT_CASE(cap_recovery),
	{}
};
static struct kunit_suite asahi_gpu_cooling_suite = {
	.name = "asahi-gpu-cooling",
	.test_cases = asahi_gpu_cooling_cases,
};
kunit_test_suite(asahi_gpu_cooling_suite);
MODULE_LICENSE("Dual MIT/GPL");
#endif
