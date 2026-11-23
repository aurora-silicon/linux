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

/* The 4x4-CTB test picture has CTB addresses 0 to 15. */
static void avd_hevc_slices_test(struct kunit *test)
{
	struct v4l2_ctrl_hevc_sps sps;
	struct v4l2_ctrl_hevc_slice_params sl[2] = {};

	avd_hevc_test_sps(&sps);
	KUNIT_EXPECT_EQ(test, avd_hevc_check_slices(&sps, sl, 2), 0);
	sl[1].slice_segment_addr = 15;
	KUNIT_EXPECT_EQ(test, avd_hevc_check_slices(&sps, sl, 2), 0);
	sl[1].slice_segment_addr = 16;
	KUNIT_EXPECT_EQ(test, avd_hevc_check_slices(&sps, sl, 2), -EINVAL);
	sl[1].slice_segment_addr = U32_MAX;
	KUNIT_EXPECT_EQ(test, avd_hevc_check_slices(&sps, sl, 2), -EINVAL);
	KUNIT_EXPECT_EQ(test, avd_hevc_check_slices(&sps, sl, 1), 0);
}

static struct kunit_case avd_hevc_test_cases[] = {
	KUNIT_CASE_PARAM(avd_hevc_geometry_test, avd_hevc_geometry_gen_params),
	KUNIT_CASE_PARAM(avd_hevc_tiles_test, avd_hevc_tiles_gen_params),
	KUNIT_CASE(avd_hevc_tiles_array_test),
	KUNIT_CASE(avd_hevc_no_tiles_test),
	KUNIT_CASE(avd_hevc_slices_test),
	{}
};

static struct kunit_suite avd_hevc_test_suite = {
	.name = "apple-avd-hevc",
	.test_cases = avd_hevc_test_cases,
};

kunit_test_suite(avd_hevc_test_suite);
