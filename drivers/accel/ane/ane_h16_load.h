/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_LOAD_H__
#define __ANE_H16_LOAD_H__

#include "ane_h16_resources.h"

/*
 * Host budget for the private mappings of one program, scratch included;
 * firmware resource replies cannot enlarge it. Each segment may map 1G, and
 * a whole-model encoder already maps 488M (390M of kernel data, 37M of
 * __TEXT, a 59M scratch), so the sum is bounded here at twice the segment
 * limit rather than at a fraction of one segment.
 */
#define ANE_H16_LOAD_BYTES_MAX SZ_2G

struct ane_h16_load_plan {
	struct ane_h16_hwx_region regions[ANE_H16_REGIONS];
	u64 allocation[ANE_H16_REGIONS];
	u64 total;
	u32 count;
};

struct ane_h16_load_mapping {
	u64 dva;
	u64 size;
	u32 permissions;
};

/* Only the backend may resolve these bindings from the validated program
 * contract. CREATE_PROCESS reply IDs do not describe resource bindings.
 */
struct ane_h16_process_binding {
	u64 hwx_vm;
	u32 index;
	u32 class;
	/* Buffer size the generic section declared at LOAD. */
	u64 bytes;
};

/* Geometry helpers do not prove DMA ownership or install a process. All
 * mappings must be backend-owned and private, with the stated permissions.
 * Wire serialization and process installation are separate backend operations.
 */
int ane_h16_load_plan_create(const struct ane_h16_hwx_snapshot *snapshot,
			     struct ane_h16_load_plan *plan);
int ane_h16_load_resources_create(const struct ane_h16_hwx_snapshot *snapshot,
				  const struct ane_h16_load_mapping *mappings,
				  unsigned int mapping_count,
				  const struct ane_h16_process_binding *bindings,
				  unsigned int binding_count,
				  struct ane_h16_resource *resources);

#endif /* __ANE_H16_LOAD_H__ */
