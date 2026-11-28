// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/module.h>
#include <linux/sizes.h>

#include "ane_t6021_fwmap.h"

#define RW	(IOMMU_READ | IOMMU_WRITE)

/* Every 16 KiB page of the pinned __TEXT is read-only for the engine, every
 * page from the DATA vmaddr to the end of the allocation stays read-write.
 */
static void fw_prot_image(struct kunit *test, const struct ane_fw_image *img)
{
	const struct ane_fw_seg *text = &img->segs[0];
	u64 end = ANE_FW_BUF_SIZE + SZ_2M;
	u64 off;

	KUNIT_ASSERT_STREQ(test, text->name, "__TEXT");
	KUNIT_ASSERT_EQ(test, img->segs[1].vmaddr, text->vmaddr + text->vmsize);
	for (off = 0; off < end; off += SZ_16K) {
		int want = off < text->vmsize ? IOMMU_READ : RW;

		KUNIT_EXPECT_EQ_MSG(test, ane_t6021_fw_prot(img->segs, ANE_FW_NSEGS,
							    off, SZ_16K, RW),
				    want, "%s +%#llx", img->name, off);
		KUNIT_EXPECT_EQ_MSG(test, ane_t6021_fw_prot(img->segs, ANE_FW_NSEGS,
							    off, SZ_16K,
							    RW | IOMMU_CACHE),
				    want | IOMMU_CACHE, "%s +%#llx", img->name, off);
	}
}

static void fw_prot_selene_test(struct kunit *test)
{
	fw_prot_image(test, &ane_fw_selene);
}

static void fw_prot_bia_test(struct kunit *test)
{
	fw_prot_image(test, &ane_fw_bia);
}

/* TEXT is found by name, wherever the table puts it; a range that is not
 * wholly inside it (here one that runs into DATA) stays writable.
 */
static void fw_prot_by_table_test(struct kunit *test)
{
	static const struct ane_fw_seg segs[ANE_FW_NSEGS] = {
		{ 0x000000, 0x100000, 0x004000, 0x100000, "__DATA" },
		{ 0x100000, 0x008000, 0x104000, 0x008000, "__TEXT" },
	};

	KUNIT_EXPECT_EQ(test, ane_t6021_fw_prot(segs, ANE_FW_NSEGS, 0, SZ_16K, RW), RW);
	KUNIT_EXPECT_EQ(test, ane_t6021_fw_prot(segs, ANE_FW_NSEGS, 0x100000,
						SZ_16K, RW), IOMMU_READ);
	KUNIT_EXPECT_EQ(test, ane_t6021_fw_prot(segs, ANE_FW_NSEGS, 0x104000,
						SZ_16K, RW), IOMMU_READ);
	KUNIT_EXPECT_EQ(test, ane_t6021_fw_prot(segs, ANE_FW_NSEGS, 0x0fc000,
						SZ_16K, RW), RW);
	KUNIT_EXPECT_EQ(test, ane_t6021_fw_prot(segs, ANE_FW_NSEGS, 0x104000,
						SZ_32K, RW), RW);
	KUNIT_EXPECT_EQ(test, ane_t6021_fw_prot(segs, ANE_FW_NSEGS, 0x108000,
						SZ_16K, RW), RW);
	KUNIT_EXPECT_EQ(test, ane_t6021_fw_prot(segs, ANE_FW_NSEGS, U64_MAX - 1,
						SZ_16K, RW), RW);
	KUNIT_EXPECT_EQ(test, ane_t6021_fw_prot(segs, 1, 0x100000, SZ_16K, RW), RW);
}

static struct kunit_case fwmap_cases[] = {
	KUNIT_CASE(fw_prot_selene_test),
	KUNIT_CASE(fw_prot_bia_test),
	KUNIT_CASE(fw_prot_by_table_test),
	{}
};

static struct kunit_suite fwmap_suite = {
	.name = "ane-t6021-fwmap",
	.test_cases = fwmap_cases,
};

kunit_test_suite(fwmap_suite);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("ANE H14 staged firmware mapping protection tests");
