// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the VP9 frame checks of the Apple video decoder.
 */

#include <kunit/test.h>

#include "avd.h"

struct avd_vp9_tiling_case {
	const char *desc;
	u16 width;
	u8 tile_rows_log2;
	u8 tile_cols_log2;
	int ret;
};

static const struct avd_vp9_tiling_case avd_vp9_tiling_cases[] = {
	{ "one tile", 1920, 0, 0, 0 },
	{ "two tile rows", 1920, 1, 0, 0 },
	{ "four tile rows", 1920, 2, 0, 0 },
	{ "eight tile rows", 1920, 3, 0, -EINVAL },
	{ "2^26 tile rows", 4096, 26, 6, -EINVAL },
	{ "2^31 tile rows", 4096, 31, 1, -EINVAL },
	{ "255 tile rows log2", 1920, 255, 0, -EINVAL },
	{ "four tile columns at 1920", 1920, 0, 2, 0 },
	{ "eight tile columns at 1920", 1920, 0, 3, -EINVAL },
	{ "sixteen tile columns at 4096", 4096, 2, 4, 0 },
	{ "32 tile columns at 4096", 4096, 0, 5, -EINVAL },
	{ "64 tile columns at 4096", 4096, 0, 6, -EINVAL },
	{ "two tile columns at 64", 64, 0, 1, -EINVAL },
	{ "one tile column at 8192", 8192, 0, 0, -EINVAL },
	{ "two tile columns at 8192", 8192, 0, 1, 0 },
};

KUNIT_ARRAY_PARAM_DESC(avd_vp9_tiling, avd_vp9_tiling_cases, desc);

static void avd_vp9_tiling_test(struct kunit *test)
{
	const struct avd_vp9_tiling_case *c = test->param_value;
	struct v4l2_ctrl_vp9_frame frame = {
		.frame_width_minus_1 = c->width - 1,
		.frame_height_minus_1 = 1079,
		.tile_rows_log2 = c->tile_rows_log2,
		.tile_cols_log2 = c->tile_cols_log2,
	};

	KUNIT_EXPECT_EQ(test, avd_vp9_check_tiling(&frame), c->ret);
}

static struct kunit_case avd_vp9_test_cases[] = {
	KUNIT_CASE_PARAM(avd_vp9_tiling_test, avd_vp9_tiling_gen_params),
	{}
};

static struct kunit_suite avd_vp9_test_suite = {
	.name = "apple-avd-vp9",
	.test_cases = avd_vp9_test_cases,
};

kunit_test_suite(avd_vp9_test_suite);
