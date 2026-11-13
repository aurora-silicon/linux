// SPDX-License-Identifier: GPL-2.0-only
/*
 * T8152/J873g ATC DisplayPort PHYs for the DCP's DPTX port (from aura
 * src/dcp/atc_phy.c, which drives ATC3).
 *
 * The bootloader trains the USB-C display before Linux starts; this driver
 * takes that link over and then follows the firmware: link rate changes
 * (stop and set up the lanes again), drive settings, the DCP0 -> ATCn/DFP0
 * display crossbar route, and with the display off or unplugged a full
 * power down and a cold start. The DCP drives one USB-C port at a time:
 * the one a sink is plugged into (each port's PD controller is polled).
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include "dcpib.h"

#define ATC_AON		0x1e00
#define ATC_CFG		0x60000
#define ATC_XBAR	0x74000
#define ATC_TIMEOUT_US	10000

struct dcpib_atc_port {
	void __iomem *base;
	unsigned int index;		/* ATC instance */
	/* display-crossbar0 DFP endpoint of this ATC */
	u32 dfp;
	struct spmi_device *pd;
	/* AP DisplayPort AUX controller aux0 (shared), for the EDID */
	struct dcpib_aux *aux;
	/* PMGR power states of the four ATC dependencies (aura gate_addr) */
	void __iomem *ps;
	void __iomem *psd;
	/* the dependencies this driver powered up, and may power down */
	u8 owned;
	/* {offset, mask, value} tunables from the ADT, before/after xbar config */
	const __be32 *tunable[2];
	int tunable_len[2];
	/* PMGR DP clock control (shared): the divider selects the link rate */
	void __iomem *dp_clock;
	bool swap;
	bool active;
	bool route_started;
	u32 link_rate;
};

struct dcpib_atc {
	struct dcpib_aux aux;
	struct dcpib_atc_port port[4];
	unsigned int nports;
	/* the port the DCP drives, and the one a sink was found on */
	struct dcpib_atc_port *cur, *next;
};

static struct dcpib_atc_port *atc_cur(struct dcpib *dcp)
{
	return ((struct dcpib_atc *)dcp->phy_priv)->cur;
}

static const u32 atc_rates[] = {
	DCPIB_LINK_RATE_RBR, DCPIB_LINK_RATE_HBR, DCPIB_LINK_RATE_HBR2,
	DCPIB_LINK_RATE_HBR3,
};

static void atc_update(struct dcpib_atc_port *atc, u32 offset, u32 mask, u32 value)
{
	u32 reg = readl(atc->base + offset);

	writel((reg & ~mask) | value, atc->base + offset);
}

static int atc_poll(struct dcpib_atc_port *atc, u32 offset, u32 mask, u32 value)
{
	u32 reg;

	return readl_poll_timeout(atc->base + offset, reg,
				  (reg & mask) == value, 1, ATC_TIMEOUT_US);
}

static int atc_route_stop(struct dcpib *dcp, struct dcpib_atc_port *atc)
{
	if (!atc->route_started)
		return 0;
	atc_update(atc, ATC_XBAR + 0x3c, 1, 0);
	atc_update(atc, ATC_XBAR, 1, 0);
	atc_update(atc, ATC_XBAR + 0xc, 1, 0);
	atc_update(atc, ATC_XBAR + 0x24, 0x100, 0);
	udelay(1);
	/*
	 * With the pipe powered off (the bootloader hands the display over
	 * that way) nothing acknowledges the stop; the route is idle then, so
	 * finish it anyway.
	 */
	if (atc_poll(atc, ATC_XBAR + 0x800, 1, 0) ||
	    atc_poll(atc, ATC_XBAR + 0x808, 1, 0) ||
	    atc_poll(atc, ATC_XBAR + 0x81c, 0x100, 0))
		dev_info(dcp->dev, "ATC%u: no crossbar stop acknowledgment\n", atc->index);
	atc_update(atc, ATC_XBAR + 8, 1, 0);
	atc_update(atc, ATC_XBAR + 0x20, 7, 0);
	atc_update(atc, ATC_XBAR + 0x38, 0x700, 0);
	atc_update(atc, ATC_XBAR + 4, 1, 1);
	atc_update(atc, ATC_XBAR + 0x1c, 1, 1);
	atc_update(atc, ATC_XBAR + 0x34, 0x100, 0x100);
	atc->route_started = false;
	dev_dbg(dcp->dev, "ATC%u: display route disabled\n", atc->index);
	return 0;
}

/*
 * A source -> ATCn/DFP0 route, PCLK1: both DFP0 mux fields select the
 * source (AppleT8142ATCDPXBAR::connect): DCP0 or an AP AUX controller.
 */
#define ATC_SEL_DCP0	0
#define ATC_SEL_AUX0	9	/* UFP endpoint 5 + 4 (getMuxSelector) */

static int atc_route_start_sel(struct dcpib *dcp, struct dcpib_atc_port *atc,
			       u32 sel)
{
	if (atc->route_started)
		return 0;
	atc_update(atc, ATC_XBAR + 0x44, 0x00f00000, sel << 20);
	atc_update(atc, ATC_XBAR + 0x44, 0x00000f00, sel << 8);
	atc_update(atc, ATC_XBAR + 4, 1, 0);
	atc_update(atc, ATC_XBAR + 0x1c, 1, 0);
	atc_update(atc, ATC_XBAR + 0x34, 0x100, 0);
	udelay(1);
	if (atc_poll(atc, ATC_XBAR + 0x804, 1, 0) ||
	    atc_poll(atc, ATC_XBAR + 0x818, 1, 0) ||
	    atc_poll(atc, ATC_XBAR + 0x82c, 0x100, 0)) {
		dev_err(dcp->dev, "ATC%u: crossbar reset acknowledgment timeout\n", atc->index);
		return -ETIMEDOUT;
	}
	atc_update(atc, ATC_XBAR + 8, 1, 1);
	atc_update(atc, ATC_XBAR + 0x20, 7, 1);
	atc_update(atc, ATC_XBAR + 0x38, 0x700, 0x100);
	atc_update(atc, ATC_XBAR + 0x48, 1, 0);
	atc_update(atc, ATC_XBAR, 1, 1);
	atc_update(atc, ATC_XBAR + 0xc, 1, 1);
	atc_update(atc, ATC_XBAR + 0x24, 0x100, 0x100);
	atc_update(atc, ATC_XBAR + 0x4c, 0x100, 0x100);
	atc_update(atc, ATC_XBAR + 0x3c, 1, 1);
	atc->route_started = true;
	dev_dbg(dcp->dev, "ATC%u: source %u to local DFP0 route configured\n", atc->index, sel);
	return 0;
}

static int atc_route_start(struct dcpib *dcp, struct dcpib_atc_port *atc)
{
	return atc_route_start_sel(dcp, atc, ATC_SEL_DCP0);
}

/* Common four-lane setup; the rate-dependent fields follow in atc_dp_start. */
static const struct {
	u32 offset, mask, value;
} atc_dp_setup[] = {
	{ 0x2044, 0xffff, 0x4b0 },
	{ 0x4c, 0x3c0000, 0x3c0000 },
	{ 0x4c80, 0x40, 0x40 },
	{ 0xdc80, 0x40, 0x40 },
	{ 0x4fd4, 0x2000000, 0x2000000 },
	{ 0x4fd4, 0x1800000, 0x800000 },
	{ 0xdfd4, 0x2000000, 0x2000000 },
	{ 0xdfd4, 0x1800000, 0x800000 },
	{ 0x4d84, 0x1c0000, 0 },
	{ 0x4fd4, 1, 0 },
	{ 0x4fd4, 8, 0 },
	{ 0x4fd4, 2, 2 },
	{ 0x4fd4, 0x10, 0x10 },
	{ 0x8904, 0x1c0000, 0 },
	{ 0x8800, 0x10000000, 0 },
	{ 0x8800, 0x80000000, 0 },
	{ 0x8800, 0x20000000, 0x20000000 },
	{ 0x8804, 1, 1 },
	{ 0xdd84, 0x1c0000, 0 },
	{ 0xdfd4, 1, 0 },
	{ 0xdfd4, 8, 0 },
	{ 0xdfd4, 2, 2 },
	{ 0xdfd4, 0x10, 0x10 },
	{ 0x11904, 0x1c0000, 0 },
	{ 0x11800, 0x10000000, 0 },
	{ 0x11800, 0x80000000, 0 },
	{ 0x11800, 0x20000000, 0x20000000 },
	{ 0x11804, 1, 1 },
	{ 0x3c04, 2, 2 },
	{ 0x3c04, 1, 1 },
	{ 0xcc04, 2, 2 },
	{ 0xcc04, 1, 1 },
};

/* Program the PMGR DP clock divider and wait for the change to settle. */
static int atc_clock_write(struct dcpib_atc_port *atc, u32 divider)
{
	u32 reg = readl(atc->dp_clock);

	writel((reg & ~0x0f000000) | (divider << 24), atc->dp_clock);
	return readl_poll_timeout(atc->dp_clock, reg, !(reg & BIT(30)), 1,
				  ATC_TIMEOUT_US);
}

/* Stop the DP lanes: route, shim clock, then both PMAs (aura atc_dp_stop). */
static int atc_dp_stop(struct dcpib *dcp, struct dcpib_atc_port *atc)
{
	u32 reg;

	atc_route_stop(dcp, atc);
	atc_update(atc, ATC_CFG + 0x14, 7, 7);
	atc_update(atc, ATC_CFG + 0x18, 1, 0);
	if (readl_poll_timeout(atc->base + ATC_CFG + 0x1c, reg, !(reg & 1), 1,
			       ATC_TIMEOUT_US)) {
		dev_err(dcp->dev, "ATC%u: DPTX clock stop failed\n", atc->index);
		return -ETIMEDOUT;
	}
	atc_update(atc, 0x2040, BIT(16), 0);
	udelay(5);
	atc_update(atc, 0x2000, BIT(15), 0);
	atc_update(atc, 0x2000, BIT(13), 0);
	atc_update(atc, 0x2000, BIT(14), 0);
	atc_update(atc, 0x2000, BIT(2), 0);
	atc_update(atc, 0x2000, BIT(3), BIT(3));
	atc_update(atc, 0xe10, BIT(2), BIT(2));
	atc_update(atc, 0xe08, BIT(29), BIT(29));
	/* Remove both PMA clock requests, then clamp and power down both lanes. */
	atc_update(atc, ATC_AON + 0x24, BIT(12) | BIT(16) | BIT(14) | BIT(18), 0);
	udelay(1);
	for (u32 offset = 0x1e18; offset <= 0x1e1c; offset += 4) {
		atc_update(atc, offset, BIT(5), 0);
		udelay(1);
		atc_update(atc, offset, BIT(0), BIT(0));
		udelay(1);
		atc_update(atc, offset, BIT(2), 0);
		udelay(1);
		atc_update(atc, offset, BIT(1), 0);
		udelay(1);
	}
	atc->link_rate = 0;
	return 0;
}

/* Four-lane DP setup at rate index r (aura atc_phy_set_link_rate). */
static int atc_dp_start(struct dcpib *dcp, struct dcpib_atc_port *atc, u32 r)
{
	static const u32 tx_ctrl[] = { 0x4eb0, 0x8b10, 0xdeb0, 0x11b10 };
	static const u32 wake_count[] = { 0xbb5, 0xbb7, 0xbb8, 0xbb9 };
	int ret;

	if (readl(atc->dp_clock) & BIT(30))
		return -EBUSY;
	atc_update(atc, ATC_CFG + 0x14, 7, 0);
	atc_update(atc, ATC_CFG + 0x18, 1, 1);
	if (!r) {
		atc_update(atc, 0xe10, BIT(2), BIT(2));
		atc_update(atc, 0xe08, BIT(29), BIT(29));
	}
	for (u32 i = 0; i < 2; i++) {
		u32 offset = 0x1e18 + 4 * i;

		atc_update(atc, ATC_AON + 0x24, BIT(12 + 2 * i), BIT(12 + 2 * i));
		udelay(1);
		atc_update(atc, ATC_AON + 0x24, BIT(16 + 2 * i), BIT(16 + 2 * i));
		udelay(1);
		atc_update(atc, offset, BIT(1), BIT(1));
		udelay(1);
		atc_update(atc, offset, BIT(2), BIT(2));
		udelay(1);
		atc_update(atc, offset, BIT(0), 0);
		udelay(1);
		atc_update(atc, offset, BIT(5), BIT(5));
		udelay(2);
	}
	atc_update(atc, 0x2000, 0x380000, r << 19);
	for (unsigned int i = 0; i < ARRAY_SIZE(atc_dp_setup); i++)
		atc_update(atc, atc_dp_setup[i].offset, atc_dp_setup[i].mask,
			   atc_dp_setup[i].value);
	for (u32 i = 0; i < 2; i++) {
		atc_update(atc, 0x8684 + i * 0x9000, 3, r ? 0 : 1);
		atc_update(atc, 0x4c80 + i * 0x9000, 0x30, r ? 0 : 0x10);
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(tx_ctrl); i++) {
		atc_update(atc, tx_ctrl[i], 0x7e07ff, 1);
		atc_update(atc, tx_ctrl[i] - 0x14, 0x7f, r < 2 ? 0x65 : 0x40);
	}
	atc_update(atc, 0x2000, 0x70, 0x10);
	atc_update(atc, 0x2000, BIT(13), BIT(13));
	atc_update(atc, 0x2044, 0xffff0000, wake_count[r] << 16);
	atc_update(atc, atc->swap ? 0x89f0 : 0x119f0, BIT(14), BIT(14));
	atc_update(atc, 0x2000, BIT(2), BIT(2));
	atc_update(atc, 0x2000, BIT(3), BIT(3));
	atc_update(atc, 0x2040, BIT(16), BIT(16));

	ret = atc_clock_write(atc, 10 - r);
	if (ret) {
		dev_err(dcp->dev, "ATC%u: DP clock change timeout\n", atc->index);
		return ret;
	}
	ret = atc_route_start(dcp, atc);
	if (ret)
		return ret;
	atc->link_rate = atc_rates[r];
	dev_info(dcp->dev, "ATC%u: four-lane DPTX setup at rate %#x\n", atc->index,
		 atc->link_rate);
	return 0;
}

/*
 * The power dependencies, in order: the ATC's USB AON (PS +0x100), its common
 * PSD, then its two DP PS registers (+0, +0x20), with the values they idle at
 * (ATC3: PS 0x1c0, PSD 58, PS 0xc0, 0xe0).
 */
#define ATC_PSD_TARGET	0x90
#define ATC_PSD_STATE	0x80
static const u32 atc_ps_offset[] = { 0x100, 0, 0x0, 0x20 };
static const u32 atc_ps_idle[] = { 0x300, 6, 0x244, 0x244 };

static int atc_psd_wait(struct dcpib_atc_port *atc, u32 state)
{
	u32 reg;

	/*
	 * Only the state: the acknowledge bit aura also waits for stays clear
	 * when Linux switches the PSD, in either direction.
	 */
	return readl_poll_timeout(atc->psd + ATC_PSD_STATE, reg,
				  (reg & 7) == state, 1, ATC_TIMEOUT_US);
}

static int atc_gate(struct dcpib_atc_port *atc, unsigned int i, bool on)
{
	u32 value = on ? (atc_ps_idle[i] & 0xfffffcf0) | 15 : atc_ps_idle[i];
	u32 reg;

	/* Already up (the bootloader's or an earlier activation's). */
	if (on && (i == 1 ? !(readl(atc->psd + ATC_PSD_STATE) & 7) :
			    (readl(atc->ps + atc_ps_offset[i]) & 0xf0) == 0xf0))
		return 0;
	if (on)
		atc->owned |= BIT(i);
	else
		atc->owned &= ~BIT(i);
	if (i == 1) {
		writel(on ? 0 : atc_ps_idle[i], atc->psd + ATC_PSD_TARGET);
		return atc_psd_wait(atc, on ? 0 : atc_ps_idle[i]);
	}
	writel(value, atc->ps + atc_ps_offset[i]);
	return readl_poll_timeout(atc->ps + atc_ps_offset[i], reg,
				  (reg & 0xf0) == ((value & 15) << 4), 1,
				  ATC_TIMEOUT_US);
}

static void atc_apply_tunables(struct dcpib_atc_port *atc, unsigned int which)
{
	const __be32 *t = atc->tunable[which];

	for (int i = 0; i + 3 <= atc->tunable_len[which]; i += 3)
		atc_update(atc, be32_to_cpu(t[i]), be32_to_cpu(t[i + 1]),
			   be32_to_cpu(t[i + 2]));
}

/*
 * Cold start: the power dependencies, the common block and AUX (aura
 * atc_gates_enable and atc_phy_activate), then the DP lanes at HBR.
 */
static int atc_activate(struct dcpib *dcp)
{
	struct dcpib_atc_port *atc = atc_cur(dcp);
	void __iomem *cfg = atc->base + ATC_CFG;
	unsigned int gates = 0;
	u32 reg;

	if (atc->active)
		return 0;
	if (!atc->tunable[0] || !atc->tunable[1])
		return -EOPNOTSUPP;
	for (; gates < ARRAY_SIZE(atc_ps_idle); gates++)
		if (atc_gate(atc, gates, true))
			goto fail;
	if (readl(cfg) != 4 || (readl(cfg + 4) & 3) || readl(cfg + 0x30))
		goto fail;
	writel(readl(cfg + 0x30) | BIT(0), cfg + 0x30);
	udelay(1);
	writel(readl(cfg) | BIT(0), cfg);
	if (readl_poll_timeout(cfg + 4, reg, reg & BIT(0), 1, ATC_TIMEOUT_US))
		goto fail;
	writel(readl(cfg) | BIT(1), cfg);
	if (readl_poll_timeout(cfg + 4, reg, reg & BIT(1), 1, ATC_TIMEOUT_US))
		goto fail;
	writel(readl(cfg) & ~BIT(2), cfg);
	udelay(1);
	writel(readl(cfg) | BIT(3), cfg);

	atc_apply_tunables(atc, 0);
	/* aciophy_xbar_config(mode=3): both PMAs carry DisplayPort. */
	atc_update(atc, 0x4c, 0x1f, 0x14);
	atc_update(atc, 0x4c, 0x3ffe0, atc->swap ? 0x100 : 0x22000);
	atc_apply_tunables(atc, 1);
	atc_update(atc, ATC_CFG + 8, BIT(1), atc->swap ? BIT(1) : 0);
	writel(readl(cfg) | BIT(4), cfg);
	if ((readl(cfg) & 0x1f) != 0x1b)
		goto fail;
	/* aciophy_aux_init: release small/big sleep, then the AUX clamp. */
	atc_update(atc, ATC_AON + 0x24, BIT(8), BIT(8));
	udelay(1);
	atc_update(atc, ATC_AON + 0x24, BIT(6), BIT(6));
	udelay(1);
	atc_update(atc, ATC_AON + 0x24, BIT(10), 0);
	udelay(1);
	atc->active = true;
	dev_info(dcp->dev, "ATC%u: common PHY and AUX up from cold\n", atc->index);
	/*
	 * As aura's activation: the lanes start at HBR right away. From cold,
	 * AUX only works with the DP PLL and PMAs running (without them the
	 * firmware reports DEVICE_NOT_RESPONDING); the firmware switches to
	 * its rate afterwards.
	 */
	return atc_dp_start(dcp, atc, 1);
fail:
	dev_err(dcp->dev, "ATC%u: cold start failed (gates %u, cfg %#x/%#x)\n", atc->index,
		gates, gates == ARRAY_SIZE(atc_ps_idle) ? readl(cfg) : 0,
		gates == ARRAY_SIZE(atc_ps_idle) ? readl(cfg + 4) : 0);
	return -EIO;
}

/* Power the PHY down (aura atc_phy_deactivate), dependencies included. */
static int atc_deactivate(struct dcpib *dcp)
{
	struct dcpib_atc_port *atc = atc_cur(dcp);
	void __iomem *cfg = atc->base + ATC_CFG;
	int ret = 0;
	u32 reg;

	if (!atc->active)
		return 0;
	if (atc->link_rate && atc_dp_stop(dcp, atc)) {
		dev_err(dcp->dev, "ATC%u: DP stop failed\n", atc->index);
		return -EIO;
	}
	atc_route_stop(dcp, atc);
	atc_update(atc, ATC_AON + 0x24, BIT(10), BIT(10));
	udelay(1);
	atc_update(atc, ATC_AON + 0x24, BIT(6), 0);
	udelay(1);
	atc_update(atc, ATC_AON + 0x24, BIT(8), 0);
	udelay(1);
	writel(readl(cfg) & ~BIT(4), cfg);
	udelay(1);
	writel(readl(cfg) & ~BIT(3), cfg);
	writel(readl(cfg) | BIT(2), cfg);
	writel(readl(cfg) & ~BIT(1), cfg);
	if (readl_poll_timeout(cfg + 4, reg, !(reg & BIT(1)), 1, ATC_TIMEOUT_US)) {
		dev_err(dcp->dev, "ATC%u: big-sleep entry timeout\n", atc->index);
		ret = -ETIMEDOUT;
	}
	writel(readl(cfg) & ~BIT(0), cfg);
	if (readl_poll_timeout(cfg + 4, reg, !(reg & BIT(0)), 1, ATC_TIMEOUT_US)) {
		dev_err(dcp->dev, "ATC%u: small-sleep entry timeout\n", atc->index);
		ret = -ETIMEDOUT;
	}
	writel(readl(cfg + 0x30) & ~BIT(0), cfg + 0x30);
	atc->active = false;
	/*
	 * Then the power dependencies it powered up: the next activation starts
	 * it cold. The USB controller of a port keeps the ones it uses.
	 */
	for (int i = ARRAY_SIZE(atc_ps_idle) - 1; i >= 0; i--)
		if ((atc->owned & BIT(i)) && atc_gate(atc, i, false))
			ret = -ETIMEDOUT;
	dev_info(dcp->dev, "ATC%u: powered down: %d\n", atc->index, ret);
	return ret;
}

static int atc_set_link_rate(struct dcpib *dcp, u32 rate)
{
	struct dcpib_atc_port *atc = atc_cur(dcp);

	u32 r;
	int ret;

	/* The firmware drops the link (rate 0) before it retrains. */
	if (!rate || rate == atc->link_rate)
		return 0;
	for (r = 0; r < ARRAY_SIZE(atc_rates); r++)
		if (atc_rates[r] == rate)
			break;
	if (r == ARRAY_SIZE(atc_rates) || !atc->active)
		return -EINVAL;
	dev_info(dcp->dev, "ATC%u: link rate %#x, was %#x\n", atc->index, rate, atc->link_rate);
	if (atc->link_rate) {
		ret = atc_dp_stop(dcp, atc);
		if (ret)
			return ret;
	}
	return atc_dp_start(dcp, atc, r);
}

static int atc_set_route(struct dcpib *dcp, bool enable)
{
	struct dcpib_atc_port *atc = atc_cur(dcp);

	if (!enable)
		return atc_route_stop(dcp, atc);
	if (!atc->active)
		return -ENODEV;
	if (atc_route_start(dcp, atc))
		return -EIO;
	mdelay(5);
	return 0;
}

/*
 * Matching T8152 fallback table, indexed by voltage * 4 + pre-emphasis; the
 * ADT has no rate-specific dp-training-table, so it serves every rate.
 */
static const u32 atc_drive_presets[16] = {
	0x025, 0x420, 0x717, 0x90f, 0x019, 0x397, 0x78f, 0x025,
	0x011, 0x38f, 0x025, 0x025, 0x000, 0x025, 0x025, 0x025,
};

static int atc_set_drive(struct dcpib *dcp,
			 const struct dcpib_drive_settings *settings, u32 count)
{
	struct dcpib_atc_port *atc = atc_cur(dcp);
	u32 presets[4];

	if (!count)
		return 0;
	if (count > 4 || !atc->active || !atc->link_rate)
		return -EINVAL;
	/* Validate the entire request before changing any lane. */
	for (u32 i = 0; i < count; i++) {
		u32 v = le32_to_cpu(settings[i].voltage);
		u32 e = le32_to_cpu(settings[i].pre_emphasis);

		if (settings[i].format || v > 3 || e > 3 || v + e > 3)
			return -EINVAL;
		presets[i] = atc_drive_presets[v * 4 + e];
	}
	for (u32 i = 0; i < count; i++) {
		u32 pma = (i / 2) ^ !atc->swap;
		/* Even DP lanes use the PMA RX pair, odd lanes its TX pair. */
		u32 offset = ((i & 1) ? 0x4eb0 : 0x8b10) + pma * 0x9000;
		u32 preset = presets[i];
		u32 control = 1 | (((preset >> 13) & 0x3f) << 17) |
			      (((preset >> 7) & 0x3f) << 5);

		atc_update(atc, offset, 0x7e07ff, control);
		atc_update(atc, offset - 0x14, 0x7f, (preset & 0x3f) | 0x40);
	}
	return 0;
}

static void atc_set_orientation(struct dcpib *dcp, bool flipped)
{
	struct dcpib_atc_port *atc = atc_cur(dcp);

	/* Applied by the next cold start; a running PHY keeps its own. */
	if (atc->active && atc->swap != flipped)
		dev_warn(dcp->dev, "ATC%u: orientation changed while active\n", atc->index);
	atc->swap = flipped;
}

/*
 * The sink's EDID through the AP AUX controller aux0, which reaches the ATC
 * through crossbar UFP aux0 like the DCP: the DCP's route comes down for
 * the read, so this runs while the firmware does no AUX itself (display
 * off, HPD not asserted yet).
 */
static int atc_read_edid(struct dcpib *dcp, u8 *buf, size_t size)
{
	struct dcpib_atc_port *atc = atc_cur(dcp);
	bool was_started = atc->route_started;
	int ret;

	if (!atc->active || !atc->aux->regs)
		return -ENODEV;
	ret = dcpib_aux_init(dcp, atc->aux);
	if (ret)
		return ret;
	atc_route_stop(dcp, atc);
	ret = atc_route_start_sel(dcp, atc, ATC_SEL_AUX0);
	if (!ret)
		ret = dcpib_aux_read_edid(dcp, atc->aux, buf, size);
	atc_route_stop(dcp, atc);
	if (was_started)
		atc_route_start(dcp, atc);
	return ret;
}

/*
 * 1 with a sink on one of the ports: the current one first, then the
 * others; select() switches to a port found that way.
 */
static int atc_detect(struct dcpib *dcp, bool *flipped)
{
	struct dcpib_atc *atc = dcp->phy_priv;
	int ret = dcpib_pd_sink(atc->cur->pd, flipped);

	atc->next = atc->cur;
	if (ret)
		return ret;
	for (unsigned int i = 0; i < atc->nports; i++) {
		struct dcpib_atc_port *port = &atc->port[i];

		if (port == atc->cur || dcpib_pd_sink(port->pd, flipped) <= 0)
			continue;
		atc->next = port;
		/* A plug the other port's HPD state does not show yet. */
		dcp->hpd = 0;
		return 1;
	}
	return 0;
}

/*
 * Drive the port a sink was found on: power the old one down and connect
 * the DPTX port to the new one's display crossbar endpoint.
 */
static void atc_select(struct dcpib *dcp)
{
	struct dcpib_atc *atc = dcp->phy_priv;
	struct dcpib_atc_port *port = atc->next;

	if (!port || port == atc->cur)
		return;
	if (atc->cur->active)
		atc_deactivate(dcp);
	dev_info(dcp->dev, "USB-C display on ATC%u now, was ATC%u\n",
		 port->index, atc->cur->index);
	atc->cur = port;
	dcp->pd = port->pd;
	dcp->dcp_output = port->dfp;
	if (dcpib_dptx_connect(dcp, 0))
		dev_err(dcp->dev, "DPTX connect to ATC%u failed\n", port->index);
}

static const struct dcpib_phy_ops atc_ops = {
	.activate = atc_activate,
	.deactivate = atc_deactivate,
	.set_link_rate = atc_set_link_rate,
	.set_drive = atc_set_drive,
	.set_route = atc_set_route,
	.set_orientation = atc_set_orientation,
	.read_edid = atc_read_edid,
	.detect = atc_detect,
	.select = atc_select,
	.max_link_rate = DCPIB_LINK_RATE_HBR2,
	/*
	 * Modes up to 1920x1080@60 and 1600x1200@60 (and 2560x1440@30) show,
	 * 1920x1200@60 and anything faster stay black at HBR2 with no error:
	 * the DCP's pixel clock on the ATC path tops out between 162 and
	 * about 190 MHz with the firmware on its iBoot interface.
	 */
	.max_pixel_rate = 130000000,
	/*
	 * 720x480@60 and 720x576@50 (27 MHz) show, 640x480@60 (25.2 MHz)
	 * stays black here while HDMI shows it.
	 */
	.min_pixel_rate = 20000000,
};

/* PMGR registers, inside blocks their power domain driver has claimed. */
static void __iomem *atc_map(struct dcpib *dcp, struct device_node *np,
			     const char *name)
{
	int i = of_property_match_string(np, "reg-names", name);
	struct resource res;

	if (i < 0 || of_address_to_resource(np, i, &res))
		return NULL;
	return devm_ioremap(dcp->dev, res.start, resource_size(&res));
}

static int atc_port_init(struct dcpib *dcp, struct dcpib_atc *atc,
			 struct device_node *np, void __iomem *dp_clock)
{
	struct dcpib_atc_port *port = &atc->port[atc->nports];
	u32 cfg, r;

	port->base = atc_map(dcp, np, "atc");
	port->ps = atc_map(dcp, np, "atc-ps");
	port->psd = atc_map(dcp, np, "atc-psd");
	if (!port->base || !port->ps || !port->psd)
		return -ENOMEM;
	port->pd = dcpib_pd_get(dcp, np);
	if (IS_ERR_OR_NULL(port->pd))
		return port->pd ? PTR_ERR(port->pd) : -ENODEV;
	/* The node without ports is ATC3's (J873g back-right). */
	port->dfp = 3;
	of_property_read_u32(np, "apple,dfp-endpoint", &port->dfp);
	port->index = port->dfp;
	port->dp_clock = dp_clock;
	port->aux = &atc->aux;
	port->tunable[0] = of_get_property(np, "apple,tunable-pre",
					   &port->tunable_len[0]);
	port->tunable[1] = of_get_property(np, "apple,tunable-post",
					   &port->tunable_len[1]);
	port->tunable_len[0] /= sizeof(__be32);
	port->tunable_len[1] /= sizeof(__be32);
	atc->nports++;

	/*
	 * The PHY block hangs the bus while it is power-gated: look at it only
	 * with its DP dependencies and the common PSD up.
	 */
	if ((readl(port->ps) & 0xf0) != 0xf0 ||
	    (readl(port->psd + ATC_PSD_STATE) & 7)) {
		dev_info(dcp->dev, "ATC%u: powered down\n", port->index);
		return 0;
	}
	/* Common block out of reset and AUX up, as aura's activation leaves it. */
	cfg = readl(port->base + ATC_CFG);
	port->active = (cfg & 0x1f) == 0x1b;
	port->swap = (readl(port->base + 0x4c) & 0x3ffe0) == 0x100;
	port->route_started = (readl(port->base + ATC_XBAR) & 1) &&
			      (readl(port->base + ATC_XBAR + 0x3c) & 1);
	r = (readl(port->base + 0x2000) >> 19) & 7;
	if (port->active && (readl(port->base + 0x2040) & BIT(16)) &&
	    r < ARRAY_SIZE(atc_rates))
		port->link_rate = atc_rates[r];
	/* The bootloader's display: its dependencies are the display's. */
	if (port->active)
		port->owned = GENMASK(ARRAY_SIZE(atc_ps_idle) - 1, 0);
	dev_info(dcp->dev,
		 "ATC%u: inherited state: active %d, rate %#x, route %d, reversed %d (cfg %#x)\n",
		 port->index, port->active, port->link_rate, port->route_started,
		 port->swap, cfg);
	return 0;
}

int dcpib_atc_init(struct dcpib *dcp)
{
	struct device_node *np = dcp->dev->of_node;
	void __iomem *dp_clock;
	struct dcpib_atc *atc;
	bool flipped;
	int ret;

	atc = devm_kzalloc(dcp->dev, sizeof(*atc), GFP_KERNEL);
	if (!atc)
		return -ENOMEM;
	dp_clock = atc_map(dcp, np, "dp-clock");
	if (!dp_clock)
		return -ENOMEM;
	/* AUX output select 9: the crossbar's UFP aux0 */
	dcpib_aux_probe(dcp, &atc->aux, 9);

	/* One child per USB-C port, or the node itself for ATC3 alone. */
	for_each_child_of_node_scoped(np, child) {
		if (!of_property_present(child, "apple,dfp-endpoint"))
			continue;
		if (atc->nports == ARRAY_SIZE(atc->port))
			break;
		ret = atc_port_init(dcp, atc, child, dp_clock);
		if (ret)
			return ret;
	}
	if (!atc->nports) {
		ret = atc_port_init(dcp, atc, np, dp_clock);
		if (ret)
			return ret;
	}

	/* The bootloader's display port, else one with a sink, else the first. */
	atc->cur = &atc->port[0];
	for (unsigned int i = 0; i < atc->nports; i++) {
		if (atc->port[i].active) {
			atc->cur = &atc->port[i];
			goto found;
		}
	}
	for (unsigned int i = 0; i < atc->nports; i++) {
		if (dcpib_pd_sink(atc->port[i].pd, &flipped) > 0) {
			atc->cur = &atc->port[i];
			break;
		}
	}
found:
	atc->next = atc->cur;
	dcp->phy = &atc_ops;
	dcp->phy_priv = atc;
	dcp->pd = atc->cur->pd;
	dcp->dcp_output = atc->cur->dfp;
	dev_info(dcp->dev, "USB-C display on ATC%u (%u ports)\n", atc->cur->index,
		 atc->nports);
	return 0;
}
