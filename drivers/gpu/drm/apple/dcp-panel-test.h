/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Notch-hidden mode geometry observed through the public DRM interface. */
static const struct dcp_display_mode panel_test_mode = {
	.mode = {
		.clock = 521065, .hdisplay = 3456, .hsync_start = 3464,
		.hsync_end = 3496, .htotal = 3536, .vdisplay = 2160,
		.vsync_start = 2278, .vsync_end = 2360, .vtotal = 2456,
		.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
		.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	},
	.vrr = true, .min_vrr = 24U << 16, .max_vrr = 120U << 16,
};
