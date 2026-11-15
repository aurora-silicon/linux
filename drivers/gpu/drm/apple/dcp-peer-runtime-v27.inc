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
	    !dcp_peer_param_v27(dcp, peer, 6, 1, dcp->output_pq ? 0x20000 : 0x10000))
		dev_info(dcp->dev, "paired engines report %ux%u each\n",
			 le32_to_cpu(tile[0]), le32_to_cpu(tile[1]));
}

int dcp_peer_modeset_v27(struct apple_dcp *dcp, u32 timing, u32 color, u32 min_vrr)
{
	struct apple_dcp *peer;
	int ret = -EOPNOTSUPP;

	if (!peer_scanout || !peer_allocation_probe ||
	    dcp->fw_compat != DCP_FIRMWARE_V_27_0)
		return ret;
	if (timing > U32_MAX - 10000 || color > U32_MAX - 10000)
		return -EINVAL;
	if (!device_trylock(dcp->dev))
		return -EBUSY;
	{
		guard(mutex)(&dcp_peer_lock);
		peer = dcp->peer_reserved;
		if (!peer)
			goto unlock;
		if (peer->peer_owner != dcp || !dcp_peer_transport_idle(dcp) ||
		    !dcp_peer_transport_idle(peer) || peer->fixed_phy ||
		    peer->nr_typec_routes || !peer->crtc ||
		    !peer->crtc->base.state || peer->crtc->base.state->active ||
		    !device_trylock(peer->dev)) {
			ret = -EBUSY;
			goto unlock;
		}
	}
	ret = dcp_peer_prepare_v27(dcp, peer, min_vrr);
	if (ret)
		goto peer_unlock;
	ret = dcp_peer_install_video_v27(dcp, peer, timing, color, 4);
	if (!ret)
		dcp_peer_finish_v27(dcp, peer);
	if (!ret) {
		guard(mutex)(&dcp_peer_lock);
		dcp->peer_frames = (struct dcp_peer_frames_v27){ .active = true };
		dev_info(dcp->dev, "paired DRM mode ready: timing=%u color=%u min_vrr=%#x\n",
			 timing, color, min_vrr);
	}
peer_unlock:
	device_unlock(peer->dev);
unlock:
	device_unlock(dcp->dev);
	return ret;
}

/* A timing faster than one engine's pixel rate: only a pair drives it. */
bool dcp_peer_needs_pair_v27(struct apple_dcp *dcp, u32 clock_khz)
{
	u64 rate = dcp->dptxport[0].caps.total_rate;

	return dcp->fw_compat == DCP_FIRMWARE_V_27_0 && rate && (u64)clock_khz * 1000 > rate;
}

/*
 * At atomic check: refuse a paired timing the pair cannot take, so that the
 * commit fails instead of a mode set that leaves the CRTC without a timing.
 * The peer's CRTC is not taken into the commit; its current state is read.
 * A CRTC being enabled reserves its peer when the link comes up, in
 * dcp_poweron() after this check; iomfb_modeset() refuses the timing then
 * if no peer was reserved.
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

/* Return1 after a paired stop,0 for a normal CRTC, or an error. */
int dcp_peer_stop_v27(struct apple_dcp *dcp)
{
	struct apple_dcp *peer;
	int ret = 0;

	if (!peer_scanout || !READ_ONCE(dcp->peer_frames.active))
		return 0;
	if (!device_trylock(dcp->dev))
		return -EBUSY;
	{
		guard(mutex)(&dcp_peer_lock);
		peer = dcp->peer_reserved;
		if (!peer || peer->peer_owner != dcp || !dcp_peer_transport_idle(dcp) ||
		    !dcp_peer_transport_idle(peer) || !device_trylock(peer->dev)) {
			ret = -EBUSY;
			goto unlock;
		}
	}
	ret = dcp_peer_probe_quiesce(dcp, peer);
	device_unlock(peer->dev);
	if (!ret)
		ret = 1;
unlock:
	device_unlock(dcp->dev);
	return ret;
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

bool dcp_peer_swap_complete_v27(struct apple_dcp *dcp, u32 id, ktime_t now,
		void (*report)(struct apple_dcp *, u32, ktime_t))
{
	struct apple_dcp *owner;
	u32 watermark;

	/* Independent CRTCs must not serialize through the pair's mutex. */
	if (!peer_scanout || (!READ_ONCE(dcp->peer_frames.active) &&
			     !READ_ONCE(dcp->peer_owner)))
		return false;
	guard(mutex)(&dcp_peer_lock);
	owner = dcp->peer_owner ?: dcp;
	if (!peer_scanout || !owner->peer_frames.active)
		return false;
	if (dcp_peer_frame_complete_v27(&owner->peer_frames, dcp != owner, id, &watermark))
		/* Queue the combined watermark to the primary IOMFB owner. */
		report(owner, watermark, now);
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
