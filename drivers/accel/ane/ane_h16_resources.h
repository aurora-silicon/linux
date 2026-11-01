/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_RESOURCES_H__
#define __ANE_H16_RESOURCES_H__

#include "ane_h16_execute.h"
#include "ane_h16_hwx.h"

/* Created only by the backend matching a validated HWX with reviewed program
 * resource bindings. No userspace field may supply this authority. Each
 * mapping is private, coherent DMA memory with no userspace alias, retained
 * for process lifetime.
 */
struct ane_h16_resource {
	u64 hwx_vm;
	u32 permissions;
	struct ane_h16_execute_buffer buffer;
};

static inline int
ane_h16_resources_validate(const struct ane_h16_hwx *hwx,
			   const struct ane_h16_resource *resources, unsigned int count)
{
	unsigned int i, j;

	if (!hwx || !resources || count < 2 || count > ANE_H16_SURFACES ||
	    count != hwx->surface_count)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		const struct ane_h16_hwx_surface *s = &hwx->surfaces[i];
		const struct ane_h16_resource *r = &resources[i];
		const struct ane_h16_execute_buffer *b = &r->buffer;
		u64 end;

		/* Only the established input/output classes are executable. */
		if (s->direction < 1 || s->direction > 2 ||
		    r->hwx_vm != s->section.vm || r->permissions != s->direction ||
		    b->class != s->direction - 1 || !s->span ||
		    s->span > s->section.size || s->section.size > s->allocation ||
		    !IS_ALIGNED(s->allocation, SZ_16K) ||
		    b->size != s->span || b->offset || !b->mapping_dva ||
		    !IS_ALIGNED(b->mapping_dva, SZ_16K) ||
		    b->mapping_size != s->allocation || b->size > b->mapping_size ||
		    check_add_overflow(b->mapping_dva, b->mapping_size, &end))
			return -EINVAL;
		for (j = 0; j < i; j++) {
			const struct ane_h16_execute_buffer *a = &resources[j].buffer;

			if (a->index == b->index ||
			    (b->mapping_dva < a->mapping_dva + a->mapping_size &&
			     a->mapping_dva < end))
				return -EINVAL;
		}
	}
	return 0;
}

#endif /* __ANE_H16_RESOURCES_H__ */
