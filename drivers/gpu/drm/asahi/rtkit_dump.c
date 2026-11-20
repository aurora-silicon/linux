// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/devcoredump.h>
#include <linux/vmalloc.h>

void asahi_save_rtkit_dump(struct device *dev, const void *data, size_t size);

/* RTKit owns its immutable shadow only during the crash callback. */
void asahi_save_rtkit_dump(struct device *dev, const void *data, size_t size)
{
	void *copy;

	if (!size)
		return;
	copy = vmalloc(size);
	if (copy) {
		memcpy(copy, data, size);
		dev_coredumpv(dev, copy, size, GFP_KERNEL);
	}
}

#include <linux/io.h>
#include <linux/ioport.h>

void *asahi_map_fw_buffer(phys_addr_t phys, size_t size);
void asahi_unmap_fw_buffer(void *ptr);

/*
 * A firmware-placed buffer outside a described region: map it only where it is no part of
 * the kernel's RAM, i.e. firmware memory the boot loader carved out.
 */
void *asahi_map_fw_buffer(phys_addr_t phys, size_t size)
{
	if (!size || region_intersects(phys, size, IORESOURCE_SYSTEM_RAM, IORES_DESC_NONE) !=
	    REGION_DISJOINT)
		return NULL;
	return memremap(phys, size, MEMREMAP_WB);
}

void asahi_unmap_fw_buffer(void *ptr)
{
	memunmap(ptr);
}
