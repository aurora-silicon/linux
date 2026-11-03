// SPDX-License-Identifier: GPL-2.0-only
/*
 * Contains implementation-defined CPU feature definitions.
 */

#define pr_fmt(fmt) "CPU features: " fmt

#include <linux/arm-smccc.h>

#include <asm/cpufeature.h>
#include <asm/apple_amx.h>
#include <asm/apple_cpufeature.h>
#include <asm/hypervisor.h>
#include <asm/virt.h>
#include <linux/cache.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/kstrtox.h>
#include <linux/preempt.h>
#include <linux/printk.h>

#ifdef CONFIG_ARM64_MEMORY_MODEL_CONTROL
static bool has_apple_feature(const struct arm64_cpu_capabilities *entry, int scope)
{
	u64 val;
	WARN_ON(scope == SCOPE_LOCAL_CPU && preemptible());

	if (read_cpuid_implementor() != ARM_CPU_IMP_APPLE)
		return false;

	val = read_sysreg(aidr_el1);
	return cpufeature_matches(val, entry);
}

static bool has_apple_tso(const struct arm64_cpu_capabilities *entry, int scope)
{
	u64 val;

	if (!has_apple_feature(entry, scope))
		return false;

	/*
	 * KVM and old versions of the macOS hypervisor will advertise TSO in
	 * AIDR_EL1, but then ignore writes to ACTLR_EL1. Test that the bit is
	 * actually writable before enabling TSO.
	 */

	val = read_sysreg(actlr_el1);
	write_sysreg(val ^ ACTLR_APPLE_TSO, actlr_el1);
	if (!((val ^ read_sysreg(actlr_el1)) & ACTLR_APPLE_TSO)) {
		pr_info_once("CPU advertises Apple TSO but it is broken, ignoring\n");
		return false;
	}

	write_sysreg(val, actlr_el1);
	return true;
}

static bool has_tso_fixed(const struct arm64_cpu_capabilities *entry, int scope)
{
	/* List of CPUs that always use the TSO memory model */
	static const struct midr_range fixed_tso_list[] = {
		MIDR_ALL_VERSIONS(MIDR_NVIDIA_DENVER),
		MIDR_ALL_VERSIONS(MIDR_NVIDIA_CARMEL),
		MIDR_ALL_VERSIONS(MIDR_FUJITSU_A64FX),
		{ /* sentinel */ }
	};

	return is_midr_in_range_list(fixed_tso_list);
}
#endif

#ifdef CONFIG_ARM64_APPLE_AMX
static bool apple_amx_off __ro_after_init;

static int __init parse_apple_amx(char *arg)
{
	bool on;
	int ret = kstrtobool(arg, &on);

	if (!ret)
		apple_amx_off = !on;
	return ret;
}
early_param("apple_amx", parse_apple_amx);

static bool has_apple_amx(const struct arm64_cpu_capabilities *entry, int scope)
{
	WARN_ON(scope == SCOPE_LOCAL_CPU && preemptible());

	if (apple_amx_off || read_cpuid_implementor() != ARM_CPU_IMP_APPLE)
		return false;

	/*
	 * M1 to M3 have AMX. M4 and later (and the A18 Pro) implement the
	 * architectural SME instead, which is what Linux supports there.
	 */
	if (cpuid_feature_extract_unsigned_field(read_sysreg_s(SYS_ID_AA64PFR1_EL1),
						 ID_AA64PFR1_EL1_SME_SHIFT))
		return false;

	/*
	 * Below EL2, AMX_CTL_EL1 and AMX_STATE_T trap to the hypervisor, and
	 * AMX only works at EL1/EL0 if EL2 sets AMX_CTL_EL2.EN_EL1. Require the
	 * hypervisor to say that it does both and switches the register file.
	 */
	if (!is_kernel_in_hyp_mode() &&
	    !kvm_arm_hyp_service_available(ARM_SMCCC_KVM_FUNC_AURORA_APPLE_AMX))
		return false;

	return cpufeature_matches(read_sysreg(aidr_el1), entry);
}

/*
 * Start with EL0 AMX off; threads get it on their first AMX instruction.
 * AT_HWCAP3/cpuinfo tell userspace that it can use AMX.
 */
static void cpu_disable_el0_amx(const struct arm64_cpu_capabilities *entry)
{
	u64 ctl = read_sysreg_s(SYS_IMP_APL_AMX_CTL_EL1);

	cpu_set_named_feature(APPLE_AMX);

	if (ctl & AMX_CTL_APPLE_EN) {
		write_sysreg_s(ctl & ~AMX_CTL_APPLE_EN, SYS_IMP_APL_AMX_CTL_EL1);
		isb();
	}
}
#endif

static bool has_apple_actlr_virt_impdef(const struct arm64_cpu_capabilities *entry, int scope)
{
	u64 midr = read_cpuid_id() & MIDR_CPU_MODEL_MASK;

	return midr >= MIDR_APPLE_M1_ICESTORM && midr <= MIDR_APPLE_M1_FIRESTORM_MAX;
}

static bool has_apple_actlr_virt(const struct arm64_cpu_capabilities *entry, int scope)
{
	u64 midr = read_cpuid_id() & MIDR_CPU_MODEL_MASK;

	return midr >= MIDR_APPLE_M2_BLIZZARD && midr <= MIDR_CPU_MODEL(ARM_CPU_IMP_APPLE, 0xfff);
}

static const struct arm64_cpu_capabilities arm64_impdef_features[] = {
#ifdef CONFIG_ARM64_MEMORY_MODEL_CONTROL
	{
		.desc = "TSO memory model (Apple)",
		.capability = ARM64_HAS_TSO_APPLE,
		.type = ARM64_CPUCAP_EARLY_LOCAL_CPU_FEATURE,
		.matches = has_apple_tso,
		.field_pos = AIDR_APPLE_TSO_SHIFT,
		.field_width = 1,
		.sign = FTR_UNSIGNED,
		.min_field_value = 1,
		.max_field_value = 1,
	},
	{
		.desc = "TSO memory model (Fixed)",
		.capability = ARM64_HAS_TSO_FIXED,
		.type = ARM64_CPUCAP_EARLY_LOCAL_CPU_FEATURE,
		.matches = has_tso_fixed,
	},
#endif
#ifdef CONFIG_ARM64_APPLE_AMX
	{
		.desc = "Matrix coprocessor (Apple AMX)",
		.capability = ARM64_HAS_AMX_APPLE,
		.type = ARM64_CPUCAP_EARLY_LOCAL_CPU_FEATURE,
		.matches = has_apple_amx,
		.cpu_enable = cpu_disable_el0_amx,
		.field_pos = AIDR_APPLE_AMX_SHIFT,
		.field_width = 1,
		.sign = FTR_UNSIGNED,
		.min_field_value = 1,
		.max_field_value = 1,
	},
#endif
	{
		.desc = "ACTLR virtualization (IMPDEF, Apple)",
		.capability = ARM64_HAS_ACTLR_VIRT_APPLE,
		.type = ARM64_CPUCAP_EARLY_LOCAL_CPU_FEATURE,
		.matches = has_apple_actlr_virt_impdef,
	},
	{
		.desc = "ACTLR virtualization (architectural?)",
		.capability = ARM64_HAS_ACTLR_VIRT,
		.type = ARM64_CPUCAP_EARLY_LOCAL_CPU_FEATURE,
		.matches = has_apple_actlr_virt,
	},
	{},
};

void __init init_cpucap_indirect_list_impdef(void)
{
	init_cpucap_indirect_list_from_array(arm64_impdef_features);
}
