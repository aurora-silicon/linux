/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 Aurora Silicon */
struct dptxport_drive_v27 {
	__le32 status_or_count;
	u8 reserved0[12];
	__le32 count_or_status;
	u8 reserved1[12];
	struct {
		__le32 type;
		__le32 voltage;
		__le32 pre;
	} lane[];
} __packed;

/*
 * A Thunderbolt DP tunnel has no PHY lanes to drive. DCP still trains the
 * link through it: SetDriveSettings (count at +0x10, entries at +0x20) is
 * answered with status 0 at +0x00 and its entries are kept as sent;
 * GetDriveSettings (count at +0x00) returns status 0 at +0x10 and the kept
 * entries at +0x20, mode words included. Every other byte is the request's,
 * and any value is accepted, so DCP always gets its reply.
 */
static int dptxport_call_drive_tunnel_v27(struct dptx_port *dptx, bool set,
					  const void *data, size_t data_size,
					  void *reply_, size_t reply_size)
{
	const u8 *request = data;
	u8 *reply = reply_;
	u32 count, i;

	if (reply_size < 0x20)
		return -EINVAL;
	memset(reply, 0, reply_size);
	memcpy(reply, request, min(data_size, reply_size));

	if (set) {
		count = data_size >= 0x14 ? get_unaligned_le32(request + 0x10) : 0;
		dptx->tunnel_drive_count = 0;
		for (i = 0; i < count && i < ARRAY_SIZE(dptx->tunnel_drive) &&
			    data_size >= 0x20 + 12 * (i + 1); i++) {
			dptx->tunnel_drive[i][0] = get_unaligned_le32(request + 0x20 + 12 * i);
			dptx->tunnel_drive[i][1] = get_unaligned_le32(request + 0x24 + 12 * i);
			dptx->tunnel_drive[i][2] = get_unaligned_le32(request + 0x28 + 12 * i);
			dptx->tunnel_drive_count = i + 1;
		}
		put_unaligned_le32(0, reply);
		return 0;
	}

	count = data_size >= 4 ? get_unaligned_le32(request) : 0;
	put_unaligned_le32(0, reply + 0x10);
	for (i = 0; i < count && i < ARRAY_SIZE(dptx->tunnel_drive) &&
		    reply_size >= 0x20 + 12 * (i + 1); i++) {
		bool kept = i < dptx->tunnel_drive_count;

		put_unaligned_le32(kept ? dptx->tunnel_drive[i][0] : 0, reply + 0x20 + 12 * i);
		put_unaligned_le32(kept ? dptx->tunnel_drive[i][1] : 0, reply + 0x24 + 12 * i);
		put_unaligned_le32(kept ? dptx->tunnel_drive[i][2] : 0, reply + 0x28 + 12 * i);
	}
	return 0;
}

static int dptxport_call_drive_v27(struct apple_epic_service *service, bool set,
				 const void *data, size_t data_size,
				 void *reply_, size_t reply_size)
{
	const struct dptxport_drive_v27 *request = data;
	struct dptxport_drive_v27 *reply = reply_;
	struct dptx_port *dptx = service->cookie;
	union phy_configure_opts opts = {};
	u32 count;
	size_t size;
	int ret;

	if (service->ep->dcp->dptx_tunnel)
		return dptxport_call_drive_tunnel_v27(dptx, set, data, data_size,
						      reply_, reply_size);
	if (data_size < sizeof(*request) || reply_size < sizeof(*reply))
		return -EINVAL;
	count = le32_to_cpu(set ? request->count_or_status : request->status_or_count);
	if (count > 4 || count > dptx->lane_count)
		return -EINVAL;
	size = sizeof(*request) + count * sizeof(request->lane[0]);
	if ((set && data_size < size) || (!set && reply_size < size))
		return -EINVAL;

	if (set) {
		for (u32 i = 0; i < count; i++) {

			if (le32_to_cpu(request->lane[i].type) ||
			    le32_to_cpu(request->lane[i].voltage) > 3 ||
			    le32_to_cpu(request->lane[i].pre) > 3 ||
			    le32_to_cpu(request->lane[i].voltage) +
			    le32_to_cpu(request->lane[i].pre) > 3)
				return -EINVAL;
			opts.dp.voltage[i] = le32_to_cpu(request->lane[i].voltage);
			opts.dp.pre[i] = le32_to_cpu(request->lane[i].pre);
		}
		opts.dp.lanes = count;
		opts.dp.link_rate = dptx->phy_ops.dp.link_rate;
		opts.dp.set_voltages = 1;
		if (count && !service->ep->dcp->dptx_tunnel) {
			if (!dptx->atcphy)
				return -ENODEV;
			ret = phy_configure(dptx->atcphy, &opts);
			if (ret)
				return ret;
		}
		for (u32 i = 0; i < count; i++) {
			dptx->drive_voltage[i] = opts.dp.voltage[i];
			dptx->drive_pre[i] = opts.dp.pre[i];
		}
	}

	memset(reply, 0, reply_size);
	if (set) {
		reply->count_or_status = cpu_to_le32(count);
	} else {

		reply->status_or_count = cpu_to_le32(count);
	}

	if (reply_size >= size) {
		for (u32 i = 0; i < count; i++) {
			reply->lane[i].voltage = cpu_to_le32(dptx->drive_voltage[i]);
			reply->lane[i].pre = cpu_to_le32(dptx->drive_pre[i]);
		}
	}
	return 0;
}
