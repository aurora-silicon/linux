// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2021 Alyssa Rosenzweig */

#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/soc/apple/rtkit.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "dcp.h"
#include "dcp-internal.h"
#include "iomfb.h"
#include "iomfb_internal.h"
#include "parser.h"
#include "trace.h"

static int dcp_tx_offset(enum dcp_context_id id)
{
	switch (id) {
	case DCP_CONTEXT_CB:
	case DCP_CONTEXT_CMD:
		return 0x00000;
	case DCP_CONTEXT_OOBCB:
	case DCP_CONTEXT_OOBCMD:
		return 0x08000;
	default:
		return -EINVAL;
	}
}

static int dcp_channel_offset(enum dcp_context_id id)
{
	switch (id) {
	case DCP_CONTEXT_ASYNC:
		return 0x40000;
	case DCP_CONTEXT_OOBASYNC:
		return 0x48000;
	case DCP_CONTEXT_CB:
		return 0x60000;
	case DCP_CONTEXT_OOBCB:
		return 0x68000;
	default:
		return dcp_tx_offset(id);
	}
}

static inline u64 dcpep_set_shmem(u64 dart_va)
{
	return FIELD_PREP(IOMFB_MESSAGE_TYPE, IOMFB_MESSAGE_TYPE_SET_SHMEM) |
	       FIELD_PREP(IOMFB_SHMEM_FLAG, IOMFB_SHMEM_FLAG_VALUE) |
	       FIELD_PREP(IOMFB_SHMEM_DVA, dart_va);
}

static inline u64 dcpep_msg(enum dcp_context_id id, u32 length, u16 offset)
{
	return FIELD_PREP(IOMFB_MESSAGE_TYPE, IOMFB_MESSAGE_TYPE_MSG) |
		FIELD_PREP(IOMFB_MSG_CONTEXT, id) |
		FIELD_PREP(IOMFB_MSG_OFFSET, offset) |
		FIELD_PREP(IOMFB_MSG_LENGTH, length);
}

static inline u64 dcpep_ack(enum dcp_context_id id)
{
	return dcpep_msg(id, 0, 0) | IOMFB_MSG_ACK;
}

/*
 * A channel is busy if we have sent a message that has yet to be
 * acked. The driver must not sent a message to a busy channel.
 */
static bool dcp_channel_busy(struct dcp_channel *ch)
{
	return (ch->depth != 0);
}

/*
 * Get the context ID passed to the DCP for a command we push. The rule is
 * simple: callback contexts are used when replying to the DCP, command
 * contexts are used otherwise. That corresponds to a non/zero call stack
 * depth. This rule frees the caller from tracking the call context manually.
 */
static enum dcp_context_id dcp_call_context(struct apple_dcp *dcp, bool oob)
{
	u8 depth = oob ? dcp->ch_oobcmd.depth : dcp->ch_cmd.depth;

	if (depth)
		return oob ? DCP_CONTEXT_OOBCB : DCP_CONTEXT_CB;
	else
		return oob ? DCP_CONTEXT_OOBCMD : DCP_CONTEXT_CMD;
}

/* Get a channel for a context */
static struct dcp_channel *dcp_get_channel(struct apple_dcp *dcp,
					   enum dcp_context_id context)
{
	switch (context) {
	case DCP_CONTEXT_CB:
		return &dcp->ch_cb;
	case DCP_CONTEXT_CMD:
		return &dcp->ch_cmd;
	case DCP_CONTEXT_OOBCB:
		return &dcp->ch_oobcb;
	case DCP_CONTEXT_OOBCMD:
		return &dcp->ch_oobcmd;
	case DCP_CONTEXT_ASYNC:
		return &dcp->ch_async;
	case DCP_CONTEXT_OOBASYNC:
		return &dcp->ch_oobasync;
	default:
		return NULL;
	}
}

/* Get the start of a packet: after the end of the previous packet */
static u16 dcp_packet_start(struct dcp_channel *ch, u8 depth)
{
	if (depth > 0)
		return ch->end[depth - 1];
	else
		return 0;
}

/* Pushes and pops the depth of the call stack with safety checks */
static u8 dcp_push_depth(u8 *depth)
{
	u8 ret = (*depth)++;

	WARN_ON(ret >= DCP_MAX_CALL_DEPTH);
	return ret;
}

static u8 dcp_pop_depth(u8 *depth)
{
	WARN_ON((*depth) == 0);

	return --(*depth);
}

/* Older firmware and the H17G method profile retain their existing transport. */
static bool iomfb_uses_queue(struct apple_dcp *dcp)
{
	return dcp->fw_compat == DCP_FIRMWARE_H17P &&
	       dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17G;
}

static bool iomfb_channels_idle(struct apple_dcp *dcp)
{
	return !dcp->ch_cmd.depth && !dcp->ch_cb.depth &&
	       !dcp->ch_oobcmd.depth && !dcp->ch_oobcb.depth &&
	       !dcp->ch_async.depth && !dcp->ch_oobasync.depth;
}

enum iomfb_opaque_x_state {
	IOMFB_OPAQUE_X_WAITING,
	IOMFB_OPAQUE_X_NEEDED,
	IOMFB_OPAQUE_X_QUEUED,
	IOMFB_OPAQUE_X_READY,
};

struct iomfb_opaque_x_transaction {
	struct iomfb_transaction transaction;
};

static void iomfb_opaque_x_start(struct apple_dcp *dcp,
				 struct iomfb_transaction *transaction);
static void iomfb_opaque_x_release(struct iomfb_transaction *transaction);

static int iomfb_enqueue_opaque_x(struct apple_dcp *dcp)
{
	struct iomfb_opaque_x_transaction *opaque;

	lockdep_assert_held(&dcp->iomfb.lock);
	if (atomic_read(&dcp->iomfb.opaque_x_state) != IOMFB_OPAQUE_X_NEEDED)
		return 0;
	if (dcp->iomfb.queued >= 32)
		return -EBUSY;
	opaque = kzalloc_obj(*opaque);
	if (!opaque)
		return -ENOMEM;
	if (atomic_cmpxchg(&dcp->iomfb.opaque_x_state, IOMFB_OPAQUE_X_NEEDED,
			   IOMFB_OPAQUE_X_QUEUED) != IOMFB_OPAQUE_X_NEEDED) {
		kfree(opaque);
		return 0;
	}

	opaque->transaction.start = iomfb_opaque_x_start;
	opaque->transaction.release = iomfb_opaque_x_release;
	dcp->iomfb.queued++;
	list_add(&opaque->transaction.link, &dcp->iomfb.pending);
	return 0;
}

static void iomfb_discard_pending(struct apple_dcp *dcp)
{
	struct iomfb_transaction *transaction, *next;

	lockdep_assert_held(&dcp->iomfb.lock);
	list_for_each_entry_safe(transaction, next, &dcp->iomfb.pending, link) {
		list_del(&transaction->link);
		dcp->iomfb.queued--;
		if (transaction->brightness_only)
			dcp->iomfb.backlight_queued = false;
		transaction->release(transaction);
	}
}

static void iomfb_queue_advance(struct apple_dcp *dcp)
{
	struct iomfb_transaction *transaction = dcp->iomfb.active;

	lockdep_assert_held(&dcp->iomfb.lock);
	if (READ_ONCE(dcp->crashed) || dcp->iomfb.stopped) {
		iomfb_discard_pending(dcp);
		/* The active operation may still be visible to firmware. */
		return;
	}
	if (!iomfb_channels_idle(dcp) || dcp->present_state_h17p.pending)
		return;

	if (transaction) {
		dcp->iomfb.active = NULL;
		cancel_delayed_work(&dcp->iomfb.timeout);
		if (transaction->brightness_only)
			dcp->iomfb.backlight_queued = false;
		/* A new level requested during a successful present stays pending. */
		if (dcp_backlight_pending(dcp)) {
			if (transaction->completed) {
				schedule_work(&dcp->bl_update_wq);
			} else if (transaction->backlight_failed) {
				unsigned int delay = dcp_backlight_retry_delay(dcp);

				if (delay)
					mod_delayed_work(system_wq, &dcp->iomfb.backlight_retry,
							 msecs_to_jiffies(delay));
				else
					dev_warn_ratelimited(dcp->dev,
							     "backlight retry limit reached\n");
			}
		}
		transaction->release(transaction);
	}
	if (iomfb_enqueue_opaque_x(dcp)) {
		WRITE_ONCE(dcp->crashed, true);
		iomfb_discard_pending(dcp);
		schedule_work(&dcp->vblank_wq);
		return;
	}
	if (!list_empty(&dcp->iomfb.pending))
		schedule_work(&dcp->iomfb.work);
}

static void iomfb_queue_work(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(work, struct apple_dcp, iomfb.work);
	struct iomfb_transaction *transaction;

	mutex_lock(&dcp->iomfb.lock);
	if (READ_ONCE(dcp->crashed) || dcp->iomfb.stopped) {
		iomfb_discard_pending(dcp);
		goto unlock;
	}
	if (dcp->iomfb.active || !iomfb_channels_idle(dcp) ||
	    dcp->present_state_h17p.pending || list_empty(&dcp->iomfb.pending))
		goto unlock;

	transaction = list_first_entry(&dcp->iomfb.pending,
				       struct iomfb_transaction, link);
	list_del(&transaction->link);
	dcp->iomfb.queued--;
	dcp->iomfb.active = transaction;
	WRITE_ONCE(dcp->iomfb.owner, current);
	dcp->iomfb.deadline = jiffies + msecs_to_jiffies(10000);
	mod_delayed_work(system_wq, &dcp->iomfb.timeout, msecs_to_jiffies(10000));
	transaction->start(dcp, transaction);
	WRITE_ONCE(dcp->iomfb.owner, NULL);
	iomfb_queue_advance(dcp);
unlock:
	mutex_unlock(&dcp->iomfb.lock);
}

static void iomfb_queue_timeout(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work),
					  struct apple_dcp, iomfb.timeout);

	mutex_lock(&dcp->iomfb.lock);
	if (dcp->iomfb.active && time_before(jiffies, dcp->iomfb.deadline)) {
		mod_delayed_work(system_wq, &dcp->iomfb.timeout,
				 dcp->iomfb.deadline - jiffies);
	} else if (dcp->iomfb.active) {
		WRITE_ONCE(dcp->crashed, true);
		dev_err(dcp->dev, "IOMFB transaction timed out\n");
		iomfb_discard_pending(dcp);
		schedule_work(&dcp->vblank_wq);
	}
	mutex_unlock(&dcp->iomfb.lock);
}

static void iomfb_opaque_x_complete(struct apple_dcp *dcp, void *out,
				    void *cookie)
{
	u32 status = out ? *(u32 *)out : ~0U;

	(void)cookie;

	if (status) {
		dev_err(dcp->dev, "opaque X property failed: %u\n", status);
		WRITE_ONCE(dcp->crashed, true);
		schedule_work(&dcp->vblank_wq);
		return;
	}

	atomic_set(&dcp->iomfb.opaque_x_state, IOMFB_OPAQUE_X_READY);
}

static void iomfb_opaque_x_start(struct apple_dcp *dcp,
				 struct iomfb_transaction *transaction)
{
	(void)transaction;
	iomfb_apply_opaque_x_h17p(dcp, iomfb_opaque_x_complete, NULL);
}

static void iomfb_opaque_x_release(struct iomfb_transaction *transaction)
{
	kfree(container_of(transaction, struct iomfb_opaque_x_transaction,
			   transaction));
}

void iomfb_opaque_x_reset_h17p(struct apple_dcp *dcp)
{
	atomic_set(&dcp->iomfb.opaque_x_state, IOMFB_OPAQUE_X_WAITING);
}

static void iomfb_backlight_retry(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work), struct apple_dcp,
					  iomfb.backlight_retry);

	if (!READ_ONCE(dcp->crashed) && !READ_ONCE(dcp->iomfb.stopped) &&
	    dcp_backlight_pending(dcp))
		schedule_work(&dcp->bl_update_wq);
}

void iomfb_queue_init(struct apple_dcp *dcp)
{
	mutex_init(&dcp->iomfb.lock);
	INIT_LIST_HEAD(&dcp->iomfb.pending);
	INIT_WORK(&dcp->iomfb.work, iomfb_queue_work);
	INIT_DELAYED_WORK(&dcp->iomfb.timeout, iomfb_queue_timeout);
	INIT_DELAYED_WORK(&dcp->iomfb.backlight_retry, iomfb_backlight_retry);
	atomic_set(&dcp->iomfb.opaque_x_state, IOMFB_OPAQUE_X_WAITING);
}

void iomfb_queue_stop(struct apple_dcp *dcp)
{
	if (!iomfb_uses_queue(dcp))
		return;

	mutex_lock(&dcp->iomfb.lock);
	dcp->iomfb.stopped = true;
	iomfb_discard_pending(dcp);
	mutex_unlock(&dcp->iomfb.lock);
	cancel_work_sync(&dcp->iomfb.work);
	cancel_delayed_work_sync(&dcp->iomfb.timeout);
	cancel_delayed_work_sync(&dcp->iomfb.backlight_retry);
}

int iomfb_queue(struct apple_dcp *dcp, struct iomfb_transaction *transaction)
{
	int ret = 0;

	mutex_lock(&dcp->iomfb.lock);
	if (READ_ONCE(dcp->crashed) || dcp->iomfb.stopped) {
		ret = -EIO;
	} else if (transaction->brightness_only && dcp->iomfb.backlight_queued) {
		ret = -EALREADY;
	} else if (dcp->iomfb.queued >= 32) {
		ret = -EBUSY;
	} else {
		if (transaction->brightness_only)
			dcp->iomfb.backlight_queued = true;
		dcp->iomfb.queued++;
		list_add_tail(&transaction->link, &dcp->iomfb.pending);
		schedule_work(&dcp->iomfb.work);
	}
	mutex_unlock(&dcp->iomfb.lock);
	return ret;
}

struct iomfb_command {
	struct iomfb_transaction transaction;
	struct dcp_method_entry method;
	dcp_callback_t callback;
	void *cookie;
	u32 in_len;
	u32 out_len;
	bool oob;
	u8 data[];
};

static void iomfb_command_start(struct apple_dcp *dcp,
				struct iomfb_transaction *transaction)
{
	struct iomfb_command *command = container_of(transaction,
						    struct iomfb_command, transaction);

	dcp_push(dcp, command->oob, &command->method, command->in_len,
		 command->out_len, command->data, command->callback, command->cookie);
}

static void iomfb_command_release(struct iomfb_transaction *transaction)
{
	kfree(container_of(transaction, struct iomfb_command, transaction));
}

static int iomfb_queue_command(struct apple_dcp *dcp, bool oob,
			       const struct dcp_method_entry *method,
			       u32 in_len, u32 out_len, void *data,
			       dcp_callback_t callback, void *cookie)
{
	struct iomfb_command *command;
	int ret;

	if ((u64)sizeof(struct dcp_packet_header) + in_len + out_len > 0x8000)
		return -EMSGSIZE;
	command = kzalloc(struct_size(command, data, in_len), GFP_KERNEL);
	if (!command)
		return -ENOMEM;
	command->transaction.start = iomfb_command_start;
	command->transaction.release = iomfb_command_release;
	command->method = *method;
	command->callback = callback;
	command->cookie = cookie;
	command->in_len = in_len;
	command->out_len = out_len;
	command->oob = oob;
	if (in_len)
		memcpy(command->data, data, in_len);
	ret = iomfb_queue(dcp, &command->transaction);
	if (ret)
		iomfb_command_release(&command->transaction);
	return ret;
}

/* Call a DCP function given by a tag */
void dcp_push(struct apple_dcp *dcp, bool oob, const struct dcp_method_entry *call,
		     u32 in_len, u32 out_len, void *data, dcp_callback_t cb,
		     void *cookie)
{
	struct dcp_method_entry resolved = *call;

	/* Reply chains stay on the serialized receiver; callers enqueue copies. */
	if (iomfb_uses_queue(dcp) && READ_ONCE(dcp->iomfb.owner) != current) {
		if (iomfb_queue_command(dcp, oob, call, in_len, out_len,
					data, cb, cookie))
			WRITE_ONCE(dcp->crashed, true);
		return;
	}

	if (dcp->fw_compat == DCP_FIRMWARE_H17P && READ_ONCE(dcp->crashed))
		return;

	if (dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17G &&
	    call->tag_h17g[0])
		memcpy(resolved.tag, call->tag_h17g, sizeof(resolved.tag));
	call = &resolved;

	enum dcp_context_id context = dcp_call_context(dcp, oob);
	struct dcp_channel *ch = dcp_get_channel(dcp, context);

	struct dcp_packet_header header = {
		.in_len = in_len,
		.out_len = out_len,

		/* Tag is reversed due to endianness of the fourcc */
		.tag[0] = call->tag[3],
		.tag[1] = call->tag[2],
		.tag[2] = call->tag[1],
		.tag[3] = call->tag[0],
	};

	u8 depth;
	u16 offset;

	if (iomfb_uses_queue(dcp) &&
	    (!ch || ch->depth >= DCP_MAX_CALL_DEPTH ||
	     (u64)sizeof(header) + in_len + out_len > 0x8000 ||
	     dcp_packet_start(ch, ch->depth) >
		0x8000 - ALIGN(sizeof(header) + in_len + out_len,
			       DCP_PACKET_ALIGNMENT))) {
		dev_err(dcp->dev, "invalid IOMFB command envelope\n");
		WRITE_ONCE(dcp->crashed, true);
		return;
	}
	depth = dcp_push_depth(&ch->depth);
	offset = dcp_packet_start(ch, depth);

	void *out = dcp->shmem + dcp_tx_offset(context) + offset;
	void *out_data = out + sizeof(header);
	size_t data_len = sizeof(header) + in_len + out_len;

	memcpy(out, &header, sizeof(header));

	if (in_len > 0)
		memcpy(out_data, data, in_len);
	/* An unwritten status must not look like a successful response. */
	if (out_len)
		memset(out_data + in_len, 0xff, out_len);

	trace_iomfb_push(dcp, call, context, offset, depth);

	ch->callbacks[depth] = cb;
	ch->cookies[depth] = cookie;
	ch->output[depth] = out + sizeof(header) + in_len;
	ch->in_len[depth] = in_len;
	ch->out_len[depth] = out_len;
	ch->end[depth] = offset + ALIGN(data_len, DCP_PACKET_ALIGNMENT);

	dcp_send_message(dcp, IOMFB_ENDPOINT,
			 dcpep_msg(context, data_len, offset));
}

/* Parse a callback tag "D123" into the ID 123. Returns -EINVAL on failure. */
int dcp_parse_tag(char tag[4])
{
	u32 d[3];
	int i;

	if (tag[3] != 'D')
		return -EINVAL;

	for (i = 0; i < 3; ++i) {
		d[i] = (u32)(tag[i] - '0');

		if (d[i] > 9)
			return -EINVAL;
	}

	return d[0] + (d[1] * 10) + (d[2] * 100);
}

/* Ack a callback from the DCP */
void dcp_ack(struct apple_dcp *dcp, enum dcp_context_id context)
{
	struct dcp_channel *ch = dcp_get_channel(dcp, context);

	dcp_pop_depth(&ch->depth);
	dcp_send_message(dcp, IOMFB_ENDPOINT,
			 dcpep_ack(context));
}

/*
 * Helper to send a DRM hotplug event. The DCP is accessed from a single
 * (RTKit) thread. To handle hotplug callbacks, we need to call
 * drm_kms_helper_hotplug_event, which does an atomic commit (via DCP) and
 * waits for vblank (a DCP callback). That means we deadlock if we call from
 * the RTKit thread! Instead, move the call to another thread via a workqueue.
 */
static int dcp_retrain_active_crtc(struct apple_connector *connector)
{
	struct drm_device *dev = connector->base.dev;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_crtc *crtc;
	int ret;

	DRM_MODESET_LOCK_ALL_BEGIN(dev, ctx, 0, ret);

	crtc = connector->base.state ? connector->base.state->crtc : NULL;
	if (crtc && crtc->state && crtc->state->active)
		ret = drm_atomic_helper_reset_crtc(crtc, &ctx);
	else
		ret = 0;

	DRM_MODESET_LOCK_ALL_END(dev, ctx, ret);

	return ret;
}

void dcp_retrain_oob(struct apple_connector *connector)
{
	struct apple_dcp *dcp = platform_get_drvdata(connector->dcp);

	if (!READ_ONCE(connector->connected) || !READ_ONCE(dcp->valid_mode))
		return;

	/*
	 * Bringing up another high-speed Type-C route can disturb an active DPTX
	 * stream without changing its HPD state.  Invalidate the IOMFB mode and
	 * use the normal hotplug worker to replay the active CRTC from process
	 * context; sending a synthetic disconnect would tear down the connector.
	 */
	WRITE_ONCE(dcp->valid_mode, false);
	schedule_work(&connector->hotplug_wq);
}

void dcp_hotplug(struct work_struct *work)
{
	struct apple_connector *connector;
	struct apple_dcp *dcp;
	int ret;

	connector = container_of(work, struct apple_connector, hotplug_wq);

	dcp = platform_get_drvdata(connector->dcp);
	dev_info(dcp->dev, "%s() connected:%d valid_mode:%d nr_modes:%u\n", __func__,
		 connector->connected, dcp->valid_mode, dcp->nr_modes);

	if (!connector->connected) {
		drm_edid_free(connector->drm_edid);
		connector->drm_edid = NULL;
	}

	/*
	 * DCP defers link training until we set a display mode. But we set
	 * display modes from atomic_flush, so userspace needs to trigger a
	 * flush, or the CRTC gets no signal.
	 */
	if (connector->base.state && !dcp->valid_mode && connector->connected) {
		drm_connector_set_link_status_property(&connector->base,
						       DRM_MODE_LINK_STATUS_BAD);

		/*
		 * A short Type-C route interruption can leave the DRM CRTC active
		 * while DCP has discarded its display mode.  Userspace is then free
		 * to keep submitting plane-only commits, none of which retrains the
		 * link.  Re-apply the active CRTC state once the DPTX link-config
		 * callback has completed.
		 */
		ret = dcp_retrain_active_crtc(connector);
		if (ret)
			dev_warn(dcp->dev,
				 "failed to retrain active CRTC after hotplug: %d\n",
				 ret);
	}

	drm_kms_helper_connector_hotplug_event(&connector->base);
}

static void dcpep_handle_cb(struct apple_dcp *dcp, enum dcp_context_id context,
			    void *data, u32 length, u16 offset)
{
	struct device *dev = dcp->dev;
	struct dcp_packet_header *hdr = data;
	void *in, *out;
	int tag;
	struct dcp_channel *ch = dcp_get_channel(dcp, context);
	u8 depth;
	bool handled;

	if (dcp->fw_compat == DCP_FIRMWARE_H17P &&
	    (length < sizeof(*hdr) || !ch || ch->depth >= DCP_MAX_CALL_DEPTH ||
	     offset >= 0x8000 || length > 0x8000 - offset)) {
		dev_err(dev, "invalid IOMFB callback envelope\n");
		dcp->crashed = true;
		return;
	}

	tag = dcp_parse_tag(hdr->tag);
	handled = tag >= 0 && tag < IOMFB_MAX_CB && dcp->cb_handlers &&
		  dcp->cb_handlers[tag];
	if (dcp->fw_compat == DCP_FIRMWARE_H17P &&
	    (tag < 0 || tag >= IOMFB_MAX_CB ||
	     (u64)sizeof(*hdr) + hdr->in_len + hdr->out_len != length ||
	     (dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17G &&
	      handled &&
	      !iomfb_validate_callback_h17p(tag, hdr->in_len, hdr->out_len)))) {
		dev_err(dev, "unqualified IOMFB callback %d (%u, %u)\n",
			tag, hdr->in_len, hdr->out_len);
		dcp->crashed = true;
		return;
	}

	in = data + sizeof(*hdr);
	out = in + hdr->in_len;

	if (!handled) {
		if (!iomfb_uses_queue(dcp) ||
		    !test_and_set_bit(tag, dcp->unknown_callbacks))
			dev_warn(dev, "received unknown callback %c%c%c%c\n",
				 hdr->tag[3], hdr->tag[2], hdr->tag[1], hdr->tag[0]);
		/*
		 * Leaving a callback unanswered wedges the coprocessor: it
		 * waits for the ack forever and the outer call never returns.
		 * On H17P answer it with a zeroed output instead.
		 */
		if (dcp->fw_compat == DCP_FIRMWARE_H17P) {
			if (hdr->out_len)
				memset(out, 0, hdr->out_len);
			depth = dcp_push_depth(&ch->depth);
			ch->output[depth] = out;
			ch->end[depth] = offset +
					 ALIGN(length, DCP_PACKET_ALIGNMENT);
			dcp_ack(dcp, context);
		}
		return;
	}

	// TODO: verify that in_len and out_len match our prototypes
	// for now just clear the out data to have at least consistent results
	if (hdr->out_len)
		memset(out, 0, hdr->out_len);

	depth = dcp_push_depth(&ch->depth);
	ch->output[depth] = out;
	ch->end[depth] = offset + ALIGN(length, DCP_PACKET_ALIGNMENT);

	if (dcp->cb_handlers[tag](dcp, tag, out, in))
		dcp_ack(dcp, context);
}

static void dcpep_handle_ack(struct apple_dcp *dcp, enum dcp_context_id context,
			     void *data, u32 length)
{
	struct dcp_packet_header *header = data;
	struct dcp_channel *ch = dcp_get_channel(dcp, context);
	bool h17p = dcp->fw_compat == DCP_FIRMWARE_H17P;
	void *cookie, *out;
	dcp_callback_t cb;

	if (!ch) {
		dev_warn(dcp->dev, "ignoring ack on context %X\n", context);
		return;
	}

	if (h17p && !ch->depth) {
		dev_warn(dcp->dev, "ignoring ack on idle context %X\n", context);
		return;
	}

	dcp_pop_depth(&ch->depth);

	cb = ch->callbacks[ch->depth];
	cookie = ch->cookies[ch->depth];

	ch->callbacks[ch->depth] = NULL;
	ch->cookies[ch->depth] = NULL;

	if (h17p) {
		/*
		 * H17P acks carry no payload and offset zero, including for
		 * nested calls, so the output cannot be located from the ack
		 * message.  It is in the original AP command record, whose
		 * address dcp_push() saved.
		 */
		out = ch->output[ch->depth];
		ch->output[ch->depth] = NULL;
	} else {
		if (header->in_len != ch->in_len[ch->depth] ||
		    header->out_len != ch->out_len[ch->depth]) {
			dev_err(dcp->dev, "invalid command response lengths\n");
			dcp->crashed = true;
			return;
		}
		out = data + sizeof(*header) + header->in_len;
	}

	if (cb)
		cb(dcp, out, cookie);
}

static void dcpep_got_msg(struct apple_dcp *dcp, u64 message)
{
	enum dcp_context_id ctx_id;
	u16 offset;
	u32 length;
	int channel_offset;
	void *data;

	ctx_id = FIELD_GET(IOMFB_MSG_CONTEXT, message);
	offset = FIELD_GET(IOMFB_MSG_OFFSET, message);
	length = FIELD_GET(IOMFB_MSG_LENGTH, message);

	channel_offset = dcp_channel_offset(ctx_id);

	if (channel_offset < 0) {
		dev_warn(dcp->dev, "invalid context received %u\n", ctx_id);
		return;
	}

	data = dcp->shmem + channel_offset + offset;

	if (FIELD_GET(IOMFB_MSG_ACK, message))
		dcpep_handle_ack(dcp, ctx_id, data, length);
	else
		dcpep_handle_cb(dcp, ctx_id, data, length, offset);
}

int dcp_get_modes(struct drm_connector *connector)
{
	struct apple_connector *apple_connector = to_apple_connector(connector);
	struct platform_device *pdev = apple_connector->dcp;
	struct apple_dcp *dcp;

	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;
	u16 min_vfreq = 0, max_vfreq = 0;
	bool vrr_capable = false;
	int i;

	/* A Type-C port has no pipeline while the fabric is moving it. */
	if (!pdev)
		return 0;
	dcp = platform_get_drvdata(pdev);

	for (i = 0; i < dcp->nr_modes; ++i) {
		if (dcp->modes[i].vrr) {
			u16 lo = dcp->modes[i].min_vrr >> 16;
			u16 hi = dcp->modes[i].max_vrr >> 16;

			if (!min_vfreq || lo < min_vfreq)
				min_vfreq = lo;
			if (hi > max_vfreq)
				max_vfreq = hi;
		}
		vrr_capable |= dcp->modes[i].vrr;
		mode = drm_mode_duplicate(dev, &dcp->modes[i].mode);

		if (!mode) {
			dev_err(dev->dev, "Failed to duplicate display mode\n");
			return 0;
		}

		drm_mode_probed_add(connector, mode);
	}
	drm_connector_set_vrr_capable_property(connector, vrr_capable);

	/* H17P sends no EPIC commands; see afk_send_epic(). */
	if (dcp->nr_modes && dcp->dcpavserv.enabled &&
	    dcp->fw_compat != DCP_FIRMWARE_H17P &&
	    !apple_connector->drm_edid) {
		const struct drm_edid *edid;
		edid = dcpavserv_copy_edid(dcp->dcpavserv.service);
		if (IS_ERR_OR_NULL(edid)) {
			dev_info(dcp->dev, "copy_edid failed: %pe\n", edid);
		} else {
			drm_edid_free(apple_connector->drm_edid);
			apple_connector->drm_edid = edid;
		}
	}
	if (dcp->nr_modes && apple_connector->drm_edid)
		drm_edid_connector_update(connector, apple_connector->drm_edid);

	/*
	 * An internal panel has no EDID, so nothing fills in the refresh range
	 * that userspace needs before it will drive VRR. Supply the range DCP
	 * reported for the mode. This has to follow the EDID update, which
	 * resets display_info.
	 */
	if (vrr_capable && max_vfreq &&
	    !connector->display_info.monitor_range.max_vfreq) {
		connector->display_info.monitor_range.min_vfreq = min_vfreq;
		connector->display_info.monitor_range.max_vfreq = max_vfreq;
	}

	return dcp->nr_modes;
}

/* The user may own drm_display_mode, so we need to search for our copy */
struct dcp_display_mode *lookup_mode(struct apple_dcp *dcp,
					    const struct drm_display_mode *mode)
{
	int i;

	for (i = 0; i < dcp->nr_modes; ++i) {
		if (drm_mode_match(mode, &dcp->modes[i].mode,
				   DRM_MODE_MATCH_TIMINGS |
					   DRM_MODE_MATCH_CLOCK))
			return &dcp->modes[i];
	}

	return NULL;
}

/*
 * H17P keeps the mode the bootloader programmed and never sends
 * set_digital_out_mode (see DCP_INHERIT_BOOT_MODE in iomfb_template.c), so a
 * modeset to any other mode would silently not be applied.  The bootloader
 * brings the panel up in its native timing, which is the mode the firmware
 * scores highest and enumerate_modes() marks preferred; offer only that one.
 */
static bool dcp_mode_settable(struct apple_dcp *dcp,
			      const struct drm_display_mode *mode)
{
	struct dcp_display_mode *dcp_mode = lookup_mode(dcp, mode);

	if (!dcp_mode)
		return false;

	if (dcp->fw_compat == DCP_FIRMWARE_H17P)
		return dcp_mode->mode.type & DRM_MODE_TYPE_PREFERRED;

	return true;
}

enum drm_mode_status dcp_mode_valid(struct drm_connector *connector,
				    const struct drm_display_mode *mode)
{
	struct apple_connector *apple_connector = to_apple_connector(connector);
	struct platform_device *pdev = apple_connector->dcp;
	struct apple_dcp *dcp;

	if (!pdev)
		return MODE_ERROR;
	dcp = platform_get_drvdata(pdev);

	return dcp_mode_settable(dcp, mode) ? MODE_OK : MODE_BAD;
}

int dcp_crtc_atomic_modeset(struct drm_crtc *crtc,
			    struct drm_atomic_state *state)
{
	struct apple_crtc *apple_crtc = to_apple_crtc(crtc);
	struct apple_dcp *dcp = platform_get_drvdata(apple_crtc->dcp);
	struct drm_crtc_state *crtc_state;
	int ret = -EIO;
	bool modeset;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (!crtc_state)
		return 0;

	modeset = drm_atomic_crtc_needs_modeset(crtc_state) || !dcp->valid_mode;

	if (!modeset)
		return 0;

	/* ignore no mode, poweroff is handled elsewhere */
	if (crtc_state->mode.hdisplay == 0 && crtc_state->mode.vdisplay == 0)
		return 0;

	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		ret = iomfb_modeset_v12_3(dcp, crtc_state);
		break;
	case DCP_FIRMWARE_V_13_5:
		ret = iomfb_modeset_v13_3(dcp, crtc_state);
		break;
	case DCP_FIRMWARE_H17P:
		ret = iomfb_modeset_h17p(dcp, crtc_state);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n",
			  dcp->fw_compat);
		break;
	}

	return ret;
}

bool dcp_crtc_mode_fixup(struct drm_crtc *crtc,
			 const struct drm_display_mode *mode,
			 struct drm_display_mode *adjusted_mode)
{
	struct apple_crtc *apple_crtc = to_apple_crtc(crtc);
	struct platform_device *pdev = apple_crtc->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	/* TODO: support synthesized modes through scaling */
	return dcp_mode_settable(dcp, mode);
}


static void iomfb_scanout_release_h17p(struct iomfb_scanout_h17p *scanout)
{
	unsigned int i;

	if (!scanout)
		return;
	for (i = 0; i < SWAP_SURFACES; i++)
		if (scanout->fb[i])
			drm_framebuffer_put(scanout->fb[i]);
	kfree(scanout);
}

static struct iomfb_scanout_h17p *
iomfb_scanout_prepare_h17p(struct apple_dcp *dcp, struct drm_crtc *crtc,
			   struct drm_atomic_state *state)
{
	struct iomfb_scanout_h17p *scanout;
	struct drm_plane *plane;
	struct drm_plane_state *old_state, *new_state;
	unsigned int i, slot;
	int index;

	lockdep_assert_held(&dcp->iomfb.lock);
	scanout = kzalloc_obj(*scanout);
	if (!scanout)
		return NULL;
	if (dcp->iomfb.scanout) {
		scanout->request = dcp->iomfb.scanout->request;
		for (i = 0; i < SWAP_SURFACES; i++) {
			scanout->fb[i] = dcp->iomfb.scanout->fb[i];
			if (scanout->fb[i])
				drm_framebuffer_get(scanout->fb[i]);
		}
	}

	for_each_oldnew_plane_in_state(state, plane, old_state, new_state, index) {
		if (old_state->crtc != crtc && new_state->crtc != crtc)
			continue;
		slot = to_apple_plane(plane)->iomfb_surf;
		if (slot >= SWAP_SURFACES) {
			iomfb_scanout_release_h17p(scanout);
			return NULL;
		}
		if (scanout->fb[slot])
			drm_framebuffer_put(scanout->fb[slot]);
		scanout->fb[slot] = NULL;
		if (new_state->crtc == crtc && new_state->visible && new_state->fb) {
			scanout->fb[slot] = new_state->fb;
			drm_framebuffer_get(new_state->fb);
		}
	}
	return scanout;
}

void iomfb_scanout_complete_h17p(struct apple_dcp *dcp)
{
	struct iomfb_scanout_h17p *scanout = dcp->iomfb.next_scanout;
	struct iomfb_scanout_h17p *previous = dcp->iomfb.scanout;
	struct dcp_swap_submit_req_h17p *request = &dcp->swap.h17p;
	unsigned int i;

	lockdep_assert_held(&dcp->iomfb.lock);
	if (!scanout)
		return;

	/* Keep common present arguments, including the null output pointers. */
	scanout->request = *request;
	/* Preserve unchanged planes when only a subset was presented. */
	for (i = 0; i < SWAP_SURFACES; i++) {
		if (!(request->swap.swap_enabled & BIT(i)) && previous) {
			scanout->request.surf[i] = previous->request.surf[i];
			scanout->request.surf_iova[i] = previous->request.surf_iova[i];
			scanout->request.swap.src_rect[i] = previous->request.swap.src_rect[i];
			scanout->request.swap.dst_rect[i] = previous->request.swap.dst_rect[i];
			scanout->request.swap.surf_ids[i] = previous->request.swap.surf_ids[i];
			scanout->request.swap.surf_flags[i] = previous->request.swap.surf_flags[i];
			scanout->request.swap.surf_unk[i] = previous->request.swap.surf_unk[i];
		}
		scanout->request.surf_null[i] = !scanout->fb[i];
	}
	/* Brightness re-presents all pinned surfaces with a fresh swap ID. */
	/* Replay only a background established by a completed present. */
	scanout->request.swap.swap_enabled =
		request->swap.swap_enabled & IOMFB_SET_BACKGROUND;
	if (previous)
		scanout->request.swap.swap_enabled |=
			previous->request.swap.swap_enabled & IOMFB_SET_BACKGROUND;
	for (i = 0; i < SWAP_SURFACES; i++)
		if (scanout->fb[i])
			scanout->request.swap.swap_enabled |= BIT(i);
	scanout->request.swap.swap_completed = scanout->request.swap.swap_enabled;
	if (!(request->swap.swap_enabled & IOMFB_SET_BACKGROUND) && previous)
		scanout->request.swap.bg_color = previous->request.swap.bg_color;
	dcp->iomfb.scanout = scanout;
	dcp->iomfb.next_scanout = NULL;
	iomfb_scanout_release_h17p(previous);
}

bool iomfb_present_brightness_only_h17p(struct apple_dcp *dcp)
{
	return dcp->iomfb.active && dcp->iomfb.active->brightness_only;
}

bool iomfb_apply_backlight_h17p(struct apple_dcp *dcp,
				const struct dcp_swap_submit_req_h17p *request,
				struct dcp_present_h17p *wire)
{
	struct iomfb_transaction *transaction = dcp->iomfb.active;
	struct dcp_backlight_present present;
	bool have_surface = false;
	unsigned int i;
	int ret;

	lockdep_assert_held(&dcp->iomfb.lock);
	if (!dcp_backlight_active(dcp)) {
		if (!dcp_has_panel(dcp))
			return true;
		if (!dcp->brightness.maximum)
			return false;
		iomfb_encode_backlight_h17p(wire, 0,
					    dcp->brightness.maximum, false);
		return true;
	}
	if (!transaction || !dcp->brightness.maximum)
		return false;
	for (i = 0; i < SWAP_SURFACES; i++)
		have_surface |= !request->surf_null[i];
	ret = dcp_backlight_prepare(dcp, have_surface, &present);
	if (ret && ret != -EALREADY)
		return false;
	if (!ret) {
		transaction->backlight = present;
		transaction->backlight_reserved = true;
	}
	iomfb_encode_backlight_h17p(wire, present.nits,
				    dcp->brightness.maximum, !ret);
	return true;
}

void iomfb_present_failed_h17p(struct apple_dcp *dcp)
{
	struct iomfb_transaction *transaction = dcp->iomfb.active;

	if (transaction && transaction->backlight_reserved) {
		dcp_backlight_complete(dcp, transaction->backlight.sequence, false);
		transaction->backlight_reserved = false;
		transaction->backlight_failed = true;
	}
}

bool iomfb_present_complete_h17p(struct apple_dcp *dcp)
{
	struct iomfb_transaction *transaction = dcp->iomfb.active;

	lockdep_assert_held(&dcp->iomfb.lock);
	if (transaction) {
		transaction->completed = true;
		if (transaction->backlight_reserved) {
			dcp_backlight_complete(dcp, transaction->backlight.sequence, true);
			transaction->backlight_reserved = false;
		}
		if (transaction->brightness_only)
			return false;
	}
	atomic_cmpxchg(&dcp->iomfb.opaque_x_state, IOMFB_OPAQUE_X_WAITING,
		       IOMFB_OPAQUE_X_NEEDED);
	iomfb_scanout_complete_h17p(dcp);
	return true;
}

static void iomfb_backlight_start(struct apple_dcp *dcp,
				  struct iomfb_transaction *transaction)
{
	struct iomfb_scanout_h17p *scanout = dcp->iomfb.scanout;
	bool have_surface = false;
	unsigned int i;

	if (!dcp_backlight_pending(dcp) || !dcp->valid_mode || !scanout ||
	    !dcp->connector || !dcp->connector->connected)
		return;
	for (i = 0; i < SWAP_SURFACES; i++)
		have_surface |= !!scanout->fb[i];
	if (!have_surface)
		return;

	dcp->swap.h17p = scanout->request;
	iomfb_present_backlight_h17p(dcp);
}

static void iomfb_backlight_release(struct iomfb_transaction *transaction)
{
	kfree(transaction);
}

static void iomfb_backlight_kick(struct apple_dcp *dcp)
{
	struct iomfb_transaction *transaction;

	transaction = kzalloc_obj(*transaction);
	if (!transaction)
		return;
	transaction->start = iomfb_backlight_start;
	transaction->release = iomfb_backlight_release;
	transaction->brightness_only = true;
	if (iomfb_queue(dcp, transaction))
		kfree(transaction);
}

int iomfb_configure_backlight_h17p(struct apple_dcp *dcp, u32 maximum,
				   bool inherited_valid, u32 inherited,
				   bool default_valid, u32 default_nits)
{
	if (!iomfb_uses_queue(dcp))
		return -EINVAL;
	return dcp_backlight_configure(dcp, maximum, inherited_valid, inherited,
				       default_valid, default_nits, iomfb_backlight_kick);
}

struct iomfb_atomic_transaction {
	struct iomfb_transaction transaction;
	struct drm_atomic_state *state;
	struct drm_crtc *crtc;
	struct apple_dcp *dcp;
	struct iomfb_scanout_h17p *scanout;
};

static void iomfb_atomic_start(struct apple_dcp *dcp,
			       struct iomfb_transaction *transaction)
{
	struct iomfb_atomic_transaction *atomic = container_of(transaction,
					 struct iomfb_atomic_transaction, transaction);

	if (!dcp->valid_mode || !dcp->connector || !dcp->connector->connected) {
		schedule_work(&dcp->vblank_wq);
		return;
	}
	atomic->scanout = iomfb_scanout_prepare_h17p(dcp, atomic->crtc, atomic->state);
	if (!atomic->scanout) {
		WRITE_ONCE(dcp->crashed, true);
		schedule_work(&dcp->vblank_wq);
		return;
	}
	dcp->iomfb.next_scanout = atomic->scanout;
	iomfb_flush_h17p(dcp, atomic->crtc, atomic->state);
}

static void iomfb_atomic_release(struct iomfb_transaction *transaction)
{
	struct iomfb_atomic_transaction *atomic = container_of(transaction,
					 struct iomfb_atomic_transaction, transaction);

	if (atomic->scanout && atomic->dcp->iomfb.scanout != atomic->scanout) {
		if (atomic->dcp->iomfb.next_scanout == atomic->scanout)
			atomic->dcp->iomfb.next_scanout = NULL;
		iomfb_scanout_release_h17p(atomic->scanout);
	}
	drm_atomic_state_put(atomic->state);
	kfree(atomic);
}

static void iomfb_queue_atomic(struct apple_dcp *dcp, struct drm_crtc *crtc,
			       struct drm_atomic_state *state)
{
	struct iomfb_atomic_transaction *atomic;

	atomic = kzalloc_obj(*atomic);
	if (!atomic)
		goto failed;
	atomic->transaction.start = iomfb_atomic_start;
	atomic->transaction.release = iomfb_atomic_release;
	atomic->state = drm_atomic_state_get(state);
	atomic->crtc = crtc;
	atomic->dcp = dcp;
	if (!iomfb_queue(dcp, &atomic->transaction))
		return;
	iomfb_atomic_release(&atomic->transaction);
failed:
	WRITE_ONCE(dcp->crashed, true);
	schedule_work(&dcp->vblank_wq);
}

void dcp_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct platform_device *pdev = to_apple_crtc(crtc)->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (iomfb_uses_queue(dcp)) {
		iomfb_queue_atomic(dcp, crtc, state);
		return;
	}

	/*
	 * DCP does not complete swaps after a link loss.  A plane-only commit
	 * arriving between the reconnect callback and the required modeset
	 * would otherwise leave an unsignalled flip event in front of the
	 * recovery commit.  Modesets set valid_mode before reaching flush.
	 *
	 * The same applies while a connector is still disconnected: resume
	 * re-runs the modeset, which marks the mode valid again, before the
	 * firmware has reported the display back.
	 */
	if (!dcp->valid_mode || !dcp->connector || !dcp->connector->connected) {
		schedule_work(&dcp->vblank_wq);
		return;
	}

	if (dcp_channel_busy(&dcp->ch_cmd))
	{
		if (!dcp->ch_cmd.warned_busy) {
			dev_err(dcp->dev, "unexpected busy command channel\n");
			dcp->ch_cmd.warned_busy = true;
		}
		/* HACK: issue a delayed vblank event to avoid timeouts in
		 * drm_atomic_helper_wait_for_vblanks().
		 */
		schedule_work(&dcp->vblank_wq);
		return;
	} else if (dcp->ch_cmd.warned_busy) {
		dcp->ch_cmd.warned_busy = false;
	}

	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_flush_v12_3(dcp, crtc, state);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_flush_v13_3(dcp, crtc, state);
		break;
	case DCP_FIRMWARE_H17P:
		iomfb_flush_h17p(dcp, crtc, state);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

static void iomfb_start(struct apple_dcp *dcp)
{
	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_start_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_start_v13_3(dcp);
		break;
	case DCP_FIRMWARE_H17P:
		iomfb_start_h17p(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

bool dcp_is_initialized(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->active;
}

static void iomfb_recv_message(struct apple_dcp *dcp, u64 message)
{
	enum dcpep_type type = FIELD_GET(IOMFB_MESSAGE_TYPE, message);

	if (dcp->fw_compat == DCP_FIRMWARE_H17P && READ_ONCE(dcp->crashed))
		return;

	if (type == IOMFB_MESSAGE_TYPE_INITIALIZED) {
		/*
		 * H17P reports its interface version (bits 63:48) and a
		 * firmware hash in the high half of the InitComplete word.
		 * The transport below it is unchanged.
		 */
		if (dcp->fw_compat == DCP_FIRMWARE_H17P)
			dev_info(dcp->dev,
				 "IOMFB: init complete %#llx (version %llu)\n",
				 message,
				 FIELD_GET(GENMASK_ULL(63, 48), message));
		iomfb_start(dcp);
	} else if (type == IOMFB_MESSAGE_TYPE_MSG)
		dcpep_got_msg(dcp, message);
	else
		dev_warn(dcp->dev, "Ignoring unknown message %llx\n", message);
}

void iomfb_recv_msg(struct apple_dcp *dcp, u64 message)
{
	if (!iomfb_uses_queue(dcp)) {
		iomfb_recv_message(dcp, message);
		return;
	}

	mutex_lock(&dcp->iomfb.lock);
	WRITE_ONCE(dcp->iomfb.owner, current);
	if (!dcp->iomfb.stopped)
		iomfb_recv_message(dcp, message);
	WRITE_ONCE(dcp->iomfb.owner, NULL);
	iomfb_queue_advance(dcp);
	mutex_unlock(&dcp->iomfb.lock);
}

int iomfb_start_rtkit(struct apple_dcp *dcp)
{
	dma_addr_t shmem_iova;
	int ret;

	if (iomfb_uses_queue(dcp)) {
		mutex_lock(&dcp->iomfb.lock);
		if (dcp->iomfb.active || dcp->iomfb.scanout) {
			mutex_unlock(&dcp->iomfb.lock);
			return -EBUSY;
		}
		dcp->iomfb.stopped = false;
		mutex_unlock(&dcp->iomfb.lock);
	}

	/*
	 * H17P firmware expects the remote allocator endpoint to be started
	 * before IOMFB.  A firmware subsystem whose endpoint was never started
	 * has nothing answering it on the AP side, and without this one the
	 * display DART faults.
	 */
	if (dcp->fw_compat == DCP_FIRMWARE_H17P) {
		ret = apple_rtkit_start_ep(dcp->rtk, REMOTE_ALLOC_ENDPOINT);
		if (ret)
			return ret;
	}
	ret = apple_rtkit_start_ep(dcp->rtk, IOMFB_ENDPOINT);
	if (ret)
		return ret;

	dcp->shmem = dma_alloc_coherent(dcp->dev, DCP_SHMEM_SIZE, &shmem_iova,
					GFP_KERNEL);
	if (!dcp->shmem)
		return -ENOMEM;

	dcp_send_message(dcp, IOMFB_ENDPOINT, dcpep_set_shmem(shmem_iova));

	return 0;
}

void iomfb_shutdown(struct apple_dcp *dcp)
{
	/* We're going down */
	dcp->active = false;
	dcp->valid_mode = false;

	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_shutdown_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_shutdown_v13_3(dcp);
		break;
	case DCP_FIRMWARE_H17P:
		iomfb_shutdown_h17p(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}
