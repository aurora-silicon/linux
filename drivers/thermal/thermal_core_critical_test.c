// SPDX-License-Identifier: GPL-2.0
/* Included by thermal_core.c to exercise critical trip confirmation. */

#include <kunit/test.h>

static void thermal_critical_test_first_sample(struct kunit *test)
{
	unsigned int count = 0;

	/* 0 and 1 keep the existing behaviour: the first sample acts. */
	KUNIT_EXPECT_TRUE(test, thermal_critical_confirm(&count, 125000, 125000, 0));
	count = 0;
	KUNIT_EXPECT_TRUE(test, thermal_critical_confirm(&count, 125000, 125000, 1));
}

static void thermal_critical_test_consecutive(struct kunit *test)
{
	unsigned int count = 0;

	KUNIT_EXPECT_FALSE(test, thermal_critical_confirm(&count, 126000, 125000, 3));
	KUNIT_EXPECT_FALSE(test, thermal_critical_confirm(&count, 125000, 125000, 3));
	KUNIT_EXPECT_TRUE(test, thermal_critical_confirm(&count, 130000, 125000, 3));
	/* Confirmed once: staying above does not act again. */
	KUNIT_EXPECT_FALSE(test, thermal_critical_confirm(&count, 130000, 125000, 3));
	KUNIT_EXPECT_EQ(test, count, 4U);
}

static void thermal_critical_test_noise(struct kunit *test)
{
	static const int samples[] = {
		119000, 126000, 117000, 125500, 110000, 127000, 124999, 128000,
	};
	unsigned int count = 0;
	int i;

	/* Single excursions separated by readings below the trip never act. */
	for (i = 0; i < ARRAY_SIZE(samples); i++)
		KUNIT_EXPECT_FALSE(test, thermal_critical_confirm(&count, samples[i],
								  125000, 2));
	KUNIT_EXPECT_EQ(test, count, 1U);
}

static void thermal_critical_test_rearm(struct kunit *test)
{
	unsigned int count = 0;

	KUNIT_EXPECT_FALSE(test, thermal_critical_confirm(&count, 125000, 125000, 2));
	KUNIT_EXPECT_TRUE(test, thermal_critical_confirm(&count, 125000, 125000, 2));
	KUNIT_EXPECT_FALSE(test, thermal_critical_confirm(&count, 100000, 125000, 2));
	KUNIT_EXPECT_EQ(test, count, 0U);
	KUNIT_EXPECT_FALSE(test, thermal_critical_confirm(&count, 125000, 125000, 2));
	KUNIT_EXPECT_TRUE(test, thermal_critical_confirm(&count, 125000, 125000, 2));
}

static void thermal_critical_test_saturates(struct kunit *test)
{
	unsigned int count = UINT_MAX - 1;

	KUNIT_EXPECT_FALSE(test, thermal_critical_confirm(&count, 125000, 125000, 3));
	KUNIT_EXPECT_FALSE(test, thermal_critical_confirm(&count, 125000, 125000, 3));
	KUNIT_EXPECT_EQ(test, count, UINT_MAX);
}

static struct kunit_case thermal_critical_test_cases[] = {
	KUNIT_CASE(thermal_critical_test_first_sample),
	KUNIT_CASE(thermal_critical_test_consecutive),
	KUNIT_CASE(thermal_critical_test_noise),
	KUNIT_CASE(thermal_critical_test_rearm),
	KUNIT_CASE(thermal_critical_test_saturates),
	{}
};

static struct kunit_suite thermal_critical_test_suite = {
	.name = "thermal_critical_samples",
	.test_cases = thermal_critical_test_cases,
};

kunit_test_suite(thermal_critical_test_suite);
