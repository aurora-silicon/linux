/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PCIE_APPLE_T8122_H
#define PCIE_APPLE_T8122_H

#define APPLE_T8122_PCIEC0_ECAM 0x730000000ULL
#define APPLE_T8122_PCIEC1_ECAM 0xb30000000ULL
#define APPLE_T8122_PCIEC_AIC_BASE 1075
#define APPLE_T8122_PCIEC0_MSI_BASE 32
#define APPLE_T8122_PCIEC1_MSI_BASE 64

/* Reject unknown apertures instead of assigning another bridge's slice. */
static inline int apple_t8122_msi_vector_base(unsigned long long ecam)
{
	switch (ecam) {
	case APPLE_T8122_PCIEC0_ECAM:
		return APPLE_T8122_PCIEC0_MSI_BASE;
	case APPLE_T8122_PCIEC1_ECAM:
		return APPLE_T8122_PCIEC1_MSI_BASE;
	default:
		return -1;
	}
}

#endif
