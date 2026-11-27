// SPDX-License-Identifier: GPL-2.0-only
/*
 * Low-level idle sequences
 */

#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/irqflags.h>

#include <asm/apple-idle.h>
#include <asm/apple_m1_pmu.h>
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
 * Preserve general-purpose and FP/SIMD state across WFI on M4 and later
 * Apple cores.
 *
 * The decision has to be made before the boot CPU first idles, which can
 * happen as soon as rest_init() starts its idle thread, long before any
 * initcall runs: on these SoCs the boot CPU loses its registers too. It is
 * made from smp_prepare_boot_cpu(), where static keys cannot be flipped
 * yet, so it is a plain flag.
 */
static bool apple_wfi_loses_regs __ro_after_init;
static bool apple_keep_idle_nop __initdata;
static bool apple_wfi_deferred __initdata;

/*
 * Older boot loaders append idle=nop after the user's arguments on these
 * cores, so it cannot be overridden on the command line, and it is ignored
 * below. This flag keeps it anyway, for comparisons.
 */
static int __init setup_apple_keep_idle_nop(char *arg)
{
	apple_keep_idle_nop = true;
	return 0;
}
early_param("apple_keep_idle_nop", setup_apple_keep_idle_nop);

void __init apple_wfi_quirk_init(void)
{
	if (!midr_is_apple_m4_or_later(read_cpuid_id()))
		return;

	apple_wfi_loses_regs = true;
	pr_info("Apple: preserving registers across WFI\n");

	/*
	 * aura and m1n1 used to pass idle=nop on M4 and later for kernels
	 * without this path. Spinning idle keeps every core busy (cpuidle's
	 * WFI states spin too): on T8142 the SoC sits near 90 C and
	 * throttles, on T6050 the system draws about 10 W more at idle, on
	 * T8140 the die idles at 85-92 C instead of 43 C. WFI is safe here.
	 *
	 * The switch waits until the built-in drivers have probed: on a
	 * K114c (M5 Max, chip 0x6050), WFI early in boot gated the unclaimed
	 * root domain nub_gpio off before pmgr and pinctrl probed.
	 */
	if (idle == ARM64_IDLE_NOP && !apple_keep_idle_nop) {
		apple_wfi_deferred = true;
		pr_info("Apple: WFI idling deferred until built-in drivers probe\n");
	}
}

/*
 * Runs after the built-in drivers' initcalls. cpu_do_idle() reads idle on
 * every entry, so the switch takes effect at once.
 */
static int __init apple_wfi_defer_enable(void)
{
	if (!apple_wfi_deferred)
		return 0;

	idle = ARM64_IDLE_WFI;
	pr_info("Apple: idling in WFI, ignoring idle=nop (after built-in probes)\n");
	return 0;
}
late_initcall_sync(apple_wfi_defer_enable);

static __always_inline bool apple_wfi_needs_save(void)
{
	return unlikely(READ_ONCE(apple_wfi_loses_regs));
}

DEFINE_PER_CPU(bool, apple_pmu_wfi_freeze);
EXPORT_PER_CPU_SYMBOL_GPL(apple_pmu_wfi_freeze);

/*
 * On M4 and later Apple cores some cycle counters keep counting in WFI
 * (for example, M5 Max Sawtooth cores at about 1.5 GHz), which inflates
 * CPU-wide perf counts with idle time. Freeze the counters and their
 * interrupt across WFI whenever perf events are active on this CPU.
 */
static __always_inline void apple_wfi(void)
{
	/* Idle runs with preemption disabled: the raw accessor is enough. */
	if (unlikely(__this_cpu_read(apple_pmu_wfi_freeze))) {
		u64 pmcr0 = read_sysreg_s(SYS_IMP_APL_PMCR0_EL1);

		write_sysreg_s(pmcr0 & ~(PMCR0_CNT_ENABLE_0_7 |
					 PMCR0_CNT_ENABLE_8_9 |
					 PMCR0_IMODE),
			       SYS_IMP_APL_PMCR0_EL1);
		isb();
		apple_cpu_wfi();
		write_sysreg_s(pmcr0, SYS_IMP_APL_PMCR0_EL1);
		isb();
	} else {
		apple_cpu_wfi();
	}
}
#else
static __always_inline bool apple_wfi_needs_save(void)
{
	return false;
}

static __always_inline void apple_wfi(void) { }
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
			apple_wfi();
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
