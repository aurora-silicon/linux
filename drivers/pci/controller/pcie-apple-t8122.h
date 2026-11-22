/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PCIE_APPLE_T8122_H
#define PCIE_APPLE_T8122_H

/*
 * A T8122 PCIe-C bridge's MSI slice, as its node describes it: the first
 * vector of the shared window it owns (apple,msi-vector-base) and that
 * vector's AIC line (msi-ranges). The internal host owns vectors 0..31 and
 * each PCIe-C bridge a 32-vector slice after them. Returns the window's AIC
 * base, or -1 for a slice that is not one of those.
 */
static inline int apple_t8122_msi_window(unsigned int base, unsigned int irq,
					 unsigned int nvecs)
{
	if (base < 32 || base % 32 || nvecs != 32 || irq < base)
		return -1;
	return irq - base;
}

#endif
