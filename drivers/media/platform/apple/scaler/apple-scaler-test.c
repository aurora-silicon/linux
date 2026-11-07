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

static struct kunit_case asc_test_cases[] = {
	KUNIT_CASE_PARAM(asc_test_csc, asc_csc_gen_params),
	KUNIT_CASE(asc_test_csc_rejects),
	KUNIT_CASE(asc_test_csc_white),
	KUNIT_CASE(asc_test_ratio),
	KUNIT_CASE(asc_test_formats),
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
