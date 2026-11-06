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
 *     emitted by the bootloader) or the command line (ctxr_exec_window=, used
 *     for VM/simulated bring-up),
 *   - reserves it from memblock and checks the image fits,
 *   - carves the non-image remainder into a gen_pool and hands it to the generic
 *     execmem confinement core, and
 *   - provides the page-table sweep that the verifier uses to prove, in a VM
 *     without real CTXR hardware, that no EL1-executable mapping escapes.
 */

#define pr_fmt(fmt) "ctxr-execmem: " fmt

#include <linux/efi.h>
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
#define CTXR_WINDOW_DEFAULT	SZ_128M

static phys_addr_t ctxr_base __initdata;
static phys_addr_t ctxr_size __initdata;
static bool ctxr_enforce __initdata = true;
static bool ctxr_active __initdata;

/*
 * Free, linear-mapped RAM inside the window, snapshotted just before the
 * window is reserved. Anything already reserved at that point (the kernel
 * image, the device tree, an initrd, /reserved-memory and memreserve entries,
 * firmware or loader regions) and any hole or no-map region is excluded, so
 * the executable pool can never hand out memory something else owns.
 */
#define CTXR_MAX_FREE		64
static struct {
	phys_addr_t start;
	phys_addr_t end;
} ctxr_free[CTXR_MAX_FREE] __initdata;
static int ctxr_nr_free __initdata;

/* From the command line. A size of 0 means none was given. */
static phys_addr_t ctxr_cmd_base __initdata;
static phys_addr_t ctxr_cmd_size __initdata;
static int ctxr_cmd_enforce __initdata = -1;

/* ctxr_exec_window=[<size>[@<base>]]: with no size, the default one. */
static int __init ctxr_exec_window_param(char *arg)
{
	phys_addr_t base = 0, size = CTXR_WINDOW_DEFAULT;
	char *p = arg, *end;

	if (p && *p) {
		size = memparse(p, &end);
		if (end == p || !size)
			goto bad;
		if (*end == '@') {
			p = end + 1;
			base = memparse(p, &end);
			if (end == p)
				goto bad;
		}
		if (*end)
			goto bad;
	}

	ctxr_cmd_base = base;
	ctxr_cmd_size = size;
	return 0;

bad:
	pr_warn("ignoring ctxr_exec_window=%s\n", arg);
	return 0;
}
early_param("ctxr_exec_window", ctxr_exec_window_param);

static int __init ctxr_exec_enforce_param(char *arg)
{
	bool enforce;

	if (kstrtobool(arg, &enforce))
		pr_warn("ignoring ctxr_exec_enforce=%s\n", arg ?: "");
	else
		ctxr_cmd_enforce = enforce;
	return 0;
}
early_param("ctxr_exec_enforce", ctxr_exec_enforce_param);

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

static void __init ctxr_snapshot_free(phys_addr_t base, phys_addr_t size)
{
	phys_addr_t start, end, wend = base + size;
	u64 i;

	ctxr_nr_free = 0;
	for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &start, &end,
				NULL) {
		if (start >= wend)
			break;
		start = max(start, base);
		end = min(end, wend);
		if (start >= end)
			continue;
		if (ctxr_nr_free == CTXR_MAX_FREE) {
			pr_warn("more than %d free ranges in the window; ignoring the rest\n",
				CTXR_MAX_FREE);
			break;
		}
		ctxr_free[ctxr_nr_free].start = start;
		ctxr_free[ctxr_nr_free].end = end;
		ctxr_nr_free++;
	}
}

/* Reserve a window. False, with the reason logged, if it can't work. */
static bool __init ctxr_establish(phys_addr_t base, phys_addr_t size, bool enforce)
{
	phys_addr_t img_start = __pa_symbol(_text), img_end = __pa_symbol(_end);
	phys_addr_t dram_start = memblock_start_of_DRAM();
	phys_addr_t dram_end = memblock_end_of_DRAM();

	if (!base) {
		/*
		 * No base given: anchor the window on the image. If that would run
		 * past the end of DRAM (the image is within @size of the top), slide
		 * it down so it ends at DRAM's end while still covering the image.
		 */
		base = round_down(img_start, CTXR_WINDOW_ALIGN);
		if (base + size > dram_end && dram_end - dram_start >= size)
			base = max(round_down(dram_end - size, CTXR_WINDOW_ALIGN),
				   round_up(dram_start, CTXR_WINDOW_ALIGN));
	}

	if (img_start < base || img_end > base + size) {
		pr_err("kernel image [%pa, %pa) does not fit window [%pa, +%llu MiB)\n",
		       &img_start, &img_end, &base,
		       (unsigned long long)(size >> 20));
		return false;
	}

	if (base < dram_start || base + size > dram_end) {
		pr_err("window [%pa, +%llu MiB) is not within DRAM\n",
		       &base, (unsigned long long)(size >> 20));
		return false;
	}

	/* Must run before the window is reserved, or every range looks taken. */
	ctxr_snapshot_free(base, size);

	if (memblock_reserve(base, size)) {
		pr_err("failed to reserve window [%pa, +%llu MiB)\n",
		       &base, (unsigned long long)(size >> 20));
		return false;
	}

	execmem_confine_set_window(base, size);

	ctxr_base = base;
	ctxr_size = size;
	ctxr_enforce = enforce;
	ctxr_active = true;

	pr_info("reserved exec window [%pa, +%llu MiB), %s\n",
		&base, (unsigned long long)(size >> 20),
		enforce ? "enforcing" : "reporting");
	return true;
}

/*
 * A reserved-memory node whose init fails goes back to memblock, which can leave a
 * hole in the window for memblock_free_all() to release.
 */
void __init ctxr_exec_keep_reserved(void)
{
	if (ctxr_active)
		memblock_reserve(ctxr_base, ctxr_size);
}

void __init ctxr_exec_reserve(void)
{
	phys_addr_t base = 0, size = 0;
	bool enforce = true;
	bool from_dt = ctxr_parse_dt(&base, &size, &enforce);

	if (ctxr_cmd_enforce >= 0)
		enforce = ctxr_cmd_enforce;

	/* A bad override leaves the machine as its bootloader described it. */
	if (ctxr_cmd_size) {
		if (ctxr_establish(ctxr_cmd_base, ctxr_cmd_size, enforce))
			return;
		pr_warn("ignoring ctxr_exec_window=\n");
	}

	if (!from_dt || ctxr_establish(base, size, enforce))
		return;

	/* The bootloader armed this window, so the kernel can't run outside it. */
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
	int i;

	if (!ctxr_active)
		return false;

	pool = gen_pool_create(PAGE_SHIFT, NUMA_NO_NODE);
	if (!pool) {
		pr_err("failed to create the window pool\n");
		goto fail;
	}

	/*
	 * Only memory that was free before the window was reserved joins the
	 * pool; the image and everything else already reserved stays out.
	 */
	for (i = 0; i < ctxr_nr_free; i++)
		ctxr_pool_add_gap(pool, ctxr_free[i].start, ctxr_free[i].end);

	if (!gen_pool_size(pool)) {
		pr_err("window has no free space for executable allocations\n");
		goto fail_pool;
	}

	if (execmem_confine_init(pool, ctxr_enforce)) {
		pr_err("failed to register the window\n");
		goto fail_pool;
	}

	pr_info("executable pool: %zu MiB usable in %d range(s) of a %llu MiB window (%llu MiB already reserved)\n",
		gen_pool_size(pool) >> 20, ctxr_nr_free,
		(unsigned long long)(ctxr_size >> 20),
		(unsigned long long)((ctxr_size - gen_pool_size(pool)) >> 20));
	return true;

fail_pool:
	gen_pool_destroy(pool);
fail:
	if (ctxr_enforce)
		panic("ctxr-execmem: cannot build the executable pool\n");
	return false;
}

/*
 * A core enforcing the window can't fetch firmware code outside it, and
 * efi_rts_wq can run on any core. Misaligned code also makes the misaligned
 * regions after it executable, wherever they are.
 */
bool __init ctxr_exec_efi_runtime_ok(void)
{
	efi_memory_desc_t *md;

	if (!IS_ENABLED(CONFIG_EFI) || !ctxr_active || !ctxr_enforce)
		return true;

	for_each_efi_memory_desc(md) {
		phys_addr_t start = md->phys_addr;
		phys_addr_t end = start + (md->num_pages << EFI_PAGE_SHIFT);

		if (md->type != EFI_RUNTIME_SERVICES_CODE)
			continue;
		if (start >= ctxr_base && end <= ctxr_base + ctxr_size &&
		    PAGE_ALIGNED(start) && PAGE_ALIGNED(end))
			continue;

		pr_warn("EFI runtime services off: firmware code [%pa, %pa) is outside the window or misaligned\n",
			&start, &end);
		return false;
	}

	return true;
}

/*
 * Page-table sweep used by the confinement verifier. For every privileged
 * (PXN-clear) executable leaf in the kernel and EFI page tables, count it and
 * flag any whose physical output lies outside the window. This is what stands
 * in, in a VM, for the hardware CTXR fault.
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

	/* EFI runtime code has its own page tables, at user addresses. */
	if (efi_enabled(EFI_RUNTIME_SERVICES)) {
		v.ptdump.range = (struct ptdump_range[]) {
			{ 0, TASK_SIZE_64 },
			{ 0, 0 }
		};
		ptdump_walk_pgd(&v.ptdump, &efi_mm, NULL);
	}

	if (nr_exec)
		*nr_exec = v.nr_exec;
	return v.violations;
}
