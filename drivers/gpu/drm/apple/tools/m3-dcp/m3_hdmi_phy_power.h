/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef M3_HDMI_PHY_POWER_H
#define M3_HDMI_PHY_POWER_H
/* Copied from M4 power component after matching the M3 26A428 native
 * powerup trace (typec-power, 2026-09-21). Register masks/order agree.
 * Power/reset only; the PHY reset remains asserted throughout.
 * Startup follows native AppleT8122TypeCPhy::aciophy_phy_powerup_rst.
 * Reverse transition follows Sven Peter / Asahi atcphy_power_off, using
 * the power/misc bits independently confirmed in the J514S ATC3 native routines.
 * No USB2, tunables, lane routing, common initialization or AUX changes.
 */
static int m3_hdmi_atc_power_idle(void __iomem *regs)
{
	return readl(regs + 0x20000) == 4 &&
	       !readl(regs + 0x20004) && !readl(regs + 0x20008) ? 0 : -EBUSY;
}

static void m3_hdmi_atc_power_update(void __iomem *regs, u32 offset, u32 clear, u32 set)
{
	u32 value = readl(regs + offset);
	writel((value & ~clear) | set, regs + offset);
	udelay(1);
}

static int m3_hdmi_atc_power_wait(void __iomem *regs, u32 mask, u32 value)
{
	u32 status;
	return readl_poll_timeout(regs + 0x20004, status,
				 (status & mask) == value, 1, 1000);
}

static int m3_hdmi_atc_power_up_reset(void __iomem *regs)
{
	int ret;
	/* Caller has checked idle and pinned its powered device before entry. */
	ndelay(500);
	m3_hdmi_atc_power_update(regs, 0x20008, 0, 1);
	m3_hdmi_atc_power_update(regs, 0x20000, 0, 1);
	ret = m3_hdmi_atc_power_wait(regs, 1, 1);
	if (ret)
		return ret;
	m3_hdmi_atc_power_update(regs, 0x20000, 0, 2);
	ret = m3_hdmi_atc_power_wait(regs, 2, 2);
	if (ret)
		return ret;
	m3_hdmi_atc_power_update(regs, 0x20000, 4, 0);
	m3_hdmi_atc_power_update(regs, 0x20000, 0, 8);
	if (readl(regs + 0x20000) != 0xb || readl(regs + 0x20008) != 1)
		return -EIO;
	pr_info("m3_hdmi_phy: reset-held PHY powerup reached control=0xb status=%#x misc=1\n",
		readl(regs + 0x20004));
	return 0;
}

static int m3_hdmi_atc_power_down(void __iomem *regs)
{
	int ret;
	/* Keep PHY reset asserted, restore clamp, then remove APB reset release
	 * and the big/small power requests in that order. No USB2 reset call.
	 */
	m3_hdmi_atc_power_update(regs, 0x20000, 0x10, 0);
	m3_hdmi_atc_power_update(regs, 0x20000, 0, 4);
	m3_hdmi_atc_power_update(regs, 0x20008, 5, 0);
	m3_hdmi_atc_power_update(regs, 0x20000, 8, 0);
	m3_hdmi_atc_power_update(regs, 0x20000, 2, 0);
	ret = m3_hdmi_atc_power_wait(regs, 2, 0);
	if (ret)
		return ret;
	m3_hdmi_atc_power_update(regs, 0x20000, 1, 0);
	ret = m3_hdmi_atc_power_wait(regs, 1, 0);
	if (ret)
		return ret;
	if (readl(regs + 0x20000) != 4 || readl(regs + 0x20004) ||
	    readl(regs + 0x20008))
		return -EIO;
	pr_info("m3_hdmi_phy: power roundtrip restored control=4 status=0 misc=0\n");
	return 0; /* Caller verifies PMGR before dropping the pin. */
}


#endif
