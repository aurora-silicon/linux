/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Kept opt-in while paired DRM lifecycle is being validated on T6050. */
static bool peer_scanout;
module_param(peer_scanout, bool, 0444);
MODULE_PARM_DESC(peer_scanout, "Enable firmware27 paired DRM scanout bring-up");

static bool dcp_peer_transport_idle(struct apple_dcp *dcp)
{
	return dcp->active && !dcp->crashed && !dcp->peer_unbinding &&
	       !dcp->ch_cmd.depth && !dcp->ch_cb.depth && !dcp->ch_async.depth &&
	       !dcp->ch_oobcmd.depth && !dcp->ch_oobcb.depth && !dcp->ch_oobasync.depth;
}

int dcp_peer_modeset_v27(struct apple_dcp *dcp, u32 timing, u32 color, bool vrr)
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
	/* Do not silently select an untiled mode when paired VRR was requested.
	 * The internal and single-pipe external adaptive-sync paths are unchanged.
	 */
	if (vrr) {
		struct dcp_peer_get_video_req_v27 req = {
			.timing_id = cpu_to_le32(timing), .color_id = cpu_to_le32(color),
		};
		struct dcp_peer_get_video_resp_v27 resp;

		ret = dcp_peer_video_call(dcp, "A364", &req, sizeof(req), &resp, sizeof(resp));
		if (ret || !(resp.success & 1) || (resp.requires_merge & 1)) {
			ret = ret ?: -EINVAL;
			goto peer_unlock;
		}
	}
	ret = dcp_peer_install_video_v27(dcp, peer, timing, color, 4);
	if (!ret) {
		guard(mutex)(&dcp_peer_lock);
		dcp->peer_frames = (struct dcp_peer_frames_v27){ .active = true };
		dev_info(dcp->dev, "paired DRM mode ready: timing=%u color=%u\n", timing, color);
	}
peer_unlock:
	device_unlock(peer->dev);
unlock:
	device_unlock(dcp->dev);
	return ret;
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
