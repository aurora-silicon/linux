#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Execute the production dispext SID0 1-slice and 2-slice window and scanout helpers on host."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FUNCTIONS = (
    'apple_dart_dcp_arena_find',
    'apple_dart_dcp_arena_claim',
    '__apple_dart_dcp_dispext_scanout_alloc',
    '__apple_dart_dcp_dispext_window_claim',
    'apple_dart_dcp_window_root',
    '__apple_dart_dcp_dispext_window_map',
    '__apple_dart_dcp_dispext_window_unmap',
)


def extract_function(text, name):
    match = re.search(r'^static (?:int|void \*)\s*' + re.escape(name) + r'\(', text, re.M)
    if not match:
        raise RuntimeError(f'missing production function {name}')
    start = text.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


def apply_mutation(bodies, mutation):
    if mutation is None:
        return bodies
    mutated = dict(bodies)
    if mutation == 'single-slice-only':
        old = 'if (!dva || !size || size > DCP_DART_DISPEXT_MAX_SIZE)'
        new = 'if (!dva || !size || size > DCP_DART_SLICE)'
        assert old in mutated['__apple_dart_dcp_dispext_window_claim']
        mutated['__apple_dart_dcp_dispext_window_claim'] = (
            mutated['__apple_dart_dcp_dispext_window_claim'].replace(old, new, 1)
        )
    elif mutation == 'allow-second-slice-dva':
        old = 'n = dcp->window_slices[root];'
        new = 'n = dcp->window_slices[root] ?: 1;'
        assert old in mutated['apple_dart_dcp_window_root']
        mutated['apple_dart_dcp_window_root'] = (
            mutated['apple_dart_dcp_window_root'].replace(old, new, 1)
        )
    elif mutation == 'skip-second-slice-unmap':
        old = 'for (unsigned int s = 0; s < slices; s++) {'
        new = 'for (unsigned int s = 0; s < 1; s++) {'
        assert mutated['__apple_dart_dcp_dispext_window_unmap'].count(old) == 2
        # Mutate the clearing loop (second occurrence) so slice 1 leaks PTEs.
        first = mutated['__apple_dart_dcp_dispext_window_unmap'].index(old)
        second = mutated['__apple_dart_dcp_dispext_window_unmap'].index(old, first + 1)
        fn = mutated['__apple_dart_dcp_dispext_window_unmap']
        mutated['__apple_dart_dcp_dispext_window_unmap'] = (
            fn[:second] + new + fn[second + len(old):]
        )
    elif mutation == 'claim-before-alloc':
        # Reintroduce the prepublication ENOMEM slice leak by claiming before host allocation.
        fn = mutated['__apple_dart_dcp_dispext_scanout_alloc']
        old_peek = 'root = apple_dart_dcp_arena_find(dcp, slices);'
        new_peek = 'root = apple_dart_dcp_arena_claim(dcp, slices, false);'
        assert old_peek in fn
        fn = fn.replace(old_peek, new_peek, 1)
        old_claim = (
            '\tspin_lock_irqsave(&dcp->lock, flags);\n'
            '\troot = apple_dart_dcp_arena_claim(dcp, slices, false);\n'
            '\tspin_unlock_irqrestore(&dcp->lock, flags);\n'
            '\tif (root < 0) {\n'
            '\t\tresult = ERR_PTR(root);\n'
            '\t\tgoto free_unpublished;\n'
            '\t}\n'
        )
        assert old_claim in fn
        fn = fn.replace(old_claim, '', 1)
        mutated['__apple_dart_dcp_dispext_scanout_alloc'] = fn
    else:
        raise ValueError(f'unknown mutation {mutation}')
    return mutated


SHIM = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t phys_addr_t;
typedef uint64_t dma_addr_t;
typedef unsigned int gfp_t;
typedef unsigned long pgprot_t;

#define SZ_16K				(16ULL * 1024)
#define SZ_32M				(32ULL * 1024 * 1024)
#define SZ_64M				(64ULL * 1024 * 1024)
#define PAGE_SIZE			SZ_16K
#define PAGE_ALIGN(x)			(((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define DCP_DART_PAGE			SZ_16K
#define DCP_DART_SLICE			SZ_32M
#define DCP_DART_ENTRIES		2048U
#define DCP_DART_ALIAS			(1ULL << 40)
#define DCP_DART_APERTURE		(1ULL << 36)
#define DCP_DART_DISPEXT_MAX_SLICES	2U
#define DCP_DART_DISPEXT_MAX_SIZE	(DCP_DART_DISPEXT_MAX_SLICES * DCP_DART_SLICE)
#define DCP_DART_DISPEXT_SCANOUT_MAX	(6ULL * DCP_DART_SLICE)

#define BIT(n)				(1UL << (n))
#define IOMMU_READ			1
#define IOMMU_WRITE			2
#define GFP_KERNEL			0
#define __GFP_ZERO			1
#define PAGE_KERNEL			0
#define DIV_ROUND_UP(n, d)		(((n) + (d) - 1) / (d))
#define READ_ONCE(x)			(x)
#define WRITE_ONCE(x, v)		((x) = (v))
#define dma_wmb()			do { } while (0)
#define dev_err(...)			((void)0)
#define struct_size(p, member, n)	(sizeof(*(p)) + (n) * sizeof((p)->member[0]))

static inline void *ERR_PTR(long error) { return (void *)(intptr_t)error; }
static inline bool IS_ERR(const void *ptr) { return (uintptr_t)ptr >= (uintptr_t)-4095L; }
static inline long PTR_ERR(const void *ptr) { return (intptr_t)ptr; }

struct list_head {
	struct list_head *next, *prev;
};

static inline void INIT_LIST_HEAD(struct list_head *head)
{
	head->next = head;
	head->prev = head;
}

static inline void list_add_tail(struct list_head *entry, struct list_head *head)
{
	entry->next = head;
	entry->prev = head->prev;
	head->prev->next = entry;
	head->prev = entry;
}

struct page {
	phys_addr_t phys;
};

struct device { int dummy; };
struct iommu_domain { int dummy; };
struct apple_dart_dcp;
struct apple_dart_domain {
	struct iommu_domain domain;
	struct apple_dart_dcp *dcp;
};
struct apple_dart {
	struct apple_dart_dcp *frontend[2];
	bool dispext;
	bool retired;
	bool regs_bad;
	bool fault_pending;
	unsigned int fail_count;
};
struct apple_dart_dcp_allocation {
	struct list_head link;
	void *cpu;
	dma_addr_t dva;
	size_t size;
	struct page *pages[];
};
struct apple_dart_dcp {
	struct apple_dart *dart;
	int lock;
	int alloc_lock;
	struct list_head allocations;
	size_t allocated;
	unsigned int sid;
	u64 root[DCP_DART_ENTRIES];
	u64 entry[DCP_DART_ENTRIES];
	u64 *table[DCP_DART_ENTRIES];
	u64 *expected[DCP_DART_ENTRIES];
	unsigned long foreign[DCP_DART_ENTRIES / 64];
	unsigned long claimed[DCP_DART_ENTRIES / 64];
	unsigned long shared_roots[DCP_DART_ENTRIES / 64];
	unsigned long arena[DCP_DART_ENTRIES / 64];
	unsigned long window[DCP_DART_ENTRIES / 64];
	u8 window_slices[DCP_DART_ENTRIES];
};

static bool test_bit(unsigned int bit, const unsigned long *map)
{
	return (map[bit / 64] >> (bit % 64)) & 1UL;
}

static void set_bit(unsigned int bit, unsigned long *map)
{
	map[bit / 64] |= 1UL << (bit % 64);
}

static void mutex_lock(int *m) { assert(!*m); *m = 1; }
static void mutex_unlock(int *m) { assert(*m == 1); *m = 0; }
#define spin_lock_irqsave(l, f) do { assert(!*(l)); *(l) = 1; (f) = 0; } while (0)
#define spin_unlock_irqrestore(l, f) do { assert(*(l) == 1); *(l) = 0; (void)(f); } while (0)
static void lockdep_assert_held(int *l) { assert(*l == 1); }

static struct apple_dart_dcp *active_sid0;
static struct apple_dart_domain active_pio_domain;
static unsigned int flush_calls;
static int force_check_err;

/* Fault injection and accounting for __apple_dart_dcp_dispext_scanout_alloc(). */
static bool fail_kzalloc;
static int fail_alloc_page_at = -1;
static bool fail_remap;
static int race_claim_slot_on_remap = -1;
static int fail_map_at = -1;
static unsigned int alloc_page_calls;
static unsigned int live_pages;
static unsigned int live_remaps;
static unsigned int live_descriptors;
static unsigned int map_calls;
static phys_addr_t next_page_phys = 0x300000000ULL;

static bool dev_is_dma_coherent(struct device *dev) { (void)dev; return false; }

static struct iommu_domain *iommu_get_domain_for_dev(struct device *dev)
{
	(void)dev;
	return &active_pio_domain.domain;
}

static struct apple_dart_domain *to_dart_domain(struct iommu_domain *dom)
{
	(void)dom;
	return &active_pio_domain;
}

static void *kzalloc(size_t sz, gfp_t gfp)
{
	void *p;

	(void)gfp;
	if (fail_kzalloc)
		return NULL;
	p = calloc(1, sz);
	assert(p);
	live_descriptors++;
	return p;
}

static void kfree(void *p)
{
	if (!p)
		return;
	assert(live_descriptors > 0);
	live_descriptors--;
	free(p);
}

static struct page *alloc_page(gfp_t gfp)
{
	struct page *pg;

	(void)gfp;
	if (fail_alloc_page_at >= 0 && (int)alloc_page_calls == fail_alloc_page_at) {
		alloc_page_calls++;
		return NULL;
	}
	alloc_page_calls++;
	pg = malloc(sizeof(*pg));
	assert(pg);
	pg->phys = next_page_phys;
	next_page_phys += PAGE_SIZE;
	live_pages++;
	return pg;
}

static void __free_page(struct page *pg)
{
	assert(pg && live_pages > 0);
	live_pages--;
	free(pg);
}

static phys_addr_t page_to_phys(struct page *pg) { return pg->phys; }
static void arch_dma_prep_coherent(struct page *pg, size_t sz) { (void)pg; (void)sz; }
static pgprot_t dma_pgprot(struct device *dev, pgprot_t prot, unsigned long attrs)
{
	(void)dev; (void)attrs;
	return prot;
}

static void *dma_common_pages_remap(struct page **pages, size_t size, pgprot_t prot,
				    const void *caller)
{
	void *p;

	(void)pages; (void)size; (void)prot; (void)caller;
	if (fail_remap)
		return NULL;
	p = malloc(16);
	assert(p);
	live_remaps++;
	if (race_claim_slot_on_remap >= 0) {
		set_bit((unsigned int)race_claim_slot_on_remap, active_sid0->claimed);
		race_claim_slot_on_remap = -1;
	}
	return p;
}

static void dma_common_free_remap(void *cpu_addr, size_t size)
{
	(void)size;
	assert(cpu_addr && live_remaps > 0);
	live_remaps--;
	free(cpu_addr);
}

static struct apple_dart_dcp *apple_dart_dcp_dispext_sid0(struct device *piodma)
{
	return piodma ? active_sid0 : NULL;
}

static bool apple_dart_dispext_retired(struct apple_dart *dart)
{
	return dart->retired;
}

static bool apple_dart_dcp_registers(struct apple_dart_dcp *dcp)
{
	if (dcp->dart->regs_bad) {
		dcp->dart->retired = true;
		return false;
	}
	return true;
}

static bool apple_dart_fault_pending(struct apple_dart *dart)
{
	return dart->fault_pending;
}

#define apple_dart_dcp_dispext_fail(dart, ...) do { (dart)->retired = true; (dart)->fail_count++; } while (0)

static bool apple_dart_dcp_iova(u64 iova)
{
	return iova >= DCP_DART_ALIAS && iova - DCP_DART_ALIAS < DCP_DART_APERTURE;
}

static bool dcp_dart_pte_frontend(u64 pte)
{
	return (pte & 1ULL) != 0;
}

static phys_addr_t apple_dart_dcp_translate(struct apple_dart_dcp *dcp, u64 iova)
{
	unsigned int root = (iova >> 25) & 2047;
	unsigned int leaf = (iova >> 14) & 2047;
	u64 pte;

	if (!dcp->table[root])
		return 0;
	pte = dcp->table[root][leaf];
	if (!(pte & 1ULL))
		return 0;
	return (pte & ~0x3ffULL) << 4;
}

static int apple_dart_dcp_map_owned(struct apple_dart_dcp *dcp, u64 iova, phys_addr_t phys,
				    size_t size, size_t count, int prot, gfp_t gfp,
				    size_t *mapped, bool shared)
{
	unsigned int root = (iova >> 25) & 2047;
	unsigned int leaf = (iova >> 14) & 2047;
	u64 pte;

	(void)prot;
	(void)gfp;
	if (fail_map_at >= 0 && (int)map_calls == fail_map_at) {
		map_calls++;
		return -EIO;
	}
	map_calls++;
	assert(shared && size == DCP_DART_PAGE && count == 1);
	assert(test_bit(root, dcp->claimed) && test_bit(root, dcp->shared_roots));
	if (!dcp->table[root]) {
		dcp->table[root] = calloc(DCP_DART_ENTRIES, sizeof(u64));
		dcp->expected[root] = calloc(DCP_DART_ENTRIES, sizeof(u64));
		assert(dcp->table[root] && dcp->expected[root]);
		dcp->entry[root] = 0x100001ULL + root;
		dcp->root[root] = dcp->entry[root];
	}
	if (dcp->table[root][leaf])
		return -EEXIST;
	pte = (phys >> 4) | 1ULL;
	dcp->expected[root][leaf] = pte;
	dcp->table[root][leaf] = pte;
	*mapped += size;
	return 0;
}

static int apple_dart_dcp_flush(struct apple_dart_dcp *dcp)
{
	(void)dcp;
	flush_calls++;
	return 0;
}

static int __apple_dart_dcp_check(struct device *piodma)
{
	(void)piodma;
	return force_check_err;
}
'''

TESTS = r'''
static struct apple_dart_dcp pio_dcp;

static void free_dcp_tables(struct apple_dart_dcp *dcp)
{
	for (unsigned int i = 0; i < DCP_DART_ENTRIES; i++) {
		free(dcp->table[i]);
		free(dcp->expected[i]);
		dcp->table[i] = NULL;
		dcp->expected[i] = NULL;
	}
}

static void reset_fault_injectors(void)
{
	fail_kzalloc = false;
	fail_alloc_page_at = -1;
	fail_remap = false;
	race_claim_slot_on_remap = -1;
	fail_map_at = -1;
	alloc_page_calls = 0;
	map_calls = 0;
	live_pages = 0;
	live_remaps = 0;
	live_descriptors = 0;
}

static void setup_dcp(struct apple_dart *dart, struct apple_dart_dcp *dcp,
		      const unsigned int *arena_slots, size_t nslots)
{
	memset(dart, 0, sizeof(*dart));
	memset(dcp, 0, sizeof(*dcp));
	memset(&pio_dcp, 0, sizeof(pio_dcp));
	dart->dispext = true;
	dcp->dart = dart;
	dcp->sid = 0;
	INIT_LIST_HEAD(&dcp->allocations);
	pio_dcp.dart = dart;
	pio_dcp.sid = 16;
	dart->frontend[0] = dcp;
	dart->frontend[1] = &pio_dcp;
	active_pio_domain.dcp = &pio_dcp;
	for (size_t i = 0; i < nslots; i++)
		set_bit(arena_slots[i], dcp->arena);
	active_sid0 = dcp;
	flush_calls = 0;
	force_check_err = 0;
	reset_fault_injectors();
}

static unsigned int count_claimed(const struct apple_dart_dcp *dcp)
{
	unsigned int n = 0;

	for (unsigned int i = 0; i < DCP_DART_ENTRIES; i++)
		if (test_bit(i, dcp->claimed))
			n++;
	return n;
}

static void test_one_and_two_slice_lifecycle(void)
{
	static const unsigned int slots[] = { 4, 5, 6, 7, 8, 9 };
	struct apple_dart dart;
	struct apple_dart_dcp dcp;
	struct device piodma = { 0 };
	static phys_addr_t phys[4096];
	u64 dva1 = 0, dva2 = 0, dva_bad = 0;
	unsigned int r2;

	setup_dcp(&dart, &dcp, slots, sizeof(slots) / sizeof(slots[0]));
	for (unsigned int i = 0; i < 4096; i++)
		phys[i] = 0x100000000ULL + (u64)i * DCP_DART_PAGE;

	/* Invalid sizes rejected before touching the arena. */
	assert(__apple_dart_dcp_dispext_window_claim(&piodma, 0, &dva_bad) == -EINVAL);
	assert(__apple_dart_dcp_dispext_window_claim(&piodma, SZ_64M + 1, &dva_bad) == -EINVAL);
	assert(__apple_dart_dcp_dispext_window_claim(&piodma, SZ_32M, NULL) == -EINVAL);

	/* 1-slice window: accepts up to 2048 pages, rejects 2049. */
	assert(__apple_dart_dcp_dispext_window_claim(&piodma, SZ_32M, &dva1) == 0);
	assert(dva1 == DCP_DART_ALIAS + 4ULL * DCP_DART_SLICE);
	assert(dcp.window_slices[4] == 1);
	assert(__apple_dart_dcp_dispext_window_map(&piodma, dva1, phys, 2049) == -EINVAL);
	assert(__apple_dart_dcp_dispext_window_map(&piodma, dva1, phys, 2048) == 0);
	assert(__apple_dart_dcp_dispext_window_map(&piodma, dva1, phys, 1) == -EBUSY);
	assert(__apple_dart_dcp_dispext_window_unmap(&piodma, dva1) == 2048);

	/* 2-slice window for 5120x2160 (42.19 MiB) -> 2700 pages across slices 5 and 6. */
	assert(__apple_dart_dcp_dispext_window_claim(&piodma, 5120ULL * 2160 * 4, &dva2) == 0);
	assert(dva2 == DCP_DART_ALIAS + 5ULL * DCP_DART_SLICE);
	r2 = (dva2 >> 25) & 2047;
	assert(r2 == 5 && dcp.window_slices[5] == 2 && dcp.window_slices[6] == 0);
	assert(test_bit(5, dcp.claimed) && test_bit(6, dcp.claimed));
	assert(test_bit(5, dcp.window) && test_bit(6, dcp.window));

	/* Second slice DVA must never be accepted as a window base. */
	assert(__apple_dart_dcp_dispext_window_map(&piodma, dva2 + DCP_DART_SLICE, phys, 1) == -EINVAL);
	assert(__apple_dart_dcp_dispext_window_unmap(&piodma, dva2 + DCP_DART_SLICE) == -EINVAL);

	/* Map 5120x2160 (2700 pages), verify both L2 tables and exact translations. */
	assert(__apple_dart_dcp_dispext_window_map(&piodma, dva2, phys, 2700) == 0);
	assert(dcp.table[5] != NULL && dcp.table[6] != NULL);
	assert(apple_dart_dcp_translate(&dcp, dva2) == phys[0]);
	assert(apple_dart_dcp_translate(&dcp, dva2 + 2047ULL * DCP_DART_PAGE) == phys[2047]);
	assert(apple_dart_dcp_translate(&dcp, dva2 + 2048ULL * DCP_DART_PAGE) == phys[2048]);
	assert(apple_dart_dcp_translate(&dcp, dva2 + 2699ULL * DCP_DART_PAGE) == phys[2699]);
	assert(apple_dart_dcp_translate(&dcp, dva2 + 2700ULL * DCP_DART_PAGE) == 0);
	assert(__apple_dart_dcp_dispext_window_map(&piodma, dva2, phys, 1) == -EBUSY);

	/* Unmap clears both slices and returns exact count; slice 6 has zero left. */
	assert(__apple_dart_dcp_dispext_window_unmap(&piodma, dva2) == 2700);
	for (unsigned int i = 0; i < DCP_DART_ENTRIES; i++) {
		assert(dcp.table[5][i] == 0 && dcp.expected[5][i] == 0);
		assert(dcp.table[6][i] == 0 && dcp.expected[6][i] == 0);
	}

	/* Remap full 64 MiB (4096 pages), reject 4097, then unmap. */
	assert(__apple_dart_dcp_dispext_window_map(&piodma, dva2, phys, 4097) == -EINVAL);
	assert(__apple_dart_dcp_dispext_window_map(&piodma, dva2, phys, 4096) == 0);
	assert(apple_dart_dcp_translate(&dcp, dva2 + 4095ULL * DCP_DART_PAGE) == phys[4095]);
	assert(__apple_dart_dcp_dispext_window_unmap(&piodma, dva2) == 4096);

	/* 2-slice window reused for <= 32 MiB before slice 1 table exists. */
	u64 dva3 = 0;
	assert(__apple_dart_dcp_dispext_window_claim(&piodma, 5120ULL * 2880 * 4, &dva3) == 0);
	assert(dva3 == DCP_DART_ALIAS + 7ULL * DCP_DART_SLICE);
	assert(__apple_dart_dcp_dispext_window_map(&piodma, dva3, phys, 128) == 0);
	assert(dcp.table[7] != NULL && dcp.table[8] == NULL);
	assert(__apple_dart_dcp_dispext_window_unmap(&piodma, dva3) == 128);
	assert(!dart.retired);

	free_dcp_tables(&dcp);
	puts("PASS 1-slice and 2-slice window claim, cross-slice map, unmap, and reuse");
}

static void test_fragmented_arena_refuses_two_slices(void)
{
	static const unsigned int slots[] = { 2, 4, 6 };
	struct apple_dart dart;
	struct apple_dart_dcp dcp;
	struct device piodma = { 0 };
	u64 dva = 0;
	void *cpu;

	setup_dcp(&dart, &dcp, slots, sizeof(slots) / sizeof(slots[0]));
	assert(__apple_dart_dcp_dispext_window_claim(&piodma, SZ_64M, &dva) == -ENOSPC);
	assert(dva == 0 && count_claimed(&dcp) == 0);

	/* Scanout alloc also fails fast without allocating any pages on a fragmented arena. */
	cpu = __apple_dart_dcp_dispext_scanout_alloc(&piodma, 5120ULL * 2160 * 4, &dva);
	assert(IS_ERR(cpu) && PTR_ERR(cpu) == -ENOSPC);
	assert(alloc_page_calls == 0 && count_claimed(&dcp) == 0);

	/* 1-slice claim still succeeds in the fragmented arena. */
	assert(__apple_dart_dcp_dispext_window_claim(&piodma, SZ_32M, &dva) == 0);
	assert(dva == DCP_DART_ALIAS + 2ULL * DCP_DART_SLICE);
	free_dcp_tables(&dcp);
	puts("PASS fragmented arena refuses 2-slice window and scanout without leaking slots");
}

static void test_slice1_drift_fails_closed_atomically(void)
{
	static const unsigned int slots[] = { 10, 11 };
	struct apple_dart dart;
	struct apple_dart_dcp dcp;
	struct device piodma = { 0 };
	static phys_addr_t phys[2100];
	u64 dva = 0;

	setup_dcp(&dart, &dcp, slots, sizeof(slots) / sizeof(slots[0]));
	for (unsigned int i = 0; i < 2100; i++)
		phys[i] = 0x200000000ULL + (u64)i * DCP_DART_PAGE;

	assert(__apple_dart_dcp_dispext_window_claim(&piodma, SZ_64M, &dva) == 0);
	assert(__apple_dart_dcp_dispext_window_map(&piodma, dva, phys, 2100) == 0);

	/* Corrupt a leaf in slice 1; unmap must retire the DART before clearing slice 0. */
	dcp.table[11][0] ^= 0x10ULL;
	assert(__apple_dart_dcp_dispext_window_unmap(&piodma, dva) == -EIO);
	assert(dart.retired && dart.fail_count == 1);
	assert(dcp.table[10][0] == dcp.expected[10][0] && dcp.table[10][0] != 0);

	free_dcp_tables(&dcp);
	puts("PASS slice-1 leaf drift fails closed without half-clearing slice 0");
}

static void test_scanout_enomem_retry_and_postpub_pin(void)
{
	/* Tight arena: only 2 adjacent slices (4, 5). Any leaked claim breaks retry! */
	static const unsigned int slots[] = { 4, 5 };
	const size_t size_5k2k = 5120ULL * 2160 * 4;
	const unsigned int pages_5k2k = size_5k2k / PAGE_SIZE;
	struct apple_dart dart;
	struct apple_dart_dcp dcp;
	struct device piodma = { 0 };
	u64 dva = 0;
	void *cpu;

	setup_dcp(&dart, &dcp, slots, sizeof(slots) / sizeof(slots[0]));

	/* 1. kzalloc ENOMEM: zero slices stranded, zero leaks. */
	fail_kzalloc = true;
	cpu = __apple_dart_dcp_dispext_scanout_alloc(&piodma, size_5k2k, &dva);
	assert(IS_ERR(cpu) && PTR_ERR(cpu) == -ENOMEM);
	assert(dva == 0 && count_claimed(&dcp) == 0 && dcp.allocated == 0);
	assert(!test_bit(4, dcp.shared_roots) && !test_bit(5, dcp.shared_roots));
	assert(live_descriptors == 0 && live_pages == 0 && live_remaps == 0);
	fail_kzalloc = false;

	/* 2. Mid-loop alloc_page ENOMEM (page 2050, past slice 0): all pages freed, zero claims. */
	alloc_page_calls = 0;
	fail_alloc_page_at = 2050;
	cpu = __apple_dart_dcp_dispext_scanout_alloc(&piodma, size_5k2k, &dva);
	assert(IS_ERR(cpu) && PTR_ERR(cpu) == -ENOMEM);
	assert(dva == 0 && count_claimed(&dcp) == 0 && dcp.allocated == 0);
	assert(!test_bit(4, dcp.shared_roots) && !test_bit(5, dcp.shared_roots));
	assert(live_descriptors == 0 && live_pages == 0 && live_remaps == 0);
	fail_alloc_page_at = -1;

	/* 3. dma_common_pages_remap ENOMEM: all pages & descriptor freed, zero claims. */
	fail_remap = true;
	cpu = __apple_dart_dcp_dispext_scanout_alloc(&piodma, size_5k2k, &dva);
	assert(IS_ERR(cpu) && PTR_ERR(cpu) == -ENOMEM);
	assert(dva == 0 && count_claimed(&dcp) == 0 && dcp.allocated == 0);
	assert(!test_bit(4, dcp.shared_roots) && !test_bit(5, dcp.shared_roots));
	assert(live_descriptors == 0 && live_pages == 0 && live_remaps == 0);
	fail_remap = false;

	/* 4. Immediate retry succeeds in the exact same 2-slice arena without reset. */
	cpu = __apple_dart_dcp_dispext_scanout_alloc(&piodma, size_5k2k, &dva);
	assert(!IS_ERR(cpu));
	assert(dva == DCP_DART_ALIAS + 4ULL * DCP_DART_SLICE);
	assert(test_bit(4, dcp.claimed) && test_bit(5, dcp.claimed));
	assert(dcp.table[4] != NULL && dcp.table[5] != NULL);
	assert(dcp.allocated == size_5k2k);
	assert(live_descriptors == 1 && live_pages == pages_5k2k && live_remaps == 1);
	assert(!dart.retired);
	free_dcp_tables(&dcp);

	/* 5. Concurrent slice claim between find and claim: remap + pages + descriptor unwound. */
	setup_dcp(&dart, &dcp, slots, sizeof(slots) / sizeof(slots[0]));
	dva = 0;
	race_claim_slot_on_remap = 5;
	cpu = __apple_dart_dcp_dispext_scanout_alloc(&piodma, size_5k2k, &dva);
	assert(IS_ERR(cpu) && PTR_ERR(cpu) == -ENOSPC);
	assert(dva == 0 && dcp.allocated == 0);
	assert(!test_bit(4, dcp.claimed) && test_bit(5, dcp.claimed));
	assert(live_descriptors == 0 && live_pages == 0 && live_remaps == 0);

	/* 6. Post-publication map failure keeps fail-closed retention (slices & pages pinned). */
	setup_dcp(&dart, &dcp, slots, sizeof(slots) / sizeof(slots[0]));
	dva = 0;
	fail_map_at = 2050;
	cpu = __apple_dart_dcp_dispext_scanout_alloc(&piodma, size_5k2k, &dva);
	assert(IS_ERR(cpu) && PTR_ERR(cpu) == -EIO);
	assert(dart.retired && dart.fail_count == 1);
	assert(test_bit(4, dcp.claimed) && test_bit(5, dcp.claimed));
	assert(dcp.allocated == size_5k2k);
	assert(live_descriptors == 1 && live_pages == pages_5k2k && live_remaps == 1);

	free_dcp_tables(&dcp);
	puts("PASS scanout prepublication ENOMEM unwinds cleanly for retry and post-publication fails closed");
}

int main(void)
{
	test_one_and_two_slice_lifecycle();
	test_fragmented_arena_refuses_two_slices();
	test_slice1_drift_fails_closed_atomically();
	test_scanout_enomem_retry_and_postpub_pin();
	return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path,
                        default=ROOT / 'drivers/iommu/apple-dart-dcp-shared.h')
    parser.add_argument('--mutation',
                        choices=('single-slice-only', 'allow-second-slice-dva',
                                 'skip-second-slice-unmap', 'claim-before-alloc'))
    args = parser.parse_args()

    text = args.source.read_text()
    bodies = {name: extract_function(text, name) for name in FUNCTIONS}
    bodies = apply_mutation(bodies, args.mutation)
    code = SHIM + '\n\n'.join(bodies[name] for name in FUNCTIONS) + '\n' + TESTS

    with tempfile.TemporaryDirectory(prefix='dart-dispext-window-') as directory:
        src = Path(directory) / 'test.c'
        binary = Path(directory) / 'test'
        src.write_text(code)
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        str(src), '-o', str(binary)], check=True)
        run = subprocess.run([str(binary)])
        if run.returncode:
            raise SystemExit(1)


if __name__ == '__main__':
    main()
