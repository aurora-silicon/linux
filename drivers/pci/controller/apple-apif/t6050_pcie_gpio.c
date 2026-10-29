// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include "t6050_pcie_gpio.h"

int t6050_pcie_gpio_perst(const struct t6050_gpio_pad_io *io, bool asserted)
{
	u32 old, desired, first, verify;
	int ret;
	if (!io || !io->read || !io->write)
		return -EINVAL;
	ret = io->read(io->context, &old);
	if (ret)
		return ret;

	if (old & (1u << 21))
		return -EIO;
	desired = (old & ~0x27fu) | 0x202u | !asserted;
	first = (desired & ~0x200u) | (old & 0x200u);
	ret = io->write(io->context, first);
	if (ret)
		return ret;
	if (!(old & 0x200u)) {
		ret = io->write(io->context, desired);
		if (ret)
			return ret;
	}
	ret = io->read(io->context, &verify);
	if (ret)
		return ret;

	return ((verify ^ desired) & 0x27fu) ? -EIO : 0;
}
