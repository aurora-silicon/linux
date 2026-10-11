// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2023 Eileen Yoon <eyn@gmx.com> */

#include <linux/math.h>
#include <linux/module.h>

#include <media/media-device.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/videobuf2-dma-sg.h>

#include "isp-cam.h"
#include "isp-cmd.h"
#include "isp-iommu.h"
#include "isp-ipc.h"
#include "isp-fw.h"
#include "isp-v4l2.h"

#define ISP_MIN_FRAMES 2
#define ISP_MAX_PLANES ISP_BUFFER_PLANES
#define ISP_MAX_PIX_FORMATS 2
#define ISP_BUFFER_TIMEOUT msecs_to_jiffies(1500)
#define ISP_STRIDE_ALIGNMENT 64

/* Capture rates offered to userspace, in frames per second */
static const unsigned int isp_frame_rates[] = { 30, 25, 24, 20, 15 };

static bool multiplanar = false;
module_param(multiplanar, bool, 0644);
MODULE_PARM_DESC(multiplanar, "Enable multiplanar API");

static_assert(sizeof(struct isp_buflist_buffer) == ISP_BUFLIST_DESC_SIZE);

struct isp_buflist {
	u64 type;
	u64 num_buffers;
	struct isp_buflist_buffer buffers[];
} __packed;

static void isp_prepare_buffer_descriptor(struct apple_isp *isp,
					  struct isp_buffer_lease *lease,
					  struct isp_buflist_buffer *descriptor)
{
	u64 tag = isp->next_buffer_tag++;

	descriptor->tag = tag;
	descriptor->pad = isp->profile ? tag >> 32 : 0;
	isp_buffer_submit_tag(lease, descriptor, !!isp->profile);
}
static_assert(sizeof(struct isp_buflist) == ISP_BUFLIST_HDR_SIZE);
/* the firmware reads at least ISP_IPC_BUFEXC_STAT_SIZE bytes of a batch */
static_assert(ISP_CMD_AREA_SIZE(0) >= ISP_IPC_BUFEXC_STAT_SIZE);

/*
 * Buffer list tag of capture metadata buffers. The pool itself is set up
 * as CISP_POOL_TYPE_META_CAPTURE; why the tags differ is not known.
 */
#define ISP_BUFLIST_POOL_CAPTURE_META 2

static void isp_capture_failed(struct apple_isp *isp)
{
	WRITE_ONCE(isp->capture_failed, true);
	if (isp->video_registered)
		vb2_queue_error(&isp->vbq);
}

/* buf_lock protects both the descriptor snapshot and its ownership phase. */
static struct isp_buffer_lease *isp_find_returned_lease(struct apple_isp *isp,
							const struct isp_buflist_buffer *report)
{
	struct isp_buffer_match match = {};
	struct isp_buffer *buf;

	for (unsigned int i = 0; i < isp_num_meta(isp); i++) {
		struct isp_surf *surf = isp->meta_surfs[i];

		if (surf)
			isp_buffer_match_candidate(&match, &surf->lease, report);
	}
	for (unsigned int i = 0; i < isp_num_capmeta(isp); i++) {
		struct isp_surf *surf = isp->capmeta_surfs[i];

		if (surf)
			isp_buffer_match_candidate(&match, &surf->lease, report);
	}
	list_for_each_entry(buf, &isp->bufs_submitted, link) {
		isp_buffer_match_candidate(&match, &buf->lease, report);
	}
	return isp_buffer_unique_match(&match);
}

int ipc_bt_handle(struct apple_isp *isp, struct isp_channel *chan)
{
	struct isp_message *req = &chan->req, *rsp = &chan->rsp;
	struct isp_buffer_lease **matches;
	struct isp_buffer *buf, *tmp;
	struct isp_buflist *bl;
	unsigned long flags;
	u64 count;
	int err = -EIO;

	if (!isp->bt_surf || req->arg1 < sizeof(*bl) ||
	    req->arg1 > isp->bt_surf->size)
		goto invalid;

	bl = apple_isp_translate(isp, isp->bt_surf, isp_fw_iova(isp, req->arg0),
				 req->arg1);
	if (!bl)
		goto invalid;
	count = bl->num_buffers;
	if (count > (req->arg1 - sizeof(*bl)) / sizeof(*bl->buffers))
		goto invalid;

	matches = kcalloc(count, sizeof(*matches), GFP_KERNEL);
	if (count && !matches) {
		err = -ENOMEM;
		goto invalid;
	}
	spin_lock_irqsave(&isp->buf_lock, flags);
	/* Validate the whole batch before changing any lease. A malformed or
	 * duplicate record must not partially release an earlier buffer.
	 */
	for (u64 i = 0; i < count; i++) {
		matches[i] = isp_find_returned_lease(isp, &bl->buffers[i]);
		if (!matches[i])
			goto unlock_invalid;
		for (u64 j = 0; j < i; j++)
			if (matches[i] == matches[j])
				goto unlock_invalid;
	}
	for (u64 i = 0; i < count; i++)
		isp_buffer_return(matches[i], &bl->buffers[i]);
	list_for_each_entry_safe(buf, tmp, &isp->bufs_submitted, link) {
		if (buf->lease.owner != ISP_BUFFER_REPORT_PENDING)
			continue;
		buf->rendered = req->arg2 == ISP_IPC_BUFEXC_FLAG_RENDER &&
				!READ_ONCE(isp->capture_failed);
		list_move_tail(&buf->link, &isp->bufs_retiring);
	}
	spin_unlock_irqrestore(&isp->buf_lock, flags);
	kfree(matches);

	rsp->arg0 = req->arg0 | ISP_IPC_FLAG_ACK;
	rsp->arg1 = 0;
	rsp->arg2 = ISP_IPC_BUFEXC_FLAG_ACK;
	return 0;

unlock_invalid:
	spin_unlock_irqrestore(&isp->buf_lock, flags);
	kfree(matches);
invalid:
	dev_err_ratelimited(isp->dev, "%s: invalid or unowned buffer return\n", chan->name);
	isp_capture_failed(isp);
	return err;
}

/* Called only after the reply validity word, doorbell and cursor publication. */
void ipc_bt_acknowledged(struct apple_isp *isp, struct isp_channel *chan)
{
	struct isp_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&isp->buf_lock, flags);
	for (unsigned int i = 0; i < isp_num_meta(isp); i++) {
		struct isp_surf *surf = isp->meta_surfs[i];

		if (surf)
			isp_buffer_acknowledge(&surf->lease);
	}
	for (unsigned int i = 0; i < isp_num_capmeta(isp); i++) {
		struct isp_surf *surf = isp->capmeta_surfs[i];

		if (surf)
			isp_buffer_acknowledge(&surf->lease);
	}
	list_for_each_entry_safe(buf, tmp, &isp->bufs_retiring, link) {
		if (WARN_ON(!isp_buffer_acknowledge(&buf->lease)))
			continue;
		if (test_bit(ISP_STATE_SLEEPING, &isp->state)) {
			list_move_tail(&buf->link, &isp->bufs_pending);
			continue;
		}
		list_del_init(&buf->link);
		buf->vb.vb2_buf.timestamp = ktime_get_ns();
		buf->vb.sequence = isp->sequence++;
		buf->vb.field = V4L2_FIELD_NONE;
		vb2_buffer_done(&buf->vb.vb2_buf,
				buf->rendered ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&isp->buf_lock, flags);
	wake_up_all(&isp->wait);
}

/*
 * With a capture metadata pool, the firmware takes the metadata buffers
 * before CH_START and the capture metadata and capture buffers after it,
 * so the batch before the start (@pre_start) has only the former.
 */
static int isp_submit_buffers(struct apple_isp *isp, bool pre_start)
{
	struct isp_format *fmt = isp_get_current_format(isp);
	struct isp_channel *chan = isp->chan_bh;
	struct isp_message *req = &chan->req;
	struct isp_buffer *buf;
	struct isp_buflist *bl = isp->buflist_surf->virt;
	struct isp_buflist_buffer *bufd;
	unsigned int needed = 0, rendered = 0;
	unsigned long flags;
	size_t offset;
	int err = 0;

	mutex_lock(&chan->lock);
	if (READ_ONCE(isp->capture_failed) || chan->tx_poisoned) {
		err = -EIO;
		goto unlock;
	}
	spin_lock_irqsave(&isp->buf_lock, flags);
	for (unsigned int i = 0; i < isp_num_meta(isp); i++)
		needed += isp->meta_surfs[i]->lease.owner == ISP_BUFFER_HOST;
	if (!(isp_num_capmeta(isp) && pre_start)) {
		for (unsigned int i = 0; i < isp_num_capmeta(isp); i++)
			needed += isp->capmeta_surfs[i]->lease.owner == ISP_BUFFER_HOST;
		list_for_each_entry(buf, &isp->bufs_pending, link) {
			if (rendered++ == isp_max_capture_buffers(isp))
				break;
			needed++;
		}
	}
	/* Leave the final value unused instead of wrapping into an older tag. */
	if (needed > (isp->profile ? U64_MAX : U32_MAX) - isp->next_buffer_tag) {
		err = -EOVERFLOW;
		goto unlock_buffers;
	}
	if (!needed)
		goto unlock_buffers;

	/* A separate batch area is immutable until BUF_H2T acknowledges it.
	 * Control commands must never overwrite an ambiguously submitted batch.
	 */
	memset(bl, 0, isp->buflist_surf->size);
	bl->type = 1;
	bufd = bl->buffers;
	for (unsigned int i = 0; i < isp_num_meta(isp); i++) {
		struct isp_surf *surf = isp->meta_surfs[i];

		if (surf->lease.owner != ISP_BUFFER_HOST)
			continue;
		bufd->num_planes = 1;
		bufd->pool_type = 0;
		bufd->iovas[0] = surf->iova;
		bufd->flags[0] = 0x40000000;
		isp_prepare_buffer_descriptor(isp, &surf->lease, bufd++);
		bl->num_buffers++;
	}
	if (isp_num_capmeta(isp) && pre_start)
		goto send;

	for (unsigned int i = 0; i < isp_num_capmeta(isp); i++) {
		struct isp_surf *surf = isp->capmeta_surfs[i];

		if (surf->lease.owner != ISP_BUFFER_HOST)
			continue;
		bufd->num_planes = 1;
		bufd->pool_type = ISP_BUFLIST_POOL_CAPTURE_META;
		bufd->iovas[0] = surf->iova;
		bufd->flags[0] = 0x40000000;
		isp_prepare_buffer_descriptor(isp, &surf->lease, bufd++);
		bl->num_buffers++;
	}
	for (rendered = 0; rendered < isp_max_capture_buffers(isp); rendered++) {
		buf = list_first_entry_or_null(&isp->bufs_pending,
					       struct isp_buffer, link);
		if (!buf)
			break;
		bufd->num_planes = fmt->num_planes;
		bufd->pool_type = (isp->profile || isp->hw->scl1) ? CISP_POOL_TYPE_RENDERED_SCL1 :
						  CISP_POOL_TYPE_RENDERED;
		offset = 0;
		for (unsigned int i = 0; i < fmt->num_planes; i++) {
			bufd->iovas[i] = buf->memory->surfs[0].iova + offset;
			bufd->flags[i] = 0x40000000;
			offset += fmt->plane_size[i];
		}
		isp_prepare_buffer_descriptor(isp, &buf->lease, bufd++);
		bl->num_buffers++;
		/* Ownership is recorded before the first hardware publication. */
		list_move_tail(&buf->link, &isp->bufs_submitted);
	}

send:
	spin_unlock_irqrestore(&isp->buf_lock, flags);
	req->arg0 = isp->buflist_surf->iova;
	req->arg1 = max_t(u64, ISP_IPC_BUFEXC_STAT_SIZE,
			  sizeof(*bl) + bl->num_buffers * sizeof(*bufd));
	req->arg2 = ISP_IPC_BUFEXC_FLAG_COMMAND;
	err = ipc_chan_send(isp, chan, ISP_BUFFER_TIMEOUT);
	if (err) {
		dev_err(isp->dev, "%s: buffer submission uncertain: %d\n", chan->name, err);
		/* The descriptor area and all leases stay owned until their reports
		 * retire or a qualified full firmware shutdown stops every DMA user.
		 */
		isp_capture_failed(isp);
	}
	goto unlock;

unlock_buffers:
	spin_unlock_irqrestore(&isp->buf_lock, flags);
unlock:
	mutex_unlock(&chan->lock);
	return err;
}

/*
 * Videobuf2 section
 */
static int isp_vb2_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			       unsigned int *num_planes, unsigned int sizes[],
			       struct device *alloc_devs[])
{
	struct apple_isp *isp = vb2_get_drv_priv(vq);
	struct isp_format *fmt = isp_get_current_format(isp);

	/* This is not strictly neccessary but makes it easy to enforce that
	 * at most 16 buffers are submitted at once. ISP on t6001 (FW 12.3)
	 * times out if more buffers are submitted than set in the buffer pool
	 * config before streaming is started.
	 */
	*nbuffers = min_t(unsigned int, *nbuffers, isp_max_capture_buffers(isp));

	if (*num_planes) {
		if (sizes[0] < fmt->total_size)
			return -EINVAL;

		return 0;
	}

	*num_planes = 1;
	sizes[0] = fmt->total_size;

	return 0;
}

static void isp_free_buffer_memory(struct apple_isp *isp,
				   struct isp_buffer_memory *memory)
{
	for (unsigned int i = 0; i < memory->num_planes; i++) {
		if (memory->surfs[i].mm)
			apple_isp_iommu_unmap_sgt(isp, &memory->surfs[i]);
		isp_unpin_buffer_pages(&memory->pins[i]);
	}
	kfree(memory);
}

void apple_isp_release_retained_buffers(struct apple_isp *isp)
{
	struct isp_buffer_memory *memory, *tmp;
	unsigned long flags;
	LIST_HEAD(released);

	if (!READ_ONCE(isp->firmware_quiescent))
		return;
	spin_lock_irqsave(&isp->buf_lock, flags);
	list_splice_init(&isp->retained_buffers, &released);
	spin_unlock_irqrestore(&isp->buf_lock, flags);
	list_for_each_entry_safe(memory, tmp, &released, link) {
		list_del(&memory->link);
		isp_free_buffer_memory(isp, memory);
	}
}

static void isp_vb2_buf_cleanup(struct vb2_buffer *vb)
{
	struct apple_isp *isp = vb2_get_drv_priv(vb->vb2_queue);
	struct isp_buffer *buf = to_isp_buffer(to_vb2_v4l2_buffer(vb));
	struct isp_buffer_memory *memory = buf->memory;
	unsigned long flags;
	bool reclaimable;

	if (!memory)
		return;
	spin_lock_irqsave(&isp->buf_lock, flags);
	list_del_init(&buf->link);
	reclaimable = isp_buffer_reclaimable(&buf->lease,
					     READ_ONCE(isp->firmware_quiescent));
	if (!reclaimable)
		list_add_tail(&memory->link, &isp->retained_buffers);
	buf->memory = NULL;
	spin_unlock_irqrestore(&isp->buf_lock, flags);
	if (reclaimable)
		isp_free_buffer_memory(isp, memory);
	else
		dev_warn(isp->dev, "retaining unretired capture backing and IOVA\n");
}

static int isp_vb2_buf_init(struct vb2_buffer *vb)
{
	struct apple_isp *isp = vb2_get_drv_priv(vb->vb2_queue);
	struct isp_buffer *buf = to_isp_buffer(to_vb2_v4l2_buffer(vb));
	struct isp_buffer_memory *memory;
	int err;

	memory = kzalloc_obj(*memory);
	if (!memory)
		return -ENOMEM;
	INIT_LIST_HEAD(&memory->link);
	INIT_LIST_HEAD(&buf->link);
	buf->memory = memory;
	for (unsigned int i = 0; i < vb->num_planes; i++) {
		struct sg_table *sgt = vb2_dma_sg_plane_desc(vb, i);

		/* Independent page references keep physical backing alive after
		 * vb2 metadata cleanup without changing vb2's num_users count.
		 */
		err = isp_pin_buffer_pages(sgt, &memory->pins[i]);
		if (err)
			goto cleanup;
		memory->num_planes = i + 1;
		err = apple_isp_iommu_map_sgt(isp, &memory->surfs[i], sgt,
					      vb2_plane_size(vb, i));
		if (err)
			goto cleanup;
	}
	return 0;

cleanup:
	buf->memory = NULL;
	isp_free_buffer_memory(isp, memory);
	return err;
}

static int isp_vb2_buf_prepare(struct vb2_buffer *vb)
{
	struct apple_isp *isp = vb2_get_drv_priv(vb->vb2_queue);
	struct isp_format *fmt = isp_get_current_format(isp);

	if (vb2_plane_size(vb, 0) < fmt->total_size)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, fmt->total_size);

	return 0;
}

static void isp_vb2_release_list(struct apple_isp *isp, struct list_head *head,
				 enum vb2_buffer_state state)
{
	struct isp_buffer *buf, *tmp;

	list_for_each_entry_safe(buf, tmp, head, link) {
		list_del_init(&buf->link);
		if (READ_ONCE(isp->firmware_quiescent))
			buf->lease.owner = ISP_BUFFER_HOST;
		/* A vb2 error return is not a DMA-release acknowledgment. An
		 * unretired lease keeps its independent backing and map on cleanup.
		 */
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
}

static void isp_vb2_release_buffers(struct apple_isp *isp,
				    enum vb2_buffer_state state)
{
	unsigned long flags;

	spin_lock_irqsave(&isp->buf_lock, flags);
	isp_vb2_release_list(isp, &isp->bufs_submitted, state);
	isp_vb2_release_list(isp, &isp->bufs_retiring, state);
	isp_vb2_release_list(isp, &isp->bufs_pending, state);
	spin_unlock_irqrestore(&isp->buf_lock, flags);
}

static void isp_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct apple_isp *isp = vb2_get_drv_priv(vb->vb2_queue);
	struct isp_buffer *buf =
		container_of(vb, struct isp_buffer, vb.vb2_buf);
	unsigned long flags;
	bool empty;

	if (READ_ONCE(isp->capture_failed)) {
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		return;
	}

	spin_lock_irqsave(&isp->buf_lock, flags);
	empty = list_empty(&isp->bufs_pending) &&
		list_empty(&isp->bufs_submitted);
	list_add_tail(&buf->link, &isp->bufs_pending);
	spin_unlock_irqrestore(&isp->buf_lock, flags);

	if (test_bit(ISP_STATE_STREAMING, &isp->state) && !empty)
		isp_submit_buffers(isp, false);
}

static int apple_isp_start_streaming(struct apple_isp *isp)
{
	unsigned long flags;
	int err;

	if (READ_ONCE(isp->capture_failed))
		return -EIO;

	err = apple_isp_start_camera(isp);
	if (err) {
		dev_err(isp->dev, "failed to start camera: %d\n", err);
		return err;
	}

	/*
	 * Resident firmware was not restarted, but the channel's pools are
	 * configured again for every stream. Metadata buffers still marked
	 * as submitted to the previous pools would never reach the new ones.
	 */
	if (isp->hw->resident_fw) {
		mutex_lock(&isp->chan_bt->lock);
		spin_lock_irqsave(&isp->buf_lock, flags);
		for (int i = 0; i < isp_num_meta(isp); i++)
			isp->meta_surfs[i]->lease.owner = ISP_BUFFER_HOST;
		for (int i = 0; i < isp_num_capmeta(isp); i++)
			isp->capmeta_surfs[i]->lease.owner = ISP_BUFFER_HOST;
		spin_unlock_irqrestore(&isp->buf_lock, flags);
		mutex_unlock(&isp->chan_bt->lock);
	}

	err = isp_submit_buffers(isp, true);
	if (err) {
		dev_err(isp->dev, "failed to send initial batch: %d\n", err);
		goto stop_camera;
	}

	err = apple_isp_start_capture(isp);
	if (err) {
		dev_err(isp->dev, "failed to start capture: %d\n", err);
		goto stop_camera;
	}

	if (isp_num_capmeta(isp)) {
		err = isp_submit_buffers(isp, false);
		if (err) {
			dev_err(isp->dev, "failed to send the capture batch: %d\n",
				err);
			apple_isp_stop_capture(isp);
			goto stop_camera;
		}
	}

	set_bit(ISP_STATE_STREAMING, &isp->state);

	return 0;

stop_camera:
	if (READ_ONCE(isp->capture_failed))
		apple_isp_firmware_halt(isp);
	else
		apple_isp_stop_camera(isp);
	return err;
}

static bool isp_capture_buffers_retired(struct apple_isp *isp)
{
	unsigned long flags;
	bool retired;

	spin_lock_irqsave(&isp->buf_lock, flags);
	retired = list_empty(&isp->bufs_submitted) && list_empty(&isp->bufs_retiring);
	spin_unlock_irqrestore(&isp->buf_lock, flags);
	return retired;
}

static int apple_isp_stop_streaming(struct apple_isp *isp)
{
	int err;

	if (!test_and_clear_bit(ISP_STATE_STREAMING, &isp->state))
		return READ_ONCE(isp->capture_failed) ? -EIO : 0;

	err = apple_isp_stop_capture(isp);
	if (err)
		isp_capture_failed(isp);
	apple_isp_stop_camera(isp);
	if (READ_ONCE(isp->capture_failed))
		return apple_isp_firmware_halt(isp) ?: -EIO;
	if (READ_ONCE(isp->firmware_quiescent))
		return 0;

	/* Resident firmware may stay alive, but a stop command ACK alone
	 * does not retire user-buffer leases. Wait for their report ACKs.
	 */
	if (!wait_event_timeout(isp->wait, isp_capture_buffers_retired(isp),
				ISP_BUFFER_TIMEOUT)) {
		isp_capture_failed(isp);
		apple_isp_firmware_halt(isp);
		return -ETIME;
	}
	return 0;
}

static int isp_vb2_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct apple_isp *isp = vb2_get_drv_priv(q);
	int err;

	isp->sequence = 0;

	/* The exposure priority is applied when the stream starts. */
	if (isp->exposure_priority)
		v4l2_ctrl_grab(isp->exposure_priority, true);

	err = apple_isp_start_streaming(isp);
	if (err) {
		if (isp->exposure_priority)
			v4l2_ctrl_grab(isp->exposure_priority, false);
		isp_vb2_release_buffers(isp, VB2_BUF_STATE_QUEUED);
	}

	return err;
}

static void isp_vb2_stop_streaming(struct vb2_queue *q)
{
	struct apple_isp *isp = vb2_get_drv_priv(q);

	apple_isp_stop_streaming(isp);
	isp_vb2_release_buffers(isp, VB2_BUF_STATE_ERROR);
	if (isp->exposure_priority)
		v4l2_ctrl_grab(isp->exposure_priority, false);
}

int apple_isp_video_suspend(struct apple_isp *isp)
{
	struct isp_buffer *buf;
	unsigned long flags;
	int err = 0;

	mutex_lock(&isp->video_lock);

	/*
	 * Keep returned buffers queued while sleeping, but only after their
	 * reports have retired. Moving submitted buffers before the return
	 * would hide firmware ownership and prevent completion validation.
	 * isp_vb2_buf_queue() does not submit while the stream is stopped.
	 */
	if (test_bit(ISP_STATE_STREAMING, &isp->state)) {
		set_bit(ISP_STATE_SLEEPING, &isp->state);
		err = apple_isp_stop_streaming(isp);
		/* Nonresident firmware can stop without returning every buffer.
		 * Only a completed full shutdown permits requeuing those leases.
		 */
		if (!err && READ_ONCE(isp->firmware_quiescent)) {
			spin_lock_irqsave(&isp->buf_lock, flags);
			list_for_each_entry(buf, &isp->bufs_submitted, link)
				buf->lease.owner = ISP_BUFFER_HOST;
			list_for_each_entry(buf, &isp->bufs_retiring, link)
				buf->lease.owner = ISP_BUFFER_HOST;
			list_splice_tail_init(&isp->bufs_submitted, &isp->bufs_pending);
			list_splice_tail_init(&isp->bufs_retiring, &isp->bufs_pending);
			spin_unlock_irqrestore(&isp->buf_lock, flags);
		}
	}

	mutex_unlock(&isp->video_lock);

	return err;
}

int apple_isp_video_resume(struct apple_isp *isp)
{
	int err;

	mutex_lock(&isp->video_lock);

	if (test_and_clear_bit(ISP_STATE_SLEEPING, &isp->state)) {
		err = apple_isp_start_streaming(isp);
		if (err) {
			dev_err(isp->dev,
				"failed to restart streaming after resume: %d\n",
				err);
			isp_vb2_release_buffers(isp, VB2_BUF_STATE_ERROR);
			vb2_queue_error(&isp->vbq);
		}
	}

	mutex_unlock(&isp->video_lock);

	return 0;
}

static const struct vb2_ops isp_vb2_ops = {
	.queue_setup = isp_vb2_queue_setup,
	.buf_init = isp_vb2_buf_init,
	.buf_cleanup = isp_vb2_buf_cleanup,
	.buf_prepare = isp_vb2_buf_prepare,
	.buf_queue = isp_vb2_buf_queue,
	.start_streaming = isp_vb2_start_streaming,
	.stop_streaming = isp_vb2_stop_streaming,
};

static int isp_set_preset(struct apple_isp *isp, struct isp_format *fmt,
			  struct isp_preset *preset)
{
	int i;
	size_t total_size;

	if (isp->profile) {
		struct isp_profile_geometry geometry;
		int err = isp_profile_p010_geometry(preset->output_dim.x,
						    preset->output_dim.y, &geometry);

		if (err || preset->index != 5)
			return -EINVAL;
		fmt->preset = preset;
		fmt->num_planes = 2;
		fmt->strides[0] = geometry.stride;
		fmt->strides[1] = geometry.stride;
		fmt->plane_size[0] = geometry.luma_size;
		fmt->plane_size[1] = geometry.chroma_size;
		fmt->total_size = geometry.total_size;
		return 0;
	}
	fmt->preset = preset;

	/* I really fucking hope they all use NV12. */
	fmt->num_planes = 2;
	fmt->strides[0] = ALIGN(preset->output_dim.x, ISP_STRIDE_ALIGNMENT);
	/* UV subsampled interleaved */
	fmt->strides[1] = ALIGN(preset->output_dim.x, ISP_STRIDE_ALIGNMENT);
	fmt->plane_size[0] = fmt->strides[0] * preset->output_dim.y;
	fmt->plane_size[1] = fmt->strides[1] * preset->output_dim.y / 2;

	total_size = 0;
	for (i = 0; i < fmt->num_planes; i++)
		total_size += fmt->plane_size[i];
	fmt->total_size = total_size;

	return 0;
}

static struct isp_preset *isp_select_preset(struct apple_isp *isp, u32 width,
				     u32 height)
{
	struct isp_preset *preset, *best = &isp->presets[0];
	int i, score, best_score = INT_MAX;

	/* Default if no dimensions */
	if (width == 0 || height == 0)
		return &isp->presets[0];

	for (i = 0; i < isp->num_presets; i++) {
		preset = &isp->presets[i];
		score = abs((int)preset->output_dim.x - (int)width) +
		abs((int)preset->output_dim.y - (int)height);
		if (score < best_score) {
			best = preset;
			best_score = score;
		}
	}

	return best;
}

/*
 * V4L2 ioctl section
 */
static int isp_vidioc_querycap(struct file *file, void *priv,
			       struct v4l2_capability *cap)
{
	strscpy(cap->card, APPLE_ISP_CARD_NAME, sizeof(cap->card));
	strscpy(cap->driver, APPLE_ISP_DEVICE_NAME, sizeof(cap->driver));

	return 0;
}

static int isp_vidioc_enum_format(struct file *file, void *fh,
				  struct v4l2_fmtdesc *f)
{
	struct apple_isp *isp = video_drvdata(file);

	if (isp->profile) {
		if (f->index)
			return -EINVAL;
		f->pixelformat = V4L2_PIX_FMT_P010;
		return 0;
	}
	if (f->index >= ISP_MAX_PIX_FORMATS)
		return -EINVAL;

	switch (f->index) {
	case 0:
		f->pixelformat = V4L2_PIX_FMT_NV12;
		break;
	case 1:
		if (!isp->multiplanar)
			return -EINVAL;
		f->pixelformat = V4L2_PIX_FMT_NV12M;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int isp_vidioc_enum_framesizes(struct file *file, void *fh,
				      struct v4l2_frmsizeenum *f)
{
	struct apple_isp *isp = video_drvdata(file);

	if (f->index >= isp->num_presets)
		return -EINVAL;

	if (isp->profile ? f->pixel_format != V4L2_PIX_FMT_P010 :
	    f->pixel_format != V4L2_PIX_FMT_NV12 && f->pixel_format != V4L2_PIX_FMT_NV12M)
		return -EINVAL;

	f->discrete.width = isp->presets[f->index].output_dim.x;
	f->discrete.height = isp->presets[f->index].output_dim.y;
	f->type = V4L2_FRMSIZE_TYPE_DISCRETE;

	return 0;
}

static int isp_vidioc_enum_frameintervals(struct file *file, void *fh,
					  struct v4l2_frmivalenum *interval)
{
	struct apple_isp *isp = video_drvdata(file);
	int i;

	/* The recorded AE settings are not a measured frame-rate guarantee. */
	if (isp->profile)
		return -EINVAL;
	if (interval->index >= ARRAY_SIZE(isp_frame_rates))
		return -EINVAL;

	if (interval->pixel_format != V4L2_PIX_FMT_NV12 &&
	    interval->pixel_format != V4L2_PIX_FMT_NV12M)
		return -EINVAL;

	for (i = 0; i < isp->num_presets; i++) {
		if (isp->presets[i].output_dim.x == interval->width &&
		    isp->presets[i].output_dim.y == interval->height)
			break;
	}
	if (i == isp->num_presets)
		return -EINVAL;

	interval->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	interval->discrete.numerator = 1;
	interval->discrete.denominator = isp_frame_rates[interval->index];
	return 0;
}

/* Returns the supported rate whose frame interval is closest to @tpf. */
static unsigned int isp_closest_frame_rate(const struct v4l2_fract *tpf)
{
	unsigned int best = isp_frame_rates[0];

	/*
	 * |n/d - 1/r| = |n * r - d| / (d * r), so the closest rate has the
	 * smallest |n * r - d| / r; compare those by cross-multiplying.
	 */
	for (int i = 1; i < ARRAY_SIZE(isp_frame_rates); i++) {
		unsigned int rate = isp_frame_rates[i];
		u64 err = abs_diff((u64)tpf->numerator * rate,
				   (u64)tpf->denominator);
		u64 best_err = abs_diff((u64)tpf->numerator * best,
					(u64)tpf->denominator);

		if (err * best < best_err * rate)
			best = rate;
	}

	return best;
}

static inline void isp_get_sp_pix_format(struct apple_isp *isp,
					 struct v4l2_format *f,
					 struct isp_format *fmt)
{
	f->fmt.pix.width = fmt->preset->output_dim.x;
	f->fmt.pix.height = fmt->preset->output_dim.y;
	f->fmt.pix.bytesperline = fmt->strides[0];
	f->fmt.pix.sizeimage = fmt->total_size;

	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.pixelformat = isp->profile ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
	f->fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_709;
	f->fmt.pix.xfer_func = V4L2_XFER_FUNC_709;
}

static inline void isp_get_mp_pix_format(struct apple_isp *isp,
					 struct v4l2_format *f,
					 struct isp_format *fmt)
{
	f->fmt.pix_mp.width = fmt->preset->output_dim.x;
	f->fmt.pix_mp.height = fmt->preset->output_dim.y;
	f->fmt.pix_mp.num_planes = fmt->num_planes;
	for (int i = 0; i < fmt->num_planes; i++) {
		f->fmt.pix_mp.plane_fmt[i].sizeimage = fmt->plane_size[i];
		f->fmt.pix_mp.plane_fmt[i].bytesperline = fmt->strides[i];
	}

	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	f->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
	f->fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	f->fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_709;
	f->fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_709;
}

static int isp_vidioc_get_format(struct file *file, void *fh,
				 struct v4l2_format *f)
{
	struct apple_isp *isp = video_drvdata(file);
	struct isp_format *fmt = isp_get_current_format(isp);

	isp_get_sp_pix_format(isp, f, fmt);

	return 0;
}

static int isp_vidioc_set_format(struct file *file, void *fh,
				 struct v4l2_format *f)
{
	struct apple_isp *isp = video_drvdata(file);
	struct isp_format *fmt = isp_get_current_format(isp);
	struct isp_preset *preset;
	int err;

	preset = isp_select_preset(isp, f->fmt.pix.width, f->fmt.pix.height);
	err = isp_set_preset(isp, fmt, preset);
	if (err)
		return err;

	isp_get_sp_pix_format(isp, f, fmt);

	isp->vbq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	return 0;
}

static int isp_vidioc_try_format(struct file *file, void *fh,
				 struct v4l2_format *f)
{
	struct apple_isp *isp = video_drvdata(file);
	struct isp_format fmt = *isp_get_current_format(isp);
	struct isp_preset *preset;
	int err;

	preset = isp_select_preset(isp, f->fmt.pix.width, f->fmt.pix.height);
	err = isp_set_preset(isp, &fmt, preset);
	if (err)
		return err;

	isp_get_sp_pix_format(isp, f, &fmt);

	return 0;
}

static int isp_vidioc_get_format_mplane(struct file *file, void *fh,
					struct v4l2_format *f)
{
	struct apple_isp *isp = video_drvdata(file);
	struct isp_format *fmt = isp_get_current_format(isp);

	if (!isp->multiplanar)
		return -ENOTTY;

	isp_get_mp_pix_format(isp, f, fmt);

	return 0;
}

static int isp_vidioc_set_format_mplane(struct file *file, void *fh,
					struct v4l2_format *f)
{
	struct apple_isp *isp = video_drvdata(file);
	struct isp_format *fmt = isp_get_current_format(isp);
	struct isp_preset *preset;
	int err;

	if (!isp->multiplanar)
		return -ENOTTY;

	preset = isp_select_preset(isp, f->fmt.pix_mp.width,
				   f->fmt.pix_mp.height);
	err = isp_set_preset(isp, fmt, preset);
	if (err)
		return err;

	isp_get_mp_pix_format(isp, f, fmt);

	isp->vbq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	return 0;
}

static int isp_vidioc_try_format_mplane(struct file *file, void *fh,
					struct v4l2_format *f)
{
	struct apple_isp *isp = video_drvdata(file);
	struct isp_format fmt = *isp_get_current_format(isp);
	struct isp_preset *preset;
	int err;

	if (!isp->multiplanar)
		return -ENOTTY;

	preset = isp_select_preset(isp, f->fmt.pix_mp.width,
				   f->fmt.pix_mp.height);
	err = isp_set_preset(isp, &fmt, preset);
	if (err)
		return err;

	isp_get_mp_pix_format(isp, f, &fmt);

	return 0;
}

static int isp_vidioc_enum_input(struct file *file, void *fh,
				 struct v4l2_input *inp)
{
	if (inp->index)
		return -EINVAL;

	strscpy(inp->name, APPLE_ISP_DEVICE_NAME, sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;

	return 0;
}

static int isp_vidioc_get_input(struct file *file, void *fh, unsigned int *i)
{
	*i = 0;

	return 0;
}

static int isp_vidioc_set_input(struct file *file, void *fh, unsigned int i)
{
	if (i)
		return -EINVAL;

	return 0;
}

static int isp_vidioc_get_param(struct file *file, void *fh,
				struct v4l2_streamparm *a)
{
	struct apple_isp *isp = video_drvdata(file);

	if (isp->profile)
		return -EINVAL;

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
	    (!isp->multiplanar ||
	     a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE))
		return -EINVAL;

	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.readbuffers = ISP_MIN_FRAMES;
	a->parm.capture.timeperframe.numerator = 1;
	a->parm.capture.timeperframe.denominator = isp->frame_rate;

	return 0;
}

static int isp_vidioc_set_param(struct file *file, void *fh,
				struct v4l2_streamparm *a)
{
	struct apple_isp *isp = video_drvdata(file);
	struct v4l2_fract *tpf = &a->parm.capture.timeperframe;

	if (isp->profile)
		return -EINVAL;

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
	    (!isp->multiplanar ||
	     a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE))
		return -EINVAL;

	/* The rate is configured when the stream starts. */
	if (vb2_is_streaming(&isp->vbq))
		return -EBUSY;

	/* A zero interval selects the nominal rate. */
	if (!tpf->numerator || !tpf->denominator)
		isp->frame_rate = ISP_FRAME_RATE_DEFAULT;
	else
		isp->frame_rate = isp_closest_frame_rate(tpf);

	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.readbuffers = ISP_MIN_FRAMES;
	tpf->numerator = 1;
	tpf->denominator = isp->frame_rate;

	return 0;
}

static const struct v4l2_ioctl_ops isp_v4l2_ioctl_ops = {
	.vidioc_querycap = isp_vidioc_querycap,

	.vidioc_enum_fmt_vid_cap = isp_vidioc_enum_format,
	.vidioc_g_fmt_vid_cap = isp_vidioc_get_format,
	.vidioc_s_fmt_vid_cap = isp_vidioc_set_format,
	.vidioc_try_fmt_vid_cap = isp_vidioc_try_format,
	.vidioc_g_fmt_vid_cap_mplane = isp_vidioc_get_format_mplane,
	.vidioc_s_fmt_vid_cap_mplane = isp_vidioc_set_format_mplane,
	.vidioc_try_fmt_vid_cap_mplane = isp_vidioc_try_format_mplane,

	.vidioc_enum_framesizes = isp_vidioc_enum_framesizes,
	.vidioc_enum_frameintervals = isp_vidioc_enum_frameintervals,
	.vidioc_enum_input = isp_vidioc_enum_input,
	.vidioc_g_input = isp_vidioc_get_input,
	.vidioc_s_input = isp_vidioc_set_input,
	.vidioc_g_parm = isp_vidioc_get_param,
	.vidioc_s_parm = isp_vidioc_set_param,

	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,

	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations isp_v4l2_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static const struct media_device_ops isp_media_device_ops = {
	.link_notify = v4l2_pipeline_link_notify,
};

static void isp_free_meta_surface(struct apple_isp *isp, struct isp_surf **slot)
{
	struct isp_surf *surf;
	unsigned long flags;

	spin_lock_irqsave(&isp->buf_lock, flags);
	surf = *slot;
	if (surf && !isp_buffer_reclaimable(&surf->lease,
					    READ_ONCE(isp->firmware_quiescent)))
		surf = NULL;
	else
		*slot = NULL;
	spin_unlock_irqrestore(&isp->buf_lock, flags);
	if (surf)
		isp_free_surface(isp, surf);
}

static void isp_free_meta_surfaces(struct apple_isp *isp)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(isp->capmeta_surfs); i++)
		isp_free_meta_surface(isp, &isp->capmeta_surfs[i]);
	for (unsigned int i = 0; i < isp_num_meta(isp); i++)
		isp_free_meta_surface(isp, &isp->meta_surfs[i]);
}

int apple_isp_setup_video(struct apple_isp *isp)
{
	struct video_device *vdev = &isp->vdev;
	struct vb2_queue *vbq = &isp->vbq;
	struct isp_format *fmt = isp_get_current_format(isp);
	int err;

	err = isp_set_preset(isp, fmt, &isp->presets[0]);
	if (err) {
		dev_err(isp->dev, "failed to set default preset: %d\n", err);
		return err;
	}

	isp->frame_rate = ISP_FRAME_RATE_DEFAULT;

	for (int i = 0; i < isp_num_meta(isp); i++) {
		isp->meta_surfs[i] =
			isp_alloc_surface_vmap(isp, isp->profile ?
				isp->profile->meta_size : isp->hw->meta_size);
		if (!isp->meta_surfs[i]) {
			isp_err(isp, "failed to alloc meta surface\n");
			err = -ENOMEM;
			goto surf_cleanup;
		}
	}

	/* Only the firmware reads and writes these, so they need no vmap. */
	for (int i = 0; i < isp_num_capmeta(isp); i++) {
		isp->capmeta_surfs[i] =
			isp_alloc_surface(isp, isp->hw->capture_meta_size);
		if (!isp->capmeta_surfs[i]) {
			isp_err(isp, "failed to alloc capture meta surface\n");
			err = -ENOMEM;
			goto surf_cleanup;
		}
	}

	media_device_init(&isp->mdev);
	isp->v4l2_dev.mdev = &isp->mdev;
	isp->mdev.ops = &isp_media_device_ops;
	isp->mdev.dev = isp->dev;
	strscpy(isp->mdev.model, APPLE_ISP_DEVICE_NAME,
		sizeof(isp->mdev.model));

	err = media_device_register(&isp->mdev);
	if (err) {
		dev_err(isp->dev, "failed to register media device: %d\n", err);
		goto media_cleanup;
	}

	isp->multiplanar = multiplanar && !isp->profile;

	/*
	 * Auto exposure is always on. By default it holds the frame rate, as
	 * V4L2 specifies; with exposure priority it may slow down in low light.
	 */
	v4l2_ctrl_handler_init(&isp->ctrl_handler, 1);
	if (!isp->profile)
		isp->exposure_priority =
			v4l2_ctrl_new_std(&isp->ctrl_handler, NULL,
					  V4L2_CID_EXPOSURE_AUTO_PRIORITY, 0, 1, 1, 0);
	if (isp->ctrl_handler.error) {
		err = isp->ctrl_handler.error;
		dev_err(isp->dev, "failed to create controls: %d\n", err);
		goto ctrl_free;
	}
	isp->v4l2_dev.ctrl_handler = &isp->ctrl_handler;

	err = v4l2_device_register(isp->dev, &isp->v4l2_dev);
	if (err) {
		dev_err(isp->dev, "failed to register v4l2 device: %d\n", err);
		goto ctrl_free;
	}

	vbq->drv_priv = isp;
	vbq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vbq->io_modes = VB2_MMAP;
	vbq->dev = isp->dev;
	vbq->ops = &isp_vb2_ops;
	vbq->mem_ops = &vb2_dma_sg_memops;
	vbq->buf_struct_size = sizeof(struct isp_buffer);
	vbq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vbq->min_queued_buffers = ISP_MIN_FRAMES;
	vbq->max_num_buffers = isp_max_capture_buffers(isp);
	vbq->lock = &isp->video_lock;

	err = vb2_queue_init(vbq);
	if (err) {
		dev_err(isp->dev, "failed to init vb2 queue: %d\n", err);
		goto v4l2_unregister;
	}

	vdev->queue = vbq;
	vdev->fops = &isp_v4l2_fops;
	vdev->ioctl_ops = &isp_v4l2_ioctl_ops;
	vdev->device_caps = V4L2_BUF_TYPE_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	if (isp->multiplanar)
		vdev->device_caps |= V4L2_CAP_VIDEO_CAPTURE_MPLANE;
	vdev->v4l2_dev = &isp->v4l2_dev;
	vdev->vfl_type = VFL_TYPE_VIDEO;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->release = video_device_release_empty;
	vdev->lock = &isp->video_lock;
	strscpy(vdev->name, APPLE_ISP_DEVICE_NAME, sizeof(vdev->name));
	video_set_drvdata(vdev, isp);

	err = video_register_device(vdev, VFL_TYPE_VIDEO, 0);
	if (err) {
		dev_err(isp->dev, "failed to register video device: %d\n", err);
		goto v4l2_unregister;
	}

	isp->video_registered = true;
	return 0;

v4l2_unregister:
	v4l2_device_unregister(&isp->v4l2_dev);
ctrl_free:
	v4l2_ctrl_handler_free(&isp->ctrl_handler);
	media_device_unregister(&isp->mdev);
media_cleanup:
	media_device_cleanup(&isp->mdev);
surf_cleanup:
	isp_free_meta_surfaces(isp);

	return err;
}

void apple_isp_remove_video(struct apple_isp *isp)
{
	isp->video_registered = false;
	vb2_video_unregister_device(&isp->vdev);
	v4l2_device_unregister(&isp->v4l2_dev);
	v4l2_ctrl_handler_free(&isp->ctrl_handler);
	media_device_unregister(&isp->mdev);
	media_device_cleanup(&isp->mdev);
}

/* After apple_isp_remove_video() and with the firmware stopped */
void apple_isp_free_video(struct apple_isp *isp)
{
	isp_free_meta_surfaces(isp);
	apple_isp_release_retained_buffers(isp);
}
