// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The DPTX port service of a T8152 iBoot interface DCP: the calls the
 * firmware makes into the AP for the PHY behind its DPTX port, and the
 * port's release and HPD calls.
 *
 * dptxep.c serves the same calls for the PHYs of the PHY framework, the
 * display crossbar mux and Thunderbolt tunnels. The T8152 PHYs are
 * programmed by t8152_atc.c and t8152_hdmi.c instead: their link rate,
 * lanes and drive settings are applied here, and the drive settings are
 * read back as they were set. The port's connect and display request are
 * dptxep.c's. Interim, until the T8152 PHYs are PHY framework providers
 * and dptxep.c can host this service.
 */

#include <linux/device.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "afk.h"
#include "dcp-internal.h"
#include "dptx-service.h"
#include "dptxep.h"
#include "t8152_display.h"

/* IOReturn values the firmware takes as call status */
#define KIO_RETURN_ERROR	0xe00002bcU
#define KIO_RETURN_BAD_ARG	0xe00002c2U
#define KIO_RETURN_UNSUPPORTED	0xe00002c7U
#define KIO_RETURN_NOT_READY	0xe00002d8U

static bool dptx_needs_port(u32 command)
{
	switch (command) {
	case DPTX_APCALL_ACTIVATE:
	case DPTX_APCALL_DEACTIVATE:
	case DPTX_APCALL_WILL_CHANGE_LINKG_CONFIG:
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

/*
 * One call; the IOReturn status. Requests carry their argument, replies
 * their value, at offset 16. @reply holds a copy of the request.
 */
static u32 dptx_apcall(struct t8152_display *disp, struct t8152_port *port,
		       u16 group, u32 command, const void *data, size_t data_size,
		       void *reply, size_t reply_size)
{
	const struct t8152_phy_ops *phy = disp->phy;
	u64 value;
	u32 arg;

	if (group && !(group == 1 && (command == DPTX_APCALL_SET_DRIVE_SETTINGS ||
				      command == DPTX_APCALL_GET_DRIVE_SETTINGS)))
		return KIO_RETURN_UNSUPPORTED;

	if ((!port || !phy) && dptx_needs_port(command))
		return KIO_RETURN_NOT_READY;

	switch (command) {
	case DPTX_APCALL_ACTIVATE:
		if (phy->activate(disp))
			return KIO_RETURN_ERROR;
		port->link_rate = LINK_RATE_HBR;
		return 0;
	case DPTX_APCALL_DEACTIVATE:
		if (phy->deactivate(disp))
			return KIO_RETURN_ERROR;
		port->link_rate = 0;
		return 0;
	case DPTX_APCALL_WILL_CHANGE_LINKG_CONFIG:
	case DPTX_APCALL_DID_CHANGE_LINK_CONFIG:
		return phy->set_route(disp, command ==
				      DPTX_APCALL_DID_CHANGE_LINK_CONFIG) ?
			       KIO_RETURN_ERROR : 0;
	case DPTX_APCALL_GET_MAX_LINK_RATE:
		value = phy ? phy->max_link_rate : LINK_RATE_HBR;
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
		if (phy->set_link_rate(disp, arg))
			return KIO_RETURN_ERROR;
		port->link_rate = arg;
		return 0;
	case DPTX_APCALL_GET_MAX_LANE_COUNT:
		value = phy && phy->max_lane_count ? phy->max_lane_count(disp) : 4;
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
		dev_info(disp->dev, "DPTX: %u active lanes\n", arg);
		if (phy->set_lane_count && phy->set_lane_count(disp, arg))
			return KIO_RETURN_ERROR;
		port->lanes = arg;
		return 0;
	case DPTX_APCALL_GET_MAX_DRIVE_SETTINGS: {
		const struct t8152_drive_settings maximum = {
			0, cpu_to_le32(3), cpu_to_le32(3)
		};

		if (reply_size < 16 + sizeof(maximum))
			return KIO_RETURN_BAD_ARG;
		memcpy(reply + 16, &maximum, sizeof(maximum));
		return 0;
	}
	case DPTX_APCALL_SET_DRIVE_SETTINGS: {
		struct t8152_drive_settings settings[4];

		if (group != 1)
			return KIO_RETURN_UNSUPPORTED;
		if (data_size < 32)
			return KIO_RETURN_BAD_ARG;
		arg = get_unaligned_le32(data + 16);
		if (arg > 4 || data_size < 32 + arg * sizeof(settings[0]))
			return KIO_RETURN_BAD_ARG;
		memcpy(settings, data + 32, arg * sizeof(settings[0]));
		if (phy->set_drive(disp, settings, arg))
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
		put_unaligned_le32(port->drive_count, reply + 16);
		memcpy(reply + 32, port->drive,
		       port->drive_count * sizeof(port->drive[0]));
		return 0;
	case DPTX_APCALL_SET_DOWN_SPREAD:
		if (data_size < 17)
			return KIO_RETURN_BAD_ARG;
		/* The link is set up with downspread disabled. */
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
		/* Link fault reports: acknowledged with a zero status. */
		dev_info(disp->dev, "DPTX: firmware reports link fault %u\n",
			 command);
		return 0;
	case DPTX_APCALL_SET_TILED_DISPLAY_HINTS:
		/* Acknowledged, the hints unused. */
		return 0;
	default:
		dev_info(disp->dev, "DPTX: unsupported call %u\n", command);
		return KIO_RETURN_UNSUPPORTED;
	}

	if (reply_size < 16 + sizeof(value))
		return KIO_RETURN_BAD_ARG;
	put_unaligned_le64(value, reply + 16);
	return 0;
}

/*
 * The reply starts as a copy of the request and carries the call's status
 * in its first word; the transport status is success unless the reply has
 * no room for that word.
 */
static int t8152_dptx_call(struct apple_epic_service *service, u16 group,
			   u32 command, const void *data, size_t size,
			   void *reply, size_t room)
{
	struct apple_dcp *dcp = service->ep->dcp;
	struct dptx_port *dptx = service->cookie;
	struct t8152_display *disp = READ_ONCE(dcp->t8152);
	u32 status;

	dev_dbg(dcp->dev, "DPTX: port %d group %u command %u size %zu\n",
		dptx ? (int)dptx->unit : -1, group, command, size);
	/* The firmware sizes the reply; it has to hold the status word. */
	if (room < sizeof(__le32)) {
		dev_warn_ratelimited(dcp->dev,
				     "DPTX: group %u command %u reply of %zu bytes has no room for its status\n",
				     group, command, room);
		return -EINVAL;
	}
	memcpy(reply, data, min(room, size));
	if (disp)
		status = dptx_apcall(disp, dptx ? &disp->port[dptx->unit] : NULL,
				     group, command, data, size, reply, room);
	else
		status = KIO_RETURN_NOT_READY;
	if (status)
		dev_dbg(dcp->dev, "DPTX: group %u command %u -> %#x\n", group,
			command, status);
	put_unaligned_le32(status, reply);
	return 0;
}

/*
 * The ports in the order the firmware announces them. A further service is
 * opened as well, without a port.
 */
static int t8152_dptx_prepare(struct apple_epic_service *service,
			      const char *name, const char *class, s64 unit)
{
	struct apple_dcp *dcp = service->ep->dcp;

	for (unsigned int i = 0; i < ARRAY_SIZE(dcp->dptxport); i++)
		if (!dcp->dptxport[i].service)
			return dptx_service_bind(service, &dcp->dptxport[i], i);
	return 0;
}

static void t8152_dptx_init(struct apple_epic_service *service,
			    const char *name, const char *class, s64 unit)
{
	if (service->cookie)
		dptx_service_publish(service);
}

/* Bound to every service of the DPTX endpoint (afk unmatched_ops). */
const struct apple_epic_service_ops t8152_dptx_port_ops = {
	.name = "dcpdptx-port",
	.prepare = t8152_dptx_prepare,
	.init = t8152_dptx_init,
	.call_v2 = t8152_dptx_call,
};

static struct apple_epic_service *t8152_dptx_service(struct t8152_display *disp,
						     unsigned int port)
{
	struct dptx_port *dptx = &disp->dcp->dptxport[port];

	return dptx_service_available(dptx) ? dptx->service : NULL;
}

/* Connect the port to the PHY the DCP drives and request the display. */
int t8152_dptx_connect(struct t8152_display *disp, unsigned int port)
{
	struct apple_epic_service *service = t8152_dptx_service(disp, port);
	int ret;

	if (!service)
		return -ENODEV;
	ret = dptxport_connect(service, 0, disp->dcp_output, disp->die, true);
	if (!ret)
		ret = dptxport_request_display(service);
	return ret;
}

/*
 * HPD as the port reports it to the firmware (group 8, command 8): the
 * reply's first word is the call's status.
 */
int t8152_dptx_set_hpd(struct t8152_display *disp, unsigned int port, bool hpd)
{
	struct apple_epic_service *service = t8152_dptx_service(disp, port);
	struct {
		u8 pad[16];
		__le32 hpd;
	} __packed cmd = { .hpd = cpu_to_le32(hpd) }, resp = {};
	size_t received;
	int ret;

	if (!service)
		return -ENODEV;
	ret = afk_service_call_len(service, 8, 8, &cmd, sizeof(cmd), 12, &resp,
				   sizeof(resp), 12, &received);
	if (ret)
		return ret;
	return get_unaligned_le32(resp.pad) ? -EIO : 0;
}

/* Release the display (group 8, command 7), then HPD low. */
int t8152_dptx_disconnect(struct t8152_display *disp, unsigned int port)
{
	struct apple_epic_service *service = t8152_dptx_service(disp, port);
	__le32 status = 0;
	size_t received;
	int ret;

	if (!service)
		return -ENODEV;
	ret = afk_service_call_len(service, 8, 7, NULL, 0, 16, &status,
				   sizeof(status), 12, &received);
	if (ret || status)
		return ret ?: -EIO;
	return t8152_dptx_set_hpd(disp, port, false);
}
