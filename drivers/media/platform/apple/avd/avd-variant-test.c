// SPDX-License-Identifier: GPL-2.0-only
/* Included by avd-drv.c to use the same OF table as avd_probe(). */
#include <kunit/test.h>

static void avd_t8112_of_variant(struct kunit *test)
{
	static const char compatible[] = "apple,t8112-avd";
	struct property prop = {
		.name = "compatible",
		.length = sizeof(compatible),
		.value = (void *)compatible,
	};
	struct device_node node = { .properties = &prop };
	const struct of_device_id *match;
	const struct avd_variant *variant;

	match = of_match_node(avd_of_match, &node);
	KUNIT_ASSERT_NOT_NULL(test, match);
	variant = match->data;
	KUNIT_ASSERT_NOT_NULL(test, variant);

	/* Both stubs use the same independently supplied decoder image. */
	KUNIT_EXPECT_STREQ(test, variant->fw_name, "apple/avd-fw-v3-t1.bin");
	KUNIT_EXPECT_NULL(test, variant->fw_name_fallback);
	KUNIT_EXPECT_NULL(test, variant->apple);
	KUNIT_EXPECT_PTR_EQ(test, variant->configure_stream, &t8112_configure_stream);
	KUNIT_EXPECT_EQ(test, variant->revision, 4);
	KUNIT_EXPECT_EQ(test, variant->quirks, (u32)AVD_QUIRK_LSR);
	KUNIT_EXPECT_EQ(test, variant->capabilities,
			(u32)(AVD_CAPABILITY_H264 | AVD_CAPABILITY_HEVC | AVD_CAPABILITY_VP9));
}

static struct kunit_case avd_variant_cases[] = {
	KUNIT_CASE(avd_t8112_of_variant),
	{}
};

static struct kunit_suite avd_variant_suite = {
	.name = "apple-avd-variant",
	.test_cases = avd_variant_cases,
};

kunit_test_suite(avd_variant_suite);
