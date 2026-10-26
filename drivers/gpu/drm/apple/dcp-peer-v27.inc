/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Bring-up gate until the framebuffer merge lifecycle is implemented. This
 * only reserves an otherwise unrouted DCP; it never replaces a physical route.
 */
static bool peer_allocation_probe;
module_param(peer_allocation_probe, bool, 0444);
MODULE_PARM_DESC(peer_allocation_probe, "Test firmware27 logical peer allocation (merge bring-up)");
static DEFINE_MUTEX(dcp_peer_lock);

bool dcp_peer_tiling_state_v27(struct apple_dcp *dcp, bool write,
			      u32 event, u32 param, u32 *value)
{
	struct apple_dcp *peer;

	guard(mutex)(&dcp_peer_lock);
	if (!peer_allocation_probe || dcp->peer_unbinding || dcp->crashed)
		return false;
	peer = dcp->peer_reserved ?: dcp->peer_owner;
	if (!peer || peer->peer_unbinding || peer->crashed)
		return false;
	/* A resource reservation is not a framebuffer pair. Before the native
	 * split-mode preparation, keep the established untiled callback path;
	 * zero-initialized fields are not yet valid tiling-manager state.
	 */
	if (!dcp->peer_tiling.vftg || !peer->peer_tiling.vftg)
		return false;
	if (write)
		return dcp_peer_tiling_set_v27(&dcp->peer_tiling,
					      &peer->peer_tiling, event, param, *value);
	return dcp_peer_tiling_get_v27(&dcp->peer_tiling, &peer->peer_tiling,
				     event, param, value);
}

void dcp_peer_display_ids_v27(struct apple_dcp *dcp, u32 own, u32 related)
{
	struct apple_dcp *peer;

	guard(mutex)(&dcp_peer_lock);
	peer = dcp->peer_reserved ?: dcp->peer_owner;
	if (!peer || own == related || dcp->peer_unbinding || peer->peer_unbinding)
		return;
	/* D575 carries external display IDs, not crossbar endpoint indices. */
	dcp->peer_tiling.display_id = own;
	dcp->peer_tiling.display_id_valid = true;
	peer->peer_tiling.display_id = related;
	peer->peer_tiling.display_id_valid = true;
}

static void dcp_peer_release_locked(struct apple_dcp *dcp)
{
	if (dcp->peer_reserved) {
		/* A link-allocation release is not a DMA-stop acknowledgement.
		 * Keep a prepared pair owned until the AP has powered both down.
		 * Firmware may still need the tiling rendezvous state to stop.
		 */
		if (dcp->peer_tiling.vftg || dcp->peer_tiling.member ||
		    dcp->peer_reserved->peer_tiling.vftg ||
		    dcp->peer_reserved->peer_tiling.member) {
			dcp->peer_tiling.modeset_complete = false;
			dcp->peer_reserved->peer_tiling.modeset_complete = false;
			return;
		}
		memset(&dcp->peer_tiling, 0, sizeof(dcp->peer_tiling));
		memset(&dcp->peer_reserved->peer_tiling, 0,
		       sizeof(dcp->peer_reserved->peer_tiling));
		dcp->peer_reserved->peer_owner = NULL;
		dcp->peer_reserved = NULL;
	}
}

static void dcp_peer_forget(struct apple_dcp *dcp)
{
	guard(mutex)(&dcp_peer_lock);
	dcp->peer_unbinding = true;
	/* The existing driver removal path owns firmware shutdown. */
	if (dcp->peer_reserved) {
		dcp->peer_reserved->peer_owner = NULL;
		memset(&dcp->peer_reserved->peer_tiling, 0,
		       sizeof(dcp->peer_reserved->peer_tiling));
		dcp->peer_reserved = NULL;
		memset(&dcp->peer_tiling, 0, sizeof(dcp->peer_tiling));
	}
	if (dcp->peer_owner) {
		memset(&dcp->peer_owner->peer_tiling, 0,
		       sizeof(dcp->peer_owner->peer_tiling));
		memset(&dcp->peer_tiling, 0, sizeof(dcp->peer_tiling));
		dcp->peer_owner->peer_reserved = NULL;
		dcp->peer_owner = NULL;
	}
}

int dcp_peer_hints_v27(struct apple_dcp *dcp, struct dptx_port *port,
		      const void *data, size_t size, void *reply, size_t reply_size)
{
	struct dcp_display_hints_v27 *r = reply;
	struct apple_dcp *peer = NULL;
	struct drm_crtc *crtc;
	u32 extra;

	if (size != sizeof(*r) || reply_size != sizeof(*r))
		return -EINVAL;
	memcpy(reply, data, sizeof(*r));
	/* The first word is hint flags, not a return code. Native stores its
	 * completion status at0x90, leaving the request hints unchanged.
	 */
	r->status = 0;
	if (!peer_allocation_probe)
		return 0;
	if (!of_device_is_compatible(dcp->dev->of_node, "apple,t6050-dcpext") ||
	    port->unit || !dcp->crtc)
		return 0;

	extra = dcp_peer_extra_pipes(r->hints, &port->caps);
	memset(&r->primary_id, 0, 32);
	guard(mutex)(&dcp_peer_lock);
	if (dcp->peer_unbinding || dcp->crashed)
		return -ENODEV;
	if (!extra || !(r->hints[0] & 1)) {
		dcp_peer_release_locked(dcp);
		memset(&r->primary_id, 0, 32);
		return 0;
	}
	/* This candidate implements a non-tiled, two-pipe reservation. Tiled
	 * panels and more than one peer require the full display allocator.
	 */
	if (extra != 1 || (r->hints[0] & 2) || dcp->peer_owner)
		return 0;
	peer = dcp->peer_reserved;
	if (!peer) {
		drm_for_each_crtc(crtc, dcp->crtc->base.dev) {
			struct apple_dcp *p = platform_get_drvdata(to_apple_crtc(crtc)->dcp);

			/* A dedicated, unrouted peer cannot race Type-C's allocator
			 * or a fixed connector's HPD handler in this bring-up step.
			 */
			if (!p || !p->active || p->crashed || p->peer_unbinding ||
			    p->peer_owner || p->peer_reserved || p->fixed_phy ||
			    p->nr_typec_routes || p->active_typec_route ||
			    (p->connector && p->connector->connected) ||
			    !dcp_peer_index_allowed(dcp->index, p->index) ||
			    p->dptx_die != dcp->dptx_die ||
			    !p->crtc || p->crtc->base.dev != dcp->crtc->base.dev ||
			    p->fw_compat != DCP_FIRMWARE_V_27_0 ||
			    !p->dptxport[0].enabled ||
			    p->dptxport[0].caps.width < port->caps.width ||
			    p->dptxport[0].caps.total_rate < port->caps.total_rate ||
			    p->dptxport[0].caps.active_rate < port->caps.active_rate ||
			    p->dptxport[0].connected || p->dptxport[1].connected ||
			    !of_device_is_compatible(p->dev->of_node, "apple,t6050-dcpext"))
				continue;
			if (!peer || p->index < peer->index)
				peer = p;
		}
		if (!peer) {
			dev_info(dcp->dev, "no eligible idle peer for %u extra pipe\n", extra);
			return 0;
		}
		dcp->peer_reserved = peer;
		peer->peer_owner = dcp;
		dev_info(dcp->dev, "reserved peer DCP%u for %ux%u, total %u active %u\n",
			 peer->index, get_unaligned_le32(r->hints + 0x38),
			 get_unaligned_le32(r->hints + 0x3c),
			 get_unaligned_le32(r->hints + 0x40),
			 get_unaligned_le32(r->hints + 0x44));
	}
	/* Native updateDisplayAllocation writes primary/peer endpoint IDs.
	 * getExtraPipeCount supplies byte8. This is full dual-pipe, so the
	 * T6050 limited-dual-pipe flag (byte9 bit1) remains clear.
	 */
	r->primary_id = cpu_to_le32(dcp->index);
	r->peer_id = cpu_to_le32(peer->index);
	r->extra_pipes = extra;
	r->flags &= ~3;
	return 0;
}
