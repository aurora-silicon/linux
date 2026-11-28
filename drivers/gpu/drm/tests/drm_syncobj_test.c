// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for drm_syncobj timeline direct-wait fence unwrapping.
 */

#include <kunit/test.h>
#include <linux/dma-fence-chain.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>

#include <drm/drm_file.h>
#include <drm/drm_syncobj.h>

struct test_fence {
	struct dma_fence base;
	spinlock_t lock;
};

static const char *test_fence_driver_name(struct dma_fence *fence)
{
	return "drm_syncobj_test";
}

static const char *test_fence_timeline_name(struct dma_fence *fence)
{
	return "test";
}

static const struct dma_fence_ops test_fence_ops = {
	.get_driver_name = test_fence_driver_name,
	.get_timeline_name = test_fence_timeline_name,
};

static struct dma_fence *alloc_test_fence(u64 ctx, u64 seqno)
{
	struct test_fence *tf = kzalloc(sizeof(*tf), GFP_KERNEL);

	if (!tf)
		return NULL;
	spin_lock_init(&tf->lock);
	dma_fence_init(&tf->base, &test_fence_ops, &tf->lock, ctx, seqno);
	return &tf->base;
}

static struct drm_file *alloc_test_file(struct kunit *test)
{
	struct drm_file *file = kunit_kzalloc(test, sizeof(*file), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, file);
	xa_init_flags(&file->syncobj_xa, XA_FLAGS_ALLOC1);
	return file;
}

static void free_test_file(struct drm_file *file)
{
	struct drm_syncobj *syncobj;
	unsigned long handle;

	xa_for_each(&file->syncobj_xa, handle, syncobj)
		drm_syncobj_put(syncobj);
	xa_destroy(&file->syncobj_xa);
}

static u32 create_test_syncobj(struct kunit *test, struct drm_file *file,
			       struct drm_syncobj **out)
{
	struct drm_syncobj *syncobj;
	u32 handle = 0;
	int ret;

	ret = drm_syncobj_create(&syncobj, 0, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = drm_syncobj_get_handle(file, syncobj, &handle);
	drm_syncobj_put(syncobj);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_GT(test, handle, 0U);

	*out = syncobj;
	return handle;
}

static void drm_syncobj_test_direct_wait_depth1_sequence(struct kunit *test)
{
	struct drm_file *file = alloc_test_file(test);
	struct drm_syncobj *syncobj;
	u32 handle = create_test_syncobj(test, file, &syncobj);
	u64 ctx = dma_fence_context_alloc(1);
	int i;

	/*
	 * Consecutive depth-1 timeline submits where each previous contained
	 * fence is signaled without ever signaling its dma_fence_chain wrapper:
	 * every point must unwrap directly to its contained fence.
	 */
	for (i = 1; i <= 10; i++) {
		struct dma_fence *payload = alloc_test_fence(ctx, i);
		struct dma_fence_chain *chain = dma_fence_chain_alloc();
		struct dma_fence *found = NULL;
		int ret;

		KUNIT_ASSERT_NOT_NULL(test, payload);
		KUNIT_ASSERT_NOT_NULL(test, chain);

		drm_syncobj_add_point(syncobj, chain, payload, i);

		ret = drm_syncobj_find_fence(file, handle, i, 0, &found);
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_EXPECT_PTR_EQ(test, found, payload);
		KUNIT_EXPECT_NULL(test, to_dma_fence_chain(found));
		dma_fence_put(found);

		/* Signal only the contained payload, leaving the chain wrapper unset. */
		dma_fence_signal(payload);
		dma_fence_put(payload);
	}

	free_test_file(file);
}

static void drm_syncobj_test_direct_wait_unsignaled_prev_stays_chain(struct kunit *test)
{
	struct drm_file *file = alloc_test_file(test);
	struct drm_syncobj *syncobj;
	u32 handle = create_test_syncobj(test, file, &syncobj);
	u64 ctx = dma_fence_context_alloc(1);
	struct dma_fence *f1 = alloc_test_fence(ctx, 1);
	struct dma_fence *f2 = alloc_test_fence(ctx, 2);
	struct dma_fence *f3 = alloc_test_fence(ctx, 3);
	struct dma_fence_chain *c1 = dma_fence_chain_alloc();
	struct dma_fence_chain *c2 = dma_fence_chain_alloc();
	struct dma_fence_chain *c3 = dma_fence_chain_alloc();
	struct dma_fence *found = NULL;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, f1);
	KUNIT_ASSERT_NOT_NULL(test, f2);
	KUNIT_ASSERT_NOT_NULL(test, f3);
	KUNIT_ASSERT_NOT_NULL(test, c1);
	KUNIT_ASSERT_NOT_NULL(test, c2);
	KUNIT_ASSERT_NOT_NULL(test, c3);

	drm_syncobj_add_point(syncobj, c1, f1, 1);
	drm_syncobj_add_point(syncobj, c2, f2, 2);
	drm_syncobj_add_point(syncobj, c3, f3, 3);

	/* f2 signaled while earlier f1 is still unsignaled: must stay wrapped. */
	dma_fence_signal(f2);
	ret = drm_syncobj_find_fence(file, handle, 3, 0, &found);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_NOT_NULL(test, to_dma_fence_chain(found));
	dma_fence_put(found);

	/* Once f1 signals too (without signaling c1/c2), point 3 unwraps to f3. */
	dma_fence_signal(f1);
	ret = drm_syncobj_find_fence(file, handle, 3, 0, &found);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, found, f3);
	KUNIT_EXPECT_NULL(test, to_dma_fence_chain(found));
	dma_fence_put(found);

	dma_fence_signal(f3);
	dma_fence_put(f1);
	dma_fence_put(f2);
	dma_fence_put(f3);
	free_test_file(file);
}

static void drm_syncobj_test_direct_wait_deep_chain_bounded(struct kunit *test)
{
	struct drm_file *file = alloc_test_file(test);
	struct drm_syncobj *syncobj;
	u32 handle = create_test_syncobj(test, file, &syncobj);
	u64 ctx = dma_fence_context_alloc(1);
	struct dma_fence *fences[11];
	struct dma_fence *found = NULL;
	int i, ret;

	/*
	 * Add 11 unsignaled points so add_point() cannot garbage-collect them,
	 * then signal the first 10 contained fences without signaling their
	 * chain wrappers. Walking is bounded at 8 predecessors, so point 11
	 * must stay wrapped in its chain.
	 */
	for (i = 0; i < ARRAY_SIZE(fences); i++) {
		struct dma_fence_chain *chain = dma_fence_chain_alloc();

		fences[i] = alloc_test_fence(ctx, i + 1);
		KUNIT_ASSERT_NOT_NULL(test, fences[i]);
		KUNIT_ASSERT_NOT_NULL(test, chain);
		drm_syncobj_add_point(syncobj, chain, fences[i], i + 1);
	}

	for (i = 0; i < ARRAY_SIZE(fences) - 1; i++)
		dma_fence_signal(fences[i]);

	ret = drm_syncobj_find_fence(file, handle, ARRAY_SIZE(fences), 0, &found);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_NOT_NULL(test, to_dma_fence_chain(found));
	dma_fence_put(found);

	dma_fence_signal(fences[ARRAY_SIZE(fences) - 1]);
	for (i = 0; i < ARRAY_SIZE(fences); i++)
		dma_fence_put(fences[i]);
	free_test_file(file);
}

static struct kunit_case drm_syncobj_test_cases[] = {
	KUNIT_CASE(drm_syncobj_test_direct_wait_depth1_sequence),
	KUNIT_CASE(drm_syncobj_test_direct_wait_unsignaled_prev_stays_chain),
	KUNIT_CASE(drm_syncobj_test_direct_wait_deep_chain_bounded),
	{}
};

static struct kunit_suite drm_syncobj_test_suite = {
	.name = "drm_syncobj_direct_wait",
	.test_cases = drm_syncobj_test_cases,
};

kunit_test_suite(drm_syncobj_test_suite);

MODULE_DESCRIPTION("KUnit tests for drm_syncobj direct-wait fence unwrapping");
MODULE_LICENSE("GPL");
