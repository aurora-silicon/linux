/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Swap (A407) and surface records of the J613/25G83 DCP firmware (macOS 26.6.2 (25G83))
 *
 * Copyright The Asahi Linux Contributors
 * Based on the M3 Pro DCP lab client by Eryk Wieliczko.
 */

#ifndef __APPLE_IOMFB_V26_6_SWAP_H__
#define __APPLE_IOMFB_V26_6_SWAP_H__

#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define DCP_V26_SWAP_SIZE		0x1bd8
#define DCP_V26_SURFACE_SIZE		0x22c

/* Offsets in the swap record. Rectangles are x, y, w, h (u32 each). */
#define DCP_V26_SWAP_TS1		0x00	/* u64 presentation timing */
#define DCP_V26_SWAP_TS2		0x08
#define DCP_V26_SWAP_TS3		0x30
#define DCP_V26_SWAP_ID			0x98
#define DCP_V26_SWAP_SURF_ID(p)		(0x9c + (p) * 4)
#define DCP_V26_SWAP_SRC(p)		(0xac + (p) * 16)	/* in the surface */
#define DCP_V26_SWAP_DST(p)		(0x10c + (p) * 16)	/* on the panel */
#define DCP_V26_SWAP_ENABLED		0x14c
#define DCP_V26_SWAP_COMPLETED		0x150
#define DCP_V26_SWAP_BACKGROUND		0x154
/* 25G83: shuffle at TEXT1c20ac preserves update at wire354; the nits
 * double at wire35e is copied to the internal record at7a8, consumed by
 * TEXT22c7b4..22c7ec as brightnessLevel (0x13), in 16.16 nits. */
#define DCP_V26_SWAP_BRIGHTNESS_UPDATE 0x354
#define DCP_V26_SWAP_BRIGHTNESS_NITS 0x35e
#define DCP_V26_SWAP_SURFACE(p)		(0x588 + (p) * DCP_V26_SURFACE_SIZE)
#define DCP_V26_SWAP_IOVA(p)		(0xe38 + (p) * 8)
#define DCP_V26_SWAP_SURF_NULL(p)	(0x1bcb + (p))
#define DCP_V26_SWAP_SURF_NULL_COUNT	10

#define DCP_V26_SWAP_LAYERS		0x7
#define DCP_V26_SWAP_SET_BACKGROUND	BIT(31)

/* Offsets in a surface record. */
#define DCP_V26_SURF_OPAQUE		0x02
#define DCP_V26_SURF_FORMAT		0x0b
#define DCP_V26_SURF_STRIDE		0x15
#define DCP_V26_SURF_WIDTH		0x21
#define DCP_V26_SURF_HEIGHT		0x25
#define DCP_V26_SURF_SIZE		0x29
#define DCP_V26_SURF_PLANE_WIDTH	0x59
#define DCP_V26_SURF_PLANE_HEIGHT	0x5d
#define DCP_V26_SURF_PLANE_STRIDE	0x69
#define DCP_V26_SURF_PLANE_SIZE		0x6d

/* 'BGRA': 8-bit linear, ARGB8888 in DRM terms. */
#define DCP_V26_FORMAT_BGRA		0x42475241

/*
 * A single-plane linear BGRA surface covering the whole framebuffer. The
 * firmware has no XRGB format: an opaque surface blends as premultiplied
 * alpha over the black background, which shows XRGB pixels unchanged.
 */
static inline void dcp_v26_encode_surface(u8 *s, u32 stride, u32 width, u32 height,
					  bool opaque)
{
	u32 bytes = stride * height;

	memset(s, 0, DCP_V26_SURFACE_SIZE);
	s[DCP_V26_SURF_OPAQUE] = opaque;
	put_unaligned_le32(1, s + 0x03);
	put_unaligned_le32(1, s + 0x07);
	put_unaligned_le32(DCP_V26_FORMAT_BGRA, s + DCP_V26_SURF_FORMAT);
	s[0x13] = 13;
	s[0x14] = 12;
	put_unaligned_le32(stride, s + DCP_V26_SURF_STRIDE);
	put_unaligned_le16(1, s + 0x19);
	s[0x1b] = 1;
	s[0x1c] = 1;
	put_unaligned_le32(width, s + DCP_V26_SURF_WIDTH);
	put_unaligned_le32(height, s + DCP_V26_SURF_HEIGHT);
	put_unaligned_le32(bytes, s + DCP_V26_SURF_SIZE);
	put_unaligned_le32(1, s + 0x35);
	put_unaligned_le64(1, s + 0x51);
	put_unaligned_le32(width, s + DCP_V26_SURF_PLANE_WIDTH);
	put_unaligned_le32(height, s + DCP_V26_SURF_PLANE_HEIGHT);
	put_unaligned_le32(stride, s + DCP_V26_SURF_PLANE_STRIDE);
	put_unaligned_le32(bytes, s + DCP_V26_SURF_PLANE_SIZE);
	put_unaligned_le16(4, s + 0x71);
	s[0x73] = 1;
	s[0x74] = 1;
	put_unaligned_le64(1, s + 0x149);
}

static inline void dcp_v26_put_rect(u8 *r, u32 x, u32 y, u32 w, u32 h)
{
	put_unaligned_le32(x, r);
	put_unaligned_le32(y, r + 4);
	put_unaligned_le32(w, r + 8);
	put_unaligned_le32(h, r + 12);
}

/*
 * The swap record for swap @id. @background fills every panel pixel that no
 * surface covers: the whole panel when @surface is NULL. With a surface on
 * @layer, the whole @width x @height surface is read (source 0,0) and shown
 * unscaled at 0,@dst_y on the panel: 0 for the full panel, the notch height
 * to keep the rows above it black.
 */
static inline void dcp_v26_encode_swap(u8 *swap, u32 id, u32 background, const u8 *surface,
				       u64 iova, u32 width, u32 height, u32 dst_y, u32 layer)
{
	memset(swap, 0, DCP_V26_SWAP_SIZE);
	put_unaligned_le32(id, swap + DCP_V26_SWAP_ID);
	put_unaligned_le32(DCP_V26_SWAP_SET_BACKGROUND | DCP_V26_SWAP_LAYERS,
			   swap + DCP_V26_SWAP_ENABLED);
	put_unaligned_le32(DCP_V26_SWAP_SET_BACKGROUND | DCP_V26_SWAP_LAYERS,
			   swap + DCP_V26_SWAP_COMPLETED);
	put_unaligned_le32(background, swap + DCP_V26_SWAP_BACKGROUND);
	memset(swap + DCP_V26_SWAP_SURF_NULL(0), 1, DCP_V26_SWAP_SURF_NULL_COUNT);
	swap[0x1bd6] = 1;
	swap[0x1bd7] = 1;
	if (surface) {
		memcpy(swap + DCP_V26_SWAP_SURFACE(layer), surface, DCP_V26_SURFACE_SIZE);
		put_unaligned_le64(iova, swap + DCP_V26_SWAP_IOVA(layer));
		put_unaligned_le32(1, swap + DCP_V26_SWAP_SURF_ID(layer));
		dcp_v26_put_rect(swap + DCP_V26_SWAP_SRC(layer), 0, 0, width, height);
		dcp_v26_put_rect(swap + DCP_V26_SWAP_DST(layer), 0, dst_y, width, height);
		swap[DCP_V26_SWAP_SURF_NULL(layer)] = 0;
	}
}

/*
 * The swap record opens with the same block of presentation timing words
 * as the 12.x/13.x firmware. The first is when the swap should be shown,
 * in the firmware's clock, or zero for no target; a flag byte at 0x338
 * makes it relative to the swap's arrival instead. The firmware shows a
 * swap whose target has passed at the next refresh (it may drop it for a
 * newer swap queued behind it), and holds one whose target is more than
 * about a frame ahead. It carries the other words along for its traces.
 * None of them sets a refresh rate.
 *
 * With all of them zero, back-to-back swaps on the 120 Hz variable-refresh
 * (ProMotion) panel complete about 16.7 ms apart, i.e. at 60 Hz. The M1/M2
 * hosts put a non-zero value in them on such panels, which makes their
 * firmware present at 120 Hz: a target long past, so "as soon as possible".
 */
static inline void dcp_v26_swap_set_timing(u8 *swap, u64 value)
{
	put_unaligned_le64(value, swap + DCP_V26_SWAP_TS1);
	put_unaligned_le64(value, swap + DCP_V26_SWAP_TS2);
	put_unaligned_le64(value, swap + DCP_V26_SWAP_TS3);
}

/* @nits as an IEEE 754 double, exact for every u32, without kernel FP. */
static inline u64 dcp_v26_nits_to_double(u32 nits)
{
	unsigned int exponent;

	if (!nits)
		return 0;
	exponent = fls(nits) - 1;
	return ((u64)(1023 + exponent) << 52) |
	       (((u64)nits << (52 - exponent)) & GENMASK_ULL(51, 0));
}

/* Asks for @nits in a swap record built by dcp_v26_encode_swap(). */
static inline void dcp_v26_encode_brightness(u8 *swap, u32 nits)
{
	swap[DCP_V26_SWAP_BRIGHTNESS_UPDATE] = 1;
	put_unaligned_le64(dcp_v26_nits_to_double(nits), swap + DCP_V26_SWAP_BRIGHTNESS_NITS);
}

#endif /* __APPLE_IOMFB_V26_6_SWAP_H__ */
