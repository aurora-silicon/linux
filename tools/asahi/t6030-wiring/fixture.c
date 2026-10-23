/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define BIT(n) (1u << (n))
struct mux_chip;
struct mux_control {
	struct mux_chip *chip;
	unsigned int index;
};
struct mux_chip {
	unsigned int controllers;
	struct mux_control mux[3];
};
struct device_node {
	const char *name, *compatible;
	struct device_node *parent, *child, *next, *remote;
	unsigned int refs, reg, source, names_count;
	bool missing_source, bad_parse;
	const char *names[4];
	struct device_node *providers[4];
	unsigned int cells[4], nargs[4];
	struct mux_chip *chip;
};
struct of_phandle_args {
	struct device_node *np;
	unsigned int args_count, args[2];
};
static bool t6030 = true;
static struct device_node *get(struct device_node *n)
{
	if (n)
		n->refs++;
	return n;
}
static void of_node_put(struct device_node *n)
{
	if (n) {
		assert(n->refs);
		n->refs--;
	}
}
static void device_node_free(struct device_node **n)
{
	of_node_put(*n);
}
#define __free(name) __attribute__((cleanup(name##_free)))
static struct device_node *of_get_parent(struct device_node *n)
{
	return get(n ? n->parent : NULL);
}
static bool of_node_name_eq(struct device_node *n, const char *s)
{
	return n && !strcmp(n->name, s);
}
static bool of_machine_is_compatible(const char *s)
{
	return t6030 && !strcmp(s, "apple,t6030");
}
static bool of_device_is_compatible(struct device_node *n, const char *s)
{
	return n && n->compatible && !strcmp(n->compatible, s);
}
static int of_property_match_string(struct device_node *n, const char *prop,
				    const char *name)
{
	assert(!strcmp(prop, "mux-control-names"));
	for (unsigned int i = 0; n && i < n->names_count; i++)
		if (!strcmp(n->names[i], name))
			return i;
	return -ENODATA;
}
static int of_property_read_u32_index(struct device_node *n, const char *prop,
				      unsigned int i, u32 *value)
{
	assert(!strcmp(prop, "apple,typec-mux-indices"));
	if (!n || n->missing_source || i >= 3)
		return -EINVAL;
	*value = n->source;
	return 0;
}
static int of_property_read_u32(struct device_node *n, const char *prop,
				u32 *value)
{
	assert(!strcmp(prop, "reg"));
	if (!n)
		return -EINVAL;
	*value = n->reg;
	return 0;
}
static int of_parse_phandle_with_args(struct device_node *n, const char *prop,
				      const char *cells, int index,
				      struct of_phandle_args *args)
{
	assert(!strcmp(prop, "mux-controls") &&
	       !strcmp(cells, "#mux-control-cells"));
	if (!n || n->bad_parse || index < 0 ||
	    (unsigned)index >= n->names_count)
		return -EINVAL;
	args->np = get(n->providers[index]);
	args->args_count = n->nargs[index];
	args->args[0] = n->cells[index];
	return 0;
}
static struct device_node *of_graph_get_port_by_id(struct device_node *n,
						   unsigned int id)
{
	for (struct device_node *p = n ? n->child : NULL; p; p = p->next)
		if (p->reg == id)
			return get(p);
	return NULL;
}
static struct device_node *next_child(struct device_node *parent,
				      struct device_node *prev)
{
	struct device_node *next = prev	  ? prev->next :
				   parent ? parent->child :
					    NULL;
	of_node_put(prev);
	return get(next);
}
#define for_each_child_of_node_scoped(parent, node)          \
	for (struct device_node * node __free(device_node) = \
		     next_child(parent, NULL);               \
	     node; node = next_child(parent, node))
#define for_each_of_graph_port_endpoint(parent, node) \
	for_each_child_of_node_scoped(parent, node)
static struct device_node *
of_graph_get_remote_port_parent(struct device_node *n)
{
	return get(n ? n->remote : NULL);
}
struct device {
	struct device_node *of_node;
};
struct apple_dcp {
	struct device *dev;
};
struct apple_dcp_typec_route {
	struct apple_dcp *dcp;
	struct mux_control *xbar, *dpin[2];
	bool dual_stream;
};
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define dev_warn_once(dev, ...) ((void)(dev))
static struct mux_control *devm_mux_control_get(struct device *dev,
						const char *name)
{
	int i = of_property_match_string(dev->of_node, "mux-control-names",
					 name);
	if (i < 0 || dev->of_node->nargs[i] != 1 ||
	    !dev->of_node->providers[i]->chip ||
	    dev->of_node->cells[i] >=
		    dev->of_node->providers[i]->chip->controllers)
		return (void *)(intptr_t)-EINVAL;
	return &dev->of_node->providers[i]->chip->mux[dev->of_node->cells[i]];
}
/* PRODUCTION_FUNCTIONS */
int main(int argc, char **argv)
{
	assert(argc == 2);
	struct mux_chip chip = { .controllers = 3 },
			other_chip = { .controllers = 3 };
	for (unsigned int i = 0; i < 3; i++) {
		chip.mux[i].chip = &chip;
		chip.mux[i].index = i;
		other_chip.mux[i].chip = &other_chip;
		other_chip.mux[i].index = i;
	}
	struct device_node provider = {
		.compatible = "apple,t6020-display-crossbar", .chip = &chip
	};
	struct device_node other = { .compatible =
					     "apple,t6020-display-crossbar",
				     .chip = &other_chip };
	struct device_node dcp = { .name = "dcpext",
				   .names_count = 1,
				   .names = { "typec0" },
				   .providers = { &provider, &provider,
						  &provider },
				   .cells = { 0, 1, 2 },
				   .nargs = { 1, 1, 1 } };
	struct device_node routes = { .name = "typec-routes", .parent = &dcp };
	struct device_node route_np = { .parent = &routes };
	struct device_node endpoint[2] = { { .remote = &route_np },
					   { .remote = &route_np } };
	endpoint[0].next = &endpoint[1];
	struct device_node port = { .reg = 3, .child = &endpoint[0] };
	struct device_node connector = { .child = &port };
	struct device dev = { .of_node = &dcp };
	struct apple_dcp engine = { .dev = &dev };
	struct apple_dcp_typec_route route = { .dcp = &engine,
					       .xbar = &chip.mux[0] };
	bool expected = true;
	int expected_dcp = 0;
	const char *test = argv[1];
	if (strstr(test, "source2"))
		dcp.source = 2;
	if (!strcmp(test, "other-soc")) {
		t6030 = false;
		expected = false;
	}
	if (!strcmp(test, "odd-source")) {
		dcp.source = 1;
		expected = false;
	}
	if (!strcmp(test, "unknown-source")) {
		dcp.source = 4;
		expected = false;
	}
	if (!strcmp(test, "missing-source")) {
		dcp.missing_source = true;
		expected = false;
	}
	if (!strcmp(test, "single-endpoint")) {
		endpoint[0].next = NULL;
		expected = false;
	}
	if (!strcmp(test, "wrong-provider")) {
		provider.compatible = "apple,t8103-display-crossbar";
		expected = false;
	}
	if (!strcmp(test, "missing-primary")) {
		dcp.names_count = 0;
		expected = false;
	}
	if (!strcmp(test, "parse-failure")) {
		dcp.bad_parse = true;
		expected = false;
	}
	if (!strcmp(test, "primary-cell1")) {
		dcp.cells[0] = 1;
		expected = false;
	}
	if (!strcmp(test, "primary-two-args")) {
		dcp.nargs[0] = 2;
		expected = false;
	}
	if (!strcmp(test, "named") || !strcmp(test, "named-other-soc") ||
	    !strcmp(test, "named-wrong-cell") ||
	    !strcmp(test, "named-other-provider") ||
	    !strcmp(test, "partial-named")) {
		dcp.names_count = 3;
		dcp.names[1] = "typec0-dpin0";
		dcp.names[2] = "typec0-dpin1";
		if (!strcmp(test, "named-other-soc"))
			t6030 = false;
		if (!strcmp(test, "named-wrong-cell")) {
			dcp.cells[2] = 1;
			expected = false;
			expected_dcp = -EINVAL;
		}
		if (!strcmp(test, "named-other-provider")) {
			dcp.providers[2] = &other;
			expected = false;
			expected_dcp = -EINVAL;
		}
		if (!strcmp(test, "partial-named")) {
			dcp.names_count = 2;
			expected = false;
			expected_dcp = -EINVAL;
		}
	}
	if (!strcmp(test, "usb4") || !strcmp(test, "usb4-other-soc") ||
	    !strcmp(test, "bad-usb4")) {
		dcp.names_count = 2;
		dcp.names[1] = "typec0-usb4";
		if (!strcmp(test, "usb4-other-soc"))
			t6030 = false;
		if (!strcmp(test, "bad-usb4")) {
			dcp.cells[1] = 0;
			expected = false;
			expected_dcp = -EINVAL;
		}
	}
	if (strncmp(test, "dcp-", 4))
		assert(apple_dp_tunnel_dual_stream(&connector) == expected);
	if (strncmp(test, "tb-", 3)) {
		int ret = dcp_typec_route_wiring(&route, &connector, 0);
		assert(ret == expected_dcp);
		if (!ret) {
			assert(route.dual_stream == expected);
			if (expected) {
				assert(route.dpin[0] == &chip.mux[1]);
				assert(route.dpin[1] == &chip.mux[2]);
			}
		}
	}
	assert(!provider.refs && !other.refs && !dcp.refs && !routes.refs &&
	       !route_np.refs && !port.refs && !endpoint[0].refs &&
	       !endpoint[1].refs);
	printf("PASS %s\n", test);
	return 0;
}
