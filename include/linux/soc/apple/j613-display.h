/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _APPLE_J613_DISPLAY_H
#define _APPLE_J613_DISPLAY_H

#include <linux/kconfig.h>
#include <linux/types.h>

struct device;
struct fwnode_handle;

#if IS_ENABLED(CONFIG_APPLE_J613_DISPLAY_SYNC)
bool apple_j613_display_begin(void);
void apple_j613_display_end(int ret);
void apple_j613_display_fail(void);
bool apple_j613_display_pending(struct device *dev);
bool apple_j613_display_skip(const struct fwnode_handle *node, bool missing);
#else
static inline bool apple_j613_display_begin(void)
{
	return true;
}

static inline void apple_j613_display_end(int ret) { }
static inline void apple_j613_display_fail(void) { }

static inline bool apple_j613_display_pending(struct device *dev)
{
	return false;
}

static inline bool apple_j613_display_skip(const struct fwnode_handle *node, bool missing)
{
	return false;
}
#endif
#endif
