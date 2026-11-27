// SPDX-License-Identifier: ISC
#include <kunit/test.h>
#include <linux/sizes.h>

#include "pcie.h"

static void brcmf_pcie_tcm_range(struct kunit *test)
{
	static const struct {
		size_t tcm_size;
		u32 offset;
		u32 len;
		bool valid;
	} cases[] = {
		{ SZ_4M, 0, 8, true },
		{ SZ_4M, SZ_4M - 8, 8, true },
		{ SZ_4M, SZ_4M - 7, 8, false },
		{ SZ_4M, SZ_4M, 8, false },
		{ SZ_4M, SZ_4M, 0, true },
		{ SZ_4M, U32_MAX - 3, 8, false },
		{ SZ_4M, U32_MAX, 8, false },
		{ 4, 0, 8, false },
		{ 0, 0, 1, false },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		KUNIT_EXPECT_EQ_MSG(test,
				    brcmf_pcie_tcm_range_valid(cases[i].tcm_size,
							       cases[i].offset,
							       cases[i].len),
				    cases[i].valid, "case %d", i);
}

static struct kunit_case brcmf_pcie_tcm_cases[] = {
	KUNIT_CASE(brcmf_pcie_tcm_range),
	{}
};

static struct kunit_suite brcmf_pcie_tcm_suite = {
	.name = "brcmfmac-pcie-tcm",
	.test_cases = brcmf_pcie_tcm_cases,
};

kunit_test_suite(brcmf_pcie_tcm_suite);

MODULE_DESCRIPTION("KUnit tests for brcmfmac PCIe TCM bounds");
MODULE_LICENSE("Dual BSD/GPL");
