// SPDX-License-Identifier: GPL-2.0-only
/* Apple AVE LFS reference descriptor sizes, not DMA allocation sizes. */
#include <kunit/test.h>
#include <linux/module.h>

#include "ave-sizes.h"

struct ave_size_case {
	u32 width;
	u32 height;
	u32 avc;
	u32 hevc;
	u32 avc_t8140;
	u32 hevc_t8140;
};

static const struct ave_size_case ave_size_cases[] = {
	{ 320, 240, 0x5a00, 0x1400, 0xb400, 0x2800 },
	{ 640, 480, 0x14a00, 0x5000, 0x29400, 0xa000 },
	{ 642, 482, 0x17600, 0x5800, 0x2ec00, 0xb000 },
	{ 1280, 720, 0x3c000, 0xf000, 0x78000, 0x1e000 },
	{ 1920, 1080, 0x85200, 0x1fe00, 0x10a400, 0x3fc00 },
	{ 1920, 1088, 0x85200, 0x1fe00, 0x10a400, 0x3fc00 },
	{ 1922, 1090, 0x8b800, 0x22e00, 0x117000, 0x45c00 },
	{ 2560, 1600, 0x101800, 0x3e800, 0x203000, 0x7d000 },
};

static void ave_size_desc(const struct ave_size_case *c, char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%ux%u", c->width, c->height);
}

KUNIT_ARRAY_PARAM(ave_size, ave_size_cases, ave_size_desc);

static void ave_lfs_ref_test(struct kunit *test)
{
	const struct ave_size_case *c = test->param_value;

	/* T6000 and T8112 both select scale 1; T8140 selects scale 2. */
	KUNIT_EXPECT_EQ(test, ave_lfs_ref_size(c->width, c->height, false, 1), c->avc);
	KUNIT_EXPECT_EQ(test, ave_lfs_ref_size(c->width, c->height, true, 1), c->hevc);
	KUNIT_EXPECT_EQ(test, ave_lfs_ref_size(c->width, c->height, false, 2), c->avc_t8140);
	KUNIT_EXPECT_EQ(test, ave_lfs_ref_size(c->width, c->height, true, 2), c->hevc_t8140);
}

static void ave_6070_working_sizes_test(struct kunit *test)
{
	struct ave_6070_avc_recon_layout recon =
		ave_6070_avc_recon_layout(1920, 1080);

	/* 1080 lines are coded as 1088: 35 tile rows for luma and chroma. */
	KUNIT_EXPECT_EQ(test, recon.luma_data, 0x20d000U);
	KUNIT_EXPECT_EQ(test, recon.chroma_data, 0x106800U);
	KUNIT_EXPECT_EQ(test, recon.luma_metadata, 0x20000U);
	KUNIT_EXPECT_EQ(test, recon.chroma_metadata, 0x8000U);
	KUNIT_EXPECT_EQ(test, recon.luma_data_offset, 0U);
	KUNIT_EXPECT_EQ(test, recon.chroma_data_offset, 0x20d000U);
	KUNIT_EXPECT_EQ(test, recon.luma_metadata_offset, 0x313800U);
	KUNIT_EXPECT_EQ(test, recon.chroma_metadata_offset, 0x333800U);
	KUNIT_EXPECT_EQ(test, recon.bytes, 0x33b800U);
	recon = ave_6070_avc_recon_layout(1922, 1090);
	KUNIT_EXPECT_EQ(test, recon.luma_data, 0x215c00U);
	KUNIT_EXPECT_EQ(test, recon.chroma_data, 0x10ae00U);
	KUNIT_EXPECT_EQ(test, recon.luma_metadata, 0x20000U);
	KUNIT_EXPECT_EQ(test, recon.bytes, 0x348a00U);
	recon = ave_6070_avc_recon_layout(3840, 2160);
	KUNIT_EXPECT_EQ(test, recon.luma_data, 0x7f8000U);
	KUNIT_EXPECT_EQ(test, recon.chroma_data, 0x3fc000U);
	KUNIT_EXPECT_EQ(test, recon.luma_metadata, 0x80000U);
	KUNIT_EXPECT_EQ(test, recon.bytes, 0xc94000U);
	KUNIT_EXPECT_EQ(test, ave_6070_colocated_size(1920, 1080, false), 0xff000U);
	KUNIT_EXPECT_EQ(test, ave_6070_colocated_size(1920, 1080, true), 0x1fe00U);
	KUNIT_EXPECT_EQ(test, ave_6070_lowres_result_size(1920, 1080), 0x20200U);
	KUNIT_EXPECT_EQ(test, ave_6070_src_neighbor_info_size(1920, 1080, false),
			0x8000U);
	KUNIT_EXPECT_EQ(test, ave_6070_src_neighbor_pixel_size(1920, 1080, false),
			0x20000U);
	KUNIT_EXPECT_EQ(test, ave_6070_src_neighbor_info_size(1920, 1080, true),
			0x30000U);
	KUNIT_EXPECT_EQ(test, ave_6070_src_neighbor_pixel_size(1920, 1080, true),
			0xb4000U);
	KUNIT_EXPECT_EQ(test,
			ave_6070_src_neighbor_data_channel_size(1920, false), 0x1a40U);
	KUNIT_EXPECT_EQ(test,
			ave_6070_src_neighbor_data_channel_size(1920, true), 0x100U);
	KUNIT_EXPECT_EQ(test, ave_6070_src_neighbor_data_size(1920, false), 0x4000U);
	KUNIT_EXPECT_EQ(test, ave_6070_src_neighbor_fw_size(1920, false), 0x4000U);
	KUNIT_EXPECT_EQ(test, ave_6070_entropy_channel_size(1920, 1080, false),
			0x1fe000U);
	KUNIT_EXPECT_EQ(test, ave_6070_entropy_channel_size(1920, 1080, true),
			0x23dc00U);
	KUNIT_EXPECT_EQ(test, ave_6070_entropy_aux_size(1920, 1080, false),
			0x1fe000U);
	KUNIT_EXPECT_EQ(test, ave_6070_entropy_aux_size(1920, 1080, true),
			0x23dc00U);
	KUNIT_EXPECT_EQ(test, ave_6070_mb_input_size(1920, 1080), 0x10000U);

	/* Non-aligned dimensions exercise the formula rather than a size table. */
	KUNIT_EXPECT_EQ(test, ave_6070_colocated_size(1922, 1090, false), 0x104e80U);
	KUNIT_EXPECT_EQ(test, ave_6070_colocated_size(1922, 1090, true), 0x22500U);
}

static struct kunit_case ave_sizes_tests[] = {
	KUNIT_CASE_PARAM(ave_lfs_ref_test, ave_size_gen_params),
	KUNIT_CASE(ave_6070_working_sizes_test),
	{ }
};

static struct kunit_suite ave_sizes_suite = {
	.name = "apple-ave-sizes",
	.test_cases = ave_sizes_tests,
};

kunit_test_suite(ave_sizes_suite);

MODULE_DESCRIPTION("Apple AVE buffer sizing tests");
MODULE_LICENSE("GPL");
