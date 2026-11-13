// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test of DMA_ATTR_NO_BOUNCE on the dma-direct path.
 *
 * The test device's bus reaches all memory, at its CPU address, except one
 * page (a dma_range_map with a hole). The DMA layer can serve the device
 * that page only through a bounce buffer.
 */
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/panic.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/swiotlb.h>

struct no_bounce {
	struct device *dev;
	/* A page the device addresses, and the one it does not. */
	struct page *near, *hidden;
};

static void no_bounce_free_page(void *page)
{
	__free_page(page);
}

static struct page *no_bounce_page(struct kunit *test)
{
	struct page *page = alloc_page(GFP_KERNEL | __GFP_ZERO);

	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, no_bounce_free_page, page));
	return page;
}

/* The DMA address of @page for the test device, as dma-direct makes it. */
static dma_addr_t no_bounce_dma(struct no_bounce *nb, struct page *page)
{
	return phys_to_dma(nb->dev, page_to_phys(page));
}

static int no_bounce_init(struct kunit *test)
{
	struct no_bounce *nb = kunit_kzalloc(test, sizeof(*nb), GFP_KERNEL);
	struct bus_dma_region *map;
	phys_addr_t start, end;
	dma_addr_t near;

	KUNIT_ASSERT_NOT_NULL(test, nb);
	nb->near = no_bounce_page(test);
	nb->hidden = no_bounce_page(test);
	start = page_to_phys(nb->hidden);
	end = start + PAGE_SIZE;

	nb->dev = kunit_device_register(test, "dma_no_bounce");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, nb->dev);
	nb->dev->coherent_dma_mask = DMA_BIT_MASK(64);
	nb->dev->dma_mask = &nb->dev->coherent_dma_mask;

	/* The driver core frees the map with the device. */
	map = kcalloc(3, sizeof(*map), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, map);
	map[0] = (struct bus_dma_region){
		.cpu_start = end, .dma_start = end, .size = U64_MAX - end,
	};
	map[1] = (struct bus_dma_region){
		.cpu_start = 0, .dma_start = 0, .size = start,
	};
	nb->dev->dma_range_map = map;
	test->priv = nb;

	/*
	 * The test needs dma-direct to map the device, the range map to decide
	 * what the device reaches, and nothing else to make a mapping bounce.
	 */
	if (get_dma_ops(nb->dev))
		kunit_skip(test, "the device is mapped by DMA operations");
	if (is_swiotlb_force_bounce(nb->dev))
		kunit_skip(test, "bouncing is forced for the device");
	if (force_dma_unencrypted(nb->dev))
		kunit_skip(test, "the device needs unencrypted memory, which bounces");
	near = no_bounce_dma(nb, nb->near);
	if (!dma_capable(nb->dev, near, PAGE_SIZE, true, 0) ||
	    dma_to_phys(nb->dev, near) != page_to_phys(nb->near) ||
	    dma_capable(nb->dev, no_bounce_dma(nb, nb->hidden), PAGE_SIZE,
			true, 0))
		kunit_skip(test, "phys_to_dma() does not follow the range map");
	return 0;
}

/*
 * Maps the page the device does not address with the attribute: true if the
 * request is refused. A mapping made against that is undone.
 */
static bool no_bounce_hidden_refused(struct no_bounce *nb)
{
	dma_addr_t addr = dma_map_page_attrs(nb->dev, nb->hidden, 0, PAGE_SIZE,
					     DMA_TO_DEVICE, DMA_ATTR_NO_BOUNCE);

	if (dma_mapping_error(nb->dev, addr))
		return true;
	dma_unmap_page_attrs(nb->dev, addr, PAGE_SIZE, DMA_TO_DEVICE,
			     DMA_ATTR_NO_BOUNCE);
	return false;
}

/*
 * Left to itself the DMA layer bounces the page the device cannot address;
 * with the attribute it fails the mapping, and maps a page the device does
 * address as that page.
 */
static void no_bounce_refuses_a_bounce_buffer(struct kunit *test)
{
	struct no_bounce *nb = test->priv;
	phys_addr_t start = page_to_phys(nb->hidden);
	u8 *hidden = page_address(nb->hidden);
	phys_addr_t bounce;
	dma_addr_t addr;

	if (!is_swiotlb_active(nb->dev))
		kunit_skip(test, "no SWIOTLB to show the bounce buffer with");

	/*
	 * An ordinary mapping lies outside the page, holds what the page held
	 * when it was mapped and does not follow a later store to the page.
	 */
	hidden[0] = 0xa5;
	addr = dma_map_page_attrs(nb->dev, nb->hidden, 0, PAGE_SIZE,
				  DMA_TO_DEVICE, 0);
	KUNIT_ASSERT_FALSE(test, dma_mapping_error(nb->dev, addr));
	bounce = dma_to_phys(nb->dev, addr);
	KUNIT_EXPECT_NE(test, (phys_addr_t)-1, bounce);
	KUNIT_EXPECT_TRUE(test, bounce < start || bounce >= start + PAGE_SIZE);
	if (bounce != (phys_addr_t)-1) {
		KUNIT_EXPECT_EQ(test, 0xa5, *(u8 *)phys_to_virt(bounce));
		hidden[0] = 0x5a;
		KUNIT_EXPECT_EQ(test, 0xa5, *(u8 *)phys_to_virt(bounce));
	}
	dma_unmap_page_attrs(nb->dev, addr, PAGE_SIZE, DMA_TO_DEVICE, 0);

	KUNIT_EXPECT_TRUE(test, no_bounce_hidden_refused(nb));

	addr = dma_map_page_attrs(nb->dev, nb->near, 0, PAGE_SIZE,
				  DMA_TO_DEVICE, DMA_ATTR_NO_BOUNCE);
	KUNIT_ASSERT_FALSE(test, dma_mapping_error(nb->dev, addr));
	KUNIT_EXPECT_EQ(test, no_bounce_dma(nb, nb->near), addr);
	dma_unmap_page_attrs(nb->dev, addr, PAGE_SIZE, DMA_TO_DEVICE,
			     DMA_ATTR_NO_BOUNCE);
}

/*
 * A scatterlist fails as a whole when one of its segments would be bounced,
 * also when a segment before it was mapped already.
 */
static void no_bounce_fails_the_whole_scatterlist(struct kunit *test)
{
	struct no_bounce *nb = test->priv;
	struct scatterlist sgl[2];
	struct sg_table sgt = { .sgl = sgl, .nents = 2, .orig_nents = 2 };
	int ret;

	sg_init_table(sgl, 2);
	sg_set_page(&sgl[0], nb->near, PAGE_SIZE, 0);
	sg_set_page(&sgl[1], nb->hidden, PAGE_SIZE, 0);
	ret = dma_map_sgtable(nb->dev, &sgt, DMA_TO_DEVICE, DMA_ATTR_NO_BOUNCE);
	if (!ret)
		dma_unmap_sgtable(nb->dev, &sgt, DMA_TO_DEVICE,
				  DMA_ATTR_NO_BOUNCE);
	KUNIT_EXPECT_EQ(test, -EIO, ret);
	ret = dma_map_sg_attrs(nb->dev, sgl, 2, DMA_TO_DEVICE,
			       DMA_ATTR_NO_BOUNCE);
	if (ret)
		dma_unmap_sg_attrs(nb->dev, sgl, 2, DMA_TO_DEVICE,
				   DMA_ATTR_NO_BOUNCE);
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* The segment the device addresses, alone, is mapped as itself. */
	sgt.nents = 1;
	sgt.orig_nents = 1;
	sg_init_table(sgl, 1);
	sg_set_page(&sgl[0], nb->near, PAGE_SIZE, 0);
	KUNIT_ASSERT_EQ(test, 0,
			dma_map_sgtable(nb->dev, &sgt, DMA_TO_DEVICE,
					DMA_ATTR_NO_BOUNCE));
	KUNIT_EXPECT_EQ(test, no_bounce_dma(nb, nb->near), sg_dma_address(sgl));
	dma_unmap_sgtable(nb->dev, &sgt, DMA_TO_DEVICE, DMA_ATTR_NO_BOUNCE);
}

/*
 * A device without SWIOTLB: a request without the attribute for the page it
 * cannot address is an address overflow, with a warning. With the attribute
 * it is a refusal like the others and must not warn.
 */
static void no_bounce_refusal_does_not_warn(struct kunit *test)
{
	struct no_bounce *nb = test->priv;
	dma_addr_t addr;

#ifdef CONFIG_SWIOTLB
	nb->dev->dma_io_tlb_mem = NULL;
#endif
	KUNIT_ASSERT_FALSE(test, is_swiotlb_active(nb->dev));
	if (test_taint(TAINT_WARN))
		kunit_skip(test, "the kernel has warned before");

	KUNIT_EXPECT_TRUE(test, no_bounce_hidden_refused(nb));
	KUNIT_EXPECT_FALSE(test, test_taint(TAINT_WARN));

	addr = dma_map_page_attrs(nb->dev, nb->near, 0, PAGE_SIZE,
				  DMA_TO_DEVICE, DMA_ATTR_NO_BOUNCE);
	KUNIT_ASSERT_FALSE(test, dma_mapping_error(nb->dev, addr));
	KUNIT_EXPECT_EQ(test, no_bounce_dma(nb, nb->near), addr);
	dma_unmap_page_attrs(nb->dev, addr, PAGE_SIZE, DMA_TO_DEVICE,
			     DMA_ATTR_NO_BOUNCE);
}

static struct kunit_case no_bounce_cases[] = {
	KUNIT_CASE(no_bounce_refuses_a_bounce_buffer),
	KUNIT_CASE(no_bounce_fails_the_whole_scatterlist),
	KUNIT_CASE(no_bounce_refusal_does_not_warn),
	{}
};

static struct kunit_suite no_bounce_suite = {
	.name = "dma_attr_no_bounce",
	.init = no_bounce_init,
	.test_cases = no_bounce_cases,
};
kunit_test_suite(no_bounce_suite);

MODULE_DESCRIPTION("KUnit test of DMA_ATTR_NO_BOUNCE");
MODULE_LICENSE("GPL");
