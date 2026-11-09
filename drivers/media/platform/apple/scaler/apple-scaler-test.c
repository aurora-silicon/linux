// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the Apple scaler/CSC register encodings.
 *
 * The expected words are what the engine was driven with for the same
 * conversions in our macOS capture (traces/display/20261108-msr-trace),
 * and they equal the textbook BT.601/BT.709/BT.2020 matrices in Q3.12.
 */
#include <kunit/test.h>
#include <linux/array_size.h>
#include <linux/math.h>
#include <linux/string.h>

#include "apple-scaler-hw.h"
#include "apple-scaler-regs.h"

struct asc_csc_case {
	const char *name;
	enum v4l2_ycbcr_encoding enc;
	enum v4l2_quantization quant;
	bool yuv_to_rgb;
	u16 coef[9];
	u32 pre[3];
	u32 post[3];
};

static const struct asc_csc_case asc_csc_cases[] = {
	{ "BT.709 limited to RGB", V4L2_YCBCR_ENC_709, V4L2_QUANTIZATION_LIM_RANGE, true,
	  { 0x12a1, 0x0000, 0x1caf, 0x12a1, 0xfc97, 0xf779, 0x12a1, 0x21cc, 0x0000 },
	  { 0x03eff000, 0x037fe000, 0x037fe000 }, { 0, 0, 0 } },
	{ "BT.601 limited to RGB", V4L2_YCBCR_ENC_601, V4L2_QUANTIZATION_LIM_RANGE, true,
	  { 0x12a1, 0x0000, 0x1989, 0x12a1, 0xf9bb, 0xf2fe, 0x12a1, 0x2047, 0x0000 },
	  { 0x03eff000, 0x037fe000, 0x037fe000 }, { 0, 0, 0 } },
	{ "BT.709 full to RGB", V4L2_YCBCR_ENC_709, V4L2_QUANTIZATION_FULL_RANGE, true,
	  { 0x1000, 0x0000, 0x1932, 0x1000, 0xfd01, 0xf883, 0x1000, 0x1db1, 0x0000 },
	  { 0x00000000, 0x037fe000, 0x037fe000 }, { 0, 0, 0 } },
	{ "BT.2020 limited to RGB", V4L2_YCBCR_ENC_BT2020, V4L2_QUANTIZATION_LIM_RANGE, true,
	  { 0x12a1, 0x0000, 0x1adc, 0x12a1, 0xfd01, 0xf598, 0x12a1, 0x2245, 0x0000 },
	  { 0x03eff000, 0x037fe000, 0x037fe000 }, { 0, 0, 0 } },
	{ "RGB to BT.709 limited", V4L2_YCBCR_ENC_709, V4L2_QUANTIZATION_LIM_RANGE, false,
	  { 0x02ec, 0x09d4, 0x00fe, 0xfe64, 0xfa95, 0x0707, 0x0707, 0xf99e, 0xff5b },
	  { 0, 0, 0 }, { 0x00101000, 0x00802000, 0x00802000 } },
	{ "RGB to BT.601 limited", V4L2_YCBCR_ENC_601, V4L2_QUANTIZATION_LIM_RANGE, false,
	  { 0x041c, 0x0811, 0x0191, 0xfda1, 0xfb58, 0x0707, 0x0707, 0xfa1e, 0xfedb },
	  { 0, 0, 0 }, { 0x00101000, 0x00802000, 0x00802000 } },
};

static void asc_csc_case_desc(const struct asc_csc_case *c, char *desc)
{
	strscpy(desc, c->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(asc_csc, asc_csc_cases, asc_csc_case_desc);

static void asc_test_csc(struct kunit *test)
{
	const struct asc_csc_case *c = test->param_value;
	struct asc_csc csc;
	int i;

	KUNIT_ASSERT_EQ(test, asc_csc_compute(c->enc, c->quant, c->yuv_to_rgb, &csc), 0);
	for (i = 0; i < 9; i++)
		KUNIT_EXPECT_EQ_MSG(test, (u16)csc.coef[i / 3][i % 3], c->coef[i],
				    "coefficient %d", i);
	for (i = 0; i < 3; i++) {
		KUNIT_EXPECT_EQ_MSG(test, asc_csc_offset_reg(csc.pre[i]), c->pre[i],
				    "pre-offset %d", i);
		KUNIT_EXPECT_EQ_MSG(test, asc_csc_offset_reg(csc.post[i]), c->post[i],
				    "post-offset %d", i);
	}
}

static void asc_test_csc_rejects(struct kunit *test)
{
	struct asc_csc csc;

	KUNIT_EXPECT_EQ(test, asc_csc_compute(V4L2_YCBCR_ENC_SMPTE240M,
					      V4L2_QUANTIZATION_LIM_RANGE, true, &csc),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, asc_csc_compute(V4L2_YCBCR_ENC_BT2020_CONST_LUM,
					      V4L2_QUANTIZATION_LIM_RANGE, true, &csc),
			-EINVAL);
}

/* Every matrix row must reproduce white and black exactly (to rounding). */
static void asc_test_csc_white(struct kunit *test)
{
	static const enum v4l2_ycbcr_encoding encs[] = {
		V4L2_YCBCR_ENC_601, V4L2_YCBCR_ENC_709, V4L2_YCBCR_ENC_BT2020,
	};
	struct asc_csc csc;
	int e, row;

	for (e = 0; e < ARRAY_SIZE(encs); e++) {
		KUNIT_ASSERT_EQ(test, asc_csc_compute(encs[e], V4L2_QUANTIZATION_FULL_RANGE,
						      false, &csc), 0);
		/* RGB -> YCbCr: Y row sums to 1.0, chroma rows to 0. */
		KUNIT_EXPECT_LE(test, abs(csc.coef[0][0] + csc.coef[0][1] + csc.coef[0][2] - 4096), 1);
		for (row = 1; row < 3; row++)
			KUNIT_EXPECT_LE(test, abs(csc.coef[row][0] + csc.coef[row][1] +
						  csc.coef[row][2]), 1);
	}
}

static void asc_test_ratio(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, asc_scale_ratio(1920, 1920), 0x400000);
	KUNIT_EXPECT_EQ(test, asc_scale_ratio(1920, 1280), 0x600000);
	KUNIT_EXPECT_EQ(test, asc_scale_ratio(1280, 1920), 0x2aaaa8);
	KUNIT_EXPECT_EQ(test, asc_scale_ratio(640, 1920), 0x155554);
	KUNIT_EXPECT_EQ(test, asc_scale_ratio(1920, 640), 0xc00000);
	KUNIT_EXPECT_EQ(test, asc_scale_ratio(1, 0), 0);
}

static void asc_test_formats(struct kunit *test)
{
	const struct asc_format *f;
	unsigned int i;

	f = asc_find_format(V4L2_PIX_FMT_NV12M);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_EXPECT_EQ(test, f->layout, 0xc);
	KUNIT_EXPECT_EQ(test, f->code, 0x1);
	KUNIT_EXPECT_EQ(test, f->sampling, 0x6);
	KUNIT_EXPECT_EQ(test, f->swizzle, 0x03000102);

	f = asc_find_format(V4L2_PIX_FMT_ARGB32);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_EXPECT_EQ(test, f->code, 0x1e);
	KUNIT_EXPECT_EQ(test, f->swizzle, 0x03020100);

	KUNIT_EXPECT_NULL(test, asc_find_format(V4L2_PIX_FMT_YUYV));

	for (i = 0; (f = asc_format_by_index(i)); i++) {
		KUNIT_EXPECT_GE(test, f->planes, 1);
		KUNIT_EXPECT_LE(test, f->planes, 2);
		KUNIT_EXPECT_EQ(test, f->yuv, f->layout != 0x1);
		KUNIT_EXPECT_EQ(test, f->planes == 2, f->yuv);
	}
}


/*
 * Final register state of three jobs as the engine was driven on macOS
 * (1920x1080, same size): NV12 BT.709 limited -> ARGB32, ARGB32 -> NV12
 * BT.709 limited, P010 BT.2020 limited -> ARGB32. The CSC matrix words
 * (0x3814..0x3834) are covered by asc_test_csc.
 */
static const struct asc_reg asc_expect_nv12_709_argb[] = {
	{ 0x00080, 0x00000001 }, { 0x0008c, 0x8000ffff }, { 0x00098, 0xfffffffe },
	{ 0x000e4, 0x00000000 }, { 0x000e8, 0x00000038 }, { 0x00110, 0x00000094 },
	{ 0x00124, 0x00000000 }, { 0x00180, 0x0000000c }, { 0x00184, 0x00000001 },
	{ 0x00188, 0x00000006 }, { 0x0018c, 0x00000000 }, { 0x00190, 0x00000000 },
	{ 0x00198, 0x007f0200 }, { 0x0019c, 0x00000100 }, { 0x001a0, 0x009ea600 },
	{ 0x001a4, 0x00000100 }, { 0x001d8, 0x00000780 }, { 0x001dc, 0x00000780 },
	{ 0x001e8, 0x00000000 }, { 0x001ec, 0x00000000 }, { 0x001f0, 0x00000000 },
	{ 0x001f8, 0x03000102 }, { 0x001fc, 0x00000780 }, { 0x00200, 0x00000438 },
	{ 0x00204, 0x0007d311 }, { 0x00208, 0x0007d311 }, { 0x0020c, 0x0007d311 },
	{ 0x00210, 0x0007d311 }, { 0x00214, 0x00000707 }, { 0x00218, 0x00000707 },
	{ 0x0021c, 0x00000707 }, { 0x00220, 0x00000707 }, { 0x00224, 0x00000020 },
	{ 0x00228, 0x00000020 }, { 0x0022c, 0x00000020 }, { 0x00230, 0x00000020 },
	{ 0x00234, 0x00000780 }, { 0x00238, 0x000003c0 }, { 0x0023c, 0x00000780 },
	{ 0x00244, 0x00000438 }, { 0x00248, 0x0000021c }, { 0x0024c, 0x00000438 },
	{ 0x00254, 0x00000001 }, { 0x00280, 0x00000001 }, { 0x00284, 0x0000801e },
	{ 0x00288, 0x00008000 }, { 0x0028c, 0x00008000 }, { 0x00290, 0x00000000 },
	{ 0x00294, 0x00000100 }, { 0x00298, 0x00000000 }, { 0x0029c, 0x00000000 },
	{ 0x002a0, 0x00000000 }, { 0x002a4, 0x00000000 }, { 0x002c0, 0x00001e00 },
	{ 0x002c4, 0x00000000 }, { 0x002c8, 0x00000000 }, { 0x002cc, 0x00000000 },
	{ 0x002d0, 0x00000000 }, { 0x002d4, 0x00000000 }, { 0x002d8, 0x03020100 },
	{ 0x002dc, 0x00000780 }, { 0x002e0, 0x00000438 }, { 0x002e4, 0x000ac211 },
	{ 0x002e8, 0x000ac211 }, { 0x002ec, 0x000ac211 }, { 0x002f0, 0x0c0ac207 },
	{ 0x002f4, 0x0c0ac207 }, { 0x002f8, 0x0c0ac207 }, { 0x002fc, 0x04000620 },
	{ 0x00300, 0x04000620 }, { 0x00304, 0x04000620 }, { 0x00308, 0x00000780 },
	{ 0x0030c, 0x00000780 }, { 0x00310, 0x00000780 }, { 0x00314, 0x00000438 },
	{ 0x00318, 0x00000438 }, { 0x0031c, 0x00000438 }, { 0x00380, 0x00000000 },
	{ 0x00480, 0x00000000 }, { 0x00800, 0x0000000d }, { 0x00804, 0x00000000 },
	{ 0x00808, 0x00000800 }, { 0x0080c, 0x00000800 }, { 0x00810, 0x00000000 },
	{ 0x00814, 0x00000000 }, { 0x00818, 0x00000800 }, { 0x0081c, 0x00000800 },
	{ 0x00820, 0x00000000 }, { 0x00824, 0x0000000c }, { 0x00900, 0x00000000 },
	{ 0x00a00, 0x00000000 }, { 0x01000, 0x00000002 }, { 0x01004, 0x00000000 },
	{ 0x01008, 0x00000000 }, { 0x0100c, 0x00400000 }, { 0x01010, 0x00400000 },
	{ 0x01014, 0x00400000 }, { 0x01018, 0x00400000 }, { 0x0101c, 0x00000000 },
	{ 0x01020, 0x00400000 }, { 0x01024, 0x00400000 }, { 0x02000, 0x00000002 },
	{ 0x02004, 0x00000000 }, { 0x02008, 0x00000000 }, { 0x0200c, 0x00400000 },
	{ 0x02010, 0x00400000 }, { 0x02014, 0x00400000 }, { 0x02018, 0x00400000 },
	{ 0x0201c, 0x00000000 }, { 0x02020, 0x00400000 }, { 0x02024, 0x00400000 },
	{ 0x03000, 0x00000000 }, { 0x03800, 0x000e0003 }, { 0x03804, 0x0000000c },
	{ 0x03808, 0x03eff000 }, { 0x0380c, 0x037fe000 }, { 0x03810, 0x037fe000 },
	{ 0x03838, 0x00000000 }, { 0x0383c, 0x00000000 }, { 0x03840, 0x00000000 },
	{ 0x07418, 0xffffffff }, { 0x0a98c, 0x80000001 }, { 0x0a990, 0x80000001 },
	{ 0x0a994, 0x80000001 }, { 0x0a998, 0x7fffffff }, { 0x0a99c, 0x7fffffff },
	{ 0x0a9a0, 0x7fffffff }, { 0x0a9a4, 0x80000001 }, { 0x0a9a8, 0x80000001 },
	{ 0x0a9ac, 0x80000001 }, { 0x0a9b0, 0x7fffffff }, { 0x0a9b4, 0x7fffffff },
	{ 0x0a9b8, 0x7fffffff }, { 0x13800, 0x00000000 }, { 0x13804, 0x00000000 },
	{ 0x13808, 0x00000000 }, { 0x1380c, 0x00000000 }, { 0x13810, 0x00000008 },
	{ 0x13814, 0x00000028 }, { 0x13818, 0x00000028 }, { 0x1381c, 0x00000020 },
	{ 0x13c00, 0x00000001 }, { 0x13c04, 0x00000000 }, { 0x13c10, 0x00000008 },
	{ 0x13c14, 0x00000008 }, { 0x13c18, 0x00000008 }, { 0x13c1c, 0x00000008 },
	{ 0x16000, 0x00000000 }, { 0x16004, 0x244024c0 }, { 0x16080, 0x00000000 },
	{ 0x16084, 0x0b710367 }, { 0x16088, 0x00000128 }, { 0x16504, 0x02000500 },
	{ 0x16508, 0x00003200 }, { 0x1651c, 0x00851400 }, { 0x16534, 0x00000008 },
	{ 0x16568, 0x00250500 }, { 0x16588, 0x00496513 }, { 0x16600, 0x00000015 },
	{ 0x16800, 0x00000000 }, { 0x25080, 0x00000000 }, { 0x2e300, 0x03804038 },
	{ 0x2e304, 0xa0a0a0a0 }, { 0x2e308, 0x02550000 }, { 0x2e30c, 0x02550255 },
	{ 0x2e310, 0x00000000 }, { 0x2e314, 0x00100008 }, { 0x2e318, 0x02550000 },
};

static const struct asc_reg asc_expect_argb_nv12_709[] = {
	{ 0x00080, 0x00000001 }, { 0x0008c, 0x0000ffff }, { 0x00098, 0xfffffffe },
	{ 0x000e4, 0x00000000 }, { 0x000e8, 0x00000038 }, { 0x00110, 0x000000b4 },
	{ 0x00124, 0x00000000 }, { 0x00180, 0x00000001 }, { 0x00184, 0x0000001e },
	{ 0x00188, 0x00000000 }, { 0x0018c, 0x00000000 }, { 0x00190, 0x00000000 },
	{ 0x00198, 0x00300000 }, { 0x0019c, 0x00000100 }, { 0x001a0, 0x00000000 },
	{ 0x001a4, 0x00000000 }, { 0x001a8, 0x00000000 }, { 0x001ac, 0x00000000 },
	{ 0x001d8, 0x00001e00 }, { 0x001dc, 0x00000000 }, { 0x001e0, 0x00000000 },
	{ 0x001e8, 0x00000000 }, { 0x001ec, 0x00000000 }, { 0x001f0, 0x00000000 },
	{ 0x001f8, 0x03020100 }, { 0x001fc, 0x00000780 }, { 0x00200, 0x00000438 },
	{ 0x00204, 0x0007d311 }, { 0x00208, 0x0007d311 }, { 0x0020c, 0x0007d311 },
	{ 0x00210, 0x0007d311 }, { 0x00214, 0x00000707 }, { 0x00218, 0x00000707 },
	{ 0x0021c, 0x00000707 }, { 0x00220, 0x00000707 }, { 0x00224, 0x00000020 },
	{ 0x00228, 0x00000020 }, { 0x0022c, 0x00000020 }, { 0x00230, 0x00000020 },
	{ 0x00234, 0x00000780 }, { 0x00238, 0x00000780 }, { 0x0023c, 0x00000780 },
	{ 0x00244, 0x00000438 }, { 0x00248, 0x00000438 }, { 0x0024c, 0x00000438 },
	{ 0x00254, 0x00000001 }, { 0x00280, 0x0000000c }, { 0x00284, 0x00008001 },
	{ 0x00288, 0x00008006 }, { 0x0028c, 0x00008000 }, { 0x00290, 0x00000200 },
	{ 0x00294, 0x00000100 }, { 0x00298, 0x001fa600 }, { 0x0029c, 0x00000100 },
	{ 0x002c0, 0x00000780 }, { 0x002c4, 0x00000780 }, { 0x002cc, 0x00000000 },
	{ 0x002d0, 0x00000000 }, { 0x002d4, 0x00000000 }, { 0x002d8, 0x03000102 },
	{ 0x002dc, 0x00000780 }, { 0x002e0, 0x00000438 }, { 0x002e4, 0x000ac211 },
	{ 0x002e8, 0x000ac211 }, { 0x002ec, 0x000ac211 }, { 0x002f0, 0x0c0ac207 },
	{ 0x002f4, 0x0c0ac207 }, { 0x002f8, 0x0c0ac207 }, { 0x002fc, 0x04000620 },
	{ 0x00300, 0x04000620 }, { 0x00304, 0x04000620 }, { 0x00308, 0x00000780 },
	{ 0x0030c, 0x000003c0 }, { 0x00310, 0x00000780 }, { 0x00314, 0x00000438 },
	{ 0x00318, 0x0000021c }, { 0x0031c, 0x00000438 }, { 0x00380, 0x00000000 },
	{ 0x00480, 0x00000000 }, { 0x00800, 0x0000000c }, { 0x00824, 0x0000000c },
	{ 0x00900, 0x00010103 }, { 0x00a00, 0x00000000 }, { 0x01000, 0x00000000 },
	{ 0x01004, 0x00000000 }, { 0x01008, 0x00000000 }, { 0x0100c, 0x00400000 },
	{ 0x01010, 0x00400000 }, { 0x01014, 0x00400000 }, { 0x01018, 0x00400000 },
	{ 0x0101c, 0x00000000 }, { 0x01020, 0x00400000 }, { 0x01024, 0x00400000 },
	{ 0x02000, 0x00000000 }, { 0x02004, 0x00000000 }, { 0x02008, 0x00000000 },
	{ 0x0200c, 0x00400000 }, { 0x02010, 0x00400000 }, { 0x02014, 0x00400000 },
	{ 0x02018, 0x00400000 }, { 0x0201c, 0x00000000 }, { 0x02020, 0x00400000 },
	{ 0x02024, 0x00400000 }, { 0x03000, 0x00000000 }, { 0x03800, 0x000e0003 },
	{ 0x03804, 0x0000000c }, { 0x03808, 0x00000000 }, { 0x0380c, 0x00000000 },
	{ 0x03810, 0x00000000 }, { 0x03838, 0x00101000 }, { 0x0383c, 0x00802000 },
	{ 0x03840, 0x00802000 }, { 0x07418, 0xffffffff }, { 0x0a98c, 0x80000001 },
	{ 0x0a990, 0x80000001 }, { 0x0a994, 0x80000001 }, { 0x0a998, 0x7fffffff },
	{ 0x0a99c, 0x7fffffff }, { 0x0a9a0, 0x7fffffff }, { 0x0a9a4, 0x80000001 },
	{ 0x0a9a8, 0x80000001 }, { 0x0a9ac, 0x80000001 }, { 0x0a9b0, 0x7fffffff },
	{ 0x0a9b4, 0x7fffffff }, { 0x0a9b8, 0x7fffffff }, { 0x13800, 0x00000000 },
	{ 0x13804, 0x00000000 }, { 0x13808, 0x00000000 }, { 0x1380c, 0x00000000 },
	{ 0x13810, 0x00000008 }, { 0x13814, 0x00000008 }, { 0x13818, 0x00000008 },
	{ 0x1381c, 0x00000008 }, { 0x13c00, 0x00000001 }, { 0x13c04, 0x00000000 },
	{ 0x13c10, 0x00000008 }, { 0x13c14, 0x00000028 }, { 0x13c18, 0x00000028 },
	{ 0x13c1c, 0x00000020 }, { 0x16000, 0x00000000 }, { 0x16004, 0x244024c0 },
	{ 0x16080, 0x00000000 }, { 0x16084, 0x0b710367 }, { 0x16088, 0x00000128 },
	{ 0x16504, 0x02000500 }, { 0x16508, 0x00003200 }, { 0x1651c, 0x00851400 },
	{ 0x16534, 0x00000008 }, { 0x16568, 0x00250500 }, { 0x16588, 0x00496513 },
	{ 0x16600, 0x00000015 }, { 0x16800, 0x00000000 }, { 0x25080, 0x00000000 },
	{ 0x2e300, 0x03804038 }, { 0x2e304, 0xa0a0a0a0 }, { 0x2e308, 0x02550000 },
	{ 0x2e30c, 0x02550255 }, { 0x2e310, 0x00000000 }, { 0x2e314, 0x00100008 },
	{ 0x2e318, 0x02550000 },
};

static const struct asc_reg asc_expect_p010_2020_argb[] = {
	{ 0x00080, 0x00000001 }, { 0x0008c, 0x8000ffff }, { 0x00098, 0xfffffffe },
	{ 0x000e4, 0x00000000 }, { 0x000e8, 0x00000038 }, { 0x00110, 0x000000bc },
	{ 0x00124, 0x00000000 }, { 0x00180, 0x0000000c }, { 0x00184, 0x00000002 },
	{ 0x00188, 0x00000012 }, { 0x0018c, 0x00000000 }, { 0x00190, 0x00000000 },
	{ 0x00198, 0x007f0200 }, { 0x0019c, 0x00000100 }, { 0x001a0, 0x00be4a00 },
	{ 0x001a4, 0x00000100 }, { 0x001d8, 0x00000f00 }, { 0x001dc, 0x00000f00 },
	{ 0x001e8, 0x00000000 }, { 0x001ec, 0x00000000 }, { 0x001f0, 0x00000000 },
	{ 0x001f8, 0x03000102 }, { 0x001fc, 0x00000780 }, { 0x00200, 0x00000438 },
	{ 0x00204, 0x0007d311 }, { 0x00208, 0x0007d311 }, { 0x0020c, 0x0007d311 },
	{ 0x00210, 0x0007d311 }, { 0x00214, 0x00000707 }, { 0x00218, 0x00000707 },
	{ 0x0021c, 0x00000707 }, { 0x00220, 0x00000707 }, { 0x00224, 0x00000020 },
	{ 0x00228, 0x00000020 }, { 0x0022c, 0x00000020 }, { 0x00230, 0x00000020 },
	{ 0x00234, 0x00000780 }, { 0x00238, 0x000003c0 }, { 0x0023c, 0x00000780 },
	{ 0x00244, 0x00000438 }, { 0x00248, 0x0000021c }, { 0x0024c, 0x00000438 },
	{ 0x00254, 0x00000001 }, { 0x00280, 0x00000001 }, { 0x00284, 0x0000801e },
	{ 0x00288, 0x00008000 }, { 0x0028c, 0x00008000 }, { 0x00290, 0x00000000 },
	{ 0x00294, 0x00000100 }, { 0x00298, 0x00000000 }, { 0x0029c, 0x00000000 },
	{ 0x002a0, 0x00000000 }, { 0x002a4, 0x00000000 }, { 0x002c0, 0x00001e00 },
	{ 0x002c4, 0x00000000 }, { 0x002c8, 0x00000000 }, { 0x002cc, 0x00000000 },
	{ 0x002d0, 0x00000000 }, { 0x002d4, 0x00000000 }, { 0x002d8, 0x03020100 },
	{ 0x002dc, 0x00000780 }, { 0x002e0, 0x00000438 }, { 0x002e4, 0x000ac211 },
	{ 0x002e8, 0x000ac211 }, { 0x002ec, 0x000ac211 }, { 0x002f0, 0x0c0ac207 },
	{ 0x002f4, 0x0c0ac207 }, { 0x002f8, 0x0c0ac207 }, { 0x002fc, 0x04000620 },
	{ 0x00300, 0x04000620 }, { 0x00304, 0x04000620 }, { 0x00308, 0x00000780 },
	{ 0x0030c, 0x00000780 }, { 0x00310, 0x00000780 }, { 0x00314, 0x00000438 },
	{ 0x00318, 0x00000438 }, { 0x0031c, 0x00000438 }, { 0x00380, 0x00000000 },
	{ 0x00480, 0x00000000 }, { 0x00800, 0x0000000d }, { 0x00804, 0x00000000 },
	{ 0x00808, 0x00000800 }, { 0x0080c, 0x00000800 }, { 0x00810, 0x00000000 },
	{ 0x00814, 0x00000000 }, { 0x00818, 0x00000800 }, { 0x0081c, 0x00000800 },
	{ 0x00820, 0x00000000 }, { 0x00824, 0x0000000c }, { 0x00900, 0x00000000 },
	{ 0x00a00, 0x02020200 }, { 0x00a80, 0x00000000 }, { 0x00a84, 0x00000002 },
	{ 0x00a88, 0x00000000 }, { 0x00a8c, 0x00000002 }, { 0x00a90, 0x00000003 },
	{ 0x00a94, 0x00000001 }, { 0x00a98, 0x00000003 }, { 0x00a9c, 0x00000001 },
	{ 0x00aa0, 0x00000000 }, { 0x00aa4, 0x00000002 }, { 0x00aa8, 0x00000000 },
	{ 0x00aac, 0x00000002 }, { 0x00ab0, 0x00000003 }, { 0x00ab4, 0x00000001 },
	{ 0x00ab8, 0x00000003 }, { 0x00abc, 0x00000001 }, { 0x01000, 0x00000002 },
	{ 0x01004, 0x00000000 }, { 0x01008, 0x00000000 }, { 0x0100c, 0x00400000 },
	{ 0x01010, 0x00400000 }, { 0x01014, 0x00400000 }, { 0x01018, 0x00400000 },
	{ 0x0101c, 0x00000000 }, { 0x01020, 0x00400000 }, { 0x01024, 0x00400000 },
	{ 0x02000, 0x00000002 }, { 0x02004, 0x00000000 }, { 0x02008, 0x00000000 },
	{ 0x0200c, 0x00400000 }, { 0x02010, 0x00400000 }, { 0x02014, 0x00400000 },
	{ 0x02018, 0x00400000 }, { 0x0201c, 0x00000000 }, { 0x02020, 0x00400000 },
	{ 0x02024, 0x00400000 }, { 0x03000, 0x00000000 }, { 0x03800, 0x000e0003 },
	{ 0x03804, 0x0000000c }, { 0x03808, 0x03eff000 }, { 0x0380c, 0x037fe000 },
	{ 0x03810, 0x037fe000 }, { 0x03838, 0x00000000 }, { 0x0383c, 0x00000000 },
	{ 0x03840, 0x00000000 }, { 0x07418, 0xffffffff }, { 0x0a98c, 0x80000001 },
	{ 0x0a990, 0x80000001 }, { 0x0a994, 0x80000001 }, { 0x0a998, 0x7fffffff },
	{ 0x0a99c, 0x7fffffff }, { 0x0a9a0, 0x7fffffff }, { 0x0a9a4, 0x80000001 },
	{ 0x0a9a8, 0x80000001 }, { 0x0a9ac, 0x80000001 }, { 0x0a9b0, 0x7fffffff },
	{ 0x0a9b4, 0x7fffffff }, { 0x0a9b8, 0x7fffffff }, { 0x13800, 0x00000000 },
	{ 0x13804, 0x00000000 }, { 0x13808, 0x00000000 }, { 0x1380c, 0x00000000 },
	{ 0x13810, 0x0000000a }, { 0x13814, 0x0000002a }, { 0x13818, 0x0000002a },
	{ 0x1381c, 0x00000020 }, { 0x13c00, 0x00000001 }, { 0x13c04, 0x00000000 },
	{ 0x13c10, 0x00000008 }, { 0x13c14, 0x00000008 }, { 0x13c18, 0x00000008 },
	{ 0x13c1c, 0x00000008 }, { 0x16000, 0x00000000 }, { 0x16004, 0x244024c0 },
	{ 0x16080, 0x00000000 }, { 0x16084, 0x0b710367 }, { 0x16088, 0x00000128 },
	{ 0x16504, 0x02000500 }, { 0x16508, 0x00003200 }, { 0x1651c, 0x00851400 },
	{ 0x16534, 0x00000008 }, { 0x16568, 0x00250500 }, { 0x16588, 0x00496513 },
	{ 0x16600, 0x00000015 }, { 0x16800, 0x00000000 }, { 0x25080, 0x00000000 },
	{ 0x2e300, 0x03804038 }, { 0x2e304, 0xa0a0a0a0 }, { 0x2e308, 0x02550000 },
	{ 0x2e30c, 0x02550255 }, { 0x2e310, 0x00000000 }, { 0x2e314, 0x00100008 },
	{ 0x2e318, 0x02550000 },
};


struct asc_job_case {
	const char *name;
	u32 src, dst;
	enum v4l2_ycbcr_encoding enc;
	const struct asc_reg *expect;
	unsigned int n;
};

#define ASC_JOB_CASE(_n, _s, _d, _e, _x) \
	{ _n, _s, _d, _e, _x, ARRAY_SIZE(_x) }

static const struct asc_job_case asc_job_cases[] = {
	ASC_JOB_CASE("NV12 BT.709 to ARGB32", V4L2_PIX_FMT_NV12M, V4L2_PIX_FMT_ARGB32,
		     V4L2_YCBCR_ENC_709, asc_expect_nv12_709_argb),
	ASC_JOB_CASE("ARGB32 to NV12 BT.709", V4L2_PIX_FMT_ARGB32, V4L2_PIX_FMT_NV12M,
		     V4L2_YCBCR_ENC_709, asc_expect_argb_nv12_709),
	ASC_JOB_CASE("P010 BT.2020 to ARGB32", V4L2_PIX_FMT_P010, V4L2_PIX_FMT_ARGB32,
		     V4L2_YCBCR_ENC_BT2020, asc_expect_p010_2020_argb),
};

static void asc_job_case_desc(const struct asc_job_case *c, char *desc)
{
	strscpy(desc, c->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(asc_job, asc_job_cases, asc_job_case_desc);

static u32 asc_expect_get(const struct asc_job_case *c, u32 off)
{
	unsigned int i;

	for (i = 0; i < c->n; i++)
		if (c->expect[i].off == off)
			return c->expect[i].val;
	return 0;
}

static void asc_image_from_expect(const struct asc_job_case *c, struct asc_image *img,
				  u32 fourcc, u32 plane, u32 stride)
{
	unsigned int i;

	img->fmt = asc_find_format(fourcc);
	img->width = 1920;
	img->height = 1080;
	for (i = 0; i < ASC_MAX_PLANES; i++) {
		img->dva[i] = asc_expect_get(c, plane + 8 * i) |
			      (u64)asc_expect_get(c, plane + 8 * i + 4) << 32;
		img->stride[i] = asc_expect_get(c, stride + 4 * i);
	}
}

static bool asc_is_matrix(u32 off)
{
	return off >= ASC_CSC_COEF(0, 0) && off <= ASC_CSC_COEF(2, 2);
}

static void asc_test_job(struct kunit *test)
{
	const struct asc_job_case *c = test->param_value;
	struct asc_reg *w;
	struct asc_csc csc;
	struct asc_job job = {};
	unsigned int i, j;
	int n;

	w = kunit_kcalloc(test, ASC_JOB_MAX_WRITES, sizeof(*w), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, w);

	asc_image_from_expect(c, &job.src, c->src, 0x198, 0x1d8);
	asc_image_from_expect(c, &job.dst, c->dst, 0x290, 0x2c0);
	KUNIT_ASSERT_NOT_NULL(test, job.src.fmt);
	KUNIT_ASSERT_NOT_NULL(test, job.dst.fmt);
	KUNIT_ASSERT_EQ(test, asc_csc_compute(c->enc, V4L2_QUANTIZATION_LIM_RANGE,
					      job.src.fmt->yuv, &csc), 0);
	job.csc = &csc;
	job.transform_id = asc_expect_get(c, ASC_TRANSFORM_ID);
	job.msr20 = true;	/* traced on the J616s (MSR21) */

	n = asc_job_build(&job, w, ASC_JOB_MAX_WRITES);
	KUNIT_ASSERT_GT(test, n, 0);

	/* START last, the pre-start tag right before it. */
	KUNIT_EXPECT_EQ(test, w[n - 1].off, ASC_START);
	KUNIT_EXPECT_EQ(test, w[n - 1].val, 1);
	KUNIT_EXPECT_EQ(test, w[n - 2].off, ASC_PRE_START);

	/* Every register macOS set ends with the same value... */
	for (i = 0; i < c->n; i++) {
		u32 val = 0;
		bool found = false;

		for (j = 0; j < n; j++)
			if (w[j].off == c->expect[i].off) {
				val = w[j].val;
				found = true;
			}
		KUNIT_EXPECT_TRUE_MSG(test, found, "%#x not written", c->expect[i].off);
		KUNIT_EXPECT_EQ_MSG(test, val, c->expect[i].val, "register %#x",
				    c->expect[i].off);
	}
	/* ...and nothing else is written. */
	for (j = 0; j < n; j++) {
		bool known = asc_is_matrix(w[j].off);

		for (i = 0; i < c->n && !known; i++)
			known = c->expect[i].off == w[j].off;
		KUNIT_EXPECT_TRUE_MSG(test, known, "unexpected write %#x", w[j].off);
	}
}

static void asc_test_job_rejects(struct kunit *test)
{
	struct asc_reg *w = kunit_kcalloc(test, ASC_JOB_MAX_WRITES, sizeof(*w), GFP_KERNEL);
	struct asc_csc csc;
	struct asc_job job = {
		.src = { .fmt = asc_find_format(V4L2_PIX_FMT_NV12M), .width = 64, .height = 64 },
		.dst = { .fmt = asc_find_format(V4L2_PIX_FMT_ARGB32), .width = 64, .height = 64 },
	};

	KUNIT_ASSERT_NOT_NULL(test, w);
	/* Y'CbCr to RGB without a matrix */
	KUNIT_EXPECT_EQ(test, asc_job_build(&job, w, ASC_JOB_MAX_WRITES), -EINVAL);
	KUNIT_ASSERT_EQ(test, asc_csc_compute(V4L2_YCBCR_ENC_709,
					      V4L2_QUANTIZATION_LIM_RANGE, true, &csc), 0);
	job.csc = &csc;
	KUNIT_EXPECT_GT(test, asc_job_build(&job, w, ASC_JOB_MAX_WRITES), 0);
	KUNIT_EXPECT_EQ(test, asc_job_build(&job, w, 8), -ENOSPC);
	/* scaling is not handled by this path yet */
	job.dst.width = 32;
	KUNIT_EXPECT_EQ(test, asc_job_build(&job, w, ASC_JOB_MAX_WRITES), -EINVAL);
}

static struct kunit_case asc_test_cases[] = {
	KUNIT_CASE_PARAM(asc_test_csc, asc_csc_gen_params),
	KUNIT_CASE(asc_test_csc_rejects),
	KUNIT_CASE(asc_test_csc_white),
	KUNIT_CASE(asc_test_ratio),
	KUNIT_CASE(asc_test_formats),
	KUNIT_CASE_PARAM(asc_test_job, asc_job_gen_params),
	KUNIT_CASE(asc_test_job_rejects),
	{}
};

static struct kunit_suite asc_test_suite = {
	.name = "apple-scaler",
	.test_cases = asc_test_cases,
};
kunit_test_suite(asc_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("KUnit tests for the Apple scaler/CSC encodings");
MODULE_LICENSE("GPL");
