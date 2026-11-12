// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for the Apple silicon camera-in-use indicator broker.
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/soc/apple/camera-indicator.h>

struct cami_fake {
	unsigned int on_calls;
	unsigned int off_calls;
	bool confirm;		/* report it shown as soon as it is asked for */
	bool want;
};

static void cami_fake_set(void *priv, bool on)
{
	struct cami_fake *fake = priv;

	fake->want = on;
	if (on)
		fake->on_calls++;
	else
		fake->off_calls++;
	if (on && fake->confirm)
		apple_cam_indicator_set_shown(true);
	if (!on)
		apple_cam_indicator_set_shown(false);
}

static const struct apple_cam_indicator_ops cami_fake_ops = {
	.set = cami_fake_set,
};

static struct device *cami_test_register(struct kunit *test,
					 struct cami_fake *fake)
{
	struct device *dev = kunit_device_register(test, "cami-test-display");

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	KUNIT_ASSERT_EQ(test, apple_cam_indicator_register(dev, &cami_fake_ops,
							   fake), 0);
	return dev;
}

/* Without a display that draws it, a camera cannot get the indicator. */
static void cami_test_no_provider(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, apple_cam_indicator_acquire(10), -ENODEV);
	KUNIT_EXPECT_FALSE(test, apple_cam_indicator_shown());
}

/* Shown, held for the stream, removed at once on release. */
static void cami_test_acquire_release(struct kunit *test)
{
	struct cami_fake fake = { .confirm = true };
	struct device *dev = cami_test_register(test, &fake);

	KUNIT_EXPECT_EQ(test, apple_cam_indicator_acquire(100), 0);
	KUNIT_EXPECT_TRUE(test, apple_cam_indicator_shown());
	KUNIT_EXPECT_EQ(test, fake.on_calls, 1);
	KUNIT_EXPECT_TRUE(test, fake.want);

	apple_cam_indicator_release();
	KUNIT_EXPECT_FALSE(test, apple_cam_indicator_shown());
	KUNIT_EXPECT_EQ(test, fake.off_calls, 1);
	KUNIT_EXPECT_FALSE(test, fake.want);

	apple_cam_indicator_unregister(dev);
}

/* A display that never confirms it: refused, and the request withdrawn. */
static void cami_test_timeout(struct kunit *test)
{
	struct cami_fake fake = { .confirm = false };
	struct device *dev = cami_test_register(test, &fake);

	KUNIT_EXPECT_EQ(test, apple_cam_indicator_acquire(20), -ETIMEDOUT);
	KUNIT_EXPECT_FALSE(test, apple_cam_indicator_shown());
	KUNIT_EXPECT_EQ(test, fake.on_calls, 1);
	KUNIT_EXPECT_EQ(test, fake.off_calls, 1);
	KUNIT_EXPECT_FALSE(test, fake.want);

	apple_cam_indicator_unregister(dev);
}

/* Two users: the indicator stays until the last one lets go. */
static void cami_test_two_users(struct kunit *test)
{
	struct cami_fake fake = { .confirm = true };
	struct device *dev = cami_test_register(test, &fake);

	KUNIT_ASSERT_EQ(test, apple_cam_indicator_acquire(100), 0);
	KUNIT_ASSERT_EQ(test, apple_cam_indicator_acquire(100), 0);
	KUNIT_EXPECT_EQ(test, fake.on_calls, 1);
	apple_cam_indicator_release();
	KUNIT_EXPECT_TRUE(test, apple_cam_indicator_shown());
	KUNIT_EXPECT_EQ(test, fake.off_calls, 0);
	apple_cam_indicator_release();
	KUNIT_EXPECT_FALSE(test, apple_cam_indicator_shown());
	KUNIT_EXPECT_EQ(test, fake.off_calls, 1);

	apple_cam_indicator_unregister(dev);
}

/* A loss reported by the display (panel off, sleep) shows at once. */
static void cami_test_loss(struct kunit *test)
{
	struct cami_fake fake = { .confirm = true };
	struct device *dev = cami_test_register(test, &fake);

	KUNIT_ASSERT_EQ(test, apple_cam_indicator_acquire(100), 0);
	apple_cam_indicator_set_shown(false);
	KUNIT_EXPECT_FALSE(test, apple_cam_indicator_shown());
	apple_cam_indicator_set_shown(true);
	KUNIT_EXPECT_TRUE(test, apple_cam_indicator_shown());

	/* The display going away takes the indicator with it. */
	apple_cam_indicator_unregister(dev);
	KUNIT_EXPECT_FALSE(test, apple_cam_indicator_shown());
	apple_cam_indicator_release();
}

/* One display only; a second one is refused. */
static void cami_test_one_provider(struct kunit *test)
{
	struct cami_fake fake = {}, other = {};
	struct device *dev = cami_test_register(test, &fake);
	struct device *dev2 = kunit_device_register(test, "cami-test-display2");

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev2);
	KUNIT_EXPECT_EQ(test, apple_cam_indicator_register(dev2, &cami_fake_ops,
							   &other), -EBUSY);
	apple_cam_indicator_unregister(dev2);	/* not the provider: no-op */
	apple_cam_indicator_unregister(dev);
}

static struct kunit_case cami_test_cases[] = {
	KUNIT_CASE(cami_test_no_provider),
	KUNIT_CASE(cami_test_acquire_release),
	KUNIT_CASE(cami_test_timeout),
	KUNIT_CASE(cami_test_two_users),
	KUNIT_CASE(cami_test_loss),
	KUNIT_CASE(cami_test_one_provider),
	{}
};

static struct kunit_suite cami_test_suite = {
	.name = "apple-camera-indicator",
	.test_cases = cami_test_cases,
};

kunit_test_suite(cami_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Apple camera-in-use indicator broker");
MODULE_LICENSE("Dual MIT/GPL");
