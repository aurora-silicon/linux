// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the picture format the Apple video decoder derives from
 * the format controls when the OUTPUT format selects a codec.
 */

#include <kunit/test.h>
#include <linux/slab.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mem2mem.h>

#include "avd.h"

struct avd_v4l2_test {
	struct avd_dev dev;
	struct avd_ctx ctx;
};

static int avd_v4l2_test_init(struct kunit *test)
{
	struct avd_v4l2_test *t;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	t->dev.capabilities = AVD_CAPABILITY_H264 | AVD_CAPABILITY_VP9 |
			      AVD_CAPABILITY_HEVC | AVD_CAPABILITY_AV1;
	t->ctx.dev = &t->dev;
	t->ctx.image_fmt = AVD_IMG_FMT_ANY;
	/* s_ctrl looks at the CAPTURE queue; it has no buffers. */
	t->ctx.fh.m2m_ctx = kunit_kzalloc(test, sizeof(*t->ctx.fh.m2m_ctx),
					  GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->ctx.fh.m2m_ctx);
	/* The OUTPUT queue holds H.264 while the controls below are set. */
	t->ctx.coded_fmt_desc =
		avd_find_coded_fmt_desc(&t->ctx, V4L2_PIX_FMT_H264_SLICE);
	KUNIT_ASSERT_NOT_NULL(test, t->ctx.coded_fmt_desc);
	KUNIT_ASSERT_EQ(test, avd_init_ctrls(&t->ctx), 0);
	test->priv = t;
	return 0;
}

static void avd_v4l2_test_exit(struct kunit *test)
{
	struct avd_v4l2_test *t = test->priv;

	v4l2_ctrl_handler_free(&t->ctx.ctrl_hdl);
}

static enum avd_image_fmt avd_test_select(struct kunit *test, u32 fourcc)
{
	struct avd_v4l2_test *t = test->priv;
	const struct avd_coded_fmt_desc *desc;

	desc = avd_find_coded_fmt_desc(&t->ctx, fourcc);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	return avd_stored_image_fmt(&t->ctx, desc);
}

static void avd_test_set_vp9(struct kunit *test, u8 profile, u8 bit_depth)
{
	struct avd_v4l2_test *t = test->priv;
	struct v4l2_ctrl_vp9_frame frame = {
		.profile = profile,
		.bit_depth = bit_depth,
		.flags = V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING |
			 V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING,
	};
	enum avd_image_fmt before = t->ctx.image_fmt;
	struct v4l2_ctrl *ctrl;
	int ret;

	ctrl = v4l2_ctrl_find(&t->ctx.ctrl_hdl, V4L2_CID_STATELESS_VP9_FRAME);
	KUNIT_ASSERT_NOT_NULL(test, ctrl);
	ret = v4l2_ctrl_s_ctrl_compound(ctrl, V4L2_CTRL_TYPE_VP9_FRAME, &frame);
	KUNIT_ASSERT_EQ(test, ret, 0);
	/* H.264's s_ctrl ignored the VP9 frame. */
	KUNIT_EXPECT_EQ(test, t->ctx.image_fmt, before);
}

/* Profile 2 set before the switch to VP9: CAPTURE must be 10-bit. */
static void avd_vp9_frame_before_switch_test(struct kunit *test)
{
	avd_test_set_vp9(test, 2, 10);
	KUNIT_EXPECT_EQ(test, avd_test_select(test, V4L2_PIX_FMT_VP9_FRAME),
			AVD_IMG_FMT_420_10BIT);
	/* The other codecs' formats do not follow the VP9 control. */
	KUNIT_EXPECT_EQ(test, avd_test_select(test, V4L2_PIX_FMT_H264_SLICE),
			AVD_IMG_FMT_ANY);
	KUNIT_EXPECT_EQ(test, avd_test_select(test, V4L2_PIX_FMT_AV1_FRAME),
			AVD_IMG_FMT_ANY);
}

/* Never set, or set to 8-bit: the default stays, every format enumerable. */
static void avd_vp9_frame_default_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, avd_test_select(test, V4L2_PIX_FMT_VP9_FRAME),
			AVD_IMG_FMT_ANY);
	avd_test_set_vp9(test, 0, 8);
	KUNIT_EXPECT_EQ(test, avd_test_select(test, V4L2_PIX_FMT_VP9_FRAME),
			AVD_IMG_FMT_ANY);
}

/* A value VP9 refuses (profile 1) selects nothing; the frame is refused later. */
static void avd_vp9_frame_refused_test(struct kunit *test)
{
	avd_test_set_vp9(test, 1, 8);
	KUNIT_EXPECT_EQ(test, avd_test_select(test, V4L2_PIX_FMT_VP9_FRAME),
			AVD_IMG_FMT_ANY);
}

/* The same holds the other way round: an H.264 SPS set while VP9 was selected. */
static void avd_h264_sps_before_switch_test(struct kunit *test)
{
	struct avd_v4l2_test *t = test->priv;
	struct v4l2_ctrl_h264_sps sps = {
		.profile_idc = 110,
		.chroma_format_idc = 1,
		.bit_depth_luma_minus8 = 2,
		.bit_depth_chroma_minus8 = 2,
		.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY,
	};
	enum avd_image_fmt before = t->ctx.image_fmt;
	struct v4l2_ctrl *ctrl;
	int ret;

	t->ctx.coded_fmt_desc =
		avd_find_coded_fmt_desc(&t->ctx, V4L2_PIX_FMT_VP9_FRAME);
	ctrl = v4l2_ctrl_find(&t->ctx.ctrl_hdl, V4L2_CID_STATELESS_H264_SPS);
	KUNIT_ASSERT_NOT_NULL(test, ctrl);
	ret = v4l2_ctrl_s_ctrl_compound(ctrl, V4L2_CTRL_TYPE_H264_SPS, &sps);
	KUNIT_ASSERT_EQ(test, ret, 0);
	/* VP9's s_ctrl ignored the SPS. */
	KUNIT_EXPECT_EQ(test, t->ctx.image_fmt, before);
	KUNIT_EXPECT_EQ(test, avd_test_select(test, V4L2_PIX_FMT_H264_SLICE),
			AVD_IMG_FMT_420_10BIT);
}

static struct kunit_case avd_v4l2_test_cases[] = {
	KUNIT_CASE(avd_vp9_frame_before_switch_test),
	KUNIT_CASE(avd_vp9_frame_default_test),
	KUNIT_CASE(avd_vp9_frame_refused_test),
	KUNIT_CASE(avd_h264_sps_before_switch_test),
	{}
};

static struct kunit_suite avd_v4l2_test_suite = {
	.name = "apple-avd-v4l2",
	.init = avd_v4l2_test_init,
	.exit = avd_v4l2_test_exit,
	.test_cases = avd_v4l2_test_cases,
};

kunit_test_suite(avd_v4l2_test_suite);
