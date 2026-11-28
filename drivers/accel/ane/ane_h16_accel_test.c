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

/*
 * EXECUTE pins a plain buffer's pages before it takes the node lock and copies
 * through them under it. The copies equal what copy_from_user() and
 * copy_to_user() move, at an offset that crosses pages; an unmapped buffer,
 * a read-only output and a wrapping range fail at the pin, holding nothing.
 */
static void ane_h16_accel_user_pages_test(struct kunit *test)
{
	const size_t size = 2 * PAGE_SIZE, off = PAGE_SIZE - 24;
	struct ane_h16_user_buffer u;
	unsigned long user, ro;
	u8 *src, *dst;
	size_t i;

	user = kunit_vm_mmap(test, NULL, 0, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_ANONYMOUS | MAP_PRIVATE, 0);
	ro = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, 0);
	KUNIT_ASSERT_TRUE(test, user && !IS_ERR_VALUE(user));
	KUNIT_ASSERT_TRUE(test, ro && !IS_ERR_VALUE(ro));
	src = kunit_kzalloc(test, size, GFP_KERNEL);
	dst = kunit_kzalloc(test, size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	for (i = 0; i < size; i++)
		src[i] = i * 7 + 3;
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)(user + off), src, size), 0);

	/* An input: three pages for a buffer that starts 24 bytes before a page end. */
	KUNIT_ASSERT_EQ(test, ane_h16_user_pin(&u, user + off, size, false), 0);
	KUNIT_EXPECT_EQ(test, u.count, 3U);
	ane_h16_user_copy(&u, dst, false);
	KUNIT_EXPECT_MEMEQ(test, dst, src, size);
	KUNIT_EXPECT_FALSE(test, u.dirty);
	ane_h16_user_unpin(&u);
	KUNIT_EXPECT_NULL(test, u.pages);

	/* An output: written through the pinned pages, read back from user memory. */
	for (i = 0; i < size; i++)
		src[i] ^= 0x5a;
	KUNIT_ASSERT_EQ(test, ane_h16_user_pin(&u, user + off, size, true), 0);
	ane_h16_user_copy(&u, src, true);
	KUNIT_EXPECT_TRUE(test, u.dirty);
	ane_h16_user_unpin(&u);
	KUNIT_ASSERT_EQ(test, copy_from_user(dst, (void __user *)(user + off), size), 0);
	KUNIT_EXPECT_MEMEQ(test, dst, src, size);

	KUNIT_EXPECT_EQ(test, ane_h16_user_pin(&u, 0, 64, false), -EFAULT);
	KUNIT_EXPECT_NULL(test, u.pages);
	KUNIT_EXPECT_EQ(test, ane_h16_user_pin(&u, ro, 64, true), -EFAULT);
	KUNIT_EXPECT_NULL(test, u.pages);
	KUNIT_EXPECT_EQ(test, ane_h16_user_pin(&u, ro, 64, false), 0);
	ane_h16_user_unpin(&u);
	KUNIT_EXPECT_EQ(test, ane_h16_user_pin(&u, U64_MAX - 8, 64, false), -EFAULT);
	KUNIT_EXPECT_NULL(test, u.pages);
	/* A size whose page count would wrap to 0 without its own check. */
	KUNIT_EXPECT_EQ(test, ane_h16_user_pin(&u, PAGE_SIZE - 1,
					       SIZE_MAX - (PAGE_SIZE - 1), false), -EFAULT);
	KUNIT_EXPECT_NULL(test, u.pages);
	KUNIT_EXPECT_EQ(test, ane_h16_user_pin(&u, 0, (size_t)INT_MAX * PAGE_SIZE + 1, false),
			-EFAULT);
	KUNIT_EXPECT_NULL(test, u.pages);
}

/* Set or clear VM_IO on the mapping at @addr, which pin_user_pages_fast() then refuses. */
static void ane_h16_accel_test_vm_io(unsigned long addr, bool on)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;

	mmap_write_lock(mm);
	vma = find_vma(mm, addr);
	if (vma && vma->vm_start <= addr) {
		if (on)
			vm_flags_set(vma, VM_IO);
		else
			vm_flags_clear(vma, VM_IO);
	}
	mmap_write_unlock(mm);
}

/*
 * A buffer in a mapping whose pages cannot be pinned (VM_PFNMAP or VM_IO: a
 * GEM-shmem or dma-buf heap mmap) is bounced: an input is read when it is
 * taken, with no lock held, an output is written back when it is put, after
 * the locks drop, and only once the request copied into it. A VM_IO flag set
 * on anonymous memory is seen by the range check but not by the lockless
 * pin, so the bounce itself is called directly here.
 */
static void ane_h16_accel_user_bounce_test(struct kunit *test)
{
	const size_t size = 2 * PAGE_SIZE, off = PAGE_SIZE - 24;
	struct ane_h16_user_buffer u;
	unsigned long user, ro;
	u8 *src, *dst;
	size_t i;

	user = kunit_vm_mmap(test, NULL, 0, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, 0);
	ro = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, 0);
	KUNIT_ASSERT_TRUE(test, user && !IS_ERR_VALUE(user));
	KUNIT_ASSERT_TRUE(test, ro && !IS_ERR_VALUE(ro));
	src = kunit_kzalloc(test, size, GFP_KERNEL);
	dst = kunit_kzalloc(test, size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, src);
	KUNIT_ASSERT_NOT_NULL(test, dst);
	for (i = 0; i < size; i++)
		src[i] = i * 5 + 1;
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)(user + off), src, size), 0);

	/* The range check finds a VM_IO mapping anywhere in the range and checks access. */
	KUNIT_EXPECT_FALSE(test, ane_h16_user_unpinnable(user + off, size, false));
	KUNIT_EXPECT_FALSE(test, ane_h16_user_unpinnable(user + off, size, true));
	ane_h16_accel_test_vm_io(user, true);
	KUNIT_EXPECT_TRUE(test, ane_h16_user_unpinnable(user + off, size, false));
	KUNIT_EXPECT_TRUE(test, ane_h16_user_unpinnable(user + off, size, true));
	/* A VM_IO mapping extending into an unmapped hole fails before bouncing. */
	{
		unsigned long hole = kunit_vm_mmap(test, NULL, 0, 2 * PAGE_SIZE,
						   PROT_READ | PROT_WRITE,
						   MAP_ANONYMOUS | MAP_PRIVATE, 0);

		KUNIT_ASSERT_TRUE(test, hole && !IS_ERR_VALUE(hole));
		KUNIT_ASSERT_EQ(test, vm_munmap(hole + PAGE_SIZE, PAGE_SIZE), 0);
		ane_h16_accel_test_vm_io(hole, true);
		KUNIT_EXPECT_TRUE(test, ane_h16_user_unpinnable(hole, PAGE_SIZE, true));
		KUNIT_EXPECT_FALSE(test, ane_h16_user_unpinnable(hole, 2 * PAGE_SIZE, false));
		KUNIT_EXPECT_FALSE(test, ane_h16_user_unpinnable(hole, 2 * PAGE_SIZE, true));
		KUNIT_EXPECT_EQ(test, ane_h16_user_get(&u, hole, 2 * PAGE_SIZE, true), -EFAULT);
		KUNIT_EXPECT_NULL(test, u.bounce);
		KUNIT_EXPECT_NULL(test, u.pages);
		ane_h16_accel_test_vm_io(hole, false);
	}
	/* A read-only VM_IO mapping is readable as an input, but fails as an output. */
	ane_h16_accel_test_vm_io(ro, true);
	KUNIT_EXPECT_TRUE(test, ane_h16_user_unpinnable(ro, 64, false));
	KUNIT_EXPECT_FALSE(test, ane_h16_user_unpinnable(ro, 64, true));
	KUNIT_EXPECT_EQ(test, ane_h16_user_get(&u, ro, 64, true), -EFAULT);
	KUNIT_EXPECT_NULL(test, u.bounce);
	KUNIT_EXPECT_NULL(test, u.pages);
	ane_h16_accel_test_vm_io(ro, false);
	ane_h16_accel_test_vm_io(user, false);

	/* An input: read into the bounce copy when taken. */
	KUNIT_ASSERT_EQ(test, ane_h16_user_bounce(&u, user + off, size, false), 0);
	KUNIT_EXPECT_NULL(test, u.pages);
	KUNIT_EXPECT_NOT_NULL(test, u.bounce);
	ane_h16_user_copy(&u, dst, false);
	KUNIT_EXPECT_MEMEQ(test, dst, src, size);
	KUNIT_EXPECT_EQ(test, ane_h16_user_put(&u), 0);
	KUNIT_EXPECT_NULL(test, u.bounce);

	/* An output the request never wrote: user memory is left as it was. */
	KUNIT_ASSERT_EQ(test, ane_h16_user_bounce(&u, user + off, size, true), 0);
	KUNIT_EXPECT_EQ(test, ane_h16_user_put(&u), 0);
	KUNIT_ASSERT_EQ(test, copy_from_user(dst, (void __user *)(user + off), size), 0);
	KUNIT_EXPECT_MEMEQ(test, dst, src, size);

	/* An output: written back at the put. */
	for (i = 0; i < size; i++)
		src[i] ^= 0xa5;
	KUNIT_ASSERT_EQ(test, ane_h16_user_bounce(&u, user + off, size, true), 0);
	ane_h16_user_copy(&u, src, true);
	KUNIT_EXPECT_EQ(test, ane_h16_user_put(&u), 0);
	KUNIT_ASSERT_EQ(test, copy_from_user(dst, (void __user *)(user + off), size), 0);
	KUNIT_EXPECT_MEMEQ(test, dst, src, size);

	/* Unmapped memory, and an output in a read-only mapping, still fail when taken. */
	KUNIT_EXPECT_EQ(test, ane_h16_user_get(&u, 0, 64, false), -EFAULT);
	KUNIT_EXPECT_NULL(test, u.bounce);
	KUNIT_EXPECT_EQ(test, ane_h16_user_get(&u, ro, 64, true), -EFAULT);
	KUNIT_EXPECT_NULL(test, u.bounce);
	KUNIT_EXPECT_NULL(test, u.pages);
}

/*
 * ane_stats and wedged land on the device with the format of the other ANE
 * drivers and follow the session: its counters and its quarantine.
 */
static void ane_h16_accel_sysfs_test(struct kunit *test)
{
	struct device *dev = drm_kunit_helper_alloc_device(test);
	struct ane_h16_session *s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);
	char *buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	struct kernfs_node *kn;
	struct ane_h16_sysfs *sys;
	u64 ticket;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	KUNIT_ASSERT_NOT_NULL(test, s);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	ane_stats_counters_init(&s->sysfs_ctrs, &s->sysfs_ring, 0);
	s->sysfs_ring.slots = &s->sysfs_slot;
	sys = ane_h16_sysfs_add(dev, s);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sys);
	kn = sysfs_get_dirent(dev->kobj.sd, "ane_stats");
	KUNIT_EXPECT_NOT_NULL(test, kn);
	sysfs_put(kn);
	kn = sysfs_get_dirent(dev->kobj.sd, "wedged");
	KUNIT_EXPECT_NOT_NULL(test, kn);
	sysfs_put(kn);

	ticket = ane_stats_begin(&s->sysfs_ctrs, &s->sysfs_ring, 1000, 1);
	ane_stats_complete(&s->sysfs_ctrs, &s->sysfs_ring, ticket, 3000, -ETIMEDOUT, 0);
	KUNIT_EXPECT_GT(test, ane_h16_stats_show(dev, &sys->stats, buf), 0);
	KUNIT_EXPECT_STREQ(test, buf, "busy_ns 2000\njobs 1\n");
	KUNIT_EXPECT_EQ(test, ane_h16_wedged_show(dev, &sys->wedged, buf), 2);
	KUNIT_EXPECT_STREQ(test, buf, "0\n");
	s->quarantined = true;
	ane_h16_wedged_show(dev, &sys->wedged, buf);
	KUNIT_EXPECT_STREQ(test, buf, "1\n");
}

static struct kunit_case ane_h16_accel_cases[] = {
	KUNIT_CASE(ane_h16_load_admission_test),
	KUNIT_CASE(ane_h16_load_size_test),
	KUNIT_CASE(ane_h16_accel_fops_test),
	KUNIT_CASE(ane_h16_accel_open_test),
	KUNIT_CASE(ane_h16_accel_user_pages_test),
	KUNIT_CASE(ane_h16_accel_user_bounce_test),
	KUNIT_CASE(ane_h16_accel_sysfs_test),
	{}
};

static struct kunit_suite ane_h16_accel_suite = {
	.name = "ane-h16-accel",
	.test_cases = ane_h16_accel_cases,
};

kunit_test_suite(ane_h16_accel_suite);

MODULE_DESCRIPTION("KUnit open and user-buffer tests for the H16 ANE accel node");
MODULE_LICENSE("Dual MIT/GPL");
