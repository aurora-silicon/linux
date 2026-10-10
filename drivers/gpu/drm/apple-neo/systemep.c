// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Sven Peter <sven@svenpeter.dev> */

#include <linux/completion.h>

#include "afk.h"
#include "dcp.h"
#include "parser.h"

static bool enable_verbose_logging;
module_param(enable_verbose_logging, bool, 0644);
MODULE_PARM_DESC(enable_verbose_logging, "Enable DCP firmware verbose logging");

/*
 * Serialized setProperty("gAFKConfigLogMask", 0xffff) IPC call which
 * will set the DCP firmware log level to the most verbose setting
 */
#define SYSTEM_SET_PROPERTY 0x43
static const u8 setprop_gAFKConfigLogMask_ffff[] = {
	0x14, 0x00, 0x00, 0x00, 0x67, 0x41, 0x46, 0x4b, 0x43, 0x6f,
	0x6e, 0x66, 0x69, 0x67, 0x4c, 0x6f, 0x67, 0x4d, 0x61, 0x73,
	0x6b, 0x00, 0x00, 0x00, 0xd3, 0x00, 0x00, 0x00, 0x40, 0x00,
	0x00, 0x84, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

struct neo_systemep_work {
	struct neo_apple_epic_service *service;
	struct work_struct work;
};

static void system_log_work(struct work_struct *work_)
{
	struct neo_systemep_work *work =
		container_of(work_, struct neo_systemep_work, work);

	neo_afk_send_command(work->service, SYSTEM_SET_PROPERTY,
			 setprop_gAFKConfigLogMask_ffff,
			 sizeof(setprop_gAFKConfigLogMask_ffff), NULL,
			 sizeof(setprop_gAFKConfigLogMask_ffff), NULL);
	complete(&work->service->ep->neo_dcp->neo_systemep_done);
	kfree(work);
}

static void system_init(struct neo_apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
	struct neo_systemep_work *work;

	if (!enable_verbose_logging)
		return;

	/*
	 * We're called from the service message handler thread and can't
	 * dispatch blocking message from there.
	 */
	work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (!work)
		return;

	work->service = service;
	INIT_WORK(&work->work, system_log_work);
	schedule_work(&work->work);
}

static void powerlog_init(struct neo_apple_epic_service *service, const char *name,
			  const char *class, s64 unit)
{
}

static int powerlog_report(struct neo_apple_epic_service *service, enum neo_epic_subtype type,
			 const void *data, size_t data_size)
{
	struct neo_dcp_system_ev_mnits mnits;
	struct neo_dcp_parse_ctx neo_parse_ctx;
	struct neo_apple_dcp *neo_dcp = service->ep->neo_dcp;
	int ret;

	/* Later reports must not overwrite a latched hint or Linux's target. */
	if (neo_dcp_backlight_active(neo_dcp))
		return 0;

	dev_dbg(neo_dcp->dev, "systemep[ch:%u]: report type:%02x len:%zu\n",
		service->channel, type, data_size);

	if (type != EPIC_SUBTYPE_STD_SERVICE)
		return 0;

	ret = neo_parse(data, data_size, &neo_parse_ctx);
	if (ret) {
		dev_warn(service->ep->neo_dcp->dev, "systemep: failed to parse report: %d\n", ret);
		return ret;
	}

	ret = neo_parse_system_log_mnits(&neo_parse_ctx, &mnits);
	if (ret) {
		/* ignore parse errors in the case dcp sends unknown log events */
		dev_dbg(neo_dcp->dev, "systemep: failed to parse mNits event: %d\n", ret);
		return 0;
	}

	dev_dbg(neo_dcp->dev, "systemep: mNits event: Nits: %u.%03u, iDAC: %u\n",
		mnits.millinits / 1000, mnits.millinits % 1000, mnits.idac);
	if (neo_dcp->fw_compat == DCP_FIRMWARE_H17P &&
	    neo_dcp->hw.neo_iomfb_method_profile != DCP_IOMFB_METHODS_H17G) {
		if (!neo_dcp_has_panel(neo_dcp))
			return 0;
		ret = neo_dcp_backlight_takeover(neo_dcp, mnits.millinits);
		if (ret && ret != -EBUSY)
			dev_dbg(neo_dcp->dev, "backlight takeover hint unavailable: %d\n", ret);
		return 0;
	}

	neo_dcp->brightness.nits = mnits.millinits / 1000;

	return 0;
}

static const struct neo_apple_epic_service_ops neo_systemep_ops[] = {
	{
		.name = "system",
		.init = system_init,
	},
	{
		.name = "powerlog-service",
		.init = powerlog_init,
		.report = powerlog_report,
	},
	{}
};

int neo_systemep_init(struct neo_apple_dcp *neo_dcp)
{
	init_completion(&neo_dcp->neo_systemep_done);

	neo_dcp->neo_systemep = neo_afk_init(neo_dcp, SYSTEM_ENDPOINT, neo_systemep_ops);
	neo_afk_start(neo_dcp->neo_systemep);

	if (!enable_verbose_logging)
		return 0;

	/*
	 * Timeouts aren't really fatal here: in the worst case we just weren't
	 * able to enable additional debug prints inside DCP
	 */
	if (!wait_for_completion_timeout(&neo_dcp->neo_systemep_done,
					 msecs_to_jiffies(MSEC_PER_SEC)))
		dev_err(neo_dcp->dev, "systemep: couldn't enable verbose logs\n");

	return 0;
}
