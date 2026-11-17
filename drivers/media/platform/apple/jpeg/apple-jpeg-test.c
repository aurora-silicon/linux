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
	KUNIT_EXPECT_TRUE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t6041, 0x101008));
	KUNIT_EXPECT_TRUE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t6041, 0x10f008));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t6041, 0x101009));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t8140, 0x101008));
	KUNIT_EXPECT_TRUE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t6050, 0x111010));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t6050, 0x110000));
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t8140, 0x111010));
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
	n = apple_jpeg_build_job(apple_jpeg_t8140.layout, &job, w, APPLE_JPEG_MAX_WRITES);
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
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x090), 0x00100000);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x090 + 4), 0x100);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0b0), 0x004fffff);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0d0), 0x01000000);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0e0), 0x017fffff);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0b8), 3);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_JPEG_IO_FLAGS), 0x32);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x1c0), 0x03000000);	/* floor */
	/* Table slot 0 holds luma 8, 6, 5, 8 at quality 75, MSB first. */
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_QTBL(0)), 0x08060508);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_QTBL_SEL), 0x14);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_IRQ_MASK), 0xff);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x19c), 1);
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
	n = apple_jpeg_build_job(apple_jpeg_t8140.layout, &job, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_ASSERT_GT(test, n, 0);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_CODEC), AJ_CODEC_422);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_MCUS_W), 41);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_MCUS_H), 135);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_PLANE1_WIDTH_M1), 641);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_PLANE1_HEIGHT_M1), 1079);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0b8), 8);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0bc), 2);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_JPEG_IO_FLAGS), 0x31);
	/* Large pictures get three cycles per input byte. */
	KUNIT_EXPECT_EQ(test, apple_jpeg_timeout(8192, 8192), 3 * 8192 * 8192);
}

static void apple_jpeg_job_reject_test(struct kunit *test)
{
	struct apple_jpeg_write *w = kunit_kcalloc(test, APPLE_JPEG_MAX_WRITES, sizeof(*w),
						   GFP_KERNEL);
	const struct apple_jpeg_reg_layout *layout = apple_jpeg_t8140.layout;
	struct apple_jpeg_job job;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, w);
	job = apple_jpeg_test_job();
	job.luma_stride = 1000;
	ret = apple_jpeg_build_job(layout, &job, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	job = apple_jpeg_test_job();
	job.output += 16;
	ret = apple_jpeg_build_job(layout, &job, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	job = apple_jpeg_test_job();
	job.input_end = job.chroma - 1;
	ret = apple_jpeg_build_job(layout, &job, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	job = apple_jpeg_test_job();
	KUNIT_EXPECT_EQ(test, apple_jpeg_build_job(layout, &job, w, 10), -ENOSPC);
}

static void apple_jpeg_layout_test(struct kunit *test)
{
	const struct apple_jpeg_reg_layout *wide = apple_jpeg_t8140.layout;
	const struct apple_jpeg_reg_layout *legacy = apple_jpeg_t6000.layout;

	KUNIT_EXPECT_PTR_EQ(test, apple_jpeg_t8112.layout, wide);
	KUNIT_EXPECT_PTR_EQ(test, apple_jpeg_t6041.layout, wide);
	KUNIT_EXPECT_PTR_EQ(test, apple_jpeg_t8122.layout, wide);
	KUNIT_EXPECT_PTR_NE(test, legacy, wide);
	KUNIT_EXPECT_TRUE(test, wide->reset_complete);
	KUNIT_EXPECT_TRUE(test, legacy->reset_complete);
	KUNIT_EXPECT_EQ(test, legacy->num_defaults, 111);
	KUNIT_EXPECT_EQ(test, wide->dma_addr_bits, 64);
	KUNIT_EXPECT_EQ(test, legacy->dma_addr_bits, 32);
	KUNIT_EXPECT_EQ(test, wide->input_plane0, 0x090);
	KUNIT_EXPECT_EQ(test, wide->input_plane1, 0x0a0);
	KUNIT_EXPECT_EQ(test, wide->input_end, 0x0b0);
	KUNIT_EXPECT_EQ(test, wide->pipe_limit0, 0x0b8);
	KUNIT_EXPECT_EQ(test, wide->pipe_limit1, 0x0bc);
	KUNIT_EXPECT_EQ(test, wide->output_start0, 0x0c0);
	KUNIT_EXPECT_EQ(test, wide->output_start1, 0x0d0);
	KUNIT_EXPECT_EQ(test, wide->output_end, 0x0e0);
	KUNIT_EXPECT_EQ(test, wide->matrix_base, 0x0e8);
	KUNIT_EXPECT_EQ(test, wide->encode_format, 0x13c);
	KUNIT_EXPECT_EQ(test, wide->component_pos_base, 0x140);
	KUNIT_EXPECT_EQ(test, wide->convert_cs, 0x150);
	KUNIT_EXPECT_EQ(test, wide->prefetch_read, 0x154);
	KUNIT_EXPECT_EQ(test, wide->prefetch_write, 0x158);
	KUNIT_EXPECT_EQ(test, wide->tiling, 0x19c);
	KUNIT_EXPECT_EQ(test, wide->timeout, 0x1c0);
	KUNIT_EXPECT_EQ(test, wide->hwrev, 0x1c4);
	KUNIT_EXPECT_EQ(test, wide->image_width, 0x258);
	KUNIT_EXPECT_EQ(test, wide->image_height, 0x25c);
	KUNIT_EXPECT_EQ(test, wide->decode_block_base, 0x16c);
	KUNIT_EXPECT_EQ(test, legacy->input_plane0, 0x08c);
	KUNIT_EXPECT_EQ(test, legacy->input_plane1, 0x090);
	KUNIT_EXPECT_EQ(test, legacy->input_end, 0x09c);
	KUNIT_EXPECT_EQ(test, legacy->pipe_limit0, 0x094);
	KUNIT_EXPECT_EQ(test, legacy->pipe_limit1, 0x098);
	KUNIT_EXPECT_EQ(test, legacy->output_start0, 0x0a0);
	KUNIT_EXPECT_EQ(test, legacy->output_start1, 0x0a4);
	KUNIT_EXPECT_EQ(test, legacy->output_end, 0x0a8);
	KUNIT_EXPECT_EQ(test, legacy->matrix_base, 0x0ac);
	KUNIT_EXPECT_EQ(test, legacy->encode_format, 0x100);
	KUNIT_EXPECT_EQ(test, legacy->component_pos_base, 0x104);
	KUNIT_EXPECT_EQ(test, legacy->convert_cs, 0x114);
	KUNIT_EXPECT_EQ(test, legacy->prefetch_read, 0x118);
	KUNIT_EXPECT_EQ(test, legacy->prefetch_write, 0x11c);
	KUNIT_EXPECT_EQ(test, legacy->tiling, 0x160);
	KUNIT_EXPECT_EQ(test, legacy->timeout, 0x184);
	KUNIT_EXPECT_EQ(test, legacy->hwrev, 0x188);
	KUNIT_EXPECT_EQ(test, legacy->image_width, 0x20c);
	KUNIT_EXPECT_EQ(test, legacy->image_height, 0x210);
	KUNIT_EXPECT_EQ(test, legacy->decode_block_base, 0x130);
	KUNIT_EXPECT_EQ(test, apple_jpeg_t8140.int_done_mask, BIT(31));
	KUNIT_EXPECT_EQ(test, apple_jpeg_t6050.int_done_mask, BIT(31));
	KUNIT_EXPECT_EQ(test, apple_jpeg_t8112.int_done_mask, BIT(31) | BIT(30));
	KUNIT_EXPECT_EQ(test, apple_jpeg_t8122.int_done_mask, BIT(31) | BIT(30));
	KUNIT_EXPECT_EQ(test, apple_jpeg_t6000.int_done_mask, BIT(31));
}

/* T6050: T8140's register map, its own reset image and per-engine DMA identifiers. */
static void apple_jpeg_t6050_layout_test(struct kunit *test)
{
	const struct apple_jpeg_reg_layout *wide = apple_jpeg_t8140.layout;
	const struct apple_jpeg_reg_layout *l = apple_jpeg_t6050.layout;
	struct apple_jpeg_write w[16];
	int n;

	KUNIT_EXPECT_PTR_NE(test, l, wide);
	KUNIT_EXPECT_EQ(test, l->input_plane0, wide->input_plane0);
	KUNIT_EXPECT_EQ(test, l->output_start0, wide->output_start0);
	KUNIT_EXPECT_EQ(test, l->output_end, wide->output_end);
	KUNIT_EXPECT_EQ(test, l->hwrev, wide->hwrev);
	KUNIT_EXPECT_EQ(test, l->image_height, wide->image_height);
	KUNIT_EXPECT_EQ(test, l->dma_addr_bits, 64);
	KUNIT_EXPECT_EQ(test, l->num_defaults, 124);
	KUNIT_EXPECT_EQ(test, l->defaults[l->num_defaults - 1].offset, 0x020);
	KUNIT_EXPECT_EQ(test, wide->num_dma_ids, 0);

	n = apple_jpeg_dma_ids(l, 0, w, ARRAY_SIZE(w));
	KUNIT_ASSERT_EQ(test, n, 12);
	KUNIT_EXPECT_EQ(test, w[0].offset, 0x098);
	KUNIT_EXPECT_EQ(test, w[0].value, 1);
	KUNIT_EXPECT_EQ(test, w[11].value, 8);
	n = apple_jpeg_dma_ids(l, 1, w, ARRAY_SIZE(w));
	KUNIT_ASSERT_EQ(test, n, 12);
	KUNIT_EXPECT_EQ(test, w[0].value, 9);
	KUNIT_EXPECT_EQ(test, w[2].value, 0x7f);	/* shared, not per engine */
	KUNIT_EXPECT_EQ(test, w[6].value, 8);
	KUNIT_EXPECT_EQ(test, w[11].value, 16);
	KUNIT_EXPECT_EQ(test, apple_jpeg_dma_ids(l, 0, w, 11), -ENOSPC);
	KUNIT_EXPECT_FALSE(test, apple_jpeg_has_422(l));
	KUNIT_EXPECT_TRUE(test, apple_jpeg_has_422(wide));
}

/* The first write to @offset at or after @from, or -1. */
static int apple_jpeg_index(const struct apple_jpeg_write *w, int n, int from, u32 offset)
{
	for (int i = from; i < n; i++)
		if (w[i].offset == offset)
			return i;
	return -1;
}

/* T6050 jobs: macOS's sequence, with 16-bit tables and no plane size registers. */
static void apple_jpeg_t6050_job_test(struct kunit *test)
{
	struct apple_jpeg_write *w = kunit_kcalloc(test, APPLE_JPEG_MAX_WRITES, sizeof(*w),
						   GFP_KERNEL);
	const struct apple_jpeg_reg_layout *l = apple_jpeg_t6050.layout;
	struct apple_jpeg_job job = apple_jpeg_test_job();
	static const u32 absent[] = { 0x064, 0x068, 0x078, 0x07c, 0x150, AJ_QTBL16(2) };
	int n;

	KUNIT_ASSERT_NOT_NULL(test, w);
	job.quality = 50;
	/* Tiling sits inside this reset image: set in place, nothing appended. */
	n = apple_jpeg_defaults(l, true, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_ASSERT_EQ(test, n, 124);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, l->tiling), 1);
	n = apple_jpeg_defaults(l, false, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, l->tiling), 0);

	n = apple_jpeg_build_job(l, &job, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_ASSERT_EQ(test, n, 238);
	KUNIT_EXPECT_EQ(test, w[0].offset, AJ_MODE);
	KUNIT_EXPECT_EQ(test, w[0].value, AJ_MODE_SETUP);
	KUNIT_EXPECT_EQ(test, w[126].offset, AJ_MODE);
	KUNIT_EXPECT_EQ(test, w[126].value, AJ_MODE_OPEN);
	KUNIT_EXPECT_EQ(test, w[n - 2].offset, AJ_MAIN_RUN);
	KUNIT_EXPECT_EQ(test, w[n - 1].offset, AJ_COMMAND);
	for (unsigned int i = 0; i < ARRAY_SIZE(absent); i++)
		KUNIT_EXPECT_EQ(test, apple_jpeg_index(w, n, 0, absent[i]), -1);

	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_CODEC), AJ_CODEC_420);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_T6050_PX_MODE), 3);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_T6050_ENC_CTRL), 0x35);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_MCUS_H), 68);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_PX_PLANE0_STRIDE), 1920);
	/* The pipe limits keep the reset image's value. */
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0b8), 0x7f);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0bc), 0x7f);
	/* Low address words first, then the high ones. */
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0e0), 0x017fffff);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0e4), 0x100);
	KUNIT_EXPECT_LT(test, apple_jpeg_index(w, n, 127, 0x0e0),
			apple_jpeg_index(w, n, 127, 0x094));
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_JPEG_IO_FLAGS), 0xb2);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_HUFFMAN_TABLE), 0x550);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_QTBL_SEL), 0x14);
	/* Annex K at quality 50, two entries per word, the first one high. */
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_QTBL16(0)), 0x0010000b);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_QTBL16(0) + 0x7c), 0x00670063);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_QTBL16(1)), 0x00110012);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_QTBL16(1) + 0x7c), 0x00630063);

	job.is422 = true;
	KUNIT_EXPECT_EQ(test, apple_jpeg_build_job(l, &job, w, APPLE_JPEG_MAX_WRITES),
			-EOPNOTSUPP);
}

static void apple_jpeg_older_hwrev_test(struct kunit *test)
{
	const struct apple_jpeg_variant *variants[] = {
		&apple_jpeg_t6000, &apple_jpeg_t6041, &apple_jpeg_t6050,
		&apple_jpeg_t8112, &apple_jpeg_t8140,
		&apple_jpeg_t8122,
	};
	const u32 raw[] = { 0x0d1013, 0x101008, 0x111010, 0x0e0008, 0x110000, 0x0f0008 };

	for (unsigned int i = 0; i < ARRAY_SIZE(variants); i++) {
		KUNIT_EXPECT_EQ(test, variants[i]->hwrev_mask, 0xffff0fff);
		for (unsigned int nibble = 0; nibble < 16; nibble++) {
			u32 hwrev = (raw[i] & 0xffff0fff) | (nibble << 12);

			KUNIT_EXPECT_TRUE(test, apple_jpeg_hwrev_supported(variants[i], hwrev));
		}
		for (unsigned int j = 0; j < ARRAY_SIZE(raw); j++)
			KUNIT_EXPECT_EQ(test, apple_jpeg_hwrev_supported(variants[i], raw[j]),
					i == j);
		KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(variants[i], raw[i] ^ 1));
		KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(variants[i], 0));
		KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(variants[i], U32_MAX));
	}
	/* The legacy selector is not in the HWREV namespace. */
	KUNIT_EXPECT_FALSE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t6000, 0x0a0103));
	KUNIT_EXPECT_TRUE(test, apple_jpeg_hwrev_supported(&apple_jpeg_t6000, 0x0d2013));
}

static void apple_jpeg_legacy_job_test(struct kunit *test)
{
	struct apple_jpeg_write *w = kunit_kcalloc(test, APPLE_JPEG_MAX_WRITES,
						 sizeof(*w), GFP_KERNEL);
	struct apple_jpeg_job job = apple_jpeg_test_job();
	int n;

	KUNIT_ASSERT_NOT_NULL(test, w);
	job.luma = 0x100000;
	job.chroma = 0x400000;
	job.input_end = 0x4ffff8;
	job.output = 0x1000000;
	for (unsigned int is422 = 0; is422 < 2; is422++) {
		job.is422 = is422;
		n = apple_jpeg_build_job(apple_jpeg_t6000.layout, &job, w,
					 APPLE_JPEG_MAX_WRITES);
		KUNIT_ASSERT_GT(test, n, 0);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x08c), 0x100000);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x090), 0x400000);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x094), is422 ? 8 : 3);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x098), is422 ? 2 : 1);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x09c), 0x4fffff);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0a0), 0x1000000);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0a4), 0x1000000);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0a8), 0x17fffff);
		/* One default and one job write for each single-word pointer. */
		for (unsigned int pointer = 0; pointer < 6; pointer++) {
			const u32 offsets[] = { 0x08c, 0x090, 0x09c, 0x0a0, 0x0a4, 0x0a8 };
			unsigned int count = 0;

			for (int i = 0; i < n; i++)
				count += w[i].offset == offsets[pointer];
			KUNIT_EXPECT_EQ(test, count, 2);
		}
		/* A spurious output high half would overwrite the matrix. */
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0ac), 0);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x100), 4);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x10c), 1);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x114), 0);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x118), 1);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x11c), 0);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x160), 1);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x184), 0x03000000);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x20c), 1920);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x210), 1080);
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, AJ_JPEG_IO_FLAGS), is422 ? 0x31 : 0x32);
		KUNIT_EXPECT_EQ(test, w[n - 1].offset, AJ_COMMAND);
	}
	/* Last representable inclusive boundaries are valid. */
	job.input_end = U32_MAX;
	job.output = 0xff800000;
	n = apple_jpeg_build_job(apple_jpeg_t6000.layout, &job, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_ASSERT_GT(test, n, 0);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0a8), U32_MAX);
}

static void apple_jpeg_defaults_test(struct kunit *test)
{
	struct apple_jpeg_write *w = kunit_kcalloc(test, APPLE_JPEG_MAX_WRITES,
						 sizeof(*w), GFP_KERNEL);
	const struct apple_jpeg_reg_layout *wide = apple_jpeg_t8140.layout;
	const struct apple_jpeg_reg_layout *legacy = apple_jpeg_t6000.layout;
	int n;

	KUNIT_ASSERT_NOT_NULL(test, w);
	n = apple_jpeg_defaults(wide, false, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_ASSERT_GT(test, n, 0);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0b0), U32_MAX);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0b4), 0xf);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0e4), 0xf);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x19c), 0);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x280), 0x1f40);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x294), 7);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x2b0), 7);
	/* Neither reset image may write its read-only identity register. */
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x1c4), 0xdeadbeef);
	n = apple_jpeg_defaults(legacy, true, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_ASSERT_GT(test, n, 0);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x09c), U32_MAX);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x0a8), U32_MAX);
	for (u32 off = 0x0d8; off <= 0x0fc; off += 4)
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, off), 0xff);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x160), 1);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x188), 0xdeadbeef);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x120), 0xdeadbeef);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x124), 0xdeadbeef);
	/* Wide reset literals must not leak into the legacy image. */
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x280), 0xdeadbeef);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x2b0), 0xdeadbeef);
	/* The sparse scratch block: one non-zero default, holes stay holes. */
	KUNIT_EXPECT_EQ(test, n, 112);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x234), 0x1f40);
	for (u32 off = 0x1fc; off <= 0x25c; off += 4) {
		if (off == 0x20c || off == 0x210 || off == 0x234 || off == 0x238 ||
		    off == 0x24c)
			continue;
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, off), 0);
	}
	for (u32 off = 0x1a0; off <= 0x1d4; off += 4) {
		if (off == 0x1a4 || off == 0x1b8)
			continue;
		KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, off), 0);
	}
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x1a4), 0xdeadbeef);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x1b8), 0xdeadbeef);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x20c), 0xdeadbeef);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x210), 0xdeadbeef);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x238), 0xdeadbeef);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x24c), 0xdeadbeef);
	n = apple_jpeg_defaults(legacy, false, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_EXPECT_EQ(test, n, 112);
	KUNIT_EXPECT_EQ(test, apple_jpeg_last(w, n, 0x160), 0);
	KUNIT_EXPECT_EQ(test, apple_jpeg_defaults(legacy, true, w, 111), -ENOSPC);
	KUNIT_EXPECT_EQ(test, apple_jpeg_defaults(wide, true, w, 1), -ENOSPC);
	KUNIT_EXPECT_EQ(test, apple_jpeg_defaults(legacy, true, w, 1), -ENOSPC);
}

static void apple_jpeg_address_reject_test(struct kunit *test)
{
	struct apple_jpeg_write *w = kunit_kcalloc(test, APPLE_JPEG_MAX_WRITES,
						 sizeof(*w), GFP_KERNEL);
	struct apple_jpeg_job job = apple_jpeg_test_job();
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, w);
	/* Each address and inclusive end must fit, without truncation. */
	job.luma = 0x100000;
	job.chroma = 0x400000;
	job.input_end = 0x4fffff;
	job.output = 0x1000000;
	for (unsigned int i = 0; i < 5; i++) {
		struct apple_jpeg_job bad = job;

		switch (i) {
		case 0:
			bad.luma |= BIT_ULL(32);
			bad.input_end = bad.luma + 0x500000;
			break;
		case 1:
			bad.chroma |= BIT_ULL(32);
			bad.input_end = bad.chroma + 0x500000;
			break;
		case 2:
			bad.input_end |= BIT_ULL(32);
			break;
		case 3:
			bad.output |= BIT_ULL(32);
			break;
		default:
			bad.output = 0xffffffe0;
			break;
		}
		w[0].value = 0xdeadbeef;
		ret = apple_jpeg_build_job(apple_jpeg_t6000.layout, &bad, w,
					   APPLE_JPEG_MAX_WRITES);
		KUNIT_EXPECT_EQ(test, ret, -ERANGE);
		KUNIT_EXPECT_EQ(test, w[0].value, 0xdeadbeef);
	}
	job.output = U64_MAX & ~31ULL;
	ret = apple_jpeg_build_job(apple_jpeg_t8140.layout, &job, w, APPLE_JPEG_MAX_WRITES);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
}

/*
 * One buffer: chroma straight after the visible luma rows, then chroma up to
 * the MCU boundary. 1080p NV12: chroma at 1080 rows, 544 chroma rows.
 */
static void apple_jpeg_raw_layout_test(struct kunit *test)
{
	static const struct {
		u32 w, h;
		bool is422;
		u32 bpl, chroma, size;
	} cases[] = {
		{ 1920, 1080, false, 1920, 1920 * 1080, 1920 * (1080 + 544) },
		{ 1280, 720, false, 1280, 1280 * 720, 1280 * (720 + 360) },
		{ 640, 480, false, 640, 640 * 480, 640 * (480 + 240) },
		{ 3840, 2160, false, 3840, 3840 * 2160, 3840 * (2160 + 1080) },
		/* Odd height and a width that pads the stride to 16. */
		{ 642, 481, false, 656, 656 * 481, 656 * (481 + 248) },
		/* 4:2:2 MCUs are 8 rows high and chroma has every row. */
		{ 1920, 1084, true, 1920, 1920 * 1084, 1920 * (1084 + 1088) },
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
		struct apple_jpeg_layout l;

		apple_jpeg_raw_layout(cases[i].w, cases[i].h, cases[i].is422, &l);
		KUNIT_EXPECT_EQ(test, l.bytesperline, cases[i].bpl);
		KUNIT_EXPECT_EQ(test, l.chroma_offset, cases[i].chroma);
		KUNIT_EXPECT_EQ(test, l.sizeimage, cases[i].size);
		/* The 4-byte start alignment of luma carries to chroma. */
		KUNIT_EXPECT_EQ(test, l.chroma_offset & 3, 0U);
	}
}

/* A kept CAPTURE size survives OUTPUT changes, within the limit. */
static void apple_jpeg_coded_size_test(struct kunit *test)
{
	u32 need720 = PAGE_ALIGN(1280 * 720 * 3 + SZ_16K);
	u32 need1080 = PAGE_ALIGN(1920 * 1080 * 3 + SZ_16K);

	KUNIT_EXPECT_EQ(test, apple_jpeg_coded_size(1280, 720, 0), need720);
	KUNIT_EXPECT_EQ(test, apple_jpeg_coded_size(1280, 720, 4096), need720);
	/* CAPTURE was sized at the 1080p default before OUTPUT moved to 720p. */
	KUNIT_EXPECT_EQ(test, apple_jpeg_coded_size(1280, 720, need1080), need1080);
	KUNIT_EXPECT_EQ(test, apple_jpeg_coded_size(1280, 720, need1080 - 1), need1080);
	KUNIT_EXPECT_EQ(test, apple_jpeg_coded_size(1280, 720, U32_MAX),
			(u32)APPLE_JPEG_CODED_LIMIT);
	/* The worst case at the largest size is above the limit and wins. */
	KUNIT_EXPECT_EQ(test, apple_jpeg_coded_size(16384, 16384, U32_MAX),
			(u32)PAGE_ALIGN(16384ULL * 16384 * 3 + SZ_16K));
}

static struct kunit_case apple_jpeg_cases[] = {
	KUNIT_CASE(apple_jpeg_hwrev_test),
	KUNIT_CASE(apple_jpeg_layout_test),
	KUNIT_CASE(apple_jpeg_older_hwrev_test),
	KUNIT_CASE(apple_jpeg_t6050_layout_test),
	KUNIT_CASE(apple_jpeg_t6050_job_test),
	KUNIT_CASE(apple_jpeg_legacy_job_test),
	KUNIT_CASE(apple_jpeg_address_reject_test),
	KUNIT_CASE(apple_jpeg_defaults_test),
	KUNIT_CASE(apple_jpeg_quant_test),
	KUNIT_CASE(apple_jpeg_job_420_test),
	KUNIT_CASE(apple_jpeg_job_422_test),
	KUNIT_CASE(apple_jpeg_job_reject_test),
	KUNIT_CASE(apple_jpeg_raw_layout_test),
	KUNIT_CASE(apple_jpeg_coded_size_test),
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
