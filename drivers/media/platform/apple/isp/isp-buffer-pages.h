/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ISP_BUFFER_PAGES_H__
#define __ISP_BUFFER_PAGES_H__

#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

struct isp_pinned_pages {
	struct page **pages;
	unsigned int count;
};

/* The ISP queue permits MMAP only: its SG extents contain whole pages. */
static inline int isp_pin_buffer_pages(struct sg_table *sgt,
				       struct isp_pinned_pages *pins)
{
	struct scatterlist *sg;
	u64 count = 0;
	unsigned int i, index = 0;

	if (!sgt || !sgt->sgl || pins->pages || pins->count)
		return -EINVAL;

	for_each_sg(sgt->sgl, sg, sgt->orig_nents, i) {
		if (sg->offset % PAGE_SIZE || !sg->length || sg->length % PAGE_SIZE)
			return -EINVAL;
		count += sg->length / PAGE_SIZE;
		if (count > UINT_MAX)
			return -EOVERFLOW;
	}
	if (!count)
		return -EINVAL;
	pins->pages = kvmalloc_array(count, sizeof(*pins->pages), GFP_KERNEL);
	if (!pins->pages)
		return -ENOMEM;
	for_each_sg(sgt->sgl, sg, sgt->orig_nents, i) {
		unsigned int first = sg->offset / PAGE_SIZE;
		unsigned int pages = sg->length / PAGE_SIZE;

		for (unsigned int j = 0; j < pages; j++) {
			struct page *page = pfn_to_page(page_to_pfn(sg_page(sg)) + first + j);

			get_page(page);
			pins->pages[index++] = page;
		}
	}
	pins->count = count;
	return 0;
}

static inline void isp_unpin_buffer_pages(struct isp_pinned_pages *pins)
{
	for (unsigned int i = 0; i < pins->count; i++)
		put_page(pins->pages[i]);
	kvfree(pins->pages);
	pins->pages = NULL;
	pins->count = 0;
}

#endif /* __ISP_BUFFER_PAGES_H__ */
