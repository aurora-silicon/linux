// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included by audio.c: a Terminate's link release ends only the link of the
 * instance it is for. The production dcpaud_release_link() runs; the audio
 * service's answer to which instance the link is on, and its link calls,
 * are stubbed.
 */
#include <kunit/platform_device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

struct dcpaud_link_test {
	bool link_on;
	u64 asked;
	unsigned int completes;
	unsigned int stops;
};

static struct dcpaud_link_test *dcpaud_link_current;

static bool dcpaud_link_test_on(struct device *dev, u64 gen)
{
	dcpaud_link_current->asked = gen;
	return dcpaud_link_current->link_on;
}

static int dcpaud_link_test_call(struct device *dev, enum dcp_audio_link op,
				 const struct dcp_sound_cookie *cookie)
{
	if (op == DCP_AUDIO_LINK_COMPLETE)
		dcpaud_link_current->completes++;
	else if (op == DCP_AUDIO_LINK_STOP)
		dcpaud_link_current->stops++;
	return 0;
}

static struct dcp_audio *dcpaud_link_setup(struct kunit *test, struct platform_device **pdev)
{
	struct dcpaud_link_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct dcp_audio *dcpaud = kunit_kzalloc(test, sizeof(*dcpaud), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	KUNIT_ASSERT_NOT_NULL(test, dcpaud);
	*pdev = kunit_platform_device_alloc(test, "dcpaud-link", PLATFORM_DEVID_AUTO);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, *pdev);
	platform_set_drvdata(*pdev, dcpaud);
	dcpaud->dev = &(*pdev)->dev;
	dcpaud->dcp_dev = &(*pdev)->dev;
	dcpaud->native_link = true;
	mutex_init(&dcpaud->link_lock);
	dcpaud_link_current = t;
	kunit_activate_static_stub(test, dcp_audiosrv_link_on, dcpaud_link_test_on);
	kunit_activate_static_stub(test, dcp_audiosrv_link, dcpaud_link_test_call);
	return dcpaud;
}

static void dcpaud_link_release_instance(struct kunit *test)
{
	struct platform_device *pdev;
	struct dcp_audio *dcpaud = dcpaud_link_setup(test, &pdev);
	u64 gen = 7;

	/* The link is on another instance: left as it is */
	dcpaud->link_prepared = true;
	dcpaud->link_started = true;
	dcpaud_release_link(pdev, &gen);
	KUNIT_EXPECT_EQ(test, dcpaud_link_current->asked, 7ULL);
	KUNIT_EXPECT_TRUE(test, dcpaud->link_prepared);
	KUNIT_EXPECT_TRUE(test, dcpaud->link_started);
	KUNIT_EXPECT_EQ(test, dcpaud_link_current->stops + dcpaud_link_current->completes, 0U);

	/* On the instance asked for: stopped and completed */
	dcpaud_link_current->link_on = true;
	dcpaud_release_link(pdev, &gen);
	KUNIT_EXPECT_FALSE(test, dcpaud->link_prepared);
	KUNIT_EXPECT_FALSE(test, dcpaud->link_started);
	KUNIT_EXPECT_EQ(test, dcpaud_link_current->stops, 1U);
	KUNIT_EXPECT_EQ(test, dcpaud_link_current->completes, 1U);

	/* Without an instance (the display's own power-off): whichever link */
	dcpaud_link_current->link_on = false;
	dcpaud->link_prepared = true;
	dcpaud_release_link(pdev, NULL);
	KUNIT_EXPECT_FALSE(test, dcpaud->link_prepared);
	KUNIT_EXPECT_EQ(test, dcpaud_link_current->completes, 2U);
}

static struct kunit_case dcpaud_link_cases[] = {
	KUNIT_CASE(dcpaud_link_release_instance),
	{}
};

static struct kunit_suite dcpaud_link_suite = {
	.name = "apple-dcpaud-release-link",
	.test_cases = dcpaud_link_cases,
};

kunit_test_suite(dcpaud_link_suite);
