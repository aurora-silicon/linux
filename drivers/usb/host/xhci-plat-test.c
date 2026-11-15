// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the generic xHCI platform driver
 */

#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <kunit/visibility.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>

#include "xhci.h"
#include "xhci-plat.h"

/* Only suspend-to-idle with the quirk, wakeup enabled and a USB wakeup keeps it running. */
static void xhci_plat_keep_running_test(struct kunit *test)
{
	static const suspend_state_t targets[] = {
		PM_SUSPEND_ON, PM_SUSPEND_TO_IDLE, PM_SUSPEND_STANDBY, PM_SUSPEND_MEM,
	};
	unsigned int i, combo;

	for (i = 0; i < ARRAY_SIZE(targets); i++) {
		for (combo = 0; combo < 8; combo++) {
			u64 quirks = (combo & 1) ? XHCI_RUN_IN_S2IDLE : 0;
			bool may_wakeup = combo & 2, usb_wakeup = combo & 4;
			bool expected = combo == 7 && targets[i] == PM_SUSPEND_TO_IDLE;

			bool kept = xhci_plat_keep_running(quirks, targets[i], may_wakeup,
							   usb_wakeup);

			KUNIT_EXPECT_EQ_MSG(test, kept, expected,
					    "target %d quirk %d may_wakeup %d usb_wakeup %d",
					    targets[i], combo & 1, may_wakeup, usb_wakeup);
		}
	}
}

/*
 * Something below a root hub has to be able to wake the system, which the USB core decided as it
 * suspended the devices. The J700 defaults are the first case: wakeup is enabled for a keyboard,
 * but disabled for the VL122 hub between it and the root hub, which does not pass the remote
 * wakeup on while it is suspended, and for the root hub. Nothing can wake the system through it.
 */
static void xhci_plat_usb_wakeup_test(struct kunit *test)
{
	struct usb_hcd *hcd = kunit_kzalloc(test, sizeof(*hcd), GFP_KERNEL);
	struct usb_device *rhdev = kunit_kzalloc(test, sizeof(*rhdev), GFP_KERNEL);

	KUNIT_ASSERT_TRUE(test, hcd && rhdev);
	KUNIT_EXPECT_FALSE(test, xhci_plat_usb_wakeup(NULL));
	KUNIT_EXPECT_FALSE(test, xhci_plat_usb_wakeup(hcd));
	hcd->self.root_hub = rhdev;
	KUNIT_EXPECT_FALSE(test, xhci_plat_usb_wakeup(hcd));

	/* Armed for connect, disconnect and over-current changes */
	rhdev->do_remote_wakeup = 1;
	KUNIT_EXPECT_TRUE(test, xhci_plat_usb_wakeup(hcd));
	rhdev->do_remote_wakeup = 0;

	/* Kept awake for a hub below, see usb_suspend_both() */
	rhdev->awake_in_sleep = 1;
	KUNIT_EXPECT_TRUE(test, xhci_plat_usb_wakeup(hcd));
}

/* Other quirks do not stand in for the suspend-to-idle one. */
static void xhci_plat_keep_running_other_quirks_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, xhci_plat_keep_running(~XHCI_RUN_IN_S2IDLE, PM_SUSPEND_TO_IDLE,
							true, true));
	KUNIT_EXPECT_TRUE(test, xhci_plat_keep_running(~0ULL, PM_SUSPEND_TO_IDLE, true, true));
}

/* Only a controller kept running in suspend-to-idle keeps its interrupt enabled across it. */
static void xhci_plat_irq_flags_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, xhci_plat_irq_flags(0), (unsigned long)IRQF_SHARED);
	KUNIT_EXPECT_EQ(test, xhci_plat_irq_flags(~XHCI_RUN_IN_S2IDLE), (unsigned long)IRQF_SHARED);
	KUNIT_EXPECT_EQ(test, xhci_plat_irq_flags(XHCI_RUN_IN_S2IDLE),
			(unsigned long)(IRQF_SHARED | IRQF_NO_SUSPEND));
}

/* A running controller is left alone; a halted, failed or unpowered one is not. */
static void xhci_plat_stopped_test(struct kunit *test)
{
	/* Running, with an event pending and a port change */
	KUNIT_EXPECT_FALSE(test, xhci_plat_stopped(0));
	KUNIT_EXPECT_FALSE(test, xhci_plat_stopped(STS_EINT | STS_PORT));
	/* Halted, Host System Error, Host Controller Error */
	KUNIT_EXPECT_TRUE(test, xhci_plat_stopped(STS_HALT));
	KUNIT_EXPECT_TRUE(test, xhci_plat_stopped(STS_FATAL | STS_HALT));
	KUNIT_EXPECT_TRUE(test, xhci_plat_stopped(STS_HCE));
	/* Register reads of a powered-off block */
	KUNIT_EXPECT_TRUE(test, xhci_plat_stopped(~0U));
}

/*
 * A controller reported dead is halted with its interrupts masked first:
 * the USB core ignores the interrupts of a dead HCD, so one left enabled
 * and pending would never be acknowledged.
 */
static void xhci_plat_quiesce_dying_test(struct kunit *test)
{
	struct xhci_op_regs *op;
	struct xhci_hcd *xhci;

	xhci = kunit_kzalloc(test, sizeof(*xhci), GFP_KERNEL);
	op = kunit_kzalloc(test, sizeof(*op), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, xhci);
	KUNIT_ASSERT_NOT_NULL(test, op);
	xhci->op_regs = (__force struct xhci_op_regs __iomem *)op;

	/* A Host System Error with an event pending, interrupts enabled */
	writel(CMD_RUN | XHCI_IRQS, &xhci->op_regs->command);
	writel(STS_HALT | STS_FATAL | STS_EINT, &xhci->op_regs->status);
	xhci_quiesce_dying(xhci);
	KUNIT_EXPECT_EQ(test, readl(&xhci->op_regs->command) & XHCI_IRQS, 0U);
	KUNIT_EXPECT_TRUE(test, xhci->xhc_state & XHCI_STATE_HALTED);

	/* Registers that no longer answer are left alone. */
	xhci->xhc_state = 0;
	writel(~0U, &xhci->op_regs->status);
	writel(XHCI_IRQS, &xhci->op_regs->command);
	xhci_quiesce_dying(xhci);
	KUNIT_EXPECT_EQ(test, readl(&xhci->op_regs->command), (u32)XHCI_IRQS);
	KUNIT_EXPECT_FALSE(test, xhci->xhc_state & XHCI_STATE_HALTED);
}

#ifdef CONFIG_PM
static int xhci_plat_test_rpm_ret;

static int xhci_plat_test_runtime_resume(struct device *dev)
{
	return xhci_plat_test_rpm_ret;
}

static struct dev_pm_domain xhci_plat_test_pm_domain = {
	.ops = { .runtime_resume = xhci_plat_test_runtime_resume },
};

static struct device *xhci_plat_test_rpm_device(struct kunit *test, int rpm_ret)
{
	struct device *dev = kunit_device_register(test, "xhci-plat-rpm-test");

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	xhci_plat_test_rpm_ret = rpm_ret;
	dev->pm_domain = &xhci_plat_test_pm_domain;
	pm_runtime_set_suspended(dev);
	/*
	 * System suspend runs the suspend callbacks with a runtime PM reference
	 * held (device_prepare()). Hold one here too, or the idle check queued
	 * after a resume starts a runtime suspend while the test looks at the
	 * state.
	 */
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	return dev;
}

static void xhci_plat_test_rpm_release(struct device *dev)
{
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	dev->pm_domain = NULL;
}

/* A runtime resume that fails before system sleep fails the system suspend. */
static void xhci_plat_resume_from_runtime_error_test(struct kunit *test)
{
	struct device *dev = xhci_plat_test_rpm_device(test, -ETIMEDOUT);

	KUNIT_EXPECT_EQ(test, xhci_plat_resume_from_runtime(dev), -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, pm_runtime_suspended(dev));
	xhci_plat_test_rpm_release(dev);
}

/* A successful one, and an active controller, let it go ahead. */
static void xhci_plat_resume_from_runtime_ok_test(struct kunit *test)
{
	struct device *dev = xhci_plat_test_rpm_device(test, 0);

	KUNIT_EXPECT_EQ(test, xhci_plat_resume_from_runtime(dev), 0);
	KUNIT_EXPECT_TRUE(test, pm_runtime_active(dev));
	KUNIT_EXPECT_EQ(test, xhci_plat_resume_from_runtime(dev), 0);
	xhci_plat_test_rpm_release(dev);
}
#endif /* CONFIG_PM */

/*
 * Port changes seen while kept running in suspend-to-idle that are not
 * wakeups. The first is from a J700 whose SuperSpeed link on the rear port
 * kept dropping to SS.Inactive while its root hub was suspended with wakeup
 * disabled: PLC, SS.Inactive, powered, no wake bits.
 */
static void xhci_s2idle_port_no_wakeup_test(struct kunit *test)
{
	static const u32 armed_bits = PORT_WKCONN_E | PORT_WKDISC_E | PORT_WKOC_E;
	static const u32 changes[] = {
		PORT_PLC | XDEV_INACTIVE | PORT_POWER,
		PORT_PLC | XDEV_POLLING | PORT_POWER,
		PORT_PLC | XDEV_RXDETECT | PORT_POWER,
		PORT_PLC | XDEV_COMP_MODE | PORT_POWER,
		/* A device that was resumed and is back in U0 or in U3 */
		PORT_PLC | XDEV_U0 | PORT_POWER | PORT_PE | PORT_CONNECT,
		PORT_PLC | XDEV_U3 | PORT_POWER | PORT_PE | PORT_CONNECT,
		/* Warm and port reset, enable and config error changes */
		PORT_WRC | PORT_RC | XDEV_U0 | PORT_POWER | PORT_PE | PORT_CONNECT,
		PORT_PEC | XDEV_DISABLED | PORT_POWER | PORT_CONNECT,
		PORT_CEC | XDEV_U0 | PORT_POWER | PORT_PE | PORT_CONNECT,
		/* Resume state, but the link change was not what was reported */
		PORT_RC | XDEV_RESUME | PORT_POWER | PORT_PE | PORT_CONNECT,
	};
	unsigned int i;

	KUNIT_EXPECT_FALSE(test, xhci_s2idle_port_wakeup(0x4002c0));

	for (i = 0; i < ARRAY_SIZE(changes); i++) {
		/* Not even with the root hub armed for wakeup */
		KUNIT_EXPECT_FALSE_MSG(test, xhci_s2idle_port_wakeup(changes[i]),
				       "portsc %#x", changes[i]);
		KUNIT_EXPECT_FALSE_MSG(test, xhci_s2idle_port_wakeup(changes[i] | armed_bits),
				       "portsc %#x", changes[i] | armed_bits);
	}
}

/* Remote wakeup signalled by a device below the port wakes the system. */
static void xhci_s2idle_port_remote_wakeup_test(struct kunit *test)
{
	u32 resume = PORT_PLC | XDEV_RESUME | PORT_POWER | PORT_PE | PORT_CONNECT;

	KUNIT_EXPECT_TRUE(test, xhci_s2idle_port_wakeup(resume));
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_port_wakeup(resume | PORT_WKDISC_E | PORT_WKOC_E));
}

/* Connect, disconnect and over-current wake it only if the port was armed for them. */
static void xhci_s2idle_port_armed_change_test(struct kunit *test)
{
	u32 connect = PORT_CSC | PORT_CONNECT | XDEV_POLLING | PORT_POWER;
	u32 disconnect = PORT_CSC | XDEV_RXDETECT | PORT_POWER;
	u32 overcurrent = PORT_OCC | PORT_OC | XDEV_DISABLED | PORT_POWER;

	/* As xhci_bus_suspend() arms an empty and a connected port */
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_port_wakeup(connect | PORT_WKCONN_E | PORT_WKOC_E));
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_port_wakeup(disconnect | PORT_WKDISC_E | PORT_WKOC_E));
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_port_wakeup(overcurrent | PORT_WKOC_E));

	/* Root hub wakeup disabled: nothing armed */
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_port_wakeup(connect));
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_port_wakeup(disconnect));
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_port_wakeup(overcurrent));

	/* A wake bit only stands for its own change */
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_port_wakeup(overcurrent | PORT_WKCONN_E |
							 PORT_WKDISC_E));
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_port_wakeup(PORT_PLC | XDEV_INACTIVE | PORT_POWER |
							 PORT_WKOC_E));
}

/* Only a hub kept awake for wakeup completing a transfer wakes the system. */
static void xhci_s2idle_transfer_wakeup_test(struct kunit *test)
{
	struct usb_device *udev = kunit_kzalloc(test, sizeof(*udev), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, udev);
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_transfer_wakeup(NULL));
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_transfer_wakeup(udev));
	udev->awake_in_sleep = 1;
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_transfer_wakeup(udev));
}

static u64 xhci_plat_test_quirks(struct kunit *test, const struct property_entry *props)
{
	struct device *dev;

	dev = kunit_device_register(test, "xhci-plat-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	KUNIT_ASSERT_EQ(test, device_create_managed_software_node(dev, props, NULL), 0);

	return xhci_plat_property_quirks(dev);
}

/* A device without properties gets no quirks. */
static void xhci_plat_no_property_quirks_test(struct kunit *test)
{
	static const struct property_entry props[] = { {} };

	KUNIT_EXPECT_EQ(test, xhci_plat_test_quirks(test, props), 0ULL);
}

/* The property a glue driver sets for suspend-to-idle maps to its quirk, and only to it. */
static void xhci_plat_run_in_s2idle_property_test(struct kunit *test)
{
	static const struct property_entry props[] = {
		PROPERTY_ENTRY_BOOL("xhci-run-in-s2idle"),
		{}
	};

	KUNIT_EXPECT_EQ(test, xhci_plat_test_quirks(test, props), XHCI_RUN_IN_S2IDLE);
}

/* Every boolean property maps to its quirk. */
static void xhci_plat_all_property_quirks_test(struct kunit *test)
{
	static const struct property_entry props[] = {
		PROPERTY_ENTRY_BOOL("usb2-lpm-disable"),
		PROPERTY_ENTRY_BOOL("usb3-lpm-capable"),
		PROPERTY_ENTRY_BOOL("quirk-broken-port-ped"),
		PROPERTY_ENTRY_BOOL("xhci-sg-trb-cache-size-quirk"),
		PROPERTY_ENTRY_BOOL("write-64-hi-lo-quirk"),
		PROPERTY_ENTRY_BOOL("xhci-missing-cas-quirk"),
		PROPERTY_ENTRY_BOOL("xhci-skip-phy-init-quirk"),
		PROPERTY_ENTRY_BOOL("xhci-run-in-s2idle"),
		{}
	};

	KUNIT_EXPECT_EQ(test, xhci_plat_test_quirks(test, props),
			XHCI_HW_LPM_DISABLE | XHCI_LPM_SUPPORT | XHCI_BROKEN_PORT_PED |
			XHCI_SG_TRB_CACHE_SIZE_QUIRK | XHCI_WRITE_64_HI_LO | XHCI_MISSING_CAS |
			XHCI_SKIP_PHY_INIT | XHCI_RUN_IN_S2IDLE);
}

static struct kunit_case xhci_plat_test_cases[] = {
	KUNIT_CASE(xhci_plat_keep_running_test),
	KUNIT_CASE(xhci_plat_keep_running_other_quirks_test),
	KUNIT_CASE(xhci_plat_usb_wakeup_test),
	KUNIT_CASE(xhci_plat_irq_flags_test),
	KUNIT_CASE(xhci_plat_stopped_test),
	KUNIT_CASE(xhci_plat_quiesce_dying_test),
#ifdef CONFIG_PM
	KUNIT_CASE(xhci_plat_resume_from_runtime_error_test),
	KUNIT_CASE(xhci_plat_resume_from_runtime_ok_test),
#endif
	KUNIT_CASE(xhci_plat_no_property_quirks_test),
	KUNIT_CASE(xhci_plat_run_in_s2idle_property_test),
	KUNIT_CASE(xhci_plat_all_property_quirks_test),
	KUNIT_CASE(xhci_s2idle_port_no_wakeup_test),
	KUNIT_CASE(xhci_s2idle_port_remote_wakeup_test),
	KUNIT_CASE(xhci_s2idle_port_armed_change_test),
	KUNIT_CASE(xhci_s2idle_transfer_wakeup_test),
	{}
};

static struct kunit_suite xhci_plat_test_suite = {
	.name = "xhci_plat",
	.test_cases = xhci_plat_test_cases,
};

#ifdef CONFIG_PM_SLEEP
/*
 * The event handling of a controller with one SuperSpeed root port, faked in
 * memory, whose root hub is suspended. PORTSC writes and port changes go
 * through xhci_s2idle_test_portsc_writel() and xhci_s2idle_test_port_change(),
 * which act as the controller does.
 */
struct xhci_s2idle_test {
	struct usb_hcd *hcd;
	struct xhci_hcd *xhci;
	struct xhci_op_regs *op;
	struct xhci_port_regs *port_regs;
	__le32 __iomem *portsc;
	union xhci_trb *trbs;
	unsigned int events;
	/* A change that the controller latches just before the next PORTSC write */
	u32 set_before_write;
	/* The PORTSC read just before which the device below signals resume, from 1 */
	unsigned int resume_at_read;
	unsigned int reads;
	/* The generic xHCI root hub and interrupt methods, as the USB core calls them */
	struct hc_driver driver;
	struct device *dev;
	char name[32];
};

static void xhci_s2idle_test_rh_resume(struct work_struct *work)
{
}

static void xhci_s2idle_test_rh_timer(struct timer_list *timer)
{
}

KUNIT_DEFINE_ACTION_WRAPPER(xhci_s2idle_test_rh_timer_delete, timer_delete_sync,
			    struct timer_list *);

/*
 * A PORTSC write as the controller takes it: the change bits and PED are
 * cleared by writing 1, the link state is only written with LWS, port power
 * and the wake bits take the value written, and the read-only bits keep
 * theirs.
 */
static void xhci_s2idle_test_portsc_writel(struct xhci_port *port, u32 val)
{
	struct xhci_s2idle_test *t = kunit_get_current_test()->priv;
	u32 w1c = PORT_PE | PORT_CHANGE_MASK;
	u32 rw = PORT_POWER | PORT_WKCONN_E | PORT_WKDISC_E | PORT_WKOC_E;
	u32 old = readl(&port->port_reg->portsc) | t->set_before_write;
	u32 now = (old & ~(w1c | rw)) | (old & w1c & ~val) | (val & rw);

	if (val & PORT_LINK_STROBE)
		now = (now & ~PORT_PLS_MASK) | (val & PORT_PLS_MASK);
	t->set_before_write = 0;
	writel(now, &port->port_reg->portsc);
}

/* The controller posts a Port Status Change Event for root port 1 */
static void xhci_s2idle_test_post_event(struct xhci_s2idle_test *t)
{
	union xhci_trb *trb = &t->trbs[t->events++];

	trb->generic.field[0] = cpu_to_le32(1 << 24);
	trb->generic.field[2] = cpu_to_le32(COMP_SUCCESS << 24);
	trb->generic.field[3] = cpu_to_le32(TRB_TYPE(TRB_PORT_STATUS) | TRB_CYCLE);
	writel(readl(&t->xhci->op_regs->status) | STS_EINT, &t->xhci->op_regs->status);
}

/*
 * The controller changes the port. It posts an event only for a change made
 * while no change bit is set, see xHCI 1.2 section 4.19.2, and returns whether
 * it did.
 */
static bool xhci_s2idle_test_port_change(struct xhci_s2idle_test *t, u32 clear, u32 set)
{
	u32 old = readl(t->portsc);
	u32 now = (old & ~clear) | set;

	writel(now, t->portsc);
	if ((old & PORT_CHANGE_MASK) || !(now & PORT_CHANGE_MASK))
		return false;
	xhci_s2idle_test_post_event(t);
	return true;
}

/* The J700 rear port: its SuperSpeed link drops to SS.Inactive, as in 0x4002c0 */
static bool xhci_s2idle_test_link_inactive(struct xhci_s2idle_test *t)
{
	return xhci_s2idle_test_port_change(t, PORT_PLS_MASK, XDEV_INACTIVE | PORT_PLC);
}

/* A device is attached, and its link trains to U0 */
static bool xhci_s2idle_test_connect(struct xhci_s2idle_test *t)
{
	return xhci_s2idle_test_port_change(t, PORT_PLS_MASK,
					    XDEV_U0 | PORT_CSC | PORT_CONNECT | PORT_PE);
}

/* The device below the port is detached */
static bool xhci_s2idle_test_disconnect(struct xhci_s2idle_test *t)
{
	u32 gone = PORT_PLS_MASK | DEV_SPEED_MASK | PORT_PE | PORT_CONNECT;

	return xhci_s2idle_test_port_change(t, gone, XDEV_RXDETECT | PORT_CSC);
}

/* The port detects an over-current, and is disabled */
static bool xhci_s2idle_test_overcurrent(struct xhci_s2idle_test *t)
{
	return xhci_s2idle_test_port_change(t, PORT_PLS_MASK, XDEV_DISABLED | PORT_OCC | PORT_OC);
}

/* The device below the port signals resume */
static bool xhci_s2idle_test_remote_wakeup(struct xhci_s2idle_test *t)
{
	return xhci_s2idle_test_port_change(t, PORT_PLS_MASK, XDEV_RESUME | PORT_PLC);
}

/* A PORTSC read, with the device below signalling resume just before the one chosen */
static u32 xhci_s2idle_test_portsc_readl(struct xhci_port *port)
{
	struct xhci_s2idle_test *t = kunit_get_current_test()->priv;

	if (++t->reads == t->resume_at_read)
		xhci_s2idle_test_remote_wakeup(t);
	return readl(&port->port_reg->portsc);
}

static unsigned int xhci_s2idle_test_count;

/* The port at @portsc, with an event posted for its change bits if any are set */
static struct xhci_s2idle_test *xhci_s2idle_test_init(struct kunit *test, u32 usbsts, u32 portsc)
{
	static const struct xhci_driver_overrides no_overrides;
	struct xhci_s2idle_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct xhci_interrupter *ir, **irs;
	struct xhci_intr_reg *ir_set;
	struct xhci_segment *seg;
	struct xhci_ring *ring;
	struct xhci_port *port, **ports;
	union xhci_trb *trbs;
	struct xhci_hcd *xhci;

	KUNIT_ASSERT_NOT_NULL(test, t);
	t->hcd = kunit_kzalloc(test, sizeof(*t->hcd) + sizeof(*xhci), GFP_KERNEL);
	t->op = kunit_kzalloc(test, sizeof(*t->op), GFP_KERNEL);
	t->port_regs = kunit_kzalloc(test, sizeof(*t->port_regs), GFP_KERNEL);
	ir = kunit_kzalloc(test, sizeof(*ir), GFP_KERNEL);
	irs = kunit_kzalloc(test, sizeof(*irs), GFP_KERNEL);
	ring = kunit_kzalloc(test, sizeof(*ring), GFP_KERNEL);
	seg = kunit_kzalloc(test, sizeof(*seg), GFP_KERNEL);
	trbs = kunit_kcalloc(test, TRBS_PER_SEGMENT, sizeof(*trbs), GFP_KERNEL);
	port = kunit_kzalloc(test, sizeof(*port), GFP_KERNEL);
	ports = kunit_kzalloc(test, sizeof(*ports), GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, t->hcd && t->op && t->port_regs && ir && irs && ring && seg &&
			  trbs && port && ports);
	ir_set = kunit_kzalloc(test, sizeof(*ir_set), GFP_KERNEL);
	t->hcd->self.root_hub = kunit_kzalloc(test, sizeof(*t->hcd->self.root_hub), GFP_KERNEL);
	KUNIT_ASSERT_TRUE(test, ir_set && t->hcd->self.root_hub);
	ir->ir_set = (__force struct xhci_intr_reg __iomem *)ir_set;

	test->priv = t;
	kunit_activate_static_stub(test, xhci_portsc_writel, xhci_s2idle_test_portsc_writel);

	/*
	 * The controller device may wake the system, as for running in s2idle.
	 * A test may set up more than one controller, so number them.
	 */
	snprintf(t->name, sizeof(t->name), "xhci-s2idle-test.%u", xhci_s2idle_test_count++);
	t->dev = kunit_device_register(test, t->name);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->dev);
	KUNIT_ASSERT_EQ(test, devm_device_init_wakeup(t->dev), 0);

	xhci = hcd_to_xhci(t->hcd);
	t->xhci = xhci;
	spin_lock_init(&xhci->lock);
	xhci->main_hcd = t->hcd;
	xhci->shared_hcd = t->hcd;
	xhci->op_regs = (__force struct xhci_op_regs __iomem *)t->op;
	xhci_init_driver(&t->driver, &no_overrides);
	t->hcd->driver = &t->driver;
	t->hcd->self.controller = t->dev;
	t->hcd->speed = HCD_USB3;
	t->hcd->state = HC_STATE_SUSPENDED;
	/* A registered and pollable root hub, as for usb_hcd_resume_root_hub() */
	t->hcd->rh_registered = 1;
	t->hcd->rh_pollable = 1;
	t->hcd->uses_new_polling = 1;
	spin_lock_init(&t->hcd->self.root_hub->dev.power.lock);
	INIT_WORK(&t->hcd->wakeup_work, xhci_s2idle_test_rh_resume);
	timer_setup(&t->hcd->rh_timer, xhci_s2idle_test_rh_timer, 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, xhci_s2idle_test_rh_timer_delete,
							&t->hcd->rh_timer), 0);

	seg->trbs = trbs;
	seg->dma = 0x10000;
	seg->next = seg;
	ring->type = TYPE_EVENT;
	ring->first_seg = seg;
	ring->deq_seg = seg;
	ring->dequeue = trbs;
	ring->cycle_state = 1;
	ir->event_ring = ring;
	irs[0] = ir;
	xhci->interrupters = irs;
	t->trbs = trbs;

	port->port_reg = (__force struct xhci_port_regs __iomem *)t->port_regs;
	port->rhub = &xhci->usb3_rhub;
	init_completion(&port->rexit_done);
	init_completion(&port->u3exit_done);
	ports[0] = port;
	xhci->hw_ports = port;
	xhci->max_ports = 1;
	xhci->usb3_rhub.ports = ports;
	xhci->usb3_rhub.num_ports = 1;
	xhci->usb3_rhub.hcd = t->hcd;
	t->portsc = &port->port_reg->portsc;

	writel(CMD_RUN | CMD_EIE, &xhci->op_regs->command);
	writel(usbsts, &xhci->op_regs->status);
	writel(portsc, t->portsc);
	if (portsc & PORT_CHANGE_MASK)
		xhci_s2idle_test_post_event(t);

	return t;
}

/* Make the port a USB 2 one, on the root hub of a USB 2 bus */
static void xhci_s2idle_test_usb2(struct xhci_s2idle_test *t)
{
	struct xhci_hcd *xhci = t->xhci;

	xhci->usb2_rhub = xhci->usb3_rhub;
	memset(&xhci->usb3_rhub, 0, sizeof(xhci->usb3_rhub));
	xhci->hw_ports->rhub = &xhci->usb2_rhub;
	t->hcd->speed = HCD_USB2;
}

static unsigned long xhci_s2idle_test_wakeups(struct xhci_s2idle_test *t)
{
	return t->dev->power.wakeup->event_count;
}

/*
 * Whether the event handling took on a remote wakeup: the resume timing of a
 * USB 2 port runs, a SuperSpeed link was moved to U0.
 */
static bool xhci_s2idle_test_resuming(struct xhci_s2idle_test *t)
{
	if (t->hcd->speed < HCD_USB3)
		return t->xhci->hw_ports->resume_timestamp &&
		       test_bit(0, &t->xhci->usb2_rhub.bus_state.resuming_ports);
	return (readl(t->portsc) & PORT_PLS_MASK) == XDEV_U0;
}

/* Whether the root hub was resumed or polled for the event */
static bool xhci_s2idle_test_rh_resumed(struct xhci_s2idle_test *t)
{
	flush_work(&t->hcd->wakeup_work);
	return test_bit(HCD_FLAG_WAKEUP_PENDING, &t->hcd->flags);
}

static bool xhci_s2idle_test_rh_polled(struct xhci_s2idle_test *t)
{
	return test_bit(HCD_FLAG_POLL_RH, &t->hcd->flags);
}

/* Whether the root hub reports a change of the port to the hub driver */
static bool xhci_s2idle_test_rh_change(struct xhci_s2idle_test *t)
{
	char buf[2] = {};

	return t->driver.hub_status_data(t->hcd, buf) > 0 && (buf[0] & BIT(1));
}

/* The port status and changes that the hub driver reads */
static u32 xhci_s2idle_test_port_status(struct kunit *test, struct xhci_s2idle_test *t)
{
	__le32 status;

	KUNIT_ASSERT_EQ(test, t->driver.hub_control(t->hcd, GetPortStatus, 0, 1, (char *)&status,
						    sizeof(status)), 0);
	return le32_to_cpu(status);
}

static void xhci_s2idle_test_clear_change(struct kunit *test, struct xhci_s2idle_test *t,
					  u16 feature)
{
	KUNIT_ASSERT_EQ(test, t->driver.hub_control(t->hcd, ClearPortFeature, feature, 1, NULL, 0),
			0);
}

static irqreturn_t xhci_s2idle_test_irq(struct xhci_s2idle_test *t)
{
	irqreturn_t ret;

	local_irq_disable();
	ret = t->driver.irq(t->hcd);
	local_irq_enable();
	return ret;
}

/* A SuperSpeed port in link training, with nothing connected */
#define XHCI_S2IDLE_TEST_EMPTY		(XDEV_POLLING | PORT_POWER)
/* A SuperSpeed port with a device suspended below it */
#define XHCI_S2IDLE_TEST_SUSPENDED	(XDEV_U3 | XDEV_SS | PORT_POWER | PORT_PE | PORT_CONNECT)
/* A USB 2 port with a high-speed device suspended below it */
#define XHCI_S2IDLE_TEST_USB2_SUSPENDED	(XDEV_U3 | XDEV_HS | PORT_POWER | PORT_PE | PORT_CONNECT)

/*
 * The J700 failure: a SuperSpeed link drops to SS.Inactive while running in
 * suspend-to-idle. No wakeup, and the root hub is neither resumed nor polled.
 * The change is cleared in PORTSC, so that the port posts an event for its
 * next one, and held for the hub driver.
 */
static void xhci_s2idle_link_change_held_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_EMPTY);

	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_link_inactive(t));
	KUNIT_ASSERT_EQ(test, readl(t->portsc), 0x4002c0U);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_irq(t), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 0UL);
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_test_rh_resumed(t));
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_test_rh_polled(t));
	KUNIT_EXPECT_EQ(test, readl(t->portsc) & PORT_CHANGE_MASK, 0U);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_port_status(test, t) >> 16,
			(u32)USB_PORT_STAT_C_LINK_STATE);
	xhci_run_in_s2idle_end(t->xhci);
}

/*
 * On resume the root hub is resumed and polled again, and the hub driver reads
 * the held change as if it had stayed in PORTSC, until it clears it.
 */
static void xhci_s2idle_link_change_resume_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_EMPTY);
	u32 status;

	xhci_run_in_s2idle_begin(t->xhci);
	xhci_s2idle_test_link_inactive(t);
	xhci_s2idle_test_irq(t);
	xhci_run_in_s2idle_end(t->xhci);
	KUNIT_EXPECT_FALSE(test, t->xhci->running_in_s2idle);
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_rh_resumed(t));
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_rh_polled(t));
	/* The root hub is suspended: the poll keeps the change for its status URB */
	KUNIT_EXPECT_TRUE(test, test_bit(HCD_FLAG_POLL_PENDING, &t->hcd->flags));
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_rh_change(t));
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 0UL);

	status = xhci_s2idle_test_port_status(test, t);
	KUNIT_EXPECT_EQ(test, status >> 16, (u32)USB_PORT_STAT_C_LINK_STATE);
	KUNIT_EXPECT_EQ(test, status & USB_PORT_STAT_LINK_STATE, (u32)USB_SS_PORT_LS_SS_INACTIVE);

	xhci_s2idle_test_clear_change(test, t, USB_PORT_FEAT_C_PORT_LINK_STATE);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_port_status(test, t) >> 16, 0U);
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_test_rh_change(t));
}

/* Outside suspend-to-idle the same change goes to the USB core at once, as before. */
static void xhci_s2idle_link_change_awake_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, STS_EINT, 0x4002c0);

	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_irq(t), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 0UL);
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_rh_resumed(t));
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_rh_polled(t));
	KUNIT_EXPECT_TRUE(test, readl(t->portsc) & PORT_PLC);
}

/* Remote wakeup from a SuperSpeed device below the port wakes the system. */
static void xhci_s2idle_remote_wakeup_event_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_SUSPENDED);

	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_remote_wakeup(t));
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_irq(t), IRQ_HANDLED);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 1UL);
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_rh_resumed(t));
	/* The event handling took the remote wakeup on and moved the link to U0 */
	KUNIT_EXPECT_EQ(test, readl(t->portsc) & PORT_PLS_MASK, (u32)XDEV_U0);
	xhci_run_in_s2idle_end(t->xhci);
}

/*
 * A held change does not keep the port from posting an event for the next
 * one: a device attached to a port armed for connect wakes the system.
 */
static void xhci_s2idle_connect_after_held_change_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_EMPTY |
							   PORT_WKCONN_E | PORT_WKOC_E);

	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_link_inactive(t));
	xhci_s2idle_test_irq(t);
	KUNIT_ASSERT_EQ(test, xhci_s2idle_test_wakeups(t), 0UL);

	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_connect(t));
	xhci_s2idle_test_irq(t);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 1UL);
	xhci_run_in_s2idle_end(t->xhci);
}

/* Nor an over-current on a port armed for it */
static void xhci_s2idle_overcurrent_after_held_change_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_EMPTY |
							   PORT_WKCONN_E | PORT_WKOC_E);

	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_link_inactive(t));
	xhci_s2idle_test_irq(t);
	KUNIT_ASSERT_EQ(test, xhci_s2idle_test_wakeups(t), 0UL);

	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_overcurrent(t));
	xhci_s2idle_test_irq(t);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 1UL);
	xhci_run_in_s2idle_end(t->xhci);
}

/* Nor the disconnect of a device whose link failed, on a port armed for it */
static void xhci_s2idle_disconnect_after_held_change_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_SUSPENDED |
							   PORT_WKDISC_E | PORT_WKOC_E);

	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_link_inactive(t));
	xhci_s2idle_test_irq(t);
	KUNIT_ASSERT_EQ(test, xhci_s2idle_test_wakeups(t), 0UL);

	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_disconnect(t));
	xhci_s2idle_test_irq(t);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 1UL);
	xhci_run_in_s2idle_end(t->xhci);
}

/*
 * Nor a remote wakeup: the over-current change of a port whose root hub may
 * not wake the system is held, and the suspended device below it then signals
 * resume.
 */
static void xhci_s2idle_remote_wakeup_after_held_change_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_SUSPENDED);

	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_port_change(t, 0, PORT_OCC));
	xhci_s2idle_test_irq(t);
	KUNIT_ASSERT_EQ(test, xhci_s2idle_test_wakeups(t), 0UL);

	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_remote_wakeup(t));
	xhci_s2idle_test_irq(t);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 1UL);
	KUNIT_EXPECT_EQ(test, readl(t->portsc) & PORT_PLS_MASK, (u32)XDEV_U0);
	xhci_run_in_s2idle_end(t->xhci);
	KUNIT_EXPECT_TRUE(test, (xhci_s2idle_test_port_status(test, t) >> 16) &
			  USB_PORT_STAT_C_OVERCURRENT);
}

/*
 * A connect that the controller latches while the link change is cleared
 * posts no event, as the port still had a change bit set. The event handling
 * reads PORTSC again after the write, and wakes the system for it.
 */
static void xhci_s2idle_connect_during_hold_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_EMPTY |
							   PORT_WKCONN_E | PORT_WKOC_E);

	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_link_inactive(t));
	t->set_before_write = PORT_CSC | PORT_CONNECT;
	xhci_s2idle_test_irq(t);
	KUNIT_EXPECT_EQ(test, t->set_before_write, 0U);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 1UL);
	/* Left in PORTSC, for the hub driver after the wakeup */
	KUNIT_EXPECT_TRUE(test, readl(t->portsc) & PORT_CSC);
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_rh_resumed(t));
	xhci_run_in_s2idle_end(t->xhci);
}

/*
 * One that does not wake the system is held as well, so the port is left with
 * no change bit set, and the hub driver reads both changes on resume.
 */
static void xhci_s2idle_change_during_hold_test(struct kunit *test)
{
	/* The root hub may not wake the system, so the port is not armed */
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_EMPTY);
	u32 status;

	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_link_inactive(t));
	t->set_before_write = PORT_CSC | PORT_CONNECT;
	xhci_s2idle_test_irq(t);
	KUNIT_EXPECT_EQ(test, t->set_before_write, 0U);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 0UL);
	KUNIT_EXPECT_EQ(test, readl(t->portsc) & PORT_CHANGE_MASK, 0U);
	xhci_run_in_s2idle_end(t->xhci);

	status = xhci_s2idle_test_port_status(test, t);
	KUNIT_EXPECT_EQ(test, status >> 16,
			(u32)(USB_PORT_STAT_C_CONNECTION | USB_PORT_STAT_C_LINK_STATE));
	KUNIT_EXPECT_TRUE(test, status & USB_PORT_STAT_CONNECTION);
}

/*
 * A change that the port latched before, and that the hub driver had no
 * chance to clear, is held once running in suspend-to-idle begins: the port
 * then posts an event for the remote wakeup of the device below it.
 */
static void xhci_s2idle_change_before_begin_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_SUSPENDED);

	/* Its event comes in before, and the change is left for the hub driver */
	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_port_change(t, 0, PORT_OCC));
	xhci_s2idle_test_irq(t);
	KUNIT_ASSERT_TRUE(test, readl(t->portsc) & PORT_OCC);

	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 0UL);
	KUNIT_EXPECT_EQ(test, readl(t->portsc) & PORT_CHANGE_MASK, 0U);
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_remote_wakeup(t));
	xhci_s2idle_test_irq(t);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 1UL);
	xhci_run_in_s2idle_end(t->xhci);
	KUNIT_EXPECT_TRUE(test, (xhci_s2idle_test_port_status(test, t) >> 16) &
			  USB_PORT_STAT_C_OVERCURRENT);
}

/*
 * One that wakes the system wakes it when running in suspend-to-idle begins,
 * and is left in PORTSC.
 */
static void xhci_s2idle_wakeup_before_begin_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_EMPTY |
							   PORT_WKCONN_E | PORT_WKOC_E);

	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_connect(t));
	xhci_s2idle_test_irq(t);
	KUNIT_ASSERT_EQ(test, xhci_s2idle_test_wakeups(t), 0UL);

	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 1UL);
	KUNIT_EXPECT_TRUE(test, readl(t->portsc) & PORT_CSC);
	xhci_run_in_s2idle_end(t->xhci);
}

/* A USB 2 remote wakeup wakes the system and starts the resume timing. */
static void xhci_s2idle_usb2_remote_wakeup_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0,
							   XHCI_S2IDLE_TEST_USB2_SUSPENDED);

	xhci_s2idle_test_usb2(t);
	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_remote_wakeup(t));
	xhci_s2idle_test_irq(t);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 1UL);
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_resuming(t));
	xhci_run_in_s2idle_end(t->xhci);
}

/*
 * A remote wakeup that starts while PLC is set, as after U3 entry, sets no new
 * PLC and so posts no event, and clearing the old PLC removes its only change
 * bit. Its link state still shows it: the system wakes, and the event handling
 * takes the resume on.
 */
static void xhci_s2idle_resume_during_plc_clear(struct kunit *test, u32 portsc, bool usb2)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, portsc);

	if (usb2)
		xhci_s2idle_test_usb2(t);
	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_port_change(t, 0, PORT_PLC));
	t->set_before_write = PORT_PLC | XDEV_RESUME;
	xhci_s2idle_test_irq(t);
	KUNIT_EXPECT_EQ(test, t->set_before_write, 0U);
	KUNIT_EXPECT_EQ(test, t->events, 1U);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 1UL);
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_resuming(t));
	xhci_run_in_s2idle_end(t->xhci);
}

static void xhci_s2idle_resume_during_plc_clear_test(struct kunit *test)
{
	xhci_s2idle_resume_during_plc_clear(test, XHCI_S2IDLE_TEST_SUSPENDED, false);
}

static void xhci_s2idle_usb2_resume_during_plc_clear_test(struct kunit *test)
{
	xhci_s2idle_resume_during_plc_clear(test, XHCI_S2IDLE_TEST_USB2_SUSPENDED, true);
}

/* So it does for a PLC still set when running in suspend-to-idle begins. */
static void xhci_s2idle_resume_during_begin_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_SUSPENDED);

	writel(readl(t->portsc) | PORT_PLC, t->portsc);
	t->set_before_write = PORT_PLC | XDEV_RESUME;
	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_EXPECT_EQ(test, t->set_before_write, 0U);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 1UL);
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_resuming(t));
	xhci_run_in_s2idle_end(t->xhci);
}

/*
 * A remote wakeup at any point of the decision on an over-current change that
 * does not wake the system: the device signals resume just before each PORTSC
 * read in turn. While the change is set, its PLC posts no event of its own. The
 * system wakes all the same, and the resume is taken on. When the decision is
 * over before the chosen read, the change is held and nothing wakes.
 */
static void xhci_s2idle_resume_during_hold(struct kunit *test, u32 portsc, bool usb2,
					   bool latched)
{
	unsigned int read;

	for (read = 1; ; read++) {
		struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, portsc);

		if (usb2)
			xhci_s2idle_test_usb2(t);
		kunit_activate_static_stub(test, xhci_portsc_readl,
					   xhci_s2idle_test_portsc_readl);
		if (latched) {
			/* Left for the hub driver before running in suspend-to-idle */
			writel(readl(t->portsc) | PORT_OCC, t->portsc);
			t->resume_at_read = read;
			xhci_run_in_s2idle_begin(t->xhci);
		} else {
			xhci_run_in_s2idle_begin(t->xhci);
			KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_port_change(t, 0, PORT_OCC));
			t->reads = 0;
			t->resume_at_read = read;
		}
		xhci_s2idle_test_irq(t);

		if (t->reads < read) {
			KUNIT_EXPECT_GE(test, read, 3U);
			KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 0UL);
			KUNIT_EXPECT_EQ(test, t->xhci->hw_ports->held_changes, (u32)PORT_OCC);
			xhci_run_in_s2idle_end(t->xhci);
			break;
		}
		KUNIT_EXPECT_GE_MSG(test, xhci_s2idle_test_wakeups(t), 1UL,
				    "resume before read %u", read);
		KUNIT_EXPECT_TRUE_MSG(test, xhci_s2idle_test_resuming(t),
				      "resume before read %u", read);
		xhci_run_in_s2idle_end(t->xhci);
	}
}

static void xhci_s2idle_resume_during_hold_test(struct kunit *test)
{
	xhci_s2idle_resume_during_hold(test, XHCI_S2IDLE_TEST_SUSPENDED, false, false);
}

static void xhci_s2idle_usb2_resume_during_hold_test(struct kunit *test)
{
	xhci_s2idle_resume_during_hold(test, XHCI_S2IDLE_TEST_USB2_SUSPENDED, true, false);
}

static void xhci_s2idle_resume_during_begin_hold_test(struct kunit *test)
{
	xhci_s2idle_resume_during_hold(test, XHCI_S2IDLE_TEST_SUSPENDED, false, true);
}

static void xhci_s2idle_usb2_resume_during_begin_hold_test(struct kunit *test)
{
	xhci_s2idle_resume_during_hold(test, XHCI_S2IDLE_TEST_USB2_SUSPENDED, true, true);
}

/*
 * Every change the hub driver reads is held across the sleep, and each is
 * cleared on its own by ClearPortFeature after resume. None comes back in the
 * next sleep.
 */
static void xhci_s2idle_held_clear_test(struct kunit *test)
{
	static const struct {
		u32 hw, usb;
		u16 feature;
	} changes[] = {
		{ PORT_CSC, USB_PORT_STAT_C_CONNECTION, USB_PORT_FEAT_C_CONNECTION },
		{ PORT_OCC, USB_PORT_STAT_C_OVERCURRENT, USB_PORT_FEAT_C_OVER_CURRENT },
		{ PORT_RC, USB_PORT_STAT_C_RESET, USB_PORT_FEAT_C_RESET },
		{ PORT_WRC, USB_PORT_STAT_C_BH_RESET, USB_PORT_FEAT_C_BH_PORT_RESET },
		{ PORT_PLC, USB_PORT_STAT_C_LINK_STATE, USB_PORT_FEAT_C_PORT_LINK_STATE },
		{ PORT_CEC, USB_PORT_STAT_C_CONFIG_ERROR, USB_PORT_FEAT_C_PORT_CONFIG_ERROR },
	};
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_SUSPENDED);
	u32 hw = 0, usb = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(changes); i++) {
		hw |= changes[i].hw;
		usb |= changes[i].usb;
	}
	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_port_change(t, 0, hw));
	xhci_s2idle_test_irq(t);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 0UL);
	KUNIT_EXPECT_EQ(test, readl(t->portsc) & PORT_CHANGE_MASK, 0U);
	xhci_run_in_s2idle_end(t->xhci);

	for (i = 0; i < ARRAY_SIZE(changes); i++) {
		KUNIT_EXPECT_EQ(test, xhci_s2idle_test_port_status(test, t) >> 16, usb);
		xhci_s2idle_test_clear_change(test, t, changes[i].feature);
		usb &= ~changes[i].usb;
	}
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_port_status(test, t) >> 16, 0U);
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_test_rh_change(t));

	xhci_run_in_s2idle_begin(t->xhci);
	xhci_run_in_s2idle_end(t->xhci);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_port_status(test, t) >> 16, 0U);
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_test_rh_change(t));
}

/*
 * A USB 2 link change is cleared but not held: the hub driver never reads one,
 * it reads the end of a resume from port_c_suspend instead.
 */
static void xhci_s2idle_usb2_plc_not_held_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0,
							   XHCI_S2IDLE_TEST_USB2_SUSPENDED);

	xhci_s2idle_test_usb2(t);
	writel(readl(t->portsc) | PORT_PLC | PORT_OCC, t->portsc);
	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 0UL);
	KUNIT_EXPECT_EQ(test, t->xhci->hw_ports->held_changes, (u32)PORT_OCC);
	KUNIT_EXPECT_EQ(test, readl(t->portsc) & PORT_CHANGE_MASK, 0U);
	xhci_run_in_s2idle_end(t->xhci);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_port_status(test, t) >> 16,
			(u32)USB_PORT_STAT_C_OVERCURRENT);
	xhci_s2idle_test_clear_change(test, t, USB_PORT_FEAT_C_OVER_CURRENT);
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_test_rh_change(t));
}

/*
 * Root hub polling stops until resume, as for a halted controller: the event
 * handling decides on every change, and a poll while the system sleeps would
 * only pass held changes to the hub driver of a root hub kept awake.
 */
static void xhci_s2idle_rh_polling_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, 0, XHCI_S2IDLE_TEST_EMPTY);

	/* Polling, as for a change shortly before the system went to sleep */
	set_bit(HCD_FLAG_POLL_RH, &t->hcd->flags);
	mod_timer(&t->hcd->rh_timer, jiffies + HZ);

	xhci_run_in_s2idle_begin(t->xhci);
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_test_rh_polled(t));
	KUNIT_EXPECT_FALSE(test, timer_pending(&t->hcd->rh_timer));
	KUNIT_ASSERT_TRUE(test, xhci_s2idle_test_link_inactive(t));
	xhci_s2idle_test_irq(t);
	KUNIT_EXPECT_FALSE(test, xhci_s2idle_test_rh_polled(t));
	KUNIT_EXPECT_FALSE(test, timer_pending(&t->hcd->rh_timer));

	xhci_run_in_s2idle_end(t->xhci);
	KUNIT_EXPECT_TRUE(test, xhci_s2idle_test_rh_polled(t));
	KUNIT_EXPECT_TRUE(test, timer_pending(&t->hcd->rh_timer));
}

/* A controller error halts it and wakes the system, so that resume deals with it. */
static void xhci_s2idle_controller_error_test(struct kunit *test)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, STS_EINT | STS_HCE | STS_HALT,
							   0);

	xhci_run_in_s2idle_begin(t->xhci);
	xhci_s2idle_test_irq(t);
	KUNIT_EXPECT_TRUE(test, t->xhci->xhc_state & XHCI_STATE_HALTED);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 1UL);
	xhci_run_in_s2idle_end(t->xhci);
}

/*
 * So it does if the failing controller never halts, with its interrupts
 * masked all the same, for a Host Controller and a Host System Error.
 */
static void xhci_s2idle_error_no_halt(struct kunit *test, u32 error)
{
	struct xhci_s2idle_test *t = xhci_s2idle_test_init(test, STS_EINT | error, 0);

	xhci_run_in_s2idle_begin(t->xhci);
	xhci_s2idle_test_irq(t);
	KUNIT_EXPECT_FALSE(test, t->xhci->xhc_state & XHCI_STATE_HALTED);
	KUNIT_EXPECT_EQ(test, readl(&t->xhci->op_regs->command) & (CMD_RUN | XHCI_IRQS), 0U);
	KUNIT_EXPECT_EQ(test, xhci_s2idle_test_wakeups(t), 1UL);
	xhci_run_in_s2idle_end(t->xhci);
}

static void xhci_s2idle_controller_error_no_halt_test(struct kunit *test)
{
	xhci_s2idle_error_no_halt(test, STS_HCE);
}

static void xhci_s2idle_system_error_no_halt_test(struct kunit *test)
{
	xhci_s2idle_error_no_halt(test, STS_FATAL);
}

static struct kunit_case xhci_s2idle_test_cases[] = {
	KUNIT_CASE(xhci_s2idle_link_change_held_test),
	KUNIT_CASE(xhci_s2idle_link_change_resume_test),
	KUNIT_CASE(xhci_s2idle_link_change_awake_test),
	KUNIT_CASE(xhci_s2idle_remote_wakeup_event_test),
	KUNIT_CASE(xhci_s2idle_connect_after_held_change_test),
	KUNIT_CASE(xhci_s2idle_overcurrent_after_held_change_test),
	KUNIT_CASE(xhci_s2idle_disconnect_after_held_change_test),
	KUNIT_CASE(xhci_s2idle_remote_wakeup_after_held_change_test),
	KUNIT_CASE(xhci_s2idle_connect_during_hold_test),
	KUNIT_CASE(xhci_s2idle_change_during_hold_test),
	KUNIT_CASE(xhci_s2idle_change_before_begin_test),
	KUNIT_CASE(xhci_s2idle_wakeup_before_begin_test),
	KUNIT_CASE(xhci_s2idle_usb2_remote_wakeup_test),
	KUNIT_CASE(xhci_s2idle_resume_during_plc_clear_test),
	KUNIT_CASE(xhci_s2idle_usb2_resume_during_plc_clear_test),
	KUNIT_CASE(xhci_s2idle_resume_during_begin_test),
	KUNIT_CASE(xhci_s2idle_resume_during_hold_test),
	KUNIT_CASE(xhci_s2idle_usb2_resume_during_hold_test),
	KUNIT_CASE(xhci_s2idle_resume_during_begin_hold_test),
	KUNIT_CASE(xhci_s2idle_usb2_resume_during_begin_hold_test),
	KUNIT_CASE(xhci_s2idle_held_clear_test),
	KUNIT_CASE(xhci_s2idle_usb2_plc_not_held_test),
	KUNIT_CASE(xhci_s2idle_rh_polling_test),
	KUNIT_CASE(xhci_s2idle_controller_error_test),
	KUNIT_CASE(xhci_s2idle_controller_error_no_halt_test),
	KUNIT_CASE(xhci_s2idle_system_error_no_halt_test),
	{}
};

static struct kunit_suite xhci_s2idle_test_suite = {
	.name = "xhci_s2idle",
	.test_cases = xhci_s2idle_test_cases,
};

kunit_test_suites(&xhci_plat_test_suite, &xhci_s2idle_test_suite);
#else
kunit_test_suite(xhci_plat_test_suite);
#endif /* CONFIG_PM_SLEEP */

MODULE_DESCRIPTION("KUnit tests for the generic xHCI platform driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
