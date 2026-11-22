/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * T602x pairs two same-die display engines for a single direct DP sink.
 */
#include <clocksource/arm_arch_timer.h>
#include <linux/kthread.h>
#include <uapi/linux/sched/types.h>
#include "dcp-peer-v13_5.h"
#include "plane.h"

struct t602x_scanout {
	struct apple_dcp *owner, *peer;
	struct apple_epic_service *controller;
	u32 width, timing, color;
	u32 spare_id;
	int spare_error;
	bool has_spare;
	u64 generation, period_ns, last_submit_ns;
	struct kthread_worker *worker;
	struct kthread_work work;
	struct drm_atomic_commit *commit;
	struct dcp_swap_submit_req_v13_3 request;
	bool failed, pair_touched, merge_enabled, quarantined;
	/* The two DCPs, their PIODMA children, and the scanout DMA device. */
	struct dcp_session_ref dma[5];
	unsigned int dma_count;
	struct drm_framebuffer *scanned[SWAP_SURFACES], *next[SWAP_SURFACES];
	struct dcp_swap_submit_req_v13_3 frames[2];
};

static DEFINE_MUTEX(t602x_scanout_lock);

bool dcp_t602x_ext13_5(struct apple_dcp *dcp)
{
	return dcp->hw.t602x_paired_dp && !dcp->main_display && dcp->index && dcp->index <= 4 &&
	       dcp->fw_compat == DCP_FIRMWARE_V_13_5;
}

static bool dcp_t602x_can_pair(struct apple_dcp *dcp)
{
	return dcp_t602x_ext13_5(dcp) &&
	       of_property_present(dcp->dev->of_node, "apple,scanout-peers");
}

bool dcp_t602x_needs_pair(struct apple_dcp *dcp, const struct drm_display_mode *mode)
{
	struct dcp_dptx_caps caps = dcp_t602x_limits(&dcp->dptxport[0].caps);
	struct dcp_display_mode entry;

	if (!dcp_t602x_can_pair(dcp) ||
	    dcp_t602x_mode_fits(caps, mode->hdisplay, mode->vdisplay, mode->htotal,
				mode->vtotal, mode->clock, 1))
		return false;

	/*
	 * With no reported clock limit, the fallback describes paired RGB.
	 * Trust ordinary firmware catalogues and their non-RGB formats.
	 */
	if (!dcp->dptxport[0].caps.total_rate && (u64)mode->clock * 1000 > caps.total_rate) {
		caps.total_rate = U32_MAX;
		if (dcp_t602x_mode_fits(caps, mode->hdisplay, mode->vdisplay, mode->htotal,
					mode->vtotal, mode->clock, 1) &&
		    lookup_mode(dcp, mode, &entry) &&
		    (!entry.t602x_pair_offered || entry.sdr_rgb.score < 0))
			return false;
	}
	return true;
}

static bool dcp_t602x_mode_has_equal_halves(const struct drm_display_mode *mode)
{
	return dcp_t602x_even_horizontal(mode->hdisplay, mode->hsync_start,
					 mode->hsync_end, mode->htotal);
}

static struct apple_dcp *dcp_t602x_idle_peer_locked(struct apple_dcp *dcp, u32 index)
{
	struct apple_dcp *peer = NULL;
	struct drm_crtc *crtc;

	lockdep_assert_held(&dcp_peer_lock);
	drm_for_each_crtc(crtc, dcp->crtc->base.dev) {
		struct apple_dcp *p = platform_get_drvdata(to_apple_crtc(crtc)->dcp);
		struct apple_connector *connector;
		struct dcp_dptx_caps caps, required;

		if (!p || p == dcp || (index && p->index != index) || !dcp_t602x_ext13_5(p) ||
		    p->dptx_die != dcp->dptx_die ||
		    !dcp_t602x_is_peer(dcp->dev->of_node, p->dev->of_node) ||
		    !p->active || p->crashed || p->peer_unbinding || READ_ONCE(p->quiescing) ||
		    p->peer_reserved || p->peer_owner ||
		    dcp_t602x_port_reserved(p) || READ_ONCE(p->active_typec_route) ||
		    READ_ONCE(p->typec_cable_connected) || READ_ONCE(p->dptxport[0].connected) ||
		    READ_ONCE(p->dptxport[1].connected))
			continue;
		caps = dcp_t602x_limits(&p->dptxport[0].caps);
		required = dcp_t602x_limits(&dcp->dptxport[0].caps);
		if (caps.width < required.width || caps.total_rate < required.total_rate ||
		    caps.active_rate < required.active_rate)
			continue;
		connector = READ_ONCE(p->connector);
		if (connector && READ_ONCE(connector->connected))
			continue;
		if (!peer || p->index < peer->index)
			peer = p;
	}
	return peer;
}

static bool dcp_t602x_pair_fits(struct apple_dcp *dcp, const struct drm_display_mode *mode)
{
	const u32 sync_flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC |
			       DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC;

	/* Only ordinary progressive timing with optional sync polarities. */
	return dcp->crtc && READ_ONCE(dcp->active_typec_route) &&
	       !READ_ONCE(dcp->dptx_tunnel) && dcp_t602x_mode_has_equal_halves(mode) &&
	       !(mode->flags & ~sync_flags) && !mode->hskew && mode->vscan <= 1 &&
	       dcp_t602x_mode_fits(dcp_t602x_limits(&dcp->dptxport[0].caps), mode->hdisplay,
				  mode->vdisplay, mode->htotal, mode->vtotal, mode->clock, 2);
}

bool dcp_t602x_mode_limited(struct apple_dcp *dcp, const struct drm_display_mode *mode)
{
	if (!dcp_t602x_needs_pair(dcp, mode))
		return false;
	if (!dcp_t602x_pair_fits(dcp, mode))
		return true;
	guard(mutex)(&dcp_peer_lock);
	return !dcp->peer_reserved && !dcp_t602x_idle_peer_locked(dcp, 0);
}

int dcp_t602x_check(struct apple_dcp *dcp, struct drm_crtc_state *state,
		   struct drm_atomic_commit *commit)
{
	struct apple_atomic_state *apple = to_apple_atomic_state(commit);
	struct drm_connector_state *connector_state;
	struct dcp_display_mode selected;
	struct drm_connector *connector;
	u64 generation;
	int i, idx;

	if (!state || !dcp_t602x_ext13_5(dcp))
		return 0;

	idx = drm_crtc_index(state->crtc);
	if (apple->t602x)
		apple->t602x[idx] = (struct dcp_t602x_assignment){};
	if (!state->active || !dcp_t602x_needs_pair(dcp, &state->mode))
		return 0;

	if (state->vrr_enabled)
		return -EINVAL;
	if (!drm_atomic_crtc_needs_modeset(state))
		return 0;

	generation = READ_ONCE(dcp->typec_generation);
	if (!dcp_t602x_pair_fits(dcp, &state->mode) ||
	    !lookup_mode_for_pair(dcp, &state->mode, &selected))
		return -EINVAL;

	/* This assignment is for an existing direct route. */
	for_each_new_connector_in_state(commit, connector, connector_state, i)
		if (connector_state->crtc == state->crtc &&
		    READ_ONCE(to_apple_connector(connector)->dcp) != to_platform_device(dcp->dev))
			return -EINVAL;

	if (generation != READ_ONCE(dcp->typec_generation))
		return -EAGAIN;

	if (!apple->t602x) {
		apple->t602x = kzalloc_objs(*apple->t602x, commit->dev->mode_config.num_crtc);
		if (!apple->t602x)
			return -ENOMEM;
	}
	apple->t602x[idx].generation = generation;
	apple->t602x[idx].checked = true;
	return 0;
}

static struct apple_dcp *dcp_t602x_crtc_pipe(struct drm_crtc *crtc)
{
	struct platform_device *pdev;

	if (t8132_ext_crtc(crtc))
		return NULL;
	pdev = to_apple_crtc(crtc)->dcp;
	return pdev ? platform_get_drvdata(pdev) : NULL;
}

bool dcp_t602x_has_pair_engine(struct drm_device *dev)
{
	struct drm_crtc *crtc;

	drm_for_each_crtc(crtc, dev) {
		struct apple_dcp *dcp = dcp_t602x_crtc_pipe(crtc);

		if (dcp && dcp_t602x_can_pair(dcp))
			return true;
	}
	return false;
}

bool dcp_t602x_topology_changed(struct drm_atomic_commit *commit)
{
	struct drm_crtc_state *state;
	struct drm_crtc *crtc;
	int i;

	for_each_new_crtc_in_state(commit, crtc, state, i) {
		struct apple_dcp *dcp = dcp_t602x_crtc_pipe(crtc);

		if (dcp && dcp_t602x_ext13_5(dcp) && drm_atomic_crtc_needs_modeset(state))
			return true;
	}
	return false;
}

int dcp_t602x_plan_commit(struct drm_atomic_commit *commit)
{
	struct dcp_t602x_assignment *assignments = to_apple_atomic_state(commit)->t602x;
	struct drm_crtc_state *state;
	struct drm_crtc *crtc;
	u32 options[32], choice[32] = {};
	u8 owners[32], indices[32] = {};
	unsigned int count = 0;
	u32 active = 0;
	int i, ret;

	guard(mutex)(&dcp_peer_lock);
	for_each_new_crtc_in_state(commit, crtc, state, i) {
		struct apple_dcp *dcp = dcp_t602x_crtc_pipe(crtc);

		if (!dcp || !dcp_t602x_ext13_5(dcp) || !state->active)
			continue;
		active |= drm_crtc_mask(crtc);
		if (dcp_t602x_port_reserved(dcp))
			return -ENOSPC;
	}

	for_each_new_crtc_in_state(commit, crtc, state, i) {
		struct apple_dcp *dcp = dcp_t602x_crtc_pipe(crtc);
		struct apple_dcp *running;

		if (!dcp || !dcp_t602x_ext13_5(dcp) || !state->active ||
		    !drm_atomic_crtc_needs_modeset(state))
			continue;
		if (!dcp_t602x_needs_pair(dcp, &state->mode))
			continue;
		if (!assignments || !assignments[i].checked ||
		    assignments[i].generation != READ_ONCE(dcp->typec_generation))
			return -EAGAIN;
		owners[count] = i;
		options[count] = 0;
		running = dcp->peer_reserved;
		for (u32 index = 1; index <= 4; index++) {
			struct apple_dcp *peer;
			unsigned int bit;

			if (running)
				peer = running->index == index ? running : NULL;
			else
				peer = dcp_t602x_idle_peer_locked(dcp, index);
			if (!peer || !peer->crtc)
				continue;
			bit = drm_crtc_index(&peer->crtc->base);
			indices[bit] = index;
			options[count] |= BIT(bit);
		}
		options[count++] &= ~active;
	}

	ret = dcp_t602x_match(options, choice, count);
	if (ret)
		return ret;
	for (unsigned int j = 0; j < count; j++)
		assignments[owners[j]].peer_index = indices[__ffs(choice[j])];
	return 0;
}

void dcp_t602x_display_hints(struct apple_dcp *dcp,
			       struct dcp_display_hints_v13_5 *reply)
{
	struct dcp_dptx_caps caps = dcp_t602x_limits(&dcp->dptxport[0].caps);
	const u32 width = get_unaligned_le32(reply->hints + 0x38);
	const u32 rate = get_unaligned_le32(reply->hints + 0x40);
	struct apple_dcp *peer;

	if (!dcp_t602x_can_pair(dcp) || !dcp->crtc ||
	    dcp->dptx_tunnel || !READ_ONCE(dcp->active_typec_route) ||
	    (reply->hints[0] & 3) != 1)
		return;
	if (!width || !rate || (width <= caps.width && rate <= caps.total_rate) ||
	    width > 2ULL * caps.width || (u64)rate > 2ULL * caps.total_rate)
		return;

	guard(mutex)(&dcp_peer_lock);
	peer = dcp->peer_reserved ?: dcp_t602x_idle_peer_locked(dcp, 0);
	if (!peer)
		return;
	reply->allocation.primary_id = cpu_to_le32(dcp->index - 1);
	reply->allocation.peer_id = cpu_to_le32(peer->index - 1);
	reply->allocation.extra_pipes = 1;
	reply->allocation.max_width = cpu_to_le32(caps.width);
	reply->allocation.max_rate = cpu_to_le32(caps.total_rate);
}

/* Firmware 13.5 A359 and A360 share the 124/116-byte insert layout. */
static int t602x_get_video(struct apple_dcp *pipe, u32 timing, u32 color,
			   struct dcp_peer_insert_video_resp *result)
{
	struct dcp_peer_insert_video_req request = {
		.timing_id = cpu_to_le32(timing),
		.color_id = cpu_to_le32(color),
	};
	int ret;

	ret = dcp_peer_call_oob(pipe, "A359", &request, sizeof(request), result, sizeof(*result));
	return ret ?: ((result->success & 1) ? 0 : -ENODATA);
}

static int t602x_read_counter(struct apple_dcp *pipe, u32 *value)
{
	u8 query[8] = { 1 };
	__le32 out;
	int ret;

	ret = dcp_peer_call_oob(pipe, "A356", query, sizeof(query), &out, sizeof(out));
	if (!ret)
		*value = le32_to_cpu(out);
	return ret;
}

static struct apple_epic_service *t602x_get_av_controller(struct apple_dcp_afkep *ep)
{
	u32 count = min_t(u32, READ_ONCE(ep->num_channels), AFK_MAX_CHANNEL);

	for (u32 i = 0; i < count; i++) {
		struct apple_epic_service *s = afk_service_get(&ep->services[i]);

		if (!s)
			continue;
		if ((unsigned long)s->cookie == BIT(0))
			return s;
		afk_service_put(s);
	}
	return NULL;
}

static struct apple_epic_service *t602x_peer_controller(struct apple_dcp *peer)
{
	struct apple_epic_service *service;
	struct apple_dcp_afkep *ep;
	int ret;

	if (!peer->dptx_controller_ep) {
		ret = dptx_controller_init(peer);
		if (ret)
			return ERR_PTR(ret);
	}
	ep = peer->dptx_controller_ep;
	if (IS_ERR_OR_NULL(ep) || ep->wire_v2)
		return ERR_PTR(-ENODEV);
	if (!wait_event_timeout(peer->t602x_pair_wait,
				(service = t602x_get_av_controller(ep)), msecs_to_jiffies(300)))
		return ERR_PTR(-ENODEV);
	return service;
}

struct t602x_ctrl_value {
	__le32 status;
	u8 reserved[12];
	__le64 value;
	u8 tail[8];
} __packed;

static int t602x_ctrl_value(struct apple_epic_service *ctrl, u32 command, u64 in, u64 *out)
{
	struct t602x_ctrl_value request = { .value = cpu_to_le64(in) }, reply = {};
	size_t actual = 0;
	int ret;

	ret = afk_service_call_timeout_len(ctrl, 0, command, &request, sizeof(request), 0,
					   &reply, sizeof(reply), 0, 1500, &actual);
	if (ret)
		return ret;
	if (actual != sizeof(reply) || reply.status)
		return -EIO;
	*out = le64_to_cpu(reply.value);
	return 0;
}

static int t602x_ctrl_power(struct apple_epic_service *ctrl, u64 value)
{
	u64 readback;
	int ret;

	ret = t602x_ctrl_value(ctrl, 6, value, &readback);
	if (!ret)
		ret = t602x_ctrl_value(ctrl, 7, 0, &readback);
	return ret ?: (readback == value ? 0 : -EIO);
}

static int t602x_end(struct dcp_peer_wait *w, size_t status_at, u32 timeout, void *reply)
{
	int ret = 0;

	if (!wait_for_completion_timeout(&w->done, msecs_to_jiffies(timeout)))
		ret = -ETIMEDOUT;
	else if (get_unaligned_le32(w->reply + status_at))
		ret = -EIO;
	else if (reply)
		memcpy(reply, w->reply, w->size);
	kref_put(&w->refs, dcp_peer_wait_free);
	return ret;
}

static int t602x_pair_power(struct apple_dcp *owner, struct apple_dcp *peer, bool on)
{
	static const struct dcp_method_entry power = {
		.name = "t602x_pair_power", .tag = { 'A', '4', '7', '2' }
	};
	u8 in[12] = {};
	struct dcp_peer_wait *w, *p;
	int r, s;

	put_unaligned_le64(on, in);
	w = dcp_peer_begin(owner, false, &power, in, sizeof(in), 8, 0xff);
	if (IS_ERR(w))
		return PTR_ERR(w);
	p = dcp_peer_begin(peer, false, &power, in, sizeof(in), 8, 0xff);
	r = t602x_end(w, 4, 2500, NULL);
	s = IS_ERR(p) ? PTR_ERR(p) : t602x_end(p, 4, 2500, NULL);
	dev_dbg(owner->dev, "13.5 pair power %u: primary %d peer %d\n", on, r, s);
	/*
	 * A472 acknowledges the request. Firmware 13.5 supplies no known
	 * power-ready event here. Keep the same settle time as the 26.6 pair
	 * before changing the link or selecting a mode.
	 */
	if (!r && !s)
		msleep(100);
	return r ?: s;
}

bool dcp_t602x_tiling(struct apple_dcp *pipe, bool write, u32 event, u32 param,
		      u32 *value)
{
	struct apple_dcp *other, *target;
	struct dcp_peer_tiling *t;

	if (!READ_ONCE(pipe->t602x_pair_clock) || (event != 1 && event != 2))
		return false;
	guard(mutex)(&dcp_peer_lock);
	other = pipe->peer_reserved ?: pipe->peer_owner;
	if (!other || pipe->peer_unbinding || other->peer_unbinding ||
	    !pipe->peer_tiling.vftg || !other->peer_tiling.vftg)
		return false;
	target = event == 1 ? pipe : other;
	t = &target->peer_tiling;
	if (write) {
		if (event != 1)
			return false;
		switch (param) {
		case 2: t->phase = *value; return true;
		case 5: t->ready = !!*value; return true;
		case 9:
			WRITE_ONCE(t->modeset_state, *value);
			if (*value >= 2)
				wake_up_all(&target->t602x_pair_wait);
			return true;
		default: return false;
		}
	}
	switch (param) {
	case 1: *value = t->member; return true;
	case 2: *value = t->phase; return true;
	case 3: *value = target->index - 1; return true;
	case 4: *value = !t->primary; return true;
	case 5: *value = t->ready; return true;
	case 7: *value = t->vftg; return true;
	case 8: *value = t->primary; return true;
	case 9: *value = t->modeset_state; return true;
	default: return false;
	}
}

static int t602x_sync_video_ids(struct apple_dcp *owner, struct apple_dcp *peer)
{
	u32 a, b, value;
	u8 set[8] = {};
	int ret;

	ret = t602x_read_counter(owner, &a);
	if (!ret)
		ret = t602x_read_counter(peer, &b);
	if (ret || a == b)
		return ret;
	/* Across wrap the newer counter is the one a small positive distance ahead. */
	value = (s32)(a - b) >= 0 ? a : b;
	put_unaligned_le32(value, set + 1);
	ret = dcp_peer_call(value == a ? peer : owner, "A356", set, sizeof(set), &a, 4);
	if (ret || a)
		return ret ?: -EIO;
	ret = t602x_read_counter(owner, &a);
	if (!ret)
		ret = t602x_read_counter(peer, &b);
	return ret ?: (a == value && b == value ? 0 : -EIO);
}

static int t602x_dsc(struct apple_dcp *owner, struct apple_dcp *peer)
{
	u8 caps[36] = {}, reply[32] = {}, bpp[8] = {}, hints[12] = {};
	u8 zero[12] = {};
	u32 required;
	u16 target;
	int ret;

	ret = dcp_peer_call_oob(owner, "A364", NULL, 0, &required, 4);
	if (ret)
		return ret;
	dev_dbg(owner->dev, "13.5 pair DSC required %u\n", required);
	if (!(required & 1))
		return 0;
	ret = dcp_peer_call_oob(owner, "A362", caps, 36, reply, 32);
	if (ret)
		return ret;
	memcpy(caps, reply, 32);
	ret = dcp_peer_call(peer, "A363", caps, 36, reply, 32);
	if (!ret)
		ret = dcp_peer_call_oob(owner, "A365", zero, 4, bpp, 8);
	if (ret || get_unaligned_le32(bpp + 2))
		return ret ?: -EIO;
	target = get_unaligned_le16(bpp);
	if (!target)
		return -ERANGE;
	put_unaligned_le16(target, caps);
	memset(caps + 2, 0, 2);
	ret = dcp_peer_call(peer, "A366", caps, 4, bpp, 8);
	if (!ret)
		ret = dcp_peer_call(peer, "A365", zero, 4, bpp, 8);
	/* The Merge interface leaves its 'this' in the nominal status. */
	if (ret || get_unaligned_le16(bpp) != target)
		return ret ?: -EIO;
	ret = dcp_peer_call_oob(owner, "A367", zero, 12, hints, 12);
	if (ret || get_unaligned_le32(hints + 8))
		return ret ?: -EIO;
	memcpy(caps, hints, 8);
	memset(caps + 8, 0, 4);
	ret = dcp_peer_call(peer, "A368", caps, 12, hints, 12);
	if (!ret && get_unaligned_le32(hints + 8))
		ret = -EIO;
	dev_dbg(owner->dev, "13.5 pair DSC verified %u/16 bpp, slice hint copy %d\n",
		 target, ret);
	return ret;
}

static void t602x_modeset_done(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_peer_wait_done(dcp, out, cookie);
	wake_up_all(&dcp->t602x_pair_wait);
}

static int t602x_set_mode(struct apple_dcp *owner, struct apple_dcp *peer,
				   struct dcp_peer_insert_video_req *video)
{
	static const struct dcp_method_entry select = {
		.name = "t602x_pair_select", .tag = { 'A', '4', '1', '2' }
	};
	__le32 mode[] = { video->color_id, video->timing_id };
	struct dcp_peer_wait *w, *p;
	u32 phase = 1;
	int ret, owner_ret;

	scoped_guard(mutex, &dcp_peer_lock) {
		owner->peer_tiling.member = peer->peer_tiling.member = true;
		owner->peer_tiling.modeset_state = peer->peer_tiling.modeset_state = 1;
	}
	ret = t602x_pair_power(owner, peer, true);
	if (!ret)
		ret = t602x_sync_video_ids(owner, peer);
	if (ret)
		return ret;
	w = dcp_peer_wait_alloc(4, 0xff);
	if (IS_ERR(w))
		return PTR_ERR(w);
	dcp_push(owner, false, &select, sizeof(mode), 4, mode, t602x_modeset_done, w);
	wait_event_timeout(owner->t602x_pair_wait,
			   READ_ONCE(owner->peer_tiling.modeset_state) >= 2 ||
			   READ_ONCE(owner->peer_unbinding) || READ_ONCE(peer->peer_unbinding) ||
			   completion_done(&w->done) || READ_ONCE(owner->crashed),
			   msecs_to_jiffies(7500));
	if (!dcp_t602x_tiling(owner, false, 1, 9, &phase) || phase < 2) {
		ret = -EPROTO;
	} else {
		ret = t602x_dsc(owner, peer);
		if (!ret) {
			p = dcp_peer_begin(peer, false, &select, mode, sizeof(mode), 4, 0xff);
			ret = IS_ERR(p) ? PTR_ERR(p) : t602x_end(p, 0, 8500, NULL);
		}
	}
	owner_ret = t602x_end(w, 0, 8500, NULL);
	dev_dbg(owner->dev, "13.5 pair native modeset: phase %u primary %d peer path %d\n",
		 phase, owner_ret, ret);
	return ret ?: owner_ret;
}

/* A360 may update opaque metadata. Both pipe insertions must preserve the raster. */
static bool t602x_insert_matches(const u8 *reply, const u8 *request)
{
	/* Each axis starts with total, active, sync width, back porch and front porch. */
	return !memcmp(reply + 4, request + 4, 5 * sizeof(__le32)) &&
	       !memcmp(reply + 36, request + 36, 5 * sizeof(__le32)) &&
	       get_unaligned_le32(reply + 68) == get_unaligned_le32(request + 68);
}

static int t602x_insert(struct apple_dcp *pipe, struct dcp_peer_insert_video_req *video)
{
	struct dcp_peer_insert_video_resp reply = {};
	int ret;

	ret = dcp_peer_call(pipe, "A360", video, sizeof(*video), &reply, sizeof(reply));
	if (!ret && !(reply.success & 1))
		ret = -ENODATA;
	if (!ret && !t602x_insert_matches(reply.video.timing, video->video.timing))
		ret = -ERANGE;
	dev_dbg(pipe->dev, "13.5 peer insertion: IDs %u/%u, %ux%u clock %u, result %d\n",
		 le32_to_cpu(video->timing_id), le32_to_cpu(video->color_id),
		 get_unaligned_le32(reply.video.timing + 8), get_unaligned_le32(reply.video.timing + 40),
		 get_unaligned_le32(reply.video.timing + 68), ret);
	if (!ret)
		video->video = reply.video;
	return ret;
}

/* An in-flight paired frame belongs to its owner CRTC. */
bool dcp_t602x_frame_complete(struct apple_dcp *pipe, u32 id, ktime_t at)
{
	if (!smp_load_acquire(&pipe->t602x_frame_waiting) ||
	    id != READ_ONCE(pipe->t602x_frame_id))
		return !!READ_ONCE(pipe->t602x_scanout);
	if (!atomic64_cmpxchg(&pipe->t602x_frame_ns, 0, ktime_to_ns(at)))
		complete(&pipe->t602x_frame_done);
	return true;
}

static int t602x_set_matrix(struct apple_dcp *pipe, const struct drm_color_ctm *ctm)
{
	struct iomfb_set_matrix_req matrix = { .location = 9 };
	struct iomfb_set_matrix_resp answer = {};
	__le32 identity = cpu_to_le32(0x10000);
	int ret;

	dcp_ctm_to_fw(matrix.matrix, ctm);
	ret = dcp_peer_call(pipe, "A422", &matrix, sizeof(matrix), &answer, sizeof(answer));
	if (ret || answer.ret)
		return ret ?: -EIO;
	/* 13.5 A427's return is a u32 output, with no status semantics. */
	return dcp_peer_call(pipe, "A427", &identity, sizeof(identity), &identity, sizeof(identity));
}

static int t602x_start_frame(struct t602x_scanout *s, u32 *id)
{
	static const struct dcp_method_entry begin = {
		.name = "t602x_start_pair", .tag = { 'A', '4', '0', '7' }
	};
	struct apple_dcp *pipes[] = { s->owner, s->peer };
	struct dcp_swap_start_req_v13_3 start = {};
	struct dcp_swap_start_resp_v13_3 ids[2] = {};
	struct dcp_peer_wait *waiting[2];
	int ret = 0;

	/* The endpoints have independent command channels. Start both, then
	 * wait for both allocated IDs before attaching any surfaces.
	 */
	for (int i = 0; i < 2; i++)
		waiting[i] = dcp_peer_begin(pipes[i], false, &begin, &start, sizeof(start),
					    sizeof(ids[i]), 0xff);
	for (int i = 0; i < 2; i++) {
		int ack = IS_ERR(waiting[i]) ? PTR_ERR(waiting[i]) :
			t602x_end(waiting[i], offsetof(struct dcp_swap_start_resp_v13_3, ret),
				  2000, &ids[i]);
		ret = ret ?: ack;
	}
	if (ret)
		return ret;
	if (ids[0].swap_id != ids[1].swap_id)
		return -EUCLEAN;
	*id = ids[0].swap_id;
	return 0;
}

static int t602x_submit_frame(struct t602x_scanout *s)
{
	static const struct dcp_method_entry submit = {
		.name = "t602x_present", .tag = { 'A', '4', '0', '8' }
	};
	struct apple_dcp *owner = s->owner;
	struct apple_dcp *pipes[] = { owner, s->peer };
	struct dcp_swap_submit_req_v13_3 *frame = s->frames;
	struct dcp_peer_wait *waiting[2];
	u64 now, stamp;
	bool prepare_next;
	u32 id;
	int ret = 0;

	if (s->spare_error)
		return s->spare_error;
	if (s->has_spare) {
		id = s->spare_id;
		s->has_spare = false;
	} else {
		ret = t602x_start_frame(s, &id);
		if (ret)
			return ret;
	}
	now = ktime_get_ns();
	prepare_next = s->last_submit_ns && now - s->last_submit_ns < 3 * s->period_ns;
	s->last_submit_ns = now;
	stamp = arch_timer_read_counter();
	WRITE_ONCE(owner->started_swap_id, id);
	dcp_arm_retained_framebuffers(owner, id);
	/* The event already carries the later of two real callback times. */
	owner->swap_start = KTIME_MIN;
	for (int i = 0; i < 2; i++) {
		frame[i].swap.swap_id = id;
		frame[i].swap.timestamp[0] = stamp;
		frame[i].swap.timestamp[1] = owner->swap_submit_timestamp ?
					     owner->swap_submit_timestamp : stamp;
		frame[i].swap.timestamp[6] = stamp;
		reinit_completion(&pipes[i]->t602x_frame_done);
		atomic64_set(&pipes[i]->t602x_frame_ns, 0);
		WRITE_ONCE(pipes[i]->t602x_frame_id, id);
		smp_store_release(&pipes[i]->t602x_frame_waiting, true);
		waiting[i] = dcp_peer_begin(pipes[i], false, &submit, &frame[i], sizeof(frame[i]),
					    sizeof(struct dcp_swap_submit_resp_v13_3), 0xff);
	}

	for (int i = 0; i < 2; i++) {
		int ack = IS_ERR(waiting[i]) ? PTR_ERR(waiting[i]) :
			t602x_end(waiting[i], offsetof(struct dcp_swap_submit_resp_v13_3, ret), 2000, NULL);

		ret = ret ?: ack;
	}
	if (ret)
		return ret;
	owner->swap_submit_timestamp = stamp;
	/*
	 * A407 allocates only an ID. Both engines accepted the current A408,
	 * so reserve a single empty slot while that frame waits for scanout.
	 * The next commit supplies all surfaces and waits its normal fences.
	 * A sparse producer leaves at most one empty slot until use or stop.
	 */
	if (prepare_next) {
		s->spare_error = t602x_start_frame(s, &s->spare_id);
		if (!s->spare_error && s->spare_id == id)
			s->spare_error = -EUCLEAN;
		s->has_spare = !s->spare_error;
	}

	for (int i = 0; i < 2; i++)
		if (!wait_for_completion_timeout(&pipes[i]->t602x_frame_done, msecs_to_jiffies(350)))
			return -ENOMSG;
	return 0;
}

/* A359 returns the physical timing associated with the selected mode ID. */
static bool t602x_axis_matches(const u8 *axis, u16 active, u16 start, u16 end, u16 total)
{
	return get_unaligned_le32(axis) == total &&
	       get_unaligned_le32(axis + 4) == active &&
	       get_unaligned_le32(axis + 8) == end - start &&
	       get_unaligned_le32(axis + 12) == total - end &&
	       get_unaligned_le32(axis + 16) == start - active;
}

static bool t602x_timing_matches(struct apple_dcp *dcp, const u8 *timing,
				const struct drm_display_mode *mode)
{
	return t602x_axis_matches(timing + 4, mode->hdisplay, mode->hsync_start,
				  mode->hsync_end, mode->htotal) &&
	       t602x_axis_matches(timing + 36, mode->vdisplay, mode->vsync_start,
				  mode->vsync_end, mode->vtotal) &&
	       dcp_t602x_rate_fits(dcp_t602x_limits(&dcp->dptxport[0].caps), mode->hdisplay,
				  mode->vdisplay, mode->htotal, mode->vtotal,
				  get_unaligned_le32(timing + T602X_CLOCK_OFFSET), 2);
}

static int t602x_start_scanout(struct t602x_scanout *s, struct drm_crtc_state *state,
			       const struct dcp_display_mode *selected)
{
	struct apple_dcp *owner = s->owner, *peer = s->peer;
	struct dcp_peer_insert_video_resp physical = {};
	struct dcp_peer_insert_video_req half = {};
	const struct drm_color_ctm *ctm = state->ctm ? state->ctm->data : NULL;
	__le32 off = 0, on = cpu_to_le32(1);
	u64 echoed, power;
	int ret;

	ret = t602x_get_video(owner, selected->timing_mode_id, selected->sdr_rgb.id, &physical);
	if (ret)
		return ret;
	if (dcp_t602x_wire_clock(physical.video.timing, sizeof(physical.video.timing)) !=
	    selected->t602x_clock_hz ||
	    !t602x_timing_matches(owner, physical.video.timing, &state->mode))
		return -ERANGE;
	half.video = physical.video;
	dcp_peer_split_timing(half.video.timing);
	half.timing_id = cpu_to_le32(selected->timing_mode_id + DCP_SPLIT_MODE_SUFFIX);
	half.color_id = cpu_to_le32(selected->sdr_rgb.id + DCP_SPLIT_MODE_SUFFIX);
	s->controller = t602x_peer_controller(peer);
	if (IS_ERR(s->controller))
		return PTR_ERR(s->controller);
	ret = t602x_ctrl_value(s->controller, 7, 0, &power);
	if (ret || power) {
		afk_service_put(s->controller);
		return ret ?: -EBUSY;
	}
	scoped_guard(mutex, &dcp_peer_lock) {
		if (owner->peer_reserved || owner->peer_owner || peer->peer_reserved ||
		    peer->peer_owner || owner->peer_unbinding || peer->peer_unbinding) {
			afk_service_put(s->controller);
			return -EBUSY;
		}
		owner->peer_reserved = peer;
		peer->peer_owner = owner;
		WRITE_ONCE(owner->t602x_scanout, s);
		WRITE_ONCE(peer->t602x_scanout, s);
	}

	/* SetVirtualDeviceMode, then confirm the spare's controller power. */
	ret = t602x_ctrl_value(s->controller, 11, 1, &echoed);
	if (!ret)
		ret = t602x_ctrl_power(s->controller, 1);
	if (ret)
		return ret;
	s->pair_touched = true;
	ret = t602x_pair_power(owner, peer, false);
	if (!ret)
		ret = dcp_peer_call(owner, "A357", &off, sizeof(off), NULL, 0);
	if (!ret)
		ret = dcp_peer_call(peer, "A357", &off, sizeof(off), NULL, 0);
	if (!ret)
		ret = dcp_peer_prepare_vftg(owner, peer, &half);
	if (ret)
		return ret;
	WRITE_ONCE(owner->t602x_pair_clock, get_unaligned_le32(half.video.timing + 68));
	WRITE_ONCE(peer->t602x_pair_clock, READ_ONCE(owner->t602x_pair_clock));
	s->merge_enabled = true;
	ret = dcp_peer_call(peer, "A361", &on, sizeof(on), NULL, 0);
	if (!ret)
		ret = t602x_insert(owner, &half);
	if (!ret)
		ret = t602x_insert(peer, &half);
	if (!ret)
		ret = t602x_set_mode(owner, peer, &half);
	if (!ret)
		ret = t602x_sync_video_ids(owner, peer);
	if (!ret)
		ret = t602x_set_matrix(owner, ctm);
	if (!ret)
		ret = t602x_set_matrix(peer, ctm);
	return ret;
}

static void t602x_put_pins(struct drm_framebuffer **fbs)
{
	for (unsigned int i = 0; i < SWAP_SURFACES; i++) {
		if (fbs[i])
			drm_framebuffer_put(fbs[i]);
		fbs[i] = NULL;
	}
}

static int t602x_prepare_frame(struct t602x_scanout *s, struct drm_atomic_commit *commit,
			       const struct dcp_swap_submit_req_v13_3 *request)
{
	struct drm_framebuffer *desired[SWAP_SURFACES];
	struct drm_crtc *crtc = &s->owner->crtc->base;
	struct drm_plane_state *old, *new;
	struct drm_plane *plane;
	int idx;

	memcpy(desired, s->scanned, sizeof(desired));
	for_each_oldnew_plane_in_state(commit, plane, old, new, idx) {
		unsigned int idx_surf = to_apple_plane(plane)->iomfb_surf;

		if (new->crtc != crtc && old->crtc != crtc)
			continue;
		if (idx_surf >= SWAP_SURFACES)
			return -EINVAL;
		desired[idx_surf] = new->crtc == crtc && new->visible ? new->fb : NULL;
	}
	for (unsigned int i = 0; i < SWAP_SURFACES; i++) {
		if (!(request->swap.swap_enabled & BIT(i)))
			continue;
		if (request->surf_null[i])
			desired[i] = NULL;
		else if (!desired[i] || !request->surf_iova[i])
			return -ENODATA;
	}
	for (unsigned int i = 0; i < SWAP_SURFACES; i++) {
		s->next[i] = desired[i];
		if (s->next[i])
			drm_framebuffer_get(s->next[i]);
	}
	for (unsigned int side = 0; side < 2; side++) {
		struct dcp_swap_submit_req_v13_3 *tile = &s->frames[side];

		*tile = *request;
		for (unsigned int i = 0; i < SWAP_SURFACES; i++) {
			if (request->surf_null[i] ||
			    dcp_peer_clip(&request->swap.src_rect[i], &request->swap.dst_rect[i],
					  true, side * s->width, s->width,
					  &tile->swap.src_rect[i], &tile->swap.dst_rect[i]))
				continue;
			tile->surf_null[i] = true;
			tile->surf_iova[i] = 0;
			tile->swap.surf_ids[i] = 0;
		}
	}
	return 0;
}

static void t602x_put_dma(struct t602x_scanout *s)
{
	while (s->dma_count) {
		struct dcp_session_ref *ref = &s->dma[--s->dma_count];

		iommu_group_put(ref->group);
		put_device(ref->dev);
	}
	module_put(THIS_MODULE);
}

/*
 * Each provider already holds the default domain for its binding. Pin
 * its device and group before asking firmware to touch either engine.
 * Unbind must be able to retain them even if allocation or module load fails.
 */
static int t602x_get_dma(struct t602x_scanout *s)
{
	struct apple_dcp *owner = s->owner, *peer = s->peer;
	struct device *devs[] = {
		owner->dev, peer->dev, owner->crtc->base.dev->dev,
		owner->piodma ? &owner->piodma->dev : NULL,
		peer->piodma ? &peer->piodma->dev : NULL,
	};

	static_assert(ARRAY_SIZE(devs) == ARRAY_SIZE(s->dma));
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	for (unsigned int i = 0; i < ARRAY_SIZE(devs); i++) {
		struct dcp_session_ref *ref = &s->dma[i];

		ref->group = devs[i] ? iommu_group_get(devs[i]) : NULL;
		if (!ref->group) {
			t602x_put_dma(s);
			return -ENODEV;
		}
		ref->dev = get_device(devs[i]);
		s->dma_count++;
	}
	return 0;
}

/* Keep the frame pins and peer claim until both engines confirm the stop. */
static int t602x_stop_locked(struct t602x_scanout *s)
{
	struct apple_dcp *owner = s->owner, *peer = s->peer;
	__le32 off = 0;
	u8 wake[12] = {}, ack[8] = {};
	u64 echoed;
	int ret;

	lockdep_assert_held(&t602x_scanout_lock);
	if (s->quarantined)
		return -ESHUTDOWN;
	kthread_flush_work(&s->work);
	if (READ_ONCE(owner->crashed) || READ_ONCE(peer->crashed) ||
	    READ_ONCE(owner->ch_cmd.depth) || READ_ONCE(peer->ch_cmd.depth)) {
		ret = -EBUSY;
		goto failed;
	}
	ret = s->pair_touched ? t602x_pair_power(owner, peer, false) : 0;
	if (!ret && s->pair_touched)
		ret = dcp_peer_call(owner, "A357", &off, sizeof(off), NULL, 0);
	if (!ret && s->merge_enabled)
		ret = dcp_peer_call(peer, "A357", &off, sizeof(off), NULL, 0);
	if (!ret && s->merge_enabled)
		ret = dcp_peer_call(peer, "A361", &off, sizeof(off), NULL, 0);
	if (ret)
		goto failed;
	put_unaligned_le64(1, wake);
	if (s->pair_touched)
		ret = dcp_peer_call(owner, "A472", wake, sizeof(wake), ack, sizeof(ack));
	if (!ret && get_unaligned_le32(ack + 4))
		ret = -EIO;
	if (!ret)
		ret = t602x_ctrl_power(s->controller, 0);
	if (!ret)
		ret = t602x_ctrl_value(s->controller, 11, 0, &echoed);
	if (ret)
		goto failed;

	scoped_guard(mutex, &dcp_peer_lock) {
		memset(&owner->peer_tiling, 0, sizeof(owner->peer_tiling));
		memset(&peer->peer_tiling, 0, sizeof(peer->peer_tiling));
		smp_store_release(&owner->t602x_frame_waiting, false);
		smp_store_release(&peer->t602x_frame_waiting, false);
		WRITE_ONCE(owner->t602x_scanout, NULL);
		WRITE_ONCE(peer->t602x_scanout, NULL);
		WRITE_ONCE(owner->t602x_pair_clock, 0);
		WRITE_ONCE(peer->t602x_pair_clock, 0);
		owner->peer_reserved = NULL;
		peer->peer_owner = NULL;
	}
	afk_service_put(s->controller);
	t602x_put_pins(s->scanned);
	t602x_put_pins(s->next);
	/* No half can read the displaced buffers after a confirmed stop. */
	dcp_release_all_retained_framebuffers(owner);
	owner->surfaces_cleared = false;
	kthread_destroy_worker(s->worker);
	dcp_t602x_release_port(peer);
	t602x_put_dma(s);
	kvfree(s);
	return 0;
failed:
	WRITE_ONCE(s->failed, true);
	dev_err(owner->dev, "13.5 desktop stop failed (%d), retaining scanout\n", ret);
	return ret;
}

int dcp_t602x_stop(struct apple_dcp *pipe)
{
	struct t602x_scanout *s;

	if (!READ_ONCE(pipe->t602x_scanout))
		return 0;
	guard(mutex)(&t602x_scanout_lock);
	s = pipe->t602x_scanout;
	return s ? t602x_stop_locked(s) : 0;
}

/*
 * A component unbind cannot return an error. After an uncertain stop,
 * retire both host endpoints and retain exactly the objects firmware can
 * still use. These on-chip devices stay registered across driver unbind.
 */
static int t602x_unbind_pair(struct apple_dcp *pipe)
{
	struct t602x_scanout *s;
	int ret;

	if (!READ_ONCE(pipe->t602x_scanout))
		return 0;
	guard(mutex)(&t602x_scanout_lock);
	s = pipe->t602x_scanout;
	if (!s)
		return 0;
	if (s->quarantined)
		return -ESHUTDOWN;
	ret = t602x_stop_locked(s);
	if (!ret)
		return 0;

	s->quarantined = true;
	WRITE_ONCE(s->owner->quiescing, true);
	WRITE_ONCE(s->peer->quiescing, true);
	scoped_guard(mutex, &dcp_retained_sessions_lock) {
		WRITE_ONCE(s->owner->retain_dma, true);
		WRITE_ONCE(s->peer->retain_dma, true);
		for (unsigned int i = 0; i < s->dma_count; i++)
			list_add_tail(&s->dma[i].link, &dcp_retained_sessions);
	}
	/* Fence either pipe against a new Type-C route until reboot. */
	smp_store_release(&s->owner->t602x_peer_reserved, true);
	afk_service_put(s->controller);
	kthread_destroy_worker(s->worker);
	return ret;
}

/* The aggregate serializes component unbind. Both contexts stay pinned. */
static void t602x_drain_callbacks(struct apple_dcp *pipe)
{
	struct t602x_scanout *s = READ_ONCE(pipe->t602x_scanout);
	struct apple_dcp *pipes[] = { s->owner, s->peer };

	for (unsigned int i = 0; i < ARRAY_SIZE(pipes); i++)
		if (pipes[i]->rtk)
			apple_rtkit_flush_rx(pipes[i]->rtk);
}

static void t602x_present_work(struct kthread_work *work)
{
	struct t602x_scanout *s = container_of(work, struct t602x_scanout, work);
	struct apple_dcp *owner = s->owner;
	struct drm_atomic_commit *commit = s->commit;
	struct drm_crtc_state *state;
	ktime_t displayed;
	u32 id;
	int ret = -EIO;

	state = drm_atomic_get_new_crtc_state(commit, &owner->crtc->base);
	if (!state || s->failed || READ_ONCE(owner->crashed) || READ_ONCE(s->peer->crashed))
		goto failed;
	if (state->color_mgmt_changed) {
		const struct drm_color_ctm *ctm = state->ctm ? state->ctm->data : NULL;

		ret = t602x_set_matrix(owner, ctm);
		if (!ret)
			ret = t602x_set_matrix(s->peer, ctm);
		if (ret)
			goto failed;
	}
	ret = t602x_prepare_frame(s, commit, &s->request);
	if (!ret)
		ret = t602x_submit_frame(s);
	if (ret)
		goto failed;

	id = READ_ONCE(owner->t602x_frame_id);
	displayed = ns_to_ktime(max(atomic64_read(&owner->t602x_frame_ns),
				    atomic64_read(&s->peer->t602x_frame_ns)));
	t602x_put_pins(s->scanned);
	memcpy(s->scanned, s->next, sizeof(s->scanned));
	memset(s->next, 0, sizeof(s->next));
	iomfb_complete_pair_v13_3(owner, id, displayed);
	goto done;
failed:
	if (!s->failed)
		dev_err(owner->dev, "paired scanout failed: %d\n", ret);
	WRITE_ONCE(s->failed, true);
	dcp_drm_crtc_error(owner->crtc, state, ret);
done:
	drm_atomic_commit_put(commit);
}

/*
 * The atomic commit retains its plane states. Power-off drains the worker
 * before dismissing either engine or releasing references to scanout buffers.
 */
bool dcp_t602x_present(struct apple_dcp *owner, struct drm_atomic_commit *commit,
		       const struct dcp_swap_submit_req_v13_3 *request)
{
	struct t602x_scanout *s;

	if (!READ_ONCE(owner->t602x_scanout))
		return false;
	guard(mutex)(&t602x_scanout_lock);
	s = owner->t602x_scanout;
	if (!s)
		return false;
	if (s->owner != owner)
		return true;
	if (s->quarantined) {
		dcp_drm_crtc_error(owner->crtc,
				   drm_atomic_get_new_crtc_state(commit, &owner->crtc->base), -ESHUTDOWN);
		return true;
	}
	kthread_flush_work(&s->work);
	if (s->failed || s->generation != READ_ONCE(owner->typec_generation)) {
		dcp_drm_crtc_error(owner->crtc,
				   drm_atomic_get_new_crtc_state(commit, &owner->crtc->base), -EIO);
		return true;
	}
	s->request = *request;
	s->commit = drm_atomic_commit_get(commit);
	kthread_queue_work(s->worker, &s->work);
	return true;
}

int dcp_t602x_modeset(struct apple_dcp *owner, struct drm_crtc_state *state,
		      const struct dcp_t602x_assignment *assignments, bool fabric_held)
{
	const struct dcp_t602x_assignment *assignment;
	struct dcp_display_mode selected;
	struct t602x_scanout *s;
	struct apple_dcp *peer;
	int ret;

	if (!dcp_t602x_ext13_5(owner))
		return -EOPNOTSUPP;
	guard(mutex)(&t602x_scanout_lock);
	s = owner->t602x_scanout;
	if (s && s->quarantined)
		return -ESHUTDOWN;
	if (s && s->owner != owner)
		return -EBUSY;
	if (!dcp_t602x_needs_pair(owner, &state->mode)) {
		if (s) {
			ret = t602x_stop_locked(s);
			if (ret)
				return ret;
		}
		return -EOPNOTSUPP;
	}
	if (state->vrr_enabled || owner->pq_requested ||
	    !dcp_t602x_pair_fits(owner, &state->mode) ||
	    !lookup_mode_for_pair(owner, &state->mode, &selected))
		return -EINVAL;
	if (!assignments)
		return -EAGAIN;
	assignment = &assignments[drm_crtc_index(state->crtc)];
	if (!assignment->peer_index || assignment->generation != READ_ONCE(owner->typec_generation))
		return -EAGAIN;
	if (s) {
		if (!dcp_t602x_peer_matches(assignment, s->peer->index, s->generation))
			return -EBUSY;
		kthread_flush_work(&s->work);
		if (!s->failed && !s->spare_error &&
		    s->timing == selected.timing_mode_id && s->color == selected.sdr_rgb.id)
			return 0;
		ret = t602x_stop_locked(s);
		if (ret)
			return ret;
	}
	scoped_guard(mutex, &dcp_peer_lock) {
		peer = dcp_t602x_idle_peer_locked(owner, assignment->peer_index);
		if (!peer || !device_trylock(peer->dev))
			return -EBUSY;
	}
	s = kvzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		ret = -ENOMEM;
		goto unlock;
	}
	s->worker = kthread_run_worker(0, "dcp-present/%u", owner->index);
	if (IS_ERR(s->worker)) {
		ret = PTR_ERR(s->worker);
		kvfree(s);
		goto unlock;
	}
	if (state->adjusted_mode.clock > 0) {
		const struct drm_display_mode *m = &state->adjusted_mode;
		/* A late wake can miss a whole refresh. Give this one worker
		 * a short fair-class request proportional to its frame period.
		 */
		struct sched_attr attr = {
			.size = sizeof(attr),
			.sched_policy = SCHED_NORMAL,
			.sched_nice = MIN_NICE,
			.sched_runtime = clamp_t(u64, div_u64((u64)m->htotal *
				m->vtotal * NSEC_PER_MSEC, m->clock) / 16,
				100 * NSEC_PER_USEC, 100 * NSEC_PER_MSEC),
		};

		sched_setattr_nocheck(s->worker->task, &attr);
	}
	kthread_init_work(&s->work, t602x_present_work);
	s->owner = owner;
	s->peer = peer;
	s->generation = assignment->generation;
	s->width = state->mode.hdisplay / 2;
	s->period_ns = div_u64((u64)state->mode.htotal * state->mode.vtotal *
			       NSEC_PER_MSEC, state->mode.clock);
	s->timing = selected.timing_mode_id;
	s->color = selected.sdr_rgb.id;
	ret = t602x_get_dma(s);
	if (ret)
		goto free;
	ret = dcp_t602x_claim_port(peer, fabric_held);
	if (ret)
		goto put_dma;
	if (mutex_trylock(&peer->external_lock)) {
		if (!dcp_t602x_peer_matches(assignment, peer->index,
					    READ_ONCE(owner->typec_generation)))
			ret = -EAGAIN;
		else if (READ_ONCE(owner->quiescing) || READ_ONCE(peer->quiescing) ||
			 READ_ONCE(owner->ch_cmd.depth) || READ_ONCE(peer->ch_cmd.depth))
			ret = -EBUSY;
		else
			ret = t602x_start_scanout(s, state, &selected);
		mutex_unlock(&peer->external_lock);
	} else {
		ret = -EBUSY;
	}
	if (ret) {
		if (READ_ONCE(owner->t602x_scanout) == s) {
			WRITE_ONCE(s->failed, true);
			if (ret != -ETIMEDOUT)
				t602x_stop_locked(s);
			goto unlock;
		}
		dcp_t602x_release_port(peer);
		goto put_dma;
	}
	owner->mode.color_mode_id = s->color;
	owner->mode.timing_mode_id = s->timing;
	owner->output_pq = false;
	owner->use_timestamps = true;
	owner->vrr_enabled = false;
	owner->surfaces_cleared = false;
	dev_dbg(owner->dev, "paired scanout: %ux%u clock %d on physical %u+%u\n",
		 state->mode.hdisplay, state->mode.vdisplay, state->mode.clock,
		 owner->index - 1, peer->index - 1);
	goto unlock;
put_dma:
	t602x_put_dma(s);
free:
	kthread_destroy_worker(s->worker);
	kvfree(s);
unlock:
	device_unlock(peer->dev);
	return ret;
}
