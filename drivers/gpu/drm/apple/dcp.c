// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2021 Alyssa Rosenzweig */

#include <linux/align.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mux/driver.h>
#include <linux/soc/apple/dp-tunnel.h>
#include <linux/moduleparam.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/string.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/workqueue.h>

#include <drm/drm_edid.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_module.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "afk.h"
#include "av.h"
#include "dcp.h"
#include "dcp-internal.h"
#include "iomfb.h"
#include "parser.h"
#include "trace.h"

#define APPLE_DCP_COPROC_CPU_CONTROL	 0x44
#define APPLE_DCP_COPROC_CPU_CONTROL_RUN BIT(4)

#define DCP_BOOT_TIMEOUT msecs_to_jiffies(1000)

static bool show_notch;
module_param(show_notch, bool, 0644);
MODULE_PARM_DESC(show_notch, "Use the full display height and shows the notch");

bool hdmi_audio;
module_param(hdmi_audio, bool, 0644);
MODULE_PARM_DESC(hdmi_audio, "Enable unstable HDMI audio support");

static bool unstable_edid = true;
module_param(unstable_edid, bool, 0644);
MODULE_PARM_DESC(unstable_edid, "Enable unstable EDID retrival support");

struct apple_dcp_typec_port {
	struct list_head link;
	struct list_head routes;
	struct device_node *connector_np;
	struct apple_dcp_typec_route *owner;
	struct apple_dcp_typec_route *secondary_owner;
	/* Keep a port on its last DCP while that pipeline remains free. */
	struct apple_dcp_typec_route *preferred_route;
	/* Ignore the USB4 fallback immediately following this port's DP teardown. */
	unsigned long dp_release_deadline;
	/* DRM connector for this physical port, driven by whichever DCP owns it */
	struct apple_connector *connector;
	/* A second logical stream through this port's USB4 dock. */
	struct apple_connector *secondary_connector;
	/* last mux state acted on, to collapse the per-candidate notifications */
	struct typec_altmode *applied_alt;
	unsigned long applied_mode;
	u32 applied_status;
	u32 applied_conf;
	bool applied_valid;
	bool hpd;
	/* Direct DP-alt: the port is in DP mode, routed or not, and its HPD. */
	bool dp_wanted;
	bool dp_hpd;
	/* dcp_typec_rebalance_locked()'s plan for the port's two streams */
	struct apple_dcp_typec_route *target;
	struct apple_dcp_typec_route *secondary_target;
	/* left out of the plan: its planned pipeline is held by a tunnel */
	bool plan_dark;
};

static DEFINE_MUTEX(dcp_typec_fabric_lock);
static LIST_HEAD(dcp_typec_ports);

static int dcp_dptx_connect(struct apple_dcp *dcp, u32 port);
static void disconnected_hpd_event(struct apple_connector *connector);

bool dcp_is_typec_output(struct apple_dcp *dcp)
{
	return dcp->active_typec_route ||
	       dcp->fixed_connector_type == DRM_MODE_CONNECTOR_USB;
}

bool dcp_is_usb4_output(struct apple_dcp *dcp)
{
	return dcp->active_typec_route && dcp->active_typec_route->tunnel;
}

static bool dcp_typec_route_is_dp(const struct typec_mux_state *state)
{
	return state->alt && state->alt->svid == USB_TYPEC_DP_SID &&
	       state->mode >= TYPEC_DP_STATE_A &&
	       state->mode <= TYPEC_DP_STATE_F;
}

/* Keep a live fixed output on its own pipeline. */
static bool dcp_typec_route_fixed_output_busy(struct apple_dcp_typec_route *route)
{
	struct apple_dcp *dcp = route->dcp;

	if (dcp->fixed_connector_type == DRM_MODE_CONNECTOR_USB)
		return false;
	if (dcp->fixed_connector && dcp->fixed_connector->connected)
		return true;
	if (dcp->hdmi_hpd && gpiod_get_value_cansleep(dcp->hdmi_hpd))
		return true;

	return false;
}

static bool dcp_typec_route_available(struct apple_dcp_typec_route *route)
{
	return !route->dcp->active_typec_route &&
	       !dcp_typec_route_fixed_output_busy(route);
}

/*
 * Machines whose USB4 docks carry two independent DP streams through one
 * port: DPIN0 drives the hybrid dcpext0, DPIN1 the Type-C-only dcpext1, and
 * each port has a second connector for the DPIN1 stream.
 */
bool dcp_typec_dual_stream(void)
{
	return apple_dp_tunnel_t602x();
}

bool dcp_is_typec_only(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return !dcp->fixed_phy;
}

/*
 * Can this route feed the given connector without changing its
 * possible_crtcs?  On dual-stream machines those are fixed at probe, since
 * compositors read them once when the connector appears and pair the
 * connector with a CRTC before this fabric has routed it.
 */
static bool dcp_typec_route_fits(struct apple_dcp_typec_route *route,
				 struct apple_connector *connector)
{
	struct apple_dcp *dcp = route->dcp;

	if (!dcp_typec_dual_stream() || !connector || !dcp->crtc)
		return true;
	return connector->candidate_crtcs & drm_crtc_mask(&dcp->crtc->base);
}

static int dcp_typec_route_activate(struct apple_dcp_typec_route *route,
				    struct mux_control *xbar);
static int dcp_typec_route_deactivate(struct apple_dcp_typec_route *route);
static int dcp_dpxbar_preselect(struct mux_control *mux, int state);
static int dcp_dptx_disconnect(struct apple_dcp *dcp, u32 port);

static int dcp_dpxbar_tunnel_select_source(struct mux_control *mux, int state)
{
	typeof(&apple_dpxbar_tunnel_select_source) select =
		symbol_get(apple_dpxbar_tunnel_select_source);
	int ret;

	if (!select)
		return -ENOENT;
	ret = select(mux, state);
	symbol_put(apple_dpxbar_tunnel_select_source);
	return ret;
}

/*
 * For a port without a prior owner, rank pipelines by CRTC index. A pipeline
 * whose fixed output is live is not a candidate at all, so a hybrid is only
 * ever ranked here when it is genuinely free.
 */
static unsigned int dcp_typec_route_score(struct apple_dcp_typec_route *route)
{
	struct apple_dcp *dcp = route->dcp;

	if (!dcp->crtc)
		return UINT_MAX - 1;

	return drm_crtc_index(&dcp->crtc->base);
}

static int dcp_typec_route_activate(struct apple_dcp_typec_route *route,
				    struct mux_control *xbar)
{
	struct apple_dcp *dcp = route->dcp;
	struct apple_connector *connector =
		xbar != route->xbar && route->tunnel_dpin == 1 ?
		route->port->secondary_connector : route->port->connector;
	int ret;

	/*
	 * The fixed output's HPD handler leaves disconnects to DCP, so the port
	 * can still be marked connected to a display that is gone. Release it
	 * (a no-op otherwise), or connecting the borrowed route returns early.
	 */
	dcp_dptx_disconnect(dcp, 0);

	if (dcp->fixed_route_selected) {
		ret = mux_control_deselect(dcp->xbar);
		if (ret)
			return ret;
		dcp->fixed_route_selected = false;
	}

	/*
	 * Thunderbolt DP IN: the crossbar connection may only be brought up
	 * once DCP has configured the link and the tunnel pixel clock runs,
	 * see dcp_tunnel_crossbar_up(). Just remember the output here.
	 */
	if (xbar != route->xbar && route->mux_index) {
		/*
		 * The T602X crossbar must point a DP IN at its pipeline before
		 * DCP probes AUX; other crossbars select it at link-up and
		 * report -EOPNOTSUPP here.
		 */
		ret = dcp_dpxbar_tunnel_select_source(xbar, route->mux_index);
		if (ret == -EOPNOTSUPP)
			ret = 0;
	} else {
		ret = xbar == route->xbar ?
			mux_control_select(xbar, route->mux_index) : 0;
	}
	if (ret) {
		if (dcp->xbar) {
			int restore_ret;

			restore_ret = mux_control_select(dcp->xbar,
							 dcp->fixed_mux_index);
			if (!restore_ret)
				dcp->fixed_route_selected = true;
			else
				dev_err(dcp->dev,
					"failed to restore fixed display route: %d\n",
					restore_ret);
		}
		return ret;
	}

	dcp->phy = route->phy;
	dcp->dptx_phy = route->dptx_phy;
	dcp->connector_type = DRM_MODE_CONNECTOR_USB;
	if (connector) {
		WRITE_ONCE(connector->dcp, to_platform_device(dcp->dev));
		dcp->typec_connector = connector;
		dcp->connector = connector;

		/*
		 * Narrow the port to the pipeline now driving it.  The encoder
		 * spans every pipeline that could, which is what lets userspace
		 * see the port as usable at all -- but only one of them is
		 * routed to the display, and userspace has no way to tell which.
		 * Offering it the choice makes it pair the port with a pipeline
		 * holding a different monitor's mode list, and the modeset is
		 * rejected with no way for it to recover.  The hotplug that
		 * follows makes it re-read this.  Dual-stream machines keep
		 * fixed possible_crtcs instead: compositors that read them once
		 * (e.g. aquamarine/Hyprland) never see the narrowing.
		 */
		if (connector->port_encoder && dcp->crtc &&
		    !dcp_typec_dual_stream())
			connector->port_encoder->possible_crtcs =
				drm_crtc_mask(&dcp->crtc->base);
	}
	dcp->active_typec_route = route;
	scoped_guard(mutex, &dcp->tb_lock) {
		route->active_xbar = xbar;
		route->tunnel = xbar != route->xbar;
		route->xbar_up = !route->tunnel;
		dcp->dptx_tunnel = route->tunnel;
		/* crossbar controls are dpphy, dpin0, dpin1: same order as the DFP port */
		dcp->dptx_dfp_port = route->tunnel ? xbar - &route->xbar->chip->mux[0] : 0;
		dcp->tb_clock_ok = false;
	}
	route->selected = true;

	dev_info(dcp->dev, "allocated Type-C DPTX PHY %u\n", route->dptx_phy);
	return 0;
}

static int dcp_typec_route_deactivate(struct apple_dcp_typec_route *route)
{
	struct apple_dcp *dcp = route->dcp;
	struct apple_connector *connector = dcp->typec_connector;
	struct mux_control *active_xbar = route->active_xbar;
	bool was_tunnel = route->tunnel;
	int ret = 0;

	/*
	 * Under tb_lock so a DCP link (re)configuration can neither select the
	 * crossbar nor restart the tunnel pixel clock behind our back.
	 */
	scoped_guard(mutex, &dcp->tb_lock) {
		if (!route->tunnel || route->xbar_up)
			ret = mux_control_deselect(route->active_xbar ?: route->xbar);
		else
			dcp_dpxbar_preselect(route->active_xbar, MUX_IDLE_DISCONNECT);
		if (ret)
			return ret;
		route->xbar_up = false;

		if (route->tunnel && dcp->phy) {
			/* the tunnel pixel clock must not outlive the tunnel */
			typeof(&apple_atc_dp_tunnel_rate) stop =
				symbol_get(apple_atc_dp_tunnel_rate);

			if (stop) {
				stop(dcp->phy, route->tunnel_dpin, 0);
				symbol_put(apple_atc_dp_tunnel_rate);
			}
		}
		route->active_xbar = NULL;
		route->tunnel = false;
		dcp->dptx_tunnel = false;
		dcp->dptx_dfp_port = 0;
		dcp->tb_dpin_set_active = NULL;
		dcp->tb_dpin_ctx = NULL;
		dcp->tb_clock_ok = false;
	}
	if (was_tunnel && route->mux_index) {
		int sel = dcp_dpxbar_tunnel_select_source(active_xbar, -1);

		if (sel && sel != -EOPNOTSUPP)
			dev_warn(dcp->dev, "DP tunnel source reset failed: %d\n", sel);
	}
	route->selected = false;
	if (dcp->active_typec_route == route)
		dcp->active_typec_route = NULL;

	if (connector && connector->dcp == to_platform_device(dcp->dev)) {
		/*
		 * Until the port is activated again it has no pipeline behind
		 * it, and nothing can read modes or EDID from it.  Report it
		 * disconnected for that window: leaving a connected connector
		 * whose ->dcp is NULL lets anything probing it in between --
		 * a compositor starting up while the fabric is still settling
		 * -- see an output it cannot get a mode for, and give up on
		 * it.  The new pipeline marks it connected again once the
		 * display has come back up on it.
		 */
		WRITE_ONCE(connector->connected, false);
		/* hotplug work queued before this checks for it */
		WRITE_ONCE(connector->dcp, NULL);

		/* Unrouted: the port could go to any of its pipelines again. */
		if (connector->port_encoder && !dcp_typec_dual_stream())
			connector->port_encoder->possible_crtcs =
				connector->candidate_crtcs;
	}
	dcp->typec_connector = NULL;
	dcp->connector = dcp->fixed_connector;

	if (dcp->fixed_connector_type != DRM_MODE_CONNECTOR_USB) {
		dcp->connector_type = dcp->fixed_connector_type;

		/*
		 * Only hand the pipeline back to its fixed output if that output
		 * is live.  Re-targeting the DPTX endpoint at the fixed PHY while
		 * nothing is attached there leaves DCP unable to train a link on a
		 * later Type-C target: it answers DEVICE_NOT_STARTED and every
		 * following DPTX call times out.  Park on a Type-C PHY instead,
		 * for the same reason the USB-C-only case does below.
		 */
		if (dcp->hdmi_hpd && gpiod_get_value_cansleep(dcp->hdmi_hpd)) {
			dcp->phy = dcp->fixed_phy;
			dcp->dptx_phy = dcp->fixed_dptx_phy;

			if (dcp->xbar) {
				ret = mux_control_select(dcp->xbar,
							 dcp->fixed_mux_index);
				if (ret)
					return ret;
				dcp->fixed_route_selected = true;
			}
		} else {
			dcp->phy = dcp->typec_routes[0].phy;
			dcp->dptx_phy = dcp->typec_routes[0].dptx_phy;
		}
	} else {
		/* Keep DPTX endpoint discovery working before a cable is attached. */
		dcp->phy = dcp->typec_routes[0].phy;
		dcp->dptx_phy = dcp->typec_routes[0].dptx_phy;
		dcp->connector_type = DRM_MODE_CONNECTOR_USB;
	}

	return 0;
}

static void dcp_typec_retrain_work(struct work_struct *work)
{
	struct apple_dcp *dcp =
		container_of(to_delayed_work(work), struct apple_dcp,
			     typec_fabric_retrain_wq);

	struct apple_connector *connector = READ_ONCE(dcp->typec_connector);

	if (READ_ONCE(dcp->active_typec_route) && connector)
		dcp_retrain_oob(connector);
}

static void dcp_typec_retrain_active_routes(void)
{
	struct apple_dcp_typec_port *port;

	list_for_each_entry(port, &dcp_typec_ports, link) {
		if (port->owner)
			mod_delayed_work(system_freezable_wq,
				 &port->owner->dcp->typec_fabric_retrain_wq,
				 msecs_to_jiffies(200));
		if (port->secondary_owner)
			mod_delayed_work(system_freezable_wq,
				 &port->secondary_owner->dcp->typec_fabric_retrain_wq,
				 msecs_to_jiffies(200));
	}
}

static struct apple_dcp_typec_route *
dcp_typec_port_route(struct apple_dcp_typec_port *port, struct apple_dcp *dcp)
{
	struct apple_dcp_typec_route *route;

	list_for_each_entry(route, &port->routes, port_link)
		if (route->dcp == dcp)
			return route;

	return NULL;
}

/*
 * The DRM device once it has bound, that is once every pipeline has its
 * CRTC and every port its connectors; NULL before.
 */
static struct drm_device *dcp_typec_drm(void)
{
	struct apple_dcp_typec_port *port;
	struct apple_dcp_typec_route *route;
	struct drm_device *drm = NULL;

	list_for_each_entry(port, &dcp_typec_ports, link) {
		if (!port->connector || !port->secondary_connector)
			return NULL;
		list_for_each_entry(route, &port->routes, port_link) {
			if (!route->dcp->crtc)
				return NULL;
			drm = route->dcp->crtc->base.dev;
		}
	}

	return drm;
}

/*
 * Has a compositor (or boot splash) taken the display?  It paired its
 * connectors with CRTCs when it started and keeps that pairing, also
 * across a VT switch, where it drops DRM master but keeps the device
 * open; moving routes under it would hand connectors to pipelines driving
 * other displays.  So the routes are frozen while any open file has ever
 * been master, and thaw when the last such file is closed.
 *
 * Taken under dcp_typec_fabric_lock, next to the routing it decides, so
 * the order is the fabric lock, then filelist_mutex.  Nothing nests them
 * the other way: filelist_mutex only covers list edits and walks, and
 * drm_release() drops it before drm_file_free() calls postclose, which is
 * where a close takes the fabric lock.  drm_open() makes a file master
 * before adding it here, but its owner cannot have paired anything before
 * open() returns, and it re-probes on the hotplugs the moves send.
 */
static bool dcp_typec_frozen(struct drm_device *drm)
{
	struct drm_file *file;

	guard(mutex)(&drm->filelist_mutex);
	list_for_each_entry(file, &drm->filelist, lhead) {
		/* set under master_mutex, and only ever from false to true */
		if (READ_ONCE(file->was_master))
			return true;
	}

	return false;
}

/* Are the routes kept in compositor pairing order right now? */
static bool dcp_typec_keep_order(void)
{
	struct drm_device *drm;

	if (!dcp_typec_dual_stream())
		return false;
	drm = dcp_typec_drm();

	return drm && READ_ONCE(drm->registered) && !dcp_typec_frozen(drm);
}

/* A Thunderbolt tunnel holds @dcp's pipeline: it never moves. */
static bool dcp_typec_tunnel_held(struct apple_dcp *dcp)
{
	return dcp->active_typec_route && dcp->active_typec_route->tunnel;
}

/*
 * Give @dcp's pipeline to the first stream, in connector order, that
 * wants one, has none yet and has it in its possible_crtcs.  A tunnel
 * stream wants one once it is set up, or as @arriving/@dpin.  A direct
 * DP-alt stream wants one once its sink asserts HPD: only then can its
 * connector read connected, and a compositor pairs only those.
 */
static void dcp_typec_plan_pipeline(struct apple_dcp *dcp, struct drm_crtc *crtc,
				    struct apple_dcp_typec_port *arriving,
				    unsigned int dpin)
{
	struct apple_dcp_typec_port *port;
	bool secondary = false;

	/* primary connectors in port order, then the DPIN1 ones */
	do {
		list_for_each_entry(port, &dcp_typec_ports, link) {
			struct apple_dcp_typec_route **slot = secondary ?
				&port->secondary_target : &port->target;
			struct apple_connector *connector = secondary ?
				port->secondary_connector : port->connector;
			struct apple_dcp_typec_route *route =
				dcp_typec_port_route(port, dcp);
			bool wants;

			if (secondary)
				wants = port->secondary_owner ||
					(port == arriving && dpin == 1);
			else
				wants = (port->owner && port->owner->tunnel) ||
					(port->dp_wanted && port->dp_hpd &&
					 !port->plan_dark) ||
					(port == arriving && dpin == 0);

			if (!wants || *slot || !route ||
			    !(connector->candidate_crtcs & drm_crtc_mask(crtc)))
				continue;

			*slot = route;
			return;
		}
		secondary = !secondary;
	} while (secondary);
}

/*
 * The pairing a compositor starting now would make, wherever the streams
 * sit at the moment: pipelines in CRTC index order, each to the first
 * stream that wants it.  A pipeline whose fixed output is live is left to
 * that output, unless a tunnel holds it: the output cannot have it back
 * then, and its connector reads disconnected.
 *
 * Tunnels are planned in connector order like any other stream, not
 * seated on the pipelines they hold: the compositor pairs their connectors
 * by order too, and seating them would plan the direct streams around
 * pairings it never makes.  A direct stream planned onto a pipeline a
 * tunnel holds cannot have it and stays dark, so it is left out and the
 * pass re-run, or every stream after it would be planned one pipeline
 * off.  Streams only ever drop out, so that settles within one pass per
 * port.  It also brings a tunnel's plan onto the pipeline it holds
 * wherever direct streams ahead of it were in the way.  A tunnel still
 * planned elsewhere cannot be brought there by any direct stream dropping
 * out, and the compositor pairs its connector wrongly whatever they do.
 */
static void dcp_typec_plan(struct drm_device *drm,
			   struct apple_dcp_typec_port *arriving, unsigned int dpin)
{
	struct apple_dcp_typec_port *port;
	struct drm_crtc *crtc;
	bool again;

	list_for_each_entry(port, &dcp_typec_ports, link)
		port->plan_dark = false;

	do {
		list_for_each_entry(port, &dcp_typec_ports, link) {
			port->target = NULL;
			port->secondary_target = NULL;
		}

		/* CRTCs are listed in index order */
		drm_for_each_crtc(crtc, drm) {
			struct apple_dcp *dcp =
				platform_get_drvdata(to_apple_crtc(crtc)->dcp);

			if (dcp->nr_typec_routes &&
			    (dcp_typec_tunnel_held(dcp) ||
			     !dcp_typec_route_fixed_output_busy(&dcp->typec_routes[0])))
				dcp_typec_plan_pipeline(dcp, crtc, arriving, dpin);
		}

		again = false;
		list_for_each_entry(port, &dcp_typec_ports, link) {
			if (port == arriving || !port->target ||
			    (port->owner && port->owner->tunnel) ||
			    !dcp_typec_tunnel_held(port->target->dcp))
				continue;
			port->plan_dark = true;
			again = true;
		}
	} while (again);
}

/* Replay HPD to the pipeline a direct DP-alt port has just been given. */
static void dcp_typec_port_attach(struct apple_dcp_typec_port *port)
{
	struct apple_dcp *dcp = port->owner->dcp;

	port->hpd = port->dp_hpd;
	if (!port->hpd)
		return;

	WRITE_ONCE(dcp->typec_cable_connected, true);
	if (dcp->typec_connector)
		dcp_dptx_connect_oob(to_platform_device(dcp->dev), 0);
}

static struct apple_dcp_typec_route *
dcp_typec_lowest_free(struct apple_dcp_typec_port *port)
{
	struct apple_dcp_typec_route *candidate, *best = NULL;
	unsigned int best_score = UINT_MAX;

	list_for_each_entry(candidate, &port->routes, port_link) {
		unsigned int score;

		if (!dcp_typec_route_available(candidate))
			continue;
		/*
		 * Lowest free CRTC index first: on dual-stream machines that
		 * is what a compositor picks from the port's fixed
		 * possible_crtcs.
		 */
		score = dcp_typec_route_score(candidate);
		if (score < best_score) {
			best = candidate;
			best_score = score;
		}
	}

	return best;
}

/*
 * Route the direct DP-alt ports left waiting for a pipeline.  Their DP
 * state is not reported again while it stays the same, so nothing else
 * would.  Each goes back to the pipeline it last had if that is free, as
 * a compositor keeps a reconnected connector's CRTC, and otherwise takes
 * the lowest free one, the CRTC a compositor gives a new connector.
 */
static void dcp_typec_route_waiting(void)
{
	struct apple_dcp_typec_port *port;
	struct apple_dcp_typec_route *route;

	list_for_each_entry(port, &dcp_typec_ports, link) {
		if (port->owner || !port->dp_wanted || !port->dp_hpd)
			continue;

		route = port->preferred_route;
		if (!route || !dcp_typec_route_available(route))
			route = dcp_typec_lowest_free(port);
		if (!route || dcp_typec_route_activate(route, route->xbar))
			continue;
		port->owner = route;
		port->dp_release_deadline = 0;
		dcp_typec_port_attach(port);
	}
}

/*
 * Keep the Type-C routes where a compositor starting now expects them.
 * Dual-stream machines keep possible_crtcs fixed, and compositors read
 * them once and pair connectors with CRTCs themselves: aquamarine
 * (Hyprland) walks the CRTCs in index order and gives each to the first
 * connected connector, in connector order, that can use it.  A connector
 * paired with a pipeline other than the one routed to its display has its
 * modes checked against the other display's list, so its modesets fail.
 * The Type-C and Thunderbolt events that route the ports come in no
 * particular order, so until a compositor owns the display (see
 * dcp_typec_frozen()) every route change re-runs that pairing from
 * scratch (dcp_typec_plan()) and follows it.
 *
 * Only direct DP-alt routes move: each goes to exactly its planned
 * pipeline.  A Thunderbolt tunnel never moves once set up, so where one
 * sits on a pipeline planned for a direct stream, or the plan has nothing
 * for it, that stream stays unrouted and its connector disconnected.
 * Placing it on some other pipeline would have the compositor cross both
 * displays; dark is the better failure.  @arriving/@dpin is a tunnel
 * stream asking for a pipeline, planned like any other; its route is
 * returned for apple_dcp_tb_dp_tunnel() to set up, or NULL if the plan
 * has none for it or another tunnel holds that one.
 *
 * A move is an unplug and replug.  Every route that moves is taken down
 * first, so that two never share a pipeline midway, then each goes up on
 * its new pipeline with its HPD replayed; the hotplugs this sends make
 * fbdev and userspace re-probe.
 */
static struct apple_dcp_typec_route *
dcp_typec_rebalance_locked(struct apple_dcp_typec_port *arriving,
			   unsigned int dpin)
{
	struct drm_device *drm = dcp_typec_drm();
	struct apple_dcp_typec_route *planned = NULL;
	struct apple_dcp_typec_port *port;

	lockdep_assert_held(&dcp_typec_fabric_lock);

	dcp_typec_plan(drm, arriving, dpin);
	if (arriving) {
		planned = dpin ? arriving->secondary_target : arriving->target;
		/* it will be refused: plan as if it had not asked */
		if (planned && dcp_typec_tunnel_held(planned->dcp)) {
			planned = NULL;
			dcp_typec_plan(drm, NULL, 0);
		}
	}

	list_for_each_entry(port, &dcp_typec_ports, link) {
		struct apple_dcp_typec_route *owner = port->owner;
		struct apple_dcp *dcp;

		if (!owner || owner->tunnel || owner == port->target)
			continue;

		dcp = owner->dcp;
		dev_info(dcp->dev, "re-routing %pOF from %s to %s for Type-C connector order\n",
			 port->connector_np, dev_name(dcp->dev),
			 port->target ? dev_name(port->target->dcp->dev) : "none");
		if (port->hpd || dcp->typec_cable_connected ||
		    (dcp->typec_connector && dcp->typec_connector->connected))
			dcp_dptx_disconnect_oob(to_platform_device(dcp->dev), 0);
		port->hpd = false;
		if (dcp_typec_route_deactivate(owner) && owner->selected) {
			/* still routed: leave the display where it is */
			dcp_typec_port_attach(port);
			continue;
		}
		port->owner = NULL;

		/* as after a DP exit, hand the hybrid back to a live HDMI */
		if (dcp->hdmi_hpd && dcp->active &&
		    gpiod_get_value_cansleep(dcp->hdmi_hpd))
			dcp_dptx_connect(dcp, 0);
	}

	list_for_each_entry(port, &dcp_typec_ports, link) {
		struct apple_dcp_typec_route *route = port->target;

		if (port->owner || !port->dp_wanted || !port->dp_hpd ||
		    port == arriving)
			continue;

		if (!route || !dcp_typec_route_available(route) ||
		    dcp_typec_route_activate(route, route->xbar)) {
			/*
			 * Dark for now.  Its unchanged DP state is not reported
			 * again, so it is placed when the plan next runs, or
			 * once a compositor owns the display, when a pipeline
			 * is freed (dcp_typec_route_waiting()).
			 */
			port->applied_valid = false;
			continue;
		}
		port->owner = route;
		port->preferred_route = route;
		port->dp_release_deadline = 0;
		dcp_typec_port_attach(port);
	}

	return planned;
}

/*
 * A route has let its pipeline go.  Until a compositor owns the display
 * the plan places everything anew; after that the pipeline goes to a port
 * left waiting for one.
 */
static void dcp_typec_pipeline_freed(void)
{
	if (!dcp_typec_dual_stream())
		return;

	if (dcp_typec_keep_order())
		dcp_typec_rebalance_locked(NULL, 0);
	else
		dcp_typec_route_waiting();
}

/*
 * Re-run the pairing pass from outside the fabric: once DRM is registered,
 * as ports routed before that could not follow it, and when the last
 * compositor or boot splash has closed the device, as the next one pairs
 * the connectors from scratch.
 */
void dcp_typec_reorder(void)
{
	if (!dcp_typec_dual_stream())
		return;

	guard(mutex)(&dcp_typec_fabric_lock);

	if (dcp_typec_keep_order())
		dcp_typec_rebalance_locked(NULL, 0);
}

static int dcp_typec_route_set(struct typec_mux_dev *mux,
			       struct typec_mux_state *state)
{
	struct apple_dcp_typec_route *route = typec_mux_get_drvdata(mux);
	struct apple_dcp_typec_port *port = route->port;
	struct apple_dcp_typec_route *best = NULL;
	bool is_dp = dcp_typec_route_is_dp(state);
	struct typec_displayport_data *dp_data = is_dp ? state->data : NULL;
	u32 dp_status = dp_data ? dp_data->status : 0;
	u32 dp_conf = dp_data ? dp_data->conf : 0;
	bool hpd, was_counted;
	int ret = 0;

	guard(mutex)(&dcp_typec_fabric_lock);

	/*
	 * Every candidate route for this port is notified with the same state,
	 * so only the first to arrive does the work; the others return early.
	 *
	 * Deliberately not a nominated coordinator: fwnode_typec_mux_get()
	 * caps the providers one connector may have and drops the remainder
	 * without a word, so a nominated route might never be called at all --
	 * and the port would then never be routed.
	 */
	if (port->applied_valid && port->applied_alt == state->alt &&
	    port->applied_mode == state->mode &&
	    port->applied_status == dp_status && port->applied_conf == dp_conf)
		return 0;

	port->applied_alt = state->alt;
	port->applied_mode = state->mode;
	port->applied_status = dp_status;
	port->applied_conf = dp_conf;
	/* Failed route acquisition must remain retryable on the next update. */
	port->applied_valid = false;

	/* did the pairing pass count this port's direct stream so far? */
	was_counted = port->dp_wanted && port->dp_hpd;

	if (!is_dp) {
		port->dp_wanted = false;
		port->dp_hpd = false;

		/* a Thunderbolt/USB4 DP tunnel is torn down by its own path */
		if (port->owner && port->owner->tunnel) {
			port->applied_valid = true;
			return 0;
		}
		if (port->owner) {
			struct apple_dcp *dcp = port->owner->dcp;

			port->preferred_route = port->owner;
			if (port->hpd || dcp->typec_cable_connected ||
			    (dcp->typec_connector &&
			     dcp->typec_connector->connected))
				dcp_dptx_disconnect_oob(to_platform_device(dcp->dev), 0);
			port->hpd = false;
			ret = dcp_typec_route_deactivate(port->owner);
			if (ret)
				return ret;
			port->owner = NULL;
			port->dp_release_deadline = jiffies + msecs_to_jiffies(10000);
			if (dcp->hdmi_hpd && dcp->active &&
			    gpiod_get_value_cansleep(dcp->hdmi_hpd))
				dcp_dptx_connect(dcp, 0);
			dcp_typec_pipeline_freed();
		} else if (was_counted && dcp_typec_keep_order()) {
			/* a stream the plan left dark is gone: plan the rest */
			dcp_typec_rebalance_locked(NULL, 0);
		}

		/*
		 * A port leaving DP can report SAFE/NONE before falling back to USB4.
		 * Resetting every other live CRTC for that same cable removal blanks
		 * unaffected displays. Keep the guard across the Type-C state sequence;
		 * a later, independent USB4 attach still gets recovery.
		 */
		if (state->mode == TYPEC_MODE_USB4) {
			if (!port->dp_release_deadline ||
			    time_after_eq(jiffies, port->dp_release_deadline))
				dcp_typec_retrain_active_routes();
			port->dp_release_deadline = 0;
		}
		port->applied_valid = true;
		return 0;
	}

	/*
	 * A Thunderbolt DP tunnel still owns the port (its teardown is on the
	 * way): don't act on or remember this state; the tunnel teardown drops
	 * what was recorded so the next update is applied.
	 */
	if (port->owner && port->owner->tunnel) {
		port->applied_valid = false;
		return 0;
	}

	hpd = dp_data && (dp_data->status & DP_STATUS_HPD_STATE);
	port->dp_wanted = true;
	port->dp_hpd = hpd;

	/*
	 * Until a compositor owns the display, a direct DP-alt stream has a
	 * pipeline only while its sink asserts HPD, and the pairing pass
	 * places it: HPD coming or going is the stream connecting or
	 * disconnecting as far as a compositor can tell.  Otherwise the port
	 * takes the lowest free pipeline on DP entry, as a compositor does.
	 */
	if (dcp_typec_keep_order()) {
		struct apple_dcp_typec_route *owner = port->owner;

		/* it connects or disconnects, or is not routed as it should be */
		if (hpd != was_counted || !owner != !hpd)
			dcp_typec_rebalance_locked(NULL, 0);
		if (!port->owner) {
			if (hpd)
				return -EBUSY;
			port->applied_valid = true;
			return 0;
		}
		/* just attached, its HPD replayed: nothing left to apply */
		if (port->owner != owner) {
			port->applied_valid = true;
			return 0;
		}
	}

	if (!port->owner) {
		if (port->preferred_route &&
		    dcp_typec_route_available(port->preferred_route))
			best = port->preferred_route;

		if (!best)
			best = dcp_typec_lowest_free(port);

		if (!best)
			return -EBUSY;

		ret = dcp_typec_route_activate(best, best->xbar);
		if (ret)
			return ret;
		port->owner = best;
		port->dp_release_deadline = 0;
	}


	if (!hpd && port->hpd) {
		dcp_dptx_disconnect_oob(to_platform_device(port->owner->dcp->dev), 0);
	} else if (hpd && !port->hpd) {
		struct apple_dcp *dcp = port->owner->dcp;

		WRITE_ONCE(dcp->typec_cable_connected, true);
		if (dcp->typec_connector)
			dcp_dptx_connect_oob(to_platform_device(dcp->dev), 0);
	} else if (hpd && dp_data && (dp_data->status & DP_STATUS_IRQ_HPD)) {
		struct apple_dcp *dcp = port->owner->dcp;

		if (dcp->typec_connector)
			dcp_retrain_oob(dcp->typec_connector);
	}
	port->hpd = hpd;
	port->applied_valid = true;

	return 0;
}

/*
 * Crossbar connection up/down, looked up at runtime so appledrm does not
 * require the crossbar driver to be built.
 */
static int dcp_dpxbar_link(struct mux_control *mux, bool up)
{
	typeof(&apple_dpxbar_link_up) fn;
	int ret;

	fn = up ? symbol_get(apple_dpxbar_link_up) :
		  symbol_get(apple_dpxbar_link_down);
	if (!fn)
		return -ENOENT;
	ret = fn(mux);
	if (up)
		symbol_put(apple_dpxbar_link_up);
	else
		symbol_put(apple_dpxbar_link_down);
	return ret;
}

static int dcp_dpxbar_preselect(struct mux_control *mux, int state)
{
	typeof(&apple_dpxbar_preselect) fn = symbol_get(apple_dpxbar_preselect);
	int ret;

	if (!fn)
		return -ENOENT;
	ret = fn(mux, state);
	symbol_put(apple_dpxbar_preselect);
	return ret;
}

/*
 * Thunderbolt DP IN, before DCP hears about the display: point the DP IN
 * output at this pipeline and wake the ATC's DP clock path. The crossbar
 * connection itself still only comes up in dcp_tunnel_crossbar_up(), but an
 * output left at its idle source (dispext0) only suits the pipeline that is
 * dispext0: any other one activates the link, gets no answer from the sink
 * and gives up with DEVICE_NOT_RESPONDING before it ever sets a link rate.
 */
static void dcp_tunnel_prepare(struct apple_dcp_typec_route *route,
			       struct mux_control *xbar)
{
	typeof(&apple_atc_dp_tunnel_open) open;
	struct apple_dcp *dcp = route->dcp;
	int ret;

	ret = dcp_dpxbar_preselect(xbar, route->mux_index);
	if (ret && ret != -EOPNOTSUPP)
		dev_warn(dcp->dev, "DP tunnel crossbar preselect failed: %d\n", ret);

	open = symbol_get(apple_atc_dp_tunnel_open);
	if (!open)
		return;
	ret = open(dcp->phy);
	symbol_put(apple_atc_dp_tunnel_open);
	if (ret && ret != -EOPNOTSUPP)
		dev_warn(dcp->dev, "DP tunnel PHY open failed: %d\n", ret);
}

/* DP IN adapter handshake through the thunderbolt glue; tb_lock held */
static int dcp_tunnel_dpin_locked(struct apple_dcp *dcp, bool active)
{
	lockdep_assert_held(&dcp->tb_lock);
	if (!dcp->tb_dpin_set_active)
		return -ENODEV;
	return dcp->tb_dpin_set_active(dcp->tb_dpin_ctx, active);
}

/*
 * Thunderbolt DP IN, from DCP's DidChangeLinkConfiguration once a link rate
 * is set: bring the crossbar connection up (FIFO/PCLK/ATC enables) now that
 * the tunnel pixel clock runs, and re-assert the DP IN adapter's
 * DPTX_INACTIVE=0 afterwards. The first time this is the mux selection; after
 * a re-link only the clocks are brought back (the mux selection and the ATC
 * output enable are kept, see dcp_tunnel_crossbar_down()).
 * Runs inside a DCP apcall: only tb_lock, which nobody holds across a DCP call.
 */
int dcp_tunnel_crossbar_up(struct apple_dcp *dcp)
{
	struct apple_dcp_typec_route *route;
	int ret;

	guard(mutex)(&dcp->tb_lock);
	route = dcp->active_typec_route;
	if (!route || !route->tunnel || !route->active_xbar)
		return -ENODEV;
	if (!dcp->tb_clock_ok) {
		dev_warn(dcp->dev, "no DP tunnel pixel clock, crossbar left down\n");
		return -EIO;
	}
	if (!route->xbar_up) {
		/* never block a DCP call on the mux semaphore */
		ret = mux_control_try_select(route->active_xbar, route->mux_index);
		if (!ret)
			route->xbar_up = true;
	} else {
		ret = dcp_dpxbar_link(route->active_xbar, true);
	}
	if (ret) {
		dev_warn(dcp->dev, "DP tunnel crossbar up failed: %d\n", ret);
		return ret;
	}
	return dcp_tunnel_dpin_locked(dcp, true);
}

/*
 * Thunderbolt DP IN, from DCP's WillChangeLinkConfiguration on an established
 * link: DP IN inactive, crossbar clocks down (mux selection and ATC output
 * enable kept). DP IN goes active again in dcp_tunnel_crossbar_up().
 */
int dcp_tunnel_crossbar_down(struct apple_dcp *dcp)
{
	struct apple_dcp_typec_route *route;

	guard(mutex)(&dcp->tb_lock);
	route = dcp->active_typec_route;
	if (!route || !route->tunnel || !route->xbar_up)
		return 0;
	dcp_tunnel_dpin_locked(dcp, false);
	return dcp_dpxbar_link(route->active_xbar, false);
}

/*
 * Thunderbolt DP IN, from DCP's SetLinkRate: start (rate != 0) or stop the
 * tunnel pixel clock. A stopped clock also takes the crossbar connection down.
 */
int dcp_tunnel_set_rate(struct apple_dcp *dcp, struct phy *phy, u32 link_rate)
{
	typeof(&apple_atc_dp_tunnel_rate) fn;
	struct apple_dcp_typec_route *route;
	int ret;

	guard(mutex)(&dcp->tb_lock);
	if (!dcp->dptx_tunnel)
		return -ENODEV;
	fn = symbol_get(apple_atc_dp_tunnel_rate);
	if (!fn) {
		dev_err(dcp->dev, "phy-apple-atc not loaded, no DP tunnel clock\n");
		return -ENOENT;
	}
	route = dcp->active_typec_route;
	if (!route) {
		symbol_put(apple_atc_dp_tunnel_rate);
		return -ENODEV;
	}
	if (!link_rate && route->xbar_up)
		dcp_dpxbar_link(route->active_xbar, false);
	ret = fn(phy, route->tunnel_dpin, link_rate);
	symbol_put(apple_atc_dp_tunnel_rate);
	dcp->tb_clock_ok = !ret && link_rate;
	if (ret)
		dev_warn(dcp->dev, "DP tunnel pixel clock (rate 0x%x) failed: %d\n",
			 link_rate, ret);
	return ret;
}

/* Thunderbolt DP IN: DCP Activate/Deactivate */
int dcp_tunnel_dpin_activate(struct apple_dcp *dcp, bool active)
{
	guard(mutex)(&dcp->tb_lock);
	if (!dcp->dptx_tunnel)
		return 0;
	return dcp_tunnel_dpin_locked(dcp, active);
}

/*
 * Thunderbolt DP tunnels: the host router's DP IN adapters sit behind the
 * crossbar's dpin0/dpin1 outputs of the port's ATC. When the Thunderbolt
 * connection manager has set up a tunnel from one of them, route a free
 * display pipeline there and tell DCP a display is attached, so it trains
 * the link (and completes DPRX) through the tunnel.
 */
int apple_dcp_tb_dp_tunnel(struct device_node *connector_np, unsigned int dpin,
			   bool active, int (*set_active)(void *ctx, bool active),
			   void *ctx)
{
	struct apple_dcp_typec_port *port = NULL, *pos;
	struct apple_dcp_typec_route *candidate, *best = NULL, *planned = NULL;
	struct apple_dcp_typec_route **slot;
	unsigned int best_score = UINT_MAX;
	struct mux_control *ctl;
	struct apple_dcp *dcp;
	bool ordered;
	int ret;

	if (!connector_np || dpin > 1)
		return -EINVAL;

	guard(mutex)(&dcp_typec_fabric_lock);

	list_for_each_entry(pos, &dcp_typec_ports, link) {
		if (pos->connector_np == connector_np) {
			port = pos;
			break;
		}
	}
	if (!port)
		return -ENODEV;
	slot = dpin ? &port->secondary_owner : &port->owner;

	if (!active) {
		if (!*slot || !(*slot)->tunnel ||
		    (*slot)->tunnel_dpin != dpin)
			return 0;
		dcp = (*slot)->dcp;
		if (port->hpd || dcp->typec_cable_connected)
			dcp_dptx_disconnect_oob(to_platform_device(dcp->dev), 0);
		ret = dcp_typec_route_deactivate(*slot);
		if (ret) {
			/* the caller's context is going away regardless */
			scoped_guard(mutex, &dcp->tb_lock) {
				dcp->dptx_tunnel = false;
				dcp->tb_dpin_set_active = NULL;
				dcp->tb_dpin_ctx = NULL;
			}
			return ret;
		}
		*slot = NULL;
		port->hpd = !!(port->owner || port->secondary_owner);
		/* re-apply the next Type-C mux state in full */
		port->applied_valid = false;
		if (dcp->hdmi_hpd && dcp->active &&
		    gpiod_get_value_cansleep(dcp->hdmi_hpd))
			dcp_dptx_connect(dcp, 0);
		dcp_typec_pipeline_freed();
		return 0;
	}

	if (*slot) {
		if ((*slot)->tunnel && (*slot)->tunnel_dpin == dpin)
			return 0;
		dev_warn((*slot)->dcp->dev,
			 "port already routed, not taking DP tunnel dpin%u\n", dpin);
		return -EBUSY;
	}
	if (port->owner && !port->owner->tunnel)
		return -EBUSY;

	/*
	 * Until a compositor owns the display, the pairing pass decides where
	 * the stream goes, moving direct DP-alt routes out of its way, and
	 * whether it gets a pipeline at all.
	 */
	ordered = dcp_typec_keep_order();
	if (ordered)
		planned = dcp_typec_rebalance_locked(port, dpin);

	list_for_each_entry(candidate, &port->routes, port_link) {
		unsigned int score;

		if (ordered && candidate != planned)
			continue;
		if (!dcp_typec_route_available(candidate))
			continue;
		if (!dcp_typec_route_fits(candidate, dpin ?
					  port->secondary_connector :
					  port->connector))
			continue;
		score = dcp_typec_route_score(candidate);
		/*
		 * DPIN0 prefers the hybrid dcpext0, which completes tunneled
		 * link training. On dual-stream machines DPIN1 is confined to
		 * dcpext1 by its connector's possible_crtcs, so both pipelines
		 * drive independent streams through one dock.
		 */
		if (dcp_typec_dual_stream() && dpin == 0 &&
		    !candidate->dcp->fixed_phy && score < UINT_MAX - 100)
			score += 100;
		if (score < best_score) {
			best = candidate;
			best_score = score;
		}
	}
	if (!best) {
		ret = -EBUSY;
		goto err_reorder;
	}

	/* The route's crossbar control is dpphy (0); dpin0/dpin1 are 1/2. */
	if (best->xbar != &best->xbar->chip->mux[0] ||
	    best->xbar->chip->controllers < 3) {
		ret = -EOPNOTSUPP;
		goto err_reorder;
	}
	ctl = &best->xbar->chip->mux[1 + dpin];

	dcp = best->dcp;
	scoped_guard(mutex, &dcp->tb_lock) {
		dcp->tb_dpin_set_active = set_active;
		dcp->tb_dpin_ctx = ctx;
	}
	best->tunnel_dpin = dpin;
	ret = dcp_typec_route_activate(best, ctl);
	if (ret) {
		scoped_guard(mutex, &dcp->tb_lock) {
			dcp->tb_dpin_set_active = NULL;
			dcp->tb_dpin_ctx = NULL;
		}
		goto err_reorder;
	}
	*slot = best;
	/* the port is in USB4 mode, not DP-alt */
	port->dp_wanted = false;
	dcp_tunnel_prepare(best, ctl);

	dev_info(dcp->dev, "display routed to Thunderbolt DP tunnel dpin%u\n", dpin);

	/*
	 * The DP IN adapter may only be woken (DPTX_INACTIVE=0) while DCP
	 * drives the DPTX, i.e. from DCP's Activate call; waking it earlier
	 * hangs the machine. dptxep calls set_active back from Activate and
	 * Deactivate (set above, before the route became a tunnel).
	 */
	if (!dcp->typec_connector)
		dev_warn(dcp->dev, "no Type-C connector for the DP tunnel\n");
	WRITE_ONCE(dcp->typec_cable_connected, true);
	port->hpd = true;
	if (dcp->typec_connector)
		dcp_dptx_connect_oob(to_platform_device(dcp->dev), 0);

	return 0;

err_reorder:
	/* the pass kept a pipeline for this stream: give it to the others */
	if (planned)
		dcp_typec_rebalance_locked(NULL, 0);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_dcp_tb_dp_tunnel);

static struct apple_dcp_typec_port *
dcp_typec_port_get(struct device_node *connector_np)
{
	struct apple_dcp_typec_port *port, *pos;

	lockdep_assert_held(&dcp_typec_fabric_lock);

	list_for_each_entry(port, &dcp_typec_ports, link) {
		if (port->connector_np == connector_np) {
			of_node_put(connector_np);
			return port;
		}
	}

	port = kzalloc_obj(*port);
	if (!port) {
		of_node_put(connector_np);
		return NULL;
	}

	INIT_LIST_HEAD(&port->routes);
	port->connector_np = connector_np;

	/*
	 * Insert in device-tree order rather than DCP probe order.  The list
	 * index becomes the DRM connector index, and userspace keys its
	 * per-monitor configuration (scale, rotation, layout) on the connector
	 * name -- so it has to mean the same physical port on every boot.
	 */
	list_for_each_entry(pos, &dcp_typec_ports, link)
		if (strcmp(of_node_full_name(connector_np),
			   of_node_full_name(pos->connector_np)) < 0)
			break;
	list_add_tail(&port->link, &pos->link);
	return port;
}

static struct apple_dcp_typec_port *dcp_typec_port_by_index(unsigned int idx)
{
	struct apple_dcp_typec_port *port;
	unsigned int i = 0;

	lockdep_assert_held(&dcp_typec_fabric_lock);

	list_for_each_entry(port, &dcp_typec_ports, link)
		if (i++ == idx)
			return port;

	return NULL;
}

unsigned int dcp_typec_nr_ports(void)
{
	struct apple_dcp_typec_port *port;
	unsigned int n = 0;

	guard(mutex)(&dcp_typec_fabric_lock);

	list_for_each_entry(port, &dcp_typec_ports, link)
		n++;

	return n;
}

struct device_node *dcp_typec_port_of_node(unsigned int idx)
{
	struct apple_dcp_typec_port *port;

	guard(mutex)(&dcp_typec_fabric_lock);

	port = dcp_typec_port_by_index(idx);

	return port ? port->connector_np : NULL;
}

bool dcp_typec_port_has_candidate(unsigned int idx, struct platform_device *pdev)
{
	struct apple_dcp_typec_port *port;
	struct apple_dcp_typec_route *route;

	guard(mutex)(&dcp_typec_fabric_lock);

	port = dcp_typec_port_by_index(idx);
	if (!port)
		return false;

	list_for_each_entry(route, &port->routes, port_link)
		if (route->dcp->dev == &pdev->dev)
			return true;

	return false;
}

void dcp_typec_port_set_connector(unsigned int idx, bool secondary,
				  struct apple_connector *connector)
{
	struct apple_dcp_typec_port *port;
	struct apple_dcp_typec_route *owner;

	guard(mutex)(&dcp_typec_fabric_lock);

	port = dcp_typec_port_by_index(idx);
	if (!port)
		return;

	if (secondary) {
		port->secondary_connector = connector;
		owner = port->secondary_owner;
	} else {
		port->connector = connector;
		owner = port->owner;
	}

	/*
	 * The port may already have been routed, either before DRM bound or
	 * while these connectors were being created.  Adopt that owner now,
	 * otherwise its display would be reported on the pipeline's fixed
	 * connector instead of the port it is actually plugged into.
	 */
	if (owner) {
		struct apple_dcp *dcp = owner->dcp;

		if (dcp->crtc && connector->port_encoder &&
		    !dcp_typec_dual_stream())
			connector->port_encoder->possible_crtcs =
				drm_crtc_mask(&dcp->crtc->base);

		connector->dcp = to_platform_device(dcp->dev);
		dcp->typec_connector = connector;
		dcp->connector = connector;
		dcp->connector_type = DRM_MODE_CONNECTOR_USB;
	}
}

static void dcp_typec_route_unregister(void *data)
{
	struct apple_dcp_typec_route *route = data;
	struct apple_dcp_typec_port *port = route->port;

	typec_mux_unregister(route->typec_mux);

	guard(mutex)(&dcp_typec_fabric_lock);
	if (port->preferred_route == route)
		port->preferred_route = NULL;
	if (port->owner == route) {
		struct apple_dcp *dcp = route->dcp;

		if (port->hpd || dcp->typec_cable_connected)
			dcp_dptx_disconnect_oob(to_platform_device(dcp->dev), 0);
		port->hpd = false;
		dcp_typec_route_deactivate(route);
		port->owner = NULL;
	}
	if (port->secondary_owner == route) {
		struct apple_dcp *dcp = route->dcp;

		if (port->hpd || dcp->typec_cable_connected)
			dcp_dptx_disconnect_oob(to_platform_device(dcp->dev), 0);
		dcp_typec_route_deactivate(route);
		port->secondary_owner = NULL;
	}
	port->hpd = !!(port->owner || port->secondary_owner);
	list_del(&route->port_link);
	if (list_empty(&port->routes)) {
		list_del(&port->link);
		of_node_put(port->connector_np);
		kfree(port);
	}
}

static int dcp_register_typec_routes(struct apple_dcp *dcp)
{
	struct device_node *routes __free(device_node) =
		of_get_child_by_name(dcp->dev->of_node, "typec-routes");
	struct device *dev = dcp->dev;
	u32 route_index;
	int ret;

	if (!routes)
		return 0;

	for_each_available_child_of_node_scoped(routes, route_np) {
		struct apple_dcp_typec_route *route;
		struct device_node *endpoint __free(device_node) = NULL;
		struct device_node *connector_np;
		struct typec_mux_desc desc = {};
		const char *name, *mux_name;

		if (dcp->nr_typec_routes == DCP_MAX_TYPEC_ROUTES)
			return dev_err_probe(dev, -E2BIG, "Too many Type-C display routes\n");

		ret = of_property_read_u32(route_np, "reg", &route_index);
		if (ret)
			return dev_err_probe(dev, ret, "%pOF: missing route index\n", route_np);
		if (route_index >= DCP_MAX_TYPEC_ROUTES)
			return dev_err_probe(dev, -EINVAL, "%pOF: invalid route index %u\n",
					     route_np, route_index);

		name = devm_kasprintf(dev, GFP_KERNEL, "typec%u", route_index);
		if (!name)
			return -ENOMEM;

		/*
		 * The DT lookups above are per-DCP, but the typec_mux class is
		 * global. Several DCPs can offer a route to the same Type-C port,
		 * so the registered mux needs a name unique across all of them.
		 */
		mux_name = devm_kasprintf(dev, GFP_KERNEL, "%s-typec%u",
					  dev_name(dev), route_index);
		if (!mux_name)
			return -ENOMEM;

		route = &dcp->typec_routes[dcp->nr_typec_routes];
		route->dcp = dcp;
		INIT_LIST_HEAD(&route->port_link);
		route->phy = devm_phy_get(dev, name);
		if (IS_ERR(route->phy))
			return dev_err_probe(dev, PTR_ERR(route->phy),
					     "%pOF: failed to get DP PHY\n", route_np);

		route->xbar = devm_mux_control_get(dev, name);
		if (IS_ERR(route->xbar))
			return dev_err_probe(dev, PTR_ERR(route->xbar),
					     "%pOF: failed to get display crossbar\n", route_np);

		ret = of_property_read_u32_index(dev->of_node, "apple,typec-mux-indices",
						 route_index, &route->mux_index);
		if (ret)
			return dev_err_probe(dev, ret, "%pOF: missing crossbar state\n",
					     route_np);

		ret = of_property_read_u32_index(dev->of_node, "apple,typec-dptx-phys",
						 route_index, &route->dptx_phy);
		if (ret)
			return dev_err_probe(dev, ret, "%pOF: missing DPTX PHY index\n",
					     route_np);

		endpoint = of_graph_get_next_endpoint(route_np, NULL);
		if (!endpoint)
			return dev_err_probe(dev, -EINVAL,
					     "%pOF: missing Type-C graph endpoint\n",
					     route_np);
		connector_np = of_graph_get_remote_port_parent(endpoint);
		if (!connector_np)
			return dev_err_probe(dev, -EINVAL,
					     "%pOF: missing Type-C connector\n",
					     route_np);

		mutex_lock(&dcp_typec_fabric_lock);
		route->port = dcp_typec_port_get(connector_np);
		if (route->port)
			list_add_tail(&route->port_link, &route->port->routes);
		mutex_unlock(&dcp_typec_fabric_lock);
		if (!route->port)
			return -ENOMEM;

		desc.fwnode = of_fwnode_handle(route_np);
		desc.set = dcp_typec_route_set;
		desc.name = mux_name;
		desc.drvdata = route;
		route->typec_mux = typec_mux_register(dev, &desc);
		if (IS_ERR(route->typec_mux)) {
			mutex_lock(&dcp_typec_fabric_lock);
			list_del(&route->port_link);
			if (list_empty(&route->port->routes)) {
				list_del(&route->port->link);
				of_node_put(route->port->connector_np);
				kfree(route->port);
			}
			mutex_unlock(&dcp_typec_fabric_lock);
			return dev_err_probe(dev, PTR_ERR(route->typec_mux),
					     "%pOF: failed to register Type-C route\n", route_np);
		}

		ret = devm_add_action_or_reset(dev, dcp_typec_route_unregister, route);
		if (ret)
			return ret;

		if (!dcp->phy)
			dcp->phy = route->phy;
		dcp->nr_typec_routes++;
	}

	if (!dcp->nr_typec_routes)
		return dev_err_probe(dev, -EINVAL, "Type-C route container is empty\n");

	dcp->phy_managed_by_typec = true;
	return 0;
}


/* copied and simplified from drm_vblank.c */
static void send_vblank_event(struct drm_device *dev,
		struct drm_pending_vblank_event *e,
		u64 seq, ktime_t now)
{
	struct timespec64 tv;

	if (e->event.base.type != DRM_EVENT_FLIP_COMPLETE)
		return;

	tv = ktime_to_timespec64(now);
	e->event.vbl.sequence = seq;
	/*
		* e->event is a user space structure, with hardcoded unsigned
		* 32-bit seconds/microseconds. This is safe as we always use
		* monotonic timestamps since linux-4.15
		*/
	e->event.vbl.tv_sec = tv.tv_sec;
	e->event.vbl.tv_usec = tv.tv_nsec / 1000;

	/*
	 * Use the same timestamp for any associated fence signal to avoid
	 * mismatch in timestamps for vsync & fence events triggered by the
	 * same HW event. Frameworks like SurfaceFlinger in Android expects the
	 * retire-fence timestamp to match exactly with HW vsync as it uses it
	 * for its software vsync modeling.
	 */
	drm_send_event_timestamp_locked(dev, &e->base, now);
}

/**
 * dcp_crtc_send_page_flip_event - helper to send vblank event after pageflip
 *
 * Compensate for unknown slack between page flip and arrival of the
 * swap_complete callback. Minimal observed duration on DCP with HDMI output
 * was around 2.3 ms. If the fb swap was submitted closer to the expected
 * swap_complete it gets a penalty of one frame duration. This is on the border
 * of unreasonable considering that Apple advertises support for 240 Hz (frame
 * duration of 4.167 ms).
 * It is unreasonable considering kwin's kms commit scheduling. Kwin commits
 * 1.5 ms + the mode's vblank time before the expected next page flip
 * completion. This results in presenting at half the display's rate for HDMI
 * outputs.
 * This might be a difference between dcp and dcpext.
 */
static void dcp_crtc_send_page_flip_event(struct apple_crtc *crtc,
					  struct drm_pending_vblank_event *e,
					  ktime_t now, ktime_t start)
{
	struct drm_device *dev = crtc->base.dev;
	u64 seq;
	unsigned int pipe = drm_crtc_index(&crtc->base);
	ktime_t flip;

	seq = 0;
	if (start != KTIME_MIN) {
		s64 delta = ktime_us_delta(now, start);
		if (delta <= 500)
			flip = now;
		else if (delta >= 2500)
			flip = ktime_sub_us(now, 1000);
		else
			flip = ktime_sub_us(now, (delta - 500) / 2);
	} else {
		flip = now;
	}
	e->pipe = pipe;
	send_vblank_event(dev, e, seq, flip);
}

/* HACK: moved here to avoid circular dependency between apple_drv and dcp */
void dcp_drm_crtc_vblank(struct apple_crtc *crtc)
{
	unsigned long flags;

	spin_lock_irqsave(&crtc->base.dev->event_lock, flags);
	if (crtc->event) {
		drm_crtc_send_vblank_event(&crtc->base, crtc->event);
		crtc->event = NULL;
	}
	spin_unlock_irqrestore(&crtc->base.dev->event_lock, flags);
}

void dcp_drm_crtc_page_flip(struct apple_dcp *dcp, ktime_t now)
{
	unsigned long flags;
	struct apple_crtc *crtc = dcp->crtc;

	spin_lock_irqsave(&crtc->base.dev->event_lock, flags);
	if (crtc->event) {
		if (crtc->event->event.base.type == DRM_EVENT_FLIP_COMPLETE)
			dcp_crtc_send_page_flip_event(crtc, crtc->event, now, dcp->swap_start);
		else
			drm_crtc_send_vblank_event(&crtc->base, crtc->event);
		crtc->event = NULL;
		dcp->swap_start = KTIME_MIN;
	}
	spin_unlock_irqrestore(&crtc->base.dev->event_lock, flags);
}

void dcp_set_dimensions(struct apple_dcp *dcp)
{
	int i;
	int width_mm = dcp->width_mm;
	int height_mm = dcp->height_mm;

	if (width_mm == 0 || height_mm == 0) {
		width_mm = dcp->panel.width_mm;
		height_mm = dcp->panel.height_mm;
	}

	/* Set the connector info */
	if (dcp->connector) {
		struct drm_connector *connector = &dcp->connector->base;

		mutex_lock(&connector->dev->mode_config.mutex);
		connector->display_info.width_mm = width_mm;
		connector->display_info.height_mm = height_mm;
		mutex_unlock(&connector->dev->mode_config.mutex);
	}

	/*
	 * Fix up any probed modes. Modes are created when parsing
	 * TimingElements, dimensions are calculated when parsing
	 * DisplayAttributes, and TimingElements may be sent first
	 */
	for (i = 0; i < dcp->nr_modes; ++i) {
		dcp->modes[i].mode.width_mm = width_mm;
		dcp->modes[i].mode.height_mm = height_mm;
	}
}

bool dcp_has_panel(struct apple_dcp *dcp)
{
	return dcp->panel.width_mm > 0;
}

int dcp_set_crc(struct drm_crtc *crtc, bool enabled)
{
	struct apple_crtc *ac = to_apple_crtc(crtc);
	struct apple_dcp *dcp = platform_get_drvdata(ac->dcp);

	dcp->crc_enabled = enabled;

	return 0;
}

/*
 * Helper to send a DRM vblank event. We do not know how call swap_submit_dcp
 * without surfaces. To avoid timeouts in drm_atomic_helper_wait_for_vblanks
 * send a vblank event via a workqueue.
 */
static void dcp_delayed_vblank(struct work_struct *work)
{
	struct apple_dcp *dcp;

	dcp = container_of(work, struct apple_dcp, vblank_wq);
	mdelay(5);
	dcp_drm_crtc_vblank(dcp->crtc);
}

#define DCP_SWAP_WATCHDOG_MS		1000
#define DCP_SWAP_WATCHDOG_RETRAINS	5

/*
 * DCP drops swaps without completing them while an external pipe is not
 * enabled, for instance after a modeset that raced a Type-C sink which had
 * not asserted HPD yet. Userspace would then wait for the flip until the
 * commit times out, stalling every output it drives. Complete the flip,
 * mark the mode invalid so later commits do not wait for DCP, and let the
 * hotplug worker re-apply the active CRTC.
 */
static void dcp_swap_watchdog(struct work_struct *work)
{
	struct apple_dcp *dcp =
		container_of(to_delayed_work(work), struct apple_dcp,
			     swap_watchdog_wq);

	dev_warn(dcp->dev, "swap not completed, retraining the display\n");
	dcp_mode_invalidate(&dcp->mode_state);
	dcp_drm_crtc_vblank(dcp->crtc);
	if (dcp->connector &&
	    dcp->swap_watchdog_retrains++ < DCP_SWAP_WATCHDOG_RETRAINS)
		schedule_work(&dcp->connector->hotplug_wq);
}

void dcp_swap_watchdog_arm(struct apple_dcp *dcp)
{
	if (dcp_is_typec_output(dcp))
		mod_delayed_work(system_wq, &dcp->swap_watchdog_wq,
				 msecs_to_jiffies(DCP_SWAP_WATCHDOG_MS));
}

void dcp_swap_watchdog_complete(struct apple_dcp *dcp)
{
	cancel_delayed_work(&dcp->swap_watchdog_wq);
	dcp->swap_watchdog_retrains = 0;
}

static void dcp_recv_msg(void *cookie, u8 endpoint, u64 message)
{
	struct apple_dcp *dcp = cookie;

	trace_dcp_recv_msg(dcp, endpoint, message);

	switch (endpoint) {
	case IOMFB_ENDPOINT:
		return iomfb_recv_msg(dcp, message);
	case AV_ENDPOINT:
		afk_receive_message(dcp->avep, message);
		return;
	case SYSTEM_ENDPOINT:
		afk_receive_message(dcp->systemep, message);
		return;
	case DISP0_ENDPOINT:
		afk_receive_message(dcp->ibootep, message);
		return;
	case DPAVSERV_ENDPOINT:
		afk_receive_message(dcp->dcpavservep, message);
		return;
	case DPTX_ENDPOINT:
		afk_receive_message(dcp->dptxep, message);
		return;
	default:
		WARN(endpoint, "unknown DCP endpoint %hhu\n", endpoint);
	}
}

static void dcp_rtk_crashed(void *cookie, const void *crashlog, size_t crashlog_size)
{
	struct apple_dcp *dcp = cookie;

	dcp->crashed = true;
	dev_err(dcp->dev, "DCP has crashed\n");
	if (dcp->connector) {
		dcp->connector->connected = 0;
		drm_edid_free(dcp->connector->drm_edid);
		dcp->connector->drm_edid = NULL;
		schedule_work(&dcp->connector->hotplug_wq);
	}
	complete(&dcp->start_done);
}

static int dcp_rtk_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;

	if (bfr->iova) {
		struct iommu_domain *domain =
			iommu_get_domain_for_dev(dcp->dev);
		phys_addr_t phy_addr;

		if (!domain)
			return -ENOMEM;

		// TODO: get map from device-tree
		phy_addr = iommu_iova_to_phys(domain, bfr->iova);
		if (!phy_addr)
			return -ENOMEM;

		// TODO: verify phy_addr, cache attribute
		bfr->buffer = memremap(phy_addr, bfr->size, MEMREMAP_WB);
		if (!bfr->buffer)
			return -ENOMEM;

		bfr->is_mapped = true;
		dev_info(dcp->dev,
			 "shmem_setup: iova: %lx -> pa: %lx -> iomem: %lx\n",
			 (uintptr_t)bfr->iova, (uintptr_t)phy_addr,
			 (uintptr_t)bfr->buffer);
	} else {
		bfr->buffer = dma_alloc_coherent(dcp->dev, bfr->size,
						 &bfr->iova, GFP_KERNEL);
		if (!bfr->buffer)
			return -ENOMEM;

		dev_info(dcp->dev, "shmem_setup: iova: %lx, buffer: %lx\n",
			 (uintptr_t)bfr->iova, (uintptr_t)bfr->buffer);
	}

	return 0;
}

static void dcp_rtk_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;

	if (bfr->is_mapped)
		memunmap(bfr->buffer);
	else
		dma_free_coherent(dcp->dev, bfr->size, bfr->buffer, bfr->iova);
}

static struct apple_rtkit_ops rtkit_ops = {
	.crashed = dcp_rtk_crashed,
	.recv_message = dcp_recv_msg,
	.shmem_setup = dcp_rtk_shmem_setup,
	.shmem_destroy = dcp_rtk_shmem_destroy,
};

void dcp_send_message(struct apple_dcp *dcp, u8 endpoint, u64 message)
{
	trace_dcp_send_msg(dcp, endpoint, message);
	apple_rtkit_send_message(dcp->rtk, endpoint, message, NULL,
				 true);
}

int dcp_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct platform_device *pdev = to_apple_crtc(crtc)->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	struct drm_crtc_state *crtc_state;
	bool needs_modeset;

	if (dcp->crashed)
		return -EINVAL;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	needs_modeset = drm_atomic_crtc_needs_modeset(crtc_state) ||
			!READ_ONCE(dcp->mode_state.valid);
	if (!needs_modeset && (!dcp->connector || !dcp->connector->connected)) {
		/*
		 * Resume restores the mode before the firmware reports the
		 * display back, so a plane-only commit lands here while the
		 * connector is still marked disconnected.  Rejecting it makes
		 * the compositor fail every flip and give up on the output;
		 * dcp_flush() defers the commit until the link returns.
		 */
		dev_dbg(dcp->dev,
			"crtc_atomic_check: deferring commit, link still down\n");
	}

	return 0;
}

int dcp_get_connector_type(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->fixed_connector_type;
}

bool dcp_has_typec_routes(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->nr_typec_routes;
}

#define DPTX_CONNECT_TIMEOUT msecs_to_jiffies(2000)
#define DPTX_TUNNEL_CONNECT_TIMEOUT msecs_to_jiffies(8000)
#define DPTX_RECONNECT_DELAY msecs_to_jiffies(1000)
#define DPTX_RECONNECT_RETRIES 5

static int dcp_dptx_connect(struct apple_dcp *dcp, u32 port)
{
	unsigned long timeout;
	int ret = 0;

	if (!dcp->phy) {
		dev_warn(dcp->dev, "dcp_dptx_connect: missing phy\n");
		return -ENODEV;
	}
	dev_info(dcp->dev,
		 "%s(port=%d) target=%u:%u typec=%d route=%s conn_type=%d connected=%d\n",
		 __func__, port, dcp->dptx_die, dcp->dptx_phy,
		 dcp_is_typec_output(dcp),
		 dcp->active_typec_route ? "borrowed" : "fixed",
		 dcp->connector_type, dcp->dptxport[port].connected);

	mutex_lock(&dcp->hpd_mutex);
	if (!dcp->dptxport[port].enabled) {
		dev_warn(dcp->dev, "dcp_dptx_connect: dptx service for port %d not enabled\n", port);
		ret = -ENODEV;
		goto out_unlock;
	}

	if (dcp->dptxport[port].connected)
		goto out_unlock;

	reinit_completion(&dcp->dptxport[port].linkcfg_completion);
	dcp->dptxport[port].atcphy = dcp->phy;
	ret = dptxport_validate_connection(dcp->dptxport[port].service,
					   dcp->dptx_dfp_port,
					   dcp->dptx_phy, dcp->dptx_die);
	if (ret) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: failed to validate DPTX target %u:%u: %d\n",
			dcp->dptx_die, dcp->dptx_phy, ret);
		goto out_unlock;
	}

	ret = dptxport_connect(dcp->dptxport[port].service,
			       dcp->dptx_dfp_port,
			       dcp->dptx_phy, dcp->dptx_die,
		       dcp_is_typec_output(dcp));
	if (ret) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: failed to connect DPTX target %u:%u: %d\n",
			dcp->dptx_die, dcp->dptx_phy, ret);
		goto out_unlock;
	}

	ret = dptxport_request_display(dcp->dptxport[port].service);
	if (ret) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: failed to request display: %d\n",
			ret);
		goto out_release;
	}
	dcp->dptxport[port].connected = true;
	if (dcp_is_typec_output(dcp)) {
		if (dcp_is_usb4_output(dcp) && apple_dp_tunnel_t602x())
			ret = dptxport_set_hpd_timeout(dcp->dptxport[port].service,
						       true, 8000);
		else
			ret = dptxport_set_hpd(dcp->dptxport[port].service, true);
		if (ret) {
			dev_err(dcp->dev,
				"dcp_dptx_connect: failed to assert Type-C HPD: %d\n",
				ret);
			dcp->dptxport[port].connected = false;
			goto out_release;
		}
	}

	mutex_unlock(&dcp->hpd_mutex);
	timeout = dcp_is_usb4_output(dcp) && apple_dp_tunnel_t602x() ?
		  DPTX_TUNNEL_CONNECT_TIMEOUT : DPTX_CONNECT_TIMEOUT;
	ret = wait_for_completion_timeout(&dcp->dptxport[port].linkcfg_completion,
					  timeout);
	if (!ret) {
		dev_err(dcp->dev,
			"dcp_dptx_connect: timed out waiting for port %u link configuration\n",
			port);
		ret = -ETIMEDOUT;
		goto out_disconnect;
	}

	dev_dbg(dcp->dev, "dcp_dptx_connect: waited %d ms for link\n",
		jiffies_to_msecs(timeout - ret));

	usleep_range(5, 10);

	if (dcp->connector_type == DRM_MODE_CONNECTOR_DisplayPort)
		dptxport_set_hpd(dcp->dptxport[port].service, true);

	if (dcp->avep)
		av_service_connect(dcp);

	return 0;

out_disconnect:
	mutex_lock(&dcp->hpd_mutex);
	dcp->dptxport[port].connected = false;
out_release:
	dptxport_release_display(dcp->dptxport[port].service);

out_unlock:
	mutex_unlock(&dcp->hpd_mutex);
	return ret;
}

static bool dcp_edid_is_placeholder(const struct drm_edid *drm_edid)
{
	const u8 *raw = (const u8 *)drm_edid_raw(drm_edid);
	static const u8 name[] = "Non-PnP";
	unsigned int i;

	if (!raw)
		return false;

	/* EDID contains interior NUL bytes, so this cannot use strnstr(). */
	for (i = 0; i + sizeof(name) - 1 <= sizeof(struct edid); i++) {
		if (!memcmp(raw + i, name, sizeof(name) - 1))
			return true;
	}

	return false;
}

void dcp_retry_placeholder_edid(struct apple_dcp *dcp,
				const struct drm_edid *drm_edid)
{
	guard(mutex)(&dcp->hpd_mutex);
	if (!dcp_is_typec_output(dcp) || !dcp->typec_cable_connected ||
	    dcp->placeholder_retried)
		return;
	if (!dcp_edid_is_placeholder(drm_edid))
		return;

	dcp->placeholder_retried = true;
	dcp->placeholder_generation = dcp->typec_generation;
	schedule_delayed_work(&dcp->placeholder_edid_wq, msecs_to_jiffies(300));
}

static void dcp_placeholder_edid_work(struct work_struct *work)
{
	struct apple_dcp *dcp =
		container_of(to_delayed_work(work), struct apple_dcp,
			     placeholder_edid_wq);
	struct apple_epic_service *service;
	u64 generation;
	int ret;

	mutex_lock(&dcp->hpd_mutex);
	generation = dcp->placeholder_generation;
	if (!dcp->typec_cable_connected || !dcp->dptxport[0].connected ||
	    !dcp->dptxport[0].enabled || generation != dcp->typec_generation)
		goto out_unlock;
	service = dcp->dptxport[0].service;

	/*
	 * Some adapters answer the first connection with a 1024x768
	 * placeholder and publish the panel EDID only after HPD drops
	 * and returns. One pulse; a second placeholder is left alone.
	 */
	ret = dptxport_set_hpd(service, false);
	if (ret) {
		dev_info(dcp->dev, "placeholder EDID: HPD drop failed: %d\n",
			 ret);
		goto out_unlock;
	}
	mutex_unlock(&dcp->hpd_mutex);

	msleep(1000);

	mutex_lock(&dcp->hpd_mutex);
	if (!dcp->typec_cable_connected || !dcp->dptxport[0].connected ||
	    generation != dcp->typec_generation)
		goto out_unlock;

	ret = dptxport_set_hpd(service, true);
	if (ret)
		dev_info(dcp->dev, "placeholder EDID: HPD assert failed: %d\n",
			 ret);
out_unlock:
	mutex_unlock(&dcp->hpd_mutex);
}

static void dcp_typec_reconnect_work(struct work_struct *work)
{
	struct apple_dcp *dcp =
		container_of(to_delayed_work(work), struct apple_dcp,
			     typec_reconnect_wq);
	int ret;

	if (!READ_ONCE(dcp->typec_cable_connected))
		return;

	ret = dcp_dptx_connect(dcp, 0);
	if (!ret) {
		dcp->typec_reconnect_tries = 0;
		return;
	}

	if (++dcp->typec_reconnect_tries <
	    (dcp_is_usb4_output(dcp) && apple_dp_tunnel_t602x() ?
	     1 : DPTX_RECONNECT_RETRIES)) {
		mod_delayed_work(system_freezable_wq, &dcp->typec_reconnect_wq,
				 DPTX_RECONNECT_DELAY);
		return;
	}

	dev_err(dcp->dev, "Type-C DPTX reconnect failed after %u retries: %d\n",
		dcp->typec_reconnect_tries, ret);
}

static void disconnected_hpd_event(struct apple_connector *con)
{
	if (con && con->connected) {
		con->connected = 0;
		drm_edid_free(con->drm_edid);
		con->drm_edid = NULL;
		drm_kms_helper_connector_hotplug_event(&con->base);
	}
}

static int dcp_dptx_disconnect(struct apple_dcp *dcp, u32 port)
{
	dev_info(dcp->dev, "%s(port=%d)\n", __func__, port);

	mutex_lock(&dcp->hpd_mutex);
	if (dcp->dptxport[port].enabled && dcp->dptxport[port].connected) {
		dptxport_release_display(dcp->dptxport[port].service);
		dcp->dptxport[port].connected = false;
	}
	mutex_unlock(&dcp->hpd_mutex);

	return 0;
}

int dcp_dptx_connect_oob(struct platform_device *pdev, u32 port)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	if (dcp_is_typec_output(dcp)) {
		cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
		guard(mutex)(&dcp->hpd_mutex);
		dcp->typec_generation++;
		WRITE_ONCE(dcp->typec_cable_connected, true);
		dcp->typec_reconnect_tries = 0;
		dcp->placeholder_retried = false;
		cancel_delayed_work(&dcp->typec_reconnect_wq);
	}

	ret = dcp_dptx_connect(dcp, port);
	if (ret && dcp_is_typec_output(dcp))
		mod_delayed_work(system_freezable_wq, &dcp->typec_reconnect_wq,
				 DPTX_RECONNECT_DELAY);

	return ret;
}

int dcp_dptx_disconnect_oob(struct platform_device *pdev, u32 port)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (dcp_is_typec_output(dcp)) {
		scoped_guard(mutex, &dcp->hpd_mutex) {
			WRITE_ONCE(dcp->typec_cable_connected, false);
			dcp->typec_generation++;
		}
		WRITE_ONCE(dcp->typec_crtc_off, false);
		reinit_completion(&dcp->typec_iomfb_hpd_ready);
		cancel_delayed_work_sync(&dcp->typec_reconnect_wq);
		cancel_delayed_work_sync(&dcp->placeholder_edid_wq);
	}

	disconnected_hpd_event(dcp->connector);

	if (dcp->avep)
		av_service_disconnect(dcp);

	if (dcp->dptxport[port].enabled)
		dptxport_set_hpd(dcp->dptxport[port].service, false);

	return dcp_dptx_disconnect(dcp, port);
}

static irqreturn_t dcp_dp2hdmi_hpd(int irq, void *data)
{
	struct apple_dcp *dcp = data;
	bool connected;

	guard(mutex)(&dcp_typec_fabric_lock);

	if (READ_ONCE(dcp->active_typec_route)) {
		/*
		 * Until a compositor owns the display, a live HDMI output
		 * takes its pipeline back from a direct DP-alt route: the
		 * compositor pairs the HDMI connector with it first.
		 */
		if (dcp_typec_keep_order() &&
		    gpiod_get_value_cansleep(dcp->hdmi_hpd)) {
			msleep(500);
			if (gpiod_get_value_cansleep(dcp->hdmi_hpd))
				dcp_typec_rebalance_locked(NULL, 0);
		}
		return IRQ_HANDLED;
	}
	connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);

	/* do nothing on disconnect and trust that dcp detects it itself.
	 * Parallel disconnect HPDs result drm disabling the CRTC even when it
	 * should not.
	 * The interrupt should be changed to rising but for now the disconnect
	 * IRQs might be helpful for debugging.
	 */
	dev_info(dcp->dev, "DP2HDMI HPD irq, connected:%d\n", connected);

	if (connected) {
		msleep(500);
		connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		dev_info(dcp->dev, "DP2HDMI HPD irq, 500ms debounce: connected:%d\n", connected);
	}

	if (connected)
		dcp_dptx_connect(dcp, 0);

	return IRQ_HANDLED;
}

void dcp_link(struct platform_device *pdev, struct apple_crtc *crtc,
	      struct apple_connector *connector)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	dcp->crtc = crtc;

	/*
	 * Type-C connectors belong to physical ports and are bound by the
	 * display fabric when a pipeline takes a route, so a pipeline with no
	 * fixed output simply has no connector until then.
	 */
	if (!connector)
		return;

	dcp->fixed_connector = connector;
	if (!dcp->active_typec_route) {
		dcp->connector = connector;
		dcp->connector_type = dcp->fixed_connector_type;
	}
}


bool dcp_fw_compat_is_12_x(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->fw_compat == DCP_FIRMWARE_V_12_3;
}

unsigned long* dcp_get_iomfb_surfaces(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->iomfb_surfaces;
}

int dcp_start(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	init_completion(&dcp->start_done);

	/* start RTKit endpoints */
	ret = systemep_init(dcp);
	if (ret)
		dev_warn(dcp->dev, "Failed to start system endpoint: %d\n", ret);

	if (unstable_edid && !dcp_has_panel(dcp)) {
		ret = dpavservep_init(dcp);
		if (ret)
			dev_warn(dcp->dev, "Failed to start DPAVSERV endpoint: %d",
				 ret);
	}

	if (dcp->phy && dcp->fw_compat >= DCP_FIRMWARE_V_13_5) {
		ret = ibootep_init(dcp);
		if (ret)
			dev_warn(dcp->dev, "Failed to start IBOOT endpoint: %d\n",
				 ret);

		ret = dptxep_init(dcp);
		if (ret) {
			dev_warn(dcp->dev, "Failed to start DPTX endpoint: %d\n",
				 ret);
#ifdef DCP_DPTX_DISCONNECT_ON_INIT
		/*
		 * This disconnect / connect cycle on init is only necessary
		 * when using dcp0 on j473, j474s and presumedly j475c.
		 * Since dcp0 is not used at the moment let's avoid this
		 * since it is possibly the cause for startup issues.
		 */
		} else if (dcp->dptxport[0].enabled) {
			bool connected;
			/* force disconnect on start - necessary if the display
			 * is already up from m1n1
			 */
			dptxport_set_hpd(dcp->dptxport[0].service, false);
			dptxport_release_display(dcp->dptxport[0].service);
			usleep_range(10 * USEC_PER_MSEC, 25 * USEC_PER_MSEC);

			connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
			dev_info(dcp->dev, "%s: DP2HDMI HPD connected:%d\n", __func__, connected);

			// necessary on j473/j474 but not on j314c
			if (connected)
				dcp_dptx_connect(dcp, 0);
#endif
		}
	} else if (dcp->phy) {
		dev_warn(dcp->dev, "OS firmware incompatible with dptxport EP\n");
	}
	ret = iomfb_start_rtkit(dcp);
	if (ret)
		dev_err(dcp->dev, "Failed to start IOMFB endpoint: %d\n", ret);

#if IS_ENABLED(CONFIG_DRM_APPLE_AUDIO)
	if (hdmi_audio) {
		ret = avep_init(dcp);
		if (ret)
			dev_warn(dcp->dev, "Failed to start AV endpoint: %d", ret);
		ret = 0;
	}
#endif

	return ret;
}

static void _dcp_poweroff(struct apple_dcp *dcp)
{
	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_poweroff_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_poweroff_v13_3(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

static int dcp_enable_dp2hdmi_hpd(struct apple_dcp *dcp)
{
	if (dcp_is_typec_output(dcp)) {
		if (READ_ONCE(dcp->typec_cable_connected))
			dcp_dptx_connect(dcp, 0);
	} else if (dcp->hdmi_hpd) {
		/* Check HPD before enabling the edge-triggered IRQ. */
		bool connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		dev_info(dcp->dev, "%s: DP2HDMI HPD connected:%d\n", __func__, connected);

		if (connected)
			dcp_dptx_connect(dcp, 0);
		else
			_dcp_poweroff(dcp);
	}

	if (dcp->hdmi_hpd_irq)
		enable_irq(dcp->hdmi_hpd_irq);

	return 0;
}

int dcp_wait_ready(struct platform_device *pdev, u64 timeout)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	if (dcp->crashed)
		return -ENODEV;
	if (dcp->active)
		return dcp_enable_dp2hdmi_hpd(dcp);
	if (timeout <= 0)
		return -ETIMEDOUT;

	ret = wait_for_completion_timeout(&dcp->start_done, timeout);
	if (ret < 0)
		return ret;

	if (dcp->crashed)
		return -ENODEV;

	if (dcp->active)
		dcp_enable_dp2hdmi_hpd(dcp);

	return dcp->active ? 0 : -ETIMEDOUT;
}

static void __maybe_unused dcp_sleep(struct apple_dcp *dcp)
{
	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_sleep_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_sleep_v13_3(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

void dcp_poweron(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	bool wait_for_typec_hpd = false;
	unsigned long remaining;
	int ret;

	if (dcp_is_typec_output(dcp)) {
		wait_for_typec_hpd = READ_ONCE(dcp->typec_crtc_off) &&
				    READ_ONCE(dcp->typec_cable_connected);
		WRITE_ONCE(dcp->typec_crtc_off, false);

		/*
		 * A Type-C CRTC disable releases its DPTX session. Re-establish it
		 * synchronously before IOMFB is powered back on.
		 */
		if (READ_ONCE(dcp->typec_cable_connected)) {
			cancel_delayed_work(&dcp->typec_reconnect_wq);
			dcp->typec_reconnect_tries = 0;
			ret = dcp_dptx_connect(dcp, 0);
			if (ret)
				mod_delayed_work(system_freezable_wq,
						 &dcp->typec_reconnect_wq,
						 DPTX_RECONNECT_DELAY);
			else if (wait_for_typec_hpd) {
				remaining = wait_for_completion_timeout(
					&dcp->typec_iomfb_hpd_ready,
					msecs_to_jiffies(3000));
				if (!remaining)
					dev_warn(dcp->dev,
						 "Type-C IOMFB hotplug not ready on wake\n");
			}
		}
	} else if (dcp->hdmi_hpd) {
		bool connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		dev_info(dcp->dev, "%s: DP2HDMI HPD connected:%d\n", __func__, connected);

		if (connected)
			dcp_dptx_connect(dcp, 0);
	}

	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_poweron_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_poweron_v13_3(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
	if (dcp->avep)
		av_service_connect(dcp);
}

void dcp_poweroff(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	cancel_delayed_work(&dcp->swap_watchdog_wq);
	if (dcp->avep)
		av_service_disconnect(dcp);

	/*
	 * Powering a Type-C CRTC off drops DCP's synthetic HPD, and the firmware
	 * reports that as an unplug. The display is still attached: keep the
	 * connector connected (see dcpep_cb_hotplug()) and let dcp_poweron()
	 * re-establish the DPTX session. Recreating it here instead makes the
	 * display vanish and come back, and compositors light a returning
	 * display, so DPMS off never sticks. Cable removal is reported through
	 * the Type-C mux.
	 */
	if (dcp_is_typec_output(dcp)) {
		reinit_completion(&dcp->typec_iomfb_hpd_ready);
		if (READ_ONCE(dcp->typec_cable_connected))
			WRITE_ONCE(dcp->typec_crtc_off, true);
		/* dcp_poweron() reconnects the link on DPMS wake. */
		cancel_delayed_work(&dcp->typec_reconnect_wq);
	}

	_dcp_poweroff(dcp);

	if (dcp_is_typec_output(dcp)) {
		/* DCP owns a synthetic HPD for Type-C. Release it with the CRTC. */
		if (dcp->dptxport[0].enabled && dcp->dptxport[0].connected) {
			ret = dptxport_set_hpd(dcp->dptxport[0].service, false);
			if (ret)
				dev_warn(dcp->dev,
					 "failed to deassert Type-C DPTX HPD: %d\n", ret);
			dcp_dptx_disconnect(dcp, 0);
		}
	} else if (dcp->hdmi_hpd) {
		bool connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		if (!connected) {
			disconnected_hpd_event(dcp->connector);
			dcp_dptx_disconnect(dcp, 0);
		}
	}
}

static void dcp_work_register_backlight(struct work_struct *work)
{
	int ret;
	struct apple_dcp *dcp;

	dcp = container_of(work, struct apple_dcp, bl_register_wq);

	mutex_lock(&dcp->bl_register_mutex);
	if (dcp->brightness.bl_dev)
		goto out_unlock;

	/* try to register backlight device, */
	ret = dcp_backlight_register(dcp);
	if (ret) {
		dev_err(dcp->dev, "Unable to register backlight device\n");
		dcp->brightness.maximum = 0;
	}

out_unlock:
	mutex_unlock(&dcp->bl_register_mutex);
}

static void dcp_work_update_backlight(struct work_struct *work)
{
	struct apple_dcp *dcp;

	dcp = container_of(work, struct apple_dcp, bl_update_wq);

	dcp_backlight_update(dcp);
}

static int dcp_create_piodma_iommu_dev(struct apple_dcp *dcp)
{
	int ret;
	struct device_node *node __free(device_node) = of_get_child_by_name(dcp->dev->of_node, "piodma");

	if (!node)
		return dev_err_probe(dcp->dev, -ENODEV,
				     "Failed to get piodma child DT node\n");

	dcp->piodma = of_platform_device_create(node, NULL, dcp->dev);
	if (!dcp->piodma)
		return dev_err_probe(dcp->dev, -ENODEV, "Failed to create piodma pdev for %pOF\n", node);

	ret = dma_set_mask_and_coherent(&dcp->piodma->dev, DMA_BIT_MASK(42));
	if (ret)
		goto err_destroy_pdev;

	ret = of_dma_configure(&dcp->piodma->dev, node, true);
	if (ret) {
		ret = dev_err_probe(dcp->dev, ret,
			"Failed to configure IOMMU child DMA\n");
		goto err_destroy_pdev;
	}

	dcp->iommu_dom = iommu_get_domain_for_dev(&dcp->piodma->dev);
	if (IS_ERR(dcp->iommu_dom)) {
		ret = dev_err_probe(dcp->dev, PTR_ERR(dcp->iommu_dom),
				    "Failed to get default iommu domain for "
				    "piodma device\n");
		dcp->iommu_dom = NULL;
		goto err_destroy_pdev;
	}

	return 0;
err_destroy_pdev:
	of_platform_device_destroy(&dcp->piodma->dev, NULL);
	return ret;
}

static int dcp_get_bw_scratch_reg(struct apple_dcp *dcp, u32 expected)
{
	struct of_phandle_args ph_args;
	u32 addr_idx, disp_idx, offset;
	int ret;

	ret = of_parse_phandle_with_args(dcp->dev->of_node, "apple,bw-scratch",
				   "#apple,bw-scratch-cells", 0, &ph_args);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to read 'apple,bw-scratch': %d\n", ret);
		return ret;
	}

	if (ph_args.args_count != 3) {
		dev_err(dcp->dev, "Unexpected 'apple,bw-scratch' arg count %d\n",
			ph_args.args_count);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	addr_idx = ph_args.args[0];
	disp_idx = ph_args.args[1];
	offset = ph_args.args[2];

	if (disp_idx != expected || disp_idx >= MAX_DISP_REGISTERS) {
		dev_err(dcp->dev, "Unexpected disp_reg value in 'apple,bw-scratch': %d\n",
			disp_idx);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	ret = of_address_to_resource(ph_args.np, addr_idx, &dcp->disp_bw_scratch_res);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to get 'apple,bw-scratch' resource %d from %pOF\n",
			addr_idx, ph_args.np);
		goto err_of_node_put;
	}
	if (offset > resource_size(&dcp->disp_bw_scratch_res) - 4) {
		ret = -EINVAL;
		goto err_of_node_put;
	}

	dcp->disp_registers[disp_idx] = &dcp->disp_bw_scratch_res;
	dcp->disp_bw_scratch_index = disp_idx;
	dcp->disp_bw_scratch_offset = offset;
	ret = 0;

err_of_node_put:
	of_node_put(ph_args.np);
	return ret;
}

static int dcp_get_bw_doorbell_reg(struct apple_dcp *dcp, u32 expected)
{
	struct of_phandle_args ph_args;
	u32 addr_idx, disp_idx;
	int ret;

	ret = of_parse_phandle_with_args(dcp->dev->of_node, "apple,bw-doorbell",
				   "#apple,bw-doorbell-cells", 0, &ph_args);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to read 'apple,bw-doorbell': %d\n", ret);
		return ret;
	}

	if (ph_args.args_count != 2) {
		dev_err(dcp->dev, "Unexpected 'apple,bw-doorbell' arg count %d\n",
			ph_args.args_count);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	addr_idx = ph_args.args[0];
	disp_idx = ph_args.args[1];

	if (disp_idx != expected || disp_idx >= MAX_DISP_REGISTERS) {
		dev_err(dcp->dev, "Unexpected disp_reg value in 'apple,bw-doorbell': %d\n",
			disp_idx);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	ret = of_address_to_resource(ph_args.np, addr_idx, &dcp->disp_bw_doorbell_res);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to get 'apple,bw-doorbell' resource %d from %pOF\n",
			addr_idx, ph_args.np);
		goto err_of_node_put;
	}
	dcp->disp_bw_doorbell_index = disp_idx;
	dcp->disp_registers[disp_idx] = &dcp->disp_bw_doorbell_res;
	ret = 0;

err_of_node_put:
	of_node_put(ph_args.np);
	return ret;
}

static int dcp_get_disp_regs(struct apple_dcp *dcp)
{
	struct platform_device *pdev = to_platform_device(dcp->dev);
	int count = pdev->num_resources - 1;
	int i, ret;

	if (count <= 0 || count > MAX_DISP_REGISTERS)
		return -EINVAL;

	for (i = 0; i < count; ++i) {
		dcp->disp_registers[i] =
			platform_get_resource(pdev, IORESOURCE_MEM, 1 + i);
	}

	/* load pmgr bandwidth scratch resource and offset */
	ret = dcp_get_bw_scratch_reg(dcp, count);
	if (ret < 0)
		return ret;
	count += 1;

	/* load pmgr bandwidth doorbell resource if present (only on t8103) */
	if (of_property_present(dcp->dev->of_node, "apple,bw-doorbell")) {
		ret = dcp_get_bw_doorbell_reg(dcp, count);
		if (ret < 0)
			return ret;
		count += 1;
	}

	dcp->nr_disp_registers = count;
	return 0;
}

#define DCP_FW_VERSION_MIN_LEN	3
#define DCP_FW_VERSION_MAX_LEN	5
#define DCP_FW_VERSION_STR_LEN	(DCP_FW_VERSION_MAX_LEN * 4)

static int dcp_read_fw_version(struct device *dev, const char *name,
			       char *version_str)
{
	u32 ver[DCP_FW_VERSION_MAX_LEN];
	int len_str;
	int len;

	len = of_property_read_variable_u32_array(dev->of_node, name, ver,
						  DCP_FW_VERSION_MIN_LEN,
						  DCP_FW_VERSION_MAX_LEN);

	switch (len) {
	case 3:
		len_str = scnprintf(version_str, DCP_FW_VERSION_STR_LEN,
				    "%d.%d.%d", ver[0], ver[1], ver[2]);
		break;
	case 4:
		len_str = scnprintf(version_str, DCP_FW_VERSION_STR_LEN,
				    "%d.%d.%d.%d", ver[0], ver[1], ver[2],
				    ver[3]);
		break;
	case 5:
		len_str = scnprintf(version_str, DCP_FW_VERSION_STR_LEN,
				    "%d.%d.%d.%d.%d", ver[0], ver[1], ver[2],
				    ver[3], ver[4]);
		break;
	default:
		len_str = strscpy(version_str, "UNKNOWN",
				  DCP_FW_VERSION_STR_LEN);
		if (len >= 0)
			len = -EOVERFLOW;
		break;
	}

	if (len_str >= DCP_FW_VERSION_STR_LEN)
		dev_warn(dev, "'%s' truncated: '%s'\n", name, version_str);

	return len;
}

static enum dcp_firmware_version dcp_check_firmware_version(struct device *dev)
{
	char compat_str[DCP_FW_VERSION_STR_LEN];
	char fw_str[DCP_FW_VERSION_STR_LEN];
	int ret;

	/* firmware version is just informative */
	dcp_read_fw_version(dev, "apple,firmware-version", fw_str);

	ret = dcp_read_fw_version(dev, "apple,firmware-compat", compat_str);
	if (ret < 0) {
		dev_err(dev, "Could not read 'apple,firmware-compat': %d\n", ret);
		return DCP_FIRMWARE_UNKNOWN;
	}

	if (strncmp(compat_str, "12.3.0", sizeof(compat_str)) == 0)
		return DCP_FIRMWARE_V_12_3;
	/*
	 * m1n1 reports firmware version 13.5 as compatible with 13.3. This is
	 * only true for the iomfb endpoint. The interface for the dptx-port
	 * endpoint changed between 13.3 and 13.5. The driver will only support
	 * firmware 13.5. Check the actual firmware version for compat version
	 * 13.3 until m1n1 reports 13.5 as "firmware-compat".
	 */
	else if ((strncmp(compat_str, "13.3.0", sizeof(compat_str)) == 0) &&
		 (strncmp(fw_str, "13.5.0", sizeof(compat_str)) == 0))
		return DCP_FIRMWARE_V_13_5;
	else if (strncmp(compat_str, "13.5.0", sizeof(compat_str)) == 0)
		return DCP_FIRMWARE_V_13_5;

	dev_err(dev, "DCP firmware-compat %s (FW: %s) is not supported\n",
		compat_str, fw_str);

	return DCP_FIRMWARE_UNKNOWN;
}

static int dcp_connector_type_from_dt(struct device_node *np)
{
	if (of_property_match_string(np, "apple,connector-type", "HDMI-A") >= 0)
		return DRM_MODE_CONNECTOR_HDMIA;
	if (of_property_match_string(np, "apple,connector-type", "DP") >= 0)
		return DRM_MODE_CONNECTOR_DisplayPort;
	if (of_property_match_string(np, "apple,connector-type", "USB-C") >= 0)
		return DRM_MODE_CONNECTOR_USB;

	return DRM_MODE_CONNECTOR_Unknown;
}

static void dcp_disable_typec_work(struct apple_dcp *dcp, bool release_cable)
{
	scoped_guard(mutex, &dcp->hpd_mutex) {
		if (release_cable)
			WRITE_ONCE(dcp->typec_cable_connected, false);
		dcp->typec_generation++;
	}
	/* Block new enqueues as well as draining users of the AFK endpoints. */
	disable_delayed_work_sync(&dcp->typec_reconnect_wq);
	disable_delayed_work_sync(&dcp->placeholder_edid_wq);
	disable_delayed_work_sync(&dcp->typec_fabric_retrain_wq);
}

static void dcp_enable_typec_work(struct apple_dcp *dcp)
{
	enable_delayed_work(&dcp->typec_reconnect_wq);
	enable_delayed_work(&dcp->placeholder_edid_wq);
	enable_delayed_work(&dcp->typec_fabric_retrain_wq);
	/* A cable can be routed before the DRM component binds. */
	if (READ_ONCE(dcp->typec_cable_connected))
		mod_delayed_work(system_freezable_wq, &dcp->typec_reconnect_wq, 0);
}

static int dcp_comp_bind(struct device *dev, struct device *main, void *data)
{
	struct device_node *panel_np;
	struct apple_dcp *dcp = dev_get_drvdata(dev);
	u32 cpu_ctrl;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42));
	if (ret)
		return ret;

	dcp->coproc_reg = devm_platform_ioremap_resource_byname(to_platform_device(dev), "coproc");
	if (IS_ERR(dcp->coproc_reg))
		return PTR_ERR(dcp->coproc_reg);

	if (dcp->index || dcp->dptx_phy || dcp->dptx_die)
		dev_info(dev, "DCP index:%u dptx target phy: %u dptx die: %u\n",
			 dcp->index, dcp->dptx_phy, dcp->dptx_die);

	if (!show_notch)
		ret = of_property_read_u32(dev->of_node, "apple,notch-height",
					   &dcp->notch_height);

	if (dcp->notch_height > MAX_NOTCH_HEIGHT)
		dcp->notch_height = MAX_NOTCH_HEIGHT;
	if (dcp->notch_height > 0)
		dev_info(dev, "Detected display with notch of %u pixel\n", dcp->notch_height);

	/* initialize brightness scale to a sensible default to avoid divide by 0*/
	dcp->brightness.scale = 65536;
	panel_np = of_get_compatible_child(dev->of_node, "apple,panel-mini-led");
	if (panel_np)
		dcp->panel.has_mini_led = true;
	else
		panel_np = of_get_compatible_child(dev->of_node, "apple,panel");

	if (panel_np) {
		const char height_prop[2][16] = { "adj-height-mm", "height-mm" };

		if (of_device_is_available(panel_np)) {
			ret = of_property_read_u32(panel_np, "apple,max-brightness",
						   &dcp->brightness.maximum);
			if (ret)
				dev_err(dev, "Missing property 'apple,max-brightness'\n");
		}

		of_property_read_u32(panel_np, "width-mm", &dcp->panel.width_mm);
		/* use adjusted height as long as the notch is hidden */
		of_property_read_u32(panel_np, height_prop[!dcp->notch_height],
				     &dcp->panel.height_mm);

		of_node_put(panel_np);
		dcp->fixed_connector_type = DRM_MODE_CONNECTOR_eDP;
		dcp->connector_type = DRM_MODE_CONNECTOR_eDP;
		INIT_WORK(&dcp->bl_register_wq, dcp_work_register_backlight);
		mutex_init(&dcp->bl_register_mutex);
		INIT_WORK(&dcp->bl_update_wq, dcp_work_update_backlight);
	}

	ret = dcp_create_piodma_iommu_dev(dcp);
	if (ret || !dcp->iommu_dom)
		return dev_err_probe(dev, ret,
				"Failed to created PIODMA iommu child device");

	ret = dcp_get_disp_regs(dcp);
	if (ret) {
		dev_err(dev, "failed to find display registers\n");
		return ret;
	}

	dcp->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(dcp->clk))
		return dev_err_probe(dev, PTR_ERR(dcp->clk),
				     "Unable to find clock\n");

	bitmap_zero(dcp->memdesc_map, DCP_MAX_MAPPINGS);
	// TDOD: mem_desc IDs start at 1, for simplicity just skip '0' entry
	set_bit(0, dcp->memdesc_map);

	INIT_WORK(&dcp->vblank_wq, dcp_delayed_vblank);
	INIT_DELAYED_WORK(&dcp->swap_watchdog_wq, dcp_swap_watchdog);

	dcp->swapped_out_fbs =
		(struct list_head)LIST_HEAD_INIT(dcp->swapped_out_fbs);

	cpu_ctrl =
		readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	writel_relaxed(cpu_ctrl | APPLE_DCP_COPROC_CPU_CONTROL_RUN,
		       dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);

	dcp->rtk = devm_apple_rtkit_init(dev, dcp, "mbox", 0, &rtkit_ops);
	if (IS_ERR(dcp->rtk))
		return dev_err_probe(dev, PTR_ERR(dcp->rtk),
				     "Failed to initialize RTKit\n");

	ret = apple_rtkit_wake(dcp->rtk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to boot RTKit: %d\n", ret);
	dcp_enable_typec_work(dcp);
	return ret;
}

/*
 * We need to shutdown DCP before tearing down the display subsystem. Otherwise
 * the DCP will crash and briefly flash a green screen of death.
 */
static void dcp_comp_unbind(struct device *dev, struct device *main, void *data)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	if (!dcp)
		return;

	if (dcp->hdmi_hpd_irq)
		disable_irq(dcp->hdmi_hpd_irq);

	dcp_disable_typec_work(dcp, true);
	typec_mux_put(dcp->typec_mux);

	if (dcp->avep) {
		av_service_disconnect(dcp);
		afk_shutdown(dcp->avep);
		dcp->avep = NULL;
	}

	if (dcp->dptxep) {
		afk_shutdown(dcp->dptxep);
		dcp->dptxep = NULL;
	}

	if (dcp->ibootep) {
		afk_shutdown(dcp->ibootep);
		dcp->ibootep = NULL;
	}

	if (dcp->systemep) {
		afk_shutdown(dcp->systemep);
		dcp->systemep = NULL;
	}

	if (dcp->dcpavservep) {
		afk_shutdown(dcp->dcpavservep);
		dpavservep_detach(dcp);
		dcp->dcpavservep = NULL;
	}

	if (dcp->shmem)
		iomfb_shutdown(dcp);

	if (dcp->piodma) {
		dcp->iommu_dom = NULL;
		of_platform_device_destroy(&dcp->piodma->dev, NULL);
		dcp->piodma = NULL;
	}

	if (dcp->connector_type == DRM_MODE_CONNECTOR_eDP) {
		cancel_work_sync(&dcp->bl_register_wq);
		cancel_work_sync(&dcp->bl_update_wq);
	}
	cancel_delayed_work_sync(&dcp->swap_watchdog_wq);
	cancel_work_sync(&dcp->vblank_wq);

	devm_clk_put(dev, dcp->clk);
	dcp->clk = NULL;
}

static const struct component_ops dcp_comp_ops = {
	.bind	= dcp_comp_bind,
	.unbind	= dcp_comp_unbind,
};

static int dcp_platform_probe(struct platform_device *pdev)
{
	enum dcp_firmware_version fw_compat;
	struct device *dev = &pdev->dev;
	struct apple_dcp *dcp;
	int ret, surf, num_surfs;
	u32 surf_en;
	u32 mux_index;

	fw_compat = dcp_check_firmware_version(dev);
	if (fw_compat == DCP_FIRMWARE_UNKNOWN)
		return -ENODEV;

	/* Check for "apple,bw-scratch" to avoid probing appledrm with outdated
	 * device trees. This prevents replacing simpledrm and ending up without
	 * display.
	 */
	if (!of_property_present(dev->of_node, "apple,bw-scratch"))
		return dev_err_probe(dev, -ENODEV, "Incompatible devicetree! "
			"Use devicetree matching this kernel.\n");

	dcp = devm_kzalloc(dev, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return -ENOMEM;

	dcp->fw_compat = fw_compat;
	dcp->dev = dev;
	/*
	 * Type-C and Thunderbolt routes can be activated as soon as they are
	 * registered below, before the DRM device binds.
	 */
	mutex_init(&dcp->hpd_mutex);
	mutex_init(&dcp->tb_lock);
	spin_lock_init(&dcp->mode_state.lock);
	spin_lock_init(&dcp->dcpavserv.lock);
	dcp->hw = *(struct apple_dcp_hw_data *)of_device_get_match_data(dev);
	dcp->fixed_connector_type = dcp_connector_type_from_dt(dev->of_node);
	dcp->connector_type = dcp->fixed_connector_type;
	of_property_read_u32(dev->of_node, "apple,dcp-index", &dcp->index);
	of_property_read_u32(dev->of_node, "apple,dptx-phy", &dcp->dptx_phy);
	of_property_read_u32(dev->of_node, "apple,dptx-die", &dcp->dptx_die);
	dcp->fixed_dptx_phy = dcp->dptx_phy;
	init_completion(&dcp->typec_iomfb_hpd_ready);
	INIT_DELAYED_WORK(&dcp->typec_reconnect_wq,
			  dcp_typec_reconnect_work);
	INIT_DELAYED_WORK(&dcp->placeholder_edid_wq,
			  dcp_placeholder_edid_work);
	INIT_DELAYED_WORK(&dcp->typec_fabric_retrain_wq,
			  dcp_typec_retrain_work);
	/* Balanced by enable at successful component bind. */
	disable_delayed_work(&dcp->typec_reconnect_wq);
	disable_delayed_work(&dcp->placeholder_edid_wq);
	disable_delayed_work(&dcp->typec_fabric_retrain_wq);

	platform_set_drvdata(pdev, dcp);

	dcp->phy = devm_phy_optional_get(dev, "dp-phy");
	if (IS_ERR(dcp->phy)) {
		dev_err(dev, "Failed to get dp-phy: %ld\n", PTR_ERR(dcp->phy));
		return PTR_ERR(dcp->phy);
	}
	dcp->fixed_phy = dcp->phy;

	bitmap_zero(dcp->iomfb_surfaces, DCP_MAX_PLANES);
	if (!of_property_present(dev->of_node, "apple,iomfb-surfaces"))
		num_surfs = 0;
	else
		num_surfs = of_property_count_elems_of_size(dev->of_node,
						    "apple,iomfb-surfaces",
						    sizeof(u32));

	if (num_surfs == 0 || num_surfs == -ENODATA) {
		set_bit(0, dcp->iomfb_surfaces);
		set_bit(1, dcp->iomfb_surfaces);
	} else if (num_surfs < 0) {
		return num_surfs;
	} else if (num_surfs > DCP_MAX_PLANES) {
		dev_err(dev, "Number of iomfb-surfaces (%d) exceeds DCP_MAX_PLANES\n",
			num_surfs);
		return -EINVAL;
	}

	surf = 0;
	of_property_for_each_u32(dev->of_node, "apple,iomfb-surfaces", surf_en) {
		if (surf_en)
			set_bit(surf, dcp->iomfb_surfaces);
		surf++;
	}

	if (dcp->phy) {
		int ret;
		/*
		 * Request DP2HDMI related GPIOs as optional for DP-altmode
		 * compatibility. J180D misses a dp2hdmi-pwren GPIO in the
		 * template ADT. TODO: check device ADT
		 */
		dcp->hdmi_hpd = devm_gpiod_get_optional(dev, "hdmi-hpd", GPIOD_IN);
		if (IS_ERR(dcp->hdmi_hpd))
			return PTR_ERR(dcp->hdmi_hpd);
		if (dcp->hdmi_hpd) {
			int irq = gpiod_to_irq(dcp->hdmi_hpd);
			if (irq < 0) {
				dev_err(dev, "failed to translate HDMI hpd GPIO to IRQ\n");
				return irq;
			}
			dcp->hdmi_hpd_irq = irq;

			ret = devm_request_threaded_irq(dev, dcp->hdmi_hpd_irq,
						NULL, dcp_dp2hdmi_hpd,
						IRQF_ONESHOT | IRQF_NO_AUTOEN |
						IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						"dp2hdmi-hpd-irq", dcp);
			if (ret < 0) {
				dev_err(dev, "failed to request HDMI hpd irq %d: %d\n",
					irq, ret);
				return ret;
			}
		}

		/*
		 * Power DP2HDMI on as it is required for the HPD irq.
		 * TODO: check if one is sufficient for the hpd to save power
		 *       on battery powered Macbooks.
		 */
		dcp->hdmi_pwren = devm_gpiod_get_optional(dev, "hdmi-pwren", GPIOD_OUT_HIGH);
		if (IS_ERR(dcp->hdmi_pwren))
			return PTR_ERR(dcp->hdmi_pwren);

		dcp->dp2hdmi_pwren = devm_gpiod_get_optional(dev, "dp2hdmi-pwren", GPIOD_OUT_HIGH);
		if (IS_ERR(dcp->dp2hdmi_pwren))
			return PTR_ERR(dcp->dp2hdmi_pwren);

		/*
		 * A DCP may have both a fixed HDMI/DP route and allocatable Type-C
		 * routes. Keep the fixed route selected until the allocator borrows
		 * this otherwise-idle pipeline for a Type-C display.
		 */
		ret = dcp->fixed_phy ?
			of_property_read_u32(dev->of_node, "mux-index", &mux_index) :
			-ENODATA;
		if (!ret) {
			dcp->fixed_mux_index = mux_index;
			dcp->xbar = devm_mux_control_get(dev, "dp-xbar");
			if (IS_ERR(dcp->xbar)) {
				dev_err(dev, "Failed to get dp-xbar: %ld\n", PTR_ERR(dcp->xbar));
				return PTR_ERR(dcp->xbar);
			}
			ret = mux_control_select(dcp->xbar, mux_index);
			if (ret)
				dev_warn(dev, "mux_control_select failed: %d\n", ret);
			else
				dcp->fixed_route_selected = true;

			/*
			 * Switch atcphy to DP-only. should move to a Macbook Pro
			 * 14-/16-inch specific DP-to-HDMI drm_bridge.
			 */
			dcp->typec_mux = fwnode_typec_mux_get(dev_fwnode(dcp->dev));
			if (!IS_ERR_OR_NULL(dcp->typec_mux)) {
				struct typec_altmode alt = {
					.svid = USB_TYPEC_DP_SID,
				};
				struct typec_mux_state state = {
					.alt = &alt,
					.mode = TYPEC_DP_STATE_C,
				};
				int ret = typec_mux_set(dcp->typec_mux, &state);
				dev_info(dev, "typec_mux_set() returned: %d\n", ret);
				if (!ret)
					dcp->phy_managed_by_typec = true;
			} else {
				dev_info(dev, "fwnode_typec_mux_get() returned: %ld\n",
						IS_ERR(dcp->typec_mux) ? PTR_ERR(dcp->typec_mux) : 0);
				dcp->typec_mux = NULL;
			}
		}
	}

	ret = dcp_register_typec_routes(dcp);
	if (ret)
		return ret;

	return component_add(&pdev->dev, &dcp_comp_ops);
}

static void dcp_platform_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dcp_comp_ops);
}

static void dcp_platform_shutdown(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dcp_comp_ops);
}

static int dcp_platform_suspend(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	/*
	 * The Type-C route reports cable removal through
	 * dcp_dptx_disconnect_oob(). A DP tunnel kept through the sleep stays
	 * connected, and resume powers the CRTC back up through dcp_poweron()
	 * as after DPMS off.
	 */
	dcp_disable_typec_work(dcp, false);
	cancel_delayed_work_sync(&dcp->swap_watchdog_wq);

	if (dcp->avep)
		av_service_disconnect(dcp);

	if (dcp->hdmi_hpd_irq) {
		disable_irq(dcp->hdmi_hpd_irq);
		if (!dcp->active_typec_route) {
			disconnected_hpd_event(dcp->connector);
			dcp_dptx_disconnect(dcp, 0);
		}
	}
	/*
	 * Set the device as a wakeup device, which forces its power
	 * domains to stay on. We need this as we do not support full
	 * shutdown properly yet.
	 */
	device_set_wakeup_path(dev);

	return 0;
}

static int dcp_platform_resume(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	dcp_enable_typec_work(dcp);
	if (dcp->hdmi_hpd_irq)
		enable_irq(dcp->hdmi_hpd_irq);

	if (dcp->avep)
		av_service_connect(dcp);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(dcp_platform_pm_ops,
				dcp_platform_suspend, dcp_platform_resume);


static const struct apple_dcp_hw_data apple_dcp_hw_t6020 = {
	.num_dptx_ports = 1,
};

static const struct apple_dcp_hw_data apple_dcp_hw_t8112 = {
	.num_dptx_ports = 2,
};

static const struct apple_dcp_hw_data apple_dcp_hw_dcp = {
	.num_dptx_ports = 0,
};

static const struct apple_dcp_hw_data apple_dcp_hw_dcpext = {
	.num_dptx_ports = 2,
};

static const struct of_device_id of_match[] = {
	{ .compatible = "apple,t6020-dcp", .data = &apple_dcp_hw_t6020,  },
	{ .compatible = "apple,t8112-dcp", .data = &apple_dcp_hw_t8112,  },
	{ .compatible = "apple,dcp",       .data = &apple_dcp_hw_dcp,    },
	{ .compatible = "apple,dcpext",    .data = &apple_dcp_hw_dcpext, },
	{}
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver apple_platform_driver = {
	.probe		= dcp_platform_probe,
	.remove		= dcp_platform_remove,
	.shutdown	= dcp_platform_shutdown,
	.driver	= {
		.name = "apple-dcp",
		.of_match_table	= of_match,
		.pm = pm_sleep_ptr(&dcp_platform_pm_ops),
	},
};

void __init dcp_register(void)
{
	platform_driver_register(&apple_platform_driver);
}

void __exit dcp_unregister(void)
{
	platform_driver_unregister(&apple_platform_driver);
}
