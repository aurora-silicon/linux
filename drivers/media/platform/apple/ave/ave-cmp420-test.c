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

static u8 *t_ymeta(struct t_pic *t, u32 tx, u32 ty)
{
	return t->surf + t->g.y_meta + ty * t->g.y_meta_pitch + tx * 32;
}

static u8 *t_cbody(struct t_pic *t, u32 tx, u32 ty)
{
	return t->surf + t->g.c + ty * t->g.c_pitch + tx * 512;
}

static u8 *t_cmeta(struct t_pic *t, u32 tx, u32 ty)
{
	return t->surf + t->g.c_meta + ty * t->g.c_meta_pitch + tx * 8;
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

	/* Every picture tile raw, all padding metadata zero */
	for (u32 i = t.g.y_meta; i < t.g.c; i++) {
		u32 row = (i - t.g.y_meta) / t.g.y_meta_pitch;
		u32 col = (i - t.g.y_meta) % t.g.y_meta_pitch / 32;
		bool vis = row < 34 && col < 60;

		KUNIT_ASSERT_EQ(test, t.surf[i], vis ? 0x1f : 0);
		raw_y += vis;
	}
	for (u32 i = t.g.c_meta; i < t.g.bytes; i++) {
		u32 row = (i - t.g.c_meta) / t.g.c_meta_pitch;
		u32 col = (i - t.g.c_meta) % t.g.c_meta_pitch / 8;
		bool vis = row < 34 && col < 60;

		KUNIT_ASSERT_EQ(test, t.surf[i], vis ? 0x3f : 0);
		raw_c += vis;
	}
	/* The firmware's own all-zero run wrote exactly these many */
	KUNIT_EXPECT_EQ(test, raw_y, 65280);
	KUNIT_EXPECT_EQ(test, raw_c, 16320);
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
static void ave_cmp420_solid(struct kunit *test)
{
	struct ave_cmp420_stats ys, cs;
	struct t_pic t;
	u8 *y, *c;

	t_pic_init(test, &t, 64, 32, 64);
	memset(t.surf, 0, t.g.bytes);
	/*
	 * Tile 0: all solid, one byte (pair) per block, packed back to back;
	 * a different value per compression tile.
	 */
	for (u32 q = 0; q < 4; q++) {
		memset(t_ymeta(&t, 0, 0) + q * 8, 0x60, 8);
		memset(t_ybody(&t, 0, 0) + q * 256, 0x40 + q, 8);
	}
	memset(t_cmeta(&t, 0, 0), 0x01, 8);
	for (u32 i = 0; i < 8; i++) {
		t_cbody(&t, 0, 0)[2 * i] = 0x70;
		t_cbody(&t, 0, 0)[2 * i + 1] = 0x90;
	}
	/*
	 * Tile 1: in each compression tile solid, raw, then raw: block 1 starts
	 * one byte in, block 2 at 33. CbCr: raw, solid, solid, raw...
	 */
	y = t_ybody(&t, 1, 0);
	for (u32 q = 0; q < 4; q++) {
		u8 *m = t_ymeta(&t, 1, 0) + q * 8;

		memset(m, 0x1f, 8);
		m[0] = 0x60;
		y[q * 256] = 0x20;
		for (u32 k = 0; k < 7 * 32; k++)
			y[q * 256 + 1 + k] = k ^ q;
	}
	c = t_cbody(&t, 1, 0);
	memcpy(t_cmeta(&t, 1, 0), "\x3f\x01\x01\x3f\x3f\x3f\x3f\x3f", 8);
	for (u32 k = 0; k < 64; k++)
		c[k] = 0x80 + k;
	c[64] = 0x11, c[65] = 0x22, c[66] = 0x33, c[67] = 0x44;
	for (u32 k = 0; k < 5 * 64; k++)
		c[68 + k] = k * 3;

	ave_cmp420_unpack(&t.g, t.surf, t.out, t.out + t.luma, t.stride, &ys, &cs);

	for (u32 py = 0; py < 32; py++) {
		for (u32 px = 0; px < 32; px++) {
			u32 q = (px >= 16) * 2 + (py >= 16);
			u32 k = t_morton(px % 16, py % 16);
			u8 want = k < 32 ? 0x20 : (k - 32) ^ q;

			KUNIT_ASSERT_EQ(test, t.out[py * 64 + px], 0x40 + q);
			KUNIT_ASSERT_EQ_MSG(test, t.out[py * 64 + 32 + px], want,
					    "mixed luma at %u,%u", px, py);
		}
	}
	for (u32 py = 0; py < 16; py++) {
		for (u32 px = 0; px < 16; px++) {
			u8 *o = t.out + t.luma + py * 64 + 2 * px;
			u32 k = t_morton(px, py), b = k / 32;
			u8 cb, cr;

			KUNIT_ASSERT_EQ(test, o[0], 0x70);
			KUNIT_ASSERT_EQ(test, o[1], 0x90);
			if (b == 0) {
				cb = 0x80 + 2 * k, cr = 0x81 + 2 * k;
			} else if (b == 1) {
				cb = 0x11, cr = 0x22;
			} else if (b == 2) {
				cb = 0x33, cr = 0x44;
			} else {
				cb = (2 * (k - 96)) * 3;
				cr = (2 * (k - 96) + 1) * 3;
			}
			KUNIT_ASSERT_EQ_MSG(test, o[32], cb, "mixed Cb at %u,%u", px, py);
			KUNIT_ASSERT_EQ_MSG(test, o[33], cr, "mixed Cr at %u,%u", px, py);
		}
	}
	KUNIT_EXPECT_EQ(test, ys.tiles_solid, 1);
	KUNIT_EXPECT_EQ(test, ys.tiles_mixed, 1);
	KUNIT_EXPECT_EQ(test, ys.solid, 32 + 4);
	KUNIT_EXPECT_EQ(test, ys.raw, 28);
	KUNIT_EXPECT_EQ(test, ys.other, 0);
	KUNIT_EXPECT_EQ(test, cs.tiles_solid, 1);
	KUNIT_EXPECT_EQ(test, cs.tiles_mixed, 1);
	KUNIT_EXPECT_EQ(test, cs.solid, 10);
	KUNIT_EXPECT_EQ(test, cs.raw, 6);
}

/* Undecoded modes: counted, marker filled, later blocks only where sized */
static void ave_cmp420_other(struct kunit *test)
{
	struct ave_cmp420_stats ys, cs;
	struct ave_cmp420_geom *g;
	struct t_pic t;
	u8 *m, *y;
	u16 hist[256];

	t_pic_init(test, &t, 32, 32, 32);
	g = &t.g;
	t_fill(&t);
	ave_cmp420_pack(g, t.surf, t.in, t.in + t.luma, t.stride);
	m = t_ymeta(&t, 0, 0);
	y = t_ybody(&t, 0, 0);
	/*
	 * TL: a sized mode-0 block of 4 bytes, then raw blocks four bytes on.
	 * BL: all zero (no data). TR: mode 7 then raw: unknown from there.
	 * BR: untouched raw.
	 */
	memset(m, 0x1f, 8);
	m[0] = 0x03;
	memmove(y + 4, y + 32, 7 * 32);
	memset(m + 8, 0, 8);
	m[16] = 0xe0;
	/* CbCr: block 2 unknown, so blocks 2..7 are lost */
	t_cmeta(&t, 0, 0)[2] = 0x55;

	ave_cmp420_unpack(g, t.surf, t.out, t.out + t.luma, t.stride, &ys, &cs);

	for (u32 py = 0; py < 32; py++) {
		for (u32 px = 0; px < 32; px++) {
			u32 q = (px >= 16) * 2 + (py >= 16);
			u32 k = t_morton(px % 16, py % 16);
			u8 got = t.out[py * 32 + px], in = t.in[py * 32 + px];

			if ((q == 0 && k < 32) || q == 1 || q == 2)
				KUNIT_ASSERT_EQ(test, got, 0xff);
			else
				KUNIT_ASSERT_EQ_MSG(test, got, in, "luma %u,%u", px, py);
		}
	}
	for (u32 py = 0; py < 16; py++) {
		for (u32 px = 0; px < 16; px++) {
			const u8 *o = t.out + t.luma + py * 32 + 2 * px;
			const u8 *i = t.in + t.luma + py * 32 + 2 * px;

			if (t_morton(px, py) < 64) {
				KUNIT_ASSERT_EQ(test, o[0], i[0]);
				KUNIT_ASSERT_EQ(test, o[1], i[1]);
			} else {
				KUNIT_ASSERT_EQ(test, o[0], 0x00);
				KUNIT_ASSERT_EQ(test, o[1], 0xff);
			}
		}
	}
	KUNIT_EXPECT_EQ(test, ys.other, 1 + 8 + 8);
	KUNIT_EXPECT_EQ(test, ys.raw, 7 + 8);
	KUNIT_EXPECT_EQ(test, ys.tiles_other, 1);
	KUNIT_EXPECT_EQ(test, cs.other, 6);
	KUNIT_EXPECT_EQ(test, cs.raw, 2);
	KUNIT_EXPECT_EQ(test, cs.tiles_other, 1);

	ave_cmp420_meta_hist(g, t.surf, false, hist);
	KUNIT_EXPECT_EQ(test, hist[0x1f], 7 + 7 + 8);
	KUNIT_EXPECT_EQ(test, hist[0x03], 1);
	KUNIT_EXPECT_EQ(test, hist[0x00], 8);
	KUNIT_EXPECT_EQ(test, hist[0xe0], 1);
	ave_cmp420_meta_hist(g, t.surf, true, hist);
	KUNIT_EXPECT_EQ(test, hist[0x3f], 7);
	KUNIT_EXPECT_EQ(test, hist[0x55], 1);
}

static struct kunit_case ave_cmp420_cases[] = {
	KUNIT_CASE(ave_cmp420_geom_1080p),
	KUNIT_CASE(ave_cmp420_pack_layout),
	KUNIT_CASE(ave_cmp420_round_trip),
	KUNIT_CASE(ave_cmp420_solid),
	KUNIT_CASE(ave_cmp420_other),
	{}
};

static struct kunit_suite ave_cmp420_suite = {
	.name = "apple-ave-cmp420",
	.test_cases = ave_cmp420_cases,
};
kunit_test_suite(ave_cmp420_suite);

MODULE_DESCRIPTION("Apple AVE compressed 4:2:0 surface tests");
MODULE_LICENSE("GPL");
