// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 */

#include <linux/gfp.h>
#include <linux/cache.h>
#include <linux/dma-map-ops.h>
#include <linux/property.h>
#include <xen/xen.h>

#include <asm/cacheflush.h>
#include <asm/xen/xen-ops.h>

void arch_sync_dma_for_device(phys_addr_t paddr, size_t size,
			      enum dma_data_direction dir)
{
	unsigned long start = (unsigned long)phys_to_virt(paddr);

	dcache_clean_poc_nosync(start, start + size);
}

void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size,
			   enum dma_data_direction dir)
{
	unsigned long start = (unsigned long)phys_to_virt(paddr);

	if (dir == DMA_TO_DEVICE)
		return;

	dcache_inval_poc_nosync(start, start + size);
}

void arch_dma_prep_coherent(struct page *page, size_t size)
{
	unsigned long start = (unsigned long)page_address(page);

	dcache_clean_poc(start, start + size);
}

void arch_setup_dma_ops(struct device *dev, bool coherent)
{
	int cls = cache_line_size_of_cpu();

	/*
	 * A device explicitly marked "dma-noncoherent" keeps non-coherent DMA:
	 * its driver may depend on the non-cacheable attributes of that path.
	 */
	if (!coherent && !CLIDR_LOC(read_sysreg(clidr_el1)) &&
	    !device_property_present(dev, "dma-noncoherent")) {
		/*
		 * This holds for every other device on the system, so say it once
		 * and keep the per-device line for debugging.
		 */
		pr_info_once("CLIDR_EL1.LoC == 0, treating devices not marked dma-noncoherent as DMA coherent\n");
		dev_dbg(dev, "CLIDR_EL1.LoC == 0, treating as coherent\n");
		coherent = true;
	}

	WARN_TAINT(!coherent && cls > ARCH_DMA_MINALIGN,
		   TAINT_CPU_OUT_OF_SPEC,
		   "%s %s: ARCH_DMA_MINALIGN smaller than CTR_EL0.CWG (%d < %d)",
		   dev_driver_string(dev), dev_name(dev),
		   ARCH_DMA_MINALIGN, cls);

	dev_assign_dma_coherent(dev, coherent);

	xen_setup_dma_ops(dev);
}
