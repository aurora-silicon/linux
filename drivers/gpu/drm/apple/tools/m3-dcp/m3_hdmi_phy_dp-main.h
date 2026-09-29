/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Adapted from the M4 sequencer, checked against J514S 26A428 native
 * main_init and its callees. Experimental; no trained link claim.
 */
#ifndef APPLE_ATCPHY_M3_DP_MAIN_H
#define APPLE_ATCPHY_M3_DP_MAIN_H
#include "m3_hdmi_phy_dp-pll.h"
#include "m3_hdmi_phy_lane-power.h"

static void m3_hdmi_dp_lane_update(void __iomem *regs, unsigned int lane, u32 offset, u32 mask, u32 value)
{
	m3_hdmi_dp_pll_update(regs, offset + lane * 0x7000, mask, value);
}

static void m3_hdmi_dp_lane_tx_initial_eq(void __iomem *regs, unsigned int lane)
{
	m3_hdmi_dp_lane_update(regs, lane, 0xc050, 0xc00f, 0);
	m3_hdmi_dp_lane_update(regs, lane, 0xc0d4, 0x10000000, 0x10000000);
	m3_hdmi_dp_lane_update(regs, lane, 0xc0d0, 0x7ffff, 1);
}

static void m3_hdmi_dp_lane_rx_initial_eq(void __iomem *regs, unsigned int lane)
{
	m3_hdmi_dp_lane_update(regs, lane, 0x9210, 0xc00f, 0);
	m3_hdmi_dp_lane_update(regs, lane, 0x9248, 0x10000000, 0x10000000);
	m3_hdmi_dp_lane_update(regs, lane, 0x9244, 0x7ffff, 0x1f81);
}

static void m3_hdmi_dp_lane_tx_prepare(void __iomem *regs, unsigned int lane, unsigned int rate)
{
	m3_hdmi_atc_lane_tx_power(regs, lane, true);
	writel(0x0aafb800, regs + 0xd044 + lane * 0x7000);
	ndelay(500);
	m3_hdmi_dp_lane_update(regs, lane, 0xd044, 0x1000000, 0x1000000);
	ndelay(250);
	m3_hdmi_dp_lane_update(regs, lane, 0xd044, 0x4000000, 0x4000000);
	ndelay(250);
	m3_hdmi_dp_lane_update(regs, lane, 0xd044, 0x7c0, 0x300);
	ndelay(250);
	m3_hdmi_dp_lane_update(regs, lane, 0xd044, 0x6c000, 0);
	ndelay(250);
	m3_hdmi_dp_lane_update(regs, lane, 0xc0d0, 0x100000, 0x100000);
	m3_hdmi_dp_lane_tx_initial_eq(regs, lane);
	m3_hdmi_dp_lane_update(regs, lane, 0xd044, 0x100000, 0x100000);
	ndelay(250);
	m3_hdmi_dp_lane_update(regs, lane, 0xd044, 0x400000, 0x400000);
	udelay(1);
	m3_hdmi_dp_lane_update(regs, lane, 0xd004, 0x3000000, 0x2000000);
	m3_hdmi_dp_lane_update(regs, lane, 0xd004, 0x30000000, 0x30000000);
	m3_hdmi_dp_lane_update(regs, lane, 0xd00c, 0x7000000, rate == 6 ? 0x5000000 : 0x4000000);
	m3_hdmi_dp_lane_update(regs, lane, 0xd010, 0x600, 0x600);
	m3_hdmi_dp_lane_update(regs, lane, 0xd01c, 0xc00000, 0x800000);
}

static void m3_hdmi_dp_lane_rx_prepare(void __iomem *regs, unsigned int lane, unsigned int rate)
{
	m3_hdmi_dp_lane_update(regs, lane, 0x9160, 1, 1);
	m3_hdmi_dp_lane_update(regs, lane, 0x91dc, 0x18000, 0x18000);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0a4, 3, 2);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0a8, 0x18000, 0x10000);
	m3_hdmi_dp_lane_update(regs, lane, 0xb06c, 0x18000, 0x10000);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0d4, 0x1f1f80, 0x121180);
	m3_hdmi_dp_lane_update(regs, lane, 0xb064, 0xfc00, 0xb800);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0d8, 7, 4);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0dc, 0x18, 0);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0d4, 0xe000, 0xc000);
	ndelay(100);
	m3_hdmi_atc_lane_rx_power(regs, lane, true);
	m3_hdmi_dp_lane_update(regs, lane, 0xb06c, 0x800, 0x800);
	m3_hdmi_dp_lane_update(regs, lane, 0x91d4, 3, 1);
	ndelay(10);
	m3_hdmi_dp_lane_update(regs, lane, 0x91d4, 2, 2);
	m3_hdmi_dp_lane_update(regs, lane, 0xb06c, 0x600, 0xc00);
	ndelay(100);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0d4, 0x50000, 0x50000);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0d8, 1, 1);
	ndelay(500);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0d4, 0x40000, 0);
	ndelay(200);
	m3_hdmi_dp_lane_update(regs, lane, 0xb068, 0x180, 0x180);
	ndelay(200);
	m3_hdmi_dp_lane_update(regs, lane, 0xb068, 0x60, 0x60);
	ndelay(200);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0d8, 3, 3);
	udelay(1);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0d8, 3, 2);
	udelay(1);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0d8, 3, 0);
	ndelay(750);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0a8, 0x60000, 0x60000);
	udelay(1);
	m3_hdmi_dp_lane_update(regs, lane, 0xb06c, 0x6000, 0x4000);
	m3_hdmi_dp_lane_rx_initial_eq(regs, lane);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0a8, 0x380000, rate == 6 ? 0x280000 : 0x200000);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0a8, 0xc00000, 0x800000);
	ndelay(50);
	m3_hdmi_dp_lane_update(regs, lane, 0xb030, 0xc00, 0xc00);
	ndelay(100);
}

static int m3_hdmi_atc_dp_main_prepare(void __iomem *regs, unsigned int pair_mask, unsigned int rate, bool ssc)
{
	int ret;
	if ((!pair_mask || pair_mask > 3) ||
	    (rate != 6 && rate != 10 && rate != 20 && rate != 30) || ssc)
		return -EOPNOTSUPP;
	ret = m3_hdmi_dp_pll_prepare(regs, pair_mask, rate, ssc);
	if (ret)
		return ret;
	for (unsigned int lane = 0; lane < 2; lane++) {
		if (!(pair_mask & (1u << lane))) continue;
		m3_hdmi_dp_lane_tx_prepare(regs, lane, rate);
		m3_hdmi_dp_lane_rx_prepare(regs, lane, rate);
	}
	m3_hdmi_dp_pll_update(regs, 0x7000, 1, 0);
	return 0;
}

static int m3_hdmi_atc_dp_main_stop(void __iomem *regs, unsigned int pair_mask)
{
	unsigned int lane;
	if (!pair_mask || pair_mask > 3)
		return -EOPNOTSUPP;
	m3_hdmi_dp_pll_update(regs, 0x7000, 1, 1);
	for(lane=0;lane<2;lane++){
	if(!(pair_mask & (1u<<lane)))continue;
	m3_hdmi_dp_lane_update(regs, lane, 0xd01c, 0xc00000, 0xc00000);
	m3_hdmi_dp_lane_update(regs, lane, 0xd010, 0x600, 0x400);
	m3_hdmi_dp_lane_update(regs, lane, 0xd004, 0x30000000, 0x20000000);
	ndelay(250);
	m3_hdmi_dp_lane_update(regs, lane, 0xd044, 0x100000, 0);
	ndelay(250);
	m3_hdmi_dp_lane_update(regs, lane, 0xd044, 0x400000, 0);
	ndelay(250);
	m3_hdmi_dp_lane_update(regs, lane, 0xd044, 0x4000000, 0);
	ndelay(250);
	m3_hdmi_dp_lane_update(regs, lane, 0xd044, 0x1000000, 0);
	ndelay(250);
	m3_hdmi_dp_lane_update(regs, lane, 0xd044, 0x1000, 0);
	m3_hdmi_atc_lane_tx_power(regs, lane, false);
	m3_hdmi_dp_lane_update(regs, lane, 0xb030, 0xc00, 0x800);
	ndelay(50);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0a8, 0xc00000, 0xc00000);
	ndelay(500);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0a8, 0x60000, 0x40000);
	m3_hdmi_dp_lane_update(regs, lane, 0xb068, 0x60, 0x40);
	ndelay(200);
	m3_hdmi_dp_lane_update(regs, lane, 0xb068, 0x180, 0x100);
	ndelay(200);
	m3_hdmi_dp_lane_update(regs, lane, 0xb0d4, 0x10000, 0);
	ndelay(100);
	m3_hdmi_dp_lane_update(regs, lane, 0xb06c, 0x600, 0x600);
	m3_hdmi_dp_lane_update(regs, lane, 0x91d4, 3, 1);
	m3_hdmi_dp_lane_update(regs, lane, 0xb06c, 0x800, 0);
	m3_hdmi_atc_lane_rx_power(regs, lane, false);
	}
	return m3_hdmi_dp_pll_stop_after_lanes(regs);
}
#endif
