// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the instruction job table of the Apple video decoder.
 */

#include <kunit/test.h>

#include "avd.h"
#include "avd-inst.h"

static void avd_job_free(void *data)
{
	struct avd_ctx *ctx = data;

	kvfree(ctx->job.segments);
}

static struct avd_ctx *avd_job_test_ctx(struct kunit *test, size_t segments)
{
	struct avd_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_EQ(test, avd_init_job(ctx, AVD_CODEC_H264, segments), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, avd_job_free, ctx),
			0);
	KUNIT_EXPECT_EQ(test, ctx->job.max, segments);
	KUNIT_EXPECT_FALSE(test, ctx->job.overflow);
	return ctx;
}

/* A full segment takes no more instructions. */
static void avd_job_segment_full_test(struct kunit *test)
{
	struct avd_ctx *ctx = avd_job_test_ctx(test, 2);
	int i;

	for (i = 0; i < AVD_MAX_INST; i++)
		push(i, "fill");
	KUNIT_EXPECT_EQ(test, ctx->job.segments[0].num, AVD_MAX_INST);
	KUNIT_EXPECT_FALSE(test, ctx->job.overflow);

	push(0xdeadbeef, "one too many");
	KUNIT_EXPECT_EQ(test, ctx->job.segments[0].num, AVD_MAX_INST);
	KUNIT_EXPECT_TRUE(test, ctx->job.overflow);
	/* The next segment was not written either. */
	KUNIT_EXPECT_EQ(test, ctx->job.segments[1].num, 0);
}

/* Segments past the allocated table are never written. */
static void avd_job_table_full_test(struct kunit *test)
{
	struct avd_ctx *ctx = avd_job_test_ctx(test, 2);
	int i;

	push(1, "header");
	ctx->job.num++;
	push(2, "first segment");
	KUNIT_EXPECT_FALSE(test, ctx->job.overflow);

	for (i = 0; i < 8; i++) {
		ctx->job.num++;
		push(3, "past the table");
	}
	KUNIT_EXPECT_TRUE(test, ctx->job.overflow);
	KUNIT_EXPECT_EQ(test, ctx->job.segments[0].num, 1);
	KUNIT_EXPECT_EQ(test, ctx->job.segments[1].num, 1);

	/* A new job starts clean. */
	KUNIT_ASSERT_EQ(test, avd_init_job(ctx, AVD_CODEC_H264, 1), 0);
	KUNIT_EXPECT_FALSE(test, ctx->job.overflow);
	KUNIT_EXPECT_EQ(test, ctx->job.max, 1);
}

static struct kunit_case avd_job_test_cases[] = {
	KUNIT_CASE(avd_job_segment_full_test),
	KUNIT_CASE(avd_job_table_full_test),
	{}
};

static struct kunit_suite avd_job_test_suite = {
	.name = "apple-avd-job",
	.test_cases = avd_job_test_cases,
};

kunit_test_suite(avd_job_test_suite);
