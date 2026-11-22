// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Included at the end of iomfb_template.c, once per template instance: runs
 * the buffer callbacks of that firmware build against a fake device and a
 * fake PIODMA domain.
 */
#include <kunit/device.h>
#include <kunit/test.h>

struct tpl_mem_test {
	struct apple_dcp *dcp;
	struct iommu_domain domain;
	size_t mapped;
	unsigned int map_calls;
	unsigned int unmap_calls;
	bool fail_unmap;
};

#ifdef CONFIG_IOMMU_API
static struct tpl_mem_test *tpl_mem_from_domain(struct iommu_domain *domain)
{
	return container_of(domain, struct tpl_mem_test, domain);
}

static int tpl_mem_map_pages(struct iommu_domain *domain, unsigned long iova,
			     phys_addr_t paddr, size_t pgsize, size_t pgcount,
			     int prot, gfp_t gfp, size_t *mapped)
{
	struct tpl_mem_test *t = tpl_mem_from_domain(domain);

	t->map_calls++;
	t->mapped += pgsize * pgcount;
	*mapped = pgsize * pgcount;
	return 0;
}

static size_t tpl_mem_unmap_pages(struct iommu_domain *domain,
				  unsigned long iova, size_t pgsize,
				  size_t pgcount,
				  struct iommu_iotlb_gather *gather)
{
	struct tpl_mem_test *t = tpl_mem_from_domain(domain);
	size_t size = min(pgsize * pgcount, t->mapped);

	t->unmap_calls++;
	if (t->fail_unmap)
		return 0;
	/* Like a page table: nothing to unmap where nothing is mapped. */
	t->mapped -= size;
	return size;
}

static const struct iommu_domain_ops tpl_mem_domain_ops = {
	.map_pages = tpl_mem_map_pages,
	.unmap_pages = tpl_mem_unmap_pages,
};
#endif

static int tpl_mem_test_init(struct kunit *test)
{
	struct tpl_mem_test *t;
	struct apple_dcp *dcp;
	int ret;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	if (!t || !dcp)
		return -ENOMEM;
	dcp->dev = kunit_device_register(test, "iomfb-mem-" KBUILD_BASENAME);
	if (IS_ERR(dcp->dev))
		return PTR_ERR(dcp->dev);
	ret = dma_coerce_mask_and_coherent(dcp->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

#ifdef CONFIG_IOMMU_API
	t->domain.type = IOMMU_DOMAIN_UNMANAGED;
	t->domain.pgsize_bitmap = PAGE_SIZE;
	t->domain.ops = &tpl_mem_domain_ops;
	dcp->iommu_dom = &t->domain;
#endif
	/* As at bind: descriptor 0 is never handed out. */
	set_bit(0, dcp->memdesc_map);
	t->dcp = dcp;
	test->priv = t;
	return 0;
}

static void tpl_mem_test_exit(struct kunit *test)
{
	struct tpl_mem_test *t = test->priv;
	u32 id;

	if (!t)
		return;
	t->fail_unmap = false;
	for (id = 1; id < DCP_MAX_MAPPINGS; id++)
		if (test_bit(id, t->dcp->memdesc_map) && t->dcp->memdesc[id].buf)
			dcpep_cb_release_mem_desc(t->dcp, &id);
}

#ifdef CONFIG_IOMMU_API
static u32 tpl_mem_alloc(struct kunit *test, u64 size)
{
	struct tpl_mem_test *t = test->priv;
	struct dcp_allocate_buffer_req req = { .size = size };
	struct dcp_allocate_buffer_resp resp;

	resp = dcpep_cb_allocate_buffer(t->dcp, &req);
	KUNIT_ASSERT_NE(test, resp.mem_desc_id, 0);
	KUNIT_ASSERT_TRUE(test, test_bit(resp.mem_desc_id, t->dcp->memdesc_map));
	return resp.mem_desc_id;
}

static bool tpl_mem_map(struct kunit *test, u32 id)
{
	struct tpl_mem_test *t = test->priv;
	struct dcp_map_buf_req req = { .buffer = id };
	struct DCP_FW_NAME(dcp_map_buf_resp) resp = { 0 };

	return dcpep_map_piodma(t->dcp, &req, &resp);
}

static bool tpl_mem_unmap(struct kunit *test, u32 id)
{
	struct tpl_mem_test *t = test->priv;
	struct dcp_unmap_buf_resp req = {
		.buffer = id,
		.dva = t->dcp->memdesc[id].dva,
	};

	return dcpep_unmap_piodma(t->dcp, &req);
}

/* A descriptor released while still mapped for PIODMA loses that mapping. */
static void tpl_mem_release_unmaps_piodma_test(struct kunit *test)
{
	struct tpl_mem_test *t = test->priv;
	u32 id;

	id = tpl_mem_alloc(test, SZ_16K);
	KUNIT_ASSERT_TRUE(test, tpl_mem_map(test, id));
	KUNIT_ASSERT_EQ(test, t->mapped, (size_t)SZ_16K);

	KUNIT_EXPECT_EQ(test, dcpep_cb_release_mem_desc(t->dcp, &id), 1);
	KUNIT_EXPECT_EQ(test, t->mapped, 0);
	KUNIT_EXPECT_FALSE(test, test_bit(id, t->dcp->memdesc_map));
}

/* An unmap without a map never reaches the domain. */
static void tpl_mem_unmap_needs_map_test(struct kunit *test)
{
	struct tpl_mem_test *t = test->priv;
	u32 id;

	id = tpl_mem_alloc(test, SZ_16K);
	KUNIT_EXPECT_FALSE(test, tpl_mem_unmap(test, id));
	KUNIT_EXPECT_EQ(test, t->unmap_calls, 0);

	/* A mapped buffer unmaps once; the second unmap is refused. */
	KUNIT_ASSERT_TRUE(test, tpl_mem_map(test, id));
	KUNIT_EXPECT_TRUE(test, tpl_mem_unmap(test, id));
	KUNIT_EXPECT_EQ(test, t->mapped, 0);
	t->unmap_calls = 0;
	KUNIT_EXPECT_FALSE(test, tpl_mem_unmap(test, id));
	KUNIT_EXPECT_EQ(test, t->unmap_calls, 0);
}

/* A buffer that is mapped already is not mapped a second time. */
static void tpl_mem_second_map_refused_test(struct kunit *test)
{
	struct tpl_mem_test *t = test->priv;
	unsigned int calls;
	u32 id;

	id = tpl_mem_alloc(test, SZ_16K);
	KUNIT_ASSERT_TRUE(test, tpl_mem_map(test, id));
	calls = t->map_calls;
	KUNIT_EXPECT_FALSE(test, tpl_mem_map(test, id));
	KUNIT_EXPECT_EQ(test, t->map_calls, calls);
	KUNIT_EXPECT_EQ(test, t->mapped, (size_t)SZ_16K);
}

/* A buffer whose PIODMA mapping could not be removed is not freed. */
static void tpl_mem_failed_unmap_keeps_buffer_test(struct kunit *test)
{
	struct tpl_mem_test *t = test->priv;
	u32 id;

	id = tpl_mem_alloc(test, SZ_16K);
	KUNIT_ASSERT_TRUE(test, tpl_mem_map(test, id));

	t->fail_unmap = true;
	KUNIT_EXPECT_EQ(test, dcpep_cb_release_mem_desc(t->dcp, &id), 0);
	KUNIT_EXPECT_TRUE(test, test_bit(id, t->dcp->memdesc_map));
	KUNIT_EXPECT_NOT_NULL(test, t->dcp->memdesc[id].buf);
	KUNIT_EXPECT_TRUE(test, t->dcp->memdesc[id].piodma_mapped);

	/* Once the unmap works, the release goes through. */
	t->fail_unmap = false;
	KUNIT_EXPECT_EQ(test, dcpep_cb_release_mem_desc(t->dcp, &id), 1);
	KUNIT_EXPECT_EQ(test, t->mapped, 0);
}

#endif /* CONFIG_IOMMU_API */

static struct kunit_case tpl_mem_cases[] = {
#ifdef CONFIG_IOMMU_API
	KUNIT_CASE(tpl_mem_release_unmaps_piodma_test),
	KUNIT_CASE(tpl_mem_unmap_needs_map_test),
	KUNIT_CASE(tpl_mem_second_map_refused_test),
	KUNIT_CASE(tpl_mem_failed_unmap_keeps_buffer_test),
#endif
	{}
};

static struct kunit_suite tpl_mem_suite = {
	.name = "apple-iomfb-mem-" KBUILD_BASENAME,
	.init = tpl_mem_test_init,
	.exit = tpl_mem_test_exit,
	.test_cases = tpl_mem_cases,
};

kunit_test_suite(tpl_mem_suite);
