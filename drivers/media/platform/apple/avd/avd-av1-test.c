// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the AV1 tile and frame size checks of the Apple video
 * decoder.
 */

#include <kunit/test.h>
#include <linux/slab.h>

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

/*
 * The run takes its own copy of the tile-group entries: replacing and freeing
 * the control's storage afterwards, as setting a larger array does, leaves
 * the copy and its count intact.
 */
static void avd_av1_copy_tile_group_test(struct kunit *test)
{
	struct v4l2_ctrl_av1_tile_group_entry expect[6] = {};
	struct v4l2_ctrl_av1_tile_group_entry *src, *bigger, *copy;
	struct v4l2_ctrl_handler *hdl;
	struct v4l2_ctrl *ctrl;
	unsigned int entries = 0;

	hdl = kunit_kzalloc(test, sizeof(*hdl), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, hdl);
	ctrl = kunit_kzalloc(test, sizeof(*ctrl), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctrl);
	KUNIT_ASSERT_EQ(test, v4l2_ctrl_handler_init(hdl, 0), 0);

	src = kcalloc(ARRAY_SIZE(expect), sizeof(*src), GFP_KERNEL);
	bigger = kcalloc(V4L2_AV1_MAX_TILE_COUNT, sizeof(*bigger), GFP_KERNEL);
	if (!src || !bigger) {
		kfree(src);
		kfree(bigger);
		v4l2_ctrl_handler_free(hdl);
		KUNIT_FAIL(test, "out of memory");
		return;
	}
	avd_av1_test_layout(expect, 2, 3);
	memcpy(src, expect, sizeof(expect));
	ctrl->handler = hdl;
	ctrl->elems = ARRAY_SIZE(expect);
	ctrl->elem_size = sizeof(*src);
	ctrl->p_cur.p = src;

	copy = avd_av1_copy_tile_group(ctrl, &entries);
	KUNIT_EXPECT_EQ(test, entries, ARRAY_SIZE(expect));

	/* A larger array replaces and frees the old storage. */
	memset(bigger, 0xff, V4L2_AV1_MAX_TILE_COUNT * sizeof(*bigger));
	ctrl->p_cur.p = bigger;
	ctrl->elems = V4L2_AV1_MAX_TILE_COUNT;
	memset(src, 0xff, sizeof(expect));
	kfree(src);

	if (copy) {
		KUNIT_EXPECT_MEMEQ(test, copy, expect, sizeof(expect));
		KUNIT_EXPECT_EQ(test, avd_av1_check_tiles(NULL, 2, 3, copy,
							  entries,
							  AVD_TEST_PAYLOAD), 0);
	} else {
		KUNIT_FAIL(test, "no copy");
	}
	KUNIT_EXPECT_EQ(test, entries, ARRAY_SIZE(expect));

	kfree(copy);
	kfree(bigger);
	v4l2_ctrl_handler_free(hdl);
}

struct avd_av1_size_case {
	const char *desc;
	u32 width;
	u32 height;
	u32 upscaled_width;
	bool superres;
	int ret;
	u8 denom;
};

/* Frames for 1920x1088 CAPTURE buffers. */
static const struct avd_av1_size_case avd_av1_size_cases[] = {
	{ "frame of the buffer size", 1920, 1088, 1920, false, 0 },
	{ "smaller frame", 1280, 720, 1280, false, 0 },
	{ "one sample", 1, 1, 1, false, 0 },
	{ "superres to the buffer width", 960, 1080, 1920, true, 0, 16 },
	{ "superres to a smaller width", 1000, 1080, 1280, true, 0, 16 },
	{ "superres with equal widths", 1920, 1080, 1920, true, 0, 16 },
	{ "frame wider than the buffer", 1921, 1080, 1921, false, -EINVAL },
	{ "frame higher than the buffer", 1920, 1089, 1920, false, -EINVAL },
	{ "frame width wraps", 0, 1080, 1920, false, -EINVAL },
	{ "frame height wraps", 1920, 0, 1920, false, -EINVAL },
	{ "no upscaled width", 1920, 1080, 0, false, -EINVAL },
	{ "no upscaled width, superres", 1920, 1080, 0, true, -EINVAL, 16 },
	{ "upscaled width below the frame's", 1920, 1080, 1919, true, -EINVAL, 16 },
	{ "upscaled width past the buffer", 960, 1080, 1921, true, -EINVAL, 16 },
	{ "huge upscaled width", 960, 1080, U32_MAX, true, -EINVAL, 16 },
	{ "upscaled width without superres", 960, 1080, 1920, false, -EINVAL },
	{ "superres, smallest denominator", 1072, 1080, 1920, true, 0, 9 },
	{ "superres, no denominator", 960, 1080, 1920, true, -EINVAL, 0 },
	{ "superres, denominator below 9", 960, 1080, 1920, true, -EINVAL, 8 },
};

KUNIT_ARRAY_PARAM_DESC(avd_av1_size, avd_av1_size_cases, desc);

static void avd_av1_frame_size_test(struct kunit *test)
{
	const struct avd_av1_size_case *c = test->param_value;
	struct v4l2_ctrl_av1_frame *frame;

	frame = kunit_kzalloc(test, sizeof(*frame), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, frame);
	frame->frame_width_minus_1 = c->width - 1;
	frame->frame_height_minus_1 = c->height - 1;
	frame->upscaled_width = c->upscaled_width;
	frame->superres_denom = c->denom;
	if (c->superres)
		frame->flags |= V4L2_AV1_FRAME_FLAG_USE_SUPERRES;

	KUNIT_EXPECT_EQ(test, avd_av1_check_frame_size(frame, 1920, 1088),
			c->ret);
}

static struct kunit_case avd_av1_test_cases[] = {
	KUNIT_CASE(avd_av1_tiles_valid_test),
	KUNIT_CASE(avd_av1_tiles_count_test),
	KUNIT_CASE(avd_av1_tiles_extent_test),
	KUNIT_CASE_PARAM(avd_av1_tiles_rowcol_test, avd_av1_rowcol_gen_params),
	KUNIT_CASE(avd_av1_copy_tile_group_test),
	KUNIT_CASE_PARAM(avd_av1_frame_size_test, avd_av1_size_gen_params),
	{}
};

static struct kunit_suite avd_av1_test_suite = {
	.name = "apple-avd-av1",
	.test_cases = avd_av1_test_cases,
};

kunit_test_suite(avd_av1_test_suite);
