// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Host-only harness: production policy/helper inserted at PRODUCTION_CODE below. */
#include <assert.h>
#include <errno.h>
#include <libfdt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define __init
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_NODES 2048
#define MAX_DEPTH 64
struct property {
	char *name;
	unsigned char *value;
	int length;
	struct property *next;
};

struct device_node {
	char *name, *full_name;
	struct device_node *parent, *child, *sibling;
	struct property *properties;
	unsigned int index, refs;
	int offset;
};

struct fixture {
	void *blob;
	struct device_node *nodes[MAX_NODES];
	unsigned int count;
};

static struct fixture live;
static void *policy;
#define __dtb_t8132_platform_data_begin policy
static unsigned int checks, diagnostics;
#define CHECK(expr)                                                      \
	do {                                                             \
		checks++;                                                \
		if (!(expr)) {                                           \
			fprintf(stderr, "check failed at line %d: %s\n", \
				__LINE__, #expr);                        \
			exit(2);                                         \
		}                                                        \
	} while (0)
#define pr_err(...) (diagnostics++)
static const char *kbasename(const char *s)
{
	const char *p = strrchr(s, '/');

	return p ? p + 1 : s;
}

static bool of_node_is_root(const struct device_node *n)
{
	return !n->parent;
}

static struct property *find_property(const struct device_node *n,
				      const char *name)
{
	for (struct property *p = n->properties; p; p = p->next)
		if (!strcmp(p->name, name))
			return p;
	return NULL;
}

static bool of_property_present(const struct device_node *n, const char *name)
{
	return find_property(n, name) != NULL;
}

static int of_property_read_string(const struct device_node *n,
				   const char *name, const char **s)
{
	struct property *p = find_property(n, name);

	if (!p)
		return -EINVAL;
	if (!p->length)
		return -ENODATA;
	if (!memchr(p->value, 0, p->length))
		return -EILSEQ;
	*s = (char *)p->value;
	return 0;
}

static bool of_device_is_compatible(const struct device_node *n,
				    const char *wanted)
{
	struct property *p = find_property(n, "compatible");

	if (!p)
		return false;
	for (int i = 0; i < p->length;) {
		size_t len = strlen((char *)p->value + i);

		if (!strcmp((char *)p->value + i, wanted))
			return true;
		i += len + 1;
	}
	return false;
}

static struct device_node *node_get(struct device_node *n)
{
	if (n)
		n->refs++;
	return n;
}

static void of_node_put(struct device_node *n)
{
	if (n) {
		CHECK(n->refs > 1);
		n->refs--;
	}
}

static void node_cleanup(struct device_node **n)
{
	of_node_put(*n);
}

static struct device_node *find_compatible(struct device_node *prev,
					   const char *compat)
{
	unsigned int first = prev ? prev->index + 1 : 0;

	of_node_put(prev);
	for (unsigned int i = first; i < live.count; i++)
		if (of_device_is_compatible(live.nodes[i], compat))
			return node_get(live.nodes[i]);
	return NULL;
}

static struct device_node *next_child(struct device_node *parent,
				      struct device_node *prev)
{
	struct device_node *next = prev ? prev->sibling : parent->child;

	of_node_put(prev);
	return node_get(next);
}

#define for_each_compatible_node(node, type, compat)         \
	for ((node) = find_compatible(NULL, compat); (node); \
	     (node) = find_compatible((node), compat))
#define for_each_child_of_node_scoped(parent, node)           \
	for (struct device_node *node                        \
		     __attribute__((cleanup(node_cleanup))) = \
		     next_child((parent), NULL);              \
	     node; node = next_child((parent), node))
struct entry {
	struct device_node *node;
	char *name;
	unsigned char *value;
	int length;
	struct entry *next;
};

struct of_changeset {
	struct entry *head, *tail;
	unsigned int count, attempts, fail_at;
};

static int queue_prop(struct of_changeset *cs, struct device_node *n,
		      const char *name, const void *v, int len)
{
	if (++cs->attempts == cs->fail_at)
		return -ENOMEM;
	struct entry *e = calloc(1, sizeof(*e));

	assert(e);
	e->node = n;
	e->name = strdup(name);
	e->value = malloc(len ? len : 1);
	assert(e->value);
	if (len)
		memcpy(e->value, v, len);
	e->length = len;
	if (cs->tail)
		cs->tail->next = e;
	else
		cs->head = e;
	cs->tail = e;
	cs->count++;
	return 0;
}

static int of_changeset_add_prop_string(struct of_changeset *cs,
					struct device_node *n, const char *name,
					const char *v)
{
	return queue_prop(cs, n, name, v, strlen(v) + 1);
}

static int of_changeset_update_prop_string(struct of_changeset *cs,
					   struct device_node *n,
					   const char *name, const char *v)
{
	return queue_prop(cs, n, name, v, strlen(v) + 1);
}

static int of_changeset_add_prop_bool(struct of_changeset *cs,
				      struct device_node *n, const char *name)
{
	return queue_prop(cs, n, name, NULL, 0);
}

/* PRODUCTION_CODE */
static void set_property(struct device_node *n, const char *name, const void *v,
			 int length)
{
	struct property *p = find_property(n, name);

	if (!p) {
		p = calloc(1, sizeof(*p));
		assert(p);
		p->name = strdup(name);
		p->next = n->properties;
		n->properties = p;
	}
	free(p->value);
	p->value = malloc(length ? length : 1);
	assert(p->value);
	if (length)
		memcpy(p->value, v, length);
	p->length = length;
}

static void apply(struct of_changeset *cs)
{
	for (struct entry *e = cs->head; e; e = e->next)
		set_property(e->node, e->name, e->value, e->length);
}

static void discard(struct of_changeset *cs)
{
	while (cs->head) {
		struct entry *e = cs->head;

		cs->head = e->next;
		free(e->name);
		free(e->value);
		free(e);
	}
	memset(cs, 0, sizeof(*cs));
}

static void *read_file(const char *path)
{
	FILE *f = fopen(path, "rb");

	assert(f);
	assert(fseek(f, 0, SEEK_END) == 0);
	long size = ftell(f);

	assert(size > 0);
	assert(fseek(f, 0, SEEK_SET) == 0);
	void *p = malloc(size);

	assert(p);
	assert(fread(p, 1, size, f) == (size_t)size);
	fclose(f);
	assert(fdt_check_header(p) == 0);
	return p;
}

static struct device_node *add_node(const char *name,
				    struct device_node *parent, int offset)
{
	assert(live.count < MAX_NODES);
	struct device_node *n = calloc(1, sizeof(*n));

	assert(n);
	n->full_name = strdup(name);
	n->name = strndup(name, strcspn(name, "@"));
	n->parent = parent;
	n->index = live.count;
	n->refs = 1;
	n->offset = offset;
	live.nodes[live.count++] = n;
	if (parent) {
		struct device_node **tail = &parent->child;

		while (*tail)
			tail = &(*tail)->sibling;
		*tail = n;
	}
	return n;
}

static void parse_node(int offset, struct device_node *parent)
{
	struct device_node *n =
		add_node(fdt_get_name(live.blob, offset, NULL), parent, offset);
	int prop, child;

	fdt_for_each_property_offset(prop, live.blob, offset) {
		int len;
		const struct fdt_property *p =
			fdt_get_property_by_offset(live.blob, prop, &len);
		assert(p);
		set_property(n, fdt_string(live.blob, fdt32_to_cpu(p->nameoff)),
			     p->data, len);
	}
	fdt_for_each_subnode(child, live.blob, offset) parse_node(child, n);
}

static void load_fixture(const char *path)
{
	assert(live.count == 0);
	live.blob = read_file(path);
	parse_node(0, NULL);
}

static void free_fixture(void)
{
	for (unsigned int i = 0; i < live.count; i++) {
		struct device_node *n = live.nodes[i];

		CHECK(n->refs == 1);
		while (n->properties) {
			struct property *p = n->properties;

			n->properties = p->next;
			free(p->name);
			free(p->value);
			free(p);
		}
		free(n->name);
		free(n->full_name);
		free(n);
	}
	free(live.blob);
	memset(&live, 0, sizeof(live));
}

static struct device_node *by_path(const char *path)
{
	int offset = fdt_path_offset(live.blob, path);

	assert(offset >= 0);
	for (unsigned int i = 0; i < live.count; i++)
		if (live.nodes[i]->offset == offset)
			return live.nodes[i];
	abort();
}

static uint64_t state_hash(void)
{
	uint64_t h = 1469598103934665603ULL;

	for (unsigned int i = 0; i < live.count; i++)
		for (struct property *p = live.nodes[i]->properties; p;
		     p = p->next) {
			for (const char *s = p->name; *s; s++)
				h = (h ^ (unsigned char)*s) * 1099511628211ULL;
			for (int j = 0; j < p->length; j++)
				h = (h ^ p->value[j]) * 1099511628211ULL;
		}
	return h;
}

static bool available(struct device_node *n)
{
	const char *s;

	return of_property_read_string(n, "status", &s) || !strcmp(s, "okay") ||
	       !strcmp(s, "ok");
}

static unsigned int validate_policy(const char *name, unsigned int staged)
{
	unsigned int mismatches = 0, domains = 0, enabled = 0, owned = 0;

	for (unsigned int i = 0; i < live.count; i++) {
		struct device_node *n = live.nodes[i];

		if (!of_device_is_compatible(n, "apple,t8132-pmgr-pwrstate"))
			continue;
		char path[512];

		CHECK(fdt_get_path(live.blob, n->offset, path, sizeof(path)) ==
		      0);
		int off = fdt_path_offset(policy, path);

		CHECK(off >= 0);
		const char *wanted = fdt_getprop(policy, off, "status", NULL);

		CHECK(wanted);
		const char *got = NULL;
		int ret = of_property_read_string(n, "status", &got);

		if (ret || strcmp(wanted, got))
			mismatches++;
		bool want_owned = fdt_getprop(policy, off,
					      "apple,firmware-owned",
					      NULL) != NULL;
		if (of_property_present(n, "apple,firmware-owned") !=
		    want_owned)
			mismatches++;
		bool want_inherited = fdt_getprop(policy, off,
						 "apple,inherited-on", NULL) != NULL;
		if (of_property_present(n, "apple,inherited-on") != want_inherited)
			mismatches++;
		domains++;
		enabled += available(n);
		owned += of_property_present(n, "apple,firmware-owned");
	}
	CHECK(domains == 129);
	printf("%s: staged=%u enabled=%u/%u firmware_owned=%u mismatches=%u\n",
	       name, staged, enabled, domains, owned, mismatches);
	return mismatches;
}

static unsigned int run_case(const char *name, const char *live_path,
			     bool native)
{
	load_fixture(live_path);
	struct device_node *main = by_path("/soc/power-management@380700000");

	CHECK(!strcmp(main->full_name, "power-management@380700000"));
	CHECK(!strcmp(main->parent->full_name, "soc"));
	CHECK(!strcmp(main->name, "power-management"));
	CHECK(fdt_path_offset(policy, main->full_name) == -FDT_ERR_BADPATH);
	struct of_changeset cs = { 0 };
	unsigned int changed = 0;

	CHECK(t8132_pmgr_policy(&cs, &changed) == 0);
	CHECK(changed == cs.count);
	unsigned int staged = cs.count;

	apply(&cs);
	unsigned int errors = validate_policy(name, staged);
#ifdef CORRECTED
	if (native)
		CHECK(staged == 0);
	CHECK(available(by_path(
		"/soc/power-management@380700000/power-controller@538")));
	CHECK(available(by_path(
		"/soc/power-management@380700000/power-controller@548")));
	CHECK(available(by_path(
		"/soc/power-management@380700000/power-controller@120")));
	CHECK(available(by_path(
		"/soc/power-management@388280000/power-controller@90")));
#else
	(void)native;
#endif
	discard(&cs);
	free_fixture();
	return errors;
}

#ifdef CORRECTED
static void run_errors(const char *legacy)
{
	unsigned int total;

	load_fixture(legacy);
	struct of_changeset cs = { 0 };
	unsigned int changed = 0;

	CHECK(t8132_pmgr_policy(&cs, &changed) == 0);
	total = cs.count;
	discard(&cs);
	free_fixture();
	for (unsigned int fail = 1; fail <= total; fail++) {
		load_fixture(legacy);
		uint64_t before = state_hash();

		cs.fail_at = fail;
		changed = 0;
		CHECK(t8132_pmgr_policy(&cs, &changed) == -ENOMEM);
		CHECK(cs.count == fail - 1 && changed == cs.count);
		CHECK(state_hash() ==
		      before); /* Abort staging: caller must not apply it. */
		discard(&cs);
		free_fixture();
	}
	printf("staging failure: all %u queue positions leave the live tree unchanged\n",
	       total);

	const char *paths[] = { "/soc/power-management@380700000",
				"/soc/power-management@388280000" };
	for (unsigned int i = 0; i < 2; i++) {
		load_fixture(legacy);
		uint64_t before = state_hash();

		changed = 0;
		struct device_node *parent = by_path(paths[i]);

		free(parent->full_name);
		parent->full_name = strdup("power-management@deadbeef");
		CHECK(t8132_pmgr_policy(&cs, &changed) == -EINVAL);
		CHECK(state_hash() == before);
		if (i == 0)
			CHECK(cs.count == 0);
		else
			CHECK(cs.count > 0);
		CHECK(diagnostics == i + 1);
		discard(&cs);
		free_fixture();
	}
	printf("missing PMGR parent: both early and late failures abort without disabling known domains\n");
	load_fixture(legacy);
	changed = 0;
	struct device_node *parent = by_path(paths[0]);
	struct device_node *unknown =
		add_node("power-controller@deadbeef", parent, -1);
	const char compat[] = "apple,t8132-pmgr-pwrstate";

	set_property(unknown, "compatible", compat, sizeof(compat));
	CHECK(t8132_pmgr_policy(&cs, &changed) == 0);
	apply(&cs);
	CHECK(!available(unknown));
	CHECK(available(by_path(
		"/soc/power-management@380700000/power-controller@538")));
	CHECK(t8132_fdt_node_offset(policy, NULL) == -FDT_ERR_BADPATH);
	struct device_node orphan = { .full_name =
					      "power-management@380700000" };
	CHECK(t8132_fdt_node_offset(policy, &orphan) == -FDT_ERR_BADPATH);
	CHECK(t8132_fdt_node_offset(policy, live.nodes[0]) == 0);
	CHECK(t8132_fdt_node_offset(policy, by_path(paths[0])) !=
	      t8132_fdt_node_offset(policy, by_path(paths[1])));
	discard(&cs);
	free_fixture();
	printf("unknown child disabled; parent ancestry, root and unit-address discrimination passed\n");
}

#endif
int main(int argc, char **argv)
{
	CHECK(argc == 4);
	CHECK(diagnostics == 0);
	unsigned int failures = 0;

	policy = read_file(argv[1]);
	failures += run_case("legacy", argv[2], false);
	failures += run_case("native", argv[3], true);
#ifdef CORRECTED
	run_errors(argv[2]);
#endif
	free(policy);
	printf("%u checks; %u policy mismatches\n", checks, failures);
	return failures ? 1 : 0;
}
