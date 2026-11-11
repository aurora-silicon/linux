/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Apple Silicon DWC3 glue: controller states, the USB2 PHY role choice and
 * the decisions of a sleep with the host kept in place, in a header so that
 * they can be tested without the hardware.
 */

#ifndef __DWC3_APPLE_STATE_H
#define __DWC3_APPLE_STATE_H

#include <linux/errno.h>
#include <linux/phy/apple-atc.h>
#include <linux/phy/phy.h>
#include <linux/usb/role.h>

/* See the comment at the top of dwc3-apple.c for how these states are used. */
enum dwc3_apple_state {
	DWC3_APPLE_PROBE_PENDING, /* Before first cable connection, dwc3_core_probe not called */
	DWC3_APPLE_NO_CABLE, /* No cable connected, dwc3 suspended after dwc3_core_exit */
	DWC3_APPLE_HOST, /* Cable connected, dwc3 in host mode */
	DWC3_APPLE_DEVICE, /* Cable connected, dwc3 in device mode */
};

/**
 * dwc3_apple_usb2_mode_before_reset() - USB2 PHY mode to select before dwc3 leaves reset
 * @target: the state the controller is brought up in
 * @mode: set to the mode to select, or to PHY_MODE_INVALID to select none
 *
 * Device mode is selected before the reset is released, because nothing selects it
 * later. Host mode is left to the xHCI root hub, which selects it once the core is out
 * of reset: on a Type-C port the PHY is already running when the glue brings the core
 * up, and switching it to host mode while the core is held in reset leaves the USB2
 * port without a connection for the whole session.
 *
 * Return: 0, or -EINVAL if @target is not DWC3_APPLE_HOST or DWC3_APPLE_DEVICE.
 */
static inline int dwc3_apple_usb2_mode_before_reset(enum dwc3_apple_state target,
						    enum phy_mode *mode)
{
	switch (target) {
	case DWC3_APPLE_HOST:
		*mode = PHY_MODE_INVALID;
		return 0;
	case DWC3_APPLE_DEVICE:
		*mode = PHY_MODE_USB_DEVICE;
		return 0;
	default:
		*mode = PHY_MODE_INVALID;
		return -EINVAL;
	}
}

/**
 * dwc3_apple_state_for_role() - state a role asks for while the core is stopped for sleep
 * @role: the role asked for
 *
 * Return: the state to bring the controller up in after the sleep.
 */
static inline enum dwc3_apple_state dwc3_apple_state_for_role(enum usb_role role)
{
	switch (role) {
	case USB_ROLE_HOST:
		return DWC3_APPLE_HOST;
	case USB_ROLE_DEVICE:
		return DWC3_APPLE_DEVICE;
	default:
		return DWC3_APPLE_NO_CABLE;
	}
}

/**
 * dwc3_apple_state_for_role_kept() - the same for a host kept in place through the sleep
 * @role: the role asked for
 * @host_only: the controller has no device mode
 *
 * Nothing is stopped or started while the sleep lasts, so what dwc3_usb_role_switch_set()
 * decides for a running controller is decided here: no device mode on a host-only port.
 *
 * Return: the state the port goes to once the sleep is over.
 */
static inline enum dwc3_apple_state dwc3_apple_state_for_role_kept(enum usb_role role,
								   bool host_only)
{
	if (role == USB_ROLE_DEVICE && host_only)
		return DWC3_APPLE_NO_CABLE;

	return dwc3_apple_state_for_role(role);
}

/**
 * dwc3_apple_sleep_may_keep() - whether a deep sleep may find the host kept in place
 * @fixed_hub: the controller serves a fixed hub
 * @phy_restart: a Type-C port whose PHY keeps the host by the core's reset
 * @typec_allowed: Type-C ports may sleep in place (the typec_sleep_in_place parameter)
 * @state: the controller's state
 * @shutting_down: the glue is going away
 * @dead: the core didn't come back from an earlier sleep
 *
 * The glue's side of the question. The PHY then has the last word, as only it knows what
 * runs on the port's lanes (apple_typec_phy_sleep_prepare()).
 *
 * Return: true if the PHY is to be asked.
 */
static inline bool dwc3_apple_sleep_may_keep(bool fixed_hub, bool phy_restart, bool typec_allowed,
					     enum dwc3_apple_state state, bool shutting_down,
					     bool dead)
{
	return (fixed_hub || (phy_restart && typec_allowed)) && state == DWC3_APPLE_HOST &&
	       !shutting_down && !dead;
}

/**
 * dwc3_apple_sleep_reads_mark() - whether the core's mark says anything after the sleep
 * @phy: what the PHY says of the sleep (apple_typec_phy_sleep_result())
 *
 * The mark is a value written to a scratch register of the core before the sleep. After an
 * error the core may be unpowered and must not be read, and a core the PHY took through its
 * reset has lost the mark whatever the SoC did.
 *
 * Return: true if the register may be read and its value counts.
 */
static inline bool dwc3_apple_sleep_reads_mark(int phy)
{
	return !phy || phy == -EBUSY || phy == APPLE_ATCPHY_SLEEP_POWER_LOST;
}

/* What the host's resume does after a sleep in place, see dwc3_apple_sleep_wake_plan(). */
enum dwc3_apple_wake {
	DWC3_APPLE_WAKE_AS_IS,		/* Nothing was lost: xHCI resumes as usual */
	DWC3_APPLE_WAKE_RESTORE,	/* The core is set up again, xHCI restores its state */
	DWC3_APPLE_WAKE_START_OVER,	/* The core is set up again, xHCI starts over */
	DWC3_APPLE_WAKE_PHY_START_OVER,	/* The same, and the PHY comes back after the core */
	DWC3_APPLE_WAKE_FAILED,		/* xHCI must not resume, nor anything touch the core */
};

/**
 * dwc3_apple_sleep_wake_plan() - what the host's resume does after a sleep in place
 * @phy: what the PHY says of the sleep (apple_typec_phy_sleep_result())
 * @core_kept: the core still holds its mark; false where dwc3_apple_sleep_reads_mark()
 *	       says not to read it
 *
 * A sleep refused before the PHY did anything (-EBUSY) left all as it was. A PHY that took
 * the core through hibernation brings it back with its registers (0), or without them if a
 * power domain went off under it: xHCI's saved state then still counts unless the core's
 * always-on side lost power as well. A PHY that held the core in reset says so, and the
 * host starts over whatever the mark reads.
 *
 * Return: the plan.
 */
static inline enum dwc3_apple_wake dwc3_apple_sleep_wake_plan(int phy, bool core_kept)
{
	switch (phy) {
	case APPLE_ATCPHY_SLEEP_RESET:
		return DWC3_APPLE_WAKE_PHY_START_OVER;
	case APPLE_ATCPHY_SLEEP_POWER_LOST:
		return core_kept ? DWC3_APPLE_WAKE_AS_IS : DWC3_APPLE_WAKE_START_OVER;
	case 0:
	case -EBUSY:
		return core_kept ? DWC3_APPLE_WAKE_AS_IS : DWC3_APPLE_WAKE_RESTORE;
	default:
		return DWC3_APPLE_WAKE_FAILED;
	}
}

#endif /* __DWC3_APPLE_STATE_H */
