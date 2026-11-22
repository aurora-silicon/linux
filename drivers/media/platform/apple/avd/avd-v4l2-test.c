// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the format ioctls of the Apple video decoder.
 */

#include <kunit/test.h>
#include <linux/fs.h>
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

static struct kunit_case avd_v4l2_test_cases[] = {
	KUNIT_CASE(avd_try_capture_fmt_test),
	KUNIT_CASE(avd_s_capture_fmt_test),
	{}
};

static struct kunit_suite avd_v4l2_test_suite = {
	.name = "apple-avd-v4l2",
	.test_cases = avd_v4l2_test_cases,
};

kunit_test_suite(avd_v4l2_test_suite);
