/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 Ryan Murray */

#ifndef __APPLE_BOOT_DISPLAY_H__
#define __APPLE_BOOT_DISPLAY_H__

#include <drm/drm_connector.h>
#include <linux/minmax.h>
#include <linux/types.h>

/*
 * Which display processors can be scanning out the boot framebuffer that
 * simplefb (or simpledrm) shows until this driver takes over.
 *
 * The boot loader puts its framebuffer on a built-in panel when the machine
 * has one, and otherwise on a fixed port such as HDMI. A processor whose
 * only output is Type-C can carry it only on a machine with nothing else.
 * Removing the boot framebuffer is safe only once a processor of the
 * highest of these classes that the machine has is running: otherwise the
 * screen the user is looking at goes black, with nothing behind it.
 */
enum apple_boot_display_class {
	APPLE_BOOT_DISPLAY_TYPEC = 0,
	APPLE_BOOT_DISPLAY_FIXED = 1,
	APPLE_BOOT_DISPLAY_PANEL = 2,
};

static inline enum apple_boot_display_class
apple_boot_display_class(int connector_type)
{
	switch (connector_type) {
	case DRM_MODE_CONNECTOR_eDP:
		return APPLE_BOOT_DISPLAY_PANEL;
	case DRM_MODE_CONNECTOR_HDMIA:
	case DRM_MODE_CONNECTOR_DisplayPort:
		return APPLE_BOOT_DISPLAY_FIXED;
	default:
		return APPLE_BOOT_DISPLAY_TYPEC;
	}
}

/**
 * apple_boot_display_up() - whether a processor that can carry the boot
 *	framebuffer came up
 * @connector_type: the fixed connector type of each display processor
 * @ready: whether each display processor started and became ready
 * @num: the number of display processors
 *
 * Return: true if a processor of the highest class present is ready, false
 * if none is or @num is 0.
 */
static inline bool apple_boot_display_up(const int *connector_type,
					 const bool *ready, unsigned int num)
{
	enum apple_boot_display_class best = APPLE_BOOT_DISPLAY_TYPEC;
	unsigned int i;

	for (i = 0; i < num; i++)
		best = max(best, apple_boot_display_class(connector_type[i]));
	for (i = 0; i < num; i++)
		if (ready[i] && apple_boot_display_class(connector_type[i]) == best)
			return true;
	return false;
}

#endif /* __APPLE_BOOT_DISPLAY_H__ */
