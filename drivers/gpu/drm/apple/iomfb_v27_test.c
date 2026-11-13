// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Aurora Silicon */

#include <kunit/test.h>
#include <linux/module.h>

#include "dcp.h"
#include "iomfb.h"
#include "iomfb_v27.h"

static void v27_method_table_test(struct kunit *test)
{
	const struct dcp_method_entry *methods;
	unsigned int missing;

	methods = iomfb_v27_methods("27.2.0");
	KUNIT_ASSERT_NOT_NULL(test, methods);
	KUNIT_EXPECT_MEMEQ(test, methods[dcpep_dfb_ready_query].tag, "A385", 4);
	KUNIT_EXPECT_PTR_EQ(test, iomfb_v27_missing_method(methods, &missing), NULL);
	KUNIT_EXPECT_EQ(test, missing, 0U);
	KUNIT_EXPECT_PTR_EQ(test, iomfb_v27_methods("27.1.0"), NULL);
}

static void v27_d590_callback_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, iomfb_v27_callback_size_valid(590, 20, 0));
	KUNIT_EXPECT_FALSE(test, iomfb_v27_callback_size_valid(590, 19, 0));
}

static void t6041_firmware_admission_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
			dcp_t6041_firmware_version("26.5.2", "26.5.2"),
			DCP_FIRMWARE_V_26_6_DESKTOP);
	KUNIT_EXPECT_EQ(test,
			dcp_t6041_firmware_version("26.6.2", "26.6.2"),
			DCP_FIRMWARE_V_26_6_DESKTOP);
	KUNIT_EXPECT_EQ(test,
			dcp_t6041_firmware_version("27.2.0", "27.2.0"),
			DCP_FIRMWARE_V_27_0);
	KUNIT_EXPECT_EQ(test,
			dcp_t6041_firmware_version("27.2.1", "27.2.0"),
			DCP_FIRMWARE_V_27_0);
	KUNIT_EXPECT_EQ(test,
			dcp_t6041_firmware_version("26.6.2", "27.2.0"),
			DCP_FIRMWARE_V_27_0);
	KUNIT_EXPECT_EQ(test,
			dcp_t6041_firmware_version("27.0.0", "27.0.0"),
			DCP_FIRMWARE_UNKNOWN);
	KUNIT_EXPECT_EQ(test,
			dcp_t6041_firmware_version("26.6.2", "26.5.2"),
			DCP_FIRMWARE_UNKNOWN);
}

static struct kunit_case v27_cases[] = {
	KUNIT_CASE(v27_method_table_test),
	KUNIT_CASE(v27_d590_callback_test),
	KUNIT_CASE(t6041_firmware_admission_test),
	{}
};

static struct kunit_suite v27_suite = {
	.name = "apple-dcp-v27",
	.test_cases = v27_cases,
};

kunit_test_suite(v27_suite);

MODULE_LICENSE("Dual MIT/GPL");
