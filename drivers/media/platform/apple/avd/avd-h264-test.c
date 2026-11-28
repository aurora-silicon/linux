// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the H.264 slice checks and Annex-B parser of the Apple
 * video decoder.
 */

#include <kunit/test.h>
#include <linux/sizes.h>
#include <media/v4l2-common.h>
#include <media/v4l2-h264.h>

#include "avd.h"
#include "avd-h264-parse.h"

struct avd_slice_data_case {
	const char *desc;
	const u8 *data;
	u32 len;
	u32 header_len;
	bool found;
	u32 off;
};

static const u8 avd_test_slice[] = {
	0x25, 0xb8, 0x00, 0x00, 0x04, 0x01, 0xaa, 0xbb,
};

/* The same slice with an emulation prevention byte in its header. */
static const u8 avd_test_slice_epb[] = {
	0x25, 0xb8, 0x00, 0x00, 0x03, 0x01, 0xaa, 0xbb,
};

static const struct avd_slice_data_case avd_slice_data_cases[] = {
	{ "empty payload", avd_test_slice, 0, 0 },
	{ "one-byte payload", avd_test_slice, 1, 0 },
	{ "two-byte payload", avd_test_slice, 2, 0 },
	{ "one byte of data", avd_test_slice, 3, 2, true, 2 },
	{ "header ends one byte early", avd_test_slice, 8, 7, true, 7 },
	{ "header ends at the end", avd_test_slice, 8, 8 },
	{ "header beyond the end", avd_test_slice, 8, 12 },
	{ "header without emulation prevention", avd_test_slice, 8, 5, true, 5 },
	{ "emulation prevention in the header", avd_test_slice_epb, 8, 5, true, 6 },
};

KUNIT_ARRAY_PARAM_DESC(avd_slice_data, avd_slice_data_cases, desc);

static void avd_slice_data_test(struct kunit *test)
{
	const struct avd_slice_data_case *c = test->param_value;
	u32 off = U32_MAX;

	KUNIT_EXPECT_EQ(test, avd_h264_find_slice_data(c->data, c->len,
						       c->header_len, &off),
			c->found);
	if (c->found)
		KUNIT_EXPECT_EQ(test, off, c->off);
	else
		KUNIT_EXPECT_EQ(test, off, U32_MAX);
}

/* The size of the slice table in avd-h264.c */
#define AVD_TEST_MAX_SLICES	4096

struct avd_slice_fits_case {
	const char *desc;
	u16 width_mbs;
	u16 height_map_units;
	bool fields;
	size_t held;
	size_t held_bytes;
	u32 len;
	size_t decoded_size;
	bool fits;
};

static const struct avd_slice_fits_case avd_slice_fits_cases[] = {
	{ "one-macroblock frame, first slice", 1, 1, false, 0, 0, 16, 384, true },
	{ "one-macroblock frame, second slice", 1, 1, false, 1, PAGE_SIZE, 16, 384 },
	{ "one-macroblock field pair, second slice", 1, 1, true, 1, PAGE_SIZE, 16, 384, true },
	{ "one-macroblock field pair, third slice", 1, 1, true, 2, 2 * PAGE_SIZE, 16, 384 },
	{ "320x240, last slice", 20, 15, false, 299, 299 * PAGE_SIZE, 16, 115200, true },
	{ "320x240, one slice over", 20, 15, false, 300, 300 * PAGE_SIZE, 16, 115200 },
	{ "1920x1088, last slice of the table", 120, 68, false,
	  AVD_TEST_MAX_SLICES - 1, 0, 16, 3133440, true },
	{ "1920x1088, one slice over the table", 120, 68, false,
	  AVD_TEST_MAX_SLICES, 0, 16, 3133440 },
	{ "data up to the budget", 120, 68, false, 1, SZ_512M - PAGE_SIZE,
	  PAGE_SIZE, SZ_256M, true },
	{ "data one byte over the budget", 120, 68, false, 1,
	  SZ_512M - PAGE_SIZE, PAGE_SIZE + 1, SZ_256M },
	{ "no decoded size, data up to the page budget", 120, 68, false, 1,
	  (AVD_TEST_MAX_SLICES - 1) * PAGE_SIZE, 1, 0, true },
	{ "no decoded size, data over the page budget", 120, 68, false, 1,
	  AVD_TEST_MAX_SLICES * PAGE_SIZE, 1, 0 },
	{ "slice of almost 4 GiB", 120, 68, false, 0, 0, U32_MAX, SZ_256M },
};

KUNIT_ARRAY_PARAM_DESC(avd_slice_fits, avd_slice_fits_cases, desc);

static void avd_slice_fits_test(struct kunit *test)
{
	const struct avd_slice_fits_case *c = test->param_value;
	struct v4l2_ctrl_h264_sps sps = {
		.pic_width_in_mbs_minus1 = c->width_mbs - 1,
		.pic_height_in_map_units_minus1 = c->height_map_units - 1,
		.flags = c->fields ? 0 : V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY,
	};

	KUNIT_EXPECT_EQ(test, avd_h264_slice_fits(&sps, c->held, c->held_bytes,
						  c->len, c->decoded_size),
			c->fits);
}

/* The H.264 OUTPUT frame size limits in avd-v4l2.c */
static const struct v4l2_frmsize_stepwise avd_test_h264_frmsize = {
	.min_width = 64, .max_width = 4096, .step_width = 64,
	.min_height = 64, .max_height = 4096, .step_height = 16,
};

struct avd_sps_size_case {
	const char *desc;
	/* OUTPUT format as the client asks for it */
	u32 width;
	u32 height;
	u16 width_mbs;
	u16 height_map_units;
	int ret;
};

static const struct avd_sps_size_case avd_sps_size_cases[] = {
	{ "1080p, OUTPUT 1920x1080", 1920, 1080, 120, 68, 0 },
	{ "1080p, OUTPUT 1920x1088", 1920, 1088, 120, 68, 0 },
	{ "720p", 1280, 720, 80, 45, 0 },
	{ "720p, one row over", 1280, 720, 80, 46, -EINVAL },
	{ "720p, one column over", 1280, 720, 81, 45, -EINVAL },
	{ "picture smaller than the format", 1920, 1088, 40, 30, 0 },
	{ "64x64, matching picture", 64, 64, 4, 4, 0 },
	{ "64x64, 4096x4096 picture", 64, 64, 256, 256, -EINVAL },
	{ "64x64, largest SPS size", 64, 64, U16_MAX, U16_MAX, -EINVAL },
};

KUNIT_ARRAY_PARAM_DESC(avd_sps_size, avd_sps_size_cases, desc);

static void avd_sps_size_test(struct kunit *test)
{
	const struct avd_sps_size_case *c = test->param_value;
	struct v4l2_ctrl_h264_sps sps = {
		.chroma_format_idc = 1,
		.pic_width_in_mbs_minus1 = c->width_mbs - 1,
		.pic_height_in_map_units_minus1 = c->height_map_units - 1,
		.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY,
	};
	struct avd_ctx *ctx;
	u32 w = c->width, h = c->height;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	/* As S_FMT(OUTPUT) does */
	v4l2_apply_frmsize_constraints(&w, &h, &avd_test_h264_frmsize);
	ctx->coded_fmt.fmt.pix_mp.width = w;
	ctx->coded_fmt.fmt.pix_mp.height = h;

	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps(ctx, &sps), c->ret);
}

/*
 * Pre-streaming format negotiation (e.g. Chromium V4L2VideoDecoder::SetExtCtrlsInit)
 * supplies chroma_format_idc and bit depth with flags == 0 and zeroed dimensions
 * before the first frame request carries the parsed SPS. Format validation accepts
 * those controls while full SPS validation still rejects interlaced streams.
 */
static void avd_sps_fmt_init_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_sps sps = {
		.chroma_format_idc = 1,
	};
	struct avd_dev *dev;
	struct avd_ctx *ctx;

	dev = kunit_kzalloc(test, sizeof(*dev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->dev = dev;
	ctx->coded_fmt.fmt.pix_mp.width = 1280;
	ctx->coded_fmt.fmt.pix_mp.height = 720;

	/* 8-bit and 10-bit 4:2:0 pre-streaming init controls */
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_fmt(ctx, &sps), 0);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, false), 0);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, true), -EINVAL);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps(ctx, &sps), -EINVAL);

	sps.bit_depth_luma_minus8 = 2;
	sps.bit_depth_chroma_minus8 = 2;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_fmt(ctx, &sps), 0);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, false), 0);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, true), -EINVAL);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps(ctx, &sps), -EINVAL);

	/* Populated interlaced SPS must still be rejected pre-streaming */
	sps = (struct v4l2_ctrl_h264_sps) {
		.chroma_format_idc = 1,
		.pic_width_in_mbs_minus1 = 79,
		.pic_height_in_map_units_minus1 = 22,
	};
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, false), -EINVAL);
	sps.pic_width_in_mbs_minus1 = 0;
	sps.pic_height_in_map_units_minus1 = 0;
	sps.flags = V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, false), -EINVAL);

	/* Unsupported formats remain rejected pre-streaming */
	sps = (struct v4l2_ctrl_h264_sps) {
		.chroma_format_idc = 3,
		.bit_depth_luma_minus8 = 2,
		.bit_depth_chroma_minus8 = 2,
	};
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_fmt(ctx, &sps), -EINVAL);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_ctrl(ctx, &sps, false), -EINVAL);
	sps.chroma_format_idc = 2;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_fmt(ctx, &sps), -EINVAL);
	sps.chroma_format_idc = 1;
	sps.bit_depth_luma_minus8 = 4;
	sps.bit_depth_chroma_minus8 = 4;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_fmt(ctx, &sps), -EINVAL);
	sps.bit_depth_luma_minus8 = 2;
	sps.bit_depth_chroma_minus8 = 0;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_fmt(ctx, &sps), -EINVAL);
}

static void avd_sps_range_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_sps sps = {
		.chroma_format_idc = 1,
		.pic_width_in_mbs_minus1 = 3,
		.pic_height_in_map_units_minus1 = 3,
		.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY,
	};
	struct avd_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->coded_fmt.fmt.pix_mp.width = 64;
	ctx->coded_fmt.fmt.pix_mp.height = 64;

	sps.log2_max_frame_num_minus4 = 12;
	sps.pic_order_cnt_type = 0;
	sps.log2_max_pic_order_cnt_lsb_minus4 = 12;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps(ctx, &sps), 0);

	sps.log2_max_frame_num_minus4 = 13;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps(ctx, &sps), -EINVAL);
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps_fmt(ctx, &sps), 0);

	sps.log2_max_frame_num_minus4 = 0;
	sps.pic_order_cnt_type = 3;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps(ctx, &sps), -EINVAL);

	sps.pic_order_cnt_type = 0;
	sps.log2_max_pic_order_cnt_lsb_minus4 = 13;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps(ctx, &sps), -EINVAL);

	/* log2_max_pic_order_cnt_lsb_minus4 is ignored when poc_type != 0 */
	sps.pic_order_cnt_type = 1;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps(ctx, &sps), 0);
	sps.pic_order_cnt_type = 2;
	KUNIT_EXPECT_EQ(test, avd_h264_validate_sps(ctx, &sps), 0);
}

static void avd_start_slices_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_sps sps = {
		.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY,
	};

	/* One slice per row */
	sps.pic_width_in_mbs_minus1 = 119;
	sps.pic_height_in_map_units_minus1 = 67;
	KUNIT_EXPECT_EQ(test, avd_h264_start_slices(&sps), 68);

	/* No more rows than macroblocks, and never more than the table */
	sps.pic_width_in_mbs_minus1 = 0;
	sps.pic_height_in_map_units_minus1 = U16_MAX;
	KUNIT_EXPECT_LE(test, avd_h264_start_slices(&sps), AVD_TEST_MAX_SLICES);
	sps.pic_width_in_mbs_minus1 = U16_MAX;
	KUNIT_EXPECT_LE(test, avd_h264_start_slices(&sps), AVD_TEST_MAX_SLICES);
	sps.flags = 0;
	KUNIT_EXPECT_LE(test, avd_h264_start_slices(&sps), AVD_TEST_MAX_SLICES);
}

static void avd_scaling_flat_test(struct kunit *test)
{
	struct v4l2_ctrl_h264_pps pps = {};
	struct v4l2_ctrl_h264_scaling_matrix *m;

	m = kunit_kzalloc(test, sizeof(*m), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, m);
	memset(m->scaling_list_4x4, 16, sizeof(m->scaling_list_4x4));
	memset(m->scaling_list_8x8, 16, sizeof(m->scaling_list_8x8));
	KUNIT_EXPECT_TRUE(test, avd_h264_scaling_is_flat(&pps, m));

	/* The 8x8 lists only count with transform_8x8_mode. */
	m->scaling_list_8x8[1][63] = 17;
	KUNIT_EXPECT_TRUE(test, avd_h264_scaling_is_flat(&pps, m));
	pps.flags = V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE;
	KUNIT_EXPECT_FALSE(test, avd_h264_scaling_is_flat(&pps, m));

	m->scaling_list_8x8[1][63] = 16;
	KUNIT_EXPECT_TRUE(test, avd_h264_scaling_is_flat(&pps, m));
	m->scaling_list_4x4[5][15] = 6;
	KUNIT_EXPECT_FALSE(test, avd_h264_scaling_is_flat(&pps, m));
}

/* Simple bit writer for synthesizing H.264 slice header bitstreams in tests. */
struct avd_test_bw {
	u8 buf[256];
	u32 pos;
	u8 bit;
	u8 zeros;
	bool epb;
};

static void bw_init(struct avd_test_bw *bw, u8 nal_hdr, bool epb)
{
	memset(bw, 0, sizeof(*bw));
	bw->buf[0] = nal_hdr;
	bw->pos = 1;
	bw->epb = epb;
}

static void bw_put_byte(struct avd_test_bw *bw, u8 b)
{
	if (bw->epb && bw->zeros >= 2 && b <= 0x03) {
		bw->buf[bw->pos++] = 0x03;
		bw->zeros = 0;
	}
	bw->buf[bw->pos++] = b;
	bw->zeros = b == 0x00 ? min_t(u8, bw->zeros + 1, 2) : 0;
}

static void bw_put_bit(struct avd_test_bw *bw, u32 v)
{
	if (v & 1)
		bw->buf[bw->pos] |= 1U << (7 - bw->bit);
	bw->bit++;
	if (bw->bit == 8) {
		u8 finished = bw->buf[bw->pos];

		bw->buf[bw->pos] = 0;
		bw->bit = 0;
		bw_put_byte(bw, finished);
	}
}

static void bw_put_bits(struct avd_test_bw *bw, u32 v, unsigned int n)
{
	while (n--)
		bw_put_bit(bw, (v >> n) & 1);
}

static void bw_put_ue(struct avd_test_bw *bw, u32 v)
{
	u32 code = v + 1;
	unsigned int bits = fls(code);
	unsigned int zeros = bits - 1;

	bw_put_bits(bw, 0, zeros);
	bw_put_bits(bw, code, bits);
}

static void bw_put_se(struct avd_test_bw *bw, s32 v)
{
	u32 code = v <= 0 ? (u32)(-2 * v) : (u32)(2 * v - 1);

	bw_put_ue(bw, code);
}

static u32 bw_finish(struct avd_test_bw *bw, bool cabac, u8 tail_byte)
{
	if (cabac) {
		while (bw->bit != 0)
			bw_put_bit(bw, 1);
	} else if (bw->bit != 0) {
		u8 finished = bw->buf[bw->pos] | ((1U << (8 - bw->bit)) - 1);

		bw->buf[bw->pos] = 0;
		bw->bit = 0;
		bw_put_byte(bw, finished);
	}
	bw_put_byte(bw, tail_byte);
	return bw->pos;
}

static void avd_annexb_scanner_test(struct kunit *test)
{
	/* AUD(9), SPS(7), PPS(8), SEI(6), EOS(10/11), Filler(12), then 2 slices */
	static const u8 multi[] = {
		0x00, 0x00, 0x00, 0x00, 0x01, 0x09, 0x10,
		0x00, 0x00, 0x00, 0x01, 0x67, 0x4d, 0x40,
		0x00, 0x00, 0x01, 0x68, 0xee,
		0x00, 0x00, 0x01, 0x06, 0x05, 0x80,
		0x00, 0x00, 0x01, 0x0a,
		0x00, 0x00, 0x01, 0x0b,
		0x00, 0x00, 0x01, 0x0c, 0xff, 0x80,
		0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x20, 0x00, 0x00,
	};
	struct avd_h264_nalu nalu = {};
	u32 cursor = 0;

	KUNIT_EXPECT_EQ(test, avd_h264_next_nalu(multi, sizeof(multi),
						 &cursor, &nalu), 1);
	KUNIT_EXPECT_EQ(test, nalu.nal_unit_type, 5);
	KUNIT_EXPECT_EQ(test, nalu.nal_ref_idc, 3);
	KUNIT_EXPECT_EQ(test, nalu.len, 3);
	KUNIT_EXPECT_EQ(test, multi[nalu.off], 0x65);
	KUNIT_EXPECT_EQ(test, multi[nalu.off + nalu.len - 1], 0x84);

	KUNIT_EXPECT_EQ(test, avd_h264_next_nalu(multi, sizeof(multi),
						 &cursor, &nalu), 1);
	KUNIT_EXPECT_EQ(test, nalu.nal_unit_type, 1);
	KUNIT_EXPECT_EQ(test, nalu.nal_ref_idc, 2);
	KUNIT_EXPECT_EQ(test, nalu.len, 3);
	KUNIT_EXPECT_EQ(test, multi[nalu.off], 0x41);
	KUNIT_EXPECT_EQ(test, multi[nalu.off + nalu.len - 1], 0x20);

	KUNIT_EXPECT_EQ(test, avd_h264_next_nalu(multi, sizeof(multi),
						 &cursor, &nalu), 0);

	/* Rejected cases: forbidden_zero_bit, DPA/B/C (2,3,4), SVC/MVC (14,20) */
	static const u8 bad_hdrs[] = { 0xe5, 0x89, 0x22, 0x23, 0x24, 0x2e, 0x34, 0x00 };

	for (unsigned int i = 0; i < ARRAY_SIZE(bad_hdrs); i++) {
		u8 buf[] = { 0x00, 0x00, 0x01, bad_hdrs[i], 0x80 };

		cursor = 0;
		KUNIT_EXPECT_EQ(test, avd_h264_next_nalu(buf, sizeof(buf),
							 &cursor, &nalu), -EINVAL);
	}

	/* Missing start code, truncated start code, empty NALU between start codes */
	{
		static const u8 no_sc[] = { 0x65, 0x88, 0x84 };
		static const u8 trunc_sc[] = { 0x00, 0x00, 0x01 };
		static const u8 empty_nal[] = { 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x65, 0x88 };
		static const u8 only_non_vcl[] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0x10 };

		cursor = 0;
		KUNIT_EXPECT_EQ(test, avd_h264_next_nalu(no_sc, sizeof(no_sc),
							 &cursor, &nalu), -EINVAL);
		cursor = 0;
		KUNIT_EXPECT_EQ(test, avd_h264_next_nalu(trunc_sc, sizeof(trunc_sc),
							 &cursor, &nalu), -EINVAL);
		cursor = 0;
		KUNIT_EXPECT_EQ(test, avd_h264_next_nalu(empty_nal, sizeof(empty_nal),
							 &cursor, &nalu), -EINVAL);
		cursor = 0;
		KUNIT_EXPECT_EQ(test, avd_h264_next_nalu(only_non_vcl, sizeof(only_non_vcl),
							 &cursor, &nalu), 0);
	}
}

static void avd_bitreader_epb_test(struct kunit *test)
{
	/*
	 * EBSP: 00 00 03 00 00 03 03 00 00 03 01
	 * RBSP: 00 00    00 00    03 00 00    01
	 */
	static const u8 ebsp[] = {
		0x65, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x03, 0x00, 0x00, 0x03, 0x01,
	};
	struct avd_h264_br br;

	avd_h264_br_init(&br, ebsp, sizeof(ebsp), 1);
	KUNIT_EXPECT_EQ(test, avd_h264_br_read_bits(&br, 16), 0x0000);
	KUNIT_EXPECT_EQ(test, avd_h264_br_read_bits(&br, 16), 0x0000);
	KUNIT_EXPECT_EQ(test, avd_h264_br_read_bits(&br, 8), 0x03);
	KUNIT_EXPECT_EQ(test, avd_h264_br_read_bits(&br, 24), 0x000001);
	KUNIT_EXPECT_FALSE(test, br.err);
	/* Reading one more bit triggers truncation error. */
	avd_h264_br_read_bits(&br, 1);
	KUNIT_EXPECT_TRUE(test, br.err);

	/* Exp-Golomb ue(v) and se(v) round-trip */
	{
		struct avd_test_bw bw;
		static const s32 se_vals[] = { 0, 1, -1, 2, -2, 127, -128 };
		u32 len;

		bw_init(&bw, 0x65, true);
		bw_put_ue(&bw, 0);
		bw_put_ue(&bw, 1);
		bw_put_ue(&bw, 2);
		bw_put_ue(&bw, 15);
		bw_put_ue(&bw, 255);
		for (unsigned int i = 0; i < ARRAY_SIZE(se_vals); i++)
			bw_put_se(&bw, se_vals[i]);
		len = bw_finish(&bw, true, 0xaa);

		avd_h264_br_init(&br, bw.buf, len, 1);
		KUNIT_EXPECT_EQ(test, avd_h264_br_read_ue(&br), 0);
		KUNIT_EXPECT_EQ(test, avd_h264_br_read_ue(&br), 1);
		KUNIT_EXPECT_EQ(test, avd_h264_br_read_ue(&br), 2);
		KUNIT_EXPECT_EQ(test, avd_h264_br_read_ue(&br), 15);
		KUNIT_EXPECT_EQ(test, avd_h264_br_read_ue(&br), 255);
		for (unsigned int i = 0; i < ARRAY_SIZE(se_vals); i++)
			KUNIT_EXPECT_EQ(test, avd_h264_br_read_se(&br), se_vals[i]);
		KUNIT_EXPECT_FALSE(test, br.err);
	}

	/* >= 32 leading zeros in ue(v) must error */
	{
		static const u8 bad_ue[] = {
			0x65, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x80,
		};

		avd_h264_br_init(&br, bad_ue, sizeof(bad_ue), 1);
		avd_h264_br_read_ue(&br);
		KUNIT_EXPECT_TRUE(test, br.err);
	}
}

struct avd_parse_test_ctx {
	struct v4l2_ctrl_h264_sps sps;
	struct v4l2_ctrl_h264_pps pps;
	struct v4l2_ctrl_h264_decode_params decode;
	struct v4l2_ctrl_h264_slice_params sl;
	struct v4l2_ctrl_h264_pred_weights weights;
	struct v4l2_h264_reflist_builder builder;
	struct v4l2_h264_reference init_p[V4L2_H264_REF_LIST_LEN];
	struct v4l2_h264_reference init_b0[V4L2_H264_REF_LIST_LEN];
	struct v4l2_h264_reference init_b1[V4L2_H264_REF_LIST_LEN];
	struct avd_test_bw bw;
};

static struct avd_parse_test_ctx *avd_parse_test_alloc(struct kunit *test)
{
	struct avd_parse_test_ctx *t;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	t->sps.pic_width_in_mbs_minus1 = 3;
	t->sps.pic_height_in_map_units_minus1 = 3;
	t->sps.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY;
	return t;
}

static void avd_slice_hdr_epb_boundary_test(struct kunit *test)
{
	struct avd_parse_test_ctx *t = avd_parse_test_alloc(test);
	struct avd_h264_br br;
	u32 data_off, nal_len;
	u8 bit_off;

	t->sps.log2_max_frame_num_minus4 = 12;
	t->sps.pic_order_cnt_type = 0;
	t->sps.log2_max_pic_order_cnt_lsb_minus4 = 12;
	t->pps.flags = V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;

	/* Direct byte-boundary EPB skip test */
	{
		static const u8 boundary_epb[] = { 0x65, 0x00, 0x00, 0x03, 0x01 };
		static const u8 trunc_epb[] = { 0x65, 0x00, 0x00, 0x03 };

		avd_h264_br_init(&br, boundary_epb, sizeof(boundary_epb), 1);
		KUNIT_EXPECT_EQ(test, avd_h264_br_read_bits(&br, 16), 0);
		KUNIT_EXPECT_EQ(test, br.pos, 3);
		KUNIT_EXPECT_EQ(test, br.bit, 0);
		avd_h264_br_skip_epb(&br);
		KUNIT_EXPECT_EQ(test, br.pos, 4);
		KUNIT_EXPECT_EQ(test, br.data[br.pos], 0x01);

		avd_h264_br_init(&br, trunc_epb, sizeof(trunc_epb), 1);
		KUNIT_EXPECT_EQ(test, avd_h264_br_read_bits(&br, 16), 0);
		avd_h264_br_skip_epb(&br);
		KUNIT_EXPECT_EQ(test, br.pos, sizeof(trunc_epb));
	}

	/* Slice header containing EPBs in 16-bit frame_num & poc_lsb (CABAC) */
	bw_init(&t->bw, 0x65, true);
	bw_put_ue(&t->bw, 0);		/* first_mb_in_slice */
	bw_put_ue(&t->bw, 2);		/* I slice */
	bw_put_ue(&t->bw, 0);		/* pps_id */
	bw_put_bits(&t->bw, 0, 16);	/* frame_num = 0 -> creates 00 00 03 */
	bw_put_ue(&t->bw, 0);		/* idr_pic_id */
	bw_put_bits(&t->bw, 0, 16);	/* pic_order_cnt_lsb = 0 -> creates 00 00 03 */
	bw_put_bits(&t->bw, 0, 2);	/* dec_ref_pic_marking */
	bw_put_se(&t->bw, -3);		/* slice_qp_delta */
	nal_len = bw_finish(&t->bw, true, 0xcd);

	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 3, 5,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.slice_type, V4L2_H264_SLICE_TYPE_I);
	KUNIT_EXPECT_EQ(test, t->sl.slice_qp_delta, -3);
	KUNIT_EXPECT_EQ(test, bit_off, 0);
	KUNIT_EXPECT_EQ(test, data_off, nal_len - 1);
	KUNIT_EXPECT_EQ(test, t->bw.buf[data_off], 0xcd);

	/* Same header with CAVLC preserves non-zero bit_off */
	t->pps.flags = 0;
	bw_init(&t->bw, 0x65, true);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 2);
	bw_put_ue(&t->bw, 0);
	bw_put_bits(&t->bw, 0, 16);
	bw_put_ue(&t->bw, 0);
	bw_put_bits(&t->bw, 0, 16);
	bw_put_bits(&t->bw, 0, 2);
	bw_put_se(&t->bw, 0);	/* 1+3+1+16+1+16+2+1 = 41 RBSP bits (bit_off=1) */
	nal_len = bw_finish(&t->bw, false, 0xef);

	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 3, 5,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.header_bit_size, 8 + 41);
	KUNIT_EXPECT_EQ(test, bit_off, 1);
	KUNIT_EXPECT_LT(test, data_off, nal_len);

	/* Header ending right at nal_len with no slice_data must fail */
	KUNIT_EXPECT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, data_off, 3, 5,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			-EINVAL);
}

static void avd_slice_hdr_fields_test(struct kunit *test)
{
	struct avd_parse_test_ctx *t = avd_parse_test_alloc(test);
	u32 data_off, nal_len;
	u8 bit_off;

	t->sps.pic_order_cnt_type = 1;
	t->pps.num_ref_idx_l0_default_active_minus1 = 1;
	t->pps.num_ref_idx_l1_default_active_minus1 = 0;
	t->pps.flags = V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE |
		       V4L2_H264_PPS_FLAG_BOTTOM_FIELD_PIC_ORDER_IN_FRAME_PRESENT |
		       V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT |
		       V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;

	t->init_b0[0] = (struct v4l2_h264_reference){
		.fields = V4L2_H264_FRAME_REF, .index = 2,
	};
	t->init_b0[1] = (struct v4l2_h264_reference){
		.fields = V4L2_H264_FRAME_REF, .index = 5,
	};
	memcpy(t->init_b1, t->init_b0, sizeof(t->init_b1));

	/* Pre-fill output structs with garbage to verify unconditional zeroing. */
	memset(&t->sl, 0x5a, sizeof(t->sl));
	memset(&t->weights, 0x5a, sizeof(t->weights));

	/* Non-IDR B slice (type 6 -> normalized to B=1), POC type 1 + bottom, CABAC */
	bw_init(&t->bw, 0x21, true);
	bw_put_ue(&t->bw, 4);		/* first_mb_in_slice */
	bw_put_ue(&t->bw, 6);		/* slice_type 6 -> B (1) */
	bw_put_ue(&t->bw, 12);		/* pps_id */
	bw_put_bits(&t->bw, 3, 4);	/* frame_num */
	bw_put_se(&t->bw, -4);		/* delta_pic_order_cnt[0] */
	bw_put_se(&t->bw, 2);		/* delta_pic_order_cnt[1] */
	bw_put_ue(&t->bw, 0);		/* redundant_pic_cnt = 0 */
	bw_put_bit(&t->bw, 1);		/* direct_spatial_mv_pred_flag */
	bw_put_bit(&t->bw, 1);		/* num_ref_idx_active_override_flag */
	bw_put_ue(&t->bw, 1);		/* num_ref_idx_l0_active_minus1 */
	bw_put_ue(&t->bw, 0);		/* num_ref_idx_l1_active_minus1 */
	bw_put_bit(&t->bw, 0);		/* ref_pic_list_modification_flag_l0 */
	bw_put_bit(&t->bw, 0);		/* ref_pic_list_modification_flag_l1 */
	bw_put_bit(&t->bw, 0);		/* adaptive_ref_pic_marking_mode_flag */
	bw_put_ue(&t->bw, 2);		/* cabac_init_idc */
	bw_put_se(&t->bw, -5);		/* slice_qp_delta */
	bw_put_ue(&t->bw, 2);		/* disable_deblocking_filter_idc */
	bw_put_se(&t->bw, -6);		/* slice_alpha_c0_offset_div2 */
	bw_put_se(&t->bw, 6);		/* slice_beta_offset_div2 */
	nal_len = bw_finish(&t->bw, true, 0xaa);

	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 1, 1,
						    &t->sps, &t->pps, &t->decode,
						    2, t->init_p, t->init_b0, t->init_b1,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.first_mb_in_slice, 4);
	KUNIT_EXPECT_EQ(test, t->sl.slice_type, V4L2_H264_SLICE_TYPE_B);
	KUNIT_EXPECT_EQ(test, t->sl.flags, V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED);
	KUNIT_EXPECT_EQ(test, t->sl.num_ref_idx_l0_active_minus1, 1);
	KUNIT_EXPECT_EQ(test, t->sl.num_ref_idx_l1_active_minus1, 0);
	KUNIT_EXPECT_EQ(test, t->sl.ref_pic_list0[0].index, 2);
	KUNIT_EXPECT_EQ(test, t->sl.ref_pic_list0[1].index, 5);
	KUNIT_EXPECT_EQ(test, t->sl.ref_pic_list0[2].index, 0xff);
	KUNIT_EXPECT_EQ(test, t->sl.cabac_init_idc, 2);
	KUNIT_EXPECT_EQ(test, t->sl.slice_qp_delta, -5);
	KUNIT_EXPECT_EQ(test, t->sl.disable_deblocking_filter_idc, 2);
	KUNIT_EXPECT_EQ(test, t->sl.slice_alpha_c0_offset_div2, -6);
	KUNIT_EXPECT_EQ(test, t->sl.slice_beta_offset_div2, 6);
	KUNIT_EXPECT_EQ(test, t->weights.luma_log2_weight_denom, 0);
	KUNIT_EXPECT_EQ(test, data_off, nal_len - 1);
	KUNIT_EXPECT_EQ(test, bit_off, 0);

	/* Reject SP/SI slice types (3, 4, 8, 9) and > 9 */
	{
		static const u32 bad_types[] = { 3, 4, 8, 9, 10 };

		for (unsigned int i = 0; i < ARRAY_SIZE(bad_types); i++) {
			bw_init(&t->bw, 0x21, true);
			bw_put_ue(&t->bw, 0);
			bw_put_ue(&t->bw, bad_types[i]);
			bw_put_ue(&t->bw, 0);
			nal_len = bw_finish(&t->bw, true, 0xaa);
			KUNIT_EXPECT_EQ(test,
					avd_h264_parse_slice_header(t->bw.buf, nal_len, 1, 1,
								    &t->sps, &t->pps, &t->decode,
								    0, NULL, NULL, NULL,
								    &t->sl, &t->weights,
								    &data_off, &bit_off),
					-EINVAL);
		}
	}

	/* Reject redundant_pic_cnt > 0 */
	bw_init(&t->bw, 0x01, true);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 2);
	bw_put_ue(&t->bw, 0);
	bw_put_bits(&t->bw, 0, 4);
	bw_put_se(&t->bw, 0);
	bw_put_se(&t->bw, 0);
	bw_put_ue(&t->bw, 1);	/* redundant_pic_cnt > 0 */
	nal_len = bw_finish(&t->bw, true, 0xaa);
	KUNIT_EXPECT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 0, 1,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			-EINVAL);

	/* Reject out-of-range cabac_init_idc (3), num_ref_idx (16), alpha (-7) */
	t->pps.flags = V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE |
		       V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
	t->sps.pic_order_cnt_type = 2;

	/* num_ref_idx_l0_active_minus1 = 16 -> reject */
	bw_init(&t->bw, 0x01, true);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 0);	/* P */
	bw_put_ue(&t->bw, 0);
	bw_put_bits(&t->bw, 1, 4);
	bw_put_bit(&t->bw, 1);	/* override */
	bw_put_ue(&t->bw, 16);	/* > 15 */
	nal_len = bw_finish(&t->bw, true, 0xaa);
	KUNIT_EXPECT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 0, 1,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			-EINVAL);

	/* cabac_init_idc = 3 -> reject */
	bw_init(&t->bw, 0x01, true);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 0);	/* P */
	bw_put_ue(&t->bw, 0);
	bw_put_bits(&t->bw, 1, 4);
	bw_put_bit(&t->bw, 0);	/* no override */
	bw_put_bit(&t->bw, 0);	/* no reflist mod */
	bw_put_ue(&t->bw, 3);	/* invalid cabac_init_idc */
	nal_len = bw_finish(&t->bw, true, 0xaa);
	KUNIT_EXPECT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 0, 1,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			-EINVAL);

	/* alpha = -7 -> reject */
	bw_init(&t->bw, 0x01, true);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 2);	/* I */
	bw_put_ue(&t->bw, 0);
	bw_put_bits(&t->bw, 1, 4);
	bw_put_se(&t->bw, 0);
	bw_put_ue(&t->bw, 0);	/* deblock idc = 0 */
	bw_put_se(&t->bw, -7);	/* out of [-6, 6] */
	bw_put_se(&t->bw, 0);
	nal_len = bw_finish(&t->bw, true, 0xaa);
	KUNIT_EXPECT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 0, 1,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			-EINVAL);
}

static void avd_slice_hdr_ref_mod_test(struct kunit *test)
{
	struct avd_parse_test_ctx *t = avd_parse_test_alloc(test);
	u32 data_off, nal_len;
	u8 bit_off;

	t->sps.log2_max_frame_num_minus4 = 0;	/* MaxPicNum = 16 */
	t->sps.pic_order_cnt_type = 2;
	t->pps.num_ref_idx_l0_default_active_minus1 = 3;
	t->pps.num_ref_idx_l1_default_active_minus1 = 1;
	t->pps.flags = V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	t->decode.frame_num = 1;
	t->decode.top_field_order_cnt = 10;
	t->decode.bottom_field_order_cnt = 10;

	/*
	 * DPB with short-term wraparound (frame_num=15 -> PicNum=-1, 14 -> -2,
	 * 0 -> 0) and a long-term reference (long_term_frame_idx=3) at slot 6.
	 * Current frame_num = 1.
	 */
	t->decode.dpb[1] = (struct v4l2_h264_dpb_entry){
		.frame_num = 0, .top_field_order_cnt = 8, .bottom_field_order_cnt = 8,
		.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE,
	};
	t->decode.dpb[3] = (struct v4l2_h264_dpb_entry){
		.frame_num = 15, .top_field_order_cnt = 6, .bottom_field_order_cnt = 6,
		.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE,
	};
	t->decode.dpb[4] = (struct v4l2_h264_dpb_entry){
		.frame_num = 14, .top_field_order_cnt = 12, .bottom_field_order_cnt = 12,
		.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE,
	};
	t->decode.dpb[6] = (struct v4l2_h264_dpb_entry){
		.frame_num = 3, .top_field_order_cnt = 2, .bottom_field_order_cnt = 2,
		.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE |
			 V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM,
	};
	/* Inactive entry that must NOT be matched */
	t->decode.dpb[9] = (struct v4l2_h264_dpb_entry){
		.frame_num = 13,
		.flags = V4L2_H264_DPB_ENTRY_FLAG_VALID,
	};

	v4l2_h264_init_reflist_builder(&t->builder, &t->decode, &t->sps,
				       t->decode.dpb);
	v4l2_h264_build_p_ref_list(&t->builder, t->init_p);
	v4l2_h264_build_b_ref_lists(&t->builder, t->init_b0, t->init_b1);
	/* Initial P list: short-term descending (1, 3, 4), then long-term (6). */
	KUNIT_ASSERT_EQ(test, t->builder.num_valid, 4);
	KUNIT_ASSERT_EQ(test, t->init_p[0].index, 1);
	KUNIT_ASSERT_EQ(test, t->init_p[1].index, 3);
	KUNIT_ASSERT_EQ(test, t->init_p[2].index, 4);
	KUNIT_ASSERT_EQ(test, t->init_p[3].index, 6);

	/*
	 * Modify P L0:
	 *   idc=2, lt=3            -> slot 6 at index 0
	 *   idc=0, diff_minus1=2   -> pred=1 - 3 = -2 -> wrap +16 = 14 -> slot 4 at index 1
	 *   idc=1, diff_minus1=0   -> pred=14 + 1 = 15 -> slot 3 at index 2
	 *   idc=3                  -> end
	 * Expected final L0: [6, 4, 3, 1].
	 */
	bw_init(&t->bw, 0x01, true);
	bw_put_ue(&t->bw, 0);		/* first_mb */
	bw_put_ue(&t->bw, 0);		/* P */
	bw_put_ue(&t->bw, 0);		/* pps_id */
	bw_put_bits(&t->bw, 1, 4);	/* frame_num = 1 */
	bw_put_bit(&t->bw, 0);		/* no num_ref_idx override (4 active) */
	bw_put_bit(&t->bw, 1);		/* ref_pic_list_modification_flag_l0 */
	bw_put_ue(&t->bw, 2);		/* long-term */
	bw_put_ue(&t->bw, 3);		/* lt_pic_num = 3 -> slot 6 */
	bw_put_ue(&t->bw, 0);		/* sub */
	bw_put_ue(&t->bw, 2);		/* diff=3 -> pic_num_no_wrap=14 -> slot 4 */
	bw_put_ue(&t->bw, 1);		/* add */
	bw_put_ue(&t->bw, 0);		/* diff=1 -> pic_num_no_wrap=15 -> slot 3 */
	bw_put_ue(&t->bw, 3);		/* end */
	bw_put_ue(&t->bw, 0);		/* cabac_init_idc */
	bw_put_se(&t->bw, 0);		/* qp_delta */
	nal_len = bw_finish(&t->bw, true, 0xaa);

	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 0, 1,
						    &t->sps, &t->pps, &t->decode,
						    t->builder.num_valid,
						    t->init_p, t->init_b0, t->init_b1,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.ref_pic_list0[0].index, 6);
	KUNIT_EXPECT_EQ(test, t->sl.ref_pic_list0[1].index, 4);
	KUNIT_EXPECT_EQ(test, t->sl.ref_pic_list0[2].index, 3);
	KUNIT_EXPECT_EQ(test, t->sl.ref_pic_list0[3].index, 1);

	/* Referencing an inactive DPB entry (frame_num=13, slot 9) must fail */
	bw_init(&t->bw, 0x01, true);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 0);
	bw_put_bits(&t->bw, 1, 4);
	bw_put_bit(&t->bw, 0);
	bw_put_bit(&t->bw, 1);	/* mod L0 */
	bw_put_ue(&t->bw, 0);	/* sub */
	bw_put_ue(&t->bw, 3);	/* diff=4 -> 1 - 4 + 16 = 13 (inactive!) */
	bw_put_ue(&t->bw, 3);
	bw_put_ue(&t->bw, 0);
	bw_put_se(&t->bw, 0);
	nal_len = bw_finish(&t->bw, true, 0xaa);

	KUNIT_EXPECT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 0, 1,
						    &t->sps, &t->pps, &t->decode,
						    t->builder.num_valid,
						    t->init_p, t->init_b0, t->init_b1,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			-EINVAL);

	/* B slice modifying both L0 and L1 */
	bw_init(&t->bw, 0x01, true);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 1);		/* B */
	bw_put_ue(&t->bw, 0);
	bw_put_bits(&t->bw, 1, 4);
	bw_put_bit(&t->bw, 1);		/* direct_spatial */
	bw_put_bit(&t->bw, 1);		/* override */
	bw_put_ue(&t->bw, 1);		/* L0 active = 2 */
	bw_put_ue(&t->bw, 1);		/* L1 active = 2 */
	bw_put_bit(&t->bw, 1);		/* mod L0 */
	bw_put_ue(&t->bw, 2);		/* long-term 3 -> slot 6 */
	bw_put_ue(&t->bw, 3);
	bw_put_ue(&t->bw, 3);		/* end L0 */
	bw_put_bit(&t->bw, 1);		/* mod L1 */
	bw_put_ue(&t->bw, 0);		/* sub diff=1 -> frame_num=0 -> slot 1 */
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 3);		/* end L1 */
	bw_put_ue(&t->bw, 1);		/* cabac_init_idc */
	bw_put_se(&t->bw, 2);		/* qp_delta */
	nal_len = bw_finish(&t->bw, true, 0xbb);

	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 0, 1,
						    &t->sps, &t->pps, &t->decode,
						    t->builder.num_valid,
						    t->init_p, t->init_b0, t->init_b1,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.ref_pic_list0[0].index, 6);
	KUNIT_EXPECT_EQ(test, t->sl.ref_pic_list1[0].index, 1);
	KUNIT_EXPECT_EQ(test, t->sl.cabac_init_idc, 1);
	KUNIT_EXPECT_EQ(test, t->sl.slice_qp_delta, 2);
}

static void avd_slice_hdr_pred_weights_test(struct kunit *test)
{
	struct avd_parse_test_ctx *t = avd_parse_test_alloc(test);
	u32 data_off, nal_len;
	u8 bit_off;

	t->sps.chroma_format_idc = 1;
	t->sps.pic_order_cnt_type = 2;
	t->pps.num_ref_idx_l0_default_active_minus1 = 1;
	t->pps.num_ref_idx_l1_default_active_minus1 = 0;
	t->pps.weighted_bipred_idc = 1;
	t->pps.flags = V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE |
		       V4L2_H264_PPS_FLAG_WEIGHTED_PRED;

	/* B slice with explicit weights (L0 has 2 refs: explicit + default; L1 has 1 ref) */
	bw_init(&t->bw, 0x01, true);
	bw_put_ue(&t->bw, 0);		/* first_mb */
	bw_put_ue(&t->bw, 1);		/* B */
	bw_put_ue(&t->bw, 0);		/* pps_id */
	bw_put_bits(&t->bw, 2, 4);	/* frame_num */
	bw_put_bit(&t->bw, 1);		/* direct_spatial */
	bw_put_bit(&t->bw, 0);		/* no override (L0=2, L1=1) */
	bw_put_bit(&t->bw, 0);		/* no L0 mod */
	bw_put_bit(&t->bw, 0);		/* no L1 mod */
	/* pred_weight_table() */
	bw_put_ue(&t->bw, 5);		/* luma_log2_weight_denom */
	bw_put_ue(&t->bw, 4);		/* chroma_log2_weight_denom */
	/* L0[0]: explicit luma & chroma */
	bw_put_bit(&t->bw, 1);
	bw_put_se(&t->bw, 40);
	bw_put_se(&t->bw, -10);
	bw_put_bit(&t->bw, 1);
	bw_put_se(&t->bw, 20);
	bw_put_se(&t->bw, 3);
	bw_put_se(&t->bw, -128);
	bw_put_se(&t->bw, 127);
	/* L0[1]: default luma & chroma */
	bw_put_bit(&t->bw, 0);
	bw_put_bit(&t->bw, 0);
	/* L1[0]: explicit luma, default chroma */
	bw_put_bit(&t->bw, 1);
	bw_put_se(&t->bw, 12);
	bw_put_se(&t->bw, 5);
	bw_put_bit(&t->bw, 0);
	bw_put_ue(&t->bw, 0);		/* cabac_init_idc */
	bw_put_se(&t->bw, 0);		/* qp_delta */
	nal_len = bw_finish(&t->bw, true, 0xaa);

	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 0, 1,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->weights.luma_log2_weight_denom, 5);
	KUNIT_EXPECT_EQ(test, t->weights.chroma_log2_weight_denom, 4);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].luma_weight[0], 40);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].luma_offset[0], -10);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].chroma_weight[0][0], 20);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].chroma_offset[0][0], 3);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].chroma_weight[0][1], -128);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].chroma_offset[0][1], 127);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].luma_weight[1], 32);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].luma_offset[1], 0);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].chroma_weight[1][0], 16);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].chroma_weight[1][1], 16);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[1].luma_weight[0], 12);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[1].luma_offset[0], 5);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[1].chroma_weight[0][0], 16);

	/* Monochrome P slice: no chroma denom or chroma weight bits in stream */
	t->sps.chroma_format_idc = 0;
	t->pps.num_ref_idx_l0_default_active_minus1 = 0;
	bw_init(&t->bw, 0x01, true);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 0);		/* P */
	bw_put_ue(&t->bw, 0);
	bw_put_bits(&t->bw, 1, 4);
	bw_put_bit(&t->bw, 0);		/* no override */
	bw_put_bit(&t->bw, 0);		/* no mod */
	bw_put_ue(&t->bw, 3);		/* luma_denom only */
	bw_put_bit(&t->bw, 1);		/* luma_flag */
	bw_put_se(&t->bw, 10);
	bw_put_se(&t->bw, -2);
	bw_put_ue(&t->bw, 0);		/* cabac_init_idc */
	bw_put_se(&t->bw, 1);		/* qp_delta */
	nal_len = bw_finish(&t->bw, true, 0xcc);

	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 0, 1,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->weights.luma_log2_weight_denom, 3);
	KUNIT_EXPECT_EQ(test, t->weights.chroma_log2_weight_denom, 0);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].luma_weight[0], 10);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].luma_offset[0], -2);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].chroma_weight[0][0], 1);
	KUNIT_EXPECT_EQ(test, t->weights.weight_factors[0].chroma_weight[0][1], 1);
	KUNIT_EXPECT_EQ(test, t->sl.slice_qp_delta, 1);

	/* Reject denom > 7 and weight > 127 */
	bw_init(&t->bw, 0x01, true);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 0);
	bw_put_bits(&t->bw, 1, 4);
	bw_put_bit(&t->bw, 0);
	bw_put_bit(&t->bw, 0);
	bw_put_ue(&t->bw, 8);	/* invalid denom */
	nal_len = bw_finish(&t->bw, true, 0xaa);
	KUNIT_EXPECT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 0, 1,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			-EINVAL);

	bw_init(&t->bw, 0x01, true);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 0);
	bw_put_bits(&t->bw, 1, 4);
	bw_put_bit(&t->bw, 0);
	bw_put_bit(&t->bw, 0);
	bw_put_ue(&t->bw, 2);
	bw_put_bit(&t->bw, 1);
	bw_put_se(&t->bw, 128);	/* out of [-128, 127] */
	bw_put_se(&t->bw, 0);
	nal_len = bw_finish(&t->bw, true, 0xaa);
	KUNIT_EXPECT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 0, 1,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			-EINVAL);
}

static void avd_slice_hdr_marking_test(struct kunit *test)
{
	struct avd_parse_test_ctx *t = avd_parse_test_alloc(test);
	u32 data_off, nal_len;
	u8 bit_off;

	t->sps.pic_order_cnt_type = 2;
	t->pps.flags = V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;

	/* Adaptive MMCO with all opcodes 1..6 terminated by 0 */
	bw_init(&t->bw, 0x41, true);
	bw_put_ue(&t->bw, 0);		/* first_mb */
	bw_put_ue(&t->bw, 2);		/* I (non-IDR) */
	bw_put_ue(&t->bw, 0);		/* pps_id */
	bw_put_bits(&t->bw, 5, 4);	/* frame_num */
	bw_put_bit(&t->bw, 1);		/* adaptive_ref_pic_marking_mode_flag */
	bw_put_ue(&t->bw, 1);		/* op 1: diff_minus1 */
	bw_put_ue(&t->bw, 2);
	bw_put_ue(&t->bw, 2);		/* op 2: lt_pic_num */
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 3);		/* op 3: diff_minus1, lt_frame_idx */
	bw_put_ue(&t->bw, 1);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 4);		/* op 4: max_lt_frame_idx_plus1 */
	bw_put_ue(&t->bw, 2);
	bw_put_ue(&t->bw, 5);		/* op 5: no args */
	bw_put_ue(&t->bw, 6);		/* op 6: lt_frame_idx */
	bw_put_ue(&t->bw, 1);
	bw_put_ue(&t->bw, 0);		/* end */
	bw_put_se(&t->bw, -4);		/* slice_qp_delta */
	nal_len = bw_finish(&t->bw, true, 0xdd);

	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 2, 1,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.slice_qp_delta, -4);
	KUNIT_EXPECT_EQ(test, data_off, nal_len - 1);
	KUNIT_EXPECT_EQ(test, t->bw.buf[data_off], 0xdd);

	/* Reject MMCO op > 6 */
	bw_init(&t->bw, 0x41, true);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 2);
	bw_put_ue(&t->bw, 0);
	bw_put_bits(&t->bw, 5, 4);
	bw_put_bit(&t->bw, 1);
	bw_put_ue(&t->bw, 7);	/* invalid MMCO op */
	nal_len = bw_finish(&t->bw, true, 0xaa);

	KUNIT_EXPECT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 2, 1,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			-EINVAL);

	/* Reject > 64 MMCO ops without terminator */
	bw_init(&t->bw, 0x41, true);
	bw_put_ue(&t->bw, 0);
	bw_put_ue(&t->bw, 2);
	bw_put_ue(&t->bw, 0);
	bw_put_bits(&t->bw, 5, 4);
	bw_put_bit(&t->bw, 1);
	for (unsigned int i = 0; i < 65; i++)
		bw_put_ue(&t->bw, 5);
	nal_len = bw_finish(&t->bw, true, 0xaa);

	KUNIT_EXPECT_EQ(test,
			avd_h264_parse_slice_header(t->bw.buf, nal_len, 2, 1,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			-EINVAL);
}

static void avd_slice_hdr_real_annexb_test(struct kunit *test)
{
	/*
	 * Real Annex-B NALUs (64x64 4:2:0) produced by libx264:
	 * 1) Main profile CABAC: AUD + IDR I-slice, P-slice (weighted_pred=1),
	 *    and non-ref B-slice (poc_type=0, deblocking=1).
	 * 2) Baseline profile CAVLC: 2-slice IDR and 2-slice P-frame
	 *    (poc_type=2, deblocking=1).
	 */
	static const u8 cabac_idr[] = {
		0x00, 0x00, 0x00, 0x01, 0x09, 0x10,
		0x00, 0x00, 0x01,
		0x65, 0x88, 0x84, 0x00, 0x87, 0x69, 0xd7, 0xdc,
	};
	static const u8 cabac_p[] = {
		0x00, 0x00, 0x00, 0x01, 0x09, 0x30,
		0x00, 0x00, 0x01,
		0x41, 0x9a, 0x29, 0xb1, 0x09, 0x7f, 0xd1, 0x64,
	};
	static const u8 cabac_b[] = {
		0x00, 0x00, 0x00, 0x01, 0x09, 0x50,
		0x00, 0x00, 0x01,
		0x01, 0x9e, 0x45, 0xe4, 0x2d, 0xff, 0x90, 0x87,
	};
	static const u8 cavlc_2slice_idr[] = {
		0x00, 0x00, 0x01,
		0x65, 0x88, 0x84, 0x08, 0x7c, 0x42, 0x22, 0xb0,
		0x00, 0x00, 0x01,
		0x65, 0x12, 0x22, 0x10, 0x77, 0xf1, 0xff, 0x87,
	};
	static const u8 cavlc_2slice_p[] = {
		0x00, 0x00, 0x00, 0x01,
		0x41, 0x9a, 0x20, 0x21, 0xfc, 0x9c, 0x3e, 0xf7,
		0x00, 0x00, 0x01,
		0x41, 0x12, 0x68, 0x80, 0x87, 0xac, 0x21, 0xf8,
	};
	struct avd_parse_test_ctx *t = avd_parse_test_alloc(test);
	struct avd_h264_nalu nalu;
	u32 cursor, data_off;
	u8 bit_off;

	t->sps.chroma_format_idc = 1;
	t->sps.pic_order_cnt_type = 0;
	t->pps.num_ref_idx_l0_default_active_minus1 = 1;
	t->pps.num_ref_idx_l1_default_active_minus1 = 0;
	t->pps.weighted_bipred_idc = 2;
	t->pps.flags = V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE |
		       V4L2_H264_PPS_FLAG_WEIGHTED_PRED |
		       V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;

	/* Main CABAC IDR */
	cursor = 0;
	KUNIT_ASSERT_EQ(test, avd_h264_next_nalu(cabac_idr, sizeof(cabac_idr),
						 &cursor, &nalu), 1);
	KUNIT_EXPECT_EQ(test, nalu.nal_unit_type, 5);
	KUNIT_EXPECT_EQ(test, nalu.nal_ref_idc, 3);
	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(cabac_idr + nalu.off, nalu.len,
						    nalu.nal_ref_idc, nalu.nal_unit_type,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.first_mb_in_slice, 0);
	KUNIT_EXPECT_EQ(test, t->sl.slice_type, V4L2_H264_SLICE_TYPE_I);
	KUNIT_EXPECT_EQ(test, t->sl.slice_qp_delta, 8);
	KUNIT_EXPECT_EQ(test, t->sl.header_bit_size, 40);
	KUNIT_EXPECT_EQ(test, data_off, 5);
	KUNIT_EXPECT_EQ(test, bit_off, 0);

	/* Main CABAC P-slice (weighted_pred_flag=1) */
	cursor = 0;
	KUNIT_ASSERT_EQ(test, avd_h264_next_nalu(cabac_p, sizeof(cabac_p),
						 &cursor, &nalu), 1);
	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(cabac_p + nalu.off, nalu.len,
						    nalu.nal_ref_idc, nalu.nal_unit_type,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.first_mb_in_slice, 0);
	KUNIT_EXPECT_EQ(test, t->sl.slice_type, V4L2_H264_SLICE_TYPE_P);
	KUNIT_EXPECT_EQ(test, t->sl.num_ref_idx_l0_active_minus1, 0);
	KUNIT_EXPECT_EQ(test, t->sl.cabac_init_idc, 0);
	KUNIT_EXPECT_EQ(test, t->sl.slice_qp_delta, 9);
	KUNIT_EXPECT_EQ(test, t->sl.header_bit_size, 44);
	KUNIT_EXPECT_EQ(test, data_off, 6);
	KUNIT_EXPECT_EQ(test, bit_off, 0);

	/* Main CABAC B-slice (nal_ref_idc=0) */
	cursor = 0;
	KUNIT_ASSERT_EQ(test, avd_h264_next_nalu(cabac_b, sizeof(cabac_b),
						 &cursor, &nalu), 1);
	KUNIT_EXPECT_EQ(test, nalu.nal_ref_idc, 0);
	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(cabac_b + nalu.off, nalu.len,
						    nalu.nal_ref_idc, nalu.nal_unit_type,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.slice_type, V4L2_H264_SLICE_TYPE_B);
	KUNIT_EXPECT_EQ(test, t->sl.flags, V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED);
	KUNIT_EXPECT_EQ(test, t->sl.slice_qp_delta, 11);
	KUNIT_EXPECT_EQ(test, t->sl.header_bit_size, 42);
	KUNIT_EXPECT_EQ(test, data_off, 6);
	KUNIT_EXPECT_EQ(test, bit_off, 0);

	/* Baseline CAVLC 2-slice IDR */
	t->sps.pic_order_cnt_type = 2;
	t->pps.num_ref_idx_l0_default_active_minus1 = 0;
	t->pps.weighted_bipred_idc = 0;
	t->pps.flags = V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;

	cursor = 0;
	KUNIT_ASSERT_EQ(test,
			avd_h264_next_nalu(cavlc_2slice_idr, sizeof(cavlc_2slice_idr),
					   &cursor, &nalu), 1);
	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(cavlc_2slice_idr + nalu.off, nalu.len,
						    nalu.nal_ref_idc, nalu.nal_unit_type,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.first_mb_in_slice, 0);
	KUNIT_EXPECT_EQ(test, t->sl.slice_type, V4L2_H264_SLICE_TYPE_I);
	KUNIT_EXPECT_EQ(test, t->sl.slice_qp_delta, 8);
	KUNIT_EXPECT_EQ(test, t->sl.header_bit_size, 36);
	KUNIT_EXPECT_EQ(test, data_off, 4);
	KUNIT_EXPECT_EQ(test, bit_off, 4);

	KUNIT_ASSERT_EQ(test,
			avd_h264_next_nalu(cavlc_2slice_idr, sizeof(cavlc_2slice_idr),
					   &cursor, &nalu), 1);
	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(cavlc_2slice_idr + nalu.off, nalu.len,
						    nalu.nal_ref_idc, nalu.nal_unit_type,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.first_mb_in_slice, 8);
	KUNIT_EXPECT_EQ(test, t->sl.slice_qp_delta, 7);
	KUNIT_EXPECT_EQ(test, t->sl.header_bit_size, 40);
	KUNIT_EXPECT_EQ(test, data_off, 5);
	KUNIT_EXPECT_EQ(test, bit_off, 0);
	KUNIT_EXPECT_EQ(test,
			avd_h264_next_nalu(cavlc_2slice_idr, sizeof(cavlc_2slice_idr),
					   &cursor, &nalu), 0);

	/* Baseline CAVLC 2-slice P-frame */
	cursor = 0;
	KUNIT_ASSERT_EQ(test,
			avd_h264_next_nalu(cavlc_2slice_p, sizeof(cavlc_2slice_p),
					   &cursor, &nalu), 1);
	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(cavlc_2slice_p + nalu.off, nalu.len,
						    nalu.nal_ref_idc, nalu.nal_unit_type,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.first_mb_in_slice, 0);
	KUNIT_EXPECT_EQ(test, t->sl.slice_type, V4L2_H264_SLICE_TYPE_P);
	KUNIT_EXPECT_EQ(test, t->sl.slice_qp_delta, 8);
	KUNIT_EXPECT_EQ(test, t->sl.header_bit_size, 34);
	KUNIT_EXPECT_EQ(test, data_off, 4);
	KUNIT_EXPECT_EQ(test, bit_off, 2);

	KUNIT_ASSERT_EQ(test,
			avd_h264_next_nalu(cavlc_2slice_p, sizeof(cavlc_2slice_p),
					   &cursor, &nalu), 1);
	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(cavlc_2slice_p + nalu.off, nalu.len,
						    nalu.nal_ref_idc, nalu.nal_unit_type,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.first_mb_in_slice, 8);
	KUNIT_EXPECT_EQ(test, t->sl.slice_qp_delta, 8);
	KUNIT_EXPECT_EQ(test, t->sl.header_bit_size, 40);
	KUNIT_EXPECT_EQ(test, data_off, 5);
	KUNIT_EXPECT_EQ(test, bit_off, 0);
}

static void avd_frame_mode_framing_test(struct kunit *test)
{
	/* Real libx264 CABAC IDR slice, unprefixed (7.3.1 NALU) and Annex-B. */
	static const u8 raw_idr[] = {
		0x65, 0x88, 0x84, 0x00, 0x87, 0x69, 0xd7, 0xdc, 0x00, 0x00,
	};
	static const u8 annexb_two_slice[] = {
		0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x08, 0x7c, 0x42, 0x22, 0xb0,
		0x00, 0x00, 0x01, 0x65, 0x12, 0x22, 0x10, 0x77, 0xf1, 0xff, 0x87,
	};
	static const u8 raw_with_embedded_sc[] = {
		0x65, 0x88, 0x84, 0x00, 0x00, 0x01, 0x65, 0x12,
	};
	static const u8 bad_raw_hdrs[] = {
		0x00, 0x06, 0x09, 0x67, 0x68, 0x22, 0x23, 0x24, 0x2e, 0x34, 0xe5,
	};
	static const u8 one_byte[] = { 0x65 };
	static const u8 trunc_zeros[] = { 0x00, 0x00 };
	struct avd_parse_test_ctx *t = avd_parse_test_alloc(test);
	struct avd_h264_nalu nalu;
	u32 cursor, data_off;
	u8 bit_off;

	/* Mode / start-code pairing matrix */
	KUNIT_EXPECT_TRUE(test,
			  avd_h264_mode_supported(V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
						  V4L2_STATELESS_H264_START_CODE_NONE));
	KUNIT_EXPECT_FALSE(test,
			   avd_h264_mode_supported(V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
						   V4L2_STATELESS_H264_START_CODE_ANNEX_B));
	KUNIT_EXPECT_TRUE(test,
			  avd_h264_mode_supported(V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
						  V4L2_STATELESS_H264_START_CODE_ANNEX_B));
	KUNIT_EXPECT_TRUE(test,
			  avd_h264_mode_supported(V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
						  V4L2_STATELESS_H264_START_CODE_NONE));
	KUNIT_EXPECT_FALSE(test,
			   avd_h264_mode_supported(-1, V4L2_STATELESS_H264_START_CODE_NONE));
	KUNIT_EXPECT_FALSE(test,
			   avd_h264_mode_supported(V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED, 2));

	/* FRAME_BASED + START_CODE_ANNEX_B: accepts Annex-B, rejects raw NALU */
	cursor = 0;
	KUNIT_EXPECT_EQ(test,
			avd_h264_next_frame_nalu(annexb_two_slice, sizeof(annexb_two_slice),
						 V4L2_STATELESS_H264_START_CODE_ANNEX_B,
						 &cursor, &nalu),
			1);
	KUNIT_EXPECT_EQ(test, nalu.nal_unit_type, 5);
	KUNIT_EXPECT_EQ(test,
			avd_h264_next_frame_nalu(annexb_two_slice, sizeof(annexb_two_slice),
						 V4L2_STATELESS_H264_START_CODE_ANNEX_B,
						 &cursor, &nalu),
			1);
	KUNIT_EXPECT_EQ(test,
			avd_h264_next_frame_nalu(annexb_two_slice, sizeof(annexb_two_slice),
						 V4L2_STATELESS_H264_START_CODE_ANNEX_B,
						 &cursor, &nalu),
			0);

	cursor = 0;
	KUNIT_EXPECT_EQ(test,
			avd_h264_next_frame_nalu(raw_idr, sizeof(raw_idr),
						 V4L2_STATELESS_H264_START_CODE_ANNEX_B,
						 &cursor, &nalu),
			-EINVAL);

	/* FRAME_BASED + START_CODE_NONE: accepts unprefixed 7.3.1 VCL slice NALU */
	t->sps.chroma_format_idc = 1;
	t->sps.pic_order_cnt_type = 0;
	t->pps.num_ref_idx_l0_default_active_minus1 = 1;
	t->pps.flags = V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE |
		       V4L2_H264_PPS_FLAG_WEIGHTED_PRED |
		       V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;

	cursor = 0;
	KUNIT_ASSERT_EQ(test,
			avd_h264_next_frame_nalu(raw_idr, sizeof(raw_idr),
						 V4L2_STATELESS_H264_START_CODE_NONE,
						 &cursor, &nalu),
			1);
	KUNIT_EXPECT_EQ(test, nalu.off, 0);
	KUNIT_EXPECT_EQ(test, nalu.len, sizeof(raw_idr) - 2);
	KUNIT_EXPECT_EQ(test, nalu.nal_unit_type, 5);
	KUNIT_EXPECT_EQ(test, nalu.nal_ref_idc, 3);
	KUNIT_EXPECT_EQ(test,
			avd_h264_next_frame_nalu(raw_idr, sizeof(raw_idr),
						 V4L2_STATELESS_H264_START_CODE_NONE,
						 &cursor, &nalu),
			0);
	KUNIT_ASSERT_EQ(test,
			avd_h264_parse_slice_header(raw_idr + nalu.off, nalu.len,
						    nalu.nal_ref_idc, nalu.nal_unit_type,
						    &t->sps, &t->pps, &t->decode,
						    0, NULL, NULL, NULL,
						    &t->sl, &t->weights,
						    &data_off, &bit_off),
			0);
	KUNIT_EXPECT_EQ(test, t->sl.slice_type, V4L2_H264_SLICE_TYPE_I);
	KUNIT_EXPECT_EQ(test, t->sl.slice_qp_delta, 8);
	KUNIT_EXPECT_EQ(test, data_off, 5);
	KUNIT_EXPECT_EQ(test, bit_off, 0);

	/* FRAME_BASED + START_CODE_NONE: Chromium Annex-B compatibility path */
	cursor = 0;
	KUNIT_EXPECT_EQ(test,
			avd_h264_next_frame_nalu(annexb_two_slice, sizeof(annexb_two_slice),
						 V4L2_STATELESS_H264_START_CODE_NONE,
						 &cursor, &nalu),
			1);
	KUNIT_EXPECT_EQ(test,
			avd_h264_next_frame_nalu(annexb_two_slice, sizeof(annexb_two_slice),
						 V4L2_STATELESS_H264_START_CODE_NONE,
						 &cursor, &nalu),
			1);
	KUNIT_EXPECT_EQ(test,
			avd_h264_next_frame_nalu(annexb_two_slice, sizeof(annexb_two_slice),
						 V4L2_STATELESS_H264_START_CODE_NONE,
						 &cursor, &nalu),
			0);

	/* Error paths for FRAME_BASED + START_CODE_NONE and invalid start_code */
	for (unsigned int i = 0; i < ARRAY_SIZE(bad_raw_hdrs); i++) {
		u8 buf[] = { bad_raw_hdrs[i], 0x88, 0x84 };

		cursor = 0;
		KUNIT_EXPECT_EQ(test,
				avd_h264_next_frame_nalu(buf, sizeof(buf),
							 V4L2_STATELESS_H264_START_CODE_NONE,
							 &cursor, &nalu),
				-EINVAL);
	}
	cursor = 0;
	KUNIT_EXPECT_EQ(test,
			avd_h264_next_frame_nalu(one_byte, sizeof(one_byte),
						 V4L2_STATELESS_H264_START_CODE_NONE,
						 &cursor, &nalu),
			-EINVAL);
	cursor = 0;
	KUNIT_EXPECT_EQ(test,
			avd_h264_next_frame_nalu(trunc_zeros, sizeof(trunc_zeros),
						 V4L2_STATELESS_H264_START_CODE_NONE,
						 &cursor, &nalu),
			-EINVAL);
	cursor = 0;
	KUNIT_EXPECT_EQ(test,
			avd_h264_next_frame_nalu(raw_with_embedded_sc, sizeof(raw_with_embedded_sc),
						 V4L2_STATELESS_H264_START_CODE_NONE,
						 &cursor, &nalu),
			-EINVAL);
	cursor = 0;
	KUNIT_EXPECT_EQ(test,
			avd_h264_next_frame_nalu(raw_idr, sizeof(raw_idr), 2,
						 &cursor, &nalu),
			-EINVAL);
}

static struct kunit_case avd_h264_test_cases[] = {
	KUNIT_CASE(avd_scaling_flat_test),
	KUNIT_CASE_PARAM(avd_slice_data_test, avd_slice_data_gen_params),
	KUNIT_CASE_PARAM(avd_slice_fits_test, avd_slice_fits_gen_params),
	KUNIT_CASE_PARAM(avd_sps_size_test, avd_sps_size_gen_params),
	KUNIT_CASE(avd_sps_fmt_init_test),
	KUNIT_CASE(avd_sps_range_test),
	KUNIT_CASE(avd_start_slices_test),
	KUNIT_CASE(avd_annexb_scanner_test),
	KUNIT_CASE(avd_frame_mode_framing_test),
	KUNIT_CASE(avd_bitreader_epb_test),
	KUNIT_CASE(avd_slice_hdr_epb_boundary_test),
	KUNIT_CASE(avd_slice_hdr_fields_test),
	KUNIT_CASE(avd_slice_hdr_ref_mod_test),
	KUNIT_CASE(avd_slice_hdr_pred_weights_test),
	KUNIT_CASE(avd_slice_hdr_marking_test),
	KUNIT_CASE(avd_slice_hdr_real_annexb_test),
	{}
};

static struct kunit_suite avd_h264_test_suite = {
	.name = "apple-avd-h264",
	.test_cases = avd_h264_test_cases,
};

kunit_test_suite(avd_h264_test_suite);
