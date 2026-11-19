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
/* Nonblocking admission query; the caller serializes finish requests into epochs. */
bool apple_atcphy_sleep_finish_done(struct phy *usb2_phy);
void apple_atcphy_sleep_finish_flush(struct phy *usb2_phy);

/* ACIO owns this transaction; @phy is its shared USB3 lane PHY handle. */
int apple_atcphy_cio_prepare(struct phy *phy);
int apple_atcphy_cio_off(struct phy *phy);
int apple_atcphy_cio_on(struct phy *phy);
int apple_atcphy_cio_finish(struct phy *phy);
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

static inline bool apple_atcphy_sleep_finish_done(struct phy *usb2_phy)
{
	return true;
}

static inline void apple_atcphy_sleep_finish_flush(struct phy *usb2_phy)
{
}

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

/*
 * The T6050 combo PHY (apple-t6050-dp-phy) under a USB4 router that sleeps in place: @dev is
 * the PHY's device. Its module may load after the caller's: take these with symbol_get().
 */
int apple_t6050_phy_cio_prepare(struct device *dev);
int apple_t6050_phy_cio_off(struct device *dev);
int apple_t6050_phy_cio_on(struct device *dev);
int apple_t6050_phy_cio_finish(struct device *dev);
int apple_t6050_phy_cio_lost(struct device *dev);

/*
 * TEST (J714s): the port's USB host on a USB4 router's USB3 tunnel, kept through the router's
 * sleep in place. The router's host BINDs and UNBINDs, the USB controller asks to KEEP its
 * host at the sleep's start, the router's host PARKs it once the router firmware has taken
 * its sleep (or once a refused sleep has woken the router), and the USB controller takes the
 * RESULT after the wake: one of APPLE_T6050_CIO_USB_* or an error.
 */
enum apple_t6050_cio_usb {
	APPLE_T6050_CIO_USB_BIND,
	APPLE_T6050_CIO_USB_UNBIND,
	APPLE_T6050_CIO_USB_KEEP,
	APPLE_T6050_CIO_USB_PARK,
	APPLE_T6050_CIO_USB_RESULT,
	/*
	 * Not a tunnel step: whether the port's USB3 PIPE is on the PHY's own lanes now (1), or
	 * not (0: four DisplayPort lanes, a USB2-only fallback, the USB4 tunnel, no mode).
	 */
	APPLE_T6050_CIO_USB_PIPE_LANES,
};
/* Kept on the tunnel's PIPE, untouched: xHCI resumes with its state. */
#define APPLE_T6050_CIO_USB_KEPT	0
/* PIPE parked on its dummy producer, the controller kept its registers: USB3 restarts later. */
#define APPLE_T6050_CIO_USB_PARKED	1
/* The PHY lost the controller in a firmware sleep: set it up again, xHCI starts over. */
#define APPLE_T6050_CIO_USB_LOST	2
int apple_t6050_phy_cio_usb(struct device *dev, enum apple_t6050_cio_usb step);

#endif
