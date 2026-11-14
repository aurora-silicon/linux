/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * What the Apple Type-C PHY offers the USB controller it serves, beyond the
 * generic PHY operations.
 */
#ifndef _LINUX_PHY_APPLE_ATC_H_
#define _LINUX_PHY_APPLE_ATC_H_

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/phy/phy.h>

/* In D0 after the sleep, but its always-on side lost power, so xHCI's saved state is gone. */
#define APPLE_ATCPHY_SLEEP_POWER_LOST	1

/*
 * The PHY held the controller in reset through the sleep, with its own USB2 and USB3 sides
 * down, and has released the reset: the core's registers are gone whether or not the SoC
 * lost power, and so is xHCI's saved state. The PHY's own sides come back with
 * apple_typec_phy_sleep_power_up(), once the core is set up again.
 */
#define APPLE_ATCPHY_SLEEP_RESET	2

extern const struct phy_ops apple_atc_usb2_phy_ops;

/*
 * For a firmware-driven system sleep, before any device suspends and without the
 * controller's locks held. 0 means the PHY takes the controller through hibernation after
 * xHCI has saved its state, then its DARTs and power domains down, and brings all of it
 * back before xHCI resumes, so the host can stay up; with USB3 its PIPE is parked for it.
 * Otherwise the controller has to be stopped for the sleep.
 */
int apple_atcphy_sleep_prepare(struct phy *usb2_phy);

/*
 * After the sleep: 0 if the controller is in D0 and its always-on side kept power, or
 * APPLE_ATCPHY_SLEEP_POWER_LOST. -ENODEV if it may be unpowered, so that its registers must
 * not be touched. Any other error: it's in D0, but the PHY isn't set up for it.
 */
int apple_atcphy_sleep_result(struct phy *usb2_phy);

/*
 * For a controller that may be unpowered after a sleep: power cycles it, as macOS's Off
 * and cold start do. APPLE_ATCPHY_SLEEP_POWER_LOST once it's back, reset, or -ENODEV.
 */
int apple_atcphy_sleep_recover(struct phy *usb2_phy);

/*
 * Ends a sleep that prepare took on, with the SuperSpeed side back as the mux wants it.
 * Without @kept the host was restarted; only a failure of the PHY's own then makes the
 * controller stop for later sleeps.
 */
void apple_atcphy_sleep_finish(struct phy *usb2_phy, bool kept);

/* The same from the PHY's work, for after a wake; the flush waits for it. */
void apple_atcphy_sleep_finish_async(struct phy *usb2_phy, bool kept);
void apple_atcphy_sleep_finish_flush(struct phy *usb2_phy);

/* ACIO owns this transaction; @phy is its shared USB3 lane PHY handle. */
#if IS_REACHABLE(CONFIG_PHY_APPLE_ATC)
int apple_atcphy_cio_prepare(struct phy *phy);
int apple_atcphy_cio_off(struct phy *phy);
int apple_atcphy_cio_on(struct phy *phy);
int apple_atcphy_cio_finish(struct phy *phy);
#else
static inline int apple_atcphy_cio_prepare(struct phy *phy)
{
	return -EOPNOTSUPP;
}

static inline int apple_atcphy_cio_off(struct phy *phy)
{
	return -EOPNOTSUPP;
}

static inline int apple_atcphy_cio_on(struct phy *phy)
{
	return -EOPNOTSUPP;
}

static inline int apple_atcphy_cio_finish(struct phy *phy)
{
	return -EOPNOTSUPP;
}
#endif

extern const struct phy_ops apple_t6040_usb2_phy_ops;

/*
 * The T6040 Type-C PHY's side of the same sleep, for a Type-C port in host mode with nothing
 * but USB on its lanes. It has no hibernation to take the controller through: it takes its
 * USB3 and USB2 sides down and the controller into reset, as a stop of the host does, and
 * brings them back the way a start does. That boots the PHY's coprocessor, so none of it
 * runs with interrupts off: the controller's glue calls each step from process context.
 * Each returns -EOPNOTSUPP for a PHY that isn't this driver's.
 *
 * apple_t6040_atcphy_sleep_prepare(): before any device suspends. 0 if the port can sleep
 * this way now, and the PHY then holds on to that until the finish.
 */
int apple_t6040_atcphy_sleep_prepare(struct phy *usb2_phy);

/*
 * Once xHCI has suspended, and before the controller's power domain may go off: USB3 down
 * and its PIPE parked, USB2 off, the controller in reset. -EBUSY, with nothing touched, if
 * the port has changed since the prepare; the sleep has to be refused then.
 */
int apple_t6040_atcphy_sleep_enter(struct phy *usb2_phy);

/*
 * After the sleep, with the controller's power domain on again and before anything touches
 * its core. 0: the PHY was never taken down (the sleep was abandoned first), and all is as
 * it was. APPLE_ATCPHY_SLEEP_RESET: the controller is out of reset again, which the first
 * call after the sleep sees to. -ENODEV: the reset didn't release, so the core must not
 * be touched. Any other error: the controller is out of reset, but USB2 didn't come back.
 */
int apple_t6040_atcphy_sleep_result(struct phy *usb2_phy);

/*
 * After APPLE_ATCPHY_SLEEP_RESET, once the core is set up again and before its PIPE is
 * routed: USB2 on, and USB3 up if it ran before the sleep. May sleep for the coprocessor's
 * boot. USB3 that doesn't come up leaves the port at USB2, as at a start of the host, and
 * is not an error. An error is USB2's, and leaves the controller out of reset for a restart
 * of the host.
 */
int apple_t6040_atcphy_sleep_power_up(struct phy *usb2_phy);

/* For -ENODEV: tries the reset's release again. APPLE_ATCPHY_SLEEP_RESET, or -ENODEV. */
int apple_t6040_atcphy_sleep_recover(struct phy *usb2_phy);

/*
 * Ends what the prepare took on. Without @kept, a failure of the PHY's own makes the host
 * stop for sleeps until it has started again.
 */
void apple_t6040_atcphy_sleep_finish(struct phy *usb2_phy, bool kept);

/* Whether @usb2_phy is the USB2 PHY of a driver that is built for this kernel. */
static inline bool apple_atcphy_is_atc(struct phy *usb2_phy)
{
	return IS_REACHABLE(CONFIG_PHY_APPLE_ATC) && usb2_phy &&
	       usb2_phy->ops == &apple_atc_usb2_phy_ops;
}

static inline bool apple_atcphy_is_t6040(struct phy *usb2_phy)
{
	return IS_REACHABLE(CONFIG_PHY_APPLE_T6040_USB2) && usb2_phy &&
	       usb2_phy->ops == &apple_t6040_usb2_phy_ops;
}

/*
 * What the controller's glue calls: the driver that @usb2_phy belongs to, found by the PHY's
 * ops, answers. A step that only one of the drivers has is nothing to do for the other's PHYs,
 * and for any other PHY.
 */
static inline int apple_typec_phy_sleep_prepare(struct phy *usb2_phy)
{
	if (apple_atcphy_is_atc(usb2_phy))
		return apple_atcphy_sleep_prepare(usb2_phy);
	if (apple_atcphy_is_t6040(usb2_phy))
		return apple_t6040_atcphy_sleep_prepare(usb2_phy);
	return -EOPNOTSUPP;
}

static inline int apple_typec_phy_sleep_enter(struct phy *usb2_phy)
{
	return apple_atcphy_is_t6040(usb2_phy) ? apple_t6040_atcphy_sleep_enter(usb2_phy) : 0;
}

static inline int apple_typec_phy_sleep_result(struct phy *usb2_phy)
{
	if (apple_atcphy_is_atc(usb2_phy))
		return apple_atcphy_sleep_result(usb2_phy);
	if (apple_atcphy_is_t6040(usb2_phy))
		return apple_t6040_atcphy_sleep_result(usb2_phy);
	return -EOPNOTSUPP;
}

static inline int apple_typec_phy_sleep_power_up(struct phy *usb2_phy)
{
	return apple_atcphy_is_t6040(usb2_phy) ? apple_t6040_atcphy_sleep_power_up(usb2_phy) : 0;
}

static inline int apple_typec_phy_sleep_recover(struct phy *usb2_phy)
{
	if (apple_atcphy_is_atc(usb2_phy))
		return apple_atcphy_sleep_recover(usb2_phy);
	if (apple_atcphy_is_t6040(usb2_phy))
		return apple_t6040_atcphy_sleep_recover(usb2_phy);
	return -EOPNOTSUPP;
}

static inline void apple_typec_phy_sleep_finish(struct phy *usb2_phy, bool kept)
{
	if (apple_atcphy_is_atc(usb2_phy))
		apple_atcphy_sleep_finish(usb2_phy, kept);
	else if (apple_atcphy_is_t6040(usb2_phy))
		apple_t6040_atcphy_sleep_finish(usb2_phy, kept);
}

/* The T6040 PHY's finish has nothing to wait for, so it ends at once. */
static inline void apple_typec_phy_sleep_finish_async(struct phy *usb2_phy, bool kept)
{
	if (apple_atcphy_is_atc(usb2_phy))
		apple_atcphy_sleep_finish_async(usb2_phy, kept);
	else if (apple_atcphy_is_t6040(usb2_phy))
		apple_t6040_atcphy_sleep_finish(usb2_phy, kept);
}

static inline void apple_typec_phy_sleep_finish_flush(struct phy *usb2_phy)
{
	if (apple_atcphy_is_atc(usb2_phy))
		apple_atcphy_sleep_finish_flush(usb2_phy);
}

#endif
