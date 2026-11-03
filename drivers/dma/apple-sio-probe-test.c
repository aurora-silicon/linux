// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by apple-sio.c: the compatible selects which firmware tables a
 * node needs. The test devices have no register range, so a probe
 * that goes on toward the hardware stops there with -EINVAL; a refused node
 * returns -ENODEV first.
 */
#include <kunit/of.h>
#include <kunit/platform_device.h>
#include <kunit/test.h>

OF_OVERLAY_DECLARE(apple_sio_probe_test);

static void sio_probe_nodes_test(struct kunit *test)
{
	const struct {
		const char *path;
		int err;
	} cases[] = {
		/* M1/M2: the generic SIO */
		{ "/sio-probe-test/sio-probe-test-t8103", -EINVAL },
		/* T6030: protocol-10 tables select the admitted path */
		{ "/sio-probe-test/sio-probe-test-t6030-v10", -EINVAL },
		{ "/sio-probe-test/sio-probe-test-t6030-earlier", -ENODEV },
		{ "/sio-probe-test/sio-probe-test-t6030-missing", -ENODEV },
		{ "/sio-probe-test/sio-probe-test-t6041-v10", -EINVAL },
		{ "/sio-probe-test/sio-probe-test-t6041-earlier", -ENODEV },
		{ "/sio-probe-test/sio-probe-test-t6041-missing", -ENODEV },
		{ "/sio-probe-test/sio-probe-test-t6041-duplicate", -ENODEV },
		{ "/sio-probe-test/sio-probe-test-t6041-unmapped", -ENODEV },
		{ "/sio-probe-test/sio-probe-test-t6041-overflow", -ENODEV },
		{ "/sio-probe-test/sio-probe-test-t6041-aux-unmapped", -ENODEV },
		{ "/sio-probe-test/sio-probe-test-t6041-aux-missing", -ENODEV },
		{ "/sio-probe-test/sio-probe-test-t6041-odd", -ENODEV },
		{ "/sio-probe-test/sio-probe-test-unknown", -ENODEV },
	};
	unsigned int i;

	KUNIT_ASSERT_EQ(test, 0, of_overlay_apply_kunit(test, apple_sio_probe_test));
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct platform_device *pdev;
		u64 mask;

		pdev = kunit_platform_device_alloc(test, "apple-sio-probe-test", i);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pdev);
		/* The device's release drops this reference */
		pdev->dev.of_node = of_find_node_by_path(cases[i].path);
		KUNIT_ASSERT_NOT_NULL_MSG(test, pdev->dev.of_node, "%s", cases[i].path);
		mask = pdev->dev.coherent_dma_mask;
		KUNIT_EXPECT_EQ_MSG(test, sio_probe(pdev), cases[i].err, "%s", cases[i].path);
		/* Bad protocol tables are refused before DMA setup. */
		if (cases[i].err == -ENODEV)
			KUNIT_EXPECT_EQ_MSG(test, pdev->dev.coherent_dma_mask, mask, "%s",
					    cases[i].path);
	}
}

static void sio_match_data_test(struct kunit *test)
{
	const struct of_device_id *match;
	struct device_node *np;
	const struct sio_hw_data *hw;

	KUNIT_ASSERT_EQ(test, 0, of_overlay_apply_kunit(test, apple_sio_probe_test));
	np = of_find_node_by_path("/sio-probe-test/sio-probe-test-t6030-v10");
	KUNIT_ASSERT_NOT_NULL(test, np);
	match = of_match_node(sio_of_match, np);
	of_node_put(np);
	KUNIT_ASSERT_NOT_NULL(test, match);
	hw = match->data;
	KUNIT_EXPECT_EQ(test, hw->fifo_threshold, 0x600U);
	KUNIT_EXPECT_TRUE(test, hw->clear_period_desc);
	KUNIT_EXPECT_TRUE(test, hw->check_boot_tables == sio_v10_check_boot_tables);

	np = of_find_node_by_path("/sio-probe-test/sio-probe-test-t8103");
	KUNIT_ASSERT_NOT_NULL(test, np);
	match = of_match_node(sio_of_match, np);
	of_node_put(np);
	KUNIT_ASSERT_NOT_NULL(test, match);
	hw = match->data;
	KUNIT_EXPECT_EQ(test, hw->fifo_threshold, 0x800U);
	KUNIT_EXPECT_FALSE(test, hw->clear_period_desc);
	KUNIT_EXPECT_FALSE(test, hw->check_boot_tables);
}

static void sio_t6041_profile_test(struct kunit *test)
{
	const struct of_device_id *match;
	struct device_node *np;
	const struct sio_hw_data *hw;
	struct sio_t6041_chan_config cfg;
	const u32 expected[] = { 2, 0, 0x800, 0x600, 0x800, 0 };

	KUNIT_ASSERT_EQ(test, 0, of_overlay_apply_kunit(test, apple_sio_probe_test));
	np = of_find_node_by_path("/sio-probe-test/sio-probe-test-t6041-v10");
	KUNIT_ASSERT_NOT_NULL(test, np);
	match = of_match_node(sio_of_match, np);
	of_node_put(np);
	KUNIT_ASSERT_NOT_NULL(test, match);
	hw = match->data;
	KUNIT_EXPECT_PTR_EQ(test, hw, &sio_hw_t6041);
	KUNIT_EXPECT_EQ(test, hw->config_size, 24U);
	KUNIT_EXPECT_TRUE(test, hw->clear_period_desc);
	KUNIT_EXPECT_TRUE(test, hw->check_boot_tables == sio_t6041_check_boot_tables);
	KUNIT_EXPECT_TRUE(test, sio_protocol_supported(hw, 10));
	KUNIT_EXPECT_FALSE(test, sio_protocol_supported(hw, 9));
	KUNIT_EXPECT_FALSE(test, sio_protocol_supported(hw, 11));
	memset(&cfg, 0xff, sizeof(cfg));
	sio_build_chan_config(hw, 2, &cfg);
	KUNIT_EXPECT_MEMEQ(test, &cfg, expected, sizeof(expected));
	KUNIT_EXPECT_EQ(test, sio_hw_generic.config_size, 20U);
	KUNIT_EXPECT_EQ(test, sio_hw_t6030.config_size, 20U);
	KUNIT_EXPECT_TRUE(test, sio_protocol_supported(&sio_hw_generic, 9));
}

static struct kunit_case sio_probe_test_cases[] = {
	KUNIT_CASE(sio_probe_nodes_test),
	KUNIT_CASE(sio_match_data_test),
	KUNIT_CASE(sio_t6041_profile_test),
	{}
};

static struct kunit_suite sio_probe_test_suite = {
	.name = "apple-sio-probe",
	.test_cases = sio_probe_test_cases,
};

kunit_test_suite(sio_probe_test_suite);
