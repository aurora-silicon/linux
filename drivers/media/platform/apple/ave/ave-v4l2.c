// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE video encoder: V4L2 stateful encoder interface
 *
 * Follows Documentation/userspace-api/media/v4l/dev-encoder.rst: the CAPTURE
 * queue carries the coded stream (H.264 or HEVC), the OUTPUT queue raw NV12.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/delay.h>
#include <linux/log2.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "ave.h"
#include "ave-regs.h"

#define AVE_NAME		"apple-ave"

/*
 * Frame size limits and input stride alignment: the range the encoder was
 * seen working at (320x240 up to the 2408x1506 panel), with a little room.
 * Larger sizes need buffer sizes nobody has checked yet.
 */
#define AVE_MIN_W		320
#define AVE_MIN_H		240
#define AVE_MAX_W		4096
#define AVE_MAX_H		2304
#define AVE_STEP		16
#define AVE_STRIDE_ALIGN	64
#define AVE_DEFAULT_W		1920
#define AVE_DEFAULT_H		1080
/*
 * Buffer counts are limited so the queues of a few concurrent sessions fit
 * the encoder's 4 GiB DART window next to their session buffers: each queue
 * may use this much, counting every buffer at the power-of-two size the DMA
 * layer maps it with. That is 16 OUTPUT frames at 3840x2160.
 */
#define AVE_OUTPUT_BUDGET	(256 * SZ_1M)
#define AVE_CAPTURE_BUDGET	(128 * SZ_1M)
#define AVE_MAX_QUEUE_BUFS	16
#define AVE_RESTART_TRIES	10
#define AVE_RESTART_WAIT_MS	100

static const u32 ave_coded_fmts[] = {
	V4L2_PIX_FMT_H264,
	V4L2_PIX_FMT_HEVC,
};

static const u32 ave_raw_fmts[] = {
	V4L2_PIX_FMT_NV12,
};

static const struct v4l2_event ave_eos_event = {
	.type = V4L2_EVENT_EOS,
};

enum ave_codec ave_ctx_codec(struct ave_ctx *ctx)
{
	return ctx->dst_fmt.pixelformat == V4L2_PIX_FMT_HEVC ? AVE_CODEC_HEVC :
							       AVE_CODEC_H264;
}

static bool ave_fmt_listed(const u32 *list, unsigned int n, u32 fourcc)
{
	for (unsigned int i = 0; i < n; i++)
		if (list[i] == fourcc)
			return true;
	return false;
}

/* HEVC Main tier levels 5 to 5.2 allow at most this many luma samples. */
#define AVE_HEVC_MAX_SAMPLES	8912896

/* @coded is the coded format the size is for: HEVC caps the picture area. */
static void ave_clamp_size(u32 coded, u32 *w, u32 *h)
{
	*w = clamp_t(u32, ALIGN(*w, 2), AVE_MIN_W, AVE_MAX_W);
	*h = clamp_t(u32, ALIGN(*h, 2), AVE_MIN_H, AVE_MAX_H);
	if (coded == V4L2_PIX_FMT_HEVC && *w * *h > AVE_HEVC_MAX_SAMPLES)
		*h = rounddown(AVE_HEVC_MAX_SAMPLES / *w, 2);
}

/*
 * Raw NV12, one buffer: Y plane, then interleaved CbCr at half height.
 * @coded is the context's coded format, which limits the size.
 */
static void ave_fill_raw(struct v4l2_pix_format_mplane *f, u32 coded)
{
	u32 stride, height;

	f->pixelformat = V4L2_PIX_FMT_NV12;
	ave_clamp_size(coded, &f->width, &f->height);
	/* Keep a client's stride, within reason, rounded to 64 bytes. */
	stride = ALIGN(clamp_t(u32, f->plane_fmt[0].bytesperline, f->width, 2 * AVE_MAX_W),
		       AVE_STRIDE_ALIGN);
	height = ALIGN(f->height, AVE_STEP);
	f->num_planes = 1;
	f->plane_fmt[0].bytesperline = stride;
	f->plane_fmt[0].sizeimage = stride * height * 3 / 2;
	f->field = V4L2_FIELD_NONE;
	memset(f->plane_fmt[0].reserved, 0, sizeof(f->plane_fmt[0].reserved));
	memset(f->reserved, 0, sizeof(f->reserved));
}

static void ave_fill_coded(struct v4l2_pix_format_mplane *f)
{
	u32 min;

	if (!ave_fmt_listed(ave_coded_fmts, ARRAY_SIZE(ave_coded_fmts), f->pixelformat))
		f->pixelformat = ave_coded_fmts[0];
	ave_clamp_size(f->pixelformat, &f->width, &f->height);
	/* The firmware's coded-data capacity plus parameter sets and headers. */
	min = ALIGN(3 * ALIGN(f->width, 32) * ALIGN(f->height, 16) / 2, SZ_16K) + SZ_64K;
	f->num_planes = 1;
	f->plane_fmt[0].bytesperline = 0;
	f->plane_fmt[0].sizeimage = clamp_t(u32, f->plane_fmt[0].sizeimage, min, SZ_32M);
	f->field = V4L2_FIELD_NONE;
	memset(f->plane_fmt[0].reserved, 0, sizeof(f->plane_fmt[0].reserved));
	memset(f->reserved, 0, sizeof(f->reserved));
}

static void ave_reset_fmts(struct ave_ctx *ctx)
{
	struct v4l2_pix_format_mplane *src = &ctx->src_fmt, *dst = &ctx->dst_fmt;

	memset(dst, 0, sizeof(*dst));
	dst->pixelformat = V4L2_PIX_FMT_H264;
	dst->width = AVE_DEFAULT_W;
	dst->height = AVE_DEFAULT_H;
	ave_fill_coded(dst);

	memset(src, 0, sizeof(*src));
	src->width = dst->width;
	src->height = dst->height;
	ave_fill_raw(src, dst->pixelformat);
	src->colorspace = V4L2_COLORSPACE_REC709;
	src->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	src->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	src->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	dst->colorspace = src->colorspace;
	dst->xfer_func = src->xfer_func;
	dst->ycbcr_enc = src->ycbcr_enc;
	dst->quantization = src->quantization;

	ctx->timeperframe = (struct v4l2_fract){ 1, 30 };
}

/* ioctls */

static int ave_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, AVE_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Apple AVE video encoder", sizeof(cap->card));
	return 0;
}

static int ave_enum_fmt_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(ave_coded_fmts))
		return -EINVAL;
	f->pixelformat = ave_coded_fmts[f->index];
	f->flags = V4L2_FMT_FLAG_COMPRESSED;
	return 0;
}

static int ave_enum_fmt_out(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(ave_raw_fmts))
		return -EINVAL;
	f->pixelformat = ave_raw_fmts[f->index];
	return 0;
}

static int ave_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *fs)
{
	if (fs->index ||
	    (!ave_fmt_listed(ave_coded_fmts, ARRAY_SIZE(ave_coded_fmts), fs->pixel_format) &&
	     !ave_fmt_listed(ave_raw_fmts, ARRAY_SIZE(ave_raw_fmts), fs->pixel_format)))
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fs->stepwise = (struct v4l2_frmsize_stepwise){
		AVE_MIN_W, AVE_MAX_W, 2, AVE_MIN_H, AVE_MAX_H, 2,
	};
	return 0;
}

static int ave_g_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = ave_file_to_ctx(file)->dst_fmt;
	return 0;
}

static int ave_g_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = ave_file_to_ctx(file)->src_fmt;
	return 0;
}

static int ave_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	ave_fill_coded(&f->fmt.pix_mp);
	return 0;
}

static int ave_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ave_ctx *ctx = ave_file_to_ctx(file);
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;

	ave_fill_raw(pix, ctx->dst_fmt.pixelformat);
	if (pix->colorspace == V4L2_COLORSPACE_DEFAULT) {
		pix->colorspace = ctx->src_fmt.colorspace;
		pix->xfer_func = ctx->src_fmt.xfer_func;
		pix->ycbcr_enc = ctx->src_fmt.ycbcr_enc;
		pix->quantization = ctx->src_fmt.quantization;
	}
	return 0;
}

static int ave_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ave_ctx *ctx = ave_file_to_ctx(file);
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;

	if (vb2_is_busy(vq))
		return -EBUSY;
	/* The coded format comes first; the raw side follows its size. */
	ave_fill_coded(pix);
	pix->colorspace = ctx->src_fmt.colorspace;
	pix->xfer_func = ctx->src_fmt.xfer_func;
	pix->ycbcr_enc = ctx->src_fmt.ycbcr_enc;
	pix->quantization = ctx->src_fmt.quantization;
	ctx->dst_fmt = *pix;
	return 0;
}

static int ave_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ave_ctx *ctx = ave_file_to_ctx(file);
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
	int ret;

	if (vb2_is_busy(vq))
		return -EBUSY;
	ret = ave_try_fmt_out(file, priv, f);
	if (ret)
		return ret;
	ctx->src_fmt = *pix;

	/* The coded size follows the raw size (crop is the whole frame). */
	ctx->dst_fmt.width = pix->width;
	ctx->dst_fmt.height = pix->height;
	ctx->dst_fmt.colorspace = pix->colorspace;
	ctx->dst_fmt.xfer_func = pix->xfer_func;
	ctx->dst_fmt.ycbcr_enc = pix->ycbcr_enc;
	ctx->dst_fmt.quantization = pix->quantization;
	ave_fill_coded(&ctx->dst_fmt);
	return 0;
}

static int ave_g_selection(struct file *file, void *priv, struct v4l2_selection *s)
{
	struct ave_ctx *ctx = ave_file_to_ctx(file);

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;
	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		s->r = (struct v4l2_rect){ 0, 0, ctx->src_fmt.width, ctx->src_fmt.height };
		return 0;
	default:
		return -EINVAL;
	}
}

static int ave_s_selection(struct file *file, void *priv, struct v4l2_selection *s)
{
	struct ave_ctx *ctx = ave_file_to_ctx(file);

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT || s->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;
	/* Only full-frame encoding for now. */
	s->r = (struct v4l2_rect){ 0, 0, ctx->src_fmt.width, ctx->src_fmt.height };
	return 0;
}

static int ave_g_parm(struct file *file, void *priv, struct v4l2_streamparm *a)
{
	struct ave_ctx *ctx = ave_file_to_ctx(file);

	if (!V4L2_TYPE_IS_OUTPUT(a->type))
		return -EINVAL;
	a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.output.timeperframe = ctx->timeperframe;
	return 0;
}

static int ave_s_parm(struct file *file, void *priv, struct v4l2_streamparm *a)
{
	struct ave_ctx *ctx = ave_file_to_ctx(file);
	struct v4l2_fract *t = &a->parm.output.timeperframe;

	if (!V4L2_TYPE_IS_OUTPUT(a->type))
		return -EINVAL;
	if (!t->numerator || !t->denominator)
		*t = ctx->timeperframe;
	/* 1 to 240 frames per second. */
	if ((u64)t->denominator > (u64)t->numerator * 240)
		*t = (struct v4l2_fract){ 1, 240 };
	if (t->numerator > t->denominator)
		*t = (struct v4l2_fract){ 1, 1 };
	ctx->timeperframe = *t;
	a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
	return 0;
}

/*
 * Drain: frames still inside the firmware (B-frames) only come out after a
 * flush, so the driver tracks the drain point itself instead of using the
 * m2m helpers, which would mark the next CAPTURE buffer last right away.
 */
static int ave_encoder_cmd(struct file *file, void *priv, struct v4l2_encoder_cmd *ec)
{
	struct ave_ctx *ctx = ave_file_to_ctx(file);
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	int ret;

	ret = v4l2_m2m_ioctl_try_encoder_cmd(file, priv, ec);
	if (ret)
		return ret;
	if (!vb2_is_streaming(v4l2_m2m_get_src_vq(m2m)) ||
	    !vb2_is_streaming(v4l2_m2m_get_dst_vq(m2m)))
		return 0;

	if (ec->cmd == V4L2_ENC_CMD_STOP) {
		if (ctx->draining || ctx->stopped)
			return 0;
		ctx->drain_src = v4l2_m2m_last_src_buf(m2m);
		ctx->draining = true;
		if (!ctx->drain_src)
			WRITE_ONCE(ctx->flush_pending, true);
	} else {
		if (ctx->draining && !ctx->stopped)
			return -EBUSY;
		ctx->draining = false;
		ctx->stopped = false;
		vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(m2m));
	}
	v4l2_m2m_try_schedule(m2m);
	return 0;
}

static int ave_subscribe_event(struct v4l2_fh *fh, const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_EOS:
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	default:
		return v4l2_ctrl_subscribe_event(fh, sub);
	}
}

static const struct v4l2_ioctl_ops ave_ioctl_ops = {
	.vidioc_querycap = ave_querycap,
	.vidioc_enum_fmt_vid_cap = ave_enum_fmt_cap,
	.vidioc_enum_fmt_vid_out = ave_enum_fmt_out,
	.vidioc_enum_framesizes = ave_enum_framesizes,
	.vidioc_g_fmt_vid_cap_mplane = ave_g_fmt_cap,
	.vidioc_g_fmt_vid_out_mplane = ave_g_fmt_out,
	.vidioc_try_fmt_vid_cap_mplane = ave_try_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = ave_try_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = ave_s_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane = ave_s_fmt_out,
	.vidioc_g_selection = ave_g_selection,
	.vidioc_s_selection = ave_s_selection,
	.vidioc_g_parm = ave_g_parm,
	.vidioc_s_parm = ave_s_parm,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_try_encoder_cmd = v4l2_m2m_ioctl_try_encoder_cmd,
	.vidioc_encoder_cmd = ave_encoder_cmd,

	.vidioc_subscribe_event = ave_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* Controls */

static int ave_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ave_ctx *ctx = container_of(ctrl->handler, struct ave_ctx, hdl);

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_BITRATE_MODE:
		ctx->bitrate_mode = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		WRITE_ONCE(ctx->bitrate, ctrl->val);
		break;
	case V4L2_CID_MPEG_VIDEO_BITRATE_PEAK:
		ctx->bitrate_peak = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		ctx->gop_size = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
		WRITE_ONCE(ctx->force_key_frame, true);
		break;
	case V4L2_CID_MPEG_VIDEO_B_FRAMES:
		ctx->b_frames = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		ctx->h264_profile = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		ctx->h264_level = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_MIN_QP:
		ctx->h264_min_qp = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_MAX_QP:
		ctx->h264_max_qp = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_LEVEL:
		ctx->hevc_level = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP:
		ctx->hevc_min_qp = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP:
		ctx->hevc_max_qp = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEADER_MODE:
	case V4L2_CID_MPEG_VIDEO_HEVC_PROFILE:
	case V4L2_CID_MIN_BUFFERS_FOR_OUTPUT:
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct v4l2_ctrl_ops ave_ctrl_ops = {
	.s_ctrl = ave_s_ctrl,
};

static int ave_init_ctrls(struct ave_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->hdl;
	const struct v4l2_ctrl_ops *ops = &ave_ctrl_ops;
	struct v4l2_ctrl *c;

	v4l2_ctrl_handler_init(hdl, 17);

	/* Constant quality needs a quality-to-QP mapping that is not known yet. */
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_CBR,
			       BIT(V4L2_MPEG_VIDEO_BITRATE_MODE_CQ),
			       V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_BITRATE, 100000, 100000000, 1,
			  4000000);
	/*
	 * In VBR mode the peak caps the data rate over any one second;
	 * 0 means twice the average, as macOS sets it.
	 */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, 0, 200000000, 1, 0);
	/* 0 means no limit; the firmware then uses 120 frames. */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_GOP_SIZE, 0, 65535, 1, 30);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, 0, 0, 0);
	/*
	 * The firmware either uses up to three B-frames (reordering them
	 * itself) or none. Baseline and intra-only H.264 never have any.
	 */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, 3, 3, 3);
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_HEADER_MODE,
			       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME,
			       BIT(V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE),
			       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME);

	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
			       V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
			       ~(BIT(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) |
				 BIT(V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE) |
				 BIT(V4L2_MPEG_VIDEO_H264_PROFILE_MAIN) |
				 BIT(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)),
			       V4L2_MPEG_VIDEO_H264_PROFILE_MAIN);
	/* A minimum: the driver raises it to what size, rate and bit rate need. */
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_LEVEL,
			       V4L2_MPEG_VIDEO_H264_LEVEL_5_2, 0,
			       V4L2_MPEG_VIDEO_H264_LEVEL_1_0);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 0, 51, 1, 0);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 0, 51, 1, 51);

	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN, 0,
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN);
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1, 0,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_1);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP, 0, 51, 1, 0);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP, 0, 51, 1, 51);

	/* The firmware holds up to five frames; one more keeps it fed. */
	c = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, 1, 32, 1,
			      AVE_MAX_SLOTS + 1);
	if (c)
		c->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (hdl->error) {
		int ret = hdl->error;

		v4l2_ctrl_handler_free(hdl);
		return ret;
	}
	ctx->fh.ctrl_handler = hdl;
	return v4l2_ctrl_handler_setup(hdl);
}

/* Queues */

static int ave_queue_setup(struct vb2_queue *vq, unsigned int *nbufs, unsigned int *nplanes,
			   unsigned int sizes[], struct device *alloc_devs[])
{
	struct ave_ctx *ctx = vb2_get_drv_priv(vq);
	const struct v4l2_pix_format_mplane *f =
		V4L2_TYPE_IS_OUTPUT(vq->type) ? &ctx->src_fmt : &ctx->dst_fmt;
	unsigned int have = vb2_get_num_buffers(vq);
	unsigned long slot = roundup_pow_of_two(PAGE_ALIGN(f->plane_fmt[0].sizeimage));
	unsigned int max = (V4L2_TYPE_IS_OUTPUT(vq->type) ? AVE_OUTPUT_BUDGET :
							    AVE_CAPTURE_BUDGET) / slot;

	max = clamp(max, V4L2_TYPE_IS_OUTPUT(vq->type) ? AVE_MAX_SLOTS + 1U : 2U,
		    AVE_MAX_QUEUE_BUFS);

	/* Frames stay with the firmware until encoded; keep one spare. */
	if (V4L2_TYPE_IS_OUTPUT(vq->type) && have + *nbufs < AVE_MAX_SLOTS + 1)
		*nbufs = AVE_MAX_SLOTS + 1 - have;
	/* A full set of both queues must fit the budget for the DART window. */
	if (have >= max) {
		dev_dbg(ctx->ave->dev, "%s queue already has its %u buffers\n",
			V4L2_TYPE_IS_OUTPUT(vq->type) ? "OUTPUT" : "CAPTURE", max);
		return -ENOMEM;
	}
	if (*nbufs > max - have)
		dev_dbg(ctx->ave->dev, "%s queue: %u buffers asked, %u given\n",
			V4L2_TYPE_IS_OUTPUT(vq->type) ? "OUTPUT" : "CAPTURE", *nbufs, max - have);
	*nbufs = min(*nbufs, max - have);
	if (*nplanes)
		return (*nplanes != 1 || sizes[0] < f->plane_fmt[0].sizeimage) ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = f->plane_fmt[0].sizeimage;
	return 0;
}

static int ave_buf_prepare(struct vb2_buffer *vb)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	const struct v4l2_pix_format_mplane *f =
		V4L2_TYPE_IS_OUTPUT(vb->type) ? &ctx->src_fmt : &ctx->dst_fmt;

	if (vb2_plane_size(vb, 0) < f->plane_fmt[0].sizeimage)
		return -EINVAL;
	/* Keep what the client filled in, so its layout can be checked. */
	if (V4L2_TYPE_IS_OUTPUT(vb->type) && !vb2_get_plane_payload(vb, 0))
		vb2_set_plane_payload(vb, 0, f->plane_fmt[0].sizeimage);
	return 0;
}

static void ave_buf_queue(struct vb2_buffer *vb)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long flags;

	spin_lock_irqsave(&ctx->queue_lock, flags);
	if (ctx->failed)
		v4l2_m2m_buf_done(to_vb2_v4l2_buffer(vb), VB2_BUF_STATE_ERROR);
	else
		v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
	spin_unlock_irqrestore(&ctx->queue_lock, flags);
}

static void ave_return_bufs(struct ave_ctx *ctx, struct vb2_queue *q,
			    enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *vbuf;

	for (;;) {
		vbuf = V4L2_TYPE_IS_OUTPUT(q->type) ?
			v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_m2m_buf_done(vbuf, state);
	}
}

/* Sessions */

int ave_session_index(struct ave_dev *ave, struct ave_ctx *ctx)
{
	for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++)
		if (ave->sessions[i] == ctx)
			return i;
	return -1;
}

static unsigned int ave_session_count(struct ave_dev *ave)
{
	unsigned int n = 0;

	for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++)
		n += !!ave->sessions[i];
	return n;
}

/*
 * Drain (with @flush), stop and close the session in the firmware. The
 * last session to go halts the firmware. Caller holds hw_lock.
 */
static void ave_session_close(struct ave_ctx *ctx, bool flush)
{
	struct ave_dev *ave = ctx->ave;
	struct ave_session *sess = ctx->sess;
	int idx = ave_session_index(ave, ctx);

	lockdep_assert_held(&ave->hw_lock);
	if (!sess || idx < 0)
		return;
	if (sess->fw_open && ave->fw_state == AVE_FW_RUNNING) {
		int err = 0;

		/* Complete is a barrier: answered after every frame before it. */
		if (flush && sess->frames) {
			err = ave_cmd_complete(ctx, AVE_DRAIN_COMPLETES);
			if (!err)
				err = ave_ipc_settle(ave, AVE_FRAME_WAIT_MS);
		}
		if (ave->fw_state == AVE_FW_RUNNING) {
			int cerr = ave_cmd_close(ctx);

			err = err ?: cerr;
		}
		/*
		 * A refused or unanswered Stop/Close/Complete, or frames still
		 * out, leave the session live in the firmware: fail the
		 * firmware so only a proven power cycle ends it.
		 */
		if ((err || sess->busy) && ave->fw_state == AVE_FW_RUNNING) {
			dev_err(ave->dev, "session close not confirmed (%d, %u frames out)\n",
				err, sess->busy);
			ave_fw_mark_broken(ave, err ?: -EBUSY, "Close");
		}
	}
	ave->sessions[idx] = NULL;
	if (!ave_session_count(ave))
		ave_fw_halt(ave);
}

/*
 * Open and Start the session in the firmware, booting the firmware first
 * if no other session runs. Returns -EAGAIN when a halted coprocessor has
 * not lost power yet. Caller holds hw_lock.
 */
static int ave_session_attach(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;
	struct ave_session *sess = ctx->sess;
	int idx = -1, ret;

	lockdep_assert_held(&ave->hw_lock);
	for (unsigned int i = 0; i < AVE_MAX_SESSIONS && idx < 0; i++)
		if (!ave->sessions[i])
			idx = i;
	if (idx < 0) {
		dev_err(ave->dev, "already encoding %u streams\n", AVE_MAX_SESSIONS);
		return -EBUSY;
	}
	if (!ave_session_count(ave)) {
		ret = ave_fw_ensure_running(ave);
		if (ret)
			return ret;
	} else if (ave->fw_state != AVE_FW_RUNNING) {
		dev_err(ave->dev, "firmware state %d, cannot add a session\n", ave->fw_state);
		return -EIO;
	}

	sess->index = idx;
	sess->frames = 0;
	sess->nudged = false;
	ave->sessions[idx] = ctx;
	ret = ctx->mctf ? ave_sess_mctf_borrow(ctx) : 0;
	if (!ret)
		ret = ave_cmd_open(ctx);
	if (!ret)
		ret = ctx->mctf ? ave_mctf_cmd_start(ctx) : ave_cmd_start(ctx);
	if (ret)
		ave_session_close(ctx, false);
	return ret;
}

/*
 * Attach with the device runtime active and one PM reference held by the
 * caller. A halted coprocessor restarts only from reset, and its power can
 * take a moment to go (the DARTs suspend a little later), so drop the
 * reference and cycle the power a few times before giving up. On return
 * the reference is still held, unless *pm_lost says otherwise.
 */
static int ave_session_attach_cycled(struct ave_ctx *ctx, bool *pm_lost)
{
	struct ave_dev *ave = ctx->ave;
	int ret = -EAGAIN;

	*pm_lost = false;
	for (unsigned int i = 0; i < AVE_RESTART_TRIES && ret == -EAGAIN; i++) {
		if (i) {
			pm_runtime_put_sync_suspend(ave->dev);
			msleep(AVE_RESTART_WAIT_MS);
			ret = pm_runtime_resume_and_get(ave->dev);
			if (ret) {
				*pm_lost = true;
				return ret;
			}
		}
		mutex_lock(&ave->hw_lock);
		ret = ave_session_attach(ctx);
		mutex_unlock(&ave->hw_lock);
	}
	if (ret == -EAGAIN)
		dev_err(ave->dev, "coprocessor did not power down; cannot restart it\n");
	return ret;
}

/* Return frames the firmware never answered; then drop the session. */
static void ave_session_free(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;
	struct ave_session *sess = ctx->sess;

	if (!sess)
		return;
	for (unsigned int i = 0; i < sess->p.slots; i++) {
		struct ave_slot *s = &sess->slot[i];

		if (s->src)
			v4l2_m2m_buf_done(s->src, VB2_BUF_STATE_ERROR);
		s->src = NULL;
		if (s->dst)
			v4l2_m2m_buf_done(s->dst, VB2_BUF_STATE_ERROR);
		s->dst = NULL;
	}
	atomic_set(&ctx->ready, 0);
	ave_sess_destroy(ctx);
	pm_runtime_mark_last_busy(ave->dev);
	pm_runtime_put_autosuspend(ave->dev);
}

/* Boot the firmware if needed, then Open and Start a session. */
static int ave_session_begin(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;
	struct iommu_domain *domain;
	struct ave_session *sess;
	bool pm_lost;
	int ret;

	if (ctx->sess)
		return 0;
	/* The domain type can still change (DMA to DMA-FQ) while bound. */
	domain = iommu_get_domain_for_dev(ave->dev);
	if (!domain || !ave_domain_type_safe(domain->type)) {
		dev_err(ave->dev,
			"encoder DMA domain is not strict (type %#x); refusing to start\n",
			domain ? domain->type : 0);
		return -EPERM;
	}
	ret = ctx->mctf ? ave_mctf_check_build(ave) : ave_fw_check_build(ave);
	if (ret)
		return ret;
	if (READ_ONCE(ave->sessions[0]) && READ_ONCE(ave->sessions[1]))
		return -EBUSY;
	/* A halted firmware restarts only from reset: let the power go first. */
	if (READ_ONCE(ave->fw_state) == AVE_FW_HALTED ||
	    READ_ONCE(ave->fw_state) == AVE_FW_BROKEN ||
	    READ_ONCE(ave->fw_state) == AVE_FW_RECOVERING)
		pm_runtime_suspend(ave->dev);
	sess = ave_sess_create(ctx);
	if (IS_ERR(sess)) {
		dev_err(ave->dev, "cannot set up a session: %ld\n", PTR_ERR(sess));
		return PTR_ERR(sess);
	}

	ctx->sess = sess;
	ret = pm_runtime_resume_and_get(ave->dev);
	if (ret) {
		ave_sess_destroy(ctx);
		return ret;
	}
	ret = ave_session_attach_cycled(ctx, &pm_lost);
	if (ret) {
		dev_err(ave->dev, "cannot start an encoding session: %d\n", ret);
		if (pm_lost)
			ave_sess_destroy(ctx);
		else
			ave_session_free(ctx);
	}
	return ret;
}

/*
 * After system sleep: the session was closed when the firmware went down.
 * Open and Start it again with the same parameters; its next frame is an
 * IDR with fresh parameter sets, so the stream stays decodable.
 */
int ave_session_reopen(struct ave_ctx *ctx)
{
	bool pm_lost;
	int ret;

	ret = ave_session_attach_cycled(ctx, &pm_lost);
	if (ret) {
		if (pm_lost) {
			/* The session's PM reference is gone; do not drop it again. */
			pm_runtime_get_noresume(ctx->ave->dev);
		}
		return ret;
	}
	ctx->sess->resume = false;
	dev_dbg(ctx->ave->dev, "session reopened after system sleep\n");
	return 0;
}

/*
 * System sleep: drain every open session, close it and halt the firmware,
 * as at the end of a stream. Sessions reopen on their next frame.
 * Caller holds hw_lock.
 */
void ave_v4l2_suspend(struct ave_dev *ave)
{
	lockdep_assert_held(&ave->hw_lock);
	for (unsigned int i = 0; i < AVE_MAX_SESSIONS; i++) {
		struct ave_ctx *ctx = ave->sessions[i];

		if (!ctx)
			continue;
		cancel_delayed_work(&ctx->poll_work);
		ave_session_close(ctx, true);
		ctx->sess->resume = true;
	}
}

static void ave_session_end(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;

	cancel_delayed_work_sync(&ctx->poll_work);
	mutex_lock(&ave->hw_lock);
	ave_session_close(ctx, true);
	mutex_unlock(&ave->hw_lock);
	ave_session_free(ctx);
}

static void ave_reset_drain(struct ave_ctx *ctx)
{
	ctx->draining = false;
	WRITE_ONCE(ctx->flush_pending, false);
	ctx->stopped = false;
	ctx->drain_src = NULL;
}

static int ave_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_queue *other = V4L2_TYPE_IS_OUTPUT(q->type) ?
		v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx) :
		v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx);
	int ret = 0;

	if (V4L2_TYPE_IS_OUTPUT(q->type))
		ctx->src_sequence = 0;
	else
		ctx->dst_sequence = 0;
	ave_reset_drain(ctx);

	/* The session starts once both queues stream; errors show up here. */
	if (vb2_is_streaming(other))
		ret = ave_session_begin(ctx);
	if (ret)
		ave_return_bufs(ctx, q, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void ave_stop_streaming(struct vb2_queue *q)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_queue *other = V4L2_TYPE_IS_OUTPUT(q->type) ?
		v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx) :
		v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx);

	ave_session_end(ctx);
	ave_reset_drain(ctx);
	ave_return_bufs(ctx, q, VB2_BUF_STATE_ERROR);
	/* vb2 hands queued buffers to buf_queue before the next STREAMON callback. */
	if (!vb2_is_streaming(other))
		WRITE_ONCE(ctx->failed, false);
}

const struct vb2_ops ave_qops = {
	.queue_setup = ave_queue_setup,
	.buf_prepare = ave_buf_prepare,
	.buf_queue = ave_buf_queue,
	.start_streaming = ave_start_streaming,
	.stop_streaming = ave_stop_streaming,
};

static int ave_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct ave_ctx *ctx = priv;
	int ret;

	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src->io_modes = VB2_MMAP | VB2_DMABUF;
	src->drv_priv = ctx;
	src->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src->ops = &ave_qops;
	src->mem_ops = &ave_vb2_memops;
	src->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src->lock = &ctx->ave->vdev_lock;
	src->dev = ctx->ave->dev;
	ret = vb2_queue_init(src);
	if (ret)
		return ret;

	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst->io_modes = VB2_MMAP | VB2_DMABUF;
	dst->drv_priv = ctx;
	dst->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst->ops = &ave_qops;
	dst->mem_ops = &vb2_dma_contig_memops;
	dst->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst->lock = &ctx->ave->vdev_lock;
	dst->dev = ctx->ave->dev;
	return vb2_queue_init(dst);
}

/*
 * Jobs. A job delivers finished frames to CAPTURE buffers and sends at most
 * one new frame. Up to five frames are inside the firmware at once; their
 * answers are collected by the job and, while frames are outstanding, by a
 * short poller, since the firmware interrupt is not wired up.
 */

#define AVE_POLL_MS		2

static int ave_job_ready(void *priv)
{
	struct ave_ctx *ctx = priv;
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;

	if (READ_ONCE(ctx->failed))
		return 0;
	if (atomic_read(&ctx->ready) || READ_ONCE(ctx->flush_pending))
		return 1;
	if (ctx->stopped || (ctx->draining && !ctx->drain_src))
		return 0;
	return v4l2_m2m_num_src_bufs_ready(m2m) > 0;
}

/*
 * Hand finished frames, in bitstream order, to CAPTURE buffers. Returns
 * true when the frame that ends a drain went out (with the LAST flag).
 */
static bool ave_deliver(struct ave_ctx *ctx)
{
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct ave_session *sess = ctx->sess;
	bool last = false;

	while (sess && sess->done_count) {
		unsigned int slot = sess->done_fifo[sess->done_head];
		struct ave_slot *s = &sess->slot[slot];
		struct vb2_v4l2_buffer *dst;
		enum vb2_buffer_state state = VB2_BUF_STATE_DONE;
		bool key = false;

		dst = v4l2_m2m_dst_buf_remove(m2m);
		if (!dst)
			break;
		if (ave_sess_output(ctx, slot, dst, &key)) {
			vb2_set_plane_payload(&dst->vb2_buf, 0, 0);
			state = VB2_BUF_STATE_ERROR;
		}
		v4l2_m2m_buf_copy_metadata(s->src, dst);
		dst->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
				V4L2_BUF_FLAG_BFRAME | V4L2_BUF_FLAG_LAST);
		dst->flags |= key ? V4L2_BUF_FLAG_KEYFRAME : V4L2_BUF_FLAG_PFRAME;
		dst->sequence = ctx->dst_sequence++;

		sess->done_head = (sess->done_head + 1) % AVE_MAX_SLOTS;
		sess->done_count--;
		atomic_dec(&ctx->ready);
		s->done = false;
		ave_sess_prepare_slot(sess, slot);

		/* The last frame of a drain carries the LAST flag. */
		if (READ_ONCE(ctx->flush_pending) && !sess->fw_open && !sess->done_count) {
			dst->flags |= V4L2_BUF_FLAG_LAST;
			last = true;
		}
		ave_vb2_put(s->input_mem);
		s->input_mem = NULL;
		v4l2_m2m_buf_done(s->src, state);
		s->src = NULL;
		v4l2_m2m_buf_done(dst, state);
	}
	return last;
}

static bool ave_slot_answered(struct ave_dev *ave, void *arg)
{
	struct ave_session *sess = arg;

	return sess->busy < sess->p.slots || sess->done_count;
}

static bool ave_frame_answered(struct ave_dev *ave, void *arg)
{
	struct ave_slot *s = arg;

	return !s->busy;
}

/* Send the next OUTPUT frame. Caller holds hw_lock. */
static int ave_encode_one(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;
	struct ave_session *sess = ctx->sess;
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *src;
	dma_addr_t input;
	bool last;
	int slot, ret;

	slot = ave_sess_free_slot(sess);
	if (slot < 0 && sess->busy == sess->p.slots) {
		/* Every slot is inside the firmware: wait for one to come back. */
		ret = ave_ipc_pump(ave, AVE_FRAME_WAIT_MS, ave_slot_answered, sess);
		if (ret)
			return ret;
		ave_deliver(ctx);
		slot = ave_sess_free_slot(sess);
	}
	if (slot < 0)
		return 0;	/* waiting for CAPTURE buffers */

	src = v4l2_m2m_src_buf_remove(m2m);
	if (!src)
		return 0;
	last = src == ctx->drain_src;
	src->sequence = ctx->src_sequence++;

	ret = ave_sess_input(ctx, slot, src, &input);
	if (!ret)
		ret = ave_cmd_process(ctx, slot, src, input, READ_ONCE(ctx->bitrate),
				      xchg(&ctx->force_key_frame, false));
	if (ret) {
		v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
		return ret;
	}

	/* Like macOS: the first frame goes alone, then a Complete. */
	if (sess->frames == 1) {
		ret = ave_ipc_pump(ave, AVE_FRAME_WAIT_MS, ave_frame_answered, &sess->slot[slot]);
		if (!ret)
			ret = ave_cmd_complete(ctx, 1);
		if (!ret)
			ret = ave_ipc_settle(ave, AVE_FRAME_WAIT_MS);
		if (ret)
			return ret;
	} else {
		ret = ave_ipc_pump(ave, 0, NULL, NULL);
		if (ret)
			return ret;
	}

	if (last) {
		ctx->drain_src = NULL;
		WRITE_ONCE(ctx->flush_pending, true);
	}
	return 0;
}

/*
 * End of a drain: flush and close the session, deliver what is left and
 * finish with a LAST buffer. Caller holds hw_lock; returns true once the
 * LAST buffer is out.
 */
static bool ave_finish_drain(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;
	struct ave_session *sess = ctx->sess;
	struct vb2_v4l2_buffer *dst;

	if (sess && sess->fw_open)
		ave_session_close(ctx, true);
	if (sess && sess->busy) {
		/* The run job fails the session. */
		dev_err(ave->dev, "%u frames never came back from the firmware\n", sess->busy);
		return false;
	}
	if (sess && sess->done_count)
		return ave_deliver(ctx);	/* false: waiting for CAPTURE buffers */

	/* Nothing left to deliver: an empty buffer carries the LAST flag. */
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	if (!dst)
		return false;
	vb2_set_plane_payload(&dst->vb2_buf, 0, 0);
	dst->flags |= V4L2_BUF_FLAG_LAST;
	dst->sequence = ctx->dst_sequence++;
	v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);
	return true;
}

/* The firmware or the session failed: return everything, fail the queues. */
void ave_job_fail(struct ave_ctx *ctx, int err)
{
	struct ave_dev *ave = ctx->ave;
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	unsigned long flags;

	/* Publish the fatal error before any teardown that can wait on PM/IPC. */
	spin_lock_irqsave(&ctx->queue_lock, flags);
	WRITE_ONCE(ctx->failed, true);
	vb2_queue_error(v4l2_m2m_get_src_vq(m2m));
	vb2_queue_error(v4l2_m2m_get_dst_vq(m2m));
	ave_return_bufs(ctx, v4l2_m2m_get_src_vq(m2m), VB2_BUF_STATE_ERROR);
	ave_return_bufs(ctx, v4l2_m2m_get_dst_vq(m2m), VB2_BUF_STATE_ERROR);
	spin_unlock_irqrestore(&ctx->queue_lock, flags);
	cancel_delayed_work(&ctx->poll_work);
	dev_err(ave->dev, "encoding failed: %d\n", err);
	mutex_lock(&ave->hw_lock);
	ave_session_close(ctx, false);
	mutex_unlock(&ave->hw_lock);
	ave_session_free(ctx);
	ave_reset_drain(ctx);
}

static void ave_run_work(struct work_struct *work)
{
	struct ave_ctx *ctx = container_of(work, struct ave_ctx, run_work);
	struct ave_dev *ave = ctx->ave;
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	bool eos = false, poll = false;
	int ret = 0;

	if (READ_ONCE(ctx->failed))
		goto finish;

	/*
	 * After a drain the next frame starts a new session; after system
	 * sleep it reopens the old one in the restarted firmware.
	 */
	if (!READ_ONCE(ctx->flush_pending) && v4l2_m2m_num_src_bufs_ready(m2m)) {
		if (!ctx->sess)
			ret = ave_session_begin(ctx);
		else if (ctx->sess->resume)
			ret = ave_session_reopen(ctx);
	}

	mutex_lock(&ave->hw_lock);
	/*
	 * A closed session (after a drain or for sleep) may still hold frames
	 * for CAPTURE buffers; only an open one must still own the firmware.
	 */
	if (!ret && ctx->sess && ctx->sess->fw_open && ave_session_index(ave, ctx) < 0) {
		dev_err(ave->dev, "session lost the firmware\n");
		ret = -EIO;
	}
	if (!ret) {
		eos = ave_deliver(ctx);
		if (!eos && !READ_ONCE(ctx->flush_pending) && ctx->sess &&
		    v4l2_m2m_num_src_bufs_ready(m2m) && v4l2_m2m_num_dst_bufs_ready(m2m))
			ret = ave_encode_one(ctx);
	}
	if (!ret && !eos) {
		eos = ave_deliver(ctx);
		if (!eos && READ_ONCE(ctx->flush_pending))
			eos = ave_finish_drain(ctx);
		/* Frames outstanding need a running firmware. */
		if (!eos && ctx->sess && ctx->sess->busy && ave->fw_state != AVE_FW_RUNNING) {
			dev_err(ave->dev, "%u frames outstanding, firmware state %d\n",
				ctx->sess->busy, ave->fw_state);
			ret = -EIO;
		}
		poll = !ret && !eos && ctx->sess && ctx->sess->busy;
	}
	mutex_unlock(&ave->hw_lock);

	if (ret) {
		ave_job_fail(ctx, ret);
	} else if (eos) {
		ave_session_free(ctx);
		ctx->draining = false;
		WRITE_ONCE(ctx->flush_pending, false);
		ctx->stopped = true;
		v4l2_event_queue_fh(&ctx->fh, &ave_eos_event);
	} else if (poll) {
		mod_delayed_work(ave->wq, &ctx->poll_work, msecs_to_jiffies(AVE_POLL_MS));
	}
finish:
	pm_runtime_mark_last_busy(ave->dev);
	v4l2_m2m_job_finish(ave->m2m_dev, m2m);
}

/* Collect frame answers while frames are outstanding and no job runs. */
static void ave_poll_work(struct work_struct *work)
{
	struct ave_ctx *ctx = container_of(to_delayed_work(work), struct ave_ctx, poll_work);
	struct ave_dev *ave = ctx->ave;
	bool again = false, fail = false;

	mutex_lock(&ave->hw_lock);
	if (ctx->sess && ave->fw_state == AVE_FW_BROKEN) {
		fail = true;
	} else if (ctx->sess && ave_session_index(ave, ctx) >= 0) {
		fail = ave_ipc_pump(ave, 0, NULL, NULL) != 0;
		if (!fail && ave_sess_starved(ctx->sess) &&
		    !v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx)) {
			dev_dbg(ave->dev, "no input, flushing %u held frames\n", ctx->sess->busy);
			ctx->sess->nudged = true;
			ctx->sess->progress = jiffies;
			fail = ave_cmd_complete(ctx, 1) != 0;
		}
		if (!fail && ave_sess_stalled(ctx->sess)) {
			/*
			 * The firmware holds frames it should have answered.
			 * Treat it as stuck, so the session fails now instead
			 * of leaving the client waiting.
			 */
			dev_err(ave->dev, "%u frames unanswered for %u ms; firmware is stuck\n",
				ctx->sess->busy, AVE_FRAME_WAIT_MS);
			ave_fw_mark_broken(ave, -ETIMEDOUT, "frame timeout");
			fail = true;
		}
		again = !fail && ctx->sess->busy;
	} else if (ctx->sess && ctx->sess->busy) {
		dev_err(ave->dev, "%u frames outstanding in a session that lost the firmware\n",
			ctx->sess->busy);
		fail = true;
	}
	mutex_unlock(&ave->hw_lock);

	/* Jobs run on the same ordered queue, so none runs right now. */
	if (fail)
		ave_job_fail(ctx, -EIO);
	else if (atomic_read(&ctx->ready))
		v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
	if (again)
		queue_delayed_work(ave->wq, &ctx->poll_work, msecs_to_jiffies(AVE_POLL_MS));
}

static void ave_device_run(void *priv)
{
	struct ave_ctx *ctx = priv;

	queue_work(ctx->ave->wq, &ctx->run_work);
}

static const struct v4l2_m2m_ops ave_m2m_ops = {
	.device_run = ave_device_run,
	.job_ready = ave_job_ready,
};

/* File operations */

static int ave_open(struct file *file)
{
	struct ave_dev *ave = video_drvdata(file);
	struct ave_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	ctx->ave = ave;
	spin_lock_init(&ctx->queue_lock);
	INIT_WORK(&ctx->run_work, ave_run_work);
	INIT_DELAYED_WORK(&ctx->poll_work, ave_poll_work);
	atomic_set(&ctx->ready, 0);
	ave_reset_fmts(ctx);

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ret = ave_init_ctrls(ctx);
	if (ret)
		goto err_fh;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(ave->m2m_dev, ctx, ave_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_ctrls;
	}
	/* Finished frames can be delivered with no new OUTPUT buffer queued. */
	v4l2_m2m_set_src_buffered(ctx->fh.m2m_ctx, true);
	v4l2_fh_add(&ctx->fh, file);
	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&ctx->hdl);
err_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int ave_release(struct file *file)
{
	struct ave_ctx *ctx = ave_file_to_ctx(file);
	struct ave_dev *ave = ctx->ave;

	mutex_lock(&ave->vdev_lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	mutex_unlock(&ave->vdev_lock);
	cancel_delayed_work_sync(&ctx->poll_work);
	cancel_work_sync(&ctx->run_work);
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations ave_fops = {
	.owner = THIS_MODULE,
	.open = ave_open,
	.release = ave_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

int ave_v4l2_register(struct ave_dev *ave)
{
	struct video_device *vdev = &ave->vdev;
	int ret;

	mutex_init(&ave->vdev_lock);
	ave->wq = alloc_ordered_workqueue("apple-ave", WQ_MEM_RECLAIM);
	if (!ave->wq)
		return -ENOMEM;

	ret = v4l2_device_register(ave->dev, &ave->v4l2_dev);
	if (ret)
		goto err_wq;

	ave->m2m_dev = v4l2_m2m_init(&ave_m2m_ops);
	if (IS_ERR(ave->m2m_dev)) {
		ret = PTR_ERR(ave->m2m_dev);
		goto err_v4l2;
	}

	strscpy(vdev->name, AVE_NAME, sizeof(vdev->name));
	vdev->fops = &ave_fops;
	vdev->ioctl_ops = &ave_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &ave->vdev_lock;
	vdev->v4l2_dev = &ave->v4l2_dev;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(vdev, ave);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_m2m;
	ret = ave_mctf_register(ave);
	if (ret)
		goto err_vdev;
	return 0;

err_vdev:
	video_unregister_device(vdev);
err_m2m:
	v4l2_m2m_release(ave->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&ave->v4l2_dev);
err_wq:
	destroy_workqueue(ave->wq);
	return ret;
}

void ave_v4l2_unregister(struct ave_dev *ave)
{
	ave_mctf_unregister(ave);
	video_unregister_device(&ave->vdev);
	v4l2_m2m_release(ave->m2m_dev);
	v4l2_device_unregister(&ave->v4l2_dev);
	destroy_workqueue(ave->wq);
}
