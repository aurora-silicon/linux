/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Apple Silicon DWC3 glue: controller states and the USB2 PHY role choice,
 * kept in a header so that the choice can be tested without the hardware.
 */

#ifndef __DWC3_APPLE_STATE_H
#define __DWC3_APPLE_STATE_H

#include <linux/errno.h>
#include <linux/phy/phy.h>

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

#endif /* __DWC3_APPLE_STATE_H */
