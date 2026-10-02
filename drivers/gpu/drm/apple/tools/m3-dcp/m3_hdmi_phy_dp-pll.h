/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Adapted from the M4 sequencer, checked against J514S 26A428 native
 * main_init and its callees. Experimental; no trained link claim.
 */
#ifndef APPLE_ATCPHY_M3_DP_PLL_H
#define APPLE_ATCPHY_M3_DP_PLL_H

#define DP_PLL_REQUEST 0x2000
#define DP_PLL_STATUS  0x7034

static void m3_hdmi_dp_pll_update(void __iomem *regs, u32 offset, u32 mask, u32 value)
{
	writel((readl(regs + offset) & ~mask) | value, regs + offset);
}

static int m3_hdmi_dp_pll_ack(void __iomem *regs, u32 expected, unsigned int timeout_us)
{
	u32 status;
	return readl_poll_timeout(regs + DP_PLL_REQUEST, status,
				 ((status >> 1) & 1) == expected, 1, timeout_us);
}

/* Capture the original ACK for both phases, not the current request bit. */
static u32 m3_hdmi_dp_pll_request_begin(void __iomem *regs)
{
	u32 old = readl(regs + DP_PLL_REQUEST);
	u32 ack = (old >> 1) & 1;
	writel((old & 0xe0000006) | 0x10000000 | (ack ^ 1),
	       regs + DP_PLL_REQUEST);
	return ack;
}

static int m3_hdmi_dp_pll_prepare(void __iomem *regs, unsigned int pair_mask, unsigned int rate, bool ssc)
{
	u32 freq_a, freq_b, freq_c, ack, status;
	int ret;
	/* Spread spectrum is outside this diagnostic. */
	if ((!pair_mask || pair_mask > 3) || ssc)
		return -EOPNOTSUPP;
	switch (rate) {
	case 0x06: freq_a = 0x1e0e021c; freq_b = 0; freq_c = 0x156600; break;
	case 0x0a: freq_a = 0x1e0e01c2; freq_b = 0x07fffffe; freq_c = 0x155200; break;
	case 0x14: freq_a = 0x1e0e01c2; freq_b = 0x07fffffe; freq_c = 0x554800; break;
	case 0x1e: freq_a = 0x1e0e02a3; freq_b = 0x0bff7ffc; freq_c = 0x564800; break;
	default: return -EOPNOTSUPP;
	}
	m3_hdmi_dp_pll_update(regs, 0x2234, 3, 0);
	m3_hdmi_dp_pll_update(regs, 0x7000, 0x1ff0, 0x490);
	writel(freq_a, regs + 0x2080);
	writel(freq_b, regs + 0x2084);
	writel(freq_c, regs + 0x2088);
	m3_hdmi_dp_pll_update(regs, 0x2208, 0x1f0000, 0x70000);
	m3_hdmi_dp_pll_update(regs, 0x2218, 1, 1);
	m3_hdmi_dp_pll_update(regs, 0x2200, 8, 8);
	ack = m3_hdmi_dp_pll_request_begin(regs);
	ret = m3_hdmi_dp_pll_ack(regs, ack ^ 1, 30);
	if (ret)
		return ret;
	ret = readl_poll_timeout(regs + DP_PLL_STATUS, status,
				 status & 8, 1, 20);
	if (ret)
		return ret;
	m3_hdmi_dp_pll_update(regs, DP_PLL_REQUEST, 0x1ffffff9, 0x10014000 | ack);
	ret = m3_hdmi_dp_pll_ack(regs, ack, 5);
	if (ret)
		return ret;
	for (unsigned int lane = 0; lane < 2; lane++)
		if (pair_mask & (1u << lane))
			m3_hdmi_dp_pll_update(regs, 0xb064 + lane * 0x7000, 0xc, 8);
	m3_hdmi_dp_pll_update(regs, 0x7000, 0xc, 0xc);
	udelay(1);
	return 0;
}

static int m3_hdmi_dp_pll_stop_after_lanes(void __iomem *regs)
{
	u32 ack, status;
	int ret;
	m3_hdmi_dp_pll_update(regs, 0x7000, 0xc, 8);
	ack = m3_hdmi_dp_pll_request_begin(regs);
	ret = m3_hdmi_dp_pll_ack(regs, ack ^ 1, 500);
	if (ret)
		return ret;
	m3_hdmi_dp_pll_update(regs, DP_PLL_REQUEST, 0x1ffffff9, 0x10000018 | ack);
	ret = m3_hdmi_dp_pll_ack(regs, ack, 500);
	if (ret)
		return ret;
	ret = readl_poll_timeout(regs + DP_PLL_STATUS, status,
				 !(status & 8), 1, 5);
	if (ret)
		return ret;
	m3_hdmi_dp_pll_update(regs, 0x2200, 8, 0);
	return 0;
}
#endif
