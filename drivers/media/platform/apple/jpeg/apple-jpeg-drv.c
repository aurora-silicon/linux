// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple Silicon JPEG encoder, V4L2 memory-to-memory.
 *
 * Takes full-range YUV in one buffer (NV12 for 4:2:0, NV16 for 4:2:2) and
 * produces baseline JPEG with the standard Huffman tables. One job runs at
 * a time; the engine is reset before each job and the completion interrupt
 * ends it. Register layout: see apple-jpeg-hw.h.
 */
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "apple-jpeg-hw.h"

#define AJ_NAME			"apple-jpeg"
#define AJ_MIN_SIZE		16
#define AJ_MAX_SIZE		16384
#define AJ_JOB_TIMEOUT_MS	2000

struct apple_jpeg {
	struct device *dev;
	void __iomem *regs;
	const struct apple_jpeg_variant *variant;
	struct dev_pm_domain_list *pd_list;
	struct mutex lock;	/* serialises ioctls and queue state */
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
	int irq;

	spinlock_t hw_lock;	/* protects the fields below */
	struct apple_jpeg_ctx *active;
	bool broken;
	u32 status, int_status, bytes;
	struct delayed_work watchdog;
	struct apple_jpeg_write writes[APPLE_JPEG_MAX_WRITES];
};

struct apple_jpeg_ctx {
	struct v4l2_fh fh;
	struct apple_jpeg *jpeg;
	struct v4l2_ctrl_handler hdl;
	struct v4l2_ctrl *quality;
	struct v4l2_ctrl *restart;
	struct v4l2_pix_format_mplane src;	/* raw, OUTPUT queue */
	struct v4l2_pix_format_mplane dst;	/* JPEG, CAPTURE queue */
	u32 dst_keep;		/* JPEG buffer size S_FMT(CAPTURE) returned */
	u32 sequence;
	/*
	 * Serialises the drain state (last source buffer, stopped) between job
	 * completion in IRQ context and ENCODER_CMD / CAPTURE QBUF in ioctl
	 * context, which hold only the queue mutex.
	 */
	spinlock_t drain_lock;
};

static struct apple_jpeg_ctx *aj_file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct apple_jpeg_ctx, fh);
}

static void aj_write(struct apple_jpeg *jpeg, u32 reg, u32 val)
{
	writel_relaxed(val, jpeg->regs + reg);
}

static u32 aj_read(struct apple_jpeg *jpeg, u32 reg)
{
	return readl_relaxed(jpeg->regs + reg);
}

/* ---- hardware --------------------------------------------------------- */

/*
 * Reset handshake. Registers outside the control block (the IRQ mask and
 * the 0x1000 block) may only be touched once MODE has opened them.
 */
static int aj_reset(struct apple_jpeg *jpeg)
{
	int n, ret;
	u32 v;

	aj_write(jpeg, AJ_MODE, AJ_MODE_RESET);
	aj_write(jpeg, AJ_MODE, AJ_MODE_SETUP);
	aj_write(jpeg, AJ_MAIN_RUN, 0);
	n = apple_jpeg_defaults(jpeg->variant->layout, false, jpeg->writes,
				ARRAY_SIZE(jpeg->writes));
	if (n < 0)
		return n;
	for (int i = 0; i < n; i++)
		aj_write(jpeg, jpeg->writes[i].offset, jpeg->writes[i].value);
	aj_write(jpeg, AJ_MODE, AJ_MODE_OPEN);
	ret = readl_relaxed_poll_timeout_atomic(jpeg->regs + AJ_COMMAND, v, !v, 1, 10000);
	if (ret)
		return ret;
	aj_write(jpeg, AJ_RST_INTERVAL, 1);
	ret = readl_relaxed_poll_timeout_atomic(jpeg->regs + AJ_RST_INTERVAL, v, v == 1, 1,
						10000);
	if (ret)
		return ret;
	aj_write(jpeg, AJ_RST_INTERVAL, 0);
	aj_write(jpeg, AJ_MODE, AJ_MODE_IDLE);
	return 0;
}

static const struct v4l2_event aj_eos = { .type = V4L2_EVENT_EOS };

static void aj_finish(struct apple_jpeg *jpeg, struct apple_jpeg_ctx *ctx, int err)
{
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *src, *dst;
	unsigned long flags;

	spin_lock_irqsave(&ctx->drain_lock, flags);
	src = v4l2_m2m_src_buf_remove(m2m);
	dst = v4l2_m2m_dst_buf_remove(m2m);
	if (src && dst) {
		if (!err && (jpeg->bytes < 4 || jpeg->bytes > vb2_plane_size(&dst->vb2_buf, 0)))
			err = -EIO;
		vb2_set_plane_payload(&dst->vb2_buf, 0, err ? 0 : jpeg->bytes);
		v4l2_m2m_buf_copy_metadata(src, dst);
		dst->sequence = ctx->sequence++;
		dst->flags |= V4L2_BUF_FLAG_KEYFRAME;
		if (v4l2_m2m_is_last_draining_src_buf(m2m, src)) {
			dst->flags |= V4L2_BUF_FLAG_LAST;
			v4l2_event_queue_fh(&ctx->fh, &aj_eos);
			v4l2_m2m_mark_stopped(m2m);
		}
		v4l2_m2m_buf_done(src, err ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
		v4l2_m2m_buf_done(dst, err ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	}
	spin_unlock_irqrestore(&ctx->drain_lock, flags);
	pm_runtime_mark_last_busy(jpeg->dev);
	pm_runtime_put_autosuspend(jpeg->dev);
	v4l2_m2m_job_finish(jpeg->m2m_dev, m2m);
}

/* Completion: snapshot, acknowledge and quiesce, in the engine's order. */
static irqreturn_t aj_irq(int irq, void *data)
{
	struct apple_jpeg *jpeg = data;
	struct apple_jpeg_ctx *ctx;
	int err = 0;

	spin_lock(&jpeg->hw_lock);
	ctx = jpeg->active;
	if (!ctx) {
		spin_unlock(&jpeg->hw_lock);
		return IRQ_NONE;
	}
	aj_read(jpeg, AJ_CODEC_MODE);
	jpeg->status = aj_read(jpeg, AJ_STATUS);
	jpeg->int_status = aj_read(jpeg, AJ_INT_STATUS);
	aj_read(jpeg, AJ_DEBUG_STATUS);
	if (!(jpeg->int_status & jpeg->variant->int_done_mask) &&
	    !(jpeg->status & AJ_STATUS_ERRORS)) {
		spin_unlock(&jpeg->hw_lock);
		return IRQ_NONE;
	}
	aj_write(jpeg, AJ_INT_STATUS, 0);
	aj_write(jpeg, AJ_MAIN_RUN, 0);
	aj_write(jpeg, jpeg->variant->layout->prefetch_read, 0);
	aj_write(jpeg, jpeg->variant->layout->prefetch_write, 0);
	aj_write(jpeg, AJ_IRQ_MASK, 0);
	aj_read(jpeg, AJ_BITSTREAM_CORRUPTION);
	jpeg->bytes = aj_read(jpeg, AJ_COMPRESSED_BYTES);
	jpeg->active = NULL;
	spin_unlock(&jpeg->hw_lock);

	if (!(jpeg->status & AJ_STATUS_DONE) || (jpeg->status & AJ_STATUS_ERRORS) ||
	    !(jpeg->int_status & jpeg->variant->int_done_mask)) {
		dev_err_ratelimited(jpeg->dev, "job failed: status %#x int %#x\n",
				    jpeg->status, jpeg->int_status);
		err = -EIO;
	}
	cancel_delayed_work(&jpeg->watchdog);
	aj_finish(jpeg, ctx, err);
	return IRQ_HANDLED;
}

static void aj_watchdog(struct work_struct *work)
{
	struct apple_jpeg *jpeg = container_of(work, struct apple_jpeg, watchdog.work);
	struct apple_jpeg_ctx *ctx;
	unsigned long flags;

	spin_lock_irqsave(&jpeg->hw_lock, flags);
	ctx = jpeg->active;
	jpeg->active = NULL;
	if (ctx) {
		aj_write(jpeg, AJ_MAIN_RUN, 0);
		aj_write(jpeg, AJ_IRQ_MASK, 0);
		/* The reset before the next job decides whether the engine recovered. */
	}
	spin_unlock_irqrestore(&jpeg->hw_lock, flags);
	if (ctx) {
		dev_err(jpeg->dev, "job timed out\n");
		aj_finish(jpeg, ctx, -ETIMEDOUT);
	}
}

static void aj_device_run(void *priv)
{
	struct apple_jpeg_ctx *ctx = priv;
	struct apple_jpeg *jpeg = ctx->jpeg;
	struct vb2_v4l2_buffer *src, *dst;
	struct apple_jpeg_layout layout;
	struct apple_jpeg_job job;
	unsigned long flags;
	dma_addr_t y;
	bool is422;
	int n, ret;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	ret = pm_runtime_resume_and_get(jpeg->dev);
	if (ret < 0) {
		pm_runtime_get_noresume(jpeg->dev);
		aj_finish(jpeg, ctx, ret);
		return;
	}

	y = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	is422 = ctx->src.pixelformat == V4L2_PIX_FMT_NV16;
	apple_jpeg_raw_layout(ctx->src.width, ctx->src.height, is422, &layout);
	job = (struct apple_jpeg_job){
		.width = ctx->src.width,
		.height = ctx->src.height,
		.is422 = is422,
		.luma_stride = layout.bytesperline,
		.chroma_stride = layout.bytesperline,
		.luma = y,
		.chroma = y + layout.chroma_offset,
		/* The engine's single input bound: the end of the negotiated surface. */
		.input_end = y + layout.sizeimage - 1,
		.output = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0),
		.output_size = vb2_plane_size(&dst->vb2_buf, 0),
		.quality = v4l2_ctrl_g_ctrl(ctx->quality),
		.restart_interval = v4l2_ctrl_g_ctrl(ctx->restart),
	};

	spin_lock_irqsave(&jpeg->hw_lock, flags);
	ret = jpeg->broken ? -EIO : aj_reset(jpeg);
	if (ret) {
		jpeg->broken = true;
		spin_unlock_irqrestore(&jpeg->hw_lock, flags);
		dev_err_ratelimited(jpeg->dev, "engine reset failed: %d\n", ret);
		aj_finish(jpeg, ctx, -EIO);
		return;
	}
	n = apple_jpeg_build_job(jpeg->variant->layout, &job, jpeg->writes,
				 ARRAY_SIZE(jpeg->writes));
	if (n < 0) {
		spin_unlock_irqrestore(&jpeg->hw_lock, flags);
		aj_finish(jpeg, ctx, n);
		return;
	}
	jpeg->active = ctx;
	/* Buffers must be visible before the start command, the last write. */
	dma_wmb();
	for (int i = 0; i < n; i++)
		writel(jpeg->writes[i].value, jpeg->regs + jpeg->writes[i].offset);
	spin_unlock_irqrestore(&jpeg->hw_lock, flags);
	schedule_delayed_work(&jpeg->watchdog, msecs_to_jiffies(AJ_JOB_TIMEOUT_MS));
}

static const struct v4l2_m2m_ops aj_m2m_ops = {
	.device_run = aj_device_run,
};

/* ---- formats ---------------------------------------------------------- */

/* Raw input: Y rows then interleaved CbCr rows in one buffer, full range. */
static void aj_fill_raw(struct v4l2_pix_format_mplane *pix)
{
	bool is422 = pix->pixelformat == V4L2_PIX_FMT_NV16;
	u32 w = clamp_t(u32, ALIGN(pix->width, 2), AJ_MIN_SIZE, AJ_MAX_SIZE);
	u32 h = clamp_t(u32, pix->height, AJ_MIN_SIZE, AJ_MAX_SIZE);
	struct apple_jpeg_layout layout;

	apple_jpeg_raw_layout(w, h, is422, &layout);
	pix->pixelformat = is422 ? V4L2_PIX_FMT_NV16 : V4L2_PIX_FMT_NV12;
	pix->width = w;
	pix->height = h;
	pix->field = V4L2_FIELD_NONE;
	pix->num_planes = 1;
	pix->flags = 0;
	memset(pix->plane_fmt, 0, sizeof(pix->plane_fmt));
	memset(pix->reserved, 0, sizeof(pix->reserved));
	/* Chroma follows the visible luma rows; the tail covers the MCU rows. */
	pix->plane_fmt[0].bytesperline = layout.bytesperline;
	pix->plane_fmt[0].sizeimage = layout.sizeimage;
	/* JPEG carries full-range BT.601; the engine does no range conversion. */
	pix->colorspace = V4L2_COLORSPACE_SRGB;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_SRGB;
}

/*
 * The coded format follows the raw one. @keep is a JPEG buffer size already
 * agreed on CAPTURE: a later OUTPUT change may grow the buffers but never
 * shrinks them below it, so REQBUFS allocates at least what S_FMT returned.
 */
static void aj_fill_coded(struct v4l2_pix_format_mplane *dst,
			  const struct v4l2_pix_format_mplane *src, u32 keep)
{
	*dst = *src;
	dst->pixelformat = V4L2_PIX_FMT_JPEG;
	dst->num_planes = 1;
	memset(dst->plane_fmt, 0, sizeof(dst->plane_fmt));
	dst->plane_fmt[0].sizeimage = apple_jpeg_coded_size(src->width, src->height, keep);
}

static int aj_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, AJ_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Apple JPEG encoder", sizeof(cap->card));
	return 0;
}

static int aj_enum_fmt_out(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	static const u32 fmts[] = { V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV16 };

	if (f->index >= ARRAY_SIZE(fmts))
		return -EINVAL;
	f->pixelformat = fmts[f->index];
	return 0;
}

static int aj_enum_fmt_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_JPEG;
	f->flags = V4L2_FMT_FLAG_COMPRESSED;
	return 0;
}

static int aj_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *fs)
{
	if (fs->index || (fs->pixel_format != V4L2_PIX_FMT_JPEG &&
			  fs->pixel_format != V4L2_PIX_FMT_NV12 &&
			  fs->pixel_format != V4L2_PIX_FMT_NV16))
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fs->stepwise = (struct v4l2_frmsize_stepwise){
		AJ_MIN_SIZE, AJ_MAX_SIZE, 2, AJ_MIN_SIZE, AJ_MAX_SIZE, 1,
	};
	return 0;
}

static int aj_g_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = aj_file_to_ctx(file)->src;
	return 0;
}

static int aj_g_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = aj_file_to_ctx(file)->dst;
	return 0;
}

static int aj_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	aj_fill_raw(&f->fmt.pix_mp);
	return 0;
}

static int aj_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	aj_fill_coded(&f->fmt.pix_mp, &aj_file_to_ctx(file)->src,
		      f->fmt.pix_mp.plane_fmt[0].sizeimage);
	return 0;
}

static int aj_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apple_jpeg_ctx *ctx = aj_file_to_ctx(file);

	if (vb2_is_busy(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx)) ||
	    vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	aj_fill_raw(&f->fmt.pix_mp);
	ctx->src = f->fmt.pix_mp;
	aj_fill_coded(&ctx->dst, &ctx->src, ctx->dst_keep);
	return 0;
}

static int aj_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apple_jpeg_ctx *ctx = aj_file_to_ctx(file);

	if (vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	/* A larger size the client asks for is honoured (bounded) and kept. */
	aj_fill_coded(&f->fmt.pix_mp, &ctx->src, f->fmt.pix_mp.plane_fmt[0].sizeimage);
	ctx->dst = f->fmt.pix_mp;
	ctx->dst_keep = ctx->dst.plane_fmt[0].sizeimage;
	return 0;
}

/*
 * STOP picks the last queued source buffer, or marks the next CAPTURE buffer
 * LAST when none is queued. Without the lock, a job completing at the same
 * time could leave the queue after STOP chose it, or before STOP looked, and
 * no buffer would ever carry LAST: the client waits for EOS forever.
 */
static int aj_encoder_cmd(struct file *file, void *priv, struct v4l2_encoder_cmd *ec)
{
	struct apple_jpeg_ctx *ctx = aj_file_to_ctx(file);
	unsigned long flags;
	int ret;

	ret = v4l2_m2m_ioctl_try_encoder_cmd(file, priv, ec);
	if (ret)
		return ret;
	if (ec->cmd != V4L2_ENC_CMD_STOP) {
		ret = v4l2_m2m_ioctl_encoder_cmd(file, priv, ec);
		/* A new stream after a drain: the next LAST is a new one. */
		if (!ret)
			vb2_clear_last_buffer_dequeued(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
		return ret;
	}
	spin_lock_irqsave(&ctx->drain_lock, flags);
	ret = v4l2_m2m_encoder_cmd(file, ctx->fh.m2m_ctx, ec);
	/* Nothing left to encode: STOP completed at once, so EOS now. */
	if (!ret && v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
		v4l2_event_queue_fh(&ctx->fh, &aj_eos);
	spin_unlock_irqrestore(&ctx->drain_lock, flags);
	return ret;
}

static int aj_subscribe_event(struct v4l2_fh *fh, const struct v4l2_event_subscription *sub)
{
	if (sub->type == V4L2_EVENT_EOS)
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	return v4l2_ctrl_subscribe_event(fh, sub);
}

static const struct v4l2_ioctl_ops aj_ioctl_ops = {
	.vidioc_querycap = aj_querycap,
	.vidioc_enum_fmt_vid_out = aj_enum_fmt_out,
	.vidioc_enum_fmt_vid_cap = aj_enum_fmt_cap,
	.vidioc_enum_framesizes = aj_enum_framesizes,
	.vidioc_g_fmt_vid_out_mplane = aj_g_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = aj_g_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = aj_try_fmt_out,
	.vidioc_try_fmt_vid_cap_mplane = aj_try_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane = aj_s_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = aj_s_fmt_cap,

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
	.vidioc_encoder_cmd = aj_encoder_cmd,

	.vidioc_subscribe_event = aj_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ---- queues ----------------------------------------------------------- */

static struct v4l2_pix_format_mplane *aj_q_fmt(struct apple_jpeg_ctx *ctx, u32 type)
{
	return V4L2_TYPE_IS_OUTPUT(type) ? &ctx->src : &ctx->dst;
}

static int aj_queue_setup(struct vb2_queue *q, unsigned int *nbufs, unsigned int *nplanes,
			  unsigned int sizes[], struct device *alloc_devs[])
{
	struct v4l2_pix_format_mplane *pix = aj_q_fmt(vb2_get_drv_priv(q), q->type);

	if (*nplanes) {
		if (*nplanes != pix->num_planes)
			return -EINVAL;
		for (unsigned int i = 0; i < *nplanes; i++)
			if (sizes[i] < pix->plane_fmt[i].sizeimage)
				return -EINVAL;
		return 0;
	}
	*nplanes = pix->num_planes;
	for (unsigned int i = 0; i < *nplanes; i++)
		sizes[i] = pix->plane_fmt[i].sizeimage;
	return 0;
}

static int aj_buf_prepare(struct vb2_buffer *vb)
{
	struct v4l2_pix_format_mplane *pix = aj_q_fmt(vb2_get_drv_priv(vb->vb2_queue), vb->type);

	for (unsigned int i = 0; i < pix->num_planes; i++) {
		if (vb2_plane_size(vb, i) < pix->plane_fmt[i].sizeimage)
			return -EINVAL;
		/* Luma (and so chroma) need 4-byte aligned starts, output 32-byte. */
		if (vb2_dma_contig_plane_dma_addr(vb, i) & 31)
			return -EINVAL;
		if (V4L2_TYPE_IS_OUTPUT(vb->type))
			vb2_set_plane_payload(vb, i, pix->plane_fmt[i].sizeimage);
		else
			vb2_set_plane_payload(vb, i, 0);
	}
	return 0;
}

static void aj_buf_queue(struct vb2_buffer *vb)
{
	struct apple_jpeg_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	unsigned long flags;

	spin_lock_irqsave(&ctx->drain_lock, flags);
	if (!V4L2_TYPE_IS_OUTPUT(vb->type) && vb2_is_streaming(vb->vb2_queue) &&
	    v4l2_m2m_dst_buf_is_last(ctx->fh.m2m_ctx)) {
		vb2_set_plane_payload(vb, 0, 0);
		vbuf->field = V4L2_FIELD_NONE;
		v4l2_m2m_last_buffer_done(ctx->fh.m2m_ctx, vbuf);
		v4l2_event_queue_fh(&ctx->fh, &aj_eos);
	} else {
		v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
	}
	spin_unlock_irqrestore(&ctx->drain_lock, flags);
}

static int aj_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct apple_jpeg_ctx *ctx = vb2_get_drv_priv(q);

	if (V4L2_TYPE_IS_OUTPUT(q->type))
		ctx->sequence = 0;
	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, q);
	return 0;
}

static void aj_stop_streaming(struct vb2_queue *q)
{
	struct apple_jpeg_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vbuf;

	for (;;) {
		vbuf = V4L2_TYPE_IS_OUTPUT(q->type) ? v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
						      v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, q);
	if (V4L2_TYPE_IS_OUTPUT(q->type) && v4l2_m2m_has_stopped(ctx->fh.m2m_ctx))
		v4l2_event_queue_fh(&ctx->fh, &aj_eos);
}

static const struct vb2_ops aj_qops = {
	.queue_setup = aj_queue_setup,
	.buf_prepare = aj_buf_prepare,
	.buf_queue = aj_buf_queue,
	.start_streaming = aj_start_streaming,
	.stop_streaming = aj_stop_streaming,
};

static int aj_init_queue(struct apple_jpeg_ctx *ctx, struct vb2_queue *q, u32 type)
{
	q->type = type;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = ctx;
	q->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	q->ops = &aj_qops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	q->lock = &ctx->jpeg->lock;
	q->dev = ctx->jpeg->dev;
	return vb2_queue_init(q);
}

static int aj_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	int ret = aj_init_queue(priv, src, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);

	return ret ?: aj_init_queue(priv, dst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
}

/* ---- file operations -------------------------------------------------- */

static int aj_s_ctrl(struct v4l2_ctrl *ctrl)
{
	return 0;	/* read when each job is built */
}

static const struct v4l2_ctrl_ops aj_ctrl_ops = {
	.s_ctrl = aj_s_ctrl,
};

static int aj_open(struct file *file)
{
	struct apple_jpeg *jpeg = video_drvdata(file);
	struct apple_jpeg_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	ctx->jpeg = jpeg;
	spin_lock_init(&ctx->drain_lock);
	ctx->src.width = 1920;
	ctx->src.height = 1080;
	ctx->src.pixelformat = V4L2_PIX_FMT_NV12;
	aj_fill_raw(&ctx->src);
	aj_fill_coded(&ctx->dst, &ctx->src, 0);
	v4l2_fh_init(&ctx->fh, video_devdata(file));

	v4l2_ctrl_handler_init(&ctx->hdl, 2);
	ctx->quality = v4l2_ctrl_new_std(&ctx->hdl, &aj_ctrl_ops,
					 V4L2_CID_JPEG_COMPRESSION_QUALITY, 1, 100, 1, 75);
	ctx->restart = v4l2_ctrl_new_std(&ctx->hdl, &aj_ctrl_ops,
					 V4L2_CID_JPEG_RESTART_INTERVAL, 0, U16_MAX, 1, 0);
	if (ctx->hdl.error) {
		ret = ctx->hdl.error;
		goto err_ctrls;
	}
	ctx->fh.ctrl_handler = &ctx->hdl;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(jpeg->m2m_dev, ctx, aj_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_ctrls;
	}
	v4l2_fh_add(&ctx->fh, file);
	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int aj_release(struct file *file)
{
	struct apple_jpeg_ctx *ctx = aj_file_to_ctx(file);
	struct apple_jpeg *jpeg = ctx->jpeg;

	mutex_lock(&jpeg->lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	mutex_unlock(&jpeg->lock);
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations aj_fops = {
	.owner = THIS_MODULE,
	.open = aj_open,
	.release = aj_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

/* ---- power management and probe --------------------------------------- */

static int aj_runtime_suspend(struct device *dev)
{
	return 0;
}

static int aj_runtime_resume(struct device *dev)
{
	struct apple_jpeg *jpeg = dev_get_drvdata(dev);

	/* The power-up state needs the reset before each job, done there. */
	jpeg->broken = false;
	return 0;
}

static int aj_suspend(struct device *dev)
{
	struct apple_jpeg *jpeg = dev_get_drvdata(dev);

	v4l2_m2m_suspend(jpeg->m2m_dev);
	return pm_runtime_force_suspend(dev);
}

static int aj_resume(struct device *dev)
{
	struct apple_jpeg *jpeg = dev_get_drvdata(dev);
	int ret = pm_runtime_force_resume(dev);

	if (ret)
		return ret;
	v4l2_m2m_resume(jpeg->m2m_dev);
	return 0;
}

static const struct dev_pm_ops aj_pm_ops = {
	RUNTIME_PM_OPS(aj_runtime_suspend, aj_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(aj_suspend, aj_resume)
};

static void aj_cleanup(void *data)
{
	struct apple_jpeg *jpeg = data;

	cancel_delayed_work_sync(&jpeg->watchdog);
	if (jpeg->m2m_dev)
		v4l2_m2m_release(jpeg->m2m_dev);
	v4l2_device_unregister(&jpeg->v4l2_dev);
}

/* Identify the engine: MODE must open the identity register first. */
static int aj_identify(struct apple_jpeg *jpeg)
{
	u32 hwrev;
	int ret = pm_runtime_resume_and_get(jpeg->dev);

	if (ret < 0)
		return ret;
	aj_write(jpeg, AJ_MODE, AJ_MODE_IDENTIFY);
	hwrev = aj_read(jpeg, jpeg->variant->layout->hwrev);
	pm_runtime_put_sync_suspend(jpeg->dev);
	if (!apple_jpeg_hwrev_supported(jpeg->variant, hwrev))
		return dev_err_probe(jpeg->dev, -ENODEV, "unsupported HWREV %#x\n", hwrev);
	dev_info(jpeg->dev, "JPEG encoder, HWREV %#x\n", hwrev);
	return 0;
}

static int aj_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_jpeg *jpeg;
	struct video_device *vdev;
	struct resource *res;
	int ret;

	jpeg = devm_kzalloc(dev, sizeof(*jpeg), GFP_KERNEL);
	if (!jpeg)
		return -ENOMEM;
	jpeg->dev = dev;
	jpeg->variant = of_device_get_match_data(dev);
	if (!jpeg->variant || !jpeg->variant->hwrev)
		return dev_err_probe(dev, -ENODEV, "engine layout not supported\n");
	if (!jpeg->variant->layout->reset_complete)
		return dev_err_probe(dev, -EOPNOTSUPP, "engine reset image incomplete\n");
	mutex_init(&jpeg->lock);
	spin_lock_init(&jpeg->hw_lock);
	INIT_DELAYED_WORK(&jpeg->watchdog, aj_watchdog);
	platform_set_drvdata(pdev, jpeg);

	if (!device_iommu_mapped(dev))
		return dev_err_probe(dev, -ENODEV, "no IOMMU\n");
	/*
	 * A second power domain, "perf", is the SoC performance request the
	 * engine needs for full speed; attach both with runtime PM links so it
	 * is held exactly while the engine is runtime-active.
	 */
	if (of_count_phandle_with_args(dev->of_node, "power-domains",
				       "#power-domain-cells") > 1) {
		static const struct dev_pm_domain_attach_data pd_data = {
			.pd_flags = PD_FLAG_DEV_LINK_ON,
		};

		ret = devm_pm_domain_attach_list(dev, &pd_data, &jpeg->pd_list);
		if (ret < 0)
			return dev_err_probe(dev, ret, "failed to attach power domains\n");
	} else if (!dev->pm_domain) {
		return dev_err_probe(dev, -ENODEV, "requires a power domain\n");
	}
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) != APPLE_JPEG_REG_SIZE)
		return -EINVAL;
	jpeg->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(jpeg->regs))
		return PTR_ERR(jpeg->regs);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(jpeg->variant->layout->dma_addr_bits));
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(dev, 100);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = aj_identify(jpeg);
	if (ret)
		return ret;

	/* The interrupt is only taken once identification passed. */
	jpeg->irq = platform_get_irq(pdev, 0);
	if (jpeg->irq < 0)
		return jpeg->irq;
	ret = devm_request_irq(dev, jpeg->irq, aj_irq, 0, dev_name(dev), jpeg);
	if (ret)
		return ret;

	ret = v4l2_device_register(dev, &jpeg->v4l2_dev);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, aj_cleanup, jpeg);
	if (ret)
		return ret;
	jpeg->m2m_dev = v4l2_m2m_init(&aj_m2m_ops);
	if (IS_ERR(jpeg->m2m_dev)) {
		ret = PTR_ERR(jpeg->m2m_dev);
		jpeg->m2m_dev = NULL;
		return ret;
	}

	vdev = &jpeg->vdev;
	strscpy(vdev->name, AJ_NAME, sizeof(vdev->name));
	vdev->fops = &aj_fops;
	vdev->ioctl_ops = &aj_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &jpeg->lock;
	vdev->v4l2_dev = &jpeg->v4l2_dev;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(vdev, jpeg);
	return video_register_device(vdev, VFL_TYPE_VIDEO, -1);
}

static void aj_remove(struct platform_device *pdev)
{
	struct apple_jpeg *jpeg = platform_get_drvdata(pdev);

	video_unregister_device(&jpeg->vdev);
}

static const struct of_device_id aj_of_match[] = {
	{ .compatible = "apple,t6000-jpeg", .data = &apple_jpeg_t6000 },
	{ .compatible = "apple,t8112-jpeg", .data = &apple_jpeg_t8112 },
	{ .compatible = "apple,t8122-jpeg", .data = &apple_jpeg_t8122 },
	{ .compatible = "apple,t6030-jpeg", .data = &apple_jpeg_t8122 },
	{ .compatible = "apple,t6031-jpeg", .data = &apple_jpeg_t8122 },
	{ .compatible = "apple,t6032-jpeg", .data = &apple_jpeg_t8122 },
	{ .compatible = "apple,t6041-jpeg", .data = &apple_jpeg_t6041 },
	{ .compatible = "apple,t8132-jpeg", .data = &apple_jpeg_t6041 },
	{ .compatible = "apple,t8140-jpeg", .data = &apple_jpeg_t8140 },
	{ }
};
MODULE_DEVICE_TABLE(of, aj_of_match);

static struct platform_driver aj_driver = {
	.probe = aj_probe,
	.remove = aj_remove,
	.driver = {
		.name = AJ_NAME,
		.of_match_table = aj_of_match,
		.pm = pm_ptr(&aj_pm_ops),
	},
};
module_platform_driver(aj_driver);

MODULE_DESCRIPTION("Apple Silicon JPEG encoder");
MODULE_LICENSE("GPL");
