// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/module.h>
#include <linux/slab.h>

#include "ane_h16_load.h"

static int h16_load_plan(const struct ane_h16_hwx *hwx, struct ane_h16_load_plan *plan)
{
	struct ane_h16_load_plan next = {};
	unsigned int i;

	if (!hwx->region_count || hwx->region_count > ANE_H16_REGIONS)
		return -ENOEXEC;
	next.count = hwx->region_count;
	for (i = 0; i < next.count; i++) {
		const struct ane_h16_hwx_region *r = &hwx->regions[i];
		u64 rounded;

		if (!r->size || check_add_overflow(r->size, (u64)SZ_16K - 1, &rounded))
			return -ENOEXEC;
		next.allocation[i] = round_down(rounded, SZ_16K);
		if (check_add_overflow(next.total, next.allocation[i], &next.total) ||
		    next.total > ANE_H16_LOAD_BYTES_MAX)
			return -E2BIG;
		next.regions[i] = *r;
	}
	*plan = next;
	return 0;
}

int ane_h16_load_plan_create(const struct ane_h16_hwx_snapshot *snapshot,
			     struct ane_h16_load_plan *plan)
{
	struct ane_h16_hwx *hwx;
	int ret;

	if (!plan)
		return -EINVAL;
	ret = ane_h16_hwx_snapshot_check(snapshot);
	if (ret)
		return ret;
	/* 16 surfaces make the parsed HWX too big for the stack. */
	hwx = kzalloc(sizeof(*hwx), GFP_KERNEL);
	if (!hwx)
		return -ENOMEM;
	/* Reparse the authenticated original, not caller-supplied cached metadata. */
	ret = ane_h16_hwx_validate(snapshot->bytes, snapshot->size, hwx);
	if (!ret)
		ret = h16_load_plan(hwx, plan);
	kfree(hwx);
	return ret;
}
EXPORT_SYMBOL_GPL(ane_h16_load_plan_create);

int ane_h16_load_resources_create(const struct ane_h16_hwx_snapshot *snapshot,
				  const struct ane_h16_load_mapping *mappings,
				  unsigned int mapping_count,
				  const struct ane_h16_process_binding *bindings,
				  unsigned int binding_count,
				  struct ane_h16_resource *resources)
{
	/* 16 surfaces make these too big for the stack. */
	struct {
		struct ane_h16_resource next[ANE_H16_SURFACES];
		struct ane_h16_load_plan plan;
		struct ane_h16_hwx hwx;
	} *w;
	unsigned int i, j;
	int ret;

	if (!mappings || !bindings || !resources)
		return -EINVAL;
	ret = ane_h16_hwx_snapshot_check(snapshot);
	if (ret)
		return ret;
	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	ret = ane_h16_hwx_validate(snapshot->bytes, snapshot->size, &w->hwx);
	if (ret)
		goto out;
	ret = h16_load_plan(&w->hwx, &w->plan);
	if (ret)
		goto out;
	ret = -EINVAL;
	if (mapping_count != w->plan.count || binding_count != w->hwx.surface_count)
		goto out;
	for (i = 0; i < mapping_count; i++) {
		const struct ane_h16_load_mapping *m = &mappings[i];
		u64 end;

		if (!m->dva || !IS_ALIGNED(m->dva, SZ_16K) ||
		    m->size != w->plan.allocation[i] ||
		    m->permissions != w->plan.regions[i].permissions ||
		    check_add_overflow(m->dva, m->size, &end) ||
		    (m->dva < 0x1000036c000ULL && end > 0x10000000000ULL))
			goto out;
		for (j = 0; j < i; j++)
			if (m->dva < mappings[j].dva + mappings[j].size &&
			    mappings[j].dva < end)
				goto out;
	}
	for (i = 0; i < binding_count; i++) {
		const struct ane_h16_hwx_surface *s = &w->hwx.surfaces[i];
		const struct ane_h16_process_binding *b = &bindings[i];

		if (b->hwx_vm != s->section.vm || b->class != s->direction - 1)
			goto out;
		for (j = 0; j < w->plan.count; j++)
			if (w->plan.regions[j].kind == ANE_H16_REGION_SURFACE &&
			    w->plan.regions[j].vm == b->hwx_vm)
				break;
		if (j == w->plan.count)
			goto out;
		w->next[i].hwx_vm = b->hwx_vm;
		w->next[i].permissions = s->direction;
		w->next[i].buffer = (struct ane_h16_execute_buffer) {
			.index = b->index,
			.class = b->class,
			.mapping_dva = mappings[j].dva,
			.mapping_size = mappings[j].size,
			.size = s->span,
		};
	}
	ret = ane_h16_resources_validate(&w->hwx, w->next, binding_count);
	if (!ret)
		memcpy(resources, w->next, binding_count * sizeof(*resources));
out:
	kfree(w);
	return ret;
}
EXPORT_SYMBOL_GPL(ane_h16_load_resources_create);

MODULE_DESCRIPTION("H16 original HWX load planning and resource geometry");
MODULE_LICENSE("Dual MIT/GPL");
