/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_SOC_AURORA_APIF_MMIO_H
#define _LINUX_SOC_AURORA_APIF_MMIO_H

#include <linux/types.h>

struct device;
struct aurora_apif;

#define AURORA_APIF_SELECTOR(table, endpoint) \
	(((u64)(table) << 32) | (endpoint))
#define AURORA_APIF_SERVICE 0xffffffffU
#define AURORA_APIF_TRANSLATE 1
#define AURORA_APIF_ALLOC_FRAME 2
#define AURORA_APIF_FREE_FRAME 3
#define AURORA_APIF_MAP_IO 4
#define AURORA_APIF_GUEST_ROOT 5
#define AURORA_APIF_BOOT_IOMMU 6
#define AURORA_APIF_BOOT_TABLE 7
#define AURORA_APIF_FIND_FRAME 8
#define AURORA_APIF_TAG_FRAME 9
#define AURORA_APIF_BOOT_DART 0

struct aurora_apif_op {
	u64 selector;
	u64 arg[6];
	u64 ret;
};

struct aurora_apif_boot_iommu {
	u64 level, count, base, size, pa, ipa, attrs, reserved;
};

/* Each client owns a buffer. Submission is synchronous and atomic-context
 * safe. Unbind drains the client and subsequent calls return -ENODEV.
 */
struct aurora_apif *aurora_apif_get(struct device *dev);
void aurora_apif_put(struct aurora_apif *apif);
/* @done counts executed operations. The transport does not interpret native
 * results; endpoint-specific status and ownership belong to the caller.
 */
int aurora_apif_submit(struct aurora_apif *apif, struct aurora_apif_op *ops,
		       u32 count, u32 *done);

#endif
