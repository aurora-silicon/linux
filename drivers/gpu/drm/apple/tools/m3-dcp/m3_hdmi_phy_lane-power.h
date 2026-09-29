/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Adapted from the M4 sequencer, checked against J514S 26A428 native
 * main_init and its callees. Experimental; no trained link claim.
 */
#ifndef APPLE_ATCPHY_M3_LANE_POWER_H
#define APPLE_ATCPHY_M3_LANE_POWER_H

/* M3 uses a single read per selected-lane bit update. */
static void m3_hdmi_atc_lane_power_field(void __iomem *regs, u32 offset,
 unsigned int shift, unsigned int lane, bool enable)
{
 u32 value = readl(regs + offset), bit = 1u << (shift + lane);
 writel(enable ? value | bit : value & ~bit, regs + offset);
}

static int m3_hdmi_atc_lane_tx_power(void __iomem *regs, unsigned int lane, bool enable)
{
	if (lane > 1)
		return -EINVAL;
	if (enable) {
		m3_hdmi_atc_lane_power_field(regs, 0x10c, 4, lane, true);
		m3_hdmi_atc_lane_power_field(regs, 0x10c, 6, lane, true);
		udelay(1);
		m3_hdmi_atc_lane_power_field(regs, 0x10c, 0, lane, true);
		m3_hdmi_atc_lane_power_field(regs, 0x10c, 2, lane, true);
		udelay(1);
		m3_hdmi_atc_lane_power_field(regs, 0x10c, 8, lane, false);
		m3_hdmi_atc_lane_power_field(regs, 0x10c, 10, lane, true);
		udelay(1);
	} else {
		m3_hdmi_atc_lane_power_field(regs, 0x10c, 8, lane, true);
		udelay(1);
		m3_hdmi_atc_lane_power_field(regs, 0x10c, 0, lane, false);
		udelay(1);
		m3_hdmi_atc_lane_power_field(regs, 0x10c, 4, lane, false);
		udelay(1);
	}
	return 0;
}

static int m3_hdmi_atc_lane_rx_power(void __iomem *regs, unsigned int lane, bool enable)
{
	if (lane > 1)
		return -EINVAL;
	if (enable) {
		m3_hdmi_atc_lane_power_field(regs, 0xc, 4, lane, true);
		m3_hdmi_atc_lane_power_field(regs, 0xc, 6, lane, true);
		udelay(1);
		m3_hdmi_atc_lane_power_field(regs, 0xc, 0, lane, true);
		m3_hdmi_atc_lane_power_field(regs, 0xc, 2, lane, true);
		udelay(1);
		m3_hdmi_atc_lane_power_field(regs, 8, 10, lane, true);
		m3_hdmi_atc_lane_power_field(regs, 8, 12, lane, true);
		udelay(1);
		m3_hdmi_atc_lane_power_field(regs, 8, 6, lane, true);
		m3_hdmi_atc_lane_power_field(regs, 8, 8, lane, true);
		udelay(1);
		m3_hdmi_atc_lane_power_field(regs, 8, 14, lane, false);
		m3_hdmi_atc_lane_power_field(regs, 8, 16, lane, true);
		udelay(1);
	} else {
		m3_hdmi_atc_lane_power_field(regs, 8, 14, lane, true);
		udelay(1);
		m3_hdmi_atc_lane_power_field(regs, 8, 6, lane, false);
		udelay(1);
		m3_hdmi_atc_lane_power_field(regs, 8, 10, lane, false);
		udelay(1);
		m3_hdmi_atc_lane_power_field(regs, 0xc, 0, lane, false);
		udelay(1);
		m3_hdmi_atc_lane_power_field(regs, 0xc, 4, lane, false);
		udelay(1);
	}
	return 0;
}
#endif
