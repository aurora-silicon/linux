/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#include "iomfb-peer-v27.h"

static void dcp_peer_swap_error(struct apple_dcp *owner, const char *where, u32 status)
{
	lockdep_assert_held(&owner->iomfb.lock);
	dev_err(owner->dev, "paired swap %s failed: %#x\n", where, status);
	dcp_peer_swap_failed_v27(owner);
	/* One engine may already have accepted this frame. A failed peer ACK
	 * is not a DMA-retirement witness for either scanout. The clean queue
	 * retains its active transaction and next-scanout references on crash. */
	WRITE_ONCE(owner->crashed, true);
	dcp_swap_watchdog_complete(owner);
	dcp_present_failed(owner);
	schedule_work(&owner->vblank_wq);
}

static void dcp_peer_submit_error_on_owner(struct apple_dcp *owner, void *data, void *cookie)
{
	u32 *status = data;
	dcp_peer_swap_error(owner, "peer submit", *status);
}

static void dcp_peer_submitted(struct apple_dcp *dcp, void *data, void *cookie)
{
	struct apple_dcp *owner = cookie;
	struct dcp_swap_submit_resp_v27 *resp = data;
	u32 status = resp ? resp->ret : ~0U;
	u32 id;

	if (owner != dcp) {
		if (status && iomfb_queue_callback(owner, dcp_peer_submit_error_on_owner,
						&status, sizeof(status), NULL)) {
			WRITE_ONCE(owner->crashed, true);
			schedule_work(&owner->vblank_wq);
		}
		return;
	}
	lockdep_assert_held(&owner->iomfb.lock);
	if (READ_ONCE(owner->peer_frames.failed))
		return;
	id = owner->swap.h17p.swap.swap_id;
	if (status) {
		dcp_peer_swap_error(owner, "primary submit", status);
		return;
	}
	if (dcp_present_submit(owner, id, true)) {
		dcp_peer_swap_error(owner, "submit state", -EPROTO);
		return;
	}
	/* Per-endpoint receive ordering handles the primary ACK before its
	 * completion. The paired watermark still requires both completions. */
	owner->swap_start = ktime_get();
	owner->swap_submit_timestamp = arch_timer_read_counter();
	dcp_swap_watchdog_arm(owner);
}

static void dcp_peer_started_on_owner(struct apple_dcp *owner, void *data, void *cookie)
{
	struct dcp_swap_start_resp_h17p *resp = data;
	struct apple_dcp *peer = dcp_peer_scanout_v27(owner);
	u32 id = owner->swap.h17p.swap.swap_id;

	lockdep_assert_held(&owner->iomfb.lock);
	if (IS_ERR_OR_NULL(peer) || peer != cookie || resp->ret || resp->swap_id != id) {
		dcp_peer_swap_error(owner, "start/ID", resp->ret ?: -EPROTO);
		return;
	}
	/* Keep the logical atomic input intact. Both split wire records are
	 * primary-owned; dcp_push copies the peer's record into its own queue. */
	iomfb_serialize_present_h17p(&owner->present_h17p, &owner->swap.h17p);
	if (!iomfb_apply_backlight_h17p(owner, &owner->swap.h17p, &owner->present_h17p)) {
		dcp_peer_swap_error(owner, "surface preparation", -EINVAL);
		return;
	}
	iomfb_serialize_present_v27(&owner->present_v27, &owner->swap.h17p);
	owner->present_v27.swap.brightness_update = owner->present_h17p.swap[0x354] & 1;
	owner->present_v27.swap.brightness_nits = get_unaligned_le64(owner->present_h17p.swap + 0x35e);
	dcp_peer_split_swap_v27(&owner->present_v27, &owner->peer_present_v27,
				owner->peer_tiling.width);
	if (dcp_present_retires(owner))
		dcp_arm_retained_framebuffers(owner, id);
	dcp_peer_swap_armed_v27(owner, id);
	trace_iomfb_swap_submit(owner, id);
	trace_iomfb_swap_submit(peer, id);
	dcp_push(owner, false, &dcp_methods[dcpep_swap_submit], sizeof(owner->present_v27),
		 sizeof(struct dcp_swap_submit_resp_v27), &owner->present_v27,
		 dcp_peer_submitted, owner);
	if (READ_ONCE(owner->crashed))
		return;
	dcp_push(peer, false, &dcp_methods[dcpep_swap_submit], sizeof(owner->peer_present_v27),
		 sizeof(struct dcp_swap_submit_resp_v27), &owner->peer_present_v27,
		 dcp_peer_submitted, owner);
	if (READ_ONCE(peer->crashed))
		dcp_peer_swap_error(owner, "peer publication", -EIO);
}

static void dcp_peer_started(struct apple_dcp *peer, void *data, void *cookie)
{
	struct apple_dcp *owner = cookie;
	struct dcp_swap_start_resp_h17p failed = { .ret = ~0U };

	if (iomfb_queue_callback(owner, dcp_peer_started_on_owner,
				 data ?: &failed, sizeof(failed), peer)) {
		WRITE_ONCE(owner->crashed, true);
		schedule_work(&owner->vblank_wq);
	}
}

static bool dcp_peer_start_second(struct apple_dcp *owner,
				 struct dcp_swap_start_resp_h17p *resp)
{
	struct dcp_swap_start_req_h17p req = {};
	struct apple_dcp *peer = dcp_peer_scanout_v27(owner);

	if (!peer)
		return false;
	if (IS_ERR(peer) || resp->ret) {
		dcp_peer_swap_error(owner, "primary start", resp->ret ?: -EIO);
		return true;
	}
	owner->swap.h17p.swap.swap_id = resp->swap_id;
	dcp_swap_start(peer, false, &req, dcp_peer_started, owner);
	return true;
}
