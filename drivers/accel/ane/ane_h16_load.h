/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __ANE_H16_LOAD_H__
#define __ANE_H16_LOAD_H__

#include "ane_h16_resources.h"

/* Conservative host budget; firmware resource replies cannot enlarge it. */
#define ANE_H16_LOAD_BYTES_MAX SZ_512M

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
