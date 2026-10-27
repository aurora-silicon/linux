/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_M4_PROFILE_H
#define APPLE_DCP_M4_PROFILE_H

#include <linux/types.h>

/* Firmware 26.6.2 internal-panel ABI, qualified for the T8132/J713 handoff. */
struct dcp_m4_profile {
	const char *name;
	u32 panel_width, panel_height;
	u32 boot_stride;		/* D101: inherited framebuffer stride */
	u32 swap_size;			/* A407 input */
	u32 swap_surface;		/* plane-0 surface */
	u32 swap_dva;			/* plane-0 surface DVA */
	u32 swap_nullable;		/* ten nullable surface bytes */
	u32 swap_tail;			/* final two bytes */
	u32 swap_nits;			/* binary64 brightness */
	u32 swap_intent;		/* swap intent, 0 for a present */
	u32 disabled_color;		/* background with no surface */
	u16 done_method;		/* swap completion callback */
	u32 done_size;
	u16 open_method;		/* first client open */
	u32 brightness_max, brightness_default;
	u32 width_mm, height_mm;
};

static const struct dcp_m4_profile dcp_m4_profile_j713 = {
	.name = "J713",
	.panel_width = 2560, .panel_height = 1664, .boot_stride = 10240,
	.swap_size = 0x1bd8, .swap_surface = 0x588, .swap_dva = 0xe38,
	.swap_nullable = 0x1bcb, .swap_tail = 0x1bd6, .swap_nits = 0x35e,
	.swap_intent = 0x17c,
	.disabled_color = 0xff000000,
	.done_method = 590, .done_size = 0x730, .open_method = 457,
	.brightness_max = 500, .brightness_default = 120,
	.width_mm = 0, .height_mm = 0,
};

/* This generation backend admits one qualified internal-panel ABI. */
static const struct dcp_m4_profile *const dcp_m4_pf = &dcp_m4_profile_j713;

#endif
