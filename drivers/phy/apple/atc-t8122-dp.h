/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * DisplayPort AUX and main-link sequences for the T8122 generation of the
 * Apple Type-C PHY (M3 family: t8122, t6030, t6031).
 *
 * Each of the two lane pairs ("PMAs") of the PHY carries two DisplayPort
 * lanes in DP mode: the transmitter block and the receiver block, which then
 * runs as a second transmitter. The helpers here only touch the PHY's core
 * register window. The caller holds the PHY lock, has the PHY powered with its
 * reset released and the common block calibrated, and owns the AUSPLL.
 */
#ifndef _APPLE_ATC_T8122_DP_H
#define _APPLE_ATC_T8122_DP_H

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/types.h>

/* AUX channel block */
#define ATC_T8122_AUX_CTRL 0x16000
#define ATC_T8122_AUX_PWRDN BIT(0)
#define ATC_T8122_AUX_PWR 0x16400
#define ATC_T8122_AUX_SLEEP_SMALL_OV BIT(0)
#define ATC_T8122_AUX_SLEEP_SMALL BIT(1)
#define ATC_T8122_AUX_SLEEP_BIG_OV BIT(2)
#define ATC_T8122_AUX_SLEEP_BIG BIT(3)
#define ATC_T8122_AUX_CLAMP_OV BIT(8)
#define ATC_T8122_AUX_CLAMP BIT(9)
#define ATC_T8122_AUX_PWR_ON_MASK                                          \
	(ATC_T8122_AUX_SLEEP_SMALL_OV | ATC_T8122_AUX_SLEEP_SMALL |       \
	 ATC_T8122_AUX_SLEEP_BIG_OV | ATC_T8122_AUX_SLEEP_BIG |           \
	 ATC_T8122_AUX_CLAMP_OV | ATC_T8122_AUX_CLAMP)
#define ATC_T8122_AUX_PWR_ON                                               \
	(ATC_T8122_AUX_SLEEP_SMALL_OV | ATC_T8122_AUX_SLEEP_SMALL |       \
	 ATC_T8122_AUX_SLEEP_BIG_OV | ATC_T8122_AUX_SLEEP_BIG |           \
	 ATC_T8122_AUX_CLAMP_OV)

/* Per lane pair sleep and clamp overrides, @l is the lane pair */
#define ATC_T8122_RX_PWR 0x0008
#define ATC_T8122_RX_SLEEP_SMALL(l) BIT(6 + (l))
#define ATC_T8122_RX_SLEEP_SMALL_OV(l) BIT(8 + (l))
#define ATC_T8122_RX_SLEEP_BIG(l) BIT(10 + (l))
#define ATC_T8122_RX_SLEEP_BIG_OV(l) BIT(12 + (l))
#define ATC_T8122_RX_CLAMP(l) BIT(14 + (l))
#define ATC_T8122_RX_CLAMP_OV(l) BIT(16 + (l))
#define ATC_T8122_RXTX_PWR 0x000c
#define ATC_T8122_RXTX_SLEEP_BIG(l) BIT(l)
#define ATC_T8122_RXTX_SLEEP_BIG_OV(l) BIT(2 + (l))
#define ATC_T8122_RXTX_SLEEP_SMALL(l) BIT(4 + (l))
#define ATC_T8122_RXTX_SLEEP_SMALL_OV(l) BIT(6 + (l))
#define ATC_T8122_TX_PWR 0x010c
#define ATC_T8122_TX_SLEEP_BIG(l) BIT(l)
#define ATC_T8122_TX_SLEEP_BIG_OV(l) BIT(2 + (l))
#define ATC_T8122_TX_SLEEP_SMALL(l) BIT(4 + (l))
#define ATC_T8122_TX_SLEEP_SMALL_OV(l) BIT(6 + (l))
#define ATC_T8122_TX_CLAMP(l) BIT(8 + (l))
#define ATC_T8122_TX_CLAMP_OV(l) BIT(10 + (l))

/* Lane pair register blocks; pair 1 sits 0x7000 above pair 0 */
#define ATC_T8122_LANE_STRIDE 0x7000
#define ATC_T8122_RX_TOP 0x9000
#define ATC_T8122_RX_SHM 0xb000
#define ATC_T8122_TX_TOP 0xc000
#define ATC_T8122_TX_SHM 0xd000

/* Equalizer, at the same offsets in TX_TOP and (+0x1c0) in RX_TOP */
#define ATC_T8122_TX_EQ1 0x0050
#define ATC_T8122_TX_EQ17 0x00d0
#define ATC_T8122_TX_EQ18 0x00d4
#define ATC_T8122_RX_EQ1 0x0210
#define ATC_T8122_RX_EQ17 0x0244
#define ATC_T8122_RX_EQ18 0x0248
#define ATC_T8122_EQ1_SWING GENMASK(3, 0)
#define ATC_T8122_EQ1_SWING_LSB GENMASK(15, 14)
#define ATC_T8122_EQ17_DEEMPH GENMASK(18, 0)
#define ATC_T8122_EQ17_AUTO_CTRL_DP BIT(20)
#define ATC_T8122_EQ18_SWING_OV BIT(28)

/* TX_SHM */
#define ATC_T8122_TX_MAIN2 0x0004
#define ATC_T8122_MAIN2_BYTECLK_SYNC_SEL BIT(24)
#define ATC_T8122_MAIN2_BYTECLK_SYNC_SEL_OV BIT(25)
#define ATC_T8122_MAIN2_BYTECLK_SYNC_EN BIT(28)
#define ATC_T8122_MAIN2_BYTECLK_SYNC_EN_OV BIT(29)
#define ATC_T8122_TX_MAIN4 0x000c
#define ATC_T8122_MAIN4_DIV2_4_EN GENMASK(25, 24)
#define ATC_T8122_MAIN4_DIV2_4_EN_OV BIT(26)
#define ATC_T8122_TX_MAIN5 0x0010
#define ATC_T8122_MAIN5_CLK_EN BIT(9)
#define ATC_T8122_MAIN5_CLK_EN_OV BIT(10)
#define ATC_T8122_TX_CLKMON1 0x001c
#define ATC_T8122_CLKMON1_HIZ BIT(22)
#define ATC_T8122_CLKMON1_HIZ_OV BIT(23)
#define ATC_T8122_TX_LDOCLK1 0x0044
#define ATC_T8122_LDOCLK1_INIT 0x0aafb800
#define ATC_T8122_LDOCLK1_VREG_ADJ GENMASK(10, 6)
#define ATC_T8122_LDOCLK1_VREF_EN BIT(12)
#define ATC_T8122_LDOCLK1_VREF_FILTER_BOOST (GENMASK(18, 17) | GENMASK(15, 14))
#define ATC_T8122_LDOCLK1_BYPASS_SMALL BIT(20)
#define ATC_T8122_LDOCLK1_BYPASS_BIG BIT(22)
#define ATC_T8122_LDOCLK1_EN_SMALL BIT(24)
#define ATC_T8122_LDOCLK1_EN_BIG BIT(26)

/* RX_TOP */
#define ATC_T8122_RX_TXMODE 0x0160
#define ATC_T8122_RX_TXMODE_EN BIT(0)
#define ATC_T8122_RX_AFE1 0x01d4
#define ATC_T8122_AFE1_DIV20_RESET_N_OV BIT(0)
#define ATC_T8122_AFE1_DIV20_RESET_N BIT(1)
#define ATC_T8122_RX_DCO1 0x01dc
#define ATC_T8122_DCO1_LPBKIN_RECOVERED_DATA GENMASK(16, 15)

/* RX_SHM */
#define ATC_T8122_RX_CTLE2 0x0030
#define ATC_T8122_CTLE2_TX_CLK_EN BIT(10)
#define ATC_T8122_CTLE2_TX_CLK_EN_OV BIT(11)
#define ATC_T8122_RX_DFE10 0x0064
#define ATC_T8122_DFE10_DFEH1FB_EN BIT(2)
#define ATC_T8122_DFE10_DFEH1FB_EN_OV BIT(3)
#define ATC_T8122_DFE10_DTVREG_ADJUST GENMASK(15, 10)
#define ATC_T8122_RX_DFE11 0x0068
#define ATC_T8122_DFE11_DTVREG_BIG_EN BIT(5)
#define ATC_T8122_DFE11_DTVREG_BIG_EN_OV BIT(6)
#define ATC_T8122_DFE11_DTVREG_SMALL_EN BIT(7)
#define ATC_T8122_DFE11_DTVREG_SMALL_EN_OV BIT(8)
#define ATC_T8122_RX_DFE12 0x006c
#define ATC_T8122_DFE12_TX_BYTECLK_SYNC_CLR BIT(9)
#define ATC_T8122_DFE12_TX_BYTECLK_SYNC_CLR_OV BIT(10)
#define ATC_T8122_DFE12_TX_BYTECLK_SYNC_EN BIT(11)
#define ATC_T8122_DFE12_TX_HRCLK_SEL BIT(13)
#define ATC_T8122_DFE12_TX_HRCLK_SEL_OV BIT(14)
#define ATC_T8122_DFE12_TX_PBIAS_EN BIT(15)
#define ATC_T8122_DFE12_TX_PBIAS_EN_OV BIT(16)
#define ATC_T8122_RX_SAVOS16 0x00a4
#define ATC_T8122_SAVOS16_RXTERM_EN BIT(0)
#define ATC_T8122_SAVOS16_RXTERM_EN_OV BIT(1)
#define ATC_T8122_RX_TERM19 0x00a8
#define ATC_T8122_TERM19_TX_TEST_EN BIT(15)
#define ATC_T8122_TERM19_TX_TEST_EN_OV BIT(16)
#define ATC_T8122_TERM19_TX_EN BIT(17)
#define ATC_T8122_TERM19_TX_EN_OV BIT(18)
#define ATC_T8122_TERM19_TX_DIV2_EN GENMASK(20, 19)
#define ATC_T8122_TERM19_TX_DIV2_EN_OV BIT(21)
#define ATC_T8122_TERM19_TX_DIV2_RST BIT(22)
#define ATC_T8122_TERM19_TX_DIV2_RST_OV BIT(23)
#define ATC_T8122_RX_VREF22 0x00d4
#define ATC_T8122_VREF22_INIT_FIELDS (GENMASK(20, 16) | GENMASK(12, 7))
#define ATC_T8122_VREF22_INIT 0x121180
#define ATC_T8122_VREF22_BIAS GENMASK(15, 13)
#define ATC_T8122_VREF22_EN BIT(16)
#define ATC_T8122_VREF22_FILTER_ADJUST GENMASK(19, 18)
#define ATC_T8122_RX_VREF23 0x00d8
#define ATC_T8122_VREF23_BOOST GENMASK(1, 0)
#define ATC_T8122_VREF23_BOOST_OV BIT(2)
#define ATC_T8122_RX_VREG1 0x00dc
#define ATC_T8122_VREG1_DTVREG_POWER_MODE GENMASK(4, 3)

/* Initial equalizer settings, before the sink requests a drive level */
#define ATC_T8122_TX_EQ17_INIT 0x00001
#define ATC_T8122_RX_EQ17_INIT 0x01f81

/**
 * struct atc_t8122_dp_rate - AUSPLL descriptor for one DisplayPort link rate
 * @link_rate: Link rate in Mb/s per lane
 * @freq_desc: AUSPLL_FREQ_DESC_A, _B and _C
 * @div2: The lane transmitters run their clock dividers (RBR only)
 */
struct atc_t8122_dp_rate {
	unsigned int link_rate;
	u32 freq_desc[3];
	bool div2;
};

/*
 * Descriptor fields: A holds the frequency counter target and the loop
 * coefficients (KI 8/3, KP 8/7), B the fractional feedback divider, C the
 * PCLK divider, LFSDM/LFCLK and the VCLK dividers.
 */
static const struct atc_t8122_dp_rate atc_t8122_dp_rates[] = {
	{ 1620, { 0x1e0e021c, 0x00000000, 0x00156600 }, true },
	{ 2700, { 0x1e0e01c2, 0x07fffffe, 0x00155200 }, false },
	{ 5400, { 0x1e0e01c2, 0x07fffffe, 0x00554800 }, false },
	{ 8100, { 0x1e0e02a3, 0x0bff7ffc, 0x00564800 }, false },
};

static inline const struct atc_t8122_dp_rate *atc_t8122_dp_rate(unsigned int link_rate)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(atc_t8122_dp_rates); i++)
		if (atc_t8122_dp_rates[i].link_rate == link_rate)
			return &atc_t8122_dp_rates[i];
	return NULL;
}

/*
 * Packed drive presets, indexed by voltage swing level and pre-emphasis level.
 * Bits [1:0] are the swing LSBs, [5:2] the swing and [23:6] the de-emphasis
 * code. Zero marks a combination DisplayPort does not allow (swing plus
 * pre-emphasis above level 3).
 */
static const u32 atc_t8122_dp_presets[4][4] = {
	{ 0x03f024, 0x237013, 0x37200b, 0x46e004 },
	{ 0x03f01c, 0x23700b, 0x372004, 0 },
	{ 0x03f013, 0x237004, 0, 0 },
	{ 0x03f004, 0, 0, 0 },
};

static inline int atc_t8122_dp_preset(unsigned int voltage, unsigned int pre, u32 *preset)
{
	if (voltage > 3 || pre > 3 || voltage + pre > 3)
		return -EINVAL;
	*preset = atc_t8122_dp_presets[voltage][pre];
	return 0;
}

/* EQ1 swing fields for a packed preset */
static inline u32 atc_t8122_dp_preset_swing(u32 preset)
{
	return FIELD_PREP(ATC_T8122_EQ1_SWING_LSB, preset & 0x3) |
	       FIELD_PREP(ATC_T8122_EQ1_SWING, (preset >> 2) & 0xf);
}

/* EQ17 de-emphasis field for a packed preset; bit 0 enables the code */
static inline u32 atc_t8122_dp_preset_deemph(u32 preset)
{
	return FIELD_PREP(ATC_T8122_EQ17_DEEMPH, ((preset >> 6) << 1) | 1);
}

/*
 * Map DisplayPort lane @lane of a link over the lane pairs in @pairs (bit
 * mask) to a lane pair and block. Lanes fill the pairs in ascending order;
 * within a pair the receiver block carries the lower lane.
 */
static inline int atc_t8122_dp_lane_map(u8 pairs, unsigned int lane, unsigned int *pair,
					bool *tx)
{
	unsigned int n = lane / 2;

	if (!pairs || pairs > 3 || lane >= 2 * hweight8(pairs))
		return -EINVAL;
	*pair = (pairs == 2 || (pairs == 3 && n)) ? 1 : 0;
	*tx = lane & 1;
	return 0;
}

static inline void atc_t8122_dp_mask(void __iomem *reg, u32 mask, u32 set)
{
	writel((readl(reg) & ~mask) | set, reg);
}

static inline void __iomem *atc_t8122_dp_lane(void __iomem *core, unsigned int pair, u32 block)
{
	return core + block + pair * ATC_T8122_LANE_STRIDE;
}

static inline void atc_t8122_dp_aux_on(void __iomem *core)
{
	atc_t8122_dp_mask(core + ATC_T8122_AUX_PWR, 0,
			  ATC_T8122_AUX_SLEEP_SMALL_OV | ATC_T8122_AUX_SLEEP_SMALL);
	udelay(1);
	atc_t8122_dp_mask(core + ATC_T8122_AUX_PWR, 0,
			  ATC_T8122_AUX_SLEEP_BIG_OV | ATC_T8122_AUX_SLEEP_BIG);
	udelay(1);
	atc_t8122_dp_mask(core + ATC_T8122_AUX_PWR, ATC_T8122_AUX_CLAMP_OV | ATC_T8122_AUX_CLAMP,
			  ATC_T8122_AUX_CLAMP_OV);
	udelay(1);
	atc_t8122_dp_mask(core + ATC_T8122_AUX_CTRL, ATC_T8122_AUX_PWRDN, 0);
	udelay(1);
}

static inline void atc_t8122_dp_aux_off(void __iomem *core)
{
	atc_t8122_dp_mask(core + ATC_T8122_AUX_CTRL, 0, ATC_T8122_AUX_PWRDN);
	udelay(1);
	atc_t8122_dp_mask(core + ATC_T8122_AUX_PWR, 0, ATC_T8122_AUX_CLAMP);
	udelay(1);
	atc_t8122_dp_mask(core + ATC_T8122_AUX_PWR, ATC_T8122_AUX_SLEEP_BIG, 0);
	udelay(1);
	atc_t8122_dp_mask(core + ATC_T8122_AUX_PWR, ATC_T8122_AUX_SLEEP_SMALL, 0);
	udelay(1);
}

static inline bool atc_t8122_dp_aux_is_on(void __iomem *core)
{
	return (readl(core + ATC_T8122_AUX_PWR) & ATC_T8122_AUX_PWR_ON_MASK) ==
		       ATC_T8122_AUX_PWR_ON &&
	       !(readl(core + ATC_T8122_AUX_CTRL) & ATC_T8122_AUX_PWRDN);
}

static inline void atc_t8122_dp_tx_power(void __iomem *core, unsigned int pair, bool on)
{
	void __iomem *reg = core + ATC_T8122_TX_PWR;

	if (on) {
		atc_t8122_dp_mask(reg, 0, ATC_T8122_TX_SLEEP_SMALL(pair) |
					  ATC_T8122_TX_SLEEP_SMALL_OV(pair));
		udelay(1);
		atc_t8122_dp_mask(reg, 0, ATC_T8122_TX_SLEEP_BIG(pair) |
					  ATC_T8122_TX_SLEEP_BIG_OV(pair));
		udelay(1);
		atc_t8122_dp_mask(reg, ATC_T8122_TX_CLAMP(pair), ATC_T8122_TX_CLAMP_OV(pair));
		udelay(1);
	} else {
		atc_t8122_dp_mask(reg, 0, ATC_T8122_TX_CLAMP(pair));
		udelay(1);
		atc_t8122_dp_mask(reg, ATC_T8122_TX_SLEEP_BIG(pair), 0);
		udelay(1);
		atc_t8122_dp_mask(reg, ATC_T8122_TX_SLEEP_SMALL(pair), 0);
		udelay(1);
	}
}

/* Power for the receiver block, which transmits in DisplayPort mode */
static inline void atc_t8122_dp_rxtx_power(void __iomem *core, unsigned int pair, bool on)
{
	void __iomem *rxtx = core + ATC_T8122_RXTX_PWR;
	void __iomem *rx = core + ATC_T8122_RX_PWR;

	if (on) {
		atc_t8122_dp_mask(rxtx, 0, ATC_T8122_RXTX_SLEEP_SMALL(pair) |
					   ATC_T8122_RXTX_SLEEP_SMALL_OV(pair));
		udelay(1);
		atc_t8122_dp_mask(rxtx, 0, ATC_T8122_RXTX_SLEEP_BIG(pair) |
					   ATC_T8122_RXTX_SLEEP_BIG_OV(pair));
		udelay(1);
		atc_t8122_dp_mask(rx, 0, ATC_T8122_RX_SLEEP_BIG(pair) |
					 ATC_T8122_RX_SLEEP_BIG_OV(pair));
		udelay(1);
		atc_t8122_dp_mask(rx, 0, ATC_T8122_RX_SLEEP_SMALL(pair) |
					 ATC_T8122_RX_SLEEP_SMALL_OV(pair));
		udelay(1);
		atc_t8122_dp_mask(rx, ATC_T8122_RX_CLAMP(pair), ATC_T8122_RX_CLAMP_OV(pair));
		udelay(1);
	} else {
		atc_t8122_dp_mask(rx, 0, ATC_T8122_RX_CLAMP(pair));
		udelay(1);
		atc_t8122_dp_mask(rx, ATC_T8122_RX_SLEEP_SMALL(pair), 0);
		udelay(1);
		atc_t8122_dp_mask(rx, ATC_T8122_RX_SLEEP_BIG(pair), 0);
		udelay(1);
		atc_t8122_dp_mask(rxtx, ATC_T8122_RXTX_SLEEP_BIG(pair), 0);
		udelay(1);
		atc_t8122_dp_mask(rxtx, ATC_T8122_RXTX_SLEEP_SMALL(pair), 0);
		udelay(1);
	}
}

/* Override the receiver feedback path before the PMA lane reset is released */
static inline void atc_t8122_dp_lane_pre_reset(void __iomem *core, unsigned int pair)
{
	atc_t8122_dp_mask(atc_t8122_dp_lane(core, pair, ATC_T8122_RX_SHM) + ATC_T8122_RX_DFE10,
			  ATC_T8122_DFE10_DFEH1FB_EN | ATC_T8122_DFE10_DFEH1FB_EN_OV,
			  ATC_T8122_DFE10_DFEH1FB_EN_OV);
}

static inline void atc_t8122_dp_tx_start(void __iomem *core, unsigned int pair, bool div2)
{
	void __iomem *top = atc_t8122_dp_lane(core, pair, ATC_T8122_TX_TOP);
	void __iomem *shm = atc_t8122_dp_lane(core, pair, ATC_T8122_TX_SHM);

	atc_t8122_dp_tx_power(core, pair, true);
	writel(ATC_T8122_LDOCLK1_INIT, shm + ATC_T8122_TX_LDOCLK1);
	ndelay(500);
	atc_t8122_dp_mask(shm + ATC_T8122_TX_LDOCLK1, 0, ATC_T8122_LDOCLK1_EN_SMALL);
	ndelay(250);
	atc_t8122_dp_mask(shm + ATC_T8122_TX_LDOCLK1, 0, ATC_T8122_LDOCLK1_EN_BIG);
	ndelay(250);
	atc_t8122_dp_mask(shm + ATC_T8122_TX_LDOCLK1, ATC_T8122_LDOCLK1_VREG_ADJ,
			  FIELD_PREP(ATC_T8122_LDOCLK1_VREG_ADJ, 12));
	ndelay(250);
	atc_t8122_dp_mask(shm + ATC_T8122_TX_LDOCLK1, ATC_T8122_LDOCLK1_VREF_FILTER_BOOST, 0);
	ndelay(250);

	atc_t8122_dp_mask(top + ATC_T8122_TX_EQ17, 0, ATC_T8122_EQ17_AUTO_CTRL_DP);
	atc_t8122_dp_mask(top + ATC_T8122_TX_EQ1,
			  ATC_T8122_EQ1_SWING | ATC_T8122_EQ1_SWING_LSB, 0);
	atc_t8122_dp_mask(top + ATC_T8122_TX_EQ18, 0, ATC_T8122_EQ18_SWING_OV);
	atc_t8122_dp_mask(top + ATC_T8122_TX_EQ17, ATC_T8122_EQ17_DEEMPH,
			  ATC_T8122_TX_EQ17_INIT);

	atc_t8122_dp_mask(shm + ATC_T8122_TX_LDOCLK1, 0, ATC_T8122_LDOCLK1_BYPASS_SMALL);
	ndelay(250);
	atc_t8122_dp_mask(shm + ATC_T8122_TX_LDOCLK1, 0, ATC_T8122_LDOCLK1_BYPASS_BIG);
	udelay(1);

	atc_t8122_dp_mask(shm + ATC_T8122_TX_MAIN2,
			  ATC_T8122_MAIN2_BYTECLK_SYNC_SEL | ATC_T8122_MAIN2_BYTECLK_SYNC_SEL_OV,
			  ATC_T8122_MAIN2_BYTECLK_SYNC_SEL_OV);
	atc_t8122_dp_mask(shm + ATC_T8122_TX_MAIN2, 0,
			  ATC_T8122_MAIN2_BYTECLK_SYNC_EN | ATC_T8122_MAIN2_BYTECLK_SYNC_EN_OV);
	atc_t8122_dp_mask(shm + ATC_T8122_TX_MAIN4,
			  ATC_T8122_MAIN4_DIV2_4_EN | ATC_T8122_MAIN4_DIV2_4_EN_OV,
			  FIELD_PREP(ATC_T8122_MAIN4_DIV2_4_EN, div2) |
			  ATC_T8122_MAIN4_DIV2_4_EN_OV);
	atc_t8122_dp_mask(shm + ATC_T8122_TX_MAIN5, 0,
			  ATC_T8122_MAIN5_CLK_EN | ATC_T8122_MAIN5_CLK_EN_OV);
	atc_t8122_dp_mask(shm + ATC_T8122_TX_CLKMON1,
			  ATC_T8122_CLKMON1_HIZ | ATC_T8122_CLKMON1_HIZ_OV,
			  ATC_T8122_CLKMON1_HIZ_OV);
}

static inline void atc_t8122_dp_rxtx_start(void __iomem *core, unsigned int pair, bool div2)
{
	void __iomem *top = atc_t8122_dp_lane(core, pair, ATC_T8122_RX_TOP);
	void __iomem *shm = atc_t8122_dp_lane(core, pair, ATC_T8122_RX_SHM);

	atc_t8122_dp_mask(top + ATC_T8122_RX_TXMODE, 0, ATC_T8122_RX_TXMODE_EN);
	atc_t8122_dp_mask(top + ATC_T8122_RX_DCO1, ATC_T8122_DCO1_LPBKIN_RECOVERED_DATA,
			  ATC_T8122_DCO1_LPBKIN_RECOVERED_DATA);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_SAVOS16,
			  ATC_T8122_SAVOS16_RXTERM_EN | ATC_T8122_SAVOS16_RXTERM_EN_OV,
			  ATC_T8122_SAVOS16_RXTERM_EN_OV);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_TERM19,
			  ATC_T8122_TERM19_TX_TEST_EN | ATC_T8122_TERM19_TX_TEST_EN_OV,
			  ATC_T8122_TERM19_TX_TEST_EN_OV);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_DFE12,
			  ATC_T8122_DFE12_TX_PBIAS_EN | ATC_T8122_DFE12_TX_PBIAS_EN_OV,
			  ATC_T8122_DFE12_TX_PBIAS_EN_OV);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_VREF22, ATC_T8122_VREF22_INIT_FIELDS,
			  ATC_T8122_VREF22_INIT);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_DFE10, ATC_T8122_DFE10_DTVREG_ADJUST,
			  FIELD_PREP(ATC_T8122_DFE10_DTVREG_ADJUST, 0x2e));
	atc_t8122_dp_mask(shm + ATC_T8122_RX_VREF23,
			  ATC_T8122_VREF23_BOOST | ATC_T8122_VREF23_BOOST_OV,
			  ATC_T8122_VREF23_BOOST_OV);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_VREG1, ATC_T8122_VREG1_DTVREG_POWER_MODE, 0);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_VREF22, ATC_T8122_VREF22_BIAS,
			  FIELD_PREP(ATC_T8122_VREF22_BIAS, 6));
	ndelay(100);

	atc_t8122_dp_rxtx_power(core, pair, true);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_DFE12, 0, ATC_T8122_DFE12_TX_BYTECLK_SYNC_EN);
	atc_t8122_dp_mask(top + ATC_T8122_RX_AFE1,
			  ATC_T8122_AFE1_DIV20_RESET_N | ATC_T8122_AFE1_DIV20_RESET_N_OV,
			  ATC_T8122_AFE1_DIV20_RESET_N_OV);
	ndelay(10);
	atc_t8122_dp_mask(top + ATC_T8122_RX_AFE1, 0, ATC_T8122_AFE1_DIV20_RESET_N);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_DFE12,
			  ATC_T8122_DFE12_TX_BYTECLK_SYNC_CLR | ATC_T8122_DFE12_TX_BYTECLK_SYNC_CLR_OV,
			  ATC_T8122_DFE12_TX_BYTECLK_SYNC_CLR_OV);
	ndelay(100);

	atc_t8122_dp_mask(shm + ATC_T8122_RX_VREF22,
			  ATC_T8122_VREF22_EN | ATC_T8122_VREF22_FILTER_ADJUST,
			  ATC_T8122_VREF22_EN | FIELD_PREP(ATC_T8122_VREF22_FILTER_ADJUST, 1));
	atc_t8122_dp_mask(shm + ATC_T8122_RX_VREF23, ATC_T8122_VREF23_BOOST,
			  FIELD_PREP(ATC_T8122_VREF23_BOOST, 1));
	ndelay(500);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_VREF22, ATC_T8122_VREF22_FILTER_ADJUST, 0);
	ndelay(200);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_DFE11, 0,
			  ATC_T8122_DFE11_DTVREG_SMALL_EN | ATC_T8122_DFE11_DTVREG_SMALL_EN_OV);
	ndelay(200);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_DFE11, 0,
			  ATC_T8122_DFE11_DTVREG_BIG_EN | ATC_T8122_DFE11_DTVREG_BIG_EN_OV);
	ndelay(200);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_VREF23, ATC_T8122_VREF23_BOOST,
			  FIELD_PREP(ATC_T8122_VREF23_BOOST, 3));
	udelay(1);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_VREF23, ATC_T8122_VREF23_BOOST,
			  FIELD_PREP(ATC_T8122_VREF23_BOOST, 2));
	udelay(1);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_VREF23, ATC_T8122_VREF23_BOOST, 0);
	ndelay(750);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_TERM19, 0,
			  ATC_T8122_TERM19_TX_EN | ATC_T8122_TERM19_TX_EN_OV);
	udelay(1);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_DFE12,
			  ATC_T8122_DFE12_TX_HRCLK_SEL | ATC_T8122_DFE12_TX_HRCLK_SEL_OV,
			  ATC_T8122_DFE12_TX_HRCLK_SEL_OV);

	atc_t8122_dp_mask(top + ATC_T8122_RX_EQ1,
			  ATC_T8122_EQ1_SWING | ATC_T8122_EQ1_SWING_LSB, 0);
	atc_t8122_dp_mask(top + ATC_T8122_RX_EQ18, 0, ATC_T8122_EQ18_SWING_OV);
	atc_t8122_dp_mask(top + ATC_T8122_RX_EQ17, ATC_T8122_EQ17_DEEMPH,
			  ATC_T8122_RX_EQ17_INIT);

	atc_t8122_dp_mask(shm + ATC_T8122_RX_TERM19,
			  ATC_T8122_TERM19_TX_DIV2_EN | ATC_T8122_TERM19_TX_DIV2_EN_OV,
			  FIELD_PREP(ATC_T8122_TERM19_TX_DIV2_EN, div2) |
			  ATC_T8122_TERM19_TX_DIV2_EN_OV);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_TERM19,
			  ATC_T8122_TERM19_TX_DIV2_RST | ATC_T8122_TERM19_TX_DIV2_RST_OV,
			  ATC_T8122_TERM19_TX_DIV2_RST_OV);
	ndelay(50);
	atc_t8122_dp_mask(shm + ATC_T8122_RX_CTLE2, 0,
			  ATC_T8122_CTLE2_TX_CLK_EN | ATC_T8122_CTLE2_TX_CLK_EN_OV);
	ndelay(100);
}

static inline void atc_t8122_dp_lane_start(void __iomem *core, unsigned int pair, bool div2)
{
	atc_t8122_dp_tx_start(core, pair, div2);
	atc_t8122_dp_rxtx_start(core, pair, div2);
}

static inline void atc_t8122_dp_lane_stop(void __iomem *core, unsigned int pair)
{
	void __iomem *tx = atc_t8122_dp_lane(core, pair, ATC_T8122_TX_SHM);
	void __iomem *top = atc_t8122_dp_lane(core, pair, ATC_T8122_RX_TOP);
	void __iomem *rx = atc_t8122_dp_lane(core, pair, ATC_T8122_RX_SHM);

	atc_t8122_dp_mask(tx + ATC_T8122_TX_CLKMON1, 0,
			  ATC_T8122_CLKMON1_HIZ | ATC_T8122_CLKMON1_HIZ_OV);
	atc_t8122_dp_mask(tx + ATC_T8122_TX_MAIN5,
			  ATC_T8122_MAIN5_CLK_EN | ATC_T8122_MAIN5_CLK_EN_OV,
			  ATC_T8122_MAIN5_CLK_EN_OV);
	atc_t8122_dp_mask(tx + ATC_T8122_TX_MAIN2,
			  ATC_T8122_MAIN2_BYTECLK_SYNC_EN | ATC_T8122_MAIN2_BYTECLK_SYNC_EN_OV,
			  ATC_T8122_MAIN2_BYTECLK_SYNC_EN_OV);
	ndelay(250);
	atc_t8122_dp_mask(tx + ATC_T8122_TX_LDOCLK1, ATC_T8122_LDOCLK1_BYPASS_SMALL, 0);
	ndelay(250);
	atc_t8122_dp_mask(tx + ATC_T8122_TX_LDOCLK1, ATC_T8122_LDOCLK1_BYPASS_BIG, 0);
	ndelay(250);
	atc_t8122_dp_mask(tx + ATC_T8122_TX_LDOCLK1, ATC_T8122_LDOCLK1_EN_BIG, 0);
	ndelay(250);
	atc_t8122_dp_mask(tx + ATC_T8122_TX_LDOCLK1, ATC_T8122_LDOCLK1_EN_SMALL, 0);
	ndelay(250);
	atc_t8122_dp_mask(tx + ATC_T8122_TX_LDOCLK1, ATC_T8122_LDOCLK1_VREF_EN, 0);
	atc_t8122_dp_tx_power(core, pair, false);

	atc_t8122_dp_mask(rx + ATC_T8122_RX_CTLE2,
			  ATC_T8122_CTLE2_TX_CLK_EN | ATC_T8122_CTLE2_TX_CLK_EN_OV,
			  ATC_T8122_CTLE2_TX_CLK_EN_OV);
	ndelay(50);
	atc_t8122_dp_mask(rx + ATC_T8122_RX_TERM19, 0,
			  ATC_T8122_TERM19_TX_DIV2_RST | ATC_T8122_TERM19_TX_DIV2_RST_OV);
	ndelay(500);
	atc_t8122_dp_mask(rx + ATC_T8122_RX_TERM19,
			  ATC_T8122_TERM19_TX_EN | ATC_T8122_TERM19_TX_EN_OV,
			  ATC_T8122_TERM19_TX_EN_OV);
	atc_t8122_dp_mask(rx + ATC_T8122_RX_DFE11,
			  ATC_T8122_DFE11_DTVREG_BIG_EN | ATC_T8122_DFE11_DTVREG_BIG_EN_OV,
			  ATC_T8122_DFE11_DTVREG_BIG_EN_OV);
	ndelay(200);
	atc_t8122_dp_mask(rx + ATC_T8122_RX_DFE11,
			  ATC_T8122_DFE11_DTVREG_SMALL_EN | ATC_T8122_DFE11_DTVREG_SMALL_EN_OV,
			  ATC_T8122_DFE11_DTVREG_SMALL_EN_OV);
	ndelay(200);
	atc_t8122_dp_mask(rx + ATC_T8122_RX_VREF22, ATC_T8122_VREF22_EN, 0);
	ndelay(100);
	atc_t8122_dp_mask(rx + ATC_T8122_RX_DFE12, 0,
			  ATC_T8122_DFE12_TX_BYTECLK_SYNC_CLR | ATC_T8122_DFE12_TX_BYTECLK_SYNC_CLR_OV);
	atc_t8122_dp_mask(top + ATC_T8122_RX_AFE1,
			  ATC_T8122_AFE1_DIV20_RESET_N | ATC_T8122_AFE1_DIV20_RESET_N_OV,
			  ATC_T8122_AFE1_DIV20_RESET_N_OV);
	atc_t8122_dp_mask(rx + ATC_T8122_RX_DFE12, ATC_T8122_DFE12_TX_BYTECLK_SYNC_EN, 0);
	atc_t8122_dp_rxtx_power(core, pair, false);
}

/* Program one block's equalizer to a packed drive preset */
static inline void atc_t8122_dp_lane_drive(void __iomem *core, unsigned int pair, bool tx,
					   u32 preset)
{
	void __iomem *eq1, *eq17, *eq18;

	if (tx) {
		void __iomem *top = atc_t8122_dp_lane(core, pair, ATC_T8122_TX_TOP);

		eq1 = top + ATC_T8122_TX_EQ1;
		eq17 = top + ATC_T8122_TX_EQ17;
		eq18 = top + ATC_T8122_TX_EQ18;
		atc_t8122_dp_mask(eq17, 0, ATC_T8122_EQ17_AUTO_CTRL_DP);
	} else {
		void __iomem *top = atc_t8122_dp_lane(core, pair, ATC_T8122_RX_TOP);

		eq1 = top + ATC_T8122_RX_EQ1;
		eq17 = top + ATC_T8122_RX_EQ17;
		eq18 = top + ATC_T8122_RX_EQ18;
	}
	atc_t8122_dp_mask(eq1, ATC_T8122_EQ1_SWING | ATC_T8122_EQ1_SWING_LSB,
			  atc_t8122_dp_preset_swing(preset));
	atc_t8122_dp_mask(eq18, 0, ATC_T8122_EQ18_SWING_OV);
	atc_t8122_dp_mask(eq17, ATC_T8122_EQ17_DEEMPH, atc_t8122_dp_preset_deemph(preset));
}

#endif
