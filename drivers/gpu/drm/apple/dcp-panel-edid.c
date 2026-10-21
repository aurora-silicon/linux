// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * A base EDID for a built-in panel.
 *
 * The DCP gives no EDID for a built-in panel, so a compositor sees no make,
 * model or physical size for it. This builds one 128-byte block from what the
 * panel's device-tree node says: the vendor and model of its most specific
 * compatible ("apple,panel-j516": make APP, name J516) and its size.
 *
 * The block makes no other claim: no product code or serial number, no
 * manufacture date, no colour primaries, no timings and no refresh range. The
 * modes and the refresh range stay the firmware's. An EDID holds the size in
 * whole centimetres, so the connector gets back the size in millimetres
 * after the update.
 */

#include <linux/ctype.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include <drm/drm_connector.h>
#include <drm/drm_edid.h>

#include "dcp-panel-edid.h"

/* Device-tree vendor prefixes and their PNP IDs. */
static const struct {
	const char *prefix;
	const char *pnp;
} dcp_panel_vendors[] = {
	{ "apple", "APP" },
};

int dcp_panel_id_from_compatible(struct dcp_panel_id *id, const char *compatible)
{
	const char *comma = strchr(compatible, ','), *model;
	size_t vendor_len, len, i;

	memset(id, 0, sizeof(*id));
	if (!comma)
		return -EINVAL;
	vendor_len = comma - compatible;
	model = comma + 1;
	if (str_has_prefix(model, "panel-"))
		model += strlen("panel-");
	else
		return -EINVAL;
	len = strlen(model);
	if (!len || len > DCP_PANEL_NAME_LEN)
		return -EINVAL;
	for (i = 0; i < len; i++) {
		if (!isalnum(model[i]) && model[i] != '-')
			return -EINVAL;
		id->name[i] = toupper(model[i]);
	}

	for (i = 0; i < ARRAY_SIZE(dcp_panel_vendors); i++) {
		if (strlen(dcp_panel_vendors[i].prefix) == vendor_len &&
		    !strncmp(compatible, dcp_panel_vendors[i].prefix, vendor_len)) {
			strscpy(id->vendor, dcp_panel_vendors[i].pnp);
			return 0;
		}
	}
	memset(id, 0, sizeof(*id));
	return -ENOENT;
}

int dcp_panel_edid_block(const struct dcp_panel_id *id, int width_mm, int height_mm,
			 struct edid *block)
{
	struct detailed_non_pixel *name;
	u16 manufacturer = 0;
	size_t len = strnlen(id->name, sizeof(id->name));
	u8 sum = 0;
	unsigned int i;

	if (width_mm <= 0 || height_mm <= 0 ||
	    DIV_ROUND_CLOSEST(width_mm, 10) > 255 || DIV_ROUND_CLOSEST(height_mm, 10) > 255)
		return -EINVAL;
	if (strnlen(id->vendor, sizeof(id->vendor)) != 3 || !len || len > DCP_PANEL_NAME_LEN)
		return -EINVAL;
	for (i = 0; i < 3; i++) {
		if (id->vendor[i] < 'A' || id->vendor[i] > 'Z')
			return -EINVAL;
		manufacturer = manufacturer << 5 | (id->vendor[i] - '@');
	}

	memset(block, 0, sizeof(*block));
	memcpy(block->header, "\x00\xff\xff\xff\xff\xff\xff\x00", sizeof(block->header));
	block->product_id.manufacturer_name = cpu_to_be16(manufacturer);
	block->version = 1;
	block->revision = 4;
	block->input = DRM_EDID_INPUT_DIGITAL;
	block->width_cm = DIV_ROUND_CLOSEST(width_mm, 10);
	block->height_cm = DIV_ROUND_CLOSEST(height_mm, 10);
	/* Standard timings 0x0101 are unused slots. */
	memset(block->standard_timings, 0x01, sizeof(block->standard_timings));

	/* The first descriptor names the panel; the other three are dummies. */
	name = &block->detailed_timings[0].data.other_data;
	name->type = EDID_DETAIL_MONITOR_NAME;
	memset(name->data.str.str, ' ', sizeof(name->data.str.str));
	memcpy(name->data.str.str, id->name, len);
	if (len < sizeof(name->data.str.str))
		name->data.str.str[len] = '\n';
	for (i = 1; i < ARRAY_SIZE(block->detailed_timings); i++)
		block->detailed_timings[i].data.other_data.type = 0x10;

	for (i = 0; i < sizeof(*block) - 1; i++)
		sum += ((const u8 *)block)[i];
	block->checksum = -sum;
	return 0;
}

int dcp_panel_edid_update(struct drm_connector *connector, const struct dcp_panel_id *id,
			  int width_mm, int height_mm)
{
	const struct drm_edid *edid;
	struct edid block;
	int ret;

	ret = dcp_panel_edid_block(id, width_mm, height_mm, &block);
	if (ret)
		return ret;
	edid = drm_edid_alloc(&block, sizeof(block));
	if (!edid)
		return -ENOMEM;
	ret = drm_edid_connector_update(connector, edid);
	drm_edid_free(edid);
	/* The update set the size from the block, in whole centimetres. */
	connector->display_info.width_mm = width_mm;
	connector->display_info.height_mm = height_mm;
	return ret;
}
