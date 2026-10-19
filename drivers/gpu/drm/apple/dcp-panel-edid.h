/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* A base EDID for a built-in panel, from its device-tree node. */
#ifndef __APPLE_DCP_PANEL_EDID_H__
#define __APPLE_DCP_PANEL_EDID_H__

#include <linux/types.h>

/* The monitor-name text of an EDID descriptor holds 13 characters. */
#define DCP_PANEL_NAME_LEN 13

/* The make and product of a panel, as its compatible gives them. */
struct dcp_panel_id {
	char vendor[4];				/* PNP ID, empty if unknown */
	char name[DCP_PANEL_NAME_LEN + 1];	/* monitor name */
};

struct drm_connector;
struct edid;

int dcp_panel_id_from_compatible(struct dcp_panel_id *id, const char *compatible);
int dcp_panel_edid_block(const struct dcp_panel_id *id, int width_mm, int height_mm,
			 struct edid *block);
int dcp_panel_edid_update(struct drm_connector *connector, const struct dcp_panel_id *id,
			  int width_mm, int height_mm);

#endif
