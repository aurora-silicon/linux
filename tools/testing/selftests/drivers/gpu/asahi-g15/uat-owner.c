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
	const char *name;
	struct device_node *child, *next;
	bool available, reusable, extra, bad_reg;
	u64 start, size;
	int refs;
};
struct resource {
	u64 start, end;
};
static struct device_node nodes[5];
static struct device_node rmem;
#define node nodes[0]
static bool missing;
static u64 start = 0x100000, size = 0x4000;
static inline struct device_node *of_node_get(struct device_node *np)
{
	if (np)
		np->refs++;
	return np;
}
static void of_node_put(struct device_node *np);
static struct device_node *of_find_node_by_path(const char *path)
{
	const char *prefix = "/reserved-memory/";

	if (missing)
		return NULL;
	if (!strcmp(path, "/reserved-memory"))
		return of_node_get(&rmem);
	assert(!strncmp(path, prefix, strlen(prefix)));
	for (struct device_node *np = rmem.child; np; np = np->next)
		if (!strcmp(np->name, path + strlen(prefix)))
			return of_node_get(np);
	return NULL;
}
static inline struct device_node *of_get_next_child(struct device_node *parent,
					   struct device_node *prev)
{
	struct device_node *next = NULL;

	if (parent)
		next = prev ? prev->next : parent->child;
	of_node_put(prev);
	return of_node_get(next);
}
#define for_each_child_of_node(parent, child) \
	for ((child) = of_get_next_child((parent), NULL); (child); \
	     (child) = of_get_next_child((parent), (child)))
static inline bool of_node_name_eq(struct device_node *np, const char *name)
{
	return np && strlen(name) == strcspn(np->name, "@") &&
		!strncmp(np->name, name, strlen(name));
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
	res->start = np->start;
	res->end = np->start + np->size - 1;
	return np->bad_reg ? -1 : 0;
}
static u64 resource_size(struct resource *res)
{
	return res->end - res->start + 1;
}
static void of_node_put(struct device_node *np)
{
	if (np)
		assert(np->refs-- > 0);
}
#include "uat-production.h"

int main(void)
{
	static const char *const names[] = {
		"uat-ttbs", "uat-pagetables", "uat-handoff", "uat-pagetables-l2"
	};
	static const char *const unit_names[] = {
		"uat-ttbs@10", "uat-pagetables@20", "uat-handoff@30", "uat-pagetables-l2@40"
	};
	rmem.child = nodes;
	for (u32 i = 0; i < 4; ++i) {
		nodes[i].available = true;
		nodes[i].start = start + i * size;
		nodes[i].size = size;
		nodes[i].name = names[i];
		nodes[i].next = i < 3 ? &nodes[i + 1] : NULL;
	}
	for (u32 i = 0; i < 4; ++i)
		assert(asahi_g15_reserved_uat(i, nodes[i].start, size));
	for (u32 i = 0; i < 4; ++i)
		nodes[i].name = unit_names[i];
	for (u32 i = 0; i < 4; ++i)
		assert(asahi_g15_reserved_uat(i, nodes[i].start, size));
	nodes[3].next = &nodes[4];
	nodes[4].name = "uat-ttbs@99";
	nodes[4].available = true;
	assert(!asahi_g15_reserved_uat(0, start, size));
	nodes[4].available = false;
	assert(!asahi_g15_reserved_uat(0, start, size));
	nodes[3].next = NULL;
	node.name = "uat-ttbs-other@10";
	assert(!asahi_g15_reserved_uat(0, start, size));
	node.name = unit_names[0];
	for (u32 i = 0; i < 4; ++i) {
		const char *saved = nodes[i].name;

		nodes[i].name = "unrelated@10";
		assert(!asahi_g15_reserved_uat(i, nodes[i].start, size));
		nodes[i].name = saved;
	}
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
	assert(rmem.refs == 0);
	for (u32 i = 0; i < ARRAY_SIZE(nodes); ++i)
		assert(nodes[i].refs == 0);
	puts("production UAT owner: unit addresses, unique owners and exact ranges: PASS");
}
