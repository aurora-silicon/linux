// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include "t6050_pcie_sequence.h"

static int rmw(const struct t6050_pcie_io *io, unsigned int reg,
	       unsigned int off, u32 clear, u32 set)
{
	u32 v;
	int ret = io->read(io->context, reg, off, &v);
	return ret ? ret : io->write(io->context, reg, off, (v & ~clear) | set);
}

int t6050_pcie_reset_port0(const struct t6050_pcie_io *io)
{
	static const struct {
		unsigned int off;
		u32 value;
	} first[] = {
        {0x88,0x110}, {0x100,0xffffffff}, {0x148,0xffffffff}, {0x210,0xffffffff},
        {0x80,0}, {0x84,0}, {0x104,0xfffffff0}, {0x124,0x100}, {0x16c,0},
        {0x13c,0}, {0x800,0x100100}, {0x808,0x1001ff}, {0x82c,0x10000},
    }, last[] = {
        {0x130,0x03020000}, {0x140,0x10}, {0x144,0x253770},
        {0x21c,0}, {0x834,0}, {0x83c,0},
    };
	int ret;
	if (!io || !io->write)
		return -EINVAL;

	for (unsigned int i = 0; i < sizeof(first) / sizeof(first[0]); i++) {
		ret = io->write(io->context, T6050_PCIE_PORT0, first[i].off,
				first[i].value);
		if (ret)
			return ret;
	}
	for (unsigned int i = 0; i < 64; i++) {
		ret = io->write(io->context, T6050_PCIE_PORT0, 0x3000 + 4 * i,
				0);
		if (ret)
			return ret;
	}
	for (unsigned int i = 0; i < 256; i++) {
		ret = io->write(io->context, T6050_PCIE_PORT0, 0x3800 + 4 * i,
				0);
		if (ret)
			return ret;
	}
	for (unsigned int i = 0; i < sizeof(last) / sizeof(last[0]); i++) {
		ret = io->write(io->context, T6050_PCIE_PORT0, last[i].off,
				last[i].value);
		if (ret)
			return ret;
	}
	return 0;
}

int t6050_pcie_enable_port0(const struct t6050_pcie_io *io,
			    unsigned int timeout_us)
{
	u32 v;
	unsigned int waited = 0;
	int ret;
	if (!io || !io->read || !io->write || !io->poll || !io->delay ||
	    !io->apply_port_tunables || !timeout_us)
		return -EINVAL;
	ret = t6050_pcie_reset_port0(io);
	if (ret)
		return ret;

	ret = io->read(io->context, T6050_PCIE_PORT0, 0x82c, &v);
	if (ret)
		return ret;
	if (!(v & (1u << 16)))
		return -EIO;
	ret = io->apply_port_tunables(io->context, "apcie-config-tunables",
				      T6050_PCIE_PORT0);
	if (ret)
		return ret;
	ret = rmw(io, T6050_PCIE_PORT0, 0x800, 0, 1);
	if (ret)
		return ret;

	for (;;) {
		ret = io->read(io->context, T6050_PCIE_PORT0, 0xa8, &v);
		if (ret)
			return ret;
		if (!(v & 1) || !(v & (1u << 16)))
			break;
		if (waited++ >= timeout_us || waited > 10000)
			return -ETIMEDOUT;
		io->delay(io->context, 1);
	}
	ret = rmw(io, T6050_PCIE_PORT0, 0x82c, 0, 1);
	if (ret)
		return ret;
	ret = io->poll(io->context, T6050_PCIE_PORT0, 0xa8, 1, 1,
		       timeout_us < 20000 ? timeout_us : 20000);
	if (ret)
		return ret;
	ret = rmw(io, T6050_PCIE_PORT0, 0x82c, 1u << 16, 0);
	if (ret)
		return ret;
	ret = io->poll(io->context, T6050_PCIE_PORT0, 0x804, 1, 1, timeout_us);
	if (ret)
		return ret;

	ret = rmw(io, T6050_PCIE_PORT0, 0x800, 0, 0x100);
	if (ret)
		return ret;
	return io->poll(io->context, T6050_PCIE_PORT0, 0x208, 4, 0, timeout_us);
}

static int find_pcie_cap(const struct t6050_pcie_io *io, unsigned int *cap)
{
	u32 v;
	int ret = io->read(io->context, T6050_PCIE_ECAM, 0x34, &v);
	if (ret)
		return ret;
	unsigned int off = v & 0xff;
	for (unsigned int i = 0; i < 48 && off; i++) {
		if (off < 0x40 || (off & 3))
			return -EIO;
		ret = io->read(io->context, T6050_PCIE_ECAM, off, &v);
		if (ret)
			return ret;
		if ((v & 0xff) == 0x10) {
			*cap = off;
			return 0;
		}
		off = (v >> 8) & 0xff;
	}
	return -EIO;
}

int t6050_pcie_configure_port0(const struct t6050_pcie_io *io,
			       unsigned int max_speed)
{
	static const char *const shadow[] = { "pcie-rc-gen3-shadow-tunables",
					      "pcie-rc-gen4-shadow-tunables",
					      "pcie-rc-gen5-shadow-tunables" };
	const u32 select[] = { 0, 1u << 24, 0 };
	unsigned int cap;
	u32 v;
	u16 v16;
	int ret;
	if (!io || !io->read || !io->write || !io->read16 || !io->write16 ||
	    !io->apply_port_tunables || max_speed != 3)
		return -EINVAL;
	ret = find_pcie_cap(io, &cap);
	if (ret)
		return ret;
	ret = rmw(io, T6050_PCIE_ECAM, 0x8bc, 0, 1);
	if (ret)
		return ret;
	ret = io->apply_port_tunables(io->context, "pcie-rc-tunables",
				      T6050_PCIE_ECAM);
	if (ret)
		goto lock;

	for (unsigned int i = 0; i < 3; i++) {
		ret = rmw(io, T6050_PCIE_ECAM, 0x890, 3u << 24, select[i]);
		if (ret)
			goto lock;
		ret = io->apply_port_tunables(io->context, shadow[i],
					      T6050_PCIE_ECAM);
		if (ret)
			goto lock;
	}
	ret = rmw(io, T6050_PCIE_ECAM, 0x194, 1, 1);
	if (ret)
		goto lock;
	ret = io->read(io->context, T6050_PCIE_ECAM, cap + 0x2c, &v);
	if (ret)
		goto lock;
	if (!(v & (1u << max_speed))) {
		ret = -EIO;
		goto lock;
	}
	ret = io->read16(io->context, T6050_PCIE_ECAM, cap + 0x30, &v16);
	if (ret)
		goto lock;
	ret = io->write16(io->context, T6050_PCIE_ECAM, cap + 0x30,
			  (v16 & ~0xf) | max_speed);
	if (ret)
		goto lock;

	ret = rmw(io, T6050_PCIE_ECAM, 0x710, 0x3fu << 16, 1u << 16);
	if (ret)
		goto lock;
	ret = rmw(io, T6050_PCIE_ECAM, 0x80c, 0x1fu << 8, 1u << 8);
	if (ret)
		goto lock;
	ret = rmw(io, T6050_PCIE_ECAM, cap + 0xc, 0x3fu << 4, 1u << 4);
lock:

{
	int cleanup = rmw(io, T6050_PCIE_ECAM, 0x8bc, 1, 0);
	if (!ret)
		ret = cleanup;
}
	if (ret)
		return ret;
	ret = rmw(io, T6050_PCIE_ECAM, cap + 0x10, 0, 0x400);
	if (ret)
		return ret;
	ret = rmw(io, T6050_PCIE_ECAM, 0x80c, 0, 1u << 17);
	if (ret)
		return ret;

	return io->write(io->context, T6050_PCIE_PORT0_INTR2AXI, 0x80, 1);
}

int t6050_pcie_train_port0(const struct t6050_pcie_io *io,
			   unsigned int timeout_us)
{
	int ret;
	if (!io || !io->read || !io->write || !io->poll || !io->delay ||
	    !io->endpoint_reset || !timeout_us)
		return -EINVAL;

	ret = rmw(io, T6050_PCIE_PORT0_PHY, 0, 0, 1);
	if (ret)
		return ret;
	ret = io->poll(io->context, T6050_PCIE_PORT0_PHY, 0, 4, 4, timeout_us);
	if (ret)
		return ret;
	ret = rmw(io, T6050_PCIE_PORT0_PHY, 0, 0, 2);
	if (ret)
		return ret;
	ret = io->poll(io->context, T6050_PCIE_PORT0_PHY, 0, 8, 8, timeout_us);
	if (ret)
		return ret;
	ret = rmw(io, T6050_PCIE_PORT0_PHY, 0, 0x10, 0);
	if (ret)
		return ret;
	io->delay(io->context, 1);
	ret = rmw(io, T6050_PCIE_PORT0_PHY, 0, 0, 0x200);
	if (ret)
		return ret;
	ret = rmw(io, T6050_PCIE_PORT0_PHY, 0, 0, 0x400);
	if (ret)
		return ret;
	io->delay(io->context, 100);
	ret = io->endpoint_reset(io->context, false);
	if (ret)
		return ret;
	ret = rmw(io, T6050_PCIE_PORT0, 0x80, 0, 1);
	if (ret)
		return ret;
	ret = io->poll(io->context, T6050_PCIE_PORT0, 0x208, 1, 1, timeout_us);
	if (ret)
		return ret;
	io->delay(io->context, 100000);
	return 0;
}
