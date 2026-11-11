// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple AVE MCTF: V4L2 memory-to-memory temporal noise filter
 *
 * One NV12 1920x1080 picture in on OUTPUT, the filtered picture out on
 * CAPTURE, with the OUTPUT timestamp. The firmware keeps the temporal
 * history itself; a session runs from STREAMON to STREAMOFF, so the next
 * STREAMON starts again with no history. The strength control applies at
 * STREAMON; the per-frame level control (-1: the strength) is
 * written into every frame.
 *
 * Sessions share the encoder's firmware, session table, power and recovery
 * with the encoder video device.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>

#include "ave.h"
#include "ave-mctf.h"

#define AVE_MCTF_DRIVER		"apple-ave"
#define AVE_MCTF_CARD		"Apple AVE MCTF"
#define AVE_MCTF_STRIDE		AVE_MCTF_WIDTH
/*
 * The pictures are 1080 lines; the buffer has room for 1088 like the
 * firmware's own surfaces, so a 16-line aligned write stays inside it.
 */
#define AVE_MCTF_SIZEIMAGE	(AVE_MCTF_STRIDE * ALIGN(AVE_MCTF_HEIGHT, 16) * 3 / 2)
#define AVE_MCTF_POLL_MS	2

static void ave_mctf_fill(struct v4l2_pix_format_mplane *f)
{
	f->pixelformat = V4L2_PIX_FMT_NV12;
	f->width = AVE_MCTF_WIDTH;
	f->height = AVE_MCTF_HEIGHT;
	f->num_planes = 1;
	f->plane_fmt[0].bytesperline = AVE_MCTF_STRIDE;
	f->plane_fmt[0].sizeimage = AVE_MCTF_SIZEIMAGE;
	f->field = V4L2_FIELD_NONE;
	if (f->colorspace == V4L2_COLORSPACE_DEFAULT) {
		f->colorspace = V4L2_COLORSPACE_REC709;
		f->xfer_func = V4L2_XFER_FUNC_DEFAULT;
		f->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
		f->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	}
	memset(f->plane_fmt[0].reserved, 0, sizeof(f->plane_fmt[0].reserved));
	memset(f->reserved, 0, sizeof(f->reserved));
}

static int ave_mctf_querycap(struct file *file, void *priv,
			     struct v4l2_capability *cap)
{
	strscpy(cap->driver, AVE_MCTF_DRIVER, sizeof(cap->driver));
	strscpy(cap->card, AVE_MCTF_CARD, sizeof(cap->card));
	return 0;
}

static int ave_mctf_enum_fmt(struct file *file, void *priv,
			     struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_NV12;
	return 0;
}

static int ave_mctf_enum_framesizes(struct file *file, void *priv,
				    struct v4l2_frmsizeenum *fs)
{
	if (fs->index || fs->pixel_format != V4L2_PIX_FMT_NV12)
		return -EINVAL;
	fs->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fs->discrete.width = AVE_MCTF_WIDTH;
	fs->discrete.height = AVE_MCTF_HEIGHT;
	return 0;
}

static struct v4l2_pix_format_mplane *ave_mctf_fmt(struct ave_ctx *ctx,
						   u32 type)
{
	return V4L2_TYPE_IS_OUTPUT(type) ? &ctx->src_fmt : &ctx->dst_fmt;
}

static int ave_mctf_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	f->fmt.pix_mp = *ave_mctf_fmt(ave_file_to_ctx(file), f->type);
	return 0;
}

static int ave_mctf_try_fmt(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	ave_mctf_fill(&f->fmt.pix_mp);
	return 0;
}

/* Only NV12 at 1920x1080 is filtered; anything else is refused. */
static int ave_mctf_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct ave_ctx *ctx = ave_file_to_ctx(file);
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);

	if (vb2_is_busy(vq))
		return -EBUSY;
	if (pix->pixelformat != V4L2_PIX_FMT_NV12 ||
	    pix->width != AVE_MCTF_WIDTH || pix->height != AVE_MCTF_HEIGHT)
		return -EINVAL;
	ave_mctf_fill(pix);
	*ave_mctf_fmt(ctx, f->type) = *pix;
	return 0;
}

static const struct v4l2_ioctl_ops ave_mctf_ioctl_ops = {
	.vidioc_querycap = ave_mctf_querycap,
	.vidioc_enum_fmt_vid_cap = ave_mctf_enum_fmt,
	.vidioc_enum_fmt_vid_out = ave_mctf_enum_fmt,
	.vidioc_enum_framesizes = ave_mctf_enum_framesizes,
	.vidioc_g_fmt_vid_cap_mplane = ave_mctf_g_fmt,
	.vidioc_g_fmt_vid_out_mplane = ave_mctf_g_fmt,
	.vidioc_try_fmt_vid_cap_mplane = ave_mctf_try_fmt,
	.vidioc_try_fmt_vid_out_mplane = ave_mctf_try_fmt,
	.vidioc_s_fmt_vid_cap_mplane = ave_mctf_s_fmt,
	.vidioc_s_fmt_vid_out_mplane = ave_mctf_s_fmt,

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

static int ave_mctf_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ave_ctx *ctx = container_of(ctrl->handler, struct ave_ctx, hdl);

	switch (ctrl->id) {
	case V4L2_CID_APPLE_AVE_MCTF_LEVEL:
		WRITE_ONCE(ctx->mctf_level, ctrl->val);
		return 0;
	case V4L2_CID_APPLE_AVE_MCTF_STRENGTH:
		WRITE_ONCE(ctx->mctf_strength, ctrl->val);
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops ave_mctf_ctrl_ops = {
	.s_ctrl = ave_mctf_s_ctrl,
};

static const struct v4l2_ctrl_config ave_mctf_level = {
	.ops = &ave_mctf_ctrl_ops,
	.id = V4L2_CID_APPLE_AVE_MCTF_LEVEL,
	.name = "MCTF Level",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = AVE_MCTF_LEVEL_MIN,
	.max = AVE_MCTF_LEVEL_MAX,
	.step = 1,
	.def = AVE_MCTF_LEVEL_DEFAULT,
};

static const struct v4l2_ctrl_config ave_mctf_strength = {
	.ops = &ave_mctf_ctrl_ops,
	.id = V4L2_CID_APPLE_AVE_MCTF_STRENGTH,
	.name = "MCTF Strength",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = 0,
	.max = AVE_MCTF_STRENGTH_MAX,
	.step = 1,
	.def = AVE_MCTF_STRENGTH_DEFAULT,
};

static int ave_mctf_init_ctrls(struct ave_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->hdl;

	v4l2_ctrl_handler_init(hdl, 2);
	v4l2_ctrl_new_custom(hdl, &ave_mctf_level, NULL);
	v4l2_ctrl_new_custom(hdl, &ave_mctf_strength, NULL);
	if (hdl->error) {
		int ret = hdl->error;

		v4l2_ctrl_handler_free(hdl);
		return ret;
	}
	ctx->fh.ctrl_handler = hdl;
	return v4l2_ctrl_handler_setup(hdl);
}

/* Both pictures stay mapped for the firmware like encoder input does. */
static int ave_mctf_queue_init(void *priv, struct vb2_queue *src,
			       struct vb2_queue *dst)
{
	struct ave_ctx *ctx = priv;
	struct vb2_queue *q[] = { src, dst };
	int ret;

	src->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	dst->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	for (unsigned int i = 0; i < ARRAY_SIZE(q); i++) {
		q[i]->io_modes = VB2_MMAP | VB2_DMABUF;
		q[i]->drv_priv = ctx;
		q[i]->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
		q[i]->ops = &ave_qops;
		q[i]->mem_ops = &ave_vb2_memops;
		q[i]->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		q[i]->lock = &ctx->ave->vdev_lock;
		q[i]->dev = ctx->ave->dev;
		ret = vb2_queue_init(q[i]);
		if (ret)
			return ret;
	}
	return 0;
}

/* Jobs: deliver filtered frames, then fill the free slots. */

static int ave_mctf_job_ready(void *priv)
{
	struct ave_ctx *ctx = priv;
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;

	if (READ_ONCE(ctx->failed))
		return 0;
	return atomic_read(&ctx->ready) ||
	       (v4l2_m2m_num_src_bufs_ready(m2m) &&
		v4l2_m2m_num_dst_bufs_ready(m2m));
}

/* Log every distinct metadata byte of one plane of the CPU surface copy */
static void ave_mctf_log_meta(struct ave_ctx *ctx, const char *plane, bool chroma)
{
	const struct ave_session *sess = ctx->sess;
	unsigned int distinct = 0;
	u16 hist[256];
	char line[128];
	int len = 0;

	ave_cmp420_meta_hist(&sess->mctf_geom, sess->mctf_surf, chroma, hist);
	line[0] = 0;
	for (unsigned int v = 0; v < ARRAY_SIZE(hist); v++) {
		if (!hist[v])
			continue;
		distinct++;
		if (len < (int)sizeof(line) - 16)
			len += scnprintf(line + len, sizeof(line) - len, " %02x:%u",
					 v, hist[v]);
	}
	dev_dbg(ctx->ave->dev, "MCTF frame %u %s metadata, %u values:%s%s\n",
		ctx->dst_sequence, plane, distinct, line,
		len < (int)sizeof(line) - 16 ? "" : " ...");
}

/*
 * Read the output surface of @slot into @dst as NV12. Raw and solid tiles
 * are exact; other tile modes are not decoded and show the marker colour.
 * The debug log counts the tile kinds of every frame.
 */
static int ave_mctf_get_picture(struct ave_ctx *ctx, unsigned int slot,
				struct vb2_v4l2_buffer *dst)
{
	struct ave_session *sess = ctx->sess;
	const struct ave_cmp420_geom *g = &sess->mctf_geom;
	size_t luma = (size_t)sess->p.stride * sess->p.height;
	const u8 *from = sess->mctf_out[slot].cpu;
	u8 *to = vb2_plane_vaddr(&dst->vb2_buf, 0);
	struct ave_cmp420_stats ys, cs;

	if (!from || !to || !sess->mctf_surf || !sess->mctf_lin)
		return -EFAULT;
	if (vb2_plane_size(&dst->vb2_buf, 0) < luma * 3 / 2)
		return -EINVAL;
	memcpy(sess->mctf_surf, from, g->bytes);
	if (READ_ONCE(ctx->ave->mctf_capture) && ctx->ave->mctf_surf_dump[0] &&
	    ctx->ave->mctf_surf_dump[1] && sess->mctf_in[sess->slot[slot].mctf_input].cpu) {
		memcpy(ctx->ave->mctf_surf_dump[0],
		       sess->mctf_in[sess->slot[slot].mctf_input].cpu, g->bytes);
		memcpy(ctx->ave->mctf_surf_dump[1], sess->mctf_surf, g->bytes);
		dev_dbg(ctx->ave->dev, "MCTF frame %u: surfaces captured\n",
			ctx->dst_sequence);
	}
	ave_cmp420_unpack(g, sess->mctf_surf, sess->mctf_lin,
			  sess->mctf_lin + luma, sess->p.stride, &ys, &cs);
	memcpy(to, sess->mctf_lin, luma * 3 / 2);
	dev_dbg(ctx->ave->dev,
		"MCTF frame %u slot %u tiles raw/solid/mixed/other: Y %u/%u/%u/%u CbCr %u/%u/%u/%u; blocks raw/solid/other: Y %u/%u/%u CbCr %u/%u/%u\n",
		ctx->dst_sequence, slot,
		ys.tiles_raw, ys.tiles_solid, ys.tiles_mixed, ys.tiles_other,
		cs.tiles_raw, cs.tiles_solid, cs.tiles_mixed, cs.tiles_other,
		ys.raw, ys.solid, ys.other, cs.raw, cs.solid, cs.other);
	ave_mctf_log_meta(ctx, "Y", false);
	ave_mctf_log_meta(ctx, "CbCr", true);
	return 0;
}

/* Hand answered frames back, in answer order. Caller holds hw_lock. */
static void ave_mctf_deliver(struct ave_ctx *ctx)
{
	struct ave_session *sess = ctx->sess;

	while (sess && sess->done_count) {
		unsigned int slot = sess->done_fifo[sess->done_head];
		struct ave_slot *s = &sess->slot[slot];
		enum vb2_buffer_state state = VB2_BUF_STATE_DONE;

		if (s->status) {
			dev_warn_ratelimited(ctx->ave->dev,
					     "MCTF frame %u: firmware status %#x\n",
					     ctx->dst_sequence, s->status);
			state = VB2_BUF_STATE_ERROR;
		}
		if (state == VB2_BUF_STATE_DONE && ave_mctf_get_picture(ctx, slot, s->dst))
			state = VB2_BUF_STATE_ERROR;
		vb2_set_plane_payload(&s->dst->vb2_buf, 0,
				      state == VB2_BUF_STATE_DONE ?
				      ctx->dst_fmt.plane_fmt[0].sizeimage : 0);
		v4l2_m2m_buf_copy_metadata(s->src, s->dst);
		s->dst->sequence = ctx->dst_sequence++;

		sess->done_head = (sess->done_head + 1) % AVE_MAX_SLOTS;
		sess->done_count--;
		atomic_dec(&ctx->ready);
		s->done = false;
		ave_vb2_put(s->input_mem);
		s->input_mem = NULL;
		ave_vb2_put(s->output_mem);
		s->output_mem = NULL;
		v4l2_m2m_buf_done(s->src, state);
		v4l2_m2m_buf_done(s->dst, state);
		s->src = NULL;
		s->dst = NULL;
	}
}

/* Send OUTPUT frames while slots and CAPTURE buffers allow. Caller holds hw_lock. */
static int ave_mctf_submit(struct ave_ctx *ctx)
{
	struct v4l2_m2m_ctx *m2m = ctx->fh.m2m_ctx;
	struct ave_session *sess = ctx->sess;
	int slot, ret = 0;

	while (!ret && v4l2_m2m_num_src_bufs_ready(m2m) &&
	       v4l2_m2m_num_dst_bufs_ready(m2m) &&
	       (slot = ave_sess_free_slot(sess)) >= 0) {
		struct vb2_v4l2_buffer *src = v4l2_m2m_src_buf_remove(m2m);
		struct vb2_v4l2_buffer *dst = v4l2_m2m_dst_buf_remove(m2m);

		src->sequence = ctx->src_sequence++;
		ret = ave_mctf_cmd_process(ctx, slot, src, dst);
		if (ret) {
			v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
			v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
		}
	}
	return ret;
}

static void ave_mctf_run_work(struct work_struct *work)
{
	struct ave_ctx *ctx = container_of(work, struct ave_ctx, run_work);
	struct ave_dev *ave = ctx->ave;
	bool poll = false;
	int ret = 0;

	if (READ_ONCE(ctx->failed))
		goto finish;
	/* After system sleep the session reopens: a new Start, no history. */
	if (ctx->sess && ctx->sess->resume)
		ret = ave_session_reopen(ctx);

	mutex_lock(&ave->hw_lock);
	if (!ret && (!ctx->sess || ave_session_index(ave, ctx) < 0)) {
		dev_err(ave->dev, "MCTF session lost the firmware\n");
		ret = -EIO;
	}
	if (!ret) {
		ave_mctf_deliver(ctx);
		ret = ave_mctf_submit(ctx);
	}
	if (!ret)
		ret = ave_ipc_pump(ave, 0, NULL, NULL);
	if (!ret) {
		ave_mctf_deliver(ctx);
		poll = ctx->sess->busy;
	}
	mutex_unlock(&ave->hw_lock);

	if (ret)
		ave_job_fail(ctx, ret);
	else if (poll)
		mod_delayed_work(ave->wq, &ctx->poll_work,
				 msecs_to_jiffies(AVE_MCTF_POLL_MS));
finish:
	pm_runtime_mark_last_busy(ave->dev);
	v4l2_m2m_job_finish(ave->mctf_m2m, ctx->fh.m2m_ctx);
}

/* Collect answers while frames are outstanding and no job runs. */
static void ave_mctf_poll_work(struct work_struct *work)
{
	struct ave_ctx *ctx = container_of(to_delayed_work(work),
					   struct ave_ctx, poll_work);
	struct ave_dev *ave = ctx->ave;
	bool again = false, fail = false;

	mutex_lock(&ave->hw_lock);
	if (ctx->sess && ave->fw_state == AVE_FW_BROKEN) {
		fail = true;
	} else if (ctx->sess && ave_session_index(ave, ctx) >= 0) {
		fail = ave_ipc_pump(ave, 0, NULL, NULL) != 0;
		if (!fail && ave_sess_stalled(ctx->sess)) {
			dev_err(ave->dev, "%u MCTF frames unanswered; firmware is stuck\n",
				ctx->sess->busy);
			ave_ipc_dump(ave, "MCTF frame timeout");
			ave_fw_mark_broken(ave, -ETIMEDOUT, "MCTF frame timeout");
			fail = true;
		}
		again = !fail && ctx->sess->busy;
	} else if (ctx->sess && ctx->sess->busy) {
		fail = true;
	}
	mutex_unlock(&ave->hw_lock);

	if (fail)
		ave_job_fail(ctx, -EIO);
	else if (atomic_read(&ctx->ready))
		v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
	if (again)
		queue_delayed_work(ave->wq, &ctx->poll_work,
				   msecs_to_jiffies(AVE_MCTF_POLL_MS));
}

static void ave_mctf_device_run(void *priv)
{
	struct ave_ctx *ctx = priv;

	queue_work(ctx->ave->wq, &ctx->run_work);
}

static const struct v4l2_m2m_ops ave_mctf_m2m_ops = {
	.device_run = ave_mctf_device_run,
	.job_ready = ave_mctf_job_ready,
};

static int ave_mctf_open(struct file *file)
{
	struct ave_dev *ave = video_drvdata(file);
	struct ave_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	ctx->ave = ave;
	ctx->mctf = true;
	ctx->mctf_level = AVE_MCTF_LEVEL_DEFAULT;
	ctx->mctf_strength = AVE_MCTF_STRENGTH_DEFAULT;
	spin_lock_init(&ctx->queue_lock);
	INIT_WORK(&ctx->run_work, ave_mctf_run_work);
	INIT_DELAYED_WORK(&ctx->poll_work, ave_mctf_poll_work);
	atomic_set(&ctx->ready, 0);
	ave_mctf_fill(&ctx->src_fmt);
	ave_mctf_fill(&ctx->dst_fmt);
	ctx->timeperframe = (struct v4l2_fract){ 1, 30 };

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ret = ave_mctf_init_ctrls(ctx);
	if (ret)
		goto err_fh;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(ave->mctf_m2m, ctx,
					    ave_mctf_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_ctrls;
	}
	/* Filtered frames are delivered with no new buffer queued. */
	v4l2_m2m_set_src_buffered(ctx->fh.m2m_ctx, true);
	v4l2_m2m_set_dst_buffered(ctx->fh.m2m_ctx, true);
	v4l2_fh_add(&ctx->fh, file);
	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&ctx->hdl);
err_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int ave_mctf_release(struct file *file)
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

static const struct v4l2_file_operations ave_mctf_fops = {
	.owner = THIS_MODULE,
	.open = ave_mctf_open,
	.release = ave_mctf_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

/* The MCTF device exists only where its command layout is qualified. */
int ave_mctf_register(struct ave_dev *ave)
{
	struct video_device *vdev = &ave->mctf_vdev;
	int ret;

	if (ave_mctf_check_build(ave))
		return 0;
	ave->mctf_m2m = v4l2_m2m_init(&ave_mctf_m2m_ops);
	if (IS_ERR(ave->mctf_m2m))
		return PTR_ERR(ave->mctf_m2m);

	strscpy(vdev->name, "apple-ave-mctf", sizeof(vdev->name));
	vdev->fops = &ave_mctf_fops;
	vdev->ioctl_ops = &ave_mctf_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &ave->vdev_lock;
	vdev->v4l2_dev = &ave->v4l2_dev;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(vdev, ave);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_m2m_release(ave->mctf_m2m);
		return ret;
	}
	ave->mctf_registered = true;
	dev_info(ave->dev, "MCTF filter at %s\n", video_device_node_name(vdev));
	return 0;
}

void ave_mctf_unregister(struct ave_dev *ave)
{
	if (!ave->mctf_registered)
		return;
	video_unregister_device(&ave->mctf_vdev);
	v4l2_m2m_release(ave->mctf_m2m);
	ave->mctf_registered = false;
}
