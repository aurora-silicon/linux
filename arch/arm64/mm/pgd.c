// SPDX-License-Identifier: GPL-2.0-only
/*
 * PGD allocation/freeing
 *
 * Copyright (C) 2012 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 */

#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/slab.h>

#include <asm/pgalloc.h>
#include <asm/page.h>
#include <asm/tlbflush.h>

static struct kmem_cache *pgd_cache __ro_after_init;
#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
static struct kmem_cache *user4k_pgd_cache __ro_after_init;
static struct kmem_cache *user16k_pgd_cache __ro_after_init;
static struct kmem_cache *user64k_pgd_cache __ro_after_init;
#endif

static bool pgdir_is_page_size(void)
{
	if (PGD_SIZE == PAGE_SIZE)
		return true;
	if (ARM64_NATIVE_PGTABLE_LEVELS == 4)
		return !pgtable_l4_enabled();
	if (ARM64_NATIVE_PGTABLE_LEVELS == 5)
		return !pgtable_l5_enabled();
	return false;
}

pgd_t *pgd_alloc(struct mm_struct *mm)
{
	gfp_t gfp = GFP_PGTABLE_USER;

#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
	if (arm64_context_user4k(&mm->context))
		return kmem_cache_alloc(user4k_pgd_cache, gfp);
	if (arm64_mm_alt_granule(mm) && mm_page_shift(mm) == 14)
		return kmem_cache_alloc(user16k_pgd_cache, gfp);
	if (arm64_mm_alt_granule(mm) && mm_page_shift(mm) == 16)
		return kmem_cache_alloc(user64k_pgd_cache, gfp);
#endif
	if (pgdir_is_page_size())
		return __pgd_alloc(mm, 0);
	else
		return kmem_cache_alloc(pgd_cache, gfp);
}

void pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
	if (arm64_context_user4k(&mm->context)) {
		kmem_cache_free(user4k_pgd_cache, pgd);
		return;
	}
	if (arm64_mm_alt_granule(mm) && mm_page_shift(mm) == 14) {
		kmem_cache_free(user16k_pgd_cache, pgd);
		return;
	}
	if (arm64_mm_alt_granule(mm) && mm_page_shift(mm) == 16) {
		kmem_cache_free(user64k_pgd_cache, pgd);
		return;
	}
#endif
	if (pgdir_is_page_size())
		__pgd_free(mm, pgd);
	else
		kmem_cache_free(pgd_cache, pgd);
}

void __init pgtable_cache_init(void)
{
#ifdef CONFIG_ARM64_USER4K_EXPERIMENTAL
	if (PAGE_SHIFT < 16)
		user64k_pgd_cache = kmem_cache_create("user64k_pgd_cache", SZ_64K, SZ_64K,
						 SLAB_PANIC, NULL);
	if (PAGE_SHIFT != 14)
		user16k_pgd_cache = kmem_cache_create("user16k_pgd_cache", SZ_16K, SZ_16K,
						 SLAB_PANIC, NULL);
	user4k_pgd_cache = kmem_cache_create("user4k_pgd_cache", SZ_4K, SZ_4K,
					   SLAB_PANIC, NULL);
#endif
	if (pgdir_is_page_size())
		return;

#ifdef CONFIG_ARM64_PA_BITS_52
	/*
	 * With 52-bit physical addresses, the architecture requires the
	 * top-level table to be aligned to at least 64 bytes.
	 */
	BUILD_BUG_ON(!IS_ALIGNED(PGD_SIZE, 64));
#endif

	/*
	 * Naturally aligned pgds required by the architecture.
	 */
	pgd_cache = kmem_cache_create("pgd_cache", PGD_SIZE, PGD_SIZE,
				      SLAB_PANIC, NULL);
}
