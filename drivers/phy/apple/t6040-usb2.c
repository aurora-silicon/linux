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
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/phy/apple-atc.h>
#include <linux/phy/apple-t6040-cio.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset-controller.h>
#include <linux/soc/apple/dp-tunnel.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/soc/apple/tunable.h>
#include <linux/suspend.h>
#include <linux/usb/pd.h>
#include <linux/usb/typec_mux.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_tbt.h>
#include <linux/wait.h>

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
 * DisplayPort (four-lane mode 3 and two-lane mode 4). Offsets are into the "core" and "cfg" windows
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
#define T6040_CORE_CFG1_TOP		0x10	/* ACIOPHY_TOP_BLK_CFG1_TOP: per-lane RX power */
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
#define T6040_CPU_RUN_START		0x10	/* RUN sets bit 4; STOP stores ~0x10 */
#define T6040_CPU_IDLE_CTRL		0x40	/* MXWRAP_BLK_IDLE_CTRL */
#define T6040_CPU_IDLE_CTRL_EN		BIT(0)
#define T6040_CPU_STATUS		0x44
#define T6040_CPU_STATUS_IDLE		BIT(1)

/* ADT acio-phy-cpuN segment-ranges: the coprocessor sees its __DATA SRAM ("cpu-data") here */
#define T6040_CPU_DATA_IOVA		0x10000000
/* The coprocessor's application endpoint, and the one message macOS sends it */
#define T6040_CPU_EP_APP		32
#define T6040_CPU_MSG_POWER_DOWN	0x0100000000000000ULL

#define T6040_RECOVERY_RETRIES		1
#define T6040_RECOVERY_PM_RETRIES	5
#define T6040_RECOVERY_PM_BACKOFF_US	2000
/* DP's power-on waits this long for the wake of a host kept through a sleep */
#define T6040_SLEEP_DP_WAIT_MS		2000

static unsigned int recovery_dwell_ms = 50;
module_param(recovery_dwell_ms, uint, 0644);
MODULE_PARM_DESC(recovery_dwell_ms,
		 "PHY coprocessor hard-recovery off dwell in milliseconds (minimum 1, default 50)");

struct t6040_recovery_state {
	bool cycle_pending;
	u8 retries;
};

static void t6040_recovery_power_down(struct t6040_recovery_state *state,
				      bool acknowledged, bool clean_stop)
{
	if (!acknowledged || !clean_stop)
		state->cycle_pending = true;
}

static bool t6040_recovery_start(struct t6040_recovery_state *state)
{
	state->retries = 0;
	return state->cycle_pending;
}

static bool t6040_recovery_retry(struct t6040_recovery_state *state)
{
	state->cycle_pending = true;
	if (state->retries >= T6040_RECOVERY_RETRIES)
		return false;
	state->retries++;
	return true;
}

static void t6040_recovery_complete(struct t6040_recovery_state *state)
{
	state->cycle_pending = false;
	state->retries = 0;
}

/*
 * The SoC lost power in a sleep, and the coprocessor and both of its power
 * domains with it: that is the power cycle a stop that wasn't clean asks for.
 */
static void __maybe_unused t6040_recovery_power_lost(struct t6040_recovery_state *state)
{
	state->cycle_pending = false;
}

enum t6040_recovery_pm_action {
	T6040_RECOVERY_PM_SUSPEND,
	T6040_RECOVERY_PM_RETRY,
	T6040_RECOVERY_PM_FALLBACK,
};

static enum t6040_recovery_pm_action
t6040_recovery_pm_action(int usage_count, int child_count,
			 unsigned int attempt, unsigned int retries)
{
	if (!usage_count && !child_count)
		return T6040_RECOVERY_PM_SUSPEND;
	if (attempt < retries)
		return T6040_RECOVERY_PM_RETRY;
	return T6040_RECOVERY_PM_FALLBACK;
}

/*
 * How often the recovery waits for the mailbox's users to go. Never when the
 * PHY is one of them: forced into suspend while it is still in use, by a phy
 * that is powered on or by the controller's reset, it stays a consumer of its
 * suppliers, and the mailbox keeps the reference of that device link however
 * long the recovery waits.
 */
static unsigned int t6040_recovery_pm_retries(bool phy_in_use)
{
	return phy_in_use ? 0 : T6040_RECOVERY_PM_RETRIES;
}

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
	struct device *mbox_dev;
	struct completion app_reply;
	struct t6040_recovery_state recovery;
	atomic_t drop_next_power_down_ack;
	struct dentry *debugfs;
	enum typec_orientation orientation;
	bool usb3_up;		/* tuned by the coprocessor this power cycle */
	bool usb3_on;		/* dwc3 holds the USB3 phy powered on */
	bool usb3_pipe;		/* dwc3 asked for the PIPE switch this power cycle */
	bool usb3_lent;		/* DP mode took the lanes of this Type-C port */
	bool pipe_switched;	/* the controller's PIPE runs on this PHY */

	/* DisplayPort; dp_ok is false when the DT lacks a DP route */
	struct phy *dp;
	struct typec_mux_dev *mux;
	bool dp_ok;
	bool dp_up;		/* DP mode is programmed on the PHY */
	bool dp_alt;		/* the Type-C port is in DP alt mode */
	bool dp_two_lanes;	/* pin D keeps USB3 on the other pair */
	bool dp_mode_two_lanes;	/* latched until DP power_off, even after mux exit */
	bool dp_mode_flip;
	bool dp_flip;		/* plug orientation reversed for the DP lanes */
	int dp_rate;		/* link rate index of the running link sequence, -1 = off */

	/* CIO (USB4/Thunderbolt); cio_ok is false unless the DT has the CIO tables */
	struct apple_tunable *cio_tunables[5];
	bool cio_ok;
	bool cio_up;		/* PHY in mode 2, holds a runtime PM reference */
	bool pipe_tunneled;	/* the controller's PIPE runs on the USB4 router */
	u8 tun_rate[2];		/* DP tunnel link rate per DP IN adapter, 0 = off */
	bool tun_pll;		/* AUSPLL programmed for DP tunnels */
	bool router_running;	/* the USB4 router runs on this port's CIO link */
	bool usb3_tunnel_req;	/* the router carries USB3 for the controller */
	bool cio_flip;

	/* A sleep with the host kept, see apple_t6040_atcphy_sleep_prepare() */
	bool sleep_armed;	/* taken on by the prepare, until the finish */
	bool sleep_down;	/* USB2 off and USB3 asleep for it, behind their consumers */
	bool sleep_reset;	/* the controller held in reset for it, behind its consumer */
	bool sleep_usb3;	/* USB3 ran at a sleep of this host, and comes back with USB2 */
	bool sleep_waking;	/* in that wake: no power cycle of the PHY's domains */
	bool sleep_back;	/* the system resumes and that wake is still to run */
	wait_queue_head_t sleep_wq;	/* DP's power-on waits here for sleep_back to end */
	bool sleep_no_keep;	/* keeping the host failed: it stops for the next sleep */
	int sleep_ret;		/* what the wake has come to, for the controller's glue */
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

/* The USB2 host sequence itself. A field that did not take leaves the PHY failed. */
static int t6040_usb2_on_locked(struct t6040_usb2_phy *tphy)
{
	u32 status, ctl, sig, tune, mode, sig_want;

	lockdep_assert_held(&tphy->lock);

	/*
	 * eUSB2 PHY init, in this exact order.
	 * Keep the five signal mutations separate: the reference sequence performs
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

	dev_info(tphy->dev, "USB2 host sequence complete\n");
	return 0;
failed:
	tphy->shutdown_failed = true;
	return -EIO;
}

static int t6040_usb2_power_on(struct phy *phy)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);
	int ret;

	guard(mutex)(&tphy->lock);
	if (tphy->shutdown_failed)
		return -EIO;

	if (tphy->mode != PHY_MODE_USB_HOST)
		return -EINVAL;
	if (tphy->powered)
		return 0;

	ret = t6040_usb2_on_locked(tphy);
	if (ret)
		return ret;
	tphy->powered = true;
	/*
	 * The controller's reset released and USB2 came up, which is all that
	 * keeping a host through a sleep needs to work again at its wake.
	 */
	tphy->sleep_no_keep = false;
	return 0;
}

/*
 * Power the PHY down: port reset (held 5 ms), USBCTL mode 4, SIDDQ, PHY
 * reset, then gate the APB and reference clocks. The host controller must be
 * halted first; this is not a substitute for an xHCI halt.
 */
static int t6040_usb2_off_locked(struct t6040_usb2_phy *tphy)
{
	u32 ctl, mode, tune;

	lockdep_assert_held(&tphy->lock);
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
	return 0;
}

/* The wake of a sleep with the host kept has run, or won't: DP stops waiting for it. */
static void t6040_sleep_back_done(struct t6040_usb2_phy *tphy)
{
	lockdep_assert_held(&tphy->lock);
	if (!tphy->sleep_back)
		return;
	WRITE_ONCE(tphy->sleep_back, false);
	wake_up_all(&tphy->sleep_wq);
}

static int t6040_usb2_power_off(struct phy *phy)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);
	int ret;

	guard(mutex)(&tphy->lock);
	if (tphy->shutdown_failed)
		return -EIO;
	if (!tphy->powered)
		return 0;

	/* Also the stop of a host whose PHY a sleep left down: the sequence only repeats. */
	ret = t6040_usb2_off_locked(tphy);
	if (ret)
		return ret;
	tphy->powered = false;
	tphy->sleep_down = false;
	t6040_sleep_back_done(tphy);
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
				  uc & T6040_CORE_UC_CTRL_TNBL_DONE, 1, USEC_PER_SEC);
}

static const struct apple_rtkit_ops t6040_cpu_rtkit_ops;

static int t6040_rtkit_start_transport(struct t6040_usb2_phy *tphy)
{
	if (tphy->rtk)
		return 0;

	tphy->rtk = apple_rtkit_init(tphy->dev, tphy, "phy-cpu", 0,
				     &t6040_cpu_rtkit_ops);
	if (IS_ERR(tphy->rtk)) {
		int ret = PTR_ERR(tphy->rtk);

		tphy->rtk = NULL;
		return ret;
	}
	return 0;
}

static void t6040_rtkit_stop_transport(struct t6040_usb2_phy *tphy)
{
	if (!tphy->rtk)
		return;
	apple_rtkit_free(tphy->rtk);
	tphy->rtk = NULL;
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
	int ret;

	ret = t6040_rtkit_start_transport(tphy);
	if (ret)
		return ret;

	if (readl(cpu + T6040_CPU_RUN) & T6040_CPU_RUN_START) {
		dev_err(tphy->dev, "USB3: the PHY coprocessor runs without an RTKit session\n");
		return -EBUSY;
	}
	writel(T6040_CPU_RUN_START, cpu + T6040_CPU_RUN);
	usleep_range(5000, 6000);
	return apple_rtkit_boot(tphy->rtk);
}

/*
 * Part C of the PHY init from the delay after the lane swap on, for the modes
 * that run the PHY firmware (USB3 and CIO): the firmware start, the tune-done
 * poll and the reset release (D). @what and @lane only label the log.
 */
static int t6040_phy_run_firmware(struct t6040_usb2_phy *tphy, const char *what, int lane)
{
	void __iomem *cpu = t6040_cpu(tphy);
	u32 uc_before;
	int ret;

	fsleep(20);
	t6040_usb2_set32(cpu + T6040_CPU_IDLE_CTRL, T6040_CPU_IDLE_CTRL_EN);
	fsleep(10);

	uc_before = readl(t6040_core(tphy) + T6040_CORE_UC_CTRL);
	if (!tphy->rtk || !apple_rtkit_is_running(tphy->rtk)) {
		ret = t6040_usb3_cpu_start(tphy);
		if (ret) {
			dev_err(tphy->dev, "%s: the PHY coprocessor was not started: %d (run %#x, idle %#x, status %#x)\n",
				what, ret, readl(cpu + T6040_CPU_RUN),
				readl(cpu + T6040_CPU_IDLE_CTRL), readl(cpu + T6040_CPU_STATUS));
			return ret;
		}
	}
	ret = t6040_usb3_wait_tuned(tphy);
	if (ret) {
		dev_err(tphy->dev, "%s: the PHY coprocessor did not tune the PHY (uc %#x -> %#x, idle %#x, status %#x)\n",
			what, uc_before, readl(t6040_core(tphy) + T6040_CORE_UC_CTRL),
			readl(cpu + T6040_CPU_IDLE_CTRL), readl(cpu + T6040_CPU_STATUS));
		return ret;
	}

	dev_info(tphy->dev, "%s PHY tuned: lane %d, coprocessor started, uc %#x -> %#x\n", what,
		 lane, uc_before, readl(t6040_core(tphy) + T6040_CORE_UC_CTRL));
	t6040_usb2_set32(t6040_cfg(tphy) + T6040_CFG_CTRL, T6040_CFG_CTRL_PHY_RESET_N);
	fsleep(100);
	return 0;
}

/*
 * C: the firmware start. The coprocessor is started on every attach and stopped on
 * every detach; each start makes the firmware tune the PHY. Then release the
 * PHY reset (D).
 */
static int t6040_usb3_run_firmware(struct t6040_usb2_phy *tphy, bool swap)
{
	t6040_usb2_mask32(t6040_cfg(tphy) + T6040_CFG_SPARE0, T6040_CFG_SPARE0_USB_LN_SWAP,
			  swap ? T6040_CFG_SPARE0_USB_LN_SWAP : 0);
	return t6040_phy_run_firmware(tphy, "USB3", swap);
}

/*
 * Detach (RTKit power state 0): Power Down, RTKit AP then IOP to idle,
 * wait up to 10 ms for the CPU to idle, STOP. The RTKit transport stays stopped
 * so the next attach can either restart it normally or first power-cycle the
 * domains. The mailbox outbox enable that macOS clears here is left as iBoot
 * set it: Linux never clears it.
 */
static void t6040_usb3_firmware_power_down(struct t6040_usb2_phy *tphy)
{
	void __iomem *cpu = t6040_cpu(tphy);
	bool acknowledged = false;
	bool clean_stop = true;
	u32 status;
	int ret;

	reinit_completion(&tphy->app_reply);
	if (tphy->rtk && apple_rtkit_is_running(tphy->rtk)) {
		ret = apple_rtkit_start_ep(tphy->rtk, T6040_CPU_EP_APP);
		if (!ret)
			ret = apple_rtkit_send_message(tphy->rtk, T6040_CPU_EP_APP,
						       T6040_CPU_MSG_POWER_DOWN, NULL, false);
		if (ret)
			dev_warn(tphy->dev, "USB3: Power Down submission failed: %d\n", ret);

		ret = apple_rtkit_idle(tphy->rtk);
		if (ret) {
			dev_warn(tphy->dev, "USB3: PHY coprocessor idle failed: %d\n", ret);
			clean_stop = false;
		}
	}

	if (readl(cpu + T6040_CPU_RUN) & T6040_CPU_RUN_START) {
		if (readl_poll_timeout(cpu + T6040_CPU_STATUS, status,
				       status & T6040_CPU_STATUS_IDLE, 1000, 10000)) {
			dev_warn(tphy->dev,
				 "USB3: the PHY coprocessor did not idle (status %#x)\n",
				 status);
			clean_stop = false;
		}
		writel(~T6040_CPU_RUN_START, cpu + T6040_CPU_RUN);
		if (readl(cpu + T6040_CPU_RUN) & T6040_CPU_RUN_START) {
			dev_warn(tphy->dev, "USB3: the PHY coprocessor STOP did not latch\n");
			clean_stop = false;
		}
	}

	/* Leave the transport stopped until the next firmware start. Free drains RX. */
	t6040_rtkit_stop_transport(tphy);
	/* Power Down is asynchronous; never wait solely for its application reply. */
	acknowledged = completion_done(&tphy->app_reply);
	if (!acknowledged)
		dev_warn(tphy->dev, "USB3: no asynchronous Power Down acknowledge\n");
	t6040_recovery_power_down(&tphy->recovery, acknowledged, clean_stop);
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

static int t6040_common_dp_force_resume(struct t6040_usb2_phy *tphy)
{
	int ret;

	ret = pm_runtime_force_resume(tphy->dev);
	if (ret)
		return ret;
	/* force_resume may preserve an idle device in suspend; this caller needs MMIO now. */
	ret = pm_runtime_resume_and_get(tphy->dev);
	if (ret < 0)
		return ret;
	pm_runtime_put_noidle(tphy->dev);
	return 0;
}

/*
 * The RTKit transport owns one phymxwrap runtime reference while its mailbox
 * is started. apple_rtkit_free() releases it before this function runs. Give
 * an unrelated user or active child a bounded interval to become idle, but do
 * not steal its reference: recovery can still make a clean firmware start
 * without cycling a domain which is legitimately shared.
 *
 * Return 0 after a real suspend, 1 when the caller should use that normal-start
 * fallback, and a negative error for a provider failure unrelated to a holder.
 * @retries is how often to wait for a holder, see t6040_recovery_pm_retries().
 */
static int t6040_usb3_suspend_mbox(struct t6040_usb2_phy *tphy, unsigned int retries)
{
	struct device *dev = tphy->mbox_dev;
	unsigned int attempt;
	int children, ret = -EBUSY, usage;

	for (attempt = 0; attempt <= retries; attempt++) {
		enum t6040_recovery_pm_action action;

		pm_runtime_barrier(dev);
		usage = atomic_read(&dev->power.usage_count);
		children = atomic_read(&dev->power.child_count);
		dev_dbg(tphy->dev,
			"USB3: phymxwrap recovery attempt %u: usage_count=%d child_count=%d\n",
			attempt, usage, children);

		action = t6040_recovery_pm_action(usage, children, attempt, retries);
		if (action == T6040_RECOVERY_PM_SUSPEND) {
			ret = pm_runtime_suspend(dev);
			if (ret >= 0 && pm_runtime_status_suspended(dev))
				return 0;
			if (ret >= 0)
				ret = -EBUSY;
			if (ret != -EAGAIN && ret != -EBUSY && ret != -EACCES)
				return ret;
		} else {
			ret = usage ? -EAGAIN : -EBUSY;
			if (action == T6040_RECOVERY_PM_FALLBACK)
				break;
		}

		if (attempt < retries) {
			unsigned int delay = T6040_RECOVERY_PM_BACKOFF_US << attempt;

			usleep_range(delay, delay + T6040_RECOVERY_PM_BACKOFF_US);
		}
	}

	usage = atomic_read(&dev->power.usage_count);
	children = atomic_read(&dev->power.child_count);
	dev_warn(tphy->dev,
		 "USB3: phymxwrap remains active (usage_count=%d child_count=%d, suspend %d); continuing with normal firmware start\n",
		 usage, children, ret);
	return 1;
}

/*
 * Drop both domains with RTKit fully detached. phymxwrap is the mailbox
 * provider's runtime domain; common_dp is this device's runtime domain. Force
 * common_dp through a balanced suspend/resume because the USB2 PHY and DWC3
 * reset can hold independent runtime references while the SuperSpeed lanes are
 * quiesced.
 */
static int t6040_usb3_hard_recover(struct t6040_usb2_phy *tphy)
{
	unsigned int dwell = max(recovery_dwell_ms, 1U);
	unsigned int retries;
	int ret, resume_ret;

	/* This also releases the mailbox provider reference synchronously. */
	t6040_rtkit_stop_transport(tphy);

	ret = pm_runtime_force_suspend(tphy->dev);
	if (ret < 0)
		return ret;

	/* Left active by that: still in use, and so still the mailbox's consumer. */
	retries = t6040_recovery_pm_retries(!pm_runtime_status_suspended(tphy->dev));
	ret = t6040_usb3_suspend_mbox(tphy, retries);
	if (ret < 0) {
		dev_err(tphy->dev, "USB3: phymxwrap suspend failed: %d\n", ret);
		goto resume_phy;
	}
	if (ret > 0) {
		ret = 0;
		goto resume_phy;
	}

	msleep(dwell);
	ret = t6040_rtkit_start_transport(tphy);
	if (ret) {
		dev_err(tphy->dev, "USB3: RTKit transport restart failed: %d\n", ret);
		goto resume_phy;
	}

	resume_ret = t6040_common_dp_force_resume(tphy);
	if (resume_ret < 0) {
		dev_err(tphy->dev, "USB3: common_dp resume failed: %d\n", resume_ret);
		t6040_rtkit_stop_transport(tphy);
		return resume_ret;
	}

	dev_warn(tphy->dev, "USB3: completed hard recovery with %u ms off dwell\n", dwell);
	return 0;

resume_phy:
	resume_ret = t6040_common_dp_force_resume(tphy);
	if (resume_ret < 0)
		return resume_ret;
	return ret;
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
 * Part A of the PHY shutdown for every firmware mode: both TX lanes
 * powered down, then the common block shut down.
 */
static void t6040_phy_tx_cmn_shutdown(struct t6040_usb2_phy *tphy)
{
	void __iomem *core = t6040_core(tphy);
	unsigned int lane;

	/* The TX power-down of lane 0, then of lane 1 */
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
}

/*
 * Part A of the PHY shutdown: the PHY's USB link side down (the same BIST
 * handshake as the PIPE switch), both TX lanes, then the common block.
 */
static void t6040_usb3_shutdown_lanes(struct t6040_usb2_phy *tphy)
{
	void __iomem *bist = t6040_bist(tphy);

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

	t6040_phy_tx_cmn_shutdown(tphy);
	fsleep(200);	/* the PHY shutdown waits 200 us after part A */
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
static void t6040_usb3_up_locked(struct t6040_usb2_phy *tphy)
{
	bool cycle;
	bool swap;
	int ret;

	lockdep_assert_held(&tphy->lock);
	if (!tphy->usb3_ok || tphy->usb3_up)
		return;
	/* Down for a sleep with the host kept: USB3 comes back with the wake, after USB2. */
	if (tphy->sleep_down) {
		dev_dbg(tphy->dev, "USB3: the PHY is down for a sleep, not started\n");
		return;
	}
	/* DP mode owns all four lanes; USB3 comes back in t6040_usb3_reclaim_locked(). */
	if (tphy->dp_up) {
		dev_dbg(tphy->dev, "USB3: lanes held by DP, USB2 only for now\n");
		return;
	}
	/* CIO owns both lanes; USB3 comes back in t6040_usb3_reclaim_locked() after it. */
	if (tphy->cio_up) {
		dev_dbg(tphy->dev, "USB3: lanes held by CIO, USB2 only for now\n");
		tphy->usb3_lent = true;
		return;
	}

	switch (tphy->orientation) {
	case TYPEC_ORIENTATION_NORMAL:
		swap = false;
		break;
	case TYPEC_ORIENTATION_REVERSE:
		swap = true;
		break;
	default:
		dev_info(tphy->dev, "USB3: no plug orientation, USB2 only\n");
		return;
	}

	/* PIPE_HANDLER_DEFAULT: the dummy PIPE clock the controller keeps using at USB2 */
	apple_tunable_apply(tphy->pipehandler, tphy->tunable_pipehandler);

	cycle = t6040_recovery_start(&tphy->recovery);
retry:
	if (cycle) {
		ret = t6040_usb3_hard_recover(tphy);
		if (ret)
			goto failed;
	}

	ret = t6040_usb3_wake(tphy);
	if (ret)
		goto sleep;	/* the coprocessor block may be unclocked: never touch it */
	t6040_usb3_tunables(tphy);
	t6040_usb3_lanes(tphy, swap);
	ret = t6040_usb3_run_firmware(tphy, swap);
	if (ret)
		goto retry_or_fail;

	t6040_recovery_complete(&tphy->recovery);
	tphy->usb3_up = true;
	dev_dbg(tphy->dev, "USB3 PHY up, lane %d\n", swap);
	return;
retry_or_fail:
	/* A retune is a full detach sequence followed by one complete fresh start. */
	t6040_usb3_shutdown_lanes(tphy);
	t6040_usb3_firmware_power_down(tphy);
	t6040_usb3_sleep(tphy);
	/*
	 * The retry power-cycles the PHY's domains, which don't go off under a system
	 * resume: a wake with the host kept leaves the port at USB2 instead.
	 */
	if (t6040_recovery_retry(&tphy->recovery) && !tphy->sleep_waking) {
		cycle = true;
		goto retry;
	}
	goto failed;

sleep:
	t6040_usb3_sleep(tphy);

failed:
	dev_warn(tphy->dev, "USB3 PHY bring-up failed (%d), USB2 only\n", ret);
}

static void t6040_cio_pipe_update_locked(struct t6040_usb2_phy *tphy);

static int t6040_usb3_power_on(struct phy *phy)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);

	guard(mutex)(&tphy->lock);
	tphy->usb3_on = true;
	t6040_usb3_up_locked(tphy);
	return 0;
}

/* aciophy_phy_shutdown with the lock held: lanes, coprocessor, then the CFG block. */
static void t6040_usb3_down_locked(struct t6040_usb2_phy *tphy)
{
	if (tphy->pipe_switched) {
		t6040_usb3_pipe_park(tphy);
		tphy->pipe_switched = false;
	}
	/* The USB3 mode 0 from 3: the park from the USB4 router uses the same writes. */
	if (tphy->pipe_tunneled) {
		t6040_usb3_pipe_park(tphy);
		tphy->pipe_tunneled = false;
	}
	/* DWC3 can drop its role before the DP fabric has released pin D. */
	if (tphy->dp_up && tphy->dp_mode_two_lanes)
		return;
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
	tphy->usb3_on = false;
	tphy->usb3_pipe = false;
	/* The host stops: what its next start brings up is what a later wake brings back. */
	tphy->sleep_usb3 = false;
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
	tphy->usb3_pipe = true;
	if (tphy->cio_up) {
		t6040_cio_pipe_update_locked(tphy);
		return 0;
	}
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
 * the "cfg" window. Pin assignment D shares USB3's running PHY and uses
 * the two-lane group link sequences below.
 */

/* The fixed DPTX tunable writes (apply_dptx_tunables, shared with USB3). */
static const struct { u32 off, clear, set; } t6040_dptx_tunables[] = {
	{ 0x00800, 0x000003f0, 0x000001c0 },	/* AUS40CMN_DIG RCAL0 */
	{ 0x00818, 0x0000f000, 0x00003000 },	/* AUS40CMN_DIG RCAL5 */
	{ 0x03888, 0x000000ff, 0x00000027 },	/* CIO4PLL FREQDESCRIPTOR_0_C */
	{ 0x28004, 0x00000038, 0x00000008 },	/* AUX_LDO_CTRL */
	{ 0x03adc, 0x00003ff0, 0x00001760 },	/* CIO4PLL CFG_FRACN_FUNC */
	/* The lane entries, T6040_DPTX_LANE_TUNABLES of them, come last. */
	{ 0x0e0cc, 0x01f80000, 0x00380000 },	/* LANE0 TX_EQ_CTRL_REG11 */
	{ 0x1e0cc, 0x01f80000, 0x00380000 },	/* LANE1 TX_EQ_CTRL_REG11 */
	{ 0x0d090, 0x01f80000, 0x00380000 },	/* LANE0 RX_CFG TX_EQ_CTRL_REG11 */
	{ 0x1d090, 0x01f80000, 0x00380000 },	/* LANE1 RX_CFG TX_EQ_CTRL_REG11 */
};

#define T6040_DPTX_LANE_TUNABLES	4

static void t6040_dp_fixed_tunables(void __iomem *core)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(t6040_dptx_tunables); i++)
		t6040_usb2_mask32(core + t6040_dptx_tunables[i].off, t6040_dptx_tunables[i].clear,
				  t6040_dptx_tunables[i].set);
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
#include "atc-t6040-dp2-data.h"

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

/* The ADT's lane0 and lane1 tables in t6040_usb3_tunable_list. */
#define T6040_USB3_TUNABLE_LANE0	4
static_assert(T6040_USB3_TUNABLE_LANE0 + 2 == T6040_DP_NR_GENERIC_TUNABLES);

/*
 * A lane's registers are back at their reset values each time the lane has been powered
 * down: after one link at a rate, one DPMS cycle or one failed training, the lane tables
 * of the ADT and the DPTX lane tunables were gone for every link that followed. Write them
 * again once the link sequence has the lanes up.
 */
static void t6040_dp_lane_tunables(struct t6040_usb2_phy *tphy)
{
	void __iomem *core = t6040_core(tphy);
	size_t i;

	for (i = T6040_USB3_TUNABLE_LANE0; i < T6040_USB3_TUNABLE_LANE0 + 2; i++)
		apple_tunable_apply(core, tphy->tunables[i]);
	for (i = ARRAY_SIZE(t6040_dptx_tunables) - T6040_DPTX_LANE_TUNABLES;
	     i < ARRAY_SIZE(t6040_dptx_tunables); i++)
		t6040_usb2_mask32(core + t6040_dptx_tunables[i].off, t6040_dptx_tunables[i].clear,
				  t6040_dptx_tunables[i].set);
}

/* Take the link down if it is up, then bring it up at rate index @idx (-1: off). */
static int t6040_dp_set_rate(struct t6040_usb2_phy *tphy, int idx)
{
	const struct atc_op *off = dp_link_off;
	const struct atc_rate_op *on = dp_link_on;
	size_t nr_off = ARRAY_SIZE(dp_link_off);
	size_t nr_on = ARRAY_SIZE(dp_link_on);
	size_t i;
	int ret;

	if (tphy->dp_mode_two_lanes) {
		if (tphy->dp_mode_flip) {
			off = dp_group0_off;
			on = dp_group0_on;
			nr_off = ARRAY_SIZE(dp_group0_off);
			nr_on = ARRAY_SIZE(dp_group0_on);
		} else {
			off = dp_group1_off;
			on = dp_group1_on;
			nr_off = ARRAY_SIZE(dp_group1_off);
			nr_on = ARRAY_SIZE(dp_group1_on);
		}
	}
	if (tphy->dp_rate >= 0) {
		for (i = 0; i < nr_off; i++) {
			ret = t6040_dp_seq_step(tphy, &off[i]);
			if (ret)
				return ret;
		}
		tphy->dp_rate = -1;
	}
	if (idx < 0)
		return 0;
	for (i = 0; i < nr_on; i++) {
		struct atc_op op = on[i].op;

		if (!(on[i].rates & BIT(idx)))
			continue;
		op.value = on[i].values[idx];
		ret = t6040_dp_seq_step(tphy, &op);
		if (ret)
			return ret;
	}
	/* Pin D leaves the lanes to USB3's firmware, which keeps them up. */
	if (!tphy->dp_mode_two_lanes)
		t6040_dp_lane_tunables(tphy);
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
	unsigned int group = (lane < 2 ? 1 : 0) ^
			     (tphy->dp_mode_two_lanes && tphy->dp_mode_flip);
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

/*
 * Give the lanes back to USB3 once the DP session is over and the port has
 * left DP alt mode while dwc3 still holds the USB3 phy on: the USB3 bring-up
 * programs the PHY afresh, as it does after DP on the next attach, and the
 * PIPE returns to the PHY if the controller had asked for it. Called when
 * the DP phy powers off and when the mux leaves DP, as either can come last.
 * tipd drops the USB role before it leaves DP alt mode, so on an unplug or a
 * DP mode change dwc3 powers the USB3 phy off first and this does nothing.
 */
static void t6040_usb3_reclaim_locked(struct t6040_usb2_phy *tphy)
{
	lockdep_assert_held(&tphy->lock);
	if (!tphy->usb3_lent || tphy->dp_up || tphy->dp_alt || tphy->cio_up)
		return;
	tphy->usb3_lent = false;
	if (!tphy->usb3_on || tphy->usb3_up)
		return;
	t6040_usb3_up_locked(tphy);
	if (tphy->usb3_up && tphy->usb3_pipe && !tphy->pipe_switched) {
		t6040_usb3_pipe_switch(tphy);
		tphy->pipe_switched = true;
	}
	dev_info(tphy->dev, "USB3 %s after DP\n", tphy->usb3_up ? "back" : "not restored");
}

/* ---- DP phy_ops ---- */

/*
 * A port that is down for a sleep with its host kept, while the system
 * resumes: the Type-C controller can report a display before the host's
 * resume has brought the PHY back, one that came to a dock during the sleep
 * or an adapter in the place of the USB device. DP waits for that wake, and
 * then finds the port as on any running host: USB3 up for pin assignment D
 * to share, or to lend its lanes. If the wake fails or doesn't come, DP goes
 * on and gets what it gets on a port without a host.
 */
static void t6040_dp_wait_host(struct t6040_usb2_phy *tphy)
{
	if (!READ_ONCE(tphy->sleep_back))
		return;
	if (!wait_event_timeout(tphy->sleep_wq, !READ_ONCE(tphy->sleep_back),
				msecs_to_jiffies(T6040_SLEEP_DP_WAIT_MS)))
		dev_warn(tphy->dev, "DP: the host's wake from the sleep did not come in %u ms\n",
			 T6040_SLEEP_DP_WAIT_MS);
}

static int t6040_dp_power_on(struct phy *phy)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);
	int ret;

	t6040_dp_wait_host(tphy);
	guard(mutex)(&tphy->lock);
	if (!tphy->dp_ok || tphy->dp_up)
		return 0;
	/* The port carries USB4/Thunderbolt: DP reaches it only as a tunnel. */
	if (tphy->cio_up)
		return -EBUSY;
	/* Pin D shares the running PHY with USB3. Keep its PIPE and firmware. */
	if (tphy->dp_two_lanes) {
		if (!tphy->usb3_ok || !tphy->dp_alt)
			return -ENOLINK;
		if (!tphy->usb3_up)
			t6040_usb3_up_locked(tphy);
		if (!tphy->usb3_up) {
			if (tphy->sleep_down)
				dev_info(tphy->dev,
					 "DP PHY pin D: the port is down for a sleep, no link\n");
			return -ENOLINK;
		}
		/* USB3 startup already applied these DPTX tunables before firmware. */
		t6040_dp_aux_init(tphy);
		tphy->dp_rate = -1;
		tphy->dp_mode_two_lanes = true;
		tphy->dp_mode_flip = tphy->dp_flip;
		tphy->dp_up = true;
		dev_info(tphy->dev, "DP PHY pin D: two lanes, USB3 retained, %s orientation\n",
			 tphy->dp_flip ? "reverse" : "normal");
		return 0;
	}
	/* C/E and the fixed HDMI PHY hand all four lanes to DP. */
	if (!t6040_dp_only(tphy)) {
		t6040_usb3_down_locked(tphy);
		tphy->usb3_lent = true;
	}
	ret = t6040_dp_phy_init(tphy, tphy->dp_flip);
	if (ret) {
		dev_err(tphy->dev, "DP PHY init failed: %d\n", ret);
		return ret;
	}
	t6040_dp_mode_setup(tphy, tphy->dp_flip);
	tphy->dp_rate = -1;
	tphy->dp_mode_two_lanes = false;
	tphy->dp_up = true;
	dev_dbg(tphy->dev, "DP PHY mode set, lane map %s\n",
		tphy->dp_flip ? "reversed" : "normal");
	return 0;
}

/*
 * The PHY's close for DP on all four lanes (with a DP client: the DP
 * transmitter's shutdown, the AUX shutdown, the PHY shutdown of mode 3): a link still up goes
 * down, then both TX lanes and the common block (part A), and after its 200 us the CFG
 * block returns to its boot state (part B; no coprocessor to stop in this mode). The AUX
 * block has no step of its own here: it goes into reset and behind the clamp with the PHY.
 *
 * Without this the fixed DP PHY of the HDMI converter stays as it ran once it is powered
 * off; a Type-C port's was put to sleep, with no step for a link still up. The sleep engine
 * of an S2R stalled on a port left as it ran, and the next power-on programmed a PHY that
 * had not been through its reset.
 */
static void t6040_dp_shutdown(struct t6040_usb2_phy *tphy)
{
	int ret = t6040_dp_set_rate(tphy, -1);

	if (ret)
		dev_warn(tphy->dev, "DP link not taken down before the PHY shutdown: %d\n", ret);
	tphy->dp_rate = -1;
	t6040_phy_tx_cmn_shutdown(tphy);
	fsleep(200);
	t6040_usb3_sleep(tphy);
	dev_dbg(tphy->dev, "DP PHY shut down: cfg %#x/%#x\n",
		readl(t6040_cfg(tphy) + T6040_CFG_CTRL), readl(t6040_cfg(tphy) + T6040_CFG_STATUS));
}

static int t6040_dp_power_off(struct phy *phy)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);
	int ret = 0;

	guard(mutex)(&tphy->lock);
	if (tphy->dp_up && tphy->dp_mode_two_lanes) {
		ret = t6040_dp_set_rate(tphy, -1);
		if (ret)
			return ret;
	}
	/*
	 * DP on all four lanes held the common block and the TX lanes on by
	 * override, with no coprocessor. Release them and put the PHY to sleep,
	 * as the CIO teardown does: the next USB3 bring-up then starts from a
	 * sleeping PHY, as on a fresh attach. Booting the coprocessor's
	 * firmware for the first time over the DP state crashes it.
	 */
	if (tphy->dp_up && !tphy->dp_mode_two_lanes) {
		t6040_dp_shutdown(tphy);
		dev_info(tphy->dev, "DP PHY down, PHY asleep\n");
	}
	tphy->dp_up = false;
	if (tphy->dp_mode_two_lanes && !tphy->usb3_on)
		t6040_usb3_down_locked(tphy);
	tphy->dp_mode_two_lanes = false;
	t6040_usb3_reclaim_locked(tphy);
	return ret;
}

/* GET_MAX_LANE_COUNT follows the negotiated pin assignment. */
static int t6040_dp_validate(struct phy *phy, enum phy_mode mode, int submode,
			     union phy_configure_opts *opts_)
{
	struct t6040_usb2_phy *tphy = phy_get_drvdata(phy);
	struct phy_configure_opts_dp *opts = &opts_->dp;

	if (mode != PHY_MODE_DP)
		return -EINVAL;
	guard(mutex)(&tphy->lock);
	opts->lanes = tphy->dp_two_lanes ? 2 : 4;
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
	if ((opts->set_lanes || opts->set_voltages) &&
	    opts->lanes > (tphy->dp_mode_two_lanes ? 2 : 4))
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

		for (i = 0; i < opts->lanes; i++) {
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

/* ---- DP tunnels: the pixel clocks of the router's DP IN adapters (PHY in CIO) ----
 *
 * The DP 1.4 tunnel's init, update and shutdown and the DP clock
 * configuration, as the platform sequences them: one fixed
 * AUSPLL frequency feeds the DPTX pixel clocks, whose select field divides it
 * per link rate. DP IN adapter 0 is taken to use PCLK1 and clock switch 1,
 * adapter 1 PCLK2 and switch 2 (the PHY does not choose; the platform gets the index
 * from the display controller). Driven only by appledrm through
 * apple_atc_dp_tunnel_rate(), and only while the PHY is in CIO.
 */
#define T6040_CORE_RCAL1		0x804	/* AUS40CMN_DIG RCAL1 */
#define T6040_CORE_RCAL1_DONE		BIT(0)
#define T6040_CORE_AUSPLL_CMD		0x3000	/* AUSPLL_TOP_APB_CMD_CMDOVERRIDE */
#define T6040_CORE_AUSPLL_CMD_REQ	BIT(0)
#define T6040_CORE_AUSPLL_CMD_ACK	BIT(1)
#define T6040_CORE_AUSPLL_FREQDESC_A	0x3080
#define T6040_CORE_AUSPLL_FREQDESC_B	0x3084
#define T6040_CORE_AUSPLL_FREQDESC_C	0x3088
#define T6040_CORE_AUSPLL_CLKOUT_MASTER	0x3200
#define T6040_CORE_AUSPLL_CLKOUT_DIV	0x3208
#define T6040_CORE_AUSPLL_BGR		0x3218
#define T6040_CORE_AUSPLL_FREQINIT_1	0x3234
#define T6040_CORE_TX_DP_CTRL0		0x7000
#define T6040_CORE_TX_DP_CTRL0_ENABLES	GENMASK(18, 16)
#define T6040_CORE_DP_PCLK_STATUS	0x7034
#define T6040_CORE_DP_PCLK_STATUS_LOCK	BIT(3)	/* AUSPLL_LOCK */

static const struct t6040_tun_pclk {
	u8 sel_shift;		/* TX_DP_CTRL0 select field */
	u32 en;			/* TX_DP_CTRL0 enable */
	u32 sw_mask, sw_gate, sw_on;	/* clock switch: cleared, set, status when on */
	u32 req[2], req_stat[2], sw_off;	/* PMGR requests, their status, cleared at off */
} t6040_tun_pclk[2] = {
	{ 4, BIT(16), 0x473, 0x10, BIT(0), { 0x20, 0x40 }, { BIT(1), BIT(2) }, 0x403 },
	{ 8, BIT(17), 0xb8c, 0x80, BIT(3), { 0x100, 0x200 }, { BIT(4), BIT(5) }, 0x80c },
};

/* The pixel clock select: RBR, HBR, HBR2, HBR3; UHBR is not tunnelled on this path. */
static int t6040_tun_select(u8 rate)
{
	switch (rate) {
	case 0x06:
		return 4;
	case 0x0a:
		return 3;
	case 0x14:
		return 1;
	case 0x1e:
		return 0;
	default:
		return -EINVAL;
	}
}

static u32 t6040_tun_enables(struct t6040_usb2_phy *tphy)
{
	return (tphy->tun_rate[0] ? t6040_tun_pclk[0].en : 0) |
	       (tphy->tun_rate[1] ? t6040_tun_pclk[1].en : 0);
}

/* AUSPLL_TOP command override: request @req with the toggle protocol, then wait for @ack. */
static int t6040_tun_pll_cmd(struct t6040_usb2_phy *tphy, u32 val, bool ack_set)
{
	void __iomem *cmd = t6040_core(tphy) + T6040_CORE_AUSPLL_CMD;
	u32 v;

	writel((readl(cmd) & 0xe0000006) | val, cmd);
	return readl_poll_timeout(cmd, v, !!(v & T6040_CORE_AUSPLL_CMD_ACK) == ack_set,
				  10, 500 * USEC_PER_MSEC);
}

/* The DP 1.4 tunnel shutdown: pixel clocks off, AUSPLL released. */
static void t6040_tun_pll_stop(struct t6040_usb2_phy *tphy)
{
	void __iomem *core = t6040_core(tphy);
	bool ack = readl(core + T6040_CORE_AUSPLL_CMD) & T6040_CORE_AUSPLL_CMD_ACK;
	u32 v;

	t6040_usb2_clear32(core + T6040_CORE_TX_DP_CTRL0, T6040_CORE_TX_DP_CTRL0_ENABLES);
	t6040_usb2_clear32(core + T6040_CORE_TX_DP_CTRL0, 0xc);
	if (t6040_tun_pll_cmd(tphy, (ack ? 0 : T6040_CORE_AUSPLL_CMD_REQ) | 0x10000000, !ack) ||
	    t6040_tun_pll_cmd(tphy, (ack ? T6040_CORE_AUSPLL_CMD_REQ : 0) | 0x10000018, ack) ||
	    readl_poll_timeout(core + T6040_CORE_DP_PCLK_STATUS, v,
			       !(v & T6040_CORE_DP_PCLK_STATUS_LOCK), 10, 500 * USEC_PER_MSEC))
		dev_warn(tphy->dev, "DP tunnel: AUSPLL release timed out (cmd %#x, pclk status %#x)\n",
			 readl(core + T6040_CORE_AUSPLL_CMD),
			 readl(core + T6040_CORE_DP_PCLK_STATUS));
	t6040_usb2_clear32(core + T6040_CORE_AUSPLL_CLKOUT_MASTER, 0x8);
	tphy->tun_pll = false;
}

/* The common block's init, then the fixed AUSPLL program of the DP 1.4 tunnel init. */
static int t6040_tun_pll_start(struct t6040_usb2_phy *tphy)
{
	void __iomem *core = t6040_core(tphy);
	bool ack;
	u32 v;

	if ((readl(core + T6040_CORE_AUSPLL_CLKOUT_MASTER) & 0x808) ||
	    (readl(core + T6040_CORE_AUSPLL_CMD) & T6040_CORE_AUSPLL_CMD_REQ)) {
		dev_warn(tphy->dev, "DP tunnel: AUSPLL already in use (master %#x, cmd %#x)\n",
			 readl(core + T6040_CORE_AUSPLL_CLKOUT_MASTER),
			 readl(core + T6040_CORE_AUSPLL_CMD));
		return -EBUSY;
	}
	t6040_usb2_clear32(core + T6040_CORE_TX_DP_CTRL0, T6040_CORE_TX_DP_CTRL0_ENABLES);

	t6040_usb2_set32(core + T6040_CORE_CMN_OVRD, 0x4);
	t6040_usb2_set32(core + T6040_CORE_CMN_OVRD, 0x8);
	udelay(1);
	t6040_usb2_set32(core + T6040_CORE_CMN_OVRD, 0x1);
	t6040_usb2_set32(core + T6040_CORE_CMN_OVRD, 0x2);
	udelay(1);
	t6040_usb2_clear32(core + T6040_CORE_CMN_OVRD, T6040_CORE_CMN_OVRD_CLAMP);
	t6040_usb2_set32(core + T6040_CORE_CMN_OVRD, 0x20);
	udelay(1);
	t6040_usb2_set32(core + T6040_CORE_CMN_BIAS, T6040_CORE_CMN_BIAS_BGBIAS_EN_OV);
	udelay(15);
	if (readl_poll_timeout(core + T6040_CORE_RCAL1, v, v & T6040_CORE_RCAL1_DONE, 1, 100))
		dev_dbg(tphy->dev, "DP tunnel: RCAL not done (%#x)\n", v);

	t6040_usb2_clear32(core + T6040_CORE_AUSPLL_FREQINIT_1, 0x3);
	writel(0x1e0e021c, core + T6040_CORE_AUSPLL_FREQDESC_A);
	t6040_usb2_mask32(core + T6040_CORE_AUSPLL_FREQDESC_B, 0x0fffffff, 0);
	t6040_usb2_mask32(core + T6040_CORE_AUSPLL_FREQDESC_C, 0x007fffff, 0x644a00);
	t6040_usb2_mask32(core + T6040_CORE_AUSPLL_CLKOUT_DIV, 0x001f0000, 0x10000);
	t6040_usb2_set32(core + T6040_CORE_AUSPLL_BGR, 0x1);
	t6040_usb2_set32(core + T6040_CORE_AUSPLL_CLKOUT_MASTER, 0x808);
	tphy->tun_pll = true;

	ack = readl(core + T6040_CORE_AUSPLL_CMD) & T6040_CORE_AUSPLL_CMD_ACK;
	if (t6040_tun_pll_cmd(tphy, (ack ? 0 : T6040_CORE_AUSPLL_CMD_REQ) | 0x10000000, true) ||
	    readl_poll_timeout(core + T6040_CORE_DP_PCLK_STATUS, v,
			       !!(v & T6040_CORE_DP_PCLK_STATUS_LOCK) == !ack,
			       10, 500 * USEC_PER_MSEC) ||
	    t6040_tun_pll_cmd(tphy, (ack ? T6040_CORE_AUSPLL_CMD_REQ : 0) | 0x10010000, ack)) {
		dev_err(tphy->dev, "DP tunnel: AUSPLL did not lock (cmd %#x, pclk status %#x)\n",
			readl(core + T6040_CORE_AUSPLL_CMD),
			readl(core + T6040_CORE_DP_PCLK_STATUS));
		t6040_tun_pll_stop(tphy);
		return -ETIMEDOUT;
	}
	t6040_usb2_set32(core + T6040_CORE_TX_DP_CTRL0, 0xc);
	return 0;
}

/* The DP 1.4 tunnel update for one pclk: select with its enable off, then the enables. */
static void t6040_tun_select_write(struct t6040_usb2_phy *tphy, unsigned int dpin, int sel)
{
	const struct t6040_tun_pclk *p = &t6040_tun_pclk[dpin];
	void __iomem *ctrl0 = t6040_core(tphy) + T6040_CORE_TX_DP_CTRL0;
	u32 v = readl(ctrl0);

	if (!!(v & p->en) != !!tphy->tun_rate[dpin] ||
	    ((v >> p->sel_shift) & 0xf) != sel) {
		v = (v & ~(p->en | (0xf << p->sel_shift))) | (sel << p->sel_shift);
		writel(v, ctrl0);
	}
	writel((v & ~T6040_CORE_TX_DP_CTRL0_ENABLES) | t6040_tun_enables(tphy), ctrl0);
}

/* The DP clock configuration for one DP 1.x switch: pixel-clock gate on, or off. */
static int t6040_tun_switch(struct t6040_usb2_phy *tphy, unsigned int dpin, bool on)
{
	const struct t6040_tun_pclk *p = &t6040_tun_pclk[dpin];
	void __iomem *cfg = t6040_cfg(tphy);
	unsigned int i;
	u32 s;

	if (on) {
		t6040_usb2_mask32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, p->sw_mask, p->sw_gate);
		return readl_poll_timeout(cfg + T6040_CFG_DP_CLKSWITCH_STAT, s, s & p->sw_on,
					  10, 5 * USEC_PER_MSEC);
	}
	t6040_usb2_set32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, p->sw_gate);
	for (i = 0; i < 2; i++) {
		if (!(readl(cfg + T6040_CFG_DP_CLKSWITCH_CTRL) & p->req[i]))
			continue;
		t6040_usb2_clear32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, p->req[i]);
		if (readl_poll_timeout(cfg + T6040_CFG_DP_CLKSWITCH_STAT, s, !(s & p->req_stat[i]),
				       10, 5 * USEC_PER_MSEC))
			dev_dbg(tphy->dev, "DP tunnel %u: request %#x not released (%#x)\n",
				dpin, p->req[i], s);
	}
	t6040_usb2_clear32(cfg + T6040_CFG_DP_CLKSWITCH_CTRL, p->sw_off);
	return 0;
}

/* All tunnel clocks down, as the lane configuration does before the ACIO shutdown. */
static void t6040_tun_stop_all_locked(struct t6040_usb2_phy *tphy)
{
	unsigned int dpin;

	lockdep_assert_held(&tphy->lock);
	if (!tphy->tun_pll)
		return;
	for (dpin = 0; dpin < 2; dpin++) {
		if (!tphy->tun_rate[dpin])
			continue;
		tphy->tun_rate[dpin] = 0;
		t6040_tun_switch(tphy, dpin, false);
	}
	t6040_tun_pll_stop(tphy);
	dev_info(tphy->dev, "DP tunnel clocks off\n");
}

static int t6040_tun_rate_locked(struct t6040_usb2_phy *tphy, unsigned int dpin, u8 rate)
{
	int sel, ret;
	u8 old = tphy->tun_rate[dpin];

	lockdep_assert_held(&tphy->lock);
	if (!rate) {
		if (!old)
			return 0;
		tphy->tun_rate[dpin] = 0;
		t6040_tun_switch(tphy, dpin, false);
		t6040_usb2_clear32(t6040_core(tphy) + T6040_CORE_TX_DP_CTRL0,
				   t6040_tun_pclk[dpin].en);
		if (!tphy->tun_rate[!dpin])
			t6040_tun_pll_stop(tphy);
		dev_dbg(tphy->dev, "DP tunnel %u off\n", dpin);
		return 0;
	}
	sel = t6040_tun_select(rate);
	if (sel < 0)
		return sel;
	if (!tphy->cio_up)
		return -EBUSY;
	if (old == rate)
		return 0;
	if (!tphy->tun_pll) {
		ret = t6040_tun_pll_start(tphy);
		if (ret)
			return ret;
	}
	tphy->tun_rate[dpin] = rate;
	t6040_tun_select_write(tphy, dpin, sel);
	ret = t6040_tun_switch(tphy, dpin, true);
	if (ret) {
		dev_err(tphy->dev, "DP tunnel %u: clock switch timed out (ctrl %#x status %#x)\n",
			dpin, readl(t6040_cfg(tphy) + T6040_CFG_DP_CLKSWITCH_CTRL),
			readl(t6040_cfg(tphy) + T6040_CFG_DP_CLKSWITCH_STAT));
		t6040_tun_rate_locked(tphy, dpin, 0);
		return ret;
	}
	dev_info(tphy->dev, "DP tunnel %u: rate %#x, select %d (ctrl0 %#x, pclk status %#x)\n",
		 dpin, rate, sel, readl(t6040_core(tphy) + T6040_CORE_TX_DP_CTRL0),
		 readl(t6040_core(tphy) + T6040_CORE_DP_PCLK_STATUS));
	return 0;
}

/*
 * The dp-tunnel.h provider for appledrm, which looks it up with symbol_get().
 * phy-apple-atc exports the same names for the older SoCs; a kernel that
 * builds both gets them from there (and T6040 tunnels then need appledrm to
 * pick a T6040 provider instead).
 */
#if !IS_ENABLED(CONFIG_PHY_APPLE_ATC)
int apple_atc_dp_tunnel_rate(struct phy *phy, unsigned int dpin, u8 rate)
{
	struct t6040_usb2_phy *tphy;

	if (!phy || phy->ops != &t6040_dp_phy_ops || dpin > 1)
		return -EINVAL;
	tphy = phy_get_drvdata(phy);
	guard(mutex)(&tphy->lock);
	return t6040_tun_rate_locked(tphy, dpin, rate);
}
EXPORT_SYMBOL_GPL(apple_atc_dp_tunnel_rate);

/* The rate call does all of it, as on every SoC but t600x. */
int apple_atc_dp_tunnel_open(struct phy *phy)
{
	if (!phy || phy->ops != &t6040_dp_phy_ops)
		return -EINVAL;
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(apple_atc_dp_tunnel_open);
#endif

/* ---- CIO: USB4/Thunderbolt (PHY mode 2) ----
 *
 * The PHY init for mode 2 with the orientation: the power-up reset (shared
 * with USB3), the generic ADT tables, the CIO core tunables (the same
 * three core tables as USB3) and the CIO-only tunables (the CIO lane
 * tables, applied in every CIO session: nothing on the PHY side picks 40G or
 * 80G), the mode-2 crossbar, the mode-2 sleep-control init (common and both
 * lanes, receivers powered by override), then part C: the PHY coprocessor
 * firmware tunes the PHY as for USB3, without the USB lane-swap bit. The
 * firmware's only message, Power Down, is sent at exit as for
 * USB3. The Type-C controller sets the mux to USB4/TBT after the orientation
 * and before it hands the cable to the host router (drivers/thunderbolt/apple.c),
 * and back to SAFE after the router has stopped; the PHY holds a runtime PM
 * reference in between. CIO is offered only with the CIO tables in the DT;
 * without them every mux state takes the path it took before.
 *
 * USB3 runs tunnelled through the router: the controller's PIPE goes to the
 * USB4 backend (the USB3 mode 3, see t6040_cio_pipe_tunnel)
 * while the PHY is in CIO and the router has asked for it.
 */

/* The ADT tables of the CIO-only tunables, in order; lane0/1 are required. */
static const char * const t6040_cio_tunable_props[] = {
	"apple,tunable-cio-uc-regs",	/* tunable_UC_REGS_CIO_DFLT, core + 0x400 */
	"apple,tunable-cio-lane0",	/* tunable_LN0_RX_TOP_CIO_DFLT, core + 0x9000 */
	"apple,tunable-cio-lane1",	/* tunable_LN1_RX_TOP_CIO_DFLT, core + 0x19000 */
	"apple,tunable-cio-lane0-eq",	/* tunable_LN0_RX_EQ_CIO_DFLT, core + 0xb000 */
	"apple,tunable-cio-lane1-eq",	/* tunable_LN1_RX_EQ_CIO_DFLT, core + 0x1b000 */
};

static_assert(ARRAY_SIZE(t6040_cio_tunable_props) ==
	      ARRAY_SIZE(((struct t6040_usb2_phy *)NULL)->cio_tunables));

/* The PIPE backend of USB3 tunnelled through the USB4 router */
#define T6040_PIPEHANDLER_MUX_CLK_USB4		2
#define T6040_PIPEHANDLER_MUX_DATA_USB4		1

/* The tunables of mode 2: generic tables, the CIO core tunables, the CIO-only tunables. */
static void t6040_cio_tunables(struct t6040_usb2_phy *tphy)
{
	size_t i;

	for (i = 0; i < T6040_USB3_NR_TUNABLES; i++)
		apple_tunable_apply(tphy->regs[t6040_usb3_tunable_list[i].region],
				    tphy->tunables[i]);
	for (i = 0; i < ARRAY_SIZE(tphy->cio_tunables); i++)
		apple_tunable_apply(t6040_core(tphy), tphy->cio_tunables[i]);
}

/* The mode-2 crossbar: lane fields of core+0x6c cleared, core+0x78[4:0] = flip, then [1:0] = 2. */
static void t6040_cio_xbar_config(struct t6040_usb2_phy *tphy, bool flip)
{
	void __iomem *core = t6040_core(tphy);

	t6040_usb2_mask32(core + T6040_CORE_XBAR, ~0xffffff03u, 0);
	t6040_usb2_mask32(core + T6040_CORE_XBAR_LANES, 0x1f, flip ? 1 : 0);
	t6040_usb2_mask32(core + T6040_CORE_XBAR, 0x3, 0x2);
}

struct t6040_cio_step {
	u16 off;
	u32 bit;	/* lane 0; lane 1 is the next bit up */
	bool set;
};

static void t6040_cio_steps(struct t6040_usb2_phy *tphy, const struct t6040_cio_step *step,
			    size_t n, unsigned int lane)
{
	size_t i;

	for (i = 0; i < n; i++) {
		t6040_usb2_mask32(t6040_core(tphy) + step[i].off, step[i].bit << lane,
				  step[i].set ? step[i].bit << lane : 0);
		udelay(1);
	}
}

/* RX power-up, per lane: the lane's receiver powered by override (CFG1_TOP, CFG0_TOP). */
static const struct t6040_cio_step t6040_cio_pwr_up_rxa[] = {
	{ T6040_CORE_CFG1_TOP, 0x10, true }, { T6040_CORE_CFG1_TOP, 0x40, true },
	{ T6040_CORE_CFG1_TOP, 0x1, true }, { T6040_CORE_CFG1_TOP, 0x4, true },
	{ T6040_CORE_CMN_OVRD, 0x400, true }, { T6040_CORE_CMN_OVRD, 0x1000, true },
	{ T6040_CORE_CMN_OVRD, 0x40, true }, { T6040_CORE_CMN_OVRD, 0x100, true },
	{ T6040_CORE_CMN_OVRD, 0x4000, false }, { T6040_CORE_CMN_OVRD, 0x10000, true },
};

/* TX override removal, per lane: the transmitter's overrides released (CFG_SLP_CTRL). */
static const struct t6040_cio_step t6040_cio_tx_rm_ovrd[] = {
	{ T6040_CORE_SLP_CTRL, 0x40, false }, { T6040_CORE_SLP_CTRL, 0x4, false },
	{ T6040_CORE_SLP_CTRL, 0x400, false },
};

/* RX power-down, per lane */
static const struct t6040_cio_step t6040_cio_rx_powerdown[] = {
	{ T6040_CORE_CMN_OVRD, 0x4000, true }, { T6040_CORE_CMN_OVRD, 0x40, false },
	{ T6040_CORE_CMN_OVRD, 0x400, false }, { T6040_CORE_CFG1_TOP, 0x1, false },
	{ T6040_CORE_CFG1_TOP, 0x10, false },
};

/*
 * The mode-2 sleep-control init: the common block's overrides removed, then
 * the RX power-up and the TX override removal per lane.
 */
static void t6040_cio_slpctrl(struct t6040_usb2_phy *tphy)
{
	unsigned int lane;

	t6040_usb2_clear32(t6040_core(tphy) + T6040_CORE_CMN_OVRD, 0x2a);
	udelay(1);
	for (lane = 0; lane < 2; lane++) {
		t6040_cio_steps(tphy, t6040_cio_pwr_up_rxa, ARRAY_SIZE(t6040_cio_pwr_up_rxa), lane);
		t6040_cio_steps(tphy, t6040_cio_tx_rm_ovrd, ARRAY_SIZE(t6040_cio_tx_rm_ovrd), lane);
	}
}

/*
 * The USB3 mode 3 from 0: the PIPE from the dummy backend to the USB4
 * router, with the lock handshake only while the controller is out of reset.
 * No non-selected override and no BIST bracket, unlike the PHY's own USB3.
 */
static void t6040_cio_pipe_tunnel(struct t6040_usb2_phy *tphy)
{
	void __iomem *ph = tphy->pipehandler;
	bool running = readl(ph + T6040_PIPEHANDLER_AON_GEN) &
		       T6040_PIPEHANDLER_AON_GEN_DWC3_RESET_N;

	if (readl(ph + T6040_PIPEHANDLER_LOCK_ACK) & T6040_PIPEHANDLER_LOCK_EN)
		dev_dbg(tphy->dev, "CIO: PIPE lock already acknowledged\n");
	t6040_usb2_clear32(ph + T6040_PIPEHANDLER_OVERRIDE_VALUES, 0xe);
	t6040_usb2_set32(ph + T6040_PIPEHANDLER_OVERRIDE, T6040_PIPEHANDLER_OVERRIDE_EN);
	if (running && t6040_usb3_pipe_lock(tphy, 6))
		dev_dbg(tphy->dev, "CIO: PIPE lock not acknowledged, switching anyway\n");
	t6040_usb3_pipe_mux(tphy, T6040_PIPEHANDLER_MUX_CLK_USB4,
			    T6040_PIPEHANDLER_MUX_DATA_USB4);
	t6040_usb2_clear32(ph + T6040_PIPEHANDLER_OVERRIDE, T6040_PIPEHANDLER_OVERRIDE_EN);
	t6040_usb2_clear32(ph + T6040_PIPEHANDLER_OVERRIDE, T6040_PIPEHANDLER_OVERRIDE_EN2);
	if (running)
		t6040_usb3_pipe_unlock(tphy);
	tphy->pipe_tunneled = true;
	dev_info(tphy->dev, "USB3 PIPE on the USB4 router\n");
}

/*
 * The PIPE is on the router while the router runs and carries USB3 for the
 * controller (the platform asks for USB3 mode 3 only for a tunnelled USB3
 * transport, once it uses the tunnel), dwc3 holds the USB3 phy with its PIPE
 * and the controller is out of reset. Otherwise it goes back to the dummy.
 */
static void t6040_cio_pipe_update_locked(struct t6040_usb2_phy *tphy)
{
	bool want;

	lockdep_assert_held(&tphy->lock);
	want = tphy->cio_up && tphy->router_running && tphy->usb3_tunnel_req &&
	       tphy->usb3_on && tphy->usb3_pipe && tphy->dwc3_running && !tphy->pipe_switched;
	if (want && !tphy->pipe_tunneled) {
		t6040_cio_pipe_tunnel(tphy);
	} else if (!want && tphy->pipe_tunneled) {
		t6040_usb3_pipe_park(tphy);
		tphy->pipe_tunneled = false;
		dev_info(tphy->dev, "USB3 PIPE back on the dummy PHY\n");
	}
}

/* The PHY shutdown of mode 2: part A, the coprocessor down, part B. */
static void t6040_cio_down_locked(struct t6040_usb2_phy *tphy)
{
	u32 uc;
	unsigned int lane;

	lockdep_assert_held(&tphy->lock);
	if (!tphy->cio_up)
		return;
	t6040_tun_stop_all_locked(tphy);
	/* The USB3 mode 0 from 3: the same writes as the park from 2 */
	if (tphy->pipe_tunneled) {
		t6040_usb3_pipe_park(tphy);
		tphy->pipe_tunneled = false;
	}
	uc = readl(t6040_core(tphy) + T6040_CORE_UC_CTRL);
	t6040_phy_tx_cmn_shutdown(tphy);
	for (lane = 0; lane < 2; lane++)
		t6040_cio_steps(tphy, t6040_cio_rx_powerdown, ARRAY_SIZE(t6040_cio_rx_powerdown),
				lane);
	fsleep(200);
	t6040_usb3_firmware_power_down(tphy);
	t6040_usb3_sleep(tphy);
	dev_info(tphy->dev, "CIO PHY asleep: uc %#x -> %#x, cfg %#x/%#x\n", uc,
		 readl(t6040_core(tphy) + T6040_CORE_UC_CTRL),
		 readl(t6040_cfg(tphy) + T6040_CFG_CTRL),
		 readl(t6040_cfg(tphy) + T6040_CFG_STATUS));
	tphy->cio_up = false;
	pm_runtime_put(tphy->dev);
}

/*
 * Bring the PHY into CIO for the plug orientation. A failure is returned to
 * the Type-C controller, which then does not hand the cable to the router.
 */
static int t6040_cio_up_locked(struct t6040_usb2_phy *tphy)
{
	void __iomem *core = t6040_core(tphy);
	unsigned int lane;
	bool cycle;
	bool flip;
	int ret;

	lockdep_assert_held(&tphy->lock);
	if (tphy->cio_up)
		return 0;

	switch (tphy->orientation) {
	case TYPEC_ORIENTATION_NORMAL:
		flip = false;
		break;
	case TYPEC_ORIENTATION_REVERSE:
		flip = true;
		break;
	default:
		dev_warn(tphy->dev, "CIO: no plug orientation\n");
		return -ENODEV;
	}
	if (tphy->dp_up) {
		dev_warn(tphy->dev, "CIO: lanes held by DP\n");
		return -EBUSY;
	}
	/* USB3 hands its lanes over and comes back once the port leaves CIO. */
	if (tphy->usb3_up || tphy->pipe_switched) {
		t6040_usb3_down_locked(tphy);
		tphy->usb3_lent = true;
	}

	ret = pm_runtime_resume_and_get(tphy->dev);
	if (ret) {
		t6040_usb3_reclaim_locked(tphy);
		return ret;
	}

	/* The coprocessor's recovery, as in t6040_usb3_up_locked(): it is the same firmware. */
	cycle = t6040_recovery_start(&tphy->recovery);
retry:
	if (cycle) {
		ret = t6040_usb3_hard_recover(tphy);
		if (ret)
			goto put;
	}

	ret = t6040_usb3_wake(tphy);
	if (ret)
		goto sleep;	/* the coprocessor block may be unclocked: never touch it */
	t6040_cio_tunables(tphy);
	t6040_cio_xbar_config(tphy, flip);
	t6040_cio_slpctrl(tphy);
	ret = t6040_phy_run_firmware(tphy, "CIO", flip);
	if (ret)
		goto retry_or_fail;

	t6040_recovery_complete(&tphy->recovery);
	tphy->cio_up = true;
	tphy->cio_flip = flip;
	dev_info(tphy->dev, "CIO PHY up (mode 2), %s orientation: xbar %#x/%#x cfg %#x/%#x\n",
		 flip ? "reverse" : "normal", readl(core + T6040_CORE_XBAR),
		 readl(core + T6040_CORE_XBAR_LANES), readl(t6040_cfg(tphy) + T6040_CFG_CTRL),
		 readl(t6040_cfg(tphy) + T6040_CFG_STATUS));
	t6040_cio_pipe_update_locked(tphy);
	return 0;

retry_or_fail:
	/*
	 * The lanes are powered by now: as t6040_cio_down_locked(), both TX lanes
	 * and the common block down, then the receivers, before the coprocessor
	 * stops and the CFG block goes back to its boot state. That is the full
	 * teardown, and one complete fresh start may follow it.
	 */
	t6040_phy_tx_cmn_shutdown(tphy);
	for (lane = 0; lane < 2; lane++)
		t6040_cio_steps(tphy, t6040_cio_rx_powerdown, ARRAY_SIZE(t6040_cio_rx_powerdown),
				lane);
	fsleep(200);
	t6040_usb3_firmware_power_down(tphy);
	t6040_usb3_sleep(tphy);
	if (t6040_recovery_retry(&tphy->recovery)) {
		cycle = true;
		goto retry;
	}
	goto put;

sleep:
	t6040_usb3_sleep(tphy);
put:
	pm_runtime_put(tphy->dev);
	dev_warn(tphy->dev, "CIO PHY bring-up failed: %d\n", ret);
	/* The port stays out of CIO: give the lanes back to USB3. */
	t6040_usb3_reclaim_locked(tphy);
	return ret;
}

/*
 * A USB4/Thunderbolt mux event only takes the port into CIO while the router
 * that runs over this PHY (a node whose apple,atc-phy points here) is enabled
 * in the live device tree. The default device trees keep the routers
 * disabled, so the port stays on its USB2/USB3/DP paths until a router is
 * enabled.
 */
static bool t6040_cio_router_enabled(struct t6040_usb2_phy *tphy)
{
	struct device_node *np;

	for_each_compatible_node(np, NULL, "apple,t6040-usb4-acio") {
		struct device_node *phy = of_parse_phandle(np, "apple,atc-phy", 0);
		bool match = phy == tphy->dev->of_node && of_device_is_available(np);

		of_node_put(phy);
		if (match) {
			of_node_put(np);
			return true;
		}
	}
	return false;
}

/* USB4 entered with Enter_USB in USB4 mode, or Thunderbolt alt mode. */
static bool t6040_cio_requested(const struct typec_mux_state *state)
{
	const struct enter_usb_data *data = state->data;

	if (state->alt)
		return state->alt->svid == USB_TYPEC_TBT_SID;
	return state->mode == TYPEC_MODE_USB4 && data &&
	       FIELD_GET(EUDO_USB_MODE_MASK, data->eudo) == EUDO_USB_MODE_USB4;
}

static void t6040_cio_remove(void *data)
{
	struct t6040_usb2_phy *tphy = data;

	guard(mutex)(&tphy->lock);
	if (tphy->router_running)
		dev_warn(tphy->dev, "CIO: unbound under a running USB4 router\n");
	tphy->router_running = false;
	t6040_cio_down_locked(tphy);
}

/* CIO needs the USB3 pieces and the CIO lane tables; anything less leaves it off. */
static int t6040_cio_probe(struct t6040_usb2_phy *tphy)
{
	struct device *dev = tphy->dev;
	size_t i;

	if (!tphy->usb3_ok ||
	    (!of_property_present(dev->of_node, t6040_cio_tunable_props[1]) &&
	     !of_property_present(dev->of_node, t6040_cio_tunable_props[2])))
		return 0;

	for (i = 0; i < ARRAY_SIZE(t6040_cio_tunable_props); i++) {
		const char *prop = t6040_cio_tunable_props[i];
		struct apple_tunable *t;

		if (!of_property_present(dev->of_node, prop)) {
			if (i == 1 || i == 2) {
				dev_warn(dev, "CIO: no %s, no USB4/Thunderbolt\n", prop);
				return 0;
			}
			continue;
		}
		t = devm_apple_tunable_parse(dev, dev->of_node, prop, tphy->res[T6040_USB3_CORE]);
		if (IS_ERR(t)) {
			dev_warn(dev, "CIO: %s: %pe, no USB4/Thunderbolt\n", prop, t);
			return 0;
		}
		tphy->cio_tunables[i] = t;
	}

	/* Registered before the mode switch, so it runs after the switch is gone. */
	if (devm_add_action_or_reset(dev, t6040_cio_remove, tphy))
		return -ENOMEM;
	tphy->cio_ok = true;
	dev_dbg(dev, "CIO (USB4/Thunderbolt) mode available\n");
	return 0;
}

/* ---- Type-C mode switch: the Type-C controller asks for DP on a USB-C port ---- */

static int t6040_dp_mux_set(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
	struct t6040_usb2_phy *tphy = typec_mux_get_drvdata(mux);
	bool dp = state->alt && state->alt->svid == USB_TYPEC_DP_SID;
	bool alt = false;

	guard(mutex)(&tphy->lock);
	/* USB4/TBT, only with the CIO tables; leaving it first takes the PHY out of CIO. */
	if (tphy->cio_ok && (tphy->cio_up || t6040_cio_router_enabled(tphy))) {
		bool cio = t6040_cio_requested(state);

		if (tphy->cio_up && !cio) {
			/* The router stops before the mux leaves USB4/TBT (apple.c). */
			if (tphy->router_running) {
				dev_err(tphy->dev, "CIO: the USB4 router still runs, not leaving CIO\n");
				return -EBUSY;
			}
			t6040_cio_down_locked(tphy);
		}
		if (cio) {
			tphy->dp_alt = false;
			tphy->dp_two_lanes = false;
			dev_dbg(tphy->dev, "Type-C mux: %s -> CIO\n",
				state->alt ? "Thunderbolt" : "USB4");
			return t6040_cio_up_locked(tphy);
		}
	}
	tphy->dp_flip = (tphy->orientation == TYPEC_ORIENTATION_REVERSE);
	/*
	 * Latch the DP alt mode state for the DCP's Type-C route. Pin
	 * assignments C and E put DP on all four lanes. D leaves the USB3
	 * pair with its running coprocessor and gives DP the other pair.
	 * The DCP brings the DP lanes up through phy_power_on()/phy_configure().
	 */
	if (dp) {
		switch (state->mode) {
		case TYPEC_DP_STATE_C:
		case TYPEC_DP_STATE_E:
			alt = true;
			break;
		case TYPEC_DP_STATE_D:
			alt = true;
			break;
		default:
			break;
		}
	}
	tphy->dp_alt = alt;
	tphy->dp_two_lanes = alt && state->mode == TYPEC_DP_STATE_D;
	dev_dbg(tphy->dev, "Type-C mux: %s state %lu -> DP alt mode %d, lane map %s\n",
		dp ? "DP" : "non-DP", state->mode, alt, tphy->dp_flip ? "reversed" : "normal");
	if (alt && tphy->dp_up && !tphy->dp_mode_two_lanes && !tphy->dp_two_lanes)
		t6040_dp_xbar_config(tphy, tphy->dp_flip);
	t6040_usb3_reclaim_locked(tphy);
	return 0;
}

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
				tphy->tunables[i] = NULL;
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

	if (ep == T6040_CPU_EP_APP) {
		if (atomic_xchg(&tphy->drop_next_power_down_ack, 0)) {
			dev_warn(tphy->dev, "USB3: dropped Power Down acknowledge (fault injection)\n");
			return;
		}
		dev_dbg(tphy->dev, "USB3: Power Down acknowledge %#llx\n", msg);
		complete(&tphy->app_reply);
	} else {
		dev_dbg(tphy->dev, "USB3: coprocessor message on endpoint %u: %#llx\n", ep, msg);
	}
}

static const struct apple_rtkit_ops t6040_cpu_rtkit_ops = {
	.flags = APPLE_RTKIT_COPROC_PLACES_BUFFERS,
	.crashed = t6040_cpu_crashed,
	.recv_message = t6040_cpu_recv_message,
	.shmem_setup = t6040_cpu_shmem_setup,
	.shmem_destroy = t6040_cpu_shmem_destroy,
};

static void t6040_rtkit_cleanup(void *data)
{
	t6040_rtkit_stop_transport(data);
}

static void t6040_mbox_dev_put(void *data)
{
	put_device(data);
}

static int t6040_get_mbox_dev(struct t6040_usb2_phy *tphy)
{
	struct of_phandle_args args;
	struct platform_device *pdev;
	int ret;

	ret = of_parse_phandle_with_args(tphy->dev->of_node, "mboxes", "#mbox-cells", 0,
					 &args);
	if (ret)
		return ret;
	pdev = of_find_device_by_node(args.np);
	of_node_put(args.np);
	if (!pdev)
		return -EPROBE_DEFER;

	tphy->mbox_dev = &pdev->dev;
	return devm_add_action_or_reset(tphy->dev, t6040_mbox_dev_put, tphy->mbox_dev);
}

static void t6040_debugfs_remove(void *data)
{
	debugfs_remove_recursive(data);
}

static int t6040_debugfs_init(struct t6040_usb2_phy *tphy)
{
	const char *name;
	int ret;

	name = devm_kasprintf(tphy->dev, GFP_KERNEL, "t6040-atcphy-%s",
			      dev_name(tphy->dev));
	if (!name)
		return -ENOMEM;
	tphy->debugfs = debugfs_create_dir(name, NULL);
	if (IS_ERR(tphy->debugfs)) {
		if (PTR_ERR(tphy->debugfs) == -ENODEV) {
			tphy->debugfs = NULL;
			return 0;
		}
		return PTR_ERR(tphy->debugfs);
	}
	debugfs_create_atomic_t("drop_next_power_down_ack", 0600, tphy->debugfs,
				&tphy->drop_next_power_down_ack);
	ret = devm_add_action_or_reset(tphy->dev, t6040_debugfs_remove, tphy->debugfs);
	if (ret)
		tphy->debugfs = NULL;
	return ret;
}

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
			tphy->tunables[i] = NULL;
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

	ret = t6040_get_mbox_dev(tphy);
	if (ret)
		return dev_err_probe(dev, ret, "failed to find PHY coprocessor mailbox\n");

	tphy->rtk = apple_rtkit_init(dev, tphy, "phy-cpu", 0, &t6040_cpu_rtkit_ops);
	if (IS_ERR(tphy->rtk)) {
		ret = PTR_ERR(tphy->rtk);
		tphy->rtk = NULL;
		if (ret == -EPROBE_DEFER)
			return ret;
		dev_warn(dev, "USB3: no PHY coprocessor mailbox (%d), USB2 only\n", ret);
		return 0;
	}
	ret = devm_add_action_or_reset(dev, t6040_rtkit_cleanup, tphy);
	if (ret)
		return ret;

	ret = t6040_debugfs_init(tphy);
	if (ret)
		return ret;

	tphy->usb3_ok = true;
	return 0;
}

/* The controller's reset, see t6040_usb2_reset_assert(). */
static void t6040_dwc3_reset_hold_locked(struct t6040_usb2_phy *tphy)
{
	lockdep_assert_held(&tphy->lock);
	t6040_usb2_clear32(tphy->pipehandler + T6040_PIPEHANDLER_AON_GEN,
			   T6040_PIPEHANDLER_AON_GEN_DWC3_RESET_N);
	t6040_usb2_set32(tphy->pipehandler + T6040_PIPEHANDLER_AON_GEN,
			 T6040_PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
}

/* -EIO if the register does not read back released: the controller may not be there. */
static int t6040_dwc3_reset_release_locked(struct t6040_usb2_phy *tphy)
{
	u32 value;

	lockdep_assert_held(&tphy->lock);
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
	t6040_dwc3_reset_hold_locked(tphy);
	/* From here the reset is its consumer's again, whatever a sleep did with it. */
	tphy->sleep_reset = false;
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
	int ret;

	PM_RUNTIME_ACQUIRE(tphy->dev, pm);
	ret = PM_RUNTIME_ACQUIRE_ERR(&pm);
	if (ret)
		return ret;

	guard(mutex)(&tphy->lock);
	if (tphy->shutdown_failed)
		return -EIO;
	ret = t6040_dwc3_reset_release_locked(tphy);
	if (ret)
		return ret;
	tphy->sleep_reset = false;
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
	init_waitqueue_head(&tphy->sleep_wq);
	atomic_set(&tphy->drop_next_power_down_ack, 0);
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

	ret = t6040_cio_probe(tphy);
	if (ret)
		return ret;

	ret = t6040_dp_probe(tphy);
	if (ret)
		return ret;

	provider = devm_of_phy_provider_register(dev, t6040_usb2_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

/* ---- a sleep with the host kept (linux/phy/apple-atc.h) ----
 *
 * A deep sleep powers the SoC down, and with it this PHY, the controller and
 * its xHC. dwc3-apple used to stop the host for it, which removes every USB
 * device, and start it again afterwards. For a port that only does USB it
 * keeps the xHCI device and its USB devices instead, and the PHY goes through
 * the same stop and start below them: down once xHCI has suspended
 * (apple_t6040_atcphy_sleep_enter()), the controller out of reset before its
 * core is touched again (apple_t6040_atcphy_sleep_result()), USB2 and USB3 back
 * once the core is set up (apple_t6040_atcphy_sleep_power_up()). xHCI then
 * starts over and the USB core resets the devices it finds again.
 *
 * The register sequences are those of power_off, power_on and the reset, in
 * the order dwc3-apple's stop and start run them, so the CFG block is parked
 * at the sleep's commit as after any detach. The platform has the PHY's firmware
 * save its state instead and restore it at the wake;
 * the devices stay without that, and it isn't
 * done here. The steps are the same whether or not the SoC then loses power
 * (a refused sleep, a dry run), and leave nothing the wake doesn't set up
 * again.
 *
 * The phys' power counts and the reset's runtime PM reference are their
 * consumers' and stay as they are; sleep_down and sleep_reset say what the
 * hardware is behind them. A host that stops after all goes through the phys'
 * and the reset's own operations, which end that.
 */

static struct t6040_usb2_phy *t6040_from_usb2_phy(struct phy *phy)
{
	if (!phy || phy->ops != &t6040_usb2_phy_ops)
		return NULL;
	return phy_get_drvdata(phy);
}

/*
 * A USB2 host that runs, alone or with USB3 on this PHY and the PIPE on it.
 * DisplayPort on the lanes and USB4/Thunderbolt have clients of their own,
 * which come back in their own order: such a port's host stops for the
 * sleep. So does one whose four lanes are DisplayPort's by its pin assignment
 * (C, E), link or not.
 *
 * Pin assignment D without a DisplayPort link is a USB port to the PHY: a
 * dock with no display on it. USB3 runs on its pair as on any other port,
 * nothing is programmed for the other pair before the DCP powers the DP phy
 * on, and the mode is only latched here for that. With a link up the port
 * stops its host like the others.
 */
static bool t6040_can_sleep_in_place(const struct t6040_usb2_phy *tphy)
{
	return tphy->pipehandler && tphy->powered && tphy->dwc3_running &&
	       tphy->mode == PHY_MODE_USB_HOST && !tphy->shutdown_failed &&
	       !tphy->sleep_no_keep && !tphy->sleep_down && !tphy->sleep_reset &&
	       !tphy->cio_up && !tphy->pipe_tunneled && !tphy->router_running &&
	       !tphy->dp_up && (!tphy->dp_alt || tphy->dp_two_lanes) &&
	       !tphy->usb3_lent && tphy->usb3_up == tphy->pipe_switched;
}

/* Room for every name of t6040_sleep_in_place_misses() at once. */
#define T6040_SLEEP_MISSES_LEN	192

/* For the log: the conditions of t6040_can_sleep_in_place() that fail, by name. */
static void t6040_sleep_in_place_misses(const struct t6040_usb2_phy *tphy, char *buf, size_t size)
{
	int len = 0;

#define T6040_MISS(cond, name) \
	do { if (cond) len += scnprintf(buf + len, size - len, " %s", name); } while (0)
	buf[0] = 0;
	T6040_MISS(!tphy->pipehandler, "no_pipehandler");
	T6040_MISS(!tphy->powered, "usb2_off");
	T6040_MISS(!tphy->dwc3_running, "dwc3_in_reset");
	T6040_MISS(tphy->mode != PHY_MODE_USB_HOST, "mode");
	T6040_MISS(tphy->shutdown_failed, "shutdown_failed");
	T6040_MISS(tphy->sleep_no_keep, "sleep_no_keep");
	T6040_MISS(tphy->sleep_down, "sleep_down");
	T6040_MISS(tphy->sleep_reset, "sleep_reset");
	T6040_MISS(tphy->cio_up, "cio_up");
	T6040_MISS(tphy->pipe_tunneled, "pipe_tunneled");
	T6040_MISS(tphy->router_running, "router_running");
	T6040_MISS(tphy->dp_up, "dp_up");
	T6040_MISS(tphy->dp_alt && !tphy->dp_two_lanes, "dp_four_lanes");
	T6040_MISS(tphy->usb3_lent, "usb3_lent");
	T6040_MISS(tphy->usb3_up != tphy->pipe_switched, "pipe_state");
#undef T6040_MISS
}

int apple_t6040_atcphy_sleep_prepare(struct phy *usb2_phy)
{
	struct t6040_usb2_phy *tphy = t6040_from_usb2_phy(usb2_phy);

	if (!tphy)
		return -EOPNOTSUPP;

	guard(mutex)(&tphy->lock);
	if (!t6040_can_sleep_in_place(tphy)) {
		char why[T6040_SLEEP_MISSES_LEN];

		t6040_sleep_in_place_misses(tphy, why, sizeof(why));
		dev_info(tphy->dev,
			 "the host stops for the sleep: USB2 %d, USB3 %d, PIPE %d, DP %d/%d, CIO %d, lent %d, failed %d/%d, because:%s\n",
			 tphy->powered, tphy->usb3_up, tphy->pipe_switched, tphy->dp_alt,
			 tphy->dp_up, tphy->cio_up, tphy->usb3_lent, tphy->shutdown_failed,
			 tphy->sleep_no_keep, why);
		return -EBUSY;
	}
	tphy->sleep_armed = true;
	tphy->sleep_ret = 0;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_t6040_atcphy_sleep_prepare);

int apple_t6040_atcphy_sleep_enter(struct phy *usb2_phy)
{
	struct t6040_usb2_phy *tphy = t6040_from_usb2_phy(usb2_phy);
	bool usb3;
	int ret;

	if (!tphy)
		return -EOPNOTSUPP;

	PM_RUNTIME_ACQUIRE(tphy->dev, pm);
	ret = PM_RUNTIME_ACQUIRE_ERR(&pm);
	if (ret)
		return ret;

	guard(mutex)(&tphy->lock);
	if (!tphy->sleep_armed)
		return -EINVAL;
	if (tphy->sleep_down)
		return 0;
	/* The mux moved since the prepare: that mode's clients don't come back this way. */
	if (!t6040_can_sleep_in_place(tphy)) {
		char why[T6040_SLEEP_MISSES_LEN];

		t6040_sleep_in_place_misses(tphy, why, sizeof(why));
		dev_err(tphy->dev,
			"USB changed for the sleep (USB3 %d, PIPE %d, DP %d/%d, CIO %d, because:%s): refusing it\n",
			tphy->usb3_up, tphy->pipe_switched, tphy->dp_alt, tphy->dp_up,
			tphy->cio_up, why);
		tphy->sleep_ret = -EBUSY;
		return -EBUSY;
	}

	/*
	 * USB3 that an earlier wake of this host left down counts as well: this
	 * sleep's wake tries it again, with the SoC's power cycle behind it if
	 * the sleep is a real one.
	 */
	usb3 = tphy->usb3_up;
	tphy->sleep_usb3 |= usb3;
	/* As dwc3-apple's stop: USB3 with its PIPE, USB2, then the controller's reset. */
	t6040_usb3_down_locked(tphy);
	ret = t6040_usb2_off_locked(tphy);
	if (ret)
		dev_err(tphy->dev, "USB2 did not power down for the sleep: %d\n", ret);
	t6040_dwc3_reset_hold_locked(tphy);
	tphy->sleep_down = true;
	tphy->sleep_reset = true;
	WRITE_ONCE(tphy->sleep_back, false);
	dev_info(tphy->dev, "PHY down for the sleep with the host kept, USB3 %s\n",
		 usb3 ? "was up" : "was off");
	return 0;
}
EXPORT_SYMBOL_GPL(apple_t6040_atcphy_sleep_enter);

/* -ENODEV while the reset does not read back released: the core must not be touched. */
static int t6040_sleep_release_locked(struct t6040_usb2_phy *tphy)
{
	lockdep_assert_held(&tphy->lock);
	if (t6040_dwc3_reset_release_locked(tphy)) {
		dev_err(tphy->dev, "the controller's reset did not release after the sleep: %#x\n",
			readl(tphy->pipehandler + T6040_PIPEHANDLER_AON_GEN));
		tphy->sleep_ret = -ENODEV;
		return -ENODEV;
	}
	tphy->sleep_reset = false;
	tphy->sleep_ret = APPLE_ATCPHY_SLEEP_RESET;
	return APPLE_ATCPHY_SLEEP_RESET;
}

int apple_t6040_atcphy_sleep_result(struct phy *usb2_phy)
{
	struct t6040_usb2_phy *tphy = t6040_from_usb2_phy(usb2_phy);
	int ret;

	if (!tphy)
		return -EOPNOTSUPP;

	PM_RUNTIME_ACQUIRE(tphy->dev, pm);
	ret = PM_RUNTIME_ACQUIRE_ERR(&pm);

	guard(mutex)(&tphy->lock);
	if (!tphy->sleep_armed)
		return -ENODATA;
	/* The first call after the sleep: without the PHY's power the reset can't release. */
	if (tphy->sleep_reset && ret)
		tphy->sleep_ret = -ENODEV;
	else if (tphy->sleep_reset)
		t6040_sleep_release_locked(tphy);
	return tphy->sleep_ret;
}
EXPORT_SYMBOL_GPL(apple_t6040_atcphy_sleep_result);

/*
 * Whether the wake of a sleep with the host kept starts USB3: it ran when the
 * port went down, and the coprocessor doesn't wait for a power cycle of its
 * domains, which a system resume can't give it. A sleep that powered the SoC
 * down was that power cycle, see t6040_usb2_resume_early().
 */
static bool t6040_sleep_wake_usb3(const struct t6040_usb2_phy *tphy)
{
	return tphy->sleep_usb3 && !tphy->recovery.cycle_pending;
}

int apple_t6040_atcphy_sleep_power_up(struct phy *usb2_phy)
{
	struct t6040_usb2_phy *tphy = t6040_from_usb2_phy(usb2_phy);
	bool tried;
	int ret;

	if (!tphy)
		return -EOPNOTSUPP;

	PM_RUNTIME_ACQUIRE(tphy->dev, pm);
	ret = PM_RUNTIME_ACQUIRE_ERR(&pm);
	if (ret)
		return ret;

	guard(mutex)(&tphy->lock);
	/* Only for a controller this sleep's result has taken out of reset. */
	if (!tphy->sleep_armed || tphy->sleep_reset)
		return -EINVAL;
	/* Whatever this comes to, DP has nothing more to wait for. */
	t6040_sleep_back_done(tphy);
	if (!tphy->sleep_down)
		return tphy->sleep_ret < 0 ? tphy->sleep_ret : 0;

	/* As dwc3-apple's start, in the core's init: USB2, then USB3 if it ran. */
	ret = tphy->shutdown_failed ? -EIO : t6040_usb2_on_locked(tphy);
	if (ret) {
		dev_err(tphy->dev, "USB2 not back after the sleep (%d): the host has to restart\n",
			ret);
		tphy->sleep_ret = ret;
		return ret;
	}
	tphy->sleep_down = false;
	tried = t6040_sleep_wake_usb3(tphy);
	if (tried) {
		tphy->sleep_waking = true;
		t6040_usb3_up_locked(tphy);
		tphy->sleep_waking = false;
	}
	/*
	 * As at a start, a port whose USB3 doesn't come up stays at USB2: the host
	 * is kept with what runs at USB2 on it, and only the SuperSpeed devices
	 * leave the bus.
	 */
	if (tphy->sleep_usb3 && !tphy->usb3_up)
		dev_warn(tphy->dev, "USB3 not back after the sleep (%s): the host stays, at USB2\n",
			 tried ? "its bring-up failed" : "its coprocessor waits for a power cycle");
	else
		dev_info(tphy->dev, "PHY back after the sleep with the host kept, USB3 %s\n",
			 tphy->usb3_up ? "up" : "off");
	return 0;
}
EXPORT_SYMBOL_GPL(apple_t6040_atcphy_sleep_power_up);

int apple_t6040_atcphy_sleep_recover(struct phy *usb2_phy)
{
	struct t6040_usb2_phy *tphy = t6040_from_usb2_phy(usb2_phy);

	if (!tphy)
		return -EOPNOTSUPP;

	PM_RUNTIME_ACQUIRE(tphy->dev, pm);
	if (PM_RUNTIME_ACQUIRE_ERR(&pm))
		return -ENODEV;

	guard(mutex)(&tphy->lock);
	/* Not held in reset by a sleep: there is nothing of this PHY's to put right. */
	if (!tphy->sleep_reset)
		return -ENODEV;
	return t6040_sleep_release_locked(tphy);
}
EXPORT_SYMBOL_GPL(apple_t6040_atcphy_sleep_recover);

/*
 * Whether a sleep ended with keeping the host having failed: the host was
 * lost, and what the wake came to is an error of the PHY's own, which is the
 * controller's reset that didn't release or USB2 that didn't come back. USB3
 * never costs the host. A sleep refused for a USB change doesn't count, nor
 * a host that was restarted for another reason.
 */
static bool t6040_sleep_keep_failed(bool kept, int sleep_ret)
{
	return !kept && sleep_ret < 0 && sleep_ret != -EBUSY;
}

void apple_t6040_atcphy_sleep_finish(struct phy *usb2_phy, bool kept)
{
	struct t6040_usb2_phy *tphy = t6040_from_usb2_phy(usb2_phy);

	if (!tphy)
		return;

	guard(mutex)(&tphy->lock);
	t6040_sleep_back_done(tphy);
	if (!tphy->sleep_armed)
		return;
	/*
	 * The host has been restarted by now. It stops for the next sleep, and
	 * the start after that one lets it be kept again.
	 */
	if (t6040_sleep_keep_failed(kept, tphy->sleep_ret)) {
		tphy->sleep_no_keep = true;
		dev_warn(tphy->dev,
			 "PHY did not come back from a sleep with the host kept (%d): the host stops for the next sleep\n",
			 tphy->sleep_ret);
	}
	tphy->sleep_armed = false;
	tphy->sleep_ret = 0;
}
EXPORT_SYMBOL_GPL(apple_t6040_atcphy_sleep_finish);

#ifdef CONFIG_HIBERNATION
static int t6040_phy_count(struct phy *phy)
{
	return IS_ERR_OR_NULL(phy) ? 0 : phy->power_count;
}

/*
 * Resume from disk is a cold boot: the PHY, its coprocessor and the
 * pipehandler come back reset, while this driver's state comes from the
 * image.  Before the image its consumers gave it back: the sleep hook
 * unbinds dwc3 (USB2/USB3 phys powered off, controller reset asserted),
 * the DCP powers the DP phy off at hibernation prepare.  What is left set
 * here describes hardware that no longer is: clear it to the state after
 * probe, so that the next power-on programs the PHY afresh (with pin D,
 * t6040_dp_power_on() otherwise takes the USB3 PHY for running).  What
 * consumers hold (powered, usb3_on, usb3_pipe, dwc3_running and the PHY
 * core counts, with their runtime PM reference) is theirs to release and
 * is kept, so that every count stays balanced; a phy still held is only
 * reported.  The port's Type-C state (orientation, DP alt mode, pin
 * assignment) describes the partner, not the PHY, and stays for the next
 * power-on.  A power cycle that the coprocessor was waiting for when the
 * image was made has happened by now.  Early, before any .restore: the
 * Type-C controller's may set the mux again.  Nothing touches the hardware
 * here.  CIO is torn down before the image (.freeze), so the image never
 * holds a CIO session.
 */
static int t6040_usb2_restore_early(struct device *dev)
{
	struct t6040_usb2_phy *tphy = dev_get_drvdata(dev);

	if (!tphy)
		return 0;
	/* Hibernation aborted before the image (PMSG_RESTORE then too): nothing restarted. */
	if (!pm_hibernate_is_restored())
		return 0;
	guard(mutex)(&tphy->lock);
	if (tphy->powered || tphy->usb3_on || tphy->dwc3_running ||
	    t6040_phy_count(tphy->dp))
		dev_warn(dev,
			 "restore: still held across the image (usb2 %d, usb3 %d, dwc3 %d, dp %d/%d): its next power-on finds the PHY cold\n",
			 tphy->powered, tphy->usb3_on, tphy->dwc3_running,
			 t6040_phy_count(tphy->dp), tphy->dp_up);
	/* .freeze refuses an image with CIO up; never trust one that has it. */
	if (tphy->cio_up) {
		dev_warn(dev, "restore: CIO was up in the image, dropped\n");
		tphy->cio_up = false;
		tphy->router_running = false;
		tphy->usb3_tunnel_req = false;
		tphy->pipe_tunneled = false;
		tphy->tun_pll = false;
		tphy->tun_rate[0] = tphy->tun_rate[1] = 0;
		pm_runtime_put_noidle(dev);
	}
	dev_info(dev, "restore: PHY state reset (usb3 up %d, pipe %d, lent %d, dp up %d, 2-lane %d)\n",
		 tphy->usb3_up, tphy->pipe_switched, tphy->usb3_lent, tphy->dp_up,
		 tphy->dp_mode_two_lanes);
	tphy->usb3_up = false;
	tphy->pipe_switched = false;
	tphy->usb3_lent = false;
	tphy->dp_up = false;
	tphy->dp_mode_two_lanes = false;
	tphy->dp_mode_flip = false;
	tphy->dp_rate = -1;
	tphy->shutdown_failed = false;
	t6040_recovery_power_lost(&tphy->recovery);
	return 0;
}

/*
 * The coprocessor's RTKit session is reset with it.  Its host side is
 * reset at power-down already (t6040_usb3_firmware_power_down()); one
 * still running here (USB3 held across the image) is reset now, in the
 * normal phase, after the DARTs its buffers live behind are restored.
 */
static int t6040_usb2_restore(struct device *dev)
{
	struct t6040_usb2_phy *tphy = dev_get_drvdata(dev);
	int ret;

	if (!tphy || IS_ERR_OR_NULL(tphy->rtk))
		return 0;
	if (!pm_hibernate_is_restored())
		return 0;
	guard(mutex)(&tphy->lock);
	if (!apple_rtkit_is_running(tphy->rtk))
		return 0;
	ret = apple_rtkit_reinit(tphy->rtk);
	dev_info(dev, "restore: USB3 coprocessor RTKit reset: %d\n", ret);
	return 0;
}

#endif

#ifdef CONFIG_PM_SLEEP
/*
 * System sleep with the port in USB4/Thunderbolt: s2idle, a firmware-driven
 * suspend (pm_suspend_via_firmware(), which brings the PHY back like a cold
 * start) and the three phases of hibernation that leave the running system
 * (freeze before the image, poweroff after it). CIO is not kept across a
 * sleep: the ACIO sleep is a full PHY shutdown, and
 * the ACIO wake a full PHY init for mode 2; the firmware-assisted S2R state
 * save is for the USB modes only. Here CIO is torn down the same way in each
 * of them, after the router has stopped (apple_t6040_cio_router_stopped(),
 * from its sleep notifier before any device callback); its DP tunnels and the
 * tunnelled PIPE go with it. Nothing is kept for resume: the Type-C
 * controller reads the port afresh after every sleep and sets the mux again,
 * which brings CIO back before it starts the router. A router still marked
 * running breaks that contract and fails the transition. Without a CIO
 * session none of this touches the hardware.
 */
static int t6040_usb2_sleep(struct device *dev, const char *what)
{
	struct t6040_usb2_phy *tphy = dev_get_drvdata(dev);

	if (!tphy)
		return 0;
	guard(mutex)(&tphy->lock);
	if (!tphy->cio_up)
		return 0;
	if (tphy->router_running) {
		dev_err(dev, "%s: the USB4 router still runs on this port; it must stop before the PHY\n",
			what);
		return -EBUSY;
	}
	t6040_cio_down_locked(tphy);
	dev_info(dev, "%s: CIO down for system sleep; the Type-C controller brings it back\n",
		 what);
	return 0;
}

static int t6040_usb2_suspend(struct device *dev)
{
	return t6040_usb2_sleep(dev, "suspend");
}

static int t6040_usb2_freeze(struct device *dev)
{
	return t6040_usb2_sleep(dev, "freeze");
}

static int t6040_usb2_poweroff(struct device *dev)
{
	return t6040_usb2_sleep(dev, "poweroff");
}

/*
 * A port that went down for the sleep with its host kept comes back in the
 * host's resume. From here until then DP waits for it in its power-on, see
 * t6040_dp_wait_host(). Early, before any .resume: the Type-C controller
 * reports a display from its own.
 *
 * If the firmware woke the system, the SoC was powered down, and the
 * coprocessor and its domains with it: no start after this sleep has a power
 * cycle of theirs to wait for or to run, the wake of a host kept through it
 * included.
 */
static int t6040_usb2_resume_early(struct device *dev)
{
	struct t6040_usb2_phy *tphy = dev_get_drvdata(dev);

	if (!tphy)
		return 0;
	guard(mutex)(&tphy->lock);
	if (pm_resume_via_firmware())
		t6040_recovery_power_lost(&tphy->recovery);
	WRITE_ONCE(tphy->sleep_back, tphy->sleep_down);
	return 0;
}

static const struct dev_pm_ops t6040_usb2_pm_ops = {
	.suspend = t6040_usb2_suspend,
	.resume_early = t6040_usb2_resume_early,
	.freeze = t6040_usb2_freeze,
	.poweroff = t6040_usb2_poweroff,
#ifdef CONFIG_HIBERNATION
	.restore_early = t6040_usb2_restore_early,
	.restore = t6040_usb2_restore,
#endif
};
#endif

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
#ifdef CONFIG_PM_SLEEP
		.pm = &t6040_usb2_pm_ops,
#endif
	},
};

/* ---- the USB4 router's view of the PHY (linux/phy/apple-t6040-cio.h) ---- */

/*
 * Run @fn on the bound PHY of @phy_np with its lock held. The device lock
 * keeps the driver bound for the call.
 */
static int t6040_cio_call(struct device_node *phy_np,
			  int (*fn)(struct t6040_usb2_phy *tphy, unsigned long arg),
			  unsigned long arg)
{
	struct platform_device *pdev;
	struct t6040_usb2_phy *tphy;
	int ret;

	if (!phy_np || !of_device_is_compatible(phy_np, "apple,t6040-atcphy"))
		return -ENODEV;
	pdev = of_find_device_by_node(phy_np);
	if (!pdev)
		return -EPROBE_DEFER;
	device_lock(&pdev->dev);
	tphy = platform_get_drvdata(pdev);
	if (pdev->dev.driver != &t6040_usb2_driver.driver || !tphy) {
		ret = -EPROBE_DEFER;
	} else {
		guard(mutex)(&tphy->lock);
		ret = fn(tphy, arg);
	}
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
	return ret;
}

static int t6040_cio_state_fn(struct t6040_usb2_phy *tphy, unsigned long arg)
{
	return tphy->cio_up ? 1 : 0;
}

static int t6040_cio_router_fn(struct t6040_usb2_phy *tphy, unsigned long running)
{
	if (running) {
		if (!tphy->cio_up)
			return -EAGAIN;
		tphy->router_running = true;
		dev_dbg(tphy->dev, "CIO: router started\n");
		t6040_cio_pipe_update_locked(tphy);
		return 0;
	}
	if (!tphy->router_running)
		return 0;
	tphy->router_running = false;
	tphy->usb3_tunnel_req = false;
	t6040_cio_pipe_update_locked(tphy);
	dev_dbg(tphy->dev, "CIO: router stopped\n");
	return 0;
}

static int t6040_cio_usb3_fn(struct t6040_usb2_phy *tphy, unsigned long on)
{
	if (on && !tphy->router_running)
		return -EAGAIN;
	tphy->usb3_tunnel_req = on;
	t6040_cio_pipe_update_locked(tphy);
	return 0;
}

int apple_t6040_cio_state(struct device_node *phy_np)
{
	return t6040_cio_call(phy_np, t6040_cio_state_fn, 0);
}
EXPORT_SYMBOL_GPL(apple_t6040_cio_state);

int apple_t6040_cio_router_started(struct device_node *phy_np)
{
	return t6040_cio_call(phy_np, t6040_cio_router_fn, 1);
}
EXPORT_SYMBOL_GPL(apple_t6040_cio_router_started);

void apple_t6040_cio_router_stopped(struct device_node *phy_np)
{
	t6040_cio_call(phy_np, t6040_cio_router_fn, 0);
}
EXPORT_SYMBOL_GPL(apple_t6040_cio_router_stopped);

int apple_t6040_cio_usb3_tunnel(struct device_node *phy_np, bool on)
{
	return t6040_cio_call(phy_np, t6040_cio_usb3_fn, on);
}
EXPORT_SYMBOL_GPL(apple_t6040_cio_usb3_tunnel);

module_platform_driver(t6040_usb2_driver);

MODULE_AUTHOR("The Asahi Linux Contributors");
MODULE_DESCRIPTION("Apple T6040 Type-C PHY, USB2/USB3 host and DisplayPort");
MODULE_LICENSE("Dual BSD/GPL");

#if IS_ENABLED(CONFIG_PHY_APPLE_T6040_KUNIT_TEST)
#include "t6040-recovery-test.c"
#endif
