// SPDX-License-Identifier: GPL-2.0
/*
 * Confined executable memory.
 *
 * Some platforms can only execute privileged code whose *physical* address
 * lies inside a fixed, hardware-enforced window (for example an Apple-silicon
 * CTXR "code text execute range" armed by the boot firmware). To run at such a
 * privilege level the kernel must keep every executable physical page inside
 * that window: the static image is placed there by the bootloader, and every
 * run-time executable allocation (modules, BPF, kprobes, ftrace trampolines)
 * must draw its backing pages from it too.
 *
 * execmem already funnels all run-time executable allocations through a small
 * set of ranges. This file backs the ranges flagged EXECMEM_CONFINE with pages
 * taken from a gen_pool carved over the window, mapped into the range's virtual
 * area so that branch-reachability (the +/-2G module window) is preserved while
 * the physical backing stays confined.
 *
 * The architecture registers the window and pool via execmem_confine_init() and
 * sets EXECMEM_CONFINE on the relevant ranges in execmem_arch_setup().
 */

#define pr_fmt(fmt) "execmem-confine: " fmt

#include <linux/execmem.h>
#include <linux/genalloc.h>
#include <linux/kasan.h>
#include <linux/kmemleak.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/set_memory.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/xarray.h>

#include <asm/tlbflush.h>

#include "internal.h"
#include "vmalloc.h"

struct confine_chunk {
	struct vm_struct	*area;
	unsigned long		pool_va;	/* linear-map VA handed out by the pool */
	size_t			size;
	struct page		**pages;
};

static phys_addr_t confine_base __ro_after_init;
static phys_addr_t confine_size __ro_after_init;
static struct gen_pool *confine_pool __ro_after_init;
static bool confine_enforce __ro_after_init;
static bool confine_ready __ro_after_init;

/* keyed by mapped-VA PFN -> struct confine_chunk */
static DEFINE_XARRAY(confine_chunks);
/* serialises pool (de)allocation and the xarray */
static DEFINE_MUTEX(confine_lock);

void __init execmem_confine_set_window(phys_addr_t base, phys_addr_t size)
{
	confine_base = base;
	confine_size = size;
}

int __init execmem_confine_init(struct gen_pool *pool, bool enforce)
{
	if (WARN_ON(confine_ready))
		return -EBUSY;
	if (WARN_ON(!pool || !confine_size))
		return -EINVAL;

	confine_pool = pool;
	confine_enforce = enforce;
	smp_wmb();
	confine_ready = true;

	{
		phys_addr_t end = confine_base + confine_size;

		pr_info("window [%pa, %pa) (%llu MiB), %s\n",
			&confine_base, &end,
			(unsigned long long)(confine_size >> 20),
			enforce ? "enforcing" : "reporting");
	}
	return 0;
}

bool execmem_confine_active(void)
{
	return confine_ready;
}

static bool phys_in_window(phys_addr_t pa)
{
	return pa >= confine_base && pa < confine_base + confine_size;
}

bool execmem_confine_pfn_in_window(unsigned long pfn)
{
	return phys_in_window(PFN_PHYS(pfn));
}

bool __init execmem_confine_overlaps(phys_addr_t start, phys_addr_t size)
{
	return start < confine_base + confine_size && confine_base < start + size;
}

/* Window pages stay reserved for good, so one that isn't went to the allocator. */
static unsigned long confine_count_freed_pages(void)
{
	unsigned long pfn, nr = 0;

	for_each_valid_pfn(pfn, PFN_DOWN(confine_base),
			   PFN_DOWN(confine_base + confine_size)) {
		if (!PageReserved(pfn_to_page(pfn)))
			nr++;
	}

	return nr;
}

void *execmem_confine_alloc(struct execmem_range *range, size_t size,
			    pgprot_t pgprot, unsigned long vm_flags)
{
	struct confine_chunk *ck;
	struct vm_struct *area;
	struct page **pages;
	unsigned long pool_va, vaddr;
	unsigned int i, nr;
	int err;

	if (WARN_ON_ONCE(!confine_ready))
		return NULL;

	size = PAGE_ALIGN(size);
	nr = size >> PAGE_SHIFT;

	/*
	 * Reserve the virtual area inside the range (then the fallback range)
	 * so module code keeps its +/-2G reachability to the kernel image; only
	 * the physical backing is confined.
	 */
	area = __get_vm_area_node(size, range->alignment, PAGE_SHIFT,
				  VM_ALLOC | vm_flags, range->start, range->end,
				  NUMA_NO_NODE, GFP_KERNEL,
				  __builtin_return_address(0));
	if (!area && range->fallback_start)
		area = __get_vm_area_node(size, range->alignment, PAGE_SHIFT,
					  VM_ALLOC | vm_flags,
					  range->fallback_start,
					  range->fallback_end, NUMA_NO_NODE,
					  GFP_KERNEL, __builtin_return_address(0));
	if (!area)
		return NULL;

	pages = kmalloc_array(nr, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		goto err_area;

	ck = kzalloc(sizeof(*ck), GFP_KERNEL);
	if (!ck)
		goto err_pages;

	mutex_lock(&confine_lock);
	pool_va = gen_pool_alloc(confine_pool, size);
	mutex_unlock(&confine_lock);
	if (!pool_va) {
		pr_warn_ratelimited("window exhausted, cannot satisfy %zu bytes\n",
				    size);
		goto err_ck;
	}

	for (i = 0; i < nr; i++)
		pages[i] = virt_to_page((void *)(pool_va + ((unsigned long)i << PAGE_SHIFT)));

	vaddr = (unsigned long)area->addr;
	err = vmap_pages_range(vaddr, vaddr + size, pgprot, pages, PAGE_SHIFT);
	if (err)
		goto err_map;	/* may be partly mapped */

	/*
	 * Publish the backing pages on the vm_struct. arm64's set_memory_rox()
	 * (used by the module loader to make text executable) walks area->pages
	 * to mirror the permission change onto the pages' linear-map alias when
	 * rodata_full is on; without this it would dereference a NULL array.
	 */
	area->pages = pages;
	area->nr_pages = nr;

	ck->area = area;
	ck->pool_va = pool_va;
	ck->size = size;
	ck->pages = pages;

	mutex_lock(&confine_lock);
	err = xa_err(xa_store(&confine_chunks, vaddr >> PAGE_SHIFT, ck,
			      GFP_KERNEL));
	mutex_unlock(&confine_lock);
	if (err)
		goto err_map;

	if ((range->flags & EXECMEM_KASAN_SHADOW) &&
	    kasan_alloc_module_shadow(area->addr, size, GFP_KERNEL) < 0)
		goto err_xa;

	/*
	 * __vmalloc_node_range() does these two for vmalloc memory. KASAN doesn't
	 * tag executable memory, so the address stays as it is.
	 */
	area->addr = kasan_unpoison_vmalloc(area->addr, size, KASAN_VMALLOC_VM_ALLOC);
	kmemleak_vmalloc(area, size, GFP_KERNEL);

	return area->addr;

err_xa:
	mutex_lock(&confine_lock);
	xa_erase(&confine_chunks, vaddr >> PAGE_SHIFT);
	mutex_unlock(&confine_lock);
err_map:
	free_vm_area(area);		/* unmaps the range and frees bookkeeping */
	/*
	 * free_vm_area() leaves the TLB flush to the lazy vmap purge: flush now,
	 * before the pages can be mapped again for someone else.
	 */
	flush_tlb_kernel_range(vaddr, vaddr + size);
	mutex_lock(&confine_lock);
	gen_pool_free(confine_pool, pool_va, size);
	mutex_unlock(&confine_lock);
	kfree(ck);
	kfree(pages);
	return NULL;
err_ck:
	kfree(ck);
err_pages:
	kfree(pages);
err_area:
	free_vm_area(area);
	return NULL;
}

bool execmem_confine_free(void *ptr)
{
	unsigned long vaddr = (unsigned long)ptr;
	struct confine_chunk *ck;
	unsigned int nr;

	if (!confine_ready || !ptr)
		return false;

	mutex_lock(&confine_lock);
	ck = xa_load(&confine_chunks, vaddr >> PAGE_SHIFT);
	if (ck)
		xa_erase(&confine_chunks, vaddr >> PAGE_SHIFT);
	mutex_unlock(&confine_lock);
	if (!ck)
		return false;

	nr = ck->size >> PAGE_SHIFT;

	kmemleak_free(ptr);

	/*
	 * We bypass vfree(), so VM_FLUSH_RESET_PERMS does not run for us: drop
	 * the mapping back to non-executable, writable before tearing it down,
	 * so the window never retains a stale executable alias.
	 */
	set_memory_nx(vaddr, nr);
	set_memory_rw(vaddr, nr);

	free_vm_area(ck->area);

	/*
	 * free_vm_area() leaves the TLB flush to the lazy vmap purge. Flush
	 * before the pages go back to the pool, or a stale writable entry for
	 * this address could reach the next module's text.
	 */
	flush_tlb_kernel_range(vaddr, vaddr + ck->size);

	mutex_lock(&confine_lock);
	gen_pool_free(confine_pool, ck->pool_va, ck->size);
	mutex_unlock(&confine_lock);

	kfree(ck->pages);
	kfree(ck);
	return true;
}

/* Weak default: no architecture page-table sweep available. */
unsigned long __weak execmem_confine_arch_verify(phys_addr_t base,
						 phys_addr_t size,
						 unsigned long *nr_exec)
{
	return 0;
}

static unsigned long __init execmem_confine_verify(unsigned long *nr_exec_out)
{
	unsigned long nr_exec = 0, violations = 0, freed;
	struct confine_chunk *ck;
	unsigned long idx;

	freed = confine_count_freed_pages();
	if (freed) {
		pr_err("%lu window page(s) reached the page allocator\n", freed);
		violations += freed;
	}

	/* Every confined allocation is backed by pool pages, inside the window. */
	mutex_lock(&confine_lock);
	xa_for_each(&confine_chunks, idx, ck) {
		phys_addr_t pa = virt_to_phys((void *)ck->pool_va);
		phys_addr_t end = pa + ck->size - 1;

		if (!phys_in_window(pa) || !phys_in_window(end)) {
			pr_err("confined chunk at %#lx backs out-of-window phys [%pa, %pa]\n",
			       (unsigned long)ck->area->addr, &pa, &end);
			violations++;
		}
	}
	mutex_unlock(&confine_lock);

	/* Architecture page-table sweep over all kernel executable mappings. */
	violations += execmem_confine_arch_verify(confine_base, confine_size,
						  &nr_exec);

	if (nr_exec_out)
		*nr_exec_out = nr_exec;
	return violations;
}

static int __init execmem_confine_late_init(void)
{
	unsigned long nr_exec = 0, violations;

	if (!confine_ready)
		return 0;

	violations = execmem_confine_verify(&nr_exec);
	pr_info("boot verify: %lu executable mapping(s), %lu violation(s): %s\n",
		nr_exec, violations, violations ? "FAIL" : "PASS");

	if (violations && confine_enforce)
		panic("execmem-confine: %lu executable mapping(s) outside the window\n",
		      violations);

	return 0;
}
late_initcall(execmem_confine_late_init);
