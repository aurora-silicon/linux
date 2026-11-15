// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Ryan Murray */

#include <kunit/test.h>
#include <linux/module.h>

#include "afk.h"

static void afk_ring_stride_test(struct kunit *test)
{
	u32 stride;

	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x10c0, 0x1000, &stride), 0);
	KUNIT_EXPECT_EQ(test, stride, 0x40U);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x1180, 0x1000, &stride), 0);
	KUNIT_EXPECT_EQ(test, stride, 0x80U);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x1000, 0x1000, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x10bd, 0x1000, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x10c1, 0x1000, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0xc0, 0, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x10c3, 0x1000, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x10c0, 0x1001, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x11c0, 0x1040, &stride), -EINVAL);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x1180, 0x1000, &stride), 0);
	KUNIT_EXPECT_EQ(test, afk_ring_advance(0xf80, 0x80, 0x1000, stride), 0U);
	KUNIT_EXPECT_EQ(test, afk_ring_advance(0xe80, 0x80, 0x1000, stride),
			0xf00U);
	KUNIT_EXPECT_EQ(test, afk_ring_stride(0x10c0, 0x1000, &stride), 0);
	KUNIT_EXPECT_EQ(test, afk_ring_advance(0xfc0, 0x40, 0x1000, stride), 0U);
}

static void afk_ring_payload_fits_test(struct kunit *test)
{
	const size_t hdr = sizeof(struct afk_qe);

	/* An entry ending exactly at the end of the ring fits. */
	KUNIT_EXPECT_TRUE(test, afk_ring_payload_fits(0, 0x1000 - hdr, 0x1000));
	KUNIT_EXPECT_TRUE(test, afk_ring_payload_fits(0xfc0, 0x30, 0x1000));
	KUNIT_EXPECT_TRUE(test, afk_ring_payload_fits(0x1000 - hdr, 0, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0xfc0, 0x31, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0xff8, 0, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0x1000, 0, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0x1040, 0, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0, 0, hdr - 1));
	/* Sizes that wrap a 32-bit offset sum must not pass for a fit. */
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0x40, 0xfffffff8, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0x40, U32_MAX - 0x3f, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0x100, U32_MAX - 0xff, 0x1000));
	KUNIT_EXPECT_FALSE(test, afk_ring_payload_fits(0, U32_MAX, 0x1000));
}

static void afk_ring_ptrs_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, afk_ring_ptrs_valid(0, 0, 0x1000, 0x40));
	KUNIT_EXPECT_TRUE(test, afk_ring_ptrs_valid(0xfc0, 0x40, 0x1000, 0x40));
	KUNIT_EXPECT_TRUE(test, afk_ring_ptrs_valid(0x80, 0xf80, 0x1000, 0x80));
	/* The write pointer must lie inside the ring. */
	KUNIT_EXPECT_FALSE(test, afk_ring_ptrs_valid(0, 0x1000, 0x1000, 0x40));
	KUNIT_EXPECT_FALSE(test, afk_ring_ptrs_valid(0, U32_MAX, 0x1000, 0x40));
	KUNIT_EXPECT_FALSE(test, afk_ring_ptrs_valid(0x1000, 0, 0x1000, 0x40));
	KUNIT_EXPECT_FALSE(test, afk_ring_ptrs_valid(U32_MAX, 0, 0x1000, 0x40));
	/* Both pointers must be block aligned. */
	KUNIT_EXPECT_FALSE(test, afk_ring_ptrs_valid(0, 0x20, 0x1000, 0x40));
	KUNIT_EXPECT_FALSE(test, afk_ring_ptrs_valid(0x10, 0, 0x1000, 0x40));
	KUNIT_EXPECT_FALSE(test, afk_ring_ptrs_valid(0x40, 0x80, 0x1000, 0x80));
	/* An uninitialised ring has no valid pointers. */
	KUNIT_EXPECT_FALSE(test, afk_ring_ptrs_valid(0, 0, 0x1000, 0));
	KUNIT_EXPECT_FALSE(test, afk_ring_ptrs_valid(0, 0, 0, 0x40));
}

struct afk_drain_ctx {
	unsigned int calls, entries;
};

/* A producer that publishes @entries entries, or one that never stops. */
static bool afk_drain_recv(void *data)
{
	struct afk_drain_ctx *ctx = data;

	return ++ctx->calls <= ctx->entries;
}

static void afk_ring_drain_test(struct kunit *test)
{
	struct afk_drain_ctx ctx = { .entries = 3 };
	unsigned int budget = afk_ring_drain_budget(0x1000, 0x40);

	KUNIT_EXPECT_EQ(test, budget, 64U);
	KUNIT_EXPECT_EQ(test, afk_ring_drain(afk_drain_recv, &ctx, budget), 3U);
	KUNIT_EXPECT_EQ(test, ctx.calls, 4U);

	/* A write pointer that keeps moving ahead exhausts the budget. */
	ctx = (struct afk_drain_ctx){ .entries = UINT_MAX };
	KUNIT_EXPECT_EQ(test, afk_ring_drain(afk_drain_recv, &ctx, budget), budget);
	KUNIT_EXPECT_EQ(test, ctx.calls, budget);

	/* Without a ring the single pass reports the missing setup. */
	KUNIT_EXPECT_EQ(test, afk_ring_drain_budget(0, 0), 1U);
}

static struct kunit_case afk_ring_cases[] = {
	KUNIT_CASE(afk_ring_ptrs_test),
	KUNIT_CASE(afk_ring_drain_test),
	KUNIT_CASE(afk_ring_stride_test),
	KUNIT_CASE(afk_ring_payload_fits_test),
	{}
};

static struct kunit_suite afk_ring_suite = {
	.name = "apple-afk-ring",
	.test_cases = afk_ring_cases,
};

kunit_test_suite(afk_ring_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple AFK ring buffers");
MODULE_LICENSE("Dual MIT/GPL");
