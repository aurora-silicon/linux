/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_PANEL_ADT_H
#define APPLE_DCP_PANEL_ADT_H

#include "dcp-display-metadata.h"

/* Only panel properties, never the machine or account identity. */
struct dcp_panel_adt {
	struct dcp_display_metadata identity;
	u32 width_mm, height_mm;
	/* /backlight EDRPotentialHeadroom, 16.16; 0 if absent */
	u32 edr_potential_headroom;
	/* /backlight LmaxProduct, the panel's peak luminance, 16.16 nits; 0 if absent */
	u32 lmax_product;
	/* /product target, only when its calibration matrix is also valid */
	enum dcp_panel_gamut gamut;
	bool found;
};

int dcp_panel_adt_parse(const void *data, size_t size, struct dcp_panel_adt *panel);
#endif
