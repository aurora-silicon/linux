// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by audio.c: a DP audio interface whose SIO never registers its
 * DMA controller (refused for an earlier boot loader's tables) gets no TX
 * channel; a PCM open fails with -ENXIO, each time, and nothing is kept.
 */
#include <kunit/device.h>
#include <kunit/of.h>
#include <kunit/test.h>

OF_OVERLAY_DECLARE(audio_no_sio_test);

static void dcpaud_no_sio_put(void *data)
{
	struct device *dev = data;

	of_node_put(dev->of_node);
	dev->of_node = NULL;
}

static void dcpaud_no_sio_test(struct kunit *test)
{
	struct dcp_audio *dcpaud = kunit_kzalloc(test, sizeof(*dcpaud), GFP_KERNEL);
	struct device *dev;

	KUNIT_ASSERT_NOT_NULL(test, dcpaud);
	KUNIT_ASSERT_EQ(test, 0, of_overlay_apply_kunit(test, audio_no_sio_test));
	dev = kunit_device_register(test, "dcpaud-no-sio");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	dev->of_node = of_find_node_by_path("/dcpaud-no-sio-test/dpaudio-no-sio");
	KUNIT_ASSERT_NOT_NULL(test, dev->of_node);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dcpaud_no_sio_put, dev), 0);
	dcpaud->dev = dev;
	mutex_init(&dcpaud->data_lock);

	KUNIT_EXPECT_EQ(test, dcpaud_init_dma(dcpaud), -ENXIO);
	KUNIT_EXPECT_NULL(test, dcpaud->chan);
	/* Every later open fails the same way, with nothing left behind */
	KUNIT_EXPECT_EQ(test, dcpaud_init_dma(dcpaud), -ENXIO);
	KUNIT_EXPECT_NULL(test, dcpaud->chan);
}

static struct kunit_case dcpaud_no_sio_cases[] = {
	KUNIT_CASE(dcpaud_no_sio_test),
	{}
};

static struct kunit_suite dcpaud_no_sio_suite = {
	.name = "apple-dcpaud-no-sio",
	.test_cases = dcpaud_no_sio_cases,
};

kunit_test_suite(dcpaud_no_sio_suite);
