/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef APPLE_DCPEXT26_WIRE_H
#define APPLE_DCPEXT26_WIRE_H

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

/* J613 26.6.2 external personality; neither classic 14.7 nor H17P. */
#define DCPEXT26_SUBMIT_BYTES 0x1bd8
#define DCPEXT26_SURFACE_BYTES 0x22c

/* Swap transaction identifier. */
#define DCPEXT26_SWAP_ID 0x98
/* Enabled surface mask and valid bit. */
#define DCPEXT26_SWAP_ENABLED 0x14c
/* Completion surface mask and valid bit. */
#define DCPEXT26_SWAP_COMPLETED 0x150
/* Opaque background ARGB color. */
#define DCPEXT26_BG_COLOR 0x154
/* Ten surface-null flags. */
#define DCPEXT26_SURF_NULL 0x1bcb
/* Unknown output pointer is null. */
#define DCPEXT26_POINTER_NULL 0x1bd6
/* Unknown output value is null. */
#define DCPEXT26_OUTPUT_NULL 0x1bd7
/* Start of the four primary surface records. */
#define DCPEXT26_SURFACES 0x588
/* Array of primary surface IOVAs. */
#define DCPEXT26_SURF_IOVA 0xe38
/* Array of primary surface identifiers. */
#define DCPEXT26_SURF_IDS 0x9c
/* Array of source rectangles. */
#define DCPEXT26_SRC_RECT 0xac
/* Array of destination rectangles. */
#define DCPEXT26_DST_RECT 0x10c
/* Surface width in pixels. */
#define DCPEXT26_SURF_WIDTH 0x21
/* First plane width in pixels. */
#define DCPEXT26_SURF_PLANE_WIDTH 0x59
/* Surface height in pixels. */
#define DCPEXT26_SURF_HEIGHT 0x25
/* First plane height in pixels. */
#define DCPEXT26_SURF_PLANE_HEIGHT 0x5d
/* Surface bytes per row. */
#define DCPEXT26_SURF_STRIDE 0x15
/* First plane bytes per row. */
#define DCPEXT26_SURF_PLANE_STRIDE 0x69
/* Surface backing size in bytes. */
#define DCPEXT26_SURF_BUF_SIZE 0x29
/* First plane size in bytes. */
#define DCPEXT26_SURF_PLANE_SIZE 0x6d
/* Surface plane count. */
#define DCPEXT26_SURF_PLANE_COUNT 3
/* Duplicate surface plane count. */
#define DCPEXT26_SURF_PLANE_COUNT2 7
/* DCP fourcc pixel format. */
#define DCPEXT26_SURF_FORMAT 0xb
/* Surface identifier. */
#define DCPEXT26_SURF_SURFACE_ID 0x35
/* Premultiplied alpha flag. */
#define DCPEXT26_SURF_PREMULTIPLIED 2
/* Transfer function selector. */
#define DCPEXT26_SURF_XFER_FUNC 0x13
/* Color space selector. */
#define DCPEXT26_SURF_COLORSPACE 0x14
/* Pixel size field; preserved native26 value. */
#define DCPEXT26_SURF_PIX_SIZE 0x19
/* Pixel element width. */
#define DCPEXT26_SURF_PEL_W 0x1b
/* Pixel element height. */
#define DCPEXT26_SURF_PEL_H 0x1c
/* First plane tile size in bytes. */
#define DCPEXT26_SURF_TILE_SIZE 0x71
/* First plane tile width. */
#define DCPEXT26_SURF_TILE_W 0x73
/* First plane tile height. */
#define DCPEXT26_SURF_TILE_H 0x74
/* Plane descriptor presence flag. */
#define DCPEXT26_SURF_HAS_PLANES 0x149
/* Component descriptor presence flag. */
#define DCPEXT26_SURF_HAS_COMP 0x51
/* Native26 swap mask including its validity bit. */
#define DCPEXT26_SWAP_MASK 0x80000007
/* Opaque black in ARGB order. */
#define DCPEXT26_BG_BLACK 0xff000000
/* DCP BGRA pixel format fourcc. */
#define DCPEXT26_FORMAT_BGRA 0x42475241
/* Width and height within each packed dcp_rect. */
#define DCPEXT26_RECT_WIDTH 8
#define DCPEXT26_RECT_HEIGHT 12

struct dcpext26_primary {
	u64 address;
	u64 backing_bytes;
	u32 width;
	u32 height;
	u32 pitch;
};

/* Validate all arithmetic before changing a caller-owned command buffer. */
static inline int dcpext26_primary_check(const struct dcpext26_primary *p)
{
	u64 bytes = (u64)p->pitch * p->height;

	if (!p->address || !p->width || !p->height ||
	    (p->pitch & 3) || (u64)p->width * 4 > p->pitch)
		return -EINVAL;
	if (bytes > (u32)~0U || bytes > p->backing_bytes ||
	    p->address > (u64)~0ULL - bytes)
		return -ERANGE;
	return 0;
}

/* Full-frame linear opaque BGRA. All unused layers stay explicitly null. */
static inline int dcpext26_primary_encode(void *buffer, size_t capacity,
					 u32 id, unsigned int layer,
					 const struct dcpext26_primary *p)
{
	u8 *wire = buffer, *surface;
	u32 bytes;
	unsigned int i;
	int ret;

	if (!buffer || capacity != DCPEXT26_SUBMIT_BYTES || !id || layer >= 4)
		return -EINVAL;
	if (p) {
		ret = dcpext26_primary_check(p);
		if (ret)
			return ret;
	}
	memset(wire, 0, capacity);
	put_unaligned_le32(id, wire + DCPEXT26_SWAP_ID);
	put_unaligned_le32(DCPEXT26_SWAP_MASK, wire + DCPEXT26_SWAP_ENABLED);
	put_unaligned_le32(DCPEXT26_SWAP_MASK, wire + DCPEXT26_SWAP_COMPLETED);
	put_unaligned_le32(DCPEXT26_BG_BLACK, wire + DCPEXT26_BG_COLOR);
	/* Ten nullable surfaces, followed by the two output-pointer flags. */
	memset(wire + DCPEXT26_SURF_NULL, 1, 10);
	wire[DCPEXT26_POINTER_NULL] = 1;
	wire[DCPEXT26_OUTPUT_NULL] = 1;
	if (!p)
		return 0;

	surface = wire + DCPEXT26_SURFACES + layer * DCPEXT26_SURFACE_BYTES;
	bytes = p->pitch * p->height;
	{
		const struct { unsigned int offset; u32 value; } words[] = {
			{DCPEXT26_SURF_WIDTH, p->width}, {DCPEXT26_SURF_PLANE_WIDTH, p->width},
			{DCPEXT26_SURF_HEIGHT, p->height}, {DCPEXT26_SURF_PLANE_HEIGHT, p->height},
			{DCPEXT26_SURF_STRIDE, p->pitch}, {DCPEXT26_SURF_PLANE_STRIDE, p->pitch},
			{DCPEXT26_SURF_BUF_SIZE, bytes}, {DCPEXT26_SURF_PLANE_SIZE, bytes},
			{DCPEXT26_SURF_PLANE_COUNT, 1}, {DCPEXT26_SURF_PLANE_COUNT2, 1}, {DCPEXT26_SURF_FORMAT, DCPEXT26_FORMAT_BGRA}, {DCPEXT26_SURF_SURFACE_ID, 1},
		};
		const struct { unsigned int offset; u8 value; } octets[] = {
			{DCPEXT26_SURF_PREMULTIPLIED, 1}, {DCPEXT26_SURF_XFER_FUNC, 13}, {DCPEXT26_SURF_COLORSPACE, 12}, {DCPEXT26_SURF_PIX_SIZE, 1},
			{DCPEXT26_SURF_PEL_W, 1}, {DCPEXT26_SURF_PEL_H, 1}, {DCPEXT26_SURF_TILE_SIZE, 4}, {DCPEXT26_SURF_TILE_W, 1}, {DCPEXT26_SURF_TILE_H, 1},
		};

		for (i = 0; i < sizeof(words) / sizeof(words[0]); i++)
			put_unaligned_le32(words[i].value, surface + words[i].offset);
		for (i = 0; i < sizeof(octets) / sizeof(octets[0]); i++)
			surface[octets[i].offset] = octets[i].value;
		put_unaligned_le64(1, surface + DCPEXT26_SURF_HAS_PLANES);
		put_unaligned_le64(1, surface + DCPEXT26_SURF_HAS_COMP);
	}
	put_unaligned_le64(p->address, wire + DCPEXT26_SURF_IOVA + layer * 8);
	put_unaligned_le32(1, wire + DCPEXT26_SURF_IDS + layer * 4);
	for (i = 0; i < 2; i++) {
		u8 *rect = wire + (i ? DCPEXT26_DST_RECT : DCPEXT26_SRC_RECT) + layer * 16;

		put_unaligned_le32(p->width, rect + DCPEXT26_RECT_WIDTH);
		put_unaligned_le32(p->height, rect + DCPEXT26_RECT_HEIGHT);
	}
	wire[DCPEXT26_SURF_NULL + layer] = 0;
	return 0;
}

static inline int dcpext26_start_reply(const void *reply, size_t length, u32 *id)
{
	const u8 *wire = reply;
	u32 value;

	if (!reply || !id || length != 8)
		return -EINVAL;
	value = get_unaligned_le32(wire);
	if (!value || get_unaligned_le32(wire + 4))
		return -EPROTO;
	*id = value;
	return 0;
}

static inline int dcpext26_submit_reply(const void *reply, size_t length)
{
	const u8 *wire = reply;

	if (!reply || length != 12)
		return -EINVAL;
	return get_unaligned_le32(wire + 5) ? -EPROTO : 0;
}

#endif
