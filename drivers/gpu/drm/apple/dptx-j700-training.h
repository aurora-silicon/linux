/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Aurora Silicon */
#ifndef APPLE_DPTX_J700_TRAINING_H
#define APPLE_DPTX_J700_TRAINING_H

/* Active lanes may be zero during the firmware's training reset. */
static int dptx_j700_validate_lanes(struct dptx_port *port, u64 requested)
{
	union phy_configure_opts capacity = {};
	int ret;

	if (!port || !port->atcphy)
		return -ENODEV;
	if (requested > 4 || requested == 3)
		return -EINVAL;
	ret = phy_validate(port->atcphy, PHY_MODE_DP, 0, &capacity);
	if (ret)
		return ret;
	return requested <= capacity.dp.lanes ? 0 : -EINVAL;
}

static int dptx_j700_training(struct apple_epic_service *service, bool write,
			      const void *data, size_t size, void *reply, size_t room)
{
	struct dptx_port *port = service->cookie;
	union phy_configure_opts options = {};
	u32 count;
	int ret;

	if (!port || !port->atcphy)
		return -ENODEV;
	if (size < 32 || room < 32)
		return -EINVAL;
	count = get_unaligned_le32(data + (write ? 16 : 0));
	if (count > 4 || (write && (!count || count == 3)) || room < 32 + 12 * count ||
	    (write && size < 32 + 12 * count))
		return -EINVAL;
	if (write) {
		ret = phy_validate(port->atcphy, PHY_MODE_DP, 0, &options);
		if (ret)
			return ret;
		if (count > options.dp.lanes)
			return -EINVAL;
		for (unsigned int i = 0; i < count; i++) {
			const u8 *entry = data + 32 + 12 * i;

			if (get_unaligned_le32(entry) || get_unaligned_le32(entry + 4) > 3 ||
			    get_unaligned_le32(entry + 8) > 3)
				return -EINVAL;
			options.dp.voltage[i] = get_unaligned_le32(entry + 4);
			options.dp.pre[i] = get_unaligned_le32(entry + 8);
		}
		options.dp.lanes = count;
		options.dp.set_voltages = true;
		ret = phy_configure(port->atcphy, &options);
		if (ret)
			return ret;
		memcpy(port->trained_voltage, options.dp.voltage, sizeof(port->trained_voltage));
		memcpy(port->trained_pre, options.dp.pre, sizeof(port->trained_pre));
	}
	memset(reply, 0, room);
	put_unaligned_le32(count, reply + (write ? 16 : 0));
	for (unsigned int i = 0; i < count; i++) {
		put_unaligned_le32(port->trained_voltage[i], reply + 36 + 12 * i);
		put_unaligned_le32(port->trained_pre[i], reply + 40 + 12 * i);
	}
	return 0;
}

#endif
