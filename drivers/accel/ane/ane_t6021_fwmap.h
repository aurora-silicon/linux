/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Engine-side protection of the staged H14 firmware image.
 *
 * Kept free of device access so the KUnit suite (ane_t6021_fwmap_test.c)
 * runs the same code as the loader (ane_t6021_fwload.c).
 */
#ifndef __ANE_T6021_FWMAP_H__
#define __ANE_T6021_FWMAP_H__

#include <linux/iommu.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include "ane_fw_validate.h"

/*
 * IOMMU protection for the engine's mapping of [off, off + len) of the
 * staged image, off counted from the image's vm base. A range that lies
 * wholly inside the image's __TEXT segment (found by name in the pinned
 * segment table) loses IOMMU_WRITE; every other range (DATA, the vmsize
 * tail, the extra RAM after the image) keeps prot. This is the device
 * side only: the kernel still writes TEXT through its own mapping (the
 * DATA base patch, the re-copy before a restart).
 */
static inline int ane_t6021_fw_prot(const struct ane_fw_seg *segs,
				    unsigned int nsegs, u64 off, u64 len,
				    int prot)
{
	unsigned int i;

	for (i = 0; i < nsegs; i++) {
		const struct ane_fw_seg *s = &segs[i];

		if (strcmp(s->name, "__TEXT"))
			continue;
		if (off >= s->vmaddr && len <= s->vmsize &&
		    off - s->vmaddr <= s->vmsize - len)
			return prot & ~IOMMU_WRITE;
	}
	return prot;
}

#endif /* __ANE_T6021_FWMAP_H__ */
