/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_DCP_FABRIC_EFFECTS_H__
#define __APPLE_DCP_FABRIC_EFFECTS_H__

#include "dcp-fabric-core.h"

/* Effect executors shared by the driver and KUnit's fake hardware. */
static inline void dcp_fabric_run_capacity(bool available, void (*notify)(void *ctx),
					   void *ctx)
{
	if (available)
		notify(ctx);
}

struct dcp_fabric_resume_ops {
	void (*enable_irq)(void *ctx);
	void (*sample)(void *ctx);
};

static inline void dcp_fabric_run_resume(const struct dcp_fabric_resume_ops *ops,
					 void *ctx)
{
	enum dcp_fabric_resume_step steps[2];
	unsigned int i;

	dcp_fabric_resume_steps(steps);
	for (i = 0; i < 2; i++) {
		switch (steps[i]) {
		case DCP_FABRIC_ENABLE_HPD_IRQ:
			ops->enable_irq(ctx);
			break;
		case DCP_FABRIC_SAMPLE_HPD:
			ops->sample(ctx);
			break;
		}
	}
}

struct dcp_fabric_resume_sample_ops {
	u64 (*edge)(void *ctx);
	int (*read_hpd)(void *ctx);
	bool (*sample)(void *ctx, u64 generation, int level);
	bool (*borrowed)(void *ctx);
	void (*connect_fixed)(void *ctx);
};

static inline void
dcp_fabric_run_resume_sample(const struct dcp_fabric_resume_sample_ops *ops, void *ctx)
{
	u64 generation = ops->edge(ctx);
	int level = ops->read_hpd(ctx);

	if (ops->sample(ctx, generation, level) && level > 0 && !ops->borrowed(ctx))
		ops->connect_fixed(ctx);
}

struct dcp_fabric_fixed_ops {
	void (*restore_phy)(void *ctx);
	int (*select_mux)(void *ctx);
};

static inline int dcp_fabric_run_fixed(bool has_fixed, bool owned, bool needs_mux,
				       const struct dcp_fabric_fixed_ops *ops, void *ctx)
{
	enum dcp_fabric_fixed_step steps[2];
	unsigned int count, i;
	int ret;

	count = dcp_fabric_fixed_steps(has_fixed, owned, needs_mux, steps);
	for (i = 0; i < count; i++) {
		switch (steps[i]) {
		case DCP_FABRIC_RESTORE_PHY:
			ops->restore_phy(ctx);
			break;
		case DCP_FABRIC_SELECT_MUX:
			ret = ops->select_mux(ctx);
			if (ret)
				return ret;
			break;
		}
	}
	return 0;
}

struct dcp_fabric_hdmi_ops {
	bool (*borrowed)(void *ctx);
	bool (*keep_order)(void *ctx);
	bool (*dual_stream)(void *ctx);
	int (*read_hpd)(void *ctx);
	void (*wait)(void *ctx, unsigned int ms);
	void (*rebalance)(void *ctx);
	void (*waiting)(void *ctx);
	void (*observed)(void *ctx, bool high, bool debounced);
	void (*connect_fixed)(void *ctx);
};

static inline void dcp_fabric_run_hdmi(const struct dcp_fabric_hdmi_ops *ops,
				       void *ctx)
{
	bool connected;

	if (ops->borrowed(ctx)) {
		if (ops->keep_order(ctx) && ops->read_hpd(ctx)) {
			ops->wait(ctx, 500);
			if (ops->read_hpd(ctx))
				ops->rebalance(ctx);
		} else if (!ops->dual_stream(ctx) && ops->read_hpd(ctx)) {
			ops->waiting(ctx);
		}
		return;
	}
	connected = ops->read_hpd(ctx);
	ops->observed(ctx, connected, false);
	if (connected) {
		ops->wait(ctx, 500);
		connected = ops->read_hpd(ctx);
		ops->observed(ctx, connected, true);
	}
	if (connected)
		ops->connect_fixed(ctx);
}

struct dcp_fabric_deactivate_ops {
	int (*deactivate)(void *ctx);
	bool (*selected)(void *ctx);
	bool (*fixed_live)(void *ctx);
	void (*keep_owner)(void *ctx);
	void (*clear_owner)(void *ctx);
	void (*connect_fixed)(void *ctx);
};

/* True discards the cached remainder and re-enters with a new snapshot. */
static inline bool
dcp_fabric_run_deactivate(const struct dcp_fabric_deactivate_ops *ops, void *ctx)
{
	enum dcp_fabric_deactivate_step steps[3];
	unsigned int count, i;
	int ret = ops->deactivate(ctx);
	bool selected = ops->selected(ctx);
	bool fixed_live = !(ret && selected) && ops->fixed_live(ctx);

	count = dcp_fabric_deactivate_steps(ret, selected, fixed_live, steps);
	for (i = 0; i < count; i++) {
		switch (steps[i]) {
		case DCP_FABRIC_KEEP_OWNER:
			ops->keep_owner(ctx);
			break;
		case DCP_FABRIC_CLEAR_OWNER:
			ops->clear_owner(ctx);
			break;
		case DCP_FABRIC_CONNECT_FIXED:
			ops->connect_fixed(ctx);
			break;
		case DCP_FABRIC_REPLAN:
			return true;
		}
	}
	return false;
}

struct dcp_fabric_rebalance_ops {
	void (*plan)(void *ctx);
	void *(*first)(void *ctx);
	void *(*next)(void *ctx, void *port);
	bool (*deactivate)(void *ctx, void *port);
	bool (*activate)(void *ctx, void *port);
};

static inline void
dcp_fabric_run_rebalance(const struct dcp_fabric_rebalance_ops *ops, void *ctx)
{
	unsigned int attempts;
	void *port;

	for (attempts = 0; ; attempts++) {
		ops->plan(ctx);
		if (attempts == 2)
			return;
		for (port = ops->first(ctx); port; port = ops->next(ctx, port))
			if (ops->deactivate(ctx, port))
				break;
		if (port)
			continue;
		for (port = ops->first(ctx); port; port = ops->next(ctx, port))
			if (ops->activate(ctx, port))
				break;
		if (!port)
			return;
	}
}

static inline int dcp_fabric_run_probe(bool has_routes,
				       int (*register_routes)(void *ctx), void *ctx)
{
	return has_routes ? register_routes(ctx) : 0;
}

struct dcp_fabric_attach_ops {
	int (*candidate)(void *ctx);
	int (*activate)(void *ctx);
	void (*rollback)(void *ctx);
	void (*claim)(void *ctx);
	bool (*external)(void *ctx);
	bool (*has_connector)(void *ctx);
	void (*queue_reconnect)(void *ctx);
	void (*connect_oob)(void *ctx);
};

static inline int dcp_fabric_run_attach(const struct dcp_fabric_attach_ops *ops,
					void *ctx)
{
	int ret = ops->candidate(ctx);

	if (ret)
		return ret;
	ret = ops->activate(ctx);
	if (ret) {
		ops->rollback(ctx);
		return ret;
	}
	ops->claim(ctx);
	if (dcp_fabric_attach_action(ops->external(ctx)) == DCP_FABRIC_ATTACH_WORK)
		ops->queue_reconnect(ctx);
	else if (ops->has_connector(ctx))
		ops->connect_oob(ctx);
	return 0;
}

static inline int
dcp_fabric_run_request(const struct dcp_fabric_port *ports, unsigned long key,
		       unsigned int dpin,
		       int (*dispatch)(void *ctx, const struct dcp_fabric_port *port),
		       void *ctx)
{
	const struct dcp_fabric_port *port;
	int ret = dcp_fabric_tunnel_request(ports, key, dpin, &port);

	return ret ? ret : dispatch(ctx, port);
}

struct dcp_fabric_link_ops {
	bool (*has_xbar)(void *ctx);
	bool (*tunnel)(void *ctx);
	bool (*t6020_xbar)(void *ctx);
	bool (*t6030_soc)(void *ctx);
	void (*record_soc)(void *ctx, bool t6030);
};

static inline bool dcp_fabric_run_t6030_link(const struct dcp_fabric_link_ops *ops,
					     void *ctx)
{
	bool t6030;

	if (!ops->has_xbar(ctx))
		return false;
	t6030 = ops->t6030_soc(ctx) && ops->t6020_xbar(ctx);
	ops->record_soc(ctx, t6030);
	return dcp_fabric_t6030_link(ops->tunnel(ctx), true, t6030);
}

#endif /* __APPLE_DCP_FABRIC_EFFECTS_H__ */
