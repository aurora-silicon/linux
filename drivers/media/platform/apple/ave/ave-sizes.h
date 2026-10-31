/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _APPLE_AVE_SIZES_H_
#define _APPLE_AVE_SIZES_H_

#include <linux/align.h>
#include <linux/math.h>
#include <linux/types.h>

/* Start +0x7d0 LFSRef: exact descriptor bytes, before DMA page rounding. */
static inline u32 ave_lfs_ref_size(u32 width, u32 height, bool hevc, u32 scale)
{
	u32 size;

	if (hevc)
		size = 256 * DIV_ROUND_UP(width, 64) * DIV_ROUND_UP(height, 64);
	else
		size = ALIGN(width, 64) * (ALIGN(height, 16) / 4 + 12);
	return size * scale;
}

#endif
