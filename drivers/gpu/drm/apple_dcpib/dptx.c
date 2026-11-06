// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * DCP DPTX port service and DPAV controllers (port of aura
 * src/dcp/dptx_port_ep.c and src/dcp/dpav_ep.c, AFKEPV2 form only).
 */

#include <linux/bitfield.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "dcpib.h"

enum dptx_apcall {
	DPTX_APCALL_ACTIVATE = 0,
	DPTX_APCALL_DEACTIVATE = 1,
	DPTX_APCALL_GET_MAX_DRIVE_SETTINGS = 2,
	DPTX_APCALL_SET_DRIVE_SETTINGS = 3,
	DPTX_APCALL_GET_DRIVE_SETTINGS = 4,
	DPTX_APCALL_WILL_CHANGE_LINK_CONFIG = 5,
	DPTX_APCALL_DID_CHANGE_LINK_CONFIG = 6,
	DPTX_APCALL_GET_MAX_LINK_RATE = 7,
	DPTX_APCALL_GET_LINK_RATE = 8,
	DPTX_APCALL_SET_LINK_RATE = 9,
	DPTX_APCALL_GET_MAX_LANE_COUNT = 10,
	DPTX_APCALL_GET_ACTIVE_LANE_COUNT = 11,
	DPTX_APCALL_SET_ACTIVE_LANE_COUNT = 12,
	DPTX_APCALL_GET_SUPPORTS_DOWN_SPREAD = 13,
	DPTX_APCALL_GET_DOWN_SPREAD = 14,
	DPTX_APCALL_SET_DOWN_SPREAD = 15,
	DPTX_APCALL_GET_SUPPORTS_LANE_MAPPING = 16,
	DPTX_APCALL_SET_LANE_MAP = 17,
	DPTX_APCALL_GET_SUPPORTS_HPD = 18,
	DPTX_APCALL_FORCE_HOTPLUG_DETECT = 19,
	DPTX_APCALL_INACTIVE_SINK_DETECTED = 20,
	DPTX_APCALL_SET_TILED_DISPLAY_HINTS = 21,
	DPTX_APCALL_DEVICE_NOT_RESPONDING = 22,
	DPTX_APCALL_DEVICE_BUSY_TIMEOUT = 23,
	DPTX_APCALL_DEVICE_NOT_STARTED = 24,
	DPTX_APCALL_GET_VIRTUAL_DEVICE_MODE = 26,
};

#define DPTX_REMOTE_PORT_CORE		GENMASK(3, 0)
#define DPTX_REMOTE_PORT_DFP		GENMASK(7, 4)
#define DPTX_REMOTE_PORT_DIE		GENMASK(11, 8)
#define DPTX_REMOTE_PORT_CONNECTED	BIT(15)

struct dptx_connection {
	__le32 unk;
	__le32 target;
} __packed;

struct dptx_connection_reply {
	__le32 unk;
	__le32 target;
	__le64 reserved;
	__le32 status;
	u8 padding[12];
} __packed;

struct dptx_hotplug {
	u8 pad[16];
	__le32 unk;
} __packed;

static bool dptx_needs_port(u32 command)
{
	switch (command) {
	case DPTX_APCALL_ACTIVATE:
	case DPTX_APCALL_DEACTIVATE:
	case DPTX_APCALL_WILL_CHANGE_LINK_CONFIG:
	case DPTX_APCALL_DID_CHANGE_LINK_CONFIG:
	case DPTX_APCALL_SET_LINK_RATE:
	case DPTX_APCALL_SET_DOWN_SPREAD:
	case DPTX_APCALL_SET_ACTIVE_LANE_COUNT:
	case DPTX_APCALL_SET_DRIVE_SETTINGS:
	case DPTX_APCALL_GET_LINK_RATE:
	case DPTX_APCALL_GET_ACTIVE_LANE_COUNT:
	case DPTX_APCALL_GET_DRIVE_SETTINGS:
		return true;
	default:
		return false;
	}
}

/* Requests carry their argument, replies their value, at offset 16. */
static u32 dptx_call(struct dcpib_service *service, u16 group, u32 command,
		     const void *data, size_t data_size, void *reply,
		     size_t reply_size)
{
	struct dcpib *dcp = service->ep->dcp;
	struct dcpib_port *port = service->cookie;
	const struct dcpib_phy_ops *phy = dcp->phy;
	u64 value;
	u32 arg;

	if (group && !(group == 1 && (command == DPTX_APCALL_SET_DRIVE_SETTINGS ||
				      command == DPTX_APCALL_GET_DRIVE_SETTINGS)))
		return KIO_RETURN_UNSUPPORTED;

	dev_dbg(dcp->dev, "DPTX: port %u group %u command %u size %zu\n",
		service->unit, group, command, data_size);
	if ((!port || !phy) && dptx_needs_port(command))
		return KIO_RETURN_NOT_READY;

	switch (command) {
	case DPTX_APCALL_ACTIVATE:
		if (phy->activate(dcp))
			return KIO_RETURN_ERROR;
		port->link_rate = DCPIB_LINK_RATE_HBR;
		return 0;
	case DPTX_APCALL_DEACTIVATE:
		if (phy->deactivate(dcp))
			return KIO_RETURN_ERROR;
		port->link_rate = 0;
		return 0;
	case DPTX_APCALL_WILL_CHANGE_LINK_CONFIG:
	case DPTX_APCALL_DID_CHANGE_LINK_CONFIG:
		return phy->set_route(dcp, command ==
				      DPTX_APCALL_DID_CHANGE_LINK_CONFIG) ?
			       KIO_RETURN_ERROR : 0;
	case DPTX_APCALL_GET_MAX_LINK_RATE:
		value = phy ? phy->max_link_rate : DCPIB_LINK_RATE_HBR;
		break;
	case DPTX_APCALL_GET_LINK_RATE:
		value = port->link_rate;
		break;
	case DPTX_APCALL_SET_LINK_RATE:
		if (data_size < 17)
			return KIO_RETURN_BAD_ARG;
		arg = ((const u8 *)data)[16];
		if (arg > phy->max_link_rate)
			return KIO_RETURN_UNSUPPORTED;
		if (phy->set_link_rate(dcp, arg))
			return KIO_RETURN_ERROR;
		port->link_rate = arg;
		return 0;
	case DPTX_APCALL_GET_MAX_LANE_COUNT:
		value = 4;
		break;
	case DPTX_APCALL_GET_ACTIVE_LANE_COUNT:
		value = port->lanes;
		break;
	case DPTX_APCALL_SET_ACTIVE_LANE_COUNT:
		if (data_size < 20)
			return KIO_RETURN_BAD_ARG;
		arg = get_unaligned_le32(data + 16);
		if (arg == 3 || arg > 4)
			return KIO_RETURN_BAD_ARG;
		dev_info(dcp->dev, "DPTX: %u active lanes\n", arg);
		if (phy->set_lane_count && phy->set_lane_count(dcp, arg))
			return KIO_RETURN_ERROR;
		port->lanes = arg;
		if (arg)
			port->trained_lanes = arg;
		return 0;
	case DPTX_APCALL_GET_MAX_DRIVE_SETTINGS: {
		const struct dcpib_drive_settings maximum = {
			0, cpu_to_le32(3), cpu_to_le32(3)
		};

		if (reply_size < 16 + sizeof(maximum))
			return KIO_RETURN_BAD_ARG;
		memcpy(reply + 16, &maximum, sizeof(maximum));
		return 0;
	}
	case DPTX_APCALL_SET_DRIVE_SETTINGS: {
		struct dcpib_drive_settings settings[4];

		if (group != 1)
			return KIO_RETURN_UNSUPPORTED;
		if (data_size < 32)
			return KIO_RETURN_BAD_ARG;
		arg = get_unaligned_le32(data + 16);
		if (arg > 4 || data_size < 32 + arg * sizeof(settings[0]))
			return KIO_RETURN_BAD_ARG;
		memcpy(settings, data + 32, arg * sizeof(settings[0]));
		if (phy->set_drive(dcp, settings, arg))
			return KIO_RETURN_ERROR;
		memcpy(port->drive, settings, arg * sizeof(settings[0]));
		port->drive_count = arg;
		return 0;
	}
	case DPTX_APCALL_GET_DRIVE_SETTINGS:
		/*
		 * The firmware reads back what it set during link training; an
		 * unsupported reply here was followed by a firmware crash.
		 * Same layout as SET: count at 16, settings from 32.
		 */
		if (group != 1)
			return KIO_RETURN_UNSUPPORTED;
		if (reply_size < 32 + port->drive_count * sizeof(port->drive[0]))
			return KIO_RETURN_BAD_ARG;
		memcpy(reply, data, min(reply_size, data_size));
		put_unaligned_le32(port->drive_count, reply + 16);
		memcpy(reply + 32, port->drive,
		       port->drive_count * sizeof(port->drive[0]));
		return 0;
	case DPTX_APCALL_SET_DOWN_SPREAD:
		if (data_size < 17)
			return KIO_RETURN_BAD_ARG;
		/* The qualified link setup runs with downspread disabled. */
		return ((const u8 *)data)[16] ? KIO_RETURN_UNSUPPORTED : 0;
	case DPTX_APCALL_GET_SUPPORTS_DOWN_SPREAD:
	case DPTX_APCALL_GET_DOWN_SPREAD:
	case DPTX_APCALL_GET_SUPPORTS_LANE_MAPPING:
	case DPTX_APCALL_GET_VIRTUAL_DEVICE_MODE:
		value = 0;
		break;
	case DPTX_APCALL_GET_SUPPORTS_HPD:
		value = 1;
		break;
	case DPTX_APCALL_INACTIVE_SINK_DETECTED:
	case DPTX_APCALL_DEVICE_NOT_RESPONDING:
	case DPTX_APCALL_DEVICE_BUSY_TIMEOUT:
	case DPTX_APCALL_DEVICE_NOT_STARTED:
		/* Link fault reports: acknowledged with a zeroed retcode. */
		dev_info(dcp->dev, "DPTX: firmware reports link fault %u\n",
			 command);
		memcpy(reply, data, min(reply_size, data_size));
		if (reply_size > 4)
			memset(reply, 0, 4);
		return 0;
	case DPTX_APCALL_SET_TILED_DISPLAY_HINTS:
		memcpy(reply, data, min(reply_size, data_size));
		if (reply_size >= 4)
			*(__le32 *)reply = cpu_to_le32(1);
		return 0;
	default:
		dev_info(dcp->dev, "DPTX: unsupported call %u\n", command);
		return KIO_RETURN_UNSUPPORTED;
	}

	if (reply_size < 16 + sizeof(value))
		return KIO_RETURN_BAD_ARG;
	put_unaligned_le64(value, reply + 16);
	return 0;
}

static const struct dcpib_service_ops dptx_ops = {
	.call = dptx_call,
};

int dcpib_dptx_init(struct dcpib *dcp)
{
	struct dcpib_ep *ep = dcpib_ep_start(dcp, DCPIB_EP_DPTX, &dptx_ops, 2);

	if (IS_ERR(ep))
		return PTR_ERR(ep);
	dcp->dptx_ep = ep;
	/* Ports in the order the firmware announced them. */
	for (unsigned int i = 0; i < ep->nr_services && i < 2; i++) {
		dcp->port[i].service = &ep->services[i];
		ep->services[i].cookie = &dcp->port[i];
	}
	return 0;
}

int dcpib_dpav_init(struct dcpib *dcp)
{
	struct dcpib_ep *ep = dcpib_ep_start(dcp, DCPIB_EP_DPAV, NULL, 4);

	if (IS_ERR(ep))
		return PTR_ERR(ep);
	dcp->dpav_ep = ep;
	return 0;
}

/* connectTo, then requestDisplay, as aura dcp_dptx_connect. */
int dcpib_dptx_connect(struct dcpib *dcp, unsigned int port)
{
	struct dcpib_service *service = dcp->port[port].service;
	u32 target = FIELD_PREP(DPTX_REMOTE_PORT_CORE, 0) |
		     FIELD_PREP(DPTX_REMOTE_PORT_DFP, dcp->dcp_output) |
		     FIELD_PREP(DPTX_REMOTE_PORT_DIE, dcp->die) |
		     DPTX_REMOTE_PORT_CONNECTED;
	struct dptx_connection cmd = {
		/* IODPTXPortAttributes supportsHPD */
		.unk = cpu_to_le32(0x100),
		.target = cpu_to_le32(target),
	};
	struct dptx_connection_reply resp = {};
	__le32 status = 0;
	int ret;

	if (!service)
		return -ENODEV;
	ret = dcpib_service_call(service, 0, 11, &cmd, sizeof(cmd), 24, &resp,
				 sizeof(resp), 0);
	if (ret)
		return ret;
	if (resp.status) {
		dev_err(dcp->dev, "DPTX: connect returned %#x\n",
			le32_to_cpu(resp.status));
		return -EIO;
	}
	if (le32_to_cpu(resp.target) != target ||
	    le32_to_cpu(resp.unk) != 0x100)
		return -EPROTO;

	ret = dcpib_service_call(service, 0, 6, NULL, 0, 16, &status,
				 sizeof(status), 12);
	if (!ret && status) {
		dev_err(dcp->dev, "DPTX: requestDisplay returned %#x\n",
			le32_to_cpu(status));
		ret = -EIO;
	}
	return ret;
}

int dcpib_dptx_set_hpd(struct dcpib *dcp, unsigned int port, bool hpd)
{
	struct dcpib_service *service = dcp->port[port].service;
	struct dptx_hotplug cmd = { .unk = cpu_to_le32(hpd) }, resp = {};
	int ret;

	if (!service)
		return -ENODEV;
	ret = dcpib_service_call(service, 8, 8, &cmd, sizeof(cmd), 12, &resp,
				 sizeof(resp), 12);
	if (ret)
		return ret;
	/* The status is the first word of the reply. */
	return get_unaligned_le32(resp.pad) ? -EIO : 0;
}
