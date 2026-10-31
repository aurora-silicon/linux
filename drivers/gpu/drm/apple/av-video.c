// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* HDR10 static link metadata for the desktop 26.x DCPAV service. */
#include <drm/drm_atomic.h>
#include <linux/slab.h>

#include "afk.h"
#include "av-video.h"
#include "connector.h"
#include "dcp-internal.h"

void av_video_init(struct apple_epic_service *service, const char *name,
		   const char *class, s64 unit)
{
	struct apple_dcp *dcp = service->ep->dcp;
	struct apple_epic_service *old;

	if (dcp->fw_compat != DCP_FIRMWARE_V_26_6_DESKTOP ||
	    service->ep->endpoint != AV_ENDPOINT)
		return;
	service = afk_service_get(service);
	if (!service)
		return;
	scoped_guard(spinlock, &dcp->av_video_lock) {
		old = dcp->av_video;
		dcp->av_video = service;
		dcp->av_video_generation++;
	}
	if (old)
		afk_service_put(old);
}

void av_video_teardown(struct apple_epic_service *service)
{
	struct apple_dcp *dcp = service->ep->dcp;
	bool owned;

	if (dcp->fw_compat != DCP_FIRMWARE_V_26_6_DESKTOP ||
	    service->ep->endpoint != AV_ENDPOINT) {
		afk_service_disable(service);
		return;
	}
	scoped_guard(spinlock, &dcp->av_video_lock) {
		owned = dcp->av_video == service;
		if (owned) {
			dcp->av_video = NULL;
			dcp->av_video_generation++;
		}
	}
	if (owned)
		afk_service_put(service);
	afk_service_disable(service);
}

/* Registered after the endpoint allocation, before it can publish services. */
static void av_video_cleanup(void *data)
{
	struct apple_dcp_afkep *ep = data;
	struct apple_dcp *dcp = ep->dcp;
	struct apple_epic_service *service = NULL;

	/* Stop publication before dropping this endpoint's owner pin. */
	afk_quiesce(ep);
	scoped_guard(spinlock, &dcp->av_video_lock) {
		if (dcp->av_video && dcp->av_video->ep == ep) {
			service = dcp->av_video;
			dcp->av_video = NULL;
			dcp->av_video_generation++;
		}
	}
	if (service)
		afk_service_put(service);
}

int av_video_register_cleanup(struct apple_dcp_afkep *ep)
{
	if (ep->dcp->fw_compat != DCP_FIRMWARE_V_26_6_DESKTOP ||
	    ep->endpoint != AV_ENDPOINT)
		return 0;
	return devm_add_action_or_reset(ep->dcp->dev, av_video_cleanup, ep);
}

/* AV is needed even with audio disabled. Leave other services untouched. */
static const struct apple_epic_service_ops av_video_ops[] = {
	{
		.name = "dcpav-device-epic",
		.reusable = true,
		.init = av_video_init,
		.teardown = av_video_teardown,
	},
	{
		.name = "DCPAVSimpleVideoInterface",
		.reusable = true,
		.init = av_video_init,
		.teardown = av_video_teardown,
	},
	{ .name = "*" },
	{},
};

const struct apple_epic_service_ops *av_video_service_ops(void)
{
	return av_video_ops;
}

/* Called only after a successful atomic modeset, never by a frame swap. */
void av_video_atomic_modeset(struct apple_dcp *dcp, struct apple_connector *connector,
			     u64 link_generation, struct drm_crtc *crtc,
			     struct drm_atomic_commit *commit)
{
	const struct drm_connector_state *state;
	struct apple_epic_service *service;
	u8 message[DCP_HDR_MESSAGE_SIZE];
	u8 reply[DCP_HDR_MESSAGE_SIZE] = {};
	u64 service_generation;
	size_t received = 0;
	u32 retcode = 0;
	int ret;
	bool equal, live;

	if (dcp->fw_compat != DCP_FIRMWARE_V_26_6_DESKTOP || dcp_has_panel(dcp) ||
	    !connector || connector != READ_ONCE(dcp->connector) ||
	    READ_ONCE(connector->dcp) != to_platform_device(dcp->dev) ||
	    !READ_ONCE(connector->connected))
		return;
	state = drm_atomic_get_new_connector_state(commit, &connector->base);
	if (!state)
		state = connector->base.state;
	/*
	 * Property removal leaves the last dictionary on a live link. An empty
	 * or zero block is not a clear operation. Live clearing needs separate
	 * protocol research; link teardown already stops the InfoFrame.
	 */
	if (!state || state->crtc != crtc || !state->hdr_output_metadata)
		return;
	if (state->hdr_output_metadata->length != sizeof(struct hdr_output_metadata))
		return;
	ret = dcp_hdr_message(message, state->hdr_output_metadata->data);
	if (ret)
		return;

	scoped_guard(spinlock, &dcp->av_video_lock) {
		service = afk_service_get(dcp->av_video);
		service_generation = dcp->av_video_generation;
	}
	if (!service)
		return;
	scoped_guard(spinlock, &connector->hdr_lock) {
		live = connector->hdr_generation == link_generation;
		equal = connector->hdr_valid && connector->hdr_dcp == dcp &&
			connector->hdr_service_generation == service_generation &&
			!memcmp(connector->hdr_data, message + 0x60 + DCP_HDR_DATA_OFFSET, DCP_HDR_DATA_SIZE);
	}
	if (!live || equal)
		goto out;

	ret = afk_send_command_with_reply_len(service, EPIC_SUBTYPE_STD_SERVICE,
					      message, sizeof(message), reply, sizeof(reply),
					      &retcode, &received);
	ret = dcp_hdr_reply(ret, retcode, reply, received);
	if (ret) {
		dev_warn_ratelimited(dcp->dev, "HDR static metadata failed: %d\n", ret);
		goto out;
	}
	/* A teardown during the command must not validate a successor's cache. */
	scoped_guard(spinlock, &connector->hdr_lock) {
		if (connector->hdr_generation == link_generation) {
			memcpy(connector->hdr_data, message + 0x60 + DCP_HDR_DATA_OFFSET, DCP_HDR_DATA_SIZE);
			connector->hdr_dcp = dcp;
			connector->hdr_service_generation = service_generation;
			connector->hdr_valid = true;
		}
	}
out:
	afk_service_put(service);
}
