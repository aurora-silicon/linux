// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the AV1 tile checks of the Apple video decoder.
 */

#include <kunit/test.h>

#include "avd.h"

#define AVD_TEST_PAYLOAD	4096

/* A rows x cols layout, entries in raster order, each tile 100 bytes. */
static void avd_av1_test_layout(struct v4l2_ctrl_av1_tile_group_entry *e,
				unsigned int rows, unsigned int cols)
{
	unsigned int r, c;

	for (r = 0; r < rows; r++)
		for (c = 0; c < cols; c++, e++) {
			e->tile_offset = (r * cols + c) * 100;
			e->tile_size = 100;
			e->tile_row = r;
			e->tile_col = c;
		}
}

static void avd_av1_tiles_valid_test(struct kunit *test)
{
	struct v4l2_ctrl_av1_tile_group_entry e[6] = {};

	avd_av1_test_layout(e, 2, 3);
	KUNIT_EXPECT_EQ(test, avd_av1_check_tiles(NULL, 2, 3, e, 6,
						  AVD_TEST_PAYLOAD), 0);
	/* Entries past the frame's tiles are not looked at. */
	e[5].tile_row = U32_MAX;
	KUNIT_EXPECT_EQ(test, avd_av1_check_tiles(NULL, 1, 3, e, 6,
						  AVD_TEST_PAYLOAD), 0);
	/* No tiles, no entries. */
	KUNIT_EXPECT_EQ(test, avd_av1_check_tiles(NULL, 0, 0, e, 0,
						  AVD_TEST_PAYLOAD), 0);
}

static void avd_av1_tiles_count_test(struct kunit *test)
{
	struct v4l2_ctrl_av1_tile_group_entry e[6] = {};

	avd_av1_test_layout(e, 2, 3);
	KUNIT_EXPECT_EQ(test, avd_av1_check_tiles(NULL, 2, 3, e, 5,
						  AVD_TEST_PAYLOAD), -EINVAL);
	KUNIT_EXPECT_EQ(test, avd_av1_check_tiles(NULL, V4L2_AV1_MAX_TILE_ROWS + 1,
						  1, e, 6, AVD_TEST_PAYLOAD),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, avd_av1_check_tiles(NULL, 1,
						  V4L2_AV1_MAX_TILE_COLS + 1,
						  e, 6, AVD_TEST_PAYLOAD),
			-EINVAL);
}

static void avd_av1_tiles_extent_test(struct kunit *test)
{
	struct v4l2_ctrl_av1_tile_group_entry e[6] = {};

	avd_av1_test_layout(e, 2, 3);
	/* The last tile ends exactly at the payload's end. */
	KUNIT_EXPECT_EQ(test, avd_av1_check_tiles(NULL, 2, 3, e, 6, 600), 0);
	KUNIT_EXPECT_EQ(test, avd_av1_check_tiles(NULL, 2, 3, e, 6, 599),
			-EINVAL);
	e[2].tile_offset = U32_MAX;
	KUNIT_EXPECT_EQ(test, avd_av1_check_tiles(NULL, 2, 3, e, 6,
						  AVD_TEST_PAYLOAD), -EINVAL);
	e[2].tile_offset = 200;
	e[2].tile_size = U32_MAX;
	KUNIT_EXPECT_EQ(test, avd_av1_check_tiles(NULL, 2, 3, e, 6,
						  AVD_TEST_PAYLOAD), -EINVAL);
}

struct avd_av1_rowcol_case {
	const char *desc;
	unsigned int idx;
	u32 tile_row;
	u32 tile_col;
	int ret;
};

/* Entry idx of a 2x3 layout gets tile_row/tile_col; every other entry is valid. */
static const struct avd_av1_rowcol_case avd_av1_rowcol_cases[] = {
	{ "last row and column", 5, 1, 2, 0 },
	{ "row equals tile_rows", 5, 2, 2, -EINVAL },
	{ "column equals tile_cols", 5, 1, 3, -EINVAL },
	{ "first entry, row equals tile_rows", 0, 2, 0, -EINVAL },
	{ "row past the tile_info arrays", 3, V4L2_AV1_MAX_TILE_ROWS + 1, 0, -EINVAL },
	{ "column past the tile_info arrays", 3, 0, V4L2_AV1_MAX_TILE_COLS + 1, -EINVAL },
	{ "huge row", 1, 0x3fffffff, 0, -EINVAL },
	{ "huge column", 1, 0, 0x3fffffff, -EINVAL },
	{ "row U32_MAX", 4, U32_MAX, 1, -EINVAL },
	{ "column U32_MAX", 4, 1, U32_MAX, -EINVAL },
};

KUNIT_ARRAY_PARAM_DESC(avd_av1_rowcol, avd_av1_rowcol_cases, desc);

static void avd_av1_tiles_rowcol_test(struct kunit *test)
{
	const struct avd_av1_rowcol_case *c = test->param_value;
	struct v4l2_ctrl_av1_tile_group_entry e[6] = {};

	avd_av1_test_layout(e, 2, 3);
	e[c->idx].tile_row = c->tile_row;
	e[c->idx].tile_col = c->tile_col;
	KUNIT_EXPECT_EQ(test, avd_av1_check_tiles(NULL, 2, 3, e, 6,
						  AVD_TEST_PAYLOAD), c->ret);
}

static struct kunit_case avd_av1_test_cases[] = {
	KUNIT_CASE(avd_av1_tiles_valid_test),
	KUNIT_CASE(avd_av1_tiles_count_test),
	KUNIT_CASE(avd_av1_tiles_extent_test),
	KUNIT_CASE_PARAM(avd_av1_tiles_rowcol_test, avd_av1_rowcol_gen_params),
	{}
};

static struct kunit_suite avd_av1_test_suite = {
	.name = "apple-avd-av1",
	.test_cases = avd_av1_test_cases,
};

kunit_test_suite(avd_av1_test_suite);
