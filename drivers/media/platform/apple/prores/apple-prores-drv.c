// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple ProRes (APR) V4L2 memory-to-memory encoder.
 *
 * One job runs at a time. The driver writes a descriptor into a 128 KiB DMA
 * ring, advances the producer head and waits for the completion interrupt.
 * Raw frames come in on the OUTPUT queue as semi-planar 4:2:2 or 4:4:4 (8 or
 * 16 bits per sample) and complete ProRes frames go out on CAPTURE.
 *
 * Based on Acelogic's J700 ProRes diagnostic, which first encoded on this
 * engine under Linux, and on the m1n1 proxyclient ProRes experiment (MIT).
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/sizes.h>
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

#include "apple-prores.h"

#define APR_NAME		"apple-prores"
#define APR_MIN_W		16
#define APR_MIN_H		16
#define APR_MAX_W		8192
#define APR_MAX_H		8192
#define APR_TIMEOUT_MS		1000
#define APR_POLL_MS		4
#define APR_AUTOSUSPEND_MS	500
#define APR_RING_ENTRIES	(APR_RING_BYTES / APR_DESC_BYTES)

struct apr_fmt {
	u32 fourcc;
	enum apr_input input;
	u8 bytes;	/* per sample */
	bool is444;
};

static const struct apr_fmt apr_raw_fmts[] = {
	{ V4L2_PIX_FMT_NV16, APR_INPUT_422_8, 1, false },
	{ V4L2_PIX_FMT_P216, APR_INPUT_422_16, 2, false },
	{ V4L2_PIX_FMT_NV24, APR_INPUT_444_8, 1, true },
	{ V4L2_PIX_FMT_P416, APR_INPUT_444_16, 2, true },
};


/*
 * Settings the engine and its AXI-to-fabric bridge need before the first
 * job, as read-modify-write (offset, mask, value), in order. T8140 values.
 */
struct apr_tunable {
	u32 offset;
	u32 mask;
	u32 value;
};

static const struct apr_tunable apr_t8140_engine_tunables[] = {
	{ 0x008, 0x00000008, 0x00000008 },
	{ 0x118, 0x0fff0fff, 0x00000280 },
	{ 0x11c, 0x0fff0fff, 0x000001e0 },
	{ 0x148, 0x00000001, 0x00000001 },
	{ 0x180, 0x8fff3fff, 0x800a04ff },
	{ 0x184, 0x000000ff, 0x00000001 },
	{ 0x188, 0x0fff0fff, 0x02c00040 },
	{ 0x1c0, 0x8fff3fff, 0x800404ff },
	{ 0x1c4, 0x000000ff, 0x00000003 },
	{ 0x1c8, 0x0fff0fff, 0x00000140 },
	{ 0x200, 0x8fff3fff, 0x800504ff },
	{ 0x204, 0x000000ff, 0x00000002 },
	{ 0x208, 0x0fff0fff, 0x01400140 },
	{ 0x240, 0x8fff3fff, 0x800004ff },
	{ 0x244, 0x000000ff, 0x00000007 },
	{ 0x248, 0x000000ff, 0x00000040 },
	{ 0x280, 0x8fff3fff, 0x800104ff },
	{ 0x284, 0x000000ff, 0x00000006 },
	{ 0x288, 0x000000ff, 0x0000007e },
	{ 0x2c0, 0x8fff3fff, 0x800204ff },
	{ 0x2c4, 0x000000ff, 0x00000005 },
	{ 0x2c8, 0x000000ff, 0x00000040 },
	{ 0x300, 0x8fff3fff, 0x800304ff },
	{ 0x304, 0x000000ff, 0x00000004 },
	{ 0x308, 0x000000ff, 0x00000040 },
	{ 0x340, 0xbfffff86, 0x81000006 },
	{ 0x344, 0xffffffff, 0x0b0a0908 },
	{ 0x348, 0x0000ffff, 0x00000720 },
	{ 0x34c, 0x000fffff, 0x000014ff },
	{ 0x350, 0x00ffffff, 0x00928170 },
	{ 0x380, 0x8fff3fff, 0x800b08ff },
	{ 0x384, 0x000000ff, 0x0000000c },
	{ 0x388, 0x0fff0fff, 0x02800040 },
	{ 0x394, 0xffffffc0, 0x00200000 },
	{ 0x3c0, 0x8fff3fff, 0x800b08ff },
	{ 0x3c4, 0x000000ff, 0x00000019 },
	{ 0x3c8, 0x0fff0fff, 0x03000010 },
	{ 0x4c0, 0x800f3fff, 0x800b0880 },
	{ 0x4c4, 0x000000ff, 0x0000001b },
	{ 0x4c8, 0x0fff0fff, 0x01f80008 },
	{ 0x500, 0x800f3fff, 0x800604ff },
	{ 0x504, 0x000000ff, 0x0000000d },
	{ 0x508, 0x0fff0fff, 0x00000100 },
	{ 0x540, 0x800f3fff, 0x80000500 },
	{ 0x544, 0x000000ff, 0x00000011 },
	{ 0x548, 0x000000ff, 0x0000007e },
	{ 0x580, 0x800f3fff, 0x80010500 },
	{ 0x584, 0x000000ff, 0x00000010 },
	{ 0x588, 0x000000ff, 0x0000007e },
	{ 0x5c0, 0x800f3fff, 0x80020500 },
	{ 0x5c4, 0x000000ff, 0x0000000f },
	{ 0x5c8, 0x000000ff, 0x00000040 },
	{ 0x600, 0x800f3fff, 0x80030500 },
	{ 0x604, 0x000000ff, 0x0000000e },
	{ 0x608, 0x000000ff, 0x00000040 },
	{ 0x640, 0x802ff040, 0x80070040 },
	{ 0x644, 0x00ffffff, 0x00071716 },
	{ 0x648, 0xffff0003, 0x00ff0003 },
	{ 0x650, 0x0fff0fff, 0x01e00008 },
	{ 0x680, 0x802ff040, 0x80081040 },
	{ 0x684, 0x00ffffff, 0x00071514 },
	{ 0x688, 0xffff0003, 0x00ff0003 },
	{ 0x690, 0x0fff0fff, 0x01e80010 },
	{ 0x6c0, 0x802ff040, 0x80092040 },
	{ 0x6c4, 0x00ffffff, 0x00071312 },
	{ 0x6c8, 0xffff0003, 0x00ff0003 },
	{ 0x6d0, 0x0fff0fff, 0x01f80008 },
	{ 0x700, 0x800f3fff, 0x800b0880 },
	{ 0x704, 0x000000ff, 0x00000018 },
	{ 0x708, 0x0fff0fff, 0x01000080 },
	{ 0x714, 0xffffffc0, 0x00200000 },
	{ 0x740, 0x800f3fff, 0x800b0880 },
	{ 0x744, 0x000000ff, 0x0000001a },
	{ 0x748, 0x0fff0fff, 0x01800008 },
	{ 0x780, 0x800f3fff, 0x800b0880 },
	{ 0x784, 0x000000ff, 0x0000001c },
	{ 0x788, 0x0fff0fff, 0x01e00008 },
	{ 0x7c0, 0x800f3fff, 0x800b0880 },
	{ 0x7c4, 0x000000ff, 0x0000001d },
	{ 0x7c8, 0x0fff0fff, 0x01e80008 },
};

static const struct apr_tunable apr_t8140_wrapper_tunables[] = {
	{ 0x0000, 0x800000ff, 0x8000001a },
	{ 0x0004, 0x80ff00ff, 0x800b000c },
	{ 0x0008, 0x80ff00ff, 0x800b000c },
	{ 0x000c, 0x80ff00ff, 0x800b000c },
	{ 0x0010, 0x80ff00ff, 0x80000003 },
	{ 0x0014, 0x80ff00ff, 0x80000003 },
	{ 0x0018, 0x800000ff, 0x8000000f },
	{ 0x001c, 0x800000ff, 0x80000020 },
	{ 0x0020, 0x80ff00ff, 0x8014000c },
	{ 0x0024, 0x80ff00ff, 0x801a0006 },
	{ 0x0028, 0x80ff00ff, 0x80090003 },
	{ 0x002c, 0x80ff00ff, 0x80090003 },
	{ 0x0030, 0x800000ff, 0x80000013 },
	{ 0x0034, 0x800000ff, 0x8000000d },
	{ 0x0038, 0x800000ff, 0x8000000d },
	{ 0x0100, 0x000000ff, 0x000000ff },
	{ 0x0108, 0x00000006, 0x00000000 },
	{ 0x0304, 0x3f3f0000, 0x04040000 },
	{ 0x0314, 0x80000000, 0x80000000 },
	{ 0x031c, 0x80000000, 0x80000000 },
	{ 0x0408, 0xffff0020, 0x00100020 },
	{ 0x0700, 0x00000400, 0x00000400 },
	{ 0x0748, 0x07ff07ff, 0x00100010 },
	{ 0x0800, 0x00000400, 0x00000400 },
	{ 0x0848, 0x07ff07ff, 0x00800040 },
	{ 0x0a00, 0xffffffff, 0x7fffffff },
	{ 0x0a04, 0x00000100, 0x00000100 },
	{ 0x4008, 0x00000001, 0x00000000 },
	{ 0x400c, 0x00000001, 0x00000000 },
	{ 0x4014, 0x00000001, 0x00000000 },
	{ 0x4018, 0x00000001, 0x00000000 },
	{ 0x401c, 0x00000001, 0x00000001 },
};

struct apr_dev {
	struct device *dev;
	void __iomem *regs;
	void __iomem *wrapper;
	struct reset_control *reset;
	int irq[2];

	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
	struct mutex vdev_lock;	/* serialises ioctls and queue state */

	/* Descriptor ring, written by the CPU and read by the engine. */
	u8 *ring;
	dma_addr_t ring_dma;
	/* Statistics area the engine owns; size from its capacity registers. */
	void *stats;
	dma_addr_t stats_dma;
	size_t stats_size;

	spinlock_t hw_lock;	/* protects the fields below */
	struct apr_ctx *active;	/* job in the engine, or NULL */
	u32 head;		/* next free ring offset */
	bool ring_ready;	/* engine registers programmed since power-up */
	bool broken;		/* timed out with no way to reset */
	unsigned long started;	/* jiffies at submission */
	struct delayed_work watchdog;
};

struct apr_ctx {
	struct v4l2_fh fh;
	struct apr_dev *apr;
	struct v4l2_ctrl_handler hdl;
	struct v4l2_ctrl *profile;
	struct v4l2_pix_format_mplane src_fmt;
	struct v4l2_pix_format_mplane dst_fmt;
	const struct apr_fmt *raw;
	enum apr_profile stream_profile;	/* fixed while OUTPUT streams */
	u32 sequence;
};

static struct apr_ctx *apr_file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct apr_ctx, fh);
}

/* ---- hardware ---------------------------------------------------------- */

static void apr_write(struct apr_dev *apr, u32 reg, u32 val)
{
	writel_relaxed(val, apr->regs + reg);
}

static u32 apr_read(struct apr_dev *apr, u32 reg)
{
	return readl_relaxed(apr->regs + reg);
}

/*
 * Program the engine from power-up state. The ring must be installed before
 * MODE enables descriptor execution, otherwise the engine fetches from IOVA 0.
 */
static void apr_apply(void __iomem *base, const struct apr_tunable *t, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		u32 v = readl_relaxed(base + t[i].offset);

		writel_relaxed((v & ~t[i].mask) | (t[i].value & t[i].mask), base + t[i].offset);
	}
}

static void apr_hw_init(struct apr_dev *apr)
{
	unsigned int t, i;

	apr_write(apr, APR_REG_MODE, APR_MODE_OFF);
	apr_write(apr, APR_REG_IRQ_ENABLE, 0);
	apr_apply(apr->regs, apr_t8140_engine_tunables, ARRAY_SIZE(apr_t8140_engine_tunables));
	apr_apply(apr->wrapper, apr_t8140_wrapper_tunables,
		  ARRAY_SIZE(apr_t8140_wrapper_tunables));
	apr_write(apr, APR_REG_RING_HEAD, 0);
	apr_write(apr, APR_REG_RING_TAIL, 0);
	apr_write(apr, APR_REG_RING_SIZE, APR_RING_BYTES);
	apr_write(apr, APR_REG_RING_LO, lower_32_bits(apr->ring_dma));
	apr_write(apr, APR_REG_RING_HI, upper_32_bits(apr->ring_dma));
	/* Both statistics consumers share one area. */
	apr_write(apr, APR_REG_STATS0_LO, lower_32_bits(apr->stats_dma) & ~0x3fU);
	apr_write(apr, APR_REG_STATS0_HI, upper_32_bits(apr->stats_dma) & 0x3ff);
	apr_write(apr, APR_REG_STATS1_LO, lower_32_bits(apr->stats_dma) & ~0x3fU);
	apr_write(apr, APR_REG_STATS1_HI, upper_32_bits(apr->stats_dma) & 0x3ff);
	apr_write(apr, APR_REG_MODE, APR_MODE_RUN);

	for (t = 0; t < APR_QTABLES; t++) {
		for (i = 0; i < 32; i++) {
			apr_write(apr, APR_REG_QUANT_LUMA(t) + 4 * i,
				  apr_qmat_reg(apr_qmat_luma[t], i));
			apr_write(apr, APR_REG_QUANT_CHROMA(t) + 4 * i,
				  apr_qmat_reg(apr_qmat_chroma[t], i));
		}
	}
	for (i = 0; i < APR_DC_SCALE_ENTRIES; i++)
		apr_write(apr, APR_REG_DC_SCALE(i), apr_dc_scale_reg(i));

	apr_write(apr, APR_REG_IRQ_STATUS, apr_read(apr, APR_REG_IRQ_STATUS));
	apr_write(apr, APR_REG_IRQ_ENABLE, APR_IRQ_DONE | APR_IRQ_ERROR);
	/* Order the setup before any descriptor publication. */
	wmb();
	apr->head = 0;
	apr->ring_ready = true;
}

static u32 apr_coded_size(u32 width, u32 height, bool is444)
{
	/* Worst case for 4444 XQ at the lowest quantiser, plus headers. */
	u64 size = (u64)ALIGN(width, 16) * ALIGN(height, 16) * (is444 ? 4 : 3) + SZ_64K;

	return min_t(u64, PAGE_ALIGN(size), SZ_256M);
}

static u8 apr_color_primaries(const struct v4l2_pix_format_mplane *f)
{
	switch (f->colorspace) {
	case V4L2_COLORSPACE_REC709:
	case V4L2_COLORSPACE_SRGB:
		return 1;
	case V4L2_COLORSPACE_SMPTE170M:
		return 6;
	case V4L2_COLORSPACE_470_SYSTEM_BG:
		return 5;
	case V4L2_COLORSPACE_BT2020:
		return 9;
	case V4L2_COLORSPACE_DCI_P3:
		return 11;
	default:
		return 2;	/* unspecified */
	}
}

static u8 apr_color_transfer(const struct v4l2_pix_format_mplane *f)
{
	switch (f->xfer_func) {
	case V4L2_XFER_FUNC_709:
		return 1;
	case V4L2_XFER_FUNC_SMPTE2084:
		return 16;
	default:
		return f->colorspace == V4L2_COLORSPACE_REC709 ? 1 : 2;
	}
}

static u8 apr_color_matrix(const struct v4l2_pix_format_mplane *f)
{
	switch (f->ycbcr_enc) {
	case V4L2_YCBCR_ENC_709:
		return 1;
	case V4L2_YCBCR_ENC_601:
		return 6;
	case V4L2_YCBCR_ENC_BT2020:
		return 9;
	default:
		return f->colorspace == V4L2_COLORSPACE_REC709 ? 1 : 2;
	}
}

static void apr_fill_frame(struct apr_ctx *ctx, struct vb2_v4l2_buffer *src,
			   struct vb2_v4l2_buffer *dst, struct apr_frame *f)
{
	const struct v4l2_pix_format_mplane *s = &ctx->src_fmt;
	u32 luma_stride = s->plane_fmt[0].bytesperline;
	dma_addr_t in = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);

	*f = (struct apr_frame){
		.width = s->width,
		.height = s->height,
		.input = ctx->raw->input,
		.profile = ctx->stream_profile,
		.luma_stride = luma_stride,
		.chroma_stride = luma_stride * (ctx->raw->is444 ? 2 : 1),
		.luma = in,
		.chroma = in + (u64)luma_stride * s->height,
		.output = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0),
		.output_size = vb2_plane_size(&dst->vb2_buf, 0),
		.primaries = apr_color_primaries(s),
		.transfer = apr_color_transfer(s),
		.matrix = apr_color_matrix(s),
	};
}

static void apr_finish_job(struct apr_dev *apr, struct apr_ctx *ctx, int err)
{
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *src, *dst;
	u32 bytes = 0;

	src = v4l2_m2m_src_buf_remove(m2m);
	dst = v4l2_m2m_dst_buf_remove(m2m);
	if (WARN_ON(!src || !dst))
		goto out;

	if (!err) {
		u8 *out = vb2_plane_vaddr(&dst->vb2_buf, 0);

		/* The engine wrote the frame through the DART; see it now. */
		dma_rmb();
		err = out ? apr_check_output(out, vb2_plane_size(&dst->vb2_buf, 0),
					     &bytes) : -EIO;
		if (err)
			dev_err_ratelimited(apr->dev, "engine returned no frame\n");
	}
	vb2_set_plane_payload(&dst->vb2_buf, 0, err ? 0 : bytes);
	v4l2_m2m_buf_copy_metadata(src, dst);
	dst->sequence = ctx->sequence++;
	dst->flags |= V4L2_BUF_FLAG_KEYFRAME;
	if (v4l2_m2m_is_last_draining_src_buf(m2m, src)) {
		static const struct v4l2_event eos = { .type = V4L2_EVENT_EOS };

		dst->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_event_queue_fh(&ctx->fh, &eos);
		v4l2_m2m_mark_stopped(m2m);
	}
	v4l2_m2m_buf_done(src, err ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	v4l2_m2m_buf_done(dst, err ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
out:
	pm_runtime_mark_last_busy(apr->dev);
	pm_runtime_put_autosuspend(apr->dev);
	v4l2_m2m_job_finish(apr->m2m_dev, m2m);
}

/*
 * Claim the active job if the engine has finished it. Returns the context to
 * finish, or NULL. Called with hw_lock held.
 */
static struct apr_ctx *apr_take_done(struct apr_dev *apr, int *err)
{
	struct apr_ctx *ctx = apr->active;
	u32 status, tail;

	if (!ctx)
		return NULL;
	status = apr_read(apr, APR_REG_IRQ_STATUS);
	if (!status)
		return NULL;
	apr_write(apr, APR_REG_IRQ_STATUS, status);
	tail = apr_read(apr, APR_REG_RING_TAIL) & APR_RING_PTR_MASK;
	*err = 0;
	if (status != APR_IRQ_DONE || tail != apr->head) {
		dev_err_ratelimited(apr->dev,
				    "job failed: status %#x tail %#x head %#x st %#x/%#x\n",
				    status, tail, apr->head,
				    apr_read(apr, APR_REG_ST0), apr_read(apr, APR_REG_ST1));
		*err = -EIO;
	}
	apr->active = NULL;
	return ctx;
}

static irqreturn_t apr_irq(int irq, void *data)
{
	struct apr_dev *apr = data;
	struct apr_ctx *ctx;
	int err = 0;

	spin_lock(&apr->hw_lock);
	ctx = apr_take_done(apr, &err);
	spin_unlock(&apr->hw_lock);
	if (!ctx)
		return IRQ_NONE;
	cancel_delayed_work(&apr->watchdog);
	apr_finish_job(apr, ctx, err);
	return IRQ_HANDLED;
}

/*
 * Backstop for a lost interrupt and for hangs. A hung engine is reset
 * through its power domain and reprogrammed; without a reset line the
 * device refuses further jobs rather than reuse memory it may still own.
 */
static void apr_watchdog(struct work_struct *work)
{
	struct apr_dev *apr = container_of(work, struct apr_dev, watchdog.work);
	struct apr_ctx *ctx;
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&apr->hw_lock, flags);
	ctx = apr_take_done(apr, &err);
	if (ctx) {
		spin_unlock_irqrestore(&apr->hw_lock, flags);
		dev_warn_once(apr->dev, "completion found without an interrupt\n");
		apr_finish_job(apr, ctx, err);
		return;
	}
	ctx = apr->active;
	if (!ctx) {
		spin_unlock_irqrestore(&apr->hw_lock, flags);
		return;
	}
	if (time_before(jiffies, apr->started + msecs_to_jiffies(APR_TIMEOUT_MS))) {
		spin_unlock_irqrestore(&apr->hw_lock, flags);
		schedule_delayed_work(&apr->watchdog, msecs_to_jiffies(APR_POLL_MS));
		return;
	}
	dev_err(apr->dev, "job timed out: head %#x tail %#x st %#x/%#x\n",
		apr->head, apr_read(apr, APR_REG_RING_TAIL),
		apr_read(apr, APR_REG_ST0), apr_read(apr, APR_REG_ST1));
	apr->active = NULL;
	apr_write(apr, APR_REG_IRQ_ENABLE, 0);
	apr_write(apr, APR_REG_MODE, APR_MODE_OFF);
	apr->ring_ready = false;
	/* No new job can start until this one is finished below. */
	spin_unlock_irqrestore(&apr->hw_lock, flags);
	if (!apr->reset || reset_control_reset(apr->reset)) {
		dev_err(apr->dev, "engine cannot be reset, refusing further jobs\n");
		spin_lock_irqsave(&apr->hw_lock, flags);
		apr->broken = true;
		spin_unlock_irqrestore(&apr->hw_lock, flags);
	}
	apr_finish_job(apr, ctx, -ETIMEDOUT);
}

static void apr_device_run(void *priv)
{
	struct apr_ctx *ctx = priv;
	struct apr_dev *apr = ctx->apr;
	struct vb2_v4l2_buffer *src, *dst;
	struct apr_frame f;
	unsigned long flags;
	void *out;
	int ret;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	ret = pm_runtime_resume_and_get(apr->dev);
	if (ret < 0) {
		pm_runtime_get_noresume(apr->dev);
		apr_finish_job(apr, ctx, ret);
		return;
	}
	apr_fill_frame(ctx, src, dst, &f);
	/* Mark the output invalid so a frame the engine never wrote is caught. */
	out = vb2_plane_vaddr(&dst->vb2_buf, 0);
	if (!out) {
		apr_finish_job(apr, ctx, -EINVAL);
		return;
	}
	memset(out, 0, 8);

	spin_lock_irqsave(&apr->hw_lock, flags);
	if (apr->broken) {
		spin_unlock_irqrestore(&apr->hw_lock, flags);
		apr_finish_job(apr, ctx, -EIO);
		return;
	}
	/*
	 * The engine's ring wrap protocol is not known, so start again at the
	 * beginning of the ring instead of wrapping. The engine is idle here.
	 */
	if (!apr->ring_ready || apr->head + APR_DESC_BYTES > APR_RING_BYTES)
		apr_hw_init(apr);
	ret = apr_build_desc(&f, apr->ring + apr->head);
	if (ret) {
		spin_unlock_irqrestore(&apr->hw_lock, flags);
		apr_finish_job(apr, ctx, ret);
		return;
	}
	apr->head += APR_DESC_BYTES;
	apr->active = ctx;
	apr->started = jiffies;
	/* Descriptor and buffers must be visible before the head moves. */
	dma_wmb();
	writel(apr->head, apr->regs + APR_REG_RING_HEAD);
	spin_unlock_irqrestore(&apr->hw_lock, flags);
	schedule_delayed_work(&apr->watchdog, msecs_to_jiffies(APR_POLL_MS));
}

static const struct v4l2_m2m_ops apr_m2m_ops = {
	.device_run = apr_device_run,
};

/* ---- formats ----------------------------------------------------------- */

static const struct apr_fmt *apr_find_raw(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(apr_raw_fmts); i++)
		if (apr_raw_fmts[i].fourcc == fourcc)
			return &apr_raw_fmts[i];
	return NULL;
}

/*
 * One contiguous plane: Y rows, then interleaved CbCr rows. The engine
 * reads whole 16-row macroblocks, so space for the rows that round the
 * height up follows the chroma region.
 */
static void apr_fill_raw(struct v4l2_pix_format_mplane *pix, const struct apr_fmt *fmt)
{
	u32 width = clamp_t(u32, ALIGN(pix->width, 2), APR_MIN_W, APR_MAX_W);
	u32 height = clamp_t(u32, pix->height, APR_MIN_H, APR_MAX_H);
	u32 min_stride = ALIGN(width * fmt->bytes, 64);
	u32 stride = ALIGN(pix->plane_fmt[0].bytesperline, 64);
	u32 cstride;

	/* Accept padding up to one extra row width, as other drivers do. */
	if (stride < min_stride || stride > 2 * min_stride)
		stride = min_stride;
	cstride = stride * (fmt->is444 ? 2 : 1);
	pix->pixelformat = fmt->fourcc;
	pix->width = width;
	pix->height = height;
	pix->field = V4L2_FIELD_NONE;
	pix->num_planes = 1;
	pix->flags = 0;
	memset(pix->reserved, 0, sizeof(pix->reserved));
	memset(pix->plane_fmt, 0, sizeof(pix->plane_fmt));
	pix->plane_fmt[0].bytesperline = stride;
	pix->plane_fmt[0].sizeimage = (stride + cstride) * ALIGN(height, 16);
	if (pix->colorspace == V4L2_COLORSPACE_DEFAULT)
		pix->colorspace = V4L2_COLORSPACE_REC709;
	if (pix->xfer_func == V4L2_XFER_FUNC_DEFAULT)
		pix->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(pix->colorspace);
	if (pix->ycbcr_enc == V4L2_YCBCR_ENC_DEFAULT)
		pix->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(pix->colorspace);
	if (pix->quantization == V4L2_QUANTIZATION_DEFAULT)
		pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
}

static void apr_fill_coded(struct v4l2_pix_format_mplane *dst,
			   const struct v4l2_pix_format_mplane *src, bool is444)
{
	*dst = *src;
	dst->pixelformat = V4L2_PIX_FMT_PRORES;
	dst->num_planes = 1;
	memset(dst->plane_fmt, 0, sizeof(dst->plane_fmt));
	dst->plane_fmt[0].sizeimage = apr_coded_size(src->width, src->height, is444);
}

static void apr_reset_fmts(struct apr_ctx *ctx)
{
	ctx->raw = &apr_raw_fmts[0];
	memset(&ctx->src_fmt, 0, sizeof(ctx->src_fmt));
	ctx->src_fmt.width = 1920;
	ctx->src_fmt.height = 1080;
	apr_fill_raw(&ctx->src_fmt, ctx->raw);
	apr_fill_coded(&ctx->dst_fmt, &ctx->src_fmt, ctx->raw->is444);
}

static int apr_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, APR_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Apple ProRes encoder", sizeof(cap->card));
	return 0;
}

static int apr_enum_fmt_out(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(apr_raw_fmts))
		return -EINVAL;
	f->pixelformat = apr_raw_fmts[f->index].fourcc;
	return 0;
}

static int apr_enum_fmt_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_PRORES;
	f->flags = V4L2_FMT_FLAG_COMPRESSED;
	return 0;
}

static int apr_enum_framesizes(struct file *file, void *priv,
			       struct v4l2_frmsizeenum *fs)
{
	if (fs->index || (fs->pixel_format != V4L2_PIX_FMT_PRORES &&
			  !apr_find_raw(fs->pixel_format)))
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fs->stepwise = (struct v4l2_frmsize_stepwise){
		APR_MIN_W, APR_MAX_W, 2, APR_MIN_H, APR_MAX_H, 1,
	};
	return 0;
}

static int apr_g_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = apr_file_to_ctx(file)->src_fmt;
	return 0;
}

static int apr_g_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = apr_file_to_ctx(file)->dst_fmt;
	return 0;
}

static int apr_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	const struct apr_fmt *fmt = apr_find_raw(f->fmt.pix_mp.pixelformat);

	apr_fill_raw(&f->fmt.pix_mp, fmt ?: &apr_raw_fmts[0]);
	return 0;
}

/* The coded size follows the raw format, as for other stateful encoders. */
static int apr_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);

	apr_fill_coded(&f->fmt.pix_mp, &ctx->src_fmt, ctx->raw->is444);
	return 0;
}

static int apr_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;

	if (vb2_is_busy(v4l2_m2m_get_src_vq(m2m)) ||
	    vb2_is_busy(v4l2_m2m_get_dst_vq(m2m)))
		return -EBUSY;
	apr_try_fmt_out(file, priv, f);
	ctx->src_fmt = f->fmt.pix_mp;
	ctx->raw = apr_find_raw(ctx->src_fmt.pixelformat);
	apr_fill_coded(&ctx->dst_fmt, &ctx->src_fmt, ctx->raw->is444);
	/* Keep the profile consistent with the chroma format. */
	if (ctx->raw->is444 != apr_profile_is_444(v4l2_ctrl_g_ctrl(ctx->profile)))
		v4l2_ctrl_s_ctrl(ctx->profile, ctx->raw->is444 ?
				   V4L2_MPEG_VIDEO_PRORES_PROFILE_4444 :
				   V4L2_MPEG_VIDEO_PRORES_PROFILE_STANDARD);
	return 0;
}

static int apr_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);

	if (vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;
	apr_try_fmt_cap(file, priv, f);
	ctx->dst_fmt = f->fmt.pix_mp;
	return 0;
}

static int apr_subscribe_event(struct v4l2_fh *fh, const struct v4l2_event_subscription *sub)
{
	if (sub->type == V4L2_EVENT_EOS)
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	return v4l2_ctrl_subscribe_event(fh, sub);
}

static const struct v4l2_ioctl_ops apr_ioctl_ops = {
	.vidioc_querycap = apr_querycap,
	.vidioc_enum_fmt_vid_out = apr_enum_fmt_out,
	.vidioc_enum_fmt_vid_cap = apr_enum_fmt_cap,
	.vidioc_enum_framesizes = apr_enum_framesizes,
	.vidioc_g_fmt_vid_out_mplane = apr_g_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = apr_g_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = apr_try_fmt_out,
	.vidioc_try_fmt_vid_cap_mplane = apr_try_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane = apr_s_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = apr_s_fmt_cap,

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
	.vidioc_encoder_cmd = v4l2_m2m_ioctl_encoder_cmd,

	.vidioc_subscribe_event = apr_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ---- queues ------------------------------------------------------------ */

static struct v4l2_pix_format_mplane *apr_q_fmt(struct apr_ctx *ctx, u32 type)
{
	return V4L2_TYPE_IS_OUTPUT(type) ? &ctx->src_fmt : &ctx->dst_fmt;
}

static int apr_queue_setup(struct vb2_queue *q, unsigned int *nbufs,
			   unsigned int *nplanes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	struct apr_ctx *ctx = vb2_get_drv_priv(q);
	u32 size = apr_q_fmt(ctx, q->type)->plane_fmt[0].sizeimage;

	if (*nplanes)
		return *nplanes == 1 && sizes[0] >= size ? 0 : -EINVAL;
	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int apr_buf_prepare(struct vb2_buffer *vb)
{
	struct apr_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	u32 size = apr_q_fmt(ctx, vb->type)->plane_fmt[0].sizeimage;

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	if (V4L2_TYPE_IS_OUTPUT(vb->type)) {
		/* The engine always reads the whole padded surface. */
		vb2_set_plane_payload(vb, 0, size);
	} else {
		vb2_set_plane_payload(vb, 0, 0);
	}
	return 0;
}

static void apr_buf_queue(struct vb2_buffer *vb)
{
	struct apr_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	if (!V4L2_TYPE_IS_OUTPUT(vb->type) &&
	    vb2_is_streaming(vb->vb2_queue) &&
	    v4l2_m2m_dst_buf_is_last(ctx->fh.m2m_ctx)) {
		vb2_set_plane_payload(vb, 0, 0);
		vbuf->field = V4L2_FIELD_NONE;
		v4l2_m2m_last_buffer_done(ctx->fh.m2m_ctx, vbuf);
		return;
	}
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static int apr_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct apr_ctx *ctx = vb2_get_drv_priv(q);

	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		if (ctx->raw->is444 != apr_profile_is_444(v4l2_ctrl_g_ctrl(ctx->profile))) {
			struct vb2_v4l2_buffer *vbuf;

			while ((vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
				v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_QUEUED);
			return -EINVAL;
		}
		ctx->stream_profile = v4l2_ctrl_g_ctrl(ctx->profile);
		v4l2_ctrl_grab(ctx->profile, true);
		ctx->sequence = 0;
	}
	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, q);
	return 0;
}

static void apr_stop_streaming(struct vb2_queue *q)
{
	struct apr_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vbuf;

	for (;;) {
		vbuf = V4L2_TYPE_IS_OUTPUT(q->type) ?
			v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, q);
	if (V4L2_TYPE_IS_OUTPUT(q->type))
		v4l2_ctrl_grab(ctx->profile, false);
	if (V4L2_TYPE_IS_OUTPUT(q->type) && v4l2_m2m_has_stopped(ctx->fh.m2m_ctx)) {
		static const struct v4l2_event eos = { .type = V4L2_EVENT_EOS };

		v4l2_event_queue_fh(&ctx->fh, &eos);
	}
}

static const struct vb2_ops apr_qops = {
	.queue_setup = apr_queue_setup,
	.buf_prepare = apr_buf_prepare,
	.buf_queue = apr_buf_queue,
	.start_streaming = apr_start_streaming,
	.stop_streaming = apr_stop_streaming,
};

static int apr_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct apr_ctx *ctx = priv;
	int ret;

	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src->io_modes = VB2_MMAP | VB2_DMABUF;
	src->drv_priv = ctx;
	src->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src->ops = &apr_qops;
	src->mem_ops = &vb2_dma_contig_memops;
	src->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src->lock = &ctx->apr->vdev_lock;
	src->dev = ctx->apr->dev;
	ret = vb2_queue_init(src);
	if (ret)
		return ret;

	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst->io_modes = VB2_MMAP | VB2_DMABUF;
	dst->drv_priv = ctx;
	dst->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst->ops = &apr_qops;
	dst->mem_ops = &vb2_dma_contig_memops;
	dst->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst->lock = &ctx->apr->vdev_lock;
	dst->dev = ctx->apr->dev;
	return vb2_queue_init(dst);
}

/* ---- controls and file operations -------------------------------------- */

/* Controls are read when a job is built; nothing to apply on change. */
static int apr_s_ctrl(struct v4l2_ctrl *ctrl)
{
	return 0;
}

static const struct v4l2_ctrl_ops apr_ctrl_ops = {
	.s_ctrl = apr_s_ctrl,
};

static int apr_init_ctrls(struct apr_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->hdl;

	v4l2_ctrl_handler_init(hdl, 1);
	/* The profile must match the raw format's chroma; checked at STREAMON. */
	ctx->profile = v4l2_ctrl_new_std_menu(hdl, &apr_ctrl_ops,
					      V4L2_CID_MPEG_VIDEO_PRORES_PROFILE,
					      V4L2_MPEG_VIDEO_PRORES_PROFILE_4444_XQ, 0,
					      V4L2_MPEG_VIDEO_PRORES_PROFILE_STANDARD);
	if (hdl->error) {
		int ret = hdl->error;

		v4l2_ctrl_handler_free(hdl);
		return ret;
	}
	ctx->fh.ctrl_handler = hdl;
	return 0;
}

static int apr_open(struct file *file)
{
	struct apr_dev *apr = video_drvdata(file);
	struct apr_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	ctx->apr = apr;
	apr_reset_fmts(ctx);
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ret = apr_init_ctrls(ctx);
	if (ret)
		goto err_fh;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(apr->m2m_dev, ctx, apr_queue_init);
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

static int apr_release(struct file *file)
{
	struct apr_ctx *ctx = apr_file_to_ctx(file);
	struct apr_dev *apr = ctx->apr;

	mutex_lock(&apr->vdev_lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	mutex_unlock(&apr->vdev_lock);
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations apr_fops = {
	.owner = THIS_MODULE,
	.open = apr_open,
	.release = apr_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

/* ---- power management and probe ---------------------------------------- */

static int apr_runtime_suspend(struct device *dev)
{
	struct apr_dev *apr = dev_get_drvdata(dev);
	unsigned long flags;

	/* Registers do not survive the power domain going off. */
	spin_lock_irqsave(&apr->hw_lock, flags);
	apr->ring_ready = false;
	spin_unlock_irqrestore(&apr->hw_lock, flags);
	return 0;
}

static int apr_runtime_resume(struct device *dev)
{
	return 0;
}

static int apr_suspend(struct device *dev)
{
	struct apr_dev *apr = dev_get_drvdata(dev);

	v4l2_m2m_suspend(apr->m2m_dev);
	return pm_runtime_force_suspend(dev);
}

static int apr_resume(struct device *dev)
{
	struct apr_dev *apr = dev_get_drvdata(dev);
	int ret;

	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;
	v4l2_m2m_resume(apr->m2m_dev);
	return 0;
}

static const struct dev_pm_ops apr_pm_ops = {
	RUNTIME_PM_OPS(apr_runtime_suspend, apr_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(apr_suspend, apr_resume)
};

static void apr_cleanup(void *data)
{
	struct apr_dev *apr = data;

	cancel_delayed_work_sync(&apr->watchdog);
	if (apr->m2m_dev)
		v4l2_m2m_release(apr->m2m_dev);
	v4l2_device_unregister(&apr->v4l2_dev);
}

static int apr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct video_device *vdev;
	struct apr_dev *apr;
	unsigned int i;
	int ret;

	apr = devm_kzalloc(dev, sizeof(*apr), GFP_KERNEL);
	if (!apr)
		return -ENOMEM;
	apr->dev = dev;
	mutex_init(&apr->vdev_lock);
	spin_lock_init(&apr->hw_lock);
	INIT_DELAYED_WORK(&apr->watchdog, apr_watchdog);
	platform_set_drvdata(pdev, apr);

	apr->regs = devm_platform_ioremap_resource_byname(pdev, "core");
	if (IS_ERR(apr->regs))
		return PTR_ERR(apr->regs);
	apr->wrapper = devm_platform_ioremap_resource_byname(pdev, "wrapper");
	if (IS_ERR(apr->wrapper))
		return PTR_ERR(apr->wrapper);
	apr->reset = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(apr->reset))
		return dev_err_probe(dev, PTR_ERR(apr->reset), "reset\n");

	/* Every buffer the engine touches goes through its DART. */
	if (!device_iommu_mapped(dev))
		return dev_err_probe(dev, -ENODEV, "no IOMMU\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	apr->ring = dmam_alloc_coherent(dev, APR_RING_BYTES, &apr->ring_dma,
					GFP_KERNEL);
	if (!apr->ring)
		return -ENOMEM;
	apr->stats_size = APR_STATS_BYTES;
	apr->stats = dmam_alloc_coherent(dev, apr->stats_size, &apr->stats_dma, GFP_KERNEL);
	if (!apr->stats)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(apr->irq); i++) {
		apr->irq[i] = platform_get_irq_optional(pdev, i);
		if (apr->irq[i] == -ENXIO && i)
			continue;
		if (apr->irq[i] < 0)
			return apr->irq[i];
		ret = devm_request_irq(dev, apr->irq[i], apr_irq, 0,
				       dev_name(dev), apr);
		if (ret)
			return ret;
	}

	ret = v4l2_device_register(dev, &apr->v4l2_dev);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, apr_cleanup, apr);
	if (ret)
		return ret;
	apr->m2m_dev = v4l2_m2m_init(&apr_m2m_ops);
	if (IS_ERR(apr->m2m_dev)) {
		ret = PTR_ERR(apr->m2m_dev);
		apr->m2m_dev = NULL;
		return ret;
	}

	pm_runtime_set_autosuspend_delay(dev, APR_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;
	/* The engine reports how much statistics space it writes. */
	if (max(apr_read(apr, APR_REG_STATS0_SIZE), apr_read(apr, APR_REG_STATS1_SIZE)) >
	    apr->stats_size) {
		pm_runtime_put_autosuspend(dev);
		return dev_err_probe(dev, -EINVAL, "statistics area larger than %zu bytes\n",
				     apr->stats_size);
	}
	dev_info(dev, "ProRes encoder, id %#x\n", apr_read(apr, APR_REG_ID));
	pm_runtime_put_autosuspend(dev);

	vdev = &apr->vdev;
	strscpy(vdev->name, APR_NAME, sizeof(vdev->name));
	vdev->fops = &apr_fops;
	vdev->ioctl_ops = &apr_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &apr->vdev_lock;
	vdev->v4l2_dev = &apr->v4l2_dev;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(vdev, apr);
	return video_register_device(vdev, VFL_TYPE_VIDEO, -1);
}

static void apr_remove(struct platform_device *pdev)
{
	struct apr_dev *apr = platform_get_drvdata(pdev);

	video_unregister_device(&apr->vdev);
}

static const struct of_device_id apr_of_match[] = {
	{ .compatible = "apple,t8140-prores" },
	{ }
};
MODULE_DEVICE_TABLE(of, apr_of_match);

static struct platform_driver apr_driver = {
	.probe = apr_probe,
	.remove = apr_remove,
	.driver = {
		.name = APR_NAME,
		.of_match_table = apr_of_match,
		.pm = pm_ptr(&apr_pm_ops),
	},
};
module_platform_driver(apr_driver);

MODULE_DESCRIPTION("Apple ProRes encoder");
MODULE_LICENSE("GPL");
