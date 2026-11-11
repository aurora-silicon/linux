// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compressed 4:2:0 surface helpers: the 1080p geometry the MCTF layout
 * uses, the raw writer's sample order and metadata, NV12 -> raw tiles ->
 * NV12 identity, exact solid and mixed raw/solid decoding, and markers for
 * modes that are not decoded. Expected offsets are worked out here from
 * the tile rules, apart from the driver's own arithmetic.
 *
 * Copyright The Aurora Silicon Contributors
 */
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "ave-cmp420.h"

struct t_pic {
	struct ave_cmp420_geom g;
	u8 *surf, *in, *out;
	u32 stride;
	size_t luma;
};

static void t_free(void *p)
{
	kvfree(p);
}

static void *t_alloc(struct kunit *test, size_t n)
{
	void *p = kvzalloc(n, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, t_free, p), 0);
	return p;
}

static void t_pic_init(struct kunit *test, struct t_pic *t, u32 w, u32 h, u32 stride)
{
	KUNIT_ASSERT_EQ(test, ave_cmp420_geom(&t->g, w, h), 0);
	t->stride = stride;
	t->luma = (size_t)stride * h;
	t->surf = t_alloc(test, t->g.bytes);
	t->in = t_alloc(test, t->luma * 3 / 2);
	t->out = t_alloc(test, t->luma * 3 / 2);
}

/* A deterministic picture with no two neighbours alike */
static void t_fill(struct t_pic *t)
{
	u32 x = 0x12345678;

	for (size_t i = 0; i < t->luma * 3 / 2; i++) {
		x = x * 1103515245 + 12345;
		t->in[i] = x >> 16;
	}
}

/* Y-first Morton index, bit by bit */
static u32 t_morton(u32 x, u32 y)
{
	u32 m = 0;

	for (u32 b = 0; b < 8; b++)
		m |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
	return m;
}

static u8 *t_ybody(struct t_pic *t, u32 tx, u32 ty)
{
	return t->surf + ty * t->g.y_pitch + tx * 1024;
}

static u8 *t_cbody(struct t_pic *t, u32 tx, u32 ty)
{
	return t->surf + t->g.c + ty * t->g.c_pitch + tx * 512;
}

/* The 1920x1080 surface the Neo MCTF Process describes */
static void ave_cmp420_geom_1080p(struct kunit *test)
{
	struct ave_cmp420_geom g;

	KUNIT_ASSERT_EQ(test, ave_cmp420_geom(&g, 1920, 1080), 0);
	KUNIT_EXPECT_EQ(test, g.tiles_x, 60);
	KUNIT_EXPECT_EQ(test, g.tiles_y, 34);
	KUNIT_EXPECT_EQ(test, g.y_pitch, 0xf000);
	KUNIT_EXPECT_EQ(test, g.y_meta, 0x1fe000);
	KUNIT_EXPECT_EQ(test, g.y_meta_pitch, 0x800);
	KUNIT_EXPECT_EQ(test, g.c, 0x21e000);
	KUNIT_EXPECT_EQ(test, g.c_pitch, 0x7800);
	KUNIT_EXPECT_EQ(test, g.c_meta, 0x31d000);
	KUNIT_EXPECT_EQ(test, g.c_meta_pitch, 0x200);
	KUNIT_EXPECT_EQ(test, g.bytes, 0x325000);
	KUNIT_EXPECT_EQ(test, ave_cmp420_geom(&g, 0, 1080), -EINVAL);
	KUNIT_EXPECT_EQ(test, ave_cmp420_geom(&g, 1921, 1080), -EINVAL);
	KUNIT_EXPECT_EQ(test, ave_cmp420_geom(&g, 1920, 1081), -EINVAL);
}

/* Where the writer puts samples, its metadata, and the padding */
static void ave_cmp420_pack_layout(struct kunit *test)
{
	struct t_pic t;
	u32 raw_y = 0, raw_c = 0;

	t_pic_init(test, &t, 1920, 1080, 1920);
	t_fill(&t);
	ave_cmp420_pack(&t.g, t.surf, t.in, t.in + t.luma, t.stride);

	/* Luma: Morton inside the tile, quadrants TL, BL, TR, BR */
	KUNIT_EXPECT_EQ(test, t_ybody(&t, 0, 0)[0], t.in[0]);
	KUNIT_EXPECT_EQ(test, t_ybody(&t, 0, 0)[1], t.in[1920]);
	KUNIT_EXPECT_EQ(test, t_ybody(&t, 0, 0)[2], t.in[1]);
	KUNIT_EXPECT_EQ(test, t_ybody(&t, 0, 0)[3], t.in[1920 + 1]);
	KUNIT_EXPECT_EQ(test, t_ybody(&t, 0, 0)[256], t.in[16 * 1920]);
	KUNIT_EXPECT_EQ(test, t_ybody(&t, 0, 0)[512], t.in[16]);
	KUNIT_EXPECT_EQ(test, t_ybody(&t, 0, 0)[768], t.in[16 * 1920 + 16]);
	KUNIT_EXPECT_EQ(test, t_ybody(&t, 0, 0)[1023], t.in[31 * 1920 + 31]);
	KUNIT_EXPECT_EQ(test, t_ybody(&t, 1, 0)[0], t.in[32]);
	KUNIT_EXPECT_EQ(test, t_ybody(&t, 0, 1)[0], t.in[32 * 1920]);
	KUNIT_EXPECT_EQ(test, t_ybody(&t, 59, 33)[t_morton(5, 7)],
			t.in[(33 * 32 + 7) * 1920 + 59 * 32 + 5]);
	/* Lines 1080..1087 are video black */
	KUNIT_EXPECT_EQ(test, t_ybody(&t, 7, 33)[t_morton(3, 24)], 0x10);
	KUNIT_EXPECT_EQ(test, t_ybody(&t, 7, 33)[t_morton(31, 31)], 0x10);

	/* CbCr pairs, same order, 16x16 tiles */
	KUNIT_EXPECT_EQ(test, t_cbody(&t, 0, 0)[0], t.in[t.luma]);
	KUNIT_EXPECT_EQ(test, t_cbody(&t, 0, 0)[1], t.in[t.luma + 1]);
	KUNIT_EXPECT_EQ(test, t_cbody(&t, 0, 0)[2], t.in[t.luma + 1920]);
	KUNIT_EXPECT_EQ(test, t_cbody(&t, 0, 0)[4], t.in[t.luma + 2]);
	KUNIT_EXPECT_EQ(test, t_cbody(&t, 0, 0)[5], t.in[t.luma + 3]);
	KUNIT_EXPECT_EQ(test, t_cbody(&t, 2, 3)[2 * t_morton(9, 4) + 1],
			t.in[t.luma + (3 * 16 + 4) * 1920 + 2 * (2 * 16 + 9) + 1]);
	/* Chroma lines 540..543 are video black */
	KUNIT_EXPECT_EQ(test, t_cbody(&t, 4, 33)[2 * t_morton(0, 12)], 0x80);
	KUNIT_EXPECT_EQ(test, t_cbody(&t, 4, 33)[2 * t_morton(15, 15) + 1], 0x80);

	/* Every metadata slot raw, the padding tiles' too: the reader visits them */
	for (u32 i = t.g.y_meta; i < t.g.c; i++) {
		KUNIT_ASSERT_EQ(test, t.surf[i], 0x1f);
		raw_y++;
	}
	for (u32 i = t.g.c_meta; i < t.g.bytes; i++) {
		KUNIT_ASSERT_EQ(test, t.surf[i], 0x3f);
		raw_c++;
	}
	/* A 64x64 Morton grid of slots */
	KUNIT_EXPECT_EQ(test, raw_y, 64 * 64 * 32);
	KUNIT_EXPECT_EQ(test, raw_c, 64 * 64 * 8);
}

static void t_round_trip(struct kunit *test, u32 w, u32 h, u32 stride)
{
	struct ave_cmp420_stats ys, cs;
	struct t_pic t;
	u32 tiles;

	t_pic_init(test, &t, w, h, stride);
	t_fill(&t);
	ave_cmp420_pack(&t.g, t.surf, t.in, t.in + t.luma, t.stride);
	memset(t.out, 0x5a, t.luma * 3 / 2);
	ave_cmp420_unpack(&t.g, t.surf, t.out, t.out + t.luma, t.stride, &ys, &cs);

	for (u32 y = 0; y < h; y++)
		KUNIT_ASSERT_EQ_MSG(test, memcmp(t.in + y * stride, t.out + y * stride, w),
				    0, "luma line %u of %ux%u", y, w, h);
	for (u32 y = 0; y < h / 2; y++)
		KUNIT_ASSERT_EQ_MSG(test, memcmp(t.in + t.luma + y * stride,
						 t.out + t.luma + y * stride, w),
				    0, "chroma line %u of %ux%u", y, w, h);
	/* Bytes beyond the width are not written */
	if (stride > w)
		KUNIT_EXPECT_EQ(test, t.out[stride - 1], 0x5a);

	tiles = t.g.tiles_x * t.g.tiles_y;
	KUNIT_EXPECT_EQ(test, ys.tiles_raw, tiles);
	KUNIT_EXPECT_EQ(test, ys.raw, tiles * 32);
	KUNIT_EXPECT_EQ(test, ys.solid + ys.other + ys.tiles_solid +
			      ys.tiles_mixed + ys.tiles_other, 0);
	KUNIT_EXPECT_EQ(test, cs.tiles_raw, tiles);
	KUNIT_EXPECT_EQ(test, cs.raw, tiles * 8);
	KUNIT_EXPECT_EQ(test, cs.solid + cs.other + cs.tiles_solid +
			      cs.tiles_mixed + cs.tiles_other, 0);
}

/* NV12 -> raw tiles -> NV12 is the identity, edges included */
static void ave_cmp420_round_trip(struct kunit *test)
{
	t_round_trip(test, 1920, 1080, 1920);
	t_round_trip(test, 72, 40, 80);		/* partial tiles both ways */
	t_round_trip(test, 30, 18, 30);		/* smaller than one tile */
	t_round_trip(test, 64, 64, 64);		/* whole tiles */
}

/* Solid blocks decode to their constants, and mixed with raw by prefix sum */
/* Metadata slot of storage tile (tx, ty): tx bits even, ty bits odd */
static u32 t_slot(u32 tx, u32 ty)
{
	u32 m = 0;

	for (u32 k = 0; k < 8; k++)
		m |= ((tx >> k) & 1) << (2 * k) | ((ty >> k) & 1) << (2 * k + 1);
	return m;
}

/*
 * Blocks the encoder's MCTF wrote on hardware for a static test card at
 * strength 0 (output equals input there), with the input samples of each
 * block in Morton order: neighbour mode 0xa4, delta mode 0x84 and solids,
 * packed from the start of the storage tile body.
 */
static const u8 t_hw_body[] = {
	0x10, 0x10, 0x10, 0x10, 0x10, 0x41, 0x84, 0x09, 0x00, 0x11,
	0x10, 0x41, 0x84, 0x09, 0x00, 0x11,
	0x11, 0x11, 0x11, 0x00, 0x00,
};
static const u8 t_hw_meta[] = { 0x60, 0x60, 0x60, 0x60, 0xa4, 0x60, 0xa4, 0x60, 0x84 };
static const u8 t_hw_a4[32] = {
	16, 16, 17, 17, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
	16, 16, 17, 17, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
};
static const u8 t_hw_84[32] = {
	17, 17, 17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18, 18,
	17, 17, 17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18, 18,
};

static void ave_cmp420_hw_blocks(struct kunit *test)
{
	struct ave_cmp420_stats ys, cs;
	struct t_pic t;
	u8 *again, *meta;

	t_pic_init(test, &t, 1920, 1080, 1920);
	again = t_alloc(test, t.g.bytes);
	/* Every other tile solid 0x55, the CbCr plane solid (0x40, 0xc0) */
	memset(t.surf + t.g.y_meta, 0x60, t.g.c - t.g.y_meta);
	memset(t.surf + t.g.c_meta, 0x01, t.g.bytes - t.g.c_meta);
	for (u32 ty = 0; ty < t.g.tiles_y; ty++)
		for (u32 tx = 0; tx < t.g.tiles_x; tx++) {
			memset(t_ybody(&t, tx, ty), 0x55, 32);
			t_cbody(&t, tx, ty)[0] = 0x40;
			for (u32 i = 0; i < 8; i++) {
				t_cbody(&t, tx, ty)[2 * i] = 0x40;
				t_cbody(&t, tx, ty)[2 * i + 1] = 0xc0;
			}
		}
	/* Tile (3, 5): the hardware blocks, then solids; slot M(3, 5) = 39 */
	KUNIT_EXPECT_EQ(test, t_slot(3, 5), 39U);
	meta = t.surf + t.g.y_meta + 32 * t_slot(3, 5);
	memcpy(meta, t_hw_meta, sizeof(t_hw_meta));
	memset(t_ybody(&t, 3, 5), 0x55, 1024);
	memcpy(t_ybody(&t, 3, 5), t_hw_body, sizeof(t_hw_body));
	/* A gradient-mode block (not decoded) of 3 bytes keeps the place */
	meta[9] = 0x42;

	ave_cmp420_unpack(&t.g, t.surf, t.out, t.out + t.luma, t.stride, &ys, &cs);
	KUNIT_EXPECT_EQ(test, ys.coded, 3U);
	KUNIT_EXPECT_EQ(test, ys.other, 1U);
	KUNIT_EXPECT_EQ(test, cs.solid, t.g.tiles_x * t.g.tiles_y * 8);

	/* Read back through the raw writer: tile bodies hold Morton samples */
	ave_cmp420_pack(&t.g, again, t.out, t.out + t.luma, t.stride);
	for (u32 j = 0; j < 32; j++) {
		const u8 *y = again + 5 * t.g.y_pitch + 3 * 1024;

		KUNIT_EXPECT_EQ(test, y[j], 0x10);
		KUNIT_EXPECT_EQ(test, y[128 + j], t_hw_a4[j]);
		KUNIT_EXPECT_EQ(test, y[160 + j], 0x11);
		KUNIT_EXPECT_EQ(test, y[192 + j], t_hw_a4[j]);
		KUNIT_EXPECT_EQ(test, y[224 + j], 0x11);
		KUNIT_EXPECT_EQ(test, y[256 + j], t_hw_84[j]);
		KUNIT_EXPECT_EQ(test, y[288 + j], 0xff);	/* the marker */
		/* the solid after it still finds its byte (0x55 filler) */
		KUNIT_EXPECT_EQ(test, y[320 + j], 0x55);
		KUNIT_EXPECT_EQ(test, again[4 * 1024 + j], 0x55);
	}
	KUNIT_EXPECT_EQ(test, t.out[t.luma], 0x40);
	KUNIT_EXPECT_EQ(test, t.out[t.luma + 1], 0xc0);
}

/*
 * CbCr blocks the encoder's MCTF wrote on hardware at strength 0, with the
 * input pairs of each block in Morton order: direct and neighbour
 * prediction for both components, region reductions and biases, and a
 * constant component on either side.
 */
static const u8 t_hw_c_body[] = {
	0x00, 0x49, 0xdf, 0x60, 0x74, 0xd3, 0xc0, 0x40, 0x00, 0xcd, 0x10, 0xc0,
	0xf3, 0xfb, 0xeb, 0xc3, 0xcf, 0xb8, 0xfa, 0x03, 0x23, 0x12, 0xa0, 0x20,
	0x41, 0xc4, 0xd1, 0x85, 0x08, 0xc4, 0x7b, 0xf3, 0x07, 0xf8, 0x83, 0x71,
	0xb8, 0x27, 0x06, 0xbb, 0xe4, 0xfb, 0xc5, 0x91, 0xeb, 0xf3, 0x0a, 0x00,
	0x89, 0x9f, 0x21, 0xd7, 0x30, 0x33, 0x92, 0xc0, 0x07, 0x43, 0x70, 0x60,
	0x0f, 0x3c, 0x70, 0x0f, 0xc0, 0xd4, 0x01, 0x6b, 0x49, 0x5f, 0x61, 0x81,
	0xc0, 0x0b, 0xfd, 0x07, 0x88, 0x1c, 0x97, 0x1b, 0xf8, 0x9f, 0x6d, 0xfe,
	0x79, 0x00, 0x1b, 0x80, 0x5f, 0x21, 0x41, 0x37, 0x3c, 0x3c, 0x68, 0x28,
	0xe0, 0x2a, 0xfc, 0x02, 0x2e, 0xe0, 0xb2, 0x2c, 0xcb, 0x02, 0x2e, 0xe0,
	0xb2, 0x2c, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1a, 0xc6, 0x94,
	0x16, 0x10, 0x08, 0x04, 0xda, 0x6c, 0x40, 0x20, 0x10, 0x08, 0xb4, 0x03,
	0x81, 0x76, 0xbb, 0x0d, 0x08, 0x04, 0x5a, 0x81, 0x40, 0x20, 0x10, 0x68,
	0xb7, 0x03, 0x81, 0x40, 0x20, 0x00, 0x00, 0x60, 0x61, 0x8a, 0x08, 0x59,
	0x18,
};
static const u8 t_hw_c_meta[8] = { 0x13, 0x1a, 0x53, 0x52, 0x07, 0xd6, 0x5f, 0xc7 };
static const u8 t_hw_c_pairs[8][64] = {
	{
		0x7d, 0x83, 0x7e, 0x83, 0x7e, 0x82, 0x7e, 0x82, 0x7d, 0x84, 0x7c, 0x83,
		0x7d, 0x83, 0x7c, 0x83, 0x7d, 0x83, 0x7e, 0x83, 0x7d, 0x83, 0x7d, 0x84,
		0x7c, 0x83, 0x7c, 0x83, 0x7d, 0x84, 0x7d, 0x83, 0x7d, 0x83, 0x7c, 0x82,
		0x7d, 0x82, 0x7c, 0x82, 0x7b, 0x82, 0x7c, 0x82, 0x7b, 0x81, 0x7c, 0x82,
		0x7d, 0x83, 0x7c, 0x82, 0x7c, 0x83, 0x7c, 0x83, 0x7b, 0x82, 0x7b, 0x81,
		0x7b, 0x82, 0x7c, 0x82,
	},
	{
		0x80, 0x82, 0x81, 0x83, 0x80, 0x80, 0x81, 0x80, 0x81, 0x83, 0x81, 0x82,
		0x82, 0x80, 0x80, 0x80, 0x7f, 0x7f, 0x81, 0x80, 0x7f, 0x81, 0x80, 0x82,
		0x80, 0x80, 0x7f, 0x81, 0x80, 0x82, 0x7f, 0x82, 0x80, 0x82, 0x7d, 0x81,
		0x7f, 0x81, 0x7d, 0x82, 0x7f, 0x82, 0x81, 0x81, 0x7f, 0x83, 0x82, 0x83,
		0x7f, 0x81, 0x7f, 0x81, 0x7e, 0x82, 0x80, 0x82, 0x82, 0x81, 0x83, 0x81,
		0x84, 0x80, 0x84, 0x7f,
	},
	{
		0x7e, 0x86, 0x7e, 0x85, 0x7e, 0x87, 0x7d, 0x86, 0x7e, 0x85, 0x7e, 0x85,
		0x7e, 0x85, 0x7e, 0x84, 0x7d, 0x87, 0x7d, 0x86, 0x7e, 0x86, 0x7e, 0x85,
		0x7f, 0x86, 0x7e, 0x85, 0x7f, 0x86, 0x7f, 0x86, 0x7e, 0x85, 0x7f, 0x86,
		0x7e, 0x84, 0x7f, 0x85, 0x7e, 0x86, 0x7e, 0x86, 0x7e, 0x85, 0x7e, 0x86,
		0x7e, 0x85, 0x7f, 0x85, 0x7d, 0x86, 0x7e, 0x86, 0x7f, 0x86, 0x7e, 0x86,
		0x7f, 0x87, 0x7e, 0x87,
	},
	{
		0x7c, 0x85, 0x7d, 0x85, 0x7c, 0x85, 0x7d, 0x85, 0x7d, 0x85, 0x7d, 0x85,
		0x7d, 0x85, 0x7d, 0x85, 0x7c, 0x86, 0x7c, 0x86, 0x7b, 0x87, 0x7c, 0x87,
		0x7d, 0x86, 0x7d, 0x86, 0x7c, 0x87, 0x7d, 0x88, 0x7c, 0x85, 0x7d, 0x84,
		0x7d, 0x84, 0x7e, 0x84, 0x7f, 0x84, 0x80, 0x85, 0x7f, 0x84, 0x7f, 0x84,
		0x7d, 0x85, 0x7e, 0x85, 0x7c, 0x87, 0x7d, 0x86, 0x7e, 0x85, 0x7e, 0x85,
		0x7e, 0x86, 0x7e, 0x87,
	},
	{
		0x7e, 0x85, 0x7f, 0x85, 0x7e, 0x85, 0x7e, 0x85, 0x7e, 0x85, 0x7f, 0x85,
		0x7e, 0x85, 0x7e, 0x85, 0x7e, 0x85, 0x7e, 0x85, 0x7e, 0x85, 0x7e, 0x85,
		0x7e, 0x85, 0x7e, 0x85, 0x7e, 0x85, 0x7e, 0x85, 0x7f, 0x85, 0x7f, 0x85,
		0x7f, 0x85, 0x7f, 0x85, 0x7e, 0x85, 0x7e, 0x85, 0x7e, 0x85, 0x7e, 0x85,
		0x7f, 0x85, 0x7f, 0x85, 0x7f, 0x85, 0x7f, 0x85, 0x7e, 0x85, 0x7e, 0x85,
		0x7e, 0x85, 0x7e, 0x85,
	},
	{
		0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xab, 0x80, 0xab,
		0x80, 0xab, 0x80, 0xab, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
		0x80, 0xab, 0x80, 0xab, 0x80, 0xab, 0x80, 0xab, 0x80, 0xab, 0x80, 0xab,
		0x80, 0xab, 0x80, 0xab, 0x80, 0xab, 0x80, 0xab, 0x80, 0xab, 0x80, 0xab,
		0x80, 0xab, 0x80, 0xab, 0x80, 0xab, 0x80, 0xab, 0x80, 0xab, 0x80, 0xab,
		0x80, 0xab, 0x80, 0xab,
	},
	{
		0xc9, 0x5a, 0xc9, 0x5a, 0x53, 0x5a, 0x53, 0x5a, 0xc9, 0x5a, 0xc9, 0x5a,
		0x53, 0x5a, 0x53, 0x5a, 0x53, 0x5a, 0x53, 0x5a, 0x89, 0x5a, 0x89, 0x5a,
		0x53, 0x5a, 0x53, 0x5a, 0x89, 0x5a, 0x89, 0x5a, 0xc9, 0x5a, 0xc9, 0x5a,
		0x53, 0x5a, 0x53, 0x5a, 0xdf, 0x5a, 0xdf, 0x5a, 0x53, 0x5a, 0x53, 0x5a,
		0x53, 0x5a, 0x53, 0x5a, 0x89, 0x5a, 0x89, 0x5a, 0x53, 0x5a, 0x53, 0x5a,
		0x89, 0x5a, 0x89, 0x5a,
	},
	{
		0x80, 0x85, 0x80, 0x84, 0x80, 0x85, 0x80, 0x85, 0x80, 0x84, 0x80, 0x85,
		0x80, 0x84, 0x80, 0x85, 0x80, 0x85, 0x80, 0x85, 0x80, 0x84, 0x80, 0x85,
		0x80, 0x85, 0x80, 0x85, 0x80, 0x84, 0x80, 0x85, 0x80, 0x85, 0x80, 0x85,
		0x80, 0x85, 0x80, 0x84, 0x80, 0x85, 0x80, 0x85, 0x80, 0x84, 0x80, 0x84,
		0x80, 0x85, 0x80, 0x84, 0x80, 0x85, 0x80, 0x85, 0x80, 0x85, 0x80, 0x85,
		0x80, 0x84, 0x80, 0x84,
	},
};

static void ave_cmp420_hw_cbcr(struct kunit *test)
{
	struct ave_cmp420_stats ys, cs;
	struct t_pic t;
	u8 *again, *c;

	t_pic_init(test, &t, 1920, 1080, 1920);
	again = t_alloc(test, t.g.bytes);
	memset(t.surf + t.g.y_meta, 0x60, t.g.c - t.g.y_meta);
	memset(t.surf + t.g.c_meta, 0x01, t.g.bytes - t.g.c_meta);
	for (u32 ty = 0; ty < t.g.tiles_y; ty++)
		for (u32 tx = 0; tx < t.g.tiles_x; tx++)
			for (u32 i = 0; i < 8; i++) {
				t_cbody(&t, tx, ty)[2 * i] = 0x40;
				t_cbody(&t, tx, ty)[2 * i + 1] = 0xc0;
			}
	memcpy(t.surf + t.g.c_meta + 8 * t_slot(3, 5), t_hw_c_meta, 8);
	memcpy(t_cbody(&t, 3, 5), t_hw_c_body, sizeof(t_hw_c_body));

	ave_cmp420_unpack(&t.g, t.surf, t.out, t.out + t.luma, t.stride, &ys, &cs);
	KUNIT_EXPECT_EQ(test, cs.coded, 8U);
	KUNIT_EXPECT_EQ(test, cs.other, 0U);
	KUNIT_EXPECT_EQ(test, cs.solid, t.g.tiles_x * t.g.tiles_y * 8 - 8);

	ave_cmp420_pack(&t.g, again, t.out, t.out + t.luma, t.stride);
	c = again + t.g.c + 5 * t.g.c_pitch + 3 * 512;
	for (u32 i = 0; i < 8; i++)
		KUNIT_EXPECT_MEMEQ(test, c + 64 * i, t_hw_c_pairs[i], 64);
}

static void ave_cmp420_bounds(struct kunit *test)
{
	static const u32 sizes[][2] = {
		{ 2, 2 }, { 30, 2 }, { 34, 34 }, { 66, 18 }, { 98, 66 },
		{ 126, 130 }, { 1922, 1082 }, { 1920, 1080 },
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(sizes); i++) {
		u32 w = sizes[i][0], h = sizes[i][1];
		struct ave_cmp420_stats ys, cs;
		struct t_pic t;
		u16 hist[256];

		t_round_trip(test, w, h, w);
		t_pic_init(test, &t, w, h, w);
		for (unsigned int r = 0; r < 4; r++) {
			u32 x = 0x9e3779b9 * (r + 1) + i;

			for (u32 k = 0; k < t.g.bytes; k++) {
				x ^= x << 13;
				x ^= x >> 17;
				x ^= x << 5;
				t.surf[k] = r == 3 ? (u8)(x & 1 ? 0x1f : 0x60) : x;
			}
			ave_cmp420_unpack(&t.g, t.surf, t.out, t.out + t.luma,
					  t.stride, &ys, &cs);
			KUNIT_EXPECT_EQ(test, ys.raw + ys.solid + ys.coded + ys.other,
					t.g.tiles_x * t.g.tiles_y * 32);
			KUNIT_EXPECT_EQ(test, cs.raw + cs.solid + cs.coded + cs.other,
					t.g.tiles_x * t.g.tiles_y * 8);
			KUNIT_EXPECT_EQ(test, ys.tiles_raw + ys.tiles_solid +
					      ys.tiles_mixed + ys.tiles_other,
					t.g.tiles_x * t.g.tiles_y);
			ave_cmp420_meta_hist(&t.g, t.surf, false, hist);
			ave_cmp420_meta_hist(&t.g, t.surf, true, hist);
		}
	}
}

static struct kunit_case ave_cmp420_cases[] = {
	KUNIT_CASE(ave_cmp420_geom_1080p),
	KUNIT_CASE(ave_cmp420_pack_layout),
	KUNIT_CASE(ave_cmp420_round_trip),
	KUNIT_CASE(ave_cmp420_hw_blocks),
	KUNIT_CASE(ave_cmp420_hw_cbcr),
	KUNIT_CASE(ave_cmp420_bounds),
	{}
};

static struct kunit_suite ave_cmp420_suite = {
	.name = "apple-ave-cmp420",
	.test_cases = ave_cmp420_cases,
};
kunit_test_suite(ave_cmp420_suite);

MODULE_DESCRIPTION("Apple AVE compressed 4:2:0 surface tests");
MODULE_LICENSE("GPL");
