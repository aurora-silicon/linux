// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Report how this machine booted: the record the boot chain leaves in
 * /chosen/aurora,boot-security (aura Stage 1 and Stage 2, U-Boot).
 *
 * Each stage adds measurement@N { stage, policy, component, verdict, sha256,
 * key-id, security-version } and keeps "state" up to date: "verified" when
 * every stage enforced a signature on the next one, "partial" when a later
 * stage did but an earlier link was not checked, "unverified" otherwise.
 * The record also carries what iBoot reports about the LocalPolicy that
 * admitted Stage 1 (apple,lp-coih and friends).
 *
 * There is no TPM: this is the firmware's account, not a proof. A stage that
 * no earlier stage verified can write anything here, so the state is only
 * called verified when the first entry is an enforcing Stage 1's.
 *
 * Exposes /sys/firmware/aurora/boot_security/{state,policy,lp_coih,
 * measurements} and logs one line at boot.
 */

#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

static struct device_node *rec;
static struct kobject *aurora_kobj, *bs_kobj;
static const char *chain_state = "none";

static const char *prop_str(struct device_node *np, const char *name)
{
	const char *s;

	return of_property_read_string(np, name, &s) ? "" : s;
}

/* The record's state, demoted unless an enforcing Stage 1 verified Stage 2. */
static const char *effective_state(void)
{
	struct device_node *m;
	bool stage1_enforced = false;
	const char *state = prop_str(rec, "state");

	for_each_child_of_node(rec, m) {
		if (!strcmp(prop_str(m, "stage"), "stage1") &&
		    !strcmp(prop_str(m, "policy"), "enforce") &&
		    !strcmp(prop_str(m, "verdict"), "verified"))
			stage1_enforced = true;
	}
	if (!strcmp(state, "verified") && !stage1_enforced)
		return "partial";
	return *state ? state : "unverified";
}

static ssize_t state_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%s\n", chain_state);
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

static ssize_t lp_coih_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	int len;
	const u8 *v = of_get_property(rec, "apple,lp-coih", &len);

	if (!v || len <= 0)
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%*phN\n", min(len, 64), v);
}

static ssize_t measurements_show(struct kobject *k, struct kobj_attribute *a,
				 char *buf)
{
	struct device_node *m;
	ssize_t n = 0;

	for_each_child_of_node(rec, m) {
		int len;
		const u8 *h = of_get_property(m, "sha256", &len);

		n += sysfs_emit_at(buf, n, "%s %s %s %s %*phN\n",
				   prop_str(m, "stage"), prop_str(m, "policy"),
				   prop_str(m, "component"),
				   prop_str(m, "verdict"),
				   h && len == 32 ? 32 : 0, h);
	}
	return n;
}

static struct kobj_attribute state_attr = __ATTR_RO(state);
static struct kobj_attribute policy_attr = __ATTR_RO(policy);
static struct kobj_attribute lp_coih_attr = __ATTR_RO(lp_coih);
static struct kobj_attribute measurements_attr = __ATTR_RO(measurements);

static struct attribute *bs_attrs[] = {
	&state_attr.attr, &policy_attr.attr, &lp_coih_attr.attr,
	&measurements_attr.attr, NULL,
};
ATTRIBUTE_GROUPS(bs);

static int __init aurora_boot_security_init(void)
{
	struct device_node *m;
	const char *last_stage = "none", *last_policy = "none";
	int ret, count = 0;

	rec = of_find_node_by_path("/chosen/aurora,boot-security");
	if (!rec)
		return -ENODEV;
	if (!of_device_is_compatible(rec, "aurora,boot-security-v1")) {
		of_node_put(rec);
		return -ENODEV;
	}
	chain_state = effective_state();
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

	pr_info("aurora: boot chain %s (%d measurements, last stage %s/%s)\n",
		chain_state, count, last_stage, last_policy);
	return 0;

err_bs:
	kobject_put(bs_kobj);
err_aurora:
	kobject_put(aurora_kobj);
err_node:
	of_node_put(rec);
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
