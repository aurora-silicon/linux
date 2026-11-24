// SPDX-License-Identifier: GPL-2.0-only
/* Included by dcp.c to exercise probe's firmware admission without hardware. */
#include <kunit/test.h>

static void dcp_firmware_13_5_compat(struct kunit *test)
{
	static const char primary[] = "apple,t8112-dcp\0apple,dcp";
	static const char external[] = "apple,t8112-dcpext\0apple,dcpext";
	__be32 firmware[] = { cpu_to_be32(13), cpu_to_be32(5), 0 };
	__be32 compat[] = { cpu_to_be32(13), cpu_to_be32(3), 0 };
	struct property properties[] = {
		{ .name = "apple,firmware-compat", .length = sizeof(compat),
		  .value = compat, .next = &properties[1] },
		{ .name = "apple,firmware-version", .length = sizeof(firmware),
		  .value = firmware, .next = &properties[2] },
		{ .name = "compatible", .length = sizeof(primary), .value = (void *)primary },
	};
	struct device_node node = { .properties = properties };
	struct device dev = {
		.init_name = "dcp-firmware-test",
		.of_node = &node,
		.driver = &apple_platform_driver.driver,
	};

	/* The 13.5 stub can advertise 13.3 IOMFB compatibility. */
	KUNIT_ASSERT_NOT_NULL(test, of_device_get_match_data(&dev));
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&dev), DCP_FIRMWARE_V_13_5);
	firmware[1] = cpu_to_be32(3);
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&dev), DCP_FIRMWARE_UNKNOWN);
	firmware[1] = cpu_to_be32(5);

	properties[2].length = sizeof(external);
	properties[2].value = (void *)external;
	KUNIT_ASSERT_NOT_NULL(test, of_device_get_match_data(&dev));
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&dev), DCP_FIRMWARE_V_13_5);
	compat[1] = cpu_to_be32(5);
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&dev), DCP_FIRMWARE_V_13_5);
}

static void dcp_firmware_27_method_admission(struct kunit *test)
{
	struct device dev = { .init_name = "dcp-firmware-test" };
	const struct dcp_method_entry *methods = iomfb_v27_methods("27.2.0");
	const struct dcp_method_entry *earlier = iomfb_v27_methods("27.0.0");
	struct dcp_method_entry *incomplete;
	unsigned int missing;

	KUNIT_ASSERT_NOT_NULL(test, methods);
	KUNIT_ASSERT_NOT_NULL(test, earlier);
	KUNIT_EXPECT_PTR_NE(test, methods, earlier);
	KUNIT_EXPECT_NE(test, memcmp(methods[dcpep_swap_submit].tag,
				     earlier[dcpep_swap_submit].tag,
				     sizeof(methods[dcpep_swap_submit].tag)), 0);
	KUNIT_EXPECT_EQ(test, dcp_v27_method_table(&dev, "27.2.0"), DCP_FIRMWARE_V_27_0);
	KUNIT_EXPECT_EQ(test, dcp_v27_method_table(&dev, "27.3.0"), DCP_FIRMWARE_UNKNOWN);
	KUNIT_EXPECT_EQ(test, dcp_v27_method_table(&dev, "27.2.1"), DCP_FIRMWARE_UNKNOWN);

	incomplete = kunit_kmalloc(test, sizeof(*methods) * dcpep_num_methods, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, incomplete);
	memcpy(incomplete, methods, sizeof(*methods) * dcpep_num_methods);
	memset(incomplete[dcpep_dfb_ready_query].tag, 0,
	       sizeof(incomplete[dcpep_dfb_ready_query].tag));
	KUNIT_EXPECT_NULL(test, iomfb_v27_missing_method(incomplete, &missing));
	KUNIT_EXPECT_EQ(test, missing, 0U);
	memset(incomplete[dcpep_swap_submit].tag, 0, sizeof(incomplete[dcpep_swap_submit].tag));
	KUNIT_EXPECT_STREQ(test, iomfb_v27_missing_method(incomplete, &missing),
			   methods[dcpep_swap_submit].name);
	KUNIT_EXPECT_EQ(test, missing, 1U);
}

/*
 * The firmware 27 display sleep sends its calls by their 27.0 IDs, so only a table that gives
 * each of them that ID carries it: 27.0's (and the J714s copy), not 27.2's or 27.3's.
 */
static void dcp_firmware_27_sleep_methods(struct kunit *test)
{
	const struct dcp_method_entry *earlier = iomfb_v27_methods("27.0.0");
	struct dcp_method_entry *table;

	KUNIT_ASSERT_NOT_NULL(test, earlier);
	KUNIT_EXPECT_NULL(test, iomfb_v27_sleep_missing_method(earlier));
	KUNIT_EXPECT_STREQ(test, iomfb_v27_sleep_missing_method(iomfb_v27_methods("27.2.0")),
			   earlier[dcpep_set_power_state].name);
	KUNIT_EXPECT_STREQ(test, iomfb_v27_sleep_missing_method(iomfb_v27_methods("27.3.0")),
			   earlier[dcpep_set_power_state].name);
	KUNIT_EXPECT_STREQ(test, iomfb_v27_sleep_missing_method(NULL),
			   earlier[dcpep_set_power_state].name);

	/*
	 * The J714s copy differs in a call the sleep does not make; one sleep call with another
	 * ID, or none, is enough to refuse the sleep.
	 */
	table = kunit_kmalloc(test, sizeof(*earlier) * dcpep_num_methods, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, table);
	memcpy(table, earlier, sizeof(*earlier) * dcpep_num_methods);
	memcpy(table[dcpep_swap_submit].tag, "A407", sizeof(table[dcpep_swap_submit].tag));
	KUNIT_EXPECT_NULL(test, iomfb_v27_sleep_missing_method(table));
	memcpy(table[dcpep_is_main_display].tag, "A462", sizeof(table[dcpep_is_main_display].tag));
	KUNIT_EXPECT_STREQ(test, iomfb_v27_sleep_missing_method(table),
			   earlier[dcpep_is_main_display].name);
	memset(table[dcpep_unnamed_500].tag, 0, sizeof(table[dcpep_unnamed_500].tag));
	KUNIT_EXPECT_STREQ(test, iomfb_v27_sleep_missing_method(table),
			   earlier[dcpep_unnamed_500].name);
}

static struct kunit_case dcp_firmware_cases[] = {
	KUNIT_CASE(dcp_firmware_13_5_compat),
	KUNIT_CASE(dcp_firmware_27_method_admission),
	KUNIT_CASE(dcp_firmware_27_sleep_methods),
	{}
};

static struct kunit_suite dcp_firmware_suite = {
	.name = "apple-dcp-firmware",
	.test_cases = dcp_firmware_cases,
};

kunit_test_suite(dcp_firmware_suite);
