// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Host fixture for the Thunderbolt DPIN1 dispatch and tiled-display split
 * admission. The production bodies of dcp_tb_dispatch_request(),
 * dcp_tb_split_locked(), dcp_typec_port_untileable(),
 * dcp_fabric_tunnel_slot() and dcp_fabric_binding_request() are pasted in;
 * only what lies below them (join, connect, teardown, release, attach,
 * rebalance) is stubbed, and every stub call is recorded.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define READ_ONCE(x) (x)
#define smp_load_acquire(p) (*(p))
#define lockdep_assert_held(l) ((void)(l))
#define scoped_guard(type, lock) for (int _sg = 1; _sg; _sg = 0)
#define time_before(a, b) ((long)((a) - (b)) < 0)
#define dev_warn(dev, ...) fprintf(stderr, __VA_ARGS__)
#define EXPORT_SYMBOL_GPL(x)

struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(n) { &(n), &(n) }
static void INIT_LIST_HEAD(struct list_head *l) { l->next = l->prev = l; }
static bool list_empty(const struct list_head *l) { return l->next == l; }
static void list_add_tail(struct list_head *n, struct list_head *h)
{
	n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n;
}
#define list_for_each_entry(pos, head, member)					\
	for (pos = container_of((head)->next, __typeof__(*pos), member);	\
	     &pos->member != (head);						\
	     pos = container_of(pos->member.next, __typeof__(*pos), member))

struct mutex { int unused; };
struct device { int unused; };
struct mux_chip;
struct mux_control { struct mux_chip *chip; };
struct mux_chip { struct mux_control mux[3]; unsigned int controllers; };

struct dptx_port { bool enabled, connected, tile_hint; };
struct apple_dcp {
	struct device *dev;
	bool external;
	struct dptx_port dptxport[2];
	struct mutex tb_lock;
	bool tb_retiring;
	int (*tb_dpin_set_active)(void *, bool);
	void *tb_dpin_ctx;
	struct {
		bool active, ready;
		u64 generation;
		int (*set_active)(void *, bool);
		void *binding;
	} split;
};

struct dcp_fabric_route {
	struct dcp_fabric_route *next;
	void *pipeline;
	bool tunnel;
	unsigned int dpin;
	u8 tunnel_clock_blocked;
};
struct dcp_fabric_port {
	unsigned long key;
	struct dcp_fabric_port *next;
	struct dcp_fabric_route *routes;
	struct dcp_fabric_route *owner[2];
	struct dcp_fabric_route *preferred;
	u32 candidate_crtcs[2];
	bool wanted, hpd;
	void *plan;
};
struct apple_dcp_typec_route {
	struct apple_dcp *dcp;
	struct list_head port_link;
	struct dcp_fabric_route core;
	bool tunnel;
	unsigned int tunnel_dpin;
	u64 tunnel_generation;
	struct mux_control *dpin[2];
	struct mux_control *xbar;
};
struct apple_dcp_typec_port {
	struct dcp_fabric_port core;
	struct list_head routes;
	struct apple_dcp_typec_route *owner, *secondary_owner;
	struct apple_dcp *split_dcp;
	unsigned long tile_hint_deadline;
};
struct dcp_tb_attach_context {
	unsigned int dpin;
	bool active;
	u64 generation;
	int (*set_active)(void *, bool);
	void *binding;
	struct apple_dcp_typec_port *port;
	struct apple_dcp_typec_route **slot;
	bool planned;
};
struct dcp_fabric_attach_ops { int unused; };

static bool tiled_split = true;
static unsigned long jiffies = 1000;
static int dcp_typec_fabric_lock;
static const struct dcp_fabric_attach_ops dcp_tb_attach_ops;

/* --- recorded stubs --- */
static struct {
	unsigned int teardown, join, connect, release, attach, rebalance;
	unsigned int attach_dpin[4];
	int teardown_ret, connect_ret;
	struct apple_dcp *attach_dcp[2];
} st;

static int dcp_tb_split_teardown_locked(struct apple_dcp_typec_port *port, bool force)
{
	(void)force;
	st.teardown++;
	if (port->split_dcp) {
		port->split_dcp->split.active = false;
		port->split_dcp->split.generation = 0;
		port->split_dcp = NULL;
	}
	return st.teardown_ret;
}

static int dcp_tb_split_join_locked(struct apple_dcp_typec_port *port,
				    struct apple_dcp_typec_route *route,
				    struct mux_control *xbar, u64 generation,
				    int (*set_active)(void *, bool), void *binding)
{
	(void)xbar;
	st.join++;
	port->split_dcp = route->dcp;
	route->dcp->split.active = true;
	route->dcp->split.generation = generation;
	route->dcp->split.set_active = set_active;
	route->dcp->split.binding = binding;
	return 0;
}

static int dcp_dptx_connect(struct apple_dcp *dcp, unsigned int port)
{
	(void)dcp; (void)port;
	st.connect++;
	return st.connect_ret;
}

static int dcp_tb_release_locked(struct apple_dcp_typec_port *port, unsigned int dpin)
{
	st.release++;
	if (dpin)
		port->secondary_owner = NULL;
	else
		port->owner = NULL;
	return 0;
}

static struct apple_dcp_typec_route attach_route[2];

static int dcp_fabric_run_attach(const struct dcp_fabric_attach_ops *ops,
				 struct dcp_tb_attach_context *ctx)
{
	struct apple_dcp_typec_route *r = &attach_route[ctx->dpin];

	(void)ops;
	st.attach_dpin[st.attach++] = ctx->dpin;
	r->dcp = st.attach_dcp[ctx->dpin];
	r->tunnel = true;
	r->tunnel_dpin = ctx->dpin;
	r->tunnel_generation = ctx->generation;
	*ctx->slot = r;
	return 0;
}

static struct apple_dcp_typec_route *
dcp_typec_rebalance_locked(struct apple_dcp_typec_port *arriving, unsigned int dpin)
{
	(void)arriving; (void)dpin;
	st.rebalance++;
	return NULL;
}

/* PRODUCTION_FUNCTIONS */

/* --- topology --- */
static struct apple_dcp dcp_a, dcp_b;
static struct apple_dcp_typec_route route_a, route_b;
static struct apple_dcp_typec_port port;
static struct mux_chip chip;

static int set_active(void *binding, bool active) { (void)binding; (void)active; return 0; }

enum kind { NONE, ALL_EXTERNAL, ALL_INTERNAL, MIXED };

static void topology(enum kind kind, bool port1_ready)
{
	memset(&st, 0, sizeof(st));
	memset(&dcp_a, 0, sizeof(dcp_a));
	memset(&dcp_b, 0, sizeof(dcp_b));
	memset(&route_a, 0, sizeof(route_a));
	memset(&route_b, 0, sizeof(route_b));
	memset(&port, 0, sizeof(port));
	memset(attach_route, 0, sizeof(attach_route));
	INIT_LIST_HEAD(&port.routes);
	chip.controllers = 3;
	for (int i = 0; i < 3; i++)
		chip.mux[i].chip = &chip;
	dcp_a.external = kind == ALL_EXTERNAL || kind == MIXED;
	dcp_b.external = kind == ALL_EXTERNAL;
	dcp_a.dptxport[1].enabled = dcp_b.dptxport[1].enabled = port1_ready;
	route_a.dcp = &dcp_a;
	route_b.dcp = &dcp_b;
	route_a.xbar = route_b.xbar = &chip.mux[0];
	if (kind != NONE) {
		list_add_tail(&route_a.port_link, &port.routes);
		list_add_tail(&route_b.port_link, &port.routes);
	}
	st.attach_dcp[0] = &dcp_a;
	st.attach_dcp[1] = &dcp_b;
	tiled_split = true;
	jiffies = 1000;
	port.tile_hint_deadline = 3000;
}

/* The dpin0 tunnel route already on the port, on @dcp. */
static void owner_on(struct apple_dcp *dcp)
{
	attach_route[0].dcp = dcp;
	attach_route[0].tunnel = true;
	attach_route[0].tunnel_dpin = 0;
	attach_route[0].tunnel_generation = 5;
	attach_route[0].xbar = &chip.mux[0];
	port.owner = &attach_route[0];
}

static int request(unsigned int dpin, bool active, u64 generation)
{
	struct dcp_tb_attach_context ctx = {
		.dpin = dpin, .active = active, .generation = generation,
		.set_active = set_active, .binding = &port,
	};

	return dcp_tb_dispatch_request(&ctx, &port.core);
}

static int split(bool active, u64 generation)
{
	struct dcp_tb_attach_context ctx = {
		.dpin = 1, .active = active, .generation = generation,
		.set_active = set_active, .binding = &port,
	};

	return dcp_tb_split_locked(&port, &ctx);
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #c); exit(1); } } while (0)

/* T6030: every route an external processor. */
static void t6030_dpin1_alone(void)
{
	topology(ALL_EXTERNAL, true);
	CHECK(split(true, 9) == -EOPNOTSUPP);
	CHECK(request(1, true, 9) == 0);
	CHECK(st.attach == 1 && st.attach_dpin[0] == 1 && port.secondary_owner);
	CHECK(!st.join && !st.connect && !st.teardown);
}

static void t6030_dpin1_alone_before_services(void)
{
	/* No port 1 service yet: the identity decides, not readiness. */
	topology(ALL_EXTERNAL, false);
	CHECK(request(1, true, 9) == 0);
	CHECK(st.attach == 1 && !st.join);
}

static void t6030_dpin1_with_external_owner(void)
{
	topology(ALL_EXTERNAL, true);
	owner_on(&dcp_a);
	dcp_a.dptxport[0].tile_hint = true;
	CHECK(split(true, 9) == -EOPNOTSUPP);
	CHECK(request(1, true, 9) == 0);
	CHECK(st.attach == 1 && st.attach_dpin[0] == 1 && !st.join);
}

static void t6030_dpin1_then_dpin0(void)
{
	topology(ALL_EXTERNAL, true);
	CHECK(request(1, true, 9) == 0);
	CHECK(request(0, true, 10) == 0);
	CHECK(st.attach == 2 && st.attach_dpin[0] == 1 && st.attach_dpin[1] == 0);
	CHECK(port.owner && port.secondary_owner && !st.join);
}

static void t6030_dpin0_then_dpin1(void)
{
	topology(ALL_EXTERNAL, true);
	CHECK(request(0, true, 10) == 0);
	CHECK(request(1, true, 9) == 0);
	CHECK(st.attach == 2 && st.attach_dpin[0] == 0 && st.attach_dpin[1] == 1);
	CHECK(port.owner && port.secondary_owner && !st.join);
}

static void t6030_split_off(void)
{
	topology(ALL_EXTERNAL, true);
	tiled_split = false;
	CHECK(request(1, true, 9) == 0);
	CHECK(st.attach == 1 && !st.join);
}

static void t6030_inactive_dpin1(void)
{
	topology(ALL_EXTERNAL, true);
	CHECK(split(false, 9) == -EOPNOTSUPP);
	CHECK(request(1, false, 9) == -ESTALE);
	CHECK(!st.attach && !st.teardown && !st.release);
}

/* M1/M2: pipelines that can host a tile keep the dpin0 wait. */
static void internal_dpin1_alone_waits(void)
{
	topology(ALL_INTERNAL, true);
	CHECK(request(1, true, 9) == -ENODEV);
	CHECK(!st.attach && !st.join);
}

static void internal_not_ready_waits(void)
{
	/* Port 1 not announced yet is no reason to give the tile up. */
	topology(ALL_INTERNAL, false);
	CHECK(request(1, true, 9) == -ENODEV);
	CHECK(!st.attach);
}

static void mixed_routes_wait(void)
{
	topology(MIXED, true);
	CHECK(request(1, true, 9) == -ENODEV);
	CHECK(!st.attach);
}

static void no_routes_wait(void)
{
	topology(NONE, true);
	CHECK(request(1, true, 9) == -ENODEV);
	CHECK(!st.attach);
}

static void internal_tile_hint_pending(void)
{
	topology(ALL_INTERNAL, true);
	owner_on(&dcp_a);
	CHECK(request(1, true, 9) == -ENODEV);
	CHECK(!st.attach && !st.join);
	jiffies = 4000;
	CHECK(request(1, true, 9) == 0);
	CHECK(st.attach == 1 && !st.join);
}

static void internal_tiled_join(void)
{
	topology(ALL_INTERNAL, true);
	owner_on(&dcp_a);
	dcp_a.dptxport[0].tile_hint = true;
	CHECK(request(1, true, 9) == 0);
	CHECK(st.join == 1 && st.connect == 1 && !st.attach);
	CHECK(port.split_dcp == &dcp_a && dcp_a.split.ready);
}

static void internal_owner_without_port1(void)
{
	topology(ALL_INTERNAL, true);
	owner_on(&dcp_a);
	dcp_a.dptxport[1].enabled = false;
	dcp_a.dptxport[0].tile_hint = true;
	CHECK(request(1, true, 9) == 0);
	CHECK(st.attach == 1 && !st.join);
}

/* An existing split is handled before anything else, on any port. */
static void split_retired_teardown_first(void)
{
	topology(ALL_EXTERNAL, true);
	port.split_dcp = &dcp_a;
	dcp_a.split.active = true;
	dcp_a.split.generation = 0;
	CHECK(request(1, true, 9) == 0);
	CHECK(st.teardown == 1 && !port.split_dcp && st.attach == 1);
}

static void split_existing_release(void)
{
	topology(ALL_EXTERNAL, true);
	port.split_dcp = &dcp_a;
	dcp_a.split.active = true;
	dcp_a.split.generation = 7;
	dcp_a.split.set_active = set_active;
	dcp_a.split.binding = &port;
	CHECK(request(1, false, 7) == 0);
	CHECK(st.teardown == 1 && !port.split_dcp && !st.attach);
}

static void split_existing_active(void)
{
	topology(ALL_EXTERNAL, true);
	port.split_dcp = &dcp_a;
	dcp_a.split.active = true;
	dcp_a.split.generation = 7;
	dcp_a.split.set_active = set_active;
	dcp_a.split.binding = &port;
	CHECK(request(1, true, 7) == -EAGAIN);
	dcp_a.split.ready = true;
	CHECK(request(1, true, 7) == 0);
	CHECK(!st.teardown && !st.attach && port.split_dcp == &dcp_a);
}

static const struct { const char *name; void (*fn)(void); } cases[] = {
	{ "t6030-dpin1-alone", t6030_dpin1_alone },
	{ "t6030-dpin1-alone-before-services", t6030_dpin1_alone_before_services },
	{ "t6030-dpin1-with-external-owner", t6030_dpin1_with_external_owner },
	{ "t6030-dpin1-then-dpin0", t6030_dpin1_then_dpin0 },
	{ "t6030-dpin0-then-dpin1", t6030_dpin0_then_dpin1 },
	{ "t6030-split-off", t6030_split_off },
	{ "t6030-inactive-dpin1", t6030_inactive_dpin1 },
	{ "internal-dpin1-alone-waits", internal_dpin1_alone_waits },
	{ "internal-not-ready-waits", internal_not_ready_waits },
	{ "mixed-routes-wait", mixed_routes_wait },
	{ "no-routes-wait", no_routes_wait },
	{ "internal-tile-hint-pending", internal_tile_hint_pending },
	{ "internal-tiled-join", internal_tiled_join },
	{ "internal-owner-without-port1", internal_owner_without_port1 },
	{ "split-retired-teardown-first", split_retired_teardown_first },
	{ "split-existing-release", split_existing_release },
	{ "split-existing-active", split_existing_active },
};

int main(int argc, char **argv)
{
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		if (argc > 1 && !strcmp(argv[1], cases[i].name)) {
			cases[i].fn();
			return 0;
		}
	fprintf(stderr, "unknown case\n");
	return 2;
}
