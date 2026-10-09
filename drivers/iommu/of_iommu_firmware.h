/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __OF_IOMMU_FIRMWARE_H
#define __OF_IOMMU_FIRMWARE_H

static inline bool of_iommu_firmware_read_only(struct device_node *node)
{
	return of_device_is_compatible(node, "apple,asc-mem") &&
	       of_property_read_bool(node, "apple,firmware-read-only");
}

/* The caller supplies the usual OF/IOMMU types and the opt-in DT policy. */
static inline struct iommu_resv_region *
of_iommu_alloc_firmware_region(phys_addr_t phys, phys_addr_t iova, size_t length,
			      enum iommu_resv_type type, bool coherent,
			      bool read_only)
{
	int prot = IOMMU_READ;

	if (!read_only)
		prot |= IOMMU_WRITE;
	if (coherent)
		prot |= IOMMU_CACHE;
	if (type == IOMMU_RESV_TRANSLATED)
		return iommu_alloc_resv_region_tr(phys, iova, length, prot, type,
						 GFP_KERNEL);
	return iommu_alloc_resv_region(iova, length, prot, type, GFP_KERNEL);
}

#endif
