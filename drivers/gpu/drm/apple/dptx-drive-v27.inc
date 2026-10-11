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
