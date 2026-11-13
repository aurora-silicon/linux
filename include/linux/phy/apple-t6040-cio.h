/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple T6040 Type-C PHY: the USB4 host router's view of the PHY's CIO
 * (USB4/Thunderbolt) mode. The Type-C controller puts the PHY into CIO
 * through the mux before it hands the cable to the router, and takes it out
 * after the router has stopped. The router uses these calls to check that
 * order and to tell the PHY whether it runs, which the PHY needs before it
 * takes CIO down for system sleep. @phy is the PHY the router's "phys"
 * property names, got with devm_phy_get(): the PHY core links the router to it
 * as its consumer, so the PHY is bound before the router probes.
 */
#ifndef __LINUX_PHY_APPLE_T6040_CIO_H
#define __LINUX_PHY_APPLE_T6040_CIO_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct phy;

#if IS_REACHABLE(CONFIG_PHY_APPLE_T6040_USB2)
/*
 * 1: CIO up and tuned by the PHY firmware; 0: not in CIO; -ENODEV: @phy is
 * not a T6040 Type-C PHY.
 */
int apple_t6040_cio_state(struct phy *phy);

/*
 * Before the router starts: marks it running on the PHY's CIO link.
 * -EAGAIN unless CIO is up; -ENODEV as above.
 */
int apple_t6040_cio_router_started(struct phy *phy);

/*
 * After every router stop, once ACIO is powered down (unplug, error, and the
 * stop for system sleep, which must come before any device's .suspend,
 * .freeze or .poweroff). Undoes apple_t6040_cio_router_started().
 */
void apple_t6040_cio_router_stopped(struct phy *phy);

/*
 * The router carries (@on) or no longer carries USB3 for the port's USB
 * controller: the controller's PIPE follows. -EAGAIN when @on and the router
 * is not marked running.
 */
int apple_t6040_cio_usb3_tunnel(struct phy *phy, bool on);
#else
static inline int apple_t6040_cio_state(struct phy *phy)
{
	return -ENODEV;
}

static inline int apple_t6040_cio_router_started(struct phy *phy)
{
	return -ENODEV;
}

static inline void apple_t6040_cio_router_stopped(struct phy *phy)
{
}

static inline int apple_t6040_cio_usb3_tunnel(struct phy *phy, bool on)
{
	return -ENODEV;
}
#endif

#endif
