// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Report how this machine booted: the record the boot chain leaves in
 * /chosen/aurora,boot-security (aura Stage 1 and Stage 2, U-Boot).
 *
 * Each stage adds measurement@N { stage, policy, component, verdict, sha256,
 * key-id, security-version } and keeps "state" up to date: "verified" when
 * every stage enforced a signature on the next one, "partial" when a later
 * stage did but an earlier link was not checked, "unverified" otherwise.
 * A stage's policy is "enforce" (refuses what does not verify), "audit"
 * (checks and records, boots anyway) or "off" (measures only). The record
 * also carries what iBoot reports about the LocalPolicy that admitted
 * Stage 1 (apple,lp-coih and friends), the Stage 1 build flavour
 * (stage1-flavour: "dev" or "secure") and U-Boot's Secure Boot mode
 * (uefi-secure-boot: "enforcing", "audit" or "off").
 *
 * There is no TPM: this is the firmware's account, not a proof. A stage that
 * no earlier stage verified can write anything here, so the state is only
 * called verified when an enforcing Stage 1 verified the next stage, and the
 * record is "advisory" otherwise.
 *
 * With APPLE_AURORA_BOOT_SECURITY_LOCKDOWN the kernel is locked down
 * (integrity) when the boot is on the secure path: UEFI Secure Boot
 * enforcing, or a verified chain. A forged record can only add lockdown.
 *
 * Exposes /sys/firmware/aurora/boot_security/{state,advisory,policy,
 * stage1_flavour,uefi_secure_boot,lockdown,lp_coih,measurements}, logs one
 * line at boot and measures the record into IMA (critical data
 * "aurora-boot-security").
 */

#include <linux/efi.h>
#include <linux/ima.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

static struct device_node *rec;
static struct kobject *aurora_kobj, *bs_kobj;
static const char *chain_state = "none";
static bool stage1_enforced;
static const char *lockdown_reason = "no";

static const char *prop_str(struct device_node *np, const char *name)
{
	const char *s;

	return of_property_read_string(np, name, &s) ? "" : s;
}

static const char *prop_or(struct device_node *np, const char *name,
			   const char *dflt)
{
	const char *s = prop_str(np, name);

	return *s ? s : dflt;
}

/* Whether an enforcing Stage 1 verified what it started. */
static bool stage1_enforced_verified(void)
{
	struct device_node *m;
	bool ok = false;

	for_each_child_of_node(rec, m) {
		if (!strcmp(prop_str(m, "stage"), "stage1") &&
		    !strcmp(prop_str(m, "policy"), "enforce") &&
		    !strcmp(prop_str(m, "verdict"), "verified"))
			ok = true;
	}
	return ok;
}

/* The record's state, demoted unless an enforcing Stage 1 verified Stage 2. */
static const char *effective_state(void)
{
	const char *state = prop_str(rec, "state");

	if (!strcmp(state, "verified") && !stage1_enforced)
		return "partial";
	return *state ? state : "unverified";
}

static ssize_t state_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%s\n", chain_state);
}

static ssize_t advisory_show(struct kobject *k, struct kobj_attribute *a,
			     char *buf)
{
	return sysfs_emit(buf, "%d\n", !stage1_enforced);
}

/* Each stage's policy, in boot order: "stage1 enforce, stage2 audit, ..." */
static ssize_t policy_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	struct device_node *m;
	const char *prev = "";
	ssize_t n = 0;

	for_each_child_of_node(rec, m) {
		const char *stage = prop_str(m, "stage");

		if (!strcmp(stage, prev))
			continue;
		n += sysfs_emit_at(buf, n, "%s%s %s", n ? ", " : "", stage,
				   prop_str(m, "policy"));
		prev = stage;
	}
	return n + sysfs_emit_at(buf, n, "\n");
}

static ssize_t stage1_flavour_show(struct kobject *k, struct kobj_attribute *a,
				   char *buf)
{
	return sysfs_emit(buf, "%s\n", prop_or(rec, "stage1-flavour", "unknown"));
}

static ssize_t uefi_secure_boot_show(struct kobject *k,
				     struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%s\n", prop_or(rec, "uefi-secure-boot", "unknown"));
}

static ssize_t lockdown_show(struct kobject *k, struct kobj_attribute *a,
			     char *buf)
{
	return sysfs_emit(buf, "%s\n", lockdown_reason);
}

static ssize_t lp_coih_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	int len;
	const u8 *v = of_get_property(rec, "apple,lp-coih", &len);

	if (!v || len <= 0)
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%*phN\n", min(len, 64), v);
}

static ssize_t format_measurements(char *buf, size_t size)
{
	struct device_node *m;
	ssize_t n = 0;

	for_each_child_of_node(rec, m) {
		int len;
		const u8 *h = of_get_property(m, "sha256", &len);

		n += scnprintf(buf + n, size - n, "%s %s %s %s %*phN\n",
			       prop_str(m, "stage"), prop_str(m, "policy"),
			       prop_str(m, "component"), prop_str(m, "verdict"),
			       h && len == 32 ? 32 : 0, h);
	}
	return n;
}

static ssize_t measurements_show(struct kobject *k, struct kobj_attribute *a,
				 char *buf)
{
	return format_measurements(buf, PAGE_SIZE);
}

static struct kobj_attribute state_attr = __ATTR_RO(state);
static struct kobj_attribute advisory_attr = __ATTR_RO(advisory);
static struct kobj_attribute policy_attr = __ATTR_RO(policy);
static struct kobj_attribute stage1_flavour_attr = __ATTR_RO(stage1_flavour);
static struct kobj_attribute uefi_secure_boot_attr = __ATTR_RO(uefi_secure_boot);
static struct kobj_attribute lockdown_attr = __ATTR_RO(lockdown);
static struct kobj_attribute lp_coih_attr = __ATTR_RO(lp_coih);
static struct kobj_attribute measurements_attr = __ATTR_RO(measurements);

static struct attribute *bs_attrs[] = {
	&state_attr.attr, &advisory_attr.attr, &policy_attr.attr,
	&stage1_flavour_attr.attr, &uefi_secure_boot_attr.attr,
	&lockdown_attr.attr, &lp_coih_attr.attr, &measurements_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(bs);

#ifdef CONFIG_APPLE_AURORA_BOOT_SECURITY_LOCKDOWN
static bool __init firmware_secure_boot(void)
{
	if (rec && !strcmp(prop_str(rec, "uefi-secure-boot"), "enforcing"))
		return true;
	if (!efi_enabled(EFI_RUNTIME_SERVICES) ||
	    !efi_rt_services_supported(EFI_RT_SUPPORTED_GET_VARIABLE))
		return false;
	return efi_get_secureboot_mode(efi.get_variable) ==
	       efi_secureboot_mode_enabled;
}

static void __init lockdown_secure_path(void)
{
	const char *why = NULL;

	if (rec && !strcmp(chain_state, "verified"))
		why = "the verified boot chain";
	else if (firmware_secure_boot())
		why = "UEFI Secure Boot";
	if (!why)
		return;
	lockdown_reason = why;
	lockdown_from_firmware(why, LOCKDOWN_INTEGRITY_MAX);
}
#else
static inline void lockdown_secure_path(void)
{
}
#endif

/* The record as text: what the sysfs files show, one line per field. */
static void measure_record(void)
{
	char *buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	size_t n;

	if (!buf)
		return;
	n = scnprintf(buf, PAGE_SIZE, "state %s\nadvisory %d\nstage1-flavour %s\n"
		      "uefi-secure-boot %s\n", chain_state, !stage1_enforced,
		      prop_or(rec, "stage1-flavour", "unknown"),
		      prop_or(rec, "uefi-secure-boot", "unknown"));
	n += format_measurements(buf + n, PAGE_SIZE - n);
	ima_measure_critical_data("aurora-boot-security", "boot-security-record",
				  buf, n, false, NULL, 0);
	kfree(buf);
}

#ifndef MODULE
/* Built in: IMA is ready only at late_initcall. */
static int __init aurora_boot_security_ima(void)
{
	if (rec)
		measure_record();
	return 0;
}
late_initcall_sync(aurora_boot_security_ima);
#endif

static int __init aurora_boot_security_init(void)
{
	struct device_node *m;
	const char *last_stage = "none", *last_policy = "none";
	int ret, count = 0;

	rec = of_find_node_by_path("/chosen/aurora,boot-security");
	if (rec && !of_device_is_compatible(rec, "aurora,boot-security-v1")) {
		of_node_put(rec);
		rec = NULL;
	}
	if (rec) {
		stage1_enforced = stage1_enforced_verified();
		chain_state = effective_state();
	}
	lockdown_secure_path();
	if (!rec)
		return -ENODEV;

	for_each_child_of_node(rec, m) {
		count++;
		last_stage = prop_str(m, "stage");
		last_policy = prop_str(m, "policy");
	}

	aurora_kobj = kobject_create_and_add("aurora", firmware_kobj);
	if (!aurora_kobj) {
		ret = -ENOMEM;
		goto err_node;
	}
	bs_kobj = kobject_create_and_add("boot_security", aurora_kobj);
	if (!bs_kobj) {
		ret = -ENOMEM;
		goto err_aurora;
	}
	ret = sysfs_create_groups(bs_kobj, bs_groups);
	if (ret)
		goto err_bs;

	pr_info("aurora: boot chain %s%s (%d measurements, last stage %s/%s, stage1 %s, uefi secure boot %s, lockdown from it: %s)\n",
		chain_state, stage1_enforced ? "" : ", advisory", count,
		last_stage, last_policy,
		prop_or(rec, "stage1-flavour", "unknown"),
		prop_or(rec, "uefi-secure-boot", "unknown"), lockdown_reason);
#ifdef MODULE
	measure_record();
#endif
	return 0;

err_bs:
	kobject_put(bs_kobj);
err_aurora:
	kobject_put(aurora_kobj);
err_node:
	of_node_put(rec);
	rec = NULL;
	return ret;
}

static void __exit aurora_boot_security_exit(void)
{
	sysfs_remove_groups(bs_kobj, bs_groups);
	kobject_put(bs_kobj);
	kobject_put(aurora_kobj);
	of_node_put(rec);
}

module_init(aurora_boot_security_init);
module_exit(aurora_boot_security_exit);
MODULE_DESCRIPTION("Report the boot chain record left by aura and U-Boot");
MODULE_LICENSE("Dual MIT/GPL");
