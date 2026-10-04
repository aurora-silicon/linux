/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Shared per-device sequencer: caller owns MMIO, power and device lock.
 * Independent packed DP drive-preset register operations. Native behavioral
 * reference: AppleM3TypeCPhy::aciophy_dptx_set_{txeq,rxtxeq}_preset,
 * 26A428 image; boolean argument false enables the packed preset. Offline qualified only;
 * the standalone HBR test used the fixed initial preset, not sink training.
 * Caller supplies a qualified 24-bit preset and owns the selected PHY pair.
 */
#ifndef APPLE_ATCPHY_M3_DP_DRIVE_H
#define APPLE_ATCPHY_M3_DP_DRIVE_H

static int m3_hdmi_atc_dp_drive_preset(void __iomem *regs, unsigned int lane, bool tx, u32 packed)
{
	u32 select, amplitude, base;
	if (lane > 1 || packed & 0xff000000)
		return -EINVAL;
	base = lane * 0x7000;
	select = ((packed & 3) << 14) | ((packed >> 2) & 0xf);
	amplitude = (((packed >> 6) & 0x3ffff) << 1) | 1;
	if (tx) {
		m3_hdmi_dp_pll_update(regs, base + 0xc0d0, 0x100000, 0x100000);
		m3_hdmi_dp_pll_update(regs, base + 0xc050, 0xc00f, select);
		m3_hdmi_dp_pll_update(regs, base + 0xc0d4, 0x10000000, 0x10000000);
		m3_hdmi_dp_pll_update(regs, base + 0xc0d0, 0x7ffff, amplitude);
	} else {
		m3_hdmi_dp_pll_update(regs, base + 0x9210, 0xc00f, select);
		m3_hdmi_dp_pll_update(regs, base + 0x9248, 0x10000000, 0x10000000);
		m3_hdmi_dp_pll_update(regs, base + 0x9244, 0x7ffff, amplitude);
	}
	return 0;
}
#endif
