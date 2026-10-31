/* SPDX-License-Identifier: GPL-2.0-only */
/* Based on the public AVD layout by Sofus Forstreuter. */
#ifndef AVD_LAYOUT_H_
#define AVD_LAYOUT_H_

#include <linux/align.h>
#include <linux/log2.h>
#include <linux/types.h>

struct avd_comp {
	u32 size;
	/* Offset to compressed storage in the capture allocation. */
	size_t start_offset;
	/* Y metadata, Y data, UV metadata, UV data, relative to start_offset. */
	u32 offsets[4];
};

/* Dimensions are nonzero and constrained to at most 16384 by the codec. */
static inline void avd_interchange_plane(u32 width, u32 height, u32 bits,
					u32 tile, u32 meta_bytes,
					u32 *data, u32 *meta)
{
	u32 cols = (width + tile - 1) / tile;
	u32 rows = (height + tile - 1) / tile;

	/* Ten-bit storage is packed, not rounded to sixteen bits per sample. */
	*data = ALIGN(cols * rows * (tile * tile * bits / 8), 128);
	*meta = ALIGN(roundup_pow_of_two(cols) * roundup_pow_of_two(rows) *
		      meta_bytes, 128);
}

static inline void avd_interchange_layout(struct avd_comp *comp,
					 u32 width, u32 height, u32 depth,
					 u32 hdiv, u32 vdiv)
{
	u32 y, y_meta, uv, uv_meta;

	avd_interchange_plane(width, height, depth, 32, 32, &y, &y_meta);
	avd_interchange_plane((width + hdiv - 1) / hdiv,
			      (height + vdiv - 1) / vdiv, 2 * depth,
			      16, 8, &uv, &uv_meta);

	/* Each component's metadata follows its image data. */
	comp->offsets[0] = y;
	comp->offsets[1] = 0;
	comp->offsets[2] = y + y_meta + uv;
	comp->offsets[3] = y + y_meta;
	comp->size = y + y_meta + uv + uv_meta;
}

#endif /* AVD_LAYOUT_H_ */
