// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

#include "dpavservep.h"

#include <drm/drm_edid.h>

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/types.h>

#include "../afk.h"
#include "../dcp.h"
#include "../dcp-internal.h"
#include "../trace.h"

static void neo_dcpavserv_init(struct neo_apple_epic_service *service, const char *name,
			  const char *class, s64 unit)
{
	struct neo_apple_dcp *neo_dcp = service->ep->neo_dcp;
	unsigned long flags;

	trace_neo_dcpavserv_init(neo_dcp, unit);

	if (unit == 0 && name && !strcmp(name, "dcpav-service-epic")) {
		spin_lock_irqsave(&neo_dcp->neo_dcpavserv.lock, flags);
		if (neo_dcp->neo_dcpavserv.enabled) {
			spin_unlock_irqrestore(&neo_dcp->neo_dcpavserv.lock, flags);
			dev_err(neo_dcp->dev,
				"DCPAVSERV: unit %lld already exists\n", unit);
			return;
		}
		neo_dcp->neo_dcpavserv.service = neo_afk_service_get(service);
		neo_dcp->neo_dcpavserv.enabled = true;
		service->cookie = &neo_dcp->neo_dcpavserv;
		complete(&neo_dcp->neo_dcpavserv.enable_completion);
		spin_unlock_irqrestore(&neo_dcp->neo_dcpavserv.lock, flags);
	}
}

static void neo_dcpavserv_teardown(struct neo_apple_epic_service *service)
{
	struct neo_apple_dcp *neo_dcp = service->ep->neo_dcp;
	unsigned long flags;
	bool owned = false;

	spin_lock_irqsave(&neo_dcp->neo_dcpavserv.lock, flags);
	if (neo_dcp->neo_dcpavserv.service == service) {
		neo_dcp->neo_dcpavserv.enabled = false;
		neo_dcp->neo_dcpavserv.service = NULL;
		reinit_completion(&neo_dcp->neo_dcpavserv.enable_completion);
		owned = true;
	}
	spin_unlock_irqrestore(&neo_dcp->neo_dcpavserv.lock, flags);
	if (owned)
		neo_afk_service_put(service);
}

void dpavservep_detach(struct neo_apple_dcp *neo_dcp)
{
	struct neo_apple_epic_service *service;
	unsigned long flags;

	spin_lock_irqsave(&neo_dcp->neo_dcpavserv.lock, flags);
	service = neo_dcp->neo_dcpavserv.service;
	neo_dcp->neo_dcpavserv.service = NULL;
	neo_dcp->neo_dcpavserv.enabled = false;
	spin_unlock_irqrestore(&neo_dcp->neo_dcpavserv.lock, flags);
	if (service) {
		neo_afk_service_disable(service);
		neo_afk_service_put(service);
	}
}

static void dcpdpserv_init(struct neo_apple_epic_service *service, const char *name,
			  const char *class, s64 unit)
{
}

static void dcpdpserv_teardown(struct neo_apple_epic_service *service)
{
	neo_afk_service_disable(service);
}

struct neo_dcpavserv_status_report {
	u32 unk00[4];
	u8 flag0;
	u8 flag1;
	u8 flag2;
	u8 flag3;
	u32 unk14[3];
	u32 status;
	u32 unk24[3];
} __packed;

struct neo_dpavserv_copy_edid_cmd {
	__le64 max_size;
	u8 _pad1[24];
	__le64 used_size;
	u8 _pad2[8];
} __packed;

#define EDID_LEADING_DATA_SIZE		8
#define EDID_BLOCK_SIZE			128
#define EDID_EXT_BLOCK_COUNT_OFFSET	0x7E
#define EDID_MAX_SIZE			SZ_32K
#define EDID_BUF_SIZE			(EDID_LEADING_DATA_SIZE + EDID_MAX_SIZE)

struct neo_dpavserv_copy_edid_resp {
	__le64 max_size;
	u8 _pad1[24];
	__le64 used_size;
	u8 _pad2[8];
	u8 data[];
} __packed;
static_assert(sizeof(struct neo_dpavserv_copy_edid_resp) == 48);

static int neo_parse_report(struct neo_apple_epic_service *service, enum neo_epic_subtype type,
			 const void *data, size_t data_size)
{
#if defined(DEBUG)
	struct neo_apple_dcp *neo_dcp = service->ep->neo_dcp;
	const struct neo_epic_service_call *call;
	const void *payload;
	size_t payload_size;

	dev_dbg(neo_dcp->dev, "dcpavserv[ch:%u]: report type:%02x len:%zu\n",
		service->channel, type, data_size);

	if (type != EPIC_SUBTYPE_STD_SERVICE)
		return 0;

	if (data_size < sizeof(*call))
		return 0;

	call = data;

	if (le32_to_cpu(call->magic) != EPIC_SERVICE_CALL_MAGIC) {
		dev_warn(neo_dcp->dev, "dcpavserv[ch:%u]: report magic 0x%08x != 0x%08x\n",
			service->channel, le32_to_cpu(call->magic), EPIC_SERVICE_CALL_MAGIC);
		return 0;
	}

	payload_size = data_size - sizeof(*call);
	if (payload_size < le32_to_cpu(call->data_len)) {
		dev_warn(neo_dcp->dev, "dcpavserv[ch:%u]: report payload size %zu call len %u\n",
			service->channel, payload_size, le32_to_cpu(call->data_len));
		return 0;
	}
	payload_size = le32_to_cpu(call->data_len);
	payload = data + sizeof(*call);

	if (le16_to_cpu(call->group) == 2 && le16_to_cpu(call->command) == 0) {
		if (payload_size == sizeof(struct neo_dcpavserv_status_report)) {
			const struct neo_dcpavserv_status_report *stat = payload;
			dev_info(neo_dcp->dev, "dcpavserv[ch:%u]: flags: 0x%02x,0x%02x,0x%02x,0x%02x status:%u\n",
				service->channel, stat->flag0, stat->flag1,
				stat->flag2, stat->flag3, stat->status);
		} else {
			dev_dbg(neo_dcp->dev, "dcpavserv[ch:%u]: report payload size %zu\n", service->channel, payload_size);
		}
	} else {
		print_hex_dump(KERN_DEBUG, "dcpavserv report: ", DUMP_PREFIX_NONE,
			       16, 1, payload, payload_size, true);
	}
#endif

	return 0;
}

static int neo_dcpavserv_report(struct neo_apple_epic_service *service,
			    enum neo_epic_subtype type, const void *data,
			    size_t data_size)
{
	return neo_parse_report(service, type, data, data_size);
}

static int dcpdpserv_report(struct neo_apple_epic_service *service,
			    enum neo_epic_subtype type, const void *data,
			    size_t data_size)
{
	return neo_parse_report(service, type, data, data_size);
}

static const struct drm_edid *neo_dcpavserv_read_edid(struct neo_apple_epic_service *service)
{
	struct neo_dpavserv_copy_edid_cmd cmd;
	struct neo_dpavserv_copy_edid_resp *resp __free(kfree) = NULL;
	size_t resp_size = sizeof(*resp) + EDID_BUF_SIZE;
	int num_blocks;
	u64 data_size;
	int ret;

	memset(&cmd, 0, sizeof(cmd));
	cmd.max_size = cpu_to_le64(EDID_BUF_SIZE);
	resp = kzalloc(resp_size, GFP_KERNEL);
	if (!resp)
		return ERR_PTR(-ENOMEM);

	ret = neo_afk_service_call(service, 1, 7, &cmd, sizeof(cmd), EDID_BUF_SIZE, resp,
			       resp_size, 0);
	if (ret < 0)
		return ERR_PTR(ret);

	if (le64_to_cpu(resp->max_size) != EDID_BUF_SIZE)
		return ERR_PTR(-EIO);

	// print_hex_dump(KERN_DEBUG, "dpavserv EDID cmd: ", DUMP_PREFIX_NONE,
	// 	       16, 1, resp, 192, true);

	data_size = le64_to_cpu(resp->used_size);
	if (data_size < EDID_LEADING_DATA_SIZE + EDID_BLOCK_SIZE ||
	    data_size > EDID_BUF_SIZE)
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

const struct drm_edid *neo_dcpavserv_copy_edid(struct neo_apple_dcp *neo_dcp)
{
	struct neo_apple_epic_service *service;
	const struct drm_edid *edid;
	unsigned long flags;

	spin_lock_irqsave(&neo_dcp->neo_dcpavserv.lock, flags);
	service = neo_afk_service_get(neo_dcp->neo_dcpavserv.service);
	spin_unlock_irqrestore(&neo_dcp->neo_dcpavserv.lock, flags);
	if (!service)
		return ERR_PTR(-ENODEV);

	edid = neo_dcpavserv_read_edid(service);
	spin_lock_irqsave(&neo_dcp->neo_dcpavserv.lock, flags);
	if (neo_dcp->neo_dcpavserv.service != service) {
		if (!IS_ERR(edid))
			drm_edid_free(edid);
		edid = ERR_PTR(-ENODEV);
	}
	spin_unlock_irqrestore(&neo_dcp->neo_dcpavserv.lock, flags);
	neo_afk_service_put(service);
	return edid;
}

static const struct neo_apple_epic_service_ops dpavservep_ops[] = {
	{
		.name = "dcpav-service-epic",
		.reusable = true,
		.init = neo_dcpavserv_init,
		.teardown = neo_dcpavserv_teardown,
		.report = neo_dcpavserv_report,
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

int dpavservep_init(struct neo_apple_dcp *neo_dcp)
{
	struct neo_apple_dcp_afkep *ep;
	int ret;

	init_completion(&neo_dcp->neo_dcpavserv.enable_completion);

	ep = neo_afk_init(neo_dcp, DPAVSERV_ENDPOINT, dpavservep_ops);
	if (IS_ERR(ep))
		return PTR_ERR(ep);
	neo_dcp->dcpavservep = ep;

	neo_dcp->dcpavservep->match_epic_name = true;

	ret = neo_afk_start(neo_dcp->dcpavservep);
	if (ret) {
		neo_afk_shutdown(ep);
		dpavservep_detach(neo_dcp);
		neo_dcp->dcpavservep = NULL;
		return ret;
	}

	ret = wait_for_completion_timeout(&neo_dcp->neo_dcpavserv.enable_completion,
					  msecs_to_jiffies(1000));
	if (ret >= 0)
		return 0;

	return ret;
}
