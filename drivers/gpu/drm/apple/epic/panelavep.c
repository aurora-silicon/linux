// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Aurora Silicon */
/*
 * The internal panel's AV and DisplayPort services on DCP firmware 27.
 *
 * The panel's display processor announces its AV and DP services (device,
 * service, video interface, controllers, power, ambient compensation, HDCP)
 * and its descriptor manager on endpoints 0x24-0x31, and the AP opens them:
 * at boot, and again whenever the firmware announces them afresh after the
 * display pipeline powered on. Until the video interface is open, the
 * firmware does not export the panel's modes after such a power-on, so a
 * mode set waits for a link that never comes up.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "../afk.h"
#include "../dcp.h"
#include "../dcp-internal.h"

static const u8 panelav_endpoints[] = {
	0x24, 0x25, 0x28, 0x29, 0x2a, 0x2b, 0x2d, 0x31,
};

/* The call the AP makes right after it opens a service, all-zero data. */
struct panelav_open_call {
	u16 group;
	u32 command;
	u32 len;
	const char *word4;	/* the DT property giving the data's fifth word; the rest is zero */
};

static const struct panelav_open_call panelav_device_call = { 0, 0xc, 0x50 };
static const struct panelav_open_call panelav_controller_call = { 0, 0xc, 0x20 };
static const struct panelav_open_call panelav_sac_call = { 0, 0x7, 0x10 };
static const struct panelav_open_call panelav_hdcp_call = {
	4, 0x6, 0x20, "apple,panel-hdcp-open-arg"
};

struct panelav_call_work {
	struct work_struct work;
	struct apple_epic_service *service;
	const struct panelav_open_call *call;
};

static void panelav_call_worker(struct work_struct *work)
{
	struct panelav_call_work *w = container_of(work, struct panelav_call_work, work);
	struct apple_epic_service *service = w->service;
	void *data;
	int ret;

	data = kzalloc(w->call->len, GFP_KERNEL);
	if (data) {
		u32 word4 = 0;

		if (w->call->word4 && w->call->len >= 5 * sizeof(u32) &&
		    !of_property_read_u32(service->ep->dcp->dev->of_node, w->call->word4, &word4))
			put_unaligned_le32(word4, data + 4 * sizeof(u32));
		ret = afk_service_call(service, w->call->group, w->call->command,
				       data, w->call->len, 0, data, w->call->len, 0);
		if (ret)
			dev_warn(service->ep->dcp->dev,
				 "panel AV: ep %02x interface %u call %u:%#x: %d\n",
				 service->ep->endpoint, service->channel, w->call->group,
				 w->call->command, ret);
		kfree(data);
	}
	afk_service_put(service);
	kfree(w);
}

static void panelav_queue_call(struct apple_epic_service *service,
			       const struct panelav_open_call *call)
{
	struct apple_dcp *dcp = service->ep->dcp;
	struct panelav_call_work *w;

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return;
	w->service = afk_service_get(service);
	if (!w->service) {
		kfree(w);
		return;
	}
	w->call = call;
	INIT_WORK(&w->work, panelav_call_worker);
	/* Not on the endpoint's own queue: the reply arrives there. */
	queue_work(dcp->panelav_wq, &w->work);
}

/*
 * The video interface is opened between the firmware's plug, which ends by
 * closing it, and the firmware's next round of reports, after which it
 * exports the panel's modes only if the interface is open. The wait after
 * the announcement that puts the open there is the device tree's
 * (apple,panel-video-open-delay-us, where the boot loader is to put it);
 * without the property the interface is opened as it is announced. A wait
 * the driver would not sleep through is not taken from the device tree.
 */
#define PANELAV_VIDEO_OPEN_MAX_US	100000

struct panelav_open_work {
	struct work_struct work;
	struct apple_epic_service *service;
};

static void panelav_open_worker(struct work_struct *work)
{
	struct panelav_open_work *w = container_of(work, struct panelav_open_work, work);
	struct apple_epic_service *service = w->service;
	u32 wait_us = 0;
	int ret;

	of_property_read_u32(service->ep->dcp->dev->of_node, "apple,panel-video-open-delay-us",
			     &wait_us);
	if (wait_us > PANELAV_VIDEO_OPEN_MAX_US)
		wait_us = 0;
	if (wait_us)
		usleep_range(wait_us, wait_us + 300);
	ret = afk_service_open(service);
	if (ret)
		dev_warn(service->ep->dcp->dev, "panel AV: video interface %u OPEN failed: %d\n",
			 service->channel, ret);
	else
		dev_info(service->ep->dcp->dev, "panel AV: video interface %u opened\n",
			 service->channel);
	afk_service_put(service);
	kfree(w);
}

static void panelav_init(struct apple_epic_service *service, const char *name,
			 const char *class, s64 unit)
{
	dev_dbg(service->ep->dcp->dev, "panel AV: ep %02x interface %u: %s (%s)\n",
		service->ep->endpoint, service->channel, name ?: "", class ?: "");
}

static void panelav_device_init(struct apple_epic_service *service,
				const char *name, const char *class, s64 unit)
{
	panelav_init(service, name, class, unit);
	panelav_queue_call(service, &panelav_device_call);
}

static void panelav_controller_init(struct apple_epic_service *service,
				    const char *name, const char *class, s64 unit)
{
	panelav_init(service, name, class, unit);
	panelav_queue_call(service, &panelav_controller_call);
}

static void panelav_sac_init(struct apple_epic_service *service,
			     const char *name, const char *class, s64 unit)
{
	panelav_init(service, name, class, unit);
	panelav_queue_call(service, &panelav_sac_call);
}

static void panelav_hdcp_init(struct apple_epic_service *service,
			      const char *name, const char *class, s64 unit)
{
	panelav_init(service, name, class, unit);
	/* The call's argument comes from the DT; without it the interface is only opened. */
	if (of_property_present(service->ep->dcp->dev->of_node, panelav_hdcp_call.word4))
		panelav_queue_call(service, &panelav_hdcp_call);
}

static void panelav_video_init(struct apple_epic_service *service,
			       const char *name, const char *class, s64 unit)
{
	struct apple_dcp *dcp = service->ep->dcp;
	struct panelav_open_work *w;

	panelav_init(service, name, class, unit);
	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return;
	w->service = afk_service_get(service);
	if (!w->service) {
		kfree(w);
		return;
	}
	INIT_WORK(&w->work, panelav_open_worker);
	queue_work(dcp->panelav_wq, &w->work);
}

/* The firmware's own calls to the SAC service: answered with its data, first word cleared. */
static int panelav_sac_call_v2(struct apple_epic_service *service, u16 group,
			       u32 command, const void *data, size_t size,
			       void *reply, size_t room)
{
	if (group != 1 || command)
		return -EOPNOTSUPP;
	memcpy(reply, data, min(size, room));
	memset(reply, 0, min_t(size_t, room, sizeof(u64)));
	return 0;
}

static const struct apple_epic_service_ops panelav_ops[] = {
	{ .name = "dcpav-controller-epic", .reusable = true, .init = panelav_controller_init },
	{ .name = "dcpdp-controller-epic", .reusable = true, .init = panelav_init },
	{ .name = "dcpav-power-epic", .reusable = true, .init = panelav_init },
	{
		.name = "dcpav-sac-epic",
		.reusable = true,
		.init = panelav_sac_init,
		.call_v2 = panelav_sac_call_v2,
	},
	{ .name = "dcpav-device-epic", .reusable = true, .init = panelav_init },
	{ .name = "dcpdp-device-epic", .reusable = true, .init = panelav_device_init },
	{ .name = "dcpav-service-epic", .reusable = true, .init = panelav_init },
	{ .name = "dcpdp-service-epic", .reusable = true, .init = panelav_init },
	{
		.name = "dcpav-video-interface-epic",
		.reusable = true,
		.open_later = true,
		.init = panelav_video_init,
	},
	{ .name = "dcpdptx-hdcp-interface", .reusable = true, .init = panelav_hdcp_init },
	{},
};

/* The descriptor manager announces no EPIC keys: it is matched by its name. */
static const struct apple_epic_service_ops panelav_md_ops[] = {
	{ .name = "md-manager", .reusable = true, .init = panelav_init },
	{},
};

static void panelav_destroy_wq(void *data)
{
	destroy_workqueue(data);
}

/* Started after IOMFB; a failure leaves the panel as it is. */
int panelavep_init(struct apple_dcp *dcp)
{
	struct apple_dcp_afkep *eps[ARRAY_SIZE(panelav_endpoints)] = {};
	unsigned int i, started = 0;
	int ret;

	for (i = 0; i < ARRAY_SIZE(panelav_endpoints); i++) {
		u8 id = panelav_endpoints[i];
		struct apple_dcp_afkep *ep;

		if (!apple_rtkit_has_endpoint(dcp->rtk, id))
			continue;
		ep = afk_init(dcp, id, id == 0x31 ? panelav_md_ops : panelav_ops);
		if (IS_ERR(ep))
			return PTR_ERR(ep);
		ep->match_epic_name = id != 0x31;
		ep->inline_v2 = true;
		eps[i] = ep;
	}

	/* Destroyed before the endpoints: a pending call ends first. */
	/* Not ordered: the video interface's OPEN must not wait behind a call. */
	dcp->panelav_wq = alloc_workqueue("apple-dcp-panelav", WQ_UNBOUND, 0);
	if (!dcp->panelav_wq)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dcp->dev, panelav_destroy_wq, dcp->panelav_wq);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(panelav_endpoints); i++) {
		if (!eps[i])
			continue;
		WRITE_ONCE(dcp->panelav[panelav_endpoints[i] - 0x20], eps[i]);
		ret = afk_start(eps[i]);
		if (ret) {
			dev_warn(dcp->dev, "panel AV endpoint %#04x did not start: %d\n",
				 panelav_endpoints[i], ret);
			continue;
		}
		started |= BIT(i);
	}
	dev_info(dcp->dev, "panel AV and DP service endpoints started: %u of %zu\n",
		 hweight32(started), ARRAY_SIZE(panelav_endpoints));
	return 0;
}
