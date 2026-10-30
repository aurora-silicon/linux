// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE video encoder: V4L2 stateful encoder interface
 *
 * Follows Documentation/userspace-api/media/v4l/dev-encoder.rst: the CAPTURE
 * queue carries the coded stream (H.264 or HEVC), the OUTPUT queue raw NV12.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "ave.h"

#define AVE_NAME		"apple-ave"

/*
 * Frame size limits and input stride alignment. Provisional: conservative
 * values the hardware is known to cover (up to the 2408x1506 panel and 1080p
 * and 4K class streams); refined once the firmware limits are described.
 */
#define AVE_MIN_W		64
#define AVE_MIN_H		64
#define AVE_MAX_W		4096
#define AVE_MAX_H		4096
#define AVE_STEP		16
#define AVE_STRIDE_ALIGN	64
#define AVE_DEFAULT_W		1920
#define AVE_DEFAULT_H		1080

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

static void ave_clamp_size(u32 *w, u32 *h)
{
	*w = clamp_t(u32, ALIGN(*w, 2), AVE_MIN_W, AVE_MAX_W);
	*h = clamp_t(u32, ALIGN(*h, 2), AVE_MIN_H, AVE_MAX_H);
}

/* Raw NV12, one buffer: Y plane, then interleaved CbCr at half height. */
static void ave_fill_raw(struct v4l2_pix_format_mplane *f)
{
	u32 stride, height;

	f->pixelformat = V4L2_PIX_FMT_NV12;
	ave_clamp_size(&f->width, &f->height);
	stride = ALIGN(max_t(u32, f->width, f->plane_fmt[0].bytesperline), AVE_STRIDE_ALIGN);
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
	ave_clamp_size(&f->width, &f->height);
	/* Room for an intra frame at low compression plus headers. */
	min = max_t(u32, f->width * f->height * 3 / 4, SZ_256K);
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
	ave_fill_raw(src);
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
	    !ave_fmt_listed(ave_coded_fmts, ARRAY_SIZE(ave_coded_fmts), fs->pixel_format))
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

	ave_fill_raw(pix);
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

static int ave_encoder_cmd(struct file *file, void *priv, struct v4l2_encoder_cmd *ec)
{
	struct ave_ctx *ctx = ave_file_to_ctx(file);
	int ret;

	ret = v4l2_m2m_ioctl_try_encoder_cmd(file, priv, ec);
	if (ret)
		return ret;
	if (!vb2_is_streaming(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx)) ||
	    !vb2_is_streaming(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return 0;

	ret = v4l2_m2m_ioctl_encoder_cmd(file, priv, ec);
	if (ret)
		return ret;
	if (ec->cmd == V4L2_ENC_CMD_STOP && v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
		v4l2_event_queue_fh(&ctx->fh, &ave_eos_event);
	if (ec->cmd == V4L2_ENC_CMD_START && v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
		vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
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
		ctx->bitrate = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_BITRATE_PEAK:
		ctx->bitrate_peak = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		ctx->gop_size = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
		ctx->force_key_frame = true;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		ctx->h264_profile = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		ctx->h264_level = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_I_PERIOD:
		ctx->h264_i_period = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_MIN_QP:
		ctx->h264_min_qp = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_MAX_QP:
		ctx->h264_max_qp = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEVC_PROFILE:
		ctx->hevc_profile = ctrl->val;
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
	case V4L2_CID_MPEG_VIDEO_B_FRAMES:
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

	v4l2_ctrl_handler_init(hdl, 18);

	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_CQ,
			       0, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_BITRATE, 100000, 200000000, 1,
			  8000000);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, 100000, 400000000, 1,
			  16000000);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_GOP_SIZE, 0, 65535, 1, 60);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, 0, 0, 0);
	/* No frame reordering until reference handling is described. */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, 0, 1, 0);
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
			       V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_LEVEL,
			       V4L2_MPEG_VIDEO_H264_LEVEL_5_2, 0,
			       V4L2_MPEG_VIDEO_H264_LEVEL_4_1);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, 0, 65535, 1, 0);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 0, 51, 1, 0);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 0, 51, 1, 51);

	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10, 0,
			       V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN);
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1, 0,
			       V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP, 0, 51, 1, 0);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP, 0, 51, 1, 51);

	c = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, 1, 32, 1, 1);
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
	if (V4L2_TYPE_IS_OUTPUT(vb->type))
		vb2_set_plane_payload(vb, 0, f->plane_fmt[0].sizeimage);
	return 0;
}

static void ave_buf_queue(struct vb2_buffer *vb)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	if (V4L2_TYPE_IS_CAPTURE(vb->type) && vb2_is_streaming(vb->vb2_queue) &&
	    v4l2_m2m_dst_buf_is_last(ctx->fh.m2m_ctx)) {
		vb2_set_plane_payload(vb, 0, 0);
		vbuf->sequence = ctx->dst_sequence++;
		v4l2_m2m_last_buffer_done(ctx->fh.m2m_ctx, vbuf);
		v4l2_event_queue_fh(&ctx->fh, &ave_eos_event);
		return;
	}
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
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

static int ave_session_begin(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;
	enum ave_codec codec = ave_ctx_codec(ctx);
	int ret;

	ret = pm_runtime_resume_and_get(ave->dev);
	if (ret)
		return ret;

	mutex_lock(&ave->hw_lock);
	ret = ave_fw_ensure_running(ave);
	if (!ret)
		ret = ave_cmd_open(ctx, codec);
	if (!ret)
		ret = ave_cmd_start(ctx, codec);
	if (ret && ctx->session_open)
		ave_cmd_finish(ctx, codec);
	mutex_unlock(&ave->hw_lock);

	if (ret) {
		dev_dbg(ave->dev, "session start failed: %d\n", ret);
		pm_runtime_mark_last_busy(ave->dev);
		pm_runtime_put_autosuspend(ave->dev);
	}
	return ret;
}

static void ave_session_end(struct ave_ctx *ctx)
{
	struct ave_dev *ave = ctx->ave;

	cancel_work_sync(&ctx->run_work);
	mutex_lock(&ave->hw_lock);
	if (ave->fw_state == AVE_FW_RUNNING)
		ave_cmd_finish(ctx, ave_ctx_codec(ctx));
	ctx->session_open = false;
	mutex_unlock(&ave->hw_lock);
	pm_runtime_mark_last_busy(ave->dev);
	pm_runtime_put_autosuspend(ave->dev);
}

static int ave_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_queue *other = V4L2_TYPE_IS_OUTPUT(q->type) ?
		v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx) :
		v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx);
	int ret = 0;

	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, q);
	if (V4L2_TYPE_IS_OUTPUT(q->type))
		ctx->src_sequence = 0;
	else
		ctx->dst_sequence = 0;

	/* The session starts once both queues stream. */
	if (vb2_is_streaming(other) && !ctx->session_open)
		ret = ave_session_begin(ctx);
	if (ret)
		ave_return_bufs(ctx, q, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void ave_stop_streaming(struct vb2_queue *q)
{
	struct ave_ctx *ctx = vb2_get_drv_priv(q);

	if (ctx->session_open)
		ave_session_end(ctx);
	ave_return_bufs(ctx, q, VB2_BUF_STATE_ERROR);
	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, q);
	if (V4L2_TYPE_IS_OUTPUT(q->type) && v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
		v4l2_event_queue_fh(&ctx->fh, &ave_eos_event);
}

static const struct vb2_ops ave_qops = {
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
	src->mem_ops = &vb2_dma_contig_memops;
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

/* Jobs: one raw frame in, one coded frame out. */

static void ave_run_work(struct work_struct *work)
{
	struct ave_ctx *ctx = container_of(work, struct ave_ctx, run_work);
	struct ave_dev *ave = ctx->ave;
	struct vb2_v4l2_buffer *src, *dst;
	enum vb2_buffer_state state = VB2_BUF_STATE_DONE;
	bool keyframe = false;
	u32 bytes = 0;
	int ret;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	mutex_lock(&ave->hw_lock);
	ret = ave_cmd_process(ctx, ave_ctx_codec(ctx), src, dst, &bytes, &keyframe);
	ctx->force_key_frame = false;
	mutex_unlock(&ave->hw_lock);
	if (ret) {
		dev_dbg(ave->dev, "frame %u failed: %d\n", ctx->src_sequence, ret);
		state = VB2_BUF_STATE_ERROR;
		bytes = 0;
	}

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	src->sequence = ctx->src_sequence++;
	dst->sequence = ctx->dst_sequence++;
	v4l2_m2m_buf_copy_metadata(src, dst);
	dst->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME | V4L2_BUF_FLAG_LAST);
	dst->flags |= keyframe ? V4L2_BUF_FLAG_KEYFRAME : V4L2_BUF_FLAG_PFRAME;
	vb2_set_plane_payload(&dst->vb2_buf, 0, bytes);

	if (v4l2_m2m_is_last_draining_src_buf(ctx->fh.m2m_ctx, src)) {
		dst->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_event_queue_fh(&ctx->fh, &ave_eos_event);
		v4l2_m2m_mark_stopped(ctx->fh.m2m_ctx);
	}

	v4l2_m2m_buf_done(src, state);
	v4l2_m2m_buf_done(dst, state);
	pm_runtime_mark_last_busy(ave->dev);
	v4l2_m2m_job_finish(ave->m2m_dev, ctx->fh.m2m_ctx);
}

static void ave_device_run(void *priv)
{
	struct ave_ctx *ctx = priv;

	queue_work(ctx->ave->wq, &ctx->run_work);
}

static const struct v4l2_m2m_ops ave_m2m_ops = {
	.device_run = ave_device_run,
};

/* File operations */

static int ave_open(struct file *file)
{
	struct ave_dev *ave = video_drvdata(file);
	struct ave_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->ave = ave;
	INIT_WORK(&ctx->run_work, ave_run_work);
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
	return 0;

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
	video_unregister_device(&ave->vdev);
	v4l2_m2m_release(ave->m2m_dev);
	v4l2_device_unregister(&ave->v4l2_dev);
	destroy_workqueue(ave->wq);
}
