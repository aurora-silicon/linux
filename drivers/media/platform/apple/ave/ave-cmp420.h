/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple lossless compressed 8-bit 4:2:0 surfaces ("&8v0"), the picture form
 * the AVE MCTF client reads and writes. CPU helpers only: a raw-tile writer
 * and a raw/solid reader. Everything else a surface can hold is counted and
 * shown as a marker; it is not decoded.
 *
 * Copyright The Aurora Silicon Contributors
 */
#ifndef _APPLE_AVE_CMP420_H_
#define _APPLE_AVE_CMP420_H_

#include <linux/types.h>

/* Storage tiles: 32x32 luma samples, 16x16 CbCr pairs */
#define AVE_CMP420_Y_TILE	32
#define AVE_CMP420_C_TILE	16
#define AVE_CMP420_Y_BODY	1024
#define AVE_CMP420_C_BODY	512
#define AVE_CMP420_Y_META	32	/* four compression tiles of eight bytes */
#define AVE_CMP420_C_META	8	/* one compression tile */
/* Metadata rows and columns are padded to a multiple of this many tiles. */
#define AVE_CMP420_META_ALIGN	64

/* One metadata byte per 32-sample block */
#define AVE_CMP420_Y_RAW	0x1f
#define AVE_CMP420_Y_SOLID	0x60
#define AVE_CMP420_C_RAW	0x3f
#define AVE_CMP420_C_SOLID	0x01

/* Video-range black, written to the padding beyond the picture */
#define AVE_CMP420_Y_BLACK	0x10
#define AVE_CMP420_C_BLACK	0x80

/*
 * What the reader writes for a block it cannot decode. Out of video range,
 * so never a sample of a valid 8v0 picture.
 */
#define AVE_CMP420_Y_MARK	0xff
#define AVE_CMP420_CB_MARK	0x00
#define AVE_CMP420_CR_MARK	0xff

/* Byte offsets inside one surface, and the visible picture */
struct ave_cmp420_geom {
	u32 width, height;
	u32 tiles_x, tiles_y;		/* storage tiles, the same for Y and CbCr */
	u32 y_pitch;			/* Y bodies at 0 */
	u32 y_meta, y_meta_pitch;
	u32 c, c_pitch;
	u32 c_meta, c_meta_pitch;
	u32 bytes;
};

/* Reader counts for one plane: 32-sample blocks, then storage tiles */
struct ave_cmp420_stats {
	u32 raw, solid, other;
	u32 tiles_raw, tiles_solid, tiles_mixed, tiles_other;
};

int ave_cmp420_geom(struct ave_cmp420_geom *g, u32 width, u32 height);
void ave_cmp420_pack(const struct ave_cmp420_geom *g, u8 *surf,
		     const u8 *y, const u8 *cbcr, u32 stride);
void ave_cmp420_unpack(const struct ave_cmp420_geom *g, const u8 *surf,
		       u8 *y, u8 *cbcr, u32 stride,
		       struct ave_cmp420_stats *ys, struct ave_cmp420_stats *cs);
void ave_cmp420_meta_hist(const struct ave_cmp420_geom *g, const u8 *surf,
			  bool chroma, u16 hist[256]);

#endif
