// SPDX-License-Identifier: GPL-2.0-only
/*
 * T8152 ACIOPHY in USB3 mode, for an ATC port wired to a fixed USB3 device:
 * the J873g front ports' on-board hub, with no Type-C mux in between.
 *
 * Reverse engineered from macOS 26A434: AppleT8152TypeCPhy::initACIO (mode 1:
 * powerup_rst, efuse_load, xbar_config, apply_tunables, deassert reset) and
 * aciophy_pipe_bist_override, AppleT8152USBXHCI::setUSB3Mode for the PIPE
 * handler between the DWC3 and the PHY.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/tunable.h>

#define ACIOPHY_TIMEOUT_US	10000

/* ATCPHY_CFG_BLK */
#define CFG			0x60000
#define CFG_PWRDN_CTRL0		(CFG + 0x00)
#define CTRL0_SLEEP_SML		BIT(0)
#define CTRL0_SLEEP_BIG		BIT(1)
#define CTRL0_CLAMP_EN		BIT(2)
#define CTRL0_AHB_RESET_N	BIT(3)
#define CTRL0_RESET_N		BIT(4)
#define CFG_PWRDN_CTRL1		(CFG + 0x04)
#define CFG_SPARE0		(CFG + 0x08)
#define SPARE0_LN_SWAP		BIT(1)
#define CFG_S2R_AON_SYNC_RST_N	(CFG + 0x30)

/* ACIOPHY_TOP_AON_BLK */
#define AON_CFG_LANE0		0x1e18
#define LANE_CLAMP_EN		BIT(0)
#define LANE_SLEEP_SML		BIT(1)
#define LANE_SLEEP_BIG		BIT(2)
#define LANE_AHB_RESET_N	BIT(5)
#define AON_CORE2MC_SLP_CTRL_2	0x1e24
#define RXA_LFPS_SML_LN0	BIT(12)
#define RXA_LFPS_BIG_LN0	BIT(16)

/* ACIOPHY_TOP_BLK_CFG0_XBAR */
#define XBAR			0x4c
#define XBAR_MODE		GENMASK(16, 0)
#define XBAR_MODE_USB3		0x110

/* ACIOPHY_BIST_BLK */
#define BIST			0x2400
#define BIST_CFG0		(BIST + 0x00)
#define BIST_CLOCK_EN		BIT(0)
#define BIST_USB_REGSYNC	BIT(6)
#define BIST_CFG2		(BIST + 0x0c)
#define BIST_PHY_STATUS_RE	BIT(0)
#define BIST_STATUS1		(BIST + 0xa0)
#define STATUS1_PHY_STATUS	BIT(6)
#define STATUS1_PHY_STATUS_2	BIT(7)
#define BIST_OV1		(BIST + 0xb0)
#define OV1_PIPE_RESET_N_VAL	BIT(0)
#define OV1_PWR_DOWN_VAL	GENMASK(8, 5)
#define BIST_OV2		(BIST + 0xb8)
#define OV2_PIPE_RESET_N	BIT(5)
#define OV2_PWR_DOWN		BIT(9)
#define OV2_MASK		0x660

/* AUSBC_PIPE_HANDLER_BLK */
#define PIPE_OVERRIDE		0x00
#define OVERRIDE_RXVALID	BIT(0)
#define OVERRIDE_RXDETECT	BIT(2)
#define PIPE_OVERRIDE_VALUES	0x04
#define OVERRIDE_VALUES_RXDETECT GENMASK(3, 1)
#define PIPE_MUX		0x0c
#define MUX_MODE		GENMASK(1, 0)
#define MUX_MODE_USB3		0
#define MUX_MODE_DUMMY		2
#define MUX_CLK			GENMASK(6, 4)
#define MUX_CLK_OFF		0
#define MUX_CLK_USB3		1
#define MUX_CLK_DUMMY		4
#define PIPE_LOCK_REQ		0x10
#define PIPE_LOCK_ACK		0x14
#define PIPE_AON_GEN		0x1c
#define AON_GEN_DWC3_RESET_N	BIT(0)
#define PIPE_NONSELECTED	0x20
#define NONSELECTED_POWER_DOWN	GENMASK(3, 0)
#define NONSELECTED_NATIVE_RESET BIT(12)

/* The ATC's PMGR dependencies: USB_AON, the common PSD, DP_0 and DP_1. */
#define PSD_STATE		0x80
#define PSD_TARGET		0x90
static const u32 ps_offset[] = { 0x100, 0, 0x0, 0x20 };
static const u32 ps_idle[] = { 0x300, 6, 0x244, 0x244 };

struct aciophy {
	struct device *dev;
	void __iomem *base;
	void __iomem *pipe;
	void __iomem *ps;
	void __iomem *psd;
	struct apple_tunable *tunable_pre;
	struct apple_tunable *tunable_post;
	/* the dependencies this driver powered up */
	u8 owned;
};

static void aciophy_update(void __iomem *reg, u32 mask, u32 value)
{
	writel((readl(reg) & ~mask) | value, reg);
}

static int aciophy_gate(struct aciophy *phy, unsigned int i, bool on)
{
	u32 value = on ? (ps_idle[i] & 0xfffffcf0) | 15 : ps_idle[i];
	u32 reg;

	if (on && (i == 1 ? !(readl(phy->psd + PSD_STATE) & 7) :
			    (readl(phy->ps + ps_offset[i]) & 0xf0) == 0xf0))
		return 0;
	if (on)
		phy->owned |= BIT(i);
	else
		phy->owned &= ~BIT(i);
	if (i == 1) {
		writel(on ? 0 : ps_idle[i], phy->psd + PSD_TARGET);
		return readl_poll_timeout(phy->psd + PSD_STATE, reg,
					  (reg & 7) == (on ? 0 : ps_idle[i]), 1,
					  ACIOPHY_TIMEOUT_US);
	}
	writel(value, phy->ps + ps_offset[i]);
	return readl_poll_timeout(phy->ps + ps_offset[i], reg,
				  (reg & 0xf0) == ((value & 15) << 4), 1,
				  ACIOPHY_TIMEOUT_US);
}

static void aciophy_gates_off(struct aciophy *phy)
{
	for (int i = ARRAY_SIZE(ps_idle) - 1; i >= 0; i--)
		if ((phy->owned & BIT(i)) && aciophy_gate(phy, i, false))
			dev_warn(phy->dev, "power state %d stuck on\n", i);
}

/*
 * aciophy_lane_pwrup_seq_sub for lane 0. macOS runs it for the USB3+DP mode
 * only; in USB3 mode the lane registers otherwise stay in reset and drop the
 * lane tunables.
 */
static void aciophy_lane_up(struct aciophy *phy)
{
	void __iomem *lane = phy->base + AON_CFG_LANE0;

	aciophy_update(phy->base + AON_CORE2MC_SLP_CTRL_2, 0, RXA_LFPS_SML_LN0);
	udelay(1);
	aciophy_update(phy->base + AON_CORE2MC_SLP_CTRL_2, 0, RXA_LFPS_BIG_LN0);
	udelay(1);
	aciophy_update(lane, 0, LANE_SLEEP_SML);
	udelay(1);
	aciophy_update(lane, 0, LANE_SLEEP_BIG);
	udelay(1);
	aciophy_update(lane, LANE_CLAMP_EN, 0);
	udelay(1);
	aciophy_update(lane, 0, LANE_AHB_RESET_N);
	udelay(1);
}

/* aciophy_mc_pwrdn_seq_sub for lane 0 */
static void aciophy_lane_down(struct aciophy *phy)
{
	void __iomem *lane = phy->base + AON_CFG_LANE0;

	aciophy_update(lane, LANE_AHB_RESET_N, 0);
	udelay(1);
	aciophy_update(lane, 0, LANE_CLAMP_EN);
	udelay(1);
	aciophy_update(lane, LANE_SLEEP_BIG, 0);
	udelay(1);
	aciophy_update(lane, LANE_SLEEP_SML, 0);
	udelay(1);
}

/* aciophy_phy_powerup_rst, efuse_load, partB and deassert reset, mode 1. */
static int aciophy_phy_up(struct aciophy *phy)
{
	void __iomem *cfg = phy->base + CFG_PWRDN_CTRL0;
	u32 reg;

	if (readl(cfg) != CTRL0_CLAMP_EN)
		return -EBUSY;
	aciophy_update(phy->base + CFG_S2R_AON_SYNC_RST_N, 0, BIT(0));
	udelay(1);
	aciophy_update(cfg, 0, CTRL0_SLEEP_SML);
	if (readl_poll_timeout(phy->base + CFG_PWRDN_CTRL1, reg, reg & CTRL0_SLEEP_SML,
			       1, ACIOPHY_TIMEOUT_US))
		return -ETIMEDOUT;
	aciophy_update(cfg, 0, CTRL0_SLEEP_BIG);
	if (readl_poll_timeout(phy->base + CFG_PWRDN_CTRL1, reg, reg & CTRL0_SLEEP_BIG,
			       1, ACIOPHY_TIMEOUT_US))
		return -ETIMEDOUT;
	aciophy_update(cfg, CTRL0_CLAMP_EN, 0);
	udelay(1);
	aciophy_update(cfg, 0, CTRL0_AHB_RESET_N);

	apple_tunable_apply(phy->base, phy->tunable_pre);
	aciophy_lane_up(phy);
	aciophy_update(phy->base + XBAR, XBAR_MODE, XBAR_MODE_USB3);
	apple_tunable_apply(phy->base, phy->tunable_post);
	aciophy_update(phy->base + CFG_SPARE0, SPARE0_LN_SWAP, 0);
	aciophy_update(cfg, 0, CTRL0_RESET_N);
	return 0;
}

static void aciophy_phy_down(struct aciophy *phy)
{
	void __iomem *cfg = phy->base + CFG_PWRDN_CTRL0;
	u32 reg;

	aciophy_update(cfg, CTRL0_RESET_N, 0);
	aciophy_lane_down(phy);
	udelay(1);
	aciophy_update(cfg, CTRL0_AHB_RESET_N, 0);
	aciophy_update(cfg, 0, CTRL0_CLAMP_EN);
	aciophy_update(cfg, CTRL0_SLEEP_BIG, 0);
	if (readl_poll_timeout(phy->base + CFG_PWRDN_CTRL1, reg, !(reg & CTRL0_SLEEP_BIG),
			       1, ACIOPHY_TIMEOUT_US))
		dev_warn(phy->dev, "big-sleep entry timeout\n");
	aciophy_update(cfg, CTRL0_SLEEP_SML, 0);
	if (readl_poll_timeout(phy->base + CFG_PWRDN_CTRL1, reg, !(reg & CTRL0_SLEEP_SML),
			       1, ACIOPHY_TIMEOUT_US))
		dev_warn(phy->dev, "small-sleep entry timeout\n");
	aciophy_update(phy->base + CFG_S2R_AON_SYNC_RST_N, BIT(0), 0);
}

static void aciophy_bist_regsync(struct aciophy *phy)
{
	aciophy_update(phy->base + BIST_CFG0, 0, BIST_USB_REGSYNC);
	readl(phy->base + BIST_CFG0);
	aciophy_update(phy->base + BIST_CFG0, BIST_USB_REGSYNC, 0);
}

static void aciophy_bist_status_ack(struct aciophy *phy)
{
	aciophy_update(phy->base + BIST_CFG2, 0, BIST_PHY_STATUS_RE);
	aciophy_update(phy->base + BIST_CFG2, BIST_PHY_STATUS_RE, 0);
}

/*
 * aciophy_pipe_bist_override: while the PIPE handler switches, hold the
 * MAC's PIPE reset and power-down, stepping power-down from P2 to P3.
 */
static void aciophy_bist_override(struct aciophy *phy, bool enable)
{
	void __iomem *status = phy->base + BIST_STATUS1;
	u32 reg;

	if (!enable) {
		aciophy_update(phy->base + BIST_OV2, OV2_MASK, 0);
		aciophy_bist_regsync(phy);
		aciophy_update(phy->base + BIST_CFG0, BIST_CLOCK_EN, 0);
		return;
	}
	aciophy_update(phy->base + BIST_CFG0, 0, BIST_CLOCK_EN);
	aciophy_update(phy->base + BIST_OV1, OV1_PWR_DOWN_VAL,
		       OV1_PIPE_RESET_N_VAL | FIELD_PREP(OV1_PWR_DOWN_VAL, 2));
	aciophy_update(phy->base + BIST_OV2, OV2_MASK, OV2_PIPE_RESET_N | OV2_PWR_DOWN);
	aciophy_bist_regsync(phy);
	if (readl_poll_timeout(status, reg, !(reg & STATUS1_PHY_STATUS_2), 1, 20000))
		dev_warn(phy->dev, "PIPE P2: PHY status stuck\n");
	aciophy_bist_status_ack(phy);
	aciophy_update(phy->base + BIST_OV1, OV1_PWR_DOWN_VAL, FIELD_PREP(OV1_PWR_DOWN_VAL, 3));
	aciophy_update(phy->base + BIST_OV2, OV2_PWR_DOWN | BIT(10), OV2_PWR_DOWN);
	aciophy_bist_regsync(phy);
	if (readl_poll_timeout(status, reg, reg & STATUS1_PHY_STATUS, 1, 130000))
		dev_warn(phy->dev, "PIPE P3: no PHY status\n");
	if (readl_poll_timeout(status, reg, !(reg & STATUS1_PHY_STATUS_2), 1, 1000))
		dev_warn(phy->dev, "PIPE P3: PHY status stuck\n");
	aciophy_bist_status_ack(phy);
}

static int aciophy_pipe_lock(struct aciophy *phy, bool lock)
{
	u32 reg;

	aciophy_update(phy->pipe + PIPE_LOCK_REQ, BIT(0), lock);
	return readl_poll_timeout(phy->pipe + PIPE_LOCK_ACK, reg, (reg & BIT(0)) == lock,
				  500, 150000);
}

static void aciophy_pipe_mux(struct aciophy *phy, u32 mode, u32 clk)
{
	aciophy_update(phy->pipe + PIPE_MUX, MUX_CLK, FIELD_PREP(MUX_CLK, MUX_CLK_OFF));
	udelay(5);
	aciophy_update(phy->pipe + PIPE_MUX, MUX_MODE, FIELD_PREP(MUX_MODE, mode));
	aciophy_update(phy->pipe + PIPE_MUX, MUX_CLK, FIELD_PREP(MUX_CLK, clk));
}

/* setUSB3Mode: the dummy PHY's PIPE to the ACIOPHY's, or back. */
static int aciophy_pipe_select(struct aciophy *phy, bool usb3)
{
	bool running = readl(phy->pipe + PIPE_AON_GEN) & AON_GEN_DWC3_RESET_N;
	u32 nonsel = usb3 ? 3 : (2 | NONSELECTED_NATIVE_RESET);
	u32 reg;
	int ret;

	if (readl(phy->pipe + PIPE_LOCK_ACK) & BIT(0))
		dev_warn(phy->dev, "PIPE interface already locked\n");
	aciophy_update(phy->pipe + PIPE_OVERRIDE_VALUES, OVERRIDE_VALUES_RXDETECT, 0);
	aciophy_update(phy->pipe + PIPE_OVERRIDE, 0,
		       OVERRIDE_RXVALID | (usb3 ? 0 : OVERRIDE_RXDETECT));
	/* Without the controller up the lock is not acknowledged; harmless. */
	if (running && aciophy_pipe_lock(phy, true))
		dev_dbg(phy->dev, "PIPE lock not acknowledged\n");
	if (usb3)
		aciophy_bist_override(phy, true);
	aciophy_update(phy->pipe + PIPE_NONSELECTED,
		       NONSELECTED_POWER_DOWN | NONSELECTED_NATIVE_RESET, nonsel);
	ret = readl_poll_timeout(phy->pipe + PIPE_NONSELECTED, reg,
				 (reg & (NONSELECTED_POWER_DOWN | NONSELECTED_NATIVE_RESET)) ==
				 nonsel, 5, 1000);
	if (ret)
		dev_err(phy->dev, "PIPE non-selected override: %#x\n", reg);
	if (usb3)
		aciophy_bist_override(phy, false);
	if (usb3)
		aciophy_pipe_mux(phy, MUX_MODE_USB3, MUX_CLK_USB3);
	else
		aciophy_pipe_mux(phy, MUX_MODE_DUMMY, MUX_CLK_DUMMY);
	aciophy_update(phy->pipe + PIPE_OVERRIDE, OVERRIDE_RXVALID | OVERRIDE_RXDETECT, 0);
	if (running && aciophy_pipe_lock(phy, false))
		dev_dbg(phy->dev, "PIPE unlock not acknowledged\n");
	return ret;
}

static int aciophy_power_on(struct phy *gphy)
{
	struct aciophy *phy = phy_get_drvdata(gphy);
	int ret;

	for (unsigned int i = 0; i < ARRAY_SIZE(ps_idle); i++) {
		ret = aciophy_gate(phy, i, true);
		if (ret) {
			dev_err(phy->dev, "power state %u: %d\n", i, ret);
			goto fail;
		}
	}
	ret = aciophy_phy_up(phy);
	if (ret) {
		dev_err(phy->dev, "PHY power-up: %d (cfg %#x/%#x)\n", ret,
			readl(phy->base + CFG_PWRDN_CTRL0), readl(phy->base + CFG_PWRDN_CTRL1));
		goto fail;
	}
	ret = aciophy_pipe_select(phy, true);
	if (ret) {
		aciophy_pipe_select(phy, false);
		aciophy_phy_down(phy);
		goto fail;
	}
	dev_info(phy->dev, "USB3 up\n");
	return 0;
fail:
	aciophy_gates_off(phy);
	return ret;
}

static int aciophy_power_off(struct phy *gphy)
{
	struct aciophy *phy = phy_get_drvdata(gphy);

	aciophy_pipe_select(phy, false);
	aciophy_phy_down(phy);
	aciophy_gates_off(phy);
	return 0;
}

static const struct phy_ops aciophy_ops = {
	.power_on = aciophy_power_on,
	.power_off = aciophy_power_off,
	.owner = THIS_MODULE,
};

/*
 * Not requested: the ATC window also holds the USB controller's DARTs, and
 * the PMGR power states of the four ATCs are interleaved, so the "atc-ps"
 * window covers registers of the other ATCs as well.
 */
static void __iomem *aciophy_map(struct platform_device *pdev, const char *name)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	void __iomem *base;

	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);
	base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	return base ?: IOMEM_ERR_PTR(-ENOMEM);
}

static int aciophy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct resource *atc;
	struct aciophy *phy;
	struct phy *gphy;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;
	phy->dev = dev;
	phy->base = aciophy_map(pdev, "atc");
	if (IS_ERR(phy->base))
		return PTR_ERR(phy->base);
	phy->pipe = devm_platform_ioremap_resource_byname(pdev, "pipehandler");
	if (IS_ERR(phy->pipe))
		return PTR_ERR(phy->pipe);
	phy->ps = aciophy_map(pdev, "atc-ps");
	if (IS_ERR(phy->ps))
		return PTR_ERR(phy->ps);
	phy->psd = aciophy_map(pdev, "atc-psd");
	if (IS_ERR(phy->psd))
		return PTR_ERR(phy->psd);

	/* The tunables are offsets into the ATC block. */
	atc = platform_get_resource_byname(pdev, IORESOURCE_MEM, "atc");
	phy->tunable_pre = devm_apple_tunable_parse(dev, dev->of_node,
						    "apple,tunable-pre", atc);
	if (IS_ERR(phy->tunable_pre))
		return dev_err_probe(dev, PTR_ERR(phy->tunable_pre),
				     "invalid apple,tunable-pre\n");
	phy->tunable_post = devm_apple_tunable_parse(dev, dev->of_node,
						     "apple,tunable-post", atc);
	if (IS_ERR(phy->tunable_post))
		return dev_err_probe(dev, PTR_ERR(phy->tunable_post),
				     "invalid apple,tunable-post\n");

	gphy = devm_phy_create(dev, NULL, &aciophy_ops);
	if (IS_ERR(gphy))
		return PTR_ERR(gphy);
	phy_set_drvdata(gphy, phy);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id aciophy_of_match[] = {
	{ .compatible = "apple,t8152-aciophy-usb3" },
	{}
};
MODULE_DEVICE_TABLE(of, aciophy_of_match);

static struct platform_driver aciophy_driver = {
	.probe = aciophy_probe,
	.driver = {
		.name = "apple-aciophy-usb3",
		.of_match_table = aciophy_of_match,
	},
};
module_platform_driver(aciophy_driver);

MODULE_DESCRIPTION("Apple T8152 ACIOPHY USB3 PHY");
MODULE_LICENSE("GPL");
