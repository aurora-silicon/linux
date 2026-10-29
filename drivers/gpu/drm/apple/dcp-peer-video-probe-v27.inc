/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Opt-in A364/A365 bring-up. Installs derived modes without selecting them;
 * paired power, modeset and swap ownership must be implemented separately.
 */
#include "iomfb-peer-v27.h"

struct dcp_peer_video_wait {
	struct kref refs;
	struct completion done;
	size_t size;
	u8 reply[sizeof(struct dcp_peer_get_video_resp_v27)];
};

static void dcp_peer_video_free(struct kref *ref)
{
	kfree(container_of(ref, struct dcp_peer_video_wait, refs));
}

static void dcp_peer_video_done(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_peer_video_wait *wait = cookie;

	if (wait->size)
		memcpy(wait->reply, out, wait->size);
	complete(&wait->done);
	kref_put(&wait->refs, dcp_peer_video_free);
}

static int dcp_peer_video_call_stream(struct apple_dcp *dcp, bool independent,
			       const char *tag,
			       void *in, size_t in_size, void *out, size_t out_size)
{
	struct dcp_method_entry method = { .name = "peer_video_test" };
	struct dcp_peer_video_wait *wait;
	int ret = 0;

	if (out_size > sizeof(wait->reply))
		return -EINVAL;
	/* An unrelated AP thread gets its own native stream. A369/A367/etc.
	 * must not be mistaken for nested callbacks of the pending A413.
	 * Stream1 (the driver's OOB context) is independent of stream0.
	 */
	if (independent && (dcp->ch_oobcmd.depth || dcp->ch_oobcb.depth))
		return -EBUSY;
	wait = kzalloc(sizeof(*wait), GFP_KERNEL);
	if (!wait)
		return -ENOMEM;
	memcpy(method.tag, tag, sizeof(method.tag));
	wait->size = out_size;
	init_completion(&wait->done);
	kref_init(&wait->refs);
	kref_get(&wait->refs);
	dcp_push(dcp, independent, &method, in_size, out_size, in,
		 dcp_peer_video_done, wait);
	if (!wait_for_completion_timeout(&wait->done,
		msecs_to_jiffies(!memcmp(tag, "A413", 4) ? 8500 : 2000)))
		ret = -ETIMEDOUT;
	else if (out_size)
		memcpy(out, wait->reply, out_size);
	/* A late ACK owns only the heap cookie, never a caller's stack. */
	kref_put(&wait->refs, dcp_peer_video_free);
	return ret;
}

static int dcp_peer_video_call(struct apple_dcp *dcp, const char *tag,
			       void *in, size_t in_size, void *out, size_t out_size)
{
	return dcp_peer_video_call_stream(dcp, false, tag, in, in_size, out, out_size);
}

#include "dcp-peer-modeset-probe-v27.inc"

static bool dcp_peer_video_idle(struct apple_dcp *dcp)
{
	return dcp->active && !dcp->crashed && !dcp->peer_unbinding &&
	       !READ_ONCE(dcp->mode_state.changing) && !dcp->ch_cmd.depth && !dcp->ch_cb.depth &&
	       !dcp->ch_async.depth && !dcp->ch_oobcmd.depth &&
	       !dcp->ch_oobcb.depth && !dcp->ch_oobasync.depth;
}

/* Shared by the explicit diagnostic and normal DRM mode selection.
 * Install4 chooses a pair only when firmware's A364 requires one.
 */
static int dcp_peer_install_video_v27(struct apple_dcp *dcp, struct apple_dcp *peer,
                                     u32 timing, u32 color, u32 install)
{
	struct device *dev = dcp->dev;
	struct dcp_peer_get_video_req_v27 req = {};
	struct dcp_peer_get_video_resp_v27 resp;
	struct dcp_peer_insert_video_req_v27 insert = {};
	struct dcp_peer_insert_video_resp_v27 inserted;
	__le32 enabled = cpu_to_le32(1);
	int ret;

	req.timing_id = cpu_to_le32(timing);
	req.color_id = cpu_to_le32(color);
	ret = dcp_peer_video_call(dcp, "A364", &req, sizeof(req), &resp, sizeof(resp));
	if (ret)
		return ret;
	dev_info(dev, "peer video timing=%u color=%u success=%u merge=%u timing=%*ph color=%*ph\n",
		 timing, color, resp.success, resp.requires_merge,
		 64, resp.video.timing, 32, resp.video.color);
	dev_info(dev, "peer timing tail=%*ph\n", 16, resp.video.timing + 64);
	if (!(resp.success & 1)) {
		ret = -ENOENT;
		return ret;
	}
	if (!install)
		return ret;
	/* Mode3 explicitly tests a reserved pair using an existing lower-rate
	 * timing. It does not advertise a mode, raise a limit, or add scanout.
	 * Keep this diagnostic independent of production mode selection.
	 */
	if (!(resp.requires_merge & 1) && install != 3) {
		if (install == 4 && (dcp->peer_tiling.vftg || peer->peer_tiling.vftg)) {
			u8 power[16] = {1}, response[8];

			ret = dcp_peer_probe_quiesce(dcp, peer);
			if (ret)
				return ret;
			/* A mode-only DRM commit does not call CRTC poweron again. */
			ret = dcp_peer_video_call(dcp, "A484", power, sizeof(power),
						  response, sizeof(response));
			if (ret || get_unaligned_le32(response + 4))
				return ret ?: -EIO;
		}
		return -EOPNOTSUPP;
	}
	ret = dcp_peer_probe_quiesce(dcp, peer);
	if (ret)
		return ret;
	insert.video = resp.video;
	dcp_peer_split_timing_v27(&insert.video);
	insert.timing_id = cpu_to_le32(timing + 10000);
	insert.color_id = cpu_to_le32(color + 10000);
	ret = dcp_peer_probe_prepare_vftg(dcp, peer, &insert);
	if (ret)
		return ret;
	ret = dcp_peer_video_call(peer, "A366", &enabled, sizeof(enabled), NULL, 0);
	if (ret)
		return ret;
	ret = dcp_peer_video_call(dcp, "A365", &insert, sizeof(insert),
				  &inserted, sizeof(inserted));
	if (!ret && !(inserted.success & 1))
		ret = -EIO;
	if (ret)
		return ret;
	/* Native passes the first call's in/out timing and color to the peer. */
	insert.video = inserted.video;
	ret = dcp_peer_video_call(peer, "A365", &insert, sizeof(insert),
				  &inserted, sizeof(inserted));
	if (!ret && !(inserted.success & 1))
		ret = -EIO;
	dev_info(dev, "peer derived mode timing=%u color=%u width=%u height=%u clock=%u result=%d (not selected)\n",
		 timing + 10000, color + 10000,
		 get_unaligned_le32(insert.video.timing + 8),
		 get_unaligned_le32(insert.video.timing + 0x28),
		 get_unaligned_le32(insert.video.timing + 0x44), ret);
	if (!ret && install >= 2)
		ret = dcp_peer_probe_modeset(dcp, peer, &insert,
					     resp.requires_merge & 1);
	return ret;
}

static ssize_t peer_video_test_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct apple_dcp *dcp, *peer;
	struct device *peer_dev = NULL;
	u32 timing, color, install;
	char extra;
	int ret;

	if (sscanf(buf, "%u %u %u %c", &timing, &color, &install, &extra) != 3 ||
	    install > 3 || timing > U32_MAX - 10000 || color > U32_MAX - 10000)
		return -EINVAL;
	if (!peer_allocation_probe)
		return -EOPNOTSUPP;
	/* Trylocks avoid the sysfs-removal/device-lock inversion. Hold both
	 * devices across RPC waits so neither devm-backed DCP can be unbound.
	 */
	if (!device_trylock(dev))
		return -EBUSY;
	dcp = dev_get_drvdata(dev);
	{
		guard(mutex)(&dcp_peer_lock);
		peer = dcp ? dcp->peer_reserved : NULL;
		if (!peer || peer->peer_owner != dcp || !dcp_peer_video_idle(dcp) ||
		    !dcp_peer_video_idle(peer) || !dcp->crtc || !peer->crtc ||
		    !dcp->crtc->base.state || !peer->crtc->base.state ||
		    (install && (dcp->crtc->base.state->active ||
				 peer->crtc->base.state->active)) || peer->fixed_phy ||
		    peer->nr_typec_routes || peer->peer_tiling.member ||
		    dcp->peer_tiling.member ||
		    dcp->fw_compat != DCP_FIRMWARE_V_27_0 ||
		    peer->fw_compat != DCP_FIRMWARE_V_27_0 ||
		    (peer->connector && peer->connector->connected)) {
			ret = -EBUSY;
			goto unlock;
		}
		if (!device_trylock(peer->dev)) {
			ret = -EBUSY;
			goto unlock;
		}
		peer_dev = peer->dev;
	}
	ret = dcp_peer_install_video_v27(dcp, peer, timing, color, install);
unlock:
	if (peer_dev)
		device_unlock(peer_dev);
	device_unlock(dev);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(peer_video_test);

static ssize_t peer_link_test_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct apple_dcp *dcp;
	bool enabled;
	int ret;

	if (kstrtobool(buf, &enabled))
		return -EINVAL;
	if (!peer_allocation_probe || !device_trylock(dev))
		return -EBUSY;
	dcp = dev_get_drvdata(dev);
	if (!dcp || !dcp->active || dcp->crashed || !dcp->fixed_phy ||
	    dcp->nr_typec_routes || dcp->connector_type != DRM_MODE_CONNECTOR_HDMIA) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	{
		guard(mutex)(&dcp_peer_lock);
		if (dcp->peer_tiling.vftg || dcp->peer_tiling.member) {
			ret = -EBUSY;
			goto unlock;
		}
	}
	/* Use the existing disconnect/connect lifecycle, serialized with HPD.
	 * This diagnostic models link re-enumeration, not a physical cable pull.
	 */
	ret = dcp_typec_test_link(to_platform_device(dev), enabled);
unlock:
	device_unlock(dev);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(peer_link_test);
