// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

#include "dpavservep.h"
#include "dpavserv-edid.h"

#define EDID_EXT_BLOCK_COUNT_OFFSET 0x7E

#include <drm/drm_edid.h>

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/types.h>

#include "../afk.h"
#include "../dcp.h"
#include "../dcp-internal.h"
#include "../connector.h"
#include "../trace.h"

static void dcpavserv_notify_edid(struct apple_epic_service *service)
{
	struct apple_dcp *dcp = service->ep->dcp;
	struct apple_connector *connector = READ_ONCE(dcp->connector);

	if (!service->ep->wire_v2 || READ_ONCE(dcp->quiescing) || !connector ||
	    !READ_ONCE(dcp->nr_modes) || !READ_ONCE(connector->connected) ||
	    READ_ONCE(connector->base.registration_state) != DRM_CONNECTOR_REGISTERED ||
	    READ_ONCE(connector->dcp) != to_platform_device(dcp->dev))
		return;
	/* get_modes may already have run before this service was announced. */
	schedule_work(&connector->hotplug_wq);
}

static bool dcpavserv_identity(struct apple_epic_service *service, const char *name,
			       const char *class, s64 unit)
{
	if (!name || strcmp(name, "dcpav-service-epic"))
		return false;
	if (!service->ep->wire_v2)
		return unit == 0;
	/* H17P's lower-case interface-id is dynamic, not a physical-port unit. */
	return service->ep->endpoint == DPAVSERV_ENDPOINT_V2 && class &&
		(!strcmp(class, "AppleDCPDP2HDMI") ||
		 !strcmp(class, "DCPDP13Service")) && service->channel &&
		service->channel <= U16_MAX && unit == service->channel;
}

static bool dcpavserv_admission_current(struct apple_dcp *dcp, u64 generation)
{
	lockdep_assert_held(&dcp->hpd_mutex);
	/* Initial service discovery may precede the first Type-C attachment. */
	return !READ_ONCE(dcp->quiescing) && !READ_ONCE(dcp->external_physically_absent) &&
		generation == dcp->typec_generation &&
		(!generation || dcp->typec_cable_connected);
}

/* Admission runs before AFKV2 OPEN and before readers can pin the service. */
static int dcpavserv_prepare(struct apple_epic_service *service, const char *name,
			    const char *class, s64 unit)
{
	struct apple_dcp *dcp = service->ep->dcp;
	bool v2 = service->ep->wire_v2;
	int ret = 0;

	if (!dcpavserv_identity(service, name, class, unit))
		return -ENODEV;
	if (v2)
		mutex_lock(&dcp->hpd_mutex);
	mutex_lock(&dcp->dcpavserv.lock);
	if (dcp->dcpavserv.enabled)
		ret = -EBUSY;
	else if (v2 && !dcpavserv_admission_current(dcp, dcp->typec_generation))
		ret = -ENODEV;
	else {
		service->admission.generation = v2 ? dcp->typec_generation : 0;
		service->admission.epoch = dcp->dcpavserv.epoch;
	}
	mutex_unlock(&dcp->dcpavserv.lock);
	if (v2)
		mutex_unlock(&dcp->hpd_mutex);
	return ret;
}

static int dcpavserv_revalidate(struct apple_epic_service *service, const char *name,
				const char *class, s64 unit)
{
	struct apple_dcp *dcp = service->ep->dcp;
	int ret = 0;

	if (!dcpavserv_identity(service, name, class, unit))
		return -ENODEV;
	mutex_lock(&dcp->hpd_mutex);
	mutex_lock(&dcp->dcpavserv.lock);
	if (dcp->dcpavserv.enabled)
		ret = -EBUSY;
	else if (service->admission.epoch != dcp->dcpavserv.epoch ||
		 !dcpavserv_admission_current(dcp, service->admission.generation))
		ret = -ENODEV;
	mutex_unlock(&dcp->dcpavserv.lock);
	mutex_unlock(&dcp->hpd_mutex);
	return ret;
}

static void dcpavserv_init(struct apple_epic_service *service, const char *name,
			  const char *class, s64 unit)
{
	struct apple_dcp *dcp = service->ep->dcp;
	bool v2 = service->ep->wire_v2;
	bool published = false;

	trace_dcpavserv_init(dcp, unit);
	if (v2)
		mutex_lock(&dcp->hpd_mutex);
	mutex_lock(&dcp->dcpavserv.lock);
	if (dcpavserv_identity(service, name, class, unit) &&
	    !dcp->dcpavserv.enabled &&
	    service->admission.epoch == dcp->dcpavserv.epoch &&
	    (!v2 || dcpavserv_admission_current(dcp, service->admission.generation))) {
		dcp->dcpavserv.service = afk_service_get(service);
		if (dcp->dcpavserv.service) {
			dcp->dcpavserv.enabled = true;
			dcp->dcpavserv.epoch++;
			service->cookie = &dcp->dcpavserv;
			complete(&dcp->dcpavserv.enable_completion);
			published = true;
		}
	}
	mutex_unlock(&dcp->dcpavserv.lock);
	if (v2)
		mutex_unlock(&dcp->hpd_mutex);
	if (!published) {
		/* A generation/registry change during OPEN cannot enable stale work. */
		afk_service_retire(service);
		return;
	}
	if (v2) {
		dev_dbg(dcp->dev, "DCPAVSERV: AFKV2 EDID service ready on channel %u\n",
			service->channel);
		dcpavserv_notify_edid(service);
	} else {
		/* Also report announcements arriving after the discovery window. */
		dev_dbg(dcp->dev, "DCPAVSERV: legacy EDID service published\n");
	}
}

/* Defer DRM cleanup: AFK callbacks must not take mode_config.mutex. */
static void dcpavserv_invalidate_edid(struct apple_dcp *dcp)
{
	struct apple_connector *connector = READ_ONCE(dcp->connector);

	lockdep_assert_held(&dcp->dcpavserv.lock);
	if (!connector || READ_ONCE(connector->dcp) != to_platform_device(dcp->dev))
		return;
	WRITE_ONCE(connector->edid_invalidation_pending, true);
	if (!READ_ONCE(dcp->quiescing) &&
	    READ_ONCE(connector->base.registration_state) == DRM_CONNECTOR_REGISTERED)
		schedule_work(&connector->hotplug_wq);
}

static void dcpavserv_teardown(struct apple_epic_service *service)
{
	struct apple_dcp *dcp = service->ep->dcp;
	bool owned = false;

	mutex_lock(&dcp->dcpavserv.lock);
	if (dcp->dcpavserv.service == service) {
		dcp->dcpavserv.enabled = false;
		dcp->dcpavserv.service = NULL;
		dcp->dcpavserv.epoch++;
		dcp->dcpavserv.opened = NULL;
		dcpavserv_invalidate_edid(dcp);
		reinit_completion(&dcp->dcpavserv.enable_completion);
		owned = true;
	}
	mutex_unlock(&dcp->dcpavserv.lock);
	if (owned)
		afk_service_put(service);
}

void dpavservep_detach(struct apple_dcp *dcp)
{
	struct apple_epic_service *service;

	mutex_lock(&dcp->dcpavserv.lock);
	service = dcp->dcpavserv.service;
	dcp->dcpavserv.service = NULL;
	dcp->dcpavserv.enabled = false;
	/* Also reject an in-flight prepare/OPEN with no published owner yet. */
	dcp->dcpavserv.epoch++;
	dcp->dcpavserv.opened = NULL;
	reinit_completion(&dcp->dcpavserv.enable_completion);
	if (service) {
		afk_service_retire(service);
		dcpavserv_invalidate_edid(dcp);
	}
	mutex_unlock(&dcp->dcpavserv.lock);
	if (service)
		afk_service_put(service);
}

static int dcpdpserv_prepare(struct apple_epic_service *service, const char *name,
			    const char *class, s64 unit)
{
	/* No production command transport is needed for this H17P service. */
	return service->ep->wire_v2 ? -EOPNOTSUPP : 0;
}

static void dcpdpserv_init(struct apple_epic_service *service, const char *name,
			  const char *class, s64 unit)
{
	/* Compact EPIC (desktop 26.x): the firmware's own client never opens it. */
	if (READ_ONCE(service->ep->compact))
		service->defer_start = true;
}

static void dcpdpserv_teardown(struct apple_epic_service *service)
{
	afk_service_disable(service);
}

struct dcpavserv_status_report {
	u32 unk00[4];
	u8 flag0;
	u8 flag1;
	u8 flag2;
	u8 flag3;
	u32 unk14[3];
	u32 status;
	u32 unk24[3];
} __packed;

struct dpavserv_copy_edid_cmd {
	__le64 max_size;
	u8 _pad1[24];
	__le64 used_size;
	u8 _pad2[8];
} __packed;

struct dpavserv_copy_edid_resp {
	__le64 max_size;
	u8 _pad1[24];
	__le64 used_size;
	u8 _pad2[8];
	u8 data[];
} __packed;
static_assert(sizeof(struct dpavserv_copy_edid_resp) == 48);

static int parse_report(struct apple_epic_service *service, enum epic_subtype type,
			 const void *data, size_t data_size)
{
#if defined(DEBUG)
	struct apple_dcp *dcp = service->ep->dcp;
	const struct epic_service_call *call;
	const void *payload;
	size_t payload_size;

	dev_dbg(dcp->dev, "dcpavserv[ch:%u]: report type:%02x len:%zu\n",
		service->channel, type, data_size);

	if (type != EPIC_SUBTYPE_STD_SERVICE)
		return 0;

	if (data_size < sizeof(*call))
		return 0;

	call = data;

	if (le32_to_cpu(call->magic) != EPIC_SERVICE_CALL_MAGIC) {
		dev_warn(dcp->dev, "dcpavserv[ch:%u]: report magic 0x%08x != 0x%08x\n",
			service->channel, le32_to_cpu(call->magic), EPIC_SERVICE_CALL_MAGIC);
		return 0;
	}

	payload_size = data_size - sizeof(*call);
	if (payload_size < le32_to_cpu(call->data_len)) {
		dev_warn(dcp->dev, "dcpavserv[ch:%u]: report payload size %zu call len %u\n",
			service->channel, payload_size, le32_to_cpu(call->data_len));
		return 0;
	}
	payload_size = le32_to_cpu(call->data_len);
	payload = data + sizeof(*call);

	if (le16_to_cpu(call->group) == 2 && le16_to_cpu(call->command) == 0) {
		if (payload_size == sizeof(struct dcpavserv_status_report)) {
			const struct dcpavserv_status_report *stat = payload;
			dev_info(dcp->dev, "dcpavserv[ch:%u]: flags: 0x%02x,0x%02x,0x%02x,0x%02x status:%u\n",
				service->channel, stat->flag0, stat->flag1,
				stat->flag2, stat->flag3, stat->status);
		} else {
			dev_dbg(dcp->dev, "dcpavserv[ch:%u]: report payload size %zu\n", service->channel, payload_size);
		}
	} else {
		print_hex_dump(KERN_DEBUG, "dcpavserv report: ", DUMP_PREFIX_NONE,
			       16, 1, payload, payload_size, true);
	}
#endif

	return 0;
}

static int dcpavserv_report(struct apple_epic_service *service,
			    enum epic_subtype type, const void *data,
			    size_t data_size)
{
	return parse_report(service, type, data, data_size);
}

static int dcpdpserv_report(struct apple_epic_service *service,
			    enum epic_subtype type, const void *data,
			    size_t data_size)
{
	return parse_report(service, type, data, data_size);
}

/*
 * A compact EPIC call goes inline through the endpoint's ring, which a
 * 32 KiB EDID request can never fit: ask for 2 KiB there, enough for 15
 * extension blocks.
 */
#define EDID_COMPACT_BUF_SIZE (EDID_LEADING_DATA_SIZE + SZ_2K)

static const struct drm_edid *dcpavserv_read_edid_legacy(struct apple_epic_service *service)
{
	struct dpavserv_copy_edid_cmd cmd;
	struct dpavserv_copy_edid_resp *resp __free(kfree) = NULL;
	size_t edid_buf_size = READ_ONCE(service->ep->compact) ? EDID_COMPACT_BUF_SIZE :
						      EDID_BUF_SIZE;
	size_t resp_size = sizeof(*resp) + edid_buf_size;
	size_t reply_size;
	int num_blocks;
	u64 data_size;
	int ret;

	memset(&cmd, 0, sizeof(cmd));
	cmd.max_size = cpu_to_le64(edid_buf_size);
	resp = kzalloc(resp_size, GFP_KERNEL);
	if (!resp)
		return ERR_PTR(-ENOMEM);

	ret = afk_service_call_with_reply_len(service, 1, 7, &cmd, sizeof(cmd),
					      edid_buf_size, resp, resp_size, 0,
					      &reply_size);
	if (ret < 0)
		return ERR_PTR(ret);

	if (reply_size < sizeof(*resp) ||
	    le64_to_cpu(resp->max_size) != edid_buf_size)
		return ERR_PTR(-EIO);

	/* Only complete EDID blocks within the received reply may be copied. */
	data_size = le64_to_cpu(resp->used_size);
	if (data_size < EDID_LEADING_DATA_SIZE + EDID_BLOCK_SIZE ||
	    data_size > edid_buf_size || data_size > reply_size - sizeof(*resp))
		return ERR_PTR(-EIO);

	/*
	 * An HDMI 2.x sink using the HF-EEODB announces fewer extension blocks
	 * in the base block than it sends, so the payload may be longer than
	 * the base block says. Accept any whole number of blocks at least that
	 * long and leave the block count to drm_edid, which knows about
	 * HF-EEODB.
	 */
	num_blocks = resp->data[EDID_LEADING_DATA_SIZE + EDID_EXT_BLOCK_COUNT_OFFSET];
	if ((1 + num_blocks) * EDID_BLOCK_SIZE > data_size - EDID_LEADING_DATA_SIZE ||
	    (data_size - EDID_LEADING_DATA_SIZE) % EDID_BLOCK_SIZE)
		return ERR_PTR(-EIO);

	return drm_edid_alloc(resp->data + EDID_LEADING_DATA_SIZE,
			      data_size - EDID_LEADING_DATA_SIZE);
}

static const struct drm_edid *dcpavserv_read_edid_inline(struct apple_epic_service *service)
{
	struct dpavserv_copy_edid_cmd cmd;
	struct dpavserv_copy_edid_resp *resp __free(kfree) = NULL;
	size_t resp_size = sizeof(*resp) + EDID_BUF_SIZE;
	size_t received, edid_size;
	int ret;

	memset(&cmd, 0, sizeof(cmd));
	cmd.max_size = cpu_to_le64(EDID_BUF_SIZE);
	resp = kzalloc(resp_size, GFP_KERNEL);
	if (!resp)
		return ERR_PTR(-ENOMEM);

	ret = afk_service_call_len(service, 1, 7, &cmd, sizeof(cmd), EDID_BUF_SIZE,
				   resp, resp_size, 0, &received);
	if (ret < 0)
		return ERR_PTR(ret);

	ret = dpavserv_edid_size(resp, received, &edid_size);
	if (ret)
		return ERR_PTR(ret);

	return drm_edid_alloc(resp->data + EDID_LEADING_DATA_SIZE, edid_size);
}

bool dcpavserv_edid_current(struct apple_dcp *dcp, u64 generation)
{
	lockdep_assert_held(&dcp->hpd_mutex);
	return dcp->hw.iomfb_method_profile !=
		DCP_IOMFB_METHODS_H17P_EXTERNAL ||
		(dcp->typec_cable_connected && generation == dcp->typec_generation &&
		 !READ_ONCE(dcp->quiescing) && !READ_ONCE(dcp->external_physically_absent));
}

bool dcpavserv_edid_service_current(struct apple_dcp *dcp, u64 epoch)
{
	lockdep_assert_held(&dcp->dcpavserv.lock);
	return dcp->dcpavserv.enabled && dcp->dcpavserv.service &&
		epoch == dcp->dcpavserv.epoch &&
		(dcp->hw.iomfb_method_profile != DCP_IOMFB_METHODS_H17P_EXTERNAL ||
		 !READ_ONCE(dcp->external_physically_absent));
}

const struct drm_edid *dcpavserv_copy_edid(struct apple_dcp *dcp, u64 generation,
					u64 *epoch)
{
	struct apple_epic_service *service;
	const struct drm_edid *edid;

	scoped_guard(mutex, &dcp->hpd_mutex) {
		if (!dcpavserv_edid_current(dcp, generation))
			return ERR_PTR(-ESTALE);
	}
	mutex_lock(&dcp->dcpavserv.lock);
	service = dcp->dcpavserv.enabled ? afk_service_get(dcp->dcpavserv.service) : NULL;
	*epoch = dcp->dcpavserv.epoch;
	mutex_unlock(&dcp->dcpavserv.lock);
	if (!service)
		return ERR_PTR(-ENODEV);

	/*
	 * The 14.7 firmware of an external processor answers EDID requests
	 * once its service has been opened; open each announced instance once.
	 *
	 * Only on Aurora's DCPLink session (T6030). The T8122 dcpext, which
	 * Linux starts and drives through the IOMFB template, reads the EDID
	 * directly, as in AsahiLinux/linux#607. Its firmware never answers the
	 * open call and drops the display link instead, about 50 ms after it,
	 * which brings the display back 4 s later: hotplug in a loop.
	 */
	if (dcp->fw_compat == DCP_FIRMWARE_V_14_7 &&
	    READ_ONCE(dcp->dcpavserv.opened) != service) {
		int ret = afk_service_call(service, 4, 6, NULL, 0, 32, NULL, 0, 32);

		if (ret) {
			afk_service_put(service);
			return ERR_PTR(ret);
		}
		WRITE_ONCE(dcp->dcpavserv.opened, service);
	}

	edid = service->ep->wire_v2 ? dcpavserv_read_edid_inline(service) :
		dcpavserv_read_edid_legacy(service);
	mutex_lock(&dcp->dcpavserv.lock);
	if (dcp->dcpavserv.service != service ||
	    !dcpavserv_edid_service_current(dcp, *epoch)) {
		if (!IS_ERR(edid))
			drm_edid_free(edid);
		edid = ERR_PTR(-ENODEV);
	}
	mutex_unlock(&dcp->dcpavserv.lock);
	afk_service_put(service);
	scoped_guard(mutex, &dcp->hpd_mutex) {
		if (!dcpavserv_edid_current(dcp, generation)) {
			if (!IS_ERR(edid))
				drm_edid_free(edid);
			edid = ERR_PTR(-ESTALE);
		}
	}
	return edid;
}

static const struct apple_epic_service_ops dpavservep_ops[] = {
	{
		.name = "dcpav-service-epic",
		.reusable = true,
		.prepare = dcpavserv_prepare,
		.revalidate = dcpavserv_revalidate,
		.init = dcpavserv_init,
		.teardown = dcpavserv_teardown,
		.report = dcpavserv_report,
	},
	{
		.name = "dcpdp-service-epic",
		.reusable = true,
		.optional = true,
		.prepare = dcpdpserv_prepare,
		.init = dcpdpserv_init,
		.teardown = dcpdpserv_teardown,
		.report = dcpdpserv_report,
	},
	{},
};

int dpavservep_init(struct apple_dcp *dcp)
{
	struct apple_dcp_afkep *ep;
	int ret;

	init_completion(&dcp->dcpavserv.enable_completion);

	ep = afk_init(dcp, dcp->hw.iomfb_method_profile ==
		      DCP_IOMFB_METHODS_H17P_EXTERNAL ?
		      DPAVSERV_ENDPOINT_V2 : DPAVSERV_ENDPOINT, dpavservep_ops);
	if (IS_ERR(ep))
		return PTR_ERR(ep);
	/* External H17P uses AFKV2 service calls with host-owned OOB buffers. */
	ep->wire_v2 = dcp->hw.iomfb_method_profile ==
		DCP_IOMFB_METHODS_H17P_EXTERNAL;
	ep->inline_v2 = false;
	dcp->dcpavservep = ep;

	dcp->dcpavservep->match_epic_name = true;

	ret = afk_start(dcp->dcpavservep);
	if (ret) {
		afk_shutdown(ep);
		dpavservep_detach(dcp);
		dcp->dcpavservep = NULL;
		return ret;
	}

	/* Admission may arrive after startup; exact guards and late notify own it. */
	if (ep->wire_v2)
		return 0;

	/*
	 * Legacy firmware need not announce an EDID service. Give it the same
	 * discovery window so an early announcement can precede get_modes,
	 * but absence is not an endpoint failure.
	 * Only dcpavserv_init() may publish a service that actually arrived.
	 */
	if (!wait_for_completion_timeout(&dcp->dcpavserv.enable_completion,
					 msecs_to_jiffies(1000)))
		dev_dbg(dcp->dev, "DCPAVSERV: no legacy EDID service within 1 s\n");

	return 0;
}
