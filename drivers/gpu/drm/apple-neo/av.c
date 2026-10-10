// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2023 Martin Povišer <povik+lin@cutebit.org> */

// #define DEBUG

#include <linux/debugfs.h>
#include <linux/kconfig.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "audio.h"
#include "afk.h"
#include "av.h"
#include "dcp.h"
#include "dcp-internal.h"

struct neo_dcp_av_audio_cmds {
	/* commands in group 0*/
	u32 open;
	u32 close;
	u32 prepare;
	u32 start_link;
	u32 stop_link;
	u32 unprepare;
	/* commands in group 1*/
	u32 get_elements;
	u32 get_product_attrs;
};

static const struct neo_dcp_av_audio_cmds neo_dcp_av_audio_cmds_v12_3 = {
	.open = 6,
	.close = 7,
	.prepare = 8,
	.start_link = 9,
	.stop_link = 12,
	.unprepare = 13,
	.get_elements = 18,
	.get_product_attrs = 20,
};

static const struct neo_dcp_av_audio_cmds neo_dcp_av_audio_cmds_v13_5 = {
	.open = 4,
	.close = 5,
	.prepare = 6,
	.start_link = 7,
	.stop_link = 10,
	.unprepare = 11,
	.get_elements = 16,
	.get_product_attrs = 18,
};

struct audiosrv_data {
	struct platform_device *neo_audio_dev;
	bool plugged;
	struct mutex plug_lock;

	struct neo_apple_epic_service *srv;
	struct rw_semaphore srv_rwsem;
	/* Workqueue for starting the audio service */
	struct work_struct start_av_service_wq;

	struct neo_dcp_av_audio_cmds cmds;

	bool warned_get_elements;
	bool warned_get_product_attrs;
	bool is_open;
};

static void neo_av_interface_init(struct neo_apple_epic_service *service, const char *name,
			      const char *class, s64 unit)
{
}

static void neo_av_interface_teardown(struct neo_apple_epic_service *service)
{
	struct neo_apple_dcp *neo_dcp = service->ep->neo_dcp;
	struct audiosrv_data *asrv = neo_dcp->audiosrv;

	neo_afk_service_disable(service);

	mutex_lock(&asrv->plug_lock);

	asrv->plugged = false;
	if (asrv->neo_audio_dev)
		neo_dcpaud_disconnect(asrv->neo_audio_dev);

	mutex_unlock(&asrv->plug_lock);
}

static void neo_av_audiosrv_init(struct neo_apple_epic_service *service, const char *name,
			     const char *class, s64 unit)
{
	struct neo_apple_dcp *neo_dcp = service->ep->neo_dcp;
	struct audiosrv_data *asrv = neo_dcp->audiosrv;

	mutex_lock(&asrv->plug_lock);

	down_write(&asrv->srv_rwsem);
	asrv->srv = service;
	up_write(&asrv->srv_rwsem);

	asrv->plugged = true;
	mutex_unlock(&asrv->plug_lock);
	schedule_work(&asrv->start_av_service_wq);
}

static void neo_av_audiosrv_teardown(struct neo_apple_epic_service *service)
{
	struct neo_apple_dcp *neo_dcp = service->ep->neo_dcp;
	struct audiosrv_data *asrv = neo_dcp->audiosrv;

	mutex_lock(&asrv->plug_lock);

	down_write(&asrv->srv_rwsem);
	asrv->srv = NULL;
	up_write(&asrv->srv_rwsem);

	asrv->plugged = false;
	if (asrv->neo_audio_dev)
		neo_dcpaud_disconnect(asrv->neo_audio_dev);

	mutex_unlock(&asrv->plug_lock);
}

int neo_dcp_audiosrv_prepare(struct device *dev, struct neo_dcp_sound_cookie *cookie)
{
	struct neo_apple_dcp *neo_dcp = dev_get_drvdata(dev);
	struct audiosrv_data *asrv = neo_dcp->audiosrv;
	int ret;

	down_write(&asrv->srv_rwsem);
	ret = neo_afk_service_call(asrv->srv, 0, asrv->cmds.prepare, cookie,
			       sizeof(*cookie), 64 - sizeof(*cookie), NULL, 0,
			       64);
	up_write(&asrv->srv_rwsem);

	return ret;
}

int neo_dcp_audiosrv_startlink(struct device *dev, struct neo_dcp_sound_cookie *cookie)
{
	struct neo_apple_dcp *neo_dcp = dev_get_drvdata(dev);
	struct audiosrv_data *asrv = neo_dcp->audiosrv;
	int ret;

	down_write(&asrv->srv_rwsem);
	ret = neo_afk_service_call(asrv->srv, 0, asrv->cmds.start_link, cookie,
			       sizeof(*cookie), 64 - sizeof(*cookie), NULL, 0,
			       64);
	up_write(&asrv->srv_rwsem);

	return ret;
}

int neo_dcp_audiosrv_stoplink(struct device *dev)
{
	struct neo_apple_dcp *neo_dcp = dev_get_drvdata(dev);
	struct audiosrv_data *asrv = neo_dcp->audiosrv;
	int ret;

	down_write(&asrv->srv_rwsem);
	ret = neo_afk_service_call(asrv->srv, 0, asrv->cmds.stop_link, NULL, 0, 64,
			       NULL, 0, 64);
	up_write(&asrv->srv_rwsem);

	return ret;
}

int neo_dcp_audiosrv_unprepare(struct device *dev)
{
	struct neo_apple_dcp *neo_dcp = dev_get_drvdata(dev);
	struct audiosrv_data *asrv = neo_dcp->audiosrv;
	int ret;

	down_write(&asrv->srv_rwsem);
	ret = neo_afk_service_call(asrv->srv, 0, asrv->cmds.unprepare, NULL, 0, 64,
			       NULL, 0, 64);
	up_write(&asrv->srv_rwsem);

	return ret;
}

static int
neo_dcp_audiosrv_osobject_call(struct neo_apple_epic_service *service, u16 group,
			   u32 command, void *output, size_t output_maxsize,
			   size_t *output_size)
{
	struct {
		__le64 max_size;
		u8 _pad1[24];
		__le64 used_size;
		u8 _pad2[8];
	} __attribute__((packed)) *hdr;
	static_assert(sizeof(*hdr) == 48);
	size_t bfr_len = output_maxsize + sizeof(*hdr);
	void *bfr __free(kfree) = NULL;
	int ret;

	bfr = kzalloc(bfr_len, GFP_KERNEL);
	if (!bfr)
		return -ENOMEM;

	hdr = bfr;
	hdr->max_size = cpu_to_le64(output_maxsize);
	ret = neo_afk_service_call(service, group, command, hdr, sizeof(*hdr), output_maxsize,
			       bfr, sizeof(*hdr) + output_maxsize, 0);
	if (ret)
		return ret;

	if (output)
		memcpy(output, bfr + sizeof(*hdr), output_maxsize);

	if (output_size)
		*output_size = le64_to_cpu(hdr->used_size);

	return 0;
}

int neo_dcp_audiosrv_get_elements(struct device *dev, void *elements, size_t maxsize)
{
	struct neo_apple_dcp *neo_dcp = dev_get_drvdata(dev);
	struct audiosrv_data *asrv = neo_dcp->audiosrv;
	size_t size;
	int ret;

	down_write(&asrv->srv_rwsem);
	ret = neo_dcp_audiosrv_osobject_call(asrv->srv, 1, asrv->cmds.get_elements,
					 elements, maxsize, &size);
	up_write(&asrv->srv_rwsem);

	if (ret && !asrv->warned_get_elements) {
		dev_err(dev, "audiosrv: error getting elements: %d\n", ret);
		asrv->warned_get_elements = true;
	} else if (!ret) {
		dev_dbg(dev, "audiosrv: got %zd bytes worth of elements\n", size);
	}

	return ret;
}

int neo_dcp_audiosrv_get_product_attrs(struct device *dev, void *attrs, size_t maxsize)
{
	struct neo_apple_dcp *neo_dcp = dev_get_drvdata(dev);
	struct audiosrv_data *asrv = neo_dcp->audiosrv;
	size_t size;
	int ret;

	down_write(&asrv->srv_rwsem);
	ret = neo_dcp_audiosrv_osobject_call(asrv->srv, 1,
					 asrv->cmds.get_product_attrs, attrs,
					 maxsize, &size);
	up_write(&asrv->srv_rwsem);

	if (ret && !asrv->warned_get_product_attrs) {
		dev_err(dev, "audiosrv: error getting product attributes: %d\n", ret);
		asrv->warned_get_product_attrs = true;
	} else if (!ret) {
		dev_dbg(dev, "audiosrv: got %zd bytes worth of product attributes\n", size);
	}

	return ret;
}

static int neo_av_audiosrv_report(struct neo_apple_epic_service *service, u32 idx,
						  const void *data, size_t data_size)
{
	dev_dbg(service->ep->neo_dcp->dev, "got audio report %d size %zx\n", idx, data_size);
#ifdef DEBUG
	print_hex_dump(KERN_DEBUG, "audio report: ", DUMP_PREFIX_NONE, 16, 1, data, data_size, true);
#endif

	return 0;
}

static const struct neo_apple_epic_service_ops neo_avep_ops[] = {
	{
		.name = "DCPAVSimpleVideoInterface",
		.init = neo_av_interface_init,
		.teardown = neo_av_interface_teardown,
	},
	{
		.name = "DCPAVAudioInterface",
		.init = neo_av_audiosrv_init,
		.report = neo_av_audiosrv_report,
		.teardown = neo_av_audiosrv_teardown,
	},
	{}
};

void neo_av_service_connect(struct neo_apple_dcp *neo_dcp)
{
	struct neo_apple_epic_service *service;
	struct audiosrv_data *asrv = neo_dcp->audiosrv;
	int ret;

	scoped_guard(rwsem_write, &asrv->srv_rwsem) {
		if (!asrv->srv)
			return;
		service = asrv->srv;
	}

	/* open AV audio service */
	dev_info(neo_dcp->dev, "%s: starting audio service, plugged:%d\n", __func__,  asrv->plugged);
	if (asrv->is_open)
		return;

	ret = neo_afk_service_call(service, 0, asrv->cmds.open, NULL, 0, 32,
			       NULL, 0, 32);
	if (ret) {
		dev_err(neo_dcp->dev, "error opening audio service: %d\n", ret);
		return;
	}
	mutex_lock(&asrv->plug_lock);
	asrv->is_open = true;

	if (asrv->neo_audio_dev)
		neo_dcpaud_connect(asrv->neo_audio_dev, asrv->plugged);
	mutex_unlock(&asrv->plug_lock);
}

void neo_av_service_disconnect(struct neo_apple_dcp *neo_dcp)
{
	struct neo_apple_epic_service *service;
	struct audiosrv_data *asrv = neo_dcp->audiosrv;
	int ret;

	scoped_guard(rwsem_write, &asrv->srv_rwsem) {
		if (!asrv->srv)
			return;
		service = asrv->srv;
	}

	/* close AV audio service */
	dev_info(neo_dcp->dev, "%s: stopping audio service\n", __func__);
	if (!asrv->is_open)
		return;

	mutex_lock(&asrv->plug_lock);

	if (asrv->neo_audio_dev)
		neo_dcpaud_disconnect(asrv->neo_audio_dev);

	mutex_unlock(&asrv->plug_lock);

	ret = neo_afk_service_call(service, 0, asrv->cmds.close, NULL, 0, 16,
			       NULL, 0, 16);
	if (ret) {
		dev_err(neo_dcp->dev, "error closing audio service: %d\n", ret);
	}
	if (service->torndown)
		neo_afk_service_disable(service);
	asrv->is_open = false;
}

static void neo_av_work_service_start(struct work_struct *work)
{
	struct audiosrv_data *audiosrv_data;
	struct neo_apple_dcp *neo_dcp;

	audiosrv_data = container_of(work, struct audiosrv_data, start_av_service_wq);

	scoped_guard(rwsem_read, &audiosrv_data->srv_rwsem) {
		if (!audiosrv_data->srv ||
		!audiosrv_data->srv->ep ||
		!audiosrv_data->srv->ep->neo_dcp) {
			pr_err("%s: dcp: av: NULL ptr during startup\n", __func__);
			return;
		}
		neo_dcp = audiosrv_data->srv->ep->neo_dcp;
	}

	neo_av_service_connect(neo_dcp);
}

int neo_avep_init(struct neo_apple_dcp *neo_dcp)
{
	struct audiosrv_data *audiosrv_data;
	struct platform_device *neo_audio_pdev;
	struct device *dev = neo_dcp->dev;
	struct device_node *endpoint, *neo_audio_node = NULL;

	audiosrv_data = devm_kzalloc(neo_dcp->dev, sizeof(*audiosrv_data), GFP_KERNEL);
	if (!audiosrv_data)
		return -ENOMEM;
	init_rwsem(&audiosrv_data->srv_rwsem);
	mutex_init(&audiosrv_data->plug_lock);

	switch (neo_dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		audiosrv_data->cmds = neo_dcp_av_audio_cmds_v12_3;
		break;
	case DCP_FIRMWARE_V_13_5:
		audiosrv_data->cmds = neo_dcp_av_audio_cmds_v13_5;
		break;
	default:
		dev_err(neo_dcp->dev, "Audio not supported for firmware\n");
		return -ENODEV;
	}

	neo_dcp->audiosrv = audiosrv_data;
	INIT_WORK(&audiosrv_data->start_av_service_wq, neo_av_work_service_start);

	endpoint = of_graph_get_endpoint_by_regs(dev->of_node, 0, 0);
	if (endpoint) {
		neo_audio_node = of_graph_get_remote_port_parent(endpoint);
		of_node_put(endpoint);
	}
	if (!neo_audio_node || !of_device_is_available(neo_audio_node)) {
		of_node_put(neo_audio_node);
		dev_info(dev, "No audio support\n");
		return 0;
	}

	neo_audio_pdev = of_find_device_by_node(neo_audio_node);
	of_node_put(neo_audio_node);
	if (!neo_audio_pdev) {
		dev_info(dev, "No DP/HDMI audio device not ready\n");
		return 0;
	}
	neo_dcp->audiosrv->neo_audio_dev = neo_audio_pdev;

	device_link_add(&neo_audio_pdev->dev, dev,
			DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME);

	neo_dcp->neo_avep = neo_afk_init(neo_dcp, AV_ENDPOINT, neo_avep_ops);
	if (IS_ERR(neo_dcp->neo_avep))
		return PTR_ERR(neo_dcp->neo_avep);
	neo_dcp->neo_avep->debugfs_entry = neo_dcp->ep_debugfs[AV_ENDPOINT - 0x20];
	return neo_afk_start(neo_dcp->neo_avep);
}
