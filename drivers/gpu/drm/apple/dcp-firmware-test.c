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
	const struct iomfb_v27_build *build = iomfb_v27_methods("27.2.0");
	const struct iomfb_v27_build *earlier_build =
		iomfb_v27_methods("27.0.0");
	const struct dcp_method_entry *methods;
	const struct dcp_method_entry *earlier;
	struct dcp_method_entry *incomplete;
	unsigned int missing;

	KUNIT_ASSERT_NOT_NULL(test, build);
	KUNIT_ASSERT_NOT_NULL(test, earlier_build);
	methods = build->methods;
	earlier = earlier_build->methods;
	KUNIT_ASSERT_NOT_NULL(test, methods);
	KUNIT_ASSERT_NOT_NULL(test, earlier);
	KUNIT_EXPECT_PTR_NE(test, methods, earlier);
	KUNIT_EXPECT_NE(test, memcmp(methods[dcpep_swap_submit].tag,
				     earlier[dcpep_swap_submit].tag,
				     sizeof(methods[dcpep_swap_submit].tag)), 0);
	KUNIT_EXPECT_EQ(test, dcp_v27_method_table(&dev, "27.2.0"), DCP_FIRMWARE_V_27_0);
	KUNIT_EXPECT_EQ(test, dcp_v27_method_table(&dev, "27.3.0"), DCP_FIRMWARE_V_27_0);
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
 * Firmware 27 provider clocks on a T602x display processor: the 27.2 ADT
 * lists none (an empty /arm-io/disp0 and dispext<n> clock-ids), and the
 * node keeps its display clock.
 */
static void dcp_firmware_27_empty_clock_table(struct kunit *test)
{
	static const char primary[] = "apple,t6020-dcp\0apple,dcp";
	__be32 display = cpu_to_be32(1);
	struct property properties[] = {
		{ .name = "apple,clock-ids", .length = 0, .value = &display,
		  .next = &properties[1] },
		{ .name = "clocks", .length = sizeof(display), .value = &display,
		  .next = &properties[2] },
		{ .name = "compatible", .length = sizeof(primary), .value = (void *)primary },
	};
	struct device_node node = { .properties = properties };
	struct device dev = { .init_name = "dcp-firmware-test", .of_node = &node };
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	dcp->dev = &dev;
	dcp->num_firmware_clocks = 3;
	KUNIT_EXPECT_EQ(test, dcp_firmware_clocks_v27(dcp), 0);
	KUNIT_EXPECT_EQ(test, dcp->num_firmware_clocks, 0U);
	KUNIT_EXPECT_NULL(test, dcp->firmware_clocks);

	/* Without the table the firmware's clocks are unknown: refused. */
	properties[0].name = "apple,unrelated";
	KUNIT_EXPECT_EQ(test, dcp_firmware_clocks_v27(dcp), -EINVAL);
}

/*
 * Explicit match-data and image-UUID pins stay ahead of generic firmware-27
 * admission: T8122 wrong/missing 27.2 UUIDs and internal T8142 unknown 27.x
 * builds are terminally refused rather than using generic v27 or H17P,
 * while generic refmac DCPs (T6030, T8112) take firmware 27 on complete 27.x
 * method tables and refuse incomplete ones.
 */
static void dcp_firmware_27_pin_precedence(struct kunit *test)
{
	static const char t6030[] = "apple,t6030-dcp";
	static const char t6041[] = "apple,t6041-dcp\0apple,dcp";
	static const char t6041_ext[] = "apple,t6041-dcpext\0apple,dcpext";
	static const char t8122[] = "apple,t8122-dcp\0apple,dcp";
	static const char t8122_ext[] = "apple,t8122-dcpext\0apple,dcpext";
	static const char t8142[] = "apple,t8142-dcp";
#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T8132)
	static const char t8132[] = "apple,t8132-dcp";
#endif
	static const char bad_uuid[] = "00000000-0000-0000-0000-000000000000";
	__be32 compat_26_6[] = { cpu_to_be32(26), cpu_to_be32(6), cpu_to_be32(2) };
	__be32 compat_27_0[] = { cpu_to_be32(27), 0, 0 };
	__be32 compat_27_1[] = { cpu_to_be32(27), cpu_to_be32(1), 0 };
	__be32 compat_27_2[] = { cpu_to_be32(27), cpu_to_be32(2), 0 };
	__be32 compat_27_3[] = { cpu_to_be32(27), cpu_to_be32(3), 0 };
	struct dcp_pin_precedence_fixture {
		struct property props[4];
		struct property ext_props[3];
		struct device_node node;
		struct device_node ext_node;
		struct device dev;
	} *fx;

	fx = kunit_kzalloc(test, sizeof(*fx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fx);

	fx->props[0] = (struct property) {
		.name = "apple,firmware-compat",
		.length = sizeof(compat_27_2),
		.value = compat_27_2,
		.next = &fx->props[1],
	};
	fx->props[1] = (struct property) {
		.name = "apple,firmware-version",
		.length = sizeof(compat_27_2),
		.value = compat_27_2,
		.next = &fx->props[2],
	};
	fx->props[2] = (struct property) {
		.name = "apple,firmware-uuid",
		.length = sizeof(bad_uuid),
		.value = (void *)bad_uuid,
		.next = &fx->props[3],
	};
	fx->props[3] = (struct property) {
		.name = "compatible",
		.length = sizeof(t6030),
		.value = (void *)t6030,
	};
	fx->ext_props[0] = (struct property) {
		.name = "apple,firmware-compat",
		.length = sizeof(compat_27_2),
		.value = compat_27_2,
		.next = &fx->ext_props[1],
	};
	fx->ext_props[1] = (struct property) {
		.name = "apple,firmware-version",
		.length = sizeof(compat_27_2),
		.value = compat_27_2,
		.next = &fx->ext_props[2],
	};
	fx->ext_props[2] = (struct property) {
		.name = "compatible",
		.length = sizeof(t8122_ext),
		.value = (void *)t8122_ext,
	};
	fx->node.properties = fx->props;
	fx->ext_node.properties = fx->ext_props;
	fx->dev.init_name = "dcp-firmware-test";
	fx->dev.of_node = &fx->node;
	fx->dev.driver = &apple_platform_driver.driver;

	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_V_27_0);

	fx->props[0].value = compat_27_3;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_V_27_0);
	fx->props[0].value = compat_27_2;

	/* T6041 internal and external UUID pins remain enforced on 27.2. */
	fx->props[3].length = sizeof(t6041);
	fx->props[3].value = (void *)t6041;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_UNKNOWN);

	fx->props[2].length = sizeof(APPLE_T6041_DCP_26B123_UUID);
	fx->props[2].value = (void *)APPLE_T6041_DCP_26B123_UUID;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_V_27_0);

	fx->props[3].length = sizeof(t6041_ext);
	fx->props[3].value = (void *)t6041_ext;
	fx->props[2].length = sizeof(bad_uuid);
	fx->props[2].value = (void *)bad_uuid;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_UNKNOWN);

	fx->props[2].name = "apple,unrelated";
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_UNKNOWN);

	fx->props[2].name = "apple,firmware-uuid";
	fx->props[2].length = sizeof(APPLE_T6041_DCP_26B123_UUID);
	fx->props[2].value = (void *)APPLE_T6041_DCP_26B123_UUID;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_V_27_0);

	/* T8122 wrong or missing 27.2 UUID is terminally refused (never takes generic v27). */
	fx->props[3].length = sizeof(t8122);
	fx->props[3].value = (void *)t8122;
	fx->props[2].name = "apple,firmware-uuid";
	fx->props[2].length = sizeof(bad_uuid);
	fx->props[2].value = (void *)bad_uuid;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_UNKNOWN);
	KUNIT_EXPECT_FALSE(test,
			   apple_t8122_dcpext_v27_2_for_internal(&fx->ext_node, &fx->node));

	fx->props[2].name = "apple,unrelated";
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_UNKNOWN);
	KUNIT_EXPECT_FALSE(test,
			   apple_t8122_dcpext_v27_2_for_internal(&fx->ext_node, &fx->node));

	fx->props[2].name = "apple,firmware-uuid";
	fx->props[2].length = sizeof(APPLE_T8122_DCP_26B123_UUID);
	fx->props[2].value = (void *)APPLE_T8122_DCP_26B123_UUID;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_V_27_0);
	KUNIT_EXPECT_TRUE(test,
			  apple_t8122_dcpext_v27_2_for_internal(&fx->ext_node, &fx->node));

	/* T8122 external with mismatched version is terminally refused regardless of live tree. */
	fx->ext_props[1].value = compat_27_0;
	KUNIT_EXPECT_FALSE(test,
			   apple_t8122_dcpext_v27_2_for_internal(&fx->ext_node, &fx->node));
	fx->dev.of_node = &fx->ext_node;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_UNKNOWN);
	fx->ext_props[1].value = compat_27_2;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			apple_t8122_dcpext_v27_2(&fx->ext_node) ?
			DCP_FIRMWARE_V_27_0 : DCP_FIRMWARE_UNKNOWN);
	fx->dev.of_node = &fx->node;

	/* Internal T8142: known 27.x takes v27, unknown 27.x is refused (never takes H17P). */
	fx->props[3].length = sizeof(t8142);
	fx->props[3].value = (void *)t8142;
	fx->props[0].value = compat_27_0;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_V_27_0);
	fx->props[0].value = compat_27_2;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_V_27_0);
	fx->props[0].value = compat_27_1;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_UNKNOWN);
	fx->props[0].value = compat_27_3;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_V_27_0);
	fx->props[0].value = compat_26_6;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			DCP_FIRMWARE_H17P);
	fx->props[0].value = compat_27_2;

#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T8132)
	/*
	 * T8132 keeps its M4 match-data pin on 27.2, never generic firmware
	 * 27: the M4 backend's on its 26.6.2 firmware path, nobody's off it.
	 */
	fx->props[3].length = sizeof(t8132);
	fx->props[3].value = (void *)t8132;
	KUNIT_EXPECT_EQ(test, dcp_check_firmware_version(&fx->dev),
			apple_t8132_firmware_path() ? DCP_FIRMWARE_M4 :
						      DCP_FIRMWARE_UNKNOWN);
#endif
}

static void dcp_typec_routes_available_children(struct kunit *test)
{
	static const char disabled[] = "disabled";
	static const char okay[] = "okay";
	struct property child_status = {
		.name = "status", .length = sizeof(disabled), .value = (void *)disabled,
	};
	struct device_node child = {
		.name = "route", .full_name = "route@0", .properties = &child_status,
	};
	struct device_node routes = {
		.name = "typec-routes", .full_name = "typec-routes",
	};
	struct device_node parent = {
		.name = "dcp", .full_name = "dcp", .child = &routes,
	};

	/* Empty typec-routes container has no available route children. */
	KUNIT_EXPECT_FALSE(test, dcp_has_available_typec_routes(&parent));

	/* Container with only disabled child routes has no available routes. */
	routes.child = &child;
	KUNIT_EXPECT_FALSE(test, dcp_has_available_typec_routes(&parent));

	/* Available child route activates Type-C route detection. */
	child_status.length = sizeof(okay);
	child_status.value = (void *)okay;
	KUNIT_EXPECT_TRUE(test, dcp_has_available_typec_routes(&parent));
}

static struct kunit_case dcp_firmware_cases[] = {
	KUNIT_CASE(dcp_firmware_13_5_compat),
	KUNIT_CASE(dcp_firmware_27_method_admission),
	KUNIT_CASE(dcp_firmware_27_empty_clock_table),
	KUNIT_CASE(dcp_firmware_27_pin_precedence),
	KUNIT_CASE(dcp_typec_routes_available_children),
	{}
};

static struct kunit_suite dcp_firmware_suite = {
	.name = "apple-dcp-firmware",
	.test_cases = dcp_firmware_cases,
};

kunit_test_suite(dcp_firmware_suite);
