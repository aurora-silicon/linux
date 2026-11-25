// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by audio.c: what the DCP reports reaches the card until it is unbound. */
#include <kunit/device.h>
#include <kunit/platform_device.h>
#include <kunit/test.h>

static void dcpaud_card_unbound_test(struct kunit *test)
{
	struct dcp_audio *dcpaud = kunit_kzalloc(test, sizeof(*dcpaud), GFP_KERNEL);
	struct platform_device *pdev;

	KUNIT_ASSERT_NOT_NULL(test, dcpaud);
	pdev = kunit_platform_device_alloc(test, "dcpaud-card", PLATFORM_DEVID_AUTO);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pdev);
	KUNIT_ASSERT_EQ(test, kunit_platform_device_add(test, pdev), 0);
	platform_set_drvdata(pdev, dcpaud);
	dcpaud->dev = &pdev->dev;
	dcpaud->port_name = "Test";
	dcpaud->port_id = "Test";
	mutex_init(&dcpaud->data_lock);
#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030_AUDIO)
	mutex_init(&dcpaud->link_lock);
	INIT_WORK(&dcpaud->stale_work, dcpaud_stale_work);
	dcpaud->native_link = true;
#endif
	INIT_DELAYED_WORK(&dcpaud->void_work, dcpaud_void_work);
	KUNIT_ASSERT_EQ(test, dcpaud_init_snd_card(dcpaud), 0);

	dcpaud_connect(pdev, true);
	KUNIT_EXPECT_TRUE(test, dcpaud->connected);
	dcpaud_disconnect(pdev);
	KUNIT_EXPECT_FALSE(test, dcpaud->connected);
	dcpaud_connect(pdev, true);

	dcpaud_comp_unbind(&pdev->dev, NULL, NULL);
	KUNIT_EXPECT_NULL(test, dcpaud->card);
	KUNIT_EXPECT_FALSE(test, dcpaud->connected);
	/* The DCP, unbound after it, still reports. */
	dcpaud_disconnect(pdev);
	dcpaud_connect(pdev, true);
	KUNIT_EXPECT_FALSE(test, dcpaud->connected);
	dcpaud_refresh_eld(pdev);
#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030_AUDIO)
	KUNIT_EXPECT_FALSE(test, work_pending(&dcpaud->stale_work));
#endif
}

static struct kunit_case dcpaud_card_cases[] = {
	KUNIT_CASE(dcpaud_card_unbound_test),
	{}
};

static struct kunit_suite dcpaud_card_suite = {
	.name = "apple-dcpaud-card",
	.test_cases = dcpaud_card_cases,
};

kunit_test_suite(dcpaud_card_suite);
