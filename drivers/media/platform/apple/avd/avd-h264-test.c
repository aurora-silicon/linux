// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the H.264 slice checks of the Apple video decoder.
 */

#include <kunit/test.h>
#include <linux/sizes.h>
#include <media/v4l2-common.h>

#include "avd.h"

struct avd_slice_data_case {
	const char *desc;
	const u8 *data;
	u32 len;
	u32 header_len;
	bool found;
	u32 off;
};

static const u8 avd_test_slice[] = {
	0x25, 0xb8, 0x00, 0x00, 0x04, 0x01, 0xaa, 0xbb,
};

/* The same slice with an emulation prevention byte in its header. */
static const u8 avd_test_slice_epb[] = {
	0x25, 0xb8, 0x00, 0x00, 0x03, 0x01, 0xaa, 0xbb,
};

static const struct avd_slice_data_case avd_slice_data_cases[] = {
	{ "empty payload", avd_test_slice, 0, 0 },
	{ "one-byte payload", avd_test_slice, 1, 0 },
	{ "two-byte payload", avd_test_slice, 2, 0 },
	{ "one byte of data", avd_test_slice, 3, 2, true, 2 },
	{ "header ends one byte early", avd_test_slice, 8, 7, true, 7 },
	{ "header ends at the end", avd_test_slice, 8, 8 },
	{ "header beyond the end", avd_test_slice, 8, 12 },
	{ "header without emulation prevention", avd_test_slice, 8, 5, true, 5 },
	{ "emulation prevention in the header", avd_test_slice_epb, 8, 5, true, 6 },
};

KUNIT_ARRAY_PARAM_DESC(avd_slice_data, avd_slice_data_cases, desc);

static void avd_slice_data_test(struct kunit *test)
{
	const struct avd_slice_data_case *c = test->param_value;
	u32 off = U32_MAX;

	KUNIT_EXPECT_EQ(test, avd_h264_find_slice_data(c->data, c->len,
						       c->header_len, &off),
			c->found);
	if (c->found)
		KUNIT_EXPECT_EQ(test, off, c->off);
	else
		KUNIT_EXPECT_EQ(test, off, U32_MAX);
}

/* The size of the slice table in avd-h264.c */
#define AVD_TEST_MAX_SLICES	4096

struct avd_slice_fits_case {
	const char *desc;
	u16 width_mbs;
	u16 height_map_units;
	bool fields;
	size_t held;
	size_t held_bytes;
	u32 len;
	size_t decoded_size;
	bool fits;
};

static const struct avd_slice_fits_case avd_slice_fits_cases[] = {
	{ "one-macroblock frame, first slice", 1, 1, false, 0, 0, 16, 384, true },
	{ "one-macroblock frame, second slice", 1, 1, false, 1, PAGE_SIZE, 16, 384 },
	{ "one-macroblock field pair, second slice", 1, 1, true, 1, PAGE_SIZE, 16, 384, true },
	{ "one-macroblock field pair, third slice", 1, 1, true, 2, 2 * PAGE_SIZE, 16, 384 },
	{ "320x240, last slice", 20, 15, false, 299, 299 * PAGE_SIZE, 16, 115200, true },
	{ "320x240, one slice over", 20, 15, false, 300, 300 * PAGE_SIZE, 16, 115200 },
	{ "1920x1088, last slice of the table", 120, 68, false,
	  AVD_TEST_MAX_SLICES - 1, 0, 16, 3133440, true },
	{ "1920x1088, one slice over the table", 120, 68, false,
	  AVD_TEST_MAX_SLICES, 0, 16, 3133440 },
	{ "data up to the budget", 120, 68, false, 1, SZ_512M - PAGE_SIZE,
	  PAGE_SIZE, SZ_256M, true },
	{ "data one byte over the budget", 120, 68, false, 1,
	  SZ_512M - PAGE_SIZE, PAGE_SIZE + 1, SZ_256M },
	{ "no decoded size, data up to the page budget", 120, 68, false, 1,
	  (AVD_TEST_MAX_SLICES - 1) * PAGE_SIZE, 1, 0, true },
	{ "no decoded size, data over the page budget", 120, 68, false, 1,
	  AVD_TEST_MAX_SLICES * PAGE_SIZE, 1, 0 },
	{ "slice of almost 4 GiB", 120, 68, false, 0, 0, U32_MAX, SZ_256M },
};

KUNIT_ARRAY_PARAM_DESC(avd_slice_fits, avd_slice_fits_cases, desc);

static void avd_slice_fits_test(struct kunit *test)
{
	const struct avd_slice_fits_case *c = test->param_value;
	struct v4l2_ctrl_h264_sps sps = {
		.pic_width_in_mbs_minus1 = c->width_mbs - 1,
		.pic_height_in_map_units_minus1 = c->height_map_units - 1,
		.flags = c->fields ? 0 : V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY,
	};

	KUNIT_EXPECT_EQ(test, avd_h264_slice_fits(&sps, c->held, c->held_bytes,
						  c->len, c->decoded_size),
			c->fits);
}

/* The H.264 OUTPUT frame size limits in avd-v4l2.c */
static const struct v4l2_frmsize_stepwise avd_test_h264_frmsize = {
	.min_width = 64, .max_width = 4096, .step_width = 64,
	.min_height = 64, .max_height = 4096, .step_height = 16,
};

struct avd_sps_size_case {
	const char *desc;
	/* OUTPUT format as the client asks for it */
	u32 width;
	u32 height;
	u16 width_mbs;
	u16 height_map_units;
	int ret;
};

static const struct avd_sps_size_case avd_sps_size_cases[] = {
	{ "1080p, OUTPUT 1920x1080", 1920, 1080, 120, 68, 0 },
	{ "1080p, OUTPUT 1920x1088", 1920, 1088, 120, 68, 0 },
	{ "720p", 1280, 720, 80, 45, 0 },
	{ "720p, one row over", 1280, 720, 80, 46, -EINVAL },
	{ "720p, one column over", 1280, 720, 81, 45, -EINVAL },
	{ "picture smaller than the format", 1920, 1088, 40, 30, 0 },
	{ "64x64, matching picture", 64, 64, 4, 4, 0 },
	{ "64x64, 4096x4096 picture", 64, 64, 256, 256, -EINVAL },
	{ "64x64, largest SPS size", 64, 64, U16_MAX, U16_MAX, -EINVAL },
};

KUNIT_ARRAY_PARAM_DESC(avd_sps_size, avd_sps_size_cases, desc);

static void avd_sps_size_test(struct kunit *test)
{
	const struct avd_sps_size_case *c = test->param_value;
	struct v4l2_ctrl_h264_sps sps = {
		.chroma_format_idc = 1,
		.pic_width_in_mbs_minus1 = c->width_mbs - 1,
		.pic_height_in_map_units_minus1 = c->height_map_units - 1,
		.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY,
	};
	struct avd_ctx *ctx;
	u32 w = c->width, h = c->height;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	/* As S_FMT(OUTPUT) does */
	v4l2_apply_frmsize_constraints(&w, &h, &avd_test_h264_frmsize);
	ctx->coded_fmt.fmt.pix_mp.width = w;
	ctx->coded_fmt.fmt.pix_mp.height = h;

	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps(ctx, &sps), c->ret);
}

/*
 * Pre-streaming format negotiation (e.g. Chromium V4L2VideoDecoder::SetExtCtrlsInit)
 * supplies chroma_format_idc and bit depth with flags == 0 and zeroed dimensions
 * before the first frame request carries the parsed SPS. Format validation accepts
 * those controls while full SPS validation still rejects interlaced streams.
 */
static void avd_sps_fmt_init_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_sps sps = {
		.chroma_format_idc = 1,
	};
	struct avd_dev *dev;
	struct avd_ctx *ctx;

	dev = kunit_kzalloc(test, sizeof(*dev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->dev = dev;
	ctx->coded_fmt.fmt.pix_mp.width = 1280;
	ctx->coded_fmt.fmt.pix_mp.height = 720;

	/* 8-bit and 10-bit 4:2:0 pre-streaming init controls */
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_fmt(ctx, &sps), 0);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, false), 0);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, true), -EINVAL);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps(ctx, &sps), -EINVAL);

	sps.bit_depth_luma_minus8 = 2;
	sps.bit_depth_chroma_minus8 = 2;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_fmt(ctx, &sps), 0);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, false), 0);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, true), -EINVAL);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps(ctx, &sps), -EINVAL);

	/* Populated interlaced SPS must still be rejected pre-streaming */
	sps = (struct v4l2_ctrl_h264_sps) {
		.chroma_format_idc = 1,
		.pic_width_in_mbs_minus1 = 79,
		.pic_height_in_map_units_minus1 = 22,
	};
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, false), -EINVAL);
	sps.pic_width_in_mbs_minus1 = 0;
	sps.pic_height_in_map_units_minus1 = 0;
	sps.flags = V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, false), -EINVAL);

	/* Unsupported formats remain rejected pre-streaming */
	sps = (struct v4l2_ctrl_h264_sps) {
		.chroma_format_idc = 3,
		.bit_depth_luma_minus8 = 2,
		.bit_depth_chroma_minus8 = 2,
	};
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_fmt(ctx, &sps), -EINVAL);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, false), -EINVAL);
	sps.chroma_format_idc = 2;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_fmt(ctx, &sps), -EINVAL);
	sps.chroma_format_idc = 1;
	sps.bit_depth_luma_minus8 = 4;
	sps.bit_depth_chroma_minus8 = 4;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_fmt(ctx, &sps), -EINVAL);
	sps.bit_depth_luma_minus8 = 2;
	sps.bit_depth_chroma_minus8 = 0;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_fmt(ctx, &sps), -EINVAL);
}

static void avd_start_slices_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_sps sps = {
		.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY,
	};

	/* One slice per row */
	sps.pic_width_in_mbs_minus1 = 119;
	sps.pic_height_in_map_units_minus1 = 67;
	KUNIT_EXPECT_EQ(test, avd_h264_start_slices(&sps), 68);

	/* No more rows than macroblocks, and never more than the table */
	sps.pic_width_in_mbs_minus1 = 0;
	sps.pic_height_in_map_units_minus1 = U16_MAX;
	KUNIT_EXPECT_LE(test, avd_h264_start_slices(&sps), AVD_TEST_MAX_SLICES);
	sps.pic_width_in_mbs_minus1 = U16_MAX;
	KUNIT_EXPECT_LE(test, avd_h264_start_slices(&sps), AVD_TEST_MAX_SLICES);
	sps.flags = 0;
	KUNIT_EXPECT_LE(test, avd_h264_start_slices(&sps), AVD_TEST_MAX_SLICES);
}

static void avd_scaling_flat_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_pps pps = {};
	struct v4l2_ctrl_h264_scaling_matrix *m;

	m = kunit_kzalloc(test, sizeof(*m), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, m);
	memset(m->scaling_list_4x4, 16, sizeof(m->scaling_list_4x4));
	memset(m->scaling_list_8x8, 16, sizeof(m->scaling_list_8x8));
	KUNIT_EXPECT_TRUE(test, avd_h264_scaling_is_flat(&pps, m));

	/* The 8x8 lists only count with transform_8x8_mode. */
	m->scaling_list_8x8[1][63] = 17;
	KUNIT_EXPECT_TRUE(test, avd_h264_scaling_is_flat(&pps, m));
	pps.flags = V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;
	KUNIT_EXPECT_FALSE(test, avd_h264_scaling_is_flat(&pps, m));

	m->scaling_list_8x8[1][63] = 16;
	KUNIT_EXPECT_TRUE(test, avd_h264_scaling_is_flat(&pps, m));
	m->scaling_list_4x4[5][15] = 6;
	KUNIT_EXPECT_FALSE(test, avd_h264_scaling_is_flat(&pps, m));
}

static struct kunit_case avd_h264_test_cases[] = {
	KUNIT_CASE(avd_scaling_flat_test),
	KUNIT_CASE_PARAM(avd_slice_data_test, avd_slice_data_gen_params),
	KUNIT_CASE_PARAM(avd_slice_fits_test, avd_slice_fits_gen_params),
	KUNIT_CASE_PARAM(avd_sps_size_test, avd_sps_size_gen_params),
	KUNIT_CASE(avd_sps_fmt_init_test),
	KUNIT_CASE(avd_start_slices_test),
	{}
};

static struct kunit_suite avd_h264_test_suite = {
	.name = "apple-avd-h264",
	.test_cases = avd_h264_test_cases,
};

kunit_test_suite(avd_h264_test_suite);
