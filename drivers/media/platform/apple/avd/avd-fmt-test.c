// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the run-time picture format check of the Apple video
 * decoder.
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <media/v4l2-common.h>

#include "avd.h"

static const struct avd_decoded_fmt_desc avd_fmt_test_decoded[] = {
	{ .fourcc = V4L2_PIX_FMT_NV12, .image_fmt = AVD_IMG_FMT_420_8BIT },
	{ .fourcc = V4L2_PIX_FMT_P010, .image_fmt = AVD_IMG_FMT_420_10BIT },
	{ .fourcc = V4L2_PIX_FMT_NV16, .image_fmt = AVD_IMG_FMT_422_8BIT },
};

static const struct avd_coded_fmt_desc avd_fmt_test_desc = {
	.num_decoded_fmts = ARRAY_SIZE(avd_fmt_test_decoded),
	.decoded_fmts = avd_fmt_test_decoded,
};

/* A context whose CAPTURE format was negotiated for @sized as @fourcc. */
static struct avd_ctx *avd_fmt_test_ctx(struct kunit *test,
					enum avd_image_fmt sized, u32 fourcc)
{
	struct avd_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct avd_dev *avd = kunit_kzalloc(test, sizeof(*avd), GFP_KERNEL);
	struct v4l2_pix_format_mplane *pix;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_NOT_NULL(test, avd);
	avd->dev = kunit_device_register(test, "avd-fmt-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, avd->dev);
	ctx->dev = avd;
	ctx->coded_fmt_desc = &avd_fmt_test_desc;
	ctx->image_fmt = sized;
	pix = &ctx->decoded_fmt.fmt.pix_mp;
	pix->pixelformat = fourcc;
	pix->width = 64;
	pix->height = 64;
	fill_comp(&ctx->comp, sized, pix->width, pix->height);
	return ctx;
}

static void avd_fmt_any_test(struct kunit *test)
{
	struct avd_ctx *ctx = avd_fmt_test_ctx(test, AVD_IMG_FMT_ANY,
					       V4L2_PIX_FMT_NV12);

	/* Buffers sized for "any" are 8-bit 4:2:0. */
	KUNIT_EXPECT_EQ(test, avd_check_run_image_fmt(ctx, AVD_IMG_FMT_ANY), 0);
	KUNIT_EXPECT_EQ(test,
			avd_check_run_image_fmt(ctx, AVD_IMG_FMT_420_8BIT), 0);
	KUNIT_EXPECT_EQ(test,
			avd_check_run_image_fmt(ctx, AVD_IMG_FMT_420_10BIT),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			avd_check_run_image_fmt(ctx, AVD_IMG_FMT_422_8BIT),
			-EINVAL);
}

/* "Any" with a 10-bit CAPTURE fourcc still has 8-bit sized storage. */
static void avd_fmt_any_p010_test(struct kunit *test)
{
	struct avd_ctx *ctx = avd_fmt_test_ctx(test, AVD_IMG_FMT_ANY,
					       V4L2_PIX_FMT_P010);

	KUNIT_EXPECT_EQ(test,
			avd_check_run_image_fmt(ctx, AVD_IMG_FMT_420_10BIT),
			-EINVAL);
}

static void avd_fmt_10bit_test(struct kunit *test)
{
	struct avd_ctx *ctx = avd_fmt_test_ctx(test, AVD_IMG_FMT_420_10BIT,
					       V4L2_PIX_FMT_P010);

	KUNIT_EXPECT_EQ(test,
			avd_check_run_image_fmt(ctx, AVD_IMG_FMT_420_10BIT), 0);
	KUNIT_EXPECT_EQ(test,
			avd_check_run_image_fmt(ctx, AVD_IMG_FMT_420_8BIT),
			-EINVAL);
}

static void avd_fmt_422_test(struct kunit *test)
{
	struct avd_ctx *ctx = avd_fmt_test_ctx(test, AVD_IMG_FMT_422_8BIT,
					       V4L2_PIX_FMT_NV16);

	KUNIT_EXPECT_EQ(test,
			avd_check_run_image_fmt(ctx, AVD_IMG_FMT_422_8BIT), 0);
	KUNIT_EXPECT_EQ(test,
			avd_check_run_image_fmt(ctx, AVD_IMG_FMT_420_8BIT),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			avd_check_run_image_fmt(ctx, AVD_IMG_FMT_422_10BIT),
			-EINVAL);
}

/* Compressed storage laid out for another size does not fit either. */
static void avd_fmt_comp_mismatch_test(struct kunit *test)
{
	struct avd_ctx *ctx = avd_fmt_test_ctx(test, AVD_IMG_FMT_420_8BIT,
					       V4L2_PIX_FMT_NV12);

	fill_comp(&ctx->comp, AVD_IMG_FMT_420_8BIT, 32, 32);
	KUNIT_EXPECT_EQ(test,
			avd_check_run_image_fmt(ctx, AVD_IMG_FMT_420_8BIT),
			-EINVAL);
}

/* Scratch sized at STREAMON for 8 bits does not take a 10-bit picture. */
static void avd_fmt_scratch_depth_test(struct kunit *test)
{
	struct avd_ctx *ctx = avd_fmt_test_ctx(test, AVD_IMG_FMT_420_10BIT,
					       V4L2_PIX_FMT_P010);

	ctx->scratch_sized = true;
	ctx->scratch_fmt = AVD_IMG_FMT_ANY;
	KUNIT_EXPECT_EQ(test,
			avd_check_run_image_fmt(ctx, AVD_IMG_FMT_420_10BIT),
			-EINVAL);
	ctx->scratch_fmt = AVD_IMG_FMT_420_10BIT;
	KUNIT_EXPECT_EQ(test,
			avd_check_run_image_fmt(ctx, AVD_IMG_FMT_420_10BIT), 0);
	/* Codecs that size no scratch from the format are not limited. */
	ctx->scratch_sized = false;
	ctx->scratch_fmt = AVD_IMG_FMT_ANY;
	KUNIT_EXPECT_EQ(test,
			avd_check_run_image_fmt(ctx, AVD_IMG_FMT_420_10BIT), 0);
}

static struct kunit_case avd_fmt_test_cases[] = {
	KUNIT_CASE(avd_fmt_any_test),
	KUNIT_CASE(avd_fmt_any_p010_test),
	KUNIT_CASE(avd_fmt_10bit_test),
	KUNIT_CASE(avd_fmt_422_test),
	KUNIT_CASE(avd_fmt_comp_mismatch_test),
	KUNIT_CASE(avd_fmt_scratch_depth_test),
	{}
};

static struct kunit_suite avd_fmt_test_suite = {
	.name = "apple-avd-fmt",
	.test_cases = avd_fmt_test_cases,
};

kunit_test_suite(avd_fmt_test_suite);
