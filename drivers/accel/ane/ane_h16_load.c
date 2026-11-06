// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/module.h>

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
	struct ane_h16_hwx hwx;
	int ret;

	if (!plan)
		return -EINVAL;
	ret = ane_h16_hwx_snapshot_check(snapshot);
	if (ret)
		return ret;
	/* Reparse the authenticated original, not caller-supplied cached metadata. */
	ret = ane_h16_hwx_validate(snapshot->bytes, snapshot->size, &hwx);
	if (ret)
		return ret;
	return h16_load_plan(&hwx, plan);
}
EXPORT_SYMBOL_GPL(ane_h16_load_plan_create);

int ane_h16_load_resources_create(const struct ane_h16_hwx_snapshot *snapshot,
				  const struct ane_h16_load_mapping *mappings,
				  unsigned int mapping_count,
				  const struct ane_h16_process_binding *bindings,
				  unsigned int binding_count,
				  struct ane_h16_resource *resources)
{
	struct ane_h16_resource next[ANE_H16_SURFACES] = {};
	struct ane_h16_load_plan plan;
	struct ane_h16_hwx hwx;
	unsigned int i, j;
	int ret;

	if (!mappings || !bindings || !resources)
		return -EINVAL;
	ret = ane_h16_hwx_snapshot_check(snapshot);
	if (ret)
		return ret;
	ret = ane_h16_hwx_validate(snapshot->bytes, snapshot->size, &hwx);
	if (ret)
		return ret;
	ret = h16_load_plan(&hwx, &plan);
	if (ret)
		return ret;
	if (mapping_count != plan.count || binding_count != hwx.surface_count)
		return -EINVAL;
	for (i = 0; i < mapping_count; i++) {
		const struct ane_h16_load_mapping *m = &mappings[i];
		u64 end;

		if (!m->dva || !IS_ALIGNED(m->dva, SZ_16K) ||
		    m->size != plan.allocation[i] ||
		    m->permissions != plan.regions[i].permissions ||
		    check_add_overflow(m->dva, m->size, &end) ||
		    (m->dva < 0x1000036c000ULL && end > 0x10000000000ULL))
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (m->dva < mappings[j].dva + mappings[j].size &&
			    mappings[j].dva < end)
				return -EINVAL;
	}
	for (i = 0; i < binding_count; i++) {
		const struct ane_h16_hwx_surface *s = &hwx.surfaces[i];
		const struct ane_h16_process_binding *b = &bindings[i];

		if (b->hwx_vm != s->section.vm || b->class != s->direction - 1)
			return -EINVAL;
		for (j = 0; j < plan.count; j++)
			if (plan.regions[j].kind == ANE_H16_REGION_SURFACE &&
			    plan.regions[j].vm == b->hwx_vm)
				break;
		if (j == plan.count)
			return -EINVAL;
		next[i].hwx_vm = b->hwx_vm;
		next[i].permissions = s->direction;
		next[i].buffer = (struct ane_h16_execute_buffer) {
			.index = b->index,
			.class = b->class,
			.mapping_dva = mappings[j].dva,
			.mapping_size = mappings[j].size,
			.size = s->span,
		};
	}
	ret = ane_h16_resources_validate(&hwx, next, binding_count);
	if (!ret)
		memcpy(resources, next, binding_count * sizeof(*resources));
	return ret;
}
EXPORT_SYMBOL_GPL(ane_h16_load_resources_create);

MODULE_DESCRIPTION("H16 original HWX load planning and resource geometry");
MODULE_LICENSE("Dual MIT/GPL");
