/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Thunderbolt driver - Tunneling support
 *
 * Copyright (c) 2014 Andreas Noever <andreas.noever@gmail.com>
 * Copyright (C) 2019, Intel Corporation
 */

#ifndef TB_TUNNEL_H_
#define TB_TUNNEL_H_

#include <linux/atomic.h>

#include "tb.h"

enum tb_tunnel_type {
	TB_TUNNEL_PCI,
	TB_TUNNEL_DP,
	TB_TUNNEL_DMA,
	TB_TUNNEL_USB3,
};

/**
 * enum tb_tunnel_state - State of a tunnel
 * @TB_TUNNEL_INACTIVE: tb_tunnel_activate() is not called for the tunnel
 * @TB_TUNNEL_ACTIVATING: tb_tunnel_activate() returned successfully for the tunnel
 * @TB_TUNNEL_ACTIVE: The tunnel is fully active
 */
enum tb_tunnel_state {
	TB_TUNNEL_INACTIVE,
	TB_TUNNEL_ACTIVATING,
	TB_TUNNEL_ACTIVE,
};

/**
 * struct tb_tunnel - Tunnel between two ports
 * @kref: Reference count
 * @tb: Pointer to the domain
 * @src_port: Source port of the tunnel
 * @dst_port: Destination port of the tunnel. For discovered incomplete
 *	      tunnels may be %NULL or null adapter port instead.
 * @npaths: Number of paths in @paths
 * @pre_activate: Optional tunnel specific initialization called before
 *		  activation. Can touch hardware.
 * @activate: Optional tunnel specific activation/deactivation
 * @post_deactivate: Optional tunnel specific de-initialization called
 *		     after deactivation. Can touch hardware.
 * @destroy: Optional tunnel specific callback called when the tunnel
 *	     memory is being released. Should not touch hardware.
 * @maximum_bandwidth: Returns maximum possible bandwidth for this tunnel
 * @allocated_bandwidth: Return how much bandwidth is allocated for the tunnel
 * @alloc_bandwidth: Change tunnel bandwidth allocation
 * @consumed_bandwidth: Return how much bandwidth the tunnel consumes
 * @release_unused_bandwidth: Release all unused bandwidth
 * @reclaim_available_bandwidth: Reclaim back available bandwidth
 * @list: Tunnels are linked using this field
 * @type: Type of the tunnel
 * @state: Current state of the tunnel
 * @max_up: Maximum upstream bandwidth (Mb/s) available for the tunnel.
 *	    Only set if the bandwidth needs to be limited.
 * @max_down: Maximum downstream bandwidth (Mb/s) available for the tunnel.
 *	      Only set if the bandwidth needs to be limited.
 * @allocated_up: Allocated upstream bandwidth (only for USB3)
 * @allocated_down: Allocated downstream bandwidth (only for USB3)
 * @bw_mode: DP bandwidth allocation mode registers can be used to
 *	     determine consumed and allocated bandwidth
 * @dprx_started: DPRX negotiation was started (tb_dp_dprx_start() was called for it)
 * @dprx_canceled: Was DPRX capabilities read poll canceled
 * @host_dp_generation: Nonzero identity of the announced display handoff
 * @host_dp_notified: The NHI glue was told this DP tunnel is up (Apple hosts)
 * @host_dp_activated: The NHI post-activation hook owns display-side state
 * @host_pci_activated: The NHI post-activation hook owns PCIe host state
 * @dprx_timeout: If set DPRX capabilities read poll work will timeout after this passes
 * @dprx_work: Worker that is scheduled to poll completion of DPRX capabilities read
 * @callback: Callback called when DP tunnel is fully activated
 * @callback_data: Data for @callback
 * @paths: All paths required by the tunnel
 */
struct tb_tunnel {
	struct kref kref;
	struct tb *tb;
	struct tb_port *src_port;
	struct tb_port *dst_port;
	size_t npaths;
	int (*pre_activate)(struct tb_tunnel *tunnel);
	int (*activate)(struct tb_tunnel *tunnel, bool activate);
	void (*post_deactivate)(struct tb_tunnel *tunnel);
	void (*destroy)(struct tb_tunnel *tunnel);
	int (*maximum_bandwidth)(struct tb_tunnel *tunnel, int *max_up,
				 int *max_down);
	int (*allocated_bandwidth)(struct tb_tunnel *tunnel, int *allocated_up,
				   int *allocated_down);
	int (*alloc_bandwidth)(struct tb_tunnel *tunnel, int *alloc_up,
			       int *alloc_down);
	int (*consumed_bandwidth)(struct tb_tunnel *tunnel, int *consumed_up,
				  int *consumed_down);
	int (*release_unused_bandwidth)(struct tb_tunnel *tunnel);
	void (*reclaim_available_bandwidth)(struct tb_tunnel *tunnel,
					    int *available_up,
					    int *available_down);
	struct list_head list;
	enum tb_tunnel_type type;
	enum tb_tunnel_state state;
	int max_up;
	int max_down;
	int allocated_up;
	int allocated_down;
	bool bw_mode;
	/* host cap of a DP tunnel's link rate (Mb/s per lane, 0 none), and its bandwidth */
	u32 max_rate;
	int max_bw;
	/*
	 * Under TB_HOST_DP_SHARED_RATE (Mb/s per lane): the highest link rate
	 * the connection manager allows, the one the DPTX was offered, and the
	 * one both adapters take without the policy. @host_relink: to be set
	 * up again at a higher rate once the debounce is over (alone now);
	 * @host_relinked_up: set up so, and set up at HBR2 again if it does
	 * not come up.
	 */
	u32 host_rate_limit;
	u32 host_rate;
	u32 host_max_rate;
	bool host_relink;
	bool host_relinked_up;
	bool dprx_started;
	bool dprx_canceled;
	u64 host_dp_generation;
	bool host_dp_notified;
	bool host_dp_activated;
	bool host_pci_activated;
	ktime_t dprx_timeout;
	struct delayed_work dprx_work;
	void (*callback)(struct tb_tunnel *tunnel, void *data);
	void *callback_data;

	struct tb_path *paths[] __counted_by(npaths);
};

struct tb_tunnel *tb_tunnel_discover_pci(struct tb *tb, struct tb_port *down,
					 bool alloc_hopid);
struct tb_tunnel *tb_tunnel_alloc_pci(struct tb *tb, struct tb_port *up,
				      struct tb_port *down);
int tb_pci_tunnel_activate_host(struct tb_tunnel *tunnel);
int tb_pci_tunnel_deactivate_host(struct tb_tunnel *tunnel);
bool tb_tunnel_reserved_pci(struct tb_port *port, int *reserved_up,
			    int *reserved_down);
struct tb_tunnel *tb_tunnel_discover_dp(struct tb *tb, struct tb_port *in,
					bool alloc_hopid,
					void (*callback)(struct tb_tunnel *, void *),
					void *callback_data);
void tb_dp_tunnel_cancel_dprx(struct tb_tunnel *tunnel);
bool tb_apple_nhi_needs_bw_grant(struct tb_nhi *nhi);
int tb_dp_cap_bandwidth(struct tb_tunnel *tunnel, int bw);
unsigned int tb_dp_host_counted_bandwidth(u32 max_rate, u32 rate, u32 lanes);
int tb_dp_reduce_bandwidth_shared(int max_bw, u32 in_rate, u32 in_lanes, u32 out_rate,
				  u32 out_lanes, u32 shared_rate, u32 *new_rate, u32 *new_lanes);
int tb_dp_host_reduce_bandwidth(int max_bw, u32 in_rate, u32 in_lanes, u32 out_rate,
				u32 out_lanes, u32 shared_rate, u32 rate_limit,
				u32 *new_rate, u32 *new_lanes);
u32 tb_dp_host_shared_rate(const struct tb_tunnel *tunnel);

/* TB_HOST_DP_SHARED_RATE link rates (Mb/s per lane) */
#define TB_DP_HOST_RATE_ALONE	8100	/* HBR3: one DP tunnel on the host router */
#define TB_DP_HOST_RATE_SHARED	5400	/* HBR2: more than one */

/**
 * enum tb_dp_relink - Whether a host DP tunnel is set up again at another rate
 * @TB_DP_RELINK_NONE: It is not
 * @TB_DP_RELINK_WAIT: Not now, it is decided again later
 * @TB_DP_RELINK_NOW: It is, now
 */
enum tb_dp_relink {
	TB_DP_RELINK_NONE,
	TB_DP_RELINK_WAIT,
	TB_DP_RELINK_NOW,
};

/*
 * TB_HOST_DP_SHARED_RATE re-link work (ms): how long it waits to be decided
 * again (also while system sleep is in progress), and the first of its
 * TB_DP_RELINK_RETRIES retries, doubled each time, when the domain does not
 * runtime resume.
 */
#define TB_DP_RELINK_DELAY_MS	2500
#define TB_DP_RELINK_POLL_MS	500
#define TB_DP_RELINK_RETRIES	5

/*
 * A re-link to a higher rate waits TB_DP_RELINK_DELAY_MS (the debounce),
 * doubled up to TB_DP_RELINK_DELAY_MAX_MS while a second DP tunnel takes
 * the rate back down within TB_DP_RELINK_CALM_MS of such a re-link.
 */
#define TB_DP_RELINK_DELAY_MAX_MS	40000
#define TB_DP_RELINK_CALM_MS		60000

/**
 * struct tb_dp_pm_state - System sleep transitions, for host DP re-links
 * @sleeping: From the start of a transition (its PREPARE event, before any
 *	      other notifier) until it is over, its POST callbacks and any
 *	      rollback included (tb_dp_pm_reopen())
 * @reopen: The POST event came: reopen once the transition is over
 */
struct tb_dp_pm_state {
	bool sleeping;
	bool reopen;
};

u32 tb_dp_host_rate_for(unsigned int tunnels);
enum tb_dp_relink tb_dp_relink_down(u32 rate, unsigned int tunnels, bool sleeping);
bool tb_dp_pm_event(struct tb_dp_pm_state *s, unsigned long event);
void tb_dp_pm_reopen(struct tb_dp_pm_state *s);
unsigned int tb_dp_relink_retry_delay(unsigned int tries);
u32 tb_dp_host_rate_after(unsigned int tunnels, bool recovering);
bool tb_dp_relink_up_wanted(unsigned int tunnels, u32 rate, u32 max_rate, bool sleeping);
enum tb_dp_relink tb_dp_relink_up(bool marked, unsigned int tunnels, u32 rate, u32 max_rate,
				  bool pairable, bool sleeping, unsigned long now,
				  unsigned long due);
unsigned int tb_dp_relink_backoff(unsigned int delay_ms, bool flapping);
enum tb_dp_relink tb_dp_relink_activation(bool relinked_up, bool active);
bool tb_dp_host_drop_relink_up(struct tb_tunnel *tunnel);

struct tb_tunnel *tb_tunnel_alloc_dp(struct tb *tb, struct tb_port *in,
				     struct tb_port *out, int link_nr,
				     int max_up, int max_down,
				     void (*callback)(struct tb_tunnel *, void *),
				     void *callback_data);
struct tb_tunnel *tb_tunnel_alloc_dma(struct tb *tb, struct tb_port *nhi,
				      struct tb_port *dst, int transmit_path,
				      int transmit_ring, int receive_path,
				      int receive_ring);
bool tb_tunnel_match_dma(const struct tb_tunnel *tunnel, int transmit_path,
			 int transmit_ring, int receive_path, int receive_ring);
struct tb_tunnel *tb_tunnel_discover_usb3(struct tb *tb, struct tb_port *down,
					  bool alloc_hopid);
struct tb_tunnel *tb_tunnel_alloc_usb3(struct tb *tb, struct tb_port *up,
				       struct tb_port *down, int max_up,
				       int max_down);

void tb_tunnel_put(struct tb_tunnel *tunnel);
void tb_dp_tunnel_deactivate_host(struct tb_tunnel *tunnel);
u64 tb_dp_tunnel_alloc_generation(void);
u64 tb_dp_generation_alloc(atomic64_t *counter);
int tb_tunnel_activate(struct tb_tunnel *tunnel);
void tb_tunnel_deactivate(struct tb_tunnel *tunnel);

/**
 * tb_tunnel_is_active() - Is tunnel fully activated
 * @tunnel: Tunnel to check
 *
 * Return: %true if @tunnel is fully activated.
 *
 * Note for DP tunnels this returns %true only once the DPRX capabilities
 * read has been issued successfully. For other tunnels, this function
 * returns %true pretty much once tb_tunnel_activate() returns successfully.
 */
static inline bool tb_tunnel_is_active(const struct tb_tunnel *tunnel)
{
	return tunnel->state == TB_TUNNEL_ACTIVE;
}

bool tb_tunnel_is_invalid(struct tb_tunnel *tunnel);
bool tb_tunnel_port_on_path(const struct tb_tunnel *tunnel,
			    const struct tb_port *port);
int tb_tunnel_maximum_bandwidth(struct tb_tunnel *tunnel, int *max_up,
				int *max_down);
int tb_tunnel_allocated_bandwidth(struct tb_tunnel *tunnel, int *allocated_up,
				  int *allocated_down);
int tb_tunnel_alloc_bandwidth(struct tb_tunnel *tunnel, int *alloc_up,
			      int *alloc_down);
int tb_tunnel_consumed_bandwidth(struct tb_tunnel *tunnel, int *consumed_up,
				 int *consumed_down);
int tb_tunnel_release_unused_bandwidth(struct tb_tunnel *tunnel);
void tb_tunnel_reclaim_available_bandwidth(struct tb_tunnel *tunnel,
					   int *available_up,
					   int *available_down);

static inline bool tb_tunnel_is_pci(const struct tb_tunnel *tunnel)
{
	return tunnel->type == TB_TUNNEL_PCI;
}

static inline bool tb_tunnel_is_dp(const struct tb_tunnel *tunnel)
{
	return tunnel->type == TB_TUNNEL_DP;
}

static inline bool tb_tunnel_is_dma(const struct tb_tunnel *tunnel)
{
	return tunnel->type == TB_TUNNEL_DMA;
}

static inline bool tb_tunnel_is_usb3(const struct tb_tunnel *tunnel)
{
	return tunnel->type == TB_TUNNEL_USB3;
}

static inline bool tb_tunnel_direction_downstream(const struct tb_tunnel *tunnel)
{
	return tb_port_path_direction_downstream(tunnel->src_port,
						 tunnel->dst_port);
}

/**
 * enum tb_tunnel_event - Tunnel related events
 * @TB_TUNNEL_ACTIVATED: A tunnel was activated
 * @TB_TUNNEL_CHANGED: There is a tunneling change in the domain. Includes
 *		       full %TUNNEL_DETAILS if the tunnel in question is known
 *		       (ICM does not provide that information).
 * @TB_TUNNEL_DEACTIVATED: A tunnel was torn down
 * @TB_TUNNEL_LOW_BANDWIDTH: Tunnel bandwidth is not optimal
 * @TB_TUNNEL_NO_BANDWIDTH: There is not enough bandwidth for a tunnel
 */
enum tb_tunnel_event {
	TB_TUNNEL_ACTIVATED,
	TB_TUNNEL_CHANGED,
	TB_TUNNEL_DEACTIVATED,
	TB_TUNNEL_LOW_BANDWIDTH,
	TB_TUNNEL_NO_BANDWIDTH,
};

void tb_tunnel_event(struct tb *tb, enum tb_tunnel_event event,
		     enum tb_tunnel_type type,
		     const struct tb_port *src_port,
		     const struct tb_port *dst_port);

const char *tb_tunnel_type_name(const struct tb_tunnel *tunnel);

#define __TB_TUNNEL_PRINT(level, tunnel, fmt, arg...)                   \
	do {                                                            \
		struct tb_tunnel *__tunnel = (tunnel);                  \
		level(__tunnel->tb, "%llx:%u <-> %llx:%u (%s): " fmt,   \
		      tb_route(__tunnel->src_port->sw),                 \
		      __tunnel->src_port->port,                         \
		      tb_route(__tunnel->dst_port->sw),                 \
		      __tunnel->dst_port->port,                         \
		      tb_tunnel_type_name(__tunnel),			\
		      ## arg);                                          \
	} while (0)

#define tb_tunnel_WARN(tunnel, fmt, arg...) \
	__TB_TUNNEL_PRINT(tb_WARN, tunnel, fmt, ##arg)
#define tb_tunnel_warn(tunnel, fmt, arg...) \
	__TB_TUNNEL_PRINT(tb_warn, tunnel, fmt, ##arg)
#define tb_tunnel_info(tunnel, fmt, arg...) \
	__TB_TUNNEL_PRINT(tb_info, tunnel, fmt, ##arg)
#define tb_tunnel_dbg(tunnel, fmt, arg...) \
	__TB_TUNNEL_PRINT(tb_dbg, tunnel, fmt, ##arg)

#endif
