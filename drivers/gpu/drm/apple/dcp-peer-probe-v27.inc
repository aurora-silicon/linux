/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Explicit, opt-in firmware bring-up command. This is not the final hotplug
 * lifecycle: use only the dedicated, otherwise unrouted peer reservation.
 */
struct dcp_peer_probe_wait {
	struct kref refs;
	struct completion done;
};

static void dcp_peer_probe_free(struct kref *ref)
{
	kfree(container_of(ref, struct dcp_peer_probe_wait, refs));
}

static void dcp_peer_probe_done(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_peer_probe_wait *wait = cookie;

	complete(&wait->done);
	kref_put(&wait->refs, dcp_peer_probe_free);
}

static ssize_t peer_setup_test_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	static const struct dcp_method_entry method = {
		.name = "setup_peer_dcp", .tag = {'A', '3', '6', '6'},
	};
	struct apple_dcp *dcp = dev_get_drvdata(dev);
	struct dcp_peer_probe_wait *wait;
	__le32 enabled = cpu_to_le32(1);
	bool value;
	long ret;

	if (kstrtobool(buf, &value) || !value)
		return -EINVAL;
	if (!peer_allocation_probe || !dcp)
		return -EOPNOTSUPP;
	wait = kzalloc(sizeof(*wait), GFP_KERNEL);
	if (!wait)
		return -ENOMEM;
	init_completion(&wait->done);
	kref_init(&wait->refs);
	{
		guard(mutex)(&dcp_peer_lock);
		if (!dcp->peer_owner || dcp->peer_owner->peer_reserved != dcp ||
		    dcp->peer_unbinding || dcp->crashed || !dcp->active ||
		    dcp->fw_compat != DCP_FIRMWARE_V_27_0 ||
		    dcp->fixed_phy || dcp->nr_typec_routes ||
		    (dcp->connector && dcp->connector->connected) ||
		    dcp->ch_cmd.depth || dcp->ch_cb.depth || dcp->ch_async.depth ||
		    dcp->ch_oobcmd.depth || dcp->ch_oobcb.depth ||
		    dcp->ch_oobasync.depth) {
			kref_put(&wait->refs, dcp_peer_probe_free);
			return -EBUSY;
		}
		kref_get(&wait->refs);
		/* Native peer-DCP setup (one bool argument): four input
		 * bytes (boolean followed by zero padding), no output.
		 */
		dcp_push(dcp, false, &method, sizeof(enabled), 0, &enabled,
			 dcp_peer_probe_done, wait);
	}
	ret = wait_for_completion_timeout(&wait->done, msecs_to_jiffies(2000));
	kref_put(&wait->refs, dcp_peer_probe_free);
	dev_info(dev, "peer A366 setup test %s\n", ret ? "acknowledged" : "timed out");
	return ret ? count : -ETIMEDOUT;
}
static DEVICE_ATTR_WO(peer_setup_test);

#include "dcp-peer-video-probe-v27.inc"
#include "dcp-peer-swap-probe-v27.inc"
#include "dcp-peer-runtime-v27.inc"

static struct attribute *dcp_peer_probe_attrs[] = {
	&dev_attr_peer_setup_test.attr,
	&dev_attr_peer_video_test.attr,
	&dev_attr_peer_link_test.attr,
	&dev_attr_peer_swap_test.attr,
	&dev_attr_peer_stop_test.attr,
	NULL,
};

static umode_t dcp_peer_probe_visible(struct kobject *kobj,
				    struct attribute *attr, int n)
{
	return peer_allocation_probe ? attr->mode : 0;
}

static const struct attribute_group dcp_peer_probe_group = {
	.attrs = dcp_peer_probe_attrs,
	.is_visible = dcp_peer_probe_visible,
};
static const struct attribute_group *dcp_peer_probe_groups[] = {
	&dcp_peer_probe_group,
	NULL,
};
