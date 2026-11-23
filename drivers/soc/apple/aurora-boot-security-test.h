/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _AURORA_BOOT_SECURITY_TEST_H
#define _AURORA_BOOT_SECURITY_TEST_H

#include <linux/types.h>

struct device_node;
struct kobject;
struct kobj_attribute;

/* Test-only access to the record reader and its sysfs callbacks. */
struct aurora_boot_security_test {
	struct device_node **rec;
	struct kobject **aurora_kobj;
	struct kobject **bs_kobj;
	const char **chain_state;
	bool *stage1_enforced;
	const char **lockdown_reason;
	bool (*stage1_enforced_verified)(void);
	const char *(*effective_state)(void);
	ssize_t (*format_measurements)(char *buf, size_t size);
	void (*measure_record)(void);
	struct kobj_attribute *state;
	struct kobj_attribute *advisory;
	struct kobj_attribute *policy;
	struct kobj_attribute *stage1_flavour;
	struct kobj_attribute *uefi_secure_boot;
	struct kobj_attribute *lockdown;
	struct kobj_attribute *lp_coih;
	struct kobj_attribute *measurements;
};

extern const struct aurora_boot_security_test aurora_bs_test;

#endif
