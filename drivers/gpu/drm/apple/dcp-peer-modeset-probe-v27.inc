/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Native paired modeset bring-up, opt-in only. Scanout is a separate step. */
/* The native power-state shim starts one asynchronous AP request per engine.
 * Submit both before waiting: either firmware may rendezvous with its peer.
 */
static int dcp_peer_probe_power(struct apple_dcp *dcp, struct apple_dcp *peer,
                                bool on)
{
	struct dcp_peer_video_wait *wait;
	struct dcp_method_entry method = { .name = "peer_power_test",
		.tag = {'A', '4', '8', '4'} };
	u8 power[16] = {}, response[8];
	int ret;

	wait = kzalloc(sizeof(*wait), GFP_KERNEL);
	if (!wait)
		return -ENOMEM;
	kref_init(&wait->refs);
	init_completion(&wait->done);
	wait->size = sizeof(response);
	put_unaligned_le64(on, power);
	kref_get(&wait->refs);
	dcp_push(dcp, false, &method, sizeof(power), sizeof(response), power,
		 dcp_peer_video_done, wait);
	ret = dcp_peer_video_call(peer, "A484", power, sizeof(power),
				  response, sizeof(response));
	if (!ret && get_unaligned_le32(response + 4))
		ret = -EIO;
	if (!wait_for_completion_timeout(&wait->done, msecs_to_jiffies(2000)))
		ret = ret ?: -ETIMEDOUT;
	else if (!ret && get_unaligned_le32(wait->reply + 4))
		ret = -EIO;
	kref_put(&wait->refs, dcp_peer_video_free);
	dev_info(dcp->dev, "peer power %u result=%d\n", on, ret);
	return ret;
}

static int dcp_peer_probe_quiesce(struct apple_dcp *dcp, struct apple_dcp *peer)
{
	__le32 manage = 0;
	int ret;

	/* Native set_digital_out_mode: pair API power0, 100ms,
	 * then manage_elements(false), A362, on each engine before A365.
	 * Only discard AP state after both engines have acknowledged power0.
	 */
	{
		guard(mutex)(&dcp_peer_lock);
		dcp->peer_tiling.modeset_complete = peer->peer_tiling.modeset_complete = false;
	}
	ret = dcp_peer_probe_power(dcp, peer, false);
	if (ret)
		return ret;
	msleep(100);
	ret = dcp_peer_video_call(dcp, "A362", &manage, sizeof(manage), NULL, 0);
	if (ret)
		return ret;
	ret = dcp_peer_video_call(peer, "A362", &manage, sizeof(manage), NULL, 0);
	if (ret)
		return ret;
	{
		guard(mutex)(&dcp_peer_lock);
		dcp->peer_frames = (struct dcp_peer_frames_v27){};
		dcp->peer_tiling.member = peer->peer_tiling.member = false;
		dcp->peer_tiling.vftg = peer->peer_tiling.vftg = false;
		dcp->peer_tiling.modeset_complete = peer->peer_tiling.modeset_complete = false;
		dcp->peer_tiling.phase = peer->peer_tiling.phase = 0;
		dcp->peer_tiling.modeset_state = peer->peer_tiling.modeset_state = 0;
		dcp->peer_tiling.ready = peer->peer_tiling.ready = false;
	}
	return 0;
}

static int dcp_peer_probe_prepare_vftg(struct apple_dcp *dcp, struct apple_dcp *peer,
				      const struct dcp_peer_insert_video_req_v27 *video)
{
	guard(mutex)(&dcp_peer_lock);
	if (dcp->peer_reserved != peer || peer->peer_owner != dcp)
		return -ENODEV;
	/* Native sets these BEFORE A366/A365, but marks membership only after
	 * both insertMergeModes calls have accepted the derived video data.
	 */
	dcp->peer_tiling.vftg = peer->peer_tiling.vftg = true;
	dcp->peer_tiling.modeset_complete = peer->peer_tiling.modeset_complete = false;
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

static int dcp_peer_probe_copy_dsc(struct apple_dcp *primary, struct apple_dcp *peer)
{
	u8 caps_req[36] = {}, caps[32], bpp_req[4] = {}, bpp[8];
	u8 hints_req[12] = {}, hints[12];
	u32 required;
	int ret;

	ret = dcp_peer_video_call_stream(primary, true, "A369", NULL, 0, &required, 4);
	if (ret || !(required & 1))
		return ret;
	ret = dcp_peer_video_call_stream(primary, true, "A367", caps_req, 36, caps, 32);
	if (ret)
		return ret;
	memcpy(caps_req, caps, 32);
	ret = dcp_peer_video_call(peer, "A368", caps_req, 36, caps, 32);
	if (ret)
		return ret;
	ret = dcp_peer_video_call_stream(primary, true, "A370", bpp_req, 4, bpp, 8);
	if (ret || get_unaligned_le32(bpp + 2))
		return ret ?: -EIO;
	memcpy(bpp_req, bpp, 2);
	ret = dcp_peer_video_call(peer, "A371", bpp_req, 4, bpp, 8);
	if (ret || get_unaligned_le32(bpp + 2))
		return ret ?: -EIO;
	ret = dcp_peer_video_call_stream(primary, true, "A372", hints_req, 12, hints, 12);
	if (ret || get_unaligned_le32(hints + 8))
		return ret ?: -EIO;
	memcpy(hints_req, hints, 8);
	ret = dcp_peer_video_call(peer, "A373", hints_req, 12, hints, 12);
	if (ret || get_unaligned_le32(hints + 8))
		return ret ?: -EIO;
	dev_info(primary->dev, "peer DSC copied: bpp=%u hints=%*ph\n",
		 get_unaligned_le16(bpp), 8, hints);
	return 0;
}

static int dcp_peer_probe_modeset(struct apple_dcp *dcp, struct apple_dcp *peer,
				 const struct dcp_peer_insert_video_req_v27 *video,
				 bool firmware_requires_merge)
{
	struct dcp_peer_video_wait *wait;
	struct dcp_method_entry method = { .name = "peer_modeset_test",
		.tag = {'A', '4', '1', '3'} };
	__le32 mode[2] = {video->color_id, video->timing_id};
	u32 primary_id, peer_id, phase, status;
	unsigned long deadline;
	int ret;

	{
		guard(mutex)(&dcp_peer_lock);
		if (dcp->peer_reserved != peer || peer->peer_owner != dcp ||
		    !dcp->peer_tiling.display_id_valid || !peer->peer_tiling.display_id_valid)
			return -ENODEV;
		dcp->peer_tiling.member = peer->peer_tiling.member = true;
		/* Native preserves the original A364 result even when the host
		 * allocation requires a pair for a lower-bandwidth timing.
		 */
		dcp->peer_tiling.mode_flag = peer->peer_tiling.mode_flag =
			!firmware_requires_merge;
	}
	ret = dcp_peer_probe_power(dcp, peer, true);
	if (ret)
		return ret;
	/* Native waits100ms after both API-power transitions, before swap IDs. */
	msleep(100);
	ret = dcp_peer_video_call(dcp, "A360", NULL, 0, &primary_id, 4);
	if (ret)
		return ret;
	ret = dcp_peer_video_call(peer, "A360", NULL, 0, &peer_id, 4);
	if (ret)
		return ret;
	if (primary_id != peer_id) {
		status = max(primary_id, peer_id);
		ret = dcp_peer_video_call(primary_id < peer_id ? dcp : peer,
					  "A361", &status, 4, NULL, 0);
		if (ret)
			return ret;
	}
	phase = 1;
	dcp_peer_tiling_state_v27(dcp, true, 1, 10, &phase);
	dcp_peer_tiling_state_v27(peer, true, 1, 10, &phase);
	wait = kzalloc(sizeof(*wait), GFP_KERNEL);
	if (!wait)
		return -ENOMEM;
	init_completion(&wait->done);
	kref_init(&wait->refs);
	kref_get(&wait->refs);
	wait->size = 4;
	dcp_push(dcp, false, &method, sizeof(mode), 4, mode,
		 dcp_peer_video_done, wait);
	/* The primary negotiates the link, signals phase2, and then waits for
	 * its peer. Waiting for its final ACK first would deadlock the pair.
	 */
	deadline = jiffies + msecs_to_jiffies(8500);
	for (;;) {
		phase = 0;
		if (!dcp_peer_tiling_state_v27(dcp, false, 1, 10, &phase)) {
			ret = -ENODEV;
			goto put;
		}
		if (phase >= 2)
			break;
		if (completion_done(&wait->done) || time_after_eq(jiffies, deadline)) {
			ret = completion_done(&wait->done) ? -EIO : -ETIMEDOUT;
			dev_err(dcp->dev, "peer primary ended before phase2: phase=%u result=%#x\n",
				phase, get_unaligned_le32(wait->reply));
			goto put;
		}
		msleep(2);
	}
	dev_info(dcp->dev, "peer primary reached modeset phase%u\n", phase);
	ret = dcp_peer_probe_copy_dsc(dcp, peer);
	if (ret)
		goto put;
	ret = dcp_peer_video_call(peer, "A413", mode, sizeof(mode), &status, 4);
	if (ret || status) {
		ret = ret ?: -EIO;
		goto put;
	}
	if (!wait_for_completion_timeout(&wait->done, msecs_to_jiffies(8500)))
		ret = -ETIMEDOUT;
	else if (get_unaligned_le32(wait->reply))
		ret = -EIO;
put:
	kref_put(&wait->refs, dcp_peer_video_free);
	if (!ret) {
		guard(mutex)(&dcp_peer_lock);
		dcp->peer_tiling.modeset_complete = peer->peer_tiling.modeset_complete = true;
	}
	dev_info(dcp->dev, "peer paired modeset result=%d (scanout not submitted)\n", ret);
	return ret;
}
