// SPDX-License-Identifier: GPL-2.0-only
/*
 * kexec for arm64
 *
 * Copyright (C) Linaro.
 * Copyright (C) Huawei Futurewei Technologies.
 */

#include <linux/delay.h>
#include <linux/execmem.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/kexec.h>
#include <linux/kprobes.h>
#include <linux/page-flags.h>
#include <linux/reboot.h>
#include <linux/set_memory.h>
#include <linux/smp.h>

#include <asm/cacheflush.h>
#include <asm/cpu_ops.h>
#include <asm/daifflags.h>
#include <asm/memory.h>
#include <asm/mmu.h>
#include <asm/mmu_context.h>
#include <asm/page.h>
#include <asm/sections.h>
#include <asm/trans_pgd.h>

/**
 * kexec_image_info - For debugging output.
 */
#define kexec_image_info(_i) _kexec_image_info(__func__, __LINE__, _i)
static void _kexec_image_info(const char *func, int line,
	const struct kimage *kimage)
{
	kexec_dprintk("%s:%d:\n", func, line);
	kexec_dprintk("  kexec kimage info:\n");
	kexec_dprintk("    type:        %d\n", kimage->type);
	kexec_dprintk("    head:        %lx\n", kimage->head);
	kexec_dprintk("    kern_reloc: %pa\n", &kimage->arch.kern_reloc);
	kexec_dprintk("    el2_vectors: %pa\n", &kimage->arch.el2_vectors);
}

void machine_kexec_cleanup(struct kimage *kimage)
{
	/* Empty routine needed to avoid build errors. */
}

/**
 * machine_kexec_prepare - Prepare for a kexec reboot.
 *
 * Called from the core kexec code when a kernel image is loaded.
 * Forbid loading a kexec kernel if we have no way of hotplugging cpus or cpus
 * are stuck in the kernel. This avoids a panic once we hit machine_kexec().
 */
int machine_kexec_prepare(struct kimage *kimage)
{
	/*
	 * A new kernel runs from ordinary memory, outside the window, so the
	 * confined secondaries could never run it. Only a crash kernel can
	 * start: it runs on the boot CPU alone, which the window doesn't
	 * confine (see crash_handoff()).
	 */
	if (execmem_confine_active() && kimage->type != KEXEC_TYPE_CRASH) {
		pr_err("Can't kexec: the kernel's code is confined to a CTXR window.\n");
		return -EOPNOTSUPP;
	}

	if (kimage->type != KEXEC_TYPE_CRASH && cpus_are_stuck_in_kernel()) {
		pr_err("Can't kexec: CPUs are stuck in the kernel.\n");
		return -EBUSY;
	}

	return 0;
}

/**
 * kexec_segment_flush - Helper to flush the kimage segments to PoC.
 */
static void kexec_segment_flush(const struct kimage *kimage)
{
	unsigned long i;

	pr_debug("%s:\n", __func__);

	for (i = 0; i < kimage->nr_segments; i++) {
		pr_debug("  segment[%lu]: %016lx - %016lx, 0x%lx bytes, %lu pages\n",
			i,
			kimage->segment[i].mem,
			kimage->segment[i].mem + kimage->segment[i].memsz,
			kimage->segment[i].memsz,
			kimage->segment[i].memsz /  PAGE_SIZE);

		dcache_clean_inval_poc(
			(unsigned long)phys_to_virt(kimage->segment[i].mem),
			(unsigned long)phys_to_virt(kimage->segment[i].mem) +
				kimage->segment[i].memsz);
	}
}

/* Allocates pages for kexec page table */
static void *kexec_page_alloc(void *arg)
{
	struct kimage *kimage = arg;
	struct page *page = kimage_alloc_control_pages(kimage, 0);
	void *vaddr = NULL;

	if (!page)
		return NULL;

	vaddr = page_address(page);
	memset(vaddr, 0, PAGE_SIZE);

	return vaddr;
}

int machine_kexec_post_load(struct kimage *kimage)
{
	int rc;
	pgd_t *trans_pgd;
	void *reloc_code = page_to_virt(kimage->control_code_page);
	long reloc_size;
	struct trans_pgd_info info = {
		.trans_alloc_page	= kexec_page_alloc,
		.trans_alloc_arg	= kimage,
	};

	/* If in place, relocation is not used, only flush next kernel */
	if (kimage->head & IND_DONE) {
		kexec_segment_flush(kimage);
		kexec_image_info(kimage);
		return 0;
	}

	kimage->arch.el2_vectors = 0;
	if (is_hyp_nvhe()) {
		rc = trans_pgd_copy_el2_vectors(&info,
						&kimage->arch.el2_vectors);
		if (rc)
			return rc;
	}

	/* Create a copy of the linear map */
	rc = trans_pgd_create_copy(&info, &trans_pgd,
				   _PAGE_OFFSET(vabits_actual), PAGE_END);
	if (rc)
		return rc;
	kimage->arch.ttbr1 = __pa(trans_pgd);
	kimage->arch.zero_page = __pa_symbol(empty_zero_page);

	reloc_size = __relocate_new_kernel_end - __relocate_new_kernel_start;
	memcpy(reloc_code, __relocate_new_kernel_start, reloc_size);
	kimage->arch.kern_reloc = __pa(reloc_code);
	rc = trans_pgd_idmap_page(&info, &kimage->arch.ttbr0,
				  &kimage->arch.t0sz, reloc_code);
	if (rc)
		return rc;
	kimage->arch.phys_offset = virt_to_phys(kimage) - (long)kimage;

	/* Flush the reloc_code in preparation for its execution. */
	dcache_clean_inval_poc((unsigned long)reloc_code,
			       (unsigned long)reloc_code + reloc_size);
	icache_inval_pou((uintptr_t)reloc_code,
			 (uintptr_t)reloc_code + reloc_size);
	kexec_image_info(kimage);

	return 0;
}

#if defined(CONFIG_CRASH_DUMP) && defined(CONFIG_ARM64_CTXR_EXECMEM)
/*
 * Under a CTXR window every core but the boot CPU fetches code only from the
 * window, and the crash kernel sits in ordinary memory. A crash handled on a
 * confined core therefore offers the jump to CPU 0, which waits for it in its
 * stop handler. The state makes taking the offer and withdrawing it after a
 * timeout exclusive, so exactly one of the two CPUs goes on.
 */
enum {
	CRASH_HANDOFF_IDLE,
	CRASH_HANDOFF_OFFERED,
	CRASH_HANDOFF_TAKEN,
	CRASH_HANDOFF_WITHDRAWN,
};

static int crash_handoff_state;
static struct kimage *crash_handoff_image;

static bool crash_needs_handoff(void)
{
	return execmem_confine_active() && smp_processor_id() != 0;
}

/*
 * Called by every CPU told to stop, with interrupts masked. On CPU 0, with a
 * window and a crash kernel loaded, wait for a confined core to hand the crash
 * kernel over instead of parking. Waiting is parking until that happens.
 */
void arm64_crash_handoff_wait(void)
{
	if (!execmem_confine_active() || smp_processor_id() != 0 ||
	    !kexec_crash_loaded())
		return;

	for (;;) {
		if (cmpxchg(&crash_handoff_state, CRASH_HANDOFF_OFFERED,
			    CRASH_HANDOFF_TAKEN) == CRASH_HANDOFF_OFFERED)
			machine_kexec(READ_ONCE(crash_handoff_image));
		wfe();
	}
}
NOKPROBE_SYMBOL(arm64_crash_handoff_wait);

/*
 * Offer the crash kernel to CPU 0 and park. Returns only when CPU 0 can't take
 * it: it never acknowledged the stop, or didn't answer within a second. The
 * panic then goes on to its normal restart.
 */
static void crash_handoff(struct kimage *kimage)
{
	unsigned int cpu = smp_processor_id();
	unsigned long timeout = USEC_PER_SEC;

	if (cpu_online(0)) {
		pr_err("CPU 0 did not stop, so the crash kernel can't start from confined CPU %u\n",
		       cpu);
		return;
	}

	pr_info("CPU %u is confined to the CTXR window, CPU 0 starts the crash kernel\n",
		cpu);
	WRITE_ONCE(crash_handoff_image, kimage);
	/* CPU 0 then sees no other CPU online, as after any crash stop. */
	set_cpu_online(cpu, false);
	if (cmpxchg(&crash_handoff_state, CRASH_HANDOFF_IDLE,
		    CRASH_HANDOFF_OFFERED) != CRASH_HANDOFF_IDLE)
		cpu_park_loop();
	dsb(ish);
	sev();

	while (timeout--) {
		if (READ_ONCE(crash_handoff_state) == CRASH_HANDOFF_TAKEN)
			cpu_park_loop();
		udelay(1);
	}

	if (cmpxchg(&crash_handoff_state, CRASH_HANDOFF_OFFERED,
		    CRASH_HANDOFF_WITHDRAWN) == CRASH_HANDOFF_TAKEN)
		cpu_park_loop();

	set_cpu_online(cpu, true);
	pr_err("CPU 0 did not take the crash kernel\n");
}
#else
static bool crash_needs_handoff(void)
{
	return false;
}

static void crash_handoff(struct kimage *kimage)
{
}
#endif

/**
 * machine_kexec - Do the kexec reboot.
 *
 * Called from the core kexec code for a sys_reboot with LINUX_REBOOT_CMD_KEXEC.
 */
void machine_kexec(struct kimage *kimage)
{
	bool in_kexec_crash = (kimage == kexec_crash_image);
	bool stuck_cpus = cpus_are_stuck_in_kernel();

	if (in_kexec_crash && crash_needs_handoff()) {
		crash_handoff(kimage);
		return;
	}

	/*
	 * New cpus may have become stuck_in_kernel after we loaded the image.
	 */
	BUG_ON(!in_kexec_crash && (stuck_cpus || (num_online_cpus() > 1)));
	WARN(in_kexec_crash && (stuck_cpus || smp_crash_stop_failed()),
		"Some CPUs may be stale, kdump will be unreliable.\n");

	pr_info("Bye!\n");

	local_daif_mask();

	/*
	 * Both restart and kernel_reloc will shutdown the MMU, disable data
	 * caches. However, restart will start new kernel or purgatory directly,
	 * kernel_reloc contains the body of arm64_relocate_new_kernel
	 * In kexec case, kimage->start points to purgatory assuming that
	 * kernel entry and dtb address are embedded in purgatory by
	 * userspace (kexec-tools).
	 * In kexec_file case, the kernel starts directly without purgatory.
	 */
	if (kimage->head & IND_DONE) {
		typeof(cpu_soft_restart) *restart;

		cpu_install_idmap();
		restart = (void *)__pa_symbol(cpu_soft_restart);
		restart(is_hyp_nvhe(), kimage->start, kimage->arch.dtb_mem,
			0, 0);
	} else {
		void (*kernel_reloc)(struct kimage *kimage);

		if (is_hyp_nvhe())
			__hyp_set_vectors(kimage->arch.el2_vectors);
		cpu_install_ttbr0(kimage->arch.ttbr0, kimage->arch.t0sz);
		kernel_reloc = (void *)kimage->arch.kern_reloc;
		kernel_reloc(kimage);
	}

	BUG(); /* Should never get here. */
}

/**
 * machine_crash_shutdown - shutdown non-crashing cpus and save registers
 */
void machine_crash_shutdown(struct pt_regs *regs)
{
	local_irq_disable();

	/* shutdown non-crashing cpus */
	crash_smp_send_stop();

	/* for crashing cpu */
	crash_save_cpu(regs, smp_processor_id());
	machine_kexec_mask_interrupts();

	pr_info("Starting crashdump kernel...\n");
}

#if defined(CONFIG_CRASH_DUMP) && defined(CONFIG_HIBERNATION)
/*
 * To preserve the crash dump kernel image, the relevant memory segments
 * should be mapped again around the hibernation.
 */
void crash_prepare_suspend(void)
{
	if (kexec_crash_image)
		arch_kexec_unprotect_crashkres();
}

void crash_post_resume(void)
{
	if (kexec_crash_image)
		arch_kexec_protect_crashkres();
}

/*
 * crash_is_nosave
 *
 * Return true only if a page is part of reserved memory for crash dump kernel,
 * but does not hold any data of loaded kernel image.
 *
 * Note that all the pages in crash dump kernel memory have been initially
 * marked as Reserved as memory was allocated via memblock_reserve().
 *
 * In hibernation, the pages which are Reserved and yet "nosave" are excluded
 * from the hibernation image. crash_is_nosave() does thich check for crash
 * dump kernel and will reduce the total size of hibernation image.
 */

bool crash_is_nosave(unsigned long pfn)
{
	int i;
	phys_addr_t addr;

	if (!crashk_res.end)
		return false;

	/* in reserved memory? */
	addr = __pfn_to_phys(pfn);
	if ((addr < crashk_res.start) || (crashk_res.end < addr)) {
		if (!crashk_low_res.end)
			return false;

		if ((addr < crashk_low_res.start) || (crashk_low_res.end < addr))
			return false;
	}

	if (!kexec_crash_image)
		return true;

	/* not part of loaded kernel image? */
	for (i = 0; i < kexec_crash_image->nr_segments; i++)
		if (addr >= kexec_crash_image->segment[i].mem &&
				addr < (kexec_crash_image->segment[i].mem +
					kexec_crash_image->segment[i].memsz))
			return false;

	return true;
}

void crash_free_reserved_phys_range(unsigned long begin, unsigned long end)
{
	unsigned long addr;
	struct page *page;

	for (addr = begin; addr < end; addr += PAGE_SIZE) {
		page = phys_to_page(addr);
		free_reserved_page(page);
	}
}
#endif /* CONFIG_HIBERNATION */
