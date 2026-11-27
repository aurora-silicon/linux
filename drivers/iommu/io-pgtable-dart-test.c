// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the Apple DART page table formats.
 *
 * Included by io-pgtable-dart.c so that the table walkers can be checked
 * against hand-computed table indexes and PTE encodings.
 */
#include <kunit/device.h>
#include <kunit/resource.h>
#include <kunit/test.h>
#include <linux/iommu.h>
#include <linux/mmzone.h>
#include <linux/percpu.h>

/* Leaf subpage protection: start 0, end 0xfff (whole page). */
#define DART_TEST_SUBPAGE_ALL	(0xfffULL << 40)

struct dart_test {
	struct device *dev;
	struct io_pgtable_cfg cfg;
	struct io_pgtable_ops *ops;
	unsigned int tlb_add_page;
	unsigned int tlb_other;
	unsigned long tlb_last_iova;
	size_t tlb_last_granule;
};

static void dart_test_tlb_flush_all(void *cookie)
{
	struct dart_test *t = cookie;

	t->tlb_other++;
}

static void dart_test_tlb_flush_walk(unsigned long iova, size_t size,
				     size_t granule, void *cookie)
{
	struct dart_test *t = cookie;

	t->tlb_other++;
}

static void dart_test_tlb_add_page(struct iommu_iotlb_gather *gather,
				   unsigned long iova, size_t granule,
				   void *cookie)
{
	struct dart_test *t = cookie;

	t->tlb_add_page++;
	t->tlb_last_iova = iova;
	t->tlb_last_granule = granule;
}

static const struct iommu_flush_ops dart_test_flush_ops = {
	.tlb_flush_all	= dart_test_tlb_flush_all,
	.tlb_flush_walk	= dart_test_tlb_flush_walk,
	.tlb_add_page	= dart_test_tlb_add_page,
};

KUNIT_DEFINE_ACTION_WRAPPER(dart_test_free_ops, free_io_pgtable_ops,
			    struct io_pgtable_ops *);

/* Exact count of pages held by iommu_alloc_pages_sz(), including per-CPU deltas. */
static long dart_test_iommu_pages(void)
{
	struct pglist_data *pgdat;
	long pages = 0;
	int cpu;

	for_each_online_pgdat(pgdat) {
		pages += atomic_long_read(&pgdat->vm_stat[NR_IOMMU_PAGES]);
		for_each_possible_cpu(cpu)
			pages += per_cpu_ptr(pgdat->per_cpu_nodestats,
					     cpu)->vm_node_stat_diff[NR_IOMMU_PAGES];
	}

	return pages;
}

/* Pages iommu_alloc_pages_sz() accounts for one table of a granule (4 for 16K on a 4K kernel). */
static long dart_test_tbl_pages(size_t granule)
{
	return 1L << get_order(granule);
}

static int dart_test_init(struct kunit *test)
{
	struct dart_test *t;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	t->dev = kunit_device_register(test, "io-pgtable-dart-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, t->dev);
	test->priv = t;

	return 0;
}

static void dart_test_cfg(struct dart_test *t, size_t pgsize, u32 ias, u32 oas)
{
	t->cfg = (struct io_pgtable_cfg) {
		.pgsize_bitmap	= pgsize,
		.ias		= ias,
		.oas		= oas,
		.coherent_walk	= true,
		.tlb		= &dart_test_flush_ops,
		.iommu_dev	= t->dev,
	};
}

static struct dart_io_pgtable *dart_test_alloc(struct kunit *test,
					       enum io_pgtable_fmt fmt,
					       size_t pgsize, u32 ias, u32 oas)
{
	struct dart_test *t = test->priv;

	dart_test_cfg(t, pgsize, ias, oas);
	t->ops = alloc_io_pgtable_ops(fmt, &t->cfg, t);
	KUNIT_ASSERT_NOT_NULL(test, t->ops);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, dart_test_free_ops,
							t->ops), 0);

	return io_pgtable_ops_to_data(t->ops);
}

static int dart_test_map(struct dart_test *t, unsigned long iova,
			 phys_addr_t paddr, size_t pgcount, int prot,
			 size_t *mapped)
{
	return t->ops->map_pages(t->ops, iova, paddr, t->cfg.pgsize_bitmap,
				 pgcount, prot, GFP_KERNEL, mapped);
}

static size_t dart_test_unmap(struct dart_test *t, unsigned long iova,
			      size_t pgcount, struct iommu_iotlb_gather *gather)
{
	return t->ops->unmap_pages(t->ops, iova, t->cfg.pgsize_bitmap, pgcount,
				   gather);
}

static phys_addr_t dart_test_phys(struct dart_test *t, unsigned long iova)
{
	return t->ops->iova_to_phys(t->ops, iova);
}

/* Leaf PTE slot found by the driver's own walker. */
static dart_iopte *dart_test_leaf(struct dart_io_pgtable *data,
				  unsigned long iova)
{
	dart_iopte *ptep = dart_get_last(data, iova);

	return ptep ? ptep + dart_get_last_index(data, iova) : NULL;
}

/* Expected table PTE for a next-level table, encoded by hand. */
static dart_iopte dart_test_table_pte(enum io_pgtable_fmt fmt, void *table)
{
	u64 pa = __pa(table);

	if (fmt == APPLE_DART)
		return (pa & GENMASK_ULL(35, 12)) | 1;
	return ((pa >> 4) & GENMASK_ULL(37, 10)) | 1;
}

/* Decode a table PTE by hand. */
static dart_iopte *dart_test_next(enum io_pgtable_fmt fmt, dart_iopte pte)
{
	if (fmt == APPLE_DART)
		return __va(pte & GENMASK_ULL(35, 12));
	return __va((pte & GENMASK_ULL(37, 10)) << 4);
}

struct dart_test_layout {
	const char *name;
	enum io_pgtable_fmt fmt;
	size_t pgsize;
	u32 ias, oas;
	u32 n_ttbrs, n_levels;
	int bits_per_level;
};

static const struct dart_test_layout dart_test_layouts[] = {
	{ "dart1-16k-ias32", APPLE_DART, SZ_16K, 32, 36, 1, 3, 11 },
	{ "dart1-4k-ias32", APPLE_DART, SZ_4K, 32, 36, 4, 3, 9 },
	{ "dart1-4k-ias30", APPLE_DART, SZ_4K, 30, 36, 1, 3, 9 },
	{ "dart1-4k-ias36", APPLE_DART, SZ_4K, 36, 36, 1, 4, 9 },
	{ "dart1-16k-ias36", APPLE_DART, SZ_16K, 36, 36, 1, 3, 11 },
	{ "dart2-16k-ias36", APPLE_DART2, SZ_16K, 36, 42, 1, 3, 11 },
	{ "dart2-16k-ias37", APPLE_DART2, SZ_16K, 37, 42, 1, 4, 11 },
	{ "dart2-16k-ias42", APPLE_DART2, SZ_16K, 42, 42, 1, 4, 11 },
};

static void dart_test_layout_desc(const struct dart_test_layout *l, char *desc)
{
	strscpy(desc, l->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(dart_test_layout, dart_test_layouts, dart_test_layout_desc);

/* Each supported format/granule/ias picks the expected TTBR count and depth. */
static void dart_test_alloc_layout(struct kunit *test)
{
	const struct dart_test_layout *l = test->param_value;
	struct dart_test *t = test->priv;
	struct dart_io_pgtable *data;
	u32 i;

	data = dart_test_alloc(test, l->fmt, l->pgsize, l->ias, l->oas);
	KUNIT_EXPECT_EQ(test, data->iop.fmt, l->fmt);
	KUNIT_EXPECT_EQ(test, t->cfg.apple_dart_cfg.n_ttbrs, l->n_ttbrs);
	KUNIT_EXPECT_EQ(test, t->cfg.apple_dart_cfg.n_levels, l->n_levels);
	KUNIT_EXPECT_EQ(test, data->bits_per_level, l->bits_per_level);
	KUNIT_EXPECT_EQ(test, DART_GRANULE(data), l->pgsize);

	for (i = 0; i < DART_MAX_TABLES; i++) {
		if (i >= l->n_ttbrs) {
			KUNIT_EXPECT_NULL(test, data->pgd[i]);
			continue;
		}
		KUNIT_ASSERT_NOT_NULL(test, data->pgd[i]);
		KUNIT_EXPECT_PTR_EQ(test, t->cfg.apple_dart_cfg.ttbr[i], data->pgd[i]);
		KUNIT_EXPECT_TRUE(test, IS_ALIGNED(__pa(data->pgd[i]), l->pgsize));
		KUNIT_EXPECT_TRUE(test, mem_is_zero(data->pgd[i], l->pgsize));
	}
	KUNIT_EXPECT_EQ(test, t->tlb_other, 0U);
}

struct dart_test_bad_cfg {
	const char *name;
	enum io_pgtable_fmt fmt;
	size_t pgsize;
	u32 ias, oas;
	bool noncoherent;
};

static const struct dart_test_bad_cfg dart_test_bad_cfgs[] = {
	{ "noncoherent", APPLE_DART2, SZ_16K, 36, 42, true },
	{ "oas40", APPLE_DART2, SZ_16K, 36, 40 },
	{ "oas48", APPLE_DART2, SZ_16K, 36, 48 },
	{ "ias-gt-oas", APPLE_DART, SZ_16K, 37, 36 },
	{ "pgsize-64k", APPLE_DART2, SZ_64K, 36, 42 },
	{ "pgsize-8k", APPLE_DART, SZ_8K, 32, 36 },
	{ "pgsize-bitmap", APPLE_DART, SZ_4K | SZ_16K, 32, 36 },
	{ "4k-ias42-too-deep", APPLE_DART2, SZ_4K, 42, 42 },
};

static void dart_test_bad_cfg_desc(const struct dart_test_bad_cfg *b, char *desc)
{
	strscpy(desc, b->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(dart_test_bad_cfg, dart_test_bad_cfgs, dart_test_bad_cfg_desc);

/* Unsupported walkers, widths and granules are refused without leaking pages. */
static void dart_test_alloc_reject(struct kunit *test)
{
	const struct dart_test_bad_cfg *b = test->param_value;
	struct dart_test *t = test->priv;
	long before = dart_test_iommu_pages();
	struct io_pgtable_ops *ops;

	dart_test_cfg(t, b->pgsize, b->ias, b->oas);
	t->cfg.coherent_walk = !b->noncoherent;
	ops = alloc_io_pgtable_ops(b->fmt, &t->cfg, t);
	KUNIT_EXPECT_NULL(test, ops);
	if (ops)
		free_io_pgtable_ops(ops);
	KUNIT_EXPECT_EQ(test, dart_test_iommu_pages(), before);
}

/* DART1 leaf PTEs: OA in bits 35:12, SP_DIS, NO_WRITE(7), NO_READ(8). */
static void dart_test_pte_dart1(struct kunit *test)
{
	static const struct {
		int prot;
		dart_iopte bits;
	} cases[] = {
		{ IOMMU_READ | IOMMU_WRITE, 0x003 },
		{ IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, 0x003 },
		{ IOMMU_READ, 0x083 },
		{ IOMMU_WRITE, 0x103 },
	};
	const phys_addr_t pa = 0x81234c000ULL;
	struct dart_test *t = test->priv;
	struct dart_io_pgtable *data;
	unsigned long iova;
	dart_iopte *ptep;
	int i;

	data = dart_test_alloc(test, APPLE_DART, SZ_16K, 32, 36);
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		iova = SZ_1M + i * SZ_16K;
		KUNIT_ASSERT_EQ(test, dart_test_map(t, iova, pa, 1, cases[i].prot, NULL), 0);
		ptep = dart_test_leaf(data, iova);
		KUNIT_ASSERT_NOT_NULL(test, ptep);
		KUNIT_EXPECT_EQ(test, *ptep, DART_TEST_SUBPAGE_ALL | pa | cases[i].bits);
		KUNIT_EXPECT_EQ(test, dart_test_phys(t, iova + 0x123), pa + 0x123);
	}

	/* 4 KiB granule DART1: same encoding, 4 KiB aligned output address. */
	data = dart_test_alloc(test, APPLE_DART, SZ_4K, 32, 36);
	KUNIT_ASSERT_EQ(test, dart_test_map(t, 0x3000, 0x876543000ULL, 1,
					    IOMMU_READ | IOMMU_WRITE, NULL), 0);
	ptep = dart_test_leaf(data, 0x3000);
	KUNIT_ASSERT_NOT_NULL(test, ptep);
	KUNIT_EXPECT_EQ(test, *ptep, 0x000fff0876543003ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, 0x3fff), 0x876543fffULL);
}

/* DART2 leaf PTEs: OA >> 4 in bits 37:10, NO_CACHE(1), NO_WRITE(2), NO_READ(3). */
static void dart_test_pte_dart2(struct kunit *test)
{
	static const struct {
		int prot;
		dart_iopte bits;
	} cases[] = {
		{ IOMMU_READ | IOMMU_WRITE, 0x3 },
		{ IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, 0x1 },
		{ IOMMU_READ | IOMMU_CACHE, 0x5 },
		{ IOMMU_WRITE | IOMMU_CACHE, 0x9 },
		{ IOMMU_READ, 0x7 },
	};
	const phys_addr_t pa = 0x2abcdef4000ULL;
	struct dart_test *t = test->priv;
	struct dart_io_pgtable *data;
	unsigned long iova;
	dart_iopte *ptep;
	int i;

	data = dart_test_alloc(test, APPLE_DART2, SZ_16K, 36, 42);
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		iova = SZ_1M + i * SZ_16K;
		KUNIT_ASSERT_EQ(test, dart_test_map(t, iova, pa, 1, cases[i].prot, NULL), 0);
		ptep = dart_test_leaf(data, iova);
		KUNIT_ASSERT_NOT_NULL(test, ptep);
		KUNIT_EXPECT_EQ(test, *ptep, 0x000fff2abcdef400ULL | cases[i].bits);
		KUNIT_EXPECT_EQ(test, dart_test_phys(t, iova + 0x3ff8), pa + 0x3ff8);
	}

	/* The top of the 42-bit output space round-trips (bits 41:40 kept). */
	iova = SZ_2M;
	KUNIT_ASSERT_EQ(test, dart_test_map(t, iova, 0x3ffffffc000ULL, 1,
					    IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, NULL), 0);
	ptep = dart_test_leaf(data, iova);
	KUNIT_ASSERT_NOT_NULL(test, ptep);
	KUNIT_EXPECT_EQ(test, *ptep, 0x000fff3ffffffc01ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, iova + 1), 0x3ffffffc001ULL);
}

/* Table PTEs carry only the next table's address and the valid bit. */
static void dart_test_table_ptes(struct kunit *test)
{
	static const enum io_pgtable_fmt fmts[] = { APPLE_DART, APPLE_DART2 };
	struct dart_test *t = test->priv;
	struct dart_io_pgtable *data;
	dart_iopte *leaf, *l1;
	int i;

	for (i = 0; i < ARRAY_SIZE(fmts); i++) {
		data = dart_test_alloc(test, fmts[i], SZ_16K, 32, fmts[i] == APPLE_DART ? 36 : 42);
		/* L1 index 3 (iova >> 25), leaf index 5 (iova >> 14). */
		KUNIT_ASSERT_EQ(test, dart_test_map(t, 0x6014000, SZ_1G, 1, IOMMU_READ, NULL), 0);
		l1 = data->pgd[0];
		leaf = dart_test_next(fmts[i], l1[3]);
		KUNIT_ASSERT_NE(test, l1[3], 0ULL);
		KUNIT_EXPECT_PTR_EQ(test, dart_test_leaf(data, 0x6014000), leaf + 5);
		KUNIT_EXPECT_EQ(test, l1[3], dart_test_table_pte(fmts[i], leaf));
		KUNIT_EXPECT_EQ(test, l1[2], 0ULL);
		KUNIT_EXPECT_EQ(test, l1[4], 0ULL);
		KUNIT_EXPECT_NE(test, leaf[5], 0ULL);
		KUNIT_EXPECT_EQ(test, leaf[4], 0ULL);
		KUNIT_EXPECT_EQ(test, leaf[6], 0ULL);
	}
}

/* High L1 indexes on a 36-bit DART2 select their own tables (no aliasing). */
static void dart_test_index_high_l1(struct kunit *test)
{
	const unsigned long hi = 0x802004000UL;	/* L1 1025, leaf 1 */
	struct dart_test *t = test->priv;
	struct dart_io_pgtable *data;
	dart_iopte *l1, *leaf;

	data = dart_test_alloc(test, APPLE_DART2, SZ_16K, 36, 42);
	KUNIT_ASSERT_EQ(test, dart_test_map(t, hi, 0x10000000, 1, IOMMU_READ, NULL), 0);
	l1 = data->pgd[0];
	KUNIT_ASSERT_NE(test, l1[1025], 0ULL);
	KUNIT_EXPECT_EQ(test, l1[1], 0ULL);
	leaf = dart_test_next(APPLE_DART2, l1[1025]);
	KUNIT_EXPECT_EQ(test, leaf[1], 0x000fff0001000007ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, hi), 0x10000000ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, 0x2004000), 0ULL);

	/* The low alias is a different IOVA and maps independently. */
	KUNIT_ASSERT_EQ(test, dart_test_map(t, 0x2004000, 0x20000000, 1, IOMMU_READ, NULL), 0);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, 0x2004000), 0x20000000ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, hi), 0x10000000ULL);
	KUNIT_EXPECT_EQ(test, dart_test_unmap(t, hi, 1, NULL), (size_t)SZ_16K);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, 0x2004000), 0x20000000ULL);
}

/* 4-level DART2 (ias 42): three table levels below the TTBR. */
static void dart_test_four_level(struct kunit *test)
{
	const unsigned long top = 0x3ffffffc000UL;	/* L1 63, L2 2047, L3 2047 */
	struct dart_test *t = test->priv;
	struct dart_io_pgtable *data;
	dart_iopte *l1, *l2, *l3;

	data = dart_test_alloc(test, APPLE_DART2, SZ_16K, 42, 42);
	KUNIT_ASSERT_EQ(test, t->cfg.apple_dart_cfg.n_levels, 4U);
	KUNIT_ASSERT_EQ(test, dart_test_map(t, top, 0x4000, 1, IOMMU_READ | IOMMU_WRITE, NULL), 0);
	KUNIT_ASSERT_EQ(test, dart_test_map(t, 0, 0x8000, 1, IOMMU_READ | IOMMU_WRITE, NULL), 0);

	/* L1 = iova >> 36, L2 = (iova >> 25) & 2047, L3 = (iova >> 14) & 2047. */
	l1 = data->pgd[0];
	KUNIT_ASSERT_NE(test, l1[63], 0ULL);
	KUNIT_ASSERT_NE(test, l1[0], 0ULL);
	l2 = dart_test_next(APPLE_DART2, l1[63]);
	KUNIT_ASSERT_NE(test, l2[2047], 0ULL);
	KUNIT_EXPECT_EQ(test, l1[63], dart_test_table_pte(APPLE_DART2, l2));
	l3 = dart_test_next(APPLE_DART2, l2[2047]);
	KUNIT_EXPECT_EQ(test, l3[2047], 0x000fff0000000403ULL);
	KUNIT_EXPECT_PTR_EQ(test, dart_test_leaf(data, top), l3 + 2047);

	KUNIT_EXPECT_EQ(test, dart_test_phys(t, top + 0x10), 0x4010ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, 0x10), 0x8010ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, top - SZ_16K), 0ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, BIT(36)), 0ULL);
	/* Beyond the single TTBR (iova >> 47 != 0). */
	KUNIT_EXPECT_EQ(test, dart_test_map(t, BIT(47), 0x4000, 1, IOMMU_READ, NULL), -ENOMEM);
}

/* 4 KiB DART1 with ias 32 splits the space over four TTBRs at 1 GiB. */
static void dart_test_four_ttbrs(struct kunit *test)
{
	const unsigned long iova = 0xc0203000UL;	/* TTBR 3, L1 1, leaf 3 */
	struct dart_test *t = test->priv;
	struct dart_io_pgtable *data;
	dart_iopte *leaf;
	size_t mapped = 0;
	int i;

	data = dart_test_alloc(test, APPLE_DART, SZ_4K, 32, 36);
	KUNIT_ASSERT_EQ(test, dart_test_map(t, iova, 0x87654000, 1, IOMMU_READ | IOMMU_WRITE,
					    &mapped), 0);
	KUNIT_EXPECT_EQ(test, mapped, (size_t)SZ_4K);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_TRUE(test, mem_is_zero(data->pgd[i], SZ_4K));
	KUNIT_ASSERT_NE(test, ((dart_iopte *)data->pgd[3])[1], 0ULL);
	KUNIT_EXPECT_EQ(test, ((dart_iopte *)data->pgd[3])[0], 0ULL);
	leaf = dart_test_next(APPLE_DART, ((dart_iopte *)data->pgd[3])[1]);
	KUNIT_EXPECT_EQ(test, leaf[3], 0x000fff0087654003ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, iova + 0xabc), 0x87654abcULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, iova - SZ_1G), 0ULL);

	/* The last page of TTBR 0 and the first page of TTBR 1. */
	KUNIT_ASSERT_EQ(test, dart_test_map(t, SZ_1G - SZ_4K, 0x1000, 1, IOMMU_READ, NULL), 0);
	KUNIT_ASSERT_EQ(test, dart_test_map(t, SZ_1G, 0x2000, 1, IOMMU_READ, NULL), 0);
	KUNIT_EXPECT_NE(test, ((dart_iopte *)data->pgd[0])[511], 0ULL);
	KUNIT_EXPECT_NE(test, ((dart_iopte *)data->pgd[1])[0], 0ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_1G - 1), 0x1fffULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_1G), 0x2000ULL);

	/* IOVA 4 GiB would need a fifth TTBR. */
	mapped = 0;
	KUNIT_EXPECT_EQ(test, dart_test_map(t, BIT_ULL(32), 0x1000, 1, IOMMU_READ, &mapped),
			-ENOMEM);
	KUNIT_EXPECT_EQ(test, mapped, (size_t)0);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, BIT_ULL(32)), 0ULL);
}

/* pgcount mappings fill consecutive PTEs and stop at the leaf table's end. */
static void dart_test_map_multi(struct kunit *test)
{
	const unsigned long start = SZ_32M - 2 * SZ_16K;	/* leaf 2046 of L1 0 */
	const phys_addr_t pa = 0x80000000;
	struct dart_test *t = test->priv;
	struct dart_io_pgtable *data;
	size_t mapped = SZ_1M, left = 5;
	unsigned long iova = start;
	int calls = 0, i;
	dart_iopte *ptep;

	data = dart_test_alloc(test, APPLE_DART2, SZ_16K, 36, 42);

	/* *mapped accumulates; the first call stops after 2 pages. */
	KUNIT_ASSERT_EQ(test, dart_test_map(t, iova, pa, left, IOMMU_READ | IOMMU_WRITE,
					    &mapped), 0);
	KUNIT_EXPECT_EQ(test, mapped, (size_t)SZ_1M + 2 * SZ_16K);

	/* Continue as iommu_map() does until all pages are in. */
	mapped -= SZ_1M;
	while (left) {
		calls++;
		left -= mapped / SZ_16K;
		iova += mapped;
		mapped = 0;
		if (!left)
			break;
		KUNIT_ASSERT_EQ(test, dart_test_map(t, iova, pa + (iova - start), left,
						    IOMMU_READ | IOMMU_WRITE, &mapped), 0);
		KUNIT_ASSERT_GT(test, mapped, (size_t)0);
	}
	KUNIT_EXPECT_EQ(test, calls, 2);
	KUNIT_EXPECT_NE(test, ((dart_iopte *)data->pgd[0])[0], 0ULL);
	KUNIT_EXPECT_NE(test, ((dart_iopte *)data->pgd[0])[1], 0ULL);

	for (i = 0; i < 5; i++) {
		ptep = dart_test_leaf(data, start + i * SZ_16K);
		KUNIT_ASSERT_NOT_NULL(test, ptep);
		KUNIT_EXPECT_EQ(test, *ptep, 0x000fff0008000003ULL + i * (SZ_16K >> 4));
		KUNIT_EXPECT_EQ(test, dart_test_phys(t, start + i * SZ_16K + 8),
				pa + i * SZ_16K + 8);
	}
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, start - SZ_16K), 0ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, start + 5 * SZ_16K), 0ULL);

	/* A large run in an empty leaf table fills all 2048 slots at once. */
	mapped = 0;
	KUNIT_ASSERT_EQ(test, dart_test_map(t, SZ_64M, 0, 4096, IOMMU_READ, &mapped), 0);
	KUNIT_EXPECT_EQ(test, mapped, (size_t)SZ_32M);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_64M + SZ_32M - 1), (phys_addr_t)SZ_32M - 1);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_64M + SZ_32M), 0ULL);
}

/* Mapping over a valid PTE fails without touching any slot of the range. */
static void dart_test_map_exists(struct kunit *test)
{
	struct dart_test *t = test->priv;
	struct dart_io_pgtable *data;
	dart_iopte old, *ptep;
	size_t mapped = 0;
	int ret, i;

	data = dart_test_alloc(test, APPLE_DART2, SZ_16K, 36, 42);
	KUNIT_ASSERT_EQ(test, dart_test_map(t, 0x10008000, 0x40000000, 1, IOMMU_READ, NULL), 0);
	ptep = dart_test_leaf(data, 0x10008000);
	KUNIT_ASSERT_NOT_NULL(test, ptep);
	old = *ptep;

	kunit_warning_suppress(test) {
		ret = dart_test_map(t, 0x10008000, 0x50000000, 1, IOMMU_READ | IOMMU_WRITE,
				    &mapped);
		KUNIT_EXPECT_EQ(test, ret, -EEXIST);
		/* Slot 2 of [0, 4) is taken: nothing of the range is written. */
		ret = dart_test_map(t, 0x10000000, 0x60000000, 4, IOMMU_READ, &mapped);
		KUNIT_EXPECT_EQ(test, ret, -EEXIST);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 2);
	}
	KUNIT_EXPECT_EQ(test, mapped, (size_t)0);
	KUNIT_EXPECT_EQ(test, *ptep, old);
	for (i = 0; i < 4; i++)
		if (i != 2)
			KUNIT_EXPECT_EQ(test, *(ptep - 2 + i), 0ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, 0x10008000), 0x40000000ULL);

	/* After an unmap the same IOVA maps again. */
	KUNIT_EXPECT_EQ(test, dart_test_unmap(t, 0x10008000, 1, NULL), (size_t)SZ_16K);
	KUNIT_EXPECT_EQ(test, dart_test_map(t, 0x10008000, 0x50000000, 1, IOMMU_WRITE, NULL), 0);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, 0x10008000), 0x50000000ULL);
}

/* Bad sizes, protections and output addresses are rejected before any table is built. */
static void dart_test_map_reject(struct kunit *test)
{
	struct dart_test *t = test->priv;
	struct dart_io_pgtable *data;
	long pages;
	size_t mapped = 0;
	int ret;

	data = dart_test_alloc(test, APPLE_DART, SZ_16K, 32, 36);
	pages = dart_test_iommu_pages();

	/* Neither read nor write: refused without a warning. */
	KUNIT_EXPECT_EQ(test, dart_test_map(t, 0, 0x4000, 1, 0, &mapped), -EINVAL);
	KUNIT_EXPECT_EQ(test, dart_test_map(t, 0, 0x4000, 1, IOMMU_CACHE, &mapped), -EINVAL);

	kunit_warning_suppress(test) {
		/* Only the single configured page size is accepted. */
		ret = t->ops->map_pages(t->ops, 0, 0x4000, SZ_4K, 1, IOMMU_READ,
					GFP_KERNEL, &mapped);
		KUNIT_EXPECT_EQ(test, ret, -EINVAL);
		ret = t->ops->map_pages(t->ops, 0, 0x8000, SZ_32K, 1, IOMMU_READ,
					GFP_KERNEL, &mapped);
		KUNIT_EXPECT_EQ(test, ret, -EINVAL);
		/* Output address at or past 2^oas. */
		ret = dart_test_map(t, 0, BIT_ULL(36), 1, IOMMU_READ, &mapped);
		KUNIT_EXPECT_EQ(test, ret, -ERANGE);
		/* A run that would cross 2^oas. */
		ret = dart_test_map(t, 0, BIT_ULL(36) - SZ_16K, 2, IOMMU_READ, &mapped);
		KUNIT_EXPECT_EQ(test, ret, -ERANGE);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 4);
	}

	/* Beyond the single TTBR of a 16 KiB DART (iova >> 36). */
	KUNIT_EXPECT_EQ(test, dart_test_map(t, BIT_ULL(36), 0x4000, 1, IOMMU_READ, &mapped),
			-ENOMEM);
	KUNIT_EXPECT_EQ(test, mapped, (size_t)0);
	KUNIT_EXPECT_TRUE(test, mem_is_zero(data->pgd[0], SZ_16K));
	KUNIT_EXPECT_EQ(test, dart_test_iommu_pages(), pages);

	/* The last page below 2^oas is fine. */
	KUNIT_EXPECT_EQ(test, dart_test_map(t, 0, BIT_ULL(36) - SZ_16K, 1, IOMMU_READ, &mapped),
			0);
	KUNIT_EXPECT_EQ(test, mapped, (size_t)SZ_16K);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, 0x3fff), BIT_ULL(36) - 1);
	KUNIT_EXPECT_EQ(test, dart_test_iommu_pages(), pages + dart_test_tbl_pages(SZ_16K));
}

/* Unmap clears exactly the requested PTEs and queues one TLB page each. */
static void dart_test_unmap_range(struct kunit *test)
{
	struct iommu_iotlb_gather gather;
	struct dart_test *t = test->priv;
	struct dart_io_pgtable *data;
	dart_iopte *ptep;
	size_t mapped = 0;

	data = dart_test_alloc(test, APPLE_DART, SZ_16K, 32, 36);
	KUNIT_ASSERT_EQ(test, dart_test_map(t, SZ_1M, SZ_1G, 4, IOMMU_READ | IOMMU_WRITE,
					    &mapped), 0);
	KUNIT_ASSERT_EQ(test, mapped, (size_t)SZ_64K);
	ptep = dart_test_leaf(data, SZ_1M);
	KUNIT_ASSERT_NOT_NULL(test, ptep);

	iommu_iotlb_gather_init(&gather);
	KUNIT_EXPECT_EQ(test, dart_test_unmap(t, SZ_1M + SZ_16K, 2, &gather), (size_t)SZ_32K);
	KUNIT_EXPECT_NE(test, ptep[0], 0ULL);
	KUNIT_EXPECT_EQ(test, ptep[1], 0ULL);
	KUNIT_EXPECT_EQ(test, ptep[2], 0ULL);
	KUNIT_EXPECT_NE(test, ptep[3], 0ULL);
	KUNIT_EXPECT_EQ(test, t->tlb_add_page, 2U);
	KUNIT_EXPECT_EQ(test, t->tlb_last_iova, (unsigned long)SZ_1M + 2 * SZ_16K);
	KUNIT_EXPECT_EQ(test, t->tlb_last_granule, (size_t)SZ_16K);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_1M), (phys_addr_t)SZ_1G);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_1M + SZ_16K), 0ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_1M + 3 * SZ_16K),
			(phys_addr_t)SZ_1G + 3 * SZ_16K);

	/* A queued gather defers invalidation to the caller. */
	gather.queued = true;
	KUNIT_EXPECT_EQ(test, dart_test_unmap(t, SZ_1M, 1, &gather), (size_t)SZ_16K);
	KUNIT_EXPECT_EQ(test, t->tlb_add_page, 2U);
	KUNIT_EXPECT_EQ(test, ptep[0], 0ULL);
	KUNIT_EXPECT_EQ(test, dart_test_unmap(t, SZ_1M + 3 * SZ_16K, 1, NULL), (size_t)SZ_16K);
	KUNIT_EXPECT_EQ(test, t->tlb_add_page, 3U);
	KUNIT_EXPECT_TRUE(test, mem_is_zero(ptep, 4 * sizeof(*ptep)));
	KUNIT_EXPECT_EQ(test, t->tlb_other, 0U);
}

/* Unmap of holes, missing tables and runs past a leaf table end. */
static void dart_test_unmap_partial(struct kunit *test)
{
	const unsigned long end = SZ_32M - SZ_16K;	/* last leaf slot of L1 0 */
	struct dart_test *t = test->priv;
	size_t size;

	dart_test_alloc(test, APPLE_DART2, SZ_16K, 36, 42);
	KUNIT_ASSERT_EQ(test, dart_test_map(t, end, 0x40000000, 2, IOMMU_READ, NULL), 0);
	KUNIT_ASSERT_EQ(test, dart_test_map(t, SZ_32M, 0x40004000, 2, IOMMU_READ, NULL), 0);
	KUNIT_ASSERT_EQ(test, dart_test_map(t, SZ_64M, 0x50000000, 2, IOMMU_READ, NULL), 0);
	KUNIT_ASSERT_EQ(test, dart_test_map(t, SZ_64M + 3 * SZ_16K, 0x50000000, 1, IOMMU_READ,
					    NULL), 0);

	/* A run crossing the leaf table end stops at it. */
	KUNIT_EXPECT_EQ(test, dart_test_unmap(t, end, 3, NULL), (size_t)SZ_16K);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_32M), 0x40004000ULL);

	kunit_warning_suppress(test) {
		/* A hole at slot 2 ends the run after two pages. */
		size = dart_test_unmap(t, SZ_64M, 4, NULL);
		KUNIT_EXPECT_EQ(test, size, (size_t)SZ_32K);
		/* No leaf table at all. */
		size = dart_test_unmap(t, SZ_1G, 1, NULL);
		KUNIT_EXPECT_EQ(test, size, (size_t)0);
		/* Nothing to do and wrong page size. */
		size = dart_test_unmap(t, SZ_32M, 0, NULL);
		KUNIT_EXPECT_EQ(test, size, (size_t)0);
		size = t->ops->unmap_pages(t->ops, SZ_32M, SZ_4K, 1, NULL);
		KUNIT_EXPECT_EQ(test, size, (size_t)0);
		/* Beyond the TTBR. */
		size = dart_test_unmap(t, BIT_ULL(36), 1, NULL);
		KUNIT_EXPECT_EQ(test, size, (size_t)0);
		KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 5);
	}
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_32M), 0x40004000ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_64M), 0ULL);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_64M + 3 * SZ_16K), 0x50000000ULL);
	KUNIT_EXPECT_EQ(test, dart_test_unmap(t, SZ_32M, 2, NULL), (size_t)SZ_32K);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_32M + SZ_16K), 0ULL);
}

/* iova_to_phys() misses at each level of an otherwise populated table. */
static void dart_test_iova_to_phys_miss(struct kunit *test)
{
	struct dart_test *t = test->priv;

	dart_test_alloc(test, APPLE_DART2, SZ_16K, 37, 42);
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, 0), 0ULL);
	KUNIT_ASSERT_EQ(test, dart_test_map(t, SZ_32M + SZ_16K, 0x12340000, 1, IOMMU_READ,
					    NULL), 0);
	/* Same leaf table, empty slot. */
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_32M), 0ULL);
	/* Same L2 table, no leaf table. */
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, 0), 0ULL);
	/* No L2 table under L1 index 1. */
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, BIT_ULL(36) + SZ_32M + SZ_16K), 0ULL);
	/* The offset within the page is kept. */
	KUNIT_EXPECT_EQ(test, dart_test_phys(t, SZ_32M + SZ_16K + 0x3fff), 0x12343fffULL);
}

struct dart_test_teardown {
	const char *name;
	enum io_pgtable_fmt fmt;
	size_t pgsize;
	u32 ias, oas;
	unsigned long iova[4];
	long tables;	/* tables held after alloc + the maps */
};

static const struct dart_test_teardown dart_test_teardowns[] = {
	/* 1 root + 2 leaf tables. */
	{ "dart1-16k", APPLE_DART, SZ_16K, 32, 36, { 0, SZ_16K, SZ_32M, SZ_32M + SZ_16K }, 3 },
	/* 4 roots + 4 leaf tables, one per TTBR. */
	{ "dart1-4k-4ttbr", APPLE_DART, SZ_4K, 32, 36, { 0, SZ_1G, 2UL * SZ_1G, 3UL * SZ_1G }, 8 },
	/* 1 root + 2 L2 + 3 leaf tables. */
	{ "dart2-16k-4level", APPLE_DART2, SZ_16K, 42, 42,
	  { 0, SZ_32M, BIT(36), BIT(36) + SZ_16K }, 6 },
};

static void dart_test_teardown_desc(const struct dart_test_teardown *td, char *desc)
{
	strscpy(desc, td->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(dart_test_teardown, dart_test_teardowns, dart_test_teardown_desc);

/* Every table page allocated by map_pages() is released by free_io_pgtable_ops(). */
static void dart_test_teardown_free(struct kunit *test)
{
	const struct dart_test_teardown *td = test->param_value;
	struct dart_test *t = test->priv;
	long before;
	int i;

	before = dart_test_iommu_pages();
	dart_test_alloc(test, td->fmt, td->pgsize, td->ias, td->oas);
	KUNIT_EXPECT_EQ(test, dart_test_iommu_pages() - before,
			t->cfg.apple_dart_cfg.n_ttbrs * dart_test_tbl_pages(td->pgsize));
	for (i = 0; i < ARRAY_SIZE(td->iova); i++)
		KUNIT_ASSERT_EQ(test, dart_test_map(t, td->iova[i], SZ_1G, 1, IOMMU_READ, NULL), 0);
	KUNIT_EXPECT_EQ(test, dart_test_iommu_pages() - before,
			td->tables * dart_test_tbl_pages(td->pgsize));

	/* Unmap keeps the tables; the teardown frees them. */
	for (i = 0; i < ARRAY_SIZE(td->iova); i++)
		KUNIT_EXPECT_EQ(test, dart_test_unmap(t, td->iova[i], 1, NULL), td->pgsize);
	KUNIT_EXPECT_EQ(test, dart_test_iommu_pages() - before,
			td->tables * dart_test_tbl_pages(td->pgsize));

	kunit_release_action(test, dart_test_free_ops, t->ops);
	t->ops = NULL;
	KUNIT_EXPECT_EQ(test, dart_test_iommu_pages(), before);
}

static struct kunit_case io_pgtable_dart_test_cases[] = {
	KUNIT_CASE_PARAM(dart_test_alloc_layout, dart_test_layout_gen_params),
	KUNIT_CASE_PARAM(dart_test_alloc_reject, dart_test_bad_cfg_gen_params),
	KUNIT_CASE(dart_test_pte_dart1),
	KUNIT_CASE(dart_test_pte_dart2),
	KUNIT_CASE(dart_test_table_ptes),
	KUNIT_CASE(dart_test_index_high_l1),
	KUNIT_CASE(dart_test_four_level),
	KUNIT_CASE(dart_test_four_ttbrs),
	KUNIT_CASE(dart_test_map_multi),
	KUNIT_CASE(dart_test_map_exists),
	KUNIT_CASE(dart_test_map_reject),
	KUNIT_CASE(dart_test_unmap_range),
	KUNIT_CASE(dart_test_unmap_partial),
	KUNIT_CASE(dart_test_iova_to_phys_miss),
	KUNIT_CASE_PARAM(dart_test_teardown_free, dart_test_teardown_gen_params),
	{}
};

static struct kunit_suite io_pgtable_dart_test_suite = {
	.name = "io-pgtable-dart",
	.init = dart_test_init,
	.test_cases = io_pgtable_dart_test_cases,
};

kunit_test_suite(io_pgtable_dart_test_suite);
