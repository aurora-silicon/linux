// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "apple-jpeg-hw.h"

static void apple_jpeg_unqualified_test(struct kunit *test)
{
	const struct apple_jpeg_variant *variants[] = {
		&apple_jpeg_t6041, &apple_jpeg_t8140,
	};
	/* Public M1 Max revision, arbitrary revisions and invalid readbacks. */
	const u32 revisions[] = { 0xd1013, 1, 0x12345678, 0, U32_MAX };
	size_t i, j;

	for (i = 0; i < ARRAY_SIZE(variants); i++) {
		KUNIT_EXPECT_EQ(test, variants[i]->num_hwrevs, (size_t)0);
		for (j = 0; j < ARRAY_SIZE(revisions); j++)
			KUNIT_EXPECT_FALSE(test,
					   apple_jpeg_hwrev_supported(variants[i], revisions[j]));
	}
}

static void apple_jpeg_exact_match_test(struct kunit *test)
{
	/* Synthetic revisions exercise the matcher without qualifying hardware. */
	const u32 revisions[] = { 0x12345678, 0x1234567a };
	const struct apple_jpeg_variant variant = {
		.hwrevs = revisions,
		.num_hwrevs = ARRAY_SIZE(revisions),
	};
	const struct apple_jpeg_variant empty = { .hwrevs = revisions };

	KUNIT_EXPECT_TRUE(test, apple_jpeg_hwrev_supported(&variant, revisions[0]));
	KUNIT_EXPECT_TRUE(test, apple_jpeg_hwrev_supported(&variant, revisions[1]));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&variant, 0x12345679));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&variant, 0));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&variant, U32_MAX));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&empty, revisions[0]));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(NULL, revisions[0]));
}

static struct kunit_case apple_jpeg_cases[] = {
	KUNIT_CASE(apple_jpeg_unqualified_test),
	KUNIT_CASE(apple_jpeg_exact_match_test),
	{ }
};

static struct kunit_suite apple_jpeg_suite = {
	.name = "apple-jpeg-admission",
	.test_cases = apple_jpeg_cases,
};

kunit_test_suite(apple_jpeg_suite);

MODULE_LICENSE("GPL");
