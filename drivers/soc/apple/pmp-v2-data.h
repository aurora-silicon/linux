/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef APPLE_PMP_V2_DATA_H
#define APPLE_PMP_V2_DATA_H

/* Private DATA profile shared by the admitted 25F84 and 25G83 builds (see
 * pmp-v2-profile.h). Never translate CODE or arbitrary heap.
 */
static inline int pmp_v2_data_range(u64 address, size_t size, u64 *physical)
{
	u64 end;

	if (!size || check_add_overflow(address, (u64)size, &end) ||
	    address < 0x103c000 || end > 0x1098000)
		return -ERANGE;
	*physical = 0x30053c000ULL + address - 0x103c000;
	return 0;
}

static inline int pmp_v2_heap_range(u64 address, size_t size)
{
	u64 physical;

	if (pmp_v2_data_range(address, size, &physical) || !IS_ALIGNED(address, 4) ||
	    address < 0x105e3c0 || size > 0x38000 || address > 0x10963c0 - size)
		return -ERANGE;
	return 0;
}
#endif
