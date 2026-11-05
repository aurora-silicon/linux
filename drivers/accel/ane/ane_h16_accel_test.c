// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Register the real H16 accel driver on a mock device and open its node
 * through its own file operations, as /dev/accel/accelN does.
 */
#include <kunit/test.h>
#include <linux/fs.h>
#include <linux/module.h>

#include <drm/drm_kunit_helpers.h>

/* The node under test is this file's copy; keep its one global symbol apart. */
#define ane_h16_accel_register ane_h16_accel_register_kunit
#include "ane_h16_accel.c"

static void ane_h16_accel_fops_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, ane_h16_fops.fop_flags & FOP_UNSIGNED_OFFSET);
	KUNIT_EXPECT_TRUE(test, ane_h16_fops.open == accel_open);
	/* Without DRIVER_GEM there is no offset manager to mmap through. */
	KUNIT_EXPECT_NULL(test, ane_h16_fops.mmap);
	KUNIT_EXPECT_FALSE(test, ane_h16_drm_driver.driver_features & DRIVER_GEM);
}

static void ane_h16_accel_open_test(struct kunit *test)
{
	struct device *dev = drm_kunit_helper_alloc_device(test);
	struct ane_h16_accel *a;
	struct inode *inode;
	struct file *filp;
	int ret;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	a = drm_kunit_helper_alloc_drm_device_with_driver(test, dev, struct ane_h16_accel, drm,
							  &ane_h16_drm_driver);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, a);
	KUNIT_ASSERT_EQ(test, drmm_mutex_init(&a->drm, &a->lock), 0);
	KUNIT_ASSERT_EQ(test, drm_dev_register(&a->drm, 0), 0);
	KUNIT_ASSERT_NOT_NULL(test, a->drm.accel);

	inode = kunit_kzalloc(test, sizeof(*inode), GFP_KERNEL);
	filp = kunit_kzalloc(test, sizeof(*filp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, inode);
	KUNIT_ASSERT_NOT_NULL(test, filp);
	inode->i_rdev = MKDEV(ACCEL_MAJOR, a->drm.accel->index);
	filp->f_op = &ane_h16_fops;
	filp->f_flags = O_RDWR;
	filp->f_mode = FMODE_READ | FMODE_WRITE;

	ret = ane_h16_fops.open(inode, filp);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (!ret) {
		struct drm_file *file = filp->private_data;

		KUNIT_ASSERT_NOT_NULL(test, file);
		KUNIT_EXPECT_NOT_NULL(test, file->driver_priv);
		KUNIT_EXPECT_EQ(test, ane_h16_fops.release(inode, filp), 0);
	}
	drm_dev_unregister(&a->drm);
}

static struct kunit_case ane_h16_accel_cases[] = {
	KUNIT_CASE(ane_h16_accel_fops_test),
	KUNIT_CASE(ane_h16_accel_open_test),
	{}
};

static struct kunit_suite ane_h16_accel_suite = {
	.name = "ane-h16-accel",
	.test_cases = ane_h16_accel_cases,
};

kunit_test_suite(ane_h16_accel_suite);

MODULE_DESCRIPTION("KUnit open test for the H16 ANE accel node");
MODULE_LICENSE("Dual MIT/GPL");
