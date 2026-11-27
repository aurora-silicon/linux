// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2023 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "afk.h"
#include "dcp.h"
#include "dcpext_mode.h"
#include "ibootep.h"

/* m1n1 src/dcp_iboot.c: iBoot commands have their own wrapper inside EPIC. */
#define IBOOT_QUERY_RX_SIZE 0x4000
#define IBOOT_MODE_SIZE 24
/* The mode the query selects: 3840x2160, see iboot_pattern_timing_rank(). */
#define IBOOT_PATTERN_WIDTH 3840
#define IBOOT_PATTERN_HEIGHT 2160

enum iboot_query_op {
	IBOOT_SET_SURFACE = 1,
	IBOOT_SET_POWER = 2,
	IBOOT_GET_HPD = 3,
	IBOOT_GET_TIMING_MODES = 4,
	IBOOT_GET_COLOR_MODES = 5,
	IBOOT_SET_MODE = 6,
};

/* Packed layouts from m1n1 src/dcp_iboot.h and swap_set_layer_cmd_v13_3. */
struct iboot_plane {
	__le32 unk1;
	__le64 addr;
	__le32 tile_size;
	__le32 stride;
	__le32 unk2[4];
	__le32 addr_format;
	__le32 unk3;
} __packed;

struct iboot_layer {
	struct iboot_plane planes[3];
	__le32 unk;
	__le32 plane_count;
	__le32 width;
	__le32 height;
	__le32 surface_format;
	__le32 colorspace;
	__le32 eotf;
	u8 transform;
	u8 padding[3];
} __packed;

struct iboot_swap_layer_v13_3 {
	__le32 unk;
	__le32 layer_id;
	struct iboot_layer layer;
	__le32 unk3;
	__le32 unk4;
	__le32 src[4];
	__le32 dst[4];
	__le32 unk2;
} __packed;

static_assert(sizeof(struct iboot_plane) == 44);
static_assert(sizeof(struct iboot_layer) == 164);
static_assert(sizeof(struct iboot_swap_layer_v13_3) == 216);

struct iboot_query {
	struct apple_epic_service *service;
	struct work_struct work;
	atomic_t busy;
	bool stopping;
	bool timing_valid;
	bool color_valid;
	/* Preserve the exact queried records, including firmware-owned padding. */
	u8 timing[IBOOT_MODE_SIZE];
	u8 color[IBOOT_MODE_SIZE];
};

/* Small wire helpers kept separate so malformed replies can be host-tested. */
static int iboot_build_query(u32 op, __le32 request[4])
{
	if (op < IBOOT_GET_HPD || op > IBOOT_GET_COLOR_MODES)
		return -EINVAL;
	request[0] = cpu_to_le32(op);
	request[1] = cpu_to_le32(16);
	request[2] = 0;
	request[3] = 0;
	return 0;
}

static int iboot_reply_payload(const u8 *reply, size_t capacity, u32 op,
			       const u8 **payload, size_t *size)
{
	u32 length;

	if (capacity < 8 || get_unaligned_le32(reply) != op)
		return -EPROTO;
	length = get_unaligned_le32(reply + 4);
	if (length < 8 || length > capacity)
		return -EPROTO;
	*payload = reply + 8;
	*size = length - 8;
	return 0;
}

static int iboot_parse_hpd(const u8 *payload, size_t size, bool *hpd,
			   u32 *timings, u32 *colors)
{
	if (size < 12 || payload[0] > 1)
		return -EPROTO;
	*hpd = payload[0];
	*timings = get_unaligned_le32(payload + 4);
	*colors = get_unaligned_le32(payload + 8);
	/* A complete mode list must fit the fixed, bounded reply buffer. */
	if (*timings > (IBOOT_QUERY_RX_SIZE - 12) / IBOOT_MODE_SIZE ||
	    *colors > (IBOOT_QUERY_RX_SIZE - 12) / IBOOT_MODE_SIZE)
		return -EOVERFLOW;
	return 0;
}

/* A mode list: a count, then as many 24-byte records. */
static int iboot_parse_mode_count(const u8 *payload, size_t size, u32 *count)
{
	if (size < 4)
		return -EPROTO;
	*count = get_unaligned_le32(payload);
	if (*count > (size - 4) / IBOOT_MODE_SIZE)
		return -EPROTO;
	return 0;
}

static int iboot_parse_modes(const u8 *payload, size_t size, u32 *count)
{
	u32 i;
	int ret;

	ret = iboot_parse_mode_count(payload, size, count);
	if (ret)
		return ret;
	for (i = 0; i < *count; i++)
		if (get_unaligned_le32(payload + 4 + i * IBOOT_MODE_SIZE) > 1)
			return -EPROTO;
	return 0;
}

/*
 * 3840x2160 timings the mode query selects, best first: the reduced-blanking
 * 59.9966 Hz timing seen through TB3 docks, then the standard 59.94 Hz and 60 Hz
 * timings that monitors report over faster USB4 links. 59.94 Hz comes before
 * 60 Hz because firmware can resolve "60 Hz" to a DSC timing on 4K120 monitors,
 * and DSC output does not yet produce a picture on this path. Returns 0 for
 * anything else.
 */
static int iboot_pattern_timing_rank(const u8 *mode)
{
	static const u32 fps[] = { 0x003bff23U, 0x003bf0a8U, 0x003c0000U };
	u32 i;

	if (get_unaligned_le32(mode + 4) != IBOOT_PATTERN_WIDTH ||
	    get_unaligned_le32(mode + 8) != IBOOT_PATTERN_HEIGHT)
		return 0;
	for (i = 0; i < ARRAY_SIZE(fps); i++)
		if (get_unaligned_le32(mode + 12) == fps[i])
			return ARRAY_SIZE(fps) - i;
	return 0;
}

static bool iboot_pattern_mode(u32 op, const u8 *mode)
{
	if (get_unaligned_le32(mode) != 1)
		return false;
	if (op == IBOOT_GET_TIMING_MODES)
		return iboot_pattern_timing_rank(mode) > 0;
	if (op == IBOOT_GET_COLOR_MODES)
		return get_unaligned_le32(mode + 4) == 1 && /* BT.601/709 */
		       get_unaligned_le32(mode + 8) == 1 && /* SDR */
		       get_unaligned_le32(mode + 12) == 1 && /* RGB */
		       get_unaligned_le32(mode + 16) == 32;
	return false;
}

static int iboot_build_pattern_request(u32 op, const void *data, size_t data_size,
				       u8 *request, size_t capacity, size_t *size)
{
	size_t expected;

	switch (op) {
	case 1: /* SetSurface */
		expected = sizeof(struct iboot_layer);
		break;
	case 2: expected = 1; break; /* SetPower */
	case 3 ... 5: /* GetHPD, GetTimingModes, GetColorModes */
		expected = 0;
		break;
	case 6: expected = 2 * IBOOT_MODE_SIZE; break; /* SetMode */
	case 15: expected = 0; break; /* SwapBegin */
	case 16: expected = sizeof(struct iboot_swap_layer_v13_3); break;
	case 18: expected = 12; break; /* SwapEnd */
	case 19: /* SwapWait */
		expected = 16;
		break;
	default: return -EINVAL;
	}
	if (data_size != expected || capacity < 16 + expected ||
	    (expected && !data))
		return -EINVAL;
	memset(request, 0, 16 + expected);
	put_unaligned_le32(op, request);
	put_unaligned_le32(16 + expected, request + 4);
	if (expected)
		memcpy(request + 16, data, expected);
	*size = 16 + expected;
	return 0;
}

static int iboot_read_query(struct iboot_query *query, u32 op, u8 *reply,
			    const u8 **payload, size_t *size)
{
	struct apple_epic_service *service = query->service;
	__le32 request[4];
	u32 retcode = 0;
	int ret;

	if (READ_ONCE(query->stopping) || READ_ONCE(service->torndown))
		return -ENODEV;
	ret = iboot_build_query(op, request);
	if (ret)
		return ret;
	memset(reply, 0, IBOOT_QUERY_RX_SIZE);
	ret = afk_send_command(service, EPIC_SUBTYPE_STD_SERVICE, request,
			       sizeof(request), reply, IBOOT_QUERY_RX_SIZE, &retcode);
	if (ret)
		return ret;
	if (retcode) {
		dev_warn(service->ep->dcp->dev, "iBoot query %u returned %#x\n", op, retcode);
		return -EIO;
	}
	ret = iboot_reply_payload(reply, IBOOT_QUERY_RX_SIZE, op, payload, size);
	if (ret)
		dev_warn(service->ep->dcp->dev,
			 "iBoot query %u malformed header: opcode=%#x length=%#x\n",
			 op, get_unaligned_le32(reply), get_unaligned_le32(reply + 4));
	return ret;
}

static int iboot_log_modes(struct iboot_query *query, u32 op, u8 *reply)
{
	struct device *dev = query->service->ep->dcp->dev;
	const u8 *payload;
	size_t size;
	u32 count, i, selected = U32_MAX;
	int ret;

	ret = iboot_read_query(query, op, reply, &payload, &size);
	if (ret)
		return ret;
	ret = iboot_parse_modes(payload, size, &count);
	if (ret)
		return ret;
	for (i = 0; i < count; i++) {
		const u8 *mode = payload + 4 + i * IBOOT_MODE_SIZE;
		int rank;

		if (!iboot_pattern_mode(op, mode))
			continue;
		if (op == IBOOT_GET_TIMING_MODES) {
			/* Best rank wins; among equals the last, since the monitor's own
			 * timings follow the firmware's fixed list. */
			rank = iboot_pattern_timing_rank(mode);
			if (selected != U32_MAX &&
			    rank < iboot_pattern_timing_rank(query->timing))
				continue;
			memcpy(query->timing, mode, IBOOT_MODE_SIZE);
			query->timing_valid = true;
			selected = i;
			continue;
		}
		memcpy(query->color, mode, IBOOT_MODE_SIZE);
		query->color_valid = true;
		selected = i;
		break;
	}
	dev_info(dev, "iBoot %s modes: %u\n",
		 op == IBOOT_GET_TIMING_MODES ? "timing" : "color", count);
	/* Count is bounded by the validated reply length, at most 682 records. */
	for (i = 0; i < count; i++) {
		const u8 *mode = payload + 4 + i * IBOOT_MODE_SIZE;
		u32 valid = get_unaligned_le32(mode);

		dev_info(dev, "iBoot mode op=%u index=%u selected=%u raw24=%*phN\n",
			 op, i, i == selected, IBOOT_MODE_SIZE, mode);

		if (op == IBOOT_GET_TIMING_MODES) {
			u32 fps = get_unaligned_le32(mode + 12);

			dev_info(dev, "iBoot timing %u: valid=%u %ux%u fps=%u.%04u rawfps=%#x\n",
				 i, valid, get_unaligned_le32(mode + 4),
				 get_unaligned_le32(mode + 8), fps >> 16,
				 ((fps & 0xffff) * 10000) >> 16, fps);
		} else {
			dev_info(dev, "iBoot color %u: valid=%u colorimetry=%u eotf=%u encoding=%u bpp=%u\n",
				 i, valid, get_unaligned_le32(mode + 4),
				 get_unaligned_le32(mode + 8), get_unaligned_le32(mode + 12),
				 get_unaligned_le32(mode + 16));
		}
	}
	return 0;
}

static int iboot_refresh_modes(struct iboot_query *query, u8 *reply)
{
	struct device *dev = query->service->ep->dcp->dev;
	const u8 *payload;
	size_t size;
	u32 timings, colors;
	bool hpd;
	int ret;

	query->timing_valid = false;
	query->color_valid = false;
	ret = iboot_read_query(query, IBOOT_GET_HPD, reply, &payload, &size);
	if (ret)
		return ret;
	ret = iboot_parse_hpd(payload, size, &hpd, &timings, &colors);
	if (ret)
		return ret;
	dev_info(dev, "iBoot HPD=%u timing-count=%u color-count=%u\n", hpd, timings, colors);
	if (!hpd)
		return -ENOLINK;
	ret = iboot_log_modes(query, IBOOT_GET_TIMING_MODES, reply);
	if (!ret)
		ret = iboot_log_modes(query, IBOOT_GET_COLOR_MODES, reply);
	return ret;
}

static void iboot_query_work(struct work_struct *work)
{
	struct iboot_query *query = container_of(work, struct iboot_query, work);
	struct device *dev = query->service->ep->dcp->dev;
	u8 *reply;
	int ret;

	reply = kzalloc(IBOOT_QUERY_RX_SIZE, GFP_KERNEL);
	if (!reply) {
		ret = -ENOMEM;
		goto done;
	}
	ret = iboot_refresh_modes(query, reply);
	kfree(reply);
done:
	if (ret && ret != -ENOLINK)
		dev_warn(dev, "iBoot mode query failed: %d\n", ret);
	atomic_set_release(&query->busy, 0);
}

static struct iboot_query *iboot_find_query(struct apple_dcp *dcp)
{
	struct apple_dcp_afkep *ep = READ_ONCE(dcp->ibootep);
	u32 i;

	if ((!dcp->external && !dcp_is_iboot(dcp)) || !ep)
		return ERR_PTR(-ENODEV);
	for (i = 0; i < AFK_MAX_CHANNEL; i++) {
		struct iboot_query *query = smp_load_acquire(&ep->services[i].cookie);

		if (!query)
			continue;
		if (READ_ONCE(query->stopping) || READ_ONCE(query->service->torndown))
			return ERR_PTR(-ENODEV);
		return query;
	}
	return ERR_PTR(-EAGAIN);
}

bool ibootep_is_ready(struct apple_dcp *dcp)
{
	return !IS_ERR(iboot_find_query(dcp));
}

int ibootep_query_modes(struct apple_dcp *dcp)
{
	struct iboot_query *query = iboot_find_query(dcp);

	if (IS_ERR(query))
		return PTR_ERR(query);
	if (atomic_cmpxchg(&query->busy, 0, 1))
		return -EBUSY;
	/* AFK replies run on ep->wq; waiting on that same queue deadlocks. */
	queue_work(system_dfl_wq, &query->work);
	return 0;
}

/*
 * One display command of the iBoot interface, with its reply payload copied
 * to @out (at least @min_size bytes of it; returns the bytes copied), or a
 * setter if @out is NULL. On AFKv2 the setters answer with a transport
 * status alone: given a reply buffer, the firmware copies an empty reply
 * into it and asserts, so they go out without one. Only the reply's own
 * length is checked, not that its first word repeats @op.
 */
static int iboot_command(struct apple_dcp *dcp, u32 op, const void *data,
			 size_t size, void *out, size_t out_size, size_t min_size)
{
	struct iboot_query *query = iboot_find_query(dcp);
	u8 request[16 + sizeof(struct iboot_swap_layer_v13_3)];
	size_t len, received = 0;
	u32 retcode = 0, reply_len;
	u8 *reply = NULL;
	int ret;

	if (IS_ERR(query))
		return PTR_ERR(query);
	ret = iboot_build_pattern_request(op, data, size, request,
					  sizeof(request), &len);
	if (ret)
		return ret;
	if (out || !query->service->ep->wire_v2) {
		reply = kzalloc(IBOOT_QUERY_RX_SIZE, GFP_KERNEL);
		if (!reply)
			return -ENOMEM;
	}
	ret = afk_send_command_len(query->service, EPIC_SUBTYPE_STD_SERVICE,
				   request, len, reply,
				   reply ? IBOOT_QUERY_RX_SIZE : 0, &retcode,
				   &received);
	if (!ret && retcode) {
		dev_dbg(dcp->dev, "iBoot command %u returned %#x\n", op, retcode);
		ret = -EIO;
	}
	if (ret || !out)
		goto out;
	reply_len = received >= 8 ? get_unaligned_le32(reply + 4) : 0;
	if (reply_len < 8 || reply_len > received || reply_len - 8 < min_size) {
		dev_err(dcp->dev, "iBoot: bad response length for command %u\n", op);
		ret = -EPROTO;
		goto out;
	}
	ret = min_t(size_t, reply_len - 8, out_size);
	memcpy(out, reply + 8, ret);
out:
	kfree(reply);
	return ret;
}

int ibootep_set_power(struct apple_dcp *dcp, bool on)
{
	u8 power = on;

	return iboot_command(dcp, IBOOT_SET_POWER, &power, sizeof(power), NULL,
			     0, 0);
}

int ibootep_get_hpd(struct apple_dcp *dcp, u32 *timings, u32 *colors)
{
	u8 payload[12];
	u32 timing_count, color_count;
	bool hpd;
	int ret;

	ret = iboot_command(dcp, IBOOT_GET_HPD, NULL, 0, payload, sizeof(payload),
			    sizeof(payload));
	if (ret >= 0)
		ret = iboot_parse_hpd(payload, ret, &hpd, &timing_count,
				      &color_count);
	if (ret)
		return ret;
	if (timings)
		*timings = timing_count;
	if (colors)
		*colors = color_count;
	return hpd;
}

static int iboot_get_modes(struct apple_dcp *dcp, u32 op, void **modes,
			   unsigned int *count)
{
	u32 n;
	u8 *buf;
	int ret;

	buf = kzalloc(IBOOT_QUERY_RX_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	ret = iboot_command(dcp, op, NULL, 0, buf, IBOOT_QUERY_RX_SIZE, 4);
	if (ret >= 0)
		ret = iboot_parse_mode_count(buf, ret, &n);
	if (!ret) {
		*modes = kmemdup(buf + 4, n * IBOOT_MODE_SIZE, GFP_KERNEL);
		if (*modes)
			*count = n;
		else
			ret = -ENOMEM;
	}
	kfree(buf);
	return ret;
}

int ibootep_get_timing_modes(struct apple_dcp *dcp,
			     struct iboot_timing_mode **modes,
			     unsigned int *count)
{
	return iboot_get_modes(dcp, IBOOT_GET_TIMING_MODES, (void **)modes, count);
}

int ibootep_get_color_modes(struct apple_dcp *dcp,
			    struct iboot_color_mode **modes, unsigned int *count)
{
	return iboot_get_modes(dcp, IBOOT_GET_COLOR_MODES, (void **)modes, count);
}

int ibootep_set_mode(struct apple_dcp *dcp, const struct iboot_timing_mode *timing,
		     const struct iboot_color_mode *color)
{
	struct {
		struct iboot_timing_mode timing;
		struct iboot_color_mode color;
	} __packed mode = { *timing, *color };

	return iboot_command(dcp, IBOOT_SET_MODE, &mode, sizeof(mode), NULL, 0, 0);
}

int ibootep_set_surface(struct apple_dcp *dcp, u64 iova, u32 width, u32 height,
			u32 stride)
{
	/* One linear BGRA plane, SDR, in colour space 2 as the bootloader sets. */
	struct iboot_layer layer = {
		.planes = { {
			.addr = cpu_to_le64(iova),
			.stride = cpu_to_le32(stride),
			.addr_format = cpu_to_le32(1),
		} },
		.plane_count = cpu_to_le32(1),
		.width = cpu_to_le32(width),
		.height = cpu_to_le32(height),
		.surface_format = cpu_to_le32(1),
		.colorspace = cpu_to_le32(2),
		.eotf = cpu_to_le32(1),
	};

	return iboot_command(dcp, IBOOT_SET_SURFACE, &layer, sizeof(layer), NULL,
			     0, 0);
}

static ssize_t dcpext_modes_query_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	bool query;
	int ret = kstrtobool(buf, &query);

	if (ret || !query)
		return -EINVAL;
	ret = ibootep_query_modes(dev_get_drvdata(dev));
	return ret ?: count;
}
static DEVICE_ATTR_WO(dcpext_modes_query);
static struct attribute *iboot_query_attrs[] = { &dev_attr_dcpext_modes_query.attr, NULL };
static const struct attribute_group iboot_query_group = { .attrs = iboot_query_attrs };

static void iboot_query_cancel(void *data)
{
	struct iboot_query *query = data;

	WRITE_ONCE(query->stopping, true);
	cancel_work_sync(&query->work);
}

static void disp_service_init(struct apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
	struct apple_dcp *dcp = service->ep->dcp;
	struct iboot_query *query;
	int ret;

	if (!dcp->external && !dcp_is_iboot(dcp))
		return;
	query = devm_kzalloc(dcp->dev, sizeof(*query), GFP_KERNEL);
	if (!query)
		return;
	query->service = service;
	atomic_set(&query->busy, 0);
	INIT_WORK(&query->work, iboot_query_work);
	ret = devm_add_action_or_reset(dcp->dev, iboot_query_cancel, query);
	if (ret)
		return;
	/*
	 * A DCP driven through this interface alone has a display driver that
	 * sends every command (the exported ibootep_* calls); it takes no
	 * manual queries next to them.
	 */
	if (dcp_is_iboot(dcp)) {
		/* Published for iboot_find_query()'s acquire. */
		smp_store_release(&service->cookie, query);
		return;
	}
	/* Remove the sysfs entry before cancelling work during device cleanup. */
	ret = devm_device_add_group(dcp->dev, &iboot_query_group);
	if (ret) {
		dev_warn(dcp->dev, "iBoot query interface unavailable: %d\n", ret);
		return;
	}
	smp_store_release(&service->cookie, query);
	dev_info(dcp->dev, "external iBoot disp0 service ready on channel %u; mode queries are manual\n",
		 service->channel);
}


static const struct apple_epic_service_ops ibootep_ops[] = {
	{
		.name = "disp0-service",
		.init = disp_service_init,
	},
	{}
};

static const struct apple_epic_service_ops external_ibootep_ops[] = {
	{
		.name = "dispext-service",
		.init = disp_service_init,
	},
	{}
};

/* DCP_FIRMWARE_IBOOT: the main DCP's service, or an external one's. */
static const struct apple_epic_service_ops iboot_variant_ops[] = {
	{
		.name = "disp0-service",
		.init = disp_service_init,
	},
	{
		.name = "dispext-service",
		.init = disp_service_init,
	},
	{}
};

int ibootep_init(struct apple_dcp *dcp)
{
	const struct apple_epic_service_ops *ops = ibootep_ops;
	int ret;

	/* The T6030 external start path may race a second start. */
	if (dcp->ibootep && dcp->fw_compat == DCP_FIRMWARE_V_14_7)
		return -EBUSY;
	if (dcp->hw.iomfb_method_profile == DCP_IOMFB_METHODS_H17P_EXTERNAL)
		ops = external_ibootep_ops;
	if (dcp_is_iboot(dcp))
		ops = iboot_variant_ops;
	dcp->ibootep = afk_init(dcp, DISP0_ENDPOINT, ops);
	if (IS_ERR(dcp->ibootep)) {
		ret = PTR_ERR(dcp->ibootep);
		dcp->ibootep = NULL;
		return ret;
	}

	return afk_start(dcp->ibootep);
}
