/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCP_M4_TIMING_H
#define APPLE_DCP_M4_TIMING_H

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include "dcp-m4-profile.h"

#define DCP_M4_TIMING_TOKENS 2048

/* The profile's qualified physical mode, not a generic panel-mode fallback. */
struct dcp_m4_timing {
	u32 id, color_id;
	u32 hdisplay, hsync_start, hsync_end, htotal;
	u32 vdisplay, vsync_start, vsync_end, vtotal;
	u32 clock_khz, refresh_16_16;
};

struct dcp_m4_timing_token {
	u64 value;
	u32 offset, count;
	u16 next;
	u8 type;
};

/* Heap storage, so a firmware dictionary cannot consume the kernel stack. */
struct dcp_m4_timing_parser {
	const u8 *data;
	u32 size, pos, used;
	struct dcp_m4_timing_token token[DCP_M4_TIMING_TOKENS];
};

static inline int dcp_m4_timing_object(struct dcp_m4_timing_parser *p, unsigned int depth)
{
	struct dcp_m4_timing_token *t;
	u32 tag, bytes = 0, children = 0;
	unsigned int index;

	if (depth > 8 || p->used == DCP_M4_TIMING_TOKENS)
		return -E2BIG;
	p->pos = (p->pos + 3) & ~3U;
	if (p->pos > p->size || p->size - p->pos < 4)
		return -EINVAL;
	tag = get_unaligned_le32(p->data + p->pos);
	p->pos += 4;
	if (tag & 0x60000000)
		return -EINVAL;
	index = p->used++;
	t = &p->token[index];
	t->type = (tag >> 24) & 31;
	t->count = tag & 0xffffff;
	t->offset = p->pos;
	switch (t->type) {
	case 1:
		if (t->count > 128)
			return -E2BIG;
		children = t->count * 2;
		break;
	case 2:
		if (t->count > 128)
			return -E2BIG;
		children = t->count;
		break;
	case 4:
		if (t->count != 64)
			return -EINVAL;
		bytes = 8;
		break;
	case 9:
	case 10:
		if (t->count > (t->type == 9 ? 256 : 65536))
			return -E2BIG;
		bytes = t->count;
		break;
	case 11:
		if (t->count > 1)
			return -EINVAL;
		t->value = t->count;
		break;
	default:
		return -EINVAL;
	}
	if (bytes > p->size - p->pos)
		return -EINVAL;
	if (t->type == 4)
		t->value = get_unaligned_le64(p->data + p->pos);
	p->pos += bytes;
	/* Counts bound containers; bit 31 is not used to extend or shorten them. */
	for (unsigned int i = 0; i < children; i++) {
		unsigned int key = p->used;
		int ret = dcp_m4_timing_object(p, depth + 1);

		if (ret)
			return ret;
		if (t->type == 1 && !(i & 1)) {
			const struct dcp_m4_timing_token *k = &p->token[key];

			if (k->type != 9 || !k->count ||
			    memchr(p->data + k->offset, 0, k->count))
				return -EINVAL;
			/* Reject duplicate keys, including recognized optional fields. */
			for (unsigned int previous = index + 1; previous < key;
			     previous = p->token[p->token[previous].next].next) {
				const struct dcp_m4_timing_token *old = &p->token[previous];

				if (old->count == k->count &&
				    !memcmp(p->data + old->offset, p->data + k->offset, k->count))
					return -EINVAL;
			}
		}
	}
	t->next = p->used;
	return 0;
}

static inline int dcp_m4_timing_field(const struct dcp_m4_timing_parser *p,
				     unsigned int dict, const char *name, unsigned int type)
{
	if (dict >= p->used || p->token[dict].type != 1)
		return -EINVAL;
	for (unsigned int key = dict + 1; key < p->token[dict].next;
	     key = p->token[p->token[key].next].next) {
		const struct dcp_m4_timing_token *k = &p->token[key];

		if (k->count == strlen(name) && !memcmp(p->data + k->offset, name, k->count))
			return p->token[k->next].type == type ? k->next : -EINVAL;
	}
	return -ENOENT;
}

static inline bool dcp_m4_timing_equals(const struct dcp_m4_timing_parser *p,
				       unsigned int dict, const char *name,
				       unsigned int type, u64 value)
{
	int index = dcp_m4_timing_field(p, dict, name, type);

	return index >= 0 && p->token[index].value == value;
}

static inline int dcp_m4_timing_axis(const struct dcp_m4_timing_parser *p,
				    unsigned int mode, const char *name, bool vertical)
{
	static const char * const fields[] = {
		"Active", "Total", "FrontPorch", "SyncWidth", "BackPorch", "PreciseSyncRate",
		"SyncPolarity", "PixelRepetition",
	};
	const u32 (*values)[8] = dcp_m4_pf->timing;
	int axis = dcp_m4_timing_field(p, mode, name, 1);

	if (!values)
		return -ENODEV;
	if (axis < 0)
		return axis;
	for (unsigned int i = 0; i < 8; i++)
		if (!dcp_m4_timing_equals(p, axis, fields[i], 4, values[vertical][i]))
			return -EINVAL;
	/* Exact qualified values also establish active + porches + sync == total. */
	return 0;
}

static inline int dcp_m4_parse_timing(const void *data, size_t size, struct dcp_m4_timing *out)
{
	struct dcp_m4_timing_parser *p;
	static const char * const color_fields[] = {
		"ID", "Depth", "PixelEncoding", "DynamicRange", "EOTF", "Colorimetry", "SupportsDSC",
	};
	static const u32 color_values[] = { 1, 8, 0, 0, 0, 16, 0 };
	int ret = -EINVAL, colors, color;
	unsigned int mode = 0, element = 1;
	const u32 *h, *v;

	if (!dcp_m4_pf->timing)
		return -ENODEV;
	if (!data || !out || size < 4 || size > (1U << 20) || get_unaligned_le32(data) != 0xd3)
		return -EINVAL;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->data = data;
	p->size = size;
	p->pos = 4;
	ret = dcp_m4_timing_object(p, 0);
	if (ret)
		goto done;
	ret = -EINVAL;
	if (p->pos != size || p->token[0].type != 2 || !p->token[0].count ||
	    p->token[0].count > dcp_m4_pf->timing_elements)
		goto done;
	/* Exactly one element is ID 2, and no other is preferred. */
	for (unsigned int i = 0; i < p->token[0].count;
	     i++, element = p->token[element].next) {
		if (dcp_m4_timing_field(p, element, "ID", 4) < 0)
			goto done;
		if (dcp_m4_timing_equals(p, element, "ID", 4, 2)) {
			if (mode)
				goto done;
			mode = element;
		} else if (!dcp_m4_timing_equals(p, element, "IsPreferred", 11, 0)) {
			goto done;
		}
	}
	if (!mode ||
	    !dcp_m4_timing_equals(p, mode, "IsVirtual", 11, 0) ||
	    !dcp_m4_timing_equals(p, mode, "IsPreferred", 11, 1) ||
	    !dcp_m4_timing_equals(p, mode, "IsInterlaced", 11, 0) ||
	    !dcp_m4_timing_equals(p, mode, "IsSplit", 11, 0) ||
	    !dcp_m4_timing_equals(p, mode, "IsOverscanned", 11, 0) ||
	    !dcp_m4_timing_equals(p, mode, "TimingType", 4, 3) ||
	    !dcp_m4_timing_equals(p, mode, "MinimumVariableRefreshRate", 4, 0) ||
	    !dcp_m4_timing_equals(p, mode, "MaximumVariableRefreshRate", 4, 0) ||
	    dcp_m4_timing_axis(p, mode, "HorizontalAttributes", false) ||
	    dcp_m4_timing_axis(p, mode, "VerticalAttributes", true))
		goto done;
	colors = dcp_m4_timing_field(p, mode, "ColorModes", 2);
	if (colors < 0 || p->token[colors].count != 1)
		goto done;
	color = colors + 1;
	if (!dcp_m4_timing_equals(p, color, "IsVirtual", 11, 0))
		goto done;
	for (unsigned int i = 0; i < 7; i++)
		if (!dcp_m4_timing_equals(p, color, color_fields[i], 4, color_values[i]))
			goto done;
	h = dcp_m4_pf->timing[0];
	v = dcp_m4_pf->timing[1];
	*out = (struct dcp_m4_timing) {
		.id = 2, .color_id = 1,
		.hdisplay = h[0], .hsync_start = h[0] + h[2],
		.hsync_end = h[0] + h[2] + h[3], .htotal = h[1],
		.vdisplay = v[0], .vsync_start = v[0] + v[2],
		.vsync_end = v[0] + v[2] + v[3], .vtotal = v[1],
		.clock_khz = (((u64)h[1] * v[1] * v[5] >> 16) + 500) / 1000,
		.refresh_16_16 = v[5],
	};
	ret = 0;
done:
	kfree(p);
	return ret;
}

#endif
