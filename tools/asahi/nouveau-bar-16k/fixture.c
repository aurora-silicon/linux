// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
typedef uint64_t resource_size_t;
typedef int vm_fault_t;

#define __iomem
#define __force
#define likely(x) (x)
#define unlikely(x) (x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PAGE_SHIFT 14U
#define PAGE_SIZE (1ULL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PFN_UP(x) (((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)

#define TTM_PL_SYSTEM 0
#define TTM_PL_TT 1
#define TTM_PL_VRAM 2
#define NV_DEVICE_INFO_V0_TESLA 0x50
#define NOUVEAU_GEM_DOMAIN_VRAM (1U << 1)
#define NOUVEAU_GEM_DOMAIN_GART (1U << 2)
#define VM_FAULT_NOPAGE 0x100
#define VM_FAULT_SIGBUS 0x002
#ifndef ERESTARTSYS
#define ERESTARTSYS 512
#endif

enum nvkm_bar_id {
	NVKM_BAR0_PRI,
	NVKM_BAR1_FB,
	NVKM_BAR2_INST,
};

struct nvkm_device;

struct nvkm_device_func {
	resource_size_t (*resource_addr)(struct nvkm_device *, enum nvkm_bar_id);
	resource_size_t (*resource_size)(struct nvkm_device *, enum nvkm_bar_id);
	bool (*resource_nonposted)(struct nvkm_device *, enum nvkm_bar_id);
};

struct nvkm_device {
	const struct nvkm_device_func *func;
	resource_size_t bar_start[3];
	resource_size_t bar_len[3];
	bool bar_np[3];
};

struct nvkm_vmm_map {
	dma_addr_t *dma;
	u64 offset;
	u64 off;
};

struct ttm_resource {
	u32 mem_type;
	u64 start;
	u64 size;
};

struct ttm_place {
	u32 fpfn;
	u32 lpfn;
};

struct ttm_buffer_object {
	void *bdev;
	struct ttm_resource *resource;
	u32 pin_count;
};

struct nouveau_bo {
	struct ttm_buffer_object bo;
	u8 kind;
	u32 valid_domains;
	u32 placed_domain;
	struct {
		int num_placement;
	} placement;
	struct ttm_place placements[2];
};

struct nouveau_drm {
	struct nvkm_device *nvkm;
	struct {
		struct {
			struct {
				u32 family;
			} info;
		} device;
	} client;
};

struct nvif_push {
	u32 *bgn;
	u32 *cur;
	u32 *seg;
	u32 *end;
	bool is_iomem;
};

static u32 vram_pushbuf[16];
static unsigned int io_write32_calls;
static unsigned int io_memcpy_to_calls;
static unsigned int validate_calls;

static __attribute__((unused)) void *checked_memcpy(void *dst, const void *src, size_t n)
{
	assert(dst < (void *)vram_pushbuf ||
	       dst >= (void *)(vram_pushbuf + ARRAY_SIZE(vram_pushbuf)));
	return memcpy(dst, src, n);
}

#define memcpy checked_memcpy

static __attribute__((unused)) void iowrite32_native(u32 val, void __iomem *addr)
{
	io_write32_calls++;
	*(u32 *)addr = val;
}

static __attribute__((unused)) void memcpy_toio(void __iomem *dst, const void *src, size_t n)
{
	io_memcpy_to_calls++;
	__builtin_memcpy(dst, src, n);
}

#define PUSH_ASSERT(a, s) assert(a)
#define PUSH_PRINTF(p, f, ...) ((void)(p))

static struct nouveau_drm *nouveau_bdev(void *bdev)
{
	return (struct nouveau_drm *)bdev;
}

static struct nouveau_bo *nouveau_bo(struct ttm_buffer_object *bo)
{
	return (struct nouveau_bo *)bo;
}

static struct nvkm_device *nvxx_device(struct nouveau_drm *drm)
{
	return drm->nvkm;
}

static void nouveau_bo_placement_set(struct nouveau_bo *nvbo, u32 domain, u32 busy)
{
	(void)busy;
	nvbo->placed_domain = domain;
}

static int nouveau_bo_validate(struct nouveau_bo *nvbo, bool intr, bool no_wait)
{
	(void)nvbo;
	(void)intr;
	(void)no_wait;
	validate_calls++;
	return 0;
}

static void ttm_bo_move_to_lru_tail_unlocked(struct ttm_buffer_object *bo)
{
	(void)bo;
}

#include "extracted.h"

static resource_size_t mock_addr(struct nvkm_device *d, enum nvkm_bar_id b)
{
	return d->bar_start[b];
}

static resource_size_t mock_size(struct nvkm_device *d, enum nvkm_bar_id b)
{
	return d->bar_len[b];
}

static bool mock_np(struct nvkm_device *d, enum nvkm_bar_id b)
{
	return d->bar_np[b];
}

static void test_addr_nonposted(void)
{
	static const struct nvkm_device_func func = {
		.resource_addr = mock_addr,
		.resource_size = mock_size,
		.resource_nonposted = mock_np,
	};
	struct nvkm_device dev = {
		.func = &func,
		.bar_start = { 0x10000000ULL, 0x20000000ULL, 0x30000000ULL },
		.bar_len = { 0x1000000ULL, 0x10000000ULL, 0x2000000ULL },
		.bar_np = { true, true, false },
	};

	assert(nvkm_device_addr_nonposted(&dev, 0x10001000ULL, 0x1000));
	assert(nvkm_device_addr_nonposted(&dev, 0x20000000ULL, 0x4000));
	assert(!nvkm_device_addr_nonposted(&dev, 0x30000000ULL, 0x1000));
	assert(!nvkm_device_addr_nonposted(&dev, 0x10ffffffULL, 0x100));
}

static void test_vmm_dma_16k_offset(void)
{
	dma_addr_t dmas[4] = { 0x10000, 0x20000, 0x30000, 0x40000 };
	struct nvkm_vmm_map map;

	/* 4 KiB sub-page offset inside first 16 KiB host page */
	map = (struct nvkm_vmm_map){ .dma = dmas, .offset = 0x1000 };
	run_vmm_dma_offset(&map);
	assert(map.dma == &dmas[0] && map.off == 0x1000);

	/* Page-aligned 16 KiB offset must yield dma[1], off == 0 */
	map = (struct nvkm_vmm_map){ .dma = dmas, .offset = 0x4000 };
	run_vmm_dma_offset(&map);
	assert(map.dma == &dmas[1] && map.off == 0);

	/* 20 KiB offset -> dma[1], off == 4 KiB */
	map = (struct nvkm_vmm_map){ .dma = dmas, .offset = 0x5000 };
	run_vmm_dma_offset(&map);
	assert(map.dma == &dmas[1] && map.off == 0x1000);
}

static void test_fault_migrate_nonposted(void)
{
	static const struct nvkm_device_func func = {
		.resource_addr = mock_addr,
		.resource_size = mock_size,
		.resource_nonposted = mock_np,
	};
	struct nvkm_device dev = {
		.func = &func,
		.bar_len = { 0x1000000ULL, 0x10000000ULL, 0x2000000ULL },
		.bar_np = { true, true, true },
	};
	struct nouveau_drm drm = {
		.nvkm = &dev,
		.client = { .device = { .info = { .family = NV_DEVICE_INFO_V0_TESLA } } },
	};
	struct ttm_resource res = { .mem_type = TTM_PL_VRAM, .start = 0, .size = PAGE_SIZE };
	struct nouveau_bo nvbo = {
		.bo = { .bdev = &drm, .resource = &res, .pin_count = 0 },
		.valid_domains = NOUVEAU_GEM_DOMAIN_VRAM | NOUVEAU_GEM_DOMAIN_GART,
	};

	validate_calls = 0;
	assert(nouveau_ttm_fault_reserve_notify(&nvbo.bo) == 0);
	assert(nvbo.placed_domain == NOUVEAU_GEM_DOMAIN_GART);
	assert(validate_calls == 1);

	/* Pinned VRAM BO stays in VRAM */
	nvbo.bo.pin_count = 1;
	nvbo.placed_domain = 0;
	validate_calls = 0;
	assert(nouveau_ttm_fault_reserve_notify(&nvbo.bo) == 0);
	assert(nvbo.placed_domain == 0 && validate_calls == 0);
}

static void test_push_iomem(void)
{
	struct nvif_push push = {
		.bgn = vram_pushbuf,
		.cur = vram_pushbuf,
		.seg = vram_pushbuf + ARRAY_SIZE(vram_pushbuf),
		.end = vram_pushbuf + ARRAY_SIZE(vram_pushbuf),
		.is_iomem = true,
	};
	const u32 payload[3] = { 0x11223344U, 0x55667788U, 0x99aabbccU };

	io_write32_calls = 0;
	io_memcpy_to_calls = 0;
	PUSH_DATA(&push, 0xdeadbeefU);
	PUSH_DATAp(X, &push, 0, 0, 0, payload, 3, "");
	assert(io_write32_calls == 1);
	assert(io_memcpy_to_calls == 1);
	assert(vram_pushbuf[0] == 0xdeadbeefU);
	assert(vram_pushbuf[1] == 0x11223344U);
	assert(vram_pushbuf[3] == 0x99aabbccU);
}

int main(void)
{
	test_addr_nonposted();
	test_vmm_dma_16k_offset();
	test_fault_migrate_nonposted();
	test_push_iomem();
	puts("nouveau-bar-16k: pass");
	return 0;
}
