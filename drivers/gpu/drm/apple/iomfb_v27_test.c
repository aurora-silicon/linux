// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Aurora Silicon */

/*
 * The M1/M2/M3 display processors are admitted into the firmware-27 wire
 * generation on exact firmware-compat 27.2.0, sharing the T6050 method table.
 * These tests cover which compatibles that admission recognises and that the
 * 27.2.0 method table is complete, so a recognised processor starts.
 */

#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>

#include "dcp.h"
#include "iomfb.h"
#include "iomfb_v27.h"

/* A fixture node: the compatible strings the device tree would carry. */
struct refmac_node {
	const char *const *compats;	/* NULL-terminated */
};

static bool refmac_node_compat_is(const void *ctx, const char *compat)
{
	const struct refmac_node *node = ctx;
	const char *const *c;

	for (c = node->compats; *c; c++)
		if (!strcmp(*c, compat))
			return true;
	return false;
}

static bool refmac_admits(const char *const *compats)
{
	const struct refmac_node node = { .compats = compats };

	return dcp_refmac_v27_compat(refmac_node_compat_is, &node);
}

/* Both the per-SoC and the generic compatible of each refmac node. */
static const char *const t8103_dcp[]	= { "apple,t8103-dcp", "apple,dcp", NULL };
static const char *const t8103_dcpext[]	= { "apple,t8103-dcpext", "apple,dcpext", NULL };
static const char *const t6000_dcp[]	= { "apple,t6000-dcp", "apple,dcp", NULL };
static const char *const t6000_dcpext[]	= { "apple,t6000-dcpext", "apple,dcpext", NULL };
static const char *const t8112_dcp[]	= { "apple,t8112-dcp", "apple,dcp", NULL };
static const char *const t8112_dcpext[]	= { "apple,t8112-dcpext", "apple,dcpext", NULL };
static const char *const t6020_dcp[]	= { "apple,t6020-dcp", "apple,dcp", NULL };
static const char *const t6020_dcpext[]	= { "apple,t6020-dcpext", "apple,dcpext", NULL };
static const char *const t8122_dcp[]	= { "apple,t8122-dcp", "apple,dcp", NULL };
static const char *const t8122_dcpext[]	= { "apple,t8122-dcpext", "apple,dcpext", NULL };
static const char *const t6030_dcp[]	= { "apple,t6030-dcp", "apple,dcp", NULL };
static const char *const t6030_dcpext[]	= { "apple,t6030-dcpext", "apple,dcpext", NULL };
static const char *const t6031_dcp[]	= { "apple,t6031-dcp", "apple,dcp", NULL };
static const char *const t6031_dcpext[]	= { "apple,t6031-dcpext", "apple,dcpext", NULL };

static void v27_refmac_admits_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, refmac_admits(t8103_dcp));
	KUNIT_EXPECT_TRUE(test, refmac_admits(t8103_dcpext));
	KUNIT_EXPECT_TRUE(test, refmac_admits(t6000_dcp));
	KUNIT_EXPECT_TRUE(test, refmac_admits(t6000_dcpext));
	KUNIT_EXPECT_TRUE(test, refmac_admits(t8112_dcp));
	KUNIT_EXPECT_TRUE(test, refmac_admits(t8112_dcpext));
	KUNIT_EXPECT_TRUE(test, refmac_admits(t6020_dcp));
	KUNIT_EXPECT_TRUE(test, refmac_admits(t6020_dcpext));
	KUNIT_EXPECT_TRUE(test, refmac_admits(t8122_dcp));
	KUNIT_EXPECT_TRUE(test, refmac_admits(t8122_dcpext));
	KUNIT_EXPECT_TRUE(test, refmac_admits(t6030_dcp));
	KUNIT_EXPECT_TRUE(test, refmac_admits(t6030_dcpext));
	KUNIT_EXPECT_TRUE(test, refmac_admits(t6031_dcp));
	KUNIT_EXPECT_TRUE(test, refmac_admits(t6031_dcpext));
}

/* The T6050 and T8142 keep their own firmware-27 paths; T6041 and M4 do not. */
static const char *const t6050_dcp[]	= { "apple,t6050-dcp", NULL };
static const char *const t8142_dcp[]	= { "apple,t8142-dcp", NULL };
static const char *const t6041_dcp[]	= { "apple,t6041-dcp", "apple,dcpext", NULL };
static const char *const t8140_dcp[]	= { "apple,t8140-dcp", NULL };

static void v27_refmac_rejects_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, refmac_admits(t6050_dcp));
	KUNIT_EXPECT_FALSE(test, refmac_admits(t8142_dcp));
	KUNIT_EXPECT_FALSE(test, refmac_admits(t6041_dcp));
	KUNIT_EXPECT_FALSE(test, refmac_admits(t8140_dcp));
}

/* The 27.2.0 table a recognised processor runs is complete, so it may start. */
static void v27_method_table_test(struct kunit *test)
{
	const struct dcp_method_entry *methods;
	unsigned int missing;

	methods = iomfb_v27_methods("27.2.0");
	KUNIT_ASSERT_NOT_NULL(test, methods);
	KUNIT_EXPECT_PTR_EQ(test, iomfb_v27_missing_method(methods, &missing), NULL);
	KUNIT_EXPECT_EQ(test, missing, 0U);
	KUNIT_EXPECT_PTR_EQ(test, iomfb_v27_methods("27.1.0"), NULL);
}

static struct kunit_case v27_cases[] = {
	KUNIT_CASE(v27_refmac_admits_test),
	KUNIT_CASE(v27_refmac_rejects_test),
	KUNIT_CASE(v27_method_table_test),
	{}
};

static struct kunit_suite v27_suite = {
	.name = "apple-dcp-v27",
	.test_cases = v27_cases,
};

kunit_test_suite(v27_suite);

MODULE_LICENSE("Dual MIT/GPL");
