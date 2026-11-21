/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_SUSPEND_H
#define __ASM_SUSPEND_H

#define NR_CTX_REGS 14
#define NR_CALLEE_SAVED_REGS 12

/*
 * struct cpu_suspend_ctx must be 16-byte aligned since it is allocated on
 * the stack, which must be 16-byte aligned on v8
 */
struct cpu_suspend_ctx {
	/*
	 * This struct must be kept in sync with
	 * cpu_do_{suspend/resume} in mm/proc.S
	 */
	u64 ctx_regs[NR_CTX_REGS];
	u64 sp;
} __aligned(16);

/*
 * Memory to save the cpu state is allocated on the stack by
 * __cpu_suspend_enter()'s caller, and populated by __cpu_suspend_enter().
 * This data must survive until cpu_resume() is called.
 *
 * This struct describes the size and the layout of the saved cpu state.
 * The layout of the callee_saved_regs is defined by the implementation
 * of __cpu_suspend_enter(), and cpu_resume(). This struct must be passed
 * in by the caller as __cpu_suspend_enter()'s stack-frame is gone once it
 * returns, and the data would be subsequently corrupted by the call to the
 * finisher.
 */
struct sleep_stack_data {
	struct cpu_suspend_ctx	system_regs;
	unsigned long		callee_saved_regs[NR_CALLEE_SAVED_REGS];
};

extern unsigned long *sleep_save_stash;

extern int cpu_suspend(unsigned long arg, int (*fn)(unsigned long));
extern void cpu_resume(void);
int __cpu_suspend_enter(struct sleep_stack_data *state);
void __cpu_suspend_exit(void);
void _cpu_resume(void);

int swsusp_arch_suspend(void);
int swsusp_arch_resume(void);
int arch_hibernation_header_save(void *addr, unsigned int max_size);
int arch_hibernation_header_restore(void *addr);

/**
 * struct arm64_hibernate_layout - What a platform adds to the boot layout check.
 * @ex: Ranges that this boot keeps out of the image and that the layout check
 *	leaves out of the memory it compares, in ascending order.
 * @nex: Number of valid entries in @ex.
 * @have_base: Whether CPU release addresses are compared as offsets from @base.
 * @base: Base address for the release addresses, if @have_base.
 */
struct arm64_hibernate_layout {
	u64 ex[2][2];
	int nex;
	bool have_base;
	u64 base;
};

/**
 * struct arm64_hibernate_platform - Platform hooks of arm64 hibernation.
 * @pfn_nosave: Whether the platform keeps page frame @pfn out of the image.
 * @layout: Mix the platform's boot facts into the layout cookie in @h with
 *	arm64_hibernate_layout_mix(), fill @lay, and return the new hash. The
 *	cookie is stored with an image and compared before it is restored: the
 *	boot that resumes an image must give the kernel the layout of the boot
 *	that wrote it.
 */
struct arm64_hibernate_platform {
	bool (*pfn_nosave)(unsigned long pfn);
	u64 (*layout)(u64 h, struct arm64_hibernate_layout *lay);
};

void arm64_hibernate_set_platform(const struct arm64_hibernate_platform *platform);
u64 arm64_hibernate_layout_mix(u64 h, const void *p, size_t len);

/* Used to resume on the CPU we hibernated on */
int hibernate_resume_nonboot_cpu_disable(void);

#endif
