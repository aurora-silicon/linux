// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2021 Alyssa Rosenzweig */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mux/driver.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/slab.h>
#include <linux/soc/apple/dp-tunnel.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/workqueue.h>

#include <drm/drm_file.h>

#include "afk.h"
#include "dcp.h"
#include "dcp-fabric.h"
#include "dcpext_scanout.h"
#include "ibootep.h"

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
	/*
	 * The 14.7 firmware drives the internal panel on the IOMFB path.
	 * The DPTX service is a separate output, so the live panel does not
	 * occupy the Type-C route.
	 */
	if (dcp->fw_compat == DCP_FIRMWARE_V_14_7)
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

static int dcp_dpxbar_tunnel_select_source(struct mux_control *mux, int state)
{
	typeof(apple_dpxbar_tunnel_select_source) *select =
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
	unsigned int score;

	if (!dcp->crtc)
		return UINT_MAX - 1;

	score = drm_crtc_index(&dcp->crtc->base);
	/*
	 * Without dual-stream docks no stream is tied to the hybrid, so it goes
	 * to a Type-C port only when no Type-C-only pipeline is free, and an HDMI
	 * display plugged in later finds its pipeline idle.  The port's encoder is
	 * narrowed to the routed pipeline before its connector reports connected,
	 * so the compositor pairs the connector with that pipeline.
	 */
	if (!dcp_typec_dual_stream() && dcp->fixed_phy)
		score += 100;

	return score;
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
	WRITE_ONCE(dcp->ext_backlight, false);
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
		/* mux_control_deselect releases its semaphore even on failure. */
		route->xbar_up = false;
		if (ret)
			dev_warn(dcp->dev, "crossbar deselect failed: %d\n", ret);
		/* Ownership is released; let the caller release its route owner too. */
		ret = 0;

		if (route->tunnel && dcp->phy) {
			/* the tunnel pixel clock must not outlive the tunnel */
			typeof(apple_atc_dp_tunnel_rate) *stop =
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
		/* no pipeline, so no backlight */
		schedule_work(&connector->bl_sync_wq);

		/* Unrouted: the port could go to any of its pipelines again. */
		if (connector->port_encoder && !dcp_typec_dual_stream())
			connector->port_encoder->possible_crtcs =
				connector->candidate_crtcs;
	}
	dcp->typec_connector = NULL;
	dcp->connector = dcp->fixed_connector;
	WRITE_ONCE(dcp->ext_backlight, false);

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

void dcp_typec_retrain_work(struct work_struct *work)
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
 *open; moving routes under it would hand connectors to pipelines driving
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

/*
 * HDMI hotplug often blinks: a monitor waking up or switching inputs drops
 * HPD for a second or so.  A Thunderbolt tunnel never moves once it has a
 * pipeline, so without dual-stream docks one may not take the hybrid until
 * HDMI has been quiet for this long, or a tunnel waiting for a pipeline
 * would take it in the blink and keep the returning HDMI display dark.
 */
#define DCP_HDMI_HOLD_MS	10000

void dcp_hdmi_hold(struct apple_dcp *dcp)
{
	WRITE_ONCE(dcp->hdmi_hold_until,
		   jiffies + msecs_to_jiffies(DCP_HDMI_HOLD_MS));
}

static bool dcp_hdmi_held(struct apple_dcp *dcp)
{
	unsigned long until = READ_ONCE(dcp->hdmi_hold_until);

	return dcp->hdmi_hpd && until && time_before(jiffies, until);
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
 * The pipeline a port without one takes: the one it last had if that is
 * free, as a compositor keeps a reconnected connector's CRTC, otherwise the
 * best-ranked free one.  Without dual-stream docks a free Type-C-only
 * pipeline comes first even when the port last had the hybrid, which an HDMI
 * display needs.
 */
static struct apple_dcp_typec_route *
dcp_typec_free_route(struct apple_dcp_typec_port *port)
{
	struct apple_dcp_typec_route *last = port->preferred_route;
	struct apple_dcp_typec_route *best = dcp_typec_lowest_free(port);

	if (!last || !dcp_typec_route_available(last))
		return best;
	if (!dcp_typec_dual_stream() && best &&
	    dcp_typec_route_score(best) < dcp_typec_route_score(last))
		return best;
	return last;
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

		route = dcp_typec_free_route(port);
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
 * A display is plugged into the HDMI port while its pipeline, the hybrid,
 * drives a Type-C port.  Without dual-stream docks nothing moves for it: a
 * compositor keeps its connector-to-CRTC pairing and could miss a moved
 * display's brief unplug, and a Thunderbolt tunnel never moves.  The HDMI
 * display waits and is handed the hybrid when the Type-C display lets it go
 * (see dcp_typec_route_deactivate()).
 */
static void dcp_typec_hdmi_waits(struct apple_dcp *dcp)
{
	struct apple_dcp_typec_route *owner = READ_ONCE(dcp->active_typec_route);

	lockdep_assert_held(&dcp_typec_fabric_lock);

	if (owner)
		dev_info(dcp->dev, "HDMI display waits: its pipeline drives the %s on %pOF\n",
			 owner->tunnel ? "Thunderbolt display" : "display",
			 owner->port->connector_np);
}

/*
 * A route has let its pipeline go.  Until a compositor owns the display of a
 * dual-stream machine the plan places everything anew; otherwise the
 * pipeline goes to a port left waiting for one.  The caller has already
 * handed a hybrid back to a live HDMI display.
 */
static void dcp_typec_pipeline_freed(void)
{
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
		best = dcp_typec_free_route(port);
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
	typeof(apple_dpxbar_link_up) *fn;
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
	typeof(apple_dpxbar_preselect) *fn = symbol_get(apple_dpxbar_preselect);
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
	typeof(apple_atc_dp_tunnel_open) *open;
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
 * T6030 drives the DP IN outputs of its T6020-style crossbar directly: the mux
 * selection only routes, and the link is brought up separately. The M2 Pro and
 * M2 Max laptops share the crossbar but take the apple_dp_tunnel_t602x() path.
 */
static bool dcp_t6030_dpin_route(struct apple_dcp_typec_route *route)
{
	return route && route->tunnel && route->active_xbar &&
		of_machine_is_compatible("apple,t6030") &&
		of_device_is_compatible(route->active_xbar->chip->dev.parent->of_node,
					"apple,t6020-display-crossbar");
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
		/* T6030 DP IN selection reserves and routes the mux only. */
		if (!ret && dcp_t6030_dpin_route(route))
			ret = dcp_dpxbar_link(route->active_xbar, true);
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
	typeof(apple_atc_dp_tunnel_rate) *fn;
	struct apple_dcp_typec_route *route;
	int ret;

	guard(mutex)(&dcp->tb_lock);
	if (!dcp->dptx_tunnel)
		return -ENODEV;
	route = dcp->active_typec_route;
	if (!route)
		return -ENODEV;
	/*
	 * The T6030 tunnel clock supports only core0 -> DP IN0 (PCLK1) so far;
	 * other routes need PCLK slot selection and accounting.
	 */
	if (link_rate && of_machine_is_compatible("apple,t6030") &&
	    (route->mux_index != 0 || dcp->dptx_dfp_port != 1))
		return -EOPNOTSUPP;
	fn = symbol_get(apple_atc_dp_tunnel_rate);
	if (!fn) {
		dev_err(dcp->dev, "phy-apple-atc not loaded, no DP tunnel clock\n");
		return -ENOENT;
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
	struct apple_dcp_typec_route *route;
	int ret;

	guard(mutex)(&dcp->tb_lock);
	if (!dcp->dptx_tunnel)
		return 0;
	/* Route the upstream engine before firmware begins AUX negotiation.
	 * T6030 DP IN .set does not enable clocks; DidChange does that later.
	 */
	route = dcp->active_typec_route;
	if (active && dcp_t6030_dpin_route(route)) {
		if (!route->xbar_up) {
			ret = mux_control_try_select(route->active_xbar, route->mux_index);
			if (ret)
				return ret;
			route->xbar_up = true;
		}
	}
	return dcp_tunnel_dpin_locked(dcp, active);
}

/*
 * Thunderbolt DP tunnels: the host router's DP IN adapters sit behind the
 * crossbar's dpin0/dpin1 outputs of the port's ATC. When the Thunderbolt
 * connection manager has set up a tunnel from one of them, route a free
 * display pipeline there and tell DCP a display is attached, so it trains
 * the link (and completes DPRX) through the tunnel.
 */
static bool dcp_tb_services_ready(struct apple_dcp *dcp)
{
	if (!dcp->external || dcp->fw_compat != DCP_FIRMWARE_V_14_7)
		return true;
	/* Pairs with DPTX RemotePort publication before tunnel acquisition. */
	return smp_load_acquire(&dcp->dptxport[0].enabled) && ibootep_is_ready(dcp);
}

int apple_dcp_tb_dp_tunnel(struct device_node *connector_np, unsigned int dpin,
			   bool active, int (*set_active)(void *ctx, bool active),
			   void *ctx)
{
	struct apple_dcp_typec_port *port = NULL, *pos;
	struct apple_dcp_typec_route *candidate, *best = NULL, *planned = NULL;
	struct apple_dcp_typec_route **slot;
	unsigned int best_score = UINT_MAX;
	bool waiting_for_external = false;
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
		return -EADDRINUSE;
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
		if (!dcp_typec_dual_stream() && dcp_hdmi_held(candidate->dcp))
			continue;
		if (!dcp_typec_route_fits(candidate, dpin ?
					  port->secondary_connector :
					  port->connector))
			continue;
		/* A boot-present tunnel may precede the explicit firmware start.
		 * Do not claim its route or spend firmware reconnect attempts
		 * until both command services have been published. Acquire
		 * pairs with RemotePort publication; iBoot checks its cookie
		 * with acquire ordering and refuses a stopping service.
		 */
		/* Retained scanout after link loss must never be reactivated. */
		if (dcpext_scanout_terminal(candidate->dcp))
			return -ESHUTDOWN;
		if (!dcp_tb_services_ready(candidate->dcp)) {
			waiting_for_external = true;
			continue;
		}
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
		ret = waiting_for_external ? -EAGAIN : -EBUSY;
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

	if (dcp->fw_compat == DCP_FIRMWARE_V_14_7 && dcp->dptxep &&
	    !dcp->dptxport[0].enabled)
		dev_warn(dcp->dev, "DPTX port not announced, not opening the controller\n");

	/*
	 * The DP IN adapter may only be woken (DPTX_INACTIVE=0) while DCP
	 * drives the DPTX, i.e. from DCP's Activate call; waking it earlier
	 * hangs the machine. dptxep calls set_active back from Activate and
	 * Deactivate (set above, before the route became a tunnel).
	 */
	if (!dcp->typec_connector && !dcp->external)
		dev_warn(dcp->dev, "no Type-C connector for the DP tunnel\n");
	WRITE_ONCE(dcp->typec_cable_connected, true);
	port->hpd = true;
	/* External firmware link bring-up precedes its DRM connector. Run it
	 * after returning to the tunnel manager, outside the fabric lock.
	 */
	if (dcp->external) {
		dcp->typec_reconnect_tries = 0;
		mod_delayed_work(system_freezable_wq, &dcp->typec_reconnect_wq, 0);
	} else if (dcp->typec_connector) {
		dcp_dptx_connect_oob(to_platform_device(dcp->dev), 0);
	}

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

int dcp_register_typec_routes(struct apple_dcp *dcp)
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

/*
 * A hybrid let go while its HDMI port was empty was parked on a Type-C PHY
 * (see dcp_typec_route_deactivate()).  Point it at the HDMI output again
 * before connecting a display that has arrived there.
 */
static int dcp_fixed_output_select(struct apple_dcp *dcp)
{
	int ret;

	lockdep_assert_held(&dcp_typec_fabric_lock);

	if (!dcp->fixed_phy || dcp->active_typec_route)
		return 0;
	dcp->phy = dcp->fixed_phy;
	dcp->dptx_phy = dcp->fixed_dptx_phy;
	if (dcp->xbar && !dcp->fixed_route_selected) {
		ret = mux_control_select(dcp->xbar, dcp->fixed_mux_index);
		if (ret)
			return ret;
		dcp->fixed_route_selected = true;
	}
	return 0;
}

/*
 * Any HDMI HPD edge: a display is there or just was.  Start the hold at the
 * edge itself, before a tunnel retry can take the fabric lock ahead of the
 * thread (see dcp_hdmi_held()).
 */
irqreturn_t dcp_dp2hdmi_hpd_edge(int irq, void *data)
{
	dcp_hdmi_hold(data);

	return IRQ_WAKE_THREAD;
}

irqreturn_t dcp_dp2hdmi_hpd(int irq, void *data)
{
	struct apple_dcp *dcp = data;
	bool connected;

	guard(mutex)(&dcp_typec_fabric_lock);

	/* again from here, should the edge handler not have run */
	dcp_hdmi_hold(dcp);

	if (READ_ONCE(dcp->active_typec_route)) {
		/*
		 * Until a compositor owns the display, a live HDMI output
		 * takes its pipeline back from a direct DP-alt route: the
		 * compositor pairs the HDMI connector with it first.  Without
		 * dual-stream docks it waits (see dcp_typec_hdmi_waits()).
		 */
		if (dcp_typec_keep_order() &&
		    gpiod_get_value_cansleep(dcp->hdmi_hpd)) {
			msleep(500);
			if (gpiod_get_value_cansleep(dcp->hdmi_hpd))
				dcp_typec_rebalance_locked(NULL, 0);
		} else if (!dcp_typec_dual_stream() &&
			   gpiod_get_value_cansleep(dcp->hdmi_hpd)) {
			dcp_typec_hdmi_waits(dcp);
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

	if (connected) {
		int ret = dcp_fixed_output_select(dcp);

		if (ret)
			dev_err(dcp->dev, "could not select the HDMI output: %d\n", ret);
		else
			dcp_dptx_connect(dcp, 0);
	}

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

/* Called after component unbind has drained all firmware callbacks. The
 * platform devices and their Type-C muxes can outlive this DRM instance.
 */
void dcp_unlink(struct drm_device *drm)
{
	struct apple_dcp_typec_port *port;
	struct drm_crtc *crtc;

	guard(mutex)(&dcp_typec_fabric_lock);

	list_for_each_entry(port, &dcp_typec_ports, link) {
		if (port->connector && port->connector->base.dev == drm)
			port->connector = NULL;
		if (port->secondary_connector &&
		    port->secondary_connector->base.dev == drm)
			port->secondary_connector = NULL;
	}

	drm_for_each_crtc(crtc, drm) {
		struct apple_crtc *apple_crtc = to_apple_crtc(crtc);
		struct apple_dcp *dcp;

		if (!apple_crtc->dcp)
			continue;
		dcp = platform_get_drvdata(apple_crtc->dcp);
		if (dcp->crtc != apple_crtc)
			continue;
		dcp->crtc = NULL;
		dcp->connector = NULL;
		dcp->fixed_connector = NULL;
		dcp->typec_connector = NULL;
		WRITE_ONCE(dcp->ext_backlight, false);
	}
}

bool dcp_has_typec_routes(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->nr_typec_routes;
}

void dcp_fabric_shutdown_dptx(struct apple_dcp *dcp)
{
	if (dcp->dptxep) {
		/* Mux/tunnel callbacks must stop using the service before its
		 * endpoint is released. Firmware callbacks are drained by AFK.
		 */
		guard(mutex)(&dcp_typec_fabric_lock);

		afk_shutdown(dcp->dptxep);
		dcp->dptxep = NULL;
		scoped_guard(mutex, &dcp->hpd_mutex) {
			for (int i = 0; i < ARRAY_SIZE(dcp->dptxport); i++) {
				dcp->dptxport[i].enabled = false;
				dcp->dptxport[i].connected = false;
				dcp->dptxport[i].service = NULL;
			}
		}
	}
}
