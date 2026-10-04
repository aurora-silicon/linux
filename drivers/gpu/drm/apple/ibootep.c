// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2023 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "afk.h"
#include "dcp.h"
#include "dcpext_mode.h"
#include "dcpext_scanout.h"
#include "ibootep.h"

/* m1n1 src/dcp_iboot.c: iBoot commands have their own wrapper inside EPIC. */
#define IBOOT_QUERY_RX_SIZE 0x4000
#define IBOOT_MODE_SIZE 24
#define IBOOT_PATTERN_WIDTH DCPEXT_WIDTH
#define IBOOT_PATTERN_HEIGHT DCPEXT_HEIGHT
#define IBOOT_PATTERN_STRIDE DCPEXT_STRIDE

enum iboot_query_op {
	IBOOT_GET_HPD = 3,
	IBOOT_GET_TIMING_MODES = 4,
	IBOOT_GET_COLOR_MODES = 5,
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
	bool pattern_requested;
	bool presentation_failed;
	u32 last_frame_swap;
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

static int iboot_parse_swap_begin(const u8 *reply, size_t received, u32 *id)
{
	const u8 *payload;
	size_t size;
	int ret;

	/* The gated 14.7 firmware writes 284 in this wrapper but returns only
	 * the 28-byte Begin record through EPIC. Accept that exact mismatch,
	 * bounded by the actual transport length; never read the absent tail.
	 */
	if (received == 28 && get_unaligned_le32(reply) == 15 &&
	    get_unaligned_le32(reply + 4) == 284) {
		payload = reply + 8;
		size = 20;
	} else {
		ret = iboot_reply_payload(reply, received, 15, &payload, &size);
		if (ret)
			return ret;
	}
	if (size < 20)
		return -EPROTO;
	*id = get_unaligned_le32(payload + 12);
	return 0;
}

static int iboot_parse_modes(const u8 *payload, size_t size, u32 *count)
{
	u32 i;

	if (size < 4)
		return -EPROTO;
	*count = get_unaligned_le32(payload);
	if (*count > (size - 4) / IBOOT_MODE_SIZE)
		return -EPROTO;
	for (i = 0; i < *count; i++)
		if (get_unaligned_le32(payload + 4 + i * IBOOT_MODE_SIZE) > 1)
			return -EPROTO;
	return 0;
}

/*
 * 3840x2160 timings the first pattern may use, best first: the reduced-blanking
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

static int iboot_pattern_params(u64 iova, size_t size, u32 stride)
{
	/* The caller owns address allocation and both DART mappings. */
	if (!iova || (iova & 0x3fff) || stride != IBOOT_PATTERN_STRIDE ||
	    size < (size_t)IBOOT_PATTERN_STRIDE * IBOOT_PATTERN_HEIGHT ||
	    size > DCPEXT_MAX_BUFFER_SIZE || iova >= (1ULL << 42) ||
	    size > (1ULL << 42) - iova)
		return -EINVAL;
	return 0;
}

static void iboot_build_pattern_layer(struct iboot_swap_layer_v13_3 *cmd,
				       u64 iova, u32 stride)
{
	memset(cmd, 0, sizeof(*cmd));
	/* Layer zero, one linear BGRA plane with opaque alpha in the supplied pixels. */
	cmd->layer.planes[0].addr = cpu_to_le64(iova);
	cmd->layer.planes[0].stride = cpu_to_le32(stride);
	cmd->layer.planes[0].addr_format = cpu_to_le32(1);
	cmd->layer.plane_count = cpu_to_le32(1);
	cmd->layer.width = cpu_to_le32(IBOOT_PATTERN_WIDTH);
	cmd->layer.height = cpu_to_le32(IBOOT_PATTERN_HEIGHT);
	cmd->layer.surface_format = cpu_to_le32(1); /* BGRA */
	cmd->layer.colorspace = cpu_to_le32(1); /* sRGB */
	cmd->layer.eotf = cpu_to_le32(1); /* SDR */
	cmd->src[0] = cmd->dst[0] = cpu_to_le32(IBOOT_PATTERN_WIDTH);
	cmd->src[1] = cmd->dst[1] = cpu_to_le32(IBOOT_PATTERN_HEIGHT);
}

static int iboot_build_pattern_request(u32 op, const void *data, size_t data_size,
				       u8 *request, size_t capacity, size_t *size)
{
	size_t expected;

	switch (op) {
	case 2: expected = 1; break; /* SetPower */
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

	if (!dcp->external || !ep)
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
	queue_work(system_unbound_wq, &query->work);
	return 0;
}

/* Observational only: inner status fields for mutation ops are not decoded.
 * AFK currently exposes capacity rather than the actual DMA reply length.
 * Never trust the firmware length to enlarge this fixed diagnostic snapshot.
 */
static void iboot_log_pattern_reply(struct device *dev, u32 op, const u8 *reply)
{
	const u8 *payload;
	size_t size;
	int header = iboot_reply_payload(reply, IBOOT_QUERY_RX_SIZE, op, &payload, &size);

	dev_info(dev, "iBoot command %u EPIC success: inner-op=%u declared-len=%u header-check=%d raw64=%*phN (capacity snapshot, inner status unknown)\n",
		 op, get_unaligned_le32(reply), get_unaligned_le32(reply + 4),
		 header, 64, reply);
}

static int iboot_pattern_command(struct iboot_query *query, u32 op,
				 const void *data, size_t data_size, u8 *reply)
{
	u8 request[16 + sizeof(struct iboot_swap_layer_v13_3)];
	size_t size;
	u32 retcode = 0;
	int ret;

	if (READ_ONCE(query->stopping) || READ_ONCE(query->service->torndown))
		return -ENODEV;
	ret = iboot_build_pattern_request(op, data, data_size, request, sizeof(request), &size);
	if (ret)
		return ret;
	memset(reply, 0, IBOOT_QUERY_RX_SIZE);
	ret = afk_send_command(query->service, EPIC_SUBTYPE_STD_SERVICE,
			       request, size, reply, IBOOT_QUERY_RX_SIZE, &retcode);
	if (retcode) {
		dev_warn(query->service->ep->dcp->dev,
			 "iBoot pattern command %u returned %#x\n", op, retcode);
		return -EIO;
	}
	if (!ret)
		iboot_log_pattern_reply(query->service->ep->dcp->dev, op, reply);
	return ret;
}

/* Unlike startup diagnostics, presentation needs the actual received length
 * before trusting the swap id. Mutation operations may have no DMA body;
 * their outer EPIC return code carries the operation's success or failure.
 */
static int iboot_frame_command(struct iboot_query *query, u32 op,
			       const void *data, size_t data_size, u8 *reply,
			       size_t *received)
{
	u8 request[16 + sizeof(struct iboot_swap_layer_v13_3)];
	size_t size;
	u32 retcode = 0;
	int ret;

	if (READ_ONCE(query->stopping) || READ_ONCE(query->service->torndown))
		return -ENODEV;
	ret = iboot_build_pattern_request(op, data, data_size, request,
					 sizeof(request), &size);
	if (ret)
		return ret;
	ret = afk_send_command_with_reply_len(query->service, EPIC_SUBTYPE_STD_SERVICE,
			request, size, reply, IBOOT_QUERY_RX_SIZE, &retcode, received);
	if (ret)
		return ret;
	return retcode ? -EIO : 0;
}

int ibootep_present_frame(struct apple_dcp *dcp, u64 iova, size_t size, u32 stride)
{
	struct iboot_swap_layer_v13_3 layer;
	struct iboot_query *query;
	__le32 end[3] = {}, wait[4] = {};
	size_t received;
	u32 id = 0, step = 15;
	u8 *reply;
	int ret;

	ret = iboot_pattern_params(iova, size, stride);
	if (ret)
		return ret;
	if (dcp->fw_compat != DCP_FIRMWARE_V_14_7)
		return -EOPNOTSUPP;
	query = iboot_find_query(dcp);
	if (IS_ERR(query))
		return PTR_ERR(query);
	might_sleep();
	if (atomic_cmpxchg(&query->busy, 0, 1))
		return -EBUSY;
	if (!query->pattern_requested || query->presentation_failed) {
		ret = -EIO;
		goto done;
	}
	reply = kzalloc(IBOOT_QUERY_RX_SIZE, GFP_KERNEL);
	if (!reply) {
		ret = -ENOMEM;
		goto done;
	}
	ret = iboot_frame_command(query, step, NULL, 0, reply, &received);
	if (ret)
		goto failed;
	ret = iboot_parse_swap_begin(reply, received, &id);
	if (ret)
		goto failed;
	if (!id || id == query->last_frame_swap) {
		ret = -EPROTO;
		goto failed;
	}
	iboot_build_pattern_layer(&layer, iova, stride);
	step = 16;
	ret = iboot_frame_command(query, step, &layer, sizeof(layer), reply, &received);
	if (ret)
		goto failed;
	step = 18;
	ret = iboot_frame_command(query, step, end, sizeof(end), reply, &received);
	if (ret)
		goto failed;
	/* Display zero, exact swap id, wait for completion, no extra delay.
	 * Mode 1 waits while this id remains in the pending/current transaction
	 * queue. It does not wait for the newly displayed frame to be replaced.
	 * AFK bounds the host wait; on timeout neither buffer may be reused.
	 */
	wait[1] = cpu_to_le32(id);
	wait[2] = cpu_to_le32(1);
	step = 19;
	ret = iboot_frame_command(query, step, wait, sizeof(wait), reply, &received);
	if (ret)
		goto failed;
	query->last_frame_swap = id;
	kfree(reply);
	goto done;
failed:
	query->presentation_failed = true;
	dev_err(dcp->dev, "external frame swap %u failed at step %u: %d; buffers retained, no retry\n",
		id, step, ret);
	kfree(reply);
done:
	atomic_set_release(&query->busy, 0);
	return ret;
}

int ibootep_rearm_pattern(struct apple_dcp *dcp)
{
	struct iboot_query *query = iboot_find_query(dcp);

	if (IS_ERR(query))
		return PTR_ERR(query);
	if (atomic_read(&query->busy))
		return -EBUSY;
	WRITE_ONCE(query->pattern_requested, false);
	return 0;
}

int ibootep_present_pattern(struct apple_dcp *dcp, u64 iova, size_t size, u32 stride)
{
	struct iboot_query *query;
	struct iboot_swap_layer_v13_3 layer;
	u8 modes[2 * IBOOT_MODE_SIZE], power = 1, end[12] = {};
	const u8 *payload;
	u8 *reply;
	size_t payload_size;
	u32 step = IBOOT_GET_HPD, swap_id = 0;
	int ret;

	ret = iboot_pattern_params(iova, size, stride);
	if (ret)
		return ret;
	query = iboot_find_query(dcp);
	if (IS_ERR(query))
		return PTR_ERR(query);
	if (dcp->fw_compat != DCP_FIRMWARE_V_14_7)
		return -EOPNOTSUPP;
	/* The caller uses its own sleepable worker, not the ordered AFK RX queue. */
	might_sleep();
	if (atomic_cmpxchg(&query->busy, 0, 1))
		return -EBUSY;
	if (query->pattern_requested) {
		ret = -EALREADY;
		goto done;
	}
	/* Even failure is one-shot; a timeout leaves the firmware state uncertain. */
	query->pattern_requested = true;
	reply = kzalloc(IBOOT_QUERY_RX_SIZE, GFP_KERNEL);
	if (!reply) {
		ret = -ENOMEM;
		goto done;
	}
	ret = iboot_refresh_modes(query, reply);
	if (ret)
		goto free;
	if (!query->timing_valid || !query->color_valid) {
		ret = -ENODATA;
		goto free;
	}
	memcpy(modes, query->timing, IBOOT_MODE_SIZE);
	memcpy(modes + IBOOT_MODE_SIZE, query->color, IBOOT_MODE_SIZE);
	iboot_build_pattern_layer(&layer, iova, stride);
	step = 2;
	ret = iboot_pattern_command(query, step, &power, sizeof(power), reply);
	if (ret)
		goto free;
	/* Match m1n1's Sonoma power-on settling interval before changing timing. */
	msleep(100);
	step = 6;
	ret = iboot_pattern_command(query, step, modes, sizeof(modes), reply);
	if (ret)
		goto free;
	step = 15;
	ret = iboot_pattern_command(query, step, NULL, 0, reply);
	if (ret)
		goto free;
	ret = iboot_reply_payload(reply, IBOOT_QUERY_RX_SIZE, step, &payload, &payload_size);
	if (ret || payload_size < 20) {
		ret = -EPROTO;
		goto free;
	}
	swap_id = get_unaligned_le32(payload + 12);
	step = 16;
	ret = iboot_pattern_command(query, step, &layer, sizeof(layer), reply);
	if (ret)
		goto free;
	step = 18;
	ret = iboot_pattern_command(query, step, end, sizeof(end), reply);
	if (!ret)
		dev_info(dcp->dev, "iBoot EPIC sequence completed for 3840x2160 BGRA pattern; physical scanout unverified, stride %u IOVA %#llx swap %u; buffer retained\n",
			 stride, iova, swap_id);
free:
	if (ret)
		dev_warn(dcp->dev, "iBoot first-pattern step %u failed: %d; no retry, buffer must remain mapped\n",
			 step, ret);
	kfree(reply);
done:
	atomic_set_release(&query->busy, 0);
	return ret;
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

	if (!dcp->external)
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
	/* Remove the sysfs entry before cancelling work during device cleanup. */
	ret = devm_device_add_group(dcp->dev, &iboot_query_group);
	if (ret) {
		dev_warn(dcp->dev, "iBoot query interface unavailable: %d\n", ret);
		return;
	}
	smp_store_release(&service->cookie, query);
	ret = dcpext_scanout_register(dcp);
	if (ret && ret != -ENODEV)
		dev_warn(dcp->dev, "external pattern interface unavailable: %d\n", ret);
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

int ibootep_init(struct apple_dcp *dcp)
{
	int ret;

	if (!of_machine_is_compatible("apple,t6030") &&
	    !of_machine_is_compatible("apple,t6031") &&
	    !of_machine_is_compatible("apple,t6032") &&
	    !of_machine_is_compatible("apple,t8122")) {
		dcp->ibootep = afk_init(dcp, DISP0_ENDPOINT, ibootep_ops);
		afk_start(dcp->ibootep);
		return 0;
	}

	if (dcp->ibootep)
		return -EBUSY;
	dcp->ibootep = afk_init(dcp, DISP0_ENDPOINT, ibootep_ops);
	if (IS_ERR(dcp->ibootep)) {
		ret = PTR_ERR(dcp->ibootep);
		dcp->ibootep = NULL;
		return ret;
	}

	return afk_start(dcp->ibootep);
}
