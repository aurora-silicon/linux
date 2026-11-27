// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple silicon scaler and colour-space converter (MSR), V4L2 mem2mem.
 *
 * Every Apple silicon SoC has one or more MSR engines: memory-to-memory
 * scalers with a programmable colour-space converter, chroma resampling,
 * flip/rotate and alpha fill. Hardware-version 10 (M1) through 22 (A18 Pro)
 * are programmed directly through MMIO and report completion with an
 * interrupt. Hardware-version 23 (M5) fronts the engine with firmware: the
 * driver builds each job's register image and queues it to the firmware.
 *
 * The OUTPUT queue takes the source image, the CAPTURE queue the result.
 * The MMIO engines convert between formats at the same size: Y'CbCr <->
 * R'G'B' with the colorimetry of the Y'CbCr side, chroma resampling and
 * alpha fill. MSR23 converts and scales (up to 4x either way per axis). It
 * also reads a crop of the source (OUTPUT crop selection), writes into a
 * rectangle of the destination (CAPTURE compose selection), rotates in
 * quarter turns and mirrors (V4L2_CID_ROTATE, _HFLIP, _VFLIP: the mirrors
 * apply to the rotated picture).
 */
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/firmware.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/sizes.h>
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

#include "../apple-video-nr.h"
#include "apple-scaler-hw.h"
#include "apple-scaler-msr23.h"
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
 * @fw_fronted: hardware-version 23 and later: jobs go through firmware queues
 *	(see asc_v23_probe())
 * @fw_name: fw_fronted: the core's image, uploaded only when the SRAM holds none
 * @powerup: fw_fronted: the register writes that prepare the engine before its
 *	core starts
 * @num_powerup: the number of @powerup writes
 */
struct apple_scaler_variant {
	u32 hw_version;
	unsigned int num_reg;
	bool msr20;
	bool fw_fronted;
	const char *fw_name;
	const struct msr23_wr *powerup;
	unsigned int num_powerup;
};

struct msr23;
struct msr23_wr;

struct apple_scaler {
	struct device *dev;
	struct msr23 *msr;	/* MSR23 (fw_fronted): firmware core and job queues */
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
	/*
	 * The CAPTURE colorimetry asked for (an RGB -> YUV conversion target),
	 * whichever queue is set first.
	 */
	u32 dst_ycbcr_req, dst_quant_req;
	u32 sequence;

	/* MSR23 geometry: the source region read, the destination region written. */
	struct v4l2_rect crop, compose;
	struct v4l2_ctrl_handler hdl;
	struct v4l2_ctrl *rotate_ctrl;
	/* fixed while either queue streams: msr23_work() reads them without asc->lock */
	struct v4l2_ctrl *job_ctrl[3];
	u16 rotate;
	bool hflip, vflip;
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

static void msr23_device_run(struct apple_scaler *asc, struct apple_scaler_ctx *ctx);

static void asc_device_run(void *priv)
{
	struct apple_scaler_ctx *ctx = priv;
	struct apple_scaler *asc = ctx->asc;
	struct vb2_v4l2_buffer *src, *dst;
	struct asc_job job = {};
	struct asc_csc csc;
	unsigned long flags;
	int n, ret;

	if (asc->variant->fw_fronted) {
		msr23_device_run(asc, ctx);
		return;
	}
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
/*
 * MSR23 builds its jobs (apple-scaler-msr23.c) for B,G,R,A, NV12, NV16, NV24,
 * P010 and GREY. The older engines never offer the MSR23-only formats.
 */
static bool asc_fmt_ok(const struct apple_scaler *asc, const struct asc_format *f)
{
	if (!f)
		return false;
	if (asc->variant->fw_fronted) {
		switch (f->fourcc) {
		case V4L2_PIX_FMT_ABGR32:
		case V4L2_PIX_FMT_NV12:
		case V4L2_PIX_FMT_NV12M:
		case V4L2_PIX_FMT_NV16:
		case V4L2_PIX_FMT_NV24:
		case V4L2_PIX_FMT_P010:
		case V4L2_PIX_FMT_GREY:
			return true;
		}
		return false;
	}
	return !f->msr23_only;
}

static void asc_fill(const struct apple_scaler *asc, struct v4l2_pix_format_mplane *pix,
		     const struct v4l2_pix_format_mplane *ref, u32 default_fourcc)
{
	const struct asc_format *f = asc_find_format(pix->pixelformat);
	/* MSR23 scales: its CAPTURE side keeps its own size. */
	bool own = asc->variant->fw_fronted || !ref;
	u32 w = own ? pix->width : ref->width, h = own ? pix->height : ref->height;
	struct v4l2_pix_format_mplane in = *pix;

	if (!asc_fmt_ok(asc, f) && asc->variant->fw_fronted)
		f = asc_find_format(default_fourcc == V4L2_PIX_FMT_ARGB32 ?
				    V4L2_PIX_FMT_ABGR32 : V4L2_PIX_FMT_NV12);
	else if (!asc_fmt_ok(asc, f))
		f = asc_find_format(default_fourcc);
	if (asc->variant->fw_fronted) {
		/* the builder's range: even sizes, 64 to 4096 */
		w = clamp_t(u32, ALIGN(w, 2), MSR23_MIN_SIZE, MSR23_MAX_WIDTH);
		h = clamp_t(u32, ALIGN(h, 2), MSR23_MIN_SIZE, MSR23_MAX_WIDTH);
	} else {
		w = clamp_t(u32, ALIGN(w, f->hsub), ASC_MIN_SIZE, ASC_MAX_SIZE);
		h = clamp_t(u32, ALIGN(h, f->vsub), ASC_MIN_SIZE, ASC_MAX_SIZE);
	}

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
		/*
		 * MSR23: 16-byte aligned strides (NV12 1440 wide: 1440 bytes);
		 * the engine takes them.
		 */
		v4l2_fill_pixfmt_mp_aligned(pix, f->fourcc, w, h,
					    asc->variant->fw_fronted ? 16 : ASC_STRIDE_ALIGN);
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

static struct v4l2_rect asc_full_rect(const struct v4l2_pix_format_mplane *pix)
{
	return (struct v4l2_rect){ 0, 0, pix->width, pix->height };
}

/*
 * After a format change: a rectangle that covered the whole old picture
 * covers the whole new one; one set by the application stays while it fits
 * (v4l2-ctl sets the CAPTURE format again after the selection, when it starts
 * streaming).
 */
static void asc_fit_rect(struct v4l2_rect *r, u32 old_w, u32 old_h,
			 const struct v4l2_pix_format_mplane *pix)
{
	bool whole = !r->left && !r->top && r->width == old_w && r->height == old_h;

	if (whole || (u64)r->left + r->width > pix->width ||
	    (u64)r->top + r->height > pix->height)
		*r = asc_full_rect(pix);
}

static int asc_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, ASC_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Apple scaler/CSC", sizeof(cap->card));
	return 0;
}

static int asc_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	const struct apple_scaler *asc = asc_file_to_ctx(file)->asc;
	const struct asc_format *fmt;
	unsigned int i, n = 0;

	for (i = 0; (fmt = asc_format_by_index(i)); i++) {
		if (!asc_fmt_ok(asc, fmt))
			continue;
		if (n++ == f->index) {
			f->pixelformat = fmt->fourcc;
			return 0;
		}
	}
	return -EINVAL;
}

static int asc_enum_framesizes(struct file *file, void *priv, struct v4l2_frmsizeenum *fs)
{
	const struct apple_scaler *asc = asc_file_to_ctx(file)->asc;
	const struct asc_format *f = asc_find_format(fs->pixel_format);

	if (fs->index || !asc_fmt_ok(asc, f))
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	if (asc->variant->fw_fronted)
		fs->stepwise = (struct v4l2_frmsize_stepwise){
			MSR23_MIN_SIZE, MSR23_MAX_WIDTH, 2, MSR23_MIN_SIZE, MSR23_MAX_WIDTH, 2,
		};
	else
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
	asc_fill(asc_file_to_ctx(file)->asc, &f->fmt.pix_mp, NULL, V4L2_PIX_FMT_NV12M);
	return 0;
}

static int asc_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apple_scaler_ctx *ctx = asc_file_to_ctx(file);

	asc_fill(ctx->asc, &f->fmt.pix_mp, &ctx->src, V4L2_PIX_FMT_ARGB32);
	return 0;
}

static int asc_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apple_scaler_ctx *ctx = asc_file_to_ctx(file);
	u32 w, h;

	if (vb2_is_busy(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx)) ||
	    vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	asc_fill(asc_file_to_ctx(file)->asc, &f->fmt.pix_mp, NULL, V4L2_PIX_FMT_NV12M);
	w = ctx->src.width;
	h = ctx->src.height;
	ctx->src = f->fmt.pix_mp;
	asc_fit_rect(&ctx->crop, w, h, &ctx->src);
	/*
	 * Re-derive CAPTURE from the new source, with the matrix and range it was asked for: a
	 * CAPTURE format set before this (v4l2-ctl sets CAPTURE first) took them from the previous
	 * source, the open-time NV12 default.
	 */
	ctx->dst.ycbcr_enc = ctx->dst_ycbcr_req;
	ctx->dst.quantization = ctx->dst_quant_req;
	w = ctx->dst.width;
	h = ctx->dst.height;
	asc_fill(ctx->asc, &ctx->dst, &ctx->src, V4L2_PIX_FMT_ARGB32);
	asc_fit_rect(&ctx->compose, w, h, &ctx->dst);
	return 0;
}

static int asc_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apple_scaler_ctx *ctx = asc_file_to_ctx(file);
	u32 w, h;

	if (vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	ctx->dst_ycbcr_req = f->fmt.pix_mp.ycbcr_enc;
	ctx->dst_quant_req = f->fmt.pix_mp.quantization;
	asc_fill(ctx->asc, &f->fmt.pix_mp, &ctx->src, V4L2_PIX_FMT_ARGB32);
	w = ctx->dst.width;
	h = ctx->dst.height;
	ctx->dst = f->fmt.pix_mp;
	asc_fit_rect(&ctx->compose, w, h, &ctx->dst);
	return 0;
}

/*
 * MSR23: crop on the OUTPUT queue is the part of the source read, compose on
 * the CAPTURE queue the part of the destination written. The engine leaves
 * the rest of the CAPTURE buffer as it is, so COMPOSE_PADDED is the compose
 * rectangle. Rectangles are even and at least 64 pixels, like the formats.
 */
static int asc_g_selection(struct file *file, void *priv, struct v4l2_selection *s)
{
	struct apple_scaler_ctx *ctx = asc_file_to_ctx(file);
	bool out = s->type == V4L2_BUF_TYPE_VIDEO_OUTPUT;

	if (!out && s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
		if (!out)
			return -EINVAL;
		s->r = ctx->crop;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		if (!out)
			return -EINVAL;
		s->r = asc_full_rect(&ctx->src);
		return 0;
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_PADDED:
		if (out)
			return -EINVAL;
		s->r = ctx->compose;
		return 0;
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		if (out)
			return -EINVAL;
		s->r = asc_full_rect(&ctx->dst);
		return 0;
	}
	return -EINVAL;
}

static int asc_s_selection(struct file *file, void *priv, struct v4l2_selection *s)
{
	struct apple_scaler_ctx *ctx = asc_file_to_ctx(file);
	bool out = s->type == V4L2_BUF_TYPE_VIDEO_OUTPUT;
	const struct v4l2_pix_format_mplane *pix = out ? &ctx->src : &ctx->dst;
	struct v4l2_rect r;
	bool le;

	if (out ? s->target != V4L2_SEL_TGT_CROP :
		  (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE || s->target != V4L2_SEL_TGT_COMPOSE))
		return -EINVAL;
	if (vb2_is_streaming(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx)) ||
	    vb2_is_streaming(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	/* even: round outwards, or inwards when only V4L2_SEL_FLAG_LE is set */
	le = (s->flags & (V4L2_SEL_FLAG_LE | V4L2_SEL_FLAG_GE)) == V4L2_SEL_FLAG_LE;
	r.width = clamp_t(u32, le ? ALIGN_DOWN(s->r.width, 2) : ALIGN(s->r.width, 2),
			  MSR23_MIN_SIZE, pix->width);
	r.height = clamp_t(u32, le ? ALIGN_DOWN(s->r.height, 2) : ALIGN(s->r.height, 2),
			   MSR23_MIN_SIZE, pix->height);
	r.left = clamp_t(s32, le ? ALIGN(s->r.left, 2) : s->r.left & ~1, 0, pix->width - r.width);
	r.top = clamp_t(s32, le ? ALIGN(s->r.top, 2) : s->r.top & ~1, 0, pix->height - r.height);
	if (out)
		ctx->crop = r;
	else
		ctx->compose = r;
	s->r = r;
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
	.vidioc_g_selection = asc_g_selection,
	.vidioc_s_selection = asc_s_selection,

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

static void asc_return_bufs(struct vb2_queue *q, enum vb2_buffer_state state)
{
	struct apple_scaler_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vbuf;

	for (;;) {
		vbuf = V4L2_TYPE_IS_OUTPUT(q->type) ? v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
						      v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_m2m_buf_done(vbuf, state);
	}
}

static int msr23_job_of(const struct apple_scaler_ctx *ctx, struct msr23_job *job);

static void asc_grab_ctrls(struct apple_scaler_ctx *ctx, bool grab)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ctx->job_ctrl); i++)
		if (ctx->job_ctrl[i])
			v4l2_ctrl_grab(ctx->job_ctrl[i], grab);
}

static int asc_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct apple_scaler_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_queue *other = V4L2_TYPE_IS_OUTPUT(q->type) ?
				  v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx) :
				  v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx);
	struct msr23_job job;

	/*
	 * MSR23: both formats are fixed once both queues stream; refuse here a
	 * pair the job builder does not cover (size ratio, area, conversion)
	 * rather than fail every job.
	 */
	if (ctx->asc->variant->fw_fronted && vb2_is_streaming(other) &&
	    msr23_job_of(ctx, &job)) {
		dev_dbg(ctx->asc->dev, "MSR23: %.4s %ux%u -> %.4s %ux%u not supported\n",
			(char *)&ctx->src.pixelformat, ctx->src.width, ctx->src.height,
			(char *)&ctx->dst.pixelformat, ctx->dst.width, ctx->dst.height);
		asc_return_bufs(q, VB2_BUF_STATE_QUEUED);
		return -EINVAL;
	}
	/*
	 * Every control a job reads is fixed while either queue streams: a
	 * rotation changes which sizes fit, a mirror can make a checked pair
	 * invalid, and msr23_work() reads them without asc->lock.
	 */
	asc_grab_ctrls(ctx, true);
	if (V4L2_TYPE_IS_OUTPUT(q->type))
		ctx->sequence = 0;
	return 0;
}

static void asc_stop_streaming(struct vb2_queue *q)
{
	struct apple_scaler_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_queue *other = V4L2_TYPE_IS_OUTPUT(q->type) ?
				  v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx) :
				  v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx);

	asc_return_bufs(q, VB2_BUF_STATE_ERROR);
	if (!vb2_is_streaming(other))
		asc_grab_ctrls(ctx, false);
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

/* ---- controls --------------------------------------------------------- */

static int asc_s_ctrl(struct v4l2_ctrl *c)
{
	struct apple_scaler_ctx *ctx = container_of(c->handler, struct apple_scaler_ctx, hdl);

	switch (c->id) {
	case V4L2_CID_ROTATE:
		ctx->rotate = c->val;
		return 0;
	case V4L2_CID_HFLIP:
		ctx->hflip = c->val;
		return 0;
	case V4L2_CID_VFLIP:
		ctx->vflip = c->val;
		return 0;
	}
	return -EINVAL;
}

static const struct v4l2_ctrl_ops asc_ctrl_ops = {
	.s_ctrl = asc_s_ctrl,
};

/* MSR23: rotation in quarter turns and the mirrors (of the rotated picture). */
static int asc_ctrls_init(struct apple_scaler_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->hdl;

	v4l2_ctrl_handler_init(hdl, 3);
	ctx->rotate_ctrl = v4l2_ctrl_new_std(hdl, &asc_ctrl_ops, V4L2_CID_ROTATE, 0, 270, 90, 0);
	ctx->job_ctrl[0] = ctx->rotate_ctrl;
	ctx->job_ctrl[1] = v4l2_ctrl_new_std(hdl, &asc_ctrl_ops, V4L2_CID_HFLIP, 0, 1, 1, 0);
	ctx->job_ctrl[2] = v4l2_ctrl_new_std(hdl, &asc_ctrl_ops, V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (hdl->error) {
		int ret = hdl->error;

		v4l2_ctrl_handler_free(hdl);
		ctx->rotate_ctrl = NULL;
		memset(ctx->job_ctrl, 0, sizeof(ctx->job_ctrl));
		return ret;
	}
	ctx->fh.ctrl_handler = hdl;
	return 0;
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
	asc_fill(ctx->asc, &ctx->src, NULL, V4L2_PIX_FMT_NV12M);
	ctx->dst.width = 1920;
	ctx->dst.height = 1080;
	ctx->dst.pixelformat = V4L2_PIX_FMT_ARGB32;
	asc_fill(ctx->asc, &ctx->dst, &ctx->src, V4L2_PIX_FMT_ARGB32);
	ctx->crop = asc_full_rect(&ctx->src);
	ctx->compose = asc_full_rect(&ctx->dst);
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	if (asc->variant->fw_fronted) {
		ret = asc_ctrls_init(ctx);
		if (ret) {
			v4l2_fh_exit(&ctx->fh);
			kfree(ctx);
			return ret;
		}
	}

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(asc->m2m_dev, ctx, asc_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		v4l2_fh_exit(&ctx->fh);
		v4l2_ctrl_handler_free(&ctx->hdl);
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
	v4l2_ctrl_handler_free(&ctx->hdl);
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

	/* MSR23: the core loses its state; asc_resume() restarts it. */
	if (asc->m2m_dev)
		v4l2_m2m_suspend(asc->m2m_dev);
	return pm_runtime_force_suspend(dev);
}

static void msr23_restart(struct msr23 *m);

static int asc_resume(struct device *dev)
{
	struct apple_scaler *asc = dev_get_drvdata(dev);
	int ret = pm_runtime_force_resume(dev);

	if (ret)
		return ret;
	if (asc->variant->fw_fronted && asc->msr)
		msr23_restart(asc->msr);
	if (asc->m2m_dev)
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

/*
 * MSR23 (M5): firmware-fronted.
 *
 *  - The engine is fronted by a Cortex-M core (the "MSR ASE core", power
 *    domain msr-ase-core). Its code is __TEXT of the "msrf" image (0x11650
 *    bytes, linked at 0x100000) in SRAM at reg[5] + 0x0; its data RAM
 *    (0x10000000 in the core's view) is reg[5] + 0x80000; its mailbox and
 *    control registers (0x50000000) are reg[5] + 0x100000.
 *  - iBoot pre-loads __TEXT (ADT scaler0: pre-loaded = 1, segment-ranges).
 *  - Start-up: the engine's power-up register writes, then the core starts
 *    with reg[5]+0x100028 = 0xe then 0 and reports reg[0]+0x78 = 2, sends a
 *    hello through the mailbox, takes one init message and four shared
 *    pages, and writes "LIVE", later "STAR", to reg[5]+0x80038.
 *
 * The driver checks the image iBoot left in SRAM (S0), uploads the variant's
 * firmware file only when the SRAM holds none and the core has never run
 * (S1), then starts the core (S2) and sends the init pages (S3).
 */

#define MSR23_TEXT_SIZE		0x11650
#define MSR23_SRAM_TEXT_MAX	0x80000
#define MSR23_SP_INIT		0x1000fffc
#define MSR23_TEXT_BASE		0x100000
#define MSR23_DRAM		0x80000		/* reg[5]: core data RAM */
#define MSR23_DRAM_MAGIC	(MSR23_DRAM + 0x38)	/* "LIVE", "STAR" */
#define MSR23_LOG_WPTR		(MSR23_DRAM + 0x80)
#define MSR23_LOG		(MSR23_DRAM + 0x200)
#define MSR23_CPU_START		0x100028
#define MSR23_STATUS		0x100124
#define MSR23_A2I_CTRL		0x108110
#define MSR23_I2A_CTRL		0x108114
#define MSR23_A2I_SEND		0x108800
#define MSR23_I2A_RECV		0x108830
#define MSR23_MBOX_EMPTY	BIT(17)
#define MSR23_FW_STATE		0x78		/* reg[0] */
#define MSR23_MAGIC_LIVE	0x4556494c
#define MSR23_MAGIC_STAR	0x52415453
#define MSR23_SHMEM_SIZE	SZ_16K
#define MAGIC_FDBE		0xfdbefdbe

struct msr23_wr {
	u8 win;
	u32 off;
	u32 val;
	u16 settle_us;	/* wait this long after the write */
};

/*
 * T8142: the writes that prepare the engine before its core starts, in order.
 * The ADT names no fields; by aperture:
 *  - reg[0] (engine control): a reset pulse at +0x4 (the core needs ~1 ms
 *    after it, so the release write carries settle_us), then +0x48/+0x50
 *    and, at the end, +0x28..+0x54 and the +0x10288.. and +0x10688.. tables;
 *  - reg[3]: +0x0..+0x34, +0x100.., +0x300.., +0x404.. and +0x4008..;
 *  - reg[8] and reg[6]: one group each.
 */
static const struct msr23_wr msr23_t8142_powerup[] = {
	{0, 0x00004, 0x00000001}, {0, 0x00004, 0x00000000, 1000}, {0, 0x00050, 0x00000000},
	{0, 0x00048, 0x00000000}, {0, 0x00050, 0x00000003},
	{3, 0x00000, 0x8000001b}, {3, 0x00004, 0x800c000d}, {3, 0x00008, 0x800c000d},
	{3, 0x0000c, 0x800c000d}, {3, 0x00010, 0x80000003}, {3, 0x00014, 0x80000003},
	{3, 0x00018, 0x80000011}, {3, 0x0001c, 0x80000014}, {3, 0x00020, 0x8014000c},
	{3, 0x00024, 0x80000004}, {3, 0x00028, 0x80000004}, {3, 0x0002c, 0x80000015},
	{3, 0x00030, 0x9000000f}, {3, 0x00034, 0x9000000f}, {3, 0x00100, 0x000000ff},
	{3, 0x00104, 0x00010003}, {3, 0x00108, 0x000413b1}, {3, 0x00300, 0x1c000211},
	{3, 0x00304, 0x04040000}, {3, 0x00314, 0x80000002}, {3, 0x0031c, 0x80000009},
	{3, 0x00404, 0x00100071}, {3, 0x00748, 0x00200000}, {3, 0x00848, 0x02000120},
	{3, 0x00a00, 0x7fffffff}, {3, 0x00a04, 0x00000101}, {3, 0x04008, 0x00000000},
	{3, 0x0400c, 0x00000000}, {3, 0x04014, 0x00000000}, {3, 0x04018, 0x00000000},
	{3, 0x0401c, 0x00000001},
	{8, 0x00000, 0x00000000}, {8, 0x00010, 0x0001fbf2}, {8, 0x00014, 0x0001fbf2},
	{8, 0x00018, 0x0001fbf3}, {8, 0x0001c, 0x0001fbf0}, {8, 0x00020, 0x0001fbf0},
	{6, 0x00000, 0x00100011},
	{0, 0x00028, 0x0101005f}, {0, 0x0002c, 0x0000005e}, {0, 0x00030, 0x0000015c},
	{0, 0x0004c, 0x01ffffff}, {0, 0x00050, 0x00000003}, {0, 0x00054, 0x00000003},
	{0, 0x10288, 0x0000000c}, {0, 0x1028c, 0x00000006}, {0, 0x10290, 0x00000004},
	{0, 0x10294, 0x00000002}, {0, 0x10298, 0x00000001},
	{0, 0x10688, 0x00010012}, {0, 0x1068c, 0x0002001c}, {0, 0x10690, 0x00040030},
	{0, 0x10694, 0x00080042}, {0, 0x106a4, 0x4ec20040}, {0, 0x106b8, 0x4ec20040},
	{0, 0x106cc, 0x4ec20040}, {0, 0x106e0, 0x4ec20040}, {0, 0x106e4, 0x00200028},
	{0, 0x106f4, 0x4ec2004c}, {0, 0x106f8, 0x0040003c}, {0, 0x10708, 0x4ec20040},
	{0, 0x1070c, 0x0020000c}, {0, 0x1071c, 0x4ec20010}, {0, 0x10720, 0x0020000c},
	{0, 0x10730, 0x4ec20040}, {0, 0x10744, 0x4ec20040}, {0, 0x10758, 0x4ec2006c},
	{0, 0x1075c, 0x00400054}, {0, 0x1076c, 0x4ec200d8}, {0, 0x10770, 0x008000a4},
	{0, 0x10780, 0x4ec200b0}, {0, 0x10784, 0x00400088}, {0, 0x10794, 0x4ec20040},
	{0, 0x107a8, 0x4ec20040}, {0, 0x107bc, 0x4ec200ac}, {0, 0x107c0, 0x00400084},
	{0, 0x107d0, 0x4ec20040}, {0, 0x107d4, 0x00400024}, {0, 0x107e4, 0x4ec20040},
	{0, 0x107f8, 0x4ec20040}, {0, 0x107fc, 0x00400014}, {0, 0x1080c, 0x4ec20034},
	{0, 0x10810, 0x00400028}, {0, 0x10820, 0x4ec2001c}, {0, 0x10824, 0x00400014},
	{0, 0x10834, 0x4ec20040}, {0, 0x10848, 0x4ec20040},
};

/*
 * Jobs.
 * Everything the firmware reads is DMA memory addressed by its offset in the
 * DART window that starts at 1 << 40:
 *  - a ring of block-list records in shared page 0x02 (0x400 words); the AP
 *    writes a record, then reg[4]+0x10038 = its write pointer in words
 *    (bit 31 flips on wrap). A record: 0xfdbefdbe, seq << 8 | 2, a job word,
 *    pair count << 16, then (block word, size word) pairs. A block word is
 *    (offset >> 7) << 8 | block id; a size word holds the size in 32-byte
 *    units from bit 20;
 *  - the blocks: a register image (80 KiB) and a spill buffer. The job word
 *    0x72000000 has the firmware tile the job; with bit 31, the AP builds the
 *    tile array (block 15) instead, which this driver does not;
 *  - completion: the status ring at reg[5]+0x83a00 (0x200 words, 20-word
 *    entries), its write pointer reg[4]+0x30008, the AP acknowledges with
 *    reg[4]+0x10010.
 * The driver builds the register image from the job's parameters
 * (apple-scaler-msr23.c).
 */
#define MSR23_WIN		BIT_ULL(40)
#define MSR23_WIN_SPAN		SZ_2G		/* block words hold offset >> 7 in 24 bits */
#define MSR23_RING_WORDS	0x400
#define MSR23_STS_BASE		0x83a00		/* reg[5] */
#define MSR23_STS_WORDS		0x200
#define MSR23_STS_ENTRY		0x14
#define MSR23_STS_OK0		0x00140007	/* status words 0 and 2 of a job that completed */
#define MSR23_STS_OK2		0x00000200
#define MSR23_Q_SUBMIT		0x10038		/* reg[4], 64-bit */
#define MSR23_Q_STAMP		0x100f8
#define MSR23_Q_STS_READ	0x10010
#define MSR23_Q_STS_WRITE	0x30008
#define MSR23_Q_SUB_DONE	0x30040
#define MSR23_JOB_TIMEOUT_US	1000000

struct msr23 {
	struct device *dev;
	struct apple_scaler *asc;
	void __iomem *w[10];
	void __iomem *dart;	/* read only: the window registers and the error word */
	resource_size_t w5_phys;
	/* shared pages, in the order of S3's messages (ids 0x10, 0x01, 0x02, 0x23) */
	void *page[4];
	dma_addr_t page_iova[4];
	bool window;		/* IOVAs are inside the 1 << 40 window */
	bool started;		/* S3 saw LIVE/STAR */
	u64 ring_wptr, sts_rptr;
	u32 seq;
	struct work_struct work;
	struct apple_scaler_ctx *ctx;
};

/*
 * The DART window the firmware addresses through: dart+0x300 = 1, +0x308 =
 * 0x10000000 (base 1 << 40 in 4K pages), +0x310 = 0x3fffffff.
 */
static void msr23_dart(struct msr23 *m, const char *when)
{
	if (!m->dart)
		return;
	dev_dbg(m->dev,
		"MSR23 %s: dart +0x300 %#x +0x308 %#x +0x310 %#x error +0x100 %#x tcr0/1 %#x %#x ttbr0/1 %#x %#x\n",
		when, readl(m->dart + 0x300), readl(m->dart + 0x308), readl(m->dart + 0x310),
		readl(m->dart + 0x100), readl(m->dart + 0x1000), readl(m->dart + 0x1004),
		readl(m->dart + 0x1400), readl(m->dart + 0x1404));
}

static bool msr23_text_valid(const u32 *vec)
{
	return vec[0] == MSR23_SP_INIT && vec[1] >= MSR23_TEXT_BASE &&
	       vec[1] < MSR23_TEXT_BASE + MSR23_SRAM_TEXT_MAX;
}

/* S0: what the SRAM holds. Returns 1 (valid, matches the file or no file), 0 (absent/different). */
static int msr23_s0(struct msr23 *m, const struct firmware *fw)
{
	void __iomem *w0 = m->w[0], *w5 = m->w[5];
	size_t len;
	u32 vec[2], crc_sram, crc_file = 0;
	bool valid, same = false;
	void *buf;

	/* a file shorter than the vector or larger than the SRAM is no image: compare nothing */
	if (fw && (fw->size < sizeof(vec) || fw->size > MSR23_SRAM_TEXT_MAX)) {
		dev_warn(m->dev, "MSR23 S0: %s has %zu bytes: not an MSR __TEXT image, ignored\n",
			 m->asc->variant->fw_name, fw->size);
		fw = NULL;
	}
	len = fw ? fw->size : MSR23_TEXT_SIZE;
	buf = kvmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memcpy_fromio(buf, w5, len);
	memcpy(vec, buf, sizeof(vec));
	valid = msr23_text_valid(vec);
	crc_sram = crc32_le(0, buf, len);
	if (fw) {
		crc_file = crc32_le(0, fw->data, fw->size);
		same = !memcmp(buf, fw->data, fw->size);
	}
	kvfree(buf);

	dev_dbg(m->dev,
		"MSR23 S0: SRAM vector SP %#x reset %#x (%s), crc32 %#x over %#zx; file %s crc32 %#x: %s\n",
		vec[0], vec[1], valid ? "an MSR image" : "no image", crc_sram, len,
		fw ? m->asc->variant->fw_name : "(none)", crc_file,
		!fw ? "no file to compare" :
		same ? "SRAM == file (pre-loaded by iBoot)" :
		valid ? "SRAM holds a different image" : "SRAM empty");
	dev_dbg(m->dev,
		"MSR23 S0: state +0x78 %#x status %#x magic %#x log wptr %#x mbox a2i %#x i2a %#x/%#x %#x/%#x (before the start: 0, -, -, -, 0x20001 x4)\n",
		readl(w0 + MSR23_FW_STATE), readl(w5 + MSR23_STATUS), readl(w5 + MSR23_DRAM_MAGIC),
		readl(w5 + MSR23_LOG_WPTR), readl(w5 + MSR23_A2I_CTRL), readl(w5 + MSR23_I2A_CTRL),
		readl(w5 + 0x124110), readl(w5 + 0x124114), readl(w5 + 0x100058));
	return valid && (same || !fw);
}

/* S1: write __TEXT into the SRAM and read it back. */
static int msr23_s1(struct msr23 *m, const struct firmware *fw)
{
	void *buf;
	int ret = 0;

	if (fw->size < 2 * sizeof(u32) || fw->size > MSR23_SRAM_TEXT_MAX ||
	    !msr23_text_valid((const u32 *)fw->data)) {
		dev_err(m->dev, "MSR23 S1: %s is not an MSR __TEXT image (%zu bytes)\n",
			m->asc->variant->fw_name, fw->size);
		return -EINVAL;
	}
	memcpy_toio(m->w[5], fw->data, fw->size);
	/* the ring record before the write pointer that hands it over */
	wmb();
	buf = kvmalloc(fw->size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memcpy_fromio(buf, m->w[5], fw->size);
	if (memcmp(buf, fw->data, fw->size)) {
		size_t i;

		for (i = 0; i < fw->size && ((u8 *)buf)[i] == fw->data[i]; i++)
			;
		dev_err(m->dev,
			"MSR23 S1: upload FAILED: readback differs from %#zx (the SRAM is not AP-writable this way)\n",
			i);
		ret = -EIO;
	} else {
		dev_dbg(m->dev, "MSR23 S1: uploaded %#zx bytes of %s to SRAM, readback ok\n",
			fw->size, m->asc->variant->fw_name);
	}
	kvfree(buf);
	return ret;
}

static void msr23_log_tail(struct msr23 *m, const char *when)
{
	void __iomem *w5 = m->w[5];
	u32 e[4];
	int i;

	dev_dbg(m->dev,
		"MSR23 %s: magic %#x status %#x log wptr %#x/%#x dram+0x18 %#x %#x %#x %#x +0x30 %#x %#x\n",
		when, readl(w5 + MSR23_DRAM_MAGIC), readl(w5 + MSR23_STATUS),
		readl(w5 + MSR23_LOG_WPTR), readl(w5 + MSR23_LOG_WPTR + 4),
		readl(w5 + MSR23_DRAM + 0x18), readl(w5 + MSR23_DRAM + 0x1c),
		readl(w5 + MSR23_DRAM + 0x20), readl(w5 + MSR23_DRAM + 0x24),
		readl(w5 + MSR23_DRAM + 0x30), readl(w5 + MSR23_DRAM + 0x34));
	for (i = 0; i < 8; i++) {
		memcpy_fromio(e, w5 + MSR23_LOG + 16 * i, sizeof(e));
		if (!e[0] && !e[1])
			break;
		dev_dbg(m->dev, "MSR23 %s: log[%d] %08x %08x %08x %08x\n", when, i, e[0], e[1],
			e[2], e[3]);
	}
}

static int msr23_recv(struct msr23 *m, u32 msg[4], unsigned int timeout_us)
{
	u32 ctl;
	int ret;

	ret = readl_poll_timeout(m->w[5] + MSR23_I2A_CTRL, ctl, !(ctl & MSR23_MBOX_EMPTY), 100,
				 timeout_us);
	if (ret)
		return ret;
	msg[0] = readl(m->w[5] + MSR23_I2A_RECV + 0x0);
	msg[1] = readl(m->w[5] + MSR23_I2A_RECV + 0x4);
	msg[2] = readl(m->w[5] + MSR23_I2A_RECV + 0x8);
	msg[3] = readl(m->w[5] + MSR23_I2A_RECV + 0xc);	/* the last read pops */
	return 0;
}

static int msr23_send(struct msr23 *m, u64 msg0, u64 msg1)
{
	u32 ctl = readl(m->w[5] + MSR23_A2I_CTRL);

	writel(lower_32_bits(msg0), m->w[5] + MSR23_A2I_SEND + 0x0);
	writel(upper_32_bits(msg0), m->w[5] + MSR23_A2I_SEND + 0x4);
	writel(lower_32_bits(msg1), m->w[5] + MSR23_A2I_SEND + 0x8);
	writel(upper_32_bits(msg1), m->w[5] + MSR23_A2I_SEND + 0xc);
	dev_dbg(m->dev, "MSR23 S3: sent %#018llx %#llx (a2i before %#x after %#x)\n", msg0, msg1,
		ctl, readl(m->w[5] + MSR23_A2I_CTRL));
	return 0;
}

/* S2: power-up table, then the start handshake. */
static int msr23_s2(struct msr23 *m)
{
	static const u32 mbox[] = { MSR23_A2I_CTRL, MSR23_I2A_CTRL, 0x124110, 0x124114 };
	void __iomem *w0 = m->w[0], *w5 = m->w[5];
	const struct apple_scaler_variant *var = m->asc->variant;
	u32 v, a, b, msg[4];
	unsigned int i;
	int ret;

	for (i = 0; i < var->num_powerup; i++) {
		const struct msr23_wr *wr = &var->powerup[i];

		writel(wr->val, m->w[wr->win] + wr->off);
		if (wr->settle_us)
			usleep_range(wr->settle_us, 2 * wr->settle_us);
	}

	for (i = 0; i < ARRAY_SIZE(mbox); i++) {
		v = readl(w5 + mbox[i]);
		writel(v & ~1U, w5 + mbox[i]);
	}
	v = readl(w0 + MSR23_FW_STATE);
	writel(0xe, w5 + MSR23_CPU_START);
	a = readl(w5 + 0x100058);
	b = readl(w5 + 0x10005c);
	writel(0, w0 + MSR23_FW_STATE);
	writel(0, w5 + MSR23_CPU_START);
	ret = readl_poll_timeout(w0 + MSR23_FW_STATE, v, v == 2, 100, 200000);
	dev_dbg(m->dev,
		"MSR23 S2: start: +0x58/+0x5c %#x %#x (want 1 1), state +0x78 -> %#x: %s\n", a, b,
		v, ret ? "NO ANSWER in 200 ms" : "core running (2)");
	if (ret) {
		msr23_log_tail(m, "S2");
		return ret;
	}
	ret = msr23_recv(m, msg, 100000);
	if (ret)
		dev_warn(m->dev, "MSR23 S2: no hello in the mailbox (i2a %#x)\n",
			 readl(w5 + MSR23_I2A_CTRL));
	else
		dev_dbg(m->dev,
			"MSR23 S2: hello %08x %08x %08x %08x (want 00000100 0 0 00100100)\n",
			msg[0], msg[1], msg[2], msg[3]);
	msr23_log_tail(m, "S2");
	return 0;
}

/* S3: the init message and the four shared pages. */
/* The firmware's view of an IOVA: its offset in the 1 << 40 window. */
static u32 msr23_off(dma_addr_t iova)
{
	return lower_32_bits(iova - MSR23_WIN);
}

static bool msr23_in_window(dma_addr_t iova, size_t len)
{
	return iova >= MSR23_WIN && iova + len <= MSR23_WIN + MSR23_WIN_SPAN;
}

/* every plane of a buffer, over its whole mapped length */
static bool msr23_vb_in_window(struct vb2_buffer *vb)
{
	unsigned int p;

	for (p = 0; p < vb->num_planes; p++)
		if (!msr23_in_window(vb2_dma_contig_plane_dma_addr(vb, p), vb2_plane_size(vb, p)))
			return false;
	return true;
}

static int msr23_s3(struct msr23 *m)
{
	static const u8 page_id[] = { 0x10, 0x01, 0x02, 0x23 };
	u64 dram = m->w5_phys + MSR23_DRAM;
	u32 msg[4], v;
	unsigned int i;
	int ret;

	m->window = true;
	for (i = 0; i < ARRAY_SIZE(page_id); i++) {
		if (!m->page[i])	/* a restart after sleep reuses them */
			m->page[i] = dmam_alloc_coherent(m->dev, MSR23_SHMEM_SIZE, &m->page_iova[i],
							 GFP_KERNEL);
		if (!m->page[i])
			return -ENOMEM;
		memset(m->page[i], 0, MSR23_SHMEM_SIZE);
		if (!msr23_in_window(m->page_iova[i], MSR23_SHMEM_SIZE))
			m->window = false;
	}
	/*
	 * Boot 35740333 sent raw IOVAs below 4 GiB (0xffffc000...): the firmware
	 * took them (STAR) but never touched the pages. The job path needs the
	 * window (1 << 40 + 0x0/0x8000/0x10000/0x18000 work).
	 */
	if (!m->window)
		dev_warn(m->dev, "MSR23 S3: pages outside the 1 << 40 window (%pad): no job path\n",
			 &m->page_iova[0]);
	msr23_dart(m, "S3 before");
	/* init message 0x00080200 on endpoint 3; the high word is the core RAM's address. */
	msr23_send(m, ((u64)lower_32_bits(dram) << 32) | 0x00080200, 3);
	for (i = 0; i < ARRAY_SIZE(page_id); i++) {
		u32 off = m->window ? msr23_off(m->page_iova[i]) : lower_32_bits(m->page_iova[i]);

		msr23_send(m, ((u64)off << 32) | 0x40000500 | (page_id[i] << 16), 0x100);
	}
	for (i = 0; i < 4; i++) {
		if (msr23_recv(m, msg, 100000))
			break;
		dev_dbg(m->dev, "MSR23 S3: reply %08x %08x %08x %08x\n", msg[0], msg[1], msg[2],
			msg[3]);
	}
	ret = readl_poll_timeout(m->w[5] + MSR23_DRAM_MAGIC, v,
				 v == MSR23_MAGIC_LIVE || v == MSR23_MAGIC_STAR, 1000, 500000);
	dev_dbg(m->dev, "MSR23 S3: pages %pad %pad %pad %pad (window %s); magic %#x: %s\n",
		&m->page_iova[0], &m->page_iova[1], &m->page_iova[2], &m->page_iova[3],
		m->window ? "yes" : "NO", v,
		ret ? "no LIVE/STAR in 500 ms" : v == MSR23_MAGIC_STAR ? "STAR (started)" : "LIVE");
	msr23_log_tail(m, "S3");
	msr23_dart(m, "S3 after");
	m->started = !ret;
	return ret;
}

/* ---- MSR23 jobs ----------------------------------------------------------- */

static void msr23_cancel_work(void *data)
{
	struct msr23 *m = data;

	cancel_work_sync(&m->work);
}

/*
 * Chroma subsampling of a MSR23 format (not v4l2_format_info(): it gives P010
 * vdiv 1). 0: no chroma plane (R'G'B' or grey).
 */
static void msr23_sub(u32 v4l2, u32 *hs, u32 *vs)
{
	switch (v4l2) {
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV12M:
	case V4L2_PIX_FMT_P010:
		*hs = 2;
		*vs = 2;
		return;
	case V4L2_PIX_FMT_NV16:
		*hs = 2;
		*vs = 1;
		return;
	case V4L2_PIX_FMT_NV24:
		*hs = 1;
		*vs = 1;
		return;
	}
	*hs = 0;
	*vs = 0;
}

static void msr23_pic(struct msr23_pic *p, const struct v4l2_pix_format_mplane *f)
{
	u32 hs, vs;

	msr23_sub(f->pixelformat, &hs, &vs);
	p->fourcc = f->pixelformat;
	p->width = f->width;
	p->height = f->height;
	p->stride[0] = f->plane_fmt[0].bytesperline;
	if (!hs)
		p->stride[1] = 0;
	else if (f->num_planes > 1)
		p->stride[1] = f->plane_fmt[1].bytesperline;
	else
		p->stride[1] = f->plane_fmt[0].bytesperline * 2 / hs;
}

static enum v4l2_ycbcr_encoding msr23_enc(const struct v4l2_pix_format_mplane *f)
{
	if (f->ycbcr_enc == V4L2_YCBCR_ENC_601 || f->ycbcr_enc == V4L2_YCBCR_ENC_BT2020)
		return f->ycbcr_enc;
	return V4L2_YCBCR_ENC_709;
}

static bool msr23_full(const struct v4l2_pix_format_mplane *f)
{
	return f->quantization == V4L2_QUANTIZATION_FULL_RANGE;
}

/*
 * The job for a context's formats. The Y'CbCr side carries the matrix and the
 * range. Y'CbCr to Y'CbCr only scales: a matrix or range change is a
 * conversion the engine would do through R'G'B', which the builder does not
 * cover.
 */
static int msr23_job_of(const struct apple_scaler_ctx *ctx, struct msr23_job *job)
{
	const struct v4l2_pix_format_mplane *s = &ctx->src, *d = &ctx->dst;
	bool ys = v4l2_is_format_yuv(v4l2_format_info(s->pixelformat));
	bool yd = v4l2_is_format_yuv(v4l2_format_info(d->pixelformat));
	const struct v4l2_pix_format_mplane *y = ys ? s : d;

	if (ys && yd && (msr23_enc(s) != msr23_enc(d) || msr23_full(s) != msr23_full(d)))
		return -EINVAL;
	memset(job, 0, sizeof(*job));
	msr23_pic(&job->src, s);
	msr23_pic(&job->dst, d);
	job->enc = msr23_enc(y);
	job->full = msr23_full(y);
	job->crop = ctx->crop;
	job->compose = ctx->compose;
	job->rotate = ctx->rotate;
	job->hflip = ctx->hflip;
	job->vflip = ctx->vflip;
	return msr23_check(job);
}

static dma_addr_t msr23_plane(const struct v4l2_pix_format_mplane *pix, struct vb2_buffer *vb,
			      unsigned int p)
{
	if (pix->num_planes > 1)
		return vb2_dma_contig_plane_dma_addr(vb, p);
	return vb2_dma_contig_plane_dma_addr(vb, 0) +
	       (dma_addr_t)p * pix->plane_fmt[0].bytesperline * pix->height;
}

static void msr23_ptr(u32 *img, u32 at, u64 a)
{
	img[at / 4] = lower_32_bits(a);
	img[at / 4 + 1] = upper_32_bits(a);
}

static u64 msr23_advance(u64 ptr, u32 n, u32 size)
{
	u64 idx = (ptr & 0x7fffffff) + n;

	if (idx >= size) {
		idx -= size;
		ptr ^= BIT_ULL(31);
	}
	return (ptr & BIT_ULL(31)) | idx;
}

/* One job, synchronous: build it at our IOVAs, submit, wait for its status entry. */
static int msr23_run(struct msr23 *m, struct apple_scaler_ctx *ctx, struct vb2_buffer *src,
		     struct vb2_buffer *dst)
{
	void __iomem *w4 = m->w[4], *w5 = m->w[5];
	u32 *ring = m->page[2], *img, st[MSR23_STS_ENTRY], rec[4 + 2 * MSR23_NBLOCKS];
	char id[40];
	struct msr23_job job;
	struct msr23_built out;
	dma_addr_t img_iova, spill_iova;
	void *spill = NULL;
	size_t spill_len = 0;
	u32 k, seq, err0, idx, nwords;
	u64 wptr, sts;
	int ret;

	snprintf(id, sizeof(id), "%.4s %ux%u -> %.4s %ux%u", (char *)&ctx->src.pixelformat,
		 ctx->src.width, ctx->src.height, (char *)&ctx->dst.pixelformat, ctx->dst.width,
		 ctx->dst.height);
	ret = msr23_job_of(ctx, &job);
	if (ret) {
		dev_warn_ratelimited(m->dev, "MSR23 job %s: not supported\n", id);
		return ret;
	}
	/* zeroed: the builder writes only the words a job sets */
	img = dma_alloc_coherent(m->dev, MSR23_IMG_LEN, &img_iova, GFP_KERNEL);
	if (!img)
		return -ENOMEM;
	ret = msr23_build(&job, img, &out);
	if (ret)
		goto out;
	spill_len = (size_t)(out.nc0 + out.nc1) * MSR23_NC_PAGE;
	spill = dma_alloc_coherent(m->dev, spill_len, &spill_iova, GFP_KERNEL);
	ret = -ENOMEM;
	if (!spill)
		goto out;
	ret = -ERANGE;
	if (!msr23_in_window(img_iova, MSR23_IMG_LEN) ||
	    !msr23_in_window(spill_iova, spill_len) ||
	    !msr23_vb_in_window(src) || !msr23_vb_in_window(dst)) {
		dev_err_ratelimited(m->dev, "MSR23 job: a buffer is outside the 1 << 40 window\n");
		goto out;
	}

	/* NC0 at the start of the spill buffer, NC1 after it */
	msr23_ptr(img, MSR23_PTR_NC0, spill_iova);
	msr23_ptr(img, MSR23_PTR_NC1, spill_iova + (dma_addr_t)out.nc0 * MSR23_NC_PAGE);
	for (k = 0; k < out.planes[0]; k++)
		msr23_ptr(img, MSR23_PTR_SRC(k), msr23_plane(&ctx->src, src, k));
	for (k = 0; k < out.planes[1]; k++)
		msr23_ptr(img, MSR23_PTR_DST(k), msr23_plane(&ctx->dst, dst, k) + out.dst_off[k]);
	/*
	 * Block 0 +0x60 is the frame id too: it must equal the record's word 1
	 * (the tiler refuses a job where they differ).
	 */
	seq = ++m->seq & 0xffffff;
	img[MSR23_FRAME_ID / 4] = seq << 8 | 2;

	/* The record: header, then the block list. */
	rec[0] = MAGIC_FDBE;
	rec[1] = seq << 8 | 2;
	rec[2] = MSR23_HDR2_FW_TILING;
	rec[3] = (2 * MSR23_NBLOCKS) << 16;
	for (k = 0; k < MSR23_NBLOCKS; k++) {
		u32 off = msr23_off(img_iova + msr23_blocks[k].off);

		rec[4 + 2 * k] = (off >> 7) << 8 | msr23_blocks[k].id;
		rec[5 + 2 * k] = msr23_blocks[k].size;
	}
	nwords = ARRAY_SIZE(rec);
	wptr = m->ring_wptr;
	idx = wptr & 0x7fffffff;
	for (k = 0; k < nwords; k++)
		ring[(idx + k) % MSR23_RING_WORDS] = rec[k];

	err0 = m->dart ? readl(m->dart + 0x100) : 0;
	dma_wmb();
	m->ring_wptr = msr23_advance(wptr, nwords, MSR23_RING_WORDS);
	writeq(ktime_get_ns() & 0xffffffff, w4 + MSR23_Q_STAMP);
	writeq(m->ring_wptr, w4 + MSR23_Q_SUBMIT);
	/* around each submission: interrupt acknowledge/unmask */
	writel(0x80000000, w5 + 0x100170);
	writel(1, w5 + 0x100178);
	writel(0x80000000, w5 + 0x100178);

	ret = readq_poll_timeout(w4 + MSR23_Q_STS_WRITE, sts, sts != m->sts_rptr, 200,
				 MSR23_JOB_TIMEOUT_US);
	if (ret) {
		dev_err(m->dev,
			"MSR23 job %s seq %u: no status in 1 s (submit %#llx, firmware took %#llx, status %#llx/%#llx, dart error %#x, magic %#x)\n",
			id, seq, m->ring_wptr, readq(w4 + MSR23_Q_SUB_DONE),
			readq(w4 + MSR23_Q_STS_WRITE), m->sts_rptr,
			m->dart ? readl(m->dart + 0x100) : 0, readl(w5 + MSR23_DRAM_MAGIC));
		msr23_log_tail(m, "job");
		/*
		 * The firmware may still be working on this job and DMA into its buffers later:
		 * keep them (leak them) and take the core out of service, so no further job reaches
		 * it until a resume restarts it.
		 */
		m->started = false;
		img = NULL;
		spill = NULL;
		dev_err(m->dev,
			"MSR23: job timed out; core out of service, job buffers kept (leaked) until reboot\n");
		goto out;
	}
	for (k = 0; k < MSR23_STS_ENTRY; k++)
		st[k] = readl(w5 + MSR23_STS_BASE +
			      4 * (((m->sts_rptr & 0x7fffffff) + k) % MSR23_STS_WORDS));
	m->sts_rptr = msr23_advance(m->sts_rptr, MSR23_STS_ENTRY, MSR23_STS_WORDS);
	writeq(m->sts_rptr, w4 + MSR23_Q_STS_READ);
	if (m->dart && readl(m->dart + 0x100) != err0) {
		dev_err(m->dev, "MSR23 job %s: DART error %#x\n", id, readl(m->dart + 0x100));
		ret = -EIO;
	} else if ((st[1] & 0xffffff00) != (seq << 8)) {
		/*
		 * The firmware is out of step: after a job it aborted, every later
		 * status entry was the previous one or zeros and the outputs were
		 * untouched. It may still own this job's buffers: keep them, as on a
		 * timeout, and take the core out of service rather than return
		 * empty frames as done.
		 */
		dev_err(m->dev,
			"MSR23 job %s: status entry %08x %08x %08x %08x is not for seq %u; core out of service\n",
			id, st[0], st[1], st[2], st[3], seq);
		msr23_log_tail(m, "job");
		m->started = false;
		img = NULL;
		spill = NULL;
		ret = -EIO;
	} else if (st[0] != MSR23_STS_OK0 || st[2] != MSR23_STS_OK2) {
		/*
		 * Every completed job ends 00140007 <seq> 00000200. A job the
		 * engine aborted ended 0015000f <seq> 00001201, and the firmware
		 * wrote no status entry after it.
		 */
		dev_err(m->dev,
			"MSR23 job %s seq %u: error status %08x %08x %08x %08x; core out of service\n",
			id, seq, st[0], st[1], st[2], st[3]);
		msr23_log_tail(m, "job");
		m->started = false;
		ret = -EIO;
	} else {
		dev_dbg(m->dev, "MSR23 job %s seq %u done (NC %u + %u pages)\n", id, seq,
			out.nc0, out.nc1);
	}
out:
	if (spill)
		dma_free_coherent(m->dev, spill_len, spill, spill_iova);
	if (img)
		dma_free_coherent(m->dev, MSR23_IMG_LEN, img, img_iova);
	return ret;
}

static void msr23_work(struct work_struct *work)
{
	struct msr23 *m = container_of(work, struct msr23, work);
	struct apple_scaler_ctx *ctx = m->ctx;
	struct vb2_v4l2_buffer *src, *dst;
	int ret;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!m->started)
		ret = -EIO;	/* the core did not come back (resume): fail, never touch it */
	else
		ret = src && dst ? msr23_run(m, ctx, &src->vb2_buf, &dst->vb2_buf) : -EINVAL;
	m->ctx = NULL;
	asc_finish(m->asc, ctx, ret);
}

/* After system sleep: start the core again from what the SRAM holds (never an upload here). */
static void msr23_restart(struct msr23 *m)
{
	u32 vec[2];

	memcpy_fromio(vec, m->w[5], sizeof(vec));
	if (!msr23_text_valid(vec)) {
		dev_warn(m->dev,
			 "MSR23: resumed with no image in the SRAM (SP %#x): the core stays down\n",
			 vec[0]);
		m->started = false;
		return;
	}
	if (msr23_s2(m) || msr23_s3(m)) {
		dev_warn(m->dev, "MSR23: restart after resume failed\n");
		m->started = false;
		return;
	}
	m->ring_wptr = readq(m->w[4] + MSR23_Q_SUBMIT);
	m->sts_rptr = readq(m->w[4] + MSR23_Q_STS_READ);
	m->seq = 0;
	dev_dbg(m->dev, "MSR23: restarted after resume (submit %#llx, status %#llx)\n",
		m->ring_wptr, m->sts_rptr);
}

static void msr23_device_run(struct apple_scaler *asc, struct apple_scaler_ctx *ctx)
{
	/* asc_finish() drops one runtime-PM reference per job. */
	pm_runtime_get_noresume(asc->dev);
	asc->msr->ctx = ctx;
	queue_work(system_dfl_wq, &asc->msr->work);
}

static void msr23_put(void *data)
{
	pm_runtime_put_sync_suspend(data);
}

static int asc_register_video(struct apple_scaler *asc);

static int asc_v23_probe(struct apple_scaler *asc, struct platform_device *pdev)
{
	struct device *dev = asc->dev;
	const struct firmware *fw = NULL;
	struct msr23 *m;
	unsigned int i;
	int ret, have;

	m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->dev = dev;
	m->asc = asc;
	asc->msr = m;
	INIT_WORK(&m->work, msr23_work);
	ret = devm_add_action_or_reset(dev, msr23_cancel_work, m);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(m->w); i++) {
		struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, i);

		if (!res)
			return dev_err_probe(dev, -EINVAL, "MSR23: no reg[%u]\n", i);
		/* reg[1] and reg[8] are the same window: map without claiming. */
		m->w[i] = devm_ioremap(dev, res->start, resource_size(res));
		if (!m->w[i])
			return -ENOMEM;
		if (i == 5)
			m->w5_phys = res->start;
	}
	{
		struct device_node *dn = of_parse_phandle(dev->of_node, "iommus", 0);
		struct resource dr;

		if (dn && !of_address_to_resource(dn, 0, &dr))
			m->dart = devm_ioremap(dev, dr.start, 0x2000);
		of_node_put(dn);
	}
	/*
	 * Every address a job holds is an offset in the DART window at 1 << 40
	 * (dart +0x308 = 0x10000000), and block words keep offset >> 7 in 24
	 * bits: top-down IOVA allocation below this limit lands in
	 * [1 << 40, 1 << 40 + 2 GiB).
	 */
	ret = dma_set_mask_and_coherent(dev, MSR23_WIN + MSR23_WIN_SPAN - 1);
	if (ret)
		return ret;
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "MSR23: failed to power up\n");

	if (firmware_request_nowarn(&fw, m->asc->variant->fw_name, dev))
		fw = NULL;
	dev_dbg(dev, "MSR23: version %#x, %s %s\n", readl(m->w[0]), m->asc->variant->fw_name,
		fw ? "loaded" : "not found");

	msr23_dart(m, "S0");
	have = msr23_s0(m, fw);
	if (have < 0) {
		ret = have;
		goto out_put;
	}
	if (!have) {
		u32 state = readl(m->w[0] + MSR23_FW_STATE);
		u32 magic = readl(m->w[5] + MSR23_DRAM_MAGIC);

		/*
		 * Never write the SRAM under a core that has run since power-up:
		 * an upload after a module reload (state 2, magic "ABRT") hung
		 * the machine. Only a cold core (state 0, no magic) can take an
		 * image.
		 */
		if (state || magic) {
			dev_warn(dev,
				 "MSR23: the core has run (state %#x magic %#x), not uploading; starting what the SRAM holds\n",
				 state, magic);
			goto start;
		}
		if (!fw) {
			dev_err(dev, "MSR23: the SRAM holds no image and %s is missing\n",
				m->asc->variant->fw_name);
			ret = -ENOENT;
			goto out_put;
		}
		ret = msr23_s1(m, fw);
		if (ret)
			goto out_put;
	}

start:
	ret = msr23_s2(m);
	if (ret)
		goto out_put;
	ret = msr23_s3(m);
	if (ret)
		dev_warn(dev, "MSR23: init incomplete (%d)\n", ret);
	release_firmware(fw);
	/* Keep the runtime-PM reference while bound (SRAM and core stay up); drop it on unbind. */
	ret = devm_add_action_or_reset(dev, msr23_put, dev);
	if (ret)
		return ret;
	if (!m->started || !m->window) {
		dev_warn(dev,
			 "MSR23: firmware core up but not ready (started %d, window %d): no job path\n",
			 m->started, m->window);
		return 0;
	}
	m->ring_wptr = readq(m->w[4] + MSR23_Q_SUBMIT);
	m->sts_rptr = readq(m->w[4] + MSR23_Q_STS_READ);
	dev_dbg(dev,
		"MSR23 queues: submit %#llx (firmware took %#llx), status read %#llx (firmware wrote %#llx)\n",
		m->ring_wptr, readq(m->w[4] + MSR23_Q_SUB_DONE), m->sts_rptr,
		readq(m->w[4] + MSR23_Q_STS_WRITE));
	ret = asc_register_video(asc);
	if (ret)
		return ret;
	dev_info(dev, "MSR23: job path up: %s\n", video_device_node_name(&asc->vdev));
	return 0;

out_put:
	release_firmware(fw);
	pm_runtime_put_sync_suspend(dev);
	return ret;
}

static void asc_cleanup(void *data)
{
	struct apple_scaler *asc = data;

	cancel_delayed_work_sync(&asc->watchdog);
	/*
	 * devres unwinds in reverse: this runs before msr23_put() powers the
	 * core down and before msr23_cancel_work(). A job still queued or
	 * running would touch the m2m device and the registers after them.
	 */
	if (asc->msr)
		cancel_work_sync(&asc->msr->work);
	if (asc->m2m_dev)
		v4l2_m2m_release(asc->m2m_dev);
	v4l2_device_unregister(&asc->v4l2_dev);
}

static int asc_register_video(struct apple_scaler *asc)
{
	struct device *dev = asc->dev;
	struct video_device *vdev;
	int ret;

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
	/* crop and compose: MSR23 only (the MMIO engines convert at the same size) */
	if (!asc->variant->fw_fronted) {
		v4l2_disable_ioctl(vdev, VIDIOC_G_SELECTION);
		v4l2_disable_ioctl(vdev, VIDIOC_S_SELECTION);
	}
	video_set_drvdata(vdev, asc);
	return video_register_device(vdev, VFL_TYPE_VIDEO, APPLE_M2M_VIDEO_NR);
}

static int asc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_scaler *asc;
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
	if (asc->variant->fw_fronted)
		return asc_v23_probe(asc, pdev);
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

	return asc_register_video(asc);
}

static void asc_remove(struct platform_device *pdev)
{
	struct apple_scaler *asc = platform_get_drvdata(pdev);

	video_unregister_device(&asc->vdev);
}

/* M1: one engine with no PIODMA-side register range. */
static const struct apple_scaler_variant apple_scaler_t8103 = {
	.hw_version = 10,
	.num_reg = 2,
};

static const struct apple_scaler_variant apple_scaler_t6000 = {
	.hw_version = 12,
	.num_reg = 3,
};

static const struct apple_scaler_variant apple_scaler_t8112 = {
	.hw_version = 13,
	.num_reg = 3,
};

/* M4: the first engine with the MSR20 control block. */
static const struct apple_scaler_variant apple_scaler_t8132 = {
	.hw_version = 20,
	.num_reg = 4,
	.msr20 = true,
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

/* M5: MSR23, firmware-fronted: the driver builds each job and queues it to the firmware. */
static const struct apple_scaler_variant apple_scaler_t8142 = {
	.hw_version = 23,
	.num_reg = 10,
	.msr20 = true,
	.fw_fronted = true,
	.fw_name = "apple/msr-t8142.bin",
	.powerup = msr23_t8142_powerup,
	.num_powerup = ARRAY_SIZE(msr23_t8142_powerup),
};

static const struct of_device_id asc_of_match[] = {
	{ .compatible = "apple,t8103-scaler", .data = &apple_scaler_t8103 },
	{ .compatible = "apple,t6000-scaler", .data = &apple_scaler_t6000 },
	{ .compatible = "apple,t8112-scaler", .data = &apple_scaler_t8112 },
	{ .compatible = "apple,t8132-scaler", .data = &apple_scaler_t8132 },
	{ .compatible = "apple,t6041-scaler", .data = &apple_scaler_t6041 },
	{ .compatible = "apple,t8140-scaler", .data = &apple_scaler_t8140 },
	{ .compatible = "apple,t8142-scaler", .data = &apple_scaler_t8142 },
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
MODULE_FIRMWARE("apple/msr-t8142.bin");
