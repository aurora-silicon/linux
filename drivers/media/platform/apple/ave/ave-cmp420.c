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
#include <linux/unaligned.h>

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

/*
 * Metadata is not row-major: the metadata of storage tile (tx, ty) sits at
 * slot M(tx, ty) of a square, power-of-two grid, with the tx bits on the
 * even bit positions and the ty bits on the odd ones.
 */
static inline u32 ave_cmp420_meta_slot(u32 tx, u32 ty)
{
	return ave_cmp420_spread(tx) | ave_cmp420_spread(ty) << 1;
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
	/* Morton slots of 8 bits per axis at most (ave_cmp420_spread()) */
	if (g->tiles_x > 256 || g->tiles_y > 256)
		return -EINVAL;
	cols = roundup_pow_of_two(max3(g->tiles_x, g->tiles_y,
				       (u32)AVE_CMP420_META_ALIGN));
	rows = cols;
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
	/* Every slot raw, the padding tiles' included: the reader visits them. */
	memset(surf + g->y_meta, AVE_CMP420_Y_RAW, g->c - g->y_meta);

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
	memset(surf + g->c_meta, AVE_CMP420_C_RAW, g->bytes - g->c_meta);

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

/* LSB-first bit reader over one block payload, zero padded */
struct ave_cmp420_bits {
	u8 buf[64 + 8];
	u32 pos;
};

static void ave_cmp420_bits_init(struct ave_cmp420_bits *b, const u8 *p, u32 len)
{
	len = min_t(u32, len, 64);
	memcpy(b->buf, p, len);
	memset(b->buf + len, 0, sizeof(b->buf) - len);
	b->pos = 0;
}

static u32 ave_cmp420_get(struct ave_cmp420_bits *b, u32 n)
{
	u64 w;

	if (!n)
		return 0;
	w = get_unaligned_le64(b->buf + min_t(u32, b->pos / 8, 64));
	w >>= b->pos % 8;
	b->pos += n;
	return w & (BIT_ULL(n) - 1);
}

static s32 ave_cmp420_sget(struct ave_cmp420_bits *b, u32 n)
{
	u32 v = ave_cmp420_get(b, n);

	return n && (v & BIT(n - 1)) ? (s32)v - (s32)BIT(n) : (s32)v;
}

/*
 * Neighbour modes (1, 5): each residual slot predicts from another slot,
 * growing out from the origin; AVE_CMP420_FROM_BASE predicts from the base.
 * The origin is sample 7 (the block centre) and sample 0 takes its slot.
 */
#define AVE_CMP420_FROM_BASE	0xff
static const u8 ave_cmp420_nb_pred[AVE_CMP420_BLOCK] = {
	[1] = 4, [2] = 3, [3] = 6, [4] = AVE_CMP420_FROM_BASE,
	[5] = AVE_CMP420_FROM_BASE, [6] = AVE_CMP420_FROM_BASE, [7] = 1,
	[8] = 9, [9] = 12, [10] = 11, [11] = 12, [12] = AVE_CMP420_FROM_BASE,
	[13] = AVE_CMP420_FROM_BASE, [14] = 12, [15] = 13,
	[16] = AVE_CMP420_FROM_BASE, [17] = 16, [18] = AVE_CMP420_FROM_BASE,
	[19] = 18, [20] = 17, [21] = 20, [22] = 19, [23] = 22,
	[24] = AVE_CMP420_FROM_BASE, [25] = 24, [26] = 24, [27] = 24,
	[28] = 25, [29] = 28, [30] = 27, [31] = 30,
};

/* Slots of the neighbour modes in an order where every predictor comes first */
static const u8 ave_cmp420_nb_order[AVE_CMP420_BLOCK - 1] = {
	4, 5, 6, 12, 13, 16, 18, 24,
	1, 3, 9, 11, 14, 15, 17, 19, 25, 26, 27,
	7, 2, 8, 10, 20, 22, 28, 30,
	21, 23, 29, 31,
};

/*
 * Decode one coded luma block (modes 0, 1, 4, 5) into 32 samples in Morton
 * order. Payload: has_bias:1, width-1:3, base:8, [four 2-bit width
 * reductions, modes 4/5], [signed bias:width], then 31 signed residuals,
 * slot k at width - reduction[k / 8]; the bias applies to full-width
 * regions only. Returns false for a mode this reader does not know.
 */
static bool ave_cmp420_y_coded(const u8 *p, u32 len, u8 mode, u8 *o)
{
	struct ave_cmp420_bits b;
	u32 w, red[4] = { 0 };
	s32 bias = 0, eff[AVE_CMP420_BLOCK] = { 0 };
	bool has_bias;
	u8 base, v[AVE_CMP420_BLOCK];

	if (mode != 0 && mode != 1 && mode != 4 && mode != 5)
		return false;
	ave_cmp420_bits_init(&b, p, len);
	has_bias = ave_cmp420_get(&b, 1);
	w = ave_cmp420_get(&b, 3) + 1;
	base = ave_cmp420_get(&b, 8);
	if (mode >= 4)
		for (u32 r = 0; r < 4; r++)
			red[r] = ave_cmp420_get(&b, 2);
	if (has_bias)
		bias = ave_cmp420_sget(&b, w);
	for (u32 k = 1; k < AVE_CMP420_BLOCK; k++) {
		u32 g = k / 8, n = w > red[g] ? w - red[g] : 0;

		eff[k] = ave_cmp420_sget(&b, n) - (red[g] ? 0 : bias);
	}
	if (mode == 0 || mode == 4) {
		o[0] = base;
		for (u32 k = 1; k < AVE_CMP420_BLOCK; k++)
			o[k] = base + eff[k];
		return true;
	}
	v[0] = base;
	for (u32 i = 0; i < ARRAY_SIZE(ave_cmp420_nb_order); i++) {
		u8 k = ave_cmp420_nb_order[i], from = ave_cmp420_nb_pred[k];

		v[k] = (from == AVE_CMP420_FROM_BASE ? base : v[from]) + eff[k];
	}
	for (u32 k = 0; k < AVE_CMP420_BLOCK; k++)
		o[k == 0 ? 7 : k == 7 ? 0 : k] = v[k];
	return true;
}

/*
 * Decode the four luma compression tiles of one storage tile into @out
 * (1024 samples, Morton order). The 32 block payloads are packed from the
 * start of the body in metadata order.
 */
static unsigned int ave_cmp420_y_stile(const u8 *body, const u8 *meta, u8 *out,
				       struct ave_cmp420_stats *st)
{
	unsigned int seen = 0;
	u32 off = 0;

	for (u32 i = 0; i < 32; i++) {
		u8 m = meta[i], *o = out + i * AVE_CMP420_BLOCK;
		u32 n = (m & 0x1f) + 1;

		if (off + n > AVE_CMP420_Y_BODY) {
			memset(o, AVE_CMP420_Y_MARK, AVE_CMP420_BLOCK);
			st->other++;
			seen |= SEEN_OTHER;
			continue;
		}
		if (m == AVE_CMP420_Y_RAW) {
			memcpy(o, body + off, AVE_CMP420_BLOCK);
			st->raw++;
			seen |= SEEN_RAW;
		} else if (m == AVE_CMP420_Y_SOLID) {
			memset(o, body[off], AVE_CMP420_BLOCK);
			st->solid++;
			seen |= SEEN_SOLID;
		} else if (ave_cmp420_y_coded(body + off, n, m >> 5, o)) {
			st->coded++;
			seen |= SEEN_RAW;
		} else {
			memset(o, AVE_CMP420_Y_MARK, AVE_CMP420_BLOCK);
			st->other++;
			seen |= SEEN_OTHER;
		}
		off += n;
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
		{
			u32 n = (m & 0x3f) + 1;
			bool fits = off + n <= size;
			struct ave_cmp420_bits b;
			u8 cb, cr;

			/* Coded RG8 block: only its two bases are known (bits 14, 22). */
			ave_cmp420_bits_init(&b, body + off, fits ? n : 0);
			b.pos = 14;
			cb = ave_cmp420_get(&b, 8);
			cr = ave_cmp420_get(&b, 8);
			for (u32 j = 0; j < AVE_CMP420_BLOCK; j++) {
				o[2 * j] = sync && fits ? cb : AVE_CMP420_CB_MARK;
				o[2 * j + 1] = sync && fits ? cr : AVE_CMP420_CR_MARK;
			}
			st->other++;
			seen |= SEEN_OTHER;
			if (fits)
				off += n;
			else
				sync = false;
		}
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
	u32 sx[AVE_CMP420_Y_TILE];
	u8 tmp[AVE_CMP420_Y_TILE * AVE_CMP420_Y_TILE];

	for (u32 x = 0; x < AVE_CMP420_Y_TILE; x++)
		sx[x] = ave_cmp420_spread(x) << 1;

	for (u32 ty = 0; ty < g->tiles_y; ty++) {
		for (u32 tx = 0; tx < g->tiles_x; tx++) {
			const u8 *body = surf + ty * g->y_pitch + tx * AVE_CMP420_Y_BODY;
			const u8 *meta = surf + g->y_meta +
					 ave_cmp420_meta_slot(tx, ty) * AVE_CMP420_Y_META;
			u32 x0 = tx * AVE_CMP420_Y_TILE, y0 = ty * AVE_CMP420_Y_TILE;
			u32 n = min_t(u32, AVE_CMP420_Y_TILE, g->width - x0);

			ave_cmp420_count_tile(st, ave_cmp420_y_stile(body, meta, tmp, st));
			for (u32 yy = 0; yy < AVE_CMP420_Y_TILE && y0 + yy < g->height; yy++) {
				u8 *d = dst + (size_t)(y0 + yy) * stride + x0;
				u32 sy = ave_cmp420_spread(yy);

				for (u32 x = 0; x < n; x++)
					d[x] = tmp[sy | sx[x]];
			}
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
			const u8 *meta = surf + g->c_meta +
					 ave_cmp420_meta_slot(tx, ty) * AVE_CMP420_C_META;
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
	const u8 *meta = surf + (chroma ? g->c_meta : g->y_meta);

	memset(hist, 0, 256 * sizeof(*hist));
	for (u32 ty = 0; ty < g->tiles_y; ty++)
		for (u32 tx = 0; tx < g->tiles_x; tx++) {
			const u8 *p = meta + ave_cmp420_meta_slot(tx, ty) * per;

			for (u32 i = 0; i < per; i++)
				if (hist[p[i]] != U16_MAX)
					hist[p[i]]++;
		}
}
