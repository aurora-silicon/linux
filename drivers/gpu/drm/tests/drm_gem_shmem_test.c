// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test suite for GEM objects backed by shmem buffers
 *
 * Copyright (C) 2023 Red Hat, Inc.
 *
 * Author: Marco Pagani <marpagan@redhat.com>
 */

#include <linux/blkdev.h>
#include <linux/dma-buf.h>
#include <linux/file.h>
#include <linux/iosys-map.h>
#include <linux/major.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/sizes.h>

#include <kunit/test.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_kunit_helpers.h>

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

#define TEST_SIZE		SZ_1M
#define TEST_BYTE		0xae

/*
 * Wrappers to avoid cast warnings when passing action functions
 * directly to kunit_add_action().
 */
KUNIT_DEFINE_ACTION_WRAPPER(kfree_wrapper, kfree, const void *);

KUNIT_DEFINE_ACTION_WRAPPER(sg_free_table_wrapper, sg_free_table,
			    struct sg_table *);

KUNIT_DEFINE_ACTION_WRAPPER(drm_gem_shmem_free_wrapper, drm_gem_shmem_free,
			    struct drm_gem_shmem_object *);

KUNIT_DEFINE_ACTION_WRAPPER(drm_gem_shmem_unpin_wrapper, drm_gem_shmem_unpin,
			    struct drm_gem_shmem_object *);

/*
 * Test creating a shmem GEM object backed by shmem buffer. The test
 * case succeeds if the GEM object is successfully allocated with the
 * shmem file node and object functions attributes set, and the size
 * attribute is equal to the correct size.
 */
static void drm_gem_shmem_test_obj_create(struct kunit *test)
{
	struct drm_device *drm_dev = test->priv;
	struct drm_gem_shmem_object *shmem;

	shmem = drm_gem_shmem_create(drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);
	KUNIT_EXPECT_EQ(test, shmem->base.size, TEST_SIZE);
	KUNIT_EXPECT_NOT_NULL(test, shmem->base.filp);
	KUNIT_EXPECT_NOT_NULL(test, shmem->base.funcs);

	drm_gem_shmem_free(shmem);
}

/*
 * Test creating a shmem GEM object from a scatter/gather table exported
 * via a DMA-BUF. The test case succeed if the GEM object is successfully
 * created with the shmem file node attribute equal to NULL and the sgt
 * attribute pointing to the scatter/gather table that has been imported.
 */
static void drm_gem_shmem_test_obj_create_private(struct kunit *test)
{
	struct drm_device *drm_dev = test->priv;
	struct drm_gem_shmem_object *shmem;
	struct drm_gem_object *gem_obj;
	struct dma_buf buf_mock;
	struct dma_buf_attachment attach_mock;
	struct sg_table *sgt;
	char *buf;
	int ret;

	/* Create a mock scatter/gather table */
	buf = kunit_kzalloc(test, TEST_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	sgt = kzalloc_obj(*sgt);
	KUNIT_ASSERT_NOT_NULL(test, sgt);

	ret = kunit_add_action_or_reset(test, kfree_wrapper, sgt);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = kunit_add_action_or_reset(test, sg_free_table_wrapper, sgt);
	KUNIT_ASSERT_EQ(test, ret, 0);

	sg_init_one(sgt->sgl, buf, TEST_SIZE);

	/*
	 * Map the sgtables otherwise drm_gem_shmem_free will cause a warning on
	 * debug kernels.
	 */
	ret = dma_map_sgtable(drm_dev->dev, sgt, DMA_BIDIRECTIONAL, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Init a mock DMA-BUF */
	buf_mock.size = TEST_SIZE;
	attach_mock.dmabuf = &buf_mock;

	gem_obj = drm_gem_shmem_prime_import_sg_table(drm_dev, &attach_mock, sgt);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, gem_obj);
	KUNIT_EXPECT_EQ(test, gem_obj->size, TEST_SIZE);
	KUNIT_EXPECT_NULL(test, gem_obj->filp);
	KUNIT_EXPECT_NOT_NULL(test, gem_obj->funcs);

	/* The scatter/gather table will be freed by drm_gem_shmem_free */
	kunit_remove_action(test, sg_free_table_wrapper, sgt);
	kunit_remove_action(test, kfree_wrapper, sgt);

	shmem = to_drm_gem_shmem_obj(gem_obj);
	KUNIT_EXPECT_PTR_EQ(test, shmem->sgt, sgt);

	drm_gem_shmem_free(shmem);
}

/*
 * Test pinning backing pages for a shmem GEM object. The test case
 * succeeds if a suitable number of backing pages are allocated, and
 * the pages table counter attribute is increased by one.
 */
static void drm_gem_shmem_test_pin_pages(struct kunit *test)
{
	struct drm_device *drm_dev = test->priv;
	struct drm_gem_shmem_object *shmem;
	int i, ret;

	shmem = drm_gem_shmem_create(drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);
	KUNIT_EXPECT_NULL(test, shmem->pages);
	KUNIT_EXPECT_EQ(test, refcount_read(&shmem->pages_use_count), 0);

	ret = kunit_add_action_or_reset(test, drm_gem_shmem_free_wrapper, shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_gem_shmem_pin(shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, shmem->pages);
	KUNIT_EXPECT_EQ(test, refcount_read(&shmem->pages_use_count), 1);

	for (i = 0; i < (shmem->base.size >> PAGE_SHIFT); i++)
		KUNIT_ASSERT_NOT_NULL(test, shmem->pages[i]);

	drm_gem_shmem_unpin(shmem);
	KUNIT_EXPECT_NULL(test, shmem->pages);
	KUNIT_EXPECT_EQ(test, refcount_read(&shmem->pages_use_count), 0);
}

/*
 * Test creating a virtual mapping for a shmem GEM object. The test
 * case succeeds if the backing memory is mapped and the reference
 * counter for virtual mapping is increased by one. Moreover, the test
 * case writes and then reads a test pattern over the mapped memory.
 */
static void drm_gem_shmem_test_vmap(struct kunit *test)
{
	struct drm_device *drm_dev = test->priv;
	struct drm_gem_shmem_object *shmem;
	struct iosys_map map;
	int ret, i;

	shmem = drm_gem_shmem_create(drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);
	KUNIT_EXPECT_NULL(test, shmem->vaddr);
	KUNIT_EXPECT_EQ(test, refcount_read(&shmem->vmap_use_count), 0);

	ret = kunit_add_action_or_reset(test, drm_gem_shmem_free_wrapper, shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_gem_shmem_vmap(shmem, &map);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, shmem->vaddr);
	KUNIT_ASSERT_FALSE(test, iosys_map_is_null(&map));
	KUNIT_EXPECT_EQ(test, refcount_read(&shmem->vmap_use_count), 1);

	iosys_map_memset(&map, 0, TEST_BYTE, TEST_SIZE);
	for (i = 0; i < TEST_SIZE; i++)
		KUNIT_EXPECT_EQ(test, iosys_map_rd(&map, i, u8), TEST_BYTE);

	drm_gem_shmem_vunmap(shmem, &map);
	KUNIT_EXPECT_NULL(test, shmem->vaddr);
	KUNIT_EXPECT_EQ(test, refcount_read(&shmem->vmap_use_count), 0);
}

/*
 * Test exporting a scatter/gather table of pinned pages suitable for
 * PRIME usage from a shmem GEM object. The test case succeeds if a
 * scatter/gather table large enough to accommodate the backing memory
 * is successfully exported.
 */
static void drm_gem_shmem_test_get_sg_table(struct kunit *test)
{
	struct drm_device *drm_dev = test->priv;
	struct drm_gem_shmem_object *shmem;
	struct sg_table *sgt;
	struct scatterlist *sg;
	unsigned int si, len = 0;
	int ret;

	shmem = drm_gem_shmem_create(drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);

	ret = kunit_add_action_or_reset(test, drm_gem_shmem_free_wrapper, shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_gem_shmem_pin(shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = kunit_add_action_or_reset(test, drm_gem_shmem_unpin_wrapper, shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);

	sgt = drm_gem_shmem_get_sg_table(shmem);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sgt);
	KUNIT_EXPECT_NULL(test, shmem->sgt);

	ret = kunit_add_action_or_reset(test, kfree_wrapper, sgt);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = kunit_add_action_or_reset(test, sg_free_table_wrapper, sgt);
	KUNIT_ASSERT_EQ(test, ret, 0);

	for_each_sgtable_sg(sgt, sg, si) {
		KUNIT_EXPECT_NOT_NULL(test, sg);
		len += sg->length;
	}

	KUNIT_EXPECT_GE(test, len, TEST_SIZE);
}

/*
 * Test pinning pages and exporting a scatter/gather table suitable for
 * driver usage from a shmem GEM object. The test case succeeds if the
 * backing pages are pinned and a scatter/gather table large enough to
 * accommodate the backing memory is successfully exported.
 */
static void drm_gem_shmem_test_get_pages_sgt(struct kunit *test)
{
	struct drm_device *drm_dev = test->priv;
	struct drm_gem_shmem_object *shmem;
	struct sg_table *sgt;
	struct scatterlist *sg;
	unsigned int si, ret, len = 0;

	shmem = drm_gem_shmem_create(drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);

	ret = kunit_add_action_or_reset(test, drm_gem_shmem_free_wrapper, shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* The scatter/gather table will be freed by drm_gem_shmem_free */
	sgt = drm_gem_shmem_get_pages_sgt(shmem);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sgt);
	KUNIT_ASSERT_NOT_NULL(test, shmem->pages);
	KUNIT_EXPECT_EQ(test, refcount_read(&shmem->pages_use_count), 1);
	KUNIT_EXPECT_PTR_EQ(test, sgt, shmem->sgt);

	for_each_sgtable_sg(sgt, sg, si) {
		KUNIT_EXPECT_NOT_NULL(test, sg);
		len += sg->length;
	}

	KUNIT_EXPECT_GE(test, len, TEST_SIZE);
}

/*
 * Test updating the madvise state of a shmem GEM object. The test
 * case checks that the function for setting madv updates it only if
 * its current value is greater or equal than zero and returns false
 * if it has a negative value.
 */
static void drm_gem_shmem_test_madvise(struct kunit *test)
{
	struct drm_device *drm_dev = test->priv;
	struct drm_gem_shmem_object *shmem;
	int ret;

	shmem = drm_gem_shmem_create(drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);
	KUNIT_ASSERT_EQ(test, shmem->madv, 0);

	ret = kunit_add_action_or_reset(test, drm_gem_shmem_free_wrapper, shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_gem_shmem_madvise(shmem, 1);
	KUNIT_EXPECT_TRUE(test, ret);
	KUNIT_ASSERT_EQ(test, shmem->madv, 1);

	/* Set madv to a negative value */
	ret = drm_gem_shmem_madvise(shmem, -1);
	KUNIT_EXPECT_FALSE(test, ret);
	KUNIT_ASSERT_EQ(test, shmem->madv, -1);

	/* Check that madv cannot be set back to a positive value */
	ret = drm_gem_shmem_madvise(shmem, 0);
	KUNIT_EXPECT_FALSE(test, ret);
	KUNIT_ASSERT_EQ(test, shmem->madv, -1);
}

/*
 * Test purging a shmem GEM object. First, assert that a newly created
 * shmem GEM object is not purgeable. Then, set madvise to a positive
 * value and call drm_gem_shmem_get_pages_sgt() to pin and dma-map the
 * backing pages. Finally, assert that the shmem GEM object is now
 * purgeable and purge it.
 */
static void drm_gem_shmem_test_purge(struct kunit *test)
{
	struct drm_device *drm_dev = test->priv;
	struct drm_gem_shmem_object *shmem;
	struct sg_table *sgt;
	int ret;

	shmem = drm_gem_shmem_create(drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);

	ret = kunit_add_action_or_reset(test, drm_gem_shmem_free_wrapper, shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_gem_shmem_is_purgeable(shmem);
	KUNIT_EXPECT_FALSE(test, ret);

	ret = drm_gem_shmem_madvise(shmem, 1);
	KUNIT_EXPECT_TRUE(test, ret);

	/* The scatter/gather table will be freed by drm_gem_shmem_free */
	sgt = drm_gem_shmem_get_pages_sgt(shmem);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sgt);

	ret = drm_gem_shmem_is_purgeable(shmem);
	KUNIT_EXPECT_TRUE(test, ret);

	ret = drm_gem_shmem_purge(shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_EXPECT_NULL(test, shmem->pages);
	KUNIT_EXPECT_NULL(test, shmem->sgt);
	KUNIT_EXPECT_EQ(test, shmem->madv, -1);
}

/* Test the opt-in mapping policy and real population, including retry after
 * releasing the backing. No large allocation is needed to exercise the path.
 */
static void drm_gem_shmem_test_bounded_reclaim(struct kunit *test)
{
	struct drm_device *drm_dev = test->priv;
	struct drm_gem_shmem_object *shmem;
	gfp_t gfp;
	int ret;

	shmem = kzalloc_obj(*shmem);
	KUNIT_ASSERT_NOT_NULL(test, shmem);
	shmem->pages_bounded_reclaim = true;
	ret = drm_gem_shmem_init(drm_dev, shmem, TEST_SIZE);
	if (ret) {
		kfree(shmem);
		KUNIT_FAIL(test, "shmem init failed: %d", ret);
		return;
	}
	ret = kunit_add_action_or_reset(test, drm_gem_shmem_free_wrapper, shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);
	gfp = mapping_gfp_mask(shmem->base.filp->f_mapping);
	KUNIT_EXPECT_NE(test, gfp & __GFP_DIRECT_RECLAIM, 0);
	KUNIT_EXPECT_EQ(test, gfp & __GFP_IO, 0);
	KUNIT_EXPECT_EQ(test, gfp & __GFP_FS, 0);
	KUNIT_EXPECT_NE(test, gfp & __GFP_RETRY_MAYFAIL, 0);
	KUNIT_EXPECT_EQ(test, gfp & __GFP_NOFAIL, 0);
	KUNIT_EXPECT_NE(test, gfp & __GFP_KSWAPD_RECLAIM, 0);
	KUNIT_EXPECT_EQ(test, gfp & __GFP_MOVABLE, 0);
	KUNIT_EXPECT_NE(test, gfp & __GFP_HIGHMEM, 0);
	ret = drm_gem_shmem_pin(shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_NOT_NULL(test, shmem->pages);
	drm_gem_shmem_unpin(shmem);
	ret = drm_gem_shmem_pin(shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);
	drm_gem_shmem_unpin(shmem);
}

/* Exercise real allocation failure in a small KUnit VM. Request all physical
 * RAM as one pinned object, which cannot fit alongside the running kernel.
 * Limit this stress case to <= 2 GiB VMs; do not exhaust a developer's host.
 */
static void drm_gem_shmem_test_free_flush(void *object)
{
	drm_gem_shmem_free(object);
	/* KUnit cases run in kernel threads: there is no return to userspace
	 * to drain fput task work before the next case needs this memory.
	 */
	flush_delayed_fput();
}

static void drm_gem_shmem_test_bounded_reclaim_oom(struct kunit *test)
{
	struct drm_device *drm_dev = test->priv;
	struct drm_gem_shmem_object *shmem;
	u64 bytes = (u64)totalram_pages() << PAGE_SHIFT;
	ktime_t start;
	s64 elapsed_ms;
	int ret;

	if (bytes > SZ_2G) {
		kunit_skip(test, "requires a VM with at most 2 GiB RAM");
		return;
	}
	shmem = kzalloc_obj(*shmem);
	KUNIT_ASSERT_NOT_NULL(test, shmem);
	shmem->pages_bounded_reclaim = true;
	ret = drm_gem_shmem_init(drm_dev, shmem, bytes);
	if (ret) {
		kfree(shmem);
		KUNIT_FAIL(test, "shmem init failed: %d", ret);
		return;
	}
	ret = kunit_add_action_or_reset(test, drm_gem_shmem_test_free_flush, shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);
	start = ktime_get();
	ret = drm_gem_shmem_pin(shmem);
	elapsed_ms = ktime_ms_delta(ktime_get(), start);
	kunit_info(test, "oversized pin: %llu bytes, ret=%d, %lld ms\n",
		   bytes, ret, elapsed_ms);
	if (!ret)
		drm_gem_shmem_unpin(shmem);
	KUNIT_EXPECT_EQ(test, ret, -ENOMEM);
	KUNIT_EXPECT_LT(test, elapsed_ms, (s64)10000);
	KUNIT_EXPECT_NULL(test, shmem->pages);
	KUNIT_EXPECT_EQ(test, refcount_read(&shmem->pages_use_count), 0);
	KUNIT_EXPECT_FALSE(test, mapping_unevictable(shmem->base.filp->f_mapping));
}

/*
 * The page cache case: memory is full of clean, reclaimable file pages and
 * almost none is free. A bounded-reclaim object larger than free memory must
 * still be populated, by dropping cache. The cache is that of a RAM disk
 * (brd, e.g. brd.rd_nr=1 brd.rd_size=1048576), read through its block device
 * mapping: ordinary file LRU pages, as kswapd and direct reclaim see them.
 */
/* Page cache pages, excluding shmem (GEM objects included). */
static unsigned long drm_gem_shmem_test_cached(void)
{
	return global_node_page_state(NR_FILE_PAGES) - global_node_page_state(NR_SHMEM);
}

static void drm_gem_shmem_test_fput(void *file)
{
	fput(file);
}

static void drm_gem_shmem_test_bounded_reclaim_cache(struct kunit *test)
{
	struct drm_device *drm_dev = test->priv;
	u64 ram = (u64)totalram_pages() << PAGE_SHIFT;
	struct drm_gem_shmem_object *shmem;
	const size_t size = SZ_256M;
	unsigned long free, cached;
	struct folio *folio;
	struct file *bdev;
	pgoff_t i, n;
	ktime_t start;
	s64 elapsed_ms;
	int ret;

	if (ram > SZ_2G) {
		kunit_skip(test, "requires a VM with at most 2 GiB RAM");
		return;
	}
	bdev = bdev_file_open_by_dev(MKDEV(RAMDISK_MAJOR, 0), BLK_OPEN_READ, NULL, NULL);
	if (IS_ERR(bdev)) {
		kunit_skip(test, "requires a RAM disk (brd): %ld", PTR_ERR(bdev));
		return;
	}
	/* Last close drops the cache; keep it open until the object is gone. */
	ret = kunit_add_action_or_reset(test, drm_gem_shmem_test_fput, bdev);
	KUNIT_ASSERT_EQ(test, ret, 0);
	n = min_t(u64, bdev_nr_bytes(file_bdev(bdev)), ram) >> PAGE_SHIFT;

	for (i = 0; i < n; i += folio_nr_pages(folio)) {
		folio = read_mapping_folio(bdev->f_mapping, i, bdev);
		if (IS_ERR(folio))
			break;
		folio_put(folio);
	}
	free = global_zone_page_state(NR_FREE_PAGES);
	cached = drm_gem_shmem_test_cached();
	kunit_info(test, "page cache: %lu MiB cached, %lu MiB free\n",
		   cached >> (20 - PAGE_SHIFT), free >> (20 - PAGE_SHIFT));
	if (((u64)free << PAGE_SHIFT) >= size || ((u64)cached << PAGE_SHIFT) < size) {
		kunit_skip(test, "page cache did not fill memory (RAM disk too small?)");
		return;
	}

	shmem = kzalloc_obj(*shmem);
	KUNIT_ASSERT_NOT_NULL(test, shmem);
	shmem->pages_bounded_reclaim = true;
	ret = drm_gem_shmem_init(drm_dev, shmem, size);
	if (ret) {
		kfree(shmem);
		KUNIT_FAIL(test, "shmem init failed: %d", ret);
		return;
	}
	ret = kunit_add_action_or_reset(test, drm_gem_shmem_test_free_flush, shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);

	start = ktime_get();
	ret = drm_gem_shmem_pin(shmem);
	elapsed_ms = ktime_ms_delta(ktime_get(), start);
	kunit_info(test, "pin of %zu MiB over page cache: ret=%d, %lld ms, %lu MiB cached after\n",
		   size >> 20, ret, elapsed_ms,
		   drm_gem_shmem_test_cached() >> (20 - PAGE_SHIFT));
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_LT(test, elapsed_ms, (s64)10000);
	if (!ret) {
		KUNIT_EXPECT_NOT_NULL(test, shmem->pages);
		drm_gem_shmem_unpin(shmem);
	}
}

static int drm_gem_shmem_test_init(struct kunit *test)
{
	struct device *dev;
	struct drm_device *drm_dev;
	int ret;

	/* Allocate a parent device */
	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	/*
	 * Set the DMA mask to 64-bits to avoid intermittent failures calling
	 * drm_gem_shmem_get_pages_sgt().
	 */
	ret = dma_set_mask(dev, DMA_BIT_MASK(64));
	KUNIT_ASSERT_EQ(test, ret, 0);

	/*
	 * The DRM core will automatically initialize the GEM core and create
	 * a DRM Memory Manager object which provides an address space pool
	 * for GEM objects allocation.
	 */
	drm_dev = __drm_kunit_helper_alloc_drm_device(test, dev, sizeof(*drm_dev),
						      0, DRIVER_GEM);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, drm_dev);

	test->priv = drm_dev;

	return 0;
}

static struct kunit_case drm_gem_shmem_test_cases[] = {
	KUNIT_CASE(drm_gem_shmem_test_obj_create),
	KUNIT_CASE(drm_gem_shmem_test_bounded_reclaim),
	KUNIT_CASE_SLOW(drm_gem_shmem_test_bounded_reclaim_oom),
	KUNIT_CASE_SLOW(drm_gem_shmem_test_bounded_reclaim_cache),
	KUNIT_CASE(drm_gem_shmem_test_obj_create_private),
	KUNIT_CASE(drm_gem_shmem_test_pin_pages),
	KUNIT_CASE(drm_gem_shmem_test_vmap),
	KUNIT_CASE(drm_gem_shmem_test_get_sg_table),
	KUNIT_CASE(drm_gem_shmem_test_get_pages_sgt),
	KUNIT_CASE(drm_gem_shmem_test_madvise),
	KUNIT_CASE(drm_gem_shmem_test_purge),
	{}
};

static struct kunit_suite drm_gem_shmem_suite = {
	.name = "drm_gem_shmem",
	.init = drm_gem_shmem_test_init,
	.test_cases = drm_gem_shmem_test_cases
};

kunit_test_suite(drm_gem_shmem_suite);

MODULE_DESCRIPTION("KUnit test suite for GEM objects backed by shmem buffers");
MODULE_LICENSE("GPL");
