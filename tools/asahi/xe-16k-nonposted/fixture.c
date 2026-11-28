// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t resource_size_t;

#define __force
#define PAGE_SHIFT 14U
#define PAGE_SIZE (1ULL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define SZ_4K 4096ULL
#define SZ_64K 65536ULL
#define XE_PTE_SHIFT 12U
#define XE_PAGE_SIZE (1ULL << XE_PTE_SHIFT)
#define XE_PTE_MASK (XE_PAGE_SIZE - 1)
#define XE_PTE_PS64 (1ULL << 8)
#define XE_VM_FLAG_64K (1U << 0)
#define XE_VRAM_FLAGS_NEED64K (1U << 0)
#define XE_BO_FLAG_SYSTEM (1U << 0)
#define DRM_XE_ATOMIC_GLOBAL 1
#define DRM_XE_ATOMIC_CPU 2
#define LMEM_BAR 2
#define IORESOURCE_MEM_NONPOSTED (1UL << 7)
#define TTM_PL_FLAG_CONTIGUOUS (1U << 0)
#define MAX_PTE_PER_SDI 64U
#define MI_STORE_DATA_IMM 0x20000000U
#define MI_SDI_NUM_QW(n) ((n) * 2U)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define lower_32_bits(x) ((u32)(x))
#define upper_32_bits(x) ((u32)((x) >> 32))
#define FIELD_PREP(mask, val) (((u32)(val)) & (mask))
#define GUC_ADS_ADDR 0xffffffffU
#define IS_ENABLED(x) 0
#define xe_assert(xe, cond) do { (void)(xe); assert(cond); } while (0)

enum ttm_caching {
	ttm_uncached,
	ttm_write_combined,
	ttm_cached,
};

enum {
	XE_PL_SYSTEM = 0,
	XE_PL_TT = 1,
	XE_PL_VRAM0 = 2,
	XE_PL_VRAM1 = 3,
	XE_PL_STOLEN = 4,
};

struct pci_dev {
	unsigned long bar_flags[6];
};

struct xe_device {
	struct {
		struct pci_dev *dev;
	} drm;
	struct {
		u32 vram_flags;
	} info;
};

struct xe_tile {
	struct xe_device *xe;
};

struct xe_pt_ops {
	u64 (*pte_encode_addr)(struct xe_device *xe, u64 addr, u16 pat_index,
			       u32 pt_level, bool devmem, u64 flags);
};

struct xe_vm {
	u32 flags;
	const struct xe_pt_ops *pt_ops;
};

struct xe_exec_queue {
	struct xe_vm *vm;
};

struct xe_migrate {
	struct xe_tile *tile;
	struct xe_exec_queue *q;
};

struct xe_bb {
	u32 cs[256];
	u32 len;
};

struct xe_res_cursor {
	u64 addr;
	u64 remaining;
};

struct ttm_tt {
	enum ttm_caching caching;
};

struct ttm_buffer_object {
	struct ttm_tt *ttm;
};

struct ttm_resource {
	u32 mem_type;
	u64 start;
	u32 placement;
	struct ttm_buffer_object *bo;
	struct {
		u64 offset;
		void *addr;
		bool is_iomem;
		bool is_nonposted;
		enum ttm_caching caching;
	} bus;
};

struct xe_vram_region {
	resource_size_t io_start;
	u8 *mapping;
};

struct xe_bo {
	struct ttm_buffer_object ttm;
	struct xe_device *xe;
	u32 flags;
	bool pinned;
	struct {
		u32 atomic_access;
	} attr;
};

struct xe_guc {
	struct {
		void *bo;
	} ads;
};

struct ttm_device {
	struct xe_device *xe;
};

static struct xe_vram_region mock_vram;
static u64 mock_ads_ggtt;
static u64 encoded_ptes[32];
static size_t encoded_count;

static struct pci_dev *to_pci_dev(struct pci_dev *d) { return d; }
static unsigned long pci_resource_flags(struct pci_dev *p, int bar) { return p->bar_flags[bar]; }
static struct xe_device *tile_to_xe(struct xe_tile *t) { return t->xe; }
static struct xe_device *ttm_to_xe_device(struct ttm_device *b) { return b->xe; }
static struct xe_bo *ttm_to_xe_bo(struct ttm_buffer_object *b) { return (struct xe_bo *)b; }
static struct xe_device *xe_bo_device(struct xe_bo *bo) { return bo->xe; }
static __attribute__((unused)) bool xe_bo_is_pinned(struct xe_bo *bo) { return bo->pinned; }
static u64 guc_bo_ggtt_addr(struct xe_guc *g, void *bo) { (void)g; (void)bo; return mock_ads_ggtt; }
static u16 xe_migrate_pat_index(struct xe_device *x, enum ttm_caching c, bool comp)
{ (void)x; (void)c; (void)comp; return 0; }
static u64 xe_res_dma(struct xe_res_cursor *cur) { return cur->addr; }
static void xe_res_next(struct xe_res_cursor *cur, u64 step)
{
	assert(step > 0 && step <= cur->remaining);
	cur->addr += step;
	cur->remaining -= step;
}
static u64 vram_region_gpu_offset(struct ttm_resource *res) { (void)res; return 0; }
static struct xe_vram_region *xe_map_resource_to_region(struct ttm_resource *res)
{ (void)res; return &mock_vram; }
static bool xe_ttm_resource_visible(struct xe_device *xe, struct ttm_resource *res)
{ (void)xe; (void)res; return true; }
static int xe_ttm_stolen_io_mem_reserve(struct xe_device *xe, struct ttm_resource *mem)
{ (void)xe; (void)mem; return 0; }

#include "extracted.h"

static u64 mock_encode_addr(struct xe_device *xe, u64 addr, u16 pat_index,
			    u32 pt_level, bool devmem, u64 flags)
{
	(void)xe; (void)pat_index; (void)pt_level; (void)devmem; (void)flags;
	assert(encoded_count < 32);
	encoded_ptes[encoded_count++] = addr;
	return addr | 1ULL;
}

static void test_16k_assumptions(void)
{
	struct xe_device xe = { 0 };
	struct xe_tile tile = { .xe = &xe };
	static const struct xe_pt_ops pt_ops = { .pte_encode_addr = mock_encode_addr };
	struct xe_vm vm = { .flags = 0, .pt_ops = &pt_ops };
	struct xe_exec_queue q = { .vm = &vm };
	struct xe_migrate m = { .tile = &tile, .q = &q };
	struct xe_bb bb = { 0 };
	struct xe_bo bo = { .xe = &xe };
	struct ttm_resource res = { .bo = &bo.ttm };
	struct xe_res_cursor cur = { .addr = 0x10000000ULL, .remaining = PAGE_SIZE };
	struct xe_guc guc = { 0 };

	/* RING_CTL_SIZE must subtract 4 KiB, not 16 KiB PAGE_SIZE */
	assert(RING_CTL_SIZE(SZ_4K) == 0);
	assert(RING_CTL_SIZE(PAGE_SIZE) == PAGE_SIZE - SZ_4K);

	/* GuC ADS GGTT address must be shifted by 4 KiB (XE_PTE_SHIFT = 12) */
	mock_ads_ggtt = 0x12345000ULL;
	assert(guc_ctl_ads_flags(&guc) == (0x12345000U >> 12));

	/* emit_pte must emit four distinct 4 KiB GPU PTEs for one 16 KiB host page */
	encoded_count = 0;
	emit_pte(&m, &bb, 0, false, false, &cur, PAGE_SIZE, &res);
	assert(encoded_count == 4);
	assert(encoded_ptes[0] == 0x10000000ULL);
	assert(encoded_ptes[1] == 0x10001000ULL);
	assert(encoded_ptes[2] == 0x10002000ULL);
	assert(encoded_ptes[3] == 0x10003000ULL);
	assert(cur.remaining == 0);

	/* Query MIN_ALIGNMENT must report at least PAGE_SIZE (16 KiB) */
	xe.info.vram_flags = 0;
	assert(test_query_min_alignment(&xe) == PAGE_SIZE);
	xe.info.vram_flags = XE_VRAM_FLAGS_NEED64K;
	assert(test_query_min_alignment(&xe) == SZ_64K);
}

static void test_nonposted_bar(void)
{
	struct pci_dev pdev = { 0 };
	struct xe_device xe = { .drm = { .dev = &pdev } };
	struct ttm_device bdev = { .xe = &xe };
	struct ttm_resource mem = { .mem_type = XE_PL_VRAM0, .start = 1 };
	struct xe_bo bo = { .xe = &xe, .flags = XE_BO_FLAG_SYSTEM, .pinned = false };

	pdev.bar_flags[LMEM_BAR] = IORESOURCE_MEM_NONPOSTED;
	mock_vram.io_start = 0x80000000ULL;
	mock_vram.mapping = NULL;

	assert(xe_ttm_io_mem_reserve(&bdev, &mem) == 0);
	assert(mem.bus.is_iomem);
	assert(mem.bus.is_nonposted);
	assert(mem.bus.caching == ttm_uncached);

	/* Unpinned VRAM+SYSTEM BO migrates to SMEM on non-posted LMEM BAR */
	assert(should_migrate_to_smem(&bo));
	/* Pinned BO does not migrate */
	bo.pinned = true;
	assert(!should_migrate_to_smem(&bo));
	/* Posted BAR does not force migration */
	bo.pinned = false;
	pdev.bar_flags[LMEM_BAR] = 0;
	assert(!should_migrate_to_smem(&bo));
}

int main(void)
{
	test_16k_assumptions();
	test_nonposted_bar();
	puts("xe-16k-nonposted: pass");
	return 0;
}
