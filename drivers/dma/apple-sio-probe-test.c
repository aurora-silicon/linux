// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by apple-sio.c: which SIO nodes the probe goes on with, with and
 * without APPLE_SIO_T6030. The test devices have no register range, so a probe
 * that goes on toward the hardware stops there with -EINVAL; a refused node
 * returns -ENODEV first.
 */
#include <kunit/of.h>
#include <kunit/platform_device.h>
#include <kunit/test.h>

OF_OVERLAY_DECLARE(apple_sio_probe_test);

static void sio_probe_nodes_test(struct kunit *test)
{
	const bool t6030 = IS_ENABLED(CONFIG_APPLE_SIO_T6030);
	const struct {
		const char *path;
		int err;
	} cases[] = {
		/* M1/M2: the generic SIO, either way */
		{ "/sio-probe-test/sio-probe-test-t8103", -EINVAL },
		/* T6030: only with its support built, and with protocol-10 tables */
		{ "/sio-probe-test/sio-probe-test-t6030-v10", t6030 ? -EINVAL : -ENODEV },
		{ "/sio-probe-test/sio-probe-test-t6030-earlier", -ENODEV },
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
		/* Without its support, a T6030 is refused before its DMA is set up */
		if (!t6030 && cases[i].err == -ENODEV)
			KUNIT_EXPECT_EQ_MSG(test, pdev->dev.coherent_dma_mask, mask, "%s",
					    cases[i].path);
	}
}

static struct kunit_case sio_probe_test_cases[] = {
	KUNIT_CASE(sio_probe_nodes_test),
	{}
};

static struct kunit_suite sio_probe_test_suite = {
	.name = "apple-sio-probe",
	.test_cases = sio_probe_test_cases,
};

kunit_test_suite(sio_probe_test_suite);
