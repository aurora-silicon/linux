// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Host fixture for the Thunderbolt DPIN1 dispatch and tiled-display split
 * admission. The production bodies of dcp_tb_dispatch_request(),
 * dcp_tb_split_locked(), dcp_typec_port_untileable(),
 * dcp_typec_route_no_dp_alt(), dcp_fabric_tunnel_slot(),
 * dcp_fabric_dpin_slot() and dcp_fabric_binding_request() are pasted in,
 * after DCP_FABRIC_DPIN_MAX and enum dcp_firmware_version;
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

/* FABRIC_DEFINES */
static inline unsigned int dcp_fabric_dpin_slot(unsigned int dpin);

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
struct device_node { const char *compatible; };
struct device { struct device_node *of_node; };
static bool of_device_is_compatible(const struct device_node *np, const char *compatible)
{
	return np && np->compatible && !strcmp(np->compatible, compatible);
}
struct mux_chip;
struct mux_control { struct mux_chip *chip; };
struct mux_chip { struct mux_control mux[3]; unsigned int controllers; };

struct dptx_port { bool enabled, connected, tile_hint; };
struct apple_dcp {
	struct device *dev;
	enum dcp_firmware_version fw_compat;
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
	struct mux_control *dpin[DCP_FABRIC_DPIN_MAX];
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
	struct apple_dcp *attach_dcp[DCP_FABRIC_DPIN_MAX];
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
	if (dcp_fabric_dpin_slot(dpin))
		port->secondary_owner = NULL;
	else
		port->owner = NULL;
	return 0;
}

static struct apple_dcp_typec_route attach_route[DCP_FABRIC_DPIN_MAX];

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
static struct device dev_a, dev_b;
static struct device_node np_a, np_b;
static struct apple_dcp_typec_route route_a, route_b;
static struct apple_dcp_typec_port port;
static struct mux_chip chip;

static int set_active(void *binding, bool active) { (void)binding; (void)active; return 0; }

enum kind { NONE, ALL_EXTERNAL, ALL_INTERNAL, MIXED, T8132_DCPEXT, T6050_FW27 };

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
	/* T6050: pipelines on the AP's own display fabric, firmware 27 */
	if (kind == T6050_FW27)
		dcp_a.fw_compat = dcp_b.fw_compat = DCP_FIRMWARE_V_27_0;
	/* A T8132 dcpext is no external processor: only its compatible says so. */
	np_a.compatible = np_b.compatible =
		kind == T8132_DCPEXT ? "apple,t8132-dcpext" : "apple,dcp";
	dev_a.of_node = &np_a;
	dev_b.of_node = &np_b;
	dcp_a.dev = &dev_a;
	dcp_b.dev = &dev_b;
	dcp_a.dptxport[1].enabled = dcp_b.dptxport[1].enabled = port1_ready;
	route_a.dcp = &dcp_a;
	route_b.dcp = &dcp_b;
	route_a.xbar = route_b.xbar = &chip.mux[0];
	if (kind != NONE) {
		list_add_tail(&route_a.port_link, &port.routes);
		list_add_tail(&route_b.port_link, &port.routes);
	}
	for (int i = 0; i < DCP_FABRIC_DPIN_MAX; i++)
		st.attach_dcp[i] = i & 1 ? &dcp_b : &dcp_a;
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

/*
 * T6050 (firmware 27): each pipeline drives one DP IN, so a dock's second
 * monitor gets a pipeline of its own, even with a tile hint.
 */
static void t6050_dpin1_alone(void)
{
	topology(T6050_FW27, true);
	CHECK(split(true, 9) == -EOPNOTSUPP);
	CHECK(request(1, true, 9) == 0);
	CHECK(st.attach == 1 && st.attach_dpin[0] == 1 && port.secondary_owner);
	CHECK(!st.join && !st.connect && !st.teardown);
}

static void t6050_dpin1_with_tile_hint(void)
{
	topology(T6050_FW27, true);
	owner_on(&dcp_a);
	dcp_a.dptxport[0].tile_hint = true;
	CHECK(split(true, 9) == -EOPNOTSUPP);
	CHECK(request(1, true, 9) == 0);
	CHECK(st.attach == 1 && st.attach_dpin[0] == 1 && !st.join);
}

/* DP IN n and n + 2 share a stream slot: DP IN 3 is a second stream. */
static void t6050_dpin3_alone(void)
{
	topology(T6050_FW27, true);
	CHECK(request(3, true, 9) == 0);
	CHECK(st.attach == 1 && st.attach_dpin[0] == 3);
	CHECK(port.secondary_owner == &attach_route[3] && !port.owner);
	CHECK(!st.join && !st.teardown);
}

static void t6050_dpin3_shares_slot(void)
{
	topology(T6050_FW27, true);
	CHECK(request(3, true, 9) == 0);
	CHECK(request(1, true, 10) == -EADDRINUSE);
	CHECK(st.attach == 1 && port.secondary_owner == &attach_route[3]);
	/* DP IN 2 takes the primary slot beside it. */
	CHECK(request(2, true, 11) == 0);
	CHECK(st.attach == 2 && st.attach_dpin[1] == 2 && port.owner == &attach_route[2]);
	CHECK(!st.join && !st.teardown);
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

/* T8132: a dcpext has no DP alt mode and no tiles; dpin1 is a display of its own. */
static void t8132_dcpext_dpin1_alone(void)
{
	topology(T8132_DCPEXT, true);
	CHECK(split(true, 9) == -EOPNOTSUPP);
	CHECK(request(1, true, 9) == 0);
	CHECK(st.attach == 1 && st.attach_dpin[0] == 1 && port.secondary_owner);
	CHECK(!st.join && !st.connect && !st.teardown);
}

static void t8132_dcpext_dpin1_with_tile_hint(void)
{
	topology(T8132_DCPEXT, true);
	owner_on(&dcp_a);
	dcp_a.dptxport[0].tile_hint = true;
	CHECK(split(true, 9) == -EOPNOTSUPP);
	CHECK(request(1, true, 9) == 0);
	CHECK(st.attach == 1 && st.attach_dpin[0] == 1 && !st.join);
}

static const struct { const char *name; void (*fn)(void); } cases[] = {
	{ "t6030-dpin1-alone", t6030_dpin1_alone },
	{ "t6030-dpin1-alone-before-services", t6030_dpin1_alone_before_services },
	{ "t6030-dpin1-with-external-owner", t6030_dpin1_with_external_owner },
	{ "t6030-dpin1-then-dpin0", t6030_dpin1_then_dpin0 },
	{ "t6030-dpin0-then-dpin1", t6030_dpin0_then_dpin1 },
	{ "t6030-split-off", t6030_split_off },
	{ "t6030-inactive-dpin1", t6030_inactive_dpin1 },
	{ "t6050-dpin1-alone", t6050_dpin1_alone },
	{ "t6050-dpin1-with-tile-hint", t6050_dpin1_with_tile_hint },
	{ "t6050-dpin3-alone", t6050_dpin3_alone },
	{ "t6050-dpin3-shares-slot", t6050_dpin3_shares_slot },
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
	{ "t8132-dcpext-dpin1-alone", t8132_dcpext_dpin1_alone },
	{ "t8132-dcpext-dpin1-with-tile-hint", t8132_dcpext_dpin1_with_tile_hint },
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
