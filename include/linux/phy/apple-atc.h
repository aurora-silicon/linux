/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * What the Apple Type-C PHY offers the USB controller it serves, beyond the
 * generic PHY operations.
 */
#ifndef _LINUX_PHY_APPLE_ATC_H_
#define _LINUX_PHY_APPLE_ATC_H_

#include <linux/errno.h>
#include <linux/kconfig.h>

struct phy;

/* In D0 after the sleep, but its always-on side lost power, so xHCI's saved state is gone. */
#define APPLE_ATCPHY_SLEEP_POWER_LOST	1

#if IS_REACHABLE(CONFIG_PHY_APPLE_ATC)
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
#else
static inline int apple_atcphy_sleep_prepare(struct phy *usb2_phy)
{
	return -EOPNOTSUPP;
}

static inline int apple_atcphy_sleep_result(struct phy *usb2_phy)
{
	return -EOPNOTSUPP;
}

static inline int apple_atcphy_sleep_recover(struct phy *usb2_phy)
{
	return -EOPNOTSUPP;
}

static inline void apple_atcphy_sleep_finish(struct phy *usb2_phy, bool kept)
{
}

static inline void apple_atcphy_sleep_finish_async(struct phy *usb2_phy, bool kept)
{
}

static inline void apple_atcphy_sleep_finish_flush(struct phy *usb2_phy)
{
}
#endif

#endif
