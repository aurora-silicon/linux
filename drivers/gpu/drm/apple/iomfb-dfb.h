/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 Aurora Silicon */

#ifndef __APPLE_IOMFB_DFB_H__
#define __APPLE_IOMFB_DFB_H__

#include <linux/align.h>
#include <linux/errno.h>
#include <linux/log2.h>
#include <linux/kernel.h>
#include <linux/types.h>

static inline u64 apple_dcp_dfb_round_size(u64 size)
{
	unsigned int bits = fls64(size);
	u64 unit, half, tail;

	if (bits <= 24)
		return size;
	unit = 1ULL << (bits - 24);
	half = unit >> 1;
	tail = size & (unit - 1);
	size &= ~(unit - 1);
	if (tail > half || (tail == half && (size & unit)))
		size += unit;
	return size;
}

static inline u64 apple_dcp_dfb_plane_size(u32 width, u32 height, u32 bpe,
					 u32 compression)
{
	u64 stride, data, cols, rows, tw = 16, th = 16;
	unsigned int order;

	if (!compression) {
		stride = ALIGN((u64)width * bpe, 128);
		return ALIGN(apple_dcp_dfb_round_size(stride * height), 0x4000);
	}
	if (compression == 2 && bpe > 4) {
		th = 8;
		if (bpe == 16)
			tw = 8;
	}
	cols = DIV_ROUND_UP(width, tw);
	rows = DIV_ROUND_UP(height, th);
	if (compression == 3) {
		stride = tw * th * cols * bpe;
		data = ALIGN(rows * stride, 128);
		return data + 8 * roundup_pow_of_two(cols) * roundup_pow_of_two(rows);
	}
	order = ilog2(0x4000 / bpe);
	stride = ALIGN((u64)width * bpe, (u64)bpe << (order / 2));
	data = ALIGN(ALIGN((u64)height, 1ULL << ((order + 1) / 2)) * stride, 128);
	return data + ALIGN(2 * roundup_pow_of_two(cols) * roundup_pow_of_two(rows), 128);
}

static inline int apple_dcp_dfb_allocation(u32 width, u32 height, u32 format,
					 u32 compression, u64 region_size,
					 u32 *surface_format, u32 *allocation)
{
	u32 bpe;
	u64 size;
	bool alpha_plane = false;

	if (!width || !height || width > 16384 || height > 16384 || !region_size)
		return -EINVAL;
	if (compression != 0 && compression != 2 && compression != 3)
		return -EOPNOTSUPP;
	switch (format) {
	case 0x62336138:
		if (compression == 2) {
			format = 0x77343061;
			bpe = 8;
		} else {
			bpe = 4;
			alpha_plane = true;
		}
		break;
	case 0x77343061:
		bpe = 8;
		break;
	case 0x42475241:
	case 0x77333072:
	case 0x6c313072:
		bpe = 4;
		break;
	default:
		return -EOPNOTSUPP;
	}
	size = apple_dcp_dfb_plane_size(width, height, bpe, compression);
	if (alpha_plane)
		size += apple_dcp_dfb_plane_size(width, height, 1, compression);
	if (size > U32_MAX)
		return -EOVERFLOW;
	if (compression)
		size = ALIGN(apple_dcp_dfb_round_size(size), 0x4000);
	if (size > region_size)
		return -ENOSPC;
	if (!size || size > U32_MAX)
		return -EOVERFLOW;
	*surface_format = format;
	*allocation = size;
	return 0;
}
#endif
