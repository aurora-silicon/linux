/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
typedef uint32_t u32;
typedef uint64_t u64;
struct device_node {
	bool available, reusable, extra, bad_reg;
};
struct resource {
	u64 start, end;
};
static struct device_node node = { .available = true };
static bool missing;
static u64 start = 0x100000, size = 0x4000;
static int refs;
static struct device_node *of_find_node_by_path(const char *path)
{
	assert(!strncmp(path, "/reserved-memory/uat-", 21));
	if (missing)
		return NULL;
	refs++;
	return &node;
}
static bool of_device_is_available(struct device_node *np)
{
	return np->available;
}
static bool of_property_read_bool(struct device_node *np, const char *name)
{
	assert(!strcmp(name, "reusable"));
	return np->reusable;
}
static const void *of_get_address(struct device_node *np, int index, void *a,
				  void *b)
{
	assert(index == 1 && !a && !b);
	return np->extra ? np : NULL;
}
static int of_address_to_resource(struct device_node *np, int index,
				  struct resource *res)
{
	assert(index == 0);
	res->start = start;
	res->end = start + size - 1;
	return np->bad_reg ? -1 : 0;
}
static u64 resource_size(struct resource *res)
{
	return res->end - res->start + 1;
}
static void of_node_put(struct device_node *np)
{
	if (np)
		refs--;
}
#include "uat-production.h"

int main(void)
{
	for (u32 i = 0; i < 4; ++i)
		assert(asahi_g15_reserved_uat(i, start, size));
	assert(!asahi_g15_reserved_uat(4, start, size));
	assert(!asahi_g15_reserved_uat(0, start + 0x4000, size));
	assert(!asahi_g15_reserved_uat(0, start, size + 0x4000));
	missing = true;
	assert(!asahi_g15_reserved_uat(0, start, size));
	missing = false;
	node.available = false;
	assert(!asahi_g15_reserved_uat(0, start, size));
	node.available = true;
	node.reusable = true;
	assert(!asahi_g15_reserved_uat(0, start, size));
	node.reusable = false;
	node.extra = true;
	assert(!asahi_g15_reserved_uat(0, start, size));
	node.extra = false;
	node.bad_reg = true;
	assert(!asahi_g15_reserved_uat(0, start, size));
	assert(refs == 0);
	puts("production UAT owner: exact ranges, unavailable/reusable/malformed/extra reservations: PASS");
}
