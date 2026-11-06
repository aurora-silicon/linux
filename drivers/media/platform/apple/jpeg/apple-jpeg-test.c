// SPDX-License-Identifier: GPL-2.0-only
/* KUnit tests for Apple JPEG identity matching and encode job programming. */
#include <kunit/test.h>
#include <linux/module.h>

#include "apple-jpeg-hw.h"

static void apple_jpeg_hwrev_test(struct kunit *test)
{
	/* The stepping nibble (bits 15:12) is ignored. */
	KUNIT_EXPECT_TRUE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t8140, 0x110000));
	KUNIT_EXPECT_TRUE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t8140, 0x11f000));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t8140, 0x110001));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t8140, 0xd1013));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t8140, 0));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t8140, U32_MAX));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t6041, 0x110000));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(NULL, 0x110000));
}

static void apple_jpeg_quant_test(struct kunit *test)
{
	u8 luma[64], chroma[64];

	/* Quality 50 is the Annex K table itself. */
	apple_jpeg_quant_tables(50, luma, chroma);
	KUNIT_EXPECT_EQ(test, luma[0], 16);
	KUNIT_EXPECT_EQ(test, luma[63], 99);
	KUNIT_EXPECT_EQ(test, chroma[0], 17);
	/* Quality 75 halves it, rounding to nearest. */
	apple_jpeg_quant_tables(75, luma, chroma);
	KUNIT_EXPECT_EQ(test, luma[0], 8);
	KUNIT_EXPECT_EQ(test, luma[1], 6);	/* 11 * 50 / 100 = 5.5 */
	KUNIT_EXPECT_EQ(test, chroma[63], 50);
	/* The extremes clamp to 1..255. */
	apple_jpeg_quant_tables(100, luma, chroma);
	KUNIT_EXPECT_EQ(test, luma[0], 1);
	apple_jpeg_quant_tables(1, luma, chroma);
	KUNIT_EXPECT_EQ(test, luma[0], 255);
	KUNIT_EXPECT_EQ(test, chroma[63], 255);
}

static struct apple_jpeg_job apple_jpeg_test_job(void)
{
	return (struct apple_jpeg_job){
		.width = 1920, .height = 1080,
		.luma_stride = 1920, .chroma_stride = 1920,
		.luma = 0x10000100000ULL, .chroma = 0x10000400000ULL,
		.input_end = 0x100004fffffULL,
		.output = 0x10001000000ULL, .output_size = 0x800000,
		.quality = 75,
	};
}

static u32 apple_jpeg_last(const struct apple_jpeg_write *w, int n, u32 offset)
{
	u32 value = 0xdeadbeef;

	for (int i = 0; i < n; i++)
		if (w[i].offset == offset)
			value = w[i].value;
	return value;
}

static void apple_jpeg_job_420_test(struct kunit *test)
{
	struct apple_jpeg_write *w = kunit_kcalloc(test, APPLE_JPEG_MAX_WRITES, sizeof(*w),
						   GFP_KERNEL);
	struct apple_jpeg_job job = apple_jpeg_test_job();
	int n;

	KUNIT_ASSERT_NOT_NULL(test, w);
	n = apple_jpeg_build_job(&job, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_ASSERT_GT(test, n, 0);
	/* Mode opens first; the start command is the very last write. */
	KUNIT_EXPECT_EQ(test, w[0].offset, AJ_MODE);
	KUNIT_EXPECT_EQ(test, w[0].value, AJ_MODE_SETUP);
	KUNIT_EXPECT_EQ(test, w[n - 2].offset, AJ_MAIN_RUN);
	KUNIT_EXPECT_EQ(test, w[n - 2].value, 1);
	KUNIT_EXPECT_EQ(test, w[n - 1].offset, AJ_COMMAND);
	KUNIT_EXPECT_EQ(test, w[n - 1].value, 1);

	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_CODEC), AJ_CODEC_420);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_MCUS_W), 120);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_MCUS_H), 68);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_PLANE0_TILE_V), 16);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_PLANE1_HEIGHT_M1), 539);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_INPUT_PLANE0), 0x00100000);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_INPUT_PLANE0 + 4), 0x100);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_INPUT_END), 0x004fffff);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_OUTPUT_START1), 0x01000000);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_OUTPUT_END), 0x017fffff);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_ENCODE_PIPE_LIMIT0), 3);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_JPEG_IO_FLAGS), 0x32);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_TIMEOUT), 0x03000000);	/* floor */
	/* Table slot 0 holds luma 8, 6, 5, 8 at quality 75, MSB first. */
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_QTBL(0)), 0x08060508);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_QTBL_SEL), 0x14);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_IRQ_MASK), 0xff);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_TILING_ENABLE), 1);
}

static void apple_jpeg_job_422_test(struct kunit *test)
{
	struct apple_jpeg_write *w = kunit_kcalloc(test, APPLE_JPEG_MAX_WRITES, sizeof(*w),
						   GFP_KERNEL);
	struct apple_jpeg_job job = apple_jpeg_test_job();
	int n;

	KUNIT_ASSERT_NOT_NULL(test, w);
	job.is422 = true;
	job.width = 641;	/* odd width: chroma rounds up */
	job.luma_stride = 656;
	job.chroma_stride = 656;
	n = apple_jpeg_build_job(&job, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_ASSERT_GT(test, n, 0);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_CODEC), AJ_CODEC_422);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_MCUS_W), 41);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_MCUS_H), 135);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_PLANE1_WIDTH_M1), 641);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_PLANE1_HEIGHT_M1), 1079);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_ENCODE_PIPE_LIMIT0), 8);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_ENCODE_PIPE_LIMIT1), 2);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_JPEG_IO_FLAGS), 0x31);
	/* Large pictures get three cycles per input byte. */
	KUNIT_EXPECT_EQ(test, apple_jpeg_timeout(8192, 8192), 3 * 8192 * 8192);
}

static void apple_jpeg_job_reject_test(struct kunit *test)
{
	struct apple_jpeg_write *w = kunit_kcalloc(test, APPLE_JPEG_MAX_WRITES, sizeof(*w),
						   GFP_KERNEL);
	struct apple_jpeg_job job;

	KUNIT_ASSERT_NOT_NULL(test, w);
	job = apple_jpeg_test_job();
	job.luma_stride = 1000;
	KUNIT_EXPECT_EQ(test, apple_jpeg_build_job(&job, w, APPLE_JPEG_MAX_WRITES), -EINVAL);
	job = apple_jpeg_test_job();
	job.output += 16;
	KUNIT_EXPECT_EQ(test, apple_jpeg_build_job(&job, w, APPLE_JPEG_MAX_WRITES), -EINVAL);
	job = apple_jpeg_test_job();
	job.input_end = job.chroma - 1;
	KUNIT_EXPECT_EQ(test, apple_jpeg_build_job(&job, w, APPLE_JPEG_MAX_WRITES), -EINVAL);
	job = apple_jpeg_test_job();
	KUNIT_EXPECT_EQ(test, apple_jpeg_build_job(&job, w, 10), -ENOSPC);
}

static struct kunit_case apple_jpeg_cases[] = {
	KUNIT_CASE(apple_jpeg_hwrev_test),
	KUNIT_CASE(apple_jpeg_quant_test),
	KUNIT_CASE(apple_jpeg_job_420_test),
	KUNIT_CASE(apple_jpeg_job_422_test),
	KUNIT_CASE(apple_jpeg_job_reject_test),
	{ }
};

static struct kunit_suite apple_jpeg_suite = {
	.name = "apple-jpeg",
	.test_cases = apple_jpeg_cases,
};
kunit_test_suite(apple_jpeg_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple JPEG encoder");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
