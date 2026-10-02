/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Basic common/AUX sequence agrees with exact M3 26A428 Unicorn traces.
 * Shared M4 implementation copied after comparison, not its calibrations.
 * Main-link and lane routing are deliberately absent at this stage.
 */
static void m3_hdmi_atc_component_update(void __iomem *regs, u32 offset, u32 clear, u32 set)
{
 writel((readl(regs + offset) & ~clear) | set, regs + offset);
}
static int m3_hdmi_atc_common_start(void __iomem *regs)
{
	u32 status;
	m3_hdmi_atc_component_update(regs, 8, 0, 4);
	m3_hdmi_atc_component_update(regs, 8, 0, 8);
	udelay(1);
	m3_hdmi_atc_component_update(regs, 8, 0, 1);
	m3_hdmi_atc_component_update(regs, 8, 0, 2);
	udelay(1);
	m3_hdmi_atc_component_update(regs, 8, 0x10, 0);
	m3_hdmi_atc_component_update(regs, 8, 0, 0x20);
	udelay(1);
	m3_hdmi_atc_component_update(regs, 0xa00, 0, 2);
	udelay(15);
	return readl_poll_timeout(regs + 0x804, status, status & 1, 1, 100);
}

#define ATC_AUX_CTRL 0x16400
#define ATC_AUX_CFG 0x16000

static void m3_hdmi_atc_aux_start(void __iomem *regs)
{
	m3_hdmi_atc_component_update(regs, ATC_AUX_CTRL, 0, 3);
	udelay(1);
	m3_hdmi_atc_component_update(regs, ATC_AUX_CTRL, 0, 0xc);
	udelay(1);
	m3_hdmi_atc_component_update(regs, ATC_AUX_CTRL, 0x300, 0x100);
	udelay(1);
	m3_hdmi_atc_component_update(regs, ATC_AUX_CFG, 1, 0);
	udelay(1);
}


static void m3_hdmi_atc_common_stop(void __iomem *regs)
{
	m3_hdmi_atc_component_update(regs, 0xa00, 2, 0);
	udelay(1);
	m3_hdmi_atc_component_update(regs, 8, 0, 0x10);
	udelay(1);
	m3_hdmi_atc_component_update(regs, 8, 1, 0);
	udelay(1);
	m3_hdmi_atc_component_update(regs, 8, 4, 0);
	udelay(1);
}

static void m3_hdmi_atc_aux_stop(void __iomem *regs)
{
	m3_hdmi_atc_component_update(regs, ATC_AUX_CFG, 0, 1);
	udelay(1);
	m3_hdmi_atc_component_update(regs, ATC_AUX_CTRL, 0, 0x200);
	udelay(1);
	m3_hdmi_atc_component_update(regs, ATC_AUX_CTRL, 8, 0);
	udelay(1);
	m3_hdmi_atc_component_update(regs, ATC_AUX_CTRL, 2, 0);
	udelay(1);
}
