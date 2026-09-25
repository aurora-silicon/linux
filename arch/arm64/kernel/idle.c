// SPDX-License-Identifier: GPL-2.0-only
/*
 * Low-level idle sequences
 */

#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/jump_label.h>

#include <asm/apple-idle.h>
#include <asm/barrier.h>
#include <asm/cpuidle.h>
#include <asm/cpufeature.h>
#include <asm/cputype.h>
#include <asm/fpsimd.h>
#include <asm/sysreg.h>

enum {
    ARM64_IDLE_WFI,
    ARM64_IDLE_YIELD,
    ARM64_IDLE_NOP,
} idle = ARM64_IDLE_WFI;

static int __init setup_idle(char *arg)
{
	if (!arg)
		return -1;
	else if (!strcmp(arg, "wfi"))
		idle = ARM64_IDLE_WFI;
	else if (!strcmp(arg, "yield"))
		idle = ARM64_IDLE_YIELD;
	else if (!strcmp(arg, "nop"))
		idle = ARM64_IDLE_NOP;
	else
		return -1;

	return 0;
}
early_param("idle", setup_idle);

#ifdef CONFIG_ARCH_APPLE
/*
 * Apple T8140 (A18 Pro) secondary CPUs can return from WFI with their
 * general-purpose and FP/SIMD registers cleared.  The key is enabled once
 * at boot on those parts; everywhere else the check is a patched-out branch.
 */
static DEFINE_STATIC_KEY_FALSE(apple_wfi_loses_regs);

static const struct midr_range apple_wfi_loses_regs_cpus[] __initconst = {
	MIDR_ALL_VERSIONS(MIDR_APPLE_TAHITI_E),
	MIDR_ALL_VERSIONS(MIDR_APPLE_TAHITI_P),
	{}
};

static int __init apple_wfi_quirk_init(void)
{
	if (is_midr_in_range_list(apple_wfi_loses_regs_cpus)) {
		static_branch_enable(&apple_wfi_loses_regs);
		pr_info("Apple T8140: preserving registers across WFI\n");
	}
	return 0;
}
early_initcall(apple_wfi_quirk_init);

static __always_inline bool apple_wfi_needs_save(void)
{
	return static_branch_unlikely(&apple_wfi_loses_regs);
}
#else
static __always_inline bool apple_wfi_needs_save(void)
{
	return false;
}
#endif

/*
 *	cpu_do_idle()
 *
 *	Idle the processor (wait for interrupt).
 *
 *	If the CPU supports priority masking we must do additional work to
 *	ensure that interrupts are not masked at the PMR (because the core will
 *	not wake up if we block the wake up signal in the interrupt controller).
 */
void __cpuidle cpu_do_idle(void)
{
	struct arm_cpuidle_irq_context context;

	arm_cpuidle_save_irq_context(&context);

	if (likely(idle == ARM64_IDLE_WFI)) {
		if (apple_wfi_needs_save()) {
			/*
			 * The FP/SIMD registers do not survive either: drop the
			 * lazy ownership so the next return to userspace reloads
			 * them, even when the same task resumes on this CPU.
			 */
			fpsimd_save_and_flush_cpu_state();
			apple_cpu_wfi();
		} else {
			dsb(sy);
			wfi();
		}
	} else if (idle == ARM64_IDLE_YIELD) {
		dsb(sy);
		asm volatile("yield" ::: "memory");
	}

	arm_cpuidle_restore_irq_context(&context);
}

/*
 * This is our default idle handler.
 */
void __cpuidle arch_cpu_idle(void)
{
	/*
	 * This should do all the clock switching and wait for interrupt
	 * tricks
	 */
	cpu_do_idle();
}
