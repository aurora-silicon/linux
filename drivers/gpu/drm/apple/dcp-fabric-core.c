// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/limits.h>
#include <linux/jiffies.h>
#include <linux/module.h>

#include "dcp-fabric-core.h"

enum dcp_fabric_wiring dcp_fabric_wiring(bool dpin0, bool dpin1, bool legacy,
					 unsigned int endpoints)
{
	if (dpin0 != dpin1)
		return DCP_FABRIC_INVALID_WIRING;
	if (endpoints < 2)
		return DCP_FABRIC_SINGLE_STREAM;
	if (dpin0 && dpin1)
		return DCP_FABRIC_DUAL_NAMED;
	return legacy ? DCP_FABRIC_DUAL_LEGACY : DCP_FABRIC_SINGLE_STREAM;
}
EXPORT_SYMBOL_GPL(dcp_fabric_wiring);

bool dcp_fabric_t6020_flow(bool usb4, bool soc_support, bool route_wired)
{
	return usb4 && soc_support && route_wired;
}
EXPORT_SYMBOL_GPL(dcp_fabric_t6020_flow);

int dcp_fabric_tunnel_request(const struct dcp_fabric_port *ports,
			      unsigned long key, unsigned int dpin,
			      const struct dcp_fabric_port **found)
{
	const struct dcp_fabric_port *port;

	*found = NULL;
	if (!key || dpin > 1)
		return -EINVAL;
	for (port = ports; port; port = port->next) {
		if (port->key != key)
			continue;
		*found = port;
		return 0;
	}
	return -ENODEV;
}
EXPORT_SYMBOL_GPL(dcp_fabric_tunnel_request);

enum dcp_fabric_capacity_action dcp_fabric_capacity_action(bool keep_order)
{
	return keep_order ? DCP_FABRIC_REBALANCE : DCP_FABRIC_PROMOTE;
}
EXPORT_SYMBOL_GPL(dcp_fabric_capacity_action);

bool dcp_fabric_fixed_busy(bool typec_only, bool independent, bool connected,
			   bool hpd)
{
	return !typec_only && !independent && (connected || hpd);
}
EXPORT_SYMBOL_GPL(dcp_fabric_fixed_busy);

u64 dcp_fabric_presence_edge(struct dcp_fabric_presence *presence,
			     unsigned long now, unsigned long window)
{
	presence->generation++;
	presence->state = DCP_FABRIC_SETTLING;
	presence->deadline = now + window;
	return presence->generation;
}
EXPORT_SYMBOL_GPL(dcp_fabric_presence_edge);

bool dcp_fabric_presence_sample(struct dcp_fabric_presence *presence,
				u64 generation, bool high,
				unsigned long now, unsigned long window)
{
	if (generation != presence->generation)
		return false;
	presence->state = high ? DCP_FABRIC_PRESENT : DCP_FABRIC_SETTLING;
	presence->deadline = high ? 0 : now + window;
	return true;
}
EXPORT_SYMBOL_GPL(dcp_fabric_presence_sample);

bool dcp_fabric_presence_recheck(struct dcp_fabric_presence *presence,
				 u64 generation, bool high,
				 unsigned long now, unsigned long window)
{
	bool present = presence->state == DCP_FABRIC_PRESENT;

	if (generation != presence->generation || high == present)
		return false;
	/* Synthesize an edge which the masked GPIO detector could not record. */
	generation = dcp_fabric_presence_edge(presence, now, window);
	dcp_fabric_presence_sample(presence, generation, high, now, window);
	return true;
}
EXPORT_SYMBOL_GPL(dcp_fabric_presence_recheck);

bool dcp_fabric_presence_expire(struct dcp_fabric_presence *presence,
				u64 generation, bool high, unsigned long now)
{
	if (generation != presence->generation ||
	    presence->state != DCP_FABRIC_SETTLING ||
	    time_before(now, presence->deadline))
		return false;
	presence->state = high ? DCP_FABRIC_PRESENT : DCP_FABRIC_ABSENT;
	presence->deadline = 0;
	return !high;
}
EXPORT_SYMBOL_GPL(dcp_fabric_presence_expire);

bool dcp_fabric_available(const struct dcp_fabric_pipeline *pipeline,
			  const struct dcp_fabric_policy *policy)
{
	return !pipeline->owned && !pipeline->fixed_busy &&
		(policy->dual_stream || pipeline->presence == DCP_FABRIC_ABSENT);
}
EXPORT_SYMBOL_GPL(dcp_fabric_available);

unsigned int dcp_fabric_score(const struct dcp_fabric_pipeline *pipeline,
			      const struct dcp_fabric_policy *policy)
{
	if (!pipeline->bound)
		return UINT_MAX - 1;
	return pipeline->crtc_index +
	       (!policy->dual_stream && pipeline->has_fixed ? 100 : 0);
}
EXPORT_SYMBOL_GPL(dcp_fabric_score);

bool dcp_fabric_fits(const struct dcp_fabric_pipeline *pipeline,
		     const struct dcp_fabric_policy *policy, u32 mask,
		     bool connector_present)
{
	return !policy->dual_stream || !connector_present || !pipeline->bound ||
	       (mask & BIT(pipeline->crtc_index));
}
EXPORT_SYMBOL_GPL(dcp_fabric_fits);

bool dcp_fabric_keep_order(bool dual_stream, bool ready, bool frozen)
{
	return dual_stream && ready && !frozen;
}
EXPORT_SYMBOL_GPL(dcp_fabric_keep_order);

bool dcp_fabric_waiting(const struct dcp_fabric_port *port)
{
	return !port->owner[0] && port->wanted && port->hpd;
}
EXPORT_SYMBOL_GPL(dcp_fabric_waiting);

bool dcp_fabric_movable(const struct dcp_fabric_route *owner,
			const struct dcp_fabric_route *target)
{
	return owner && !owner->tunnel && owner != target;
}
EXPORT_SYMBOL_GPL(dcp_fabric_movable);

struct dcp_fabric_route *
dcp_fabric_free_route(const struct dcp_fabric_port *port,
		      const struct dcp_fabric_policy *policy)
{
	struct dcp_fabric_route *route, *best = NULL, *last = port->preferred;
	unsigned int best_score = UINT_MAX;

	for (route = port->routes; route; route = route->next) {
		unsigned int score;

		if (!dcp_fabric_available(route->pipeline, policy))
			continue;
		score = dcp_fabric_score(route->pipeline, policy);
		if (score < best_score) {
			best = route;
			best_score = score;
		}
	}
	if (!last || !dcp_fabric_available(last->pipeline, policy))
		return best;
	if (!policy->dual_stream && best &&
	    best_score < dcp_fabric_score(last->pipeline, policy))
		return best;
	return last;
}
EXPORT_SYMBOL_GPL(dcp_fabric_free_route);

static struct dcp_fabric_route *
dcp_fabric_port_route(const struct dcp_fabric_port *port,
		      const struct dcp_fabric_pipeline *pipeline)
{
	struct dcp_fabric_route *route;

	for (route = port->routes; route; route = route->next)
		if (route->pipeline == pipeline)
			return route;
	return NULL;
}

static void dcp_fabric_plan_pipeline(struct dcp_fabric_pipeline *pipeline,
				     const struct dcp_fabric_port *ports,
				     const struct dcp_fabric_port *arriving,
				     unsigned int dpin)
{
	const struct dcp_fabric_port *port;
	unsigned int stream;

	for (stream = 0; stream < 2; stream++) {
		for (port = ports; port; port = port->next) {
			struct dcp_fabric_route *route;
			bool wants;

			if (stream)
				wants = port->owner[1] ||
					(port == arriving && dpin == 1);
			else
				wants = (port->owner[0] &&
					 port->owner[0]->tunnel) ||
					(port->wanted && port->hpd &&
					 !port->plan->dark) ||
					(port == arriving && dpin == 0);
			if (!wants || port->plan->target[stream] ||
			    !(port->candidate_crtcs[stream] &
			      BIT(pipeline->crtc_index)))
				continue;
			route = dcp_fabric_port_route(port, pipeline);
			if (!route)
				continue;
			port->plan->target[stream] = route;
			return;
		}
	}
}

void dcp_fabric_plan(struct dcp_fabric_pipeline *pipelines,
		     const struct dcp_fabric_port *ports,
		     const struct dcp_fabric_port *arriving, unsigned int dpin)
{
	const struct dcp_fabric_port *port;
	struct dcp_fabric_pipeline *pipeline;
	bool again;

	for (port = ports; port; port = port->next)
		port->plan->dark = false;
	do {
		for (port = ports; port; port = port->next) {
			port->plan->target[0] = NULL;
			port->plan->target[1] = NULL;
		}
		/* The adapter supplies pipelines in CRTC index order. */
		for (pipeline = pipelines; pipeline; pipeline = pipeline->next)
			if (pipeline->tunnel_held || !pipeline->fixed_busy)
				dcp_fabric_plan_pipeline(pipeline, ports,
							 arriving, dpin);
		again = false;
		for (port = ports; port; port = port->next) {
			struct dcp_fabric_route *target = port->plan->target[0];

			if (port == arriving || !target ||
			    (port->owner[0] && port->owner[0]->tunnel) ||
			    !target->pipeline->tunnel_held)
				continue;
			port->plan->dark = true;
			again = true;
		}
	} while (again);
}
EXPORT_SYMBOL_GPL(dcp_fabric_plan);

/* 1 means an already handed, matching stream; zero admits allocation. */
int dcp_fabric_tunnel_slot(const struct dcp_fabric_port *port,
			   unsigned int dpin)
{
	const struct dcp_fabric_route *slot = port->owner[dpin];

	if (slot)
		return slot->tunnel && slot->dpin == dpin ? 1 : -EADDRINUSE;
	if (port->owner[0] && !port->owner[0]->tunnel)
		return -EBUSY;
	return 0;
}
EXPORT_SYMBOL_GPL(dcp_fabric_tunnel_slot);

struct dcp_fabric_route *
dcp_fabric_tunnel_candidate(const struct dcp_fabric_port *port,
			    const struct dcp_fabric_policy *policy,
			    const struct dcp_fabric_route *planned,
			    bool ordered, unsigned int dpin,
			    bool connector_present, int *error)
{
	struct dcp_fabric_route *route, *best = NULL;
	unsigned int best_score = UINT_MAX;
	bool waiting = false;

	for (route = port->routes; route; route = route->next) {
		const struct dcp_fabric_pipeline *pipeline = route->pipeline;
		unsigned int score;

		if ((ordered && route != planned) ||
		    !dcp_fabric_available(pipeline, policy))
			continue;
		if (!dcp_fabric_fits(pipeline, policy,
				     port->candidate_crtcs[dpin],
				     connector_present))
			continue;
		if (pipeline->terminal) {
			*error = -ESHUTDOWN;
			return NULL;
		}
		if (!pipeline->services_ready) {
			waiting = true;
			continue;
		}
		score = dcp_fabric_score(pipeline, policy);
		if (policy->dual_stream && !dpin && !pipeline->has_fixed &&
		    score < UINT_MAX - 100)
			score += 100;
		if (score < best_score) {
			best = route;
			best_score = score;
		}
	}
	*error = best ? 0 : waiting ? -EAGAIN : -EBUSY;
	return best;
}
EXPORT_SYMBOL_GPL(dcp_fabric_tunnel_candidate);

unsigned int dcp_fabric_deactivate_steps(int error, bool selected, bool fixed_live,
					 enum dcp_fabric_deactivate_step steps[3])
{
	unsigned int count = 0;

	if (error && selected) {
		steps[count++] = DCP_FABRIC_KEEP_OWNER;
	} else {
		steps[count++] = DCP_FABRIC_CLEAR_OWNER;
		if (fixed_live)
			steps[count++] = DCP_FABRIC_CONNECT_FIXED;
	}
	/* Preserve fixed-output effects before discarding the failed plan. */
	if (error)
		steps[count++] = DCP_FABRIC_REPLAN;
	return count;
}
EXPORT_SYMBOL_GPL(dcp_fabric_deactivate_steps);

void dcp_fabric_resume_steps(enum dcp_fabric_resume_step steps[2])
{
	steps[0] = DCP_FABRIC_ENABLE_HPD_IRQ;
	steps[1] = DCP_FABRIC_SAMPLE_HPD;
}
EXPORT_SYMBOL_GPL(dcp_fabric_resume_steps);

unsigned int dcp_fabric_fixed_steps(bool has_fixed, bool owned, bool needs_mux,
				    enum dcp_fabric_fixed_step steps[2])
{
	if (!has_fixed || owned)
		return 0;
	steps[0] = DCP_FABRIC_RESTORE_PHY;
	if (needs_mux) {
		steps[1] = DCP_FABRIC_SELECT_MUX;
		return 2;
	}
	return 1;
}
EXPORT_SYMBOL_GPL(dcp_fabric_fixed_steps);

u32 dcp_fabric_connector_mask(bool dual_stream, bool bound, bool routed,
			      unsigned int index, u32 candidates)
{
	return !dual_stream && bound && routed ? BIT(index) : candidates;
}
EXPORT_SYMBOL_GPL(dcp_fabric_connector_mask);

enum dcp_fabric_attach_action dcp_fabric_attach_action(bool external)
{
	return external ? DCP_FABRIC_ATTACH_WORK : DCP_FABRIC_ATTACH_OOB;
}
EXPORT_SYMBOL_GPL(dcp_fabric_attach_action);

bool dcp_fabric_t6030_link(bool tunnel, bool has_xbar, bool t6030_dpin)
{
	return tunnel && has_xbar && t6030_dpin;
}
EXPORT_SYMBOL_GPL(dcp_fabric_t6030_link);

MODULE_DESCRIPTION("Apple display fabric decisions");
MODULE_LICENSE("Dual MIT/GPL");
