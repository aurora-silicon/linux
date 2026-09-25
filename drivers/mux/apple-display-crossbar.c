// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple Silicon Display Crossbar multiplexer driver
 *
 * Copyright (C) Asahi Linux Contributors
 *
 * Author: Sven Peter <sven@svenpeter.dev>
 *
 * T602x DPIN0/DPIN1: the stock t602x set() only programs DPPHY select
 * bits (state<<20 | state<<8) and ATC bit 8. Mux index is ignored, so
 * "Switched dpin0" never routes analog onto ACIO DP IN. Use the same
 * CROSSBAR_MUX_CTRL field layout as t8103/t6000 at T602X_REG_030, and
 * ATC_DPIN0/ATC_DPIN1 instead of the hardcoded 0x100 DPPHY bit.
 */

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mux/driver.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/dp-tunnel.h>

/*
 * T602x register interface is clearly different so most of the names below are
 * probably wrong.
 */

#define T602X_FIFO_WR_DPTX_CLK_EN 0x000
#define T602X_FIFO_WR_N_CLK_EN 0x004
#define T602X_FIFO_WR_UNK_EN 0x008
#define T602X_REG_00C 0x00c
#define T602X_REG_014 0x014
#define T602X_REG_018 0x018
#define T602X_REG_01C 0x01c
#define T602X_FIFO_RD_PCLK2_EN 0x024
#define T602X_FIFO_RD_N_CLK_EN 0x028
#define T602X_FIFO_RD_UNK_EN 0x02c
#define T602X_REG_030 0x030
#define T602X_REG_034 0x034

#define T602X_REG_804_STAT 0x804 // status of 0x004
#define T602X_REG_810_STAT 0x810 // status of 0x014
#define T602X_REG_81C_STAT 0x81c // status of 0x024

/*
 * T8013, T600x, T8112 dp crossbar registers.
 */

#define FIFO_WR_DPTX_CLK_EN 0x000
#define FIFO_WR_N_CLK_EN 0x004
#define FIFO_WR_UNK_EN 0x008
#define FIFO_RD_PCLK1_EN 0x020
#define FIFO_RD_PCLK2_EN 0x024
#define FIFO_RD_N_CLK_EN 0x028
#define FIFO_RD_UNK_EN 0x02c

#define OUT_PCLK1_EN 0x040
#define OUT_PCLK2_EN 0x044
#define OUT_N_CLK_EN 0x048
#define OUT_UNK_EN 0x04c

#define CROSSBAR_DISPEXT_EN 0x050
#define CROSSBAR_MUX_CTRL 0x060
#define CROSSBAR_MUX_CTRL_DPPHY_SELECT0 GENMASK(23, 20)
#define CROSSBAR_MUX_CTRL_DPIN1_SELECT0 GENMASK(19, 16)
#define CROSSBAR_MUX_CTRL_DPIN0_SELECT0 GENMASK(15, 12)
#define CROSSBAR_MUX_CTRL_DPPHY_SELECT1 GENMASK(11, 8)
#define CROSSBAR_MUX_CTRL_DPIN1_SELECT1 GENMASK(7, 4)
#define CROSSBAR_MUX_CTRL_DPIN0_SELECT1 GENMASK(3, 0)
#define CROSSBAR_ATC_EN 0x070

#define FIFO_WR_DPTX_CLK_EN_STAT 0x800
#define FIFO_WR_N_CLK_EN_STAT 0x804
#define FIFO_RD_PCLK1_EN_STAT 0x820
#define FIFO_RD_PCLK2_EN_STAT 0x824
#define FIFO_RD_N_CLK_EN_STAT 0x828

#define OUT_PCLK1_EN_STAT 0x840
#define OUT_PCLK2_EN_STAT 0x844
#define OUT_N_CLK_EN_STAT 0x848

#define UNK_TUNABLE 0xc00

#define ATC_DPIN0 BIT(0)
#define ATC_DPIN1 BIT(4)
#define ATC_DPPHY BIT(8)

enum { MUX_DPPHY = 0, MUX_DPIN0 = 1, MUX_DPIN1 = 2, MUX_MAX = 3 };
static const char *apple_dpxbar_names[MUX_MAX] = { "dpphy", "dpin0", "dpin1" };

struct apple_dpxbar_hw {
	unsigned int n_ufp;
	u32 tunable;
	const struct mux_control_ops *ops;
};

struct apple_dpxbar {
	struct device *dev;
	void __iomem *regs;
	int selected_dispext[MUX_MAX];
	spinlock_t lock;
};

static inline void dpxbar_mask32(struct apple_dpxbar *xbar, u32 reg, u32 mask,
				 u32 set)
{
	u32 value = readl(xbar->regs + reg);
	value &= ~mask;
	value |= set;
	writel(value, xbar->regs + reg);
}

static inline void dpxbar_set32(struct apple_dpxbar *xbar, u32 reg, u32 set)
{
	dpxbar_mask32(xbar, reg, 0, set);
}

static inline void dpxbar_clear32(struct apple_dpxbar *xbar, u32 reg, u32 clear)
{
	dpxbar_mask32(xbar, reg, clear, 0);
}

static u32 t602x_atc_bit(unsigned int index)
{
	switch (index) {
	case MUX_DPIN0:
		return ATC_DPIN0;
	case MUX_DPIN1:
		return ATC_DPIN1;
	case MUX_DPPHY:
	default:
		return ATC_DPPHY;
	}
}

static u32 t602x_mux_mask(unsigned int index)
{
	switch (index) {
	case MUX_DPIN0:
		return CROSSBAR_MUX_CTRL_DPIN0_SELECT0 |
		       CROSSBAR_MUX_CTRL_DPIN0_SELECT1;
	case MUX_DPIN1:
		return CROSSBAR_MUX_CTRL_DPIN1_SELECT0 |
		       CROSSBAR_MUX_CTRL_DPIN1_SELECT1;
	case MUX_DPPHY:
	default:
		return CROSSBAR_MUX_CTRL_DPPHY_SELECT0 |
		       CROSSBAR_MUX_CTRL_DPPHY_SELECT1;
	}
}

static u32 t602x_mux_set(unsigned int index, unsigned int mux_state)
{
	switch (index) {
	case MUX_DPIN0:
		return FIELD_PREP(CROSSBAR_MUX_CTRL_DPIN0_SELECT0, mux_state) |
		       FIELD_PREP(CROSSBAR_MUX_CTRL_DPIN0_SELECT1, mux_state);
	case MUX_DPIN1:
		return FIELD_PREP(CROSSBAR_MUX_CTRL_DPIN1_SELECT0, mux_state) |
		       FIELD_PREP(CROSSBAR_MUX_CTRL_DPIN1_SELECT1, mux_state);
	case MUX_DPPHY:
	default:
		return FIELD_PREP(CROSSBAR_MUX_CTRL_DPPHY_SELECT0, mux_state) |
		       FIELD_PREP(CROSSBAR_MUX_CTRL_DPPHY_SELECT1, mux_state);
	}
}

static void t602x_dump(struct apple_dpxbar *xbar, const char *tag)
{
	static const u32 offs[] = {
		0x000, 0x004, 0x008, 0x00c, 0x014, 0x018, 0x01c, 0x024,
		0x028, 0x02c, 0x030, 0x034, 0x040, 0x044, 0x048, 0x04c,
		0x050, 0x060, 0x070,
	};
	char buf[320];
	int n = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(offs); i++) {
		u32 val = readl(xbar->regs + offs[i]);

		n += scnprintf(buf + n, sizeof(buf) - n, " %03x=%08x",
			       offs[i], val);
		if (n >= (int)sizeof(buf) - 20)
			break;
	}
	dev_dbg(xbar->dev, "t602x %s:%s\n", tag, buf);
	dev_dbg(xbar->dev,
		"t602x %s clk: 000=%08x 800=%08x 020=%08x 820=%08x 024=%08x 81c=%08x\n",
		tag,
		readl(xbar->regs + T602X_FIFO_WR_DPTX_CLK_EN),
		readl(xbar->regs + FIFO_WR_DPTX_CLK_EN_STAT),
		readl(xbar->regs + FIFO_RD_PCLK1_EN),
		readl(xbar->regs + FIFO_RD_PCLK1_EN_STAT),
		readl(xbar->regs + T602X_FIFO_RD_PCLK2_EN),
		readl(xbar->regs + T602X_REG_81C_STAT));
}

static int apple_dpxbar_set_t602x(struct mux_control *mux, int state)
{
	struct apple_dpxbar *dpxbar = mux_chip_priv(mux->chip);
	unsigned int index = mux_control_get_index(mux);
	unsigned long flags;
	unsigned int mux_state;
	unsigned int dispext_bit;
	unsigned int dispext_bit_en;
	u32 atc_bit, mux_mask, mux_val;
	bool enable;
	int ret = 0;

	if (index >= MUX_MAX)
		return -EINVAL;

	if (state == MUX_IDLE_DISCONNECT) {
		mux_state = 0;
		enable = false;
	} else if (state >= 0 && state < 9) {
		dispext_bit = 1 << state;
		dispext_bit_en = 1 << (2 * state);
		mux_state = state;
		enable = true;
	} else {
		return -EINVAL;
	}

	atc_bit = t602x_atc_bit(index);
	mux_mask = t602x_mux_mask(index);
	mux_val = t602x_mux_set(index, mux_state);

	spin_lock_irqsave(&dpxbar->lock, flags);

	/* ensure the selected dispext isn't already used in this crossbar */
	if (enable) {
		for (int i = 0; i < MUX_MAX; ++i) {
			if (i == index)
				continue;
			if (dpxbar->selected_dispext[i] == state) {
				spin_unlock_irqrestore(&dpxbar->lock, flags);
				return -EBUSY;
			}
		}
	}

	if (dpxbar->selected_dispext[index] >= 0) {
		u32 prev_dispext_bit = 1 << dpxbar->selected_dispext[index];
		u32 prev_dispext_bit_en = 1 << (2 * dpxbar->selected_dispext[index]);

		dpxbar_clear32(dpxbar, T602X_FIFO_RD_UNK_EN, prev_dispext_bit);
		dpxbar_clear32(dpxbar, T602X_FIFO_WR_DPTX_CLK_EN, prev_dispext_bit);
		/* DPIN0 uses the same single-bit source gate on up and down. */
		dpxbar_clear32(dpxbar, T602X_REG_00C,
			      index == MUX_DPIN0 ? prev_dispext_bit : prev_dispext_bit_en);

		dpxbar_clear32(dpxbar, T602X_REG_01C, atc_bit);

		dpxbar_clear32(dpxbar, T602X_FIFO_WR_UNK_EN, prev_dispext_bit);
		dpxbar_clear32(dpxbar, T602X_REG_018, prev_dispext_bit_en);

		dpxbar_clear32(dpxbar, T602X_FIFO_RD_N_CLK_EN, atc_bit);

		dpxbar_set32(dpxbar, T602X_FIFO_WR_N_CLK_EN, prev_dispext_bit);
		dpxbar_set32(dpxbar, T602X_REG_014, 0x4);

		/* Native T602x DPIN0 teardown restores the read reset at +0x24. */
		dpxbar_set32(dpxbar, index == MUX_DPIN0 ?
			    T602X_FIFO_RD_PCLK2_EN : FIFO_RD_PCLK1_EN, atc_bit);

		dpxbar_clear32(dpxbar, T602X_REG_034, atc_bit);
		dpxbar_clear32(dpxbar, CROSSBAR_ATC_EN, atc_bit);
		dpxbar_clear32(dpxbar, CROSSBAR_DISPEXT_EN, prev_dispext_bit);
		dpxbar_mask32(dpxbar, T602X_REG_030, mux_mask, 0);

		dpxbar->selected_dispext[index] = -1;
	}

	if (enable) {
		dpxbar_mask32(dpxbar, T602X_REG_030, mux_mask, mux_val);
		udelay(10);

		dpxbar_clear32(dpxbar, T602X_FIFO_WR_N_CLK_EN, dispext_bit);
		dpxbar_clear32(dpxbar, T602X_REG_014, 0x4);

		dpxbar_clear32(dpxbar, T602X_FIFO_RD_PCLK2_EN, atc_bit);

		dpxbar_set32(dpxbar, T602X_FIFO_WR_UNK_EN, dispext_bit);
		dpxbar_set32(dpxbar, T602X_REG_018, dispext_bit_en);

		dpxbar_set32(dpxbar, T602X_FIFO_RD_N_CLK_EN, atc_bit);
		dpxbar_set32(dpxbar, T602X_FIFO_WR_DPTX_CLK_EN, dispext_bit);
		dpxbar_set32(dpxbar, T602X_REG_00C, dispext_bit);

		dpxbar_set32(dpxbar, T602X_REG_01C, atc_bit);
		dpxbar_set32(dpxbar, T602X_REG_034, atc_bit);
		dpxbar_set32(dpxbar, CROSSBAR_ATC_EN, atc_bit);
		/*
		 * t8103 enables the dispext source at 0x050. T602x left
		 * that register at 0, so dpin0 was selected with no
		 * source clock into the ACIO analog PHY.
		 */
		dpxbar_set32(dpxbar, CROSSBAR_DISPEXT_EN, dispext_bit);

		dpxbar_set32(dpxbar, T602X_FIFO_RD_UNK_EN, dispext_bit);

		dpxbar->selected_dispext[index] = state;
	}

	spin_unlock_irqrestore(&dpxbar->lock, flags);

	if (enable) {
		dev_info(dpxbar->dev, "Switched %s to dispext%u,%u\n",
			 apple_dpxbar_names[index], mux_state >> 1,
			 mux_state & 1);
		dev_dbg(dpxbar->dev, "t602x atc=0x%x mux=0x%x\n", atc_bit,
			mux_val);
	} else {
		dev_info(dpxbar->dev, "Switched %s to disconnected state\n",
			 apple_dpxbar_names[index]);
	}

	t602x_dump(dpxbar, enable ? apple_dpxbar_names[index] : "idle");

	return ret;
}

static int apple_dpxbar_set(struct mux_control *mux, int state)
{
	struct apple_dpxbar *dpxbar = mux_chip_priv(mux->chip);
	unsigned int index = mux_control_get_index(mux);
	unsigned long flags;
	unsigned int mux_state;
	unsigned int dispext_bit;
	unsigned int dispext_bit_en;
	unsigned int atc_bit;
	bool enable;
	int ret = 0;
	u32 mux_mask, mux_set;

	if (state == MUX_IDLE_DISCONNECT) {
		mux_state = 0;
		enable = false;
	} else if (state >= 0 && state < 9) {
		dispext_bit = 1 << state;
		dispext_bit_en = 1 << (2 * state);
		mux_state = state;
		enable = true;
	} else {
		return -EINVAL;
	}

	switch (index) {
	case MUX_DPPHY:
		mux_mask = CROSSBAR_MUX_CTRL_DPPHY_SELECT0 |
			   CROSSBAR_MUX_CTRL_DPPHY_SELECT1;
		mux_set =
			FIELD_PREP(CROSSBAR_MUX_CTRL_DPPHY_SELECT0, mux_state) |
			FIELD_PREP(CROSSBAR_MUX_CTRL_DPPHY_SELECT1, mux_state);
		atc_bit = ATC_DPPHY;
		break;
	case MUX_DPIN0:
		mux_mask = CROSSBAR_MUX_CTRL_DPIN0_SELECT0 |
			   CROSSBAR_MUX_CTRL_DPIN0_SELECT1;
		mux_set =
			FIELD_PREP(CROSSBAR_MUX_CTRL_DPIN0_SELECT0, mux_state) |
			FIELD_PREP(CROSSBAR_MUX_CTRL_DPIN0_SELECT1, mux_state);
		atc_bit = ATC_DPIN0;
		break;
	case MUX_DPIN1:
		mux_mask = CROSSBAR_MUX_CTRL_DPIN1_SELECT0 |
			   CROSSBAR_MUX_CTRL_DPIN1_SELECT1;
		mux_set =
			FIELD_PREP(CROSSBAR_MUX_CTRL_DPIN1_SELECT0, mux_state) |
			FIELD_PREP(CROSSBAR_MUX_CTRL_DPIN1_SELECT1, mux_state);
		atc_bit = ATC_DPIN1;
		break;
	default:
		return -EINVAL;
	}

	spin_lock_irqsave(&dpxbar->lock, flags);

	/* ensure the selected dispext isn't already used in this crossbar */
	if (enable) {
		for (int i = 0; i < MUX_MAX; ++i) {
			if (i == index)
				continue;
			if (dpxbar->selected_dispext[i] == state) {
				spin_unlock_irqrestore(&dpxbar->lock, flags);
				return -EBUSY;
			}
		}
	}

	dpxbar_set32(dpxbar, OUT_N_CLK_EN, atc_bit);
	dpxbar_clear32(dpxbar, OUT_UNK_EN, atc_bit);
	dpxbar_clear32(dpxbar, OUT_PCLK1_EN, atc_bit);
	dpxbar_clear32(dpxbar, CROSSBAR_ATC_EN, atc_bit);

	if (dpxbar->selected_dispext[index] >= 0) {
		u32 prev_dispext_bit = 1 << dpxbar->selected_dispext[index];
		u32 prev_dispext_bit_en = 1 << (2 * dpxbar->selected_dispext[index]);

		dpxbar_set32(dpxbar, FIFO_WR_N_CLK_EN, prev_dispext_bit);
		dpxbar_set32(dpxbar, FIFO_RD_N_CLK_EN, prev_dispext_bit);
		dpxbar_clear32(dpxbar, FIFO_WR_UNK_EN, prev_dispext_bit);
		dpxbar_clear32(dpxbar, FIFO_RD_UNK_EN, prev_dispext_bit_en);
		dpxbar_clear32(dpxbar, FIFO_WR_DPTX_CLK_EN, prev_dispext_bit);
		dpxbar_clear32(dpxbar, FIFO_RD_PCLK1_EN, prev_dispext_bit);
		dpxbar_clear32(dpxbar, CROSSBAR_DISPEXT_EN, prev_dispext_bit);

		dpxbar->selected_dispext[index] = -1;
	}

	dpxbar_mask32(dpxbar, CROSSBAR_MUX_CTRL, mux_mask, mux_set);

	if (enable) {
		dpxbar_clear32(dpxbar, FIFO_WR_N_CLK_EN, dispext_bit);
		dpxbar_clear32(dpxbar, FIFO_RD_N_CLK_EN, dispext_bit);
		dpxbar_clear32(dpxbar, OUT_N_CLK_EN, atc_bit);
		dpxbar_set32(dpxbar, FIFO_WR_UNK_EN, dispext_bit);
		dpxbar_set32(dpxbar, FIFO_RD_UNK_EN, dispext_bit_en);
		dpxbar_set32(dpxbar, OUT_UNK_EN, atc_bit);
		dpxbar_set32(dpxbar, FIFO_WR_DPTX_CLK_EN, dispext_bit);
		dpxbar_set32(dpxbar, FIFO_RD_PCLK1_EN, dispext_bit);
		dpxbar_set32(dpxbar, OUT_PCLK1_EN, atc_bit);
		dpxbar_set32(dpxbar, CROSSBAR_ATC_EN, atc_bit);
		dpxbar_set32(dpxbar, CROSSBAR_DISPEXT_EN, dispext_bit);

		/*
		 * Work around some HW quirk:
		 * Without toggling the RD_PCLK enable here the connection
		 * doesn't come up. Testing has shown that a delay of about
		 * 5 usec is required which is doubled here to be on the
		 * safe side.
		 */
		dpxbar_clear32(dpxbar, FIFO_RD_PCLK1_EN, dispext_bit);
		udelay(10);
		dpxbar_set32(dpxbar, FIFO_RD_PCLK1_EN, dispext_bit);

		dpxbar->selected_dispext[index] = state;
	}

	spin_unlock_irqrestore(&dpxbar->lock, flags);

	if (enable)
		dev_info(dpxbar->dev, "Switched %s to dispext%u,%u\n",
			 apple_dpxbar_names[index], mux_state >> 1,
			 mux_state & 1);
	else
		dev_info(dpxbar->dev, "Switched %s to disconnected state\n",
			 apple_dpxbar_names[index]);

	return ret;
}

static const struct mux_control_ops apple_dpxbar_ops = {
	.set = apple_dpxbar_set,
};

static const struct mux_control_ops apple_dpxbar_t602x_ops = {
	.set = apple_dpxbar_set_t602x,
};

/*
 * Bring the clock/FIFO gates up on a mux whose dispext source is already
 * selected, without touching the mux selection itself -- used by
 * dcp_tunnel_crossbar_up() for a mux that dcp_typec_route_activate()
 * already selected. Same register set as apple_dpxbar_set_t602x()'s
 * enable path, but parameterized on the live selected_dispext[index]
 * instead of a hardcoded source, and this index's own atc_bit instead of
 * always dpin0's.
 */
int apple_dpxbar_link_up(struct mux_control *mux)
{
	struct apple_dpxbar *xbar;
	unsigned int index;
	unsigned long flags;
	u32 dispext_bit, atc_bit;
	int state, ret = 0;

	if (!mux || mux->chip->ops != &apple_dpxbar_t602x_ops)
		return -EINVAL;
	index = mux_control_get_index(mux);
	atc_bit = t602x_atc_bit(index);
	xbar = mux_chip_priv(mux->chip);

	spin_lock_irqsave(&xbar->lock, flags);
	state = xbar->selected_dispext[index];
	if (state < 0) {
		spin_unlock_irqrestore(&xbar->lock, flags);
		return -ENODEV;
	}
	dispext_bit = 1 << state;

	dpxbar_clear32(xbar, T602X_FIFO_WR_N_CLK_EN, dispext_bit);
	dpxbar_clear32(xbar, T602X_REG_014, dispext_bit);
	dpxbar_clear32(xbar, index == MUX_DPIN0 ? T602X_FIFO_RD_PCLK2_EN :
			     FIFO_RD_PCLK1_EN, atc_bit);
	udelay(1);

	dpxbar_set32(xbar, T602X_FIFO_WR_UNK_EN, dispext_bit);
	dpxbar_mask32(xbar, T602X_REG_018, GENMASK(5, 4), BIT(4));
	/*
	 * The only hardware-verified value for this 2-bit field is dpin0's
	 * BIT(0), which is both ATC_DPIN0 and MUX_DPIN0's own enum value (1)
	 * coincidentally overlapping at bit 0 -- so this field's encoding
	 * for a second index was never independently confirmed on real
	 * hardware. FIELD_PREP(..., index), matching
	 * MUX_DPPHY=0/MUX_DPIN0=1/MUX_DPIN1=2, is an inferred generalization
	 * of that single known-good dpin0 data point, not a value verified
	 * against dpin1 hardware. If dpin1 misbehaves, this field is a
	 * prime suspect.
	 */
	dpxbar_mask32(xbar, T602X_FIFO_RD_N_CLK_EN, GENMASK(1, 0),
		      FIELD_PREP(GENMASK(1, 0), index));
	dpxbar_set32(xbar, T602X_FIFO_WR_DPTX_CLK_EN, dispext_bit);
	dpxbar_set32(xbar, T602X_REG_00C, dispext_bit);
	dpxbar_set32(xbar, T602X_REG_01C, atc_bit);
	dpxbar_set32(xbar, T602X_REG_034, atc_bit);
	dpxbar_set32(xbar, CROSSBAR_ATC_EN, atc_bit);
	dpxbar_set32(xbar, CROSSBAR_DISPEXT_EN, dispext_bit);
	dpxbar_set32(xbar, T602X_FIFO_RD_UNK_EN, dispext_bit);
	spin_unlock_irqrestore(&xbar->lock, flags);

	dev_info(xbar->dev, "%s: crossbar link up (dispext=%d atc=0x%x)\n",
		 apple_dpxbar_names[index], state, atc_bit);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dpxbar_link_up);

/*
 * Inverse of apple_dpxbar_link_up(): gate the clocks back down without
 * deselecting the mux or dropping CROSSBAR_ATC_EN/CROSSBAR_DISPEXT_EN --
 * mux selection and the ATC output enable are kept across a link
 * down/up cycle. Re-asserts the same reset bits apple_dpxbar_link_up()
 * releases, so a later link_up() on the same route starts from the same
 * state as a fresh selection.
 */
int apple_dpxbar_link_down(struct mux_control *mux)
{
	struct apple_dpxbar *xbar;
	unsigned int index;
	unsigned long flags;
	u32 dispext_bit, atc_bit;
	int state;

	if (!mux || mux->chip->ops != &apple_dpxbar_t602x_ops)
		return -EINVAL;
	index = mux_control_get_index(mux);
	atc_bit = t602x_atc_bit(index);
	xbar = mux_chip_priv(mux->chip);

	spin_lock_irqsave(&xbar->lock, flags);
	state = xbar->selected_dispext[index];
	if (state < 0) {
		spin_unlock_irqrestore(&xbar->lock, flags);
		return -ENODEV;
	}
	dispext_bit = 1 << state;

	dpxbar_set32(xbar, T602X_FIFO_WR_N_CLK_EN, dispext_bit);
	dpxbar_set32(xbar, T602X_REG_014, dispext_bit);
	dpxbar_set32(xbar, index == MUX_DPIN0 ? T602X_FIFO_RD_PCLK2_EN :
			   FIFO_RD_PCLK1_EN, atc_bit);
	spin_unlock_irqrestore(&xbar->lock, flags);

	dev_info(xbar->dev, "%s: crossbar link down (dispext=%d atc=0x%x)\n",
		 apple_dpxbar_names[index], state, atc_bit);
	return 0;
}
EXPORT_SYMBOL_GPL(apple_dpxbar_link_down);

static int apple_dpxbar_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mux_chip *mux_chip;
	struct apple_dpxbar *dpxbar;
	const struct apple_dpxbar_hw *hw;
	int ret;

	hw = of_device_get_match_data(dev);
	mux_chip = devm_mux_chip_alloc(dev, MUX_MAX, sizeof(*dpxbar));
	if (IS_ERR(mux_chip))
		return PTR_ERR(mux_chip);

	dpxbar = mux_chip_priv(mux_chip);
	mux_chip->ops = hw->ops;
	spin_lock_init(&dpxbar->lock);

	dpxbar->dev = dev;
	dpxbar->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dpxbar->regs))
		return PTR_ERR(dpxbar->regs);

	if (!of_device_is_compatible(dev->of_node, "apple,t6020-display-crossbar")) {
		readl(dpxbar->regs + UNK_TUNABLE);
		writel(hw->tunable, dpxbar->regs + UNK_TUNABLE);
		readl(dpxbar->regs + UNK_TUNABLE);
	}

	for (unsigned int i = 0; i < MUX_MAX; ++i) {
		mux_chip->mux[i].states = hw->n_ufp;
		mux_chip->mux[i].idle_state = MUX_IDLE_DISCONNECT;
		dpxbar->selected_dispext[i] = -1;
	}

	ret = devm_mux_chip_register(dev, mux_chip);
	if (ret < 0)
		return ret;

	return 0;
}

static const struct apple_dpxbar_hw apple_dpxbar_hw_t8103 = {
	.n_ufp = 2,
	.tunable = 0,
	.ops = &apple_dpxbar_ops,
};

static const struct apple_dpxbar_hw apple_dpxbar_hw_t8112 = {
	.n_ufp = 4,
	.tunable = 4278196325,
	.ops = &apple_dpxbar_ops,
};

static const struct apple_dpxbar_hw apple_dpxbar_hw_t6000 = {
	.n_ufp = 9,
	.tunable = 5,
	.ops = &apple_dpxbar_ops,
};

static const struct apple_dpxbar_hw apple_dpxbar_hw_t6020 = {
	.n_ufp = 9,
	.ops = &apple_dpxbar_t602x_ops,
};

static const struct of_device_id apple_dpxbar_ids[] = {
	{
		.compatible = "apple,t8103-display-crossbar",
		.data = &apple_dpxbar_hw_t8103,
	},
	{
		.compatible = "apple,t8112-display-crossbar",
		.data = &apple_dpxbar_hw_t8112,
	},
	{
		.compatible = "apple,t6000-display-crossbar",
		.data = &apple_dpxbar_hw_t6000,
	},
	{
		.compatible = "apple,t6020-display-crossbar",
		.data = &apple_dpxbar_hw_t6020,
	},
	{}
};
MODULE_DEVICE_TABLE(of, apple_dpxbar_ids);

static struct platform_driver apple_dpxbar_driver = {
	.driver = {
		.name = "apple-display-crossbar",
		.of_match_table	= apple_dpxbar_ids,
	},
	.probe = apple_dpxbar_probe,
};
module_platform_driver(apple_dpxbar_driver);

MODULE_DESCRIPTION("Apple Silicon display crossbar multiplexer driver");
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
MODULE_LICENSE("GPL v2");
