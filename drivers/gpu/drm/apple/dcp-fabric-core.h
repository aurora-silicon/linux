/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef __APPLE_DCP_FABRIC_CORE_H__
#define __APPLE_DCP_FABRIC_CORE_H__

#include <linux/errno.h>
#include <linux/types.h>

/*
 * A Thunderbolt DP tunnel enters through one of up to four DP IN adapters of
 * the port's host router, each behind a crossbar output of its own. A port
 * carries two streams, primary and secondary: DP IN n and n + 2 share a
 * stream slot. Where every pipeline drives one DP IN of its own (T6050, fixed
 * CRTCs), DP IN 2 and 3 have owner slots of their own as well; see
 * dcp_fabric_tunnel_owner().
 */
#define DCP_FABRIC_DPIN_MAX	4

static inline unsigned int dcp_fabric_dpin_slot(unsigned int dpin)
{
	return dpin & 1;
}

/*
 * The owner slot (dcp_fabric_port.owner[]) of DP IN @dpin: its stream slot,
 * or the DP IN itself when @per_dpin, each pipeline taking one DP IN.
 */
static inline unsigned int dcp_fabric_tunnel_owner(unsigned int dpin,
						   bool per_dpin)
{
	return per_dpin ? dpin : dcp_fabric_dpin_slot(dpin);
}

enum dcp_fabric_presence_state {
	DCP_FABRIC_ABSENT,
	DCP_FABRIC_SETTLING,
	DCP_FABRIC_PRESENT,
};

struct dcp_fabric_presence {
	enum dcp_fabric_presence_state state;
	u64 generation;
	unsigned long deadline;
};

struct dcp_fabric_pipeline {
	struct dcp_fabric_pipeline *next;
	unsigned int crtc_index;
	bool bound;
	bool has_fixed;
	bool fixed_busy;
	bool fixed_recent;
	bool owned;
	bool tunnel_held;
	enum dcp_fabric_presence_state presence;
	bool terminal;
	bool services_ready;
	bool external;
	bool t6030_dpin;
};

struct dcp_fabric_route {
	struct dcp_fabric_route *next;
	struct dcp_fabric_pipeline *pipeline;
	bool tunnel;
	unsigned int dpin;
	/* DP IN adapters this source cannot clock; direct DP is unaffected. */
	u8 tunnel_clock_blocked;
	/* Carries DP tunnels only, never a direct DP-alt stream. */
	bool tunnel_only;
};

struct dcp_fabric_plan {
	struct dcp_fabric_route *target[2];
	bool dark;
};

/* Input topology is immutable; only the separate plan records are written. */
struct dcp_fabric_port {
	unsigned long key;
	struct dcp_fabric_port *next;
	struct dcp_fabric_route *routes;
	/*
	 * Indexed by dcp_fabric_tunnel_owner(): [0] the primary stream (direct
	 * DP, DP IN 0, and DP IN 2 unless per-DP IN), [1] the secondary (DP
	 * IN 1, and 3 unless per-DP IN), [2] and [3] DP IN 2 and 3 with per-DP
	 * IN slots.
	 */
	struct dcp_fabric_route *owner[DCP_FABRIC_DPIN_MAX];
	struct dcp_fabric_route *preferred;
	u32 candidate_crtcs[2];
	bool wanted;
	bool hpd;
	struct dcp_fabric_plan *plan;
};

struct dcp_fabric_policy {
	bool dual_stream;
	/* routes follow the CRTC a modeset pairs them with */
	bool follow;
	/* a hybrid pipeline is not kept back for a fixed display */
	bool hybrid_first;
};

enum dcp_fabric_wiring {
	DCP_FABRIC_SINGLE_STREAM,
	DCP_FABRIC_DUAL_NAMED,
	DCP_FABRIC_DUAL_LEGACY,
	DCP_FABRIC_INVALID_WIRING,
};

enum dcp_fabric_wiring dcp_fabric_wiring(bool dpin0, bool dpin1, bool legacy,
					 unsigned int endpoints);
bool dcp_fabric_t6020_flow(bool usb4, bool soc_support, bool connector_wired);
/*
 * T6030's even external sources feed DP IN0/1 through DPTX PCLK1/2. Keep this
 * pure policy helper independent of OF/PHY header dependencies. The fabric
 * tests check parity with apple_dp_tunnel_t6030_pclk() in dp-tunnel.h.
 */
static inline int dcp_fabric_t6030_pclk(unsigned int source, unsigned int dpin)
{
	return dpin < 2 && (source == 0 || source == 2) ? (int)dpin : -EOPNOTSUPP;
}
u8 dcp_fabric_tunnel_clock_blocked(bool t6030, unsigned int source);

enum dcp_fabric_capacity_action {
	DCP_FABRIC_PROMOTE,
	DCP_FABRIC_REBALANCE,
};

enum dcp_fabric_fixed_step {
	DCP_FABRIC_RESTORE_PHY,
	DCP_FABRIC_SELECT_MUX,
};

enum dcp_fabric_deactivate_step {
	DCP_FABRIC_KEEP_OWNER,
	DCP_FABRIC_CLEAR_OWNER,
	DCP_FABRIC_CONNECT_FIXED,
	DCP_FABRIC_REPLAN,
};

enum dcp_fabric_resume_step {
	DCP_FABRIC_ENABLE_HPD_IRQ,
	DCP_FABRIC_SAMPLE_HPD,
};

unsigned int dcp_fabric_deactivate_steps(int error, bool selected, bool fixed_live,
					 enum dcp_fabric_deactivate_step steps[3]);
void dcp_fabric_resume_steps(enum dcp_fabric_resume_step steps[2]);

enum dcp_fabric_follow {
	DCP_FABRIC_FOLLOW_STAY,
	DCP_FABRIC_FOLLOW_MOVE,
	DCP_FABRIC_FOLLOW_SWAP,
	DCP_FABRIC_FOLLOW_REFUSE,
};

enum dcp_fabric_follow
dcp_fabric_follow(const struct dcp_fabric_route *from,
		  const struct dcp_fabric_route *to,
		  const struct dcp_fabric_route *holder,
		  const struct dcp_fabric_route *holder_back, bool holder_off,
		  const struct dcp_fabric_policy *policy);

/* All preparation completes before destructive effects; failures restore sources. */
struct dcp_fabric_follow_ops {
	int (*prepare)(void *ctx, unsigned int slot);
	int (*validate)(void *ctx, unsigned int slot);
	int (*detach)(void *ctx, unsigned int slot, bool destination);
	bool (*retained)(void *ctx, unsigned int slot, bool destination);
	int (*attach)(void *ctx, unsigned int slot, bool restore);
	void (*publish)(void *ctx, unsigned int slot, bool restore);
	void (*lost)(void *ctx, unsigned int slot);
};

int dcp_fabric_follow_execute(const struct dcp_fabric_follow_ops *ops,
			      void *ctx, unsigned int count);

enum dcp_fabric_attach_action {
	DCP_FABRIC_ATTACH_OOB,
	DCP_FABRIC_ATTACH_WORK,
};

int dcp_fabric_tunnel_request(const struct dcp_fabric_port *ports,
			      unsigned long key, unsigned int dpin,
			      const struct dcp_fabric_port **found);
enum dcp_fabric_capacity_action dcp_fabric_capacity_action(bool keep_order);
bool dcp_fabric_fixed_busy(bool typec_only, bool independent, bool connected,
			   bool hpd);
bool dcp_fabric_available(const struct dcp_fabric_pipeline *pipeline,
			  const struct dcp_fabric_policy *policy);
u64 dcp_fabric_presence_edge(struct dcp_fabric_presence *presence,
			     unsigned long now, unsigned long window);
bool dcp_fabric_presence_sample(struct dcp_fabric_presence *presence,
				u64 generation, bool high,
				unsigned long now, unsigned long window);
bool dcp_fabric_presence_recheck(struct dcp_fabric_presence *presence,
				 u64 generation, bool high,
				 unsigned long now, unsigned long window);
bool dcp_fabric_presence_expire(struct dcp_fabric_presence *presence,
				u64 generation, bool high, unsigned long now);
unsigned int dcp_fabric_score(const struct dcp_fabric_pipeline *pipeline,
			      const struct dcp_fabric_policy *policy);
bool dcp_fabric_fits(const struct dcp_fabric_pipeline *pipeline,
		     const struct dcp_fabric_policy *policy, u32 mask,
		     bool connector_present);
bool dcp_fabric_keep_order(bool dual_stream, bool ready, bool frozen);
bool dcp_fabric_ordered_dual_stream(bool wired, bool t6030);
bool dcp_fabric_waiting(const struct dcp_fabric_port *port);
bool dcp_fabric_movable(const struct dcp_fabric_route *owner,
			const struct dcp_fabric_route *target);
struct dcp_fabric_route *
dcp_fabric_free_route(const struct dcp_fabric_port *port,
		      const struct dcp_fabric_policy *policy);
void dcp_fabric_plan(struct dcp_fabric_pipeline *pipelines,
		     const struct dcp_fabric_port *ports,
		     const struct dcp_fabric_port *arriving, unsigned int dpin);
int dcp_fabric_tunnel_slot(const struct dcp_fabric_port *port,
			   unsigned int dpin, bool per_dpin);
struct dcp_fabric_route *
dcp_fabric_tunnel_candidate(const struct dcp_fabric_port *port,
			    const struct dcp_fabric_policy *policy,
			    const struct dcp_fabric_route *planned,
			    bool ordered, unsigned int dpin,
			    bool connector_present, int *error);
unsigned int dcp_fabric_fixed_steps(bool has_fixed, bool owned, bool needs_mux,
				    enum dcp_fabric_fixed_step steps[2]);
u32 dcp_fabric_connector_mask(bool dual_stream, bool bound, bool routed,
			      unsigned int index, u32 candidates);
u32 dcp_fabric_port_crtc_mask(bool secondary, bool ordered_dual, bool typec_only,
			    u32 mask, u32 candidate);
enum dcp_fabric_attach_action dcp_fabric_attach_action(bool external);
bool dcp_fabric_t6030_link(bool tunnel, bool has_xbar, bool t6030_dpin);

/*
 * T6050's platform limit on merged (dual-pipe) modes: whether a display may
 * take a mode its own engine cannot drive, @extra engines more than its own,
 * while @displays external displays, it included, are present on the
 * platform's @engines external display engines. A full pair is given only
 * while the displays and the pair leave an external engine unused; on the
 * four engines of a T6050, a display pairs alone or with one other external
 * display present, not with two. Otherwise the platform offers only a limited
 * pipe, which the driver does not implement, and the display runs on its own
 * engine at a timing that one engine drives.
 */
bool dcp_fabric_merged_fits(unsigned int displays, unsigned int extra,
			    unsigned int engines);

#endif /* __APPLE_DCP_FABRIC_CORE_H__ */
