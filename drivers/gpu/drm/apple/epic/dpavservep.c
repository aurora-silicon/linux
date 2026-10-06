// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

#include "dpavservep.h"
#include "dpavservep-edid.h"

#include <drm/drm_edid.h>

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/types.h>

#include "../afk.h"
#include "../dcp.h"
#include "../dcp-internal.h"
#include "../trace.h"

static void dcpavserv_init(struct apple_epic_service *service, const char *name,
			  const char *class, s64 unit)
{
	struct apple_dcp *dcp = service->ep->dcp;
	unsigned long flags;

	trace_dcpavserv_init(dcp, unit);

	if (unit == 0 && name && !strcmp(name, "dcpav-service-epic")) {
		spin_lock_irqsave(&dcp->dcpavserv.lock, flags);
		if (dcp->dcpavserv.enabled) {
			spin_unlock_irqrestore(&dcp->dcpavserv.lock, flags);
			dev_err(dcp->dev,
				"DCPAVSERV: unit %lld already exists\n", unit);
			return;
		}
		dcp->dcpavserv.service = afk_service_get(service);
		dcp->dcpavserv.enabled = true;
		service->cookie = &dcp->dcpavserv;
		complete(&dcp->dcpavserv.enable_completion);
		spin_unlock_irqrestore(&dcp->dcpavserv.lock, flags);
	}
}

static void dcpavserv_teardown(struct apple_epic_service *service)
{
	struct apple_dcp *dcp = service->ep->dcp;
	unsigned long flags;
	bool owned = false;

	spin_lock_irqsave(&dcp->dcpavserv.lock, flags);
	if (dcp->dcpavserv.service == service) {
		dcp->dcpavserv.enabled = false;
		dcp->dcpavserv.service = NULL;
		dcp->dcpavserv.opened = NULL;
		reinit_completion(&dcp->dcpavserv.enable_completion);
		owned = true;
	}
	spin_unlock_irqrestore(&dcp->dcpavserv.lock, flags);
	if (owned)
		afk_service_put(service);
}

void dpavservep_detach(struct apple_dcp *dcp)
{
	struct apple_epic_service *service;
	unsigned long flags;

	spin_lock_irqsave(&dcp->dcpavserv.lock, flags);
	service = dcp->dcpavserv.service;
	dcp->dcpavserv.service = NULL;
	dcp->dcpavserv.enabled = false;
	dcp->dcpavserv.opened = NULL;
	spin_unlock_irqrestore(&dcp->dcpavserv.lock, flags);
	if (service) {
		afk_service_disable(service);
		afk_service_put(service);
	}
}

static void dcpdpserv_init(struct apple_epic_service *service, const char *name,
			  const char *class, s64 unit)
{
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

static const struct drm_edid *dcpavserv_read_edid(struct apple_epic_service *service)
{
	struct dpavserv_copy_edid_cmd cmd;
	struct dpavserv_copy_edid_resp *resp __free(kfree) = NULL;
	size_t resp_size = sizeof(*resp) + DPAVSERV_EDID_BUF_SIZE;
	size_t reply_size, edid_size;
	int ret;

	memset(&cmd, 0, sizeof(cmd));
	cmd.max_size = cpu_to_le64(DPAVSERV_EDID_BUF_SIZE);
	resp = kzalloc(resp_size, GFP_KERNEL);
	if (!resp)
		return ERR_PTR(-ENOMEM);

	ret = afk_service_call_with_reply_len(service, 1, 7, &cmd, sizeof(cmd),
					      DPAVSERV_EDID_BUF_SIZE, resp,
					      resp_size, 0, &reply_size);
	if (ret < 0)
		return ERR_PTR(ret);

	ret = dcpavserv_edid_size(resp, reply_size, &edid_size);
	if (ret)
		return ERR_PTR(ret);

	return drm_edid_alloc(resp->data + DPAVSERV_EDID_LEADING_SIZE, edid_size);
}

const struct drm_edid *dcpavserv_copy_edid(struct apple_dcp *dcp)
{
	struct apple_epic_service *service;
	const struct drm_edid *edid;
	unsigned long flags;

	spin_lock_irqsave(&dcp->dcpavserv.lock, flags);
	service = afk_service_get(dcp->dcpavserv.service);
	spin_unlock_irqrestore(&dcp->dcpavserv.lock, flags);
	if (!service)
		return ERR_PTR(-ENODEV);

	/*
	 * The 14.7 firmware of an external processor answers EDID requests
	 * once its service has been opened; open each announced instance once.
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

	edid = dcpavserv_read_edid(service);
	spin_lock_irqsave(&dcp->dcpavserv.lock, flags);
	if (dcp->dcpavserv.service != service) {
		if (!IS_ERR(edid))
			drm_edid_free(edid);
		edid = ERR_PTR(-ENODEV);
	}
	spin_unlock_irqrestore(&dcp->dcpavserv.lock, flags);
	afk_service_put(service);
	return edid;
}

static const struct apple_epic_service_ops dpavservep_ops[] = {
	{
		.name = "dcpav-service-epic",
		.reusable = true,
		.init = dcpavserv_init,
		.teardown = dcpavserv_teardown,
		.report = dcpavserv_report,
	},
	{
		.name = "dcpdp-service-epic",
		.reusable = true,
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

	ep = afk_init(dcp, DPAVSERV_ENDPOINT, dpavservep_ops);
	if (IS_ERR(ep))
		return PTR_ERR(ep);
	dcp->dcpavservep = ep;

	dcp->dcpavservep->match_epic_name = true;

	ret = afk_start(dcp->dcpavservep);
	if (ret) {
		afk_shutdown(ep);
		dpavservep_detach(dcp);
		dcp->dcpavservep = NULL;
		return ret;
	}

	ret = wait_for_completion_timeout(&dcp->dcpavserv.enable_completion,
					  msecs_to_jiffies(1000));
	if (ret >= 0)
		return 0;

	return ret;
}
