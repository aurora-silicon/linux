// SPDX-License-Identifier: GPL-2.0
/*
 * Apple Silicon DWC3 Glue driver
 * Copyright (C) The Asahi Linux Contributors
 *
 * Based on:
 *  - dwc3-qcom.c Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *  - dwc3-of-simple.c Copyright (c) 2015 Texas Instruments Incorporated - https://www.ti.com
 */

#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/phy/apple-atc.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeup.h>
#include <linux/reset.h>
#include <linux/soc/apple/sleep.h>
#include <linux/soc/apple/t8132.h>
#include <linux/soc/apple/tunable.h>
#include <linux/suspend.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/usb/phy.h>

#include "dwc3-apple-state.h"
#include "glue.h"
#include "io.h"

/*
 * This platform requires a very specific sequence of operations to bring up dwc3 and its USB3 PHY:
 *
 * 1) The PHY itself has to be brought up; for this we need to know the mode (USB3,
 *    USB3+DisplayPort, USB4, etc) and the lane orientation. This happens through typec_mux_set.
 * 2) DWC3 has to be brought up but we must not touch the gadget area or start xhci yet.
 * 3) The PHY bring-up has to be finalized and dwc3's PIPE interface has to be switched to the
 *    USB3 PHY, this is done inside phy_set_mode.
 * 4) We can now initialize xhci or gadget mode.
 *
 * We can switch 1 and 2 but 3 has to happen after (1 and 2) and 4 has to happen after 3.
 *
 * And then to bring this all down again:
 *
 * 1) DWC3 has to exit host or gadget mode and must no longer touch those registers
 * 2) The PHY has to switch dwc3's PIPE interface back to the dummy backend
 * 3) The PHY itself can be shut down, this happens from typec_mux_set
 *
 * We also can't transition the PHY from one mode to another while dwc3 is up and running (this is
 * slightly wrong, some transitions are possible, others aren't but because we have no documentation
 * for this I'd rather play it safe).
 *
 * After both the PHY and dwc3 are initialized we will only ever see a single "new device connected"
 * event. If we just keep them running only the first device plugged in will ever work. XHCI's port
 * status register actually does show the correct state but no interrupt ever comes in. In gadget
 * mode we don't even get a USBDisconnected event and everything looks like there's still something
 * connected on the other end.
 * This can be partially explained because the USB2 D+/D- lines are connected through a stateful
 * eUSB2 repeater which in turn is controlled by a variant of the TI TPS6598x USB PD chip which
 * resets the repeater out-of-band everytime the CC lines are (dis)connected. This then requires a
 * PHY reset to make sure the PHY and the eUSB2 repeater state are synchronized again.
 *
 * And to make this all extra fun: If we get the order of some of this wrong either the port is just
 * broken until a phy+dwc3 reset, or it's broken until a full SoC reset (likely because we can't
 * reset some parts of the PHY), or some watchdog kicks in after a few seconds and forces a full SoC
 * reset (mostly seen this with USB4/Thunderbolt but there's clearly some watchdog that hates
 * invalid states).
 *
 * Hence there's really no good way to keep dwc3 fully up and running after we disconnect a cable
 * because then we can't shut down the PHY anymore. And if we kept the PHY running in whatever mode
 * it was until the next cable is connected we'd need to tear it all down and bring it back up again
 * anyway to detect and use the next device.
 *
 * Instead, we just shut down everything when a cable is disconnected and transition to
 * DWC3_APPLE_NO_CABLE.
 * During initial probe we don't have any information about the connected cable and can't bring up
 * the PHY properly and thus also can't fully bring up dwc3. Instead, we just keep everything off
 * and defer the first dwc3 probe until we get the first cable connected event. Until then we stay
 * in DWC3_APPLE_PROBE_PENDING.
 * Once a cable is connected we then keep track of the controller mode here by transitioning to
 * DWC3_APPLE_HOST or DWC3_APPLE_DEVICE.
 *
 * Some boards do not wire the controller's USB2 pairs to a connector but to a fixed USB2 hub on
 * the board, whose downstream ports serve the Type-C connectors (Apple J700). Such a controller is
 * a host whatever happens at the connectors and never sees a role change: it is brought up in
 * DWC3_APPLE_HOST at probe and stays there. The Type-C port controllers only drive the PHY's
 * Type-C mux for the SuperSpeed lanes, and the PHY follows those mode changes on its own. It keeps
 * to the order above for the SuperSpeed side: the USB core holds the controller's SuperSpeed root
 * port powered off for the PHY, port runtime PM included, from before the PIPE goes to the dummy
 * backend until the PIPE is back on the USB3 PHY. The USB2 root port and the hub on it stay up
 * throughout.
 *
 * Whenever the core is up, for either kind of port, it is kept running across system sleep, see
 * dwc3_apple_init(). In host mode xHCI then keeps running in suspend-to-idle when a USB device
 * may wake the system, as a halted xHC could not signal the wakeup.
 *
 * A deep sleep takes the controller's power domain away. Behind a fixed hub on T8140 the host
 * sleeps in place, as macOS's does: xHCI suspends and resumes as usual, and the PHY takes the
 * core through hibernation in between, so the hub and its devices stay. The core comes back
 * without its registers and is set up again in place. With its always-on side kept, xHCI
 * restores its saved state and the hub resumes from U3, with no reset; without it xHCI starts
 * over, and the devices reset-resume. Anywhere else, or when the PHY can't, the glue stops
 * everything before the freezer runs, the way a cable disconnect does, and brings it back
 * once the system has resumed. See dwc3_apple_pm_notify().
 *
 * A T6040 Type-C port in host mode sleeps in place as well, in another way, as its PHY has no
 * hibernation to take the core through and can't bring itself back with interrupts off (it
 * boots a coprocessor). Below the xHCI device, which stays with its USB devices, the port goes
 * through the stop and the start of a cable session:
 *
 *  - Before the freezer (dwc3_apple_pm_notify()): the PHY says whether the port only does USB.
 *    The glue's device stops being a syscore device for this sleep, so that its power domain
 *    goes off before the commit like a stopped controller's and the PM core runs its callbacks
 *    and orders it after the DARTs and the PHY. xHCI's own wakeup is off for it, so that xHCI
 *    halts whatever state the sleep ends up in. A role the port controller asks for from here
 *    on is kept for after the sleep.
 *  - Device suspend, after xHCI's (dwc3_apple_suspend(), the parent's callback): the PHY takes
 *    USB3 and USB2 down and the core into reset. This is what leaves the PHY parked at the
 *    commit. Nothing touches the core from here on.
 *  - The noirq phases: the power domain goes off and comes back on, and the DARTs with it,
 *    through genpd, whether or not the SoC lost power in between.
 *  - Device resume, in xHCI's before it touches the core (dwc3_apple_pre_host_resume()): the
 *    PHY releases the reset, the core is set up again as at a start, the PHY brings USB2 and
 *    USB3 back and gets the PIPE, and xHCI is told that its state is lost. It starts over,
 *    and the USB core reset-resumes the devices (USB persist): they keep their device numbers,
 *    and a mounted disk its block device.
 *  - After the thaw (dwc3_apple_pm_notify()): back to a syscore device, and the role the port
 *    controller asked for meanwhile.
 *
 * The same steps run when the sleep is refused late or is a dry run. If any of them fails,
 * the host is restarted the old way once the system has resumed, and the devices enumerate
 * again; a core that may be unpowered is left alone until its reset releases. A port with a
 * DisplayPort link, or whose four lanes are DisplayPort's, or in USB4/Thunderbolt stops for
 * the sleep as before.
 *
 * Runtime PM (T6040 and T8132, see dwc3_apple_core_probe()): the glue holds a reference from
 * the reset deassert to the end of the shutdown, so with no cable the controller can
 * runtime-suspend and its power domain drop, its DARTs (which save and restore their state) and,
 * through the device link of its phys, the PHY with it. On T8132 a port that has not seen a cable
 * yet can suspend as well, with runtime PM held by the glue until the first core probe.
 *
 * enum dwc3_apple_state is defined in dwc3-apple-state.h.
 */

#define DWC3_APPLE_NR_TUNABLES 8
#define DWC3_APPLE_NR_DRD_REGS 3

/**
 * struct dwc3_apple - Apple-specific DWC3 USB controller
 * @dwc: Core DWC3 structure
 * @dev: Pointer to the device structure
 * @mmio_resource: Resource to be passed to dwc3_core_probe
 * @apple_regs: Apple-specific DWC3 registers
 * @reset: Reset control
 * @usb2_phy: USB2 PHY, set to device mode before the core is brought up; the xHCI root hub
 *	      sets host mode
 * @usb3_phy: USB3 PHY, configured after the core is brought up
 * @role_sw: USB role switch
 * @fixed_hub: A fixed hub sits on the controller's USB2 port, the controller stays in host mode
 * @host_only: The controller must never enter DWC3_APPLE_DEVICE, see dwc3_apple_host_only()
 * @lock: Mutex for synchronizing access
 * @state: Current state of the controller, see enum dwc3_apple_state and the comment above
 * @core_active: Core still owns initialized PHYs
 * @core_exit_pending: Event buffers have been cleaned for this shutdown
 * @shutting_down: Reject new roles after the shutdown consumer has stopped
 * @reset_pending: Controller reset must complete before another start
 * @usb2_power_off_pending: Core-owned USB2 references still to release
 * @usb3_power_off_pending: Core-owned USB3 references still to release
 * @idle_rpm: Let the controller runtime-suspend while the core is down
 * @rpm_held: The glue holds a runtime PM reference for the running core
 * @pre_core_rpm: Runtime PM is the glue's own until the first core bring-up
 * @dt_dis_u3_susphy: The device tree keeps USB3 PHY suspend off itself
 * @sleep_ddr: The host votes for DRAM in a sleep while it runs (apple,sleep-ddr)
 * @sleep_restart: A Type-C port whose PHY keeps the host through a deep sleep by holding the
 *		   core in reset, see dwc3_apple_suspend()
 * @sleep_xhci_wakeup: xHCI's wakeup setting, which is off for such a sleep
 * @drd_regs: The controller's optional blocks outside dwc3-core (drd-cfg, drd-bulk, drd-ausbc),
 *	      mapped for their tunables
 * @tunables: Controller tunables from the device tree, see dwc3_apple_tunable_list
 */
struct dwc3_apple {
	struct dwc3 dwc;

	struct device *dev;
	struct resource *mmio_resource;
	void __iomem *apple_regs;

	struct reset_control *reset;
	struct phy *usb2_phy;
	struct phy *usb3_phy;
	struct usb_role_switch *role_sw;
	bool fixed_hub;
	/* The root port the fixed hub sits on, its node's reg. */
	u32 hub_port;
	bool host_only;

	struct mutex lock;

	enum dwc3_apple_state state;
	bool core_active;
	bool core_exit_pending;
	bool shutting_down;
	bool reset_pending;
	unsigned long usb2_power_off_pending;
	unsigned long usb3_power_off_pending;
	bool idle_rpm;
	bool rpm_held;
	bool pre_core_rpm;
	bool dt_dis_u3_susphy;
	bool sleep_ddr;
	struct notifier_block pm_nb;
	/* Kept through a deep sleep, with the PHY's hibernation, and the GUID it had. */
	bool sleep_in_place;
	u32 sleep_guid;
	/* The core lost its registers in that sleep and was set up again in place. */
	bool sleep_reinit;
	/* Looks for the fixed hub once a host restarted after a sleep has had time to find it. */
	struct delayed_work hub_check;
	/* The core didn't come back from a sleep or a power cycle: nothing may touch it. */
	bool sleep_dead;
	/*
	 * Stopped for a deep sleep: the state to come back to, and the wakeup setting. The
	 * state also for a Type-C port's host kept through it, see dwc3_apple_suspend().
	 */
	bool sleep_stopped;
	bool sleep_wakeup;
	enum dwc3_apple_state sleep_state;
	bool sleep_restart;
	bool sleep_xhci_wakeup;

	void __iomem *drd_regs[DWC3_APPLE_NR_DRD_REGS];
	struct apple_tunable *tunables[DWC3_APPLE_NR_TUNABLES];
};

#define to_dwc3_apple(d) container_of((d), struct dwc3_apple, dwc)

/*
 * Apple Silicon dwc3 vendor-specific registers
 *
 * These registers were identified by tracing XNU's memory access patterns and correlating them with
 * debug output over serial to determine their names. We don't exactly know what these do but
 * without these USB3 devices sometimes don't work.
 */
#define APPLE_DWC3_REGS_START 0xcd00
#define APPLE_DWC3_REGS_END 0xcdff

#define APPLE_DWC3_CIO_LFPS_OFFSET 0xcd38
#define APPLE_DWC3_CIO_LFPS_OFFSET_VALUE 0xf800f80

#define APPLE_DWC3_CIO_BW_NGT_OFFSET 0xcd3c
#define APPLE_DWC3_CIO_BW_NGT_OFFSET_VALUE 0xfc00fc0

#define APPLE_DWC3_CIO_LINK_TIMER 0xcd40
#define APPLE_DWC3_CIO_PENDING_HP_TIMER GENMASK(23, 16)
#define APPLE_DWC3_CIO_PENDING_HP_TIMER_VALUE 0x14
#define APPLE_DWC3_CIO_PM_LC_TIMER GENMASK(15, 8)
#define APPLE_DWC3_CIO_PM_LC_TIMER_VALUE 0xa
#define APPLE_DWC3_CIO_PM_ENTRY_TIMER GENMASK(7, 0)
#define APPLE_DWC3_CIO_PM_ENTRY_TIMER_VALUE 0x10

static inline void dwc3_apple_writel(struct dwc3_apple *appledwc, u32 offset, u32 value)
{
	writel(value, appledwc->apple_regs + offset - APPLE_DWC3_REGS_START);
}

static inline u32 dwc3_apple_readl(struct dwc3_apple *appledwc, u32 offset)
{
	return readl(appledwc->apple_regs + offset - APPLE_DWC3_REGS_START);
}

static inline void dwc3_apple_mask(struct dwc3_apple *appledwc, u32 offset, u32 mask, u32 value)
{
	u32 reg;

	reg = dwc3_apple_readl(appledwc, offset);
	reg &= ~mask;
	reg |= value;
	dwc3_apple_writel(appledwc, offset, reg);
}

/*
 * macOS's T8140 controller reset sets GCTL's hibernation enable and a power-down scale
 * of 0xd. Without the enable the core's power management accepts a
 * D0/D3 request but saves and restores nothing, so the PHY's hibernation for a sleep
 * needs it.
 */
#define DWC3_APPLE_T8140_PWRDNSCALE 0xd

static void dwc3_apple_setup_hibernation(struct dwc3_apple *appledwc)
{
	struct dwc3 *dwc = &appledwc->dwc;
	u32 reg;

	if (!of_device_is_compatible(appledwc->dev->of_node, "apple,t8140-dwc3"))
		return;
	reg = dwc3_readl(dwc, DWC3_GCTL);
	reg &= ~DWC3_GCTL_PWRDNSCALE_MASK;
	reg |= DWC3_GCTL_PWRDNSCALE(DWC3_APPLE_T8140_PWRDNSCALE) | DWC3_GCTL_GBLHIBERNATIONEN;
	dwc3_writel(dwc, DWC3_GCTL, reg);
}

static void dwc3_apple_setup_cio(struct dwc3_apple *appledwc)
{
	dwc3_apple_writel(appledwc, APPLE_DWC3_CIO_LFPS_OFFSET, APPLE_DWC3_CIO_LFPS_OFFSET_VALUE);
	dwc3_apple_writel(appledwc, APPLE_DWC3_CIO_BW_NGT_OFFSET,
			  APPLE_DWC3_CIO_BW_NGT_OFFSET_VALUE);
	dwc3_apple_mask(appledwc, APPLE_DWC3_CIO_LINK_TIMER, APPLE_DWC3_CIO_PENDING_HP_TIMER,
			FIELD_PREP(APPLE_DWC3_CIO_PENDING_HP_TIMER,
				   APPLE_DWC3_CIO_PENDING_HP_TIMER_VALUE));
	dwc3_apple_mask(appledwc, APPLE_DWC3_CIO_LINK_TIMER, APPLE_DWC3_CIO_PM_LC_TIMER,
			FIELD_PREP(APPLE_DWC3_CIO_PM_LC_TIMER, APPLE_DWC3_CIO_PM_LC_TIMER_VALUE));
	dwc3_apple_mask(appledwc, APPLE_DWC3_CIO_LINK_TIMER, APPLE_DWC3_CIO_PM_ENTRY_TIMER,
			FIELD_PREP(APPLE_DWC3_CIO_PM_ENTRY_TIMER,
				   APPLE_DWC3_CIO_PM_ENTRY_TIMER_VALUE));
}

/*
 * The controller tunables of the ADT (macOS applyAllTunables after its controller reset), by
 * the register block their offsets are relative to. The core's soft reset undoes them, so they
 * are applied after every core init.
 */
enum dwc3_apple_tunable_block {
	DWC3_APPLE_TUNABLE_GLOBALS,	/* DWC3_GLOBALS_REGS_START */
	DWC3_APPLE_TUNABLE_LINK,	/* dwc3-core + 0xd000 */
	DWC3_APPLE_TUNABLE_DBG,		/* dwc3-core + 0xd800 */
	DWC3_APPLE_TUNABLE_CIO,		/* dwc3-core + 0xcd20, in dwc3-apple */
	/* The optional blocks outside dwc3-core, in drd_regs[] order */
	DWC3_APPLE_TUNABLE_DRD_CFG,	/* drd-cfg */
	DWC3_APPLE_TUNABLE_DRD_BULK,	/* drd-bulk */
	DWC3_APPLE_TUNABLE_DRD_AUSBC,	/* drd-ausbc */
	DWC3_APPLE_NR_TUNABLE_BLOCKS
};

static const char * const dwc3_apple_drd_reg_names[DWC3_APPLE_NR_DRD_REGS] = {
	"drd-cfg", "drd-bulk", "drd-ausbc",
};

static_assert(DWC3_APPLE_NR_TUNABLE_BLOCKS - DWC3_APPLE_TUNABLE_DRD_CFG == DWC3_APPLE_NR_DRD_REGS);

#define DWC3_APPLE_LINK_REGS_START	0xd000
#define DWC3_APPLE_DBG_REGS_START	0xd800
#define DWC3_APPLE_LINK_DBG_SIZE	0x800
/* The CIO registers, the fixed ones of dwc3_apple_setup_cio() included */
#define DWC3_APPLE_CIO_REGS_START	0xcd20

/*
 * In the order they are applied. A table is optional, and one for a block that the device tree
 * does not describe is refused. The CIO table follows dwc3_apple_setup_cio() and overrides its
 * fixed values (T6050).
 */
static const struct {
	const char *prop;
	enum dwc3_apple_tunable_block block;
} dwc3_apple_tunable_list[] = {
	{ "apple,tunable-bulk", DWC3_APPLE_TUNABLE_DRD_BULK },
	{ "apple,tunable-gbl-default", DWC3_APPLE_TUNABLE_GLOBALS },
	{ "apple,tunable-gbl-host", DWC3_APPLE_TUNABLE_GLOBALS },
	{ "apple,tunable-cio", DWC3_APPLE_TUNABLE_CIO },
	{ "apple,tunable-dbg", DWC3_APPLE_TUNABLE_DBG },
	{ "apple,tunable-cfg-host", DWC3_APPLE_TUNABLE_DRD_CFG },
	{ "apple,tunable-link", DWC3_APPLE_TUNABLE_LINK },
	{ "apple,tunable-ausbc", DWC3_APPLE_TUNABLE_DRD_AUSBC },
};

static_assert(ARRAY_SIZE(dwc3_apple_tunable_list) == DWC3_APPLE_NR_TUNABLES);

static void __iomem *dwc3_apple_tunable_base(struct dwc3_apple *appledwc,
					     enum dwc3_apple_tunable_block block)
{
	switch (block) {
	case DWC3_APPLE_TUNABLE_GLOBALS:
		return appledwc->dwc.regs;
	case DWC3_APPLE_TUNABLE_LINK:
		return appledwc->apple_regs + DWC3_APPLE_LINK_REGS_START - APPLE_DWC3_REGS_START;
	case DWC3_APPLE_TUNABLE_DBG:
		return appledwc->apple_regs + DWC3_APPLE_DBG_REGS_START - APPLE_DWC3_REGS_START;
	case DWC3_APPLE_TUNABLE_CIO:
		return appledwc->apple_regs + DWC3_APPLE_CIO_REGS_START - APPLE_DWC3_REGS_START;
	case DWC3_APPLE_TUNABLE_DRD_CFG:
	case DWC3_APPLE_TUNABLE_DRD_BULK:
	case DWC3_APPLE_TUNABLE_DRD_AUSBC:
		return appledwc->drd_regs[block - DWC3_APPLE_TUNABLE_DRD_CFG];
	case DWC3_APPLE_NR_TUNABLE_BLOCKS:
		break;
	}
	return NULL;
}

static void dwc3_apple_apply_tunables(struct dwc3_apple *appledwc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dwc3_apple_tunable_list); i++)
		apple_tunable_apply(dwc3_apple_tunable_base(appledwc,
							    dwc3_apple_tunable_list[i].block),
				    appledwc->tunables[i]);
}

/* The tunables are optional; a malformed one fails the probe. */
static int dwc3_apple_parse_tunables(struct dwc3_apple *appledwc, struct platform_device *pdev)
{
	struct resource *core = appledwc->mmio_resource;
	/* A block the device tree does not describe stays empty, and its table is refused. */
	struct resource blocks[DWC3_APPLE_NR_TUNABLE_BLOCKS] = {
		[DWC3_APPLE_TUNABLE_GLOBALS] = DEFINE_RES_MEM(core->start + DWC3_GLOBALS_REGS_START,
							      DWC3_GLOBALS_REGS_END -
							      DWC3_GLOBALS_REGS_START + 1),
		[DWC3_APPLE_TUNABLE_LINK] = DEFINE_RES_MEM(core->start + DWC3_APPLE_LINK_REGS_START,
							   DWC3_APPLE_LINK_DBG_SIZE),
		[DWC3_APPLE_TUNABLE_DBG] = DEFINE_RES_MEM(core->start + DWC3_APPLE_DBG_REGS_START,
							  DWC3_APPLE_LINK_DBG_SIZE),
		[DWC3_APPLE_TUNABLE_CIO] = DEFINE_RES_MEM(core->start + DWC3_APPLE_CIO_REGS_START,
							  APPLE_DWC3_REGS_END -
							  DWC3_APPLE_CIO_REGS_START + 1),
	};
	struct device *dev = appledwc->dev;
	int i;

	for (i = 0; i < DWC3_APPLE_NR_DRD_REGS; i++) {
		struct resource *res;

		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   dwc3_apple_drd_reg_names[i]);
		if (!res)
			continue;
		appledwc->drd_regs[i] = devm_ioremap_resource(dev, res);
		if (IS_ERR(appledwc->drd_regs[i]))
			return PTR_ERR(appledwc->drd_regs[i]);
		blocks[DWC3_APPLE_TUNABLE_DRD_CFG + i] = *res;
	}

	for (i = 0; i < ARRAY_SIZE(dwc3_apple_tunable_list); i++) {
		const char *prop = dwc3_apple_tunable_list[i].prop;
		struct apple_tunable *t;

		if (!of_property_present(dev->of_node, prop))
			continue;
		t = devm_apple_tunable_parse(dev, dev->of_node, prop,
					     &blocks[dwc3_apple_tunable_list[i].block]);
		if (IS_ERR(t))
			return dev_err_probe(dev, PTR_ERR(t), "Invalid %s\n", prop);
		appledwc->tunables[i] = t;
	}
	return 0;
}

static void dwc3_apple_set_ptrcap(struct dwc3_apple *appledwc, u32 mode)
{
	guard(spinlock_irqsave)(&appledwc->dwc.lock);
	dwc3_set_prtcap(&appledwc->dwc, mode, false);
}

/* Written to the core's GUID scratch register before a sleep in place, read back after it. */
#define DWC3_APPLE_SLEEP_GUID 0x534c5045

/* macOS waits up to 100 ms for this after the core's D0, and only logs a timeout. */
#define DWC3_APPLE_CFG_STATUS 0x2c
#define DWC3_APPLE_CFG_STATUS_PIPE_PHY_READY BIT(30)
#define DWC3_APPLE_PIPE_PHY_READY_US 100000

/*
 * The T6040 core came up against the dummy PHY: reset its USB3 PIPE side once the PIPE has
 * moved (USB2 is not affected).
 */
static void dwc3_apple_usb3_pipe_reset(struct dwc3_apple *appledwc)
{
	struct dwc3 *dwc = &appledwc->dwc;
	u32 reg;

	reg = dwc3_readl(dwc, DWC3_GUSB3PIPECTL(0));
	dwc3_writel(dwc, DWC3_GUSB3PIPECTL(0), reg | DWC3_GUSB3PIPECTL_PHYSOFTRST);
	usleep_range(1000, 1500);
	dwc3_writel(dwc, DWC3_GUSB3PIPECTL(0), reg & ~DWC3_GUSB3PIPECTL_PHYSOFTRST);
	msleep(100);
}

/*
 * The core is powered but its registers are at their reset values. Set it up again the way
 * dwc3_apple_core_start() and dwc3_apple_set_role() do, in place: the PHYs and the xHCI
 * device stay, so xHCI can restore its saved state, or start over. Runs in the resume,
 * while nothing else touches the core.
 *
 * With @phy_down the PHY held the core in reset with its own sides down (T6040). The phys'
 * power counts never dropped, so dwc3_core_init() powers nothing on: the PHY brings USB2 and
 * USB3 back itself, after the core's init as at a start, where they come up inside it.
 */
static int dwc3_apple_core_reinit(struct dwc3_apple *appledwc, bool phy_down)
{
	struct dwc3 *dwc = &appledwc->dwc;
	int i, ret;

	ret = dwc3_core_init(dwc);
	if (ret)
		return ret;
	/* dwc3_core_init() took PHY references the core holds already. */
	for (i = 0; i < dwc->num_usb3_ports; i++) {
		phy_power_off(dwc->usb3_generic_phy[i]);
		phy_exit(dwc->usb3_generic_phy[i]);
	}
	for (i = 0; i < dwc->num_usb2_ports; i++) {
		phy_power_off(dwc->usb2_generic_phy[i]);
		phy_exit(dwc->usb2_generic_phy[i]);
	}
	if (phy_down) {
		ret = apple_typec_phy_sleep_power_up(appledwc->usb2_phy);
		if (ret)
			return ret;
	}

	dwc3_apple_setup_cio(appledwc);
	dwc3_apple_apply_tunables(appledwc);
	dwc3_apple_setup_hibernation(appledwc);
	dwc3_apple_set_ptrcap(appledwc, DWC3_GCTL_PRTCAP_HOST);
	dwc->dis_u3_susphy_quirk = appledwc->dt_dis_u3_susphy;
	dwc3_enable_susphy(dwc, true);
	/* A PHY that came back in a USB3 mode routes the PIPE to it now, before xHCI starts. */
	ret = phy_set_mode(appledwc->usb3_phy, PHY_MODE_USB_HOST);
	if (ret)
		dev_warn(appledwc->dev, "USB3 PHY setup failed after the sleep, USB2 only, err=%d\n",
			 ret);
	else if (phy_down && appledwc->usb3_phy &&
		 of_device_is_compatible(appledwc->dev->of_node, "apple,t6040-dwc3"))
		dwc3_apple_usb3_pipe_reset(appledwc);

	return 0;
}

/*
 * Runs before xHCI touches the core after a sleep in place. A core that may be unpowered must
 * be left alone, as an access could be an SError: dwc3_apple_sleep_kept() deals with it once
 * the system is back. A core that came back without its registers is set up again. xHCI then
 * restores its saved state, as macOS's does, unless the always-on side lost power too: then
 * it's told it lost its state (a positive return). A sleep refused before the hibernation
 * left the core as it was.
 *
 * A PHY that held the core in reset for the sleep (T6040) has released it by now and comes
 * back with the core, in dwc3_apple_core_reinit(): xHCI always starts over then.
 * dwc3_apple_sleep_wake_plan() has the cases.
 */
static int dwc3_apple_pre_host_resume(struct dwc3 *dwc)
{
	struct dwc3_apple *appledwc = to_dwc3_apple(dwc);
	bool kept = false;
	int phy, ret;
	u32 val;

	if (!READ_ONCE(appledwc->sleep_in_place))
		return 0;

	phy = apple_typec_phy_sleep_result(appledwc->usb2_phy);
	if (dwc3_apple_sleep_reads_mark(phy)) {
		if (appledwc->drd_regs[0] &&	/* drd-cfg */
		    readl_poll_timeout(appledwc->drd_regs[0] + DWC3_APPLE_CFG_STATUS, val,
				       val & DWC3_APPLE_CFG_STATUS_PIPE_PHY_READY, 100,
				       DWC3_APPLE_PIPE_PHY_READY_US))
			dev_warn(appledwc->dev, "PIPE PHY not ready after the sleep: %#x\n", val);
		kept = dwc3_readl(dwc, DWC3_GUID) == DWC3_APPLE_SLEEP_GUID;
	}

	switch (dwc3_apple_sleep_wake_plan(phy, kept)) {
	case DWC3_APPLE_WAKE_AS_IS:
		return 0;
	case DWC3_APPLE_WAKE_RESTORE:
		ret = dwc3_apple_core_reinit(appledwc, false);
		if (ret)
			break;
		appledwc->sleep_reinit = true;
		dev_dbg(appledwc->dev,
			"core lost its registers in the sleep: set up again, xHCI restores its state\n");
		return 0;
	case DWC3_APPLE_WAKE_START_OVER:
		ret = dwc3_apple_core_reinit(appledwc, false);
		if (ret)
			break;
		appledwc->sleep_reinit = true;
		dev_dbg(appledwc->dev,
			"core and its always-on side lost power in the sleep: set up again, xHCI starts over\n");
		return 1;
	case DWC3_APPLE_WAKE_PHY_START_OVER:
		ret = dwc3_apple_core_reinit(appledwc, true);
		if (ret)
			break;
		appledwc->sleep_reinit = true;
		dev_info(appledwc->dev,
			 "host kept through the sleep: core and PHY set up again, xHCI starts over\n");
		return 1;
	case DWC3_APPLE_WAKE_FAILED:
	default:
		ret = phy < 0 ? phy : -EINVAL;
		break;
	}
	dev_err(appledwc->dev, "not resuming xHCI, the core didn't come back: %d\n", ret);

	return ret;
}

static const struct dwc3_glue_ops dwc3_apple_glue_ops = {
	.pre_host_resume = dwc3_apple_pre_host_resume,
};

static bool dwc3_apple_host_before_core_init(struct dwc3_apple *appledwc,
					     enum dwc3_apple_state state)
{
	return state == DWC3_APPLE_HOST && apple_t8132_usb_host_first() &&
		of_device_is_compatible(appledwc->dev->of_node, "apple,t8132-dwc3");
}

static int dwc3_apple_core_probe(struct dwc3_apple *appledwc, enum dwc3_apple_state state)
{
	struct dwc3_probe_data probe_data = {};
	void *group;
	int ret;

	lockdep_assert_held(&appledwc->lock);
	WARN_ON_ONCE(appledwc->state != DWC3_APPLE_PROBE_PENDING);

	appledwc->dwc.dev = appledwc->dev;
	appledwc->dwc.glue_ops = &dwc3_apple_glue_ops;
	probe_data.dwc = &appledwc->dwc;
	probe_data.res = appledwc->mmio_resource;
	probe_data.ignore_clocks_and_resets = true;
	probe_data.skip_core_init_mode = true;
	if (dwc3_apple_host_before_core_init(appledwc, state))
		probe_data.initial_role = USB_ROLE_HOST;
	probe_data.properties = DWC3_DEFAULT_PROPERTIES;
	/*
	 * The controller has no wake signal of its own, and a halted xHC writes no events, so its
	 * interrupt cannot fire either. xHCI keeps running instead and decides which of its events
	 * wake the system; the core stays powered across system sleep, see dwc3_apple_init().
	 */
	probe_data.properties.xhci_run_in_s2idle = true;

	/*
	 * This runs from the role switch callback, so a failure leaves the glue
	 * device bound with everything dwc3_core_probe() acquired through devres
	 * still attached, the request of the core register region included. The
	 * next cable event retries dwc3_core_probe() on this struct and would fail
	 * with -EBUSY on that region. Group the devres of one attempt so that a
	 * failed attempt can be released.
	 */
	group = devres_open_group(appledwc->dev, NULL, GFP_KERNEL);
	if (!group)
		return -ENOMEM;

	ret = dwc3_core_probe(&probe_data);
	/* The glue remains bound even when a first core attempt unwinds. */
	dev_set_drvdata(appledwc->dev, &appledwc->dwc);
	if (ret) {
		devres_release_group(appledwc->dev, group);
		/*
		 * dwc3_core_probe() initialised the locks, work items and lists of
		 * the embedded struct dwc3. The retry expects a zeroed struct, as
		 * on the first attempt.
		 */
		memset(&appledwc->dwc, 0, sizeof(appledwc->dwc));
		return ret;
	}
	devres_remove_group(appledwc->dev, group);

	/*
	 * dwc3_core_probe() enabled runtime PM with the device active and forbade it, for the
	 * dwc3 platform driver's own callbacks, which this glue does not use. The core is being
	 * brought up now, so take the glue's reference (see dwc3_apple_core_start()) and, where
	 * that is tested, allow runtime PM so the reference alone decides.
	 */
	pm_runtime_get_noresume(appledwc->dev);
	appledwc->rpm_held = true;
	if (appledwc->idle_rpm)
		pm_runtime_allow(appledwc->dev);

	appledwc->state = DWC3_APPLE_NO_CABLE;
	return 0;
}

static int dwc3_apple_core_init(struct dwc3_apple *appledwc, enum dwc3_apple_state state)
{
	int ret;

	lockdep_assert_held(&appledwc->lock);

	switch (appledwc->state) {
	case DWC3_APPLE_PROBE_PENDING:
		ret = dwc3_apple_core_probe(appledwc, state);
		if (ret)
			dev_err(appledwc->dev, "Failed to probe DWC3 Core, err=%d\n", ret);
		break;
	case DWC3_APPLE_NO_CABLE:
		/* Physical reset also loses the role on a later cable session. */
		if (dwc3_apple_host_before_core_init(appledwc, state)) {
			appledwc->dwc.dr_mode = USB_DR_MODE_HOST;
			dwc3_apple_set_ptrcap(appledwc, DWC3_GCTL_PRTCAP_HOST);
		}
		ret = dwc3_core_init(&appledwc->dwc);
		if (ret)
			dev_err(appledwc->dev, "Failed to initialize DWC3 Core, err=%d\n", ret);
		break;
	default:
		/* Unreachable unless there's a bug in this driver */
		WARN_ON_ONCE(1);
		ret = -EINVAL;
		break;
	}

	return ret;
}

/* Keep failed releases owned by the glue, without repeating completed teardown. */
static int dwc3_apple_core_stop(struct dwc3_apple *appledwc)
{
	struct dwc3 *dwc = &appledwc->dwc;
	int i, ret;

	lockdep_assert_held(&appledwc->lock);
	if (appledwc->core_active) {
		if (!appledwc->core_exit_pending) {
			/* The PHY hands the PIPE back in P3, as it took it over */
			dwc->dis_u3_susphy_quirk = appledwc->dt_dis_u3_susphy;
			dwc3_enable_susphy(dwc, true);
			dwc3_event_buffers_cleanup(dwc);
			appledwc->core_exit_pending = true;
		}
		for (i = 0; i < dwc->num_usb3_ports; i++) {
			if (!(appledwc->usb3_power_off_pending & BIT(i)))
				continue;
			ret = phy_power_off(dwc->usb3_generic_phy[i]);
			if (ret)
				return ret;
			appledwc->usb3_power_off_pending &= ~BIT(i);
		}
		for (i = 0; i < dwc->num_usb2_ports; i++) {
			if (!(appledwc->usb2_power_off_pending & BIT(i)))
				continue;
			ret = phy_power_off(dwc->usb2_generic_phy[i]);
			if (ret)
				return ret;
			appledwc->usb2_power_off_pending &= ~BIT(i);
		}
		usb_phy_set_suspend(dwc->usb3_phy, 1);
		usb_phy_set_suspend(dwc->usb2_phy, 1);
		dwc3_core_exit_after_phy_power_off(dwc);
		appledwc->core_active = false;
		appledwc->core_exit_pending = false;
	}
	if (appledwc->reset_pending) {
		ret = reset_control_assert(appledwc->reset);
		if (ret)
			return ret;
		appledwc->reset_pending = false;
	}
	dev_pm_syscore_device(appledwc->dev, false);
	if (appledwc->rpm_held) {
		appledwc->rpm_held = false;
		pm_runtime_mark_last_busy(appledwc->dev);
		pm_runtime_put_autosuspend(appledwc->dev);
	}

	return 0;
}

/*
 * Bring the core up for the given role: configure the USB2 PHY, release the reset and probe
 * (on the first call) or initialise the core.
 */
static int dwc3_apple_core_start(struct dwc3_apple *appledwc, enum dwc3_apple_state state)
{
	enum phy_mode mode;
	int ret, ret_reset;

	lockdep_assert_held(&appledwc->lock);

	/* A failed exit must finish before any new PHY reference is acquired. */
	ret = dwc3_apple_core_stop(appledwc);
	if (ret)
		return ret;

	/*
	 * The USB2 PHY on this platform must be configured for device mode before dwc3 tries to
	 * access it. Otherwise, the new configuration will sometimes only take affect after the
	 * *next* time dwc3 is brought up which causes the connected device to just not work.
	 * Host mode is left to the xHCI root hub, which selects it once the core is out of reset
	 * (usb_add_hcd() asks for PHY_MODE_USB_HOST_SS, which the PHY rejects, and then for
	 * PHY_MODE_USB_HOST). On a Type-C port the PHY is already running at this point, because
	 * the Type-C mux starts it before the role switch reaches this driver, and switching the
	 * running PHY to host mode while the core is still held in reset leaves the USB2 port
	 * without a connection for the whole session. A PHY that is in host mode already needs no
	 * selection here either.
	 * The USB3 PHY must be configured later after dwc3 has already been initialized.
	 * Both PHYs were looked up at probe, so device mode is also selected on the first
	 * bring-up, before dwc3_core_probe() has looked them up itself.
	 */
	ret = dwc3_apple_usb2_mode_before_reset(state, &mode);
	if (ret) {
		/* Unreachable unless there's a bug in this driver */
		return ret;
	}
	/* T8132 latches the host role while its USB2 PHY is stopped. */
	if (of_device_is_compatible(appledwc->dev->of_node, "apple,t8132-dwc3") &&
	    state == DWC3_APPLE_HOST)
		mode = PHY_MODE_USB_HOST;
	if (mode != PHY_MODE_INVALID) {
		ret = phy_set_mode(appledwc->usb2_phy, mode);
		if (ret) {
			if (of_device_is_compatible(appledwc->dev->of_node, "apple,t8132-dwc3"))
				return ret;
			dev_warn(appledwc->dev, "Failed to set the USB2 PHY mode, err=%d\n", ret);
		}
	}

	/*
	 * Power an idle port's controller up for its first core probe, then leave runtime PM
	 * to dwc3_core_probe(), which expects it disabled with the device active.
	 */
	if (appledwc->pre_core_rpm) {
		ret = pm_runtime_resume_and_get(appledwc->dev);
		if (ret)
			return ret;
		pm_runtime_disable(appledwc->dev);
		pm_runtime_put_noidle(appledwc->dev);
		appledwc->pre_core_rpm = false;
	}

	/*
	 * Before the first core probe the controller is powered, with runtime PM off or handed
	 * over above; dwc3_apple_core_probe() takes the reference then.
	 */
	if (appledwc->state != DWC3_APPLE_PROBE_PENDING && !appledwc->rpm_held) {
		ret = pm_runtime_resume_and_get(appledwc->dev);
		if (ret)
			return ret;
		appledwc->rpm_held = true;
	}

	ret = reset_control_deassert(appledwc->reset);
	if (ret) {
		dev_err(appledwc->dev, "Failed to deassert reset, err=%d\n", ret);
		/* Nothing was started: drop the reference again. */
		ret_reset = dwc3_apple_core_stop(appledwc);
		if (ret_reset)
			dev_warn(appledwc->dev, "Failed to finish core shutdown, err=%d\n", ret_reset);
		return ret;
	}

	appledwc->reset_pending = true;
	/* Keep register access powered until both PHY shutdown and reset finish. */
	dev_pm_syscore_device(appledwc->dev, true);
	ret = dwc3_apple_core_init(appledwc, state);
	if (ret) {
		ret_reset = dwc3_apple_core_stop(appledwc);
		if (ret_reset)
			dev_warn(appledwc->dev, "Failed to finish core shutdown, err=%d\n", ret_reset);
		return ret;
	}

	/*
	 * Now that the core is initialized and already went through dwc3_core_soft_reset we can
	 * configure some unknown Apple-specific settings and then bring up xhci or gadget mode.
	 */
	/* Native J713 host reset uses the same CIO setup before xHCI starts. */
	if (!of_device_is_compatible(appledwc->dev->of_node, "apple,t8132-dwc3") ||
	    dwc3_apple_host_before_core_init(appledwc, state))
		dwc3_apple_setup_cio(appledwc);
	dwc3_apple_apply_tunables(appledwc);
	dwc3_apple_setup_hibernation(appledwc);
	appledwc->core_active = true;
	appledwc->usb2_power_off_pending = BIT(appledwc->dwc.num_usb2_ports) - 1;
	appledwc->usb3_power_off_pending = BIT(appledwc->dwc.num_usb3_ports) - 1;

	return 0;
}

/* Select the role in the core and route the USB3 PHY, before xhci or the gadget is started */
static int dwc3_apple_set_role(struct dwc3_apple *appledwc, enum dwc3_apple_state state)
{
	bool host = state == DWC3_APPLE_HOST;
	int ret;

	lockdep_assert_held(&appledwc->lock);

	appledwc->dwc.dr_mode = host ? USB_DR_MODE_HOST : USB_DR_MODE_PERIPHERAL;
	dwc3_apple_set_ptrcap(appledwc, host ? DWC3_GCTL_PRTCAP_HOST : DWC3_GCTL_PRTCAP_DEVICE);
	/*
	 * This platform requires SUSPHY to be enabled here already in order to properly
	 * configure the PHY and switch dwc3's PIPE interface to USB3 PHY. The USB2 PHY's
	 * device mode and the T8132 host role were selected in dwc3_apple_core_start();
	 * other host roles are selected by the xHCI root hub as it starts.
	 */
	appledwc->dwc.dis_u3_susphy_quirk = appledwc->dt_dis_u3_susphy;
	dwc3_enable_susphy(&appledwc->dwc, true);
	ret = phy_set_mode(appledwc->usb3_phy, host ? PHY_MODE_USB_HOST : PHY_MODE_USB_DEVICE);
	if (ret) {
		/*
		 * A failed T8132 BIST or PIPE handoff must not start xHCI. Neither must a failed
		 * T6050 one: that PHY reports a port left on USB2 (four DisplayPort lanes) as
		 * success, and its errors leave the PIPE without a known producer.
		 */
		if (of_device_is_compatible(appledwc->dev->of_node, "apple,t8132-dwc3") ||
		    of_device_is_compatible(appledwc->dev->of_node, "apple,t6050-dwc3")) {
			dev_err(appledwc->dev, "USB3 PHY setup failed, err=%d\n", ret);
			return ret;
		}
		dev_warn(appledwc->dev, "USB3 PHY setup failed, USB2 only, err=%d\n", ret);
	} else if (host && appledwc->usb3_phy &&
		   of_device_is_compatible(appledwc->dev->of_node, "apple,t8132-dwc3")) {
		/*
		 * The T8132 PHY does not bring a SuperSpeed link back from U3 once
		 * the core has suspended it: keep it out of P3 from here on.
		 */
		appledwc->dwc.dis_u3_susphy_quirk = true;
		dwc3_enable_susphy(&appledwc->dwc, true);
	} else if (appledwc->usb3_phy &&
		   of_device_is_compatible(appledwc->dev->of_node, "apple,t6040-dwc3")) {
		dwc3_apple_usb3_pipe_reset(appledwc);
	}
	return 0;
}

/*
 * No device mode: the T6040 USB2 PHY (phy-apple-t6040-usb2) and the T6050 Type-C PHY implement
 * host mode only, and a kernel built without gadget support turns dwc3_gadget_init() into a stub
 * that succeeds. Such a controller must not follow a USB_ROLE_DEVICE request from the Type-C port
 * controller (a charger or a host on the other end), or the core would end up in device mode on
 * top of a PHY powered in host mode.
 */
static bool dwc3_apple_host_only(struct device *dev)
{
	if (!IS_ENABLED(CONFIG_USB_DWC3_GADGET) && !IS_ENABLED(CONFIG_USB_DWC3_DUAL_ROLE))
		return true;

	return of_device_is_compatible(dev->of_node, "apple,t6040-dwc3") ||
	       of_device_is_compatible(dev->of_node, "apple,t6050-dwc3");
}

static int dwc3_apple_init(struct dwc3_apple *appledwc, enum dwc3_apple_state state)
{
	int ret, ret_reset;

	lockdep_assert_held(&appledwc->lock);

	/*
	 * A host-only controller starts as a host: mark the role before the core comes up so that
	 * dwc3_core_soft_reset() leaves the unused gadget side alone. A failed first core probe
	 * zeroes the struct, so this is done on every start.
	 */
	if (state == DWC3_APPLE_HOST && appledwc->host_only)
		appledwc->dwc.current_dr_role = DWC3_GCTL_PRTCAP_HOST;

	ret = dwc3_apple_core_start(appledwc, state);
	if (ret)
		return ret;

	ret = dwc3_apple_set_role(appledwc, state);
	if (ret)
		goto core_exit;

	switch (state) {
	case DWC3_APPLE_HOST:
		ret = dwc3_host_init(&appledwc->dwc);
		if (ret) {
			dev_err(appledwc->dev, "Failed to initialize host, ret=%d\n", ret);
			goto core_exit;
		}
		/*
		 * A USB2 PHY with a port reset (the T6050 Type-C PHY) takes it once the root hub is
		 * up; for the other PHYs, which have none, this is a no-op.
		 */
		ret = phy_reset(appledwc->usb2_phy);
		if (ret) {
			dev_err(appledwc->dev, "Failed to reset the USB2 port, ret=%d\n", ret);
			dwc3_host_exit(&appledwc->dwc);
			goto core_exit;
		}
		break;
	case DWC3_APPLE_DEVICE:
		ret = dwc3_gadget_init(&appledwc->dwc);
		if (ret) {
			dev_err(appledwc->dev, "Failed to initialize gadget, ret=%d\n", ret);
			goto core_exit;
		}
		break;
	default:
		/* Unreachable unless there's a bug in this driver */
		WARN_ON_ONCE(1);
		ret = -EINVAL;
		goto core_exit;
	}

	appledwc->state = state;
	/* macOS votes USB_SLP_DDR from here until the host's Off, through every sleep. */
	if (state == DWC3_APPLE_HOST && appledwc->sleep_ddr)
		apple_sleep_usb_slp_ddr(true);
	/*
	 * A connected cable leaves the Apple DWC3 core and its PHY as one live,
	 * stateful unit.  The glue driver tears that unit down explicitly on a
	 * role change or disconnect.  Do not let generic system PM independently
	 * power-gate the DWC3 device in between: xHCI then observes a dead core on
	 * resume and cannot restore the root hubs without a full cable teardown.
	 * The same holds for a fixed-hub controller, whose core is up for as long
	 * as the driver is bound. As a syscore device the core is not counted as
	 * suspended in its power domain, so the domain stays on, and xHCI saves
	 * its state on suspend and restores it on resume.
	 */
	dev_pm_syscore_device(appledwc->dev, true);
	return 0;

core_exit:
	ret_reset = dwc3_apple_core_stop(appledwc);
	if (ret_reset)
		dev_warn(appledwc->dev, "Failed to finish core shutdown, err=%d\n", ret_reset);

	return ret;
}

static int dwc3_apple_exit(struct dwc3_apple *appledwc);

/* A hub that hasn't enumerated by then didn't come back. */
#define DWC3_APPLE_HUB_CHECK_MS 5000

/* Any way a fixed-hub host came out of a sleep, its hub must be there a little later. */
static void dwc3_apple_check_hub_later(struct dwc3_apple *appledwc)
{
	if (appledwc->fixed_hub)
		mod_delayed_work(system_freezable_wq, &appledwc->hub_check,
				 msecs_to_jiffies(DWC3_APPLE_HUB_CHECK_MS));
}

/*
 * Bring a controller up again after a sleep that stopped it or lost it. A fixed-hub host
 * starts as at probe, where the core has no role yet, so dwc3_core_soft_reset() runs too:
 * rebinding the driver brought the J700's hub back where a restart without it didn't.
 */
static int dwc3_apple_start_after_sleep(struct dwc3_apple *appledwc, enum dwc3_apple_state state)
{
	int ret;

	lockdep_assert_held(&appledwc->lock);
	if (appledwc->fixed_hub)
		appledwc->dwc.current_dr_role = 0;
	ret = dwc3_apple_init(appledwc, state);
	if (ret) {
		dev_err(appledwc->dev, "Failed to start after sleep, err=%d\n", ret);
		return ret;
	}
	dwc3_apple_check_hub_later(appledwc);
	return 0;
}

/* Whether the fixed hub is on its USB2 root port. Takes the xHCI device's lock. */
static bool dwc3_apple_hub_present(struct dwc3_apple *appledwc)
{
	struct platform_device *xhci = appledwc->dwc.xhci;
	struct usb_device *root;
	struct usb_hcd *hcd;
	bool present = true;

	lockdep_assert_held(&appledwc->lock);
	if (!xhci)
		return true;

	device_lock(&xhci->dev);
	hcd = xhci->dev.driver ? platform_get_drvdata(xhci) : NULL;
	root = hcd ? hcd->self.root_hub : NULL;
	if (root) {
		usb_lock_device(root);
		present = usb_hub_find_child(root, appledwc->hub_port);
		usb_unlock_device(root);
	}
	device_unlock(&xhci->dev);

	return present;
}

/* Whether @udev or a device below it can wake the system. Takes the hubs' locks. */
static bool dwc3_apple_can_wake(struct usb_device *udev)
{
	struct usb_device *child;
	bool wake = device_may_wakeup(&udev->dev);
	int port1;

	if (wake || !udev->maxchild)
		return wake;

	usb_lock_device(udev);
	usb_hub_for_each_child(udev, port1, child) {
		wake = dwc3_apple_can_wake(child);
		if (wake)
			break;
	}
	usb_unlock_device(udev);

	return wake;
}

/*
 * The fixed hub's ports with a device at or below them that can wake the system, bit n for
 * port n. The hub's own setting doesn't count. Takes the xHCI device's lock and the hubs'.
 */
static unsigned long dwc3_apple_hub_wake_ports(struct dwc3_apple *appledwc)
{
	struct platform_device *xhci = appledwc->dwc.xhci;
	struct usb_device *root, *hub, *child;
	unsigned long ports = 0;
	struct usb_hcd *hcd;
	int port1;

	lockdep_assert_held(&appledwc->lock);
	if (!xhci)
		return 0;

	device_lock(&xhci->dev);
	hcd = xhci->dev.driver ? platform_get_drvdata(xhci) : NULL;
	root = hcd ? hcd->self.root_hub : NULL;
	if (root) {
		usb_lock_device(root);
		hub = usb_hub_find_child(root, appledwc->hub_port);
		if (hub) {
			usb_lock_device(hub);
			usb_hub_for_each_child(hub, port1, child) {
				if (port1 < BITS_PER_LONG && dwc3_apple_can_wake(child))
					ports |= BIT(port1);
			}
			usb_unlock_device(hub);
		}
		usb_unlock_device(root);
	}
	device_unlock(&xhci->dev);

	return ports;
}

/*
 * Once after a sleep: if the hub isn't there, stop the host and start it again, with the
 * hub's link down for a moment first, as unbinding and binding the driver does. This also
 * catches an xHCI that didn't come back after the core was set up again.
 */
static void dwc3_apple_hub_check(struct work_struct *work)
{
	struct dwc3_apple *appledwc = container_of(to_delayed_work(work), struct dwc3_apple,
						   hub_check);
	int ret;

	guard(mutex)(&appledwc->lock);
	if (appledwc->shutting_down || appledwc->state != DWC3_APPLE_HOST ||
	    dwc3_apple_hub_present(appledwc))
		return;

	dev_warn(appledwc->dev, "hub not back after the sleep: starting the host again\n");
	ret = dwc3_apple_exit(appledwc);
	if (ret) {
		dev_err(appledwc->dev, "Failed to stop the host again, err=%d\n", ret);
		return;
	}
	msleep(500);
	appledwc->dwc.current_dr_role = 0;
	ret = dwc3_apple_init(appledwc, DWC3_APPLE_HOST);
	if (ret)
		dev_err(appledwc->dev, "Failed to start the host again, err=%d\n", ret);
}

static int dwc3_apple_exit(struct dwc3_apple *appledwc)
{
	lockdep_assert_held(&appledwc->lock);

	switch (appledwc->state) {
	case DWC3_APPLE_PROBE_PENDING:
	case DWC3_APPLE_NO_CABLE:
		break;
	case DWC3_APPLE_DEVICE:
		dwc3_gadget_exit(&appledwc->dwc);
		appledwc->state = DWC3_APPLE_NO_CABLE;
		break;
	case DWC3_APPLE_HOST:
		/* dwc3_host_exit() lets the USB3 PHY into P3 for the PIPE handback */
		appledwc->dwc.dis_u3_susphy_quirk = appledwc->dt_dis_u3_susphy;
		dwc3_host_exit(&appledwc->dwc);
		appledwc->state = DWC3_APPLE_NO_CABLE;
		if (appledwc->sleep_ddr)
			apple_sleep_usb_slp_ddr(false);
		break;
	}

	return dwc3_apple_core_stop(appledwc);
}

static int dwc3_usb_role_switch_set(struct usb_role_switch *sw, enum usb_role role)
{
	struct dwc3_apple *appledwc = usb_role_switch_get_drvdata(sw);
	int ret;

	guard(mutex)(&appledwc->lock);

	if (appledwc->shutting_down)
		return -ESHUTDOWN;

	/* A controller behind a fixed hub is a host regardless of the connectors */
	if (appledwc->fixed_hub)
		return 0;

	/* Stopped for a sleep: this is what comes back after it. */
	if (appledwc->sleep_stopped) {
		appledwc->sleep_state = dwc3_apple_state_for_role(role);
		return 0;
	}

	/*
	 * Kept through a sleep, with the core in reset for some of it: nothing stops or starts
	 * until the sleep is over, see dwc3_apple_sleep_restart_end(). Nor while the core may
	 * be unpowered after one: dwc3_apple_recover() starts what is asked for here.
	 */
	if (appledwc->sleep_in_place || appledwc->sleep_dead) {
		appledwc->sleep_state = dwc3_apple_state_for_role_kept(role, appledwc->host_only);
		return 0;
	}

	/*
	 * Stay off instead of starting device mode (see dwc3_apple_host_only()). Reporting success
	 * keeps the port controller from retrying: get() then reports USB_ROLE_NONE.
	 */
	if (role == USB_ROLE_DEVICE && appledwc->host_only) {
		dev_info_ratelimited(appledwc->dev, "No device mode on this port: staying off\n");
		role = USB_ROLE_NONE;
	}

	/*
	 * Skip role switches if appledwc is already in the desired state. The
	 * USB-C port controller on M2 and M1/M2 Pro/Max/Ultra devices issues
	 * additional interrupts which results in usb_role_switch_set_role()
	 * calls with the current role.
	 * Ignore those calls here to ensure the USB-C port controller and
	 * appledwc are in a consistent state.
	 * This matches the behaviour in __dwc3_set_mode().
	 * Do no handle USB_ROLE_NONE for DWC3_APPLE_NO_CABLE and
	 * DWC3_APPLE_PROBE_PENDING since that is no-op anyway.
	 */
	if (appledwc->state == DWC3_APPLE_HOST && role == USB_ROLE_HOST)
		return 0;
	if (appledwc->state == DWC3_APPLE_DEVICE && role == USB_ROLE_DEVICE)
		return 0;

	/*
	 * We need to tear all of dwc3 down and re-initialize it every time a cable is
	 * connected or disconnected or when the mode changes. See the documentation for enum
	 * dwc3_apple_state for details.
	 */
	ret = dwc3_apple_exit(appledwc);
	if (ret)
		return ret;

	switch (role) {
	case USB_ROLE_NONE:
		/* Nothing to do if no cable is connected */
		return 0;
	case USB_ROLE_HOST:
		return dwc3_apple_init(appledwc, DWC3_APPLE_HOST);
	case USB_ROLE_DEVICE:
		return dwc3_apple_init(appledwc, DWC3_APPLE_DEVICE);
	default:
		dev_err(appledwc->dev, "Invalid target role: %d\n", role);
		return -EINVAL;
	}
}

static enum usb_role dwc3_usb_role_switch_get(struct usb_role_switch *sw)
{
	struct dwc3_apple *appledwc = usb_role_switch_get_drvdata(sw);

	guard(mutex)(&appledwc->lock);

	switch (appledwc->sleep_stopped || (appledwc->sleep_restart &&
					    (appledwc->sleep_in_place || appledwc->sleep_dead)) ?
		appledwc->sleep_state : appledwc->state) {
	case DWC3_APPLE_HOST:
		return USB_ROLE_HOST;
	case DWC3_APPLE_DEVICE:
		return USB_ROLE_DEVICE;
	case DWC3_APPLE_NO_CABLE:
	case DWC3_APPLE_PROBE_PENDING:
		return USB_ROLE_NONE;
	default:
		/* Unreachable unless there's a bug in this driver */
		dev_err(appledwc->dev, "Invalid internal state: %d\n", appledwc->state);
		return USB_ROLE_NONE;
	}
}

static int dwc3_apple_setup_role_switch(struct dwc3_apple *appledwc)
{
	struct usb_role_switch_desc dwc3_role_switch = { NULL };

	dwc3_role_switch.fwnode = dev_fwnode(appledwc->dev);
	dwc3_role_switch.set = dwc3_usb_role_switch_set;
	dwc3_role_switch.get = dwc3_usb_role_switch_get;
	dwc3_role_switch.driver_data = appledwc;
	appledwc->role_sw = usb_role_switch_register(appledwc->dev, &dwc3_role_switch);
	if (IS_ERR(appledwc->role_sw))
		return PTR_ERR(appledwc->role_sw);

	return 0;
}

/*
 * What a host lost in a sleep in place comes back as, once it is stopped: a host, or on a
 * Type-C port whatever the port controller has asked for since the sleep began.
 */
static void dwc3_apple_restart_lost(struct dwc3_apple *appledwc)
{
	enum dwc3_apple_state state = appledwc->sleep_restart ? appledwc->sleep_state :
								DWC3_APPLE_HOST;

	lockdep_assert_held(&appledwc->lock);
	if (state == DWC3_APPLE_HOST || state == DWC3_APPLE_DEVICE)
		dwc3_apple_start_after_sleep(appledwc, state);
}

/*
 * A core that may be unpowered gets one more power cycle, as macOS's Off and cold start, and
 * the host starts over once it's back. Until then nothing may touch it, and every sleep is
 * refused, as xHCI's suspend would touch it. Returns whether it's back.
 */
static bool dwc3_apple_recover(struct dwc3_apple *appledwc)
{
	int ret;

	lockdep_assert_held(&appledwc->lock);
	ret = apple_typec_phy_sleep_recover(appledwc->usb2_phy);
	if (ret < 0) {
		if (!appledwc->sleep_dead)
			dev_err(appledwc->dev,
				"USB host unpowered after the sleep (%d): USB off, sleeps refused until it's back or a reboot\n",
				ret);
		appledwc->sleep_dead = true;
		return false;
	}
	if (appledwc->sleep_dead)
		dev_info(appledwc->dev, "USB host back after a power cycle: restarting it\n");
	appledwc->sleep_dead = false;
	ret = dwc3_apple_exit(appledwc);
	if (ret)
		dev_err(appledwc->dev, "Failed to stop after the power cycle, err=%d\n", ret);
	else
		dwc3_apple_restart_lost(appledwc);

	return true;
}

/*
 * After a sleep in place: the PHY says whether the core came back to D0, and the GUID register
 * whether it kept its registers. A core set up again at the resume kept its host and devices,
 * which reset-resumed. Otherwise restart the host as after a stop, and the devices enumerate
 * again. A core that may be unpowered gets another power cycle first. Returns whether the
 * host was kept.
 */
static bool dwc3_apple_sleep_kept(struct dwc3_apple *appledwc)
{
	struct dwc3 *dwc = &appledwc->dwc;
	u32 guid = 0;
	int ret;

	appledwc->sleep_in_place = false;
	ret = apple_typec_phy_sleep_result(appledwc->usb2_phy);
	if (ret == -ENODEV) {
		dwc3_apple_recover(appledwc);
		return false;
	}
	if (appledwc->sleep_reinit) {
		appledwc->sleep_reinit = false;
		dev_dbg(appledwc->dev, "host kept through the sleep, set up again\n");
		dwc3_apple_check_hub_later(appledwc);
		return true;
	}
	if (dwc3_apple_sleep_reads_mark(ret)) {
		guid = dwc3_readl(dwc, DWC3_GUID);
		if (guid == DWC3_APPLE_SLEEP_GUID) {
			dwc3_writel(dwc, DWC3_GUID, appledwc->sleep_guid);
			dev_dbg(appledwc->dev, "host kept through the sleep\n");
			dwc3_apple_check_hub_later(appledwc);
			return true;
		}
		ret = -EIO;
		dev_warn(appledwc->dev,
			 "host lost its state in the sleep (%d, GUID %#x): restarting it\n",
			 ret, guid);
	} else {
		/* The mark wasn't read: what the PHY says, or the set-up after it, is why. */
		dev_warn(appledwc->dev,
			 "host not set up again after the sleep (PHY %d): restarting it\n", ret);
	}
	ret = dwc3_apple_exit(appledwc);
	if (ret)
		dev_err(appledwc->dev, "Failed to stop after sleep, err=%d\n", ret);
	else
		dwc3_apple_restart_lost(appledwc);

	return false;
}

/*
 * Whether xHCI is suspended: its controller halted, and nothing of its touching the core.
 * An xHCI device without its driver touches nothing either. Takes the xHCI device's lock.
 */
static bool dwc3_apple_xhci_suspended(struct dwc3_apple *appledwc)
{
	struct platform_device *xhci = appledwc->dwc.xhci;
	struct usb_hcd *hcd;
	bool suspended;

	lockdep_assert_held(&appledwc->lock);
	if (!xhci)
		return true;

	device_lock(&xhci->dev);
	hcd = xhci->dev.driver ? platform_get_drvdata(xhci) : NULL;
	suspended = !hcd || !HCD_HW_ACCESSIBLE(hcd);
	device_unlock(&xhci->dev);

	return suspended;
}

/* After a sleep: still suspended, and not by runtime PM, so its resume failed. */
static bool dwc3_apple_xhci_resume_failed(struct dwc3_apple *appledwc)
{
	struct platform_device *xhci = appledwc->dwc.xhci;

	return xhci && !pm_runtime_suspended(&xhci->dev) && dwc3_apple_xhci_suspended(appledwc);
}

/*
 * A Type-C port's host is kept through this sleep, by its PHY (T6040): see the comment at the
 * top. From here to dwc3_apple_sleep_restart_end() the glue's device is an ordinary one for
 * the PM core, which then runs dwc3_apple_suspend() after xHCI's suspend, lets the power
 * domain go off and orders xHCI's resume after the DARTs' and the PHY's. xHCI has to halt
 * for it in any state the sleep ends up in, so it can't be kept running for a USB wakeup.
 */
static void dwc3_apple_sleep_restart_begin(struct dwc3_apple *appledwc)
{
	struct platform_device *xhci = appledwc->dwc.xhci;

	lockdep_assert_held(&appledwc->lock);
	appledwc->sleep_state = appledwc->state;
	appledwc->sleep_xhci_wakeup = xhci && device_may_wakeup(&xhci->dev);
	if (appledwc->sleep_xhci_wakeup)
		device_set_wakeup_enable(&xhci->dev, false);
	dev_pm_syscore_device(appledwc->dev, false);
}

/*
 * The end of such a sleep, with @kept from dwc3_apple_sleep_kept(), which has restarted a
 * host that was lost. A host that was kept is the glue's to keep powered again. If xHCI
 * didn't resume on it, or the port controller asked for another role meanwhile, it stops
 * now, and what was asked for starts. Returns whether the host is still the one kept.
 */
static bool dwc3_apple_sleep_restart_end(struct dwc3_apple *appledwc, bool kept)
{
	struct platform_device *xhci = appledwc->dwc.xhci;
	enum dwc3_apple_state state = appledwc->sleep_state;
	int ret;

	lockdep_assert_held(&appledwc->lock);
	if (!kept || appledwc->state != DWC3_APPLE_HOST)
		return false;

	dev_pm_syscore_device(appledwc->dev, true);
	if (xhci && appledwc->sleep_xhci_wakeup)
		device_set_wakeup_enable(&xhci->dev, true);
	if (state == DWC3_APPLE_HOST && !dwc3_apple_xhci_resume_failed(appledwc))
		return true;

	if (state == DWC3_APPLE_HOST)
		dev_warn(appledwc->dev, "xHCI did not resume after the sleep: restarting the host\n");
	else
		dev_info(appledwc->dev, "role changed during the sleep: stopping the host kept\n");
	ret = dwc3_apple_exit(appledwc);
	if (ret)
		dev_err(appledwc->dev, "Failed to stop after sleep, err=%d\n", ret);
	else
		dwc3_apple_restart_lost(appledwc);

	return false;
}

/*
 * Runs before the freezer, when the sleep's real target isn't known yet, so this goes by the
 * state "mem" means now. Writing "freeze" with deep selected costs a USB restart and nothing else.
 * A stop that fails refuses the sleep. PM_POST_SUSPEND follows an abandoned sleep as well.
 */
static int dwc3_apple_pm_notify_locked(struct dwc3_apple *appledwc, unsigned long action,
				       bool in_place, bool *finish, bool *kept)
{
	struct device *dev = appledwc->dev;
	int ret;

	lockdep_assert_held(&appledwc->lock);

	switch (action) {
	case PM_SUSPEND_PREPARE:
		/* Any sleep: xHCI's suspend would touch a core that may be unpowered. */
		if (appledwc->sleep_dead && !appledwc->shutting_down &&
		    !dwc3_apple_recover(appledwc)) {
			dev_err(dev, "refusing the sleep: the USB host is still unpowered\n");
			return notifier_from_errno(-EIO);
		}
		if (pm_suspend_default_s2idle() || appledwc->shutting_down) {
			*finish = in_place;
			*kept = true;
			break;
		}
		/* A Type-C port's role changed while its PHY was asked: stopped already. */
		if (in_place && appledwc->state != DWC3_APPLE_HOST) {
			in_place = false;
			*finish = true;
			*kept = true;
		}
		/* xHCI suspends and resumes as usual, and the wakeup setting stays. */
		if (in_place) {
			appledwc->sleep_guid = dwc3_readl(&appledwc->dwc, DWC3_GUID);
			dwc3_writel(&appledwc->dwc, DWC3_GUID, DWC3_APPLE_SLEEP_GUID);
			appledwc->sleep_in_place = true;
			/* On a Type-C port xHCI's wakeup is off for it: nothing there can wake. */
			if (appledwc->sleep_restart) {
				dwc3_apple_sleep_restart_begin(appledwc);
				break;
			}
			/* For the port controllers' USB wake. Changes after this go unseen. */
			apple_sleep_usb_set_wake_ports(dwc3_apple_hub_wake_ports(appledwc));
			break;
		}
		appledwc->sleep_state = appledwc->state;
		ret = dwc3_apple_exit(appledwc);
		if (ret)
			return notifier_from_errno(ret);
		/* A wakeup path would keep the domain on, and it's going away. */
		appledwc->sleep_wakeup = device_may_wakeup(dev);
		device_set_wakeup_enable(dev, false);
		appledwc->sleep_stopped = true;
		break;
	case PM_POST_SUSPEND:
		if (appledwc->sleep_in_place) {
			apple_sleep_usb_set_wake_ports(0);
			*kept = dwc3_apple_sleep_kept(appledwc);
			if (appledwc->sleep_restart)
				*kept = dwc3_apple_sleep_restart_end(appledwc, *kept);
			*finish = true;
			break;
		}
		if (!appledwc->sleep_stopped)
			break;
		appledwc->sleep_stopped = false;
		device_set_wakeup_enable(dev, appledwc->sleep_wakeup);
		if (appledwc->shutting_down ||
		    (appledwc->sleep_state != DWC3_APPLE_HOST &&
		     appledwc->sleep_state != DWC3_APPLE_DEVICE))
			break;
		dwc3_apple_start_after_sleep(appledwc, appledwc->sleep_state);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

/*
 * The PHY's sleep prepare and finish may take the SuperSpeed port down and up, under the
 * controller's device lock, so they run outside the glue's lock.
 */
static int dwc3_apple_pm_notify(struct notifier_block *nb, unsigned long action, void *unused)
{
	struct dwc3_apple *appledwc = container_of(nb, struct dwc3_apple, pm_nb);
	bool in_place = false, finish = false, kept = false;
	int ret;

	/*
	 * The last wake's SuperSpeed restore ends before this sleep looks at the PHY. The hub
	 * check takes the glue's lock, and has nothing to look at until the sleep is over.
	 */
	if (action == PM_SUSPEND_PREPARE) {
		apple_typec_phy_sleep_finish_flush(appledwc->usb2_phy);
		cancel_delayed_work_sync(&appledwc->hub_check);
	}
	if (action == PM_SUSPEND_PREPARE && !pm_suspend_default_s2idle()) {
		scoped_guard(mutex, &appledwc->lock)
			in_place = dwc3_apple_sleep_may_keep(appledwc->fixed_hub,
							     appledwc->sleep_restart,
							     appledwc->state,
							     appledwc->shutting_down,
							     appledwc->sleep_dead);
		in_place = in_place && !apple_typec_phy_sleep_prepare(appledwc->usb2_phy);
	}

	scoped_guard(mutex, &appledwc->lock)
		ret = dwc3_apple_pm_notify_locked(appledwc, action, in_place, &finish, &kept);

	/*
	 * After a wake, user space doesn't wait for SuperSpeed: it comes back in the background,
	 * from the PHY's work, which an unbind of this device doesn't wait for under its lock.
	 */
	if (finish && action == PM_POST_SUSPEND)
		apple_typec_phy_sleep_finish_async(appledwc->usb2_phy, kept);
	else if (finish)
		apple_typec_phy_sleep_finish(appledwc->usb2_phy, kept);

	return ret;
}

/*
 * Only for a Type-C port's host kept through a sleep (T6040, see the comment at the top):
 * otherwise the core is either down or the device a syscore one, which the PM core skips.
 * xHCI, the child, has suspended: its controller is halted and its state saved, for what
 * that's worth. The PHY now takes USB3 and USB2 down and the core into reset, in the order
 * of dwc3_apple_core_stop(), and nothing touches the core until dwc3_apple_pre_host_resume().
 * This may sleep, for the PHY's coprocessor, which is why it isn't a noirq callback.
 *
 * The power domain then goes off in the noirq phase like a stopped controller's, also with a
 * USB device below that may wake the system: no wakeup of a powered-down SoC comes through
 * the controller. An error refuses the sleep, with the PHY and the core as they were.
 */
static int dwc3_apple_suspend(struct device *dev)
{
	struct dwc3_apple *appledwc = to_dwc3_apple(dev_get_drvdata(dev));
	struct dwc3 *dwc = &appledwc->dwc;
	int ret;

	guard(mutex)(&appledwc->lock);
	if (!appledwc->sleep_in_place || !appledwc->sleep_restart)
		return 0;

	if (!dwc3_apple_xhci_suspended(appledwc)) {
		dev_err(dev, "xHCI still runs: refusing the sleep\n");
		return -EBUSY;
	}
	/* The PHY hands the PIPE back in P3, as it took it over */
	dwc->dis_u3_susphy_quirk = appledwc->dt_dis_u3_susphy;
	dwc3_enable_susphy(dwc, true);
	ret = apple_typec_phy_sleep_enter(appledwc->usb2_phy);
	if (ret) {
		dev_err(dev, "PHY did not take the host through the sleep: %d, refusing it\n", ret);
		return ret;
	}
	device_set_out_band_wakeup(dev);

	return 0;
}

static const struct dev_pm_ops dwc3_apple_pm_ops = {
	.suspend = pm_sleep_ptr(dwc3_apple_suspend),
};

static void dwc3_apple_pm_notifier_remove(void *data)
{
	struct dwc3_apple *appledwc = data;

	unregister_pm_notifier(&appledwc->pm_nb);
}

/* A fixed hub's node gives the root port it sits on in its reg, 1 if it doesn't. */
static bool dwc3_apple_has_fixed_hub(struct device *dev, u32 *port)
{
	struct device_node *hub __free(device_node) =
		of_get_available_child_by_name(dev->of_node, "hub");

	*port = 1;
	if (hub)
		of_property_read_u32(hub, "reg", port);

	return hub;
}

static int dwc3_apple_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dwc3_apple *appledwc;
	int ret;

	appledwc = devm_kzalloc(&pdev->dev, sizeof(*appledwc), GFP_KERNEL);
	if (!appledwc)
		return -ENOMEM;

	appledwc->dev = &pdev->dev;
	appledwc->fixed_hub = dwc3_apple_has_fixed_hub(dev, &appledwc->hub_port);
	appledwc->host_only = dwc3_apple_host_only(dev);
	appledwc->sleep_restart = !appledwc->fixed_hub &&
				  of_device_is_compatible(dev->of_node, "apple,t6040-dwc3");
	appledwc->dt_dis_u3_susphy = device_property_read_bool(dev, "snps,dis_u3_susphy_quirk");
	appledwc->sleep_ddr = device_property_read_bool(dev, "apple,sleep-ddr");
	/* The PHY driver and the DARTs follow runtime PM on these SoCs. */
	appledwc->idle_rpm = of_device_is_compatible(dev->of_node, "apple,t6040-dwc3") ||
			     of_device_is_compatible(dev->of_node, "apple,t8132-dwc3");
	mutex_init(&appledwc->lock);
	/* A never-connected port has no generic core probe to publish this. */
	appledwc->dwc.dev = dev;
	platform_set_drvdata(pdev, &appledwc->dwc);

	appledwc->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(appledwc->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(appledwc->reset),
				     "Failed to get reset control\n");

	appledwc->usb2_phy = devm_phy_get(dev, "usb2-phy");
	if (IS_ERR(appledwc->usb2_phy))
		return dev_err_probe(dev, PTR_ERR(appledwc->usb2_phy),
				     "Failed to get the USB2 PHY\n");

	/*
	 * Older T8132 device trees and the T6040 ports without a USB3 PHY
	 * expose only USB2; a NULL PHY is a no-op.
	 */
	if (of_device_is_compatible(dev->of_node, "apple,t8132-dwc3") ||
	    of_device_is_compatible(dev->of_node, "apple,t6040-dwc3"))
		appledwc->usb3_phy = devm_phy_optional_get(dev, "usb3-phy");
	else
		appledwc->usb3_phy = devm_phy_get(dev, "usb3-phy");
	if (IS_ERR(appledwc->usb3_phy))
		return dev_err_probe(dev, PTR_ERR(appledwc->usb3_phy),
				     "Failed to get the USB3 PHY\n");

	ret = reset_control_assert(appledwc->reset);
	if (ret) {
		dev_err(&pdev->dev, "Failed to assert reset, err=%d\n", ret);
		return ret;
	}

	appledwc->mmio_resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dwc3-core");
	if (!appledwc->mmio_resource) {
		dev_err(dev, "Failed to get DWC3 MMIO\n");
		return -EINVAL;
	}

	appledwc->apple_regs = devm_platform_ioremap_resource_byname(pdev, "dwc3-apple");
	if (IS_ERR(appledwc->apple_regs))
		return dev_err_probe(dev, PTR_ERR(appledwc->apple_regs),
				     "Failed to map Apple-specific MMIO\n");

	ret = dwc3_apple_parse_tunables(appledwc, pdev);
	if (ret)
		return ret;

	/*
	 * Like a PCI xHCI controller, allow USB devices to wake the system by default. This has
	 * to be set before dwc3_core_probe(), which hands it on to the xHCI device.
	 */
	ret = devm_device_init_wakeup(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to set up wakeup\n");

	/*
	 * On this platform, DWC3 can only be brought up after parts of the PHY have been
	 * initialized with knowledge of the target mode and cable orientation from typec_set_mux.
	 * Since this has not happened here we cannot setup DWC3 yet and instead defer this until
	 * the first cable is connected. See the documentation for enum dwc3_apple_state for
	 * details.
	 */
	appledwc->state = DWC3_APPLE_PROBE_PENDING;
	ret = devm_delayed_work_autocancel(dev, &appledwc->hub_check, dwc3_apple_hub_check);
	if (ret)
		return ret;
	appledwc->pm_nb.notifier_call = dwc3_apple_pm_notify;
	ret = register_pm_notifier(&appledwc->pm_nb);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, dwc3_apple_pm_notifier_remove, appledwc);
	if (ret)
		return ret;
	/*
	 * Until the first cable there is no core to keep powered: let the controller
	 * runtime-suspend, and its DARTs with it, so that its power domain can drop.
	 * Tested on T8132; on the T6041 (M4 Max) the never-plugged ports are the usual case.
	 */
	if (appledwc->idle_rpm && !appledwc->fixed_hub &&
	    (of_device_is_compatible(dev->of_node, "apple,t8132-dwc3") ||
	     of_machine_is_compatible("apple,t6041"))) {
		pm_runtime_set_active(dev);
		pm_runtime_enable(dev);
		appledwc->pre_core_rpm = true;
	}
	ret = dwc3_apple_setup_role_switch(appledwc);
	if (ret) {
		if (appledwc->pre_core_rpm)
			pm_runtime_disable(dev);
		return dev_err_probe(&pdev->dev, ret, "Failed to setup role switch\n");
	}

	if (!appledwc->fixed_hub)
		return 0;

	/*
	 * The hub is the controller's permanent USB2 peer, and the PHY has kept
	 * its USB2 path up for it since its own probe, so there is nothing to
	 * wait for: bring the host up now.
	 */
	scoped_guard(mutex, &appledwc->lock)
		ret = dwc3_apple_init(appledwc, DWC3_APPLE_HOST);
	if (ret) {
		/* A failed cleanup remains visible even when probe cannot be retried. */
		if (appledwc->core_active || appledwc->reset_pending)
			dev_err(dev, "Host startup left pending PHY/reset cleanup\n");
		if (appledwc->state != DWC3_APPLE_PROBE_PENDING)
			dwc3_core_remove_no_shutdown(&appledwc->dwc);
		usb_role_switch_unregister(appledwc->role_sw);
		return dev_err_probe(dev, ret, "Failed to start host mode\n");
	}

	return 0;
}

static void dwc3_apple_remove(struct platform_device *pdev)
{
	struct dwc3 *dwc = platform_get_drvdata(pdev);
	struct dwc3_apple *appledwc = to_dwc3_apple(dwc);

	guard(mutex)(&appledwc->lock);

	appledwc->shutting_down = true;
	usb_role_switch_unregister(appledwc->role_sw);

	/*
	 * If we're still in DWC3_APPLE_PROBE_PENDING we never got any cable connected event and
	 * dwc3_core_probe was never called and there's hence no need to call dwc3_core_remove.
	 * dwc3_apple_exit can be called unconditionally because it checks the state itself.
	 */
	if (dwc3_apple_exit(appledwc))
		dev_err(appledwc->dev, "Removal left pending PHY/reset cleanup\n");
	if (appledwc->state != DWC3_APPLE_PROBE_PENDING)
		dwc3_core_remove_no_shutdown(&appledwc->dwc);
	if (appledwc->pre_core_rpm)
		pm_runtime_disable(appledwc->dev);
}

/* Stop the consumer and release its PHY epoch before the supplier shutdown. */
static void dwc3_apple_shutdown(struct platform_device *pdev)
{
	struct dwc3_apple *appledwc = to_dwc3_apple(platform_get_drvdata(pdev));
	int ret;

	if (!of_device_is_compatible(appledwc->dev->of_node, "apple,t8132-dwc3"))
		return;
	guard(mutex)(&appledwc->lock);
	appledwc->shutting_down = true;
	ret = dwc3_apple_exit(appledwc);
	if (ret)
		dev_err(appledwc->dev,
			"Shutdown retains incomplete PHY/reset ownership: %d\n", ret);
}

static const struct of_device_id dwc3_apple_of_match[] = {
	{ .compatible = "apple,t8132-dwc3" },
	{ .compatible = "apple,t8103-dwc3" },
	{}
};
MODULE_DEVICE_TABLE(of, dwc3_apple_of_match);

static struct platform_driver dwc3_apple_driver = {
	.probe		= dwc3_apple_probe,
	.remove		= dwc3_apple_remove,
	.shutdown	= dwc3_apple_shutdown,
	.driver		= {
		.name	= "dwc3-apple",
		.of_match_table	= dwc3_apple_of_match,
		.pm	= pm_ptr(&dwc3_apple_pm_ops),
	},
};

module_platform_driver(dwc3_apple_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sven Peter <sven@kernel.org>");
MODULE_DESCRIPTION("DesignWare DWC3 Apple Silicon Glue Driver");
