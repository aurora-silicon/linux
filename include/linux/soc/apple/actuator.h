/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __APPLE_MTP_ACTUATOR_H__
#define __APPLE_MTP_ACTUATOR_H__

#include <linux/hid.h>
#include <linux/kconfig.h>
#include <linux/string.h>

#define APPLE_HP_WAVEFORMDEEPCLICK	0x000e2001

/*
 * The actuator is a HID device of its own, named by the transport: "Apple
 * MTP actuator" on the MTP machines, "Apple SPI Actuator" on the SPI ones.
 */
static inline bool apple_taptic_is_actuator(struct hid_device *hdev)
{
	return !strcmp(hdev->name, "Apple MTP actuator") ||
	       !strcmp(hdev->name, "Apple SPI Actuator");
}

#if IS_REACHABLE(CONFIG_HID_APPLE_MTP_HAPTIC)
int apple_taptic_send(struct hid_device *hdev, u16 effect_type, u8 strength, u8 softness);
int apple_taptic_switch_modes(struct hid_device *hdev, bool host_controlled);

#else
static inline int apple_taptic_send(struct hid_device *hdev, u16 effect_type,
				    u8 strength, u8 softness)
{
	return -ENODEV;
}

static inline int apple_taptic_switch_modes(struct hid_device *hdev,
					    bool host_controlled)
{
	return -ENODEV;
}
#endif
#endif /* __APPLE_MTP_ACTUATOR_H__ */
