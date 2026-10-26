/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Explicit background-only paired swap diagnostic. No framebuffer DMA or
 * DRM events are owned here. Both DRM CRTCs must remain inactive.
 */
struct dcp_peer_background {
	struct iomfb_transaction transaction;
	struct dcp_peer_video_wait *wait;
	struct dcp_swap_submit_req_v27 wire;
	bool sent;
};

static void dcp_peer_background_done(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_peer_background *background = cookie;
	struct dcp_swap_submit_resp_v27 failed = { .ret = ~0U };
	struct dcp_swap_submit_resp_v27 *response = out ?: &failed;

	if (!dcp_present_submit_h17p(&dcp->present_state_h17p,
				     background->wire.swap.swap_id, !response->ret))
		WRITE_ONCE(dcp->crashed, true);
	dcp_peer_video_done(dcp, response, background->wait);
}

static void dcp_peer_background_start(struct apple_dcp *dcp,
				      struct iomfb_transaction *transaction)
{
	struct dcp_peer_background *background =
		container_of(transaction, struct dcp_peer_background, transaction);
	struct dcp_method_entry method = { .name = "peer_background_test",
		.tag = {'A', '4', '0', '8'} };
	struct dcp_swap_submit_resp_v27 failed = { .ret = ~0U };

	background->sent = true;
	if (!dcp_present_begin_h17p(&dcp->present_state_h17p,
				    background->wire.swap.swap_id)) {
		dcp_peer_video_done(dcp, &failed, background->wait);
		return;
	}
	dcp_push(dcp, false, &method, sizeof(background->wire), sizeof(failed),
		 &background->wire, dcp_peer_background_done, background);
}

static void dcp_peer_background_release(struct iomfb_transaction *transaction)
{
	struct dcp_peer_background *background =
		container_of(transaction, struct dcp_peer_background, transaction);

	if (!background->sent)
		kref_put(&background->wait->refs, dcp_peer_video_free);
	kfree(background);
}

static int dcp_peer_queue_background(struct apple_dcp *dcp,
				     struct dcp_swap_submit_req_v27 *wire,
				     struct dcp_peer_video_wait *wait)
{
	struct dcp_peer_background *background;
	int ret;

	background = kzalloc_obj(*background);
	if (!background)
		return -ENOMEM;
	background->wire = *wire;
	background->wait = wait;
	kref_get(&wait->refs);
	background->transaction.start = dcp_peer_background_start;
	background->transaction.release = dcp_peer_background_release;
	ret = iomfb_queue(dcp, &background->transaction);
	if (ret)
		dcp_peer_background_release(&background->transaction);
	return ret;
}

static int dcp_peer_probe_background(struct apple_dcp *dcp, struct apple_dcp *peer)
{
	struct dcp_swap_start_req_h17p start = {};
	struct dcp_swap_start_resp_h17p a, b;
	struct dcp_swap_submit_req_v27 *submit;
	struct dcp_swap_submit_resp_v27 response;
	struct dcp_peer_video_wait *wait, *peer_wait;
	unsigned long deadline;
	int ret;

	submit = kzalloc(sizeof(*submit), GFP_KERNEL);
	wait = kzalloc(sizeof(*wait), GFP_KERNEL);
	peer_wait = kzalloc(sizeof(*peer_wait), GFP_KERNEL);
	if (!submit || !wait || !peer_wait) {
		kfree(submit);
		kfree(wait);
		kfree(peer_wait);
		return -ENOMEM;
	}
	kref_init(&wait->refs);
	init_completion(&wait->done);
	wait->size = sizeof(response);
	kref_init(&peer_wait->refs);
	init_completion(&peer_wait->done);
	peer_wait->size = sizeof(response);
	ret = dcp_peer_video_call(dcp, "A406", &start, sizeof(start), &a, sizeof(a));
	if (ret || a.ret) {
		ret = ret ?: -EIO;
		goto out;
	}
	ret = dcp_peer_video_call(peer, "A406", &start, sizeof(start), &b, sizeof(b));
	if (ret || b.ret || a.swap_id != b.swap_id) {
		dev_err(dcp->dev, "peer swap-start mismatch/error: %u/%u result=%d\n",
			a.swap_id, ret ? 0 : b.swap_id, ret);
		ret = ret ?: -EIO;
		goto out;
	}
	submit->swap.swap_id = a.swap_id;
	submit->swap.swap_enabled = submit->swap.swap_completed = IOMFB_SET_BACKGROUND | 7;
	submit->swap.bg_color = 0xffff0000;
	memset(submit->surf_null, 1, sizeof(submit->surf_null));
	submit->unknown_pointer_null = submit->unknown_output_null = true;
	ret = dcp_peer_queue_background(dcp, submit, wait);
	if (ret)
		goto out;
	/* Queue both engines before waiting, with each present tracked by its
	 * own IOMFB transaction and completion state. */
	submit->swap.bg_color = 0xff0000ff;
	ret = dcp_peer_queue_background(peer, submit, peer_wait);
	if (ret)
		goto out;
	if (!wait_for_completion_timeout(&peer_wait->done, msecs_to_jiffies(2000))) {
		ret = -ETIMEDOUT;
		goto out;
	}
	memcpy(&response, peer_wait->reply, sizeof(response));
	if (response.ret) {
		ret = -EIO;
		goto out;
	}
	if (!wait_for_completion_timeout(&wait->done, msecs_to_jiffies(2000))) {
		ret = -ETIMEDOUT;
		goto out;
	}
	memcpy(&response, wait->reply, sizeof(response));
	if (response.ret) {
		ret = -EIO;
		goto out;
	}
	deadline = jiffies + msecs_to_jiffies(2000);
	while (!READ_ONCE(dcp->have_swap_complete) || !READ_ONCE(peer->have_swap_complete) ||
	       READ_ONCE(dcp->last_swap_id) != a.swap_id ||
	       READ_ONCE(peer->last_swap_id) != a.swap_id) {
		if (time_after_eq(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			goto out;
		}
		msleep(2);
	}
	dev_info(dcp->dev, "peer background swap %u completed on both engines (optical unverified)\n",
		 a.swap_id);
out:
	dev_info(dcp->dev, "peer background test result=%d\n", ret);
	kref_put(&wait->refs, dcp_peer_video_free);
	kref_put(&peer_wait->refs, dcp_peer_video_free);
	kfree(submit);
	return ret;
}

static ssize_t peer_swap_test_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct apple_dcp *dcp, *peer;
	struct device *peer_dev = NULL;
	bool enabled;
	int ret;

	if (kstrtobool(buf, &enabled) || !enabled)
		return -EINVAL;
	if (!peer_allocation_probe || !device_trylock(dev))
		return -EBUSY;
	dcp = dev_get_drvdata(dev);
	{
		guard(mutex)(&dcp_peer_lock);
		peer = dcp ? dcp->peer_reserved : NULL;
		if (!peer || peer->peer_owner != dcp || !dcp_peer_video_idle(dcp) ||
		    !dcp_peer_video_idle(peer) || READ_ONCE(dcp->peer_frames.active) || !dcp->peer_tiling.member ||
		    !peer->peer_tiling.member || !dcp->peer_tiling.modeset_complete ||
		    !peer->peer_tiling.modeset_complete || !dcp->crtc || !peer->crtc ||
		    !dcp->crtc->base.state || !peer->crtc->base.state ||
		    dcp->crtc->base.state->active || peer->crtc->base.state->active ||
		    dcp->fw_compat != DCP_FIRMWARE_V_27_0 ||
		    peer->fw_compat != DCP_FIRMWARE_V_27_0 ||
		    !device_trylock(peer->dev)) {
			ret = -EBUSY;
			goto out;
		}
		peer_dev = peer->dev;
	}
	ret = dcp_peer_probe_background(dcp, peer);
out:
	if (peer_dev)
		device_unlock(peer_dev);
	device_unlock(dev);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(peer_swap_test);

/* End a diagnostic pair using the native power/element sequence. Keep the
 * reservation on failure: a failed ACK is not evidence that DMA has stopped.
 */
static ssize_t peer_stop_test_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct apple_dcp *dcp, *peer;
	struct device *peer_dev = NULL;
	bool enabled;
	int ret;

	if (kstrtobool(buf, &enabled) || !enabled)
		return -EINVAL;
	if (!peer_allocation_probe || !device_trylock(dev))
		return -EBUSY;
	dcp = dev_get_drvdata(dev);
	{
		guard(mutex)(&dcp_peer_lock);
		peer = dcp ? dcp->peer_reserved : NULL;
		if (!peer || peer->peer_owner != dcp || !dcp_peer_video_idle(dcp) ||
		    !dcp_peer_video_idle(peer) || !dcp->crtc || !peer->crtc ||
		    !dcp->crtc->base.state || !peer->crtc->base.state ||
		    dcp->crtc->base.state->active || peer->crtc->base.state->active ||
		    dcp->fw_compat != DCP_FIRMWARE_V_27_0 ||
		    peer->fw_compat != DCP_FIRMWARE_V_27_0 ||
		    !device_trylock(peer->dev)) {
			ret = -EBUSY;
			goto out;
		}
		peer_dev = peer->dev;
	}
	ret = dcp_peer_probe_quiesce(dcp, peer);
	dev_info(dev, "peer stop test result=%d\n", ret);
out:
	if (peer_dev)
		device_unlock(peer_dev);
	device_unlock(dev);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(peer_stop_test);
