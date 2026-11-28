/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Paired DRM scanout of a mode one firmware-27 pipe cannot drive alone. */
static bool peer_scanout = true;
module_param(peer_scanout, bool, 0444);
MODULE_PARM_DESC(peer_scanout, "Firmware27 paired DRM scanout (default: on)");

static bool dcp_peer_transport_idle(struct apple_dcp *dcp)
{
	return dcp->active && !dcp->crashed && !dcp->peer_unbinding &&
	       !dcp->ch_cmd.depth && !dcp->ch_cb.depth && !dcp->ch_async.depth &&
	       !dcp->ch_oobcmd.depth && !dcp->ch_oobcb.depth && !dcp->ch_oobasync.depth;
}

/* One of macOS's calls around a paired mode set, on the primary and then the peer. */
static int dcp_peer_both_v27(struct apple_dcp *dcp, struct apple_dcp *peer, const char *tag,
			     void *in, size_t in_size, void *out, size_t out_size, bool status)
{
	struct apple_dcp *engine[2] = { dcp, peer };
	u8 reply[0x7c];

	for (unsigned int i = 0; i < 2; i++) {
		int ret = dcp_peer_video_call(engine[i], tag, in, in_size,
					      out_size ? reply : NULL, out_size);

		if (!ret && status && get_unaligned_le32(reply))
			ret = -EIO;
		if (ret) {
			dev_err(dcp->dev, "paired %s on %s failed: %d\n", tag,
				dev_name(engine[i]->dev), ret);
			return ret;
		}
		if (out && out_size)
			memcpy(out, reply, out_size);
	}
	return 0;
}

/* set_parameter (A450) on both engines; value[0] in 16.16 where it is a rate or a factor. */
static int dcp_peer_param_v27(struct apple_dcp *dcp, struct apple_dcp *peer, u32 param,
			      u32 count, u32 value)
{
	struct dcp_set_parameter_dcp p = { .param = param, .value = { value }, .count = count };

	return dcp_peer_both_v27(dcp, peer, "A450", &p, sizeof(p), NULL, sizeof(u32), true);
}

/*
 * What macOS sends to both engines before a paired mode set (A411 2; adaptive
 * sync, parameter 15, its minimum rate or 0 for a fixed rate; parameter 0x1f)
 * and after it (A449; parameter 3 = 1.0; A453; parameter 6 = 2.0 for a PQ
 * colour mode, else 1.0).
 */
static int dcp_peer_prepare_v27(struct apple_dcp *dcp, struct apple_dcp *peer, u32 min_vrr)
{
	__le32 device = cpu_to_le32(2);

	return dcp_peer_both_v27(dcp, peer, "A411", &device, sizeof(device), NULL, 4, false) ?:
	       dcp_peer_param_v27(dcp, peer, 15, 3, min_vrr) ?:
	       dcp_peer_param_v27(dcp, peer, 0x1f, 2, 0);
}

static void dcp_peer_finish_v27(struct apple_dcp *dcp, struct apple_dcp *peer)
{
	u8 config[0x84] = {};
	__le32 zero = 0, tile[2] = {};

	put_unaligned_le32(1, config + 0x1c);
	put_unaligned_le32(1, config + 0x3c);
	if (!dcp_peer_both_v27(dcp, peer, "A449", config, sizeof(config), NULL, 0x7c, false) &&
	    !dcp_peer_param_v27(dcp, peer, 3, 1, 0x10000) &&
	    !dcp_peer_both_v27(dcp, peer, "A453", &zero, sizeof(zero), tile, sizeof(tile), false) &&
	    !dcp_peer_param_v27(dcp, peer, 6, 1, dcp->output_hdr ? 0x20000 : 0x10000))
		dev_info(dcp->dev, "paired engines report %ux%u each\n",
			 le32_to_cpu(tile[0]), le32_to_cpu(tile[1]));
}

/*
 * While the platform limit leaves the display no pair, it is set one of the
 * modes one engine drives (dcp_peer_check_v27()), and no paired call runs for
 * it. A pair it ran is stopped first: both engines powered off, the peer left
 * off and this engine powered on again. -EOPNOTSUPP then has the caller set
 * the mode on this engine alone.
 */
static bool dcp_peer_limited_pair_locked(struct apple_dcp *dcp, bool *prepared)
{
	struct apple_dcp *peer = dcp->peer_reserved;

	lockdep_assert_held(&dcp_peer_lock);
	*prepared = false;
	if (!peer || !dcp_peer_limited_locked(dcp, NULL, NULL))
		return false;
	*prepared = dcp->peer_tiling.vftg || peer->peer_tiling.vftg;
	return true;
}

static int dcp_peer_limited_stop_v27(struct apple_dcp *dcp, struct apple_dcp *peer)
{
	u8 power[16] = {1}, response[8];
	int ret;

	ret = dcp_peer_probe_quiesce(dcp, peer, false);
	if (ret)
		return ret;
	ret = dcp_peer_video_call(dcp, "A484", power, sizeof(power),
				  response, sizeof(response));
	if (ret || get_unaligned_le32(response + 4))
		return ret ?: -EIO;
	dev_info(dcp->dev, "platform limit: pair stopped, its peer left off\n");
	return -EOPNOTSUPP;
}

static void dcp_peer_transition_end(struct apple_dcp *dcp)
{
	scoped_guard(mutex, &dcp_peer_lock) {
		WRITE_ONCE(dcp->peer_transition_seq,
			   dcp->peer_transition_seq + 1);
		smp_store_release(&dcp->peer_transition_active, false);
	}
	wake_up_all(&dcp_peer_stop_wq);
}

int dcp_peer_modeset_v27(struct apple_dcp *dcp, u32 timing, u32 color, u32 min_vrr)
{
	struct apple_dcp *peer;
	bool limited, prepared, vrr_only;
	int ret = -EOPNOTSUPP;

	if (!peer_scanout || !peer_allocation_probe ||
	    dcp->fw_compat != DCP_FIRMWARE_V_27_0)
		return ret;
	if (timing > U32_MAX - 10000 || color > U32_MAX - 10000)
		return -EINVAL;
	/*
	 * No paired call, nor wait for one, for a display without a peer or one
	 * the limit leaves no pair. The device lock below may be the probe's.
	 */
	scoped_guard(mutex, &dcp_peer_lock) {
		if (!dcp->peer_reserved)
			return ret;
		limited = dcp_peer_limited_pair_locked(dcp, &prepared);
		if (limited && !prepared)
			return ret;
		if (dcp->peer_transition_active)
			return -EBUSY;
		smp_store_release(&dcp->peer_transition_active, true);
	}
	if (!device_trylock(dcp->dev)) {
		ret = -EBUSY;
		goto clear_transition;
	}
	iomfb_pause_peer(dcp, true);
	ret = iomfb_queue_settle(dcp, msecs_to_jiffies(500));
	if (ret) {
		if (ret == -ETIMEDOUT)
			ret = -EBUSY;
		goto unpause;
	}
	ret = -EOPNOTSUPP;
	{
		guard(mutex)(&dcp_peer_lock);
		peer = dcp->peer_reserved;
		if (!peer)
			goto unpause;
		/* The limit may have come or gone since the check above. */
		limited = dcp_peer_limited_pair_locked(dcp, &prepared);
		if (limited && !prepared)
			goto unpause;
		if (peer->peer_owner != dcp || !dcp_peer_transport_idle(dcp) ||
		    !dcp_peer_transport_idle(peer) || peer->fixed_phy ||
		    peer->nr_typec_routes || !peer->crtc ||
		    !peer->crtc->base.state || peer->crtc->base.state->active ||
		    !device_trylock(peer->dev)) {
			ret = -EBUSY;
			goto unpause;
		}
		vrr_only = dcp->peer_tiling.modeset_complete &&
			   peer->peer_tiling.modeset_complete &&
			   dcp_peer_vrr_only_v27(&dcp->peer_frames, timing, color,
						 min_vrr, dcp->output_hdr);
	}
	if (limited) {
		ret = dcp_peer_limited_stop_v27(dcp, peer);
		if (ret && ret != -EOPNOTSUPP)
			dcp_mode_invalidate(&dcp->mode_state);
		goto peer_unlock;
	}
	/*
	 * Adaptive sync uses parameter 15 on both engines. Leave a running
	 * pair installed when its timing and color are still current. If
	 * either engine refuses the update, rebuild both with the new rate.
	 */
	if (vrr_only) {
		bool still_owned, updated = false;

		ret = dcp_peer_param_v27(dcp, peer, 15, 3, min_vrr);
		scoped_guard(mutex, &dcp_peer_lock) {
			still_owned = dcp->peer_reserved == peer &&
				      peer->peer_owner == dcp;
			limited = still_owned &&
				  dcp_peer_limited_pair_locked(dcp, &prepared);
			if (!ret && !limited && still_owned &&
			    dcp->peer_tiling.modeset_complete &&
			    peer->peer_tiling.modeset_complete &&
			    dcp_peer_vrr_only_v27(&dcp->peer_frames, timing,
						  color, min_vrr,
						  dcp->output_hdr)) {
				dcp->peer_frames.min_vrr = min_vrr;
				dev_dbg(dcp->dev, "paired adaptive sync minimum %#x\n", min_vrr);
				updated = true;
			}
		}
		if (updated)
			goto peer_unlock;
		if (!still_owned) {
			ret = -EBUSY;
			goto peer_unlock;
		}
		if (limited) {
			ret = prepared ? dcp_peer_limited_stop_v27(dcp, peer)
				       : -EOPNOTSUPP;
			if (ret && ret != -EOPNOTSUPP)
				dcp_mode_invalidate(&dcp->mode_state);
			goto peer_unlock;
		}
	}
	ret = dcp_peer_prepare_v27(dcp, peer, min_vrr);
	if (ret)
		goto peer_unlock;
	ret = dcp_peer_install_video_v27(dcp, peer, timing, color, 4);
	if (ret && ret != -EOPNOTSUPP)
		dcp_mode_invalidate(&dcp->mode_state);
	if (!ret)
		dcp_peer_finish_v27(dcp, peer);
	if (!ret) {
		guard(mutex)(&dcp_peer_lock);
		dcp->peer_frames = (struct dcp_peer_frames_v27){
			.timing = timing,
			.color = color,
			.min_vrr = min_vrr,
			.output_hdr = dcp->output_hdr,
			.active = true,
		};
		dev_info(dcp->dev, "paired DRM mode ready: timing=%u color=%u min_vrr=%#x\n",
			 timing, color, min_vrr);
	}
peer_unlock:
	device_unlock(peer->dev);
unpause:
	iomfb_pause_peer(dcp, false);
	device_unlock(dcp->dev);
clear_transition:
	dcp_peer_transition_end(dcp);
	return ret;
}

/* A timing faster than one engine's pixel rate: only a pair drives it. */
bool dcp_peer_needs_pair_v27(struct apple_dcp *dcp, u32 clock_khz)
{
	u64 rate = dcp->dptxport[0].caps.total_rate;

	return dcp->fw_compat == DCP_FIRMWARE_V_27_0 && rate && (u64)clock_khz * 1000 > rate;
}

/* A timing only a pair drives, while the platform limit leaves no pair. */
bool dcp_peer_mode_limited_v27(struct apple_dcp *dcp, u32 clock_khz)
{
	if (!dcp_peer_needs_pair_v27(dcp, clock_khz))
		return false;
	guard(mutex)(&dcp_peer_lock);
	return dcp_peer_limited_locked(dcp, NULL, NULL);
}

/*
 * At atomic check: refuse a paired timing the pair cannot take, so that the
 * commit fails instead of a mode set that leaves the CRTC without a timing.
 * The peer's CRTC is not taken into the commit; its current state is read.
 * A CRTC being enabled reserves its peer when the link comes up, in
 * dcp_poweron() after this check; iomfb_modeset() refuses the timing then
 * if no peer was reserved. While the platform limit leaves the display no
 * pair, a paired timing is refused whatever the peer's state.
 */
int dcp_peer_check_v27(struct apple_dcp *dcp, struct drm_atomic_commit *commit,
		       struct drm_crtc_state *crtc_state)
{
	u64 rate = dcp->dptxport[0].caps.total_rate;
	struct drm_crtc_state *peer_state;
	struct drm_crtc *peer_crtc;
	bool pair;

	if (!crtc_state->enable || !drm_atomic_crtc_needs_modeset(crtc_state) ||
	    dcp->fw_compat != DCP_FIRMWARE_V_27_0 || !rate)
		return 0;
	pair = dcp_peer_needs_pair_v27(dcp, crtc_state->mode.clock);
	dev_info(dcp->dev, "mode %ux%u@%u: %u kHz against one engine's %llu Hz: %s\n",
		 crtc_state->mode.hdisplay, crtc_state->mode.vdisplay,
		 drm_mode_vrefresh(&crtc_state->mode), crtc_state->mode.clock, rate,
		 pair ? "needs the pair" : "one engine");
	if (!pair)
		return 0;
	if (!peer_scanout || !peer_allocation_probe)
		return -EINVAL;
	scoped_guard(mutex, &dcp_peer_lock) {
		struct apple_dcp *peer = dcp->peer_reserved;
		unsigned int displays, engines;

		/* Not even for a CRTC being enabled: the limit has no pair to give. */
		if (dcp_peer_limited_locked(dcp, &displays, &engines)) {
			dev_info_ratelimited(dcp->dev,
					     "mode %ux%u@%u refused: %u external displays on %u engines leave no pair\n",
					     crtc_state->mode.hdisplay,
					     crtc_state->mode.vdisplay,
					     drm_mode_vrefresh(&crtc_state->mode),
					     displays, engines);
			return -EINVAL;
		}
		if (!peer && crtc_state->active_changed)
			return 0;
		if (!peer || peer->peer_owner != dcp || peer->fixed_phy ||
		    peer->nr_typec_routes || !peer->crtc || dcp->crashed || peer->crashed)
			return -EINVAL;
		peer_crtc = &peer->crtc->base;
	}
	peer_state = drm_atomic_get_new_crtc_state(commit, peer_crtc);
	if (peer_state ? peer_state->active :
	    (peer_crtc->state && READ_ONCE(peer_crtc->state->active)))
		return -EINVAL;
	return 0;
}

static int dcp_peer_stop_v27_once(struct apple_dcp *dcp, bool *contended,
				  const unsigned long *deadline)
{
	struct apple_dcp *peer;
	int ret = 0;

	if (contended)
		*contended = false;
	if (!peer_scanout)
		return 0;
	scoped_guard(mutex, &dcp_peer_lock) {
		if (dcp->peer_transition_active) {
			if (contended)
				*contended = true;
			return -EBUSY;
		}
		if (!dcp->peer_frames.active)
			return 0;
		if (deadline && time_after_eq(jiffies, *deadline))
			return -EBUSY;
		smp_store_release(&dcp->peer_transition_active, true);
	}
	if (!device_trylock(dcp->dev)) {
		ret = -EBUSY;
		goto clear_transition;
	}
	/* Re-check under device_lock: a racing unplug/power-off may have finished. */
	if (!READ_ONCE(dcp->peer_frames.active))
		goto unlock;
	/*
	 * Pause new presentations and wait for any in-flight paired swap to
	 * settle before dcp_peer_probe_quiesce() clears modeset_complete and
	 * powers both engines down. Otherwise iomfb_queue_work() can launch a
	 * queued swap mid-quiesce and mark the primary DCP crashed.
	 */
	iomfb_pause_peer(dcp, true);
	ret = iomfb_queue_settle(dcp, msecs_to_jiffies(500));
	if (ret == -ETIMEDOUT && iomfb_retire_stalled_present_h17p(dcp))
		ret = iomfb_queue_settle(dcp, msecs_to_jiffies(100));
	if (ret) {
		if (ret == -ETIMEDOUT)
			ret = -EBUSY;
		goto unpause;
	}
	{
		guard(mutex)(&dcp_peer_lock);
		if (!dcp->peer_frames.active) {
			ret = 0;
			goto unpause;
		}
		peer = dcp->peer_reserved;
		if (!peer || peer->peer_owner != dcp || !dcp_peer_transport_idle(dcp) ||
		    !dcp_peer_transport_idle(peer) || !device_trylock(peer->dev)) {
			ret = -EBUSY;
			goto unpause;
		}
	}
	ret = dcp_peer_probe_quiesce(dcp, peer, true);
	device_unlock(peer->dev);
	if (!ret)
		ret = 1;
unpause:
	iomfb_pause_peer(dcp, false);
unlock:
	device_unlock(dcp->dev);
clear_transition:
	dcp_peer_transition_end(dcp);
	return ret;
}

/* Return 1 after a paired stop, 0 for a normal CRTC, or an error. */
int dcp_peer_stop_v27(struct apple_dcp *dcp)
{
	return dcp_peer_stop_v27_once(dcp, NULL, NULL);
}

/*
 * Stop an active pair, waiting on dcp_peer_stop_wq for any in-flight paired
 * stop or modeset (bounded by timeout_ms) and bounding retry admission so no
 * new attempt starts at or after the deadline (an already-admitted attempt runs
 * to completion).
 */
int dcp_peer_stop_wait_v27(struct apple_dcp *dcp, unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	bool contended;
	int ret;

	if (dcp->fw_compat != DCP_FIRMWARE_V_27_0)
		return 0;
	for (;;) {
		u32 seq = READ_ONCE(dcp->peer_transition_seq);
		unsigned long now, remaining;

		ret = dcp_peer_stop_v27_once(dcp, &contended, &deadline);
		if (ret != -EBUSY)
			return ret;
		now = jiffies;
		if (time_after_eq(now, deadline))
			return -EBUSY;
		remaining = deadline - now;
		if (contended) {
			wait_event_timeout(dcp_peer_stop_wq,
					   !smp_load_acquire(&dcp->peer_transition_active) &&
					   (READ_ONCE(dcp->peer_transition_seq) != seq ||
					    !smp_load_acquire(&dcp->peer_frames.active)),
					   remaining);
		} else {
			wait_event_timeout(dcp_peer_stop_wq,
					   !smp_load_acquire(&dcp->peer_transition_active) &&
					   !smp_load_acquire(&dcp->peer_frames.active),
					   min_t(unsigned long, remaining,
						 max_t(unsigned long, 1,
						       msecs_to_jiffies(5))));
		}
	}
}

/*
 * The display of a running pair goes: its cable or tunnel, or its release
 * at a sleep's entry. The primary's firmware keeps a merged pipe lit across
 * the HPD low and answers that call only once the AP has powered the pair
 * down; with user space frozen nothing else does that within the call's
 * second, and the HPD low times out (-110), the route stays owned and the
 * sleep is refused. Stop the pair first, as macOS tears its dual pipe down
 * (power 0 on both engines, A362(0) on the main, then the peer) before the
 * DPTX release it sends to the main DCP only. A swap in flight, or a power-off
 * of the CRTC holding the device lock (~220 ms for A484(0) + 100 ms + A362(0)),
 * is waited out before giving up.
 * Returns 1 after a stop, 0 if no pair runs, or an error.
 */
static int dcp_peer_unplug_v27(struct apple_dcp *dcp)
{
	return dcp_peer_stop_wait_v27(dcp, 1000);
}

struct apple_dcp *dcp_peer_scanout_v27(struct apple_dcp *dcp)
{
	struct apple_dcp *peer;

	if (!peer_scanout || !READ_ONCE(dcp->peer_frames.active))
		return NULL;
	guard(mutex)(&dcp_peer_lock);
	if (!dcp->peer_frames.active)
		return NULL;
	peer = dcp->peer_reserved;
	if (!peer || peer->peer_owner != dcp || dcp->peer_frames.failed ||
	    !dcp->peer_tiling.modeset_complete || !peer->peer_tiling.modeset_complete ||
	    dcp->crashed || peer->crashed || dcp->peer_unbinding || peer->peer_unbinding)
		return ERR_PTR(-EIO);
	return peer;
}

void dcp_peer_swap_armed_v27(struct apple_dcp *dcp, u32 id)
{
	guard(mutex)(&dcp_peer_lock);
	dcp_peer_frame_submit_v27(&dcp->peer_frames, id);
}

void dcp_peer_swap_failed_v27(struct apple_dcp *dcp)
{
	guard(mutex)(&dcp_peer_lock);
	dcp->peer_frames.failed = true;
	dcp_mode_invalidate(&dcp->mode_state);
}

void dcp_peer_record_retired_v27(struct apple_dcp *dcp, u32 swap_id)
{
	struct apple_dcp *peer;

	if (!peer_scanout)
		return;
	guard(rwsem_read)(&dcp_peer_retire_rwsem);
	scoped_guard(mutex, &dcp_peer_lock) {
		peer = dcp->peer_reserved;
		if (!peer || peer->peer_owner != dcp || peer->peer_unbinding)
			return;
	}
	/*
	 * Preserve iomfb.lock -> dcp_peer_lock order while revalidating the
	 * reservation before mutating peer->present_state_h17p.
	 */
	scoped_guard(mutex, &peer->iomfb.lock) {
		guard(mutex)(&dcp_peer_lock);
		if (dcp->peer_reserved != peer || peer->peer_owner != dcp ||
		    peer->peer_unbinding || peer->iomfb.stopped)
			return;
		dcp_present_record_retired_h17p(&peer->present_state_h17p,
						swap_id);
	}
}

bool dcp_peer_swap_complete_v27(struct apple_dcp *dcp, u32 id, ktime_t now, bool shown,
		void (*report)(struct apple_dcp *, u32, ktime_t, bool))
{
	struct apple_dcp *owner;
	u32 watermark;
	bool both_shown;

	/* Independent CRTCs must not serialize through the pair's mutex. */
	if (!peer_scanout || (!READ_ONCE(dcp->peer_frames.active) &&
			     !READ_ONCE(dcp->peer_owner)))
		return false;
	guard(mutex)(&dcp_peer_lock);
	owner = dcp->peer_owner ?: dcp;
	if (!peer_scanout || !owner->peer_frames.active)
		return false;
	if (dcp_peer_frame_complete_v27(&owner->peer_frames, dcp != owner, id, shown,
					&watermark, &both_shown))
		/* Queue the combined watermark to the primary IOMFB owner. */
		report(owner, watermark, now, both_shown);
	return true;
}

int dcp_peer_color_v27(struct apple_dcp *dcp, struct iomfb_set_matrix_req *matrix)
{
	struct apple_dcp *peer = dcp_peer_scanout_v27(dcp);
	u32 status, identity = 0x10000;
	int ret;

	if (!peer)
		return 0;
	if (IS_ERR(peer))
		return PTR_ERR(peer);
	ret = dcp_peer_video_call(peer, "A423", matrix, sizeof(*matrix), &status, 4);
	if (ret || status)
		return ret ?: -EIO;
	ret = dcp_peer_video_call(peer, "A428", &identity, sizeof(identity), &status, 4);
	return ret ?: (status ? -EIO : 0);
}
