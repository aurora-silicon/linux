// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple lossless compressed 8-bit 4:2:0 surfaces (see ave-cmp420.h).
 *
 * A surface is four regions: Y bodies, Y metadata, CbCr bodies, CbCr
 * metadata. Storage tiles are row-major: 32x32 luma samples in a 1024-byte
 * body with 32 metadata bytes, 16x16 CbCr pairs in a 512-byte body with 8
 * metadata bytes. Metadata rows and columns are padded to 64 tiles. Inside a
 * tile, samples (CbCr: Cb,Cr pairs) are in Y-first Morton order: y bits at
 * index bits 0, 2, 4..., x bits at 1, 3, 5...
 *
 * Compression works on 16x16 tiles: a luma storage tile is four of them,
 * top-left, bottom-left, top-right, bottom-right (Morton order again), each
 * in its own 256-byte quarter of the body with its own eight metadata bytes;
 * a CbCr storage tile is one. Each metadata byte describes 32 consecutive
 * Morton-order samples; a block's payload starts where the previous one
 * ends. Raw blocks (Y 0x1f, CbCr 0x3f) carry the samples as they are,
 * solid blocks (Y 0x60, CbCr 0x01) one sample (pair) for all 32.
 *
 * The writer marks every block raw, so its output is a lossless surface
 * with no compression. The reader decodes raw and solid blocks exactly. Any
 * other block is counted and filled with a marker. In luma the ordinary
 * lossless modes (all but mode 7, upper three bits) carry their size in the
 * low five bits, so the reader keeps its place after them; after any other
 * unknown block, and after any unknown CbCr block, the rest of that
 * compression tile is unknown too.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include "ave-cmp420.h"

#define AVE_CMP420_CTILE	16	/* compression tile, samples */
#define AVE_CMP420_BLOCK	32	/* samples per metadata byte */

#define SEEN_RAW	BIT(0)
#define SEEN_SOLID	BIT(1)
#define SEEN_OTHER	BIT(2)

/* Spread the low 8 bits of @v to the even bits */
static inline u32 ave_cmp420_spread(u32 v)
{
	v &= 0xff;
	v = (v | (v << 4)) & 0x0f0f;
	v = (v | (v << 2)) & 0x3333;
	v = (v | (v << 1)) & 0x5555;
	return v;
}

int ave_cmp420_geom(struct ave_cmp420_geom *g, u32 width, u32 height)
{
	u32 cols, rows;

	if (!width || !height || width % 2 || height % 2 ||
	    width > 8192 || height > 8192)
		return -EINVAL;
	g->width = width;
	g->height = height;
	g->tiles_x = DIV_ROUND_UP(width, AVE_CMP420_Y_TILE);
	g->tiles_y = DIV_ROUND_UP(height, AVE_CMP420_Y_TILE);
	cols = ALIGN(g->tiles_x, AVE_CMP420_META_ALIGN);
	rows = ALIGN(g->tiles_y, AVE_CMP420_META_ALIGN);
	g->y_pitch = g->tiles_x * AVE_CMP420_Y_BODY;
	g->y_meta = g->tiles_y * g->y_pitch;
	g->y_meta_pitch = cols * AVE_CMP420_Y_META;
	g->c = g->y_meta + rows * g->y_meta_pitch;
	g->c_pitch = g->tiles_x * AVE_CMP420_C_BODY;
	g->c_meta = g->c + g->tiles_y * g->c_pitch;
	g->c_meta_pitch = cols * AVE_CMP420_C_META;
	g->bytes = g->c_meta + rows * g->c_meta_pitch;
	return 0;
}

static void ave_cmp420_pack_y(const struct ave_cmp420_geom *g, u8 *surf,
			      const u8 *src, u32 stride)
{
	u32 sx[AVE_CMP420_Y_TILE];
	u8 row[AVE_CMP420_Y_TILE];

	for (u32 x = 0; x < AVE_CMP420_Y_TILE; x++)
		sx[x] = ave_cmp420_spread(x) << 1;
	memset(surf + g->y_meta, 0, g->c - g->y_meta);

	for (u32 ty = 0; ty < g->tiles_y; ty++) {
		for (u32 tx = 0; tx < g->tiles_x; tx++) {
			u8 *body = surf + ty * g->y_pitch + tx * AVE_CMP420_Y_BODY;
			u32 x0 = tx * AVE_CMP420_Y_TILE;
			u32 n = min_t(u32, AVE_CMP420_Y_TILE, g->width - x0);

			for (u32 yy = 0; yy < AVE_CMP420_Y_TILE; yy++) {
				u32 y = ty * AVE_CMP420_Y_TILE + yy;
				u32 sy = ave_cmp420_spread(yy);
				const u8 *s = row;

				if (y < g->height && n == AVE_CMP420_Y_TILE) {
					s = src + (size_t)y * stride + x0;
				} else {
					memset(row, AVE_CMP420_Y_BLACK, sizeof(row));
					if (y < g->height)
						memcpy(row, src + (size_t)y * stride + x0, n);
				}
				for (u32 x = 0; x < AVE_CMP420_Y_TILE; x++)
					body[sy | sx[x]] = s[x];
			}
			memset(surf + g->y_meta + ty * g->y_meta_pitch +
			       tx * AVE_CMP420_Y_META, AVE_CMP420_Y_RAW,
			       AVE_CMP420_Y_META);
		}
	}
}

static void ave_cmp420_pack_c(const struct ave_cmp420_geom *g, u8 *surf,
			      const u8 *src, u32 stride)
{
	u32 cw = g->width / 2, ch = g->height / 2;
	u32 sx[AVE_CMP420_C_TILE];
	u8 row[2 * AVE_CMP420_C_TILE];

	for (u32 x = 0; x < AVE_CMP420_C_TILE; x++)
		sx[x] = ave_cmp420_spread(x) << 1;
	memset(surf + g->c_meta, 0, g->bytes - g->c_meta);

	for (u32 ty = 0; ty < g->tiles_y; ty++) {
		for (u32 tx = 0; tx < g->tiles_x; tx++) {
			u8 *body = surf + g->c + ty * g->c_pitch + tx * AVE_CMP420_C_BODY;
			u32 x0 = tx * AVE_CMP420_C_TILE;
			u32 n = min_t(u32, AVE_CMP420_C_TILE, cw - x0);

			for (u32 yy = 0; yy < AVE_CMP420_C_TILE; yy++) {
				u32 y = ty * AVE_CMP420_C_TILE + yy;
				u32 sy = ave_cmp420_spread(yy);
				const u8 *s = row;

				if (y < ch && n == AVE_CMP420_C_TILE) {
					s = src + (size_t)y * stride + 2 * x0;
				} else {
					memset(row, AVE_CMP420_C_BLACK, sizeof(row));
					if (y < ch)
						memcpy(row, src + (size_t)y * stride + 2 * x0,
						       2 * n);
				}
				for (u32 x = 0; x < AVE_CMP420_C_TILE; x++) {
					u32 m = 2 * (sy | sx[x]);

					body[m] = s[2 * x];
					body[m + 1] = s[2 * x + 1];
				}
			}
			memset(surf + g->c_meta + ty * g->c_meta_pitch +
			       tx * AVE_CMP420_C_META, AVE_CMP420_C_RAW,
			       AVE_CMP420_C_META);
		}
	}
}

/**
 * ave_cmp420_pack - write a linear NV12 picture as an all-raw surface
 * @g: surface geometry
 * @surf: the surface, @g->bytes
 * @y: luma rows, @g->height of them
 * @cbcr: interleaved CbCr rows, @g->height / 2 of them
 * @stride: bytes per row of both planes
 *
 * Every visible metadata byte is the raw value, the padding metadata zero,
 * samples beyond the picture video black.
 */
void ave_cmp420_pack(const struct ave_cmp420_geom *g, u8 *surf,
		     const u8 *y, const u8 *cbcr, u32 stride)
{
	ave_cmp420_pack_y(g, surf, y, stride);
	ave_cmp420_pack_c(g, surf, cbcr, stride);
}

/* Decode one luma compression tile into @out in Morton order */
static unsigned int ave_cmp420_y_ctile(const u8 *body, const u8 *meta, u8 *out,
				       struct ave_cmp420_stats *st)
{
	const u32 size = AVE_CMP420_CTILE * AVE_CMP420_CTILE;
	unsigned int seen = 0;
	bool sync = true;
	u32 off = 0;

	for (u32 i = 0; i < 8; i++) {
		u8 m = meta[i];
		u8 *o = out + i * AVE_CMP420_BLOCK;

		if (sync && m == AVE_CMP420_Y_RAW && off + AVE_CMP420_BLOCK <= size) {
			memcpy(o, body + off, AVE_CMP420_BLOCK);
			off += AVE_CMP420_BLOCK;
			st->raw++;
			seen |= SEEN_RAW;
			continue;
		}
		if (sync && m == AVE_CMP420_Y_SOLID && off < size) {
			memset(o, body[off], AVE_CMP420_BLOCK);
			off += 1;
			st->solid++;
			seen |= SEEN_SOLID;
			continue;
		}
		memset(o, AVE_CMP420_Y_MARK, AVE_CMP420_BLOCK);
		st->other++;
		seen |= SEEN_OTHER;
		if (sync && m && (m >> 5) != 7)
			off += (m & 0x1f) + 1;
		else
			sync = false;
	}
	return seen;
}

/* Decode one CbCr compression tile (a whole storage tile) into @out */
static unsigned int ave_cmp420_c_ctile(const u8 *body, const u8 *meta, u8 *out,
				       struct ave_cmp420_stats *st)
{
	const u32 size = AVE_CMP420_C_BODY;
	unsigned int seen = 0;
	bool sync = true;
	u32 off = 0;

	for (u32 i = 0; i < 8; i++) {
		u8 m = meta[i];
		u8 *o = out + i * 2 * AVE_CMP420_BLOCK;

		if (sync && m == AVE_CMP420_C_RAW && off + 2 * AVE_CMP420_BLOCK <= size) {
			memcpy(o, body + off, 2 * AVE_CMP420_BLOCK);
			off += 2 * AVE_CMP420_BLOCK;
			st->raw++;
			seen |= SEEN_RAW;
			continue;
		}
		if (sync && m == AVE_CMP420_C_SOLID && off + 2 <= size) {
			for (u32 j = 0; j < AVE_CMP420_BLOCK; j++) {
				o[2 * j] = body[off];
				o[2 * j + 1] = body[off + 1];
			}
			off += 2;
			st->solid++;
			seen |= SEEN_SOLID;
			continue;
		}
		for (u32 j = 0; j < AVE_CMP420_BLOCK; j++) {
			o[2 * j] = AVE_CMP420_CB_MARK;
			o[2 * j + 1] = AVE_CMP420_CR_MARK;
		}
		st->other++;
		seen |= SEEN_OTHER;
		/* The CbCr mode sizes are not known: lose the place. */
		sync = false;
	}
	return seen;
}

static void ave_cmp420_count_tile(struct ave_cmp420_stats *st, unsigned int seen)
{
	if (seen & SEEN_OTHER)
		st->tiles_other++;
	else if (seen == SEEN_RAW)
		st->tiles_raw++;
	else if (seen == SEEN_SOLID)
		st->tiles_solid++;
	else
		st->tiles_mixed++;
}

static void ave_cmp420_unpack_y(const struct ave_cmp420_geom *g, const u8 *surf,
				u8 *dst, u32 stride, struct ave_cmp420_stats *st)
{
	u32 sx[AVE_CMP420_CTILE];
	u8 tmp[AVE_CMP420_CTILE * AVE_CMP420_CTILE];

	for (u32 x = 0; x < AVE_CMP420_CTILE; x++)
		sx[x] = ave_cmp420_spread(x) << 1;

	for (u32 ty = 0; ty < g->tiles_y; ty++) {
		for (u32 tx = 0; tx < g->tiles_x; tx++) {
			const u8 *body = surf + ty * g->y_pitch + tx * AVE_CMP420_Y_BODY;
			const u8 *meta = surf + g->y_meta + ty * g->y_meta_pitch +
					 tx * AVE_CMP420_Y_META;
			unsigned int seen = 0;

			for (u32 q = 0; q < 4; q++) {
				u32 x0 = tx * AVE_CMP420_Y_TILE + (q >> 1) * AVE_CMP420_CTILE;
				u32 y0 = ty * AVE_CMP420_Y_TILE + (q & 1) * AVE_CMP420_CTILE;
				u32 n;

				seen |= ave_cmp420_y_ctile(body + q * (AVE_CMP420_Y_BODY / 4),
							   meta + q * 8, tmp, st);
				if (x0 >= g->width)
					continue;
				n = min_t(u32, AVE_CMP420_CTILE, g->width - x0);
				for (u32 yy = 0; yy < AVE_CMP420_CTILE &&
				     y0 + yy < g->height; yy++) {
					u8 *d = dst + (size_t)(y0 + yy) * stride + x0;
					u32 sy = ave_cmp420_spread(yy);

					for (u32 x = 0; x < n; x++)
						d[x] = tmp[sy | sx[x]];
				}
			}
			ave_cmp420_count_tile(st, seen);
		}
	}
}

static void ave_cmp420_unpack_c(const struct ave_cmp420_geom *g, const u8 *surf,
				u8 *dst, u32 stride, struct ave_cmp420_stats *st)
{
	u32 cw = g->width / 2, ch = g->height / 2;
	u32 sx[AVE_CMP420_C_TILE];
	u8 tmp[AVE_CMP420_C_BODY];

	for (u32 x = 0; x < AVE_CMP420_C_TILE; x++)
		sx[x] = ave_cmp420_spread(x) << 1;

	for (u32 ty = 0; ty < g->tiles_y; ty++) {
		for (u32 tx = 0; tx < g->tiles_x; tx++) {
			const u8 *body = surf + g->c + ty * g->c_pitch +
					 tx * AVE_CMP420_C_BODY;
			const u8 *meta = surf + g->c_meta + ty * g->c_meta_pitch +
					 tx * AVE_CMP420_C_META;
			u32 x0 = tx * AVE_CMP420_C_TILE, y0 = ty * AVE_CMP420_C_TILE;
			u32 n = min_t(u32, AVE_CMP420_C_TILE, cw - x0);

			ave_cmp420_count_tile(st, ave_cmp420_c_ctile(body, meta, tmp, st));
			for (u32 yy = 0; yy < AVE_CMP420_C_TILE && y0 + yy < ch; yy++) {
				u8 *d = dst + (size_t)(y0 + yy) * stride + 2 * x0;
				u32 sy = ave_cmp420_spread(yy);

				for (u32 x = 0; x < n; x++) {
					u32 m = 2 * (sy | sx[x]);

					d[2 * x] = tmp[m];
					d[2 * x + 1] = tmp[m + 1];
				}
			}
		}
	}
}

/**
 * ave_cmp420_unpack - read a surface as a linear NV12 picture
 * @g: surface geometry
 * @surf: the surface, @g->bytes
 * @y: luma rows to fill, @g->height of them
 * @cbcr: CbCr rows to fill, @g->height / 2 of them
 * @stride: bytes per row of both planes
 * @ys: luma block and tile counts, zeroed here
 * @cs: CbCr block and tile counts, zeroed here
 *
 * Raw and solid blocks are exact; every other block becomes the marker.
 */
void ave_cmp420_unpack(const struct ave_cmp420_geom *g, const u8 *surf,
		       u8 *y, u8 *cbcr, u32 stride,
		       struct ave_cmp420_stats *ys, struct ave_cmp420_stats *cs)
{
	memset(ys, 0, sizeof(*ys));
	memset(cs, 0, sizeof(*cs));
	ave_cmp420_unpack_y(g, surf, y, stride, ys);
	ave_cmp420_unpack_c(g, surf, cbcr, stride, cs);
}

/**
 * ave_cmp420_meta_hist - count each metadata byte value of the picture tiles
 * @g: surface geometry
 * @surf: the surface
 * @chroma: the CbCr metadata instead of the luma
 * @hist: counts by value, saturating
 */
void ave_cmp420_meta_hist(const struct ave_cmp420_geom *g, const u8 *surf,
			  bool chroma, u16 hist[256])
{
	u32 per = chroma ? AVE_CMP420_C_META : AVE_CMP420_Y_META;
	u32 pitch = chroma ? g->c_meta_pitch : g->y_meta_pitch;
	const u8 *meta = surf + (chroma ? g->c_meta : g->y_meta);

	memset(hist, 0, 256 * sizeof(*hist));
	for (u32 ty = 0; ty < g->tiles_y; ty++) {
		const u8 *p = meta + ty * pitch;

		for (u32 i = 0; i < g->tiles_x * per; i++)
			if (hist[p[i]] != U16_MAX)
				hist[p[i]]++;
	}
}
