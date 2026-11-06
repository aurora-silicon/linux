// SPDX-License-Identifier: GPL-2.0-only
/*
 * The read-only DART2 decoder against values built from the format rules:
 * leaf protection bits, the PA field, TTBR encoding, and walk geometry.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <kunit/test.h>
#include <linux/module.h>

#include "ave-dart-pte.h"

static u64 t_pte(u64 pa, bool r, bool w, bool cache)
{
	return ((pa >> 4) & GENMASK_ULL(37, 10)) | (r ? 0 : BIT_ULL(3)) |
	       (w ? 0 : BIT_ULL(2)) | (cache ? 0 : BIT_ULL(1)) | BIT_ULL(0);
}

static void ave_dart_leaf_test(struct kunit *test)
{
	struct ave_dart_leaf l;

	l = ave_dart2_leaf(t_pte(0x8123454000ULL, true, true, true));
	KUNIT_EXPECT_TRUE(test, l.valid && l.r && l.w && l.cache);
	KUNIT_EXPECT_EQ(test, l.pa, 0x8123454000ULL);

	l = ave_dart2_leaf(t_pte(0x10001c4c000ULL, true, false, true));
	KUNIT_EXPECT_TRUE(test, l.valid && l.r && !l.w && l.cache);
	KUNIT_EXPECT_EQ(test, l.pa, 0x10001c4c000ULL);

	l = ave_dart2_leaf(t_pte(0x4000, false, true, false));
	KUNIT_EXPECT_TRUE(test, l.valid && !l.r && l.w && !l.cache);

	/* Subpage fields above the PA field do not leak into the PA */
	l = ave_dart2_leaf(t_pte(0x8000, true, true, true) | GENMASK_ULL(51, 40));
	KUNIT_EXPECT_EQ(test, l.pa, 0x8000ULL);

	l = ave_dart2_leaf(0);
	KUNIT_EXPECT_FALSE(test, l.valid);
}

static void ave_dart_walk_test(struct kunit *test)
{
	/* TTBR: valid | (PA >> 14) << 2 */
	KUNIT_EXPECT_EQ(test, ave_dart_ttbr_pa(0x1 | (0x812345ULL << 2)),
			0x812345ULL << 14);
	/* 36-bit input: two levels (2048 x 2048 x 16 KiB = 64 GiB) */
	KUNIT_EXPECT_EQ(test, ave_dart_levels(36), 2U);
	KUNIT_EXPECT_EQ(test, ave_dart_levels(32), 2U);
	KUNIT_EXPECT_EQ(test, ave_dart_levels(42), 3U);
	/* 0x8_000d0000: leaf index 0x34, level-1 index 0x400 */
	KUNIT_EXPECT_EQ(test, ave_dart_index(0x8000d0000ULL, 0), 0x34U);
	KUNIT_EXPECT_EQ(test, ave_dart_index(0x8000d0000ULL, 1), 0x400U);
	KUNIT_EXPECT_EQ(test, ave_dart_index(0x8001f4000ULL, 0), 0x7dU);
	KUNIT_EXPECT_TRUE(test, ave_dart_iova_in_range(0x8001f4000ULL, 2));
	KUNIT_EXPECT_FALSE(test, ave_dart_iova_in_range(0x1000000000ULL, 2));
}

static struct kunit_case ave_dart_pte_cases[] = {
	KUNIT_CASE(ave_dart_leaf_test),
	KUNIT_CASE(ave_dart_walk_test),
	{}
};

static struct kunit_suite ave_dart_pte_suite = {
	.name = "apple-ave-dart-pte",
	.test_cases = ave_dart_pte_cases,
};
kunit_test_suite(ave_dart_pte_suite);

MODULE_DESCRIPTION("Apple AVE read-only DART2 decoder tests");
MODULE_LICENSE("GPL");
