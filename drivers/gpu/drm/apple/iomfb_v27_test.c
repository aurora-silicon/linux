// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Aurora Silicon */

/*
 * Firmware 27 is selected by the firmware-compat a DCP reports, on every SoC:
 * 27 or later runs the firmware-27 instance when the build has a complete
 * method table and is not started otherwise; older firmware is left to the
 * older interfaces. These tests cover that rule and the in-tree tables.
 */

#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>

#include "dcp.h"
#include "iomfb.h"
#include "iomfb_v27.h"

static enum dcp_v27_admission admission(const char *compat)
{
	const struct dcp_method_entry *methods;
	unsigned int num_missing;
	const char *missing;

	return dcp_v27_admission(compat, &methods, &missing, &num_missing);
}

/* Builds with a complete table start, whatever SoC reports them. */
static void v27_complete_table_admits_test(struct kunit *test)
{
	const struct dcp_method_entry *methods;
	unsigned int num_missing;
	const char *missing;

	KUNIT_EXPECT_EQ(test, dcp_v27_admission("27.2.0", &methods, &missing,
						&num_missing),
			DCP_V27_ADMITTED);
	KUNIT_EXPECT_PTR_EQ(test, methods, iomfb_v27_methods("27.2.0"));
	KUNIT_EXPECT_PTR_EQ(test, missing, NULL);
	KUNIT_EXPECT_EQ(test, num_missing, 0U);
	KUNIT_EXPECT_EQ(test, admission("27.0.0"), DCP_V27_ADMITTED);
}

/* 27 or later without a usable table is refused, never handed down. */
static void v27_unusable_table_refuses_test(struct kunit *test)
{
	const struct dcp_method_entry *methods;
	unsigned int num_missing;
	const char *missing;

	KUNIT_EXPECT_EQ(test, admission("27.1.0"), DCP_V27_NO_TABLE);
	KUNIT_EXPECT_EQ(test, admission("28.0.0"), DCP_V27_NO_TABLE);
	KUNIT_EXPECT_EQ(test, dcp_v27_admission("27.3.0", &methods, &missing,
						&num_missing),
			DCP_V27_INCOMPLETE);
	KUNIT_EXPECT_NOT_NULL(test, missing);
	KUNIT_EXPECT_GT(test, num_missing, 0U);
}

/* Older firmware keeps the 26.x, 14.x and 13.x selection. */
static void v27_older_firmware_not_v27_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, admission("26.6.2"), DCP_V27_NOT_V27);
	KUNIT_EXPECT_EQ(test, admission("26.5.2"), DCP_V27_NOT_V27);
	KUNIT_EXPECT_EQ(test, admission("14.7.0"), DCP_V27_NOT_V27);
	KUNIT_EXPECT_EQ(test, admission("13.5.0"), DCP_V27_NOT_V27);
	KUNIT_EXPECT_EQ(test, admission("12.3.0"), DCP_V27_NOT_V27);
	KUNIT_EXPECT_EQ(test, admission("UNKNOWN"), DCP_V27_NOT_V27);
}

static struct kunit_case v27_cases[] = {
	KUNIT_CASE(v27_complete_table_admits_test),
	KUNIT_CASE(v27_unusable_table_refuses_test),
	KUNIT_CASE(v27_older_firmware_not_v27_test),
	{}
};

static struct kunit_suite v27_suite = {
	.name = "apple-dcp-v27",
	.test_cases = v27_cases,
};

kunit_test_suite(v27_suite);

MODULE_LICENSE("Dual MIT/GPL");
