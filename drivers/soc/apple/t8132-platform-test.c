// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <kunit/test.h>
#include <linux/of.h>
#include <linux/soc/apple/t8132.h>

static void t8132_test_legacy_descriptions(struct kunit *test)
{
	struct device_node *mcc __free(device_node) = NULL;
	struct device_node *pmc __free(device_node) = NULL;
	struct device_node *pmp __free(device_node) = NULL;
	struct device_node *gpu __free(device_node) = NULL;
	struct device_node *gfx __free(device_node) = NULL;
	struct device_node *clpc __free(device_node) = NULL;
	struct device_node *provider;
	u32 reg[8], agent;
	const char *status;

	if (!apple_t8132_air()) {
		kunit_skip(test, "requires the J713/J715 legacy-loader fixture");
		return;
	}
	mcc = of_find_compatible_node(NULL, NULL, "apple,t8132-mcc");
	pmc = of_find_compatible_node(NULL, NULL, "apple,t8132-pmc");
	pmp = of_find_compatible_node(NULL, NULL, "apple,t8132-pmp-v2");
	gpu = of_find_compatible_node(NULL, NULL, "apple,agx-t8132");
	KUNIT_ASSERT_NOT_NULL(test, mcc);
	KUNIT_ASSERT_NOT_NULL(test, pmc);
	KUNIT_ASSERT_NOT_NULL(test, pmp);
	KUNIT_EXPECT_TRUE(test, of_device_is_available(pmc));
	if (of_device_is_compatible(mcc, "kunit,existing")) {
		KUNIT_EXPECT_FALSE(test, of_device_is_available(mcc));
		KUNIT_ASSERT_EQ(test, of_property_read_string(mcc, "status", &status), 0);
		KUNIT_EXPECT_STREQ(test, status, "disabled");
		KUNIT_EXPECT_FALSE(test, of_property_present(mcc, "apple,boot-dma-devices"));
		KUNIT_EXPECT_EQ(test, of_get_child_count(mcc->parent), 5);
	} else {
		KUNIT_EXPECT_TRUE(test, of_device_is_available(mcc));
		KUNIT_EXPECT_STREQ(test, mcc->name, "memory-controller");
		KUNIT_ASSERT_EQ(test, of_property_read_u32_array(mcc, "reg", reg, 8), 0);
		KUNIT_EXPECT_EQ(test, reg[0], 2U);
		KUNIT_EXPECT_EQ(test, reg[1], 0x20000000U);
		KUNIT_EXPECT_EQ(test, reg[3], 0x1c1c04U);
		provider = of_parse_phandle(mcc, "apple,boot-dma-devices", 0);
		KUNIT_EXPECT_PTR_EQ(test, provider, pmp);
		of_node_put(provider);
	}
	provider = of_parse_phandle(pmc, "apple,pmp", 0);
	KUNIT_EXPECT_PTR_EQ(test, provider, pmp);
	of_node_put(provider);
	KUNIT_ASSERT_EQ(
		test, of_property_read_u32(pmc, "apple,static-voters", &agent),
		0);
	KUNIT_EXPECT_EQ(test, agent, 3U);
	clpc = of_get_child_by_name(pmc, "performance-controller");
	KUNIT_ASSERT_NOT_NULL(test, clpc);
	gfx = of_parse_phandle(clpc, "apple,gpu-power-state", 0);
	KUNIT_ASSERT_NOT_NULL(test, gfx);
	if (gpu) {
		provider = of_parse_phandle(gpu, "power-domains", 0);
		KUNIT_EXPECT_PTR_EQ(test, provider, gfx);
		of_node_put(provider);
	} else {
		KUNIT_EXPECT_TRUE(test,
			of_device_is_compatible(gfx, "apple,t8132-pmgr-pwrstate"));
		KUNIT_ASSERT_EQ(test, of_property_read_u32(gfx, "reg", &agent), 0);
		KUNIT_EXPECT_EQ(test, agent, 0x250U);
	}
}

static struct kunit_case t8132_platform_cases[] = {
	KUNIT_CASE(t8132_test_legacy_descriptions),
	{},
};

static struct kunit_suite t8132_platform_suite = {
	.name = "apple-t8132-platform",
	.test_cases = t8132_platform_cases,
};

kunit_test_suite(t8132_platform_suite);
MODULE_DESCRIPTION("KUnit tests for the Apple T8132 platform descriptions");
MODULE_LICENSE("Dual MIT/GPL");
