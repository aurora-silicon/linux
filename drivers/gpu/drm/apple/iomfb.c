// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2021 Alyssa Rosenzweig */

#include <kunit/visibility.h>
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
#include <linux/soc/apple/dp-tunnel.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/unaligned.h>

#include <drm/drm_atomic.h>
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

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030)
	/*
	 * 14.7: a call made while a context-0 callback runs continues that
	 * conversation (context 0, bit 9 clear); any other call opens a new
	 * stream on context 2.
	 */
	if (iomfb_is_v14_7(dcp))
		return dcp->ch_cb.depth ? DCP_CONTEXT_CB : DCP_CONTEXT_CMD;
#endif

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

/*
 * Older firmware and the H17G method profile retain their existing transport.
 * The J514S 14.7 panel has one serialized owner, as H17P.
 */
static bool iomfb_uses_queue(struct apple_dcp *dcp)
{
	return (dcp->fw_compat == DCP_FIRMWARE_H17P &&
		dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17G) ||
	       iomfb_is_v14_7(dcp);
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
	/* DCPEXT rejects enableGPAlphaDiv; retain the panel-only workaround. */
	if (dcp->hw.iomfb_method_profile ==
	    DCP_IOMFB_METHODS_H17P_EXTERNAL)
		return 0;
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
		dev_err(dcp->dev,
			"IOMFB transaction timed out: cmd=%u oob=%u present=%u accepted=%u swap=%u\n",
			dcp->ch_cmd.depth, dcp->ch_oobcmd.depth,
			dcp->present_state_h17p.pending, dcp->present_state_h17p.accepted,
			dcp->present_state_h17p.swap_id);
		if (dcp->ch_cmd.depth && dcp->ch_cmd.depth <= DCP_MAX_CALL_DEPTH) {
			const struct dcp_packet_header *h =
				&dcp->ch_cmd.header[dcp->ch_cmd.depth - 1];

			dev_err(dcp->dev, "pending IOMFB %c%c%c%c in=%u out=%u\n",
				h->tag[3], h->tag[2], h->tag[1], h->tag[0],
				h->in_len, h->out_len);
		}
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
	INIT_LIST_HEAD(&dcp->iomfb.dropped);
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

/*
 * Wait until every queued transaction, including a present's completion, has
 * finished.  A stopped or crashed queue has nothing left to deliver.
 */
bool iomfb_queue_drain(struct apple_dcp *dcp, unsigned long timeout)
{
	unsigned long deadline = jiffies + timeout;
	bool idle;

	if (!iomfb_uses_queue(dcp))
		return true;

	for (;;) {
		mutex_lock(&dcp->iomfb.lock);
		idle = READ_ONCE(dcp->crashed) || dcp->iomfb.stopped ||
		       (!dcp->iomfb.active && list_empty(&dcp->iomfb.pending) &&
			!dcp->present_state_h17p.pending);
		mutex_unlock(&dcp->iomfb.lock);
		if (idle)
			return true;
		if (time_after(jiffies, deadline))
			return false;
		usleep_range(2000, 4000);
	}
}

/* Admission is independent of transport: replies and control RPCs remain live. */
void iomfb_pause_presentation(struct apple_dcp *dcp, bool pause)
{
	guard(mutex)(&dcp->iomfb.lock);
	dcp->iomfb.presentation_paused = pause;
	if (!pause && dcp->external_hotplug_pending) {
		unsigned int action;

		dcp->external_hotplug_pending = false;
		action = dcp_mode_hotplug(&dcp->mode_state, dcp->external_hotplug_connected,
					  dcp->connector ? &dcp->connector->connected : NULL);
		dcp_handle_hotplug_actions(dcp, action);
	}
}

/*
 * Hold the 14.7 panel's presents across its display controller's A472 off and
 * on.  The controller accepts a present while it is off but never completes
 * it, and the queue then waits for it: the A472 that would turn the controller
 * back on queues behind it until the queue timeout declares the coprocessor
 * crashed.  Holding before the A472 off is queued keeps the queue order: a
 * present admitted earlier still goes out first, to a running controller.
 * Admission refuses a held present as a paused one: an atomic commit completes
 * with a synthetic vblank, and a brightness level stays pending for the
 * backlight restore at power-on.
 */
void iomfb_hold_panel_presents(struct apple_dcp *dcp, bool hold)
{
	guard(mutex)(&dcp->iomfb.lock);
	dcp->iomfb.panel_held = hold;
}

/* A dead transport is not proof that DMA or a pending present has stopped. */
int iomfb_queue_settle(struct apple_dcp *dcp, unsigned long timeout)
{
	unsigned long deadline = jiffies + timeout;
	bool idle;

	for (;;) {
		mutex_lock(&dcp->iomfb.lock);
		if (READ_ONCE(dcp->crashed) || dcp->iomfb.stopped) {
			mutex_unlock(&dcp->iomfb.lock);
			return -EIO;
		}
		idle = !dcp->iomfb.active && !dcp->iomfb.queued &&
		       list_empty(&dcp->iomfb.pending) &&
		       !dcp->present_state_h17p.pending && iomfb_channels_idle(dcp);
		mutex_unlock(&dcp->iomfb.lock);
		if (idle)
			return 0;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;
		usleep_range(2000, 4000);
	}
}

int iomfb_queue(struct apple_dcp *dcp, struct iomfb_transaction *transaction)
{
	int ret = 0;

	mutex_lock(&dcp->iomfb.lock);
	if (READ_ONCE(dcp->crashed) || dcp->iomfb.stopped) {
		ret = -EIO;
	} else if (transaction->presentation &&
		   (dcp->iomfb.presentation_paused || dcp->iomfb.panel_held)) {
		ret = -EAGAIN;
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

struct iomfb_crc_transaction {
	struct iomfb_transaction transaction;
	u32 swap_id;
};

static void iomfb_crc_complete(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct iomfb_crc_transaction *crc = cookie;
	struct dcp_packet_header *header;
	u32 value;

	/* A106 has no input and a single little-endian blend-output CRC. */
	if (!out)
		goto invalid;
	header = out - sizeof(*header);
	if (header->in_len || header->out_len != sizeof(__le32))
		goto invalid;
	value = get_unaligned_le32(out);
	if (READ_ONCE(dcp->crc_enabled))
		drm_crtc_add_crc_entry(&dcp->crtc->base, true, crc->swap_id, &value);
	return;

invalid:
	dev_err(dcp->dev, "invalid blend CRC response\n");
	WRITE_ONCE(dcp->crashed, true);
}

static void iomfb_crc_start(struct apple_dcp *dcp,
			    struct iomfb_transaction *transaction)
{
	static const struct dcp_method_entry method = {
		.tag = { 'A', '1', '0', '6' },
		.name = "read_blend_crc",
	};
	struct iomfb_crc_transaction *crc = container_of(transaction,
						       struct iomfb_crc_transaction,
						       transaction);

	if (READ_ONCE(dcp->crc_enabled))
		dcp_push(dcp, false, &method, 0, sizeof(__le32), NULL,
			 iomfb_crc_complete, crc);
}

static void iomfb_crc_release(struct iomfb_transaction *transaction)
{
	kfree(container_of(transaction, struct iomfb_crc_transaction, transaction));
}

void iomfb_queue_crc_h17p(struct apple_dcp *dcp, u32 swap_id)
{
	struct iomfb_crc_transaction *crc;

	lockdep_assert_held(&dcp->iomfb.lock);
	if (!READ_ONCE(dcp->crc_enabled) || READ_ONCE(dcp->crashed) ||
	    dcp->iomfb.stopped || dcp->iomfb.presentation_paused || dcp->iomfb.queued >= 32)
		return;
	crc = kzalloc_obj(*crc);
	if (!crc)
		return;
	crc->swap_id = swap_id;
	crc->transaction.start = iomfb_crc_start;
	crc->transaction.release = iomfb_crc_release;
	/* Read the completed frame before another queued present changes it. */
	list_add(&crc->transaction.link, &dcp->iomfb.pending);
	dcp->iomfb.queued++;
	/* The receiver advances the queue only after acknowledging completion. */
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

#if IS_ENABLED(CONFIG_DRM_APPLE_KUNIT_TEST)
	/* A KUnit test answers for the firmware. */
	if (dcp->test_push) {
		dcp->test_push(dcp, call, cb, cookie);
		return;
	}
#endif

	/* Reply chains stay on the serialized receiver; callers enqueue copies. */
	if (iomfb_uses_queue(dcp) && READ_ONCE(dcp->iomfb.owner) != current) {
		if (iomfb_queue_command(dcp, oob, call, in_len, out_len,
					data, cb, cookie))
			WRITE_ONCE(dcp->crashed, true);
		return;
	}

	if (dcp_fail_stop(dcp) && READ_ONCE(dcp->crashed))
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

	/*
	 * 14.7 sends no out-of-band call, and a second top-level call while
	 * one is outstanding outside a callback has no owner.
	 */
	if (iomfb_is_v14_7(dcp) &&
	    (oob || (context == DCP_CONTEXT_CMD && dcp->ch_cmd.depth))) {
		dev_err(dcp->dev, "IOMFB %c%c%c%c has no 14.7 call context\n",
			call->tag[0], call->tag[1], call->tag[2], call->tag[3]);
		iomfb_v14_7_fail(dcp, "invalid call context");
		return;
	}

	if ((iomfb_uses_queue(dcp) || iomfb_is_v14_7(dcp)) &&
	    (!ch || ch->depth >= DCP_MAX_CALL_DEPTH ||
	     (u64)sizeof(header) + in_len + out_len > 0x8000 ||
	     dcp_packet_start(ch, ch->depth) >
		0x8000 - ALIGN(sizeof(header) + in_len + out_len,
			       DCP_PACKET_ALIGNMENT))) {
		dev_err(dcp->dev, "invalid IOMFB command envelope\n");
		WRITE_ONCE(dcp->crashed, true);
		if (iomfb_is_v14_7(dcp))
			iomfb_v14_7_fail(dcp, "invalid command envelope");
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
	/* 14.7 calls go out with zeroed output storage. */
	if (out_len && iomfb_is_v14_7(dcp))
		memset(out_data + in_len, 0, out_len);

	trace_iomfb_push(dcp, call, context, offset, depth);

	ch->callbacks[depth] = cb;
	ch->cookies[depth] = cookie;
	ch->output[depth] = out + sizeof(header) + in_len;
	ch->header[depth] = header;
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

void dcp_handle_hotplug_actions(struct apple_dcp *dcp, unsigned int action)
{
	if (action & DCP_HOTPLUG_VBLANK)
		schedule_work(&dcp->vblank_wq);
	if ((action & DCP_HOTPLUG_NOTIFY) && dcp->connector)
		schedule_work(&dcp->connector->hotplug_wq);
}

void dcp_retrain_oob(struct apple_connector *connector)
{
	struct platform_device *pdev = READ_ONCE(connector->dcp);
	struct apple_dcp *dcp;

	/* a Type-C port with no pipeline behind it has nothing to retrain */
	if (!pdev || !READ_ONCE(connector->connected))
		return;
	dcp = platform_get_drvdata(pdev);

	/*
	 * Bringing up another high-speed Type-C route can disturb an active DPTX
	 * stream without changing its HPD state.  Invalidate the IOMFB mode and
	 * use the normal hotplug worker to replay the active CRTC from process
	 * context; sending a synthetic disconnect would tear down the connector.
	 */
	dcp_mode_invalidate(&dcp->mode_state);
	schedule_work(&connector->hotplug_wq);
}

static void dcp_clear_connector_edid(struct apple_connector *connector)
{
	lockdep_assert_held(&connector->base.dev->mode_config.mutex);
	drm_edid_connector_update(&connector->base, NULL);
	drm_edid_free(connector->drm_edid);
	connector->drm_edid = NULL;
	connector->drm_edid_firmware_derived = false;
}

static void dcp_consume_edid_invalidation(struct apple_connector *connector)
{
	lockdep_assert_held(&connector->base.dev->mode_config.mutex);
	/* Preserve a disconnect even if the port reconnected before probing. */
	if (xchg(&connector->edid_invalidation_pending, false))
		dcp_clear_connector_edid(connector);
}

/*
 * The external H17P pipeline learns the modes of each cable connection from
 * firmware messages that can lag the link; see dcp-announce.h.
 */
static bool dcp_announce_gated(struct apple_dcp *dcp,
			       struct apple_connector *connector)
{
	return dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17P_EXTERNAL &&
	       READ_ONCE(dcp->connector) == connector;
}

/*
 * Queue the timeout work of a held connection: at once for a hold whose
 * timeout has not started for the current cable generation, as only that
 * work starts it.
 */
void dcp_announce_arm(struct apple_dcp *dcp)
{
	struct dcp_announce *a = &dcp->announce;
	unsigned long now = jiffies, delay = 0;

	lockdep_assert_held(&dcp->hpd_mutex);
	if (a->stopped || !a->holding)
		return;
	if (a->timed && a->generation == dcp->typec_generation &&
	    time_before(now, a->deadline))
		delay = a->deadline - now;
	mod_delayed_work(system_freezable_wq, &dcp->announce_wq, delay);
}

/*
 * Evaluate what DRM is to report for the connection.  @start starts the
 * timeout of a held connection; only the timeout work does that.
 */
static enum dcp_announce_status
dcp_announce_eval(struct apple_dcp *dcp, struct apple_connector *connector,
		  bool start)
{
	struct dcp_announce *a = &dcp->announce;
	struct dcp_announce_facts facts = {
		.generation = dcp->typec_generation,
		.connected = READ_ONCE(connector->connected),
		.present = !READ_ONCE(dcp->external_physically_absent),
		.failed = READ_ONCE(dcp->external_error) || READ_ONCE(dcp->crashed),
		.lost = dcp_announce_lost(a, dcp->typec_generation),
	};
	enum dcp_announce_status status;
	unsigned long now = jiffies;
	bool held = a->holding, announced = a->announced;

	lockdep_assert_held(&dcp->hpd_mutex);
	if (facts.connected && facts.present && !facts.failed)
		facts.ready = dcp_external_modes_ready(dcp);
	status = dcp_announce_update(a, &facts, now);
	if (status == DCP_ANNOUNCE_HOLD) {
		if (start)
			dcp_announce_start(a, now);
		dcp_announce_arm(dcp);
	} else if (held) {
		/* The hold ended: its timeout work has nothing left to do. */
		cancel_delayed_work(&dcp->announce_wq);
		if (status == DCP_ANNOUNCE_CONNECTED && !facts.ready && !facts.failed)
			dev_info(dcp->dev, "display modes not ready after %u ms\n",
				 announced ? DCP_ANNOUNCE_RESUME_TIMEOUT_MS :
				 DCP_ANNOUNCE_TIMEOUT_MS);
	}
	return status;
}

/*
 * Evaluate the connection outside the hotplug worker.  An event that this
 * makes owed is the worker's to send, so queue it, unless the announcement
 * is stopped: dcp_enable_typec_work() queues it then.
 */
static void dcp_announce_poll(struct apple_dcp *dcp, struct apple_connector *connector,
			      bool start)
{
	bool owed = dcp_announce_owed(&dcp->announce);

	dcp_announce_eval(dcp, connector, start);
	if (!owed && dcp_announce_owed(&dcp->announce) && !dcp->announce.stopped)
		schedule_work(&connector->hotplug_wq);
}

/*
 * The timeout of a held connection.  It runs on the freezable workqueue, as
 * the link work does, so the timeout of a hold starts here, once the link
 * can come back for the hold's generation.  Everything it does is under
 * hpd_mutex: a hold that ended meanwhile, or an announcement stopped for
 * teardown or sleep, gets nothing more from here.
 */
void dcp_announce_work(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work), struct apple_dcp,
					     announce_wq);
	struct apple_connector *con = READ_ONCE(dcp->connector);

	scoped_guard(mutex, &dcp->hpd_mutex) {
		if (dcp->announce.holding && !dcp->announce.stopped && con &&
		    dcp_announce_gated(dcp, con) &&
		    READ_ONCE(con->dcp) == to_platform_device(dcp->dev))
			dcp_announce_poll(dcp, con, true);
	}
	dcp_test_point(dcp, DCP_TEST_ANNOUNCE_TIMER_DONE);
}

/* The connection state DRM reports for @connector. */
bool dcp_connector_connected(struct apple_connector *connector)
{
	struct platform_device *pdev = READ_ONCE(connector->dcp);
	struct apple_dcp *dcp = pdev ? platform_get_drvdata(pdev) : NULL;

	if (!dcp || !dcp_announce_gated(dcp, connector))
		return READ_ONCE(connector->connected);

	guard(mutex)(&dcp->hpd_mutex);
	dcp_announce_poll(dcp, connector, false);
	return dcp->announce.announced;
}

/* The CRTC userspace keeps active on @connector while its link is down. */
static struct drm_crtc *dcp_external_unlinked_crtc(struct apple_dcp *dcp,
						   struct apple_connector *connector)
{
	struct drm_connector_state *conn_state = connector->base.state;
	struct drm_crtc *crtc = conn_state ? conn_state->crtc : NULL;

	drm_modeset_lock_assert_held(&connector->base.dev->mode_config.connection_mutex);
	if (!crtc || !crtc->state || !crtc->state->active ||
	    READ_ONCE(dcp->mode_state.valid))
		return NULL;
	return crtc;
}

static bool dcp_external_needs_relink(struct apple_dcp *dcp,
				      struct apple_connector *connector)
{
	struct drm_device *dev = connector->base.dev;
	struct drm_modeset_acquire_ctx ctx;
	bool unlinked = false;
	int ret;

	DRM_MODESET_LOCK_ALL_BEGIN(dev, ctx, 0, ret);
	unlinked = !!dcp_external_unlinked_crtc(dcp, connector);
	DRM_MODESET_LOCK_ALL_END(dev, ctx, ret);

	return unlinked;
}

/* Commit @crtc with its mode set again and the link of @connector good. */
static int dcp_external_commit_relink(struct apple_connector *connector,
				      struct drm_crtc *crtc,
				      struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_state *state;
	int ret;

	state = drm_atomic_state_alloc(crtc->dev);
	if (!state)
		return -ENOMEM;
	state->acquire_ctx = ctx;
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto out;
	}
	conn_state = drm_atomic_get_connector_state(state, &connector->base);
	if (IS_ERR(conn_state)) {
		ret = PTR_ERR(conn_state);
		goto out;
	}
	crtc_state->connectors_changed = true;
	conn_state->link_status = DRM_LINK_STATUS_GOOD;
	ret = drm_atomic_commit(state);
out:
	drm_atomic_state_put(state);

	return ret;
}

/*
 * Set the mode of the CRTC userspace keeps active on @connector again.  If
 * that fails, or leaves the link down, mark the link bad and lose the
 * connection of cable generation @generation.  Both are decided under the
 * modeset locks, so a CRTC that userspace switches off is either switched
 * off first and left alone, or found lost by dcp_external_crtc_released().
 *
 * Returns the error that lost the connection, or 0.
 */
static int dcp_external_reset_crtc(struct apple_dcp *dcp,
				   struct apple_connector *connector, u64 generation)
{
	struct drm_device *dev = connector->base.dev;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_crtc *crtc;
	int ret, err = 0;

	DRM_MODESET_LOCK_ALL_BEGIN(dev, ctx, 0, ret);

	err = 0;
	crtc = dcp_external_unlinked_crtc(dcp, connector);
	if (!crtc)
		goto unlock;
	ret = dcp_external_commit_relink(connector, crtc, &ctx);
	if (ret == -EDEADLK)
		goto unlock;
	if (!ret && READ_ONCE(dcp->mode_state.valid))
		goto unlock;

	err = ret ?: -EIO;
	ret = 0;
	scoped_guard(mutex, &dcp->hpd_mutex) {
		if (generation == dcp->typec_generation) {
			connector->base.state->link_status = DRM_LINK_STATUS_BAD;
			dcp_announce_lose(&dcp->announce, generation);
			/* The caller's event is the one this owes. */
			dcp_announce_notify(&dcp->announce,
					    dcp_announce_eval(dcp, connector, false));
		} else {
			/* A newer connection gets a link of its own. */
			err = 0;
		}
	}
unlock:
	DRM_MODESET_LOCK_ALL_END(dev, ctx, ret);

	return ret ?: err;
}

/*
 * The link of an external display came back, or was disturbed, under a CRTC
 * that userspace keeps active.  Userspace sees the display it had and need
 * not set its mode again, so set it here.  A mode that needs its link
 * proven, such as the 180 Hz exception mode, is admitted only against the
 * EDID a probe read for the current cable generation (see
 * dcp_180hz_mode_current()), so probe first, as userspace would.
 *
 * A link that cannot be set up again is marked bad, and the connection is
 * lost: reported gone until userspace releases the CRTC, see
 * dcp-announce.h.  The caller sends the hotplug event.
 */
static void dcp_external_relink(struct apple_dcp *dcp,
				struct apple_connector *connector)
{
	struct drm_device *dev = connector->base.dev;
	u64 generation;
	int ret;

	if (!dcp_external_needs_relink(dcp, connector))
		return;

	scoped_guard(mutex, &dcp->hpd_mutex)
		generation = dcp->typec_generation;
	scoped_guard(mutex, &dev->mode_config.mutex)
		connector->base.funcs->fill_modes(&connector->base,
						  dev->mode_config.max_width,
						  dev->mode_config.max_height);

	ret = dcp_external_reset_crtc(dcp, connector, generation);
	if (ret)
		dev_warn(dcp->dev, "display link not restored (%d), reporting it disconnected\n",
			 ret);
}

/*
 * The CRTC of the external display was switched off.  If its connection was
 * lost, report the display again, as a new connection, once per cable
 * generation (see dcp_announce_retry()).
 */
void dcp_external_crtc_released(struct apple_dcp *dcp)
{
	struct apple_connector *con = READ_ONCE(dcp->connector);

	guard(mutex)(&dcp->hpd_mutex);
	if (dcp_announce_retry(&dcp->announce, dcp->typec_generation) &&
	    !dcp->announce.stopped && con &&
	    READ_ONCE(con->dcp) == to_platform_device(dcp->dev))
		schedule_work(&con->hotplug_wq);
}

void dcp_hotplug(struct work_struct *work)
{
	struct apple_connector *connector;
	struct platform_device *pdev;
	struct apple_dcp *dcp;
	bool connected, gated;
	int ret;

	connector = container_of(work, struct apple_connector, hotplug_wq);
	scoped_guard(mutex, &connector->base.dev->mode_config.mutex)
		dcp_consume_edid_invalidation(connector);

	pdev = READ_ONCE(connector->dcp);
	if (!pdev) {	/* a Type-C port unrouted after this was queued */
		scoped_guard(mutex, &connector->base.dev->mode_config.mutex) {
			if (!READ_ONCE(connector->dcp) && connector->drm_edid_firmware_derived) {
				drm_edid_connector_update(&connector->base, NULL);
				connector->drm_edid_firmware_derived = false;
			}
		}
		drm_kms_helper_connector_hotplug_event(&connector->base);
		return;
	}
	dcp = platform_get_drvdata(pdev);
	dev_info(dcp->dev, "%s() connected:%d valid_mode:%d nr_modes:%u\n", __func__,
		 connector->connected, READ_ONCE(dcp->mode_state.valid), dcp->nr_modes);

	scoped_guard(mutex, &connector->base.dev->mode_config.mutex) {
		if (READ_ONCE(connector->dcp) == pdev)
			dcp_update_connector_dimensions(dcp, &connector->base);
		if (!READ_ONCE(connector->connected))
			dcp_clear_connector_edid(connector);
	}

	gated = dcp_announce_gated(dcp, connector);
	if (gated) {
		enum dcp_announce_status status;
		bool notify;

		scoped_guard(mutex, &dcp->hpd_mutex) {
			status = dcp_announce_eval(dcp, connector, false);
			notify = dcp_announce_notify(&dcp->announce, status);
			connected = dcp->announce.announced;
		}
		/* The last missing fact or the timeout queues this work again. */
		if (!notify)
			return;
	} else {
		connected = READ_ONCE(connector->connected);
	}

	/*
	 * DCP defers link training until we set a display mode. But we set
	 * display modes from atomic_flush, so userspace needs to trigger a
	 * flush, or the CRTC gets no signal.
	 */
	if (gated) {
		if (connector->base.state && !READ_ONCE(dcp->mode_state.valid) && connected)
			dcp_external_relink(dcp, connector);
	} else if (connector->base.state && !READ_ONCE(dcp->mode_state.valid) && connected &&
		   !(dcp_is_usb4_output(dcp) && apple_dp_tunnel_t602x())) {
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

	dcp_test_point(dcp, DCP_TEST_HOTPLUG_EVENT);
	drm_kms_helper_connector_hotplug_event(&connector->base);
}

/*
 * J700/25G83 asks D134 after publishing its external video interface.
 * The observed async record has no input and a four-byte result. Use the
 * existing zero reply for this record only; its feature remains unsupported.
 */
static bool dcp_external_zero_reply(struct apple_dcp *dcp, int tag,
				    enum dcp_context_id context,
				    u32 in_len, u32 out_len)
{
	return tag == 134 && context == DCP_CONTEXT_ASYNC &&
	       in_len == 0 && out_len == sizeof(u32) &&
	       dcp->hw.iomfb_method_profile ==
	       DCP_IOMFB_METHODS_H17P_EXTERNAL &&
	       of_machine_is_compatible("apple,j700") &&
	       of_device_is_compatible(dcp->dev->of_node, "apple,t8140-dcpext");
}

/*
 * A 14.7 callback can arrive on either route of the first two streams, with
 * or without the new-stream flag (bit 9).  Its record is in the callback area
 * of its route and stream, and its ACK keeps its own context nibble; the
 * bookkeeping uses the callback channel of that route and stream.
 */
static struct dcp_channel *dcp_v14_7_cb_channel(struct apple_dcp *dcp,
						 enum dcp_context_id context)
{
	switch ((unsigned int)context) {
	case 0: case 2:
		return &dcp->ch_cb;
	case 1: case 3:
		return &dcp->ch_async;
	case 4: case 6:
		return &dcp->ch_oobcb;
	case 5: case 7:
		return &dcp->ch_oobasync;
	default:
		return NULL;
	}
}

static int dcp_v14_7_cb_offset(enum dcp_context_id context)
{
	if ((unsigned int)context > 7)
		return -EINVAL;
	return ((context & 1) ? 0x40000 : 0x60000) + ((context & 4) ? 0x8000 : 0);
}

/*
 * 14.7 callbacks: the exact shape of every callback the J514S session
 * handles is known, so any other number or shape, a missing handler or a
 * continuation callback off context 0 is a protocol failure.  The session
 * latches and the callback is never acknowledged with an invented reply.
 */
static void dcpep_handle_cb_v14_7(struct apple_dcp *dcp,
				  enum dcp_context_id context, void *data,
				  u32 length, u16 offset)
{
	struct dcp_channel *ch = dcp_v14_7_cb_channel(dcp, context);
	struct dcp_packet_header *hdr = data;
	void *in, *out;
	int tag;
	u8 depth;

	if (length < sizeof(*hdr) || !ch || ch->depth >= DCP_MAX_CALL_DEPTH ||
	    offset % DCP_PACKET_ALIGNMENT || offset >= 0x8000 ||
	    length > 0x8000 - offset) {
		dev_err(dcp->dev, "invalid IOMFB callback envelope context=%u offset=%#x length=%#x\n",
			context, offset, length);
		iomfb_v14_7_fail(dcp, "invalid callback envelope");
		return;
	}

	tag = dcp_parse_tag(hdr->tag);
	if (tag < 0 || (u64)sizeof(*hdr) + hdr->in_len + hdr->out_len != length ||
	    !iomfb_check_callback_v14_7(tag, hdr->in_len, hdr->out_len) ||
	    !dcp->cb_handlers || !dcp->cb_handlers[tag]) {
		dev_err(dcp->dev, "unsupported IOMFB callback %c%c%c%c (%u, %u) context=%u\n",
			hdr->tag[3], hdr->tag[2], hdr->tag[1], hdr->tag[0],
			hdr->in_len, hdr->out_len, context);
		iomfb_v14_7_fail(dcp, "unsupported callback");
		return;
	}
	if (context != DCP_CONTEXT_CB && iomfb_v14_7_needs_continuation(tag)) {
		dev_err(dcp->dev, "callback D%03d needs a continuation but arrived on context %u\n",
			tag, context);
		iomfb_v14_7_fail(dcp, "continuation callback off context 0");
		return;
	}

	in = data + sizeof(*hdr);
	out = in + hdr->in_len;
	if (hdr->out_len)
		memset(out, 0, hdr->out_len);

	depth = dcp_push_depth(&ch->depth);
	ch->output[depth] = out;
	ch->end[depth] = offset + ALIGN(length, DCP_PACKET_ALIGNMENT);
	if (dcp->v14_7)
		dcp->v14_7->callbacks++;

	/* A deferred reply ACKs from its continuation; a failure never does. */
	if (!dcp->cb_handlers[tag](dcp, tag, out, in) || READ_ONCE(dcp->crashed))
		return;

	dma_wmb();
	dcp_pop_depth(&ch->depth);
	dcp_send_message(dcp, IOMFB_ENDPOINT, dcpep_ack(context));
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
	bool handled, zero_reply;

	if (iomfb_is_v14_7(dcp)) {
		dcpep_handle_cb_v14_7(dcp, context, data, length, offset);
		return;
	}

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
	      !iomfb_check_callback_h17p(dcp, tag, hdr->in_len, hdr->out_len)))) {
		dev_err(dev, "unsupported IOMFB callback %d (%u, %u)\n",
			tag, hdr->in_len, hdr->out_len);
		dcp->crashed = true;
		return;
	}

	in = data + sizeof(*hdr);
	out = in + hdr->in_len;

	if (!handled) {
		zero_reply = dcp_external_zero_reply(dcp, tag, context,
						     hdr->in_len, hdr->out_len);
		if (!zero_reply &&
		    (!iomfb_uses_queue(dcp) ||
		     !test_and_set_bit(tag, dcp->unknown_callbacks)))
			dev_warn(dev, "received unknown callback %c%c%c%c (%u, %u) context=%u\n",
				 hdr->tag[3], hdr->tag[2], hdr->tag[1], hdr->tag[0],
				 hdr->in_len, hdr->out_len, context);
		if (dcp->hw.iomfb_method_profile ==
		    DCP_IOMFB_METHODS_H17P_EXTERNAL && !zero_reply) {
			/* Retain the failed session without inventing a successful reply. */
			dcp->crashed = true;
			complete(&dcp->start_done);
			return;
		}
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

static void dcpep_handle_ack(struct apple_dcp *dcp, enum dcp_context_id context)
{
	struct dcp_channel *ch = dcp_get_channel(dcp, context);
	const struct dcp_packet_header *record, *sent;
	void *cookie, *out;
	dcp_callback_t cb;

	if (!ch) {
		dev_warn(dcp->dev, "ignoring ack on context %X\n", context);
		if (iomfb_is_v14_7(dcp))
			iomfb_v14_7_fail(dcp, "reply on an unknown context");
		return;
	}

	if (!ch->depth) {
		dev_warn(dcp->dev, "ignoring ack on idle context %X\n", context);
		if (iomfb_is_v14_7(dcp))
			iomfb_v14_7_fail(dcp, "reply without a pending call");
		return;
	}

	dcp_pop_depth(&ch->depth);

	cb = ch->callbacks[ch->depth];
	cookie = ch->cookies[ch->depth];
	out = ch->output[ch->depth];
	sent = &ch->header[ch->depth];

	ch->callbacks[ch->depth] = NULL;
	ch->cookies[ch->depth] = NULL;
	ch->output[ch->depth] = NULL;

	/*
	 * An ack completes the innermost pending command on its context.  The
	 * firmware writes the reply in place into that AP command record, whose
	 * address dcp_push() saved; the ack itself carries neither offset nor
	 * length on any firmware.  A command sent from within a callback is
	 * acked on the callback context, whose receive area holds the callback
	 * record rather than the reply, so the ack cannot be used to find it.
	 *
	 * On older firmware the lengths in the record header must still be
	 * the ones that were sent; otherwise the record was overwritten and
	 * its output cannot be trusted.  The tag is left out of the check: it
	 * has only been seen preserved on 13.5 firmware.
	 */
	if (dcp->fw_compat != DCP_FIRMWARE_H17P) {
		record = out - sent->in_len - sizeof(*record);
		if (record->in_len != sent->in_len ||
		    record->out_len != sent->out_len) {
			dev_err(dcp->dev,
				"corrupt %c%c%c%c command record on context %X\n",
				sent->tag[3], sent->tag[2], sent->tag[1],
				sent->tag[0], context);
			WRITE_ONCE(dcp->crashed, true);
			/*
			 * Still complete the command, without an output, so
			 * that its callback drops the cookie reference and
			 * wakes the waiter instead of leaving it to time out.
			 */
			out = NULL;
		}
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
	if (iomfb_is_v14_7(dcp) && !FIELD_GET(IOMFB_MSG_ACK, message))
		channel_offset = dcp_v14_7_cb_offset(ctx_id);

	if (channel_offset < 0) {
		dev_warn(dcp->dev, "invalid context received %u\n", ctx_id);
		if (iomfb_is_v14_7(dcp))
			iomfb_v14_7_fail(dcp, "message on an unknown context");
		return;
	}

	data = dcp->shmem + channel_offset + offset;

	if (FIELD_GET(IOMFB_MSG_ACK, message))
		dcpep_handle_ack(dcp, ctx_id);
	else
		dcpep_handle_cb(dcp, ctx_id, data, length, offset);
}

/* Retained arrays are not freed while mode readers can still refer to them. */
static struct dcp_display_mode *dcp_modes_snapshot(struct apple_dcp *dcp,
						 unsigned int *count)
{
	struct dcp_display_mode *modes;
	unsigned long flags;

	spin_lock_irqsave(&dcp->metadata.lock, flags);
	modes = dcp->modes;
	*count = dcp->nr_modes;
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	return modes;
}

/* Caller holds hpd_mutex -> dcpavserv.lock -> metadata.lock. */
static bool dcp_exception_mode_current_locked(struct apple_dcp *dcp,
					     const struct dcp_display_mode *mode)
{
	if (!mode->automatic_rgb8_quirk)
		return true;
	return dcp_180hz_mode_current(dcp, mode);
}

static bool dcp_publish_mode(struct apple_dcp *dcp, struct drm_connector *connector,
			     const struct dcp_display_mode *source,
			     struct drm_display_mode *copy)
{
	unsigned long flags;
	bool eligible;

	/* Ordinary modes keep their existing publication policy. */
	if (!source->automatic_rgb8_quirk) {
		drm_mode_probed_add(connector, copy);
		return true;
	}
	/* External get_modes holds hpd_mutex after its final EDID generation check. */
	/* Allocation happened outside metadata.lock; recheck before linking. */
	spin_lock_irqsave(&dcp->metadata.lock, flags);
	eligible = dcp->connector && &dcp->connector->base == connector &&
		dcp_exception_mode_current_locked(dcp, source);
	if (eligible)
		/* drm_mode_probed_add only links a list under mode_config.mutex. */
		drm_mode_probed_add(connector, copy);
	spin_unlock_irqrestore(&dcp->metadata.lock, flags);
	if (!eligible)
		drm_mode_destroy(connector->dev, copy);
	return eligible;
}

int dcp_get_modes(struct drm_connector *connector)
{
	struct apple_connector *apple_connector = to_apple_connector(connector);
	struct platform_device *pdev = READ_ONCE(apple_connector->dcp);
	struct apple_dcp *dcp;

	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;
	struct dcp_display_mode *modes;
	unsigned int nr_modes, published = 0;
	u16 min_vfreq = 0, max_vfreq = 0;
	bool vrr_capable = false;
	bool physical_edid;
	bool pending_180hz = false, published_180hz = false;
	u64 edid_generation = 0;
	u64 service_epoch = 0;
	unsigned int i;

	dcp_consume_edid_invalidation(apple_connector);

	/* A Type-C port has no pipeline while the fabric is moving it. */
	if (!pdev) {
		if (apple_connector->drm_edid_firmware_derived) {
			drm_edid_connector_update(connector, NULL);
			apple_connector->drm_edid_firmware_derived = false;
		}
		return 0;
	}
	dcp = platform_get_drvdata(pdev);
	modes = dcp_modes_snapshot(dcp, &nr_modes);

	physical_edid = dcp->hw.iomfb_method_profile ==
		DCP_IOMFB_METHODS_H17P_EXTERNAL;
	/* get_modes holds mode_config.mutex; HPD paths never acquire it. */
	if (physical_edid) {
		mutex_lock(&dcp->hpd_mutex);
		edid_generation = dcp->typec_generation;
	}
	mutex_lock(&dcp->dcpavserv.lock);
	/* Consume CLOSE invalidation under the publication/retirement lock. */
	dcp_consume_edid_invalidation(apple_connector);
	if (apple_connector->drm_edid &&
	    (!dcpavserv_edid_service_current(dcp, apple_connector->drm_edid_service_epoch) ||
	     (physical_edid && apple_connector->drm_edid_generation != edid_generation)))
		dcp_clear_connector_edid(apple_connector);
	if (physical_edid) {
		if (!dcpavserv_edid_current(dcp, edid_generation)) {
			dcp_clear_connector_edid(apple_connector);
			goto edid_done;
		}
	}

	/* Internal H17P still has no supported EDID command transport. */
	if (nr_modes && !apple_connector->drm_edid &&
	    ((dcp->fw_compat != DCP_FIRMWARE_H17P &&
	      !iomfb_is_v14_7_panel(dcp)) ||
	     (dcp->dcpavservep && dcp->dcpavservep->wire_v2))) {
		const struct drm_edid *edid;
		/* Let physical disconnect advance the generation during the request. */
		mutex_unlock(&dcp->dcpavserv.lock);
		if (physical_edid)
			mutex_unlock(&dcp->hpd_mutex);
		edid = dcpavserv_copy_edid(dcp, edid_generation, &service_epoch);
		if (physical_edid)
			mutex_lock(&dcp->hpd_mutex);
		mutex_lock(&dcp->dcpavserv.lock);
		if (physical_edid) {
			if (!dcpavserv_edid_current(dcp, edid_generation) ||
			    READ_ONCE(apple_connector->dcp) != pdev) {
				if (!IS_ERR_OR_NULL(edid))
					drm_edid_free(edid);
				dcp_clear_connector_edid(apple_connector);
				goto edid_done;
			}
		}
		if (!IS_ERR_OR_NULL(edid) &&
		    !dcpavserv_edid_service_current(dcp, service_epoch)) {
			drm_edid_free(edid);
			dcp_clear_connector_edid(apple_connector);
			goto edid_done;
		}
		/* Retirement may have queued cleanup while this request waited. */
		dcp_consume_edid_invalidation(apple_connector);
		if (IS_ERR_OR_NULL(edid)) {
			/*
			 * There is no AV service for an internal panel, nor
			 * for an external display while its link is down;
			 * do not report that on every mode probe.
			 */
			if (PTR_ERR(edid) == -ENODEV)
				dev_dbg(dcp->dev, "no EDID source\n");
			else
				dev_warn(dcp->dev, "copy_edid failed: %pe\n", edid);
		} else {
			drm_edid_free(apple_connector->drm_edid);
			apple_connector->drm_edid = edid;
			apple_connector->drm_edid_generation = edid_generation;
			apple_connector->drm_edid_service_epoch = service_epoch;
		}
	}
	if (nr_modes && apple_connector->drm_edid) {
		int update = drm_edid_connector_update(connector, apple_connector->drm_edid);

		if (update) {
			dev_warn(dcp->dev, "raw EDID publication failed: %d\n", update);
			dcp_clear_connector_edid(apple_connector);
		} else {
			apple_connector->drm_edid_firmware_derived = false;
		}
	} else {
		u64 generation = 0;
		const struct drm_edid *derived = nr_modes ?
			dcp_firmware_edid(dcp, &generation) : ERR_PTR(-ENODATA);

		if (!IS_ERR(derived)) {
			bool log = !apple_connector->drm_edid_firmware_derived ||
				apple_connector->drm_edid_generation != generation;
			int update;

			apple_connector->drm_edid_firmware_derived = true;
			apple_connector->drm_edid_generation = generation;
			update = drm_edid_connector_update(connector, derived);
			drm_edid_free(derived);
			if (update || !dcp_firmware_metadata_current(dcp, generation)) {
				drm_edid_connector_update(connector, NULL);
				apple_connector->drm_edid_firmware_derived = false;
			} else if (log) {
				dev_info(dcp->dev, "using firmware-derived EDID metadata for connection %llu\n",
					 generation);
			}
		} else if (apple_connector->drm_edid_firmware_derived) {
			drm_edid_connector_update(connector, NULL);
			apple_connector->drm_edid_firmware_derived = false;
		}
	}

edid_done:
	/* Publish after a possibly delayed raw read, under its physical generation. */
	for (i = 0; i < nr_modes; ++i) {
		mode = drm_mode_duplicate(dev, &modes[i].mode);
		if (!mode) {
			dev_err(dev->dev, "Failed to duplicate display mode\n");
			published = 0;
			goto modes_done;
		}
		pending_180hz |= modes[i].automatic_rgb8_quirk &&
			dcp_180hz_pending(dcp, &modes[i]);
		if (!dcp_publish_mode(dcp, connector, &modes[i], mode))
			continue;
		published_180hz |= modes[i].automatic_rgb8_quirk;
		published++;
		if (modes[i].vrr) {
			u16 lo = modes[i].min_vrr >> 16;
			u16 hi = modes[i].max_vrr >> 16;

			if (!min_vfreq || lo < min_vfreq)
				min_vfreq = lo;
			if (hi > max_vfreq)
				max_vfreq = hi;
		}
		vrr_capable |= modes[i].vrr;
	}
	drm_connector_set_vrr_capable_property(connector, vrr_capable);
modes_done:
	mutex_unlock(&dcp->dcpavserv.lock);
	if (physical_edid)
		mutex_unlock(&dcp->hpd_mutex);
	if (pending_180hz && !published_180hz)
		dcp_180hz_retry(dcp);
	if (apple_connector->drm_edid)
		dcp_retry_placeholder_edid(dcp, apple_connector->drm_edid);

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

	return published;
}

/* The user may own drm_display_mode, so we need to search for our copy */
struct dcp_display_mode *lookup_mode(struct apple_dcp *dcp,
					    const struct drm_display_mode *mode)
{
	struct dcp_display_mode *modes;
	unsigned int count, i;
	unsigned long flags;
	bool eligible;

	modes = dcp_modes_snapshot(dcp, &count);
	for (i = 0; i < count; ++i) {
		if (drm_mode_match(mode, &modes[i].mode,
				   DRM_MODE_MATCH_TIMINGS |
					   DRM_MODE_MATCH_CLOCK)) {
			if (!modes[i].automatic_rgb8_quirk)
				return &modes[i];
			mutex_lock(&dcp->hpd_mutex);
			mutex_lock(&dcp->dcpavserv.lock);
			spin_lock_irqsave(&dcp->metadata.lock, flags);
			eligible = dcp_exception_mode_current_locked(dcp, &modes[i]);
			spin_unlock_irqrestore(&dcp->metadata.lock, flags);
			mutex_unlock(&dcp->dcpavserv.lock);
			mutex_unlock(&dcp->hpd_mutex);
			if (eligible)
				return &modes[i];
		}
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

	if ((dcp->fw_compat == DCP_FIRMWARE_H17P ||
	     iomfb_is_v14_7_panel(dcp)) &&
	    dcp->hw.iomfb_method_profile !=
	    DCP_IOMFB_METHODS_H17P_EXTERNAL)
		return dcp_mode->mode.type & DRM_MODE_TYPE_PREFERRED;

	return true;
}

enum drm_mode_status dcp_mode_valid(struct drm_connector *connector,
				    const struct drm_display_mode *mode)
{
	struct apple_connector *apple_connector = to_apple_connector(connector);
	struct platform_device *pdev = READ_ONCE(apple_connector->dcp);
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
	unsigned int action;
	int ret = -EIO;
	bool modeset;

	/* Disconnect cannot pass the admission check before these RPCs finish. */
	guard(mutex)(&dcp->external_lock);
	if (dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17P_EXTERNAL &&
	    (!dcp->external_running || dcp->external_error || READ_ONCE(dcp->quiescing) ||
	     READ_ONCE(dcp->crashed) || READ_ONCE(dcp->iomfb.presentation_paused))) {
		/*
		 * The pipe of this CRTC did not start, so it has no link and its
		 * old mode is gone with it: the hotplug worker sets it up again
		 * or reports the display lost.  A pipe that runs with its
		 * presentation paused on purpose keeps its mode.
		 */
		if (!dcp->external_running || dcp->external_error) {
			dcp_mode_invalidate(&dcp->mode_state);
			dcp_handle_hotplug_actions(dcp, DCP_HOTPLUG_NOTIFY);
		}
		return -EPIPE;
	}

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (!crtc_state)
		return 0;

	modeset = drm_atomic_crtc_needs_modeset(crtc_state) ||
		  !READ_ONCE(dcp->mode_state.valid);

	if (!modeset)
		return 0;

	/* ignore no mode, poweroff is handled elsewhere */
	if (crtc_state->mode.hdisplay == 0 && crtc_state->mode.vdisplay == 0)
		return 0;

	dcp_mode_begin(&dcp->mode_state);
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
	case DCP_FIRMWARE_V_14_7:
		ret = iomfb_is_v14_7_external(dcp) ? iomfb_v14_7_ext_modeset(dcp, crtc_state) :
		      iomfb_modeset_v14_7(dcp, crtc_state);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n",
			  dcp->fw_compat);
		break;
	}

	action = dcp_mode_finish(&dcp->mode_state, !ret, dcp->connector ?
				&dcp->connector->connected : NULL);
	/* An external mode set that failed leaves the CRTC without a link too. */
	if (ret && dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17P_EXTERNAL)
		action |= DCP_HOTPLUG_NOTIFY;
	dcp_handle_hotplug_actions(dcp, action);

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
	INIT_LIST_HEAD(&scanout->link);
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
	struct iomfb_scanout_h17p *dropped, *next;

	lockdep_assert_held(&dcp->iomfb.lock);
	/* The firmware is past every present before this one. */
	list_for_each_entry_safe(dropped, next, &dcp->iomfb.dropped, link) {
		list_del(&dropped->link);
		iomfb_scanout_release_h17p(dropped);
	}
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

/*
 * The firmware accepts presents while an external pipe is not enabled, for
 * instance after a mode set that failed because the sink went away, and then
 * drops them: neither a completion nor an abort follows.  Such a present
 * would hold the queue until its timeout declares the coprocessor crashed,
 * and a teardown waiting for the queue to settle would fail.
 *
 * Called by the swap watchdog: retire the present.  The firmware might still
 * complete it or read it: until the firmware completes a later present, a
 * completion of it is ignored, and the framebuffers of the present and those
 * it displaced stay referenced.  Without a completed present for
 * IOMFB_DROPPED_MAX retired ones, the queue is left to its timeout.  Returns
 * false if the present ended or a newer one re-armed the watchdog in the
 * meantime.  Other firmware has no present state.
 */
bool iomfb_retire_dropped_present_h17p(struct apple_dcp *dcp)
{
	struct dcp_present_state_h17p *state = &dcp->present_state_h17p;
	u32 swap_id;

	if (!iomfb_uses_queue(dcp))
		return true;

	guard(mutex)(&dcp->iomfb.lock);
	swap_id = state->swap_id;
	if (time_before(jiffies, dcp->swap_watchdog_deadline) ||
	    !state->pending || !state->accepted)
		return false;
	if (!dcp_present_retire_h17p(state, swap_id))
		return true;

	iomfb_present_failed_h17p(dcp);
	if (!iomfb_present_brightness_only_h17p(dcp)) {
		dcp_unarm_retained_framebuffers(dcp, swap_id);
		if (dcp->iomfb.next_scanout) {
			list_add_tail(&dcp->iomfb.next_scanout->link,
				      &dcp->iomfb.dropped);
			dcp->iomfb.next_scanout = NULL;
		}
	}
	iomfb_queue_advance(dcp);
	return true;
}

/* Release a present's scanout unless it is displayed or was dropped. */
VISIBLE_IF_KUNIT void iomfb_scanout_put_h17p(struct apple_dcp *dcp,
					     struct iomfb_scanout_h17p *scanout)
{
	/* A transaction that never started is released without the lock. */
	if (!scanout)
		return;
	lockdep_assert_held(&dcp->iomfb.lock);
	if (scanout == dcp->iomfb.scanout || !list_empty(&scanout->link))
		return;
	if (dcp->iomfb.next_scanout == scanout)
		dcp->iomfb.next_scanout = NULL;
	iomfb_scanout_release_h17p(scanout);
}

static void iomfb_backlight_start(struct apple_dcp *dcp,
				  struct iomfb_transaction *transaction)
{
	struct iomfb_scanout_h17p *scanout = dcp->iomfb.scanout;
	bool have_surface = false;
	unsigned int i;

	if (iomfb_is_v14_7(dcp)) {
		iomfb_present_backlight_v14_7(dcp);
		return;
	}
	if (!dcp_backlight_pending(dcp) || !READ_ONCE(dcp->mode_state.valid) || !scanout ||
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
	transaction->presentation = true;
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
#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030)
	/* The 14.7 snapshot this present becomes once it completes. */
	struct iomfb_scanout_v14_7 *scanout_v14_7;
#endif
};

static void iomfb_atomic_start(struct apple_dcp *dcp,
			       struct iomfb_transaction *transaction)
{
	struct iomfb_atomic_transaction *atomic = container_of(transaction,
					 struct iomfb_atomic_transaction, transaction);

	if (!READ_ONCE(dcp->mode_state.valid) || !dcp->connector || !dcp->connector->connected) {
		schedule_work(&dcp->vblank_wq);
		return;
	}
#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030)
	if (iomfb_is_v14_7(dcp)) {
		/* 14.7 external: no new present while the display is off or inactive. */
		if (iomfb_is_v14_7_external(dcp) && !dcp_ext_v14_7_presentable(dcp)) {
			schedule_work(&dcp->vblank_wq);
			return;
		}
		atomic->scanout_v14_7 = iomfb_present_atomic_v14_7(dcp, atomic->crtc,
								    atomic->state);
		return;
	}
#endif
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

	iomfb_scanout_put_h17p(atomic->dcp, atomic->scanout);
#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030)
	iomfb_present_release_v14_7(atomic->dcp, atomic->scanout_v14_7);
#endif
	drm_atomic_state_put(atomic->state);
	kfree(atomic);
}

static void iomfb_queue_atomic(struct apple_dcp *dcp, struct drm_crtc *crtc,
			       struct drm_atomic_state *state)
{
	struct iomfb_atomic_transaction *atomic;
	int ret;

	if (READ_ONCE(dcp->iomfb.presentation_paused) ||
	    READ_ONCE(dcp->iomfb.panel_held)) {
		schedule_work(&dcp->vblank_wq);
		return;
	}
	atomic = kzalloc_obj(*atomic);
	if (!atomic)
		goto failed;
	atomic->transaction.start = iomfb_atomic_start;
	atomic->transaction.presentation = true;
	atomic->transaction.release = iomfb_atomic_release;
	atomic->state = drm_atomic_state_get(state);
	atomic->crtc = crtc;
	atomic->dcp = dcp;
	ret = iomfb_queue(dcp, &atomic->transaction);
	if (!ret)
		return;
	iomfb_atomic_release(&atomic->transaction);
	if (ret == -EAGAIN) {
		schedule_work(&dcp->vblank_wq);
		return;
	}
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
	if (!READ_ONCE(dcp->mode_state.valid) || !dcp->connector || !dcp->connector->connected) {
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
	case DCP_FIRMWARE_V_14_7:
		iomfb_flush_v14_7(dcp, crtc, state);
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
	case DCP_FIRMWARE_V_14_7:
		iomfb_start_v14_7(dcp);
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

/*
 * The 14.7 READY word: type 1, the packet alignment (64) in bits 47:16 and no
 * firmware hash; bit 8 is routing.  It arrives once, and IOMFB traffic only
 * after it.
 */
static bool iomfb_v14_7_check_message(struct apple_dcp *dcp, u64 message)
{
	enum dcpep_type type = FIELD_GET(IOMFB_MESSAGE_TYPE, message);
	struct dcp_v14_7_session *s = dcp->v14_7;

	if (type == IOMFB_MESSAGE_TYPE_INITIALIZED) {
		if (s->ready_word || (message & 0xfeff) != 1 ||
		    (message >> 16) != DCP_PACKET_ALIGNMENT) {
			dev_err(dcp->dev, "unexpected IOMFB READY %#llx\n", message);
			iomfb_v14_7_fail(dcp, "unexpected READY");
			return false;
		}
		s->ready_word = message;
		dev_info(dcp->dev, "IOMFB READY %#018llx; client open A401 -> A455\n",
			 message);
	} else if (type == IOMFB_MESSAGE_TYPE_MSG && !s->ready_word) {
		dev_err(dcp->dev, "IOMFB message %#llx before READY\n", message);
		iomfb_v14_7_fail(dcp, "message before READY");
		return false;
	}
	return true;
}

static void iomfb_recv_message(struct apple_dcp *dcp, u64 message)
{
	enum dcpep_type type = FIELD_GET(IOMFB_MESSAGE_TYPE, message);

	if (dcp_fail_stop(dcp) && READ_ONCE(dcp->crashed))
		return;

	if (iomfb_is_v14_7(dcp) &&
	    !iomfb_v14_7_check_message(dcp, message))
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
	if (dcp->fw_compat == DCP_FIRMWARE_H17P &&
	    dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17P_EXTERNAL) {
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

	/*
	 * The 14.7 INIT descriptor: bytes 0-11 zero and the packet alignment
	 * at +12, in the DCP's DMA window.  The arena is never freed.
	 */
	if (iomfb_is_v14_7(dcp)) {
		if (shmem_iova < DCP_V14_7_DVA_BASE ||
		    shmem_iova - DCP_V14_7_DVA_BASE > DCP_V14_7_DVA_SIZE - DCP_SHMEM_SIZE) {
			dev_err(dcp->dev, "IOMFB arena %pad outside the DCP window\n",
				&shmem_iova);
			iomfb_v14_7_fail(dcp, "IOMFB arena outside the DMA window");
			return -ERANGE;
		}
		put_unaligned_le32(DCP_PACKET_ALIGNMENT, dcp->shmem + 12);
		dma_wmb();
		dev_info(dcp->dev, "IOMFB INIT %#llx (arena %pad, %#x bytes)\n",
			 dcpep_set_shmem(shmem_iova), &shmem_iova, DCP_SHMEM_SIZE);
	}

	dcp_send_message(dcp, IOMFB_ENDPOINT, dcpep_set_shmem(shmem_iova));

	return 0;
}

void iomfb_shutdown(struct apple_dcp *dcp)
{
	/* We're going down */
	dcp->active = false;
	dcp_mode_invalidate(&dcp->mode_state);

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
	case DCP_FIRMWARE_V_14_7:
		iomfb_shutdown_v14_7(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

#if IS_ENABLED(CONFIG_DRM_APPLE_PARSER_KUNIT_TEST)
#include "iomfb-parser-test.c"
#endif
