// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include "t6050_pcie_sequence.h"

int t6050_pcie_configure_msi_port0(const struct t6050_pcie_io *io, u64 doorbell,
				   unsigned int count)
{
	int ret;
	if (!io || !io->write || (doorbell & 0xfff) || !count || count > 256)
		return -EINVAL;
	ret = io->write(io->context, T6050_PCIE_PORT0, 0x124, 1);
	if (ret)
		return ret;
	ret = io->write(io->context, T6050_PCIE_PORT0, 0x16c, (u32)doorbell);
	if (ret)
		return ret;
	ret = io->write(io->context, T6050_PCIE_PORT0, 0x170,
			(u32)(doorbell >> 32));
	if (ret)
		return ret;
	for (unsigned int i = 0; i < count; i++) {
		ret = io->write(io->context, T6050_PCIE_PORT0, 0x3800 + 4 * i,
				0x80000000 | i);
		if (ret)
			return ret;
	}
	return 0;
}
