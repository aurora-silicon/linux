// SPDX-License-Identifier: GPL-2.0-only
/* Backing ownership tests; these slots are not fault/rmap-owned mappings. */
#include <kunit/test.h>
#include <linux/highmem.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mm_subpage.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#ifdef CONFIG_NET
#include <linux/skbuff.h>
#if IS_ENABLED(CONFIG_NET_SOCK_MSG) && IS_ENABLED(CONFIG_INET)
#include <linux/skmsg.h>
#endif
#ifdef CONFIG_INET
#include <linux/net.h>
#include <net/net_namespace.h>
#endif
#include <linux/uio.h>
#endif
#include <asm/cpufeature.h>
#include <asm/mte.h>

#define DATA_SLOTS MM_SUBPAGES_PER_PAGE

struct data_fixture {
	struct mm_subpage_pool *pool;
	struct mm_subpage *held[2 * DATA_SLOTS + 2];
};

static void data_fixture_free(void *arg)
{
	struct data_fixture *fixture = arg;
	unsigned int i;

	if (fixture->pool)
		mm_subpage_pool_close(fixture->pool);
	for (i = 0; i < ARRAY_SIZE(fixture->held); i++)
		if (fixture->held[i])
			mm_subpage_put(fixture->held[i]);
	if (fixture->pool)
		mm_subpage_pool_put(fixture->pool);
}

static struct data_fixture *data_fixture_create_granule(struct kunit *test, unsigned int shift)
{
	struct data_fixture *fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);

	if (!fixture)
		return NULL;
	fixture->pool = mm_subpage_pool_create_granule(GFP_KERNEL, shift);
	if (!fixture->pool)
		return NULL;
	if (kunit_add_action_or_reset(test, data_fixture_free, fixture))
		return NULL;
	return fixture;
}

static struct data_fixture *data_fixture_create(struct kunit *test)
{
	return data_fixture_create_granule(test, MM_SUBPAGE_SHIFT);
}

static void put_held(struct data_fixture *fixture, unsigned int index)
{
	mm_subpage_put(fixture->held[index]);
	fixture->held[index] = NULL;
}

static struct mm_subpage *hold_alloc(struct data_fixture *fixture, unsigned int index)
{
	struct mm_subpage *slot = mm_subpage_alloc(fixture->pool);

	if (!IS_ERR(slot))
		fixture->held[index] = slot;
	return slot;
}

static int supply_backing(struct mm_subpage_pool *pool)
{
	struct folio *folio = folio_alloc(GFP_KERNEL, 0);
	int ret;

	if (!folio)
		return -ENOMEM;
	/* Check that stale allocation contents never become exposed. */
	memset(folio_address(folio), 0xcc, PAGE_SIZE);
	ret = mm_subpage_pool_add_folio(pool, folio, GFP_KERNEL);
	if (ret)
		folio_put(folio);
	return ret;
}

static int supply_backing_at(struct mm_subpage_pool *pool, unsigned int offset)
{
	struct folio *folio = folio_alloc(GFP_KERNEL, 0);
	int ret;

	if (!folio)
		return -ENOMEM;
	ret = mm_subpage_pool_add_folio_at(pool, folio, offset, GFP_KERNEL);
	if (ret)
		folio_put(folio);
	return ret;
}

static struct mm_subpage *hold_alloc_at(struct data_fixture *fixture, unsigned int index,
				       unsigned int offset)
{
	struct mm_subpage *slot = mm_subpage_alloc_at(fixture->pool, offset);

	if (!IS_ERR(slot))
		fixture->held[index] = slot;
	return slot;
}

static void *slot_address(struct mm_subpage *slot)
{
	return folio_address(mm_subpage_folio(slot)) + mm_subpage_offset(slot);
}

static void user4k_data_reuse_test(struct kunit *test)
{
	struct data_fixture *fixture = data_fixture_create(test);
	struct folio *folio;
	struct mm_subpage *slot;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_alloc(fixture->pool)), -EAGAIN);
	KUNIT_ASSERT_EQ(test, supply_backing(fixture->pool), 0);
	KUNIT_EXPECT_EQ(test, supply_backing(fixture->pool), -EEXIST);
	for (i = 0; i < DATA_SLOTS; i++) {
		slot = hold_alloc(fixture, i);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, slot);
		folio = mm_subpage_folio(slot);
		KUNIT_EXPECT_EQ(test, folio_ref_count(folio), (int)i + 1);
		KUNIT_EXPECT_PTR_EQ(test, folio, mm_subpage_folio(fixture->held[0]));
		KUNIT_EXPECT_EQ(test, mm_subpage_offset(slot), i * SZ_4K);
		KUNIT_EXPECT_EQ(test, mm_subpage_phys(slot), PFN_PHYS(folio_pfn(folio)) + i * SZ_4K);
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(slot), 0, SZ_4K), NULL);
		memset(slot_address(slot), 0x10 + i, SZ_4K);
	}
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(fixture->pool), 1UL);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_alloc(fixture->pool)), -EAGAIN);
	/* A pin, alias or deferred-TLB holder keeps this slot unavailable. */
	fixture->held[DATA_SLOTS] = fixture->held[1];
	mm_subpage_get(fixture->held[DATA_SLOTS]);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), (int)DATA_SLOTS + 1);
	put_held(fixture, 1);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), (int)DATA_SLOTS);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_alloc(fixture->pool)), -EAGAIN);
	put_held(fixture, DATA_SLOTS);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), (int)DATA_SLOTS - 1);
	slot = hold_alloc(fixture, DATA_SLOTS + 1);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, slot);
	KUNIT_EXPECT_EQ(test, mm_subpage_offset(slot), (unsigned int)SZ_4K);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(slot), 0, SZ_4K), NULL);
	for (i = 0; i < DATA_SLOTS; i++) {
		if (i != 1)
			KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(fixture->held[i]),
						   0x10 + i, SZ_4K), NULL);
	}
	mm_subpage_pool_close(fixture->pool);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_alloc(fixture->pool)), -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, supply_backing(fixture->pool), -ESHUTDOWN);
	for (i = 0; i < DATA_SLOTS; i++)
		if (i != 1)
			put_held(fixture, i);
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(fixture->pool), 1UL);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 1);
	put_held(fixture, DATA_SLOTS + 1);
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(fixture->pool), 0UL);
}

static void user4k_data_copy_test(struct kunit *test)
{
	struct data_fixture *source = data_fixture_create(test);
	struct data_fixture *dest = data_fixture_create(test);
	struct mm_subpage *copy;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, source);
	KUNIT_ASSERT_NOT_NULL(test, dest);
	KUNIT_ASSERT_EQ(test, supply_backing(source->pool), 0);
	KUNIT_ASSERT_EQ(test, supply_backing(dest->pool), 0);
	for (i = 0; i < DATA_SLOTS; i++) {
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc(source, i));
		memset(slot_address(source->held[i]), 0x50 + i, SZ_4K);
	}
	source->held[DATA_SLOTS] = source->held[1];
	mm_subpage_get(source->held[DATA_SLOTS]);
	copy = mm_subpage_copy(dest->pool, source->held[1]);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, copy);
	dest->held[0] = copy;
	KUNIT_EXPECT_NE(test, mm_subpage_phys(copy), mm_subpage_phys(source->held[1]));
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(copy), 0x51, SZ_4K), NULL);
	memset(slot_address(copy), 0xee, SZ_4K);
	for (i = 0; i < DATA_SLOTS; i++)
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(source->held[i]), 0x50 + i, SZ_4K), NULL);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_copy(source->pool, source->held[1])), -EAGAIN);
	/* Copy into a free neighboring slot in the same physical owner. */
	put_held(source, 2);
	copy = mm_subpage_copy(source->pool, source->held[1]);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, copy);
	source->held[2] = copy;
	KUNIT_EXPECT_PTR_EQ(test, mm_subpage_folio(copy), mm_subpage_folio(source->held[1]));
	KUNIT_EXPECT_EQ(test, mm_subpage_offset(copy), 2U * SZ_4K);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(copy), 0x51, SZ_4K), NULL);
	memset(slot_address(copy), 0xdd, SZ_4K);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(source->held[1]), 0x51, SZ_4K), NULL);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(source->held[3]), 0x53, SZ_4K), NULL);
}

static void user4k_data_lookup_test(struct kunit *test)
{
	struct data_fixture *fixture = data_fixture_create(test);
	struct mm_subpage *slot, *found;
	struct folio *folio;
	phys_addr_t phys;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	folio = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	phys = PFN_PHYS(folio_pfn(folio));
	KUNIT_EXPECT_PTR_EQ(test, mm_subpage_get_from_phys(phys), NULL);
	if (mm_subpage_pool_add_folio(fixture->pool, folio, GFP_KERNEL)) {
		folio_put(folio);
		KUNIT_FAIL(test, "failed to supply lookup backing");
		return;
	}
	/* A published owner does not expose uninitialised or unissued slots. */
	for (i = 0; i < DATA_SLOTS; i++)
		KUNIT_EXPECT_PTR_EQ(test, mm_subpage_get_from_phys(phys + i * SZ_4K), NULL);
	for (i = 0; i < DATA_SLOTS; i++) {
		slot = hold_alloc(fixture, i);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, slot);
		memset(slot_address(slot), 0x90 + i, SZ_4K);
		KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 2 * (int)i + 1);
		if (!i)
			KUNIT_EXPECT_EQ(test, mm_subpage_pool_add_folio(fixture->pool, folio,
									      GFP_KERNEL), -EEXIST);
		found = mm_subpage_get_from_phys(phys + i * SZ_4K);
		KUNIT_ASSERT_NOT_NULL(test, found);
		fixture->held[i + DATA_SLOTS] = found;
		KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 2 * (int)i + 2);
		KUNIT_EXPECT_PTR_EQ(test, found, slot);
		KUNIT_EXPECT_PTR_EQ(test, mm_subpage_get_from_phys(phys + i * SZ_4K + 1), NULL);
	}
	/* A second pool must not zero or acquire already owned backing. */
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_add_folio(fixture->pool, folio, GFP_KERNEL),
			-EINVAL);
	for (i = 0; i < DATA_SLOTS; i++) {
		put_held(fixture, i);
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(fixture->held[i + DATA_SLOTS]),
						   0x90 + i, SZ_4K), NULL);
	}
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_alloc(fixture->pool)), -EAGAIN);
	put_held(fixture, DATA_SLOTS + 1);
	KUNIT_EXPECT_PTR_EQ(test, mm_subpage_get_from_phys(phys + SZ_4K), NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc(fixture, 1));
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(fixture->held[1]), 0, SZ_4K), NULL);
	mm_subpage_pool_close(fixture->pool);
	/* Keep the physical allocation stable while checking final unbinding. */
	folio_get(folio);
	put_held(fixture, 1);
	for (i = 0; i < DATA_SLOTS; i++)
		if (i != 1)
			put_held(fixture, i + DATA_SLOTS);
	for (i = 0; i < DATA_SLOTS; i++)
		KUNIT_EXPECT_PTR_EQ(test, mm_subpage_get_from_phys(phys + i * SZ_4K), NULL);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 1);
	folio_put(folio);
}

static void user4k_data_exact_slot_test(struct kunit *test)
{
	struct data_fixture *fixture = data_fixture_create(test);
	struct mm_subpage *source, *copy, *reused;
	struct folio *first;
	static const unsigned int neighbors[] = { 0, SZ_4K, 3 * SZ_4K };
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_EXPECT_EQ(test, supply_backing_at(fixture->pool, 1), -EINVAL);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_alloc_at(fixture->pool, PAGE_SIZE)), -EINVAL);
	KUNIT_ASSERT_EQ(test, supply_backing_at(fixture->pool, SZ_8K), 0);
	source = hold_alloc_at(fixture, 0, SZ_8K);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, source);
	first = mm_subpage_folio(source);
	KUNIT_EXPECT_EQ(test, mm_subpage_offset(source), (unsigned int)SZ_8K);
	memset(slot_address(source), 0xa7, SZ_4K);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_copy_at(fixture->pool, source, 1)), -EINVAL);
	/* Three free neighbors cannot satisfy a request for this occupied slot. */
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_copy_at(fixture->pool, source, SZ_8K)), -EAGAIN);
	KUNIT_EXPECT_EQ(test, supply_backing(fixture->pool), -EEXIST);
	KUNIT_ASSERT_EQ(test, supply_backing_at(fixture->pool, SZ_8K), 0);
	KUNIT_EXPECT_EQ(test, supply_backing_at(fixture->pool, SZ_8K), -EEXIST);
	copy = mm_subpage_copy_at(fixture->pool, source, SZ_8K);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, copy);
	fixture->held[1] = copy;
	KUNIT_EXPECT_PTR_NE(test, mm_subpage_folio(copy), first);
	KUNIT_EXPECT_EQ(test, mm_subpage_offset(copy), (unsigned int)SZ_8K);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(copy), 0xa7, SZ_4K), NULL);
	memset(slot_address(copy), 0x4b, SZ_4K);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(source), 0xa7, SZ_4K), NULL);
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(fixture->pool), 2UL);
	for (i = 0; i < ARRAY_SIZE(neighbors); i++) {
		struct mm_subpage *slot = hold_alloc_at(fixture, i + 2, neighbors[i]);

		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, slot);
		KUNIT_EXPECT_PTR_EQ(test, mm_subpage_folio(slot), first);
		KUNIT_EXPECT_EQ(test, mm_subpage_offset(slot), neighbors[i]);
		memset(slot_address(slot), 0x30 + i, SZ_4K);
	}
	KUNIT_EXPECT_EQ(test, folio_ref_count(first), 4);
	fixture->held[5] = source;
	mm_subpage_get(source);
	put_held(fixture, 0);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_alloc_at(fixture->pool, SZ_8K)), -EAGAIN);
	put_held(fixture, 5);
	reused = hold_alloc_at(fixture, 6, SZ_8K);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, reused);
	KUNIT_EXPECT_PTR_EQ(test, mm_subpage_folio(reused), first);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(reused), 0, SZ_4K), NULL);
	KUNIT_EXPECT_EQ(test, folio_ref_count(first), 4);
	for (i = 0; i < ARRAY_SIZE(neighbors); i++)
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(fixture->held[i + 2]),
						   0x30 + i, SZ_4K), NULL);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(copy), 0x4b, SZ_4K), NULL);
	mm_subpage_pool_close(fixture->pool);
	for (i = 2; i <= 4; i++)
		put_held(fixture, i);
	put_held(fixture, 6);
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(fixture->pool), 1UL);
	put_held(fixture, 1);
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(fixture->pool), 0UL);
}

static void user4k_data_close_test(struct kunit *test)
{
	struct data_fixture *fixture = data_fixture_create(test);
	struct mm_subpage *slot;

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_EQ(test, supply_backing(fixture->pool), 0);
	/* Closing with no issued slots must release that entire unused owner. */
	mm_subpage_pool_close(fixture->pool);
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(fixture->pool), 0UL);
	fixture = data_fixture_create(test);
	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_EQ(test, supply_backing(fixture->pool), 0);
	slot = hold_alloc(fixture, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, slot);
	memset(slot_address(slot), 0xa7, SZ_4K);
	mm_subpage_pool_close(fixture->pool);
	mm_subpage_pool_put(fixture->pool);
	fixture->pool = NULL;
	/* The originating pool/mm reference is gone; a child's slot remains live. */
	fixture->held[1] = slot;
	mm_subpage_get(slot);
	put_held(fixture, 0);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(slot), 0xa7, SZ_4K), NULL);
	put_held(fixture, 1);
}

struct deferred_slot {
	struct rcu_head rcu;
	struct mm_subpage *slot;
	struct completion done;
};

static void deferred_slot_put(struct rcu_head *rcu)
{
	struct deferred_slot *pending = container_of(rcu, struct deferred_slot, rcu);

	mm_subpage_put(pending->slot);
	complete(&pending->done);
}

static void data_rcu_barrier(void *unused)
{
	rcu_barrier();
}

static void user4k_data_deferred_test(struct kunit *test)
{
	struct data_fixture *fixture = data_fixture_create(test);
	struct deferred_slot *pending = kunit_kzalloc(test, sizeof(*pending), GFP_KERNEL);
	struct mm_subpage *probe;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_NOT_NULL(test, pending);
	KUNIT_ASSERT_EQ(test, supply_backing(fixture->pool), 0);
	for (i = 0; i < DATA_SLOTS; i++)
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc(fixture, i));
	init_completion(&pending->done);
	pending->slot = fixture->held[0];
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, data_rcu_barrier, NULL), 0);
	mm_subpage_get(pending->slot);
	rcu_read_lock();
	call_rcu(&pending->rcu, deferred_slot_put);
	put_held(fixture, 0);
	/* The delayed reference must prevent recycling before the grace period. */
	probe = mm_subpage_alloc(fixture->pool);
	for (i = 1; i < DATA_SLOTS; i++)
		put_held(fixture, i);
	rcu_read_unlock();
	if (!IS_ERR(probe))
		fixture->held[DATA_SLOTS] = probe;
	KUNIT_EXPECT_EQ(test, PTR_ERR(probe), -EAGAIN);
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&pending->done, 5 * HZ), 0UL);
	/* The callback retired the final live slot and the whole native owner. */
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(fixture->pool), 0UL);
}

static void user4k_data_backing_reject_test(struct kunit *test)
{
	struct data_fixture *fixture = data_fixture_create(test);
	struct folio *folio;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	folio = folio_alloc(GFP_KERNEL, 1);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	ret = mm_subpage_pool_add_folio(fixture->pool, folio, GFP_KERNEL);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	if (ret)
		folio_put(folio);
	folio = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	folio_get(folio);
	ret = mm_subpage_pool_add_folio(fixture->pool, folio, GFP_KERNEL);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 2);
	folio_put(folio);
	if (ret)
		folio_put(folio);
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(fixture->pool), 0UL);
}

struct data_worker {
	struct mm_subpage_pool *pool;
	struct task_struct *task;
	struct completion ready, go, done;
	unsigned int id, checks;
	int error;
};

static struct mm_subpage *alloc_with_backing(struct mm_subpage_pool *pool)
{
	unsigned int tries;

	for (tries = 0; tries < 128; tries++) {
		struct mm_subpage *slot = mm_subpage_alloc(pool);
		int ret;

		if (!IS_ERR(slot) || PTR_ERR(slot) != -EAGAIN)
			return slot;
		ret = supply_backing(pool);
		if (ret && ret != -EEXIST)
			return ERR_PTR(ret);
		cond_resched();
	}
	return ERR_PTR(-EAGAIN);
}

static int data_worker_run(void *arg)
{
	struct data_worker *worker = arg;
	struct mm_subpage *slot = NULL;
	unsigned int round;
	u8 pattern = 0;

	for (round = 0; round < 64; round++) {
		slot = alloc_with_backing(worker->pool);
		if (IS_ERR(slot)) {
			worker->error = PTR_ERR(slot);
			slot = NULL;
			break;
		}
		pattern = 1 + round + worker->id * 64;
		memset(slot_address(slot), pattern, SZ_4K);
		mm_subpage_get(slot);
		mm_subpage_put(slot);
		schedule_timeout_uninterruptible(1);
		if (memchr_inv(slot_address(slot), pattern, SZ_4K)) {
			worker->error = -EIO;
			break;
		}
		worker->checks++;
		if (round != 63) {
			mm_subpage_put(slot);
			slot = NULL;
		}
	}
	complete(&worker->ready);
	wait_for_completion(&worker->go);
	if (slot) {
		if (memchr_inv(slot_address(slot), pattern, SZ_4K))
			worker->error = -EIO;
		mm_subpage_put(slot);
	}
	complete(&worker->done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(1);
	return 0;
}

static void data_worker_stop(void *arg)
{
	struct data_worker *worker = arg;

	complete_all(&worker->go);
	kthread_stop(worker->task);
}

static void user4k_data_concurrent_test(struct kunit *test)
{
	struct data_fixture *fixture = data_fixture_create(test);
	struct data_worker *workers = kunit_kcalloc(test, 2, sizeof(*workers), GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_NOT_NULL(test, workers);
	for (i = 0; i < 2; i++) {
		workers[i].pool = fixture->pool;
		workers[i].id = i;
		init_completion(&workers[i].ready);
		init_completion(&workers[i].go);
		init_completion(&workers[i].done);
		workers[i].task = kthread_run(data_worker_run, &workers[i], "user4k-data-%u", i);
		KUNIT_ASSERT_FALSE(test, IS_ERR(workers[i].task));
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, data_worker_stop, &workers[i]), 0);
	}
	for (i = 0; i < 2; i++)
		KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&workers[i].ready, 10 * HZ), 0UL);
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(fixture->pool), 1UL);
	mm_subpage_pool_close(fixture->pool);
	for (i = 0; i < 2; i++)
		complete(&workers[i].go);
	for (i = 0; i < 2; i++) {
		KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&workers[i].done, 10 * HZ), 0UL);
		KUNIT_EXPECT_EQ(test, workers[i].error, 0);
		KUNIT_EXPECT_EQ(test, workers[i].checks, 64U);
	}
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(fixture->pool), 0UL);
}

struct lookup_worker {
	struct task_struct *task;
	struct completion observed;
	phys_addr_t phys;
	unsigned long checks;
	int error;
};

static int lookup_worker_run(void *arg)
{
	struct lookup_worker *worker = arg;
	bool observed = false;

	while (!kthread_should_stop()) {
		struct mm_subpage *slot = mm_subpage_get_from_phys(worker->phys);

		if (slot) {
			unsigned char *data = slot_address(slot);
			unsigned char value = READ_ONCE(*data);

			if (mm_subpage_phys(slot) != worker->phys ||
			    (value != 0x5a && value != 0xab) ||
			    memchr_inv(data, value, SZ_4K))
				worker->error = -EINVAL;
			worker->checks++;
			mm_subpage_put(slot);
			if (!observed) {
				complete(&worker->observed);
				observed = true;
			}
			if (worker->error)
				break;
		}
		cond_resched();
	}
	while (!kthread_should_stop())
		schedule_timeout_interruptible(1);
	return 0;
}

static void lookup_worker_stop(void *arg)
{
	struct lookup_worker *worker = arg;

	if (worker->task) {
		kthread_stop(worker->task);
		worker->task = NULL;
	}
}

static void user4k_data_lookup_race_test(struct kunit *test)
{
	struct data_fixture *fixture = data_fixture_create(test);
	struct lookup_worker *worker = kunit_kzalloc(test, sizeof(*worker), GFP_KERNEL);
	unsigned int copied = 0, i;
	unsigned long deadline;
	struct folio *folio;
	struct mm_subpage *found;

	KUNIT_ASSERT_NOT_NULL(test, fixture);
	KUNIT_ASSERT_NOT_NULL(test, worker);
	KUNIT_ASSERT_EQ(test, supply_backing(fixture->pool), 0);
	for (i = 0; i < DATA_SLOTS; i++)
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc(fixture, i));
	memset(slot_address(fixture->held[0]), 0x5a, SZ_4K);
	memset(slot_address(fixture->held[1]), 0xab, SZ_4K);
	worker->phys = mm_subpage_phys(fixture->held[2]);
	put_held(fixture, 2);
	init_completion(&worker->observed);
	worker->task = kthread_run(lookup_worker_run, worker, "user4k-lookup");
	KUNIT_ASSERT_FALSE(test, IS_ERR(worker->task));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, lookup_worker_stop, worker), 0);
	if (num_online_cpus() > 1) {
		unsigned int cpu = cpumask_next(cpumask_first(cpu_online_mask), cpu_online_mask);

		KUNIT_ASSERT_EQ(test, set_cpus_allowed_ptr(worker->task, cpumask_of(cpu)), 0);
	}
	/* Alternate complete contents; a reader must never see a partial copy. */
	deadline = jiffies + 30 * HZ;
	while (copied < 2048 && time_before(jiffies, deadline)) {
		struct mm_subpage *dest = mm_subpage_copy(fixture->pool,
							fixture->held[copied & 1]);
		bool observed = true;
		phys_addr_t phys;

		if (PTR_ERR_OR_ZERO(dest) == -EAGAIN) {
			/* Let a scheduled-out lookup holder retire its reference. */
			schedule_timeout_uninterruptible(1);
			continue;
		}
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dest);
		phys = mm_subpage_phys(dest);
		if (!copied)
			observed = wait_for_completion_timeout(&worker->observed, 5 * HZ) != 0;
		mm_subpage_put(dest);
		KUNIT_ASSERT_TRUE(test, observed);
		KUNIT_ASSERT_EQ(test, phys, worker->phys);
		copied++;
		cond_resched();
	}
	/* Retire the owner while lookup is still running, then verify unbinding. */
	folio = mm_subpage_folio(fixture->held[0]);
	folio_get(folio);
	mm_subpage_pool_close(fixture->pool);
	for (i = 0; i < DATA_SLOTS; i++)
		if (i != 2)
			put_held(fixture, i);
	lookup_worker_stop(worker);
	found = mm_subpage_get_from_phys(worker->phys);
	KUNIT_EXPECT_PTR_EQ(test, found, NULL);
	if (found)
		mm_subpage_put(found);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 1);
	folio_put(folio);
	KUNIT_EXPECT_EQ(test, worker->error, 0);
	KUNIT_EXPECT_GT(test, worker->checks, 0UL);
	KUNIT_EXPECT_EQ(test, copied, 2048U);
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(fixture->pool), 0UL);
}

static void user4k_data_swapcache_reuse_test(struct kunit *test)
{
	struct data_fixture *f = data_fixture_create(test);
	struct mm_subpage *attempt;
	struct folio *folio;
	unsigned int i;
	bool intact;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, supply_backing(f->pool), 0);
	for (i = 0; i < 4; i++) {
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, i, i * MM_SUBPAGE_SIZE));
		memset(slot_address(f->held[i]), 0x60 + i, MM_SUBPAGE_SIZE);
	}
	folio = mm_subpage_folio(f->held[0]);
	put_held(f, 1);
	/* Synthetic cache membership only; no swap entry or I/O is fabricated. */
	__folio_set_swapbacked(folio);
	folio_set_swapcache(folio);
	attempt = mm_subpage_alloc_at(f->pool, MM_SUBPAGE_SIZE);
	intact = !memchr_inv(folio_address(folio) + MM_SUBPAGE_SIZE, 0x61, MM_SUBPAGE_SIZE);
	folio_clear_swapcache(folio);
	__folio_clear_swapbacked(folio);
	/* Restore the real state before assertions can unwind the fixture. */
	if (!IS_ERR(attempt))
		f->held[1] = attempt;
	KUNIT_EXPECT_TRUE(test, intact);
	KUNIT_ASSERT_EQ(test, PTR_ERR_OR_ZERO(attempt), -EAGAIN);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, 1, MM_SUBPAGE_SIZE));
	for (i = 0; i < 4; i++)
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(f->held[i]),
						    i == 1 ? 0 : 0x60 + i, MM_SUBPAGE_SIZE), NULL);
}

static void restore_folio_free(void *arg)
{
	struct folio *folio = arg;

	/* Data-only fixtures never create a real swap entry or mapping. */
	folio_clear_swapcache(folio);
	__folio_clear_swapbacked(folio);
	folio_put(folio);
}

static struct mm_subpage *restore_test_slot_granule(struct folio *folio, unsigned int offset, unsigned int shift)
{
	struct mm_subpage *slot;

	folio_lock(folio);
	__folio_set_swapbacked(folio);
	folio_set_swapcache(folio);
	slot = mm_subpage_restore_granule(folio, offset, shift, GFP_KERNEL);
	folio_clear_swapcache(folio);
	__folio_clear_swapbacked(folio);
	folio_unlock(folio);
	return slot;
}

static struct mm_subpage *restore_test_slot(struct folio *folio, unsigned int offset)
{
	return restore_test_slot_granule(folio, offset, MM_SUBPAGE_SHIFT);
}

static void test_data_restore_granule(struct kunit *test, unsigned int shift)
{
	struct data_fixture *f = data_fixture_create_granule(test, shift);
	struct folio *folio;
	struct mm_subpage *slot;
	unsigned int i, size = 1U << shift;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, supply_backing(f->pool), 0);
	for (i = 0; i < 4; i++) {
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, i, i * size));
		memset(slot_address(f->held[i]), 0x70 + i, size);
	}
	folio = mm_subpage_folio(f->held[0]);
	folio_get(folio);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, restore_folio_free, folio), 0);
	put_held(f, 1);
	slot = restore_test_slot_granule(folio, size, shift);
	if (!IS_ERR(slot))
		f->held[1] = slot;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, slot);
	KUNIT_EXPECT_EQ(test, mm_subpage_offset(slot), (unsigned int)size);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 5);
	slot = restore_test_slot_granule(folio, 0, shift);
	if (!IS_ERR(slot))
		f->held[4] = slot;
	KUNIT_ASSERT_PTR_EQ(test, slot, f->held[0]);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 6);
	put_held(f, 4);
	/* Retire the entire old owner, retaining only a native backing reference. */
	for (i = 0; i < 4; i++)
		put_held(f, i);
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(f->pool), 0UL);
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 1);
	for (i = 0; i < 4; i++) {
		slot = restore_test_slot_granule(folio, i * size, shift);
		if (!IS_ERR(slot))
			f->held[i] = slot;
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, slot);
		KUNIT_EXPECT_PTR_EQ(test, mm_subpage_folio(slot), folio);
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(slot), 0x70 + i,
						    size), NULL);
	}
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 5);
	KUNIT_EXPECT_EQ(test, PTR_ERR(restore_test_slot_granule(folio, 1, shift)), -EINVAL);
	KUNIT_EXPECT_EQ(test, PTR_ERR(restore_test_slot_granule(folio, PAGE_SIZE, shift)), -EINVAL);
	if (shift != MM_SUBPAGE_SHIFT) {
		KUNIT_EXPECT_EQ(test, PTR_ERR(restore_test_slot(folio, 0)), -EINVAL);
		KUNIT_EXPECT_EQ(test, PTR_ERR(restore_test_slot_granule(folio, SZ_4K, shift)), -EINVAL);
	}

}

static void user4k_data_restore_test(struct kunit *test)
{
	test_data_restore_granule(test, MM_SUBPAGE_SHIFT);
}

static void user16k_data_restore_test(struct kunit *test)
{
	if (PAGE_SIZE <= SZ_16K) {
		kunit_skip(test, "requires native backing larger than 16K");
		return;
	}
	test_data_restore_granule(test, 14);
}

struct restore_worker {
	struct folio *folio;
	struct task_struct *task;
	struct completion done;
	unsigned int checks;
	int error;
};

static int restore_worker_run(void *arg)
{
	struct restore_worker *w = arg;
	unsigned long deadline = jiffies + 20 * HZ;

	while (!kthread_should_stop() && w->checks < 1024 && time_before(jiffies, deadline)) {
		struct mm_subpage *slot;

		folio_lock(w->folio);
		slot = mm_subpage_restore(w->folio, MM_SUBPAGE_SIZE, GFP_KERNEL);
		folio_unlock(w->folio);
		if (PTR_ERR_OR_ZERO(slot) == -EAGAIN) {
			cond_resched();
			continue;
		}
		if (IS_ERR(slot)) {
			w->error = PTR_ERR(slot);
			break;
		}
		if (memchr_inv(slot_address(slot), 0x83, MM_SUBPAGE_SIZE))
			w->error = -EIO;
		/* Retirement deliberately occurs without the folio lock. */
		mm_subpage_put(slot);
		if (w->error)
			break;
		w->checks++;
		cond_resched();
	}
	complete(&w->done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(1);
	return 0;
}

static void restore_worker_stop(void *arg)
{
	struct restore_worker *w = arg;

	if (w->task) {
		kthread_stop(w->task);
		w->task = NULL;
	}
}

static void user4k_data_restore_race_test(struct kunit *test)
{
	struct folio *folio = folio_alloc(GFP_KERNEL, 0);
	struct restore_worker *workers;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, folio);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, restore_folio_free, folio), 0);
	memset(folio_address(folio), 0x83, PAGE_SIZE);
	__folio_mark_uptodate(folio);
	__folio_set_swapbacked(folio);
	folio_set_swapcache(folio);
	workers = kunit_kcalloc(test, 2, sizeof(*workers), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, workers);
	for (i = 0; i < 2; i++) {
		workers[i].folio = folio;
		init_completion(&workers[i].done);
		workers[i].task = kthread_run(restore_worker_run, &workers[i], "user4k-restore-%u", i);
		if (IS_ERR(workers[i].task)) {
			workers[i].task = NULL;
			KUNIT_FAIL(test, "restore worker creation failed");
			return;
		}
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, restore_worker_stop, &workers[i]), 0);
	}
	for (i = 0; i < 2; i++) {
		KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&workers[i].done, 25 * HZ), 0UL);
		restore_worker_stop(&workers[i]);
		KUNIT_EXPECT_EQ(test, workers[i].error, 0);
		KUNIT_EXPECT_EQ(test, workers[i].checks, 1024U);
	}
	KUNIT_EXPECT_EQ(test, folio_ref_count(folio), 1);
}

static void test_data_migration_granule(struct kunit *test, unsigned int shift)
{
	unsigned int mode, size = 1U << shift;

	/* Success, rollback, all PTEs discarded, and partial-folio success. */
	for (mode = 0; mode < 4; mode++) {
		struct data_fixture *f = data_fixture_create_granule(test, shift);
		struct folio *src, *dst, *target;
		unsigned int i, nr = mode == 3 ? 3 : 4;
		int ret, src_refs = 0, dst_refs = 0, blocked = 0;
		bool unpublished = true, active = false;

		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_ASSERT_EQ(test, supply_backing(f->pool), 0);
		for (i = 0; i < nr; i++) {
			unsigned int index = mode == 3 && i ? i + 1 : i;

			KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, i, index * size));
			memset(slot_address(f->held[i]), 0x80 + index, size);
		}
		src = mm_subpage_folio(f->held[0]);
		folio_get(src);
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, restore_folio_free, src), 0);
		dst = folio_alloc(GFP_KERNEL, 0);
		KUNIT_ASSERT_NOT_NULL(test, dst);
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, restore_folio_free, dst), 0);
		folio_lock(src);
		folio_lock(dst);
		ret = mm_subpage_migrate_prepare(src, dst);
		if (!ret) {
			struct mm_subpage *probe;

			src_refs = folio_ref_count(src);
			dst_refs = folio_ref_count(dst);
			active = mm_subpage_migrating(src) && mm_subpage_migrating(dst);
			probe = mm_subpage_alloc_at(f->pool, size);
			blocked = PTR_ERR_OR_ZERO(probe);
			if (!IS_ERR(probe))
				mm_subpage_put(probe);
			/* Emulate migration entries: their old mapping refs are gone. */
			for (i = 0; i < nr; i++)
				put_held(f, i);
			probe = mm_subpage_get_from_phys(PFN_PHYS(folio_pfn(src)));
			unpublished &= !probe;
			if (probe)
				mm_subpage_put(probe);
			probe = mm_subpage_get_from_phys(PFN_PHYS(folio_pfn(dst)));
			unpublished &= !probe;
			if (probe)
				mm_subpage_put(probe);
			if (mode != 1 && mode != 2) {
				copy_user_highpage(&dst->page, &src->page, 0, NULL);
				__folio_mark_uptodate(dst);
			}
			target = mode == 1 ? src : dst;
			if (mode != 2)
				for (i = 0; i < nr; i++) {
					unsigned int index = mode == 3 && i ? i + 1 : i;

					f->held[i] = mm_subpage_migrate_restore(target, index * size);
				}
			mm_subpage_migrate_finish(src);
			mm_subpage_migrate_finish(dst);
		}
		folio_unlock(dst);
		folio_unlock(src);
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_EXPECT_TRUE(test, active);
		KUNIT_EXPECT_TRUE(test, unpublished);
		KUNIT_EXPECT_EQ(test, src_refs, (int)nr + 1);
		KUNIT_EXPECT_EQ(test, dst_refs, 1);
		KUNIT_EXPECT_EQ(test, blocked, -EAGAIN);
		KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(f->pool), mode == 2 ? 0UL : 1UL);
		KUNIT_EXPECT_EQ(test, folio_ref_count(src), mode == 1 ? (int)nr + 1 : 1);
		KUNIT_EXPECT_EQ(test, folio_ref_count(dst), mode == 0 || mode == 3 ? (int)nr + 1 : 1);
		if (mode != 2)
			for (i = 0; i < nr; i++) {
				unsigned int index = mode == 3 && i ? i + 1 : i;

				KUNIT_EXPECT_PTR_EQ(test, mm_subpage_folio(f->held[i]), mode == 1 ? src : dst);
				KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(f->held[i]),
								    0x80 + index, size), NULL);
			}
		if (mode == 3) {
			KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, 3, size));
			KUNIT_EXPECT_PTR_EQ(test, mm_subpage_folio(f->held[3]), dst);
			KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(f->held[3]), 0,
							    size), NULL);
		}
	}
}

static void user4k_data_migration_owner_test(struct kunit *test)
{
	test_data_migration_granule(test, MM_SUBPAGE_SHIFT);
}

static void user16k_data_migration_owner_test(struct kunit *test)
{
	if (PAGE_SIZE <= SZ_16K) {
		kunit_skip(test, "requires native backing larger than 16K");
		return;
	}
	test_data_migration_granule(test, 14);
}

#ifdef CONFIG_ARM64_MTE
static void test_slot_tags_set(void *addr, unsigned int tag)
{
	unsigned long value = (unsigned long)tag << MTE_TAG_SHIFT;
	unsigned int offset;

	for (offset = 0; offset < MM_SUBPAGE_SIZE; offset += MTE_GRANULE_SIZE)
		asm volatile(".arch_extension memtag\n\tstg %0, [%1]"
			     : : "r" (value), "r" (addr + offset) : "memory");
}

static bool test_slot_tags_equal(void *addr, unsigned int expected)
{
	unsigned int offset;

	for (offset = 0; offset < MM_SUBPAGE_SIZE; offset += MTE_GRANULE_SIZE) {
		unsigned long value = 0;

		asm volatile(".arch_extension memtag\n\tldg %0, [%1]"
			     : "+r" (value) : "r" (addr + offset) : "memory");
		if (((value >> MTE_TAG_SHIFT) & 0xf) != expected)
			return false;
	}
	return true;
}
#endif

static void user4k_data_mte_test(struct kunit *test)
{
#ifdef CONFIG_ARM64_MTE
	struct data_fixture *f;
	struct folio *folio;
	unsigned int i;

	if (!system_supports_mte()) {
		kunit_skip(test, "MTE is not available on this CPU");
		return;
	}
	f = data_fixture_create(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, supply_backing(f->pool), 0);
	for (i = 0; i < 4; i++)
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, i, i * MM_SUBPAGE_SIZE));
	folio = mm_subpage_folio(f->held[0]);
	KUNIT_ASSERT_TRUE(test, try_page_mte_tagging(&folio->page));
	mte_clear_page_tags(folio_address(folio));
	for (i = 0; i < 4; i++) {
		memset(slot_address(f->held[i]), 0x40 + i, MM_SUBPAGE_SIZE);
		test_slot_tags_set(slot_address(f->held[i]), i + 1);
	}
	set_page_mte_tagged(&folio->page);
	put_held(f, 1);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, 1, MM_SUBPAGE_SIZE));
	for (i = 0; i < 4; i++) {
		KUNIT_EXPECT_TRUE(test, test_slot_tags_equal(slot_address(f->held[i]), i == 1 ? 0 : i + 1));
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(f->held[i]),
						    i == 1 ? 0 : 0x40 + i, MM_SUBPAGE_SIZE), NULL);
	}
	put_held(f, 1);
	f->held[1] = mm_subpage_copy_at(f->pool, f->held[0], MM_SUBPAGE_SIZE);
	if (IS_ERR(f->held[1])) {
		f->held[1] = NULL;
		KUNIT_FAIL(test, "tagged slot copy failed");
		return;
	}
	for (i = 0; i < 4; i++) {
		KUNIT_EXPECT_TRUE(test, test_slot_tags_equal(slot_address(f->held[i]), i == 1 ? 1 : i + 1));
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(f->held[i]),
						    i == 1 ? 0x40 : 0x40 + i, MM_SUBPAGE_SIZE), NULL);
	}
	/* A fresh destination must initialise tags outside the copied slot too. */
	KUNIT_ASSERT_EQ(test, supply_backing_at(f->pool, 3 * MM_SUBPAGE_SIZE), 0);
	f->held[4] = mm_subpage_copy_at(f->pool, f->held[2], 3 * MM_SUBPAGE_SIZE);
	if (IS_ERR(f->held[4])) {
		f->held[4] = NULL;
		KUNIT_FAIL(test, "fresh tagged slot copy failed");
		return;
	}
	folio = mm_subpage_folio(f->held[4]);
	KUNIT_EXPECT_TRUE(test, page_mte_tagged(&folio->page));
	for (i = 0; i < 4; i++)
		KUNIT_EXPECT_TRUE(test, test_slot_tags_equal(folio_address(folio) +
						      i * MM_SUBPAGE_SIZE, i == 3 ? 3 : 0));
#else
	kunit_skip(test, "MTE is not configured");
#endif
}

static void extent_folio_put(void *arg)
{
	folio_put(arg);
}

static void user4k_data_copy_extent_test(struct kunit *test)
{
	const unsigned int sizes[] = { SZ_4K, SZ_16K, SZ_64K };

	for (unsigned int n = 0; n < ARRAY_SIZE(sizes); n++) {
		struct folio *source, *dest;
		unsigned int size = sizes[n], to_offset;
		void *from, *to;

		if (size > PAGE_SIZE)
			continue;
		source = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
		KUNIT_ASSERT_NOT_NULL(test, source);
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, extent_folio_put, source), 0);
		dest = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
		KUNIT_ASSERT_NOT_NULL(test, dest);
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, extent_folio_put, dest), 0);
		from = folio_address(source);
		to = folio_address(dest);
		to_offset = PAGE_SIZE - size;
		for (unsigned int i = 0; i < DATA_SLOTS; i++)
			memset(from + i * SZ_4K, 0x40 + i, SZ_4K);
		for (unsigned int tagged = 0; tagged < 2; tagged++) {
			memset(to, 0xac, PAGE_SIZE);
#ifdef CONFIG_ARM64_MTE
			if (system_supports_mte()) {
				if (try_page_mte_tagging(&dest->page))
					mte_clear_page_tags(to);
				for (unsigned int i = 0; i < DATA_SLOTS; i++)
					test_slot_tags_set(to + i * SZ_4K, 9);
				set_page_mte_tagged(&dest->page);
				if (tagged) {
					if (try_page_mte_tagging(&source->page))
						mte_clear_page_tags(from);
					for (unsigned int i = 0; i < DATA_SLOTS; i++)
						test_slot_tags_set(from + i * SZ_4K, 1 + i % 15);
					set_page_mte_tagged(&source->page);
				}
			}
#endif
			KUNIT_ASSERT_EQ(test, copy_user_subpage_range(&dest->page, to_offset,
					&source->page, 0, size), 0);
			for (unsigned int i = 0; i < DATA_SLOTS; i++) {
				unsigned int offset = i * SZ_4K;
				bool copied = offset >= to_offset;
				unsigned int source_index = copied ? (offset - to_offset) / SZ_4K : 0;

				KUNIT_EXPECT_PTR_EQ(test, memchr_inv(to + offset,
					copied ? 0x40 + source_index : 0xac, SZ_4K), NULL);
				KUNIT_EXPECT_PTR_EQ(test, memchr_inv(from + offset, 0x40 + i, SZ_4K), NULL);
#ifdef CONFIG_ARM64_MTE
				if (system_supports_mte())
					KUNIT_EXPECT_TRUE(test, test_slot_tags_equal(to + offset,
						copied ? (tagged ? 1 + source_index % 15 : 0) : 9));
#endif
			}
			clear_user_subpage_range(&dest->page, to_offset, size);
			for (unsigned int i = 0; i < DATA_SLOTS; i++) {
				unsigned int offset = i * SZ_4K;
				bool cleared = offset >= to_offset;

				KUNIT_EXPECT_PTR_EQ(test, memchr_inv(to + offset, cleared ? 0 : 0xac,
								    SZ_4K), NULL);
#ifdef CONFIG_ARM64_MTE
				if (system_supports_mte())
					KUNIT_EXPECT_TRUE(test, test_slot_tags_equal(to + offset, cleared ? 0 : 9));
#endif
			}
		}
	}
}

#ifdef CONFIG_NET
struct skb_test_state {
	struct folio *folio;
#ifdef CONFIG_INET
	struct socket *socket;
#endif
	struct sk_buff *skbs[5];
};

static void skb_test_cleanup(void *arg)
{
	struct skb_test_state *state = arg;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(state->skbs); i++) {
		if (state->skbs[i])
			state->skbs[i]->sk = NULL;
		kfree_skb(state->skbs[i]);
	}
#ifdef CONFIG_INET
	if (state->socket)
		sock_release(state->socket);
#endif
	/* This fixture marks ownership only; it installs no anonymous rmap. */
	if (state->folio)
		state->folio->mapping = NULL;
}

static void user4k_data_skb_test(struct kunit *test)
{
	struct data_fixture *f = data_fixture_create(test);
	struct skb_test_state *state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	struct mm_subpage *attempt;
	struct iov_iter iter;
	struct bio_vec bvec;
	unsigned char *bytes = kunit_kmalloc(test, SZ_4K, GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_NOT_NULL(test, bytes);
	KUNIT_ASSERT_EQ(test, supply_backing(f->pool), 0);
	for (i = 0; i < 4; i++) {
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, i, i * SZ_4K));
		memset(slot_address(f->held[i]), 0x51 + i, SZ_4K);
	}
	state->folio = mm_subpage_folio(f->held[0]);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, skb_test_cleanup, state), 0);
	state->folio->mapping = (void *)FOLIO_MAPPING_ANON;
	state->skbs[0] = alloc_skb(0, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, state->skbs[0]);
	state->skbs[0]->ip_summed = CHECKSUM_UNNECESSARY;
	bvec_set_page(&bvec, &state->folio->page, SZ_4K, SZ_4K);
	iov_iter_bvec(&iter, ITER_SOURCE, &bvec, 1, SZ_4K);
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(state->skbs[0], &iter, SZ_4K), (ssize_t)SZ_4K);
	KUNIT_ASSERT_NOT_NULL(test, skb_zcopy(state->skbs[0]));
	state->skbs[1] = pskb_copy(state->skbs[0], GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, state->skbs[1]);
	/* Append a distinct quarter while the callback object has two owners. */
	bvec_set_page(&bvec, &state->folio->page, SZ_4K, 2 * SZ_4K);
	iov_iter_bvec(&iter, ITER_SOURCE, &bvec, 1, SZ_4K);
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(state->skbs[0], &iter, SZ_4K), (ssize_t)SZ_4K);
	KUNIT_EXPECT_PTR_NE(test, skb_zcopy(state->skbs[0]), skb_zcopy(state->skbs[1]));
	state->skbs[2] = skb_clone(state->skbs[1], GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, state->skbs[2]);
	state->skbs[3] = alloc_skb(0, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, state->skbs[3]);
	skb_split(state->skbs[0], state->skbs[3], SZ_2K);
	state->skbs[4] = skb_copy(state->skbs[3], GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, state->skbs[4]);
	put_held(f, 1);
	put_held(f, 2);
	attempt = mm_subpage_alloc_at(f->pool, SZ_4K);
	if (!IS_ERR(attempt))
		f->held[1] = attempt;
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(attempt), -EAGAIN);
	for (i = 0; i < 2; i++) {
		kfree_skb(state->skbs[i]);
		state->skbs[i] = NULL;
	}
	KUNIT_ASSERT_EQ(test, skb_copy_bits(state->skbs[2], 0, bytes, SZ_4K), 0);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(bytes, 0x52, SZ_4K), NULL);
	KUNIT_ASSERT_EQ(test, skb_copy_bits(state->skbs[3], SZ_2K, bytes, SZ_4K), 0);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(bytes, 0x53, SZ_4K), NULL);
	for (i = 2; i < 4; i++) {
		kfree_skb(state->skbs[i]);
		state->skbs[i] = NULL;
	}
	/* A full data copy must not keep ownership of the original quarters. */
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, 1, SZ_4K));
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, 2, 2 * SZ_4K));
	memset(slot_address(f->held[1]), 0xe1, SZ_4K);
	memset(slot_address(f->held[2]), 0xe2, SZ_4K);
	KUNIT_ASSERT_EQ(test, skb_copy_bits(state->skbs[4], SZ_2K, bytes, SZ_4K), 0);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(bytes, 0x53, SZ_4K), NULL);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 4);
}
#ifdef CONFIG_INET
static void test_data_skb_prune_granule(struct kunit *test, unsigned int shift)
{
	struct data_fixture *f = data_fixture_create_granule(test, shift);
	struct skb_test_state *state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	struct sk_buff *skb;
	struct bio_vec bvec;
	struct iov_iter iter;
	unsigned int i, size = 1U << shift;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_EQ(test, supply_backing(f->pool), 0);
	for (i = 0; i < 4; i++)
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, i, i * size));
	state->folio = mm_subpage_folio(f->held[0]);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, skb_test_cleanup, state), 0);
	KUNIT_ASSERT_EQ(test, sock_create_kern(&init_net, AF_INET, SOCK_STREAM, 0, &state->socket), 0);
	state->folio->mapping = (void *)FOLIO_MAPPING_ANON;
	state->skbs[0] = alloc_skb(0, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, state->skbs[0]);
	skb = state->skbs[0];
	/* A live socket association prevents trim from condensing data into the head. */
	skb->sk = state->socket->sk;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	bvec_set_page(&bvec, &state->folio->page, 16, size - SZ_4K + 16);
	iov_iter_bvec(&iter, ITER_SOURCE, &bvec, 1, 16);
	KUNIT_ASSERT_EQ(test, skb_splice_from_iter(skb, &iter, 16), (ssize_t)16);
	for (i = 0; i < 1024; i++) {
		bvec_set_page(&bvec, &state->folio->page, size, (1 + i % 3) * size);
		iov_iter_bvec(&iter, ITER_SOURCE, &bvec, 1, size);
		KUNIT_ASSERT_EQ(test, skb_splice_from_iter(skb, &iter, size), (ssize_t)size);
		/* Four fixture refs, two frag refs, two current quarter refs. */
		KUNIT_ASSERT_EQ(test, folio_ref_count(state->folio), 8);
		KUNIT_ASSERT_EQ(test, pskb_trim(skb, 16), 0);
		KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 7);
	}
	skb->sk = NULL;
	kfree_skb(skb);
	state->skbs[0] = NULL;
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 4);
}

static void user4k_data_skb_prune_test(struct kunit *test)
{
	test_data_skb_prune_granule(test, MM_SUBPAGE_SHIFT);
}

static void user16k_data_skb_prune_test(struct kunit *test)
{
	if (PAGE_SIZE <= SZ_16K) {
		kunit_skip(test, "requires native backing larger than 16K");
		return;
	}
	test_data_skb_prune_granule(test, 14);
}

#endif

static void user4k_data_skb_checksum_test(struct kunit *test)
{
	struct data_fixture *f = data_fixture_create(test);
	struct skb_test_state *state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	struct bio_vec bvec[2];
	struct iov_iter iter;
	unsigned char *bytes;
	unsigned int i, owned;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_EQ(test, supply_backing(f->pool), 0);
	for (i = 0; i < 4; i++)
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, i, i * SZ_4K));
	state->folio = mm_subpage_folio(f->held[0]);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, skb_test_cleanup, state), 0);
	bytes = folio_address(state->folio);
	for (i = 0; i < PAGE_SIZE; i++)
		bytes[i] = i * 13 + (i >> 12) * 31;
	for (owned = 0; owned < 2; owned++) {
		struct sk_buff *skb;

		state->folio->mapping = owned ? (void *)FOLIO_MAPPING_ANON : NULL;
		state->skbs[0] = alloc_skb(0, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, state->skbs[0]);
		skb = state->skbs[0];
		skb->ip_summed = CHECKSUM_NONE;
		bvec_set_page(&bvec[0], &state->folio->page, SZ_4K - 13, 13);
		bvec_set_page(&bvec[1], &state->folio->page, SZ_4K, SZ_4K);
		iov_iter_bvec(&iter, ITER_SOURCE, bvec, 2, SZ_8K - 13);
		KUNIT_ASSERT_EQ(test, skb_splice_from_iter(skb, &iter, SZ_8K - 13),
				(ssize_t)(SZ_8K - 13));
		KUNIT_EXPECT_EQ(test, (__force u16)csum_fold(skb->csum),
				(__force u16)csum_fold(skb_checksum(skb, 0, skb->len, 0)));
		kfree_skb(skb);
		state->skbs[0] = NULL;
		KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 4);
	}
}

#endif

struct refs_test_state {
	struct folio *folio;
	struct mm_subpage_refs *refs[3];
};

static void refs_test_cleanup(void *arg)
{
	struct refs_test_state *state = arg;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(state->refs); i++)
		mm_subpage_refs_put(state->refs[i]);
	if (state->folio)
		state->folio->mapping = NULL;
}

static bool refs_test_keep(const struct mm_subpage *slot, void *context)
{
	unsigned long mask = *(unsigned long *)context;

	return mask & BIT(mm_subpage_offset(slot) >> MM_SUBPAGE_SHIFT);
}

static void user4k_data_refset_test(struct kunit *test)
{
	struct data_fixture *f = data_fixture_create(test);
	struct refs_test_state *state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	struct mm_subpage *attempt;
	struct page *page;
	unsigned long mask;
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_EQ(test, supply_backing(f->pool), 0);
	for (i = 0; i < DATA_SLOTS; i++)
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, i, i * SZ_4K));
	state->folio = mm_subpage_folio(f->held[0]);
	page = &state->folio->page;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, refs_test_cleanup, state), 0);
	/* Ownership fixture only; no anonymous rmap is installed. */
	state->folio->mapping = (void *)FOLIO_MAPPING_ANON;
	KUNIT_ASSERT_EQ(test, mm_subpage_refs_add(&state->refs[0], page,
			SZ_4K + 17, SZ_8K - 34, GFP_KERNEL), 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)DATA_SLOTS + 2);
	KUNIT_ASSERT_EQ(test, mm_subpage_refs_add(&state->refs[0], page, SZ_4K, SZ_4K, GFP_KERNEL), 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)DATA_SLOTS + 2);
	state->refs[1] = mm_subpage_refs_get(state->refs[0]);
	KUNIT_ASSERT_EQ(test, mm_subpage_refs_add(&state->refs[1], page, 3 * SZ_4K, SZ_4K, GFP_KERNEL), 0);
	KUNIT_EXPECT_PTR_NE(test, state->refs[0], state->refs[1]);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)DATA_SLOTS + 5);
	mask = BIT(2);
	KUNIT_ASSERT_EQ(test, mm_subpage_refs_prune(&state->refs[1], refs_test_keep, &mask, GFP_KERNEL), 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)DATA_SLOTS + 3);
	state->refs[2] = mm_subpage_refs_get(state->refs[0]);
	mask = BIT(1);
	KUNIT_ASSERT_EQ(test, mm_subpage_refs_prune(&state->refs[2], refs_test_keep, &mask, GFP_KERNEL), 0);
	KUNIT_EXPECT_PTR_NE(test, state->refs[0], state->refs[2]);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)DATA_SLOTS + 4);
	KUNIT_EXPECT_EQ(test, mm_subpage_refs_add(&state->refs[0], page, PAGE_SIZE, 1, GFP_KERNEL), -EINVAL);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)DATA_SLOTS + 4);
	mask = 0;
	for (i = 1; i < 3; i++) {
		KUNIT_ASSERT_EQ(test, mm_subpage_refs_prune(&state->refs[i], refs_test_keep, &mask, GFP_KERNEL), 0);
		KUNIT_EXPECT_PTR_EQ(test, state->refs[i], NULL);
	}
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)DATA_SLOTS + 2);
	put_held(f, 1);
	attempt = mm_subpage_alloc_at(f->pool, SZ_4K);
	if (!IS_ERR(attempt))
		f->held[1] = attempt;
	KUNIT_ASSERT_EQ(test, PTR_ERR_OR_ZERO(attempt), -EAGAIN);
	mm_subpage_refs_put(state->refs[0]);
	state->refs[0] = NULL;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, 1, SZ_4K));
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)DATA_SLOTS);
	for (i = 0; i < 1024; i++) {
		KUNIT_ASSERT_EQ(test, mm_subpage_refs_add(&state->refs[0], page, 0, PAGE_SIZE, GFP_KERNEL), 0);
		KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 2 * (int)DATA_SLOTS);
		mask = BIT(i % DATA_SLOTS);
		KUNIT_ASSERT_EQ(test, mm_subpage_refs_prune(&state->refs[0], refs_test_keep, &mask, GFP_KERNEL), 0);
		KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)DATA_SLOTS + 1);
	}
	mm_subpage_refs_put(state->refs[0]);
	state->refs[0] = NULL;
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)DATA_SLOTS);
}

#if IS_ENABLED(CONFIG_NET_SOCK_MSG) && IS_ENABLED(CONFIG_INET)
struct skmsg_test_state {
	struct folio *folio;
	struct socket *socket;
	struct sk_msg messages[2];
	struct sk_msg borrowed;
};

static void skmsg_test_cleanup(void *arg)
{
	struct skmsg_test_state *state = arg;
	unsigned int i;

	if (state->socket) {
		for (i = 0; i < ARRAY_SIZE(state->messages); i++)
			sk_msg_free(state->socket->sk, &state->messages[i]);
		sk_msg_subpages_release(&state->borrowed);
		sock_release(state->socket);
	}
	if (state->folio)
		state->folio->mapping = NULL;
}

static void test_data_skmsg_granule(struct kunit *test, unsigned int shift)
{
	struct data_fixture *f = data_fixture_create_granule(test, shift);
	struct skmsg_test_state *state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	struct mm_subpage *attempt;
	struct sk_msg *src, *dst;
	struct page *page;
	struct sock *sk;
	struct bio_vec bvec;
	struct iov_iter iter;
	unsigned int i, size = 1U << shift;

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_EQ(test, supply_backing(f->pool), 0);
	for (i = 0; i < 4; i++) {
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, i, i * size));
		memset(slot_address(f->held[i]), 0x60 + i, size);
	}
	state->folio = mm_subpage_folio(f->held[0]);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, skmsg_test_cleanup, state), 0);
	KUNIT_ASSERT_EQ(test, sock_create_kern(&init_net, AF_INET, SOCK_STREAM, 0, &state->socket), 0);
	state->folio->mapping = (void *)FOLIO_MAPPING_ANON;
	page = &state->folio->page;
	sk = state->socket->sk;
	src = &state->messages[0];
	dst = &state->messages[1];
	sk_msg_init(src);
	sk_msg_init(dst);
	KUNIT_ASSERT_EQ(test, sk_msg_page_add(src, page, (size / 2) - 13, size + (size / 2) + 13), 0);
	sk_mem_charge(sk, (size / 2) - 13);
	KUNIT_ASSERT_EQ(test, sk_msg_page_add(src, page, size, 2 * size), 0);
	sk_mem_charge(sk, size);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 8);
	KUNIT_ASSERT_EQ(test, sk_msg_clone(sk, dst, src, 0, size + (size / 2) - 13), 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 11);
	sk_msg_free_partial(sk, src, (size / 2) - 13);
	KUNIT_ASSERT_EQ(test, sk_msg_page_add(src, page, size, 0), 0);
	sk_mem_charge(sk, size);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 11);
	sk_msg_free(sk, dst);
	put_held(f, 1);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, 1, size));
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 8);
	sk_msg_free_partial(sk, src, size);
	for (i = 0; i < 1024; i++) {
		unsigned int index = (i + 1) & 3;

		KUNIT_ASSERT_EQ(test, sk_msg_page_add(src, page, size, index * size), 0);
		sk_mem_charge(sk, size);
		KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 8);
		sk_msg_free_partial(sk, src, size);
		KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 7);
	}
	sk_msg_free(sk, src);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 4);
	/* A borrowed descriptor copy has an independent ownership lifetime. */
	KUNIT_ASSERT_EQ(test, sk_msg_page_add(src, page, size, 0), 0);
	sk_mem_charge(sk, size);
	memcpy(&state->borrowed, src, sizeof(*src));
	sk_msg_subpages_get(&state->borrowed);
	sk_msg_free(sk, src);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 5);
	put_held(f, 0);
	attempt = mm_subpage_alloc_at(f->pool, 0);
	if (!IS_ERR(attempt))
		f->held[0] = attempt;
	KUNIT_ASSERT_EQ(test, PTR_ERR_OR_ZERO(attempt), -EAGAIN);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(sg_virt(&state->borrowed.sg.data[0]), 0x60, size), NULL);
	sk_msg_subpages_release(&state->borrowed);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, 0, 0));
	/* Full transfer moves the set, and zero-copy iterator trim releases its references. */
	KUNIT_ASSERT_EQ(test, sk_msg_page_add(src, page, size, 3 * size), 0);
	sk_mem_charge(sk, size);
	sk_msg_xfer_full(dst, src);
	KUNIT_EXPECT_EQ(test, src->sg.size, 0U);
	sk_msg_free(sk, dst);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 4);
	bvec_set_page(&bvec, page, size - 17, 3 * size + 17);
	iov_iter_bvec(&iter, ITER_SOURCE, &bvec, 1, size - 17);
	KUNIT_ASSERT_EQ(test, sk_msg_zerocopy_from_iter(sk, &iter, src, size - 17), 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 6);
	sk_msg_trim(sk, src, 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), 4);
}

static void user4k_data_skmsg_test(struct kunit *test)
{
	test_data_skmsg_granule(test, MM_SUBPAGE_SHIFT);
}

static void user16k_data_skmsg_test(struct kunit *test)
{
	if (PAGE_SIZE <= SZ_16K) {
		kunit_skip(test, "requires native backing larger than 16K");
		return;
	}
	test_data_skmsg_granule(test, 14);
}
#endif

static void user16k_data_owner_test(struct kunit *test)
{
	struct data_fixture *f, *dest, *small;
	struct mm_subpage *slot, *found;
	unsigned int count = PAGE_SIZE / SZ_16K;

	if (PAGE_SIZE <= SZ_16K) {
		kunit_skip(test, "requires native backing larger than 16K");
		return;
	}
	f = data_fixture_create_granule(test, 14);
	dest = data_fixture_create_granule(test, 14);
	small = data_fixture_create(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, dest);
	KUNIT_ASSERT_NOT_NULL(test, small);
	KUNIT_ASSERT_EQ(test, supply_backing(f->pool), 0);
	KUNIT_ASSERT_EQ(test, supply_backing(dest->pool), 0);
	KUNIT_ASSERT_EQ(test, supply_backing(small->pool), 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc(small, 0));
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_alloc_at(f->pool, SZ_4K)), -EINVAL);
	for (unsigned int i = 0; i < count; i++) {
		slot = hold_alloc_at(f, i, i * SZ_16K);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, slot);
		KUNIT_EXPECT_EQ(test, mm_subpage_shift(slot), 14U);
		KUNIT_EXPECT_EQ(test, mm_subpage_size(slot), (unsigned int)SZ_16K);
		KUNIT_EXPECT_EQ(test, mm_subpage_offset(slot), i * (unsigned int)SZ_16K);
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(slot), 0, SZ_16K), NULL);
		memset(slot_address(slot), 0x30 + i, SZ_16K);
		for (unsigned int quarter = 0; quarter < 4; quarter++) {
			found = mm_subpage_get_from_phys(mm_subpage_phys(slot) + quarter * SZ_4K);
			KUNIT_ASSERT_PTR_EQ(test, found, slot);
			mm_subpage_put(found);
		}
	}
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_alloc(f->pool)), -EAGAIN);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_copy(dest->pool, small->held[0])), -EINVAL);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_copy(small->pool, f->held[0])), -EINVAL);
	slot = mm_subpage_copy_at(dest->pool, f->held[count - 1], SZ_16K);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, slot);
	dest->held[0] = slot;
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(slot), 0x30 + count - 1, SZ_16K), NULL);
	/* An interior-byte lookup holds the whole identity across release/reuse. */
	f->held[count] = mm_subpage_get_from_phys(mm_subpage_phys(f->held[1]) + 3 * SZ_4K);
	KUNIT_ASSERT_PTR_EQ(test, f->held[count], f->held[1]);
	put_held(f, 1);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_alloc_at(f->pool, SZ_16K)), -EAGAIN);
	put_held(f, count);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, 1, SZ_16K));
	for (unsigned int i = 0; i < count; i++)
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(slot_address(f->held[i]),
						    i == 1 ? 0 : 0x30 + i, SZ_16K), NULL);
	mm_subpage_pool_close(f->pool);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_alloc(f->pool)), -ESHUTDOWN);
	for (unsigned int i = 0; i < count; i++)
		put_held(f, i);
	KUNIT_EXPECT_EQ(test, mm_subpage_pool_backing_pages(f->pool), 0UL);
}

static void user16k_data_refset_test(struct kunit *test)
{
	struct data_fixture *f;
	struct refs_test_state *state;
	unsigned int count = PAGE_SIZE / SZ_16K;
	unsigned long mask;
	struct page *page;

	if (PAGE_SIZE <= SZ_16K) {
		kunit_skip(test, "requires native backing larger than 16K");
		return;
	}
	f = data_fixture_create_granule(test, 14);
	state = kunit_kzalloc(test, sizeof(*state), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_NOT_NULL(test, state);
	KUNIT_ASSERT_EQ(test, supply_backing(f->pool), 0);
	for (unsigned int i = 0; i < count; i++)
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, i, i * SZ_16K));
	state->folio = mm_subpage_folio(f->held[0]);
	page = &state->folio->page;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, refs_test_cleanup, state), 0);
	/* Metadata-only ownership fixture: no VMA or rmap is published. */
	state->folio->mapping = (void *)FOLIO_MAPPING_ANON;
	KUNIT_ASSERT_EQ(test, mm_subpage_refs_add(&state->refs[0], page,
			SZ_16K - 1024, 2048, GFP_KERNEL), 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)count + 2);
	KUNIT_ASSERT_EQ(test, mm_subpage_refs_add(&state->refs[0], page, 0, PAGE_SIZE, GFP_KERNEL), 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)count * 2);
	KUNIT_ASSERT_EQ(test, mm_subpage_refs_add(&state->refs[0], page, 0, PAGE_SIZE, GFP_KERNEL), 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)count * 2);
	state->refs[1] = mm_subpage_refs_get(state->refs[0]);
	mask = BIT(SZ_16K >> MM_SUBPAGE_SHIFT);
	KUNIT_ASSERT_EQ(test, mm_subpage_refs_prune(&state->refs[1], refs_test_keep, &mask, GFP_KERNEL), 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)count * 2 + 1);
	mask = 0;
	KUNIT_ASSERT_EQ(test, mm_subpage_refs_prune(&state->refs[0], refs_test_keep, &mask, GFP_KERNEL), 0);
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)count + 1);
	put_held(f, 1);
	KUNIT_EXPECT_EQ(test, PTR_ERR(mm_subpage_alloc_at(f->pool, SZ_16K)), -EAGAIN);
	KUNIT_ASSERT_EQ(test, mm_subpage_refs_prune(&state->refs[1], refs_test_keep, &mask, GFP_KERNEL), 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, 1, SZ_16K));
	KUNIT_EXPECT_EQ(test, folio_ref_count(state->folio), (int)count);
}

static void user16k_data_mte_test(struct kunit *test)
{
#ifdef CONFIG_ARM64_MTE
	struct data_fixture *f;
	struct folio *folio;
	struct mm_subpage *copy;
	unsigned int count = PAGE_SIZE / SZ_16K;
	int ret;

	if (PAGE_SIZE <= SZ_16K || !system_supports_mte()) {
		kunit_skip(test, "requires MTE and native backing larger than 16K");
		return;
	}
	f = data_fixture_create_granule(test, 14);
	KUNIT_ASSERT_NOT_NULL(test, f);
	folio = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	if (try_page_mte_tagging(&folio->page))
		mte_clear_page_tags(folio_address(folio));
	for (unsigned int i = 0; i < DATA_SLOTS; i++)
		test_slot_tags_set(folio_address(folio) + i * SZ_4K, 9);
	set_page_mte_tagged(&folio->page);
	ret = mm_subpage_pool_add_folio(f->pool, folio, GFP_KERNEL);
	if (ret)
		folio_put(folio);
	KUNIT_ASSERT_EQ(test, ret, 0);
	for (unsigned int i = 0; i < count; i++)
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, i, i * SZ_16K));
	for (unsigned int i = 0; i < DATA_SLOTS; i++) {
		void *address = folio_address(folio) + i * SZ_4K;

		KUNIT_EXPECT_TRUE(test, test_slot_tags_equal(address, 0));
		memset(address, 0x50 + i, SZ_4K);
		test_slot_tags_set(address, 1 + i % 15);
	}
	put_held(f, 1);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, hold_alloc_at(f, 1, SZ_16K));
	for (unsigned int i = 0; i < DATA_SLOTS; i++) {
		bool cleared = i >= 4 && i < 8;
		void *address = folio_address(folio) + i * SZ_4K;

		KUNIT_EXPECT_TRUE(test, test_slot_tags_equal(address, cleared ? 0 : 1 + i % 15));
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(address, cleared ? 0 : 0x50 + i, SZ_4K), NULL);
	}
	put_held(f, 1);
	copy = mm_subpage_copy_at(f->pool, f->held[count - 1], SZ_16K);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, copy);
	f->held[1] = copy;
	for (unsigned int i = 0; i < DATA_SLOTS; i++) {
		unsigned int source = i >= 4 && i < 8 ? i + DATA_SLOTS - 8 : i;
		void *address = folio_address(folio) + i * SZ_4K;

		KUNIT_EXPECT_TRUE(test, test_slot_tags_equal(address, 1 + source % 15));
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(address, 0x50 + source, SZ_4K), NULL);
	}
#else
	kunit_skip(test, "MTE is not configured");
#endif
}

static void expect_file_coverage(struct kunit *test, struct folio *folio,
		unsigned int leaves, unsigned int first, unsigned int tail,
		unsigned int final)
{
	struct mm_subpage_mapcounts counts;
	unsigned int i;

	KUNIT_EXPECT_EQ(test, mm_subpage_file_mapcounts(&folio->page, &counts), leaves);
	for (i = 0; i < DATA_SLOTS; i++) {
		unsigned int expected = i == 0 ? first :
			i == DATA_SLOTS - 1 ? final : i >= DATA_SLOTS - 4 ? tail : 0;

		KUNIT_EXPECT_EQ_MSG(test, counts.slots[i], expected, "coverage slot %u", i);
	}
}

static void user16k_file_coverage_test(struct kunit *test)
{
	struct folio *folio;
	phys_addr_t base, tail, last;

	if (PAGE_SHIFT <= 14) {
		kunit_skip(test, "16K alternative file coverage requires larger native backing");
		return;
	}
	folio = folio_alloc(GFP_KERNEL, 0);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, extent_folio_put, folio), 0);
	base = PFN_PHYS(folio_pfn(folio));
	tail = base + PAGE_SIZE - SZ_16K;
	last = base + PAGE_SIZE - SZ_4K;
	expect_file_coverage(test, folio, 0, 0, 0, 0);
	__mm_subpage_file_map_add(tail, SZ_16K);
	expect_file_coverage(test, folio, 1, 0, 1, 1);
	__mm_subpage_file_map_add(last, SZ_4K);
	expect_file_coverage(test, folio, 2, 0, 1, 2);
	__mm_subpage_file_map_add(tail, SZ_16K);
	expect_file_coverage(test, folio, 3, 0, 2, 3);
	__mm_subpage_file_map_add(base, SZ_4K);
	expect_file_coverage(test, folio, 4, 1, 2, 3);
	__mm_subpage_file_map_del(tail, SZ_16K);
	expect_file_coverage(test, folio, 3, 1, 1, 2);
	__mm_subpage_file_map_del(last, SZ_4K);
	expect_file_coverage(test, folio, 2, 1, 1, 1);
	__mm_subpage_file_map_del(tail, SZ_16K);
	expect_file_coverage(test, folio, 1, 1, 0, 0);
	__mm_subpage_file_map_del(base, SZ_4K);
	expect_file_coverage(test, folio, 0, 0, 0, 0);
}

static struct kunit_case user4k_data_cases[] = {
	KUNIT_CASE(user4k_data_reuse_test),
	KUNIT_CASE(user4k_data_refset_test),
#if IS_ENABLED(CONFIG_NET_SOCK_MSG) && IS_ENABLED(CONFIG_INET)
	KUNIT_CASE(user4k_data_skmsg_test),
	KUNIT_CASE(user16k_data_skmsg_test),
#endif
#ifdef CONFIG_NET
	KUNIT_CASE(user4k_data_skb_test),
	KUNIT_CASE(user4k_data_skb_checksum_test),

#ifdef CONFIG_INET
	KUNIT_CASE(user4k_data_skb_prune_test),
	KUNIT_CASE(user16k_data_skb_prune_test),
#endif
#endif
	KUNIT_CASE(user4k_data_mte_test),
	KUNIT_CASE(user4k_data_copy_extent_test),
	KUNIT_CASE(user16k_data_owner_test),
	KUNIT_CASE(user16k_file_coverage_test),
	KUNIT_CASE(user16k_data_refset_test),
	KUNIT_CASE(user16k_data_mte_test),
	KUNIT_CASE(user4k_data_swapcache_reuse_test),
	KUNIT_CASE(user4k_data_restore_test),
	KUNIT_CASE(user16k_data_restore_test),
	KUNIT_CASE(user4k_data_restore_race_test),
	KUNIT_CASE(user4k_data_migration_owner_test),
	KUNIT_CASE(user16k_data_migration_owner_test),
	KUNIT_CASE(user4k_data_copy_test),
	KUNIT_CASE(user4k_data_lookup_test),
	KUNIT_CASE(user4k_data_exact_slot_test),
	KUNIT_CASE(user4k_data_close_test),
	KUNIT_CASE(user4k_data_deferred_test),
	KUNIT_CASE(user4k_data_backing_reject_test),
	KUNIT_CASE(user4k_data_concurrent_test),
	KUNIT_CASE(user4k_data_lookup_race_test),
	{}
};

static void user4k_data_suite_exit(struct kunit_suite *suite)
{
	/* Drain batched metadata frees before the disposable guest powers off. */
	kvfree_rcu_barrier();
}

static struct kunit_suite user4k_data_suite = {
	.name = "arm64-user4k-data",
	.test_cases = user4k_data_cases,
	.suite_exit = user4k_data_suite_exit,
};

kunit_test_suite(user4k_data_suite);
MODULE_LICENSE("GPL");
