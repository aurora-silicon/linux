// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Apple T6040 Type-C PHY -- USB2 and USB3 host
 *
 * This deliberately does not share the T8103/T8122 ATC PHY compatible. T6040
 * has CIO4/TB5-era resources and a different event-bank layout. The
 * implemented operations are the USB2 host sequence and the USB3 host attach
 * (mode 4) decoded from the paired AppleT6040TypeCPhy driver.
 *
 * USB3: the Type-C orientation switch records the plug orientation; power_on
 * of the USB3 PHY wakes the PHY, applies the tunables and the crossbar for
 * that orientation and starts the PHY coprocessor (RTKit), which signals
 * that it has tuned the PHY; set_mode then moves the controller's PIPE from
 * the dummy PHY to this one, which dwc3-apple does after the core init and
 * before xHCI starts. power_off parks the PIPE again, powers the lanes down,
 * stops the coprocessor and resets its RTKit state, and puts the PHY to
 * sleep, as macOS does on every detach: every attach boots the firmware again.
 * Without a plug orientation, or when the coprocessor does not come up, the
 * PIPE stays on the dummy PHY and the port keeps working at USB2.
 *
 * The driver performs no ATC-register writes during probe. The power
 * sequences only cover the PHY: the host controller must be stopped before
 * power_off, which dwc3-apple does on every role change.
 *
 * Runtime PM: the PHY core keeps this device active from power_on to
 * power_off, and the DWC3 reset below holds it while the controller is out of
 * reset (the controller runs on the pipehandler's PIPE clock). In between,
 * the PHY's common_dp power state can drop with an idle port.
 */

#include <dt-bindings/phy/phy.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset-controller.h>
#include <linux/soc/apple/dp-tunnel.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/soc/apple/tunable.h>
#include <linux/usb/typec_mux.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>

#define T6040_USB2_USBCTL		0x00
#define T6040_USB2_USBCTL_MODE		GENMASK(2, 0)
#define T6040_USB2_USBCTL_MODE_HOST	2

#define T6040_USB2_CTL			0x04
#define T6040_USB2_CTL_RESET		BIT(0)
#define T6040_USB2_CTL_PORT_RESET	BIT(1)
#define T6040_USB2_CTL_APB_RESET_N	BIT(2)
#define T6040_USB2_CTL_SIDDQ		BIT(3)

#define T6040_USB2_SIG			0x08
#define T6040_USB2_SIG_VBUSDET_FORCE_VAL	BIT(0)
#define T6040_USB2_SIG_VBUSDET_FORCE_EN	BIT(1)
#define T6040_USB2_SIG_HOST_0		BIT(12)
#define T6040_USB2_SIG_HOST_1		BIT(13)
#define T6040_USB2_SIG_HOST_2		BIT(14)

#define T6040_USB2_MISCTUNE		0x1c
#define T6040_USB2_MISCTUNE_APBCLK_GATE_OFF	BIT(29)
#define T6040_USB2_MISCTUNE_REFCLK_GATE_OFF	BIT(30)

#define T6040_USB2_EVT_CTL		0x00
#define T6040_USB2_EVT_CTL_EN		BIT(0)
#define T6040_USB2_EVT_CTL_LOAD_CNT	BIT(3)
#define T6040_USB2_EVT_STATUS		0x20

/*
 * DWC3 reset and clamp in the pipehandler AON general register: bit 0 is the
 * controller's VCC reset (cleared = reset asserted), bit 4 forces the clamp.
 * Same layout as PIPEHANDLER_AON_GEN on T8103/T8122 (atc.c).
 */
#define T6040_PIPEHANDLER_AON_GEN		0x1c
#define T6040_PIPEHANDLER_AON_GEN_DWC3_RESET_N	BIT(0)
#define T6040_PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN	BIT(4)

/* The controller's PIPE mux: the M1-M3 layout of atc.c. */
#define T6040_PIPEHANDLER_OVERRIDE		0x00
#define T6040_PIPEHANDLER_OVERRIDE_EN		BIT(0)
#define T6040_PIPEHANDLER_OVERRIDE_EN2		BIT(2)
#define T6040_PIPEHANDLER_OVERRIDE_VALUES	0x04
#define T6040_PIPEHANDLER_MUX_CTRL		0x0c
#define T6040_PIPEHANDLER_MUX_CTRL_CLK		GENMASK(5, 3)
#define T6040_PIPEHANDLER_MUX_CTRL_DATA		GENMASK(1, 0)
#define T6040_PIPEHANDLER_MUX_CLK_USB3		1
#define T6040_PIPEHANDLER_MUX_CLK_DUMMY		4
#define T6040_PIPEHANDLER_MUX_DATA_USB3		0
#define T6040_PIPEHANDLER_MUX_DATA_DUMMY	2
#define T6040_PIPEHANDLER_LOCK_REQ		0x10
#define T6040_PIPEHANDLER_LOCK_ACK		0x14
#define T6040_PIPEHANDLER_LOCK_EN		BIT(0)
#define T6040_PIPEHANDLER_NONSELECTED_OVERRIDE	0x20
#define T6040_PIPEHANDLER_NONSELECTED_MODE	GENMASK(3, 0)
#define T6040_PIPEHANDLER_NONSELECTED_BUSY	BIT(12)
#define T6040_PIPEHANDLER_DUMMY_PHY_READY	BIT(15)

/*
 * DisplayPort (PHY mode 3). Offsets are into the "core" and "cfg" windows
 * the USB3 path uses; names follow the paired AppleT6040TypeCPhy driver.
 */
#define T6040_CORE_AUX_CTRL		0x28000	/* ACIOPHY_AUX_TOP_BLK_AUX_CTRL */
#define T6040_CORE_AUX_PWR		0x28400	/* ACIOPHY_AUX_SHM_BLK_AUX_PWR_REG */

#define T6040_CFG_DP_CLKSWITCH_CTRL	0x14	/* ATCPHY_DP_CLKSWITCH_CONTROL_REGS */
#define T6040_CFG_DP_CLKSWITCH_STAT	0x18	/* ATCPHY_DP_CLKSWITCH_STATUS_REGS */

/* PHY core ("core"), atc-phy reg[2] and the blocks behind it */
#define T6040_CORE_CMN_OVRD		0x0c	/* CFG0_TOP */
#define T6040_CORE_CMN_OVRD_SLEEP_B_BIG	BIT(0)
#define T6040_CORE_CMN_OVRD_SLEEP_B_SML	BIT(2)
#define T6040_CORE_CMN_OVRD_CLAMP	BIT(4)
#define T6040_CORE_SLP_CTRL		0x120	/* CFG_SLP_CTRL */
#define T6040_CORE_CMN_BIAS		0xa00	/* AUS40CMN_SHM CMN_BIAS_REG */
#define T6040_CORE_CMN_BIAS_BGBIAS_EN_OV	BIT(1)
#define T6040_CORE_UC_CTRL		0x260	/* ACIOPHY_TOP_BLK_UC_CTRL_REG */
#define T6040_CORE_UC_CTRL_TNBL_DONE	BIT(10)	/* the coprocessor has tuned the PHY */
#define T6040_CORE_XBAR			0x6c
#define T6040_CORE_XBAR_LANES		0x78
#define T6040_CORE_BIST			0x29000	/* ACIOPHY_BIST_BLK, atc-phy reg[35] */
#define T6040_BIST_CFG0			0x004
#define T6040_BIST_CFG1			0x008
#define T6040_BIST_STATUS		0x104
#define T6040_BIST_STATUS_PHY		BIT(6)
#define T6040_BIST_STATUS_PHY_2		BIT(8)
#define T6040_BIST_OVRD0		0x10c
#define T6040_BIST_OVRD1		0x110

/* ATCPHY_CFG ("cfg"), atc-phy reg[39]: power requests and their acks */
#define T6040_CFG_CTRL			0x00
#define T6040_CFG_CTRL_SLEEP_B_SML	BIT(0)
#define T6040_CFG_CTRL_SLEEP_B_BIG	BIT(1)
#define T6040_CFG_CTRL_CLAMP		BIT(2)
#define T6040_CFG_CTRL_APB_RESET_N	BIT(3)
#define T6040_CFG_CTRL_PHY_RESET_N	BIT(4)
#define T6040_CFG_STATUS		0x04
#define T6040_CFG_STATUS_SLEEP_B_SML	BIT(0)
#define T6040_CFG_STATUS_SLEEP_B_BIG	BIT(1)
#define T6040_CFG_STATUS_AHB_CLK	BIT(8)
#define T6040_CFG_SPARE0		0x08
#define T6040_CFG_SPARE0_S2R_FSM_EN	BIT(0)
#define T6040_CFG_SPARE0_USB_LN_SWAP	BIT(1)
#define T6040_CFG_AHB			0x28
#define T6040_CFG_AHB_CLK_REQ_N		BIT(0)

/* MXWrap CPU control ("cpu"), atc-phy reg[38], of the PHY coprocessor */
#define T6040_CPU_RUN			0x28
#define T6040_CPU_RUN_START		0x10	/* AppleMxWrap::_runCPU; STOP stores ~0x10 */
#define T6040_CPU_IDLE_CTRL		0x40	/* MXWRAP_BLK_IDLE_CTRL */
#define T6040_CPU_IDLE_CTRL_EN		BIT(0)
#define T6040_CPU_STATUS		0x44
#define T6040_CPU_STATUS_IDLE		BIT(1)

/* ADT acio-phy-cpuN segment-ranges: the coprocessor sees its __DATA SRAM ("cpu-data") here */
#define T6040_CPU_DATA_IOVA		0x10000000
/* The coprocessor's application endpoint, and the one message macOS sends it */
#define T6040_CPU_EP_APP		32
#define T6040_CPU_MSG_POWER_DOWN	0x0100000000000000ULL

enum t6040_usb3_region {
	T6040_USB3_AXI2AF,
	T6040_USB3_AXI2AF_LIOA,
	T6040_USB3_FABRIC,
	T6040_USB3_COMMON_CFG,
	T6040_USB3_CORE,
	T6040_USB3_CFG,
	T6040_USB3_CPU,
	T6040_USB3_NR_REGIONS,
};

static const char * const t6040_usb3_region_names[T6040_USB3_NR_REGIONS] = {
	[T6040_USB3_AXI2AF] = "axi2af",
	[T6040_USB3_AXI2AF_LIOA] = "axi2af-lioa",
	[T6040_USB3_FABRIC] = "fabric",
	[T6040_USB3_COMMON_CFG] = "common-cfg",
	[T6040_USB3_CORE] = "core",
	[T6040_USB3_CFG] = "cfg",
	[T6040_USB3_CPU] = "cpu",
};

/*
 * The ADT tables AppleT6040TypeCPhy applies at every USB3 power-on (mode 4),
 * in its order; the device tree carries them per port, as calibration.
 */
static const struct {
	const char *prop;
	enum t6040_usb3_region region;
} t6040_usb3_tunable_list[] = {
	{ "apple,tunable-axi2af", T6040_USB3_AXI2AF },
	{ "apple,tunable-fabric", T6040_USB3_FABRIC },
	{ "apple,tunable-common-cfg", T6040_USB3_COMMON_CFG },
	{ "apple,tunable-axi2af-lioa", T6040_USB3_AXI2AF_LIOA },
	{ "apple,tunable-lane0", T6040_USB3_CORE },
	{ "apple,tunable-lane1", T6040_USB3_CORE },
	{ "apple,tunable-aus40cmn-shm", T6040_USB3_CORE },
	{ "apple,tunable-auspll", T6040_USB3_CORE },
	{ "apple,tunable-cio4pll", T6040_USB3_CORE },
};

#define T6040_USB3_TUNABLE_AUSPLL	7
#define T6040_USB3_NR_TUNABLES		ARRAY_SIZE(t6040_usb3_tunable_list)

struct t6040_usb2_phy {
	struct device *dev;
	void __iomem *usb2;
	void __iomem *evt;
	struct phy *phy;
	void __iomem *pipehandler;
	struct reset_controller_dev rcdev;
	struct mutex lock;
	enum phy_mode mode;
	bool powered;
	bool shutdown_failed;
	bool dwc3_running;	/* reset deasserted: holds a runtime PM reference */

	/* USB3; usb3_ok is false when the device tree lacks part of it */
	struct phy *usb3;
	bool usb3_ok;
	void __iomem *regs[T6040_USB3_NR_REGIONS];
	struct resource *res[T6040_USB3_NR_REGIONS];
	struct resource *cpu_data;
	struct apple_tunable *tunables[T6040_USB3_NR_TUNABLES];
	struct apple_tunable *tunable_pipehandler;
	/* ADT tunable_USB2PHY_HOST, applied after the signal setup. */
	struct apple_tunable *tunable_usb2_host;
	struct apple_rtkit *rtk;
	struct completion app_reply;
	enum typec_orientation orientation;
	bool usb3_up;		/* tuned by the coprocessor this power cycle */
	bool pipe_switched;	/* the controller's PIPE runs on this PHY */

	/* DisplayPort (PHY mode 3); dp_ok is false when the DT lacks a DP route */
	struct phy *dp;
	struct typec_mux_dev *mux;
	bool dp_ok;
	bool dp_up;		/* DP mode is programmed on the PHY */
	bool dp_alt;		/* the Type-C port is in DP alt mode, pin assignment C/E */
	bool dp_flip;		/* plug orientation reversed for the DP lanes */
	int dp_rate;		/* link rate index of the running link sequence, -1 = off */
};

static inline void t6040_usb2_mask32(void __iomem *reg, u32 mask, u32 set)
{
	u32 value = readl(reg);

	value &= ~mask;
	value |= set;
	writel(value, reg);
}

static inline void t6040_usb2_set32(void __iomem *reg, u32 set)
{
	t6040_usb2_mask32(reg, 0, set);
}

static inline void t6040_usb2_clear32(void __iomem *reg, u32 clear)
{
	t6040_usb2_mask32(reg, clear, 0);
}

static int t6040_usb2_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);

	if (mode != PHY_MODE_USB_HOST)
		return -EOPNOTSUPP;

	guard(mutex)(&tphy->lock);
	if (tphy->shutdown_failed)
		return -EIO;
	if (tphy->powered && tphy->mode != mode)
		return -EBUSY;

	tphy->mode = mode;
	return 0;
}

/* The five signal bits the init sequence sets, as the host tunable leaves them. */
#define T6040_USB2_SIG_INIT	(T6040_USB2_SIG_HOST_0 | T6040_USB2_SIG_HOST_1 | \
				 T6040_USB2_SIG_HOST_2 | T6040_USB2_SIG_VBUSDET_FORCE_VAL | \
				 T6040_USB2_SIG_VBUSDET_FORCE_EN)

static u32 t6040_usb2_sig_want(struct t6040_usb2_phy *tphy)
{
	struct apple_tunable *t = tphy->tunable_usb2_host;
	u32 want = T6040_USB2_SIG_INIT;
	size_t i;

	for (i = 0; t && i < t->sz; i++)
		if (t->values[i].offset == T6040_USB2_SIG)
			want = (want & ~t->values[i].mask) | t->values[i].value;
	return want & T6040_USB2_SIG_INIT;
}

static int t6040_usb2_power_on(struct phy *phy)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);
	u32 status, ctl, sig, tune, mode, sig_want;

	guard(mutex)(&tphy->lock);
	if (tphy->shutdown_failed)
		return -EIO;

	if (tphy->mode != PHY_MODE_USB_HOST)
		return -EINVAL;
	if (tphy->powered)
		return 0;

	/*
	 * AppleT6040TypeCPhy::eusb2phy_init(false, false), in exact order.
	 * Keep the five signal mutations separate: the paired driver performs
	 * five read/modify/write operations rather than one combined write.
	 */
	t6040_usb2_set32(tphy->usb2 + T6040_USB2_SIG,
			 T6040_USB2_SIG_HOST_2);
	t6040_usb2_set32(tphy->usb2 + T6040_USB2_SIG,
			 T6040_USB2_SIG_HOST_1);
	t6040_usb2_set32(tphy->usb2 + T6040_USB2_SIG,
			 T6040_USB2_SIG_HOST_0);
	t6040_usb2_set32(tphy->usb2 + T6040_USB2_SIG,
			 T6040_USB2_SIG_VBUSDET_FORCE_VAL);
	t6040_usb2_set32(tphy->usb2 + T6040_USB2_SIG,
			 T6040_USB2_SIG_VBUSDET_FORCE_EN);

	/*
	 * The host tunable then sets the signal fields for host mode: on the
	 * J616s it clears SIG[14:12] again (mask 0x7003, value 0x3). Left at
	 * their init values, which are the device-mode ones, the USB2 port
	 * never sees a connect.
	 */
	apple_tunable_apply(tphy->usb2, tphy->tunable_usb2_host);

	msleep(10);

	t6040_usb2_clear32(tphy->usb2 + T6040_USB2_CTL,
			   T6040_USB2_CTL_SIDDQ);
	udelay(10);
	t6040_usb2_clear32(tphy->usb2 + T6040_USB2_CTL,
			   T6040_USB2_CTL_RESET);
	t6040_usb2_clear32(tphy->usb2 + T6040_USB2_CTL,
			   T6040_USB2_CTL_PORT_RESET);

	t6040_usb2_set32(tphy->evt + T6040_USB2_EVT_CTL,
			 T6040_USB2_EVT_CTL_LOAD_CNT |
			 T6040_USB2_EVT_CTL_EN);

	t6040_usb2_set32(tphy->usb2 + T6040_USB2_CTL,
			 T6040_USB2_CTL_APB_RESET_N);
	t6040_usb2_clear32(tphy->usb2 + T6040_USB2_MISCTUNE,
			   T6040_USB2_MISCTUNE_APBCLK_GATE_OFF);
	t6040_usb2_clear32(tphy->usb2 + T6040_USB2_MISCTUNE,
			   T6040_USB2_MISCTUNE_REFCLK_GATE_OFF);

	udelay(30);
	status = readl(tphy->evt + T6040_USB2_EVT_STATUS);
	if (status == U32_MAX)
		goto failed;
	dev_info(tphy->dev, "USB2 event status %#x\n", status);

	msleep(5);
	t6040_usb2_mask32(tphy->usb2 + T6040_USB2_USBCTL,
			  T6040_USB2_USBCTL_MODE,
			  T6040_USB2_USBCTL_MODE_HOST);

	/* Confirm the programmed fields before the core can start the host. */
	sig_want = t6040_usb2_sig_want(tphy);
	ctl = readl(tphy->usb2 + T6040_USB2_CTL);
	sig = readl(tphy->usb2 + T6040_USB2_SIG);
	tune = readl(tphy->usb2 + T6040_USB2_MISCTUNE);
	mode = readl(tphy->usb2 + T6040_USB2_USBCTL);
	status = readl(tphy->evt + T6040_USB2_EVT_CTL);
	if (ctl == U32_MAX || sig == U32_MAX || tune == U32_MAX ||
	    mode == U32_MAX || status == U32_MAX ||
	    (ctl & (T6040_USB2_CTL_RESET | T6040_USB2_CTL_PORT_RESET |
		    T6040_USB2_CTL_SIDDQ | T6040_USB2_CTL_APB_RESET_N)) !=
		   T6040_USB2_CTL_APB_RESET_N ||
	    (sig & T6040_USB2_SIG_INIT) != sig_want ||
	    (tune & (T6040_USB2_MISCTUNE_APBCLK_GATE_OFF |
		     T6040_USB2_MISCTUNE_REFCLK_GATE_OFF)) ||
	    (mode & T6040_USB2_USBCTL_MODE) != T6040_USB2_USBCTL_MODE_HOST ||
	    !(status & T6040_USB2_EVT_CTL_EN))
		goto failed;

	tphy->powered = true;
	dev_info(tphy->dev, "USB2 host sequence complete\n");
	return 0;
failed:
	tphy->shutdown_failed = true;
	return -EIO;
}

/*
 * Power the PHY down: port reset (held 5 ms), USBCTL mode 4, SIDDQ, PHY
 * reset, then gate the APB and reference clocks. The host controller must be
 * halted first; this is not a substitute for an xHCI halt.
 */
static int t6040_usb2_power_off(struct phy *phy)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);
	u32 ctl, mode, tune;

	guard(mutex)(&tphy->lock);
	if (tphy->shutdown_failed)
		return -EIO;
	if (!tphy->powered)
		return 0;
	tphy->shutdown_failed = true;

	if (!(readl(tphy->usb2 + T6040_USB2_CTL) & T6040_USB2_CTL_PORT_RESET)) {
		t6040_usb2_set32(tphy->usb2 + T6040_USB2_CTL,
				 T6040_USB2_CTL_PORT_RESET);
		msleep(5);
	}
	t6040_usb2_mask32(tphy->usb2 + T6040_USB2_USBCTL,
			  T6040_USB2_USBCTL_MODE, 4);
	t6040_usb2_set32(tphy->usb2 + T6040_USB2_CTL,
			 T6040_USB2_CTL_SIDDQ);
	t6040_usb2_set32(tphy->usb2 + T6040_USB2_CTL,
			 T6040_USB2_CTL_RESET);
	t6040_usb2_set32(tphy->usb2 + T6040_USB2_MISCTUNE,
			 T6040_USB2_MISCTUNE_APBCLK_GATE_OFF);
	t6040_usb2_set32(tphy->usb2 + T6040_USB2_MISCTUNE,
			 T6040_USB2_MISCTUNE_REFCLK_GATE_OFF);
	/* The vendor driver waits 500 ns after gating the clocks. */
	ndelay(500);

	/*
	 * Do not report the PHY off, or allow another power cycle, after a
	 * write that did not land.
	 */
	ctl = readl(tphy->usb2 + T6040_USB2_CTL);
	mode = readl(tphy->usb2 + T6040_USB2_USBCTL);
	tune = readl(tphy->usb2 + T6040_USB2_MISCTUNE);
	if (ctl == U32_MAX || mode == U32_MAX || tune == U32_MAX ||
	    (ctl & (T6040_USB2_CTL_PORT_RESET | T6040_USB2_CTL_SIDDQ |
		    T6040_USB2_CTL_RESET)) !=
		   (T6040_USB2_CTL_PORT_RESET | T6040_USB2_CTL_SIDDQ |
		    T6040_USB2_CTL_RESET) ||
	    (mode & T6040_USB2_USBCTL_MODE) != 4 ||
	    (tune & (T6040_USB2_MISCTUNE_APBCLK_GATE_OFF |
		     T6040_USB2_MISCTUNE_REFCLK_GATE_OFF)) !=
		    (T6040_USB2_MISCTUNE_APBCLK_GATE_OFF |
		     T6040_USB2_MISCTUNE_REFCLK_GATE_OFF))
		return -EIO;
	tphy->shutdown_failed = false;
	tphy->powered = false;
	return 0;
}

static const struct phy_ops t6040_usb2_phy_ops = {
	.owner = THIS_MODULE,
	.set_mode = t6040_usb2_set_mode,
	.power_on = t6040_usb2_power_on,
	.power_off = t6040_usb2_power_off,
};

/* ---- USB3 ---- */

#define t6040_core(tphy)	((tphy)->regs[T6040_USB3_CORE])
#define t6040_cfg(tphy)		((tphy)->regs[T6040_USB3_CFG])
#define t6040_cpu(tphy)		((tphy)->regs[T6040_USB3_CPU])
#define t6040_bist(tphy)	(t6040_core(tphy) + T6040_CORE_BIST)

static int t6040_usb3_cfg_ack(struct t6040_usb2_phy *tphy, const char *what,
			      u32 bit, bool set)
{
	u32 status;
	int ret;

	ret = readl_poll_timeout(t6040_cfg(tphy) + T6040_CFG_STATUS, status,
				 !!(status & bit) == set, 1, 1000);
	if (ret)
		dev_err(tphy->dev, "USB3: no %s acknowledge (status %#x)\n", what, status);
	udelay(1);
	return ret;
}

/* A: powerup_rst, every acknowledge within 1 ms. */
static int t6040_usb3_wake(struct t6040_usb2_phy *tphy)
{
	void __iomem *cfg = t6040_cfg(tphy);

	t6040_usb2_set32(cfg + T6040_CFG_CTRL, T6040_CFG_CTRL_SLEEP_B_SML);
	udelay(1);
	if (t6040_usb3_cfg_ack(tphy, "SLEEP_B_SML", T6040_CFG_STATUS_SLEEP_B_SML, true))
		return -ETIMEDOUT;
	t6040_usb2_set32(cfg + T6040_CFG_CTRL, T6040_CFG_CTRL_SLEEP_B_BIG);
	udelay(1);
	if (t6040_usb3_cfg_ack(tphy, "SLEEP_B_BIG", T6040_CFG_STATUS_SLEEP_B_BIG, true))
		return -ETIMEDOUT;
	t6040_usb2_clear32(cfg + T6040_CFG_CTRL, T6040_CFG_CTRL_CLAMP);
	udelay(8);
	t6040_usb2_clear32(cfg + T6040_CFG_AHB, T6040_CFG_AHB_CLK_REQ_N);
	if (t6040_usb3_cfg_ack(tphy, "AHB clock", T6040_CFG_STATUS_AHB_CLK, false))
		return -ETIMEDOUT;
	t6040_usb2_set32(cfg + T6040_CFG_CTRL, T6040_CFG_CTRL_APB_RESET_N);
	return 0;
}

/*
 * B1: the tables, then apply_dptx_tunables' fixed writes (atc-phy reg[] offsets
 * relative to the core) and AUSPLL_CORE once more.
 */
static void t6040_usb3_tunables(struct t6040_usb2_phy *tphy)
{
	static const struct { u32 off, clear, set; } dptx[] = {
		{ 0x00800, 0x000003f0, 0x000001c0 },	/* AUS40CMN_DIG RCAL0 */
		{ 0x00818, 0x0000f000, 0x00003000 },	/* AUS40CMN_DIG RCAL5 */
		{ 0x03888, 0x000000ff, 0x00000027 },	/* CIO4PLL FREQDESCRIPTOR_0_C */
		{ 0x28004, 0x00000038, 0x00000008 },	/* AUX_LDO_CTRL */
		{ 0x03adc, 0x00003ff0, 0x00001760 },	/* CIO4PLL CFG_FRACN_FUNC */
		{ 0x0e0cc, 0x01f80000, 0x00380000 },	/* LANE0 TX_EQ_CTRL_REG11 */
		{ 0x1e0cc, 0x01f80000, 0x00380000 },	/* LANE1 TX_EQ_CTRL_REG11 */
		{ 0x0d090, 0x01f80000, 0x00380000 },	/* LANE0 RX_CFG TX_EQ_CTRL_REG11 */
		{ 0x1d090, 0x01f80000, 0x00380000 },	/* LANE1 RX_CFG TX_EQ_CTRL_REG11 */
	};
	size_t i;

	for (i = 0; i < T6040_USB3_NR_TUNABLES; i++)
		apple_tunable_apply(tphy->regs[t6040_usb3_tunable_list[i].region],
				    tphy->tunables[i]);
	for (i = 0; i < ARRAY_SIZE(dptx); i++)
		t6040_usb2_mask32(t6040_core(tphy) + dptx[i].off, dptx[i].clear, dptx[i].set);
	apple_tunable_apply(t6040_core(tphy), tphy->tunables[T6040_USB3_TUNABLE_AUSPLL]);
}

/* B2: the crossbar for USB3 on the plug's lane; B3: release that lane's overrides. */
static void t6040_usb3_lanes(struct t6040_usb2_phy *tphy, bool swap)
{
	static const struct { u32 off, bit; } lane_ovrd[] = {
		{ 0x10, 0x4 }, { 0x10, 0x40 }, { 0x0c, 0x10000 }, { 0x0c, 0x100 },
		{ 0x0c, 0x1000 }, { 0x120, 0x40 }, { 0x120, 0x4 }, { 0x120, 0x400 },
	};
	void __iomem *core = t6040_core(tphy);
	size_t i;

	if (swap)
		t6040_usb2_mask32(core + T6040_CORE_XBAR, ~0xfebfff03u, 0x28);
	else
		t6040_usb2_mask32(core + T6040_CORE_XBAR, ~0xfd7fff03u, 0x44);
	t6040_usb2_mask32(core + T6040_CORE_XBAR_LANES, 0x1ffff, swap ? 0x111 : 0x110);
	t6040_usb2_mask32(core + T6040_CORE_XBAR, 0x3, 0x2);

	/* cmn_rm_ovrd, then txrx_rm_ovrd of the lane */
	t6040_usb2_clear32(core + T6040_CORE_CMN_OVRD, 0x2a);
	udelay(1);
	for (i = 0; i < ARRAY_SIZE(lane_ovrd); i++) {
		t6040_usb2_clear32(core + lane_ovrd[i].off, lane_ovrd[i].bit << swap);
		udelay(1);
	}
}

static int t6040_usb3_wait_tuned(struct t6040_usb2_phy *tphy)
{
	u32 uc;

	return readl_poll_timeout(t6040_core(tphy) + T6040_CORE_UC_CTRL, uc,
				  uc & T6040_CORE_UC_CTRL_TNBL_DONE, 1000, USEC_PER_SEC);
}

/*
 * Run the coprocessor and boot RTKit. Every detach stops it again
 * (t6040_usb3_firmware_power_down), so every attach is a fresh firmware boot
 * with its own HELLO. A coprocessor that runs without an RTKit session (it
 * was never stopped) does not boot again and cannot be used.
 */
static int t6040_usb3_cpu_start(struct t6040_usb2_phy *tphy)
{
	void __iomem *cpu = t6040_cpu(tphy);

	if (readl(cpu + T6040_CPU_RUN) & T6040_CPU_RUN_START) {
		dev_err(tphy->dev, "USB3: the PHY coprocessor runs without an RTKit session\n");
		return -EBUSY;
	}
	writel(T6040_CPU_RUN_START, cpu + T6040_CPU_RUN);
	usleep_range(5000, 6000);
	return apple_rtkit_boot(tphy->rtk);
}

/*
 * C: runFirmware. macOS starts the coprocessor on every attach and stops it on
 * every detach; each start makes the firmware tune the PHY. Then release the
 * PHY reset (D).
 */
static int t6040_usb3_run_firmware(struct t6040_usb2_phy *tphy, bool swap)
{
	void __iomem *cpu = t6040_cpu(tphy);
	u32 uc_before;
	int ret;

	t6040_usb2_mask32(t6040_cfg(tphy) + T6040_CFG_SPARE0, T6040_CFG_SPARE0_USB_LN_SWAP,
			  swap ? T6040_CFG_SPARE0_USB_LN_SWAP : 0);
	fsleep(20);
	t6040_usb2_set32(cpu + T6040_CPU_IDLE_CTRL, T6040_CPU_IDLE_CTRL_EN);
	fsleep(10);

	uc_before = readl(t6040_core(tphy) + T6040_CORE_UC_CTRL);
	if (!apple_rtkit_is_running(tphy->rtk)) {
		ret = t6040_usb3_cpu_start(tphy);
		if (ret) {
			dev_err(tphy->dev, "USB3: the PHY coprocessor was not started: %d (run %#x, idle %#x, status %#x)\n",
				ret, readl(cpu + T6040_CPU_RUN),
				readl(cpu + T6040_CPU_IDLE_CTRL), readl(cpu + T6040_CPU_STATUS));
			return ret;
		}
	}
	ret = t6040_usb3_wait_tuned(tphy);
	if (ret) {
		dev_err(tphy->dev, "USB3: the PHY coprocessor did not tune the PHY (uc %#x -> %#x, idle %#x, status %#x)\n",
			uc_before, readl(t6040_core(tphy) + T6040_CORE_UC_CTRL),
			readl(cpu + T6040_CPU_IDLE_CTRL), readl(cpu + T6040_CPU_STATUS));
		return ret;
	}

	dev_info(tphy->dev, "USB3 PHY tuned: lane %d, coprocessor started, uc %#x -> %#x\n", swap,
		 uc_before, readl(t6040_core(tphy) + T6040_CORE_UC_CTRL));
	t6040_usb2_set32(t6040_cfg(tphy) + T6040_CFG_CTRL, T6040_CFG_CTRL_PHY_RESET_N);
	fsleep(100);
	return 0;
}

/*
 * Detach, RTBuddy::setPowerState(0): Power Down, RTKit AP then IOP to idle,
 * wait up to 10 ms for the CPU to idle, STOP. The RTKit state is reset so the
 * next attach boots the firmware as the first one did. The mailbox outbox
 * enable that macOS clears here is left as iBoot set it: Linux never clears it.
 */
static void t6040_usb3_firmware_power_down(struct t6040_usb2_phy *tphy)
{
	void __iomem *cpu = t6040_cpu(tphy);
	u32 status;
	int ret;

	if (apple_rtkit_is_running(tphy->rtk)) {
		reinit_completion(&tphy->app_reply);
		ret = apple_rtkit_start_ep(tphy->rtk, T6040_CPU_EP_APP);
		if (!ret)
			ret = apple_rtkit_send_message(tphy->rtk, T6040_CPU_EP_APP,
						       T6040_CPU_MSG_POWER_DOWN, NULL, false);
		if (!ret && !wait_for_completion_timeout(&tphy->app_reply, msecs_to_jiffies(500)))
			ret = -ETIMEDOUT;
		if (ret)
			dev_warn(tphy->dev, "USB3: no Power Down acknowledge: %d\n", ret);

		ret = apple_rtkit_idle(tphy->rtk);
		if (ret)
			dev_warn(tphy->dev, "USB3: PHY coprocessor idle failed: %d\n", ret);
	}

	if (!(readl(cpu + T6040_CPU_RUN) & T6040_CPU_RUN_START))
		return;
	if (readl_poll_timeout(cpu + T6040_CPU_STATUS, status, status & T6040_CPU_STATUS_IDLE,
			       1000, 10000))
		dev_warn(tphy->dev, "USB3: the PHY coprocessor did not idle (status %#x)\n",
			 status);
	writel(~T6040_CPU_RUN_START, cpu + T6040_CPU_RUN);

	ret = apple_rtkit_reinit(tphy->rtk);
	if (ret)
		dev_warn(tphy->dev, "USB3: RTKit reset failed: %d\n", ret);
}

/*
 * aciophy_phy_shutdown_partB: the CFG block back to its boot state (ctrl 0x4:
 * clamped, asleep, in reset; AHB clock not requested), so the next attach
 * runs A from the start.
 */
static void t6040_usb3_sleep(struct t6040_usb2_phy *tphy)
{
	void __iomem *cfg = t6040_cfg(tphy);

	udelay(5);
	t6040_usb2_set32(cfg + T6040_CFG_AHB, T6040_CFG_AHB_CLK_REQ_N);
	t6040_usb3_cfg_ack(tphy, "AHB clock release", T6040_CFG_STATUS_AHB_CLK, true);
	t6040_usb2_clear32(cfg + T6040_CFG_CTRL, T6040_CFG_CTRL_PHY_RESET_N);
	udelay(1);
	t6040_usb2_clear32(cfg + T6040_CFG_CTRL, T6040_CFG_CTRL_APB_RESET_N);
	udelay(1);
	t6040_usb2_set32(cfg + T6040_CFG_CTRL, T6040_CFG_CTRL_CLAMP);
	udelay(1);
	t6040_usb2_clear32(cfg + T6040_CFG_CTRL, T6040_CFG_CTRL_SLEEP_B_BIG);
	udelay(1);
	t6040_usb3_cfg_ack(tphy, "SLEEP_B_BIG release", T6040_CFG_STATUS_SLEEP_B_BIG, false);
	t6040_usb2_clear32(cfg + T6040_CFG_CTRL, T6040_CFG_CTRL_SLEEP_B_SML);
	udelay(1);
	t6040_usb3_cfg_ack(tphy, "SLEEP_B_SML release", T6040_CFG_STATUS_SLEEP_B_SML, false);
	t6040_usb2_clear32(cfg + T6040_CFG_SPARE0, T6040_CFG_SPARE0_S2R_FSM_EN);
	udelay(1);
}

/* Sets PHY_STATUS(_2) handshake bits in the BIST block and waits as macOS does, without failing. */
static void t6040_usb3_bist_pulse(struct t6040_usb2_phy *tphy, u32 reg, u32 bit)
{
	t6040_usb2_set32(t6040_bist(tphy) + reg, bit);
	t6040_usb2_clear32(t6040_bist(tphy) + reg, bit);
}

static void t6040_usb3_bist_wait(struct t6040_usb2_phy *tphy, u32 bit, bool set,
				 unsigned int ms)
{
	u32 status;

	if (readl_poll_timeout(t6040_bist(tphy) + T6040_BIST_STATUS, status,
			       !!(status & bit) == set, 1000, ms * 1000))
		dev_dbg(tphy->dev, "USB3: BIST status %#x\n", status);
}

static void t6040_usb3_bist_on(struct t6040_usb2_phy *tphy)
{
	void __iomem *bist = t6040_bist(tphy);

	t6040_usb2_set32(bist + T6040_BIST_CFG0, BIT(0));
	t6040_usb2_mask32(bist + T6040_BIST_OVRD0, ~0xe1ffffffu, 0x4100000);
	t6040_usb2_mask32(bist + T6040_BIST_OVRD1, ~0xfffff99fu, 0x220);
	t6040_usb3_bist_pulse(tphy, T6040_BIST_CFG0, BIT(3));
	t6040_usb3_bist_wait(tphy, T6040_BIST_STATUS_PHY_2, false, 20);
	t6040_usb3_bist_pulse(tphy, T6040_BIST_CFG1, BIT(5));
	t6040_usb2_mask32(bist + T6040_BIST_OVRD0, ~0xe1ffffffu, 0x6000000);
	t6040_usb2_mask32(bist + T6040_BIST_OVRD1, ~0xfffff9ffu, 0x200);
	t6040_usb3_bist_pulse(tphy, T6040_BIST_CFG0, BIT(3));
	t6040_usb3_bist_wait(tphy, T6040_BIST_STATUS_PHY, true, 130);
	t6040_usb3_bist_wait(tphy, T6040_BIST_STATUS_PHY_2, false, 1);
	t6040_usb3_bist_pulse(tphy, T6040_BIST_CFG1, BIT(5));
}

static void t6040_usb3_bist_off(struct t6040_usb2_phy *tphy)
{
	t6040_usb2_clear32(t6040_bist(tphy) + T6040_BIST_OVRD1, 0x660);
	t6040_usb3_bist_pulse(tphy, T6040_BIST_CFG0, BIT(3));
	t6040_usb2_clear32(t6040_bist(tphy) + T6040_BIST_CFG0, BIT(0));
}

/*
 * aciophy_phy_shutdown_partA: the PHY's USB link side down (the same BIST
 * handshake as the PIPE switch), both TX lanes, then the common block.
 */
static void t6040_usb3_shutdown_lanes(struct t6040_usb2_phy *tphy)
{
	void __iomem *bist = t6040_bist(tphy);
	void __iomem *core = t6040_core(tphy);
	unsigned int lane;

	t6040_usb2_set32(bist + T6040_BIST_CFG0, BIT(0));
	t6040_usb2_clear32(bist + T6040_BIST_OVRD0, 0x100000);
	t6040_usb2_mask32(bist + T6040_BIST_OVRD1, 0x60, 0x20);
	t6040_usb3_bist_pulse(tphy, T6040_BIST_CFG0, BIT(3));
	t6040_usb3_bist_wait(tphy, T6040_BIST_STATUS_PHY_2, true, 1000);
	t6040_usb2_mask32(bist + T6040_BIST_OVRD0, ~0xe1ffffffu, 0x4000000);
	t6040_usb2_mask32(bist + T6040_BIST_OVRD1, 0x600, 0x200);
	t6040_usb3_bist_pulse(tphy, T6040_BIST_CFG0, BIT(3));
	t6040_usb3_bist_wait(tphy, T6040_BIST_STATUS_PHY_2, true, 1000);
	t6040_usb2_clear32(bist + T6040_BIST_CFG0, BIT(0));

	/* aciophy_tx_powerdown(0), (1) */
	for (lane = 0; lane < 2; lane++) {
		t6040_usb2_set32(core + T6040_CORE_SLP_CTRL, 0x100 << lane);
		udelay(1);
		t6040_usb2_clear32(core + T6040_CORE_SLP_CTRL, 0x1 << lane);
		udelay(1);
		t6040_usb2_clear32(core + T6040_CORE_SLP_CTRL, 0x10 << lane);
		udelay(1);
	}

	/* aciophy_cmn_shutdown */
	t6040_usb2_clear32(core + T6040_CORE_CMN_BIAS, T6040_CORE_CMN_BIAS_BGBIAS_EN_OV);
	udelay(1);
	t6040_usb2_set32(core + T6040_CORE_CMN_OVRD, T6040_CORE_CMN_OVRD_CLAMP);
	udelay(1);
	t6040_usb2_clear32(core + T6040_CORE_CMN_OVRD, T6040_CORE_CMN_OVRD_SLEEP_B_BIG);
	udelay(1);
	t6040_usb2_clear32(core + T6040_CORE_CMN_OVRD, T6040_CORE_CMN_OVRD_SLEEP_B_SML);
	udelay(1);
	fsleep(200);
}

static int t6040_usb3_pipe_lock(struct t6040_usb2_phy *tphy, unsigned int ms)
{
	u32 ack;

	t6040_usb2_set32(tphy->pipehandler + T6040_PIPEHANDLER_LOCK_REQ,
			 T6040_PIPEHANDLER_LOCK_EN);
	return readl_poll_timeout(tphy->pipehandler + T6040_PIPEHANDLER_LOCK_ACK, ack,
				  ack & T6040_PIPEHANDLER_LOCK_EN, 500, ms * 1000);
}

static void t6040_usb3_pipe_unlock(struct t6040_usb2_phy *tphy)
{
	u32 ack;

	t6040_usb2_clear32(tphy->pipehandler + T6040_PIPEHANDLER_LOCK_REQ,
			   T6040_PIPEHANDLER_LOCK_EN);
	if (readl_poll_timeout(tphy->pipehandler + T6040_PIPEHANDLER_LOCK_ACK, ack,
			       !(ack & T6040_PIPEHANDLER_LOCK_EN), 500, 6000))
		dev_dbg(tphy->dev, "USB3: PIPE lock acknowledge did not clear\n");
}

static void t6040_usb3_pipe_mux(struct t6040_usb2_phy *tphy, u32 clk, u32 data)
{
	void __iomem *mux = tphy->pipehandler + T6040_PIPEHANDLER_MUX_CTRL;

	t6040_usb2_clear32(mux, T6040_PIPEHANDLER_MUX_CTRL_CLK);
	udelay(5);
	t6040_usb2_mask32(mux, T6040_PIPEHANDLER_MUX_CTRL_DATA,
			  FIELD_PREP(T6040_PIPEHANDLER_MUX_CTRL_DATA, data));
	t6040_usb2_mask32(mux, T6040_PIPEHANDLER_MUX_CTRL_CLK,
			  FIELD_PREP(T6040_PIPEHANDLER_MUX_CTRL_CLK, clk));
}

/*
 * E: move the PIPE from the dummy PHY to this one, inside macOS's BIST
 * bracket. The lock is never acknowledged on T6040 (the dummy PHY clocks the
 * handshake, and so far it has not answered); the switch works without it.
 */
static void t6040_usb3_pipe_switch(struct t6040_usb2_phy *tphy)
{
	void __iomem *ph = tphy->pipehandler;
	u32 val;

	/* macOS's controller reset() sets DUMMY_PHY_READY once; Linux's dwc3 does not. */
	if (!(readl(ph + T6040_PIPEHANDLER_NONSELECTED_OVERRIDE) &
	      T6040_PIPEHANDLER_DUMMY_PHY_READY)) {
		t6040_usb2_set32(ph + T6040_PIPEHANDLER_NONSELECTED_OVERRIDE,
				 T6040_PIPEHANDLER_DUMMY_PHY_READY);
		usleep_range(1000, 1500);
	}
	t6040_usb2_clear32(ph + T6040_PIPEHANDLER_OVERRIDE_VALUES, 0xe);
	t6040_usb2_set32(ph + T6040_PIPEHANDLER_OVERRIDE, T6040_PIPEHANDLER_OVERRIDE_EN);
	if (t6040_usb3_pipe_lock(tphy, 20))
		dev_dbg(tphy->dev, "USB3: PIPE lock not acknowledged, switching anyway\n");

	t6040_usb3_bist_on(tphy);
	t6040_usb2_mask32(ph + T6040_PIPEHANDLER_NONSELECTED_OVERRIDE, ~0xffffeff0u, 0x3);
	if (readl_poll_timeout(ph + T6040_PIPEHANDLER_NONSELECTED_OVERRIDE, val,
			       (val & T6040_PIPEHANDLER_NONSELECTED_MODE) == 3 &&
			       !(val & T6040_PIPEHANDLER_NONSELECTED_BUSY), 5, 1000))
		dev_dbg(tphy->dev, "USB3: non-selected override %#x\n", val);
	t6040_usb3_bist_off(tphy);

	t6040_usb3_pipe_mux(tphy, T6040_PIPEHANDLER_MUX_CLK_USB3,
			    T6040_PIPEHANDLER_MUX_DATA_USB3);
	t6040_usb2_clear32(ph + T6040_PIPEHANDLER_OVERRIDE, T6040_PIPEHANDLER_OVERRIDE_EN);
	t6040_usb2_clear32(ph + T6040_PIPEHANDLER_OVERRIDE, T6040_PIPEHANDLER_OVERRIDE_EN2);
	t6040_usb3_pipe_unlock(tphy);
}

/* macOS setUSB3Mode(0) from 2: the PIPE back on the dummy PHY. */
static void t6040_usb3_pipe_park(struct t6040_usb2_phy *tphy)
{
	void __iomem *ph = tphy->pipehandler;
	u32 val;

	t6040_usb2_clear32(ph + T6040_PIPEHANDLER_OVERRIDE_VALUES, 0xe);
	t6040_usb2_set32(ph + T6040_PIPEHANDLER_OVERRIDE, T6040_PIPEHANDLER_OVERRIDE_EN);
	t6040_usb2_set32(ph + T6040_PIPEHANDLER_OVERRIDE, T6040_PIPEHANDLER_OVERRIDE_EN2);
	if (t6040_usb3_pipe_lock(tphy, 150))
		dev_dbg(tphy->dev, "USB3: PIPE lock not acknowledged, parking anyway\n");
	t6040_usb2_mask32(ph + T6040_PIPEHANDLER_NONSELECTED_OVERRIDE,
			  T6040_PIPEHANDLER_NONSELECTED_MODE, 0x1002);
	if (readl_poll_timeout(ph + T6040_PIPEHANDLER_NONSELECTED_OVERRIDE, val,
			       (val & T6040_PIPEHANDLER_NONSELECTED_MODE) == 2 &&
			       (val & T6040_PIPEHANDLER_NONSELECTED_BUSY), 5, 1000))
		dev_dbg(tphy->dev, "USB3: non-selected override %#x\n", val);
	t6040_usb3_pipe_mux(tphy, T6040_PIPEHANDLER_MUX_CLK_DUMMY,
			    T6040_PIPEHANDLER_MUX_DATA_DUMMY);
	t6040_usb2_clear32(ph + T6040_PIPEHANDLER_OVERRIDE, T6040_PIPEHANDLER_OVERRIDE_EN);
	t6040_usb2_clear32(ph + T6040_PIPEHANDLER_OVERRIDE, T6040_PIPEHANDLER_OVERRIDE_EN2);
	t6040_usb3_pipe_unlock(tphy);
}

/*
 * dwc3 powers this on in its core init, with the controller out of reset and
 * its PIPE on the dummy PHY. A failure here leaves the port at USB2, so it is
 * logged and not returned: the USB2 PHY and the core must still come up.
 */
static int t6040_usb3_power_on(struct phy *phy)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);
	bool swap;
	int ret;

	guard(mutex)(&tphy->lock);
	if (!tphy->usb3_ok || tphy->usb3_up)
		return 0;

	switch (tphy->orientation) {
	case TYPEC_ORIENTATION_NORMAL:
		swap = false;
		break;
	case TYPEC_ORIENTATION_REVERSE:
		swap = true;
		break;
	default:
		dev_info(tphy->dev, "USB3: no plug orientation, USB2 only\n");
		return 0;
	}

	/* PIPE_HANDLER_DEFAULT: the dummy PIPE clock the controller keeps using at USB2 */
	apple_tunable_apply(tphy->pipehandler, tphy->tunable_pipehandler);

	ret = t6040_usb3_wake(tphy);
	if (ret)
		goto sleep;	/* the coprocessor block may be unclocked: never touch it */
	t6040_usb3_tunables(tphy);
	t6040_usb3_lanes(tphy, swap);
	ret = t6040_usb3_run_firmware(tphy, swap);
	if (ret)
		goto usb2_only;

	tphy->usb3_up = true;
	dev_dbg(tphy->dev, "USB3 PHY up, lane %d\n", swap);
	return 0;

usb2_only:
	/* power_off skips a PHY that never came up: undo here, so the next attach starts clean */
	t6040_usb3_firmware_power_down(tphy);
sleep:
	t6040_usb3_sleep(tphy);
	dev_warn(tphy->dev, "USB3 PHY bring-up failed (%d), USB2 only\n", ret);
	return 0;
}

/* aciophy_phy_shutdown with the lock held: lanes, coprocessor, then the CFG block. */
static void t6040_usb3_down_locked(struct t6040_usb2_phy *tphy)
{
	if (tphy->pipe_switched) {
		t6040_usb3_pipe_park(tphy);
		tphy->pipe_switched = false;
	}
	if (tphy->usb3_up) {
		u32 uc = readl(t6040_core(tphy) + T6040_CORE_UC_CTRL);

		t6040_usb3_shutdown_lanes(tphy);
		t6040_usb3_firmware_power_down(tphy);
		t6040_usb3_sleep(tphy);
		dev_info(tphy->dev, "USB3 PHY asleep: uc %#x -> %#x, cfg %#x/%#x\n", uc,
			 readl(t6040_core(tphy) + T6040_CORE_UC_CTRL),
			 readl(t6040_cfg(tphy) + T6040_CFG_CTRL),
			 readl(t6040_cfg(tphy) + T6040_CFG_STATUS));
		tphy->usb3_up = false;
	}
}

static int t6040_usb3_power_off(struct phy *phy)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);

	guard(mutex)(&tphy->lock);
	t6040_usb3_down_locked(tphy);
	return 0;
}

/*
 * Host only. dwc3-apple calls this after the core init and before xHCI
 * starts; the xHCI root hub calls it again as it starts, also for a port
 * that stays at USB2, which must not fail.
 */
static int t6040_usb3_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);

	if (mode != PHY_MODE_USB_HOST && mode != PHY_MODE_USB_HOST_SS)
		return -EOPNOTSUPP;

	guard(mutex)(&tphy->lock);
	if (!tphy->usb3_up || tphy->pipe_switched)
		return 0;
	t6040_usb3_pipe_switch(tphy);
	tphy->pipe_switched = true;
	dev_info(tphy->dev, "USB3 PIPE on the PHY\n");
	return 0;
}

static const struct phy_ops t6040_usb3_phy_ops = {
	.owner = THIS_MODULE,
	.set_mode = t6040_usb3_set_mode,
	.power_on = t6040_usb3_power_on,
	.power_off = t6040_usb3_power_off,
};

/* tipd sets the orientation before the USB role, and NONE after the role is dropped. */
static int t6040_usb3_sw_set(struct typec_switch_dev *sw, enum typec_orientation orientation)
{
	struct t6040_usb2_phy *tphy = typec_switch_get_drvdata(sw);

	guard(mutex)(&tphy->lock);
	tphy->orientation = orientation;
	return 0;
}

static void t6040_usb3_switch_unregister(void *sw)
{
	typec_switch_unregister(sw);
}

/* ---- DisplayPort (PHY mode 3: DP on all four lanes) ----
 *
 * The fixed DP PHY of an HDMI converter (no USB controller, no pipehandler)
 * and a Type-C PHY whose port the Type-C controller has put into DP alt mode
 * with pin assignment C or E are brought up the same way, as the paired
 * AppleT6040TypeCPhy driver does for its mode 3: wake the CFG block, apply the
 * generic ADT tunables and the DPTX tunables, set the lane crossbar to DP on
 * all lanes, hold the common block and both TX lanes on (the PHY coprocessor
 * does not run in this mode) and release the PHY reset. The DCP then sets the
 * link rate through phy_configure(): the per-rate link sequence is the T6050
 * fixed-DP table (atc-t6050-dp-data.h; the T6050 ADT names its PHY
 * "atc-phy,t6050", "atc-phy,t6040"), followed by the T6040 DP clock switch in
 * the "cfg" window. Pin assignment D (two DP lanes next to USB3) is not
 * supported.
 */

/* The fixed DPTX tunable writes (apply_dptx_tunables, shared with USB3). */
static void t6040_dp_fixed_tunables(void __iomem *core)
{
	static const struct { u32 off, clear, set; } dptx[] = {
		{ 0x00800, 0x000003f0, 0x000001c0 },	/* AUS40CMN_DIG RCAL0 */
		{ 0x00818, 0x0000f000, 0x00003000 },	/* AUS40CMN_DIG RCAL5 */
		{ 0x03888, 0x000000ff, 0x00000027 },	/* CIO4PLL FREQDESCRIPTOR_0_C */
		{ 0x28004, 0x00000038, 0x00000008 },	/* AUX_LDO_CTRL */
		{ 0x03adc, 0x00003ff0, 0x00001760 },	/* CIO4PLL CFG_FRACN_FUNC */
		{ 0x0e0cc, 0x01f80000, 0x00380000 },	/* LANE0 TX_EQ_CTRL_REG11 */
		{ 0x1e0cc, 0x01f80000, 0x00380000 },	/* LANE1 TX_EQ_CTRL_REG11 */
		{ 0x0d090, 0x01f80000, 0x00380000 },	/* LANE0 RX_CFG TX_EQ_CTRL_REG11 */
		{ 0x1d090, 0x01f80000, 0x00380000 },	/* LANE1 RX_CFG TX_EQ_CTRL_REG11 */
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(dptx); i++)
		t6040_usb2_mask32(core + dptx[i].off, dptx[i].clear, dptx[i].set);
}

/* aciophy_xbar_config for PHY mode 3 (DP on all four lanes). */
static void t6040_dp_xbar_config(struct t6040_usb2_phy *tphy, bool flip)
{
	void __iomem *core = t6040_core(tphy);

	/* core+0x6c, orientation-independent for DP */
	t6040_usb2_mask32(core + T6040_CORE_XBAR, ~0xfc3fff03u, 0x48);
	/* core+0x78: lanes[4:0] = 0x14, then [17:5] by orientation */
	t6040_usb2_mask32(core + T6040_CORE_XBAR_LANES, 0x1f, 0x14);
	t6040_usb2_mask32(core + T6040_CORE_XBAR_LANES, 0x3ffe0, flip ? 0x100 : 0x22000);
	/* core+0x6c[1:0] = 2 (common to every mode) */
	t6040_usb2_mask32(core + T6040_CORE_XBAR, 0x3, 0x2);
}

/*
 * aciophy_dptx_adjust_lane_mode: the core+0x6c lane fields [4:2] and [7:5]
 * are 7 for every DP 1.x link rate (UHBR rates would use 2).
 */
static void t6040_dp_adjust_lane_mode(struct t6040_usb2_phy *tphy)
{
	t6040_usb2_set32(t6040_core(tphy) + T6040_CORE_XBAR, 0x1c | 0xe0);
}

/* aciophy_aux_init: the DP AUX block. */
static void t6040_dp_aux_init(struct t6040_usb2_phy *tphy)
{
	void __iomem *core = t6040_core(tphy);

	t6040_usb2_set32(core + T6040_CORE_AUX_PWR, 0x3);
	t6040_usb2_set32(core + T6040_CORE_AUX_PWR, 0xc);
	t6040_usb2_mask32(core + T6040_CORE_AUX_PWR, 0x300, 0x100);
	t6040_usb2_clear32(core + T6040_CORE_AUX_CTRL, 0x1);
}

/*
 * aciophy_configure_dp_clks: the two DP clock switches of the "cfg" window
 * (control at 0x14, status at 0x18). Each switch enables its pixel clock,
 * requests the two PMGR DP clocks and releases the requests one by one once
 * they are acknowledged. On the J616s the pixel-clock enable is acknowledged
 * with the enable bit alone and the PMGR acknowledgements arrive with the
 * 0x400 (SWITCH1) / 0x800 (SWITCH2) bit set next to the requests; the
 * requests are retried without that bit should the acknowledgment not come.
 */
static int t6040_dp_configure_clks(struct t6040_usb2_phy *tphy)
{
	void __iomem *cfg = t6040_cfg(tphy);
	u32 s;
	int ret;

#define DP_CLK_POLL(bit, set) \
	readl_poll_timeout(cfg + T6040_CFG_DP_CLKSWITCH_STAT, s, \
			   !!(s & BIT(bit)) == (set), 10, 200000)

	/* SWITCH1: pixel clock, then the PMGR DP0/DP1 requests */
	t6040_usb2_mask32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, 0x473, 0x10);
	ret = DP_CLK_POLL(0, true);
	if (ret)
		goto timeout;
	t6040_usb2_set32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, 0x60 | 0x400);
	ret = DP_CLK_POLL(2, true) ?: DP_CLK_POLL(1, true);
	if (ret) {
		dev_dbg(tphy->dev, "DP clock switch 1: no PMGR ack with 0x400 (status %#x)\n", s);
		t6040_usb2_clear32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, 0x400);
		ret = DP_CLK_POLL(2, true) ?: DP_CLK_POLL(1, true);
		if (ret)
			goto timeout;
	}
	t6040_usb2_clear32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, 0x20);
	ret = DP_CLK_POLL(1, false);
	if (ret)
		goto timeout;
	t6040_usb2_clear32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, 0x40);
	ret = DP_CLK_POLL(2, false);
	if (ret)
		goto timeout;

	/* SWITCH2, the same way */
	t6040_usb2_clear32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, 0x403);
	t6040_usb2_mask32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, 0xb8c, 0x80);
	ret = DP_CLK_POLL(3, true);
	if (ret)
		goto timeout;
	t6040_usb2_set32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, 0x300 | 0x800);
	ret = DP_CLK_POLL(5, true) ?: DP_CLK_POLL(4, true);
	if (ret) {
		dev_dbg(tphy->dev, "DP clock switch 2: no PMGR ack with 0x800 (status %#x)\n", s);
		t6040_usb2_clear32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, 0x800);
		ret = DP_CLK_POLL(5, true) ?: DP_CLK_POLL(4, true);
		if (ret)
			goto timeout;
	}
	t6040_usb2_clear32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, 0x100);
	ret = DP_CLK_POLL(4, false);
	if (ret)
		goto timeout;
	t6040_usb2_clear32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, 0x200);
	ret = DP_CLK_POLL(5, false);
	if (ret)
		goto timeout;
	t6040_usb2_clear32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, 0x80c);
	dev_dbg(tphy->dev, "DP clock switches on (ctrl %#x status %#x)\n",
		readl(cfg + T6040_CFG_DP_CLKSWITCH_CTRL),
		readl(cfg + T6040_CFG_DP_CLKSWITCH_STAT));
	return 0;

timeout:
	dev_err(tphy->dev, "DP clock switch timed out (ctrl %#x status %#x)\n",
		readl(cfg + T6040_CFG_DP_CLKSWITCH_CTRL),
		readl(cfg + T6040_CFG_DP_CLKSWITCH_STAT));
	return ret;
#undef DP_CLK_POLL
}

/* A PHY without the USB3 pieces (the HDMI converter's) only ever does DP. */
static bool t6040_dp_only(struct t6040_usb2_phy *tphy)
{
	return !tphy->usb3_ok;
}

/* apply_tunables' generic tables: the first six of t6040_usb3_tunable_list. */
#define T6040_DP_NR_GENERIC_TUNABLES	6

/*
 * slpctrl_init for DP on both lanes: the cmn_rm_ovrd/txrx_rm_ovrd releases of
 * t6040_usb3_lanes for lane 0 and lane 1 (USB3 releases only the plug's lane).
 * Without the PHY coprocessor nothing powers the common block and the TX lanes
 * once their overrides are released, so they are held on by override: the
 * reverse of aciophy_cmn_shutdown / aciophy_tx_powerdown, with the override
 * enables (0x2a; 0x4/0x40/0x400 per lane) set.
 */
static void t6040_dp_slpctrl(struct t6040_usb2_phy *tphy)
{
	void __iomem *core = t6040_core(tphy);
	unsigned int lane;

	t6040_usb2_set32(core + T6040_CORE_CMN_BIAS, T6040_CORE_CMN_BIAS_BGBIAS_EN_OV);
	udelay(1);
	t6040_usb2_set32(core + T6040_CORE_CMN_OVRD, T6040_CORE_CMN_OVRD_SLEEP_B_SML | 0x2a);
	udelay(1);
	t6040_usb2_set32(core + T6040_CORE_CMN_OVRD, T6040_CORE_CMN_OVRD_SLEEP_B_BIG);
	udelay(1);
	t6040_usb2_clear32(core + T6040_CORE_CMN_OVRD, T6040_CORE_CMN_OVRD_CLAMP);
	udelay(1);
	for (lane = 0; lane < 2; lane++) {
		t6040_usb2_set32(core + T6040_CORE_SLP_CTRL, (0x4 | 0x40 | 0x400) << lane);
		t6040_usb2_set32(core + T6040_CORE_SLP_CTRL, 0x10 << lane);
		udelay(1);
		t6040_usb2_set32(core + T6040_CORE_SLP_CTRL, 0x1 << lane);
		udelay(1);
		t6040_usb2_clear32(core + T6040_CORE_SLP_CTRL, 0x100 << lane);
		udelay(1);
	}
	dev_dbg(tphy->dev, "DP lanes held on: cmn_ovrd %#x slp_ctrl %#x bias %#x\n",
		readl(core + T6040_CORE_CMN_OVRD), readl(core + T6040_CORE_SLP_CTRL),
		readl(core + T6040_CORE_CMN_BIAS));
}

/*
 * aciophy_phy_init for mode 3: wake the CFG block (shared with USB3), apply
 * the generic ADT tables and the DPTX tunables, xbar_config, slpctrl_init and
 * the reset deassert of partC (no coprocessor firmware in this mode).
 */
static int t6040_dp_phy_init(struct t6040_usb2_phy *tphy, bool flip)
{
	void __iomem *cfg = t6040_cfg(tphy);
	size_t i;
	int ret;

	ret = t6040_usb3_wake(tphy);
	if (ret)
		return ret;
	for (i = 0; i < T6040_DP_NR_GENERIC_TUNABLES; i++)
		apple_tunable_apply(tphy->regs[t6040_usb3_tunable_list[i].region],
				    tphy->tunables[i]);
	t6040_dp_fixed_tunables(t6040_core(tphy));
	t6040_dp_xbar_config(tphy, flip);
	t6040_dp_slpctrl(tphy);
	t6040_usb2_set32(cfg + T6040_CFG_CTRL, T6040_CFG_CTRL_PHY_RESET_N);
	fsleep(100);
	dev_dbg(tphy->dev, "DP PHY init (mode 3): cfg %#x/%#x\n",
		readl(cfg + T6040_CFG_CTRL), readl(cfg + T6040_CFG_STATUS));
	return 0;
}

/* aciophy_dptx_main_init up to the link rate, which .configure sets. */
static void t6040_dp_mode_setup(struct t6040_usb2_phy *tphy, bool flip)
{
	t6040_dp_fixed_tunables(t6040_core(tphy));
	t6040_dp_xbar_config(tphy, flip);
	t6040_dp_adjust_lane_mode(tphy);
	t6040_dp_aux_init(tphy);
}

/*
 * The per-rate link sequences of the T6050 fixed DP PHY, which shares the
 * ATC PHY core with the T6040. Bank numbers are the atc-phy "reg" indices of
 * the T6050/T6040 ADT; banks 2..34 are offsets into "core". Bank 39 is the
 * T6050 clock shim; on the T6040 the DP clock switch of the "cfg" window
 * stands in for it and runs after the lane program, as AppleT6040TypeCPhy
 * does. The sequences hold no power-domain or tunable steps that apply here.
 */
enum atc_operation { ATC_MASK, ATC_TUNABLE, ATC_DELAY, ATC_POLL, ATC_POWER };
struct atc_op {
	u8 op, bank;
	u16 offset;
	u32 mask, value, timeout;
};

struct atc_rate_op {
	struct atc_op op;
	u8 rates;
	u32 values[4];
};

#include "atc-t6050-dp-data.h"

static const struct { u8 bank; u32 offset; } t6040_dp_core_banks[] = {
	{2, 0}, {3, 0x400}, {4, 0x800}, {5, 0xa00}, {6, 0x1000},
	{7, 0x3000}, {8, 0x3200}, {9, 0x3800}, {10, 0x3a00},
	{14, 0x7000}, {16, 0x9000}, {18, 0xb000}, {19, 0xc000},
	{20, 0xd000}, {21, 0xe000}, {22, 0xf000}, {25, 0x19000},
	{27, 0x1b000}, {28, 0x1c000}, {29, 0x1d000}, {30, 0x1e000},
	{31, 0x1f000}, {33, 0x28000}, {34, 0x28400},
};

static void __iomem *t6040_dp_bank(struct t6040_usb2_phy *tphy, u8 bank)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(t6040_dp_core_banks); i++)
		if (t6040_dp_core_banks[i].bank == bank)
			return t6040_core(tphy) + t6040_dp_core_banks[i].offset;
	return NULL;
}

static int t6040_dp_seq_step(struct t6040_usb2_phy *tphy, const struct atc_op *s)
{
	void __iomem *reg;
	u32 v;
	int ret;

	if (s->op == ATC_DELAY) {
		udelay(s->value);
		return 0;
	}
	if (s->op == ATC_POWER || s->op == ATC_TUNABLE || s->bank == 39)
		return 0;
	reg = t6040_dp_bank(tphy, s->bank);
	if (!reg) {
		dev_err(tphy->dev, "DP link sequence: no bank %u\n", s->bank);
		return -EINVAL;
	}
	reg += s->offset;
	if (s->op == ATC_MASK) {
		writel((readl(reg) & ~s->mask) | s->value, reg);
		return 0;
	}
	ret = readl_poll_timeout(reg, v, (v & s->mask) == s->value,
				 s->timeout > 1000 ? 1000 : 1, s->timeout);
	if (ret)
		dev_err(tphy->dev, "DP link sequence: bank %u+%#x timed out: %#x & %#x != %#x\n",
			s->bank, s->offset, v, s->mask, s->value);
	return ret;
}

/* Take the link down if it is up, then bring it up at rate index @idx (-1: off). */
static int t6040_dp_set_rate(struct t6040_usb2_phy *tphy, int idx)
{
	size_t i;
	int ret;

	if (tphy->dp_rate >= 0) {
		for (i = 0; i < ARRAY_SIZE(dp_link_off); i++) {
			ret = t6040_dp_seq_step(tphy, &dp_link_off[i]);
			if (ret)
				return ret;
		}
		tphy->dp_rate = -1;
	}
	if (idx < 0)
		return 0;
	for (i = 0; i < ARRAY_SIZE(dp_link_on); i++) {
		struct atc_op op = dp_link_on[i].op;

		if (!(dp_link_on[i].rates & BIT(idx)))
			continue;
		op.value = dp_link_on[i].values[idx];
		ret = t6040_dp_seq_step(tphy, &op);
		if (ret)
			return ret;
	}
	ret = t6040_dp_configure_clks(tphy);
	if (ret)
		return ret;
	tphy->dp_rate = idx;
	dev_dbg(tphy->dev, "DP link on, rate index %d\n", idx);
	return 0;
}

/* Drive settings of the T6050 sequence: lanes 0/1 on PMA group 1, 2/3 on group 0. */
static void t6040_dp_drive(struct t6040_usb2_phy *tphy, unsigned int lane, u32 preset)
{
	unsigned int group = lane < 2 ? 1 : 0;
	bool tx = lane & 1;
	void __iomem *base = t6040_dp_bank(tphy, (tx ? 21 : 20) + 9 * group);
	u32 ctrl = ((preset >> 2) & 0xf) | ((preset & 3) << 14);
	u32 eq = ((preset >> 5) & 0x7fffe) | 1;

	if (tx)
		t6040_usb2_mask32(base + 0x100, BIT(20), BIT(20));
	t6040_usb2_mask32(base + (tx ? 0x58 : 0x68), 0xc00f, ctrl);
	t6040_usb2_mask32(base + (tx ? 0x104 : 0xac), BIT(29), BIT(29));
	t6040_usb2_mask32(base + (tx ? 0x100 : 0xa8), 0x7ffff, eq);
}

/* ---- DP phy_ops ---- */

static int t6040_dp_power_on(struct phy *phy)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);
	int ret;

	guard(mutex)(&tphy->lock);
	if (!tphy->dp_ok || tphy->dp_up)
		return 0;
	/*
	 * A Type-C PHY in DP alt mode (pin assignment C/E) hands all four
	 * lanes to DP: aciophy_phy_shutdown of the USB3 side first, then the
	 * same mode-3 init as the fixed DP PHY.
	 */
	if (!t6040_dp_only(tphy))
		t6040_usb3_down_locked(tphy);
	ret = t6040_dp_phy_init(tphy, tphy->dp_flip);
	if (ret) {
		dev_err(tphy->dev, "DP PHY init failed: %d\n", ret);
		return ret;
	}
	t6040_dp_mode_setup(tphy, tphy->dp_flip);
	tphy->dp_rate = -1;
	tphy->dp_up = true;
	dev_dbg(tphy->dev, "DP PHY mode set, lane map %s\n",
		tphy->dp_flip ? "reversed" : "normal");
	return 0;
}

static int t6040_dp_power_off(struct phy *phy)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);

	guard(mutex)(&tphy->lock);
	tphy->dp_up = false;
	return 0;
}

/* GET_MAX_LANE_COUNT: DP on all four lanes in mode 3. */
static int t6040_dp_validate(struct phy *phy, enum phy_mode mode, int submode,
			     union phy_configure_opts *opts_)
{
	struct phy_configure_opts_dp *opts = &opts_->dp;

	if (mode != PHY_MODE_DP)
		return -EINVAL;
	opts->lanes = 4;
	return 0;
}

static int t6040_dp_configure(struct phy *phy, union phy_configure_opts *opts_)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);
	struct phy_configure_opts_dp *opts = &opts_->dp;
	int idx = -1;
	int ret;

	guard(mutex)(&tphy->lock);
	if (!tphy->dp_up)
		return -EINVAL;

	if (opts->set_rate) {
		switch (opts->link_rate) {
		case 1620:
			idx = 0;
			break;
		case 2700:
			idx = 1;
			break;
		case 5400:
			idx = 2;
			break;
		case 8100:
			idx = 3;
			break;
		case 0:
			idx = -1;
			break;
		default:
			return -EINVAL;
		}
		if (idx != tphy->dp_rate) {
			ret = t6040_dp_set_rate(tphy, idx);
			if (ret)
				return ret;
		}
	}
	if (opts->set_voltages && tphy->dp_rate >= 0) {
		unsigned int i;

		for (i = 0; i < opts->lanes && i < 4; i++) {
			unsigned int v = min_t(unsigned int, opts->voltage[i], 3);
			unsigned int pe = min_t(unsigned int, opts->pre[i], 3);

			t6040_dp_drive(tphy, i, dp_training_default[4 * v + pe]);
		}
	}
	return 0;
}

/* ACTIVATE/DEACTIVATE: PHY_MODE_DP, submode = the DCP's DPTX port index. */
static int t6040_dp_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	if (mode == PHY_MODE_DP || mode == PHY_MODE_INVALID)
		return 0;
	return -EOPNOTSUPP;
}

static const struct phy_ops t6040_dp_phy_ops = {
	.owner = THIS_MODULE,
	.power_on = t6040_dp_power_on,
	.power_off = t6040_dp_power_off,
	.configure = t6040_dp_configure,
	.validate = t6040_dp_validate,
	.set_mode = t6040_dp_set_mode,
};

/* ---- Type-C mode switch: the Type-C controller asks for DP on a USB-C port ---- */

static int t6040_dp_mux_set(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
	struct t6040_usb2_phy *tphy = typec_mux_get_drvdata(mux);
	bool dp = state->alt && state->alt->svid == USB_TYPEC_DP_SID;
	bool alt = false;

	guard(mutex)(&tphy->lock);
	tphy->dp_flip = (tphy->orientation == TYPEC_ORIENTATION_REVERSE);
	/*
	 * Latch the DP alt mode state for the DCP's Type-C route. Pin
	 * assignments C and E put DP on all four lanes (PHY mode 3, as the
	 * fixed DP route); D (two DP lanes next to USB3, PHY mode 4 with the
	 * coprocessor) is not supported. The DP lanes themselves are brought
	 * up by the DCP through phy_power_on()/phy_configure() on the DP phy.
	 */
	if (dp) {
		switch (state->mode) {
		case TYPEC_DP_STATE_C:
		case TYPEC_DP_STATE_E:
			alt = true;
			break;
		case TYPEC_DP_STATE_D:
			dev_warn_once(tphy->dev, "Type-C mux: DP pin assignment D (two lanes next to USB3) not supported\n");
			break;
		default:
			break;
		}
	}
	tphy->dp_alt = alt;
	dev_dbg(tphy->dev, "Type-C mux: %s state %lu -> DP alt mode %d, lane map %s\n",
		dp ? "DP" : "non-DP", state->mode, alt, tphy->dp_flip ? "reversed" : "normal");
	if (alt && tphy->dp_up)
		t6040_dp_xbar_config(tphy, tphy->dp_flip);
	return 0;
}

/* For the DCP's Type-C route: is this PHY's port in DP alt mode (C/E)? */
bool apple_t6040_phy_dp_alt(struct phy *phy)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);

	return tphy && tphy->dp_alt;
}
EXPORT_SYMBOL_GPL(apple_t6040_phy_dp_alt);

static void t6040_dp_mux_unregister(void *mux)
{
	typec_mux_unregister(mux);
}

/*
 * Everything the DP route needs from the device tree; a node without it gets
 * no DP phy. A USB-C PHY offers DP through a Type-C mode switch; a fixed DP
 * route (an HDMI converter's PHY) is driven straight by the DCP with no mux
 * and uses the normal lane map.
 */
static int t6040_dp_probe(struct t6040_usb2_phy *tphy)
{
	struct device *dev = tphy->dev;
	unsigned int i;

	tphy->dp_rate = -1;

	/*
	 * DP mode needs the "core" and "cfg" windows. A USB-C PHY has them
	 * alongside its USB regions; a fixed DP PHY has them without any
	 * pipehandler or USB3 coprocessor. A node with neither is USB2 only.
	 */
	if (!tphy->regs[T6040_USB3_CORE] || !tphy->regs[T6040_USB3_CFG])
		return 0;

	tphy->dp = devm_phy_create(dev, NULL, &t6040_dp_phy_ops);
	if (IS_ERR(tphy->dp))
		return PTR_ERR(tphy->dp);
	phy_set_drvdata(tphy->dp, tphy);

	/* DP-only node: the generic apply_tunables tables (absent = not applied). */
	if (t6040_dp_only(tphy)) {
		for (i = 0; i < T6040_DP_NR_GENERIC_TUNABLES; i++) {
			const char *prop = t6040_usb3_tunable_list[i].prop;

			if (!of_property_present(dev->of_node, prop) ||
			    !tphy->res[t6040_usb3_tunable_list[i].region]) {
				dev_warn(dev, "DP: no %s\n", prop);
				continue;
			}
			struct resource *res = tphy->res[t6040_usb3_tunable_list[i].region];

			tphy->tunables[i] = devm_apple_tunable_parse(dev, dev->of_node, prop, res);
			if (IS_ERR(tphy->tunables[i])) {
				dev_warn(dev, "DP: %s: %pe\n", prop, tphy->tunables[i]);
				tphy->tunables[i] = NULL;
			}
		}
	}

	if (of_property_present(dev->of_node, "mode-switch")) {
		struct typec_mux_desc mux_desc = {
			.drvdata = tphy,
			.fwnode = dev_fwnode(dev),
			.set = t6040_dp_mux_set,
		};

		tphy->mux = typec_mux_register(dev, &mux_desc);
		if (IS_ERR(tphy->mux))
			return PTR_ERR(tphy->mux);
		if (devm_add_action_or_reset(dev, t6040_dp_mux_unregister, tphy->mux))
			return -ENOMEM;
	}

	tphy->dp_ok = true;
	return 0;
}

/* ---- the coprocessor's RTKit ---- */

/* The coprocessor places its buffers in its own __DATA SRAM. */
static int t6040_cpu_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct t6040_usb2_phy *tphy = cookie;
	resource_size_t size = resource_size(tphy->cpu_data);

	if (!bfr->iova || bfr->iova < T6040_CPU_DATA_IOVA || bfr->size > size ||
	    bfr->iova - T6040_CPU_DATA_IOVA > size - bfr->size) {
		dev_err(tphy->dev, "USB3: coprocessor buffer %pad+%#zx outside its SRAM\n",
			&bfr->iova, bfr->size);
		return -EINVAL;
	}
	bfr->iomem = ioremap_np(tphy->cpu_data->start + (bfr->iova - T6040_CPU_DATA_IOVA),
				bfr->size);
	if (!bfr->iomem)
		return -ENOMEM;
	bfr->is_mapped = true;
	return 0;
}

static void t6040_cpu_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	if (bfr->iomem)
		iounmap(bfr->iomem);
}

static void t6040_cpu_crashed(void *cookie, const void *crashlog, size_t size)
{
	struct t6040_usb2_phy *tphy = cookie;

	dev_err(tphy->dev, "USB3: the PHY coprocessor crashed\n");
}

static void t6040_cpu_recv_message(void *cookie, u8 ep, u64 msg)
{
	struct t6040_usb2_phy *tphy = cookie;

	if (ep == T6040_CPU_EP_APP)
		complete(&tphy->app_reply);
	else
		dev_dbg(tphy->dev, "USB3: coprocessor message on endpoint %u: %#llx\n", ep, msg);
}

static const struct apple_rtkit_ops t6040_cpu_rtkit_ops = {
	.flags = APPLE_RTKIT_COPROC_PLACES_BUFFERS,
	.crashed = t6040_cpu_crashed,
	.recv_message = t6040_cpu_recv_message,
	.shmem_setup = t6040_cpu_shmem_setup,
	.shmem_destroy = t6040_cpu_shmem_destroy,
};

/*
 * Map the PHY register windows shared by the USB3 and DisplayPort paths. Each
 * is optional: a USB-C PHY has the full set, the fixed HDMI DP PHY (atcphy3)
 * lacks none of core/cfg but has no pipehandler. Returns true once at least
 * "core" and "cfg" are mapped, which is all DP mode needs.
 */
static bool t6040_map_regions(struct t6040_usb2_phy *tphy, struct platform_device *pdev)
{
	struct device *dev = tphy->dev;
	size_t i;

	for (i = 0; i < T6040_USB3_NR_REGIONS; i++) {
		tphy->res[i] = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							    t6040_usb3_region_names[i]);
		if (!tphy->res[i])
			continue;
		tphy->regs[i] = devm_ioremap_resource(dev, tphy->res[i]);
		if (IS_ERR(tphy->regs[i])) {
			dev_warn(dev, "\"%s\" region: %pe\n",
				 t6040_usb3_region_names[i], tphy->regs[i]);
			tphy->regs[i] = NULL;
			tphy->res[i] = NULL;
		}
	}
	return tphy->regs[T6040_USB3_CORE] && tphy->regs[T6040_USB3_CFG];
}

/*
 * Everything USB3 needs from the device tree. A missing piece leaves the
 * port at USB2; only a mailbox that has not probed yet defers.
 */
static int t6040_usb3_probe(struct t6040_usb2_phy *tphy, struct platform_device *pdev)
{
	struct device *dev = tphy->dev;
	struct typec_switch_desc sw_desc = {
		.drvdata = tphy,
		.fwnode = dev_fwnode(dev),
		.set = t6040_usb3_sw_set,
	};
	struct typec_switch_dev *sw;
	struct resource *ph;
	size_t i;
	int ret;

	tphy->usb3 = devm_phy_create(dev, NULL, &t6040_usb3_phy_ops);
	if (IS_ERR(tphy->usb3))
		return PTR_ERR(tphy->usb3);
	phy_set_drvdata(tphy->usb3, tphy);
	init_completion(&tphy->app_reply);

	if (!of_property_present(dev->of_node, "orientation-switch"))
		return 0;

	/*
	 * Registered first: tipd waits for the switch the DT names, so the port
	 * keeps USB2, roles and PD even if a USB3 piece below is missing.
	 */
	sw = typec_switch_register(dev, &sw_desc);
	if (IS_ERR(sw))
		return PTR_ERR(sw);
	ret = devm_add_action_or_reset(dev, t6040_usb3_switch_unregister, sw);
	if (ret)
		return ret;

	for (i = 0; i < T6040_USB3_NR_REGIONS; i++) {
		if (!tphy->regs[i]) {
			dev_warn(dev, "USB3: no \"%s\" region, USB2 only\n",
				 t6040_usb3_region_names[i]);
			return 0;
		}
	}
	tphy->cpu_data = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cpu-data");
	ph = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pipehandler");
	if (!tphy->cpu_data || !ph) {
		dev_warn(dev, "USB3: no coprocessor SRAM region, USB2 only\n");
		return 0;
	}

	for (i = 0; i < T6040_USB3_NR_TUNABLES; i++) {
		tphy->tunables[i] =
			devm_apple_tunable_parse(dev, dev->of_node, t6040_usb3_tunable_list[i].prop,
						 tphy->res[t6040_usb3_tunable_list[i].region]);
		if (IS_ERR(tphy->tunables[i])) {
			dev_warn(dev, "USB3: %s: %pe, USB2 only\n",
				 t6040_usb3_tunable_list[i].prop, tphy->tunables[i]);
			return 0;
		}
	}
	tphy->tunable_pipehandler = devm_apple_tunable_parse(dev, dev->of_node,
							     "apple,tunable-pipehandler", ph);
	if (IS_ERR(tphy->tunable_pipehandler)) {
		dev_warn(dev, "USB3: apple,tunable-pipehandler: %pe, USB2 only\n",
			 tphy->tunable_pipehandler);
		return 0;
	}

	tphy->rtk = devm_apple_rtkit_init(dev, tphy, "phy-cpu", 0, &t6040_cpu_rtkit_ops);
	if (IS_ERR(tphy->rtk)) {
		ret = PTR_ERR(tphy->rtk);
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_warn(dev, "USB3: no PHY coprocessor mailbox (%d), USB2 only\n", ret);
		return 0;
	}

	tphy->usb3_ok = true;
	return 0;
}

/*
 * dwc3-apple asserts this at probe, deasserts it before dwc3_core_init()
 * (which then powers this PHY on), and asserts it again on stop. As in
 * atc.c, assert drops RESET_N and then sets the clamp; deassert releases
 * the clamp and then sets RESET_N.
 */
static int t6040_usb2_reset_assert(struct reset_controller_dev *rcdev,
				   unsigned long id)
{
	struct t6040_usb2_phy *tphy =
		container_of(rcdev, struct t6040_usb2_phy, rcdev);
	int ret;

	PM_RUNTIME_ACQUIRE(tphy->dev, pm);
	ret = PM_RUNTIME_ACQUIRE_ERR(&pm);
	if (ret)
		return ret;

	guard(mutex)(&tphy->lock);
	t6040_usb2_clear32(tphy->pipehandler + T6040_PIPEHANDLER_AON_GEN,
			   T6040_PIPEHANDLER_AON_GEN_DWC3_RESET_N);
	t6040_usb2_set32(tphy->pipehandler + T6040_PIPEHANDLER_AON_GEN,
			 T6040_PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
	if (tphy->dwc3_running) {
		tphy->dwc3_running = false;
		pm_runtime_put(tphy->dev);
	}
	return 0;
}

static int t6040_usb2_reset_deassert(struct reset_controller_dev *rcdev,
				     unsigned long id)
{
	struct t6040_usb2_phy *tphy =
		container_of(rcdev, struct t6040_usb2_phy, rcdev);
	u32 value;
	int ret;

	PM_RUNTIME_ACQUIRE(tphy->dev, pm);
	ret = PM_RUNTIME_ACQUIRE_ERR(&pm);
	if (ret)
		return ret;

	guard(mutex)(&tphy->lock);
	if (tphy->shutdown_failed)
		return -EIO;
	t6040_usb2_clear32(tphy->pipehandler + T6040_PIPEHANDLER_AON_GEN,
			   T6040_PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
	t6040_usb2_set32(tphy->pipehandler + T6040_PIPEHANDLER_AON_GEN,
			 T6040_PIPEHANDLER_AON_GEN_DWC3_RESET_N);

	value = readl(tphy->pipehandler + T6040_PIPEHANDLER_AON_GEN);
	if (value == U32_MAX ||
	    (value & (T6040_PIPEHANDLER_AON_GEN_DWC3_RESET_N |
		      T6040_PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN)) !=
		    T6040_PIPEHANDLER_AON_GEN_DWC3_RESET_N)
		return -EIO;
	if (!tphy->dwc3_running) {
		pm_runtime_get_noresume(tphy->dev);
		tphy->dwc3_running = true;
	}
	return 0;
}

static const struct reset_control_ops t6040_usb2_reset_ops = {
	.assert = t6040_usb2_reset_assert,
	.deassert = t6040_usb2_reset_deassert,
};

static int t6040_usb2_reset_xlate(struct reset_controller_dev *rcdev,
				  const struct of_phandle_args *reset_spec)
{
	return 0;
}

static struct phy *t6040_usb2_xlate(struct device *dev,
				    const struct of_phandle_args *args)
{
	struct t6040_usb2_phy *tphy = dev_get_drvdata(dev);

	if (args->args_count != 1)
		return ERR_PTR(-ENODEV);

	switch (args->args[0]) {
	case PHY_TYPE_USB2:
		return tphy->phy;
	case PHY_TYPE_USB3:
		return tphy->usb3;
	case PHY_TYPE_DP:
		return tphy->dp;
	default:
		return ERR_PTR(-ENODEV);
	}
}

static int t6040_usb2_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct t6040_usb2_phy *tphy;
	struct phy_provider *provider;
	int ret;

	tphy = devm_kzalloc(dev, sizeof(*tphy), GFP_KERNEL);
	if (!tphy)
		return -ENOMEM;

	tphy->dev = dev;
	/*
	 * dwc3 powers on its phys from dwc3_core_init(), which runs inside
	 * dwc3_core_probe() -- before any consumer can deliver phy_set_mode()
	 * to this phy. dwc3-apple's early phy_set_mode() call happens while
	 * dwc.usb2_generic_phy[] is still unpopulated, so it is a silent no-op
	 * on a NULL phy, and a PHY_MODE_INVALID default then fails power_on
	 * with -EINVAL ("failed to initialize core", ticket 108). This
	 * provider implements exactly one mode, so start in it; set_mode
	 * still rejects every request that is not PHY_MODE_USB_HOST.
	 */
	tphy->mode = PHY_MODE_USB_HOST;
	mutex_init(&tphy->lock);
	platform_set_drvdata(pdev, tphy);

	tphy->usb2 = devm_platform_ioremap_resource_byname(pdev, "bank0");
	if (IS_ERR(tphy->usb2))
		return dev_err_probe(dev, PTR_ERR(tphy->usb2),
				     "failed to map USB2 bank 0\n");

	if (of_property_present(dev->of_node, "apple,tunable-usb2-host")) {
		tphy->tunable_usb2_host =
			devm_apple_tunable_parse(dev, dev->of_node, "apple,tunable-usb2-host",
						 platform_get_resource_byname(pdev, IORESOURCE_MEM,
									      "bank0"));
		if (IS_ERR(tphy->tunable_usb2_host))
			return dev_err_probe(dev, PTR_ERR(tphy->tunable_usb2_host),
					     "invalid apple,tunable-usb2-host\n");
	}

	tphy->evt = devm_platform_ioremap_resource_byname(pdev, "bank1");
	if (IS_ERR(tphy->evt))
		return dev_err_probe(dev, PTR_ERR(tphy->evt),
				     "failed to map USB2 event bank 1\n");

	/* Before devm_phy_create(): the PHY core then manages the references. */
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	tphy->phy = devm_phy_create(dev, NULL, &t6040_usb2_phy_ops);
	if (IS_ERR(tphy->phy))
		return PTR_ERR(tphy->phy);
	phy_set_drvdata(tphy->phy, tphy);

	/*
	 * The USB-C PHYs have a pipehandler and drive a dwc3 (reset provider);
	 * the fixed HDMI DP PHY (atcphy3) has neither. Map it and register the
	 * reset controller only when present, so a DP-only node still probes.
	 */
	if (platform_get_resource_byname(pdev, IORESOURCE_MEM, "pipehandler")) {
		tphy->pipehandler = devm_platform_ioremap_resource_byname(pdev,
									  "pipehandler");
		if (IS_ERR(tphy->pipehandler))
			return dev_err_probe(dev, PTR_ERR(tphy->pipehandler),
					     "failed to map pipehandler bank\n");

		tphy->rcdev.owner = THIS_MODULE;
		tphy->rcdev.nr_resets = 1;
		tphy->rcdev.ops = &t6040_usb2_reset_ops;
		tphy->rcdev.of_node = dev->of_node;
		tphy->rcdev.of_reset_n_cells = 0;
		tphy->rcdev.of_xlate = t6040_usb2_reset_xlate;
		ret = devm_reset_controller_register(dev, &tphy->rcdev);
		if (ret)
			return ret;
	}

	/* Map the core/cfg (and USB3) windows once; both sub-probes share them. */
	t6040_map_regions(tphy, pdev);

	ret = t6040_usb3_probe(tphy, pdev);
	if (ret)
		return ret;

	ret = t6040_dp_probe(tphy);
	if (ret)
		return ret;

	provider = devm_of_phy_provider_register(dev, t6040_usb2_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id t6040_usb2_of_match[] = {
	{ .compatible = "apple,t6040-atcphy" },
	{}
};
MODULE_DEVICE_TABLE(of, t6040_usb2_of_match);

static struct platform_driver t6040_usb2_driver = {
	.probe = t6040_usb2_probe,
	.driver = {
		.name = "phy-apple-t6040-usb2",
		.of_match_table = t6040_usb2_of_match,
	},
};
module_platform_driver(t6040_usb2_driver);

MODULE_AUTHOR("The Asahi Linux Contributors");
MODULE_DESCRIPTION("Apple T6040 Type-C PHY, USB2/USB3 host and DisplayPort");
MODULE_LICENSE("Dual BSD/GPL");
