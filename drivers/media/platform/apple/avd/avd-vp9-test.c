// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the VP9 frame checks of the Apple video decoder.
 */

#include <kunit/test.h>
#include <media/v4l2-mem2mem.h>

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

#define AVD_TEST_MAX_TILES	2

struct avd_vp9_tiles_case {
	const char *desc;
	const u8 *data;
	u32 payload;
	u32 header;
	unsigned int tiles;
	int ret;
	u32 offsets[AVD_TEST_MAX_TILES];
	u32 sizes[AVD_TEST_MAX_TILES];
};

/* Two tiles after a 2-byte header: a 3-byte tile, then the rest. */
static const u8 avd_test_two_tiles[] = {
	0xaa, 0xbb, 0x00, 0x00, 0x00, 0x03, 0x01, 0x02, 0x03, 0x04, 0x05,
};

/* The first tile's size reaches one byte past the payload. */
static const u8 avd_test_long_tile[] = {
	0xaa, 0xbb, 0x00, 0x00, 0x00, 0x06, 0x01, 0x02, 0x03, 0x04, 0x05,
};

/* The first tile's size is the largest u32. */
static const u8 avd_test_huge_tile[] = {
	0xaa, 0xbb, 0xff, 0xff, 0xff, 0xff, 0x01, 0x02, 0x03, 0x04, 0x05,
};

/* The first tile fills the payload and leaves nothing for the last one. */
static const u8 avd_test_no_last_tile[] = {
	0xaa, 0xbb, 0x00, 0x00, 0x00, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05,
};

/* An empty first tile. */
static const u8 avd_test_empty_tile[] = {
	0xaa, 0xbb, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
};

static const struct avd_vp9_tiles_case avd_vp9_tiles_cases[] = {
	{ "one tile", avd_test_two_tiles, 11, 2, 1, 0, { 2 }, { 9 } },
	{ "two tiles", avd_test_two_tiles, 11, 2, 2, 0, { 6, 9 }, { 3, 2 } },
	{ "headers beyond the payload", avd_test_two_tiles, 11, 12, 1, -EINVAL },
	{ "headers fill the payload", avd_test_two_tiles, 11, 11, 1, -EINVAL },
	{ "headers beyond the payload, two tiles", avd_test_two_tiles, 11,
	  0xffff + 0xffff, 2, -EINVAL },
	{ "three bytes for a tile size", avd_test_two_tiles, 5, 2, 2, -EINVAL },
	{ "no bytes for a tile size", avd_test_two_tiles, 2, 2, 2, -EINVAL },
	{ "tile beyond the payload", avd_test_long_tile, 11, 2, 2, -EINVAL },
	{ "tile size wraps", avd_test_huge_tile, 11, 2, 2, -EINVAL },
	{ "no data for the last tile", avd_test_no_last_tile, 11, 2, 2, -EINVAL,
	  { 6 }, { 5 } },
	{ "empty tile", avd_test_empty_tile, 11, 2, 2, -EINVAL },
	{ "no mapping", NULL, 11, 2, 1, -EINVAL },
};

KUNIT_ARRAY_PARAM_DESC(avd_vp9_tiles, avd_vp9_tiles_cases, desc);

/* Walks the tiles as set_tiles() does. */
static void avd_vp9_tiles_test(struct kunit *test)
{
	const struct avd_vp9_tiles_case *c = test->param_value;
	u32 offset = c->header, size;
	int ret = 0;

	for (unsigned int i = 0; i < c->tiles; i++) {
		ret = avd_vp9_next_tile(c->data, c->payload, i == c->tiles - 1,
					&offset, &size);
		if (ret)
			break;
		KUNIT_EXPECT_EQ(test, offset, c->offsets[i]);
		KUNIT_EXPECT_EQ(test, size, c->sizes[i]);
		KUNIT_EXPECT_LE(test, (u64)offset + size, (u64)c->payload);
		offset += size;
	}
	KUNIT_EXPECT_EQ(test, ret, c->ret);
}

/* The frame size against a 1920x1088 coded format. */
static void avd_vp9_frame_fits_test(struct kunit *test)
{
	struct v4l2_ctrl_vp9_frame frame = {
		.frame_width_minus_1 = 1919,
		.frame_height_minus_1 = 1079,
	};

	KUNIT_EXPECT_TRUE(test, avd_vp9_frame_fits(&frame, 1920, 1088));
	frame.frame_height_minus_1 = 1087;
	KUNIT_EXPECT_TRUE(test, avd_vp9_frame_fits(&frame, 1920, 1088));
	frame.frame_width_minus_1 = 959;
	frame.frame_height_minus_1 = 539;
	KUNIT_EXPECT_TRUE(test, avd_vp9_frame_fits(&frame, 1920, 1088));

	frame.frame_width_minus_1 = 1920;
	frame.frame_height_minus_1 = 1079;
	KUNIT_EXPECT_FALSE(test, avd_vp9_frame_fits(&frame, 1920, 1088));
	frame.frame_width_minus_1 = 1919;
	frame.frame_height_minus_1 = 1088;
	KUNIT_EXPECT_FALSE(test, avd_vp9_frame_fits(&frame, 1920, 1088));
	frame.frame_width_minus_1 = 4095;
	frame.frame_height_minus_1 = 2175;
	KUNIT_EXPECT_FALSE(test, avd_vp9_frame_fits(&frame, 64, 64));
	frame.frame_width_minus_1 = 0xffff;
	frame.frame_height_minus_1 = 0xffff;
	KUNIT_EXPECT_FALSE(test, avd_vp9_frame_fits(&frame, 4096, 4096));
}

/*
 * A frame larger than the OUTPUT format is refused when the control is set,
 * once the OUTPUT queue streams and the format can no longer follow it.
 */
static void avd_vp9_try_frame_size_test(struct kunit *test)
{
	static const struct v4l2_ctrl_config cfg = {
		.id = V4L2_CID_STATELESS_VP9_FRAME,
	};
	struct v4l2_m2m_ctx *m2m_ctx;
	struct v4l2_ctrl_vp9_frame *frame;
	struct v4l2_ctrl *ctrl;
	struct avd_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	m2m_ctx = kunit_kzalloc(test, sizeof(*m2m_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, m2m_ctx);
	ctx->fh.m2m_ctx = m2m_ctx;
	ctx->coded_fmt.fmt.pix_mp.width = 1920;
	ctx->coded_fmt.fmt.pix_mp.height = 1088;

	v4l2_ctrl_handler_init(&ctx->ctrl_hdl, 1);
	ctrl = v4l2_ctrl_new_custom(&ctx->ctrl_hdl, &cfg, NULL);
	KUNIT_ASSERT_EQ(test, ctx->ctrl_hdl.error, 0);
	KUNIT_ASSERT_NOT_NULL(test, ctrl);
	frame = ctrl->p_new.p_vp9_frame;

	/* Not streaming: the format may still be set to the stream's size. */
	frame->frame_width_minus_1 = 4095;
	frame->frame_height_minus_1 = 2159;
	KUNIT_EXPECT_EQ(test, avd_vp9_fmt_ops.try_ctrl(ctx, ctrl), 0);

	m2m_ctx->out_q_ctx.q.streaming = 1;
	KUNIT_EXPECT_EQ(test, avd_vp9_fmt_ops.try_ctrl(ctx, ctrl), -EINVAL);
	frame->frame_width_minus_1 = 1919;
	frame->frame_height_minus_1 = 1088;
	KUNIT_EXPECT_EQ(test, avd_vp9_fmt_ops.try_ctrl(ctx, ctrl), -EINVAL);
	frame->frame_height_minus_1 = 1087;
	KUNIT_EXPECT_EQ(test, avd_vp9_fmt_ops.try_ctrl(ctx, ctrl), 0);
	frame->frame_width_minus_1 = 1279;
	frame->frame_height_minus_1 = 719;
	KUNIT_EXPECT_EQ(test, avd_vp9_fmt_ops.try_ctrl(ctx, ctrl), 0);

	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
}

static struct kunit_case avd_vp9_test_cases[] = {
	KUNIT_CASE(avd_vp9_frame_fits_test),
	KUNIT_CASE(avd_vp9_try_frame_size_test),
	KUNIT_CASE_PARAM(avd_vp9_tiling_test, avd_vp9_tiling_gen_params),
	KUNIT_CASE_PARAM(avd_vp9_tiles_test, avd_vp9_tiles_gen_params),
	{}
};

static struct kunit_suite avd_vp9_test_suite = {
	.name = "apple-avd-vp9",
	.test_cases = avd_vp9_test_cases,
};

kunit_test_suite(avd_vp9_test_suite);
