// SPDX-License-Identifier: GPL-2.0
/*
 * Confine the kernel's executable code to a fixed physical window.
 *
 * On Apple-silicon machines whose boot firmware arms a CTXR (code text execute
 * range) on the secondary CPUs, privileged code may only be fetched from inside
 * that hardware window. To run the kernel at EL2 rather than dropping it to EL1,
 * every executable physical page must live inside one such window: the static
 * image (placed there by the bootloader) and every run-time executable
 * allocation (modules, BPF, kprobes, ftrace).
 *
 * This file owns the arm64 end of that contract:
 *   - it learns the window from the device tree (/chosen/aurora,ctxr-exec-window,
 *     emitted by the EL2 monitor) or the command line (ctxr_exec_window=, used
 *     for VM/simulated bring-up),
 *   - reserves it from memblock and checks the image fits,
 *   - carves the non-image remainder into a gen_pool and hands it to the generic
 *     execmem confinement core, and
 *   - provides the page-table sweep that the verifier uses to prove, in a VM
 *     without real CTXR hardware, that no EL1-executable mapping escapes.
 */

#define pr_fmt(fmt) "ctxr-execmem: " fmt

#include <linux/execmem.h>
#include <linux/genalloc.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/of_fdt.h>
#include <linux/ptdump.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/ctxr_execmem.h>
#include <asm/memory.h>
#include <asm/pgtable.h>
#include <asm/pgtable-hwdef.h>
#include <asm/sections.h>

/* Window granularity when the base is derived rather than given. */
#define CTXR_WINDOW_ALIGN	SZ_2M
/* Default size if only "ctxr_exec_window" with no value is given. */
#define CTXR_WINDOW_DEFAULT	SZ_1G

static phys_addr_t ctxr_base __initdata;
static phys_addr_t ctxr_size __initdata;
static bool ctxr_enforce __initdata = true;
static bool ctxr_active __initdata;

static bool __init ctxr_parse_cmdline(phys_addr_t *base, phys_addr_t *size,
				      bool *enforce)
{
	const char *p;
	char *endp;

	p = strstr(boot_command_line, "ctxr_exec_window=");
	if (!p)
		return false;
	p += strlen("ctxr_exec_window=");

	*size = memparse(p, &endp);
	if (!*size)
		*size = CTXR_WINDOW_DEFAULT;
	if (*endp == '@')
		*base = memparse(endp + 1, NULL);
	else
		*base = 0;	/* derive from the image below */

	p = strstr(boot_command_line, "ctxr_exec_enforce=");
	if (p)
		*enforce = (p[strlen("ctxr_exec_enforce=")] != '0');

	return true;
}

static bool __init ctxr_parse_dt(phys_addr_t *base, phys_addr_t *size,
				 bool *enforce)
{
	unsigned long chosen;
	const __be32 *prop;
	int len;

	chosen = of_get_flat_dt_subnode_by_name(of_get_flat_dt_root(), "chosen");
	if ((long)chosen < 0)
		return false;

	/* <base_hi base_lo size_hi size_lo> */
	prop = of_get_flat_dt_prop(chosen, "aurora,ctxr-exec-window", &len);
	if (!prop || len != 4 * sizeof(__be32))
		return false;

	*base = ((u64)be32_to_cpu(prop[0]) << 32) | be32_to_cpu(prop[1]);
	*size = ((u64)be32_to_cpu(prop[2]) << 32) | be32_to_cpu(prop[3]);

	prop = of_get_flat_dt_prop(chosen, "aurora,ctxr-exec-enforce", &len);
	if (prop && len == sizeof(__be32))
		*enforce = !!be32_to_cpu(prop[0]);

	return *size != 0;
}

void __init ctxr_exec_reserve(void)
{
	phys_addr_t base = 0, size = 0, img_start, img_end;
	bool enforce = true;

	if (!ctxr_parse_cmdline(&base, &size, &enforce) &&
	    !ctxr_parse_dt(&base, &size, &enforce))
		return;

	if (!size)
		return;

	img_start = __pa_symbol(_text);
	img_end = __pa_symbol(_end);

	if (!base)
		base = round_down(img_start, CTXR_WINDOW_ALIGN);

	if (img_start < base || img_end > base + size) {
		pr_err("kernel image [%pa, %pa) does not fit window [%pa, +%llu MiB)\n",
		       &img_start, &img_end, &base,
		       (unsigned long long)(size >> 20));
		goto fail;
	}

	if (base < memblock_start_of_DRAM() ||
	    base + size > memblock_end_of_DRAM()) {
		pr_err("window [%pa, +%llu MiB) is not within DRAM\n",
		       &base, (unsigned long long)(size >> 20));
		goto fail;
	}

	if (memblock_reserve(base, size)) {
		pr_err("failed to reserve window [%pa, +%llu MiB)\n",
		       &base, (unsigned long long)(size >> 20));
		goto fail;
	}

	ctxr_base = base;
	ctxr_size = size;
	ctxr_enforce = enforce;
	ctxr_active = true;

	pr_info("reserved exec window [%pa, +%llu MiB), %s\n",
		&base, (unsigned long long)(size >> 20),
		enforce ? "enforcing" : "reporting");
	return;

fail:
	if (enforce)
		panic("ctxr-execmem: cannot establish the executable window\n");
	pr_warn("confinement disabled\n");
}

static void __init ctxr_pool_add_gap(struct gen_pool *pool, phys_addr_t from,
				     phys_addr_t to)
{
	from = PAGE_ALIGN(from);
	to = round_down(to, PAGE_SIZE);
	if (to <= from)
		return;

	if (gen_pool_add_virt(pool, (unsigned long)__va(from), from, to - from,
			      NUMA_NO_NODE))
		pr_err("failed to add [%pa, %pa) to the pool\n", &from, &to);
}

bool __init ctxr_exec_execmem_setup(void)
{
	struct gen_pool *pool;
	phys_addr_t img_start, img_end;

	if (!ctxr_active)
		return false;

	pool = gen_pool_create(PAGE_SHIFT, NUMA_NO_NODE);
	if (!pool) {
		pr_err("failed to create the window pool\n");
		goto fail;
	}

	/* The image occupies part of the window; the rest becomes the pool. */
	img_start = __pa_symbol(_text);
	img_end = __pa_symbol(_end);
	ctxr_pool_add_gap(pool, ctxr_base, img_start);
	ctxr_pool_add_gap(pool, img_end, ctxr_base + ctxr_size);

	if (!gen_pool_size(pool)) {
		pr_err("window has no free space for executable allocations\n");
		goto fail_pool;
	}

	if (execmem_confine_init(ctxr_base, ctxr_size, pool, ctxr_enforce)) {
		pr_err("failed to register the window\n");
		goto fail_pool;
	}

	pr_info("executable pool: %zu MiB usable in a %llu MiB window\n",
		gen_pool_size(pool) >> 20,
		(unsigned long long)(ctxr_size >> 20));
	return true;

fail_pool:
	gen_pool_destroy(pool);
fail:
	if (ctxr_enforce)
		panic("ctxr-execmem: cannot build the executable pool\n");
	return false;
}

/*
 * Page-table sweep used by the confinement verifier. For every privileged
 * (PXN-clear) executable leaf in the kernel page tables, count it and flag any
 * whose physical output lies outside the window. This is what stands in, in a
 * VM, for the hardware CTXR fault.
 */
struct ctxr_verify {
	struct ptdump_state ptdump;
	phys_addr_t base;
	phys_addr_t end;
	unsigned long nr_exec;
	unsigned long violations;
};

static void ctxr_check_leaf(struct ctxr_verify *v, unsigned long addr,
			    pteval_t val, phys_addr_t leaf_size)
{
	phys_addr_t pa, last;

	if (!(val & PTE_VALID))
		return;
	if (val & PTE_PXN)		/* not executable at EL1 */
		return;

	v->nr_exec++;

	pa = PFN_PHYS(pte_pfn(__pte(val)));
	last = pa + leaf_size - 1;
	if (pa < v->base || last >= v->end) {
		v->violations++;
		pr_warn("EL1-exec mapping va %#lx -> phys [%pa, %pa] outside window\n",
			addr, &pa, &last);
	}
}

static void ctxr_note_pte(struct ptdump_state *st, unsigned long addr, pte_t pte)
{
	ctxr_check_leaf(container_of(st, struct ctxr_verify, ptdump), addr,
			pte_val(pte), PAGE_SIZE);
}

static void ctxr_note_pmd(struct ptdump_state *st, unsigned long addr, pmd_t pmd)
{
	ctxr_check_leaf(container_of(st, struct ctxr_verify, ptdump), addr,
			pmd_val(pmd), PMD_SIZE);
}

static void ctxr_note_pud(struct ptdump_state *st, unsigned long addr, pud_t pud)
{
	ctxr_check_leaf(container_of(st, struct ctxr_verify, ptdump), addr,
			pud_val(pud), PUD_SIZE);
}

/*
 * p4d/pgd leaves never carry executable kernel mappings on arm64, but the
 * page-table walker calls every level's note callback (including for holes and
 * a final flush), so each must be non-NULL.
 */
static void ctxr_note_p4d(struct ptdump_state *st, unsigned long addr,
			  p4d_t p4d)
{
}

static void ctxr_note_pgd(struct ptdump_state *st, unsigned long addr,
			  pgd_t pgd)
{
}

static void ctxr_note_flush(struct ptdump_state *st)
{
}

unsigned long execmem_confine_arch_verify(phys_addr_t base, phys_addr_t size,
					  unsigned long *nr_exec)
{
	struct ctxr_verify v = {
		.base = base,
		.end = base + size,
		.ptdump = {
			.note_page_pte = ctxr_note_pte,
			.note_page_pmd = ctxr_note_pmd,
			.note_page_pud = ctxr_note_pud,
			.note_page_p4d = ctxr_note_p4d,
			.note_page_pgd = ctxr_note_pgd,
			.note_page_flush = ctxr_note_flush,
			.range = (struct ptdump_range[]) {
				{ _PAGE_OFFSET(vabits_actual), ~0UL },
				{ 0, 0 }
			},
		},
	};

	ptdump_walk_pgd(&v.ptdump, &init_mm, NULL);

	if (nr_exec)
		*nr_exec = v.nr_exec;
	return v.violations;
}
