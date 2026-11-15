// SPDX-License-Identifier: GPL-2.0-only
/* Actual type1 lifecycle with recorded translations; no physical device DMA. */
#include <linux/completion.h>
#include <linux/shmem_fs.h>
#include <linux/mman.h>
#include <linux/sizes.h>

#define VFIO_TEST_VA SZ_64M
#define VFIO_TEST_IOVA SZ_256M

struct vfio_test_domain {
	struct iommu_domain core;
	struct vfio_domain type1;
	struct xarray translations;
	unsigned long mapped;
	unsigned long fail_after;
	unsigned long largest_map;
	unsigned int syncs;
	struct folio *observed;
	struct kunit *test;
};

struct vfio_fragment_fixture {
	struct vfio_iommu *iommu;
	struct vfio_test_domain domains[2];
	struct vfio_iommu_group group;
	struct vfio_device device;
	struct kunit *test;
	unsigned int external_refs;
	unsigned int notifications;
};

static int vfio_test_domain_map(struct iommu_domain *domain, unsigned long iova,
		phys_addr_t phys, size_t pgsize, size_t pgcount, int prot,
		gfp_t gfp, size_t *mapped)
{
	struct vfio_test_domain *mock = container_of(domain, typeof(*mock), core);
	unsigned long length = pgsize * pgcount;

	mock->largest_map = max(mock->largest_map, length);
	for (unsigned long done = 0; done < length; done += SZ_4K) {
		int ret;

		if (mock->fail_after && mock->mapped >= mock->fail_after)
			return -ENOMEM;
		ret = xa_insert(&mock->translations, (iova + done) >> 12,
				xa_mk_value((phys + done) >> 12), gfp);
		if (ret)
			return ret;
		mock->mapped += SZ_4K;
		*mapped += SZ_4K;
	}
	return 0;
}

static size_t vfio_test_domain_unmap(struct iommu_domain *domain, unsigned long iova,
		size_t pgsize, size_t pgcount, struct iommu_iotlb_gather *gather)
{
	struct vfio_test_domain *mock = container_of(domain, typeof(*mock), core);
	unsigned long length = pgsize * pgcount, done;

	for (done = 0; done < length; done += SZ_4K) {
		if (!xa_erase(&mock->translations, (iova + done) >> 12))
			break;
		mock->mapped -= SZ_4K;
	}
	return done;
}

static phys_addr_t vfio_test_domain_translate(struct iommu_domain *domain, dma_addr_t iova)
{
	struct vfio_test_domain *mock = container_of(domain, typeof(*mock), core);
	void *entry = xa_load(&mock->translations, iova >> 12);

	return entry ? ((phys_addr_t)xa_to_value(entry) << 12) + (iova & (SZ_4K - 1)) : 0;
}

static void vfio_test_domain_sync(struct iommu_domain *domain,
				 struct iommu_iotlb_gather *gather)
{
	struct vfio_test_domain *mock = container_of(domain, typeof(*mock), core);

	mock->syncs++;
	if (mock->observed)
		KUNIT_EXPECT_TRUE(mock->test, folio_maybe_dma_pinned(mock->observed));
}

static const struct iommu_domain_ops vfio_test_domain_ops = {
	.map_pages = vfio_test_domain_map,
	.unmap_pages = vfio_test_domain_unmap,
	.iova_to_phys = vfio_test_domain_translate,
	.iotlb_sync = vfio_test_domain_sync,
};

static void vfio_test_notify(struct vfio_device *device, u64 iova, u64 length)
{
	struct vfio_fragment_fixture *f = container_of(device, typeof(*f), device);

	f->notifications++;
	while (f->external_refs) {
		vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA, 1);
		f->external_refs--;
	}
}

static const struct vfio_device_ops vfio_test_device_ops = {
	.dma_unmap = vfio_test_notify,
};

static void vfio_fragment_fixture_free(void *ptr)
{
	struct vfio_fragment_fixture *f = ptr;
	struct rb_node *node;

	while ((node = rb_first(&f->iommu->dma_list))) {
		struct vfio_dma *dma = rb_entry(node, struct vfio_dma, node);
		struct rb_node *pin;

		while ((pin = rb_first(&dma->pfn_list))) {
			struct vfio_pfn *vpfn = rb_entry(pin, struct vfio_pfn, node);

			vfio_unpin_page_external(dma, vpfn->iova,
						list_empty(&f->iommu->domain_list));
		}
		vfio_remove_dma(f->iommu, dma);
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(f->domains); i++) {
		KUNIT_EXPECT_EQ(f->test, f->domains[i].mapped, 0UL);
		KUNIT_EXPECT_TRUE(f->test, xa_empty(&f->domains[i].translations));
		xa_destroy(&f->domains[i].translations);
	}
	kfree(f->iommu);
}

static struct vfio_fragment_fixture *vfio_fragment_fixture_new(struct kunit *test)
{
	struct vfio_fragment_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	if (!f)
		return NULL;
	f->test = test;
	f->iommu = vfio_iommu_type1_open(VFIO_TYPE1v2_IOMMU);
	if (IS_ERR(f->iommu))
		return NULL;
	for (unsigned int i = 0; i < ARRAY_SIZE(f->domains); i++) {
		struct vfio_test_domain *mock = &f->domains[i];

		xa_init(&mock->translations);
		mock->test = test;
		mock->core.ops = &vfio_test_domain_ops;
		mock->core.type = IOMMU_DOMAIN_UNMANAGED;
		mock->core.pgsize_bitmap = SZ_4K | PAGE_SIZE;
		mock->type1.domain = &mock->core;
		INIT_LIST_HEAD(&mock->type1.next);
		INIT_LIST_HEAD(&mock->type1.group_list);
	}
	f->group.iommu_group = (struct iommu_group *)f;
	list_add(&f->group.next, &f->iommu->emulated_iommu_groups);
	f->iommu->num_non_pinned_groups = 1;
	f->device.ops = &vfio_test_device_ops;
	vfio_iommu_type1_register_device(f->iommu, &f->device);
	if (kunit_add_action_or_reset(test, vfio_fragment_fixture_free, f))
		return NULL;
	return f;
}

static void vfio_test_file_put(void *ptr)
{
	fput(ptr);
}

static struct file *vfio_fragment_file(struct kunit *test, unsigned long length,
				     unsigned char value)
{
	struct file *file = shmem_file_setup("vfio-fragments", length, EMPTY_VMA_FLAGS);
	unsigned char *data;
	loff_t pos = 0;

	if (IS_ERR(file))
		return file;
	if (kunit_add_action_or_reset(test, vfio_test_file_put, file))
		return ERR_PTR(-ENOMEM);
	data = kunit_kmalloc(test, length, GFP_KERNEL);
	if (!data)
		return ERR_PTR(-ENOMEM);
	memset(data, value, length);
	if (kernel_write(file, data, length, &pos) != length)
		return ERR_PTR(-EIO);
	return file;
}

static unsigned long vfio_fragment_source(struct vfio_account_mm *ctx,
					 struct file *file, unsigned long length)
{
	unsigned long addr;

	kthread_use_mm(ctx->mm);
	addr = vm_mmap(file, VFIO_TEST_VA, length, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_FIXED_NOREPLACE, 0);
	kthread_unuse_mm(ctx->mm);
	return addr;
}

static int vfio_fragment_test_map(struct vfio_fragment_fixture *f,
				  struct vfio_account_mm *ctx,
				  unsigned long offset, unsigned long length)
{
	struct vfio_iommu_type1_dma_map map = {
		.argsz = sizeof(map),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
		.vaddr = VFIO_TEST_VA + offset,
		.iova = VFIO_TEST_IOVA,
		.size = length,
	};
	int ret;

	kthread_use_mm(ctx->mm);
	ret = vfio_dma_do_map(f->iommu, &map);
	kthread_unuse_mm(ctx->mm);
	return ret;
}

static void vfio_fragments_lifetime_test(struct kunit *test)
{
	unsigned int shifts[] = { 12, 14 };

	for (unsigned int i = 0; i < ARRAY_SIZE(shifts); i++) {
		struct vfio_account_mm *ctx;
		struct vfio_fragment_fixture *f;
		struct vfio_dma *dma;
		struct file *file;
		struct page *page;
		unsigned char data[64];
		struct vfio_iommu_type1_dma_unmap unmap = {
			.argsz = sizeof(unmap), .iova = VFIO_TEST_IOVA, .size = 2 * PAGE_SIZE,
		};
		struct vfio_bitmap bitmap = {};

		if (shifts[i] == PAGE_SHIFT)
			continue;
		ctx = vfio_account_mm_new(test, shifts[i]);
		KUNIT_ASSERT_NOT_NULL(test, ctx);
		file = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x71);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
		KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
		f = vfio_fragment_fixture_new(test);
		KUNIT_ASSERT_NOT_NULL(test, f);
		list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
		vfio_update_pgsize_bitmap(f->iommu);
		KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, ctx, 0, 2 * PAGE_SIZE), 0);
		dma = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
		KUNIT_ASSERT_NOT_NULL(test, dma);
		KUNIT_ASSERT_NOT_NULL(test, dma->fragments);
		KUNIT_EXPECT_EQ(test, dma->locked_vm, 2UL);
		for (unsigned int d = 0; d < 2; d++)
			f->domains[d].observed = vfio_fragment_find(dma, 0)->pin.folio;
		KUNIT_EXPECT_EQ(test, f->domains[0].mapped, 2 * PAGE_SIZE);
		KUNIT_EXPECT_GE(test, f->domains[0].largest_map, PAGE_SIZE);
		KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
			VFIO_TEST_IOVA + 17, 1, IOMMU_READ | IOMMU_WRITE, &page), 1);
		f->external_refs++;
		KUNIT_EXPECT_EQ(test, page_to_phys(page),
				iommu_iova_to_phys(&f->domains[0].core, VFIO_TEST_IOVA));
		KUNIT_EXPECT_EQ(test, dma->locked_vm, 2UL);
		/* The user VA goes away; retained pins are still the DMA buffer. */
		ctx->live = false;
		mmput(ctx->mm);
		KUNIT_ASSERT_EQ(test, atomic_read(&ctx->mm->mm_users), 0);
		KUNIT_ASSERT_EQ(test, vfio_iommu_type1_dma_rw(f->iommu, VFIO_TEST_IOVA,
			data, sizeof(data), false), 0);
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(data, 0x71, sizeof(data)), NULL);
		KUNIT_ASSERT_EQ(test, vfio_iommu_replay(f->iommu, &f->domains[1].type1), 0);
		list_add_tail(&f->domains[1].type1.next, &f->iommu->domain_list);
		KUNIT_EXPECT_EQ(test, f->domains[1].mapped, 2 * PAGE_SIZE);
		KUNIT_EXPECT_EQ(test, iommu_iova_to_phys(&f->domains[0].core, VFIO_TEST_IOVA),
				iommu_iova_to_phys(&f->domains[1].core, VFIO_TEST_IOVA));
		KUNIT_ASSERT_EQ(test, vfio_dma_do_unmap(f->iommu, &unmap, &bitmap), 0);
		KUNIT_EXPECT_EQ(test, unmap.size, 2 * PAGE_SIZE);
		KUNIT_EXPECT_EQ(test, f->notifications, 1U);
		KUNIT_EXPECT_GT(test, f->domains[0].syncs, 0U);
		KUNIT_EXPECT_GT(test, f->domains[1].syncs, 0U);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
		kunit_release_action(test, vfio_fragment_fixture_free, f);
		kunit_info(test, "type1 retained %uK user/%luK native pins after mm exit and replay",
			   1U << (shifts[i] - 10), PAGE_SIZE >> 10);
	}
}

static void vfio_fragments_lazy_test(struct kunit *test)
{
	struct vfio_account_mm *ctx = vfio_account_mm_new(test, 12);
	struct vfio_fragment_fixture *f;
	struct file *file;
	struct vfio_dma *dma;
	struct page *page;
	unsigned char data[64];

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	file = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x52);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
	f = vfio_fragment_fixture_new(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, ctx, 0, 2 * PAGE_SIZE), 0);
	dma = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
	KUNIT_ASSERT_NOT_NULL(test, dma);
	KUNIT_ASSERT_NOT_NULL(test, dma->fragments);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 0UL);
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_dma_rw(f->iommu, VFIO_TEST_IOVA,
		data, sizeof(data), false), 0);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(data, 0x52, sizeof(data)), NULL);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 0UL);
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
		VFIO_TEST_IOVA + 37, 1, IOMMU_READ, &page), 1);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 1UL);
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
		VFIO_TEST_IOVA, 1, IOMMU_READ, &page), 1);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 1UL);
	ctx->live = false;
	mmput(ctx->mm);
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
		VFIO_TEST_IOVA, 1, IOMMU_READ, &page), 1);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 1UL);
	KUNIT_EXPECT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
		VFIO_TEST_IOVA + PAGE_SIZE, 1, IOMMU_READ, &page), -EFAULT);
	for (unsigned int i = 0; i < 3; i++)
		vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA + 37, 1);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 0UL);
	KUNIT_EXPECT_TRUE(test, RB_EMPTY_ROOT(&dma->fragments->records));
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
	kunit_release_action(test, vfio_fragment_fixture_free, f);
}

static void vfio_fragments_map_failure_test(struct kunit *test)
{
	struct vfio_account_mm *ctx = vfio_account_mm_new(test, 12);
	struct vfio_fragment_fixture *f;
	struct file *file;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	file = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x25);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
	f = vfio_fragment_fixture_new(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
	list_add_tail(&f->domains[1].type1.next, &f->iommu->domain_list);
	f->domains[1].fail_after = SZ_4K;
	KUNIT_EXPECT_EQ(test, vfio_fragment_test_map(f, ctx, 0, 2 * PAGE_SIZE), -ENOMEM);
	KUNIT_EXPECT_TRUE(test, RB_EMPTY_ROOT(&f->iommu->dma_list));
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
	KUNIT_EXPECT_EQ(test, f->domains[0].mapped, 0UL);
	KUNIT_EXPECT_EQ(test, f->domains[1].mapped, 0UL);
	f->domains[1].fail_after = 0;
	KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, ctx, 0, 2 * PAGE_SIZE), 0);
	kunit_release_action(test, vfio_fragment_fixture_free, f);
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
}

static void vfio_fragments_fine_test(struct kunit *test)
{
	unsigned int shifts[] = { PAGE_SHIFT, 12, 14 };

	for (unsigned int i = 0; i < ARRAY_SIZE(shifts); i++) {
		struct vfio_account_mm *ctx;
		struct vfio_fragment_fixture *f;
		struct vfio_dma *dma;
		struct file *file;
		struct page *page = NULL;
		unsigned char data[64];
		unsigned long length = PAGE_SIZE + SZ_4K;
		size_t copied = 0;

		if (i && shifts[i] == PAGE_SHIFT)
			continue;
		ctx = vfio_account_mm_new(test, shifts[i]);
		KUNIT_ASSERT_NOT_NULL(test, ctx);
		file = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x64);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
		KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
		f = vfio_fragment_fixture_new(test);
		KUNIT_ASSERT_NOT_NULL(test, f);
		list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
		vfio_update_pgsize_bitmap(f->iommu);
		KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, ctx, SZ_4K, length), 0);
		dma = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
		KUNIT_ASSERT_NOT_NULL(test, dma);
		KUNIT_ASSERT_NOT_NULL(test, dma->fragments);
		KUNIT_EXPECT_EQ(test, f->domains[0].mapped, length);
		KUNIT_EXPECT_EQ(test, dma->locked_vm, 2UL);
		/* The physical base is offset inside a native page, not representable. */
		KUNIT_EXPECT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
			VFIO_TEST_IOVA, 1, IOMMU_READ, &page), -EINVAL);
		KUNIT_EXPECT_EQ(test, dma->locked_vm, 2UL);
		KUNIT_EXPECT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
			VFIO_TEST_IOVA + PAGE_SIZE, 1, IOMMU_READ, &page), -EINVAL);
		KUNIT_ASSERT_EQ(test, vfio_dma_bitmap_alloc_all(f->iommu,
			vfio_dirty_pgsize(f->iommu)), 0);
		f->iommu->dirty_page_tracking = true;
		memset(data, 0x97, sizeof(data));
		KUNIT_ASSERT_EQ(test, vfio_iommu_type1_dma_rw_chunk(f->iommu,
			VFIO_TEST_IOVA + SZ_4K - 17, data, sizeof(data), true, &copied), 0);
		KUNIT_EXPECT_EQ(test, copied, sizeof(data));
		KUNIT_EXPECT_EQ(test, bitmap_weight(dma->bitmap, length / SZ_4K), 2U);
		KUNIT_EXPECT_TRUE(test, test_bit(0, dma->bitmap));
		KUNIT_EXPECT_TRUE(test, test_bit(1, dma->bitmap));
		memset(data, 0, sizeof(data));
		KUNIT_ASSERT_EQ(test, vfio_iommu_type1_dma_rw(f->iommu,
			VFIO_TEST_IOVA + SZ_4K - 17, data, sizeof(data), false), 0);
		KUNIT_EXPECT_PTR_EQ(test, memchr_inv(data, 0x97, sizeof(data)), NULL);
		KUNIT_EXPECT_EQ(test, vfio_iommu_type1_dma_rw(f->iommu,
			VFIO_TEST_IOVA + length, data, sizeof(data), false), -EINVAL);
		kunit_release_action(test, vfio_fragment_fixture_free, f);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
		kunit_info(test, "%uK userspace fine IOVA/physical offsets and dirty bytes checked",
			   1U << (shifts[i] - 10));
	}
}

static void vfio_fragments_replay_failure_test(struct kunit *test)
{
	struct vfio_account_mm *ctx = vfio_account_mm_new(test, 12);
	struct vfio_fragment_fixture *f;
	struct vfio_dma *dma;
	struct file *file;
	struct page *page;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	file = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x58);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
	f = vfio_fragment_fixture_new(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, ctx, 0, 2 * PAGE_SIZE), 0);
	dma = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
	KUNIT_ASSERT_NOT_NULL(test, dma);
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
		VFIO_TEST_IOVA, 1, IOMMU_READ, &page), 1);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 1UL);
	f->domains[0].fail_after = SZ_4K;
	KUNIT_EXPECT_EQ(test, vfio_iommu_replay(f->iommu, &f->domains[0].type1), -ENOMEM);
	KUNIT_EXPECT_EQ(test, f->domains[0].mapped, 0UL);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 1UL);
	KUNIT_EXPECT_FALSE(test, dma->iommu_mapped);
	f->domains[0].fail_after = 0;
	KUNIT_ASSERT_EQ(test, vfio_iommu_replay(f->iommu, &f->domains[0].type1), 0);
	list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 2UL);
	/* Detaching the last hardware domain keeps only the external page. */
	vfio_iommu_unmap_unpin_reaccount(f->iommu);
	list_del_init(&f->domains[0].type1.next);
	vfio_update_pgsize_bitmap(f->iommu);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 1UL);
	KUNIT_EXPECT_EQ(test, f->domains[0].mapped, 0UL);
	vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA, 1);
	KUNIT_EXPECT_EQ(test, dma->locked_vm, 0UL);
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
	kunit_release_action(test, vfio_fragment_fixture_free, f);
}

static void vfio_fragments_anon_owner_test(struct kunit *test)
{
	struct vfio_account_mm *old = vfio_account_mm_new(test, 12);
	struct vfio_account_mm *next = vfio_account_mm_new(test, PAGE_SHIFT);
	struct vfio_fragment_fixture *f;
	struct vfio_dma *dma;
	unsigned char *data = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	unsigned long addr, copied, charge;
	unsigned char check[64];

	KUNIT_ASSERT_NOT_NULL(test, old);
	KUNIT_ASSERT_NOT_NULL(test, next);
	KUNIT_ASSERT_NOT_NULL(test, data);
	memset(data, 0x49, PAGE_SIZE);
	kthread_use_mm(old->mm);
	addr = vm_mmap(NULL, VFIO_TEST_VA, PAGE_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0);
	copied = addr == VFIO_TEST_VA ?
		copy_to_user((void __user *)addr, data, PAGE_SIZE) : PAGE_SIZE;
	kthread_unuse_mm(old->mm);
	KUNIT_ASSERT_EQ(test, addr, VFIO_TEST_VA);
	KUNIT_ASSERT_EQ(test, copied, 0UL);
	f = vfio_fragment_fixture_new(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
	KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, old, 0, PAGE_SIZE), 0);
	dma = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
	KUNIT_ASSERT_NOT_NULL(test, dma);
	KUNIT_ASSERT_NOT_NULL(test, vfio_fragment_find(dma, 0)->pin.subpage);
	charge = dma->locked_vm;
	KUNIT_ASSERT_GT(test, charge, 0UL);
	old->live = false;
	mmput(old->mm);
	KUNIT_ASSERT_EQ(test, vfio_change_dma_mm(dma, dma->task, next->mm, true), 0);
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(old->mm), 0UL);
	KUNIT_EXPECT_EQ(test, mm_locked_vm_native_pages(next->mm), charge);
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_dma_rw(f->iommu, VFIO_TEST_IOVA,
		check, sizeof(check), false), 0);
	KUNIT_EXPECT_PTR_EQ(test, memchr_inv(check, 0x49, sizeof(check)), NULL);
	KUNIT_ASSERT_EQ(test, vfio_iommu_replay(f->iommu, &f->domains[1].type1), 0);
	list_add_tail(&f->domains[1].type1.next, &f->iommu->domain_list);
	kunit_release_action(test, vfio_fragment_fixture_free, f);
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(next->mm), 0UL);
	kunit_info(test, "anonymous 4K slot pins survive source exit and native-owner transfer");
}

static void vfio_fragments_domain_granule_test(struct kunit *test)
{
	struct vfio_account_mm *ctx = vfio_account_mm_new(test, 12);
	struct vfio_fragment_fixture *f;
	struct vfio_dma *dma;
	struct file *file;
	struct page *page;

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	file = vfio_fragment_file(test, PAGE_SIZE, 0x73);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, PAGE_SIZE), VFIO_TEST_VA);
	f = vfio_fragment_fixture_new(test);
	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, ctx, 0, PAGE_SIZE), 0);
	dma = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
	KUNIT_ASSERT_NOT_NULL(test, dma);
	KUNIT_ASSERT_EQ(test, vfio_dma_bitmap_alloc_all(f->iommu,
		vfio_dirty_pgsize(f->iommu)), 0);
	f->iommu->dirty_page_tracking = true;
	KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
		VFIO_TEST_IOVA + 31, 1, IOMMU_READ, &page), 1);
	KUNIT_EXPECT_EQ(test, bitmap_weight(dma->bitmap, PAGE_SIZE / SZ_4K),
			(unsigned int)(PAGE_SIZE / SZ_4K));
	f->domains[0].core.pgsize_bitmap = SZ_16K;
	KUNIT_ASSERT_EQ(test, vfio_iommu_replay(f->iommu, &f->domains[0].type1), 0);
	list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
	vfio_update_pgsize_bitmap(f->iommu);
	KUNIT_EXPECT_EQ(test, 1UL << __ffs(f->iommu->pgsize_bitmap), SZ_16K);
	KUNIT_EXPECT_EQ(test, vfio_dirty_pgsize(f->iommu), SZ_4K);
	vfio_iommu_unmap_unpin_reaccount(f->iommu);
	list_del_init(&f->domains[0].type1.next);
	vfio_update_pgsize_bitmap(f->iommu);
	KUNIT_EXPECT_EQ(test, 1UL << __ffs(f->iommu->pgsize_bitmap), SZ_4K);
	KUNIT_EXPECT_EQ(test, vfio_dirty_pgsize(f->iommu), SZ_4K);
	vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA + 31, 1);
	kunit_release_action(test, vfio_fragment_fixture_free, f);
	KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
}

static void vfio_fragments_native_owner_test(struct kunit *test)
{
	for (unsigned int variant = 0; variant < 9; variant++) {
		bool same_mm = variant == 8;
		bool mapped = same_mm || (variant & 1);
		bool alias = same_mm || (variant & 2), different = variant & 4;
		struct vfio_account_mm *old = vfio_account_mm_new(test, PAGE_SHIFT);
		struct vfio_account_mm *next = same_mm ? old : vfio_account_mm_new(test, 12);
		struct vfio_fragment_fixture *f;
		struct vfio_fragment_provider *prepared;
		struct file *file, *replacement;
		struct vfio_dma *dma;
		struct page *pages[2];
		phys_addr_t external[2];
		unsigned long unique, addr;
		unsigned char data[32];
		int ret;

		KUNIT_ASSERT_NOT_NULL(test, old);
		KUNIT_ASSERT_NOT_NULL(test, next);
		file = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x63);
		replacement = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x44);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, replacement);
		KUNIT_ASSERT_EQ(test, vfio_fragment_source(old, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
		if (alias) {
			kthread_use_mm(old->mm);
			addr = vm_mmap(file, VFIO_TEST_VA + PAGE_SIZE, PAGE_SIZE,
				PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, 0);
			kthread_unuse_mm(old->mm);
			KUNIT_ASSERT_EQ(test, addr, VFIO_TEST_VA + PAGE_SIZE);
		}
		f = vfio_fragment_fixture_new(test);
		KUNIT_ASSERT_NOT_NULL(test, f);
		if (mapped) {
			list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
			vfio_update_pgsize_bitmap(f->iommu);
		}
		KUNIT_ASSERT_EQ(test, vfio_fragment_test_map(f, old, 0, 2 * PAGE_SIZE), 0);
		dma = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
		KUNIT_ASSERT_NOT_NULL(test, dma);
		KUNIT_ASSERT_NULL(test, dma->fragments);
		if (different) {
			kthread_use_mm(old->mm);
			addr = vm_mmap(replacement, VFIO_TEST_VA, 2 * PAGE_SIZE,
				PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, 0);
			kthread_unuse_mm(old->mm);
			KUNIT_ASSERT_EQ(test, addr, VFIO_TEST_VA);
		}
		KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu,
			f->group.iommu_group, VFIO_TEST_IOVA + 17, 2,
			IOMMU_READ | IOMMU_WRITE, pages), 2);
		for (unsigned int i = 0; i < 2; i++)
			external[i] = page_to_phys(pages[i]);
		if (mapped && different)
			KUNIT_EXPECT_NE(test, external[0],
				iommu_iova_to_phys(&f->domains[0].core, VFIO_TEST_IOVA));
		KUNIT_ASSERT_EQ(test, dma->locked_vm, 2UL);
		if (!same_mm) {
			old->live = false;
			mmput(old->mm);
			KUNIT_ASSERT_EQ(test, atomic_read(&old->mm->mm_users), 0);
		}

		prepared = vfio_fragments_prepare_native(f->iommu, dma);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, prepared);
		unique = mapped ? (alias ? 1UL : 2UL) : 0;
		if (!mapped || different)
			unique += different || !alias ? 2UL : 1UL;
		KUNIT_EXPECT_EQ(test, vfio_fragments_native_pages(prepared), unique);
		/* Failed new-owner credit must not publish metadata or release pins. */
		if (!same_mm && task_rlimit(dma->task, RLIMIT_MEMLOCK) != RLIM_INFINITY) {
			unsigned long budget = task_rlimit(dma->task, RLIMIT_MEMLOCK) >>
				mm_page_shift(next->mm);

			next->mm->locked_vm = budget;
			KUNIT_EXPECT_EQ(test, vfio_change_dma_mm_pages(dma, dma->task,
				next->mm, false, unique), -ENOMEM);
			KUNIT_EXPECT_PTR_EQ(test, dma->mm, old->mm);
			KUNIT_EXPECT_EQ(test, dma->locked_vm, 2UL);
			KUNIT_EXPECT_EQ(test, mm_locked_vm_native_pages(old->mm), 2UL);
			KUNIT_EXPECT_EQ(test, next->mm->locked_vm, budget);
			next->mm->locked_vm = 0;
		}
		vfio_fragments_abort_native(prepared);
		KUNIT_EXPECT_PTR_EQ(test, dma->fragments, NULL);
		KUNIT_EXPECT_TRUE(test, folio_maybe_dma_pinned(page_folio(pages[0])));
		kthread_use_mm(next->mm);
		ret = vfio_change_dma_owner(f->iommu, dma,
			VFIO_TEST_VA + (same_mm ? SZ_4K : 0));
		kthread_unuse_mm(next->mm);
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_ASSERT_NOT_NULL(test, dma->fragments);
		KUNIT_EXPECT_EQ(test, dma->locked_vm, unique);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_native_pages(old->mm), same_mm ? unique : 0UL);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_native_pages(next->mm), unique);
		KUNIT_ASSERT_EQ(test, vfio_iommu_type1_dma_rw(f->iommu, VFIO_TEST_IOVA,
			data, sizeof(data), false), 0);
		KUNIT_EXPECT_PTR_EQ(test,
			memchr_inv(data, !mapped && different ? 0x44 : 0x63, sizeof(data)), NULL);
		KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu,
			f->group.iommu_group, VFIO_TEST_IOVA + 31, 2,
			IOMMU_READ | IOMMU_WRITE, pages), 2);
		for (unsigned int i = 0; i < 2; i++)
			KUNIT_EXPECT_EQ(test, page_to_phys(pages[i]), external[i]);
		KUNIT_EXPECT_EQ(test, dma->locked_vm, unique);
		KUNIT_ASSERT_EQ(test, vfio_iommu_replay(f->iommu, &f->domains[1].type1), 0);
		list_add_tail(&f->domains[1].type1.next, &f->iommu->domain_list);
		vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA + 31, 2);
		vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA + 17, 2);
		KUNIT_EXPECT_EQ(test, dma->locked_vm,
			mapped ? (alias ? 1UL : 2UL) : unique);
		kunit_release_action(test, vfio_fragment_fixture_free, f);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_native_pages(next->mm), 0UL);
		kunit_info(test, "native ownership variant=%u preserves PIN identity and unique charge",
			   variant);
	}
}

static int vfio_fragment_map_area(struct vfio_fragment_fixture *f,
		struct vfio_account_mm *ctx, unsigned long iova_offset,
		unsigned long va_offset, unsigned long length, unsigned int flags)
{
	struct vfio_iommu_type1_dma_map map = {
		.argsz = sizeof(map),
		.flags = flags,
		.vaddr = VFIO_TEST_VA + va_offset,
		.iova = VFIO_TEST_IOVA + iova_offset,
		.size = length,
	};
	int ret;

	kthread_use_mm(ctx->mm);
	ret = vfio_dma_do_map(f->iommu, &map);
	kthread_unuse_mm(ctx->mm);
	return ret;
}

static void vfio_fragments_external_span_test(struct kunit *test)
{
	unsigned int rw = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;

	for (unsigned int mapped = 0; mapped < 2; mapped++) {
		struct vfio_account_mm *a = vfio_account_mm_new(test, 12);
		struct vfio_account_mm *b = vfio_account_mm_new(test, PAGE_SHIFT);
		struct vfio_fragment_fixture *f = vfio_fragment_fixture_new(test);
		struct file *file = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x6b);
		struct vfio_iommu_type1_dma_unmap unmap = {
			.argsz = sizeof(unmap), .iova = VFIO_TEST_IOVA, .size = SZ_4K,
		};
		struct vfio_bitmap bitmap = {};
		struct vfio_dma *first, *rest;
		struct page *pages[3];
		unsigned long bits = 2 * PAGE_SIZE / SZ_4K - 1;

		KUNIT_ASSERT_NOT_NULL(test, a);
		KUNIT_ASSERT_NOT_NULL(test, b);
		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
		KUNIT_ASSERT_EQ(test, vfio_fragment_source(a, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
		KUNIT_ASSERT_EQ(test, vfio_fragment_source(b, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
		if (mapped) {
			list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
			vfio_update_pgsize_bitmap(f->iommu);
		}
		KUNIT_ASSERT_EQ(test, vfio_fragment_map_area(f, a, 0, 0, SZ_4K, rw), 0);
		KUNIT_ASSERT_EQ(test, vfio_fragment_map_area(f, b, SZ_4K, SZ_4K,
			2 * PAGE_SIZE - SZ_4K, rw), 0);
		first = vfio_find_dma(f->iommu, VFIO_TEST_IOVA, 1);
		rest = vfio_find_dma(f->iommu, VFIO_TEST_IOVA + SZ_4K, 1);
		KUNIT_ASSERT_NOT_NULL(test, first);
		KUNIT_ASSERT_NOT_NULL(test, rest);
		f->iommu->dirty_page_tracking = true;
		KUNIT_ASSERT_EQ(test, vfio_dma_bitmap_alloc_all(f->iommu, SZ_4K), 0);
		KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu,
			f->group.iommu_group, VFIO_TEST_IOVA + 37, 2,
			IOMMU_READ | IOMMU_WRITE, pages), 2);
		KUNIT_EXPECT_EQ(test, first->locked_vm, 1UL);
		KUNIT_EXPECT_EQ(test, rest->locked_vm, 2UL);
		KUNIT_EXPECT_EQ(test, bitmap_weight(first->bitmap, 1), 1);
		KUNIT_EXPECT_EQ(test, bitmap_weight(rest->bitmap, bits), (int)bits);
		bitmap_clear(rest->bitmap, 0, bits);
		vfio_dma_populate_bitmap(rest, SZ_4K);
		KUNIT_EXPECT_EQ(test, bitmap_weight(rest->bitmap, bits), (int)bits);
		if (mapped)
			KUNIT_EXPECT_EQ(test, page_to_phys(pages[0]),
				iommu_iova_to_phys(&f->domains[0].core, VFIO_TEST_IOVA));
		/* A failed larger request must leave prior caller references intact. */
		KUNIT_EXPECT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu,
			f->group.iommu_group, VFIO_TEST_IOVA, 3, IOMMU_READ, pages), -EINVAL);
		KUNIT_EXPECT_PTR_EQ(test, pages[0], NULL);
		KUNIT_EXPECT_PTR_EQ(test, pages[1], NULL);
		KUNIT_EXPECT_PTR_EQ(test, pages[2], NULL);
		a->live = b->live = false;
		mmput(a->mm);
		mmput(b->mm);
		KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu,
			f->group.iommu_group, VFIO_TEST_IOVA, 2, IOMMU_READ, pages), 2);
		vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA, 2);
		/* Release the second page first: it shares the second DMA owner. */
		vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA + PAGE_SIZE + 7, 1);
		KUNIT_EXPECT_EQ(test, rest->locked_vm, mapped ? 2UL : 1UL);
		f->external_refs = 1;
		KUNIT_ASSERT_EQ(test, vfio_dma_do_unmap(f->iommu, &unmap, &bitmap), 0);
		KUNIT_EXPECT_EQ(test, f->notifications, 1U);
		KUNIT_EXPECT_EQ(test, f->external_refs, 0U);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(a->mm), 0UL);
		KUNIT_EXPECT_EQ(test, rest->locked_vm, mapped ? 2UL : 0UL);
		unmap.iova += SZ_4K;
		unmap.size = 2 * PAGE_SIZE - SZ_4K;
		KUNIT_ASSERT_EQ(test, vfio_dma_do_unmap(f->iommu, &unmap, &bitmap), 0);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(b->mm), 0UL);
		kunit_release_action(test, vfio_fragment_fixture_free, f);
	}
	for (unsigned int failure = 0; failure < 4; failure++) {
		struct vfio_account_mm *ctx = vfio_account_mm_new(test, 12);
		struct vfio_fragment_fixture *f = vfio_fragment_fixture_new(test);
		struct file *file = vfio_fragment_file(test, 2 * PAGE_SIZE, 0x3a);
		struct page *page = NULL;
		struct rb_node *node;
		unsigned long va = failure == 2 ? PAGE_SIZE + SZ_4K : SZ_4K;
		int expected = failure == 1 ? -EPERM : -EINVAL;

		KUNIT_ASSERT_NOT_NULL(test, ctx);
		KUNIT_ASSERT_NOT_NULL(test, f);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
		KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, 2 * PAGE_SIZE), VFIO_TEST_VA);
		KUNIT_ASSERT_EQ(test, vfio_fragment_map_area(f, ctx, 0, 0, SZ_4K, rw), 0);
		if (failure == 3)
			va += SZ_4K;
		if (failure != 0)
			KUNIT_ASSERT_EQ(test, vfio_fragment_map_area(f, ctx, SZ_4K, va,
				SZ_4K, failure == 1 ? VFIO_DMA_MAP_FLAG_READ : rw), 0);
		KUNIT_EXPECT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu,
			f->group.iommu_group, VFIO_TEST_IOVA, 1,
			IOMMU_READ | IOMMU_WRITE, &page), expected);
		KUNIT_EXPECT_PTR_EQ(test, page, NULL);
		KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
		for (node = rb_first(&f->iommu->dma_list); node; node = rb_next(node)) {
			struct vfio_dma *dma = rb_entry(node, struct vfio_dma, node);

			KUNIT_EXPECT_TRUE(test, RB_EMPTY_ROOT(&dma->pfn_list));
			KUNIT_EXPECT_TRUE(test, RB_EMPTY_ROOT(&dma->fragments->records));
		}
		kunit_release_action(test, vfio_fragment_fixture_free, f);
	}
}

#define VFIO_STRESS_CYCLES 1024

struct vfio_pin_stress {
	struct vfio_fragment_fixture *fixture;
	struct vfio_device consumer;
	struct vfio_device registrar;
	struct vfio_device observer;
	struct task_struct *competing_thread;
	struct task_struct *pin_thread;
	struct task_struct *register_thread;
	struct mutex consumer_lock;
	struct completion pin_go;
	struct completion pin_ready;
	struct completion notify_entered;
	struct completion pin_done;
	struct completion register_go;
	struct completion register_entered;
	struct completion register_done;
	struct completion competing_go;
	struct completion competing_done;
	unsigned long competing_size;
	int competing_ret;
	bool competing;
	unsigned int competing_unmaps;
	unsigned int observations;
	atomic_t error;
	unsigned int pins;
	unsigned int registrations;
	bool stopping;
};

static void vfio_stress_error(struct vfio_pin_stress *s, int ret)
{
	atomic_cmpxchg(&s->error, 0, ret < 0 ? ret : -EIO);
}

static void vfio_stress_notify(struct vfio_device *device, u64 iova, u64 length)
{
	struct vfio_pin_stress *s = container_of(device, typeof(*s), consumer);
	struct vfio_fragment_fixture *f = s->fixture;

	/* The callback holds the production device-list lock, not the DMA lock. */
	complete(&s->register_go);
	if (!wait_for_completion_timeout(&s->register_entered, msecs_to_jiffies(5000)))
		vfio_stress_error(s, -ETIMEDOUT);
	complete(&s->notify_entered);
	mutex_lock(&s->consumer_lock);
	f->notifications++;
	while (f->external_refs) {
		vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA, 1);
		f->external_refs--;
	}
	mutex_unlock(&s->consumer_lock);
	if (s->competing) {
		/* A second unmap may free the DMA while this callback still runs. */
		complete(&s->competing_go);
		if (!wait_for_completion_timeout(&s->competing_done, msecs_to_jiffies(5000)))
			vfio_stress_error(s, -ETIMEDOUT);
	}
}

static void vfio_stress_observe(struct vfio_device *device, u64 iova, u64 length)
{
	struct vfio_pin_stress *s = container_of(device, typeof(*s), observer);

	s->observations++;
	if (iova != VFIO_TEST_IOVA || length != SZ_4K)
		vfio_stress_error(s, -ESTALE);
}

static void vfio_stress_empty_notify(struct vfio_device *device, u64 iova, u64 length)
{
}

static const struct vfio_device_ops vfio_stress_consumer_ops = {
	.dma_unmap = vfio_stress_notify,
};

static const struct vfio_device_ops vfio_stress_registrar_ops = {
	.dma_unmap = vfio_stress_empty_notify,
};

static const struct vfio_device_ops vfio_stress_observer_ops = {
	.dma_unmap = vfio_stress_observe,
};

static int vfio_stress_competing_unmaps(void *data)
{
	struct vfio_pin_stress *s = data;

	for (;;) {
		struct vfio_iommu_type1_dma_unmap unmap = {
			.argsz = sizeof(unmap), .iova = VFIO_TEST_IOVA,
					.size = SZ_4K,
		};
		struct vfio_bitmap bitmap = {};

		wait_for_completion(&s->competing_go);
		if (READ_ONCE(s->stopping) || kthread_should_stop())
			break;
		s->competing_ret = vfio_dma_do_unmap(s->fixture->iommu, &unmap, &bitmap);
		s->competing_size = unmap.size;
		s->competing_unmaps++;
		complete(&s->competing_done);
	}
	return 0;
}

static int vfio_stress_pinner(void *data)
{
	struct vfio_pin_stress *s = data;
	struct vfio_fragment_fixture *f = s->fixture;

	for (;;) {
		struct page *page = NULL, *original;
		void *bytes;
		int ret;

		wait_for_completion(&s->pin_go);
		if (READ_ONCE(s->stopping) || kthread_should_stop())
			break;
		mutex_lock(&s->consumer_lock);
		ret = vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
			VFIO_TEST_IOVA + 37, 1, IOMMU_READ | IOMMU_WRITE, &page);
		if (ret != 1) {
			vfio_stress_error(s, ret);
			mutex_unlock(&s->consumer_lock);
			complete(&s->pin_ready);
			complete(&s->pin_done);
			continue;
		}
		f->external_refs++;
		s->pins++;
		original = page;
		complete(&s->pin_ready);
		/* Unmap cannot revoke a reference before the consumer publishes it. */
		if (!wait_for_completion_timeout(&s->notify_entered, msecs_to_jiffies(5000)))
			vfio_stress_error(s, -ETIMEDOUT);
		if (!READ_ONCE(s->stopping)) {
			ret = vfio_iommu_type1_pin_pages(f->iommu, f->group.iommu_group,
				VFIO_TEST_IOVA + 17, 1, IOMMU_READ, &page);
			if (ret == 1) {
				f->external_refs++;
				s->pins++;
				if (page != original)
					vfio_stress_error(s, -ESTALE);
				vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA + 17, 1);
				f->external_refs--;
			} else {
				vfio_stress_error(s, ret);
			}
			bytes = kmap_local_page(original);
			if (memchr_inv(bytes, 0x6b, PAGE_SIZE))
				vfio_stress_error(s, -EIO);
			kunmap_local(bytes);
		}
		vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA + 37, 1);
		f->external_refs--;
		mutex_unlock(&s->consumer_lock);
		complete(&s->pin_done);
	}
	return 0;
}

static int vfio_stress_registrations(void *data)
{
	struct vfio_pin_stress *s = data;

	for (;;) {
		wait_for_completion(&s->register_go);
		if (READ_ONCE(s->stopping) || kthread_should_stop())
			break;
		complete(&s->register_entered);
		vfio_iommu_type1_register_device(s->fixture->iommu, &s->registrar);
		vfio_iommu_type1_unregister_device(s->fixture->iommu, &s->registrar);
		s->registrations++;
		complete(&s->register_done);
	}
	return 0;
}

static void vfio_stress_free(void *data)
{
	struct vfio_pin_stress *s = data;
	struct vfio_fragment_fixture *f = s->fixture;

	WRITE_ONCE(s->stopping, true);
	complete_all(&s->pin_go);
	complete_all(&s->notify_entered);
	complete_all(&s->register_go);
	complete_all(&s->competing_go);
	if (s->competing_thread) {
		kthread_stop(s->competing_thread);
		put_task_struct(s->competing_thread);
	}
	if (s->pin_thread) {
		kthread_stop(s->pin_thread);
		put_task_struct(s->pin_thread);
	}
	if (s->register_thread) {
		kthread_stop(s->register_thread);
		put_task_struct(s->register_thread);
	}
	mutex_lock(&s->consumer_lock);
	while (f->external_refs) {
		vfio_iommu_type1_unpin_pages(f->iommu, VFIO_TEST_IOVA, 1);
		f->external_refs--;
	}
	mutex_unlock(&s->consumer_lock);
	vfio_iommu_type1_unregister_device(f->iommu, &s->consumer);
	vfio_iommu_type1_unregister_device(f->iommu, &s->observer);
}

static struct vfio_pin_stress *vfio_stress_new(struct kunit *test,
					     struct vfio_fragment_fixture *f)
{
	struct vfio_pin_stress *s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);

	if (!s)
		return NULL;
	s->fixture = f;
	mutex_init(&s->consumer_lock);
	init_completion(&s->pin_go);
	init_completion(&s->pin_ready);
	init_completion(&s->notify_entered);
	init_completion(&s->pin_done);
	init_completion(&s->register_go);
	init_completion(&s->register_entered);
	init_completion(&s->register_done);
	init_completion(&s->competing_go);
	init_completion(&s->competing_done);
	atomic_set(&s->error, 0);
	s->consumer.ops = &vfio_stress_consumer_ops;
	s->registrar.ops = &vfio_stress_registrar_ops;
	s->observer.ops = &vfio_stress_observer_ops;
	vfio_iommu_type1_unregister_device(f->iommu, &f->device);
	/* list_add puts the consumer before the observer in callback order. */
	vfio_iommu_type1_register_device(f->iommu, &s->observer);
	vfio_iommu_type1_register_device(f->iommu, &s->consumer);
	if (kunit_add_action_or_reset(test, vfio_stress_free, s))
		return NULL;
	s->pin_thread = kthread_run(vfio_stress_pinner, s, "vfio-pin-stress");
	if (IS_ERR(s->pin_thread)) {
		s->pin_thread = NULL;
		return NULL;
	}
	get_task_struct(s->pin_thread);
	s->register_thread = kthread_run(vfio_stress_registrations, s, "vfio-reg-stress");
	if (IS_ERR(s->register_thread)) {
		s->register_thread = NULL;
		return NULL;
	}
	get_task_struct(s->register_thread);
	s->competing_thread = kthread_run(vfio_stress_competing_unmaps, s, "vfio-unmap-stress");
	if (IS_ERR(s->competing_thread)) {
		s->competing_thread = NULL;
		return NULL;
	}
	get_task_struct(s->competing_thread);
	return s;
}

static void vfio_fragments_concurrency_test(struct kunit *test)
{
	unsigned int shifts[] = { 12, 14, 16 };
	unsigned int rw = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;

	for (unsigned int shift = 0; shift < ARRAY_SIZE(shifts); shift++) {
		if (shifts[shift] > PAGE_SHIFT)
			continue;
		for (unsigned int mapped = 0; mapped < 2; mapped++) {
			struct vfio_account_mm *ctx = vfio_account_mm_new(test, shifts[shift]);
			struct vfio_fragment_fixture *f = vfio_fragment_fixture_new(test);
			struct file *file = vfio_fragment_file(test, PAGE_SIZE, 0x6b);
			struct vfio_pin_stress *s;

			KUNIT_ASSERT_NOT_NULL(test, ctx);
			KUNIT_ASSERT_NOT_NULL(test, f);
			KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
			KUNIT_ASSERT_EQ(test, vfio_fragment_source(ctx, file, PAGE_SIZE),
				VFIO_TEST_VA);
			s = vfio_stress_new(test, f);
			KUNIT_ASSERT_NOT_NULL(test, s);
			if (mapped) {
				list_add_tail(&f->domains[0].type1.next, &f->iommu->domain_list);
				vfio_update_pgsize_bitmap(f->iommu);
			}
			for (unsigned int cycle = 0; cycle < VFIO_STRESS_CYCLES; cycle++) {
				struct vfio_iommu_type1_dma_unmap unmap = {
					.argsz = sizeof(unmap), .iova = VFIO_TEST_IOVA,
					.size = SZ_4K,
				};
				struct vfio_bitmap bitmap = {};
				bool fine = cycle & 1;
				unsigned long remaining = PAGE_SIZE - SZ_4K;

				KUNIT_ASSERT_EQ(test,
					vfio_fragment_map_area(f, ctx, 0, 0, SZ_4K, rw), 0);
				for (unsigned long offset = SZ_4K; offset < PAGE_SIZE;) {
					unsigned long length = fine ? SZ_4K : remaining;

					KUNIT_ASSERT_EQ(test, vfio_fragment_map_area(f, ctx, offset,
						offset, length, rw), 0);
					offset += length;
				}
				if (cycle == VFIO_STRESS_CYCLES - 1) {
					struct page *page;

					KUNIT_ASSERT_EQ(test, vfio_iommu_type1_pin_pages(f->iommu,
						f->group.iommu_group, VFIO_TEST_IOVA, 1,
						IOMMU_READ, &page), 1);
					f->external_refs = 1;
					ctx->live = false;
					mmput(ctx->mm);
					KUNIT_ASSERT_EQ(test, atomic_read(&ctx->mm->mm_users), 0);
				}
				s->competing = cycle & 2;
				complete(&s->pin_go);
				KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&s->pin_ready,
					msecs_to_jiffies(5000)), 0UL);
				KUNIT_ASSERT_EQ(test, atomic_read(&s->error), 0);
				KUNIT_ASSERT_EQ(test,
					vfio_dma_do_unmap(f->iommu, &unmap, &bitmap), 0);
				KUNIT_EXPECT_EQ(test, unmap.size, s->competing ? 0UL : SZ_4K);
				if (s->competing) {
					KUNIT_EXPECT_EQ(test, s->competing_ret, 0);
					KUNIT_EXPECT_EQ(test, s->competing_size, SZ_4K);
				}
				unmap.iova += SZ_4K;
				unmap.size = remaining;
				KUNIT_ASSERT_EQ(test,
					vfio_dma_do_unmap(f->iommu, &unmap, &bitmap), 0);
				KUNIT_EXPECT_EQ(test, unmap.size, remaining);
				KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&s->pin_done,
					msecs_to_jiffies(5000)), 0UL);
				KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&s->register_done,
					msecs_to_jiffies(5000)), 0UL);
				KUNIT_ASSERT_EQ(test, atomic_read(&s->error), 0);
				KUNIT_EXPECT_EQ(test, f->external_refs, 0U);
				KUNIT_EXPECT_EQ(test, mm_locked_vm_bytes(ctx->mm), 0UL);
				KUNIT_EXPECT_TRUE(test, RB_EMPTY_ROOT(&f->iommu->dma_list));
				KUNIT_EXPECT_EQ(test, f->domains[0].mapped, 0UL);
				reinit_completion(&s->pin_ready);
				reinit_completion(&s->notify_entered);
				reinit_completion(&s->pin_done);
				reinit_completion(&s->register_entered);
				reinit_completion(&s->register_done);
				reinit_completion(&s->competing_done);
			}
			KUNIT_EXPECT_EQ(test, s->pins, 2U * VFIO_STRESS_CYCLES);
			KUNIT_EXPECT_EQ(test, s->registrations, (unsigned int)VFIO_STRESS_CYCLES);
			KUNIT_EXPECT_EQ(test, f->notifications, (unsigned int)VFIO_STRESS_CYCLES);
			KUNIT_EXPECT_EQ(test, s->observations, (unsigned int)VFIO_STRESS_CYCLES);
			KUNIT_EXPECT_EQ(test, s->competing_unmaps, VFIO_STRESS_CYCLES / 2U);
			kunit_info(test, "%uK user/%luK native mapped=%u: %u forced pin/unmap/register cycles, half with competing unmap",
				1U << (shifts[shift] - 10), PAGE_SIZE >> 10,
				mapped, VFIO_STRESS_CYCLES);
			kunit_release_action(test, vfio_stress_free, s);
			kunit_release_action(test, vfio_fragment_fixture_free, f);
		}
	}
}

static struct kunit_case vfio_fragments_cases[] = {
	KUNIT_CASE(vfio_fragments_lifetime_test),
	KUNIT_CASE(vfio_fragments_lazy_test),
	KUNIT_CASE(vfio_fragments_map_failure_test),
	KUNIT_CASE(vfio_fragments_fine_test),
	KUNIT_CASE(vfio_fragments_replay_failure_test),
	KUNIT_CASE(vfio_fragments_anon_owner_test),
	KUNIT_CASE(vfio_fragments_domain_granule_test),
	KUNIT_CASE(vfio_fragments_native_owner_test),
	KUNIT_CASE(vfio_fragments_external_span_test),
	KUNIT_CASE(vfio_fragments_concurrency_test),
	{}
};

static struct kunit_suite vfio_fragments_suite = {
	.name = "vfio-type1-fragments",
	.test_cases = vfio_fragments_cases,
};

kunit_test_suite(vfio_fragments_suite);
