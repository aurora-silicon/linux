// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint64_t u64;
typedef int64_t loff_t;
typedef uint64_t dma_addr_t;
typedef uint64_t resource_size_t;

#define __iomem
#define PAGE_SHIFT 14U
#define PAGE_SIZE (1ULL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define AMDGPU_GPU_PAGE_SHIFT 12U
#define AMDGPU_GPU_PAGE_SIZE (1ULL << AMDGPU_GPU_PAGE_SHIFT)
#define AMDGPU_GPU_PAGES_IN_CPU_PAGE (PAGE_SIZE / AMDGPU_GPU_PAGE_SIZE)
#define AMDGPU_PTE_VALID (1ULL << 0)
#define AMDGPU_PTE_PRT_FLAG(adev) ((void)(adev), 0ULL)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define offset_in_page(p) ((p) & ~PAGE_MASK)
#define PAGE_ALIGN(x) (((x) + PAGE_SIZE - 1) & PAGE_MASK)
#define check_add_overflow(a, b, d) __builtin_add_overflow(a, b, d)

#define IORESOURCE_MEM (1UL << 1)
#define IORESOURCE_UNSET (1UL << 2)
#define IORESOURCE_ROM_ENABLE (1UL << 3)
#define IORESOURCE_MEM_NONPOSTED (1UL << 7)
#define PCI_ROM_RESOURCE 6

struct ttm_resource {
	int dummy;
};

struct amdgpu_res_cursor {
	uint64_t start;
	uint64_t size;
	uint64_t remaining;
};

struct amdgpu_vm_update_params {
	dma_addr_t *pages_addr;
};

struct resource {
	void *parent;
	unsigned long flags;
	resource_size_t start;
	resource_size_t len;
};

struct pci_dev {
	struct resource resource[7];
};

struct amdgpu_device {
	struct pci_dev *pdev;
	struct {
		resource_size_t aper_base;
		resource_size_t aper_size;
		resource_size_t visible_vram_size;
	} gmc;
};

static uint64_t mapped_gpu_pages[64];
static size_t mapped_count;
static unsigned int ioremap_calls;
static unsigned int ioremap_wc_calls;
static unsigned int ioremap_np_calls;
static unsigned int iounmap_calls;
static u8 fake_bar[PAGE_SIZE * 2];

static void amdgpu_res_first(struct ttm_resource *res, uint64_t start,
			     uint64_t size, struct amdgpu_res_cursor *cur)
{
	(void)res;
	cur->start = start;
	cur->size = size;
	cur->remaining = size;
}

static void amdgpu_res_next(struct amdgpu_res_cursor *cur, uint64_t size)
{
	assert(size > 0 && size <= cur->remaining);
	cur->start += size;
	cur->size -= size;
	cur->remaining -= size;
}

static uint64_t map_gart(const dma_addr_t *pages_addr, uint64_t addr)
{
	return pages_addr[addr >> PAGE_SHIFT] | (addr & ~PAGE_MASK);
}

static int amdgpu_vm_ptes_update(struct amdgpu_vm_update_params *params,
				 uint64_t start, uint64_t end,
				 uint64_t addr, uint64_t flags)
{
	uint64_t i;

	(void)flags;
	assert(end > start);
	for (i = 0; i < (end - start); i++) {
		uint64_t gpu_pfn = start + i;
		uint64_t cur = addr + i * AMDGPU_GPU_PAGE_SIZE;
		uint64_t phys = params->pages_addr ?
			map_gart(params->pages_addr, cur) : cur;

		assert(gpu_pfn < 64);
		mapped_gpu_pages[gpu_pfn] = phys;
		mapped_count++;
	}
	return 0;
}

static bool amdgpu_sriov_vf(struct amdgpu_device *adev)
{
	(void)adev;
	return true;
}

static unsigned long pci_resource_flags(struct pci_dev *pdev, int bar)
{
	return pdev->resource[bar].flags;
}

static resource_size_t pci_resource_start(struct pci_dev *pdev, int bar)
{
	return pdev->resource[bar].start;
}

static resource_size_t pci_resource_len(struct pci_dev *pdev, int bar)
{
	return pdev->resource[bar].len;
}

static int pci_assign_resource(struct pci_dev *pdev, int resno)
{
	(void)pdev;
	(void)resno;
	return 0;
}

static int pci_enable_rom(struct pci_dev *pdev)
{
	(void)pdev;
	return 0;
}

static void pci_disable_rom(struct pci_dev *pdev)
{
	(void)pdev;
}

static size_t pci_get_rom_size(struct pci_dev *pdev, void __iomem *rom,
			       size_t size)
{
	(void)pdev;
	(void)rom;
	return size;
}

static void __iomem *ioremap(resource_size_t start, size_t size)
{
	(void)start;
	(void)size;
	ioremap_calls++;
	return fake_bar;
}

static void __iomem *ioremap_wc(resource_size_t start, size_t size)
{
	(void)start;
	(void)size;
	ioremap_wc_calls++;
	return fake_bar;
}

static void __iomem *ioremap_np(resource_size_t start, size_t size)
{
	(void)start;
	(void)size;
	ioremap_np_calls++;
	return fake_bar;
}

static void iounmap(void __iomem *p)
{
	assert(p == fake_bar);
	iounmap_calls++;
}

static void memcpy_fromio(void *dst, const void __iomem *src, size_t len)
{
	memcpy(dst, src, len);
}

#include "extracted.h"

static void test_vm_16k_subpage_mapping(void)
{
	struct amdgpu_vm_update_params params = { 0 };
	struct amdgpu_device adev = { 0 };
	/* Host pages: 0 and 1 contiguous, 2 discontiguous, 3 discontiguous. */
	dma_addr_t pages[4] = {
		0x10000000ULL,
		0x10004000ULL,
		0x20000000ULL,
		0x30000000ULL,
	};

	/* Case 1: 8 KiB mapping at 4 KiB offset inside host page 0. */
	memset(mapped_gpu_pages, 0, sizeof(mapped_gpu_pages));
	mapped_count = 0;
	assert(run_vm_update_loop(&params, &adev, NULL, pages,
				  0x1000, 0, 1, AMDGPU_PTE_VALID, 0) == 0);
	assert(mapped_count == 2);
	assert(mapped_gpu_pages[0] == 0x10001000ULL);
	assert(mapped_gpu_pages[1] == 0x10002000ULL);

	/*
	 * Case 2: 32 KiB (8 x 4K GPU pages) starting at 4 KiB offset in page 0.
	 * Spans pages 0 (3 GPU pages), 1 (4 GPU pages, contiguous with 0),
	 * and 2 (1 GPU page, discontiguous!).
	 */
	memset(mapped_gpu_pages, 0, sizeof(mapped_gpu_pages));
	mapped_count = 0;
	assert(run_vm_update_loop(&params, &adev, NULL, pages,
				  0x1000, 0, 7, AMDGPU_PTE_VALID, 0) == 0);
	assert(mapped_count == 8);
	for (uint64_t i = 0; i < 7; i++)
		assert(mapped_gpu_pages[i] == 0x10001000ULL + i * 0x1000ULL);
	assert(mapped_gpu_pages[7] == 0x20000000ULL);

	/*
	 * Case 3: 16 KiB (4 x 4K GPU pages) starting at 4 KiB offset in page 1,
	 * crossing into discontiguous page 2.
	 */
	memset(mapped_gpu_pages, 0, sizeof(mapped_gpu_pages));
	mapped_count = 0;
	assert(run_vm_update_loop(&params, &adev, NULL, pages,
				  0x5000, 0, 3, AMDGPU_PTE_VALID, 0) == 0);
	assert(mapped_count == 4);
	assert(mapped_gpu_pages[0] == 0x10005000ULL);
	assert(mapped_gpu_pages[1] == 0x10006000ULL);
	assert(mapped_gpu_pages[2] == 0x10007000ULL);
	assert(mapped_gpu_pages[3] == 0x20000000ULL);
}

static void test_nonposted_rom_and_bar0(void)
{
	struct pci_dev pdev = { 0 };
	struct amdgpu_device adev = { .pdev = &pdev };
	size_t rom_sz = 0;
	u8 out[16];

	pdev.resource[PCI_ROM_RESOURCE].parent = (void *)1;
	pdev.resource[PCI_ROM_RESOURCE].start = 0x80000000ULL;
	pdev.resource[PCI_ROM_RESOURCE].len = 0x10000;
	pdev.resource[PCI_ROM_RESOURCE].flags = IORESOURCE_MEM | IORESOURCE_MEM_NONPOSTED;

	ioremap_calls = 0;
	ioremap_np_calls = 0;
	assert(pci_map_rom(&pdev, &rom_sz) == fake_bar);
	assert(ioremap_np_calls == 1 && ioremap_calls == 0);

	pdev.resource[0].start = 0x90000000ULL;
	pdev.resource[0].len = PAGE_SIZE * 2;
	pdev.resource[0].flags = IORESOURCE_MEM | IORESOURCE_MEM_NONPOSTED;
	memset(fake_bar, 0xab, sizeof(fake_bar));
	ioremap_wc_calls = 0;
	ioremap_np_calls = 0;
	assert(amdgpu_device_read_fb_via_bar0(&adev, 0x100, out, sizeof(out)) == 0);
	assert(ioremap_np_calls == 1 && ioremap_wc_calls == 0);
	assert(out[0] == 0xab && out[15] == 0xab);
}

int main(void)
{
	test_vm_16k_subpage_mapping();
	test_nonposted_rom_and_bar0();
	puts("amdgpu-vm-16k: pass");
	return 0;
}
