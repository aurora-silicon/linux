/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_DISPLAY_METADATA_H
#define APPLE_DCP_DISPLAY_METADATA_H

#include <linux/types.h>

/* Values reported by DisplayAttributes, not a copy of the sink's EDID. */
struct dcp_display_metadata {
	u32 width_cm, height_cm;
	u32 chromaticity[3][2];
	u32 white[2];
	u32 gamma;
	u32 product, serial, week, year;
	char manufacturer[4];
	char name[14];
	char serial_text[14];
	/* Optional HDR capability: false and zero when not reported */
	bool pq, static_metadata_type1, bt2020_rgb;
	u32 max_luminance, max_fall, min_luminance; /* 16.16 cd/m2 */
};

/* Calibrated target named by the boot-populated raw ADT. */
enum dcp_panel_gamut {
	DCP_PANEL_GAMUT_UNSPECIFIED,
	DCP_PANEL_GAMUT_SRGB,
	DCP_PANEL_GAMUT_DISPLAY_P3,
};

/*
 * A standard named colour space a panel is calibrated to, selected by the
 * boot-populated raw ADT. Coordinates are CIE 1931 x/y in units of 1/65536
 * and gamma is 16.16, as in the metadata.
 */
struct dcp_color_space {
	const char *name;
	u32 chromaticity[3][2];
	u32 white[2];
	u32 gamma;
};

const struct dcp_color_space *dcp_color_space_lookup(const char *name);
const struct dcp_color_space *dcp_panel_gamut_color_space(enum dcp_panel_gamut gamut);

struct drm_edid;
struct drm_display_mode;
struct dcp_display_mode;
const struct drm_edid *dcp_metadata_edid(const struct dcp_display_metadata *metadata,
					 const struct drm_display_mode *mode);

const struct drm_edid *dcp_panel_edid(const struct dcp_display_metadata *metadata,
				      const struct dcp_color_space *target,
				      const struct dcp_display_mode *modes, unsigned int count,
				      u32 width_mm, u32 height_mm);

#endif
