/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Desktop 26.6 dual-pipe peer DCP — allocation, tiling rendezvous and
 * paired modeset for a sink wider than one pipe's maximum (G95NC 7680x2160).
 *
 * The 26.6 firmware uses the same A360-A373 display-pipeline methods as
 * firmware 27; its set_power_state is A473.  The sequence follows the
 * T6050 firmware-27 peer (aurora-silicon/linux#485), with the method and
 * callback numbers of the desktop 26.6 firmware: D115 reads the tiling
 * state, D116 writes it, and the peer's swaps go as a direct
 * swap_start/swap_submit chain (nothing advances the IOMFB transaction
 * queue on this firmware).  The two DISPEXT pipes merge upwards only:
 * the primary is firmware endpoint 1 (dcpext0), the peer endpoint 2.
 */

static DEFINE_MUTEX(dcp_peer_lock);

/* ---- tiling state (D114/D115 callbacks) ---- */

bool dcp_peer_tiling_state_26_6(struct apple_dcp *dcp, bool write,
				u32 event, u32 param, u32 *value)
{
	struct apple_dcp *peer;

	guard(mutex)(&dcp_peer_lock);
	if (dcp->peer_unbinding || dcp->crashed)
		return false;
	peer = dcp->peer_reserved ?: dcp->peer_owner;
	if (!peer || peer->peer_unbinding || peer->crashed)
		return false;
	if (!dcp->peer_tiling.vftg || !peer->peer_tiling.vftg)
		return false;
	if (write)
		return dcp_peer_tiling_set(&dcp->peer_tiling,
					   &peer->peer_tiling, event, param, *value);
	return dcp_peer_tiling_get(&dcp->peer_tiling, &peer->peer_tiling,
				   event, param, value);
}

/* ---- synchronous RPC helper ---- */

struct dcp_peer_wait {
	struct kref refs;
	struct completion done;
	size_t size;
	u8 reply[128];
};

static void dcp_peer_wait_free(struct kref *ref)
{
	kfree(container_of(ref, struct dcp_peer_wait, refs));
}

static void dcp_peer_wait_done(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_peer_wait *wait = cookie;

	if (wait->size && out)
		memcpy(wait->reply, out, min(wait->size, sizeof(wait->reply)));
	complete(&wait->done);
	kref_put(&wait->refs, dcp_peer_wait_free);
}

static int dcp_peer_call(struct apple_dcp *dcp, const char *tag,
			 void *in, size_t in_size, void *out, size_t out_size)
{
	struct dcp_method_entry method = { .name = "peer_call" };
	struct dcp_peer_wait *wait;
	int ret = 0;

	if (out_size > sizeof(wait->reply))
		return -EINVAL;
	wait = kzalloc(sizeof(*wait), GFP_KERNEL);
	if (!wait)
		return -ENOMEM;
	memcpy(method.tag, tag, 4);
	wait->size = out_size;
	init_completion(&wait->done);
	kref_init(&wait->refs);
	kref_get(&wait->refs);
	dcp_push(dcp, false, &method, in_size, out_size, in,
		 dcp_peer_wait_done, wait);
	if (!wait_for_completion_timeout(&wait->done,
		msecs_to_jiffies(!memcmp(tag, "A413", 4) ? 8500 : 2000)))
		ret = -ETIMEDOUT;
	else if (out_size && out)
		memcpy(out, wait->reply, out_size);
	kref_put(&wait->refs, dcp_peer_wait_free);
	return ret;
}

static int dcp_peer_call_oob(struct apple_dcp *dcp, const char *tag,
			     void *in, size_t in_size, void *out, size_t out_size)
{
	struct dcp_method_entry method = { .name = "peer_call_oob" };
	struct dcp_peer_wait *wait;
	int ret = 0;

	if (out_size > sizeof(wait->reply))
		return -EINVAL;
	wait = kzalloc(sizeof(*wait), GFP_KERNEL);
	if (!wait)
		return -ENOMEM;
	memcpy(method.tag, tag, 4);
	wait->size = out_size;
	init_completion(&wait->done);
	kref_init(&wait->refs);
	kref_get(&wait->refs);
	dcp_push(dcp, true, &method, in_size, out_size, in,
		 dcp_peer_wait_done, wait);
	if (!wait_for_completion_timeout(&wait->done, msecs_to_jiffies(2000)))
		ret = -ETIMEDOUT;
	else if (out_size && out)
		memcpy(out, wait->reply, out_size);
	kref_put(&wait->refs, dcp_peer_wait_free);
	return ret;
}

/* ---- peer release / forget ---- */

/*
 * The primary's display is gone (cable removed, port moved, system sleep):
 * give the reserved peer back.  A merged pair is stopped first; a pair that
 * still runs is kept.  The next allocation reserves a peer again.  Without
 * this the peer stays marked as an owner's peer after its port was taken by
 * another display, and swap_complete would discard that display's own swaps.
 */
/* Drop @owner's reservation of @peer unless their pair runs; peer lock held. */
static void dcp_peer_drop_locked(struct apple_dcp *owner, struct apple_dcp *peer)
{
	lockdep_assert_held(&dcp_peer_lock);
	if (owner->peer_reserved != peer || owner->peer_tiling.vftg ||
	    owner->peer_tiling.member || peer->peer_tiling.vftg ||
	    peer->peer_tiling.member)
		return;
	memset(&owner->peer_tiling, 0, sizeof(owner->peer_tiling));
	memset(&peer->peer_tiling, 0, sizeof(peer->peer_tiling));
	peer->peer_owner = NULL;
	owner->peer_reserved = NULL;
	owner->peer_active = false;
	dev_info(owner->dev, "peer %s no longer reserved: it drives a display of its own\n",
		 dev_name(peer->dev));
}

void dcp_peer_release_26_6(struct apple_dcp *dcp)
{
	guard(mutex)(&dcp_peer_lock);
	if (dcp->peer_reserved) {
		struct apple_dcp *peer = dcp->peer_reserved;

		if (dcp->peer_tiling.vftg || dcp->peer_tiling.member ||
		    peer->peer_tiling.vftg || peer->peer_tiling.member)
			return;
		memset(&dcp->peer_tiling, 0, sizeof(dcp->peer_tiling));
		memset(&peer->peer_tiling, 0, sizeof(peer->peer_tiling));
		peer->peer_owner = NULL;
		dcp->peer_reserved = NULL;
		dcp->peer_active = false;
	}
}

void dcp_peer_bound_26_6(struct apple_dcp *dcp)
{
	guard(mutex)(&dcp_peer_lock);
	dcp->peer_unbinding = false;
}

static void dcp_peer_forget(struct apple_dcp *dcp)
{
	guard(mutex)(&dcp_peer_lock);
	dcp->peer_unbinding = true;
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

/* ---- display IDs from D575 hotplug ---- */

void dcp_peer_display_ids(struct apple_dcp *dcp, u32 own, u32 related)
{
	struct apple_dcp *peer;

	guard(mutex)(&dcp_peer_lock);
	peer = dcp->peer_reserved ?: dcp->peer_owner;
	if (!peer || own == related || dcp->peer_unbinding || peer->peer_unbinding)
		return;
	dcp->peer_tiling.display_id = own;
	dcp->peer_tiling.display_id_valid = true;
	peer->peer_tiling.display_id = related;
	peer->peer_tiling.display_id_valid = true;
}

/* ---- peer allocation (DPTX call 21 handler) ---- */

/*
 * Unlike T6050, where the peer is a pipe without outputs, the only T6041
 * candidate (dcpext0) has Type-C routes and HDMI.  It qualifies while idle:
 * no active route and nothing connected.  Its fixed HDMI path brings the
 * DPTX link up at start without a sink (dcp_desktop_dp2hdmi_hpd()); that
 * link alone does not make it busy.  Returns a reason mask, 0 = eligible.
 */
static bool dcp_peer_hdmi_sinkless(struct apple_dcp *p)
{
	return p->fixed_phy && p->hdmi_hpd &&
	       !(p->connector && READ_ONCE(p->connector->connected));
}

static u32 dcp_peer_reject_26_6(struct apple_dcp *dcp, struct apple_dcp *p)
{
	u32 why = 0;

	if (!p->active || p->crashed || p->peer_unbinding)
		why |= BIT(0);
	if (p->peer_owner || p->peer_reserved)
		why |= BIT(1);
	if (p->active_typec_route)
		why |= BIT(2);
	if ((p->connector && p->connector->connected) || p->dptxport[1].connected)
		why |= BIT(3);
	if (p->dptxport[0].connected && !dcp_peer_hdmi_sinkless(p))
		why |= BIT(8);
	if (!dcp_peer_index_allowed_26_6(dcp->peer_endpoint, p->peer_endpoint))
		why |= BIT(4);
	if (p->dptx_die != dcp->dptx_die)
		why |= BIT(5);
	if (!p->crtc || p->crtc->base.dev != dcp->crtc->base.dev)
		why |= BIT(6);
	if (p->fw_compat != DCP_FIRMWARE_V_26_6_DESKTOP)
		why |= BIT(7);
	return why;
}

int dcp_peer_hints_26_6(struct apple_dcp *dcp, struct dptx_port *port,
			const void *data, size_t size, void *reply, size_t reply_size)
{
	struct dcp_display_hints *r = reply;
	struct apple_dcp *peer = NULL;
	struct drm_crtc *crtc;
	u32 width;

	if (size != sizeof(*r) || reply_size != sizeof(*r))
		return -EINVAL;
	memcpy(reply, data, sizeof(*r));
	r->status = 0;
	if (!of_device_is_compatible(dcp->dev->of_node, "apple,t6041-dcpext") ||
	    port->unit || !dcp->crtc)
		return 0;

	width = get_unaligned_le32(r->hints + 0x38);
	if (!(r->hints[0] & 1) || !width || width <= DCP_DESKTOP_PIPE_MAX_WIDTH) {
		memset(&r->allocation, 0, sizeof(r->allocation));
		return 0;
	}

	guard(mutex)(&dcp_peer_lock);
	if (dcp->peer_unbinding || dcp->crashed)
		return -ENODEV;
	/*
	 * A reservation made before a pipe got a display of its own (a Type-C
	 * display moved there, or an HDMI display that came) is stale: the
	 * pipe is nobody's peer any more.  A running pair keeps it.
	 */
	if (dcp->peer_owner && READ_ONCE(dcp->active_typec_route))
		dcp_peer_drop_locked(dcp->peer_owner, dcp);
	if (dcp->peer_reserved &&
	    (dcp_peer_reject_26_6(dcp, dcp->peer_reserved) & ~BIT(1)))
		dcp_peer_drop_locked(dcp, dcp->peer_reserved);
	if (dcp->peer_owner) {
		memset(&r->allocation, 0, sizeof(r->allocation));
		return 0;
	}
	peer = dcp->peer_reserved;
	if (!peer) {
		drm_for_each_crtc(crtc, dcp->crtc->base.dev) {
			struct apple_dcp *p = platform_get_drvdata(to_apple_crtc(crtc)->dcp);
			u32 why;

			if (!p || p == dcp ||
			    !of_device_is_compatible(p->dev->of_node, "apple,t6041-dcpext"))
				continue;
			why = dcp_peer_reject_26_6(dcp, p);
			dev_dbg(dcp->dev, "peer candidate %s endpoint %u: reject %#x\n",
				dev_name(p->dev), p->peer_endpoint, why);
			if (why)
				continue;
			if (!peer || p->peer_endpoint < peer->peer_endpoint)
				peer = p;
		}
		if (!peer) {
			dev_info(dcp->dev, "no idle peer pipe for a %u px display\n", width);
			WRITE_ONCE(dcp->peer_starved, true);
			dcp_fabric_hybrid_kick(dcp, 0);
			memset(&r->allocation, 0, sizeof(r->allocation));
			return 0;
		}
		WRITE_ONCE(dcp->peer_starved, false);
		dcp->peer_reserved = peer;
		peer->peer_owner = dcp;
		dev_info(dcp->dev, "reserved peer %s endpoint %u for %ux%u\n",
			 dev_name(peer->dev), peer->peer_endpoint, width,
			 get_unaligned_le32(r->hints + 0x3c));
	}

	r->primary_id = cpu_to_le32(dcp->peer_endpoint);
	r->peer_id = cpu_to_le32(peer->peer_endpoint);
	/* One extra pipe, full dual pipe: the limited-dual-pipe flag stays clear. */
	r->extra_pipes = 1;
	r->flags &= ~3;
	return 0;
}

/* ---- paired power (A473 set_power_state) ---- */

/* As dcp_power_state_req(): the state, no transition flags, a result. */
static void dcp_peer_power_req(u8 *req, bool on)
{
	memset(req, 0, 16);
	put_unaligned_le64(on, req);
	memset(req + 13, 0xaa, 3);
}

static int dcp_peer_power(struct apple_dcp *dcp, struct apple_dcp *peer, bool on)
{
	struct dcp_peer_wait *wait;
	struct dcp_method_entry method = { .name = "peer_power",
		.tag = {'A', '4', '7', '3'} };
	u8 power[16], response[8];
	int ret;

	wait = kzalloc(sizeof(*wait), GFP_KERNEL);
	if (!wait)
		return -ENOMEM;
	kref_init(&wait->refs);
	init_completion(&wait->done);
	wait->size = sizeof(response);
	dcp_peer_power_req(power, on);
	kref_get(&wait->refs);
	dcp_push(dcp, false, &method, sizeof(power), sizeof(response), power,
		 dcp_peer_wait_done, wait);
	ret = dcp_peer_call(peer, "A473", power, sizeof(power),
			    response, sizeof(response));
	if (!ret && get_unaligned_le32(response + 4))
		ret = -EIO;
	if (!wait_for_completion_timeout(&wait->done, msecs_to_jiffies(2000)))
		ret = ret ?: -ETIMEDOUT;
	else if (!ret && get_unaligned_le32(wait->reply + 4))
		ret = -EIO;
	kref_put(&wait->refs, dcp_peer_wait_free);
	dev_dbg(dcp->dev, "peer power %u result=%d\n", on, ret);
	return ret;
}

/* ---- quiesce: tear down the pair ---- */

static int dcp_peer_quiesce(struct apple_dcp *dcp, struct apple_dcp *peer)
{
	__le32 manage = 0;
	int ret;

	{
		guard(mutex)(&dcp_peer_lock);
		dcp->peer_tiling.modeset_complete = false;
		peer->peer_tiling.modeset_complete = false;
	}
	ret = dcp_peer_power(dcp, peer, false);
	if (ret)
		return ret;
	msleep(100);
	ret = dcp_peer_call(dcp, "A362", &manage, sizeof(manage), NULL, 0);
	if (ret)
		return ret;
	ret = dcp_peer_call(peer, "A362", &manage, sizeof(manage), NULL, 0);
	if (ret)
		return ret;
	{
		guard(mutex)(&dcp_peer_lock);
		dcp->peer_tiling.member = peer->peer_tiling.member = false;
		dcp->peer_tiling.vftg = peer->peer_tiling.vftg = false;
		dcp->peer_tiling.modeset_complete = false;
		peer->peer_tiling.modeset_complete = false;
		dcp->peer_tiling.phase = peer->peer_tiling.phase = 0;
		dcp->peer_tiling.modeset_state = peer->peer_tiling.modeset_state = 0;
		dcp->peer_tiling.ready = peer->peer_tiling.ready = false;
	}
	return 0;
}

/*
 * Back to one pipe: quiesce the pair, then power the primary again.  A DRM
 * commit that only changes the mode does not power the CRTC on again.
 */
static int dcp_peer_unpair(struct apple_dcp *dcp, struct apple_dcp *peer)
{
	u8 power[16], response[8];
	int ret;

	dcp_peer_power_req(power, true);
	ret = dcp_peer_quiesce(dcp, peer);
	if (!ret)
		ret = dcp_peer_call(dcp, "A473", power, sizeof(power),
				    response, sizeof(response));
	if (!ret && get_unaligned_le32(response + 4))
		ret = -EIO;
	dev_info(dcp->dev, "peer unpair result=%d\n", ret);
	return ret;
}

/*
 * The primary is being powered off or put to sleep (DPMS, lock screen):
 * stop the pair first.  On m4.135 the lock screen's DPMS-off ran the
 * single-pipe power-off against a merged pair: set_power_state(0) timed
 * out, the DPTX HPD deassert failed and the firmware looped until reboot.
 * Returns 1 after a paired stop, 0 for an unpaired pipe, or an error.
 */
int dcp_peer_stop_26_6(struct apple_dcp *dcp)
{
	struct apple_dcp *peer;
	int ret;

	scoped_guard(mutex, &dcp_peer_lock) {
		peer = dcp->peer_reserved;
		if (!peer || peer->peer_owner != dcp ||
		    (!dcp->peer_tiling.vftg && !dcp->peer_tiling.member &&
		     !dcp->peer_active))
			return 0;
		dcp->peer_active = false;
	}
	ret = dcp_peer_quiesce(dcp, peer);
	dev_info(dcp->dev, "peer stop before power-off: %d\n", ret);
	return ret ? ret : 1;
}

/* ---- DSC copy from primary to peer ---- */

static int dcp_peer_copy_dsc(struct apple_dcp *primary, struct apple_dcp *peer)
{
	u8 caps_req[36] = {}, caps[32], bpp_req[4] = {}, bpp[8];
	u8 hints_req[12] = {}, hints[12];
	u32 required;
	int ret;

	ret = dcp_peer_call_oob(primary, "A369", NULL, 0, &required, 4);
	dev_dbg(primary->dev, "peer DSC required: ret=%d value=%#x\n", ret, required);
	if (ret || !(required & 1))
		return ret;
	ret = dcp_peer_call_oob(primary, "A367", caps_req, 36, caps, 32);
	if (ret)
		return ret;
	memcpy(caps_req, caps, 32);
	ret = dcp_peer_call(peer, "A368", caps_req, 36, caps, 32);
	if (ret)
		return ret;
	ret = dcp_peer_call_oob(primary, "A370", bpp_req, 4, bpp, 8);
	if (ret || get_unaligned_le32(bpp + 2))
		return ret ?: -EIO;
	memcpy(bpp_req, bpp, 2);
	ret = dcp_peer_call(peer, "A371", bpp_req, 4, bpp, 8);
	if (ret || get_unaligned_le32(bpp + 2))
		return ret ?: -EIO;
	ret = dcp_peer_call_oob(primary, "A372", hints_req, 12, hints, 12);
	if (ret || get_unaligned_le32(hints + 8))
		return ret ?: -EIO;
	memcpy(hints_req, hints, 8);
	ret = dcp_peer_call(peer, "A373", hints_req, 12, hints, 12);
	if (ret || get_unaligned_le32(hints + 8))
		return ret ?: -EIO;
	dev_dbg(primary->dev, "peer DSC copied: bpp=%u hints=%*ph\n",
		get_unaligned_le16(bpp), 8, hints);
	return 0;
}

/* ---- split timing for the half-width derived mode ---- */

static void dcp_peer_split_timing(u8 *timing)
{
	static const u8 half_fields[] = { 4, 8, 12, 16, 20, 28, 68 };

	for (unsigned int i = 0; i < sizeof(half_fields); i++) {
		u8 *field = timing + half_fields[i];

		put_unaligned_le32(get_unaligned_le32(field) >> 1, field);
	}
	put_unaligned_le32((s32)get_unaligned_le32(timing + 24) / 2,
			   timing + 24);
}

/* ---- A364/A365: get video data and insert merge modes ---- */

struct dcp_peer_video_data {
	u8 timing[0x50];
	u8 color[0x20];
} __packed;

struct dcp_peer_get_video_req {
	struct dcp_peer_video_data video;
	__le32 timing_id, color_id;
	u8 requires_merge, timing_null, color_null, requires_merge_null;
} __packed;

struct dcp_peer_get_video_resp {
	struct dcp_peer_video_data video;
	u8 requires_merge, success, padding[2];
} __packed;

struct dcp_peer_insert_video_req {
	struct dcp_peer_video_data video;
	__le32 timing_id, color_id;
	u8 timing_null, color_null, padding[2];
} __packed;

struct dcp_peer_insert_video_resp {
	struct dcp_peer_video_data video;
	u8 success, padding[3];
} __packed;

static_assert(sizeof(struct dcp_peer_get_video_req) == 0x7c);
static_assert(sizeof(struct dcp_peer_get_video_resp) == 0x74);
static_assert(sizeof(struct dcp_peer_insert_video_req) == 0x7c);
static_assert(sizeof(struct dcp_peer_insert_video_resp) == 0x74);

/* ---- paired modeset ---- */

static int dcp_peer_prepare_vftg(struct apple_dcp *dcp, struct apple_dcp *peer,
				 const struct dcp_peer_insert_video_req *video)
{
	guard(mutex)(&dcp_peer_lock);
	if (dcp->peer_reserved != peer || peer->peer_owner != dcp)
		return -ENODEV;
	dcp->peer_tiling.vftg = peer->peer_tiling.vftg = true;
	dcp->peer_tiling.modeset_complete = false;
	peer->peer_tiling.modeset_complete = false;
	dcp->peer_tiling.primary = true;
	peer->peer_tiling.primary = false;
	dcp->peer_tiling.width = peer->peer_tiling.width =
		get_unaligned_le32(video->video.timing + 8);
	dcp->peer_tiling.height = peer->peer_tiling.height =
		get_unaligned_le32(video->video.timing + 0x28);
	dcp->peer_tiling.x = dcp->peer_tiling.y = peer->peer_tiling.y = 0;
	peer->peer_tiling.x = dcp->peer_tiling.width;
	return 0;
}

/*
 * A360/A361: both engines must hand out the same swap ID for a paired
 * frame.  The primary consumed one more ID than the peer during the paired
 * mode set on m4.134 (peer 74, primary 75), so this runs again afterwards.
 */
static int dcp_peer_sync_swap_ids(struct apple_dcp *dcp, struct apple_dcp *peer,
				  const char *when)
{
	u32 primary_id, peer_id, status;
	int ret;

	ret = dcp_peer_call(dcp, "A360", NULL, 0, &primary_id, 4);
	if (ret)
		return ret;
	ret = dcp_peer_call(peer, "A360", NULL, 0, &peer_id, 4);
	if (ret)
		return ret;
	dev_dbg(dcp->dev, "peer master swap IDs %s: primary %u peer %u\n",
		when, primary_id, peer_id);
	if (primary_id != peer_id) {
		status = max(primary_id, peer_id);
		ret = dcp_peer_call(primary_id < peer_id ? dcp : peer,
				    "A361", &status, 4, NULL, 0);
	}
	return ret;
}

/*
 * Adaptive sync (IOMFBParameter 14, minimum rate in 16.16 Hz, 0 = off) goes
 * to both pipes before the paired mode set, as the single-pipe mode set
 * sends it before the mode is selected.  A pipe keeps the parameter across
 * a replug: a peer that last ran a VRR mode alone would otherwise merge
 * with VRR on while the primary runs without it, which crashed its firmware.
 */
static int dcp_peer_adaptive_sync(struct apple_dcp *pipe, u32 min_vrr)
{
	struct dcp_set_parameter_dcp param = {
		.param = IOMFBPARAM_ADAPTIVE_SYNC,
		.value = { min_vrr },
		.count = 3,
	};
	u32 status = 0;
	int ret;

	ret = dcp_peer_call(pipe, "A442", &param, sizeof(param), &status, 4);
	dev_dbg(pipe->dev, "peer adaptive sync minRR %#x: ret=%d status=%#x\n",
		min_vrr, ret, status);
	return ret;
}

static int dcp_peer_modeset(struct apple_dcp *dcp, struct apple_dcp *peer,
			    const struct dcp_peer_insert_video_req *video,
			    bool firmware_requires_merge, u32 min_vrr)
{
	struct dcp_peer_wait *wait;
	struct dcp_method_entry method = { .name = "peer_modeset",
		.tag = {'A', '4', '1', '3'} };
	/* A413 takes the color mode first, as dcp_set_digital_out_mode_req. */
	__le32 mode[2] = {video->color_id, video->timing_id};
	u32 phase, status;
	unsigned long deadline;
	int ret;

	{
		guard(mutex)(&dcp_peer_lock);
		if (dcp->peer_reserved != peer || peer->peer_owner != dcp ||
		    !dcp->peer_tiling.display_id_valid ||
		    !peer->peer_tiling.display_id_valid)
			return -ENODEV;
		dcp->peer_tiling.member = peer->peer_tiling.member = true;
		dcp->peer_tiling.mode_flag = peer->peer_tiling.mode_flag =
			!firmware_requires_merge;
	}
	ret = dcp_peer_power(dcp, peer, true);
	if (ret)
		return ret;
	msleep(100);

	ret = dcp_peer_adaptive_sync(dcp, min_vrr);
	if (!ret)
		ret = dcp_peer_adaptive_sync(peer, min_vrr);
	if (ret)
		return ret;

	ret = dcp_peer_sync_swap_ids(dcp, peer, "before A413");
	if (ret)
		return ret;

	/* Set phase to 1 on both engines */
	phase = 1;
	dcp_peer_tiling_state_26_6(dcp, true, 1, 10, &phase);
	dcp_peer_tiling_state_26_6(peer, true, 1, 10, &phase);

	/* A413 on primary (async), then wait for phase >= 2 */
	wait = kzalloc(sizeof(*wait), GFP_KERNEL);
	if (!wait)
		return -ENOMEM;
	init_completion(&wait->done);
	kref_init(&wait->refs);
	kref_get(&wait->refs);
	wait->size = 4;
	dcp_push(dcp, false, &method, sizeof(mode), 4, mode,
		 dcp_peer_wait_done, wait);

	deadline = jiffies + msecs_to_jiffies(8500);
	for (;;) {
		phase = 0;
		if (!dcp_peer_tiling_state_26_6(dcp, false, 1, 10, &phase)) {
			ret = -ENODEV;
			goto put;
		}
		if (phase >= 2)
			break;
		/*
		 * The 26.6 desktop primary merges the timing and completes its
		 * A413 without waiting for the peer (no phase2 rendezvous seen
		 * on m4.132): go on to the peer once it succeeded.
		 */
		if (completion_done(&wait->done)) {
			status = get_unaligned_le32(wait->reply);
			dev_dbg(dcp->dev,
				"peer primary A413 done before phase2: phase=%u status=%#x\n",
				phase, status);
			if (status) {
				ret = -EIO;
				goto put;
			}
			break;
		}
		if (time_after_eq(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			dev_err(dcp->dev,
				"peer primary timed out before phase2: phase=%u\n", phase);
			goto put;
		}
		msleep(2);
	}
	dev_dbg(dcp->dev, "peer primary reached modeset phase%u\n", phase);

	/* Copy DSC parameters from primary to peer */
	ret = dcp_peer_copy_dsc(dcp, peer);
	if (ret)
		goto put;

	/* A413 on peer */
	ret = dcp_peer_call(peer, "A413", mode, sizeof(mode), &status, 4);
	if (ret || status) {
		dev_err(dcp->dev, "peer A413 failed: ret=%d status=%#x\n", ret, status);
		ret = ret ?: -EIO;
		goto put;
	}

	/* Wait for primary's A413 to complete */
	if (!wait_for_completion_timeout(&wait->done, msecs_to_jiffies(8500)))
		ret = -ETIMEDOUT;
	else if (get_unaligned_le32(wait->reply))
		ret = -EIO;
	if (!ret)
		ret = dcp_peer_sync_swap_ids(dcp, peer, "after the paired modeset");
put:
	kref_put(&wait->refs, dcp_peer_wait_free);
	if (!ret) {
		guard(mutex)(&dcp_peer_lock);
		dcp->peer_tiling.modeset_complete = true;
		peer->peer_tiling.modeset_complete = true;
	}
	dev_dbg(dcp->dev, "peer paired modeset result=%d\n", ret);
	return ret;
}

/* ---- install video: A364+A365+A366+modeset ---- */

static int dcp_peer_install_video(struct apple_dcp *dcp, struct apple_dcp *peer,
				  u32 timing, u32 color, u32 min_vrr)
{
	struct dcp_peer_get_video_req req = {};
	struct dcp_peer_get_video_resp resp;
	struct dcp_peer_insert_video_req insert = {};
	struct dcp_peer_insert_video_resp inserted;
	__le32 enabled = cpu_to_le32(1);
	int ret;

	req.timing_id = cpu_to_le32(timing);
	req.color_id = cpu_to_le32(color);
	ret = dcp_peer_call(dcp, "A364", &req, sizeof(req), &resp, sizeof(resp));
	if (ret)
		return ret;
	dev_dbg(dcp->dev, "peer video timing=%u color=%u success=%u merge=%u\n",
		timing, color, resp.success, resp.requires_merge);
	if (!(resp.success & 1))
		return -ENOENT;
	if (!(resp.requires_merge & 1)) {
		if (dcp->peer_tiling.vftg || peer->peer_tiling.vftg) {
			ret = dcp_peer_unpair(dcp, peer);
			if (ret)
				return ret;
		}
		return -EOPNOTSUPP;
	}

	/* An HDMI sink plugged later connects the link again (HPD irq). */
	if (peer->dptxport[0].connected && dcp_peer_hdmi_sinkless(peer)) {
		scoped_guard(mutex, &peer->hpd_mutex)
			if (dptx_service_available(&peer->dptxport[0]))
				dptxport_set_hpd(peer->dptxport[0].service, false);
		ret = dcp_dptx_disconnect(peer, 0);
		dev_dbg(dcp->dev, "peer HDMI link released: %d\n", ret);
		if (ret)
			return ret;
	}

	ret = dcp_peer_quiesce(dcp, peer);
	if (ret)
		return ret;

	insert.video = resp.video;
	dcp_peer_split_timing(insert.video.timing);
	insert.timing_id = cpu_to_le32(timing + 10000);
	insert.color_id = cpu_to_le32(color + 10000);

	ret = dcp_peer_prepare_vftg(dcp, peer, &insert);
	if (ret)
		return ret;

	/* A366: setup_peer_dcp on the peer */
	ret = dcp_peer_call(peer, "A366", &enabled, sizeof(enabled), NULL, 0);
	if (ret)
		return ret;

	/* A365: insertMergeModes on primary */
	ret = dcp_peer_call(dcp, "A365", &insert, sizeof(insert),
			    &inserted, sizeof(inserted));
	if (!ret && !(inserted.success & 1))
		ret = -EIO;
	if (ret)
		return ret;

	/* A365: insertMergeModes on peer with primary's output */
	insert.video = inserted.video;
	ret = dcp_peer_call(peer, "A365", &insert, sizeof(insert),
			    &inserted, sizeof(inserted));
	if (!ret && !(inserted.success & 1))
		ret = -EIO;
	if (ret)
		return ret;

	dev_dbg(dcp->dev, "peer derived mode timing=%u color=%u width=%u height=%u\n",
		timing + 10000, color + 10000,
		get_unaligned_le32(insert.video.timing + 8),
		get_unaligned_le32(insert.video.timing + 0x28));

	/* Paired modeset: A473+A360/A361+A413 on both */
	return dcp_peer_modeset(dcp, peer, &insert, resp.requires_merge & 1,
				min_vrr);
}

/* ---- swaps on the peer pipe ---- */

static_assert(sizeof(struct dcp_swap_submit_req_v26_6_desktop) == DCP_V26_SWAP_SIZE);

static const struct dcp_method_entry dcp_peer_swap_start_method = {
	.name = "peer_swap_start", .tag = { 'A', '4', '0', '6' },
};

static const struct dcp_method_entry dcp_peer_swap_submit_method = {
	.name = "peer_swap_submit", .tag = { 'A', '4', '0', '7' },
};

static const struct dcp_method_entry dcp_peer_set_master_method = {
	.name = "peer_set_master_swap_id", .tag = { 'A', '3', '6', '1' },
};

/*
 * One swap on a pipe: swap_start, then swap_submit of @wire.  The desktop
 * 26.6 firmware does not use the IOMFB transaction queue (nothing advances
 * it there: m4.134 timed every peer swap out after 10 s), so the two calls
 * are chained directly.  The firmware's swap ID must equal @swap_id unless
 * that is U32_MAX; on a mismatch the peer's master swap ID is set to it
 * once and the swap is started again.  @owner is the primary of a paired
 * DRM frame.
 */
struct dcp_peer_swap {
	struct apple_dcp *owner;
	u32 swap_id;
	__le32 master;
	bool retried;
	struct dcp_swap_submit_req_v26_6_desktop wire;
};

static void dcp_peer_frame_failed(struct apple_dcp *owner)
{
	guard(mutex)(&dcp_peer_lock);
	owner->peer_failed = true;
}

static void dcp_peer_swap_finish(struct apple_dcp *dcp, struct dcp_peer_swap *ps,
				 int result, u32 swap_id)
{
	if (result) {
		dev_err_ratelimited(dcp->dev, "peer swap %u failed: %d\n", swap_id, result);
		if (ps->owner)
			dcp_peer_frame_failed(ps->owner);
	}
	kfree(ps);
}

static void dcp_peer_swap_submitted(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_peer_swap *ps = cookie;
	struct dcp_swap_submit_resp_v26_6_desktop *resp = out;

	dcp_peer_swap_finish(dcp, ps, resp && !resp->ret ? 0 : -EIO,
			     ps->wire.swap.swap_id);
}

static void dcp_peer_swap_begin(struct apple_dcp *dcp, struct dcp_peer_swap *ps);

static void dcp_peer_swap_resynced(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_peer_swap *ps = cookie;

	dcp_peer_swap_begin(dcp, ps);
}

static void dcp_peer_swap_started(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_peer_swap *ps = cookie;
	struct dcp_swap_start_resp_v26_6_desktop *resp = out;

	if (!resp || resp->ret) {
		dcp_peer_swap_finish(dcp, ps, -EIO, resp ? resp->swap_id : 0);
		return;
	}
	if (ps->swap_id != U32_MAX && resp->swap_id != ps->swap_id) {
		if (!ps->retried && !READ_ONCE(dcp->crashed)) {
			dev_info_ratelimited(dcp->dev,
					     "peer swap ID %u, primary %u: setting the master ID\n",
					     resp->swap_id, ps->swap_id);
			ps->retried = true;
			ps->master = cpu_to_le32(ps->swap_id);
			dcp_push(dcp, false, &dcp_peer_set_master_method,
				 sizeof(ps->master), 0, &ps->master,
				 dcp_peer_swap_resynced, ps);
			return;
		}
		dev_err(dcp->dev, "peer swap ID %u, primary %u\n",
			resp->swap_id, ps->swap_id);
		dcp_peer_swap_finish(dcp, ps, -EPROTO, resp->swap_id);
		return;
	}
	ps->wire.swap.swap_id = resp->swap_id;
	dcp_push(dcp, false, &dcp_peer_swap_submit_method, sizeof(ps->wire),
		 sizeof(struct dcp_swap_submit_resp_v26_6_desktop), &ps->wire,
		 dcp_peer_swap_submitted, ps);
}

static void dcp_peer_swap_begin(struct apple_dcp *dcp, struct dcp_peer_swap *ps)
{
	struct dcp_swap_start_req_v26_6_desktop req = {
		.client = dcp->desktop->client,
		.padding = { 0xaa, 0xaa, 0xaa },
	};

	dcp_push(dcp, false, &dcp_peer_swap_start_method, sizeof(req),
		 sizeof(struct dcp_swap_start_resp_v26_6_desktop), &req,
		 dcp_peer_swap_started, ps);
}

/* Takes over @ps unless it returns an error. */
static int dcp_peer_queue_swap(struct apple_dcp *dcp, struct dcp_peer_swap *ps)
{
	if (READ_ONCE(dcp->crashed) || !dcp->active || !dcp->desktop) {
		kfree(ps);
		return -EIO;
	}
	dcp_peer_swap_begin(dcp, ps);
	return 0;
}

/*
 * The part of a layer inside the tile [@tile_x, @tile_x + @tile_w), in the
 * tile's coordinates.  The shared source boundary is rounded once, so the
 * halves neither lose nor repeat a source column.
 */
static bool dcp_peer_clip(const struct dcp_rect *src, const struct dcp_rect *dst,
			  u32 tile_x, u32 tile_w,
			  struct dcp_rect *tile_src, struct dcp_rect *tile_dst)
{
	u64 left = max_t(u64, dst->x, tile_x);
	u64 right = min_t(u64, (u64)dst->x + dst->w, (u64)tile_x + tile_w);
	u64 first, last;

	if (!src->w || !src->h || !dst->w || !dst->h || left >= right)
		return false;
	first = div_u64((left - dst->x) * src->w + dst->w / 2, dst->w);
	last = div_u64((right - dst->x) * src->w + dst->w / 2, dst->w);
	if (first >= last || (u64)src->x + last > U32_MAX)
		return false;
	*tile_src = *src;
	tile_src->x += first;
	tile_src->w = last - first;
	*tile_dst = *dst;
	tile_dst->x = left - tile_x;
	tile_dst->w = right - left;
	return true;
}

/* Both pipes scan the same buffers: the display DARTs share one domain. */
static void dcp_peer_split_swap(struct dcp_swap_submit_req_v26_6_desktop *left,
				struct dcp_swap_submit_req_v26_6_desktop *right,
				u32 tile_w)
{
	*right = *left;
	for (unsigned int i = 0; i < SWAP_SURFACES; i++) {
		struct dcp_swap_submit_req_v26_6_desktop *halves[] = { left, right };
		struct dcp_rect src = left->swap.src_rect[i];
		struct dcp_rect dst = left->swap.dst_rect[i];
		bool present = !left->surf_null[i];

		for (unsigned int side = 0; side < 2; side++) {
			struct dcp_swap_submit_req_v26_6_desktop *req = halves[side];
			struct dcp_rect tile_src, tile_dst;

			if (present && dcp_peer_clip(&src, &dst, side * tile_w, tile_w,
						     &tile_src, &tile_dst)) {
				req->swap.src_rect[i] = tile_src;
				req->swap.dst_rect[i] = tile_dst;
			} else {
				req->surf_null[i] = true;
				req->surf_iova[i] = 0;
				memset(&req->swap.src_rect[i], 0, sizeof(struct dcp_rect));
				memset(&req->swap.dst_rect[i], 0, sizeof(struct dcp_rect));
			}
		}
	}
}

/*
 * Paired DRM frame: the primary's swap_start returned @swap_id for @req.
 * @req keeps the left tile; the right tile goes to the peer as its own swap.
 */
void dcp_peer_swap_submit_26_6(struct apple_dcp *dcp,
			       struct dcp_swap_submit_req_v26_6_desktop *req,
			       u32 swap_id)
{
	struct dcp_peer_swap *ps;
	struct apple_dcp *peer;
	u32 tile_w;

	if (!READ_ONCE(dcp->peer_active))
		return;
	scoped_guard(mutex, &dcp_peer_lock) {
		peer = dcp->peer_reserved;
		if (!dcp->peer_active || dcp->peer_failed || !peer ||
		    peer->peer_owner != dcp || peer->crashed || peer->peer_unbinding ||
		    !dcp->peer_tiling.modeset_complete ||
		    !peer->peer_tiling.modeset_complete || !dcp->peer_tiling.width)
			return;
		tile_w = dcp->peer_tiling.width;
	}
	ps = kzalloc(sizeof(*ps), GFP_KERNEL);
	if (!ps) {
		dcp_peer_frame_failed(dcp);
		return;
	}
	ps->owner = dcp;
	ps->swap_id = swap_id;
	dcp_peer_split_swap(req, &ps->wire, tile_w);
	if (dcp_peer_queue_swap(peer, ps)) {
		dev_err_ratelimited(dcp->dev, "peer swap %u not queued\n", swap_id);
		dcp_peer_frame_failed(dcp);
	}
}

/*
 * A swap completed on a reserved peer.  Its own CRTC is idle; the frame is
 * the primary's, which reports it from its own completion.
 */
bool dcp_peer_swap_complete_26_6(struct apple_dcp *dcp, u32 swap_id)
{
	/* Only a peer without a display of its own: its swaps are the pair's. */
	if (!READ_ONCE(dcp->peer_owner) || READ_ONCE(dcp->active_typec_route))
		return false;
	WRITE_ONCE(dcp->last_swap_id, swap_id);
	WRITE_ONCE(dcp->have_swap_complete, true);
	if (!dcp->peer_complete_logged) {
		dcp->peer_complete_logged = true;
		dev_info(dcp->dev, "peer pipe completed swap %u\n", swap_id);
	}
	return true;
}

/*
 * DRM mode set on a primary with a reserved peer.  A mode wider than one
 * pipe is set on both pipes through the merge path; -EOPNOTSUPP leaves the
 * mode to the single-pipe path, also after a failed paired mode set.
 */
int dcp_peer_drm_modeset_26_6(struct apple_dcp *dcp, u32 hdisplay,
			      u32 timing, u32 color, u32 min_vrr)
{
	struct apple_dcp *peer;
	bool restart = false;
	int ret;

	scoped_guard(mutex, &dcp_peer_lock) {
		peer = dcp->peer_reserved;
		if (!peer || peer->peer_owner != dcp)
			return -EOPNOTSUPP;
		dcp->peer_active = false;
	}
	if (hdisplay <= DCP_DESKTOP_PIPE_MAX_WIDTH || timing > U32_MAX - 10000 ||
	    color > U32_MAX - 10000) {
		if (dcp->peer_tiling.vftg || peer->peer_tiling.vftg)
			dcp_peer_unpair(dcp, peer);
		return -EOPNOTSUPP;
	}
	/*
	 * DRM sets the same mode again right after the pair came up.  The
	 * merged pipe no longer resolves the original timing in A364, so a
	 * second install would fail and tear the pair down: keep it.  The pair
	 * runs with the mode's adaptive-sync rate, so VRR switched on or off
	 * keeps it too; only a changed rate for the same timing (a new mode
	 * list) stops the pair and installs it again.
	 */
	scoped_guard(mutex, &dcp_peer_lock) {
		if (dcp->peer_tiling.modeset_complete &&
		    peer->peer_tiling.modeset_complete && !dcp->peer_failed &&
		    dcp->peer_mode_timing == timing && dcp->peer_mode_color == color) {
			if (dcp->peer_min_vrr == min_vrr) {
				dcp->peer_active = true;
				dev_info(dcp->dev,
					 "paired DRM modeset %u px timing %u color %u: kept\n",
					 hdisplay, timing, color);
				return 0;
			}
			restart = true;
		}
	}
	if (restart) {
		ret = dcp_peer_quiesce(dcp, peer);
		dev_info(dcp->dev, "paired DRM modeset: adaptive sync %#x -> %#x, pair stopped: %d\n",
			 dcp->peer_min_vrr, min_vrr, ret);
		if (ret)
			return -EOPNOTSUPP;
	}
	ret = dcp_peer_install_video(dcp, peer, timing, color, min_vrr);
	dev_info(dcp->dev, "paired DRM modeset %u px timing %u color %u adaptive sync %#x: %d\n",
		 hdisplay, timing, color, min_vrr, ret);
	if (ret) {
		if (dcp->peer_tiling.vftg || peer->peer_tiling.vftg)
			dcp_peer_unpair(dcp, peer);
		return -EOPNOTSUPP;
	}
	scoped_guard(mutex, &dcp_peer_lock) {
		dcp->peer_active = true;
		dcp->peer_failed = false;
		dcp->peer_mode_timing = timing;
		dcp->peer_mode_color = color;
		dcp->peer_min_vrr = min_vrr;
		peer->peer_complete_logged = false;
	}
	return 0;
}
