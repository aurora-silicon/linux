// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Register the real H16 accel driver on a mock device and open its node
 * through its own file operations, as /dev/accel/accelN does.
 */
#include <kunit/test.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/module.h>

#include <drm/drm_kunit_helpers.h>

/* The node under test is this file's copy; keep its one global symbol apart. */
#define ane_h16_accel_register ane_h16_accel_register_kunit
#include "ane_h16_accel.c"

/*
 * A node over an awake session with one open file, for calling LOAD
 * directly. The session's lock is the node lock, as registration sets it.
 */
struct ane_h16_load_node {
	struct ane_h16_accel a;
	struct ane_h16_session s;
	struct ane_h16_accel_file f;
	struct ane_h16_program p;
	struct drm_file file;
	struct file filp;
};

static struct ane_h16_load_node *ane_h16_load_node(struct kunit *test)
{
	struct ane_h16_load_node *n = kunit_kzalloc(test, sizeof(*n), GFP_KERNEL);

	if (!n)
		return NULL;
	mutex_init(&n->a.lock);
	init_waitqueue_head(&n->s.wake);
	n->s.lock = &n->a.lock;
	n->a.session = &n->s;
	mutex_init(&n->f.lock);
	n->file.driver_priv = &n->f;
	n->file.filp = &n->filp;
	return n;
}

static int ane_h16_load_call(struct ane_h16_load_node *n, struct drm_ane_h16_load *args)
{
	return ane_h16_load_ioctl(&n->a.drm, args, &n->file);
}

/*
 * LOAD is admitted before it reads any user memory. On a file that has a
 * program, or that has another LOAD in flight, it fails with -EBUSY before
 * the copies, so pointers that fault are never read; with O_NONBLOCK during
 * system sleep it fails with -EAGAIN. An admitted LOAD that faults leaves
 * the file able to load again and holds no lock.
 */
static void ane_h16_load_admission_test(struct kunit *test)
{
	struct ane_h16_load_node *n = ane_h16_load_node(test);
	/* Read before admission, these would fail the LOAD with -EFAULT. */
	struct drm_ane_h16_load args = {
		.hwx_ptr = 1,
		.hwx_size = ANE_H16_HWX_MAX,
		.sections_ptr = 1,
		.section_count = DRM_ANE_H16_SECTIONS,
	};

	KUNIT_ASSERT_NOT_NULL(test, n);
	/* A program of this firmware boot: not lost, so the file is busy. */
	n->f.program = &n->p;
	KUNIT_EXPECT_EQ(test, ane_h16_load_call(n, &args), -EBUSY);
	KUNIT_EXPECT_PTR_EQ(test, n->f.program, &n->p);
	KUNIT_EXPECT_FALSE(test, n->f.loading);

	/* Another LOAD on this file is reading its HWX; its mark stays. */
	n->f.program = NULL;
	n->f.loading = true;
	KUNIT_EXPECT_EQ(test, ane_h16_load_call(n, &args), -EBUSY);
	KUNIT_EXPECT_TRUE(test, n->f.loading);
	n->f.loading = false;

	/* Admitted, the section table faults; the file can load again. */
	KUNIT_EXPECT_EQ(test, ane_h16_load_call(n, &args), -EFAULT);
	KUNIT_EXPECT_NULL(test, n->f.program);
	KUNIT_EXPECT_FALSE(test, n->f.loading);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&n->f.lock));
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&n->a.lock));

	n->s.asleep = true;
	n->filp.f_flags = O_NONBLOCK;
	KUNIT_EXPECT_EQ(test, ane_h16_load_call(n, &args), -EAGAIN);
	KUNIT_EXPECT_FALSE(test, n->f.loading);
	n->s.asleep = false;
	n->filp.f_flags = 0;

	args.flags = 1;
	KUNIT_EXPECT_EQ(test, ane_h16_load_call(n, &args), -EINVAL);
	KUNIT_EXPECT_FALSE(test, n->f.loading);
}

/*
 * An admitted LOAD reads its sections, then refuses an HWX larger than
 * ANE_H16_HWX_MAX or shorter than its header before allocating it. An HWX
 * within the bounds is read and validated, and the file can load again.
 */
static void ane_h16_load_size_test(struct kunit *test)
{
	struct ane_h16_load_node *n = ane_h16_load_node(test);
	struct drm_ane_h16_section sections[DRM_ANE_H16_SECTIONS];
	struct drm_ane_h16_load args;
	unsigned long user;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, n);
	user = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_ANONYMOUS | MAP_PRIVATE, 0);
	KUNIT_ASSERT_TRUE(test, user && !IS_ERR_VALUE(user));
	/* Each section is the smallest allowed, from zeroed bytes after the table. */
	for (i = 0; i < DRM_ANE_H16_SECTIONS; i++)
		sections[i] = (struct drm_ane_h16_section) {
			.kind = DRM_ANE_H16_SECTION_GENERIC + i,
			.size = ANE_H16_AUX_MIN,
			.data_ptr = user + sizeof(sections),
		};
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)user, sections, sizeof(sections)), 0);
	args = (struct drm_ane_h16_load) {
		.hwx_ptr = user,
		.hwx_size = ANE_H16_HWX_MAX + 1,
		.sections_ptr = user,
		.section_count = DRM_ANE_H16_SECTIONS,
	};
	KUNIT_EXPECT_EQ(test, ane_h16_load_call(n, &args), -EINVAL);
	KUNIT_EXPECT_FALSE(test, n->f.loading);
	args.hwx_size = 31;
	KUNIT_EXPECT_EQ(test, ane_h16_load_call(n, &args), -EINVAL);
	KUNIT_EXPECT_FALSE(test, n->f.loading);
	/* The page holds no HWX: read, refused by the validator, and freed. */
	args.hwx_size = PAGE_SIZE;
	KUNIT_EXPECT_EQ(test, ane_h16_load_call(n, &args), -ENOEXEC);
	KUNIT_EXPECT_NULL(test, n->f.program);
	KUNIT_EXPECT_FALSE(test, n->f.loading);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&n->a.lock));
}

static void ane_h16_accel_fops_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, ane_h16_fops.fop_flags & FOP_UNSIGNED_OFFSET);
	KUNIT_EXPECT_TRUE(test, ane_h16_fops.open == accel_open);
	/* No GEM offset manager: surfaces map through the driver's own mmap. */
	KUNIT_EXPECT_TRUE(test, ane_h16_fops.mmap == ane_h16_mmap);
	KUNIT_EXPECT_FALSE(test, ane_h16_drm_driver.driver_features & DRIVER_GEM);
	KUNIT_EXPECT_EQ(test, ane_h16_drm_driver.minor, 1);
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
	KUNIT_CASE(ane_h16_load_admission_test),
	KUNIT_CASE(ane_h16_load_size_test),
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
