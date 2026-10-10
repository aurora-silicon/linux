// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/test.h>
#include <linux/module.h>

#include "afk.h"

static void neo_afk_ring_stride_test(struct kunit *test)
{
	u32 stride;

	KUNIT_EXPECT_EQ(test, neo_afk_ring_stride(0x10c0, 0x1000, &stride), 0);
	KUNIT_EXPECT_EQ(test, stride, 0x40U);
	KUNIT_EXPECT_EQ(test, neo_afk_ring_stride(0x1180, 0x1000, &stride), 0);
	KUNIT_EXPECT_EQ(test, stride, 0x80U);
	KUNIT_EXPECT_EQ(test, neo_afk_ring_stride(0x1000, 0x1000, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, neo_afk_ring_stride(0x10bd, 0x1000, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, neo_afk_ring_stride(0x10c1, 0x1000, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, neo_afk_ring_stride(0xc0, 0, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, neo_afk_ring_stride(0x10c3, 0x1000, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, neo_afk_ring_stride(0x10c0, 0x1001, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, neo_afk_ring_stride(0x11c0, 0x1040, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, neo_afk_ring_stride(0x1180, 0x1000, &stride), 0);
	KUNIT_EXPECT_EQ(test, neo_afk_ring_advance(0xf80, 0x80, 0x1000, stride), 0U);
	KUNIT_EXPECT_EQ(test, neo_afk_ring_advance(0xe80, 0x80, 0x1000, stride),
			0xf00U);
	KUNIT_EXPECT_EQ(test, neo_afk_ring_stride(0x10c0, 0x1000, &stride), 0);
	KUNIT_EXPECT_EQ(test, neo_afk_ring_advance(0xfc0, 0x40, 0x1000, stride), 0U);
}

static struct kunit_case neo_afk_ring_cases[] = {
	KUNIT_CASE(neo_afk_ring_stride_test),
	{}
};

static struct kunit_suite neo_afk_ring_suite = {
	.name = "apple-afk-ring",
	.test_cases = neo_afk_ring_cases,
};

kunit_test_suite(neo_afk_ring_suite);

MODULE_LICENSE("Dual MIT/GPL");
