// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the format ioctls and the control helpers of the Apple
 * video decoder.
 */

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
#include <media/v4l2-mem2mem.h>

#include "avd.h"

static const struct avd_coded_fmt_ops avd_test_ops = {};

static const struct avd_decoded_fmt_desc avd_test_decoded_fmts[] = {
	{ .fourcc = V4L2_PIX_FMT_NV12, .image_fmt = AVD_IMG_FMT_420_8BIT },
};

static const struct avd_coded_fmt_desc avd_test_coded_desc = {
	.fourcc = V4L2_PIX_FMT_H264_SLICE,
	.frmsize = {
		.min_width = 64, .max_width = 4096, .step_width = 64,
		.min_height = 64, .max_height = 4096, .step_height = 16,
	},
	.ops = &avd_test_ops,
	.num_decoded_fmts = ARRAY_SIZE(avd_test_decoded_fmts),
	.decoded_fmts = avd_test_decoded_fmts,
};

struct avd_test_fmt {
	struct avd_ctx *ctx;
	struct file *file;
	struct vb2_queue *cap_q;
};

/* A 64x64 context with its default CAPTURE format, as after open(). */
static void avd_test_fmt_init(struct kunit *test, struct avd_test_fmt *t)
{
	struct v4l2_m2m_ctx *m2m_ctx;

	t->ctx = kunit_kzalloc(test, sizeof(*t->ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->ctx);
	m2m_ctx = kunit_kzalloc(test, sizeof(*m2m_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, m2m_ctx);
	t->file = kunit_kzalloc(test, sizeof(*t->file), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->file);

	t->ctx->fh.m2m_ctx = m2m_ctx;
	t->file->private_data = &t->ctx->fh;
	t->cap_q = &m2m_ctx->cap_q_ctx.q;

	t->ctx->coded_fmt_desc = &avd_test_coded_desc;
	t->ctx->coded_fmt.fmt.pix_mp.width = 64;
	t->ctx->coded_fmt.fmt.pix_mp.height = 64;
	t->ctx->image_fmt = AVD_IMG_FMT_420_8BIT;
	avd_reset_decoded_fmt(t->ctx);
}

static void avd_test_capture_fmt(struct v4l2_format *f, u32 width, u32 height)
{
	memset(f, 0, sizeof(*f));
	f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	f->fmt.pix_mp.width = width;
	f->fmt.pix_mp.height = height;
}

static void avd_test_expect_comp(struct kunit *test,
				 const struct avd_comp *got,
				 const struct avd_comp *want)
{
	KUNIT_EXPECT_EQ(test, got->size, want->size);
	KUNIT_EXPECT_EQ(test, got->start_offset, want->start_offset);
	KUNIT_EXPECT_MEMEQ(test, got->offsets, want->offsets,
			   sizeof(want->offsets));
}

/* TRY_FMT(CAPTURE) must leave the context's buffer layout alone. */
static void avd_try_capture_fmt_test(struct kunit *test)
{
	struct avd_test_fmt t;
	struct v4l2_format f, decoded;
	struct avd_comp comp;

	avd_test_fmt_init(test, &t);
	comp = t.ctx->comp;
	decoded = t.ctx->decoded_fmt;
	KUNIT_ASSERT_NE(test, comp.size, 0);

	/* CAPTURE buffers allocated, as when streaming. */
	t.cap_q->is_busy = 1;
	avd_test_capture_fmt(&f, 4096, 4096);
	KUNIT_EXPECT_EQ(test, avd_try_capture_fmt(t.file, NULL, &f), 0);
	KUNIT_EXPECT_EQ(test, f.fmt.pix_mp.width, 4096);
	KUNIT_EXPECT_GT(test, f.fmt.pix_mp.plane_fmt[0].sizeimage,
			decoded.fmt.pix_mp.plane_fmt[0].sizeimage);
	avd_test_expect_comp(test, &t.ctx->comp, &comp);
	KUNIT_EXPECT_MEMEQ(test, &t.ctx->decoded_fmt, &decoded, sizeof(decoded));

	/* S_FMT is refused on the busy queue and changes nothing either. */
	avd_test_capture_fmt(&f, 4096, 4096);
	KUNIT_EXPECT_EQ(test, avd_s_capture_fmt(t.file, NULL, &f), -EBUSY);
	avd_test_expect_comp(test, &t.ctx->comp, &comp);

	/* The same holds with no buffers. */
	t.cap_q->is_busy = 0;
	avd_test_capture_fmt(&f, 1920, 1088);
	KUNIT_EXPECT_EQ(test, avd_try_capture_fmt(t.file, NULL, &f), 0);
	avd_test_expect_comp(test, &t.ctx->comp, &comp);
	KUNIT_EXPECT_MEMEQ(test, &t.ctx->decoded_fmt, &decoded, sizeof(decoded));
}

/* S_FMT(CAPTURE) lays out the compressed area for the format it sets. */
static void avd_s_capture_fmt_test(struct kunit *test)
{
	struct avd_test_fmt t;
	struct v4l2_format f;
	struct avd_comp want = {};

	avd_test_fmt_init(test, &t);

	avd_test_capture_fmt(&f, 1920, 1088);
	KUNIT_ASSERT_EQ(test, avd_s_capture_fmt(t.file, NULL, &f), 0);

	want.start_offset = 1920 * 1088 * 3 / 2;
	fill_comp(&want, AVD_IMG_FMT_420_8BIT, 1920, 1088);
	avd_test_expect_comp(test, &t.ctx->comp, &want);
	KUNIT_EXPECT_EQ(test, t.ctx->decoded_fmt.fmt.pix_mp.width, 1920);
	KUNIT_EXPECT_EQ(test, t.ctx->decoded_fmt.fmt.pix_mp.height, 1088);
	KUNIT_EXPECT_EQ(test,
			t.ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
			want.start_offset + want.size);

	/* A later TRY_FMT reports its own layout but stores nothing. */
	avd_test_capture_fmt(&f, 4096, 2176);
	KUNIT_EXPECT_EQ(test, avd_try_capture_fmt(t.file, NULL, &f), 0);
	avd_test_expect_comp(test, &t.ctx->comp, &want);
}

/* A context whose control handler holds the VP9 frame control. */
static struct v4l2_ctrl *avd_test_ctrl_init(struct kunit *test,
					    struct avd_ctx **ctxp)
{
	static const struct v4l2_ctrl_config cfg = {
		.id = V4L2_CID_STATELESS_VP9_FRAME,
	};
	struct avd_ctx *ctx;
	struct v4l2_ctrl *ctrl;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	v4l2_ctrl_handler_init(&ctx->ctrl_hdl, 1);
	ctrl = v4l2_ctrl_new_custom(&ctx->ctrl_hdl, &cfg, NULL);
	KUNIT_ASSERT_EQ(test, ctx->ctrl_hdl.error, 0);
	KUNIT_ASSERT_NOT_NULL(test, ctrl);

	*ctxp = ctx;
	return ctrl;
}

static int avd_test_set_vp9_width(struct v4l2_ctrl *ctrl, u16 width)
{
	struct v4l2_ctrl_vp9_frame frame = *ctrl->p_cur.p_vp9_frame;

	frame.frame_width_minus_1 = width - 1;
	frame.frame_height_minus_1 = width - 1;
	return v4l2_ctrl_s_ctrl_compound(ctrl, V4L2_CTRL_TYPE_VP9_FRAME,
					 &frame);
}

/* A copy keeps the value it was taken from when the control is set again. */
static void avd_ctrl_copy_test(struct kunit *test)
{
	struct v4l2_ctrl_vp9_frame *copy;
	struct v4l2_ctrl *ctrl;
	struct avd_ctx *ctx;

	ctrl = avd_test_ctrl_init(test, &ctx);
	copy = kunit_kzalloc(test, sizeof(*copy) + 1, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, copy);

	KUNIT_ASSERT_EQ(test, avd_test_set_vp9_width(ctrl, 64), 0);
	KUNIT_ASSERT_EQ(test, avd_ctrl_copy(ctx, V4L2_CID_STATELESS_VP9_FRAME,
					    copy, sizeof(*copy)), 0);
	KUNIT_EXPECT_MEMEQ(test, copy, ctrl->p_cur.p, sizeof(*copy));
	KUNIT_EXPECT_TRUE(test, avd_vp9_frame_fits(copy, 64, 64));

	KUNIT_ASSERT_EQ(test, avd_test_set_vp9_width(ctrl, 4096), 0);
	KUNIT_EXPECT_FALSE(test, avd_vp9_frame_fits(ctrl->p_cur.p_vp9_frame,
						    64, 64));
	KUNIT_EXPECT_EQ(test, copy->frame_width_minus_1, 63);
	KUNIT_EXPECT_EQ(test, copy->frame_height_minus_1, 63);
	KUNIT_EXPECT_TRUE(test, avd_vp9_frame_fits(copy, 64, 64));

	/* Only a destination of the control's own size is filled. */
	memset(copy, 0, sizeof(*copy));
	KUNIT_EXPECT_EQ(test, avd_ctrl_copy(ctx, V4L2_CID_STATELESS_VP9_FRAME,
					    copy, sizeof(*copy) - 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, avd_ctrl_copy(ctx, V4L2_CID_STATELESS_VP9_FRAME,
					    copy, sizeof(*copy) + 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, copy->frame_width_minus_1, 0);
	/* So is a control that the handler does not have. */
	KUNIT_EXPECT_EQ(test,
			avd_ctrl_copy(ctx, V4L2_CID_STATELESS_VP9_COMPRESSED_HDR,
				      copy, sizeof(*copy)), -EINVAL);

	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
}

struct avd_test_copy_work {
	struct work_struct work;
	struct completion done;
	struct v4l2_ctrl *ctrl;
	struct v4l2_ctrl_vp9_frame copy;
	int ret;
};

static void avd_test_copy_work_fn(struct work_struct *work)
{
	struct avd_test_copy_work *w =
		container_of(work, struct avd_test_copy_work, work);

	w->ret = avd_ctrl_copy_value(w->ctrl, &w->copy, sizeof(w->copy));
	complete(&w->done);
}

/* The copy waits for whoever holds the control lock and sees its whole update. */
static void avd_ctrl_copy_locked_test(struct kunit *test)
{
	struct avd_test_copy_work *w;
	struct v4l2_ctrl *ctrl;
	struct avd_ctx *ctx;

	ctrl = avd_test_ctrl_init(test, &ctx);
	w = kunit_kzalloc(test, sizeof(*w), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, w);
	w->ctrl = ctrl;
	w->ret = -EINPROGRESS;
	init_completion(&w->done);
	INIT_WORK(&w->work, avd_test_copy_work_fn);

	KUNIT_ASSERT_EQ(test, avd_test_set_vp9_width(ctrl, 64), 0);

	v4l2_ctrl_lock(ctrl);
	ctrl->p_cur.p_vp9_frame->frame_width_minus_1 = 4095;
	schedule_work(&w->work);
	KUNIT_EXPECT_EQ(test,
			wait_for_completion_timeout(&w->done,
						    msecs_to_jiffies(200)), 0);
	KUNIT_EXPECT_EQ(test, w->ret, -EINPROGRESS);
	ctrl->p_cur.p_vp9_frame->frame_height_minus_1 = 4095;
	v4l2_ctrl_unlock(ctrl);

	flush_work(&w->work);
	KUNIT_EXPECT_EQ(test, w->ret, 0);
	KUNIT_EXPECT_EQ(test, w->copy.frame_width_minus_1, 4095);
	KUNIT_EXPECT_EQ(test, w->copy.frame_height_minus_1, 4095);

	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
}

static struct kunit_case avd_v4l2_test_cases[] = {
	KUNIT_CASE(avd_try_capture_fmt_test),
	KUNIT_CASE(avd_s_capture_fmt_test),
	KUNIT_CASE(avd_ctrl_copy_test),
	KUNIT_CASE(avd_ctrl_copy_locked_test),
	{}
};

static struct kunit_suite avd_v4l2_test_suite = {
	.name = "apple-avd-v4l2",
	.test_cases = avd_v4l2_test_cases,
};

kunit_test_suite(avd_v4l2_test_suite);
