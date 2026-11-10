/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_M4_PROFILE_H
#define APPLE_DCP_M4_PROFILE_H

#include <linux/types.h>

/* Firmware 26.6.2 internal-panel ABI, qualified for the T8132/J713 handoff. */
struct dcp_m4_profile {
	const char *name;
	const char *machine;		/* machine compatible */
	u32 panel_width, panel_height;
	u32 notch_rows;			/* rows the boot loader may hide */
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
	/*
	 * The qualified preferred timing (ID 2), horizontal then vertical:
	 * Active, Total, FrontPorch, SyncWidth, BackPorch, PreciseSyncRate,
	 * SyncPolarity, PixelRepetition. NULL: none, so the panel is not
	 * adopted.
	 */
	const u32 (*timing)[8];
	u32 timing_elements;		/* most timing elements reported */
	/*
	 * D451 may also ask for an alignment that is a power of two up to a
	 * page, besides the 4 every qualified panel firmware asks for.
	 */
	bool alloc_page_align;
};

static const u32 dcp_m4_timing_j713[2][8] = {
	{ 2560, 2640, 8, 32, 40, 6877358, 1, 0 },
	{ 1664, 1749, 33, 8, 44, 3932166, 1, 0 },
};

static const struct dcp_m4_profile dcp_m4_profile_j713 = {
	.name = "J713", .machine = "apple,j713",
	.panel_width = 2560, .panel_height = 1664, .notch_rows = 64,
	.boot_stride = 10240,
	.swap_size = 0x1bd8, .swap_surface = 0x588, .swap_dva = 0xe38,
	.swap_nullable = 0x1bcb, .swap_tail = 0x1bd6, .swap_nits = 0x35e,
	.swap_intent = 0x17c,
	.disabled_color = 0xff000000,
	.done_method = 590, .done_size = 0x730, .open_method = 457,
	.brightness_max = 500, .brightness_default = 120,
	.width_mm = 0, .height_mm = 0,
	.timing = dcp_m4_timing_j713, .timing_elements = 1,
};

/* The J715's panel timing, as macOS reports it (TimingElements, ID 2). */
static const u32 dcp_m4_timing_j715[2][8] = {
	{ 2880, 2960, 8, 32, 40, 7710975, 1, 0 },
	{ 1864, 1961, 47, 8, 42, 3932165, 1, 0 },
};

/* The J715: the same firmware ABI with a 2880x1864 panel. */
static const struct dcp_m4_profile dcp_m4_profile_j715 = {
	.name = "J715", .machine = "apple,j715",
	.panel_width = 2880, .panel_height = 1864, .notch_rows = 64,
	.boot_stride = 11520,
	.swap_size = 0x1bd8, .swap_surface = 0x588, .swap_dva = 0xe38,
	.swap_nullable = 0x1bcb, .swap_tail = 0x1bd6, .swap_nits = 0x35e,
	.swap_intent = 0x17c,
	.disabled_color = 0xff000000,
	.done_method = 590, .done_size = 0x730, .open_method = 457,
	.brightness_max = 500, .brightness_default = 120,
	.width_mm = 0, .height_mm = 0,
	.timing = dcp_m4_timing_j715, .timing_elements = 1,
};

/* The J604's preferred 120 Hz timing (macOS TimingElements, ID 2). */
static const u32 dcp_m4_timing_j604[2][8] = {
	{ 3024, 3104, 8, 32, 40, 16986999, 1, 0 },
	{ 1964, 2160, 102, 8, 86, 7864351, 1, 0 },
};

/*
 * The J604: assumed to be the same firmware ABI, with a 3024x1964 XDR panel
 * rated 1000 nits for SDR. It reports six timings; only ID 2 is adopted.
 */
static const struct dcp_m4_profile dcp_m4_profile_j604 = {
	.name = "J604", .machine = "apple,j604",
	.panel_width = 3024, .panel_height = 1964, .notch_rows = 74,
	.boot_stride = 12096,
	.swap_size = 0x1bd8, .swap_surface = 0x588, .swap_dva = 0xe38,
	.swap_nullable = 0x1bcb, .swap_tail = 0x1bd6, .swap_nits = 0x35e,
	.swap_intent = 0x17c,
	.disabled_color = 0xff000000,
	.done_method = 590, .done_size = 0x730, .open_method = 457,
	.brightness_max = 1000, .brightness_default = 120,
	.width_mm = 0, .height_mm = 0,
	.timing = dcp_m4_timing_j604, .timing_elements = 6,
	.alloc_page_align = true,
};

static const struct dcp_m4_profile *const dcp_m4_profiles[] = {
	&dcp_m4_profile_j713,
	&dcp_m4_profile_j715,
	&dcp_m4_profile_j604,
};

/* The machine's profile, set before the first DCP session starts. */
extern const struct dcp_m4_profile *dcp_m4_pf;

#endif
