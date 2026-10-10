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
#include <linux/soc/apple/dp-tunnel.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/unaligned.h>

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

static int neo_dcp_tx_offset(enum neo_dcp_context_id id)
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

static int neo_dcp_channel_offset(enum neo_dcp_context_id id)
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
		return neo_dcp_tx_offset(id);
	}
}

static inline u64 dcpep_set_shmem(u64 dart_va)
{
	return FIELD_PREP(IOMFB_MESSAGE_TYPE, IOMFB_MESSAGE_TYPE_SET_SHMEM) |
	       FIELD_PREP(IOMFB_SHMEM_FLAG, IOMFB_SHMEM_FLAG_VALUE) |
	       FIELD_PREP(IOMFB_SHMEM_DVA, dart_va);
}

static inline u64 dcpep_msg(enum neo_dcp_context_id id, u32 length, u16 offset)
{
	return FIELD_PREP(IOMFB_MESSAGE_TYPE, IOMFB_MESSAGE_TYPE_MSG) |
		FIELD_PREP(IOMFB_MSG_CONTEXT, id) |
		FIELD_PREP(IOMFB_MSG_OFFSET, offset) |
		FIELD_PREP(IOMFB_MSG_LENGTH, length);
}

static inline u64 dcpep_ack(enum neo_dcp_context_id id)
{
	return dcpep_msg(id, 0, 0) | IOMFB_MSG_ACK;
}

/*
 * A channel is busy if we have sent a message that has yet to be
 * acked. The driver must not sent a message to a busy channel.
 */
static bool neo_dcp_channel_busy(struct neo_dcp_channel *ch)
{
	return (ch->depth != 0);
}

/*
 * Get the context ID passed to the DCP for a command we push. The rule is
 * simple: callback contexts are used when replying to the DCP, command
 * contexts are used otherwise. That corresponds to a non/zero call stack
 * depth. This rule frees the caller from tracking the call context manually.
 */
static enum neo_dcp_context_id neo_dcp_call_context(struct neo_apple_dcp *neo_dcp, bool oob)
{
	u8 depth = oob ? neo_dcp->ch_oobcmd.depth : neo_dcp->ch_cmd.depth;

	if (depth)
		return oob ? DCP_CONTEXT_OOBCB : DCP_CONTEXT_CB;
	else
		return oob ? DCP_CONTEXT_OOBCMD : DCP_CONTEXT_CMD;
}

/* Get a channel for a context */
static struct neo_dcp_channel *neo_dcp_get_channel(struct neo_apple_dcp *neo_dcp,
					   enum neo_dcp_context_id context)
{
	switch (context) {
	case DCP_CONTEXT_CB:
		return &neo_dcp->ch_cb;
	case DCP_CONTEXT_CMD:
		return &neo_dcp->ch_cmd;
	case DCP_CONTEXT_OOBCB:
		return &neo_dcp->ch_oobcb;
	case DCP_CONTEXT_OOBCMD:
		return &neo_dcp->ch_oobcmd;
	case DCP_CONTEXT_ASYNC:
		return &neo_dcp->ch_async;
	case DCP_CONTEXT_OOBASYNC:
		return &neo_dcp->ch_oobasync;
	default:
		return NULL;
	}
}

/* Get the start of a packet: after the end of the previous packet */
static u16 neo_dcp_packet_start(struct neo_dcp_channel *ch, u8 depth)
{
	if (depth > 0)
		return ch->end[depth - 1];
	else
		return 0;
}

/* Pushes and pops the depth of the call stack with safety checks */
static u8 neo_dcp_push_depth(u8 *depth)
{
	u8 ret = (*depth)++;

	WARN_ON(ret >= DCP_MAX_CALL_DEPTH);
	return ret;
}

static u8 neo_dcp_pop_depth(u8 *depth)
{
	WARN_ON((*depth) == 0);

	return --(*depth);
}

/* Older firmware and the H17G method profile retain their existing transport. */
static bool neo_iomfb_uses_queue(struct neo_apple_dcp *neo_dcp)
{
	return neo_dcp->fw_compat == DCP_FIRMWARE_H17P &&
	       neo_dcp->hw.neo_iomfb_method_profile != DCP_IOMFB_METHODS_H17G;
}

static bool neo_iomfb_channels_idle(struct neo_apple_dcp *neo_dcp)
{
	return !neo_dcp->ch_cmd.depth && !neo_dcp->ch_cb.depth &&
	       !neo_dcp->ch_oobcmd.depth && !neo_dcp->ch_oobcb.depth &&
	       !neo_dcp->ch_async.depth && !neo_dcp->ch_oobasync.depth;
}

enum neo_iomfb_opaque_x_state {
	IOMFB_OPAQUE_X_WAITING,
	IOMFB_OPAQUE_X_NEEDED,
	IOMFB_OPAQUE_X_QUEUED,
	IOMFB_OPAQUE_X_READY,
};

struct neo_iomfb_opaque_x_transaction {
	struct neo_iomfb_transaction transaction;
};

static void neo_iomfb_opaque_x_start(struct neo_apple_dcp *neo_dcp,
				 struct neo_iomfb_transaction *transaction);
static void neo_iomfb_opaque_x_release(struct neo_iomfb_transaction *transaction);

static int neo_iomfb_enqueue_opaque_x(struct neo_apple_dcp *neo_dcp)
{
	struct neo_iomfb_opaque_x_transaction *opaque;

	lockdep_assert_held(&neo_dcp->neo_iomfb.lock);
	if (atomic_read(&neo_dcp->neo_iomfb.opaque_x_state) != IOMFB_OPAQUE_X_NEEDED)
		return 0;
	if (neo_dcp->neo_iomfb.queued >= 32)
		return -EBUSY;
	opaque = kzalloc_obj(*opaque);
	if (!opaque)
		return -ENOMEM;
	if (atomic_cmpxchg(&neo_dcp->neo_iomfb.opaque_x_state, IOMFB_OPAQUE_X_NEEDED,
			   IOMFB_OPAQUE_X_QUEUED) != IOMFB_OPAQUE_X_NEEDED) {
		kfree(opaque);
		return 0;
	}

	opaque->transaction.start = neo_iomfb_opaque_x_start;
	opaque->transaction.release = neo_iomfb_opaque_x_release;
	neo_dcp->neo_iomfb.queued++;
	list_add(&opaque->transaction.link, &neo_dcp->neo_iomfb.pending);
	return 0;
}

static void neo_iomfb_discard_pending(struct neo_apple_dcp *neo_dcp)
{
	struct neo_iomfb_transaction *transaction, *next;

	lockdep_assert_held(&neo_dcp->neo_iomfb.lock);
	list_for_each_entry_safe(transaction, next, &neo_dcp->neo_iomfb.pending, link) {
		list_del(&transaction->link);
		neo_dcp->neo_iomfb.queued--;
		if (transaction->brightness_only)
			neo_dcp->neo_iomfb.backlight_queued = false;
		transaction->release(transaction);
	}
}

static void neo_iomfb_queue_advance(struct neo_apple_dcp *neo_dcp)
{
	struct neo_iomfb_transaction *transaction = neo_dcp->neo_iomfb.active;

	lockdep_assert_held(&neo_dcp->neo_iomfb.lock);
	if (READ_ONCE(neo_dcp->crashed) || neo_dcp->neo_iomfb.stopped) {
		neo_iomfb_discard_pending(neo_dcp);
		/* The active operation may still be visible to firmware. */
		return;
	}
	if (!neo_iomfb_channels_idle(neo_dcp) || neo_dcp->present_state_h17p.pending)
		return;

	if (transaction) {
		neo_dcp->neo_iomfb.active = NULL;
		cancel_delayed_work(&neo_dcp->neo_iomfb.timeout);
		if (transaction->brightness_only)
			neo_dcp->neo_iomfb.backlight_queued = false;
		/* A new level requested during a successful present stays pending. */
		if (neo_dcp_backlight_pending(neo_dcp)) {
			if (transaction->completed) {
				schedule_work(&neo_dcp->bl_update_wq);
			} else if (transaction->backlight_failed) {
				unsigned int delay = neo_dcp_backlight_retry_delay(neo_dcp);

				if (delay)
					mod_delayed_work(system_wq, &neo_dcp->neo_iomfb.backlight_retry,
							 msecs_to_jiffies(delay));
				else
					dev_warn_ratelimited(neo_dcp->dev,
							     "backlight retry limit reached\n");
			}
		}
		transaction->release(transaction);
	}
	if (neo_iomfb_enqueue_opaque_x(neo_dcp)) {
		WRITE_ONCE(neo_dcp->crashed, true);
		neo_iomfb_discard_pending(neo_dcp);
		schedule_work(&neo_dcp->vblank_wq);
		return;
	}
	if (!list_empty(&neo_dcp->neo_iomfb.pending))
		schedule_work(&neo_dcp->neo_iomfb.work);
}

static void neo_iomfb_queue_work(struct work_struct *work)
{
	struct neo_apple_dcp *neo_dcp = container_of(work, struct neo_apple_dcp, neo_iomfb.work);
	struct neo_iomfb_transaction *transaction;

	mutex_lock(&neo_dcp->neo_iomfb.lock);
	if (READ_ONCE(neo_dcp->crashed) || neo_dcp->neo_iomfb.stopped) {
		neo_iomfb_discard_pending(neo_dcp);
		goto unlock;
	}
	if (neo_dcp->neo_iomfb.active || !neo_iomfb_channels_idle(neo_dcp) ||
	    neo_dcp->present_state_h17p.pending || list_empty(&neo_dcp->neo_iomfb.pending))
		goto unlock;

	transaction = list_first_entry(&neo_dcp->neo_iomfb.pending,
				       struct neo_iomfb_transaction, link);
	list_del(&transaction->link);
	neo_dcp->neo_iomfb.queued--;
	neo_dcp->neo_iomfb.active = transaction;
	WRITE_ONCE(neo_dcp->neo_iomfb.owner, current);
	neo_dcp->neo_iomfb.deadline = jiffies + msecs_to_jiffies(10000);
	mod_delayed_work(system_wq, &neo_dcp->neo_iomfb.timeout, msecs_to_jiffies(10000));
	transaction->start(neo_dcp, transaction);
	WRITE_ONCE(neo_dcp->neo_iomfb.owner, NULL);
	neo_iomfb_queue_advance(neo_dcp);
unlock:
	mutex_unlock(&neo_dcp->neo_iomfb.lock);
}

static void neo_iomfb_queue_timeout(struct work_struct *work)
{
	struct neo_apple_dcp *neo_dcp = container_of(to_delayed_work(work),
					  struct neo_apple_dcp, neo_iomfb.timeout);

	mutex_lock(&neo_dcp->neo_iomfb.lock);
	if (neo_dcp->neo_iomfb.active && time_before(jiffies, neo_dcp->neo_iomfb.deadline)) {
		mod_delayed_work(system_wq, &neo_dcp->neo_iomfb.timeout,
				 neo_dcp->neo_iomfb.deadline - jiffies);
	} else if (neo_dcp->neo_iomfb.active) {
		WRITE_ONCE(neo_dcp->crashed, true);
		dev_err(neo_dcp->dev, "IOMFB transaction timed out\n");
		neo_iomfb_discard_pending(neo_dcp);
		schedule_work(&neo_dcp->vblank_wq);
	}
	mutex_unlock(&neo_dcp->neo_iomfb.lock);
}

static void neo_iomfb_opaque_x_complete(struct neo_apple_dcp *neo_dcp, void *out,
				    void *cookie)
{
	u32 status = out ? *(u32 *)out : ~0U;

	(void)cookie;

	if (status) {
		dev_err(neo_dcp->dev, "opaque X property failed: %u\n", status);
		WRITE_ONCE(neo_dcp->crashed, true);
		schedule_work(&neo_dcp->vblank_wq);
		return;
	}

	atomic_set(&neo_dcp->neo_iomfb.opaque_x_state, IOMFB_OPAQUE_X_READY);
}

static void neo_iomfb_opaque_x_start(struct neo_apple_dcp *neo_dcp,
				 struct neo_iomfb_transaction *transaction)
{
	(void)transaction;
	neo_iomfb_apply_opaque_x_h17p(neo_dcp, neo_iomfb_opaque_x_complete, NULL);
}

static void neo_iomfb_opaque_x_release(struct neo_iomfb_transaction *transaction)
{
	kfree(container_of(transaction, struct neo_iomfb_opaque_x_transaction,
			   transaction));
}

void neo_iomfb_opaque_x_reset_h17p(struct neo_apple_dcp *neo_dcp)
{
	atomic_set(&neo_dcp->neo_iomfb.opaque_x_state, IOMFB_OPAQUE_X_WAITING);
}

static void neo_iomfb_backlight_retry(struct work_struct *work)
{
	struct neo_apple_dcp *neo_dcp = container_of(to_delayed_work(work), struct neo_apple_dcp,
					  neo_iomfb.backlight_retry);

	if (!READ_ONCE(neo_dcp->crashed) && !READ_ONCE(neo_dcp->neo_iomfb.stopped) &&
	    neo_dcp_backlight_pending(neo_dcp))
		schedule_work(&neo_dcp->bl_update_wq);
}

void neo_iomfb_queue_init(struct neo_apple_dcp *neo_dcp)
{
	mutex_init(&neo_dcp->neo_iomfb.lock);
	INIT_LIST_HEAD(&neo_dcp->neo_iomfb.pending);
	INIT_WORK(&neo_dcp->neo_iomfb.work, neo_iomfb_queue_work);
	INIT_DELAYED_WORK(&neo_dcp->neo_iomfb.timeout, neo_iomfb_queue_timeout);
	INIT_DELAYED_WORK(&neo_dcp->neo_iomfb.backlight_retry, neo_iomfb_backlight_retry);
	atomic_set(&neo_dcp->neo_iomfb.opaque_x_state, IOMFB_OPAQUE_X_WAITING);
}

void neo_iomfb_queue_stop(struct neo_apple_dcp *neo_dcp)
{
	if (!neo_iomfb_uses_queue(neo_dcp))
		return;

	mutex_lock(&neo_dcp->neo_iomfb.lock);
	neo_dcp->neo_iomfb.stopped = true;
	neo_iomfb_discard_pending(neo_dcp);
	mutex_unlock(&neo_dcp->neo_iomfb.lock);
	cancel_work_sync(&neo_dcp->neo_iomfb.work);
	cancel_delayed_work_sync(&neo_dcp->neo_iomfb.timeout);
	cancel_delayed_work_sync(&neo_dcp->neo_iomfb.backlight_retry);
}

/*
 * Wait until every queued transaction, including a present's completion, has
 * finished.  A stopped or crashed queue has nothing left to deliver.
 */
bool neo_iomfb_queue_drain(struct neo_apple_dcp *neo_dcp, unsigned long timeout)
{
	unsigned long deadline = jiffies + timeout;
	bool idle;

	if (!neo_iomfb_uses_queue(neo_dcp))
		return true;

	for (;;) {
		mutex_lock(&neo_dcp->neo_iomfb.lock);
		idle = READ_ONCE(neo_dcp->crashed) || neo_dcp->neo_iomfb.stopped ||
		       (!neo_dcp->neo_iomfb.active && list_empty(&neo_dcp->neo_iomfb.pending) &&
			!neo_dcp->present_state_h17p.pending);
		mutex_unlock(&neo_dcp->neo_iomfb.lock);
		if (idle)
			return true;
		if (time_after(jiffies, deadline))
			return false;
		usleep_range(2000, 4000);
	}
}

int neo_iomfb_queue(struct neo_apple_dcp *neo_dcp, struct neo_iomfb_transaction *transaction)
{
	int ret = 0;

	mutex_lock(&neo_dcp->neo_iomfb.lock);
	if (READ_ONCE(neo_dcp->crashed) || neo_dcp->neo_iomfb.stopped) {
		ret = -EIO;
	} else if (transaction->brightness_only && neo_dcp->neo_iomfb.backlight_queued) {
		ret = -EALREADY;
	} else if (neo_dcp->neo_iomfb.queued >= 32) {
		ret = -EBUSY;
	} else {
		if (transaction->brightness_only)
			neo_dcp->neo_iomfb.backlight_queued = true;
		neo_dcp->neo_iomfb.queued++;
		list_add_tail(&transaction->link, &neo_dcp->neo_iomfb.pending);
		schedule_work(&neo_dcp->neo_iomfb.work);
	}
	mutex_unlock(&neo_dcp->neo_iomfb.lock);
	return ret;
}

struct neo_iomfb_crc_transaction {
	struct neo_iomfb_transaction transaction;
	u32 swap_id;
};

static void neo_iomfb_crc_complete(struct neo_apple_dcp *neo_dcp, void *out, void *cookie)
{
	struct neo_iomfb_crc_transaction *crc = cookie;
	struct neo_dcp_packet_header *header;
	u32 value;

	/* A106 has no input and a single little-endian blend-output CRC. */
	if (!out)
		goto invalid;
	header = out - sizeof(*header);
	if (header->in_len || header->out_len != sizeof(__le32))
		goto invalid;
	value = get_unaligned_le32(out);
	if (READ_ONCE(neo_dcp->crc_enabled))
		drm_crtc_add_crc_entry(&neo_dcp->crtc->base, true, crc->swap_id, &value);
	return;

invalid:
	dev_err(neo_dcp->dev, "invalid blend CRC response\n");
	WRITE_ONCE(neo_dcp->crashed, true);
}

static void neo_iomfb_crc_start(struct neo_apple_dcp *neo_dcp,
			    struct neo_iomfb_transaction *transaction)
{
	static const struct neo_dcp_method_entry method = {
		.tag = { 'A', '1', '0', '6' },
		.name = "read_blend_crc",
	};
	struct neo_iomfb_crc_transaction *crc = container_of(transaction,
						       struct neo_iomfb_crc_transaction,
						       transaction);

	if (READ_ONCE(neo_dcp->crc_enabled))
		neo_dcp_push(neo_dcp, false, &method, 0, sizeof(__le32), NULL,
			 neo_iomfb_crc_complete, crc);
}

static void neo_iomfb_crc_release(struct neo_iomfb_transaction *transaction)
{
	kfree(container_of(transaction, struct neo_iomfb_crc_transaction, transaction));
}

void neo_iomfb_queue_crc_h17p(struct neo_apple_dcp *neo_dcp, u32 swap_id)
{
	struct neo_iomfb_crc_transaction *crc;

	lockdep_assert_held(&neo_dcp->neo_iomfb.lock);
	if (!READ_ONCE(neo_dcp->crc_enabled) || READ_ONCE(neo_dcp->crashed) ||
	    neo_dcp->neo_iomfb.stopped || neo_dcp->neo_iomfb.queued >= 32)
		return;
	crc = kzalloc_obj(*crc);
	if (!crc)
		return;
	crc->swap_id = swap_id;
	crc->transaction.start = neo_iomfb_crc_start;
	crc->transaction.release = neo_iomfb_crc_release;
	/* Read the completed frame before another queued present changes it. */
	list_add(&crc->transaction.link, &neo_dcp->neo_iomfb.pending);
	neo_dcp->neo_iomfb.queued++;
	/* The receiver advances the queue only after acknowledging completion. */
}

struct neo_iomfb_command {
	struct neo_iomfb_transaction transaction;
	struct neo_dcp_method_entry method;
	neo_dcp_callback_t callback;
	void *cookie;
	u32 in_len;
	u32 out_len;
	bool oob;
	u8 data[];
};

static void neo_iomfb_command_start(struct neo_apple_dcp *neo_dcp,
				struct neo_iomfb_transaction *transaction)
{
	struct neo_iomfb_command *command = container_of(transaction,
						    struct neo_iomfb_command, transaction);

	neo_dcp_push(neo_dcp, command->oob, &command->method, command->in_len,
		 command->out_len, command->data, command->callback, command->cookie);
}

static void neo_iomfb_command_release(struct neo_iomfb_transaction *transaction)
{
	kfree(container_of(transaction, struct neo_iomfb_command, transaction));
}

static int neo_iomfb_queue_command(struct neo_apple_dcp *neo_dcp, bool oob,
			       const struct neo_dcp_method_entry *method,
			       u32 in_len, u32 out_len, void *data,
			       neo_dcp_callback_t callback, void *cookie)
{
	struct neo_iomfb_command *command;
	int ret;

	if ((u64)sizeof(struct neo_dcp_packet_header) + in_len + out_len > 0x8000)
		return -EMSGSIZE;
	command = kzalloc(struct_size(command, data, in_len), GFP_KERNEL);
	if (!command)
		return -ENOMEM;
	command->transaction.start = neo_iomfb_command_start;
	command->transaction.release = neo_iomfb_command_release;
	command->method = *method;
	command->callback = callback;
	command->cookie = cookie;
	command->in_len = in_len;
	command->out_len = out_len;
	command->oob = oob;
	if (in_len)
		memcpy(command->data, data, in_len);
	ret = neo_iomfb_queue(neo_dcp, &command->transaction);
	if (ret)
		neo_iomfb_command_release(&command->transaction);
	return ret;
}

/* Call a DCP function given by a tag */
void neo_dcp_push(struct neo_apple_dcp *neo_dcp, bool oob, const struct neo_dcp_method_entry *call,
		     u32 in_len, u32 out_len, void *data, neo_dcp_callback_t cb,
		     void *cookie)
{
	struct neo_dcp_method_entry resolved = *call;

	/* Reply chains stay on the serialized receiver; callers enqueue copies. */
	if (neo_iomfb_uses_queue(neo_dcp) && READ_ONCE(neo_dcp->neo_iomfb.owner) != current) {
		if (neo_iomfb_queue_command(neo_dcp, oob, call, in_len, out_len,
					data, cb, cookie))
			WRITE_ONCE(neo_dcp->crashed, true);
		return;
	}

	if (neo_dcp->fw_compat == DCP_FIRMWARE_H17P && READ_ONCE(neo_dcp->crashed))
		return;

	if (neo_dcp->hw.neo_iomfb_method_profile == DCP_IOMFB_METHODS_H17G &&
	    call->tag_h17g[0])
		memcpy(resolved.tag, call->tag_h17g, sizeof(resolved.tag));
	call = &resolved;

	enum neo_dcp_context_id context = neo_dcp_call_context(neo_dcp, oob);
	struct neo_dcp_channel *ch = neo_dcp_get_channel(neo_dcp, context);

	struct neo_dcp_packet_header header = {
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

	if (neo_iomfb_uses_queue(neo_dcp) &&
	    (!ch || ch->depth >= DCP_MAX_CALL_DEPTH ||
	     (u64)sizeof(header) + in_len + out_len > 0x8000 ||
	     neo_dcp_packet_start(ch, ch->depth) >
		0x8000 - ALIGN(sizeof(header) + in_len + out_len,
			       DCP_PACKET_ALIGNMENT))) {
		dev_err(neo_dcp->dev, "invalid IOMFB command envelope\n");
		WRITE_ONCE(neo_dcp->crashed, true);
		return;
	}
	depth = neo_dcp_push_depth(&ch->depth);
	offset = neo_dcp_packet_start(ch, depth);

	void *out = neo_dcp->shmem + neo_dcp_tx_offset(context) + offset;
	void *out_data = out + sizeof(header);
	size_t data_len = sizeof(header) + in_len + out_len;

	memcpy(out, &header, sizeof(header));

	if (in_len > 0)
		memcpy(out_data, data, in_len);
	/* An unwritten status must not look like a successful response. */
	if (out_len)
		memset(out_data + in_len, 0xff, out_len);

	trace_neo_iomfb_push(neo_dcp, call, context, offset, depth);

	ch->callbacks[depth] = cb;
	ch->cookies[depth] = cookie;
	ch->output[depth] = out + sizeof(header) + in_len;
	ch->header[depth] = header;
	ch->end[depth] = offset + ALIGN(data_len, DCP_PACKET_ALIGNMENT);

	neo_dcp_send_message(neo_dcp, IOMFB_ENDPOINT,
			 dcpep_msg(context, data_len, offset));
}

/* Parse a callback tag "D123" into the ID 123. Returns -EINVAL on failure. */
int neo_dcp_parse_tag(char tag[4])
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
void neo_dcp_ack(struct neo_apple_dcp *neo_dcp, enum neo_dcp_context_id context)
{
	struct neo_dcp_channel *ch = neo_dcp_get_channel(neo_dcp, context);

	neo_dcp_pop_depth(&ch->depth);
	neo_dcp_send_message(neo_dcp, IOMFB_ENDPOINT,
			 dcpep_ack(context));
}

/*
 * Helper to send a DRM hotplug event. The DCP is accessed from a single
 * (RTKit) thread. To handle hotplug callbacks, we need to call
 * drm_kms_helper_hotplug_event, which does an atomic commit (via DCP) and
 * waits for vblank (a DCP callback). That means we deadlock if we call from
 * the RTKit thread! Instead, move the call to another thread via a workqueue.
 */
static int neo_dcp_retrain_active_crtc(struct neo_apple_connector *connector)
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

void neo_dcp_handle_hotplug_actions(struct neo_apple_dcp *neo_dcp, unsigned int action)
{
	if (action & DCP_HOTPLUG_VBLANK)
		schedule_work(&neo_dcp->vblank_wq);
	if ((action & DCP_HOTPLUG_NOTIFY) && neo_dcp->connector)
		schedule_work(&neo_dcp->connector->hotplug_wq);
}

void neo_dcp_retrain_oob(struct neo_apple_connector *connector)
{
	struct platform_device *pdev = READ_ONCE(connector->neo_dcp);
	struct neo_apple_dcp *neo_dcp;

	/* a Type-C port with no pipeline behind it has nothing to retrain */
	if (!pdev || !READ_ONCE(connector->connected))
		return;
	neo_dcp = platform_get_drvdata(pdev);

	/*
	 * Bringing up another high-speed Type-C route can disturb an active DPTX
	 * stream without changing its HPD state.  Invalidate the IOMFB mode and
	 * use the normal hotplug worker to replay the active CRTC from process
	 * context; sending a synthetic disconnect would tear down the connector.
	 */
	neo_dcp_mode_invalidate(&neo_dcp->mode_state);
	schedule_work(&connector->hotplug_wq);
}

void neo_dcp_hotplug(struct work_struct *work)
{
	struct neo_apple_connector *connector;
	struct platform_device *pdev;
	struct neo_apple_dcp *neo_dcp;
	int ret;

	connector = container_of(work, struct neo_apple_connector, hotplug_wq);

	pdev = READ_ONCE(connector->neo_dcp);
	if (!pdev) {	/* a Type-C port unrouted after this was queued */
		drm_kms_helper_connector_hotplug_event(&connector->base);
		return;
	}
	neo_dcp = platform_get_drvdata(pdev);
	dev_info(neo_dcp->dev, "%s() connected:%d valid_mode:%d nr_modes:%u\n", __func__,
		 connector->connected, READ_ONCE(neo_dcp->mode_state.valid), neo_dcp->nr_modes);

	if (!connector->connected) {
		drm_edid_free(connector->drm_edid);
		connector->drm_edid = NULL;
	}

	/*
	 * DCP defers link training until we set a display mode. But we set
	 * display modes from atomic_flush, so userspace needs to trigger a
	 * flush, or the CRTC gets no signal.
	 */
	if (connector->base.state && !READ_ONCE(neo_dcp->mode_state.valid) && connector->connected &&
	    !(neo_dcp_is_usb4_output(neo_dcp) && false)) {
		drm_connector_set_link_status_property(&connector->base,
						       DRM_MODE_LINK_STATUS_BAD);

		/*
		 * A short Type-C route interruption can leave the DRM CRTC active
		 * while DCP has discarded its display mode.  Userspace is then free
		 * to keep submitting plane-only commits, none of which retrains the
		 * link.  Re-apply the active CRTC state once the DPTX link-config
		 * callback has completed.
		 */
		ret = neo_dcp_retrain_active_crtc(connector);
		if (ret)
			dev_warn(neo_dcp->dev,
				 "failed to retrain active CRTC after hotplug: %d\n",
				 ret);
	}

	drm_kms_helper_connector_hotplug_event(&connector->base);
}

static void dcpep_handle_cb(struct neo_apple_dcp *neo_dcp, enum neo_dcp_context_id context,
			    void *data, u32 length, u16 offset)
{
	struct device *dev = neo_dcp->dev;
	struct neo_dcp_packet_header *hdr = data;
	void *in, *out;
	int tag;
	struct neo_dcp_channel *ch = neo_dcp_get_channel(neo_dcp, context);
	u8 depth;
	bool handled;

	if (neo_dcp->fw_compat == DCP_FIRMWARE_H17P &&
	    (length < sizeof(*hdr) || !ch || ch->depth >= DCP_MAX_CALL_DEPTH ||
	     offset >= 0x8000 || length > 0x8000 - offset)) {
		dev_err(dev, "invalid IOMFB callback envelope\n");
		neo_dcp->crashed = true;
		return;
	}

	tag = neo_dcp_parse_tag(hdr->tag);
	handled = tag >= 0 && tag < IOMFB_MAX_CB && neo_dcp->cb_handlers &&
		  neo_dcp->cb_handlers[tag];
	if (neo_dcp->fw_compat == DCP_FIRMWARE_H17P &&
	    (tag < 0 || tag >= IOMFB_MAX_CB ||
	     (u64)sizeof(*hdr) + hdr->in_len + hdr->out_len != length ||
	     (neo_dcp->hw.neo_iomfb_method_profile != DCP_IOMFB_METHODS_H17G &&
	      handled &&
	      !neo_iomfb_check_callback_h17p(neo_dcp, tag, hdr->in_len, hdr->out_len)))) {
		dev_err(dev, "unqualified IOMFB callback %d (%u, %u)\n",
			tag, hdr->in_len, hdr->out_len);
		neo_dcp->crashed = true;
		return;
	}

	in = data + sizeof(*hdr);
	out = in + hdr->in_len;

	if (!handled) {
		if (!neo_iomfb_uses_queue(neo_dcp) ||
		    !test_and_set_bit(tag, neo_dcp->unknown_callbacks))
			dev_warn(dev, "received unknown callback %c%c%c%c\n",
				 hdr->tag[3], hdr->tag[2], hdr->tag[1], hdr->tag[0]);
		/*
		 * Leaving a callback unanswered wedges the coprocessor: it
		 * waits for the ack forever and the outer call never returns.
		 * On H17P answer it with a zeroed output instead.
		 */
		if (neo_dcp->fw_compat == DCP_FIRMWARE_H17P) {
			if (hdr->out_len)
				memset(out, 0, hdr->out_len);
			depth = neo_dcp_push_depth(&ch->depth);
			ch->output[depth] = out;
			ch->end[depth] = offset +
					 ALIGN(length, DCP_PACKET_ALIGNMENT);
			neo_dcp_ack(neo_dcp, context);
		}
		return;
	}

	// TODO: verify that in_len and out_len match our prototypes
	// for now just clear the out data to have at least consistent results
	if (hdr->out_len)
		memset(out, 0, hdr->out_len);

	depth = neo_dcp_push_depth(&ch->depth);
	ch->output[depth] = out;
	ch->end[depth] = offset + ALIGN(length, DCP_PACKET_ALIGNMENT);

	if (neo_dcp->cb_handlers[tag](neo_dcp, tag, out, in))
		neo_dcp_ack(neo_dcp, context);
}

static void dcpep_handle_ack(struct neo_apple_dcp *neo_dcp, enum neo_dcp_context_id context)
{
	struct neo_dcp_channel *ch = neo_dcp_get_channel(neo_dcp, context);
	const struct neo_dcp_packet_header *record, *sent;
	void *cookie, *out;
	neo_dcp_callback_t cb;

	if (!ch) {
		dev_warn(neo_dcp->dev, "ignoring ack on context %X\n", context);
		return;
	}

	if (!ch->depth) {
		dev_warn(neo_dcp->dev, "ignoring ack on idle context %X\n", context);
		return;
	}

	neo_dcp_pop_depth(&ch->depth);

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
	if (neo_dcp->fw_compat != DCP_FIRMWARE_H17P) {
		record = out - sent->in_len - sizeof(*record);
		if (record->in_len != sent->in_len ||
		    record->out_len != sent->out_len) {
			dev_err(neo_dcp->dev,
				"corrupt %c%c%c%c command record on context %X\n",
				sent->tag[3], sent->tag[2], sent->tag[1],
				sent->tag[0], context);
			WRITE_ONCE(neo_dcp->crashed, true);
			/*
			 * Still complete the command, without an output, so
			 * that its callback drops the cookie reference and
			 * wakes the waiter instead of leaving it to time out.
			 */
			out = NULL;
		}
	}

	if (cb)
		cb(neo_dcp, out, cookie);
}

static void dcpep_got_msg(struct neo_apple_dcp *neo_dcp, u64 message)
{
	enum neo_dcp_context_id ctx_id;
	u16 offset;
	u32 length;
	int channel_offset;
	void *data;

	ctx_id = FIELD_GET(IOMFB_MSG_CONTEXT, message);
	offset = FIELD_GET(IOMFB_MSG_OFFSET, message);
	length = FIELD_GET(IOMFB_MSG_LENGTH, message);

	channel_offset = neo_dcp_channel_offset(ctx_id);

	if (channel_offset < 0) {
		dev_warn(neo_dcp->dev, "invalid context received %u\n", ctx_id);
		return;
	}

	data = neo_dcp->shmem + channel_offset + offset;

	if (FIELD_GET(IOMFB_MSG_ACK, message))
		dcpep_handle_ack(neo_dcp, ctx_id);
	else
		dcpep_handle_cb(neo_dcp, ctx_id, data, length, offset);
}

int neo_dcp_get_modes(struct drm_connector *connector)
{
	struct neo_apple_connector *neo_apple_connector = to_apple_connector(connector);
	struct platform_device *pdev = READ_ONCE(neo_apple_connector->neo_dcp);
	struct neo_apple_dcp *neo_dcp;

	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;
	u16 min_vfreq = 0, max_vfreq = 0;
	bool vrr_capable = false;
	int i;

	/* A Type-C port has no pipeline while the fabric is moving it. */
	if (!pdev)
		return 0;
	neo_dcp = platform_get_drvdata(pdev);

	for (i = 0; i < neo_dcp->nr_modes; ++i) {
		if (neo_dcp->modes[i].vrr) {
			u16 lo = neo_dcp->modes[i].min_vrr >> 16;
			u16 hi = neo_dcp->modes[i].max_vrr >> 16;

			if (!min_vfreq || lo < min_vfreq)
				min_vfreq = lo;
			if (hi > max_vfreq)
				max_vfreq = hi;
		}
		vrr_capable |= neo_dcp->modes[i].vrr;
		mode = drm_mode_duplicate(dev, &neo_dcp->modes[i].mode);

		if (!mode) {
			dev_err(dev->dev, "Failed to duplicate display mode\n");
			return 0;
		}

		drm_mode_probed_add(connector, mode);
	}
	drm_connector_set_vrr_capable_property(connector, vrr_capable);

	/* H17P sends no EPIC commands; see afk_send_epic(). */
	if (neo_dcp->nr_modes && neo_dcp->fw_compat != DCP_FIRMWARE_H17P &&
	    !neo_apple_connector->drm_edid) {
		const struct drm_edid *edid;
		edid = dcpavserv_copy_edid(neo_dcp);
		if (IS_ERR_OR_NULL(edid)) {
			/*
			 * An internal panel has no AV service and so no EDID;
			 * do not report that on every mode probe.
			 */
			if (neo_dcp_has_panel(neo_dcp) && PTR_ERR(edid) == -ENODEV)
				dev_dbg(neo_dcp->dev, "no EDID source for the panel\n");
			else
				dev_info(neo_dcp->dev, "copy_edid failed: %pe\n", edid);
		} else {
			drm_edid_free(neo_apple_connector->drm_edid);
			neo_apple_connector->drm_edid = edid;
		}
	}
	if (neo_dcp->nr_modes && neo_apple_connector->drm_edid) {
		drm_edid_connector_update(connector, neo_apple_connector->drm_edid);
		neo_dcp_retry_placeholder_edid(neo_dcp, neo_apple_connector->drm_edid);
	}

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

	return neo_dcp->nr_modes;
}

/* The user may own drm_display_mode, so we need to search for our copy */
struct neo_dcp_display_mode *lookup_mode(struct neo_apple_dcp *neo_dcp,
					    const struct drm_display_mode *mode)
{
	int i;

	for (i = 0; i < neo_dcp->nr_modes; ++i) {
		if (drm_mode_match(mode, &neo_dcp->modes[i].mode,
				   DRM_MODE_MATCH_TIMINGS |
					   DRM_MODE_MATCH_CLOCK))
			return &neo_dcp->modes[i];
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
static bool neo_dcp_mode_settable(struct neo_apple_dcp *neo_dcp,
			      const struct drm_display_mode *mode)
{
	struct neo_dcp_display_mode *neo_dcp_mode = lookup_mode(neo_dcp, mode);

	if (!neo_dcp_mode)
		return false;

	if (neo_dcp->fw_compat == DCP_FIRMWARE_H17P)
		return neo_dcp_mode->mode.type & DRM_MODE_TYPE_PREFERRED;

	return true;
}

enum drm_mode_status neo_dcp_mode_valid(struct drm_connector *connector,
				    const struct drm_display_mode *mode)
{
	struct neo_apple_connector *neo_apple_connector = to_apple_connector(connector);
	struct platform_device *pdev = READ_ONCE(neo_apple_connector->neo_dcp);
	struct neo_apple_dcp *neo_dcp;

	if (!pdev)
		return MODE_ERROR;
	neo_dcp = platform_get_drvdata(pdev);

	return neo_dcp_mode_settable(neo_dcp, mode) ? MODE_OK : MODE_BAD;
}

int neo_dcp_crtc_atomic_modeset(struct drm_crtc *crtc,
			    struct drm_atomic_state *state)
{
	struct neo_apple_crtc *neo_apple_crtc = to_apple_crtc(crtc);
	struct neo_apple_dcp *neo_dcp = platform_get_drvdata(neo_apple_crtc->neo_dcp);
	struct drm_crtc_state *crtc_state;
	unsigned int action;
	int ret = -EIO;
	bool modeset;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (!crtc_state)
		return 0;

	modeset = drm_atomic_crtc_needs_modeset(crtc_state) ||
		  !READ_ONCE(neo_dcp->mode_state.valid);

	if (!modeset)
		return 0;

	/* ignore no mode, poweroff is handled elsewhere */
	if (crtc_state->mode.hdisplay == 0 && crtc_state->mode.vdisplay == 0)
		return 0;

	neo_dcp_mode_begin(&neo_dcp->mode_state);
	switch (neo_dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		ret = neo_iomfb_modeset_v12_3(neo_dcp, crtc_state);
		break;
	case DCP_FIRMWARE_V_13_5:
		ret = neo_iomfb_modeset_v13_3(neo_dcp, crtc_state);
		break;
	case DCP_FIRMWARE_H17P:
		ret = neo_iomfb_modeset_h17p(neo_dcp, crtc_state);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n",
			  neo_dcp->fw_compat);
		break;
	}

	action = neo_dcp_mode_finish(&neo_dcp->mode_state, !ret, neo_dcp->connector ?
				&neo_dcp->connector->connected : NULL);
	neo_dcp_handle_hotplug_actions(neo_dcp, action);

	return ret;
}

bool neo_dcp_crtc_mode_fixup(struct drm_crtc *crtc,
			 const struct drm_display_mode *mode,
			 struct drm_display_mode *adjusted_mode)
{
	struct neo_apple_crtc *neo_apple_crtc = to_apple_crtc(crtc);
	struct platform_device *pdev = neo_apple_crtc->neo_dcp;
	struct neo_apple_dcp *neo_dcp = platform_get_drvdata(pdev);

	/* TODO: support synthesized modes through scaling */
	return neo_dcp_mode_settable(neo_dcp, mode);
}


static void neo_iomfb_scanout_release_h17p(struct neo_iomfb_scanout_h17p *scanout)
{
	unsigned int i;

	if (!scanout)
		return;
	for (i = 0; i < SWAP_SURFACES; i++)
		if (scanout->fb[i])
			drm_framebuffer_put(scanout->fb[i]);
	kfree(scanout);
}

static struct neo_iomfb_scanout_h17p *
neo_iomfb_scanout_prepare_h17p(struct neo_apple_dcp *neo_dcp, struct drm_crtc *crtc,
			   struct drm_atomic_state *state)
{
	struct neo_iomfb_scanout_h17p *scanout;
	struct drm_plane *plane;
	struct drm_plane_state *old_state, *new_state;
	unsigned int i, slot;
	int index;

	lockdep_assert_held(&neo_dcp->neo_iomfb.lock);
	scanout = kzalloc_obj(*scanout);
	if (!scanout)
		return NULL;
	if (neo_dcp->neo_iomfb.scanout) {
		scanout->request = neo_dcp->neo_iomfb.scanout->request;
		for (i = 0; i < SWAP_SURFACES; i++) {
			scanout->fb[i] = neo_dcp->neo_iomfb.scanout->fb[i];
			if (scanout->fb[i])
				drm_framebuffer_get(scanout->fb[i]);
		}
	}

	for_each_oldnew_plane_in_state(state, plane, old_state, new_state, index) {
		if (old_state->crtc != crtc && new_state->crtc != crtc)
			continue;
		slot = to_apple_plane(plane)->neo_iomfb_surf;
		if (slot >= SWAP_SURFACES) {
			neo_iomfb_scanout_release_h17p(scanout);
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

void neo_iomfb_scanout_complete_h17p(struct neo_apple_dcp *neo_dcp)
{
	struct neo_iomfb_scanout_h17p *scanout = neo_dcp->neo_iomfb.next_scanout;
	struct neo_iomfb_scanout_h17p *previous = neo_dcp->neo_iomfb.scanout;
	struct neo_dcp_swap_submit_req_h17p *request = &neo_dcp->swap.h17p;
	unsigned int i;

	lockdep_assert_held(&neo_dcp->neo_iomfb.lock);
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
	neo_dcp->neo_iomfb.scanout = scanout;
	neo_dcp->neo_iomfb.next_scanout = NULL;
	neo_iomfb_scanout_release_h17p(previous);
}

bool neo_iomfb_present_brightness_only_h17p(struct neo_apple_dcp *neo_dcp)
{
	return neo_dcp->neo_iomfb.active && neo_dcp->neo_iomfb.active->brightness_only;
}

bool neo_iomfb_apply_backlight_h17p(struct neo_apple_dcp *neo_dcp,
				const struct neo_dcp_swap_submit_req_h17p *request,
				struct neo_dcp_present_h17p *wire)
{
	struct neo_iomfb_transaction *transaction = neo_dcp->neo_iomfb.active;
	struct neo_dcp_backlight_present present;
	bool have_surface = false;
	unsigned int i;
	int ret;

	lockdep_assert_held(&neo_dcp->neo_iomfb.lock);
	if (!neo_dcp_backlight_active(neo_dcp)) {
		if (!neo_dcp_has_panel(neo_dcp))
			return true;
		if (!neo_dcp->brightness.maximum)
			return false;
		neo_iomfb_encode_backlight_h17p(wire, 0,
					    neo_dcp->brightness.maximum, false);
		return true;
	}
	if (!transaction || !neo_dcp->brightness.maximum)
		return false;
	for (i = 0; i < SWAP_SURFACES; i++)
		have_surface |= !request->surf_null[i];
	ret = neo_dcp_backlight_prepare(neo_dcp, have_surface, &present);
	if (ret && ret != -EALREADY)
		return false;
	if (!ret) {
		transaction->backlight = present;
		transaction->backlight_reserved = true;
	}
	neo_iomfb_encode_backlight_h17p(wire, present.nits,
				    neo_dcp->brightness.maximum, !ret);
	return true;
}

void neo_iomfb_present_failed_h17p(struct neo_apple_dcp *neo_dcp)
{
	struct neo_iomfb_transaction *transaction = neo_dcp->neo_iomfb.active;

	if (transaction && transaction->backlight_reserved) {
		neo_dcp_backlight_complete(neo_dcp, transaction->backlight.sequence, false);
		transaction->backlight_reserved = false;
		transaction->backlight_failed = true;
	}
}

bool neo_iomfb_present_complete_h17p(struct neo_apple_dcp *neo_dcp)
{
	struct neo_iomfb_transaction *transaction = neo_dcp->neo_iomfb.active;

	lockdep_assert_held(&neo_dcp->neo_iomfb.lock);
	if (transaction) {
		transaction->completed = true;
		if (transaction->backlight_reserved) {
			neo_dcp_backlight_complete(neo_dcp, transaction->backlight.sequence, true);
			transaction->backlight_reserved = false;
		}
		if (transaction->brightness_only)
			return false;
	}
	atomic_cmpxchg(&neo_dcp->neo_iomfb.opaque_x_state, IOMFB_OPAQUE_X_WAITING,
		       IOMFB_OPAQUE_X_NEEDED);
	neo_iomfb_scanout_complete_h17p(neo_dcp);
	return true;
}

static void neo_iomfb_backlight_start(struct neo_apple_dcp *neo_dcp,
				  struct neo_iomfb_transaction *transaction)
{
	struct neo_iomfb_scanout_h17p *scanout = neo_dcp->neo_iomfb.scanout;
	bool have_surface = false;
	unsigned int i;

	if (!neo_dcp_backlight_pending(neo_dcp) || !READ_ONCE(neo_dcp->mode_state.valid) || !scanout ||
	    !neo_dcp->connector || !neo_dcp->connector->connected)
		return;
	for (i = 0; i < SWAP_SURFACES; i++)
		have_surface |= !!scanout->fb[i];
	if (!have_surface)
		return;

	neo_dcp->swap.h17p = scanout->request;
	neo_iomfb_present_backlight_h17p(neo_dcp);
}

static void neo_iomfb_backlight_release(struct neo_iomfb_transaction *transaction)
{
	kfree(transaction);
}

static void neo_iomfb_backlight_kick(struct neo_apple_dcp *neo_dcp)
{
	struct neo_iomfb_transaction *transaction;

	transaction = kzalloc_obj(*transaction);
	if (!transaction)
		return;
	transaction->start = neo_iomfb_backlight_start;
	transaction->release = neo_iomfb_backlight_release;
	transaction->brightness_only = true;
	if (neo_iomfb_queue(neo_dcp, transaction))
		kfree(transaction);
}

int neo_iomfb_configure_backlight_h17p(struct neo_apple_dcp *neo_dcp, u32 maximum,
				   bool inherited_valid, u32 inherited,
				   bool default_valid, u32 default_nits)
{
	if (!neo_iomfb_uses_queue(neo_dcp))
		return -EINVAL;
	return neo_dcp_backlight_configure(neo_dcp, maximum, inherited_valid, inherited,
				       default_valid, default_nits, neo_iomfb_backlight_kick);
}

struct neo_iomfb_atomic_transaction {
	struct neo_iomfb_transaction transaction;
	struct drm_atomic_state *state;
	struct drm_crtc *crtc;
	struct neo_apple_dcp *neo_dcp;
	struct neo_iomfb_scanout_h17p *scanout;
};

static void neo_iomfb_atomic_start(struct neo_apple_dcp *neo_dcp,
			       struct neo_iomfb_transaction *transaction)
{
	struct neo_iomfb_atomic_transaction *atomic = container_of(transaction,
					 struct neo_iomfb_atomic_transaction, transaction);

	if (!READ_ONCE(neo_dcp->mode_state.valid) || !neo_dcp->connector || !neo_dcp->connector->connected) {
		schedule_work(&neo_dcp->vblank_wq);
		return;
	}
	atomic->scanout = neo_iomfb_scanout_prepare_h17p(neo_dcp, atomic->crtc, atomic->state);
	if (!atomic->scanout) {
		WRITE_ONCE(neo_dcp->crashed, true);
		schedule_work(&neo_dcp->vblank_wq);
		return;
	}
	neo_dcp->neo_iomfb.next_scanout = atomic->scanout;
	neo_iomfb_flush_h17p(neo_dcp, atomic->crtc, atomic->state);
}

static void neo_iomfb_atomic_release(struct neo_iomfb_transaction *transaction)
{
	struct neo_iomfb_atomic_transaction *atomic = container_of(transaction,
					 struct neo_iomfb_atomic_transaction, transaction);

	if (atomic->scanout && atomic->neo_dcp->neo_iomfb.scanout != atomic->scanout) {
		if (atomic->neo_dcp->neo_iomfb.next_scanout == atomic->scanout)
			atomic->neo_dcp->neo_iomfb.next_scanout = NULL;
		neo_iomfb_scanout_release_h17p(atomic->scanout);
	}
	drm_atomic_state_put(atomic->state);
	kfree(atomic);
}

static void neo_iomfb_queue_atomic(struct neo_apple_dcp *neo_dcp, struct drm_crtc *crtc,
			       struct drm_atomic_state *state)
{
	struct neo_iomfb_atomic_transaction *atomic;

	atomic = kzalloc_obj(*atomic);
	if (!atomic)
		goto failed;
	atomic->transaction.start = neo_iomfb_atomic_start;
	atomic->transaction.release = neo_iomfb_atomic_release;
	atomic->state = drm_atomic_state_get(state);
	atomic->crtc = crtc;
	atomic->neo_dcp = neo_dcp;
	if (!neo_iomfb_queue(neo_dcp, &atomic->transaction))
		return;
	neo_iomfb_atomic_release(&atomic->transaction);
failed:
	WRITE_ONCE(neo_dcp->crashed, true);
	schedule_work(&neo_dcp->vblank_wq);
}

void neo_dcp_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct platform_device *pdev = to_apple_crtc(crtc)->neo_dcp;
	struct neo_apple_dcp *neo_dcp = platform_get_drvdata(pdev);

	if (neo_iomfb_uses_queue(neo_dcp)) {
		neo_iomfb_queue_atomic(neo_dcp, crtc, state);
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
	if (!READ_ONCE(neo_dcp->mode_state.valid) || !neo_dcp->connector || !neo_dcp->connector->connected) {
		schedule_work(&neo_dcp->vblank_wq);
		return;
	}

	if (neo_dcp_channel_busy(&neo_dcp->ch_cmd))
	{
		if (!neo_dcp->ch_cmd.warned_busy) {
			dev_err(neo_dcp->dev, "unexpected busy command channel\n");
			neo_dcp->ch_cmd.warned_busy = true;
		}
		/* HACK: issue a delayed vblank event to avoid timeouts in
		 * drm_atomic_helper_wait_for_vblanks().
		 */
		schedule_work(&neo_dcp->vblank_wq);
		return;
	} else if (neo_dcp->ch_cmd.warned_busy) {
		neo_dcp->ch_cmd.warned_busy = false;
	}

	switch (neo_dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		neo_iomfb_flush_v12_3(neo_dcp, crtc, state);
		break;
	case DCP_FIRMWARE_V_13_5:
		neo_iomfb_flush_v13_3(neo_dcp, crtc, state);
		break;
	case DCP_FIRMWARE_H17P:
		neo_iomfb_flush_h17p(neo_dcp, crtc, state);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", neo_dcp->fw_compat);
		break;
	}
}

static void neo_iomfb_start(struct neo_apple_dcp *neo_dcp)
{
	switch (neo_dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		neo_iomfb_start_v12_3(neo_dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		neo_iomfb_start_v13_3(neo_dcp);
		break;
	case DCP_FIRMWARE_H17P:
		neo_iomfb_start_h17p(neo_dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", neo_dcp->fw_compat);
		break;
	}
}

bool neo_dcp_is_initialized(struct platform_device *pdev)
{
	struct neo_apple_dcp *neo_dcp = platform_get_drvdata(pdev);

	return neo_dcp->active;
}

static void neo_iomfb_recv_message(struct neo_apple_dcp *neo_dcp, u64 message)
{
	enum dcpep_type type = FIELD_GET(IOMFB_MESSAGE_TYPE, message);

	if (neo_dcp->fw_compat == DCP_FIRMWARE_H17P && READ_ONCE(neo_dcp->crashed))
		return;

	if (type == IOMFB_MESSAGE_TYPE_INITIALIZED) {
		/*
		 * H17P reports its interface version (bits 63:48) and a
		 * firmware hash in the high half of the InitComplete word.
		 * The transport below it is unchanged.
		 */
		if (neo_dcp->fw_compat == DCP_FIRMWARE_H17P)
			dev_info(neo_dcp->dev,
				 "IOMFB: init complete %#llx (version %llu)\n",
				 message,
				 FIELD_GET(GENMASK_ULL(63, 48), message));
		neo_iomfb_start(neo_dcp);
	} else if (type == IOMFB_MESSAGE_TYPE_MSG)
		dcpep_got_msg(neo_dcp, message);
	else
		dev_warn(neo_dcp->dev, "Ignoring unknown message %llx\n", message);
}

void neo_iomfb_recv_msg(struct neo_apple_dcp *neo_dcp, u64 message)
{
	if (!neo_iomfb_uses_queue(neo_dcp)) {
		neo_iomfb_recv_message(neo_dcp, message);
		return;
	}

	mutex_lock(&neo_dcp->neo_iomfb.lock);
	WRITE_ONCE(neo_dcp->neo_iomfb.owner, current);
	if (!neo_dcp->neo_iomfb.stopped)
		neo_iomfb_recv_message(neo_dcp, message);
	WRITE_ONCE(neo_dcp->neo_iomfb.owner, NULL);
	neo_iomfb_queue_advance(neo_dcp);
	mutex_unlock(&neo_dcp->neo_iomfb.lock);
}

int neo_iomfb_start_rtkit(struct neo_apple_dcp *neo_dcp)
{
	dma_addr_t shmem_iova;
	int ret;

	if (neo_iomfb_uses_queue(neo_dcp)) {
		mutex_lock(&neo_dcp->neo_iomfb.lock);
		if (neo_dcp->neo_iomfb.active || neo_dcp->neo_iomfb.scanout) {
			mutex_unlock(&neo_dcp->neo_iomfb.lock);
			return -EBUSY;
		}
		neo_dcp->neo_iomfb.stopped = false;
		mutex_unlock(&neo_dcp->neo_iomfb.lock);
	}

	/*
	 * H17P firmware expects the remote allocator endpoint to be started
	 * before IOMFB.  A firmware subsystem whose endpoint was never started
	 * has nothing answering it on the AP side, and without this one the
	 * display DART faults.
	 */
	if (neo_dcp->fw_compat == DCP_FIRMWARE_H17P) {
		ret = apple_rtkit_start_ep(neo_dcp->rtk, REMOTE_ALLOC_ENDPOINT);
		if (ret)
			return ret;
	}
	ret = apple_rtkit_start_ep(neo_dcp->rtk, IOMFB_ENDPOINT);
	if (ret)
		return ret;

	neo_dcp->shmem = dma_alloc_coherent(neo_dcp->dev, DCP_SHMEM_SIZE, &shmem_iova,
					GFP_KERNEL);
	if (!neo_dcp->shmem)
		return -ENOMEM;

	neo_dcp_send_message(neo_dcp, IOMFB_ENDPOINT, dcpep_set_shmem(shmem_iova));

	return 0;
}

void neo_iomfb_shutdown(struct neo_apple_dcp *neo_dcp)
{
	/* We're going down */
	neo_dcp->active = false;
	neo_dcp_mode_invalidate(&neo_dcp->mode_state);

	switch (neo_dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		neo_iomfb_shutdown_v12_3(neo_dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		neo_iomfb_shutdown_v13_3(neo_dcp);
		break;
	case DCP_FIRMWARE_H17P:
		neo_iomfb_shutdown_h17p(neo_dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", neo_dcp->fw_compat);
		break;
	}
}
