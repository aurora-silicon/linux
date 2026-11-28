/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PCIE_APPLE_T8122_H
#define PCIE_APPLE_T8122_H

#ifndef __KERNEL__
#include <stdbool.h>
#endif

/*
 * A T8122 PCIe-C bridge's MSI slice, as its node describes it: the first
 * vector of the shared window it owns (apple,msi-vector-base) and that
 * vector's AIC line (msi-ranges). The internal host owns vectors 0..31 and
 * each PCIe-C bridge a 32-vector slice after them, up to vector 255. Returns
 * the window's AIC base, or -1 for a slice that is not one of those.
 */
static inline int apple_t8122_msi_window(unsigned int base, unsigned int irq,
					 unsigned int nvecs)
{
	if (base < 32 || base > 224 || base % 32 || nvecs != 32 || irq < base)
		return -1;
	return irq - base;
}

/*
 * A PCIe-C bridge's MSI slice against the one its SoC's table gives this ECAM
 * aperture (expected) in a window starting at AIC aic_base. The node names it
 * with apple,msi-vector-base and a 32-vector msi-ranges at aic_base + base.
 * An old experimental T6030 boot DT gave every host the global window
 * (AIC 1342, 512 vectors) and no base: that one is corrected to the expected
 * slice. Returns 0 for a slice accepted as described, 1 for a corrected one
 * (*irq and *nvecs updated), -1 for anything else.
 */
static inline int apple_pciec_msi_slice(bool has_base, unsigned int base,
					unsigned int expected, unsigned int aic_base,
					unsigned int *irq, unsigned int *nvecs)
{
	if (!has_base) {
		if (aic_base != 1342 || *irq != 1342 || *nvecs != 512)
			return -1;
		base = expected;
		*nvecs = 32;
		*irq += expected;
		return base >= 0x20 && base % 32 == 0 && base + 32 <= 256 ? 1 : -1;
	}
	if (base != expected || base < 0x20 || base % 32 || *nvecs != 32 ||
	    base + *nvecs > 256 || *irq != aic_base + base)
		return -1;
	return 0;
}

#endif
