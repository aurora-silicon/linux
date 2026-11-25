// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the HEVC picture, tile and slice checks of the Apple video
 * decoder.
 */

#include <kunit/test.h>

#include "avd.h"

/* 64x64 luma samples in 16x16 CTBs: 4x4 CTBs. */
static void avd_hevc_test_sps(struct v4l2_ctrl_hevc_sps *sps)
{
	*sps = (struct v4l2_ctrl_hevc_sps) {
		.pic_width_in_luma_samples = 64,
		.pic_height_in_luma_samples = 64,
		.log2_min_luma_coding_block_size_minus3 = 0,
		.log2_diff_max_min_luma_coding_block_size = 1,
	};
}

struct avd_hevc_geometry_case {
	const char *desc;
	u16 width;
	u16 height;
	u8 min_cb_minus3;
	u8 diff;
	int ret;
};

static const struct avd_hevc_geometry_case avd_hevc_geometry_cases[] = {
	{ "16x16 CTBs", 64, 64, 0, 1, 0 },
	{ "32x32 CTBs", 1920, 1080, 0, 2, 0 },
	{ "64x64 CTBs", 4096, 2160, 0, 3, 0 },
	{ "64x64 CTBs from 16x16 coding blocks", 4096, 2160, 1, 2, 0 },
	{ "8x8 CTBs", 64, 64, 0, 0, -EINVAL },
	{ "128x128 CTBs", 64, 64, 0, 4, -EINVAL },
	{ "coding block size beyond any CTB", 64, 64, 255, 255, -EINVAL },
	{ "zero width", 0, 64, 0, 1, -EINVAL },
	{ "zero height", 64, 0, 0, 1, -EINVAL },
};

KUNIT_ARRAY_PARAM_DESC(avd_hevc_geometry, avd_hevc_geometry_cases, desc);

static void avd_hevc_geometry_test(struct kunit *test)
{
	const struct avd_hevc_geometry_case *c = test->param_value;
	struct v4l2_ctrl_hevc_sps sps = {
		.pic_width_in_luma_samples = c->width,
		.pic_height_in_luma_samples = c->height,
		.log2_min_luma_coding_block_size_minus3 = c->min_cb_minus3,
		.log2_diff_max_min_luma_coding_block_size = c->diff,
	};

	KUNIT_EXPECT_EQ(test, avd_hevc_check_sps_geometry(&sps), c->ret);
}

/*
 * Pre-streaming format negotiation (e.g. Chromium V4L2VideoDecoder::SetExtCtrlsInit)
 * supplies coded dimensions, chroma_format_idc and bit depth with zeroed CTB size
 * fields. Format validation accepts those while full geometry validation still
 * requires 16x16..64x64 CTBs.
 */
static void avd_hevc_sps_fmt_init_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_sps sps = {
		.pic_width_in_luma_samples = 1280,
		.pic_height_in_luma_samples = 720,
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
	KUNIT_EXPECT_EQ(test, avd_hevc_validate_sps_fmt(ctx, &sps), 0);
	KUNIT_EXPECT_EQ(test, avd_hevc_check_sps_geometry(&sps), -EINVAL);

	sps.bit_depth_luma_minus8 = 2;
	sps.bit_depth_chroma_minus8 = 2;
	KUNIT_EXPECT_EQ(test, avd_hevc_validate_sps_fmt(ctx, &sps), 0);

	/* 10-bit 4:2:2 is admitted through the P210 capture path. */
	sps.chroma_format_idc = 2;
	KUNIT_EXPECT_EQ(test, avd_hevc_validate_sps_fmt(ctx, &sps), 0);
	KUNIT_EXPECT_EQ(test, avd_hevc_check_sps_geometry(&sps), -EINVAL);
	sps.chroma_format_idc = 1;

	/* Oversized or unsupported formats remain rejected pre-streaming */
	sps.pic_width_in_luma_samples = 1920;
	KUNIT_EXPECT_EQ(test, avd_hevc_validate_sps_fmt(ctx, &sps), -EINVAL);
	sps.pic_width_in_luma_samples = 1280;
	sps.chroma_format_idc = 3;
	KUNIT_EXPECT_EQ(test, avd_hevc_validate_sps_fmt(ctx, &sps), -EINVAL);
	sps.chroma_format_idc = 1;
	sps.bit_depth_luma_minus8 = 4;
	sps.bit_depth_chroma_minus8 = 4;
	KUNIT_EXPECT_EQ(test, avd_hevc_validate_sps_fmt(ctx, &sps), -EINVAL);
	sps.bit_depth_luma_minus8 = 2;
	sps.bit_depth_chroma_minus8 = 0;
	KUNIT_EXPECT_EQ(test, avd_hevc_validate_sps_fmt(ctx, &sps), -EINVAL);
}

struct avd_hevc_tiles_case {
	const char *desc;
	bool uniform;
	u8 cols_minus1;
	u8 rows_minus1;
	u8 col_width_minus1[2];
	u8 row_height_minus1[2];
	int ret;
	/* tile id of every CTB in tile scan order, if accepted */
	u32 ids[16];
};

static const struct avd_hevc_tiles_case avd_hevc_tiles_cases[] = {
	{ "explicit 2x2 tiles", false, 1, 1, { 0 }, { 1 }, 0,
	  { 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 3, 3, 3, 3, 3, 3 } },
	{ "explicit 3x1 tiles", false, 2, 0, { 1, 0 }, { 0 }, 0,
	  { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2 } },
	{ "uniform 3x1 tiles", true, 2, 0, { 0 }, { 0 }, 0,
	  { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2 } },
	{ "uniform 4x4 tiles", true, 3, 3, { 0 }, { 0 }, 0,
	  { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 } },
	{ "explicit columns leave the last one empty", false, 1, 0, { 3 },
	  { 0 }, -EINVAL },
	{ "explicit columns wider than the picture", false, 1, 0, { 4 },
	  { 0 }, -EINVAL },
	{ "explicit columns far wider than the picture", false, 2, 0,
	  { 200, 200 }, { 0 }, -EINVAL },
	{ "explicit rows taller than the picture", false, 0, 1, { 0 },
	  { 4 }, -EINVAL },
	{ "explicit rows leave the last one empty", false, 0, 2, { 0 },
	  { 1, 1 }, -EINVAL },
	{ "uniform, more columns than CTBs", true, 4, 0, { 0 }, { 0 },
	  -EINVAL },
	{ "uniform, more rows than CTBs", true, 0, 4, { 0 }, { 0 },
	  -EINVAL },
};

KUNIT_ARRAY_PARAM_DESC(avd_hevc_tiles, avd_hevc_tiles_cases, desc);

static void avd_hevc_tiles_test(struct kunit *test)
{
	const struct avd_hevc_tiles_case *c = test->param_value;
	struct v4l2_ctrl_hevc_sps sps;
	struct v4l2_ctrl_hevc_pps pps = {
		.flags = V4L2_HEVC_PPS_FLAG_TILES_ENABLED |
			 (c->uniform ? V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING : 0),
		.num_tile_columns_minus1 = c->cols_minus1,
		.num_tile_rows_minus1 = c->rows_minus1,
	};
	u32 ids[16];

	avd_hevc_test_sps(&sps);
	memcpy(pps.column_width_minus1, c->col_width_minus1,
	       sizeof(c->col_width_minus1));
	memcpy(pps.row_height_minus1, c->row_height_minus1,
	       sizeof(c->row_height_minus1));

	KUNIT_EXPECT_EQ(test, avd_hevc_check_tiles(&sps, &pps), c->ret);

	/* The table builder must refuse the same layouts, before any write. */
	memset(ids, 0xff, sizeof(ids));
	KUNIT_ASSERT_EQ(test,
			avd_hevc_kunit_compute_tiles(&sps, &pps, ids,
						     ARRAY_SIZE(ids)),
			c->ret);
	if (!c->ret)
		KUNIT_EXPECT_MEMEQ(test, ids, c->ids, sizeof(ids));
}

/*
 * More tile columns or rows than the PPS control has size entries for are
 * refused by the check itself, even for a picture wide or tall enough.
 */
static void avd_hevc_tiles_array_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_sps sps;
	struct v4l2_ctrl_hevc_pps pps = {
		.flags = V4L2_HEVC_PPS_FLAG_TILES_ENABLED |
			 V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING,
	};

	avd_hevc_test_sps(&sps);
	sps.pic_width_in_luma_samples = 8192;
	sps.pic_height_in_luma_samples = 4320;

	pps.num_tile_columns_minus1 = ARRAY_SIZE(pps.column_width_minus1) - 1;
	pps.num_tile_rows_minus1 = ARRAY_SIZE(pps.row_height_minus1) - 1;
	KUNIT_EXPECT_EQ(test, avd_hevc_check_tiles(&sps, &pps), 0);

	pps.num_tile_columns_minus1 = ARRAY_SIZE(pps.column_width_minus1);
	KUNIT_EXPECT_EQ(test, avd_hevc_check_tiles(&sps, &pps), -EINVAL);

	pps.num_tile_columns_minus1 = ARRAY_SIZE(pps.column_width_minus1) - 1;
	pps.num_tile_rows_minus1 = ARRAY_SIZE(pps.row_height_minus1);
	KUNIT_EXPECT_EQ(test, avd_hevc_check_tiles(&sps, &pps), -EINVAL);
}

static void avd_hevc_no_tiles_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_sps sps;
	struct v4l2_ctrl_hevc_pps pps = {};
	u32 ids[16];

	avd_hevc_test_sps(&sps);
	KUNIT_EXPECT_EQ(test, avd_hevc_check_tiles(&sps, &pps), 0);
	KUNIT_EXPECT_EQ(test,
			avd_hevc_kunit_compute_tiles(&sps, &pps, ids,
						     ARRAY_SIZE(ids)),
			0);
}

struct avd_hevc_slices_case {
	const char *desc;
	bool tiles;
	u8 type;
	u32 flags;
	u32 addr;
	u8 col_idx;
	u8 l0_last;
	u8 l0_ref;
	u32 bytes;
	u32 data_offset;
	u32 num_entry_points;
	u32 entry_point;
	u32 available_entry_points;
	u32 payload;
	int ret;
};

#define AVD_TMVP V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED

static const struct avd_hevc_slices_case avd_hevc_slices_cases[] = {
	{ "I slice at the first CTB", .type = V4L2_HEVC_SLICE_TYPE_I,
	  .bytes = 100, .data_offset = 10, .payload = 100 },
	{ "I slice at the last CTB", .type = V4L2_HEVC_SLICE_TYPE_I,
	  .addr = 15, .bytes = 100, .data_offset = 10, .payload = 100 },
	{ "slice address past the picture", .type = V4L2_HEVC_SLICE_TYPE_I,
	  .addr = 16, .bytes = 100, .payload = 100, .ret = -EINVAL },
	{ "slice address far past the picture", .type = V4L2_HEVC_SLICE_TYPE_I,
	  .addr = U32_MAX, .bytes = 100, .payload = 100, .ret = -EINVAL },
	{ "slice data ends one byte after the payload",
	  .type = V4L2_HEVC_SLICE_TYPE_I, .bytes = 100, .data_offset = 10,
	  .payload = 99, .ret = -EINVAL },
	{ "slice header longer than the slice", .type = V4L2_HEVC_SLICE_TYPE_I,
	  .bytes = 10, .data_offset = 11, .payload = 100, .ret = -EINVAL },
	{ "payload beyond the 31-bit range", .type = V4L2_HEVC_SLICE_TYPE_I,
	  .bytes = 100, .payload = U32_MAX, .ret = -EINVAL },
	{ "P slice, collocated picture in the list",
	  .type = V4L2_HEVC_SLICE_TYPE_P, .flags = AVD_TMVP, .col_idx = 1,
	  .l0_last = 1, .l0_ref = 15, .bytes = 100, .payload = 100 },
	{ "P slice, collocated index past the active list",
	  .type = V4L2_HEVC_SLICE_TYPE_P, .flags = AVD_TMVP, .col_idx = 2,
	  .l0_last = 1, .bytes = 100, .payload = 100, .ret = -EINVAL },
	{ "P slice without TMVP, unused collocated index",
	  .type = V4L2_HEVC_SLICE_TYPE_P, .col_idx = 2, .l0_last = 1,
	  .bytes = 100, .payload = 100 },
	{ "P slice, collocated index past the list array",
	  .type = V4L2_HEVC_SLICE_TYPE_P, .col_idx = 16, .l0_last = 15,
	  .bytes = 100, .payload = 100, .ret = -EINVAL },
	{ "P slice, missing reference marked past the DPB",
	  .type = V4L2_HEVC_SLICE_TYPE_P, .l0_ref = 255, .bytes = 100,
	  .payload = 100 },
	{ "B slice, missing L1 reference marked past the DPB",
	  .type = V4L2_HEVC_SLICE_TYPE_B, .l0_ref = 255, .bytes = 100,
	  .payload = 100 },
	{ "two tiles, entry point inside the slice", .tiles = true,
	  .type = V4L2_HEVC_SLICE_TYPE_I, .bytes = 100, .data_offset = 10,
	  .num_entry_points = 1, .entry_point = 40,
	  .available_entry_points = 1, .payload = 100 },
	{ "two tiles, entry point past the payload", .tiles = true,
	  .type = V4L2_HEVC_SLICE_TYPE_I, .bytes = 100, .data_offset = 10,
	  .num_entry_points = 1, .entry_point = 91,
	  .available_entry_points = 1, .payload = 100, .ret = -EINVAL },
	{ "two tiles, entry point far past the payload", .tiles = true,
	  .type = V4L2_HEVC_SLICE_TYPE_I, .bytes = 100, .data_offset = 10,
	  .num_entry_points = 1, .entry_point = U32_MAX,
	  .available_entry_points = 1, .payload = 100, .ret = -EINVAL },
	{ "two tiles, more entry points than given", .tiles = true,
	  .type = V4L2_HEVC_SLICE_TYPE_I, .bytes = 100, .data_offset = 10,
	  .num_entry_points = 2, .entry_point = 40,
	  .available_entry_points = 1, .payload = 100, .ret = -EINVAL },
};

KUNIT_ARRAY_PARAM_DESC(avd_hevc_slices, avd_hevc_slices_cases, desc);

static void avd_hevc_slices_test(struct kunit *test)
{
	const struct avd_hevc_slices_case *c = test->param_value;
	struct v4l2_ctrl_hevc_decode_params *decode;
	struct v4l2_ctrl_hevc_slice_params *sl;
	struct v4l2_ctrl_hevc_sps sps;
	struct v4l2_ctrl_hevc_pps pps = {};
	u32 ep[2] = { c->entry_point, c->entry_point };

	decode = kunit_kzalloc(test, sizeof(*decode), GFP_KERNEL);
	sl = kunit_kzalloc(test, sizeof(*sl), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, decode);
	KUNIT_ASSERT_NOT_NULL(test, sl);

	avd_hevc_test_sps(&sps);
	if (c->tiles) {
		pps.flags = V4L2_HEVC_PPS_FLAG_TILES_ENABLED |
			    V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING;
		pps.num_tile_columns_minus1 = 1;
	}
	decode->num_active_dpb_entries = 2;
	sl->slice_type = c->type;
	sl->flags = c->flags;
	sl->slice_segment_addr = c->addr;
	sl->collocated_ref_idx = c->col_idx;
	sl->num_ref_idx_l0_active_minus1 = c->l0_last;
	sl->num_ref_idx_l1_active_minus1 = c->l0_last;
	memset(sl->ref_idx_l0, c->l0_ref, sizeof(sl->ref_idx_l0));
	memset(sl->ref_idx_l1, c->l0_ref, sizeof(sl->ref_idx_l1));
	sl->bit_size = c->bytes * 8;
	sl->data_byte_offset = c->data_offset;
	sl->num_entry_point_offsets = c->num_entry_points;

	KUNIT_EXPECT_EQ(test,
			avd_hevc_check_slices(&sps, &pps, decode, sl, 1, ep,
					      c->available_entry_points,
					      c->payload),
			c->ret);
}

/* A slice only fits after the ones before it. */
static void avd_hevc_two_slices_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_decode_params *decode;
	struct v4l2_ctrl_hevc_slice_params *sl;
	struct v4l2_ctrl_hevc_sps sps;
	struct v4l2_ctrl_hevc_pps pps = {};
	u32 ep = 0;

	decode = kunit_kzalloc(test, sizeof(*decode), GFP_KERNEL);
	sl = kunit_kcalloc(test, 2, sizeof(*sl), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, decode);
	KUNIT_ASSERT_NOT_NULL(test, sl);

	avd_hevc_test_sps(&sps);
	sl[0].slice_type = V4L2_HEVC_SLICE_TYPE_I;
	sl[0].bit_size = 60 * 8;
	sl[0].data_byte_offset = 5;
	sl[1] = sl[0];
	sl[1].slice_segment_addr = 8;

	KUNIT_EXPECT_EQ(test, avd_hevc_check_slices(&sps, &pps, decode, sl, 2,
						    &ep, 1, 120), 0);
	KUNIT_EXPECT_EQ(test, avd_hevc_check_slices(&sps, &pps, decode, sl, 2,
						    &ep, 1, 119), -EINVAL);
}

/*
 * The entry point offsets split a slice's data into its tiles; the last tile
 * takes what the others leave. Offsets that add up to more than the slice
 * holds leave it less than nothing, even when each one fits the payload.
 */
static void avd_hevc_entry_points_sum_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_decode_params *decode;
	struct v4l2_ctrl_hevc_slice_params *sl;
	struct v4l2_ctrl_hevc_sps sps;
	struct v4l2_ctrl_hevc_pps pps = {
		.flags = V4L2_HEVC_PPS_FLAG_TILES_ENABLED |
			 V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING,
		.num_tile_columns_minus1 = 3,
	};
	u32 ep[6] = { 20, 20, 20, 20, 20, 20 };

	decode = kunit_kzalloc(test, sizeof(*decode), GFP_KERNEL);
	sl = kunit_kcalloc(test, 2, sizeof(*sl), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, decode);
	KUNIT_ASSERT_NOT_NULL(test, sl);

	avd_hevc_test_sps(&sps);
	sl[0].slice_type = V4L2_HEVC_SLICE_TYPE_I;
	sl[0].bit_size = 100 * 8;
	sl[0].data_byte_offset = 10;
	sl[0].num_entry_point_offsets = 3;
	sl[1] = sl[0];
	sl[1].slice_segment_addr = 8;

	/* 10 header bytes, three tiles of 20 bytes and a last one of 30. */
	KUNIT_EXPECT_EQ(test, avd_hevc_check_slices(&sps, &pps, decode, sl, 1,
						    ep, 3, 100), 0);
	KUNIT_EXPECT_EQ(test, avd_hevc_check_slices(&sps, &pps, decode, sl, 2,
						    ep, 6, 200), 0);

	/* One byte more than the 90 data bytes of the slice. */
	ep[0] = 30;
	ep[1] = 30;
	ep[2] = 31;
	KUNIT_EXPECT_EQ(test, avd_hevc_check_slices(&sps, &pps, decode, sl, 1,
						    ep, 3, 100), -EINVAL);
	/*
	 * The same in the first of two slices: every offset and their sum
	 * stay inside the payload, but not inside the slice.
	 */
	KUNIT_EXPECT_EQ(test, avd_hevc_check_slices(&sps, &pps, decode, sl, 2,
						    ep, 6, 200), -EINVAL);

	/* And in the second slice. */
	ep[0] = 20;
	ep[1] = 20;
	ep[2] = 20;
	ep[5] = 51;
	KUNIT_EXPECT_EQ(test, avd_hevc_check_slices(&sps, &pps, decode, sl, 2,
						    ep, 6, 200), -EINVAL);
}

static struct kunit_case avd_hevc_test_cases[] = {
	KUNIT_CASE_PARAM(avd_hevc_geometry_test, avd_hevc_geometry_gen_params),
	KUNIT_CASE(avd_hevc_sps_fmt_init_test),
	KUNIT_CASE_PARAM(avd_hevc_tiles_test, avd_hevc_tiles_gen_params),
	KUNIT_CASE(avd_hevc_tiles_array_test),
	KUNIT_CASE(avd_hevc_no_tiles_test),
	KUNIT_CASE_PARAM(avd_hevc_slices_test, avd_hevc_slices_gen_params),
	KUNIT_CASE(avd_hevc_two_slices_test),
	KUNIT_CASE(avd_hevc_entry_points_sum_test),
	{}
};

static struct kunit_suite avd_hevc_test_suite = {
	.name = "apple-avd-hevc",
	.test_cases = avd_hevc_test_cases,
};

kunit_test_suite(avd_hevc_test_suite);
