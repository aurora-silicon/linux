// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Apple dptx PHY driver
 *
 * Copyright (C) The Asahi Linux Contributors
 * Author: Janne Grunau <j@jannau.net>
 *
 * based on drivers/phy/apple/atc.c
 *
 * Copyright (C) The Asahi Linux Contributors
 * Author: Sven Peter <sven@svenpeter.dev>
 */

#include "dptx.h"

#include <asm/io.h>
#include "linux/of.h"
#include <dt-bindings/phy/phy.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-dp.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/types.h>

#define DPTX_MAX_LANES    4
#define DPTX_LANE0_OFFSET 0x5000
#define DPTX_LANE_STRIDE  0x1000
#define DPTX_LANE_END     (DPTX_LANE0_OFFSET + DPTX_MAX_LANES * DPTX_LANE_STRIDE)

enum apple_dptx_type {
	DPTX_PHY_T8112,
	DPTX_PHY_T6020,
	DPTX_PHY_T6050,
};

/* The PLL core calibration the board describes, in t6050_pll_core_cfg() order. */
enum {
	T6050_DCOCFG,
	T6050_DTCVREG,
	T6050_DTCCAL,
	T6050_DCDADJ,
	T6050_DLFCFG,
	T6050_RODCOROLEAKCOMP,
	T6050_NR_CAL,
};

static const char *const t6050_cal_props[T6050_NR_CAL] = {
	"apple,pll-dcocfg", "apple,pll-dtcvreg", "apple,pll-dtccal",
	"apple,pll-dcdadj", "apple,pll-dlfcfg", "apple,pll-rodcoroleakcomp",
};

struct apple_dptx_phy_hw {
	enum apple_dptx_type type;
};

struct apple_dptx_phy {
	struct device *dev;

	struct apple_dptx_phy_hw hw;

	int dp_link_rate;

	struct {
		void __iomem *core;
		void __iomem *dptx;
	} regs;

	/* T6050 */
	u32 cal[T6050_NR_CAL];
	bool cal_set[T6050_NR_CAL];
	u32 source_select;
	bool pclkdrv_select;
	bool active;
	u32 rate;		/* Mbit/s; 0: none programmed */

	struct phy *phy_dp;
	struct phy_provider *phy_provider;

	struct mutex lock;

	// TODO: m1n1 port things to clean up
	u32 active_lanes;
};


static inline void mask32(void __iomem *reg, u32 mask, u32 set)
{
	u32 value = readl(reg);
	value &= ~mask;
	value |= set;
	writel(value, reg);
}

static inline void set32(void __iomem *reg, u32 set)
{
	mask32(reg, 0, set);
}

static inline void clear32(void __iomem *reg, u32 clear)
{
	mask32(reg, clear, 0);
}


static int dptx_phy_set_active_lane_count(struct apple_dptx_phy *phy, u32 num_lanes)
{
	u32 l, ctrl;

	dev_dbg(phy->dev, "set_active_lane_count(%u)\n", num_lanes);

	if (num_lanes == 3 || num_lanes > DPTX_MAX_LANES)
		return -1;

	ctrl = readl(phy->regs.dptx + 0x4000);
	writel(ctrl, phy->regs.dptx + 0x4000);

	for (l = 0; l < num_lanes; l++) {
		u64 offset = 0x5000 + 0x1000 * l;
		readl(phy->regs.dptx + offset);
		writel(0x100, phy->regs.dptx + offset);
	}
	for (; l < DPTX_MAX_LANES; l++) {
		u64 offset = 0x5000 + 0x1000 * l;
		readl(phy->regs.dptx + offset);
		writel(0x300, phy->regs.dptx + offset);
	}
	for (l = 0; l < num_lanes; l++) {
		u64 offset = 0x5000 + 0x1000 * l;
		readl(phy->regs.dptx + offset);
		writel(0x0, phy->regs.dptx + offset);
	}
	for (; l < DPTX_MAX_LANES; l++) {
		u64 offset = 0x5000 + 0x1000 * l;
		readl(phy->regs.dptx + offset);
		writel(0x300, phy->regs.dptx + offset);
	}

	if (num_lanes > 0) {
		// clear32(phy->regs.dptx + 0x4000, 0x4000000);
		ctrl = readl(phy->regs.dptx + 0x4000);
		ctrl &= ~0x4000000;
		writel(ctrl, phy->regs.dptx + 0x4000);
	}
	phy->active_lanes = num_lanes;

	return 0;
}

static int dptx_phy_activate(struct apple_dptx_phy *phy, u32 dcp_index)
{
	u32 val_2014;
	u32 val_4008;
	u32 val_4408;

	dev_dbg(phy->dev, "activate(dcp:%u)\n", dcp_index);

	// MMIO: R.4   0x23c500010 (dptx-phy[1], offset 0x10) = 0x0
	// MMIO: W.4   0x23c500010 (dptx-phy[1], offset 0x10) = 0x0
	readl(phy->regs.core + 0x10);
	writel(dcp_index, phy->regs.core + 0x10);

	// MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x444
	// MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x454
	set32(phy->regs.core + 0x48, 0x010);
	// MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x454
	// MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x474
	set32(phy->regs.core + 0x48, 0x020);
	// MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x474
	// MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x434
	clear32(phy->regs.core + 0x48, 0x040);
	// MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x434
	// MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x534
	set32(phy->regs.core + 0x48, 0x100);
	// MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x534
	// MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x734
	set32(phy->regs.core + 0x48, 0x200);
	// MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x734
	// MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x334
	clear32(phy->regs.core + 0x48, 0x400);
	// MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x334
	// MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x335
	set32(phy->regs.core + 0x48, 0x001);
	// MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x335
	// MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x337
	set32(phy->regs.core + 0x48, 0x002);
	// MMIO: R.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x337
	// MMIO: W.4   0x23c500048 (dptx-phy[1], offset 0x48) = 0x333
	clear32(phy->regs.core + 0x48, 0x004);

	// MMIO: R.4   0x23c542014 (dptx-phy[0], offset 0x2014) = 0x80a0c
	val_2014 = readl(phy->regs.dptx + 0x2014);
	// MMIO: W.4   0x23c542014 (dptx-phy[0], offset 0x2014) = 0x300a0c
	writel((0x30 << 16) | (val_2014 & 0xffff), phy->regs.dptx + 0x2014);

	// MMIO: R.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x644800
	// MMIO: W.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
	set32(phy->regs.dptx + 0x20b8, 0x010000);

	// MMIO: R.4   0x23c542220 (dptx-phy[0], offset 0x2220) = 0x11090a2
	// MMIO: W.4   0x23c542220 (dptx-phy[0], offset 0x2220) = 0x11090a0
	clear32(phy->regs.dptx + 0x2220, 0x0000002);

	// MMIO: R.4   0x23c54222c (dptx-phy[0], offset 0x222c) = 0x103003
	// MMIO: W.4   0x23c54222c (dptx-phy[0], offset 0x222c) = 0x103803
	set32(phy->regs.dptx + 0x222c, 0x000800);
	// MMIO: R.4   0x23c54222c (dptx-phy[0], offset 0x222c) = 0x103803
	// MMIO: W.4   0x23c54222c (dptx-phy[0], offset 0x222c) = 0x103903
	set32(phy->regs.dptx + 0x222c, 0x000100);

	// MMIO: R.4   0x23c542230 (dptx-phy[0], offset 0x2230) = 0x2308804
	// MMIO: W.4   0x23c542230 (dptx-phy[0], offset 0x2230) = 0x2208804
	clear32(phy->regs.dptx + 0x2230, 0x0100000);

	// MMIO: R.4   0x23c542278 (dptx-phy[0], offset 0x2278) = 0x18300811
	// MMIO: W.4   0x23c542278 (dptx-phy[0], offset 0x2278) = 0x10300811
	clear32(phy->regs.dptx + 0x2278, 0x08000000);

	// MMIO: R.4   0x23c5422a4 (dptx-phy[0], offset 0x22a4) = 0x1044200
	// MMIO: W.4   0x23c5422a4 (dptx-phy[0], offset 0x22a4) = 0x1044201
	set32(phy->regs.dptx + 0x22a4, 0x0000001);

	// MMIO: R.4   0x23c544008 (dptx-phy[0], offset 0x4008) = 0x18030
	val_4008 = readl(phy->regs.dptx + 0x4008);
	// MMIO: W.4   0x23c544008 (dptx-phy[0], offset 0x4008) = 0x30030
	writel((0x6 << 15) | (val_4008 & 0x7fff), phy->regs.dptx + 0x4008);
	// MMIO: R.4   0x23c544008 (dptx-phy[0], offset 0x4008) = 0x30030
	// MMIO: W.4   0x23c544008 (dptx-phy[0], offset 0x4008) = 0x30010
	clear32(phy->regs.dptx + 0x4008, 0x00020);

	// MMIO: R.4   0x23c54420c (dptx-phy[0], offset 0x420c) = 0x88e3
	// MMIO: W.4   0x23c54420c (dptx-phy[0], offset 0x420c) = 0x88c3
	clear32(phy->regs.dptx + 0x420c, 0x0020);

	// MMIO: R.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x0
	// MMIO: W.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000000
	set32(phy->regs.dptx + 0x4600, 0x8000000);

	// MMIO: R.4   0x23c545040 (dptx-phy[0], offset 0x5040) = 0x21780
	// MMIO: W.4   0x23c545040 (dptx-phy[0], offset 0x5040) = 0x221780
	// MMIO: R.4   0x23c546040 (dptx-phy[0], offset 0x6040) = 0x21780
	// MMIO: W.4   0x23c546040 (dptx-phy[0], offset 0x6040) = 0x221780
	// MMIO: R.4   0x23c547040 (dptx-phy[0], offset 0x7040) = 0x21780
	// MMIO: W.4   0x23c547040 (dptx-phy[0], offset 0x7040) = 0x221780
	// MMIO: R.4   0x23c548040 (dptx-phy[0], offset 0x8040) = 0x21780
	// MMIO: W.4   0x23c548040 (dptx-phy[0], offset 0x8040) = 0x221780
	for (u32 loff = DPTX_LANE0_OFFSET; loff < DPTX_LANE_END;
	     loff += DPTX_LANE_STRIDE)
		set32(phy->regs.dptx + loff + 0x40, 0x200000);

	// MMIO: R.4   0x23c545040 (dptx-phy[0], offset 0x5040) = 0x221780
	// MMIO: W.4   0x23c545040 (dptx-phy[0], offset 0x5040) = 0x2a1780
	// MMIO: R.4   0x23c546040 (dptx-phy[0], offset 0x6040) = 0x221780
	// MMIO: W.4   0x23c546040 (dptx-phy[0], offset 0x6040) = 0x2a1780
	// MMIO: R.4   0x23c547040 (dptx-phy[0], offset 0x7040) = 0x221780
	// MMIO: W.4   0x23c547040 (dptx-phy[0], offset 0x7040) = 0x2a1780
	// MMIO: R.4   0x23c548040 (dptx-phy[0], offset 0x8040) = 0x221780
	// MMIO: W.4   0x23c548040 (dptx-phy[0], offset 0x8040) = 0x2a1780
	for (u32 loff = DPTX_LANE0_OFFSET; loff < DPTX_LANE_END;
	     loff += DPTX_LANE_STRIDE)
		set32(phy->regs.dptx + loff + 0x40, 0x080000);

	// MMIO: R.4   0x23c545244 (dptx-phy[0], offset 0x5244) = 0x18
	// MMIO: W.4   0x23c545244 (dptx-phy[0], offset 0x5244) = 0x8
	// MMIO: R.4   0x23c546244 (dptx-phy[0], offset 0x6244) = 0x18
	// MMIO: W.4   0x23c546244 (dptx-phy[0], offset 0x6244) = 0x8
	// MMIO: R.4   0x23c547244 (dptx-phy[0], offset 0x7244) = 0x18
	// MMIO: W.4   0x23c547244 (dptx-phy[0], offset 0x7244) = 0x8
	// MMIO: R.4   0x23c548244 (dptx-phy[0], offset 0x8244) = 0x18
	// MMIO: W.4   0x23c548244 (dptx-phy[0], offset 0x8244) = 0x8
	for (u32 loff = DPTX_LANE0_OFFSET; loff < DPTX_LANE_END;
	     loff += DPTX_LANE_STRIDE)
		clear32(phy->regs.dptx + loff + 0x244, 0x10);

	// MMIO: R.4   0x23c542214 (dptx-phy[0], offset 0x2214) = 0x1e0
	// MMIO: W.4   0x23c542214 (dptx-phy[0], offset 0x2214) = 0x1e1
	set32(phy->regs.dptx + 0x2214, 0x001);

	// MMIO: R.4   0x23c542224 (dptx-phy[0], offset 0x2224) = 0x20086001
	// MMIO: W.4   0x23c542224 (dptx-phy[0], offset 0x2224) = 0x20086000
	clear32(phy->regs.dptx + 0x2224, 0x00000001);

	// MMIO: R.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2000
	// MMIO: W.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2002
	set32(phy->regs.dptx + 0x2200, 0x0002);

	// MMIO: R.4   0x23c541000 (dptx-phy[0], offset 0x1000) = 0xe0000003
	// MMIO: W.4   0x23c541000 (dptx-phy[0], offset 0x1000) = 0xe0000001
	clear32(phy->regs.dptx + 0x1000, 0x00000002);

	// MMIO: R.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x41
	// MMIO: W.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x49
	set32(phy->regs.dptx + 0x4004, 0x08);

	/* TODO: no idea what happens here, supposedly setting/clearing some bits */
	// MMIO: R.4   0x23c544404 (dptx-phy[0], offset 0x4404) = 0x555d444
	readl(phy->regs.dptx + 0x4404);
	// MMIO: W.4   0x23c544404 (dptx-phy[0], offset 0x4404) = 0x555d444
	writel(0x555d444, phy->regs.dptx + 0x4404);
	// MMIO: R.4   0x23c544404 (dptx-phy[0], offset 0x4404) = 0x555d444
	readl(phy->regs.dptx + 0x4404);
	// MMIO: W.4   0x23c544404 (dptx-phy[0], offset 0x4404) = 0x555d444
	writel(0x555d444, phy->regs.dptx + 0x4404);

	dptx_phy_set_active_lane_count(phy, 0);

	// MMIO: R.4   0x23c544200 (dptx-phy[0], offset 0x4200) = 0x4002430
	// MMIO: W.4   0x23c544200 (dptx-phy[0], offset 0x4200) = 0x4002420
	clear32(phy->regs.dptx + 0x4200, 0x0000010);

	// MMIO: R.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000000
	// MMIO: W.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000000
	clear32(phy->regs.dptx + 0x4600, 0x0000001);
	// MMIO: R.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000000
	// MMIO: W.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000001
	set32(phy->regs.dptx + 0x4600, 0x0000001);
	// MMIO: R.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000001
	// MMIO: W.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000003
	set32(phy->regs.dptx + 0x4600, 0x0000002);
	// MMIO: R.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000043
	// MMIO: R.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000043
	// MMIO: W.4   0x23c544600 (dptx-phy[0], offset 0x4600) = 0x8000041
	/* TODO: read first to check if the previous set(...,0x2) sticked? */
	readl(phy->regs.dptx + 0x4600);
	clear32(phy->regs.dptx + 0x4600, 0x0000001);

	// MMIO: R.4   0x23c544408 (dptx-phy[0], offset 0x4408) = 0x482
	// MMIO: W.4   0x23c544408 (dptx-phy[0], offset 0x4408) = 0x482
	/* TODO: probably a set32 of an already set bit */
	val_4408 = readl(phy->regs.dptx + 0x4408);
	if (val_4408 != 0x482 && val_4408 != 0x483)
		dev_warn(
			phy->dev,
			"unexpected initial value at regs.dptx offset 0x4408: 0x%03x\n",
			val_4408);
	writel(val_4408, phy->regs.dptx + 0x4408);
	// MMIO: R.4   0x23c544408 (dptx-phy[0], offset 0x4408) = 0x482
	// MMIO: W.4   0x23c544408 (dptx-phy[0], offset 0x4408) = 0x483
	set32(phy->regs.dptx + 0x4408, 0x001);

	return 0;
}

static int dptx_phy_deactivate(struct apple_dptx_phy *phy)
{
	return 0;
}

static int dptx_phy_set_link_rate(struct apple_dptx_phy *phy, u32 link_rate)
{
	u32 sts_1008, sts_1014, val_100c, val_20b0, val_20b4;

	dev_dbg(phy->dev, "set_link_rate(%u)\n", link_rate);

	// MMIO: R.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x49
	// MMIO: W.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x49
	set32(phy->regs.dptx + 0x4004, 0x08);

	// MMIO: R.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
	// MMIO: W.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
	clear32(phy->regs.dptx + 0x4000, 0x0000040);

	// MMIO: R.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x49
	// MMIO: W.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x41
	clear32(phy->regs.dptx + 0x4004, 0x08);

	// MMIO: R.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
	// MMIO: W.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
	clear32(phy->regs.dptx + 0x4000, 0x2000000);
	// MMIO: R.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
	// MMIO: W.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
	set32(phy->regs.dptx + 0x4000, 0x1000000);

	// MMIO: R.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2002
	// MMIO: R.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2002
	// MMIO: W.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2000
	/* TODO: what is this read checking for? */
	readl(phy->regs.dptx + 0x2200);
	clear32(phy->regs.dptx + 0x2200, 0x0002);

	// MMIO: R.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf000
	// MMIO: W.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf000
	// MMIO: R.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf000
	// MMIO: W.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf008
	/* TODO: what is the setting/clearing? */
	val_100c = readl(phy->regs.dptx + 0x100c);
	writel(val_100c, phy->regs.dptx + 0x100c);
	set32(phy->regs.dptx + 0x100c, 0x0008);

	// MMIO: R.4   0x23c541014 (dptx-phy[0], offset 0x1014) = 0x1
	sts_1014 = readl(phy->regs.dptx + 0x1014);
	if (sts_1014 != 0x1)
		dev_dbg(phy->dev, "unexpected?: dptx[0x1014]: %02x\n", sts_1014);

	// MMIO: R.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf008
	// MMIO: W.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf000
	clear32(phy->regs.dptx + 0x100c, 0x0008);

	// MMIO: R.4   0x23c541008 (dptx-phy[0], offset 0x1008) = 0x1
	sts_1008 = readl(phy->regs.dptx + 0x1008);
	if (sts_1008 != 0x1)
		dev_dbg(phy->dev, "unexpected?: dptx[0x1008]: %02x\n", sts_1008);

	// MMIO: R.4   0x23c542220 (dptx-phy[0], offset 0x2220) = 0x11090a0
	// MMIO: W.4   0x23c542220 (dptx-phy[0], offset 0x2220) = 0x1109020
	clear32(phy->regs.dptx + 0x2220, 0x0000080);

	// MMIO: R.4   0x23c5420b0 (dptx-phy[0], offset 0x20b0) = 0x1e0e01c2
	// MMIO: W.4   0x23c5420b0 (dptx-phy[0], offset 0x20b0) = 0x1e0e01c2
	val_20b0 = readl(phy->regs.dptx + 0x20b0);
	/* TODO: what happens on dptx-phy */
	if (phy->hw.type == DPTX_PHY_T6020)
		val_20b0 = (val_20b0 & ~0x3ff) | 0x2a3;
	writel(val_20b0, phy->regs.dptx + 0x20b0);

	// MMIO: R.4   0x23c5420b4 (dptx-phy[0], offset 0x20b4) = 0x7fffffe
	// MMIO: W.4   0x23c5420b4 (dptx-phy[0], offset 0x20b4) = 0x7fffffe
	val_20b4 = readl(phy->regs.dptx + 0x20b4);
	/* TODO: what happens on dptx-phy */
	if (phy->hw.type == DPTX_PHY_T6020)
		val_20b4 = (val_20b4 | 0x4000000) & ~0x0008000;
	writel(val_20b4, phy->regs.dptx + 0x20b4);

	// MMIO: R.4   0x23c5420b4 (dptx-phy[0], offset 0x20b4) = 0x7fffffe
	// MMIO: W.4   0x23c5420b4 (dptx-phy[0], offset 0x20b4) = 0x7fffffe
	val_20b4 = readl(phy->regs.dptx + 0x20b4);
	/* TODO: what happens on dptx-phy */
	if (phy->hw.type == DPTX_PHY_T6020)
		val_20b4 = (val_20b4 | 0x0000001) & ~0x0000004;
	writel(val_20b4, phy->regs.dptx + 0x20b4);

	// MMIO: R.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
	// MMIO: W.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
	/* TODO: unclear */
	set32(phy->regs.dptx + 0x20b8, 0);
	// MMIO: R.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
	// MMIO: W.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
	/* TODO: unclear */
	set32(phy->regs.dptx + 0x20b8, 0);
	// MMIO: R.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
	// MMIO: W.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
	/* TODO: unclear */
	if (phy->hw.type == DPTX_PHY_T6020)
		set32(phy->regs.dptx + 0x20b8, 0x010000);
	else
		set32(phy->regs.dptx + 0x20b8, 0);
	// MMIO: R.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x654800
	// MMIO: W.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x454800
	clear32(phy->regs.dptx + 0x20b8, 0x200000);

	// MMIO: R.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x454800
	// MMIO: W.4   0x23c5420b8 (dptx-phy[0], offset 0x20b8) = 0x454800
	/* TODO: unclear */
	set32(phy->regs.dptx + 0x20b8, 0);

	// MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x0
	// MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x8
	// MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x8
	// MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0xc
	// MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0xc
	// MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x4000c
	// MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x4000c
	// MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0xc
	// MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0xc
	// MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x8000c
	// MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x8000c
	// MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0xc
	// MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0xc
	// MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x8
	// MMIO: R.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x8
	// MMIO: W.4   0x23c5000a0 (dptx-phy[1], offset 0xa0) = 0x0
	set32(phy->regs.core + 0xa0, 0x8);
	set32(phy->regs.core + 0xa0, 0x4);
	set32(phy->regs.core + 0xa0, 0x40000);
	clear32(phy->regs.core + 0xa0, 0x40000);
	set32(phy->regs.core + 0xa0, 0x80000);
	clear32(phy->regs.core + 0xa0, 0x80000);
	clear32(phy->regs.core + 0xa0, 0x4);
	clear32(phy->regs.core + 0xa0, 0x8);

	// MMIO: R.4   0x23c542000 (dptx-phy[0], offset 0x2000) = 0x2
	// MMIO: W.4   0x23c542000 (dptx-phy[0], offset 0x2000) = 0x2
	/* TODO: unclear */
	set32(phy->regs.dptx + 0x2000, 0x0);

	// MMIO: R.4   0x23c542018 (dptx-phy[0], offset 0x2018) = 0x0
	// MMIO: W.4   0x23c542018 (dptx-phy[0], offset 0x2018) = 0x0
	clear32(phy->regs.dptx + 0x2018, 0x0);

	// MMIO: R.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf000
	// MMIO: W.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf007
	set32(phy->regs.dptx + 0x100c, 0x0007);
	// MMIO: R.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf007
	// MMIO: W.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf00f
	set32(phy->regs.dptx + 0x100c, 0x0008);

	// MMIO: R.4   0x23c541014 (dptx-phy[0], offset 0x1014) = 0x38f
	sts_1014 = readl(phy->regs.dptx + 0x1014);
	if (sts_1014 != 0x38f)
		dev_dbg(phy->dev, "unexpected?: dptx[0x1014]: %02x\n", sts_1014);

	// MMIO: R.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf00f
	// MMIO: W.4   0x23c54100c (dptx-phy[0], offset 0x100c) = 0xf007
	clear32(phy->regs.dptx + 0x100c, 0x0008);

	// MMIO: R.4   0x23c541008 (dptx-phy[0], offset 0x1008) = 0x9
	sts_1008 = readl(phy->regs.dptx + 0x1008);
	if (sts_1008 != 0x9)
		dev_dbg(phy->dev, "unexpected?: dptx[0x1008]: %02x\n", sts_1008);

	// MMIO: R.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2000
	// MMIO: W.4   0x23c542200 (dptx-phy[0], offset 0x2200) = 0x2002
	set32(phy->regs.dptx + 0x2200, 0x0002);

	// MMIO: R.4   0x23c545010 (dptx-phy[0], offset 0x5010) = 0x18003000
	// MMIO: W.4   0x23c545010 (dptx-phy[0], offset 0x5010) = 0x18003000
	// MMIO: R.4   0x23c546010 (dptx-phy[0], offset 0x6010) = 0x18003000
	// MMIO: W.4   0x23c546010 (dptx-phy[0], offset 0x6010) = 0x18003000
	// MMIO: R.4   0x23c547010 (dptx-phy[0], offset 0x7010) = 0x18003000
	// MMIO: W.4   0x23c547010 (dptx-phy[0], offset 0x7010) = 0x18003000
	// MMIO: R.4   0x23c548010 (dptx-phy[0], offset 0x8010) = 0x18003000
	// MMIO: W.4   0x23c548010 (dptx-phy[0], offset 0x8010) = 0x18003000
	writel(0x18003000, phy->regs.dptx + 0x8010);
	for (u32 loff = DPTX_LANE0_OFFSET; loff < DPTX_LANE_END; loff += DPTX_LANE_STRIDE) {
		u32 val_l010 = readl(phy->regs.dptx + loff + 0x10);
		writel(val_l010, phy->regs.dptx + loff + 0x10);
	}

	// MMIO: R.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x41021ac
	// MMIO: W.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x51021ac
	set32(phy->regs.dptx + 0x4000, 0x1000000);
	// MMIO: R.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x51021ac
	// MMIO: W.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x71021ac
	set32(phy->regs.dptx + 0x4000, 0x2000000);

	// MMIO: R.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x41
	// MMIO: W.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x49
	set32(phy->regs.dptx + 0x4004, 0x08);

	// MMIO: R.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x71021ac
	// MMIO: W.4   0x23c544000 (dptx-phy[0], offset 0x4000) = 0x71021ec
	set32(phy->regs.dptx + 0x4000, 0x0000040);

	// MMIO: R.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x49
	// MMIO: W.4   0x23c544004 (dptx-phy[0], offset 0x4004) = 0x48
	clear32(phy->regs.dptx + 0x4004, 0x01);

	return 0;
}

/*
 * T6050 (M5 Ultra) lpdptx. The two banks are T602x's ("core" and "dptx", the
 * PLL command register at dptx+0x100c), but the PLL calibration, clocking,
 * rate programming, AUX calibration and PHY power are T8122/T8132's. Every
 * write is a field update; nothing below replays a captured register word.
 */
static void t6050_rmw(void __iomem *base, u32 off, u32 shift, u32 mask, u32 val)
{
	mask32(base + off, mask, (val << shift) & mask);
}

#define T6050_D(phy, off, shift, mask, val) \
	t6050_rmw((phy)->regs.dptx, off, shift, mask, val)
#define T6050_C(phy, off, shift, mask, val) \
	t6050_rmw((phy)->regs.core, off, shift, mask, val)

/* core+0x48: PLL (bits 6:4), AUX (10:8) and PHY (2:0) power, 1 us apart. */
static void t6050_block_power(struct apple_dptx_phy *phy, u32 lsb, bool on)
{
	if (on) {
		T6050_C(phy, 0x48, lsb, BIT(lsb), 1);
		udelay(1);
		T6050_C(phy, 0x48, lsb + 1, BIT(lsb + 1), 1);
		udelay(1);
		T6050_C(phy, 0x48, lsb + 2, BIT(lsb + 2), 0);
		udelay(1);
	} else {
		T6050_C(phy, 0x48, lsb + 2, BIT(lsb + 2), 1);
		udelay(1);
		T6050_C(phy, 0x48, lsb + 1, BIT(lsb + 1), 0);
		udelay(1);
		T6050_C(phy, 0x48, lsb, BIT(lsb), 0);
		udelay(1);
	}
}

static void t6050_device_power(struct apple_dptx_phy *phy, bool on)
{
	T6050_C(phy, 0x10, 0, 0x7, phy->source_select);
	t6050_block_power(phy, 4, on);	/* PLL */
	t6050_block_power(phy, 8, on);	/* AUX */
	t6050_block_power(phy, 0, on);	/* PHY */
}

/* The board's PLL core calibration (an absent value leaves its field), then fixed settings. */
static void t6050_pll_core_cfg(struct apple_dptx_phy *phy)
{
	const u32 *v = phy->cal;
	const bool *set = phy->cal_set;

	if (set[T6050_DCOCFG]) {
		T6050_D(phy, 0x2240, 1, 0x6, (v[T6050_DCOCFG] >> 3) & 3);
		T6050_D(phy, 0x2240, 4, 0x70, v[T6050_DCOCFG] & 7);
	}
	if (set[T6050_DTCVREG])
		T6050_D(phy, 0x2230, 0, 0x1f, v[T6050_DTCVREG]);
	if (set[T6050_DTCCAL])
		T6050_D(phy, 0x22e8, 17, 0x60000, v[T6050_DTCCAL]);
	if (set[T6050_DCDADJ])
		T6050_D(phy, 0x2240, 22, 0x7c00000, v[T6050_DCDADJ]);
	if (set[T6050_DLFCFG])
		T6050_D(phy, 0x22b0, 20, 0x700000, v[T6050_DLFCFG]);
	if (set[T6050_RODCOROLEAKCOMP])
		T6050_D(phy, 0x2240, 19, 0x180000, v[T6050_RODCOROLEAKCOMP]);

	T6050_D(phy, 0x22e8, 0, 0x1, 1);
	T6050_D(phy, 0x2240, 3, 0x8, 1);
	T6050_D(phy, 0x2240, 0, 0x1, 1);
	T6050_D(phy, 0x2224, 10, 0xc00, 2);
	T6050_D(phy, 0x22b0, 27, 0x08000000, 0);
	T6050_D(phy, 0x2338, 23, 0x0f800000, 0xf);
}

/* The pixel clock driver: dptx+0x2200 bit 3 with pclkdrv-select, else bit 1. */
static bool t6050_pclk(struct apple_dptx_phy *phy, bool on)
{
	u32 bit = phy->pclkdrv_select ? 3 : 1;

	if (!on) {
		if (!(readl(phy->regs.dptx + 0x2200) & BIT(bit)))
			return false;
	}
	T6050_D(phy, 0x2200, bit, BIT(bit), on);
	return true;
}

static void t6050_configure_pll(struct apple_dptx_phy *phy)
{
	T6050_D(phy, 0x2218, 0, 0x1, 1);
	T6050_D(phy, 0x2234, 0, 0x3, 0);
	t6050_pclk(phy, true);
	T6050_D(phy, 0x2320, 1, 0x6, 1);	/* single-ended clock output */
}

/* AUX impedance calibration: start, sample completion once, copy the result. */
static int t6050_impedance_calibration(struct apple_dptx_phy *phy)
{
	u32 done, code;

	T6050_D(phy, 0x4224, 0, 0x1, 0);
	usleep_range(1000, 1200);
	T6050_D(phy, 0x4224, 0, 0x1, 1);
	udelay(1);
	T6050_D(phy, 0x4200, 4, 0x3f0, 0x3c);
	udelay(1);
	T6050_D(phy, 0x4200, 0, 0x1, 1);
	udelay(125);
	done = readl(phy->regs.dptx + 0x4204) & 1;
	udelay(10);
	T6050_D(phy, 0x4200, 0, 0x1, 0);
	code = (readl(phy->regs.dptx + 0x4210) >> 18) & 0x7f;
	if (!done) {
		dev_err(phy->dev, "impedance calibration did not complete (0x4204 %#x)\n",
			readl(phy->regs.dptx + 0x4204));
		return -ETIMEDOUT;
	}
	T6050_D(phy, 0x4228, 16, 0x7f0000, code);
	T6050_D(phy, 0x4228, 15, 0x8000, 1);
	dev_dbg(phy->dev, "impedance calibration code %#x\n", code);
	return 0;
}

static int t6050_activate(struct apple_dptx_phy *phy)
{
	t6050_device_power(phy, true);
	phy->active = true;
	t6050_pll_core_cfg(phy);	/* PLL top, PHY and AUX configuration: nothing on this board */
	t6050_configure_pll(phy);
	/* PLL activation */
	T6050_D(phy, 0x1000, 1, 0x2, 0);
	T6050_D(phy, 0x4004, 3, 0x8, 1);
	/* PHY activation: every lane, highest first */
	udelay(1);
	for (int l = DPTX_MAX_LANES - 1; l >= 0; l--)
		T6050_D(phy, DPTX_LANE0_OFFSET + DPTX_LANE_STRIDE * l, 9, 0x200, 0);
	/* AUX power-up */
	T6050_D(phy, 0xd000, 0, 0x1, 0);
	return t6050_impedance_calibration(phy);
}

/* One PLL command (7 up, 0 down): request, 100 us, acknowledgment, lock state. */
static int t6050_pll_command(struct apple_dptx_phy *phy, u32 cmd)
{
	bool up = cmd == 7;
	u32 ack, sts;

	T6050_D(phy, 0x100c, 0, 0x7, cmd);
	T6050_D(phy, 0x100c, 3, 0x8, 1);
	udelay(100);
	ack = readl(phy->regs.dptx + 0x1014);
	T6050_D(phy, 0x100c, 3, 0x8, 0);
	sts = readl(phy->regs.dptx + 0x1008);
	if (!(ack & 1)) {
		dev_err(phy->dev, "PLL power %s: no acknowledgment (0x1014 %#x)\n",
			up ? "up" : "down", ack);
		return -ETIMEDOUT;
	}
	if (!!(sts & BIT(3)) != up) {
		dev_err(phy->dev, "PLL power %s: lock is %s (0x1008 %#x)\n",
			up ? "up" : "down", up ? "missing" : "still set", sts);
		return -EIO;
	}
	return 0;
}

static int t6050_pll_power_down(struct apple_dptx_phy *phy)
{
	int ret;

	if (!t6050_pclk(phy, false))
		return 0;	/* already down */
	ret = t6050_pll_command(phy, 0);
	udelay(1);
	return ret;
}

static int t6050_pll_power_up(struct apple_dptx_phy *phy)
{
	int ret = t6050_pll_command(phy, 7);

	if (!ret)
		t6050_pclk(phy, true);
	return ret;
}

static void t6050_phy_power_down(struct apple_dptx_phy *phy)
{
	T6050_D(phy, 0x4004, 0, 0x1, 1);
	T6050_D(phy, 0x4000, 6, 0x40, 0);
	T6050_D(phy, 0x4004, 3, 0x8, 0);
	T6050_D(phy, 0x4000, 25, 0x2000000, 0);
	T6050_D(phy, 0x4000, 24, 0x1000000, 0);
}

static void t6050_phy_power_up(struct apple_dptx_phy *phy)
{
	T6050_D(phy, 0x4004, 3, 0x8, 1);
	for (int l = 0; l < DPTX_MAX_LANES; l++)
		T6050_D(phy, DPTX_LANE0_OFFSET + DPTX_LANE_STRIDE * l + 0x10,
			27, 0x38000000, 4);
	T6050_D(phy, 0x4030, 0, 0x1, 1);	/* TXA LDO low-power mode */
	udelay(1);
	T6050_D(phy, 0x4000, 24, 0x1000000, 1);
	ndelay(250);
	T6050_D(phy, 0x4000, 25, 0x2000000, 1);
	udelay(1);
	T6050_D(phy, 0x4000, 6, 0x40, 1);
	udelay(1);
	T6050_D(phy, 0x4004, 0, 0x1, 0);
}

static int t6050_set_lanes(struct apple_dptx_phy *phy, u32 n)
{
	if (n == 3 || n > DPTX_MAX_LANES)
		return -EINVAL;
	if (!phy->active)
		return 0;
	T6050_D(phy, 0x4000, 26, 0x4000000, 1);
	udelay(1);
	for (u32 l = 0; l < DPTX_MAX_LANES; l++)
		T6050_D(phy, DPTX_LANE0_OFFSET + DPTX_LANE_STRIDE * l, 8, 0x100, n <= l);
	if (n)
		T6050_D(phy, 0x4000, 26, 0x4000000, 0);
	phy->active_lanes = n;
	return 0;
}

/*
 * The PLL record of a link rate (after the T8122 adjustment of its first two
 * words): the integer/fraction divider q08 + q0c / q10, the analog fields and
 * whether the PLL clock output is bypassed.
 */
struct t6050_pll_cfg {
	u32 rate;
	u8 q00, q04, q14, q18, q1c;
	u16 q08, q0c, q10, q24, q28;
	bool bypass;
};

static const struct t6050_pll_cfg t6050_pll_cfgs[] = {
	{ 1620, 1, 0, 1, 5, 0x13, 0x87, 0, 0, 0x80, 0x8804, true },
	{ 2700, 1, 0, 1, 5, 0x09, 0x70, 0x1fff, 0x3ffe, 0x80, 0x8804, true },
	{ 5400, 1, 1, 1, 5, 0x04, 0x70, 0x1fff, 0x3ffe, 0x100, 0x8984, true },
	{ 8100, 1, 1, 1, 6, 0x04, 0xa8, 0x2ffd, 0x3ffc, 0x100, 0x8984, false },
};

static void t6050_shadow_update(struct apple_dptx_phy *phy)
{
	set32(phy->regs.core + 0xa0, 0x8);
	set32(phy->regs.core + 0xa0, 0x4);
	set32(phy->regs.core + 0xa0, 0x40000);
	clear32(phy->regs.core + 0xa0, 0x40000);
	set32(phy->regs.core + 0xa0, 0x80000);
	clear32(phy->regs.core + 0xa0, 0x80000);
	clear32(phy->regs.core + 0xa0, 0x4);
	clear32(phy->regs.core + 0xa0, 0x8);
}

/* Program a link rate's PLL frequency and half-rate fields (the PLL and PHY stay as they are). */
static int t6050_program_rate(struct apple_dptx_phy *phy, u32 rate)
{
	const struct t6050_pll_cfg *c = NULL;
	u32 q10, f, e;
	bool half;

	for (int i = 0; i < ARRAY_SIZE(t6050_pll_cfgs); i++)
		if (t6050_pll_cfgs[i].rate == rate)
			c = &t6050_pll_cfgs[i];
	if (!c)
		return -EINVAL;

	q10 = c->q10 ? c->q10 : 1;
	f = 4 * c->q08 + (4 * c->q0c) / q10;
	e = c->q0c > 1 ? c->q10 : 0;
	half = rate >= 2000;

	T6050_D(phy, 0x222c, 1, 0x2, c->bypass);
	T6050_D(phy, 0x222c, 0, 0x1, !c->bypass);
	T6050_D(phy, 0x2244, 0, 0xffff, c->q28);
	T6050_D(phy, 0x2234, 23, 0xff800000, c->q24);
	T6050_D(phy, 0x20b0, 0, 0x3ff, f);
	T6050_D(phy, 0x20b0, 10, 0x400, 0);
	T6050_D(phy, 0x20b4, 14, 0x0fffc000, c->q0c);
	T6050_D(phy, 0x20b4, 0, 0x3fff, e);
	T6050_D(phy, 0x20b8, 9, 0x3e00, c->q1c);
	T6050_D(phy, 0x20b8, 14, 0xc000, c->q14);
	T6050_D(phy, 0x20b8, 16, 0xf0000, c->q18);
	T6050_D(phy, 0x20b8, 20, 0x300000, c->q00);
	T6050_D(phy, 0x20b8, 22, 0x400000, c->q04);
	t6050_shadow_update(phy);

	/* half-link-rate: PHY, then PLL */
	T6050_D(phy, 0x4004, 5, 0x20, !half);
	T6050_D(phy, 0x4004, 6, 0x40, half);
	T6050_D(phy, 0x2000, 29, 0x20000000, !half);
	T6050_D(phy, 0x201c, 5, 0x20, !half);
	phy->rate = rate;
	return 0;
}

/*
 * A link reconfiguration as the DCP brackets it: PHY and PLL down, the new
 * lane count and rate, then (for a nonzero rate) PLL and PHY up.
 */
static int t6050_reconfigure(struct apple_dptx_phy *phy, bool set_lanes,
			     u32 lanes, bool set_rate, u32 rate)
{
	int ret;

	if (!phy->active)
		return -ENODEV;
	t6050_phy_power_down(phy);
	ret = t6050_pll_power_down(phy);
	if (ret)
		return ret;
	if (set_lanes) {
		ret = t6050_set_lanes(phy, lanes);
		if (ret)
			return ret;
	}
	if (set_rate) {
		if (!rate) {
			phy->rate = 0;
			return 0;
		}
		ret = t6050_program_rate(phy, rate);
		if (ret)
			return ret;
	}
	if (!phy->rate)
		return 0;
	ret = t6050_pll_power_up(phy);
	if (ret)
		return ret;
	t6050_phy_power_up(phy);
	return 0;
}

static int t6050_deactivate(struct apple_dptx_phy *phy)
{
	int ret;

	if (!phy->active)
		return 0;
	t6050_set_lanes(phy, 0);
	t6050_phy_power_down(phy);
	for (int l = 0; l < DPTX_MAX_LANES; l++)
		T6050_D(phy, DPTX_LANE0_OFFSET + DPTX_LANE_STRIDE * l, 9, 0x200, 1);
	ret = t6050_pll_power_down(phy);
	T6050_D(phy, 0xd000, 0, 0x1, 1);	/* AUX power-down */
	phy->active = false;
	phy->rate = 0;
	t6050_device_power(phy, false);
	return ret;
}

static int t6050_probe(struct apple_dptx_phy *phy)
{
	struct device_node *np = phy->dev->of_node;
	u32 v;

	for (int i = 0; i < T6050_NR_CAL; i++) {
		if (of_property_read_u32(np, t6050_cal_props[i], &v) || v == 0xffffffff)
			continue;
		phy->cal[i] = v;
		phy->cal_set[i] = true;
	}
	/* The crossbar's ufp endpoint u: 5 and 6 select themselves, 0xf none, any other u + 1. */
	if (of_property_read_u32(np, "apple,ufp-endpoint", &v))
		return dev_err_probe(phy->dev, -EINVAL, "no apple,ufp-endpoint\n");
	phy->source_select = (v == 5 || v == 6) ? v : v == 0xf ? 0 : v + 1;
	phy->pclkdrv_select = !of_property_read_u32(np, "apple,pclkdrv-select", &v) && v;

	return 0;
}

static int dptx_phy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct apple_dptx_phy *dptx_phy = phy_get_drvdata(phy);

	if (dptx_phy->hw.type == DPTX_PHY_T6050) {
		int ret = -EINVAL;

		mutex_lock(&dptx_phy->lock);
		if (mode == PHY_MODE_INVALID)
			ret = t6050_deactivate(dptx_phy);
		else if (mode == PHY_MODE_DP)
			ret = dptx_phy->active ? 0 : t6050_activate(dptx_phy);
		mutex_unlock(&dptx_phy->lock);
		return ret;
	}

	switch (mode) {
	case PHY_MODE_INVALID:
		return dptx_phy_deactivate(dptx_phy);
	case PHY_MODE_DP:
		if (submode < 0 || submode > 5)
			return -EINVAL;
		return dptx_phy_activate(dptx_phy, submode);
	default:
		break;
	}

	return -EINVAL;
}

static int dptx_phy_validate(struct phy *phy, enum phy_mode mode, int submode,
			     union phy_configure_opts *opts_)
{
	struct phy_configure_opts_dp *opts = &opts_->dp;

	if (mode == PHY_MODE_INVALID) {
		memset(opts, 0, sizeof(*opts));
		return 0;
	}

	if (mode != PHY_MODE_DP)
		return -EINVAL;
	if (submode < 0 || submode > 5)
		return -EINVAL;

	opts->lanes = 4;
	opts->link_rate = 8100;

	for (int i = 0; i < 4; ++i) {
		opts->voltage[i] = 3;
		opts->pre[i] = 3;
	}

	return 0;
}

static int dptx_phy_configure(struct phy *phy, union phy_configure_opts *opts_)
{
	struct phy_configure_opts_dp *opts = &opts_->dp;
	struct apple_dptx_phy *dptx_phy = phy_get_drvdata(phy);
	enum dptx_phy_link_rate link_rate;
	int ret = 0;

	if (dptx_phy->hw.type == DPTX_PHY_T6050) {
		if (!opts->set_lanes && !opts->set_rate)
			return 0;
		mutex_lock(&dptx_phy->lock);
		ret = t6050_reconfigure(dptx_phy, opts->set_lanes, opts->lanes,
					opts->set_rate, opts->link_rate);
		mutex_unlock(&dptx_phy->lock);
		return ret;
	}

	if (opts->set_lanes) {
		mutex_lock(&dptx_phy->lock);
		ret = dptx_phy_set_active_lane_count(dptx_phy, opts->lanes);
		mutex_unlock(&dptx_phy->lock);
	}

	if (opts->set_rate) {
		switch (opts->link_rate) {
		case 1620:
			link_rate = DPTX_PHY_LINK_RATE_RBR;
			break;
		case 2700:
			link_rate = DPTX_PHY_LINK_RATE_HBR;
			break;
		case 5400:
			link_rate = DPTX_PHY_LINK_RATE_HBR2;
			break;
		case 8100:
			link_rate = DPTX_PHY_LINK_RATE_HBR3;
			break;
		case 0:
			// TODO: disable!
			return 0;
			break;
		default:
			dev_err(dptx_phy->dev, "Unsupported link rate: %d\n",
				opts->link_rate);
			return -EINVAL;
		}

		mutex_lock(&dptx_phy->lock);
		ret = dptx_phy_set_link_rate(dptx_phy, link_rate);
		mutex_unlock(&dptx_phy->lock);
	}

	return ret;
}

static const struct phy_ops apple_atc_dp_phy_ops = {
	.owner = THIS_MODULE,
	.configure = dptx_phy_configure,
	.validate = dptx_phy_validate,
	.set_mode = dptx_phy_set_mode,
};

static int dptx_phy_probe(struct platform_device *pdev)
{
	struct apple_dptx_phy *dptx_phy;
	struct device *dev = &pdev->dev;

	dptx_phy = devm_kzalloc(dev, sizeof(*dptx_phy), GFP_KERNEL);
	if (!dptx_phy)
		return -ENOMEM;

	dptx_phy->dev = dev;
	dptx_phy->hw =
		*(struct apple_dptx_phy_hw *)of_device_get_match_data(dev);
	platform_set_drvdata(pdev, dptx_phy);

	mutex_init(&dptx_phy->lock);

	dptx_phy->regs.core =
		devm_platform_ioremap_resource_byname(pdev, "core");
	if (IS_ERR(dptx_phy->regs.core))
		return PTR_ERR(dptx_phy->regs.core);
	dptx_phy->regs.dptx =
		devm_platform_ioremap_resource_byname(pdev, "dptx");
	if (IS_ERR(dptx_phy->regs.dptx))
		return PTR_ERR(dptx_phy->regs.dptx);

	/* create phy */
	dptx_phy->phy_dp =
		devm_phy_create(dptx_phy->dev, NULL, &apple_atc_dp_phy_ops);
	if (IS_ERR(dptx_phy->phy_dp))
		return PTR_ERR(dptx_phy->phy_dp);
	phy_set_drvdata(dptx_phy->phy_dp, dptx_phy);

	dptx_phy->phy_provider =
		devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(dptx_phy->phy_provider))
		return PTR_ERR(dptx_phy->phy_provider);

	if (dptx_phy->hw.type == DPTX_PHY_T6050)
		return t6050_probe(dptx_phy);
	return 0;
}

static const struct apple_dptx_phy_hw apple_dptx_hw_t6020 = {
	.type = DPTX_PHY_T6020,
};

static const struct apple_dptx_phy_hw apple_dptx_hw_t8112 = {
	.type = DPTX_PHY_T8112,
};

static const struct apple_dptx_phy_hw apple_dptx_hw_t6050 = {
	.type = DPTX_PHY_T6050,
};

static const struct of_device_id dptx_phy_match[] = {
	{ .compatible = "apple,t6020-dptx-phy", .data = &apple_dptx_hw_t6020 },
	{ .compatible = "apple,t8112-dptx-phy", .data = &apple_dptx_hw_t8112 },
	{ .compatible = "apple,t6050-dptx-phy", .data = &apple_dptx_hw_t6050 },
	{},
};
MODULE_DEVICE_TABLE(of, dptx_phy_match);

static struct platform_driver dptx_phy_driver = {
	.driver = {
		.name = "phy-apple-dptx",
		.of_match_table = dptx_phy_match,
	},
	.probe = dptx_phy_probe,
};

module_platform_driver(dptx_phy_driver);

MODULE_AUTHOR("Janne Grunau <j@jananu.net>");
MODULE_DESCRIPTION("Apple DP TX PHY driver");

MODULE_LICENSE("GPL");
