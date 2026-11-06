// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Read-only Apple device tree decoding. The wire layout is documented by
 * m1n1's public src/adt.h (GPL-2.0-or-later OR BSD-2-Clause). No firmware
 * implementation or captured device identifiers are used here.
 */
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "dcp-panel-adt.h"

struct dcp_adt_cursor {
	const u8 *data;
	size_t pos, size;
	u32 nodes;
};

static const u8 *dcp_adt_bytes(struct dcp_adt_cursor *c, size_t size)
{
	const u8 *p;

	if (c->pos > c->size || size > c->size - c->pos)
		return ERR_PTR(-EINVAL);
	p = c->data + c->pos;
	c->pos += size;
	return p;
}

/* Hash panel serial bytes into EDID's numeric serial; never log the input. */
static u32 dcp_adt_serial(const u8 *data, size_t size)
{
	u32 serial = 2166136261U;

	for (size_t i = 0; i < size; i++)
		serial = (serial ^ data[i]) * 16777619U;
	return serial ?: 1;
}

static void dcp_adt_panel_property(struct dcp_panel_adt *panel, const char *name,
				   const u8 *value, u32 size)
{
	struct dcp_display_metadata *id = &panel->identity;
	u32 number = size == 4 ? get_unaligned_le32(value) : 0;

	if ((!strcmp(name, "vendor-id") || !strcmp(name, "display-vendor-id")) &&
	    size == 4 && number <= 0x7fff) {
		char manufacturer[4] = { (number >> 10 & 31) + '@',
			(number >> 5 & 31) + '@', (number & 31) + '@', 0 };

		if (manufacturer[0] >= 'A' && manufacturer[0] <= 'Z' &&
		    manufacturer[1] >= 'A' && manufacturer[1] <= 'Z' &&
		    manufacturer[2] >= 'A' && manufacturer[2] <= 'Z')
			memcpy(id->manufacturer, manufacturer, sizeof(manufacturer));
	} else if ((!strcmp(name, "product-id") || !strcmp(name, "display-product-id") ||
		    !strcmp(name, "panel-id")) && size == 4) {
		id->product = number & U16_MAX;
	} else if ((!strcmp(name, "serial-number") ||
		    !strcmp(name, "raw-panel-serial-number")) && size && size <= 256) {
		id->serial = dcp_adt_serial(value, size);
	} else if (!strcmp(name, "display-serial-number") && size == 4) {
		id->serial = number;
	} else if (!strcmp(name, "width-mm") && size == 4 && number && number <= 2550) {
		panel->width_mm = number;
	} else if (!strcmp(name, "height-mm") && size == 4 && number && number <= 2550) {
		panel->height_mm = number;
	}
}

static int dcp_adt_node(struct dcp_adt_cursor *c, struct dcp_panel_adt *panel,
			u32 depth, bool product_parent)
{
	const u8 *header = dcp_adt_bytes(c, 8);
	size_t props_start, props_end;
	u32 props, children;
	char node_name[33] = {};
	bool product, selected, backlight;

	if (IS_ERR(header))
		return PTR_ERR(header);
	if (depth > 32 || ++c->nodes > 4096)
		return -E2BIG;
	props = get_unaligned_le32(header);
	children = get_unaligned_le32(header + 4);
	if (props > 4096 || children > 4096)
		return -E2BIG;
	props_start = c->pos;
	/* Names need not be the first property. Validate every extent first. */
	for (u32 i = 0; i < props; i++) {
		const u8 *prop = dcp_adt_bytes(c, 36), *value;
		u32 size;

		if (IS_ERR(prop) || !memchr(prop, 0, 32))
			return -EINVAL;
		size = get_unaligned_le32(prop + 32) & 0x7fffffff;
		value = dcp_adt_bytes(c, ALIGN((size_t)size, 4));
		if (IS_ERR(value))
			return PTR_ERR(value);
		if (!strcmp((const char *)prop, "name") && size && size <= 32 &&
		    memchr(value, 0, size))
			memcpy(node_name, value, size);
	}
	props_end = c->pos;
	product = depth == 1 && !strcmp(node_name, "product");
	selected = (depth == 1 && !strcmp(node_name, "panel")) ||
		   (product_parent && !strcmp(node_name, "panel"));
	backlight = depth == 1 && !strcmp(node_name, "backlight");
	/* /product carries raw-panel-serial-number on machines without /panel. */
	c->pos = props_start;
	for (u32 i = 0; i < props; i++) {
		const u8 *prop = c->data + c->pos;
		u32 size = get_unaligned_le32(prop + 32) & 0x7fffffff;
		const char *name = (const char *)prop;

		if (selected || (product && !strcmp(name, "raw-panel-serial-number")))
			dcp_adt_panel_property(panel, name, prop + 36, size);
		else if (backlight && !strcmp(name, "EDRPotentialHeadroom") && size == 4)
			panel->edr_potential_headroom = get_unaligned_le32(prop + 36);
		c->pos += 36 + ALIGN((size_t)size, 4);
	}
	/* A valid ADT may have neither panel node; retain the OF defaults. */
	panel->found |= selected;
	c->pos = props_end;
	for (u32 i = 0; i < children; i++) {
		int ret = dcp_adt_node(c, panel, depth + 1, product);

		if (ret)
			return ret;
	}
	return 0;
}

int dcp_panel_adt_parse(const void *data, size_t size, struct dcp_panel_adt *panel)
{
	struct dcp_adt_cursor cursor = { .data = data, .size = size };
	struct dcp_panel_adt parsed = *panel;
	int ret;

	if (!data || size > SZ_16M)
		return -EINVAL;
	ret = dcp_adt_node(&cursor, &parsed, 0, false);
	if (!ret)
		*panel = parsed;
	return ret;
}
