// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * T8132 (iomfb_m4) external dual-pipe peer bonding: peer reservation,
 * D575/D115/D116 tiling state, A364/A365 split modeset, phase-2 OOB DSC
 * copy, peer SID0 scanout sharing, tile-split paired swaps, and teardown.
 *
 * Included by iomfb_m4.c.
 */

/* ---- T8132 external dual-pipe peer bonding ---- */

static bool dcp_m4_peer_is_paired(struct iomfb_m4 *dcp)
{
	bool paired;

	mutex_lock(&dcp_m4_peer_lock);
	paired = !dcp->peer_unbinding && (dcp->peer_reserved || dcp->peer_owner);
	mutex_unlock(&dcp_m4_peer_lock);
	return paired;
}

static bool dcp_m4_peer_tiling_state(struct iomfb_m4 *dcp, bool write,
				     u32 event, u32 param, u32 *value)
{
	struct iomfb_m4 *peer;
	bool ok = false;

	mutex_lock(&dcp_m4_peer_lock);
	if (dcp->peer_unbinding || READ_ONCE(dcp->error))
		goto out;
	peer = dcp->peer_reserved ?: dcp->peer_owner;
	if (!peer || peer->peer_unbinding || READ_ONCE(peer->error))
		goto out;
	if (!dcp->peer_tiling.vftg || !peer->peer_tiling.vftg)
		goto out;
	if (write)
		ok = dcp_peer_tiling_set(&dcp->peer_tiling, &peer->peer_tiling,
					 event, param, *value);
	else
		ok = dcp_peer_tiling_get(&dcp->peer_tiling, &peer->peer_tiling,
					 event, param, value);
out:
	mutex_unlock(&dcp_m4_peer_lock);
	return ok;
}

static void dcp_m4_peer_display_ids(struct iomfb_m4 *dcp, u32 own, u32 related)
{
	struct iomfb_m4 *peer;

	mutex_lock(&dcp_m4_peer_lock);
	peer = dcp->peer_reserved ?: dcp->peer_owner;
	if (!peer || own == related || dcp->peer_unbinding || peer->peer_unbinding) {
		mutex_unlock(&dcp_m4_peer_lock);
		return;
	}
	dcp->peer_tiling.display_id = own;
	dcp->peer_tiling.display_id_valid = true;
	peer->peer_tiling.display_id = related;
	peer->peer_tiling.display_id_valid = true;
	mutex_unlock(&dcp_m4_peer_lock);
	dev_dbg(dcp->dev, "peer D575 display IDs: own=%u related=%u\n", own, related);
}

static void dcp_m4_peer_drop_locked(struct iomfb_m4 *owner, struct iomfb_m4 *peer)
{
	lockdep_assert_held(&dcp_m4_peer_lock);
	if (owner->peer_reserved != peer || owner->peer_tiling.vftg ||
	    owner->peer_tiling.member || peer->peer_tiling.vftg ||
	    peer->peer_tiling.member)
		return;
	memset(&owner->peer_tiling, 0, sizeof(owner->peer_tiling));
	memset(&peer->peer_tiling, 0, sizeof(peer->peer_tiling));
	peer->peer_owner = NULL;
	owner->peer_reserved = NULL;
	owner->peer_active = false;
}

static void dcp_m4_peer_release(struct iomfb_m4 *dcp)
{
	mutex_lock(&dcp_m4_peer_lock);
	if (dcp->peer_reserved)
		dcp_m4_peer_drop_locked(dcp, dcp->peer_reserved);
	mutex_unlock(&dcp_m4_peer_lock);
}

static void dcp_m4_peer_schedule_stop(struct iomfb_m4 *dcp)
{
	bool need_quiesce = false;

	mutex_lock(&dcp_m4_peer_lock);
	if (dcp->peer_reserved) {
		struct iomfb_m4 *peer = dcp->peer_reserved;

		if (dcp->peer_tiling.vftg || dcp->peer_tiling.member || dcp->peer_active ||
		    peer->peer_tiling.vftg || peer->peer_tiling.member) {
			dcp->peer_active = false;
			need_quiesce = true;
		} else {
			dcp_m4_peer_drop_locked(dcp, peer);
		}
	}
	mutex_unlock(&dcp_m4_peer_lock);
	if (need_quiesce)
		schedule_work(&dcp->peer_stop_work);
}

static void dcp_m4_peer_forget(struct iomfb_m4 *dcp)
{
	mutex_lock(&dcp_m4_peer_lock);
	dcp->peer_unbinding = true;
	if (!list_empty(&dcp->ext_peer_node))
		list_del_init(&dcp->ext_peer_node);
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
	mutex_unlock(&dcp_m4_peer_lock);
}

static u32 dcp_m4_peer_reject(struct iomfb_m4 *dcp, struct iomfb_m4 *p)
{
	struct dcp_m4_runtime *pr = smp_load_acquire(&p->runtime);
	u32 why = 0;

	if (!p->external || !pr || !smp_load_acquire(&pr->done) ||
	    READ_ONCE(p->error) || p->peer_unbinding)
		why |= BIT(0);
	if (p->peer_owner || p->peer_reserved)
		why |= BIT(1);
	if (READ_ONCE(p->owner->active_typec_route))
		why |= BIT(2);
	if (READ_ONCE(p->ext_sink) || (pr && pr->ext_connected))
		why |= BIT(3);
	if (!dcp_peer_index_allowed_26_6(dcp->owner->index, p->owner->index))
		why |= BIT(4);
	return why;
}

int iomfb_m4_peer_hints(struct apple_dcp *owner, unsigned int port_unit,
			const void *data, size_t size, void *reply, size_t reply_size)
{
	struct iomfb_m4 *dcp = owner->v26_6;
	struct dcp_display_hints *r = reply;
	struct iomfb_m4 *peer = NULL, *p;
	u32 width, height, total, active;

	if (!dcp || !dcp->external)
		return -ENODEV;
	if (size != sizeof(*r) || reply_size < sizeof(*r)) {
		dev_warn(dcp->dev, "SetHints: unexpected size=%zu reply_size=%zu (expected %zu)\n",
			 size, reply_size, sizeof(*r));
		return -ENODEV;
	}
	memcpy(reply, data, sizeof(*r));
	r->status = 0;
	memset(&r->allocation, 0, sizeof(r->allocation));

	width = get_unaligned_le32(r->hints + 0x38);
	height = get_unaligned_le32(r->hints + 0x3c);
	total = get_unaligned_le32(r->hints + 0x40);
	active = get_unaligned_le32(r->hints + 0x44);
	dev_dbg(dcp->dev, "SetHints: port=%u valid=%u tiled=%u %ux%u total=%u active=%u\n",
		port_unit, r->hints[0] & 1, !!(r->hints[0] & 2),
		width, height, total, active);

	if (port_unit || !(r->hints[0] & 1)) {
		dcp_m4_peer_release(dcp);
		return 0;
	}
	if (!dcp_m4_peer_needs_pair(r))
		return 0;

	mutex_lock(&dcp_m4_peer_lock);
	if (dcp->peer_unbinding || READ_ONCE(dcp->error)) {
		mutex_unlock(&dcp_m4_peer_lock);
		return -ENODEV;
	}
	if (dcp->peer_owner && READ_ONCE(dcp->owner->active_typec_route))
		dcp_m4_peer_drop_locked(dcp->peer_owner, dcp);
	if (dcp->peer_reserved &&
	    (dcp_m4_peer_reject(dcp, dcp->peer_reserved) & ~BIT(1)))
		dcp_m4_peer_drop_locked(dcp, dcp->peer_reserved);
	if (dcp->peer_owner) {
		mutex_unlock(&dcp_m4_peer_lock);
		return 0;
	}
	peer = dcp->peer_reserved;
	if (!peer) {
		list_for_each_entry(p, &dcp_m4_ext_list, ext_peer_node) {
			u32 why;

			if (p == dcp)
				continue;
			why = dcp_m4_peer_reject(dcp, p);
			dev_dbg(dcp->dev, "peer candidate %s (index %u): reject %#x\n",
				dev_name(p->dev), p->owner->index, why);
			if (why)
				continue;
			if (!peer || p->owner->index < peer->owner->index)
				peer = p;
		}
		if (!peer) {
			mutex_unlock(&dcp_m4_peer_lock);
			dev_info(dcp->dev, "no idle peer pipe for %ux%u active=%u\n",
				 width, height, active);
			return 0;
		}
		dcp->peer_reserved = peer;
		peer->peer_owner = dcp;
		dev_info(dcp->dev, "reserved peer %s (index %u) for %ux%u active=%u\n",
			 dev_name(peer->dev), peer->owner->index, width, height, active);
	}
	r->primary_id = cpu_to_le32(owner->index);
	r->peer_id = cpu_to_le32(peer->owner->index);
	r->extra_pipes = 1;
	r->flags &= ~3;
	mutex_unlock(&dcp_m4_peer_lock);
	return 0;
}

/* ---- Paired modeset, OOB DSC copy, peer DART sharing, and swaps ---- */

/*
 * Lock ordering invariant: primary->lock (lower DCP index) is ALWAYS acquired
 * before peer->lock (higher DCP index), and dcp_m4_peer_lock is ALWAYS a leaf
 * lock (never held across RPCs or while acquiring dcp->lock).
 */
static struct iomfb_m4 *dcp_m4_peer_get_locked(struct iomfb_m4 *dcp,
					       struct dcp_m4_runtime **peer_r_out,
					       bool active_only, u32 *tile_w_out)
{
	struct iomfb_m4 *peer;
	struct dcp_m4_runtime *peer_r;
	u32 tile_w = 0;

	lockdep_assert_held(&dcp->lock);
	mutex_lock(&dcp_m4_peer_lock);
	peer = dcp->peer_reserved;
	if (!peer || peer->peer_owner != dcp || dcp->peer_unbinding || peer->peer_unbinding ||
	    READ_ONCE(peer->error) ||
	    (active_only && (!dcp->peer_active || dcp->peer_failed ||
			     !dcp->peer_tiling.modeset_complete ||
			     !peer->peer_tiling.modeset_complete || !dcp->peer_tiling.width)))
		peer = NULL;
	else
		tile_w = dcp->peer_tiling.width;
	mutex_unlock(&dcp_m4_peer_lock);
	if (!peer)
		return NULL;

	mutex_lock_nested(&peer->lock, SINGLE_DEPTH_NESTING);
	peer_r = peer->runtime;
	mutex_lock(&dcp_m4_peer_lock);
	if (dcp->peer_reserved != peer || peer->peer_owner != dcp ||
	    dcp->peer_unbinding || peer->peer_unbinding || READ_ONCE(peer->error) ||
	    !peer_r || !smp_load_acquire(&peer_r->done) ||
	    (active_only && (!dcp->peer_active || dcp->peer_failed ||
			     !dcp->peer_tiling.modeset_complete ||
			     !peer->peer_tiling.modeset_complete || !dcp->peer_tiling.width))) {
		mutex_unlock(&dcp_m4_peer_lock);
		mutex_unlock(&peer->lock);
		return NULL;
	}
	tile_w = dcp->peer_tiling.width;
	mutex_unlock(&dcp_m4_peer_lock);
	if (peer_r_out)
		*peer_r_out = peer_r;
	if (tile_w_out)
		*tile_w_out = tile_w;
	return peer;
}

/* Synchronous RPC on a primary or peer runtime (does not require ext_connected). */
static int dcp_m4_peer_call(struct dcp_m4_runtime *r, unsigned int method, u8 stream,
			    const void *input, u32 input_size, void *output, u32 output_size,
			    unsigned int timeout_ms)
{
	struct iomfb_m4 *dcp = r->dcp;
	struct dcp_m4_transaction *t = &dcp->transaction;
	int ret;

	if (!r || !smp_load_acquire(&r->done) || READ_ONCE(dcp->error))
		return -ENODEV;
	if (method > 999 || stream > 1 || t->calls || t->callbacks || t->error) {
		dev_warn(dcp->dev, "peer A%03u on stream %u not sent (calls %u, callbacks %u, error %d)\n",
			 method, stream, t->calls, t->callbacks, t->error);
		return -EINVAL;
	}
	r->deadline = jiffies + msecs_to_jiffies(timeout_ms);
	ret = dcp_m4_runtime_call_stream(r, method, stream, false, input, input_size,
					 output, output_size);
	r->deadline = 0;
	return ret;
}

struct dcp_m4_peer_rpc_work {
	struct work_struct work;
	struct completion done;
	struct dcp_m4_runtime *r;
	unsigned int method;
	u8 stream;
	const void *input;
	u32 input_size;
	void *output;
	u32 output_size;
	unsigned int timeout_ms;
	int ret;
};

static void dcp_m4_peer_rpc_fn(struct work_struct *work)
{
	struct dcp_m4_peer_rpc_work *w = container_of(work, struct dcp_m4_peer_rpc_work, work);

	w->ret = dcp_m4_peer_call(w->r, w->method, w->stream, w->input, w->input_size,
				  w->output, w->output_size, w->timeout_ms);
	complete(&w->done);
}

/* Paired A473(on) sent to primary and peer in parallel (both locks held by caller). */
static int dcp_m4_peer_power(struct dcp_m4_runtime *r, struct dcp_m4_runtime *peer_r, bool on)
{
	struct dcp_m4_power_state_req req = { .state = cpu_to_le64(on) };
	struct dcp_m4_power_state_reply p_reply = {}, s_reply = {};
	struct dcp_m4_peer_rpc_work pw = {
		.r = peer_r, .method = 473, .stream = 1,
		.input = &req, .input_size = sizeof(req),
		.output = &s_reply, .output_size = sizeof(s_reply),
		.timeout_ms = 5000,
	};
	int ret;

	INIT_WORK_ONSTACK(&pw.work, dcp_m4_peer_rpc_fn);
	init_completion(&pw.done);
	queue_work(system_dfl_wq, &pw.work);

	ret = dcp_m4_peer_call(r, 473, 1, &req, sizeof(req), &p_reply, sizeof(p_reply), 5000);
	flush_work(&pw.work);
	destroy_work_on_stack(&pw.work);

	if (!ret && p_reply.status)
		ret = -EIO;
	if (!ret && pw.ret)
		ret = pw.ret;
	if (!ret && s_reply.status)
		ret = -EIO;
	dev_dbg(r->dcp->dev, "peer power %u: ret=%d (primary status=%#x, peer ret=%d status=%#x)\n",
		on, ret, le32_to_cpu(p_reply.status), pw.ret, le32_to_cpu(s_reply.status));
	return ret;
}

static int dcp_m4_peer_quiesce(struct dcp_m4_runtime *r, struct dcp_m4_runtime *peer_r)
{
	struct iomfb_m4 *dcp = r->dcp, *peer = peer_r->dcp;
	__le32 manage = 0;
	int ret;

	mutex_lock(&dcp_m4_peer_lock);
	dcp->peer_active = false;
	dcp->peer_tiling.modeset_complete = false;
	peer->peer_tiling.modeset_complete = false;
	mutex_unlock(&dcp_m4_peer_lock);

	ret = dcp_m4_peer_power(r, peer_r, false);
	if (ret)
		return ret;
	msleep(100);
	ret = dcp_m4_peer_call(r, 362, 0, &manage, sizeof(manage), NULL, 0, 5000);
	if (ret)
		return ret;
	ret = dcp_m4_peer_call(peer_r, 362, 0, &manage, sizeof(manage), NULL, 0, 5000);
	if (ret)
		return ret;

	mutex_lock(&dcp_m4_peer_lock);
	dcp->peer_tiling.member = peer->peer_tiling.member = false;
	dcp->peer_tiling.vftg = peer->peer_tiling.vftg = false;
	dcp->peer_tiling.modeset_complete = peer->peer_tiling.modeset_complete = false;
	dcp->peer_tiling.phase = peer->peer_tiling.phase = 0;
	dcp->peer_tiling.modeset_state = peer->peer_tiling.modeset_state = 0;
	dcp->peer_tiling.ready = peer->peer_tiling.ready = false;
	dcp->peer_active = false;
	mutex_unlock(&dcp_m4_peer_lock);
	return 0;
}

static int dcp_m4_peer_unpair(struct dcp_m4_runtime *r, struct dcp_m4_runtime *peer_r)
{
	struct dcp_m4_power_state_req req = { .state = cpu_to_le64(1) };
	struct dcp_m4_power_state_reply reply = {};
	int ret;

	ret = dcp_m4_peer_quiesce(r, peer_r);
	if (!ret && r->ext_connected) {
		ret = dcp_m4_peer_call(r, 473, 1, &req, sizeof(req), &reply, sizeof(reply), 5000);
		if (!ret && reply.status)
			ret = -EIO;
	}
	dev_info(r->dcp->dev, "peer unpair result=%d\n", ret);
	return ret;
}

struct dcp_m4_peer_dsc {
	struct completion ready;
	bool required;
	u8 caps[32];
	u8 bpp[2];
	u8 hints[8];
	int ret;
};

/* Stream-1 OOB query on primary (top-level or while stream-0 A413 is paused). */
static int dcp_m4_peer_call_oob(struct dcp_m4_runtime *r, unsigned int method,
				const void *input, u32 input_size,
				void *output, u32 output_size, unsigned int timeout_ms)
{
	struct iomfb_m4 *dcp = r->dcp;
	struct dcp_m4_transaction *t = &dcp->transaction;
	unsigned long saved_deadline;
	int ret;

	if (!r || !smp_load_acquire(&r->done) || READ_ONCE(dcp->error))
		return -ENODEV;
	if (method > 999 || t->callbacks || t->error || t->oob ||
	    (t->calls && !(t->calls == 1 && !t->call[0].packet.stream))) {
		dev_warn(dcp->dev, "peer OOB A%03u not sent (calls %u, callbacks %u, oob %u, error %d)\n",
			 method, t->calls, t->callbacks, t->oob, t->error);
		return -EINVAL;
	}
	saved_deadline = r->deadline;
	r->deadline = jiffies + msecs_to_jiffies(timeout_ms);
	ret = dcp_m4_runtime_call_stream(r, method, 1, false, input, input_size,
					 output, output_size);
	r->deadline = saved_deadline;
	return ret;
}

static void dcp_m4_peer_read_dsc_oob(struct dcp_m4_runtime *r)
{
	struct dcp_m4_peer_dsc *dsc = r->peer_dsc;
	u8 caps_req[36] = {}, bpp_req[4] = {}, bpp[8] = {};
	u8 hints_req[12] = {}, hints[12] = {};
	__le32 required = 0;
	int ret;

	if (!dsc || completion_done(&dsc->ready))
		return;
	ret = dcp_m4_peer_call_oob(r, 369, NULL, 0, &required, sizeof(required), 5000);
	dev_dbg(r->dcp->dev, "peer DSC required: ret=%d value=%#x\n",
		ret, le32_to_cpu(required));
	if (ret || !(le32_to_cpu(required) & 1)) {
		dsc->required = false;
		dsc->ret = ret;
		complete(&dsc->ready);
		return;
	}
	ret = dcp_m4_peer_call_oob(r, 367, caps_req, sizeof(caps_req),
				   dsc->caps, sizeof(dsc->caps), 5000);
	if (!ret) {
		ret = dcp_m4_peer_call_oob(r, 370, bpp_req, sizeof(bpp_req),
					   bpp, sizeof(bpp), 5000);
		if (!ret && get_unaligned_le32(bpp + 2))
			ret = -EIO;
		if (!ret)
			memcpy(dsc->bpp, bpp, sizeof(dsc->bpp));
	}
	if (!ret) {
		ret = dcp_m4_peer_call_oob(r, 372, hints_req, sizeof(hints_req),
					   hints, sizeof(hints), 5000);
		if (!ret && get_unaligned_le32(hints + 8))
			ret = -EIO;
		if (!ret)
			memcpy(dsc->hints, hints, sizeof(dsc->hints));
	}
	dsc->required = !ret;
	dsc->ret = ret;
	complete(&dsc->ready);
}

static int dcp_m4_peer_write_dsc(struct dcp_m4_runtime *r, struct dcp_m4_runtime *peer_r,
				 const struct dcp_m4_peer_dsc *dsc)
{
	u8 caps_req[36] = {}, caps[32] = {};
	u8 bpp_req[4] = {}, bpp[8] = {};
	u8 hints_req[12] = {}, hints[12] = {};
	int ret;

	if (dsc->ret)
		return dsc->ret;
	if (!dsc->required)
		return 0;
	memcpy(caps_req, dsc->caps, sizeof(dsc->caps));
	ret = dcp_m4_peer_call(peer_r, 368, 0, caps_req, sizeof(caps_req),
			       caps, sizeof(caps), 5000);
	if (ret)
		return ret;
	memcpy(bpp_req, dsc->bpp, sizeof(dsc->bpp));
	ret = dcp_m4_peer_call(peer_r, 371, 0, bpp_req, sizeof(bpp_req),
			       bpp, sizeof(bpp), 5000);
	if (ret || get_unaligned_le32(bpp + 2))
		return ret ?: -EIO;
	memcpy(hints_req, dsc->hints, sizeof(dsc->hints));
	ret = dcp_m4_peer_call(peer_r, 373, 0, hints_req, sizeof(hints_req),
			       hints, sizeof(hints), 5000);
	if (ret || get_unaligned_le32(hints + 8))
		return ret ?: -EIO;
	dev_dbg(r->dcp->dev, "peer DSC copied: bpp=%u hints=%*ph\n",
		get_unaligned_le16(dsc->bpp), 8, dsc->hints);
	return 0;
}

static int dcp_m4_peer_sync_swap_ids(struct dcp_m4_runtime *r, struct dcp_m4_runtime *peer_r,
				     const char *when)
{
	__le32 primary_id = 0, peer_id = 0, master;
	u32 p_id, s_id;
	int ret;

	ret = dcp_m4_peer_call(r, 360, 0, NULL, 0, &primary_id, sizeof(primary_id), 5000);
	if (ret)
		return ret;
	ret = dcp_m4_peer_call(peer_r, 360, 0, NULL, 0, &peer_id, sizeof(peer_id), 5000);
	if (ret)
		return ret;
	p_id = le32_to_cpu(primary_id);
	s_id = le32_to_cpu(peer_id);
	dev_dbg(r->dcp->dev, "peer master swap IDs %s: primary %u peer %u\n", when, p_id, s_id);
	if (p_id != s_id) {
		master = cpu_to_le32(max(p_id, s_id));
		ret = dcp_m4_peer_call(p_id < s_id ? r : peer_r, 361, 0,
				       &master, sizeof(master), NULL, 0, 5000);
	}
	return ret;
}

static int dcp_m4_peer_modeset(struct dcp_m4_runtime *r, struct dcp_m4_runtime *peer_r,
			       const struct dcp_m4_peer_insert_video_req *video,
			       bool firmware_requires_merge)
{
	struct iomfb_m4 *dcp = r->dcp, *peer = peer_r->dcp;
	__le32 mode[2] = { video->color_id, video->timing_id };
	__le32 p_status = 0, s_status = 0;
	struct dcp_m4_peer_rpc_work a413 = {
		.r = r, .method = 413, .stream = 0,
		.input = mode, .input_size = sizeof(mode),
		.output = &p_status, .output_size = sizeof(p_status),
		.timeout_ms = 10000,
	};
	struct dcp_m4_peer_dsc dsc = {};
	unsigned long deadline;
	u32 phase;
	bool primary_done = false;
	int ret;

	mutex_lock(&dcp_m4_peer_lock);
	if (dcp->peer_reserved != peer || peer->peer_owner != dcp ||
	    !dcp->peer_tiling.display_id_valid || !peer->peer_tiling.display_id_valid) {
		mutex_unlock(&dcp_m4_peer_lock);
		return -ENODEV;
	}
	dcp->peer_tiling.member = peer->peer_tiling.member = true;
	dcp->peer_tiling.mode_flag = peer->peer_tiling.mode_flag = !firmware_requires_merge;
	mutex_unlock(&dcp_m4_peer_lock);

	ret = dcp_m4_peer_power(r, peer_r, true);
	if (ret)
		return ret;
	msleep(100);

	ret = dcp_m4_peer_sync_swap_ids(r, peer_r, "before A413");
	if (ret)
		return ret;

	phase = 1;
	dcp_m4_peer_tiling_state(dcp, true, 1, 10, &phase);
	dcp_m4_peer_tiling_state(peer, true, 1, 10, &phase);

	init_completion(&dsc.ready);
	r->peer_dsc = &dsc;
	r->peer_dsc_trigger = false;

	INIT_WORK_ONSTACK(&a413.work, dcp_m4_peer_rpc_fn);
	init_completion(&a413.done);
	queue_work(system_dfl_wq, &a413.work);

	deadline = jiffies + msecs_to_jiffies(9000);
	for (;;) {
		phase = 0;
		if (!dcp_m4_peer_tiling_state(dcp, false, 1, 10, &phase)) {
			ret = -ENODEV;
			goto wait_primary;
		}
		if (completion_done(&dsc.ready))
			break;
		if (completion_done(&a413.done)) {
			primary_done = true;
			break;
		}
		if (time_after_eq(jiffies, deadline)) {
			dev_err(dcp->dev, "peer primary A413 timed out at phase=%u\n", phase);
			ret = -ETIMEDOUT;
			goto wait_primary;
		}
		msleep(2);
	}

	if (primary_done) {
		flush_work(&a413.work);
		if (a413.ret || p_status) {
			dev_err(dcp->dev, "peer primary A413 failed: ret=%d status=%#x\n",
				a413.ret, le32_to_cpu(p_status));
			ret = a413.ret ?: -EIO;
			goto out;
		}
		if (!completion_done(&dsc.ready))
			dcp_m4_peer_read_dsc_oob(r);
	}

	ret = dcp_m4_peer_write_dsc(r, peer_r, &dsc);
	if (ret)
		goto wait_primary;

	ret = dcp_m4_peer_call(peer_r, 413, 0, mode, sizeof(mode),
			       &s_status, sizeof(s_status), 10000);
	if (ret || s_status) {
		dev_err(dcp->dev, "peer A413 failed: ret=%d status=%#x\n",
			ret, le32_to_cpu(s_status));
		ret = ret ?: -EIO;
	}

wait_primary:
	if (!primary_done) {
		flush_work(&a413.work);
		if (!ret && (a413.ret || p_status)) {
			dev_err(dcp->dev, "peer primary A413 failed: ret=%d status=%#x\n",
				a413.ret, le32_to_cpu(p_status));
			ret = a413.ret ?: -EIO;
		}
	}
	if (!ret)
		ret = dcp_m4_peer_sync_swap_ids(r, peer_r, "after paired modeset");
out:
	r->peer_dsc_trigger = false;
	r->peer_dsc = NULL;
	destroy_work_on_stack(&a413.work);
	if (!ret) {
		mutex_lock(&dcp_m4_peer_lock);
		dcp->peer_tiling.modeset_complete = true;
		peer->peer_tiling.modeset_complete = true;
		mutex_unlock(&dcp_m4_peer_lock);
	}
	dev_dbg(dcp->dev, "peer paired modeset result=%d\n", ret);
	return ret;
}

/*
 * Query A364 and, if the mode requires dual-pipe merge, install the split
 * derived mode on both primary and peer. Returns 0 on paired success,
 * -EOPNOTSUPP if the mode runs on a single pipe.
 */
static int dcp_m4_peer_install_video(struct dcp_m4_runtime *r, struct dcp_m4_runtime *peer_r,
				     u32 timing, u32 color, u32 width, u32 height)
{
	struct iomfb_m4 *dcp = r->dcp, *peer = peer_r->dcp;
	struct dcp_m4_peer_get_video_req req = {};
	struct dcp_m4_peer_get_video_resp resp = {};
	struct dcp_m4_peer_insert_video_req insert = {};
	struct dcp_m4_peer_insert_video_resp inserted = {};
	__le32 enabled = cpu_to_le32(1);
	u32 tile_w, tile_h;
	int ret;

	if (timing > U32_MAX - 10000 || color > U32_MAX - 10000)
		return -EOPNOTSUPP;

	req.timing_id = cpu_to_le32(timing);
	req.color_id = cpu_to_le32(color);
	ret = dcp_m4_ext_call(r, 364, 0, &req, sizeof(req), &resp, sizeof(resp), 5000);
	if (ret)
		return ret;
	dev_dbg(dcp->dev, "peer A364 timing=%u color=%u requires_merge=%u success=%u\n",
		timing, color, resp.requires_merge & 1, resp.success & 1);
	if (!(resp.success & 1))
		return -ENOENT;
	if (!(resp.requires_merge & 1))
		return -EOPNOTSUPP;

	ret = dcp_m4_peer_quiesce(r, peer_r);
	if (ret)
		return ret;

	insert.video = resp.video;
	dcp_m4_peer_split_timing(insert.video.timing);
	insert.timing_id = cpu_to_le32(timing + 10000);
	insert.color_id = cpu_to_le32(color + 10000);
	tile_w = get_unaligned_le32(insert.video.timing + 8);
	tile_h = get_unaligned_le32(insert.video.timing + 0x28);

	mutex_lock(&dcp_m4_peer_lock);
	if (dcp->peer_reserved != peer || peer->peer_owner != dcp) {
		mutex_unlock(&dcp_m4_peer_lock);
		return -ENODEV;
	}
	dcp->peer_tiling.vftg = peer->peer_tiling.vftg = true;
	dcp->peer_tiling.modeset_complete = peer->peer_tiling.modeset_complete = false;
	dcp->peer_tiling.primary = true;
	peer->peer_tiling.primary = false;
	dcp->peer_tiling.width = peer->peer_tiling.width = tile_w;
	dcp->peer_tiling.height = peer->peer_tiling.height = tile_h;
	dcp->peer_tiling.x = dcp->peer_tiling.y = peer->peer_tiling.y = 0;
	peer->peer_tiling.x = tile_w;
	mutex_unlock(&dcp_m4_peer_lock);

	ret = dcp_m4_peer_call(peer_r, 366, 0, &enabled, sizeof(enabled), NULL, 0, 5000);
	if (ret)
		return ret;

	ret = dcp_m4_peer_call(r, 365, 0, &insert, sizeof(insert),
			       &inserted, sizeof(inserted), 5000);
	if (!ret && !(inserted.success & 1))
		ret = -EIO;
	if (ret)
		return ret;

	insert.video = inserted.video;
	ret = dcp_m4_peer_call(peer_r, 365, 0, &insert, sizeof(insert),
			       &inserted, sizeof(inserted), 5000);
	if (!ret && !(inserted.success & 1))
		ret = -EIO;
	if (ret)
		return ret;

	dev_dbg(dcp->dev, "peer derived mode timing=%u color=%u tile=%ux%u\n",
		timing + 10000, color + 10000, tile_w, tile_h);

	ret = dcp_m4_peer_modeset(r, peer_r, &insert, resp.requires_merge & 1);
	if (ret)
		return ret;

	peer_r->ext_width = width;
	peer_r->ext_height = height;
	mutex_lock(&dcp_m4_peer_lock);
	dcp->peer_active = true;
	dcp->peer_failed = false;
	dcp->peer_mode_timing = timing;
	dcp->peer_mode_color = color;
	mutex_unlock(&dcp_m4_peer_lock);
	return 0;
}

/* Map primary's retained SID0 copy surfaces into peer's SID0 DART (zero extra RAM). */
static int dcp_m4_peer_drm_buffers(struct dcp_m4_runtime *r, struct dcp_m4_runtime *peer_r)
{
	struct iomfb_m4 *dcp = r->dcp, *peer = peer_r->dcp;
	int ret;

	ret = dcp_m4_ext_scanout_ok(peer, "peer DRM buffers");
	if (ret)
		return ret;
	for (unsigned int i = 0; i < 2; i++) {
		u64 src_dva = r->ext_buf[i].dva;
		u64 peer_dva = 0;

		if (!r->ext_buf_cpu[i] || !src_dva)
			return -ENODEV;
		if (r->ext_peer_buf_src[i] == src_dva && r->ext_peer_buf_dva[i])
			continue;
		ret = apple_dart_dcp_dispext_scanout_map_peer(&dcp->piodma->dev, src_dva,
							      &peer->piodma->dev, &peer_dva);
		dev_dbg(peer->dev, "peer copy surface %u: src %#llx -> peer SID0 %#llx: %d\n",
			i, src_dva, peer_dva, ret);
		if (ret)
			return ret;
		r->ext_peer_buf_dva[i] = peer_dva;
		r->ext_peer_buf_src[i] = src_dva;
	}
	peer_r->ext_peer_buf_ready = true;
	return 0;
}

struct dcp_m4_peer_swap_work {
	struct work_struct work;
	struct dcp_m4_runtime *r;
	const void *input;
	u8 submitted[12];
	int link_gen;
	bool sync;
	bool d590_missing;
	int ret;
};

static void dcp_m4_peer_swap_fn(struct work_struct *work)
{
	struct dcp_m4_peer_swap_work *w = container_of(work, struct dcp_m4_peer_swap_work, work);
	struct dcp_m4_runtime *r = w->r;
	u64 word;
	int ret;

	r->deadline = jiffies + msecs_to_jiffies(5000);
	ret = dcp_m4_runtime_call(r, 407, false, w->input, DCP_M4_SWAP_SIZE,
				  w->submitted, sizeof(w->submitted));
	if (!ret)
		ret = dcp_m4_swap_record_reply(&r->swap, w->submitted, sizeof(w->submitted));
	if (w->sync) {
		while (!ret && !r->swap.completed) {
			ret = dcp_m4_ext_swap_wait(r, w->link_gen, &word);
			w->d590_missing = ret == -ETIMEDOUT || ret == -ENOLINK;
			if (!ret)
				ret = dcp_m4_runtime_callback(r, word);
		}
		if (!ret)
			ret = dcp_m4_runtime_live(r);
		if (!ret && dcp_m4_swap_take_completion(&r->swap) != 1)
			ret = -EPROTO;
	}
	r->deadline = 0;
	w->ret = ret;
}

static int dcp_m4_peer_swap_handoff(struct dcp_m4_runtime *r, int win, int link_gen,
				    unsigned int async_depth, bool is_peer)
{
	struct dcp_m4_ext_inflight *e =
		&r->ext_q[min_t(unsigned int, r->ext_qn, DCP_M4_EXT_INFLIGHT - 1)];
	struct dcp_m4_swap_state handed;
	int ret;

	ret = WARN_ON_ONCE(r->ext_qn >= DCP_M4_EXT_INFLIGHT) ? -EBUSY :
	      dcp_m4_swap_hand_off(&r->swap, &handed);
	if (ret)
		return ret;
	*e = (struct dcp_m4_ext_inflight){
		.swap = handed,
		.win = win,
		.gen = link_gen,
		.deadline = jiffies + msecs_to_jiffies(5000),
		.flip_held = !is_peer && (r->ext_qn || async_depth < 2),
	};
	if (!is_peer)
		r->ext_swap_now = !e->flip_held;
	r->ext_qn++;
	if (e->swap.completed) {
		dcp_m4_ext_async_done(r, r->ext_qn - 1, DCP_M4_EXT_END_TAKEN);
	} else {
		queue_delayed_work(r->wq, &r->ext_async_work, msecs_to_jiffies(5000));
		if ((!is_peer && !r->ext_connected) || atomic_read(&r->ext_link_gen) != link_gen)
			mod_delayed_work(r->wq, &r->ext_async_work, 0);
	}
	return 0;
}

/*
 * Paired swap on primary (left tile) and peer (right tile), or surfaceless
 * blank when @frame is NULL. Both primary->lock and peer->lock are held.
 * When @async_depth > 0, returns after both A407 replies and tracks both swaps
 * in flight; when @async_depth == 0, waits for D590 on both pipes in parallel.
 */
static int dcp_m4_peer_swap(struct dcp_m4_runtime *r, struct dcp_m4_runtime *peer_r,
			    const struct dcp_m4_frame *frame,
			    const struct dcp_m4_frame *peer_frame,
			    int win, int peer_win, unsigned int async_depth, u32 tile_w)
{
	struct iomfb_m4 *dcp = r->dcp, *peer = peer_r->dcp;
	u8 start[16] = {0}, p_started[8], s_started[8], p_submitted[12];
	int p_link_gen = atomic_read(&r->ext_link_gen);
	int s_link_gen = atomic_read(&peer_r->ext_link_gen);
	u32 width = r->ext_width, height = r->ext_height;
	u8 *input_l = NULL, *input_r = NULL;
	u32 id = 0, peer_id = 0;
	bool p_d590_missing = false;
	u64 word;
	int ret;

	if (frame) {
		if (!peer_frame || !width || !height || !tile_w || tile_w * 2 > width ||
		    dcp_m4_frame_check_ext(frame) || dcp_m4_frame_check_ext(peer_frame) ||
		    frame->width != width || frame->height != height ||
		    peer_frame->width != width || peer_frame->height != height)
			return -EINVAL;
	}
	if ((dcp->piodma && apple_dart_dcp_dispext_stream_ok(&dcp->piodma->dev, 0) == -EIO) ||
	    (peer->piodma && apple_dart_dcp_dispext_stream_ok(&peer->piodma->dev, 0) == -EIO))
		return -EIO;
	if (r->ext_orphan_start || peer_r->ext_orphan_start)
		return -EBUSY;

	input_l = kzalloc(DCP_M4_SWAP_SIZE, GFP_KERNEL);
	input_r = kzalloc(DCP_M4_SWAP_SIZE, GFP_KERNEL);
	if (!input_l || !input_r) {
		kfree(input_l);
		kfree(input_r);
		return -ENOMEM;
	}

	/* 1. Start primary swap (A406). */
	r->deadline = jiffies + msecs_to_jiffies(5000);
	ret = dcp_m4_runtime_call(r, 406, false, start, sizeof(start),
				  p_started, sizeof(p_started));
	r->deadline = 0;
	if (!ret)
		ret = dcp_m4_swap_start_reply(p_started, sizeof(p_started), &id);
	if (!ret && (dcp_m4_ext_lost_slot(r, id) >= 0 || dcp_m4_ext_early_slot(r, id) >= 0)) {
		dev_warn(dcp->dev, "paired A406 reused unresolved swap %u on primary\n", id);
		r->ext_orphan_start = id;
		r->ext_hold_id = 0;
		r->ext_hold_probe = false;
		ret = -EBUSY;
		goto free_bufs;
	}
	if (ret)
		goto fail_primary;

	/* 2. Start peer swap (A406), resyncing master swap ID via A361 once on mismatch. */
	peer_r->deadline = jiffies + msecs_to_jiffies(5000);
	ret = dcp_m4_runtime_call(peer_r, 406, false, start, sizeof(start),
				  s_started, sizeof(s_started));
	peer_r->deadline = 0;
	if (!ret)
		ret = dcp_m4_swap_start_reply(s_started, sizeof(s_started), &peer_id);
	if (!ret && peer_id != id) {
		__le32 master = cpu_to_le32(id);

		dev_info_ratelimited(peer->dev,
				     "peer swap ID %u != primary %u: setting master ID\n",
				     peer_id, id);
		ret = dcp_m4_peer_call(peer_r, 361, 0, &master, sizeof(master), NULL, 0, 5000);
		if (!ret) {
			peer_r->deadline = jiffies + msecs_to_jiffies(5000);
			ret = dcp_m4_runtime_call(peer_r, 406, false, start, sizeof(start),
						  s_started, sizeof(s_started));
			peer_r->deadline = 0;
		}
		if (!ret)
			ret = dcp_m4_swap_start_reply(s_started, sizeof(s_started), &peer_id);
		if (!ret && peer_id != id) {
			dev_err(peer->dev, "peer swap ID %u still != primary %u\n", peer_id, id);
			ret = -EPROTO;
		}
	}
	if (!ret && (dcp_m4_ext_lost_slot(peer_r, peer_id) >= 0 ||
		     dcp_m4_ext_early_slot(peer_r, peer_id) >= 0)) {
		dev_warn(peer->dev, "paired A406 reused unresolved swap %u on peer\n", peer_id);
		peer_r->ext_orphan_start = peer_id;
		r->ext_orphan_start = id;
		ret = -EBUSY;
		goto free_bufs;
	}
	if (ret) {
		r->ext_orphan_start = id;
		mutex_lock(&dcp_m4_peer_lock);
		dcp->peer_failed = true;
		mutex_unlock(&dcp_m4_peer_lock);
		goto free_bufs;
	}

	/* 3. Encode left and right tile swap payloads. */
	ret = dcp_m4_swap_begin(&r->swap, id) ?: dcp_m4_swap_begin(&peer_r->swap, peer_id);
	if (!ret)
		ret = dcp_m4_swap_encode_on(input_l, DCP_M4_SWAP_SIZE, id, true, frame, NULL, 0,
					    false, 0, dcp_m4_frame_check_ext, width, height);
	if (!ret)
		ret = dcp_m4_swap_encode_on(input_r, DCP_M4_SWAP_SIZE, peer_id, true, peer_frame,
					    NULL, 0, false, 0, dcp_m4_frame_check_ext,
					    width, height);
	if (!ret && frame) {
		ret = dcp_m4_swap_set_tile(input_l, DCP_M4_SWAP_SIZE, width, 0, tile_w, height) ?:
		      dcp_m4_swap_set_tile(input_r, DCP_M4_SWAP_SIZE, width, tile_w, tile_w, height);
	}
	if (ret)
		goto fail_both;

	/* 4. Submit A407 on peer (worker) and primary (here) in parallel. */
	{
		struct dcp_m4_peer_swap_work sw = {
			.r = peer_r, .input = input_r, .link_gen = s_link_gen,
			.sync = !async_depth,
		};

		peer_r->ext_swap_surface = -1;
		peer_r->ext_swap_win = peer_win;
		peer_r->ext_swap_async = async_depth;
		peer_r->ext_swap_now = false;

		INIT_WORK_ONSTACK(&sw.work, dcp_m4_peer_swap_fn);
		queue_work(system_dfl_wq, &sw.work);

		r->deadline = jiffies + msecs_to_jiffies(5000);
		ret = dcp_m4_runtime_call(r, 407, false, input_l, DCP_M4_SWAP_SIZE,
					  p_submitted, sizeof(p_submitted));
		if (!ret)
			ret = dcp_m4_swap_record_reply(&r->swap, p_submitted, sizeof(p_submitted));
		if (!async_depth) {
			while (!ret && !r->swap.completed) {
				ret = dcp_m4_ext_swap_wait(r, p_link_gen, &word);
				p_d590_missing = ret == -ETIMEDOUT || ret == -ENOLINK;
				if (!ret)
					ret = dcp_m4_runtime_callback(r, word);
			}
			if (!ret)
				ret = dcp_m4_runtime_live(r);
			if (!ret && dcp_m4_swap_take_completion(&r->swap) != 1)
				ret = -EPROTO;
		}
		r->deadline = 0;

		flush_work(&sw.work);
		destroy_work_on_stack(&sw.work);

		peer_r->ext_swap_async = 0;
		peer_r->ext_swap_win = -1;

		if (async_depth) {
			int pret = ret, sret = sw.ret;

			if (!pret)
				pret = dcp_m4_peer_swap_handoff(r, win, p_link_gen,
								async_depth, false);
			if (!sret)
				sret = dcp_m4_peer_swap_handoff(peer_r, peer_win, s_link_gen,
								async_depth, true);
			ret = pret ?: sret;
			if (ret) {
				mutex_lock(&dcp_m4_peer_lock);
				dcp->peer_failed = true;
				mutex_unlock(&dcp_m4_peer_lock);
			}
			goto free_bufs;
		}

		if (ret && p_d590_missing &&
		    dcp_m4_ext_swap_give_up(r, &r->swap, -1, false, ret))
			p_d590_missing = false;
		else if (ret) {
			dcp_m4_swap_fail(&r->swap, ret);
			dcp_m4_transaction_fail(&dcp->transaction, ret);
			dcp_m4_failed(dcp, ret);
		}

		if (sw.ret && sw.d590_missing &&
		    dcp_m4_ext_swap_give_up(peer_r, &peer_r->swap, -1, false, sw.ret))
			sw.d590_missing = false;
		else if (sw.ret) {
			dcp_m4_swap_fail(&peer_r->swap, sw.ret);
			dcp_m4_transaction_fail(&peer->transaction, sw.ret);
			dcp_m4_failed(peer, sw.ret);
		}

		if (!ret && sw.ret)
			ret = sw.ret;
		if (ret) {
			mutex_lock(&dcp_m4_peer_lock);
			dcp->peer_failed = true;
			mutex_unlock(&dcp_m4_peer_lock);
		}
		goto free_bufs;
	}

fail_both:
	dcp_m4_swap_fail(&peer_r->swap, ret);
	dcp_m4_transaction_fail(&peer->transaction, ret);
	dcp_m4_failed(peer, ret);
fail_primary:
	dcp_m4_swap_fail(&r->swap, ret);
	dcp_m4_transaction_fail(&dcp->transaction, ret);
	dcp_m4_failed(dcp, ret);
free_bufs:
	kfree(input_l);
	kfree(input_r);
	return ret;
}

static void dcp_m4_peer_stop_work(struct work_struct *work)
{
	struct iomfb_m4 *dcp = container_of(work, struct iomfb_m4, peer_stop_work);
	struct iomfb_m4 *peer;
	struct dcp_m4_runtime *r, *peer_r = NULL;
	bool merged = false;

	mutex_lock(&dcp->lock);
	r = dcp->runtime;
	peer = dcp_m4_peer_get_locked(dcp, &peer_r, false, NULL);
	if (r && peer && peer_r) {
		dcp_m4_ext_settle(peer, peer_r, "peer-stop");
		mutex_lock(&dcp_m4_peer_lock);
		merged = dcp->peer_tiling.vftg || dcp->peer_tiling.member ||
			 peer->peer_tiling.vftg || peer->peer_tiling.member;
		mutex_unlock(&dcp_m4_peer_lock);
		if (merged && !READ_ONCE(dcp->error) && !READ_ONCE(peer->error)) {
			int ret = dcp_m4_peer_quiesce(r, peer_r);

			dev_info(dcp->dev, "peer stop on disconnect: %d\n", ret);
		}
		mutex_unlock(&peer->lock);
	}
	mutex_unlock(&dcp->lock);
	dcp_m4_peer_release(dcp);
}

