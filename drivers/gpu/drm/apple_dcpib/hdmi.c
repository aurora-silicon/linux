// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The J873g HDMI port: the dedicated T8152 dptx-phy in display-crossbar0's
 * lpdptx slot, driven by DCPEXT0 and converted by a PS190 (aura
 * src/dcp/dptx_phy.c, DPTX_PHY_T8152_HDMI).
 *
 * The bootloader powers the phy up, applies its tunables, selects the DCP
 * and trains the link. This keeps that setup and reprograms what a link
 * training changes: the PLL for a new link rate and the active lanes. Drive
 * settings are only recorded, as on macOS, and there is no per-link route.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#include "dcpib.h"

#define HDMI_LANES		4
#define HDMI_LANE0		0x4500
#define HDMI_LANE_STRIDE	0x2800

struct dcpib_hdmi {
	void __iomem *core;
	void __iomem *apb;
	/* AP DisplayPort AUX controller aux1, for the EDID */
	struct dcpib_aux aux;
};

/* AppleT8112DPTXPort::setDPTXSourceSelect: crossbar UFP feeding the phy */
#define HDMI_SOURCE_SELECT	0x10
#define HDMI_SOURCE_AUX1	6	/* UFP endpoint of dptx1-aux */

/* AppleT8152DPTXPort::setBitsInReg */
static void hdmi_set_bits(struct dcpib_hdmi *hdmi, u32 offset, u32 shift,
			  u32 mask, u32 value)
{
	u32 reg = readl(hdmi->core + offset);

	writel((reg & ~mask) | (value << shift), hdmi->core + offset);
}

static void hdmi_apb_update(struct dcpib_hdmi *hdmi, u32 offset, u32 mask,
			    u32 value)
{
	u32 reg = readl(hdmi->apb + offset);

	writel((reg & ~mask) | value, hdmi->apb + offset);
}

/*
 * AppleT8152DPTXPort::getTunableConfig: the DPPLLConfig words for each link
 * rate (constant tables in AppleDisplayCrossbar, 26A434).
 */
static const struct {
	u32 rate;	/* DPCD units */
	u32 w[22];
} hdmi_pll_configs[] = {
	{ DCPIB_LINK_RATE_RBR,
	  { 1, 0x21c, 0, 0, 1, 0x13, 1, 0, 0, 0x1a, 1, 0x3c, 2, 1, 0, 0, 1, 0,
	    0, 0, 1, 3 } },
	{ DCPIB_LINK_RATE_HBR,
	  { 1, 0x1c2, 0x1fff, 0x3ffe, 1, 0x13, 1, 0, 0, 0x1a, 1, 0x32, 2, 1, 0,
	    0, 1, 0, 0, 0, 1, 3 } },
	{ DCPIB_LINK_RATE_HBR2,
	  { 0, 0x1c2, 0x1fff, 0x3ffe, 1, 9, 1, 0, 0, 0x1a, 1, 0x32, 1, 0, 0, 1,
	    0, 0, 0, 1, 1, 3 } },
	{ DCPIB_LINK_RATE_HBR3,
	  { 0, 0x2a3, 0x2ffd, 0x3ffc, 1, 9, 1, 0, 0, 0x1a, 1, 0x5a, 1, 0, 0, 1,
	    0, 0, 0, 1, 1, 3 } },
};

/*
 * AppleT8152DPTXPort::pllPowerUp/pllPowerDown: command in 0x408[2:0],
 * request bit 3, acknowledge bit 19.
 */
static int hdmi_pll_command(struct dcpib *dcp, struct dcpib_hdmi *hdmi,
			    u32 command)
{
	int i;

	hdmi_set_bits(hdmi, 0x408, 6, 0x40, 1);
	hdmi_set_bits(hdmi, 0x408, 0, 0x7, command);
	hdmi_set_bits(hdmi, 0x408, 3, 0x8, 1);
	for (i = 0; i < 10; i++) {
		udelay(16);
		if (readl(hdmi->core + 0x408) & BIT(19))
			break;
	}
	hdmi_set_bits(hdmi, 0x408, 3, 0x8, 0);
	hdmi_set_bits(hdmi, 0x408, 6, 0x40, 0);
	if (i == 10) {
		dev_err(dcp->dev, "HDMI: no ACK to PLL command %u\n", command);
		return -ETIMEDOUT;
	}
	return 0;
}

/*
 * AppleT8152DPTXPort::phySetActiveLaneCount: lane-disable bits, 0x2420 holds
 * the mask of unused lanes.
 */
static void hdmi_set_lanes(struct dcpib_hdmi *hdmi, u32 lanes)
{
	static const u32 offsets[] = { 0x64, 0x24, 0x24, 0x48, 0x48, 0x5c, 0x5c, 0x48 };
	static const u32 shifts[] = { 4, 4, 1, 0, 1, 2, 3, 4 };
	static const u32 masks[] = { 0x10, 0x10, 0x2, 0x1, 0x2, 0x4, 0x8, 0x10 };
	u32 lane, i;

	hdmi_set_bits(hdmi, 0x2400, 0, 0x1, 0);
	for (lane = 0; lane < HDMI_LANES; lane++) {
		u32 base = HDMI_LANE0 + lane * HDMI_LANE_STRIDE;

		for (i = 0; i < ARRAY_SIZE(offsets); i++)
			hdmi_set_bits(hdmi, base + offsets[i], shifts[i],
				      masks[i], lane >= lanes);
	}
	hdmi_set_bits(hdmi, 0x2420, 0, 0xf, 0xf & ~((1 << lanes) - 1));
	hdmi_set_bits(hdmi, 0x2400, 0, 0x1, 1);
}

static int hdmi_activate(struct dcpib *dcp)
{
	return 0;
}

static int hdmi_deactivate(struct dcpib *dcp)
{
	dev_info(dcp->dev, "HDMI: keeping the link up on deactivate\n");
	return 0;
}

/*
 * AppleDPTXPort::setLinkRate with the T8152 hooks: phyPowerDown,
 * pllPowerDown, pllConfigureFrequency, phy/pllSetHalfLinkRate, pllPowerUp.
 */
static int hdmi_set_link_rate(struct dcpib *dcp, u32 rate)
{
	struct dcpib_hdmi *hdmi = dcp->phy_priv;
	const u32 *w = NULL;
	bool half;
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(hdmi_pll_configs); i++)
		if (hdmi_pll_configs[i].rate == rate)
			w = hdmi_pll_configs[i].w;
	if (!w)
		return -EINVAL;

	dev_info(dcp->dev, "HDMI: link rate %#x\n", rate);
	hdmi_set_bits(hdmi, 0x2400, 0, 0x1, 0);
	ret = hdmi_pll_command(dcp, hdmi, 0);
	if (ret)
		return ret;

	/* pllConfigureFrequency, downspread off */
	hdmi_set_bits(hdmi, 0x6c0, 0, 0x7ff, w[1]);
	hdmi_set_bits(hdmi, 0x6c0, 11, 0x800, w[6]);
	hdmi_set_bits(hdmi, 0x6c0, 13, 0x2000, w[10]);
	hdmi_set_bits(hdmi, 0x6c0, 18, 0x3c0000, w[21]);
	hdmi_set_bits(hdmi, 0x6c4, 14, 0xfffc000, w[2]);
	hdmi_set_bits(hdmi, 0x6c4, 0, 0x3fff,
		      (!(w[7] & 1) && w[2] < 2) ? 0 : w[3]);
	hdmi_set_bits(hdmi, 0x6c8, 14, 0xc000, w[0]);
	hdmi_set_bits(hdmi, 0x6c8, 8, 0x100, 0);
	hdmi_set_bits(hdmi, 0x6c8, 0, 0xff, w[8]);
	hdmi_set_bits(hdmi, 0x6c8, 9, 0x3e00, w[5]);
	hdmi_set_bits(hdmi, 0x6c8, 16, 0x10000, w[17]);
	hdmi_set_bits(hdmi, 0x6c8, 26, 0xfc000000, w[9]);
	hdmi_set_bits(hdmi, 0x6cc, 0, 0x7f, w[11]);
	hdmi_set_bits(hdmi, 0x6cc, 9, 0x600, w[13]);
	hdmi_set_bits(hdmi, 0x6cc, 11, 0x7800, w[14]);
	hdmi_set_bits(hdmi, 0x6cc, 15, 0x38000, w[15]);
	hdmi_set_bits(hdmi, 0x6cc, 20, 0x100000, w[16]);
	hdmi_set_bits(hdmi, 0x6cc, 21, 0x200000, w[18]);
	hdmi_set_bits(hdmi, 0x6cc, 22, 0xc00000, w[19]);
	hdmi_set_bits(hdmi, 0x6cc, 24, 0x3000000, w[4]);
	hdmi_set_bits(hdmi, 0x6cc, 26, 0xc000000, w[20]);

	/* AppleT8112DPTXPort::updateLinkRateDependentShadowRegisters */
	hdmi_apb_update(hdmi, 0xa0, BIT(3), BIT(3));
	hdmi_apb_update(hdmi, 0xa0, BIT(2), BIT(2));
	hdmi_apb_update(hdmi, 0xa0, BIT(18), BIT(18));
	hdmi_apb_update(hdmi, 0xa0, BIT(18), 0);
	hdmi_apb_update(hdmi, 0xa0, BIT(19), BIT(19));
	hdmi_apb_update(hdmi, 0xa0, BIT(19), 0);
	hdmi_apb_update(hdmi, 0xa0, BIT(2), 0);
	hdmi_apb_update(hdmi, 0xa0, BIT(3), 0);

	/* No UHBR rates; half link rate from HBR up. */
	half = rate >= DCPIB_LINK_RATE_HBR;
	hdmi_set_bits(hdmi, 0x2400, 2, 0x4, 0);
	hdmi_set_bits(hdmi, 0x2400, 3, 0x8, !half);
	hdmi_set_bits(hdmi, 0x0000, 17, 0x20000, !half);

	ret = hdmi_pll_command(dcp, hdmi, 7);
	if (ret)
		return ret;
	if (!(readl(hdmi->core + 0x438) & BIT(30))) {
		dev_err(dcp->dev, "HDMI: PLL failed to lock at rate %#x\n", rate);
		return -EIO;
	}
	return 0;
}

static int hdmi_set_lane_count(struct dcpib *dcp, u32 lanes)
{
	/* T8152 takes no zero lane count. */
	if (lanes)
		hdmi_set_lanes(dcp->phy_priv, lanes);
	return 0;
}

static int hdmi_set_drive(struct dcpib *dcp,
			  const struct dcpib_drive_settings *settings, u32 count)
{
	return 0;
}

static int hdmi_set_route(struct dcpib *dcp, bool enable)
{
	return 0;
}

/*
 * The sink's EDID through the AP AUX controller aux1: the phy takes its
 * source from aux1 for the read (its AUX reaches the PS190, which reads the
 * monitor's DDC), then from the DCP again. The display is off meanwhile.
 */
static int hdmi_read_edid(struct dcpib *dcp, u8 *buf, size_t size)
{
	struct dcpib_hdmi *hdmi = dcp->phy_priv;
	u32 src;
	int ret;

	if (!hdmi->aux.regs)
		return -ENODEV;
	ret = dcpib_aux_init(dcp, &hdmi->aux);
	if (ret)
		return ret;
	src = readl(hdmi->apb + HDMI_SOURCE_SELECT);
	writel((src & ~0xf) | HDMI_SOURCE_AUX1, hdmi->apb + HDMI_SOURCE_SELECT);
	ret = dcpib_aux_read_edid(dcp, &hdmi->aux, buf, size);
	writel(src, hdmi->apb + HDMI_SOURCE_SELECT);
	return ret;
}

static const struct dcpib_phy_ops hdmi_ops = {
	.activate = hdmi_activate,
	.deactivate = hdmi_deactivate,
	.set_link_rate = hdmi_set_link_rate,
	.set_lane_count = hdmi_set_lane_count,
	.set_drive = hdmi_set_drive,
	.set_route = hdmi_set_route,
	.read_edid = hdmi_read_edid,
	.max_link_rate = DCPIB_LINK_RATE_HBR3,
	/* display-crossbar0 DFP endpoint of the lpdptx slot */
};

/* The DCP node that describes the HDMI phy (DCPEXT0's). */
struct device_node *dcpib_hdmi_node(void)
{
	struct device_node *np;

	for_each_compatible_node(np, NULL, "apple,t8152-dcp-iboot")
		if (of_property_read_bool(np, "apple,hdmi"))
			return np;
	return NULL;
}

static void __iomem *hdmi_map(struct device *dev, struct device_node *np,
			      const char *name)
{
	int index = of_property_match_string(np, "reg-names", name);

	if (index < 0)
		return IOMEM_ERR_PTR(index);
	return devm_of_iomap(dev, np, index, NULL);
}

static bool hdmi_power_active(struct device_node *np, const char *name)
{
	int index = of_property_match_string(np, "reg-names", name);
	void __iomem *regs;
	u32 state;

	if (index < 0)
		return false;
	regs = of_iomap(np, index);
	if (!regs)
		return false;
	state = readl(regs);
	iounmap(regs);
	/* Both the requested and actual PMGR states must be active. */
	return (state & GENMASK(7, 0)) == GENMASK(7, 0);
}

/*
 * The DCP the bootloader wired to the phy (aura writes its DCP index: 0 for
 * the main DCP on the HDMI-only route, 1 for DCPEXT0 next to a USB-C
 * display), or -ENODEV if it left the PHY off or the PLL unlocked.
 */
int dcpib_hdmi_source(struct device_node *np)
{
	int core = of_property_match_string(np, "reg-names", "phy-core");
	int apb = of_property_match_string(np, "reg-names", "phy-apb");
	void __iomem *core_regs, *apb_regs;
	int ret = -ENODEV;

	/* The PHY aperture aborts while gated, including on a USB-C-only boot. */
	if (core < 0 || apb < 0 || !hdmi_power_active(np, "phy-ps"))
		return -ENODEV;
	core_regs = of_iomap(np, core);
	apb_regs = of_iomap(np, apb);
	if (core_regs && apb_regs && readl(core_regs + 0x438) & BIT(30))
		ret = readl(apb_regs + HDMI_SOURCE_SELECT) & 0xf;
	if (apb_regs)
		iounmap(apb_regs);
	if (core_regs)
		iounmap(core_regs);
	if (ret == 1 && !hdmi_power_active(np, "coproc-ps"))
		return -ENODEV;
	if (ret != 0 && ret != 1)
		return -ENODEV;
	return ret;
}

/* The phy, its AUX controller and its HPD line come from np. */
int dcpib_hdmi_init(struct dcpib *dcp, struct device_node *np)
{
	struct dcpib_hdmi *hdmi;

	hdmi = devm_kzalloc(dcp->dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;
	hdmi->core = hdmi_map(dcp->dev, np, "phy-core");
	if (IS_ERR(hdmi->core))
		return PTR_ERR(hdmi->core);
	hdmi->apb = hdmi_map(dcp->dev, np, "phy-apb");
	if (IS_ERR(hdmi->apb))
		return PTR_ERR(hdmi->apb);

	/* AUX output select 0x10: the lpdptx phy */
	dcpib_aux_probe_node(dcp, &hdmi->aux, 0x10, np);

	dev_info(dcp->dev, "HDMI: inherited source select %u, PLL %slocked\n",
		 readl(hdmi->apb + HDMI_SOURCE_SELECT) & 0xf,
		 readl(hdmi->core + 0x438) & BIT(30) ? "" : "not ");

	dcp->phy = &hdmi_ops;
	dcp->phy_priv = hdmi;
	/* display-crossbar0 DFP endpoint lpdptx */
	dcp->dcp_output = 4;
	return 0;
}
