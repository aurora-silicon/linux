/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Logical peer allocation for a merged mode. This only reserves an otherwise
 * unrouted DCP; it never replaces a physical route.
 */
static bool peer_allocation_probe = true;
module_param(peer_allocation_probe, bool, 0444);
MODULE_PARM_DESC(peer_allocation_probe, "Firmware27 logical peer allocation for merged modes (default: on)");

/*
 * The platform limit on merged modes (dcp_fabric_merged_fits()) holds on the
 * board it has run on, for the J714s's T6050 external display processors.
 */
static bool dcp_peer_limit_applies(struct apple_dcp *dcp)
{
	return dcp->fw_compat == DCP_FIRMWARE_V_27_0 && dcp_j714s_ext(dcp->dev);
}

/*
 * Whether the platform limit leaves @dcp's display no pair. The external
 * engines of its DRM device are counted, and the external displays present on
 * them: its own always, since its display hints come before its connector is
 * reported connected.
 */
static bool dcp_peer_limited_locked(struct apple_dcp *dcp, unsigned int *displays,
				    unsigned int *engines)
{
	unsigned int present = 0, count = 0;
	struct drm_crtc *crtc;

	lockdep_assert_held(&dcp_peer_lock);
	if (!dcp_peer_limit_applies(dcp) || !dcp->crtc)
		return false;
	drm_for_each_crtc(crtc, dcp->crtc->base.dev) {
		struct apple_dcp *p = platform_get_drvdata(to_apple_crtc(crtc)->dcp);
		struct apple_connector *con;

		if (!p || !dcp_peer_limit_applies(p))
			continue;
		count++;
		con = READ_ONCE(p->connector);
		if (p == dcp || (con && READ_ONCE(con->connected)))
			present++;
	}
	if (displays)
		*displays = present;
	if (engines)
		*engines = count;
	/* The pair this driver sets up takes one engine more. */
	return !dcp_fabric_merged_fits(present, 1, count);
}

/* More than the display processors of any board with the limit. */
#define DCP_PEER_LIMIT_DISPLAYS	8

/*
 * An external display of @drm came or went: the platform limit may now give
 * another display its pair, or take it away. Have the connector of each
 * display whose limit changed probed again, so that its merged modes are
 * offered only while a pair fits (dcp_mode_valid()). A display that runs a
 * pair when the limit takes it away is also reported with a bad link, for the
 * compositor to set one of the modes it is left with.
 */
void dcp_peer_limit_update_v27(struct drm_device *drm)
{
	struct apple_connector *probe[DCP_PEER_LIMIT_DISPLAYS];
	bool bad[DCP_PEER_LIMIT_DISPLAYS];
	unsigned int n = 0, i;
	struct drm_crtc *crtc;

	/* Neither a board without the limit nor a device without CRTCs. */
	if (!of_machine_is_compatible("apple,j714s") || !drm->mode_config.num_crtc)
		return;
	scoped_guard(mutex, &dcp_peer_lock) {
		drm_for_each_crtc(crtc, drm) {
			struct apple_dcp *p = platform_get_drvdata(to_apple_crtc(crtc)->dcp);
			unsigned int displays = 0, engines = 0;
			struct apple_connector *con;
			bool limited;

			if (!p || !dcp_peer_limit_applies(p))
				continue;
			limited = dcp_peer_limited_locked(p, &displays, &engines);
			if (limited == p->peer_limited)
				continue;
			p->peer_limited = limited;
			/* Only a display with merged modes has a peer reserved. */
			con = READ_ONCE(p->connector);
			if (!con || !READ_ONCE(con->connected) || !p->peer_reserved ||
			    n == ARRAY_SIZE(probe))
				continue;
			dev_info(p->dev, "platform limit: %u external displays on %u engines: %s\n",
				 displays, engines, limited ?
				 "no pair, modes beyond one engine withdrawn" :
				 "a pair fits, modes beyond one engine offered again");
			probe[n] = con;
			bad[n++] = limited && (p->peer_frames.active || p->peer_tiling.vftg);
		}
	}
	/* Outside dcp_peer_lock: atomic check takes it under the modeset locks. */
	for (i = 0; i < n; i++) {
		if (bad[i])
			drm_connector_set_link_status_property(&probe[i]->base,
							       DRM_MODE_LINK_STATUS_BAD);
		dcp_queue_hotplug(probe[i]);
	}
}

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
	/* A resource reservation is not a framebuffer pair. Before the
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
	/* The first word is hint flags, not a return code. The completion
	 * status has a field of its own, leaving the request hints unchanged.
	 */
	r->status = 0;
	if (!peer_allocation_probe)
		return 0;
	if (!of_device_is_compatible(dcp->dev->of_node, "apple,t6050-dcpext") ||
	    port->unit || !dcp->crtc)
		return 0;

	extra = dcp_peer_extra_pipes(r->hints, &port->caps);
	memset(&r->allocation, 0, sizeof(r->allocation));
	guard(mutex)(&dcp_peer_lock);
	if (dcp->peer_unbinding || dcp->crashed)
		return -ENODEV;
	if (!extra || !(r->hints[0] & 1)) {
		dcp_peer_release_locked(dcp);
		memset(&r->allocation, 0, sizeof(r->allocation));
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
	/*
	 * The platform limit keeps the reservation: the peer is an engine no
	 * display is routed to, and the firmware keeps the merged timings for
	 * when a pair fits again (dcp_peer_limit_update_v27()). Until then DRM
	 * offers only the modes one engine drives; a mode set of one of them
	 * stops a pair that ran and sets the mode on this engine alone
	 * (dcp_peer_install_video_v27()).
	 */
	{
		unsigned int displays, engines;

		dcp->peer_limited = dcp_peer_limited_locked(dcp, &displays, &engines);
		if (dcp->peer_limited)
			dev_info(dcp->dev, "platform limit: %u external displays on %u engines: no pair, modes beyond one engine withheld\n",
				 displays, engines);
	}
	/* The display allocation update writes primary/peer endpoint IDs.
	 * The extra pipe count supplies byte8. This is full dual-pipe, so the
	 * T6050 limited-dual-pipe flag (byte9 bit1) remains clear.
	 */
	r->primary_id = cpu_to_le32(dcp->index);
	r->peer_id = cpu_to_le32(peer->index);
	r->extra_pipes = extra;
	r->flags &= ~3;
	return 0;
}
