// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple silicon scaler and colour-space converter (MSR), V4L2 mem2mem.
 *
 * Every Apple silicon SoC has one or more MSR engines: memory-to-memory
 * scalers with a programmable colour-space converter, chroma resampling,
 * flip/rotate and alpha fill. Hardware-version 10 (M1) through 22 (A18 Pro)
 * are programmed directly through MMIO and report completion with an
 * interrupt; later generations front the engine with firmware and are not
 * handled here.
 *
 * The OUTPUT queue takes the source image, the CAPTURE queue the result.
 * This version converts between formats at the same size: Y'CbCr <->
 * R'G'B' with the colorimetry of the Y'CbCr side, chroma resampling and
 * alpha fill. Scaling comes once the filter path is qualified.
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
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "apple-scaler-hw.h"
#include "apple-scaler-regs.h"

#define ASC_NAME		"apple-scaler"
#define ASC_MIN_REG_SIZE	0x28000
#define ASC_MIN_SIZE		16
#define ASC_MAX_SIZE		16384
#define ASC_STRIDE_ALIGN	128
#define ASC_JOB_TIMEOUT_MS	500
#define ASC_IDLE_TIMEOUT_US	10000

/**
 * struct apple_scaler_variant - per-SoC engine description
 * @hw_version: MSR hardware-version of this SoC (from the hardware description)
 * @num_reg: number of register apertures the binding describes
 * @msr20: the engine has the MSR20-generation control block
 */
struct apple_scaler_variant {
	u32 hw_version;
	unsigned int num_reg;
	bool msr20;
};

struct apple_scaler {
	struct device *dev;
	void __iomem *regs;
	resource_size_t regs_size;
	const struct apple_scaler_variant *variant;
	struct dev_pm_domain_list *pd_list;
	struct reset_control *rst;
	struct mutex lock;	/* serialises ioctls and queue state */
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
	int irq;
	u32 id;

	spinlock_t hw_lock;	/* protects the fields below */
	struct apple_scaler_ctx *active;
	bool need_init;
	u32 transform_id;
	struct delayed_work watchdog;
	struct asc_reg writes[ASC_JOB_MAX_WRITES];
};

struct apple_scaler_ctx {
	struct v4l2_fh fh;
	struct apple_scaler *asc;
	struct v4l2_pix_format_mplane src;	/* OUTPUT queue */
	struct v4l2_pix_format_mplane dst;	/* CAPTURE queue */
	u32 sequence;
};

static struct apple_scaler_ctx *asc_file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct apple_scaler_ctx, fh);
}

static u32 asc_read(struct apple_scaler *asc, u32 reg)
{
	return readl_relaxed(asc->regs + reg);
}

static void asc_write(struct apple_scaler *asc, u32 reg, u32 val)
{
	writel_relaxed(val, asc->regs + reg);
}

/* ---- hardware --------------------------------------------------------- */


/*
 * After power-up or a failed job: wait for idle, pulse the engine reset
 * (no register may be read while it is held), clear stale status and
 * enable completion and errors.
 */
static int asc_hw_init(struct apple_scaler *asc)
{
	u32 v;
	int ret;

	ret = readl_relaxed_poll_timeout(asc->regs + ASC_IS_RUNNING, v, !(v & BIT(0)),
					 10, ASC_IDLE_TIMEOUT_US);
	if (ret)
		return ret;
	asc_write(asc, ASC_RESET, ASC_RESET_ENGINE);
	asc_write(asc, ASC_RESET, 0);
	ret = readl_relaxed_poll_timeout(asc->regs + ASC_IS_RUNNING, v, !(v & BIT(0)),
					 10, ASC_IDLE_TIMEOUT_US);
	if (ret)
		return ret;
	asc_write(asc, ASC_IRQ_STATUS, asc_read(asc, ASC_IRQ_STATUS));
	asc_write(asc, ASC_IRQ_ENABLE, ASC_IRQ_DONE | ASC_IRQ_ERRORS);
	asc->need_init = false;
	return 0;
}

static void asc_finish(struct apple_scaler *asc, struct apple_scaler_ctx *ctx, int err)
{
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	enum vb2_buffer_state state = err ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE;
	struct vb2_v4l2_buffer *src, *dst;
	unsigned int i;

	src = v4l2_m2m_src_buf_remove(m2m);
	dst = v4l2_m2m_dst_buf_remove(m2m);
	if (src && dst) {
		for (i = 0; i < dst->vb2_buf.num_planes; i++)
			vb2_set_plane_payload(&dst->vb2_buf, i,
					      err ? 0 : ctx->dst.plane_fmt[i].sizeimage);
		v4l2_m2m_buf_copy_metadata(src, dst);
		dst->sequence = ctx->sequence++;
		v4l2_m2m_buf_done(src, state);
		v4l2_m2m_buf_done(dst, state);
	}
	pm_runtime_put_autosuspend(asc->dev);
	v4l2_m2m_job_finish(asc->m2m_dev, m2m);
}

/* A failed job leaves the engine in an unknown state: reset before the next. */
static void asc_recover(struct apple_scaler *asc)
{
	asc->need_init = true;
}

static irqreturn_t asc_irq(int irq, void *data)
{
	struct apple_scaler *asc = data;
	struct apple_scaler_ctx *ctx;
	u32 status;
	int err = 0;

	if (pm_runtime_get_if_active(asc->dev) <= 0)
		return IRQ_NONE;

	spin_lock(&asc->hw_lock);
	status = asc_read(asc, ASC_IRQ_STATUS);
	if (!status) {
		spin_unlock(&asc->hw_lock);
		pm_runtime_put_noidle(asc->dev);
		return IRQ_NONE;
	}
	asc_write(asc, ASC_IRQ_STATUS, status);
	ctx = asc->active;
	if (!ctx || !(status & (ASC_IRQ_DONE | ASC_IRQ_ERRORS))) {
		spin_unlock(&asc->hw_lock);
		dev_warn_ratelimited(asc->dev, "unexpected interrupt, status %#x\n", status);
		pm_runtime_put_noidle(asc->dev);
		return IRQ_HANDLED;
	}
	asc->active = NULL;
	if (status & ASC_IRQ_ERRORS) {
		dev_err_ratelimited(asc->dev, "job failed: status %#x debug %#x\n",
				    status, asc_read(asc, ASC_DEBUG_STATUS));
		err = -EIO;
	}
	spin_unlock(&asc->hw_lock);
	pm_runtime_put_noidle(asc->dev);

	cancel_delayed_work(&asc->watchdog);
	if (err)
		asc_recover(asc);
	asc_finish(asc, ctx, err);
	return IRQ_HANDLED;
}

static void asc_watchdog(struct work_struct *work)
{
	struct apple_scaler *asc = container_of(work, struct apple_scaler, watchdog.work);
	struct apple_scaler_ctx *ctx;
	unsigned long flags;

	spin_lock_irqsave(&asc->hw_lock, flags);
	ctx = asc->active;
	asc->active = NULL;
	if (ctx)
		asc_write(asc, ASC_IRQ_ENABLE, 0);
	spin_unlock_irqrestore(&asc->hw_lock, flags);
	if (ctx) {
		dev_err(asc->dev, "job timed out (running %#x)\n",
			asc_read(asc, ASC_IS_RUNNING));
		asc_recover(asc);
		asc_finish(asc, ctx, -ETIMEDOUT);
	}
}

static void asc_image(struct asc_image *img, const struct v4l2_pix_format_mplane *pix,
		      struct vb2_buffer *vb)
{
	unsigned int i;

	img->fmt = asc_find_format(pix->pixelformat);
	img->width = pix->width;
	img->height = pix->height;
	if (img->fmt->lossless) {
		struct asc_lossless_geom g;
		u64 base = vb2_dma_contig_plane_dma_addr(vb, 0);

		asc_lossless_geom(pix->width, pix->height, &g);
		img->dva[0] = base;
		img->dva[1] = base + g.c;
		img->meta[0] = base + g.y_meta;
		img->meta[1] = base + g.c_meta;
		img->stride[0] = g.y_pitch;
		img->stride[1] = g.c_pitch;
		return;
	}
	for (i = 0; i < img->fmt->planes; i++) {
		if (pix->num_planes > 1) {
			img->dva[i] = vb2_dma_contig_plane_dma_addr(vb, i);
			img->stride[i] = pix->plane_fmt[i].bytesperline;
		} else {
			/* Chroma follows luma in the same buffer, at the same stride. */
			img->dva[i] = vb2_dma_contig_plane_dma_addr(vb, 0) +
				      (u64)i * pix->plane_fmt[0].bytesperline * pix->height;
			img->stride[i] = pix->plane_fmt[0].bytesperline;
		}
	}
}

static void asc_device_run(void *priv)
{
	struct apple_scaler_ctx *ctx = priv;
	struct apple_scaler *asc = ctx->asc;
	struct vb2_v4l2_buffer *src, *dst;
	struct asc_job job = {};
	struct asc_csc csc;
	unsigned long flags;
	int n, ret;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	ret = pm_runtime_resume_and_get(asc->dev);
	if (ret < 0) {
		pm_runtime_get_noresume(asc->dev);
		asc_finish(asc, ctx, ret);
		return;
	}

	/*
	 * Engine reset and interrupt setup, once per power-up and after any
	 * failure. If the engine does not go idle, fall back to the PMGR reset
	 * of its power domain and try once more.
	 */
	if (asc->need_init) {
		ret = asc_hw_init(asc);
		if (ret && asc->rst && !reset_control_reset(asc->rst))
			ret = asc_hw_init(asc);
		if (ret) {
			dev_err_ratelimited(asc->dev, "engine does not go idle: %d\n", ret);
			asc_finish(asc, ctx, -EIO);
			return;
		}
	}

	asc_image(&job.src, &ctx->src, &src->vb2_buf);
	asc_image(&job.dst, &ctx->dst, &dst->vb2_buf);
	if (job.src.fmt->yuv != job.dst.fmt->yuv) {
		const struct v4l2_pix_format_mplane *y = job.src.fmt->yuv ? &ctx->src : &ctx->dst;

		ret = asc_csc_compute(y->ycbcr_enc, y->quantization, job.src.fmt->yuv, &csc);
		if (ret) {
			asc_finish(asc, ctx, ret);
			return;
		}
		job.csc = &csc;
	}

	spin_lock_irqsave(&asc->hw_lock, flags);
	/* Job tags as the engine is driven on macOS: a counter above bit 3, bit 2 set. */
	job.transform_id = (++asc->transform_id << 3) | BIT(2);
	job.msr20 = asc->variant->msr20;
	n = asc_job_build(&job, asc->writes, ARRAY_SIZE(asc->writes));
	if (n < 0) {
		spin_unlock_irqrestore(&asc->hw_lock, flags);
		asc_finish(asc, ctx, n);
		return;
	}
	/* The job list must fit this engine's register aperture. */
	for (int i = 0; i < n; i++) {
		if (asc->writes[i].off + sizeof(u32) > asc->regs_size) {
			spin_unlock_irqrestore(&asc->hw_lock, flags);
			dev_err_ratelimited(asc->dev, "job register %#x is outside the aperture\n",
					    asc->writes[i].off);
			asc_finish(asc, ctx, -EINVAL);
			return;
		}
	}
	asc->active = ctx;
	/* Buffers must be visible to the engine before START, the last write. */
	dma_wmb();
	for (int i = 0; i < n; i++)
		writel(asc->writes[i].val, asc->regs + asc->writes[i].off);
	spin_unlock_irqrestore(&asc->hw_lock, flags);
	schedule_delayed_work(&asc->watchdog, msecs_to_jiffies(ASC_JOB_TIMEOUT_MS));
}

static const struct v4l2_m2m_ops asc_m2m_ops = {
	.device_run = asc_device_run,
};

/* ---- formats ---------------------------------------------------------- */

/*
 * Fill in a format the engine can take. The CAPTURE side has the OUTPUT
 * size (no scaling yet). The Y'CbCr side of a conversion carries the
 * colorimetry; the R'G'B' side is full range with the same primaries and
 * transfer function, since the engine converts only the encoding.
 */
static void asc_fill(struct v4l2_pix_format_mplane *pix, const struct v4l2_pix_format_mplane *ref,
		     u32 default_fourcc)
{
	const struct asc_format *f = asc_find_format(pix->pixelformat);
	u32 w = ref ? ref->width : pix->width, h = ref ? ref->height : pix->height;
	struct v4l2_pix_format_mplane in = *pix;

	if (!f)
		f = asc_find_format(default_fourcc);
	w = clamp_t(u32, ALIGN(w, f->hsub), ASC_MIN_SIZE, ASC_MAX_SIZE);
	h = clamp_t(u32, ALIGN(h, f->vsub), ASC_MIN_SIZE, ASC_MAX_SIZE);

	memset(pix, 0, sizeof(*pix));
	if (f->lossless) {
		struct asc_lossless_geom g;

		asc_lossless_geom(w, h, &g);
		pix->pixelformat = f->fourcc;
		pix->width = w;
		pix->height = h;
		pix->num_planes = 1;
		pix->plane_fmt[0].bytesperline = g.y_pitch;
		pix->plane_fmt[0].sizeimage = g.bytes;
	} else {
		v4l2_fill_pixfmt_mp_aligned(pix, f->fourcc, w, h, ASC_STRIDE_ALIGN);
	}
	pix->field = V4L2_FIELD_NONE;

	if (ref && ref->colorspace != V4L2_COLORSPACE_DEFAULT) {
		pix->colorspace = ref->colorspace;
		pix->xfer_func = ref->xfer_func;
	} else {
		pix->colorspace = in.colorspace ?: V4L2_COLORSPACE_REC709;
		pix->xfer_func = in.xfer_func;
	}
	if (f->yuv) {
		pix->ycbcr_enc = in.ycbcr_enc;
		if (ref && v4l2_is_format_yuv(v4l2_format_info(ref->pixelformat)))
			pix->ycbcr_enc = ref->ycbcr_enc;
		if (pix->ycbcr_enc == V4L2_YCBCR_ENC_DEFAULT)
			pix->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(pix->colorspace);
		/* Only the encodings the matrix can express. */
		if (pix->ycbcr_enc != V4L2_YCBCR_ENC_601 && pix->ycbcr_enc != V4L2_YCBCR_ENC_709 &&
		    pix->ycbcr_enc != V4L2_YCBCR_ENC_BT2020)
			pix->ycbcr_enc = V4L2_YCBCR_ENC_709;
		pix->quantization = in.quantization;
		if (ref && v4l2_is_format_yuv(v4l2_format_info(ref->pixelformat)))
			pix->quantization = ref->quantization;
		if (pix->quantization == V4L2_QUANTIZATION_DEFAULT)
			pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	} else {
		pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
		pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	}
	if (pix->xfer_func == V4L2_XFER_FUNC_DEFAULT)
		pix->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(pix->colorspace);
}

static int asc_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, ASC_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Apple scaler/CSC", sizeof(cap->card));
	return 0;
}

static int asc_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	const struct asc_format *fmt = asc_format_by_index(f->index);

	if (!fmt)
		return -EINVAL;
	f->pixelformat = fmt->fourcc;
	return 0;
}

static int asc_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *fs)
{
	const struct asc_format *f = asc_find_format(fs->pixel_format);

	if (fs->index || !f)
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fs->stepwise = (struct v4l2_frmsize_stepwise){
		ASC_MIN_SIZE, ASC_MAX_SIZE, f->hsub, ASC_MIN_SIZE, ASC_MAX_SIZE, f->vsub,
	};
	return 0;
}

static int asc_g_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = asc_file_to_ctx(file)->src;
	return 0;
}

static int asc_g_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = asc_file_to_ctx(file)->dst;
	return 0;
}

static int asc_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	asc_fill(&f->fmt.pix_mp, NULL, V4L2_PIX_FMT_NV12M);
	return 0;
}

static int asc_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	asc_fill(&f->fmt.pix_mp, &asc_file_to_ctx(file)->src, V4L2_PIX_FMT_ARGB32);
	return 0;
}

static int asc_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apple_scaler_ctx *ctx = asc_file_to_ctx(file);

	if (vb2_is_busy(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx)) ||
	    vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	asc_fill(&f->fmt.pix_mp, NULL, V4L2_PIX_FMT_NV12M);
	ctx->src = f->fmt.pix_mp;
	asc_fill(&ctx->dst, &ctx->src, V4L2_PIX_FMT_ARGB32);
	return 0;
}

static int asc_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apple_scaler_ctx *ctx = asc_file_to_ctx(file);

	if (vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	asc_fill(&f->fmt.pix_mp, &ctx->src, V4L2_PIX_FMT_ARGB32);
	ctx->dst = f->fmt.pix_mp;
	return 0;
}

static const struct v4l2_ioctl_ops asc_ioctl_ops = {
	.vidioc_querycap = asc_querycap,
	.vidioc_enum_fmt_vid_out = asc_enum_fmt,
	.vidioc_enum_fmt_vid_cap = asc_enum_fmt,
	.vidioc_enum_framesizes = asc_enum_framesizes,
	.vidioc_g_fmt_vid_out_mplane = asc_g_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = asc_g_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = asc_try_fmt_out,
	.vidioc_try_fmt_vid_cap_mplane = asc_try_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane = asc_s_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = asc_s_fmt_cap,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ---- queues ----------------------------------------------------------- */

static struct v4l2_pix_format_mplane *asc_q_fmt(struct apple_scaler_ctx *ctx, u32 type)
{
	return V4L2_TYPE_IS_OUTPUT(type) ? &ctx->src : &ctx->dst;
}

static int asc_queue_setup(struct vb2_queue *q, unsigned int *nbufs, unsigned int *nplanes,
			   unsigned int sizes[], struct device *alloc_devs[])
{
	struct v4l2_pix_format_mplane *pix = asc_q_fmt(vb2_get_drv_priv(q), q->type);
	unsigned int i;

	if (*nplanes) {
		if (*nplanes != pix->num_planes)
			return -EINVAL;
		for (i = 0; i < *nplanes; i++)
			if (sizes[i] < pix->plane_fmt[i].sizeimage)
				return -EINVAL;
		return 0;
	}
	*nplanes = pix->num_planes;
	for (i = 0; i < *nplanes; i++)
		sizes[i] = pix->plane_fmt[i].sizeimage;
	return 0;
}

static int asc_buf_prepare(struct vb2_buffer *vb)
{
	struct v4l2_pix_format_mplane *pix = asc_q_fmt(vb2_get_drv_priv(vb->vb2_queue), vb->type);
	unsigned int i;

	for (i = 0; i < pix->num_planes; i++) {
		if (vb2_plane_size(vb, i) < pix->plane_fmt[i].sizeimage)
			return -EINVAL;
		if (vb2_dma_contig_plane_dma_addr(vb, i) & (ASC_STRIDE_ALIGN - 1))
			return -EINVAL;
		if (V4L2_TYPE_IS_OUTPUT(vb->type))
			vb2_set_plane_payload(vb, i, pix->plane_fmt[i].sizeimage);
		else
			vb2_set_plane_payload(vb, i, 0);
	}
	return 0;
}

static void asc_buf_queue(struct vb2_buffer *vb)
{
	struct apple_scaler_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int asc_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct apple_scaler_ctx *ctx = vb2_get_drv_priv(q);

	if (V4L2_TYPE_IS_OUTPUT(q->type))
		ctx->sequence = 0;
	return 0;
}

static void asc_stop_streaming(struct vb2_queue *q)
{
	struct apple_scaler_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vbuf;

	for (;;) {
		vbuf = V4L2_TYPE_IS_OUTPUT(q->type) ? v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
						      v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops asc_qops = {
	.queue_setup = asc_queue_setup,
	.buf_prepare = asc_buf_prepare,
	.buf_queue = asc_buf_queue,
	.start_streaming = asc_start_streaming,
	.stop_streaming = asc_stop_streaming,
};

static int asc_init_queue(struct apple_scaler_ctx *ctx, struct vb2_queue *q, u32 type)
{
	q->type = type;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = ctx;
	q->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	q->ops = &asc_qops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	q->lock = &ctx->asc->lock;
	q->dev = ctx->asc->dev;
	return vb2_queue_init(q);
}

static int asc_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	int ret = asc_init_queue(priv, src, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);

	return ret ?: asc_init_queue(priv, dst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
}

/* ---- file operations -------------------------------------------------- */

static int asc_open(struct file *file)
{
	struct apple_scaler *asc = video_drvdata(file);
	struct apple_scaler_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	ctx->asc = asc;
	ctx->src.width = 1920;
	ctx->src.height = 1080;
	ctx->src.pixelformat = V4L2_PIX_FMT_NV12M;
	asc_fill(&ctx->src, NULL, V4L2_PIX_FMT_NV12M);
	ctx->dst.pixelformat = V4L2_PIX_FMT_ARGB32;
	asc_fill(&ctx->dst, &ctx->src, V4L2_PIX_FMT_ARGB32);
	v4l2_fh_init(&ctx->fh, video_devdata(file));

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(asc->m2m_dev, ctx, asc_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		v4l2_fh_exit(&ctx->fh);
		kfree(ctx);
		return ret;
	}
	v4l2_fh_add(&ctx->fh, file);
	return 0;
}

static int asc_release(struct file *file)
{
	struct apple_scaler_ctx *ctx = asc_file_to_ctx(file);
	struct apple_scaler *asc = ctx->asc;

	mutex_lock(&asc->lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	mutex_unlock(&asc->lock);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations asc_fops = {
	.owner = THIS_MODULE,
	.open = asc_open,
	.release = asc_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

/* ---- power management and probe --------------------------------------- */

static int asc_runtime_suspend(struct device *dev)
{
	return 0;
}

static int asc_runtime_resume(struct device *dev)
{
	struct apple_scaler *asc = dev_get_drvdata(dev);

	/* The engine loses its state when powered down. */
	asc->need_init = true;
	return 0;
}

static int asc_suspend(struct device *dev)
{
	struct apple_scaler *asc = dev_get_drvdata(dev);

	v4l2_m2m_suspend(asc->m2m_dev);
	return pm_runtime_force_suspend(dev);
}

static int asc_resume(struct device *dev)
{
	struct apple_scaler *asc = dev_get_drvdata(dev);
	int ret = pm_runtime_force_resume(dev);

	if (ret)
		return ret;
	v4l2_m2m_resume(asc->m2m_dev);
	return 0;
}

static const struct dev_pm_ops asc_pm_ops = {
	RUNTIME_PM_OPS(asc_runtime_suspend, asc_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(asc_suspend, asc_resume)
};

static int asc_identify(struct apple_scaler *asc)
{
	u32 running, status;
	int ret;

	ret = pm_runtime_resume_and_get(asc->dev);
	if (ret < 0)
		return dev_err_probe(asc->dev, ret, "failed to power up\n");

	asc->id = asc_read(asc, ASC_HW_VERSION);
	running = asc_read(asc, ASC_IS_RUNNING);
	status = asc_read(asc, ASC_IRQ_STATUS);
	/* Masked until the first job enables it. */
	asc_write(asc, ASC_IRQ_ENABLE, 0);
	if (status)
		asc_write(asc, ASC_IRQ_STATUS, status);

	pm_runtime_put_sync_suspend(asc->dev);

	if (running)
		dev_warn(asc->dev, "engine busy at probe (%#x)\n", running);
	dev_info(asc->dev, "MSR hardware-version %u, id %#x, stale status %#x\n",
		 asc->variant->hw_version, asc->id, status);
	return 0;
}

static void asc_cleanup(void *data)
{
	struct apple_scaler *asc = data;

	cancel_delayed_work_sync(&asc->watchdog);
	if (asc->m2m_dev)
		v4l2_m2m_release(asc->m2m_dev);
	v4l2_device_unregister(&asc->v4l2_dev);
}

static int asc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_scaler *asc;
	struct video_device *vdev;
	struct resource *res;
	int ret;

	asc = devm_kzalloc(dev, sizeof(*asc), GFP_KERNEL);
	if (!asc)
		return -ENOMEM;
	asc->dev = dev;
	asc->variant = of_device_get_match_data(dev);
	if (!asc->variant)
		return -ENODEV;
	mutex_init(&asc->lock);
	spin_lock_init(&asc->hw_lock);
	INIT_DELAYED_WORK(&asc->watchdog, asc_watchdog);
	asc->need_init = true;
	platform_set_drvdata(pdev, asc);

	if (!device_iommu_mapped(dev))
		return dev_err_probe(dev, -ENODEV, "no IOMMU\n");

	/*
	 * MSR-V and the ASE core domain below it; link both so they are on
	 * exactly while the engine is runtime-active.
	 */
	if (of_count_phandle_with_args(dev->of_node, "power-domains",
				       "#power-domain-cells") > 1) {
		static const struct dev_pm_domain_attach_data pd_data = {
			.pd_flags = PD_FLAG_DEV_LINK_ON,
		};

		ret = devm_pm_domain_attach_list(dev, &pd_data, &asc->pd_list);
		if (ret < 0)
			return dev_err_probe(dev, ret, "failed to attach power domains\n");
	} else if (!dev->pm_domain) {
		return dev_err_probe(dev, -ENODEV, "requires a power domain\n");
	}

	if (!platform_get_resource(pdev, IORESOURCE_MEM, asc->variant->num_reg - 1))
		return dev_err_probe(dev, -EINVAL, "expected %u register ranges\n",
				     asc->variant->num_reg);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (resource_size(res) < ASC_MIN_REG_SIZE)
		return dev_err_probe(dev, -EINVAL, "MSR aperture too small\n");
	asc->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(asc->regs))
		return PTR_ERR(asc->regs);
	asc->regs_size = resource_size(res);

	asc->rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(asc->rst))
		return dev_err_probe(dev, PTR_ERR(asc->rst), "failed to get reset\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(dev, 100);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = asc_identify(asc);
	if (ret)
		return ret;

	/* Interrupt 0 is the MSR (completion/error); 1 is the PIODMA engine. */
	asc->irq = platform_get_irq(pdev, 0);
	if (asc->irq < 0)
		return asc->irq;
	ret = devm_request_irq(dev, asc->irq, asc_irq, 0, dev_name(dev), asc);
	if (ret)
		return ret;

	ret = v4l2_device_register(dev, &asc->v4l2_dev);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, asc_cleanup, asc);
	if (ret)
		return ret;
	asc->m2m_dev = v4l2_m2m_init(&asc_m2m_ops);
	if (IS_ERR(asc->m2m_dev)) {
		ret = PTR_ERR(asc->m2m_dev);
		asc->m2m_dev = NULL;
		return ret;
	}

	vdev = &asc->vdev;
	strscpy(vdev->name, ASC_NAME, sizeof(vdev->name));
	vdev->fops = &asc_fops;
	vdev->ioctl_ops = &asc_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &asc->lock;
	vdev->v4l2_dev = &asc->v4l2_dev;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(vdev, asc);
	return video_register_device(vdev, VFL_TYPE_VIDEO, -1);
}

static void asc_remove(struct platform_device *pdev)
{
	struct apple_scaler *asc = platform_get_drvdata(pdev);

	video_unregister_device(&asc->vdev);
}

static const struct apple_scaler_variant apple_scaler_t6000 = {
	.hw_version = 12,
	.num_reg = 3,
};

static const struct apple_scaler_variant apple_scaler_t8112 = {
	.hw_version = 13,
	.num_reg = 3,
};

static const struct apple_scaler_variant apple_scaler_t6041 = {
	.hw_version = 21,
	.num_reg = 4,
	.msr20 = true,
};

static const struct apple_scaler_variant apple_scaler_t8140 = {
	.hw_version = 22,
	.num_reg = 4,
	.msr20 = true,
};

static const struct of_device_id asc_of_match[] = {
	{ .compatible = "apple,t6000-scaler", .data = &apple_scaler_t6000 },
	{ .compatible = "apple,t8112-scaler", .data = &apple_scaler_t8112 },
	{ .compatible = "apple,t6041-scaler", .data = &apple_scaler_t6041 },
	{ .compatible = "apple,t8140-scaler", .data = &apple_scaler_t8140 },
	{ }
};
MODULE_DEVICE_TABLE(of, asc_of_match);

static struct platform_driver asc_driver = {
	.probe = asc_probe,
	.remove = asc_remove,
	.driver = {
		.name = ASC_NAME,
		.of_match_table = asc_of_match,
		.pm = pm_ptr(&asc_pm_ops),
	},
};
module_platform_driver(asc_driver);

MODULE_DESCRIPTION("Apple silicon scaler and colour-space converter");
MODULE_LICENSE("GPL");
