// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Transitional /chosen aliases for boot loaders that predate the aurora names.
 *
 * aura publishes its boot facts and switches in /chosen as "aurora,<name>"
 * (for example aurora,os-fw-version or aurora,aura-stage2-version). Boot
 * loaders built before that rename publish the same values as "asahi,<name>",
 * with "m1n1" where aura now says "aura". So that every reader in the tree
 * only has to know the aurora name, this copies each such legacy property to
 * its aurora name at early_initcall time, unless the aurora name is already
 * present. Nothing is removed and nothing is overwritten.
 *
 * LEGACY-ASAHI(2027-02-28): remove this file once no supported boot loader
 * publishes the old names.
 */

#define pr_fmt(fmt) "aurora-chosen: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>

#define AURORA_LEGACY_PREFIX "asahi,"
#define AURORA_LEGACY_LOADER "m1n1"

/* "asahi,m1n1-stage2-version" -> "aurora,aura-stage2-version" */
static char *aurora_legacy_chosen_name(const char *old)
{
	const char *rest = old + strlen(AURORA_LEGACY_PREFIX);
	size_t len = strlen("aurora,") + strlen(rest) + 1;
	const char *hit;
	char *name, *p;

	name = kzalloc(len, GFP_KERNEL);
	if (!name)
		return NULL;
	p = name + sprintf(name, "aurora,");
	while ((hit = strstr(rest, AURORA_LEGACY_LOADER))) {
		memcpy(p, rest, hit - rest);
		p += hit - rest;
		p += sprintf(p, "aura");
		rest = hit + strlen(AURORA_LEGACY_LOADER);
	}
	strcpy(p, rest);
	return name;
}

static int __init aurora_legacy_chosen_aliases(void)
{
	struct device_node *chosen;
	struct property *pp, *copy;
	unsigned int added = 0;
	char *name;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return 0;

	for_each_property_of_node(chosen, pp) {
		if (strncmp(pp->name, AURORA_LEGACY_PREFIX, strlen(AURORA_LEGACY_PREFIX)))
			continue;
		name = aurora_legacy_chosen_name(pp->name);
		if (!name)
			break;
		if (of_property_present(chosen, name)) {
			kfree(name);
			continue;
		}
		copy = kzalloc(sizeof(*copy), GFP_KERNEL);
		if (!copy) {
			kfree(name);
			break;
		}
		copy->name = name;
		copy->length = pp->length;
		copy->value = kmemdup(pp->value, pp->length, GFP_KERNEL);
		if (pp->length && !copy->value) {
			kfree(name);
			kfree(copy);
			break;
		}
		/* of_add_property() takes ownership; the copy lives for the boot. */
		if (of_add_property(chosen, copy)) {
			kfree(copy->value);
			kfree(name);
			kfree(copy);
			continue;
		}
		added++;
	}
	of_node_put(chosen);
	if (added)
		pr_info("%u legacy /chosen properties aliased to their aurora names\n", added);
	return 0;
}
early_initcall(aurora_legacy_chosen_aliases);
