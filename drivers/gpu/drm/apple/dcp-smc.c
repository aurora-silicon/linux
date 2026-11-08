// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Host end of the firmware-declared DCP-to-SMC AFK route. */

#include <kunit/static_stub.h>

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/mfd/macsmc.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/unaligned.h>

#include "afk.h"
#include "dcp-internal.h"
#include "dcp-smc.h"

#define DCP_SMC_MSG_READ_KEY	0x10
#define DCP_SMC_DATA		GENMASK_ULL(63, 32)
#define DCP_SMC_SIZE		GENMASK_ULL(23, 16)
#define DCP_SMC_ID		GENMASK_ULL(15, 12)
#define DCP_SMC_MSG		GENMASK_ULL(7, 0)
#define DCP_SMC_RESULT		DCP_SMC_MSG
#define DCP_SMC_RESULT_FAILED	1
#define DCP_SMC_INLINE_MAX	4
#define DCP_SMC_OPEN_TIMEOUT_MS	500

#define DCP_SMC_ROUTE		"dcp-2-smc"

static int dcp_smc_read_key(struct apple_smc *smc, smc_key key,
			    void *data, size_t size)
{
	if (IS_ENABLED(CONFIG_DRM_APPLE_KUNIT_TEST))
		KUNIT_STATIC_STUB_REDIRECT(dcp_smc_read_key, smc, key, data, size);

	return apple_smc_read(smc, key, data, size);
}

static int dcp_smc_handle_message(struct apple_smc *smc, u64 request,
				  u64 *response)
{
	u32 value = 0;
	u8 size = FIELD_GET(DCP_SMC_SIZE, request);
	u8 id = FIELD_GET(DCP_SMC_ID, request);
	smc_key key = FIELD_GET(DCP_SMC_DATA, request);
	int ret;

	if (FIELD_GET(DCP_SMC_MSG, request) != DCP_SMC_MSG_READ_KEY)
		return -EOPNOTSUPP;
	if (!size || size > DCP_SMC_INLINE_MAX)
		return -EMSGSIZE;

	ret = dcp_smc_read_key(smc, key, &value, size);
	if (ret != size) {
		*response = FIELD_PREP(DCP_SMC_ID, id) |
			FIELD_PREP(DCP_SMC_RESULT, DCP_SMC_RESULT_FAILED);
		return 0;
	}

	*response = FIELD_PREP(DCP_SMC_DATA, value) |
		FIELD_PREP(DCP_SMC_SIZE, size) |
		FIELD_PREP(DCP_SMC_ID, id);
	return 0;
}

static int dcp_smc_afk_call(struct apple_epic_service *service, u32 command,
			    const void *data, size_t data_size,
			    void *reply, size_t reply_size)
{
	struct apple_dcp *dcp = service->ep->dcp;
	u64 response;
	int ret;

	/* The AFK command is only an envelope; the native SMC message is data. */
	(void)command;
	if (data_size != sizeof(u64) || reply_size < sizeof(u64))
		return -EMSGSIZE;

	ret = dcp_smc_handle_message(dcp->desktop->smc,
				     get_unaligned_le64(data), &response);
	if (ret)
		return ret;
	put_unaligned_le64(response, reply);
	return 0;
}

static const struct apple_epic_service_ops dcp_smc_service_ops[] = {
	{
		.name = "afksmc-role-manager",
		.call = dcp_smc_afk_call,
	},
	{
		.name = "afksmc-service-ep",
		.call = dcp_smc_afk_call,
	},
	{
		.name = "afksmc-service",
		.call = dcp_smc_afk_call,
	},
};

static bool dcp_smc_afk_supported(struct apple_dcp *dcp)
{
	return dcp->fw_compat == DCP_FIRMWARE_V_26_6_DESKTOP &&
		!of_device_is_compatible(dcp->dev->of_node, "apple,t6041-dcpext") &&
		of_machine_is_compatible("apple,j616s");
}

int dcp_smc_afk_probe(struct apple_dcp *dcp)
{
	struct platform_device *smc_pdev;
	struct device_node *smc_np;
	struct device_link *link;
	struct apple_smc *smc;

	if (!dcp_smc_afk_supported(dcp))
		return 0;

	smc_np = of_find_compatible_node(NULL, NULL, "apple,t6041-smc");
	if (!smc_np)
		return dev_err_probe(dcp->dev, -ENODEV,
				     "DCP-to-SMC route has no SMC device\n");
	smc_pdev = of_find_device_by_node(smc_np);
	of_node_put(smc_np);
	if (!smc_pdev)
		return dev_err_probe(dcp->dev, -EPROBE_DEFER,
				     "waiting for the SMC device\n");

	smc = platform_get_drvdata(smc_pdev);
	if (!smc) {
		put_device(&smc_pdev->dev);
		return dev_err_probe(dcp->dev, -EPROBE_DEFER,
				     "waiting for the macsmc driver\n");
	}
	link = device_link_add(dcp->dev, &smc_pdev->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(&smc_pdev->dev);
	if (!link)
		return -ENOMEM;

	dcp->desktop->smc = smc;
	return 0;
}

int dcp_smc_afk_publish(struct apple_dcp *dcp)
{
	char name[32];
	int i, ret, first_error = 0;

	if (!dcp_smc_afk_supported(dcp))
		return 0;
	if (!dcp->desktop->smc || IS_ERR_OR_NULL(dcp->systemep))
		return -ENODEV;

	for (i = 0; i < ARRAY_SIZE(dcp_smc_service_ops); i++) {
		snprintf(name, sizeof(name), "%s:%s", DCP_SMC_ROUTE,
			 dcp_smc_service_ops[i].name);
		dcp->desktop->smc_services[i] =
			afk_publish_service(dcp->systemep, name,
					    &dcp_smc_service_ops[i]);
		if (IS_ERR(dcp->desktop->smc_services[i])) {
			ret = PTR_ERR(dcp->desktop->smc_services[i]);
			dcp->desktop->smc_services[i] = NULL;
			if (!first_error)
				first_error = ret;
		}
	}

	/* Publish the complete route before waiting for its individual opens. */
	for (i = 0; i < ARRAY_SIZE(dcp_smc_service_ops); i++) {
		if (!dcp->desktop->smc_services[i])
			continue;
		ret = afk_wait_service_open(dcp->desktop->smc_services[i],
					    DCP_SMC_OPEN_TIMEOUT_MS);
		if (ret && !first_error)
			first_error = ret;
	}

	if (first_error)
		dev_warn(dcp->dev,
			 "DCP-to-SMC AFK route was not fully opened: %d; panel temperature will use the firmware fallback\n",
			 first_error);
	else
		dev_info(dcp->dev, "DCP-to-SMC AFK route is ready\n");
	return first_error;
}

#if IS_ENABLED(CONFIG_DRM_APPLE_KUNIT_TEST)
#include "dcp-smc-test.c"
#endif
