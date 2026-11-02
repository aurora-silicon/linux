// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple Silicon JPEG mem2mem scaffold. No hardware jobs are implemented.
 * Register offset provenance: Asahi Linux m1n1 (MIT), commit
 * f3ac615eb1376d3bbba24cd84c414b9e41892adb, proxyclient/m1n1/hw/jpeg.py.
 */
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "apple-jpeg-hw.h"

struct apple_jpeg {
	struct device *dev;
	void __iomem *regs;
	const struct apple_jpeg_variant *variant;
	struct mutex lock; /* Serializes video operations and queue/context state. */
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
};

struct apple_jpeg_ctx {
	struct v4l2_fh fh;
	struct apple_jpeg *jpeg;
	struct v4l2_pix_format_mplane output;
	struct v4l2_pix_format_mplane capture;
};

static struct apple_jpeg_ctx *fh_to_ctx(void *priv)
{
	return container_of(priv, struct apple_jpeg_ctx, fh);
}

static struct v4l2_pix_format_mplane *
apple_jpeg_format(struct apple_jpeg_ctx *ctx, enum v4l2_buf_type type)
{
	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return &ctx->output;
	if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return &ctx->capture;
	return NULL;
}

/* Allocation policy for the scaffold, not a qualified hardware size limit. */
static void apple_jpeg_normalize_format(struct v4l2_pix_format_mplane *pix)
{
	u32 width = clamp_t(u32, pix->width, 16, 8192);
	u32 height = clamp_t(u32, pix->height, 16, 8192);
	u32 fourcc = pix->pixelformat;

	memset(pix, 0, sizeof(*pix));
	pix->width = ALIGN(width, 16);
	pix->height = ALIGN(height, 16);
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_JPEG;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_SRGB;

	if (fourcc == V4L2_PIX_FMT_NV12M) {
		pix->pixelformat = fourcc;
		pix->num_planes = 2;
		pix->plane_fmt[0].bytesperline = pix->width;
		pix->plane_fmt[0].sizeimage = pix->width * pix->height;
		pix->plane_fmt[1].bytesperline = pix->width;
		pix->plane_fmt[1].sizeimage = pix->width * pix->height / 2;
	} else {
		pix->pixelformat = V4L2_PIX_FMT_JPEG;
		pix->num_planes = 1;
		pix->plane_fmt[0].sizeimage = pix->width * pix->height * 3 + 4096;
	}
}

static int apple_jpeg_querycap(struct file *file, void *priv,
			       struct v4l2_capability *cap)
{
	struct apple_jpeg_ctx *ctx = fh_to_ctx(priv);

	strscpy(cap->driver, "apple-jpeg", sizeof(cap->driver));
	strscpy(cap->card, "Apple JPEG scaffold", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(ctx->jpeg->dev));
	return 0;
}

static int apple_jpeg_enum_fmt(struct file *file, void *priv,
			       struct v4l2_fmtdesc *f)
{
	if (f->index > 1)
		return -EINVAL;
	f->pixelformat = f->index ? V4L2_PIX_FMT_NV12M : V4L2_PIX_FMT_JPEG;
	return 0;
}

static int apple_jpeg_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix = apple_jpeg_format(fh_to_ctx(priv), f->type);

	if (!pix)
		return -EINVAL;
	f->fmt.pix_mp = *pix;
	return 0;
}

static int apple_jpeg_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	if (!apple_jpeg_format(fh_to_ctx(priv), f->type))
		return -EINVAL;
	apple_jpeg_normalize_format(&f->fmt.pix_mp);
	return 0;
}

static int apple_jpeg_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct apple_jpeg_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_pix_format_mplane *pix, *other;
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	int ret;

	ret = apple_jpeg_try_fmt(file, priv, f);
	if (ret)
		return ret;
	if (vb2_is_busy(v4l2_m2m_get_src_vq(m2m)) ||
	    vb2_is_busy(v4l2_m2m_get_dst_vq(m2m)))
		return -EBUSY;

	pix = apple_jpeg_format(ctx, f->type);
	other = pix == &ctx->output ? &ctx->capture : &ctx->output;
	*pix = f->fmt.pix_mp;
	*other = *pix;
	other->pixelformat = pix->pixelformat == V4L2_PIX_FMT_JPEG ?
		V4L2_PIX_FMT_NV12M : V4L2_PIX_FMT_JPEG;
	apple_jpeg_normalize_format(other);
	return 0;
}

static int apple_jpeg_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
				  unsigned int *num_planes, unsigned int sizes[],
				  struct device *alloc_devs[])
{
	struct apple_jpeg_ctx *ctx = vb2_get_drv_priv(q);
	struct v4l2_pix_format_mplane *pix = apple_jpeg_format(ctx, q->type);
	unsigned int i;

	if (*num_planes) {
		if (*num_planes != pix->num_planes)
			return -EINVAL;
		for (i = 0; i < *num_planes; i++)
			if (sizes[i] < pix->plane_fmt[i].sizeimage)
				return -EINVAL;
		return 0;
	}
	*num_planes = pix->num_planes;
	for (i = 0; i < *num_planes; i++)
		sizes[i] = pix->plane_fmt[i].sizeimage;
	return 0;
}

static int apple_jpeg_buf_prepare(struct vb2_buffer *vb)
{
	struct apple_jpeg_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format_mplane *pix = apple_jpeg_format(ctx, vb->type);
	unsigned int i;

	for (i = 0; i < pix->num_planes; i++) {
		if (vb2_plane_size(vb, i) < pix->plane_fmt[i].sizeimage)
			return -EINVAL;
		if (!V4L2_TYPE_IS_OUTPUT(vb->type))
			vb2_set_plane_payload(vb, i, 0);
	}
	return 0;
}

static void apple_jpeg_buf_queue(struct vb2_buffer *vb)
{
	struct apple_jpeg_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static void apple_jpeg_return_buffers(struct apple_jpeg_ctx *ctx,
				      enum v4l2_buf_type type,
				      enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *buf;

	for (;;) {
		buf = V4L2_TYPE_IS_OUTPUT(type) ?
			v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!buf)
			break;
		v4l2_m2m_buf_done(buf, state);
	}
}

static int apple_jpeg_start_streaming(struct vb2_queue *q, unsigned int count)
{
	/* VB2 requires queued buffers back on a failed STREAMON. No MMIO here. */
	apple_jpeg_return_buffers(vb2_get_drv_priv(q), q->type, VB2_BUF_STATE_QUEUED);
	return -EOPNOTSUPP;
}

static void apple_jpeg_stop_streaming(struct vb2_queue *q)
{
	apple_jpeg_return_buffers(vb2_get_drv_priv(q), q->type, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops apple_jpeg_queue_ops = {
	.queue_setup = apple_jpeg_queue_setup,
	.buf_prepare = apple_jpeg_buf_prepare,
	.buf_queue = apple_jpeg_buf_queue,
	.start_streaming = apple_jpeg_start_streaming,
	.stop_streaming = apple_jpeg_stop_streaming,
};

static int apple_jpeg_init_queue(struct apple_jpeg_ctx *ctx, struct vb2_queue *q,
				 enum v4l2_buf_type type)
{
	q->type = type;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = ctx;
	q->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	q->ops = &apple_jpeg_queue_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	q->lock = &ctx->jpeg->lock;
	q->dev = ctx->jpeg->dev;
	return vb2_queue_init(q);
}

static int apple_jpeg_queue_init(void *priv, struct vb2_queue *src,
				 struct vb2_queue *dst)
{
	struct apple_jpeg_ctx *ctx = priv;
	int ret;

	ret = apple_jpeg_init_queue(ctx, src, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (ret)
		return ret;
	return apple_jpeg_init_queue(ctx, dst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
}

static int apple_jpeg_job_ready(void *priv)
{
	return 0;
}

static void apple_jpeg_device_run(void *priv)
{
	struct apple_jpeg_ctx *ctx = priv;

	/* Defensive fallback; STREAMON and job_ready prevent scheduling. */
	apple_jpeg_return_buffers(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
				  VB2_BUF_STATE_ERROR);
	apple_jpeg_return_buffers(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				  VB2_BUF_STATE_ERROR);
	v4l2_m2m_job_finish(ctx->jpeg->m2m_dev, ctx->fh.m2m_ctx);
}

static const struct v4l2_m2m_ops apple_jpeg_m2m_ops = {
	.device_run = apple_jpeg_device_run,
	.job_ready = apple_jpeg_job_ready,
};

static int apple_jpeg_open(struct file *file)
{
	struct apple_jpeg *jpeg = video_drvdata(file);
	struct apple_jpeg_ctx *ctx;
	int ret = 0;

	if (mutex_lock_interruptible(&jpeg->lock))
		return -ERESTARTSYS;
	if (!video_is_registered(&jpeg->vdev)) {
		ret = -ENODEV;
		goto unlock;
	}
	ctx = kzalloc_obj(*ctx);
	if (!ctx) {
		ret = -ENOMEM;
		goto unlock;
	}
	ctx->jpeg = jpeg;
	ctx->output.width = 640;
	ctx->output.height = 480;
	ctx->output.pixelformat = V4L2_PIX_FMT_JPEG;
	apple_jpeg_normalize_format(&ctx->output);
	ctx->capture = ctx->output;
	ctx->capture.pixelformat = V4L2_PIX_FMT_NV12M;
	apple_jpeg_normalize_format(&ctx->capture);
	v4l2_fh_init(&ctx->fh, &jpeg->vdev);
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(jpeg->m2m_dev, ctx, apple_jpeg_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		v4l2_fh_exit(&ctx->fh);
		kfree(ctx);
		goto unlock;
	}
	v4l2_fh_add(&ctx->fh, file);
unlock:
	mutex_unlock(&jpeg->lock);
	return ret;
}

static int apple_jpeg_release(struct file *file)
{
	struct apple_jpeg_ctx *ctx = fh_to_ctx(file->private_data);
	struct apple_jpeg *jpeg = ctx->jpeg;

	mutex_lock(&jpeg->lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	mutex_unlock(&jpeg->lock);
	return 0;
}

static const struct v4l2_file_operations apple_jpeg_fops = {
	.owner = THIS_MODULE,
	.open = apple_jpeg_open,
	.release = apple_jpeg_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static const struct v4l2_ioctl_ops apple_jpeg_ioctl_ops = {
	.vidioc_querycap = apple_jpeg_querycap,
	.vidioc_enum_fmt_vid_cap = apple_jpeg_enum_fmt,
	.vidioc_enum_fmt_vid_out = apple_jpeg_enum_fmt,
	.vidioc_g_fmt_vid_cap_mplane = apple_jpeg_g_fmt,
	.vidioc_g_fmt_vid_out_mplane = apple_jpeg_g_fmt,
	.vidioc_try_fmt_vid_cap_mplane = apple_jpeg_try_fmt,
	.vidioc_try_fmt_vid_out_mplane = apple_jpeg_try_fmt,
	.vidioc_s_fmt_vid_cap_mplane = apple_jpeg_s_fmt,
	.vidioc_s_fmt_vid_out_mplane = apple_jpeg_s_fmt,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
};

static int apple_jpeg_runtime_resume(struct device *dev)
{
	struct apple_jpeg *jpeg = dev_get_drvdata(dev);
	u32 hwrev = readl(jpeg->regs + APPLE_JPEG_HWREV);

	/* Identify before any reset writes; no target reset profile is known. */
	if (!apple_jpeg_hwrev_supported(jpeg->variant, hwrev))
		return dev_err_probe(dev, -ENODEV, "unqualified HWREV %#x\n", hwrev);
	return 0;
}

static int apple_jpeg_runtime_suspend(struct device *dev)
{
	/* This scaffold never starts DMA or changes the engine's register state. */
	return 0;
}

static const struct dev_pm_ops apple_jpeg_pm_ops = {
	RUNTIME_PM_OPS(apple_jpeg_runtime_suspend, apple_jpeg_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static int apple_jpeg_check_dma(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct of_phandle_args args;
	u64 range[2];
	int ret;

	if (of_count_phandle_with_args(dev->of_node, "iommus", "#iommu-cells") != 1)
		return -EINVAL;
	ret = of_parse_phandle_with_args(dev->of_node, "iommus", "#iommu-cells", 0, &args);
	if (ret)
		return ret;
	of_node_put(args.np);
	if (args.args_count != 1 || args.args[0] != 0)
		return -EINVAL;
	if (!domain || (domain->type != IOMMU_DOMAIN_DMA &&
			domain->type != IOMMU_DOMAIN_DMA_FQ))
		return -ENODEV;
	if (of_property_count_u64_elems(dev->of_node, "apple,dma-range") != 2)
		return -EINVAL;
	ret = of_property_read_u64_array(dev->of_node, "apple,dma-range", range, 2);
	if (ret)
		return ret;
	if (range[0] != APPLE_JPEG_DMA_BASE || range[1] != APPLE_JPEG_DMA_SIZE)
		return -EINVAL;
	/* BIT(40) is part of the DART alias, not a 32-bit physical DMA mask. */
	return dma_set_mask_and_coherent(dev, DMA_BIT_MASK(41));
}

static void apple_jpeg_v4l2_release(struct v4l2_device *v4l2_dev)
{
	struct apple_jpeg *jpeg = container_of(v4l2_dev, struct apple_jpeg, v4l2_dev);

	if (jpeg->m2m_dev)
		v4l2_m2m_release(jpeg->m2m_dev);
	put_device(jpeg->dev);
	mutex_destroy(&jpeg->lock);
	kfree(jpeg);
}

static int apple_jpeg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_jpeg *jpeg;
	struct resource *res;
	int ret;

	ret = apple_jpeg_check_dma(dev);
	if (ret)
		return dev_err_probe(dev, ret, "requires a dedicated SID-0 DART window\n");
	if (!dev->pm_domain)
		return dev_err_probe(dev, -ENODEV, "requires a power domain\n");
	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	/* Do not request an IRQ before acknowledgment semantics are qualified. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) != APPLE_JPEG_REG_SIZE)
		return -EINVAL;

	jpeg = kzalloc_obj(*jpeg);
	if (!jpeg)
		return -ENOMEM;
	jpeg->dev = get_device(dev);
	jpeg->variant = of_device_get_match_data(dev);
	mutex_init(&jpeg->lock);
	jpeg->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(jpeg->regs)) {
		ret = PTR_ERR(jpeg->regs);
		goto free_jpeg;
	}
	platform_set_drvdata(pdev, jpeg);
	pm_runtime_set_autosuspend_delay(dev, 100);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto disable_pm;
	ret = pm_runtime_put_sync_suspend(dev);
	if (ret < 0)
		goto disable_pm;

	ret = v4l2_device_register(dev, &jpeg->v4l2_dev);
	if (ret)
		goto disable_pm;
	jpeg->v4l2_dev.release = apple_jpeg_v4l2_release;
	jpeg->m2m_dev = v4l2_m2m_init(&apple_jpeg_m2m_ops);
	if (IS_ERR(jpeg->m2m_dev)) {
		ret = PTR_ERR(jpeg->m2m_dev);
		jpeg->m2m_dev = NULL;
		goto unregister_v4l2;
	}
	strscpy(jpeg->vdev.name, "apple-jpeg", sizeof(jpeg->vdev.name));
	jpeg->vdev.v4l2_dev = &jpeg->v4l2_dev;
	jpeg->vdev.fops = &apple_jpeg_fops;
	jpeg->vdev.ioctl_ops = &apple_jpeg_ioctl_ops;
	jpeg->vdev.release = video_device_release_empty;
	jpeg->vdev.lock = &jpeg->lock;
	jpeg->vdev.vfl_dir = VFL_DIR_M2M;
	jpeg->vdev.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(&jpeg->vdev, jpeg);
	ret = video_register_device(&jpeg->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto unregister_v4l2;
	return 0;

unregister_v4l2:
	v4l2_device_unregister(&jpeg->v4l2_dev);
	pm_runtime_disable(dev);
	pm_runtime_dont_use_autosuspend(dev);
	platform_set_drvdata(pdev, NULL);
	v4l2_device_put(&jpeg->v4l2_dev);
	return ret;
disable_pm:
	pm_runtime_disable(dev);
	pm_runtime_dont_use_autosuspend(dev);
	platform_set_drvdata(pdev, NULL);
free_jpeg:
	put_device(jpeg->dev);
	mutex_destroy(&jpeg->lock);
	kfree(jpeg);
	return ret;
}

static void apple_jpeg_remove(struct platform_device *pdev)
{
	struct apple_jpeg *jpeg = platform_get_drvdata(pdev);

	mutex_lock(&jpeg->lock);
	v4l2_device_disconnect(&jpeg->v4l2_dev);
	video_unregister_device(&jpeg->vdev);
	mutex_unlock(&jpeg->lock);
	v4l2_device_unregister(&jpeg->v4l2_dev);
	pm_runtime_disable(&pdev->dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	platform_set_drvdata(pdev, NULL);
	/* Open files retain the video_device's V4L2 reference until last close. */
	v4l2_device_put(&jpeg->v4l2_dev);
}

static const struct of_device_id apple_jpeg_of_match[] = {
	{ .compatible = "apple,t6041-jpeg", .data = &apple_jpeg_t6041 },
	{ .compatible = "apple,t8140-jpeg", .data = &apple_jpeg_t8140 },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_jpeg_of_match);

static struct platform_driver apple_jpeg_driver = {
	.probe = apple_jpeg_probe,
	.remove = apple_jpeg_remove,
	.driver = {
		.name = "apple-jpeg",
		.of_match_table = apple_jpeg_of_match,
		.pm = pm_ptr(&apple_jpeg_pm_ops),
	},
};
module_platform_driver(apple_jpeg_driver);

MODULE_DESCRIPTION("Apple Silicon JPEG mem2mem scaffold");
MODULE_LICENSE("GPL");
