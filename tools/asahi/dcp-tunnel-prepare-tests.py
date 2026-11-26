#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify DP tunnel prepare runs pre-commit in activate and unwinds on failure.

Extracts the production dcp_tunnel_prepare(), dcp_typec_route_activate(),
dcp_tb_activate(), dcp_tb_rollback(), dcp_tb_claim(),
dcp_tb_split_join_locked(), dcp_tb_split_teardown_locked(),
dcp_tunnel_set_rate(), dcp_tb_finish_request(),
dcp_follow_activate(), and dcp_fabric_run_attach() from dcp-fabric.c,
dcp-fabric-effects.h, and dcp-fabric-session.h, compiles them with ASan/UBSan
against host mocks, and verifies:
- preselect + ATC DP tunnel open run inside dcp_typec_route_activate() before
  state publication and before dcp_tb_claim() / follow commit, using route->phy
  (never stale dcp->phy) and without holding dcp->tb_lock;
- preselect runs for mux_index == 0 as well as nonzero mux indices;
- partial failure (preselect succeeds, PHY open fails) unwinds crossbar
  preselect with MUX_IDLE_DISCONNECT;
- tunnel source select (T602X) is unwound with -1 if dcp_tunnel_prepare() fails;
- fixed_xbar is restored only if it was deselected during activation;
- both dcp_fabric_run_attach() (TB attach) and dcp_follow_activate() (follow/
  HDMI reclaim, including tiled split) roll back cleanly and return the error;
- direct DP-alt routes never invoke dcp_tunnel_prepare().
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FABRIC = 'drivers/gpu/drm/apple/dcp-fabric.c'
EFFECTS = 'drivers/gpu/drm/apple/dcp-fabric-effects.h'
SESSION = 'drivers/gpu/drm/apple/dcp-fabric-session.h'

MUTATIONS = {
    'ignore-preselect-fail': (
        '\t} else if (ret != -EOPNOTSUPP && ret != -ENOENT) {\n'
        '\t\tdev_warn(dcp->dev, "DP tunnel crossbar preselect failed: %d\\n", ret);\n'
        '\t\treturn ret;\n'
        '\t}',
        '\t} else if (ret != -EOPNOTSUPP && ret != -ENOENT) {\n'
        '\t\tdev_warn(dcp->dev, "DP tunnel crossbar preselect failed: %d\\n", ret);\n'
        '\t}',
    ),
    'fail-on-preselect-enoent': (
        '\t} else if (ret != -EOPNOTSUPP && ret != -ENOENT) {\n',
        '\t} else if (ret != -EOPNOTSUPP) {\n',
    ),
    'ignore-open-fail': (
        '\t\tif (preselected)\n'
        '\t\t\tdcp_dpxbar_preselect(xbar, MUX_IDLE_DISCONNECT);\n'
        '\t\treturn ret;\n'
        '\t}',
        '\t\tif (preselected)\n'
        '\t\t\tdcp_dpxbar_preselect(xbar, MUX_IDLE_DISCONNECT);\n'
        '\t}',
    ),
    'no-preselect-unwind': (
        '\t\tif (preselected)\n'
        '\t\t\tdcp_dpxbar_preselect(xbar, MUX_IDLE_DISCONNECT);\n',
        '\t\t(void)preselected;\n',
    ),
    'no-source-unwind': (
        '\t\t\tif (ret && source_selected)\n'
        '\t\t\t\tdcp_dpxbar_tunnel_select_source(xbar, -1);\n',
        '\t\t\t(void)source_selected;\n',
    ),
    'use-stale-dcp-phy': (
        '\tif (dcp_phy_clocks_tunnel(route->phy))\n'
        '\t\treturn 0;\n'
        '\topen = symbol_get(apple_atc_dp_tunnel_open);\n'
        '\tif (!open)\n'
        '\t\treturn 0;\n'
        '\tret = open(route->phy);',
        '\tif (dcp_phy_clocks_tunnel(dcp->phy))\n'
        '\t\treturn 0;\n'
        '\topen = symbol_get(apple_atc_dp_tunnel_open);\n'
        '\tif (!open)\n'
        '\t\treturn 0;\n'
        '\tret = open(dcp->phy);',
    ),
    'skip-mux0-prepare': (
        '\tif (xbar != route->xbar) {',
        '\tif (xbar != route->xbar && route->mux_index) {',
    ),
    'restore-unselected-fixed': (
        '\t\tif (fixed_deselected && dcp->fixed_xbar) {',
        '\t\tif ((fixed_deselected || true) && dcp->fixed_xbar) {',
    ),
    'split-teardown-stale-phy': (
        '\t\tif (dcp->split.phy &&\n'
        '\t\t    (dcp->split.ready || dcp->split.clock_held ||\n'
        '\t\t     dcp->split.clock_ok)) {\n'
        '\t\t\tif (dcp_phy_clocks_tunnel(dcp->split.phy)) {\n'
        '\t\t\t\tdcp_phy_tunnel_rate(dcp->split.phy, 1, 0);\n'
        '\t\t\t} else {\n'
        '\t\t\t\ttypeof(apple_atc_dp_tunnel_rate) *stop =\n'
        '\t\t\t\t\tsymbol_get(apple_atc_dp_tunnel_rate);\n\n'
        '\t\t\t\tif (stop) {\n'
        '\t\t\t\t\tstop(dcp->split.phy, 1, 0);\n'
        '\t\t\t\t\tsymbol_put(apple_atc_dp_tunnel_rate);\n'
        '\t\t\t\t}\n'
        '\t\t\t}\n'
        '\t\t}',
        '\t\tif (dcp->phy) {\n'
        '\t\t\tif (dcp_phy_clocks_tunnel(dcp->phy)) {\n'
        '\t\t\t\tdcp_phy_tunnel_rate(dcp->phy, 1, 0);\n'
        '\t\t\t} else {\n'
        '\t\t\t\ttypeof(apple_atc_dp_tunnel_rate) *stop =\n'
        '\t\t\t\t\tsymbol_get(apple_atc_dp_tunnel_rate);\n\n'
        '\t\t\t\tif (stop) {\n'
        '\t\t\t\t\tstop(dcp->phy, 1, 0);\n'
        '\t\t\t\t\tsymbol_put(apple_atc_dp_tunnel_rate);\n'
        '\t\t\t\t}\n'
        '\t\t\t}\n'
        '\t\t}',
    ),
    'split-teardown-ungated': (
        '\t\tif (dcp->split.phy &&\n'
        '\t\t    (dcp->split.ready || dcp->split.clock_held ||\n'
        '\t\t     dcp->split.clock_ok)) {',
        '\t\tif (dcp->split.phy) {',
    ),
    'split-teardown-ignore-clock-ok': (
        '\t\tif (dcp->split.phy &&\n'
        '\t\t    (dcp->split.ready || dcp->split.clock_held ||\n'
        '\t\t     dcp->split.clock_ok)) {',
        '\t\tif (dcp->split.phy &&\n'
        '\t\t    (dcp->split.ready || dcp->split.clock_held)) {',
    ),
    'split-teardown-ignore-clock-held': (
        '\t\tif (dcp->split.phy &&\n'
        '\t\t    (dcp->split.ready || dcp->split.clock_held ||\n'
        '\t\t     dcp->split.clock_ok)) {',
        '\t\tif (dcp->split.phy &&\n'
        '\t\t    (dcp->split.ready || dcp->split.clock_ok)) {',
    ),
    'split-clock-held-clear-on-err': (
        '\t\t\tret = dcp_route_clock_rate(route, phy, 1, link_rate);\n'
        '\t\t\tif (!ret)\n'
        '\t\t\t\tdcp->split.clock_held = !!link_rate;\n'
        '\t\t}\n'
        '\t\tdcp->split.clock_ok = !ret && link_rate;',
        '\t\t\tret = dcp_route_clock_rate(route, phy, 1, link_rate);\n'
        '\t\t}\n'
        '\t\tdcp->split.clock_held = !ret && link_rate;\n'
        '\t\tdcp->split.clock_ok = !ret && link_rate;',
    ),
    'split-clock-held-clear-on-rate-fail': (
        '\t\t\tret = dcp_route_clock_rate(route, phy, 1, link_rate);\n'
        '\t\t\tif (!ret)\n'
        '\t\t\t\tdcp->split.clock_held = !!link_rate;',
        '\t\t\tret = dcp_route_clock_rate(route, phy, 1, link_rate);\n'
        '\t\t\tdcp->split.clock_held = !ret && link_rate;',
    ),
    'split-clock-held-no-clear-on-zero': (
        '\t\t\tif (!ret)\n'
        '\t\t\t\tdcp->split.clock_held = !!link_rate;',
        '\t\t\tif (!ret && link_rate)\n'
        '\t\t\t\tdcp->split.clock_held = true;',
    ),
    'split-join-no-init-clock-held': (
        '\t\tdcp->split.clock_ok = false;\n'
        '\t\tdcp->split.clock_held = false;\n'
        '\t\tdcp->split.generation = generation;',
        '\t\tdcp->split.clock_ok = false;\n'
        '\t\tdcp->split.generation = generation;',
    ),
    'split-teardown-no-clear-clock-held': (
        '\t\tdcp->split.clock_ok = false;\n'
        '\t\tdcp->split.clock_held = false;\n'
        '\t\tdcp->split.ready = false;',
        '\t\tdcp->split.clock_ok = false;\n'
        '\t\tdcp->split.ready = false;',
    ),
}


def block(text, start):
    opening = text.index('{', start)
    depth = 0
    for end in range(opening, len(text)):
        depth += text[end] == '{'
        depth -= text[end] == '}'
        if not depth:
            return text[start:end + 1]
    raise AssertionError('unbalanced production block')


def function(text, name):
    match = re.search(
        r'\n((?:static\s+)?(?:inline\s+)?[^\n;{}]*\b' + name + r'\([^;{}]*\)\n)\{',
        text,
    )
    assert match, name
    start = match.start(1)
    if text.startswith(name + '(', start):
        start = text.rindex('\n', 0, start - 1) + 1
    return block(text, start) + '\n'


HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define lockdep_assert_held(l) ((void)(l))
#define dev_info(dev, ...) ((void)(dev))
#define dev_warn(dev, ...) ((void)(dev))
#define dev_err(dev, ...) ((void)(dev))

#define MUX_IDLE_DISCONNECT (-2)
#define DRM_MODE_CONNECTOR_USB 20
#define DCP_FIRMWARE_V_14_7 3
#define DCP_FIRMWARE_V_26_6_DESKTOP 5

static unsigned long jiffies = 1000;
static unsigned long msecs_to_jiffies(unsigned int m) { return m; }

struct mutex { int held; };
static struct mutex *mutex_lock_ret(struct mutex *m) { assert(!m->held); m->held = 1; return m; }
static void mutex_unlock_fn(struct mutex **m) { assert((*m)->held); (*m)->held = 0; }
#define guard(kind) struct mutex *_g __attribute__((cleanup(mutex_unlock_fn))) = mutex_lock_ret
#define scoped_guard(kind, lock) \
	for (struct mutex *_sg __attribute__((cleanup(mutex_unlock_fn))) = \
	     mutex_lock_ret(lock), *_once = (struct mutex *)1; \
	     _once; _once = NULL)

struct phy { const char *name; bool clocks_tunnel; };
struct mux_chip;
struct mux_control { struct mux_chip *chip; int state; bool selected; };
struct mux_chip { struct mux_control mux[5]; };

struct drm_encoder { u32 possible_crtcs; };
struct apple_connector {
	void *dcp;
	bool edid_live;
	struct drm_encoder *port_encoder;
	u32 candidate_crtcs;
};
struct drm_crtc { unsigned int index; };
struct apple_crtc { struct drm_crtc base; };
struct dptx_port { bool enabled; bool connected; void *service; };
struct dcp_split {
	bool active;
	bool ready;
	struct mux_control *xbar;
	struct phy *phy;
	int mux_state;
	bool xbar_up;
	bool clock_ok;
	bool clock_held;
	u64 generation;
	int (*set_active)(void *binding, bool active);
	void *binding;
};

struct apple_dcp_typec_route;
struct apple_dcp {
	void *dev;
	int fw_compat;
	struct mutex tb_lock;
	struct phy *phy;
	struct mux_control *xbar;
	struct mux_control *fixed_xbar;
	int fixed_mux_index;
	bool fixed_route_selected;
	unsigned int dptx_phy;
	int connector_type;
	bool ext_backlight;
	struct apple_connector *typec_connector;
	struct apple_connector *connector;
	struct apple_crtc *crtc;
	struct apple_dcp_typec_route *active_typec_route;
	struct mux_control *direct_xbar_up;
	bool dptx_tunnel;
	unsigned int dptx_dfp_port;
	bool tb_clock_ok;
	bool tb_clock_refused;
	int (*tb_dpin_set_active)(void *ctx, bool active);
	void *tb_dpin_ctx;
	u64 tb_generation;
	u64 typec_generation;
	bool typec_cable_connected;
	bool external;
	void *dptxep;
	struct dptx_port dptxport[2];
	struct apple_dcp *peer_owner;
	struct dcp_split split;
};

struct apple_dcp_typec_port {
	union {
		struct {
			struct apple_dcp_typec_route *owner;
			struct apple_dcp_typec_route *secondary_owner;
		};
		struct apple_dcp_typec_route *slots[2];
	};
	struct apple_dcp_typec_route *preferred_route;
	struct apple_dcp_typec_route *tb_last[2];
	struct apple_dcp *split_dcp;
	unsigned long tile_hint_deadline;
	bool dp_wanted;
	bool hpd;
};

struct apple_dcp_typec_route {
	struct apple_dcp *dcp;
	struct apple_dcp_typec_port *port;
	struct phy *phy;
	struct mux_control *xbar;
	struct mux_control *active_xbar;
	struct mux_control *dpin[4];
	unsigned int mux_index;
	unsigned int dptx_phy;
	unsigned int tunnel_dpin;
	u64 tunnel_generation;
	bool tunnel;
	bool xbar_up;
	bool selected;
	bool deactivate_xbar_done;
	int deactivate_error;
	bool sleep_parked;
};

struct dcp_tb_attach_context {
	u64 generation;
	struct apple_dcp_typec_port *port;
	struct apple_dcp_typec_route **slot;
	struct apple_dcp_typec_route *best;
	struct mux_control *ctl;
	unsigned int dpin;
	bool active;
	int (*set_active)(void *binding, bool active);
	void *binding;
};

struct dcp_typec_follow_slot {
	struct apple_dcp_typec_port *port;
	bool tunnel;
	unsigned int dpin;
	u64 tunnel_generation;
	int (*set_active)(void *binding, bool active);
	void *binding;
	bool split;
	u64 split_generation;
	int (*split_set_active)(void *binding, bool active);
	void *split_binding;
};

enum dcp_fabric_attach_action {
	DCP_FABRIC_ATTACH_OOB,
	DCP_FABRIC_ATTACH_WORK,
};

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

static enum dcp_fabric_attach_action dcp_fabric_attach_action(bool external)
{
	return external ? DCP_FABRIC_ATTACH_WORK : DCP_FABRIC_ATTACH_OOB;
}

enum ev_kind {
	EV_FIXED_DESELECT = 1,
	EV_FIXED_SELECT,
	EV_SOURCE_SELECT,
	EV_SOURCE_RESET,
	EV_PRESELECT,
	EV_PRESELECT_UNDO,
	EV_PHY_OPEN,
	EV_TRY_SELECT,
	EV_COMMIT_MODES,
	EV_ROLLBACK,
	EV_CLAIM,
	EV_CONNECT_OOB,
	EV_SPLIT_JOIN,
	EV_SPLIT_TEARDOWN,
};

static enum ev_kind trace[64];
static unsigned int trace_n;
static void record(enum ev_kind ev)
{
	if (trace_n < sizeof(trace) / sizeof(trace[0]))
		trace[trace_n++] = ev;
}

static struct apple_dcp *active_dcp;
static struct phy *stale_old_phy;
static struct mux_control *split_xbar_mux;
static int dcp_typec_fabric_lock;
static int mock_source_select_ret;
static int mock_preselect_ret;
static int mock_split_source_select_ret;
static int mock_split_preselect_ret;
static int mock_phy_open_ret;
static bool mock_phy_open_present;
static int mock_try_select_ret;
static int mock_split_disconnect_ret;
static int mock_clock_rate_ret;
static unsigned int source_select_calls, source_reset_calls;
static unsigned int split_source_select_calls, split_source_reset_calls;
static unsigned int preselect_calls, preselect_undo_calls;
static unsigned int split_preselect_calls, split_preselect_undo_calls;
static int last_preselect_state;
static unsigned int phy_open_calls;
static struct phy *phy_open_arg;
static unsigned int tunnel_rate_calls, old_phy_rate_calls;
static struct phy *last_rate_phy;
static unsigned int last_rate_dpin;
static u8 last_rate_val;
static unsigned int fixed_select_calls, fixed_deselect_calls;
static unsigned int connect_oob_calls, queue_reconnect_calls;

static unsigned int dcp_fabric_dpin_slot(unsigned int dpin) { return dpin & 1; }
static bool dcp_is_t8132_ext(const struct apple_dcp *dcp) { (void)dcp; return false; }
static bool dcp_typec_route_no_dp_alt(struct apple_dcp_typec_route *r) { (void)r; return false; }
static bool dcp_typec_narrows(struct apple_dcp *dcp) { (void)dcp; return false; }
static unsigned int drm_crtc_index(struct drm_crtc *c) { return c->index; }
static u32 dcp_fabric_connector_mask(bool d, bool b, bool r, unsigned int i, u32 c)
{ (void)d; (void)b; (void)r; (void)i; return c; }
static void *to_platform_device(void *dev) { return dev; }
static void dcp_tb_events_retire(struct apple_dcp *dcp) { (void)dcp; }
static void dcp_firmware_metadata_connection(struct apple_dcp *dcp, u64 g, bool b)
{ (void)dcp; (void)g; (void)b; }
static int dcp_dptx_disconnect(struct apple_dcp *dcp, unsigned int p)
{ (void)dcp; return p == 1 ? mock_split_disconnect_ret : 0; }
static bool dcp_t8132_late_release(struct apple_dcp *dcp, const char *what)
{ (void)dcp; (void)what; return false; }
static int apple_dp_tunnel_t8132_pclk(unsigned int src, unsigned int dpin)
{ (void)src; (void)dpin; return 0; }
static int dcp_t8132_crossbar_down_locked(struct apple_dcp *dcp,
					  struct apple_dcp_typec_route *r,
					  const char *why)
{ (void)dcp; (void)r; (void)why; return 0; }
static void t8132_ext_drm_hotplug(void *dev) { (void)dev; }
static bool of_machine_is_compatible(const char *compat)
{ (void)compat; return false; }
static int apple_dp_tunnel_t6030_pclk(unsigned int src, unsigned int dpin)
{ (void)src; (void)dpin; return 0; }
static int dcp_dpxbar_link(struct mux_control *mux, bool up)
{ (void)mux; (void)up; return 0; }
static struct apple_dcp_typec_route **
dcp_tb_slot(struct apple_dcp_typec_port *port, unsigned int dpin)
{ return &port->slots[dpin & 1]; }
static void dcp_tb_binding_retire(struct apple_dcp *dcp,
				  struct apple_dcp_typec_route *route)
{
	scoped_guard(mutex, &dcp->tb_lock) {
		dcp->tb_generation = 0;
		if (route)
			route->tunnel_generation = 0;
		dcp->tb_dpin_set_active = NULL;
		dcp->tb_dpin_ctx = NULL;
	}
}
static void dcp_peer_release_26_6(struct apple_dcp *dcp) { (void)dcp; }
static void dcp_modes_begin_attachment(struct apple_dcp *dcp)
{ (void)dcp; record(EV_COMMIT_MODES); }
static void apple_connector_edid_set_live(struct apple_connector *c, bool live)
{ if (c) c->edid_live = live; }
static void apple_connector_set_pipeline(struct apple_connector *c, void *pdev)
{ if (c) c->dcp = pdev; }
static struct apple_connector *
dcp_typec_route_connector(struct apple_dcp_typec_route *r, bool tunnel)
{ (void)r; (void)tunnel; return NULL; }
static struct mux_control *
dcp_typec_tunnel_ctl(struct apple_dcp_typec_route *r, unsigned int dpin)
{ return dpin < 4 ? r->dpin[dpin] : NULL; }

static bool dcp_phy_clocks_tunnel(struct phy *phy)
{
	return phy && phy->clocks_tunnel;
}

static int mux_control_deselect(struct mux_control *mux)
{
	assert(mux->selected);
	mux->selected = false;
	fixed_deselect_calls++;
	record(EV_FIXED_DESELECT);
	return 0;
}

static int mux_control_select(struct mux_control *mux, int state)
{
	assert(!mux->selected);
	mux->selected = true;
	mux->state = state;
	fixed_select_calls++;
	record(EV_FIXED_SELECT);
	return 0;
}

static int mux_control_try_select(struct mux_control *mux, int state)
{
	record(EV_TRY_SELECT);
	if (mock_try_select_ret)
		return mock_try_select_ret;
	mux->selected = true;
	mux->state = state;
	return 0;
}

static int dcp_dpxbar_tunnel_select_source(struct mux_control *mux, int state)
{
	if (mux == split_xbar_mux) {
		if (state < 0) {
			split_source_reset_calls++;
			return 0;
		}
		split_source_select_calls++;
		return mock_split_source_select_ret;
	}
	assert(!active_dcp->tb_lock.held);
	if (state < 0) {
		source_reset_calls++;
		record(EV_SOURCE_RESET);
		return 0;
	}
	source_select_calls++;
	record(EV_SOURCE_SELECT);
	return mock_source_select_ret;
}

static int dcp_dpxbar_preselect(struct mux_control *mux, int state)
{
	if (mux == split_xbar_mux) {
		if (state == MUX_IDLE_DISCONNECT) {
			split_preselect_undo_calls++;
			return 0;
		}
		split_preselect_calls++;
		return mock_split_preselect_ret;
	}
	assert(!active_dcp->tb_lock.held);
	if (state == MUX_IDLE_DISCONNECT) {
		preselect_undo_calls++;
		record(EV_PRESELECT_UNDO);
		return 0;
	}
	preselect_calls++;
	last_preselect_state = state;
	record(EV_PRESELECT);
	return mock_preselect_ret;
}

static int apple_atc_dp_tunnel_open(struct phy *phy)
{
	assert(!active_dcp->tb_lock.held);
	phy_open_calls++;
	phy_open_arg = phy;
	record(EV_PHY_OPEN);
	return mock_phy_open_ret;
}

static int record_tunnel_rate(struct phy *phy, unsigned int dpin, u8 rate)
{
	tunnel_rate_calls++;
	last_rate_phy = phy;
	last_rate_dpin = dpin;
	last_rate_val = rate;
	if (phy == stale_old_phy)
		old_phy_rate_calls++;
	return 0;
}

static int dcp_phy_tunnel_rate(struct phy *phy, unsigned int dpin, u8 rate)
{
	return record_tunnel_rate(phy, dpin, rate);
}

static int apple_atc_dp_tunnel_rate(struct phy *phy, unsigned int dpin, u8 rate)
{
	return record_tunnel_rate(phy, dpin, rate);
}

static int dcp_route_clock_rate(struct apple_dcp_typec_route *route,
				struct phy *phy, unsigned int dpin, u8 rate)
{
	(void)route;
	record_tunnel_rate(phy, dpin, rate);
	return mock_clock_rate_ret;
}

static int dptxport_set_hpd(void *service, bool hpd)
{
	(void)service;
	(void)hpd;
	return 0;
}

static bool mock_symbol_present(const void *fn)
{
	if (fn == (const void *)apple_atc_dp_tunnel_open)
		return mock_phy_open_present;
	return true;
}

#define symbol_get(fn) (mock_symbol_present((const void *)(fn)) ? (fn) : NULL)
#define symbol_put(fn) ((void)(fn))

static int dcp_tunnel_prepare(struct apple_dcp_typec_route *route,
			      struct mux_control *xbar);
'''

CHECKS = r'''
static int dummy_set_active(void *ctx, bool active)
{
	(void)ctx;
	(void)active;
	return 0;
}

static int dummy_candidate(void *data)
{
	(void)data;
	return 0;
}

static bool dummy_external(void *data)
{
	return ((struct dcp_tb_attach_context *)data)->best->dcp->external;
}

static bool dummy_has_connector(void *data)
{
	(void)data;
	return true;
}

static void dummy_queue_reconnect(void *data)
{
	(void)data;
	queue_reconnect_calls++;
}

static void dummy_connect_oob(void *data)
{
	(void)data;
	connect_oob_calls++;
	record(EV_CONNECT_OOB);
}

static void traced_rollback(void *data)
{
	record(EV_ROLLBACK);
	dcp_tb_rollback(data);
}

static void traced_claim(void *data)
{
	record(EV_CLAIM);
	dcp_tb_claim(data);
}

static const struct dcp_fabric_attach_ops test_attach_ops = {
	.candidate = dummy_candidate,
	.activate = dcp_tb_activate,
	.rollback = traced_rollback,
	.claim = traced_claim,
	.external = dummy_external,
	.has_connector = dummy_has_connector,
	.queue_reconnect = dummy_queue_reconnect,
	.connect_oob = dummy_connect_oob,
};

static int failures;
#define CHECK(c) do { if (!(c)) { \
	printf("FAIL line %d (%s: %s)\n", __LINE__, case_name, #c); \
	failures++; } } while (0)

struct fixture {
	struct mux_chip chip;
	struct mux_control fixed_xbar;
	struct phy old_phy;
	struct phy route_phy;
	struct apple_dcp dcp;
	struct apple_dcp_typec_port port;
	struct apple_dcp_typec_route route;
	struct apple_dcp_typec_route *slot;
	int dev_token;
	int binding_token;
};

static void setup(struct fixture *f, unsigned int mux_index, bool fixed_selected)
{
	memset(f, 0, sizeof(*f));
	for (unsigned int i = 0; i < 5; i++)
		f->chip.mux[i].chip = &f->chip;
	f->old_phy.name = "old-fixed-phy";
	f->route_phy.name = "tunnel-atc-phy";
	f->fixed_xbar.selected = fixed_selected;
	f->dcp.dev = &f->dev_token;
	f->dcp.phy = &f->old_phy;
	f->dcp.fixed_xbar = &f->fixed_xbar;
	f->dcp.fixed_mux_index = 4;
	f->dcp.fixed_route_selected = fixed_selected;
	f->route.dcp = &f->dcp;
	f->route.port = &f->port;
	f->route.phy = &f->route_phy;
	f->route.xbar = &f->chip.mux[0];
	f->route.dpin[0] = &f->chip.mux[1];
	f->route.dpin[1] = &f->chip.mux[2];
	f->route.mux_index = mux_index;
	f->route.dptx_phy = 1;

	active_dcp = &f->dcp;
	stale_old_phy = &f->old_phy;
	split_xbar_mux = f->route.dpin[1];
	trace_n = 0;
	mock_source_select_ret = -EOPNOTSUPP;
	mock_preselect_ret = 0;
	mock_split_source_select_ret = -EOPNOTSUPP;
	mock_split_preselect_ret = 0;
	mock_phy_open_ret = 0;
	mock_phy_open_present = true;
	mock_try_select_ret = 0;
	mock_split_disconnect_ret = 0;
	mock_clock_rate_ret = 0;
	source_select_calls = source_reset_calls = 0;
	split_source_select_calls = split_source_reset_calls = 0;
	preselect_calls = preselect_undo_calls = 0;
	split_preselect_calls = split_preselect_undo_calls = 0;
	last_preselect_state = -99;
	phy_open_calls = 0;
	phy_open_arg = NULL;
	tunnel_rate_calls = old_phy_rate_calls = 0;
	last_rate_phy = NULL;
	last_rate_dpin = 99;
	last_rate_val = 99;
	fixed_select_calls = fixed_deselect_calls = 0;
	connect_oob_calls = queue_reconnect_calls = 0;
}

static struct dcp_tb_attach_context make_tb_ctx(struct fixture *f)
{
	return (struct dcp_tb_attach_context){
		.generation = 42,
		.port = &f->port,
		.slot = &f->slot,
		.best = &f->route,
		.ctl = f->route.dpin[0],
		.dpin = 0,
		.set_active = dummy_set_active,
		.binding = &f->binding_token,
	};
}

int main(void)
{
	const char *case_name;
	struct fixture f;
	struct dcp_tb_attach_context ctx;
	int ret;

	/* 1. T600x attach succeeds: preselect + PHY open run pre-commit on route->phy. */
	case_name = "tb-attach-ok-t600x";
	setup(&f, 2, true);
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == 0);
	CHECK(preselect_calls == 1 && last_preselect_state == 2 && !preselect_undo_calls);
	CHECK(phy_open_calls == 1 && phy_open_arg == &f.route_phy);
	CHECK(f.slot == &f.route && f.dcp.active_typec_route == &f.route);
	CHECK(f.route.selected && f.route.tunnel && f.dcp.dptx_tunnel);
	CHECK(f.dcp.phy == &f.route_phy && f.port.hpd && f.dcp.typec_cable_connected);
	CHECK(connect_oob_calls == 1 && !f.dcp.fixed_route_selected);
	CHECK(trace_n == 7 &&
	      trace[0] == EV_FIXED_DESELECT &&
	      trace[1] == EV_SOURCE_SELECT &&
	      trace[2] == EV_PRESELECT &&
	      trace[3] == EV_PHY_OPEN &&
	      trace[4] == EV_COMMIT_MODES &&
	      trace[5] == EV_CLAIM &&
	      trace[6] == EV_CONNECT_OOB);

	/* 2. mux_index == 0 still runs preselect(0) and PHY open. */
	case_name = "tb-attach-ok-mux0";
	setup(&f, 0, false);
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == 0);
	CHECK(!source_select_calls && preselect_calls == 1 && last_preselect_state == 0);
	CHECK(phy_open_calls == 1 && phy_open_arg == &f.route_phy);

	/* 3. T602x/T6030: preselect & open return -EOPNOTSUPP -> treated as success. */
	case_name = "tb-attach-ok-eopnotsupp";
	setup(&f, 2, false);
	mock_source_select_ret = 0;
	mock_preselect_ret = -EOPNOTSUPP;
	mock_phy_open_ret = -EOPNOTSUPP;
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == 0 && source_select_calls == 1 && !source_reset_calls);
	CHECK(preselect_calls == 1 && !preselect_undo_calls && phy_open_calls == 1);
	CHECK(f.slot == &f.route && f.route.selected);

	/* 4. T6040/T6041: PHY clocks tunnel itself -> skips apple_atc_dp_tunnel_open. */
	case_name = "tb-attach-ok-phy-clocks-tunnel";
	setup(&f, 2, false);
	f.route_phy.clocks_tunnel = true;
	mock_preselect_ret = -EOPNOTSUPP;
	mock_phy_open_ret = -EINVAL;
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == 0 && phy_open_calls == 0 && f.slot == &f.route);

	/* 5. T6050: no apple_atc_dp_tunnel_open symbol -> succeeds cleanly. */
	case_name = "tb-attach-ok-no-open-symbol";
	setup(&f, 2, false);
	mock_preselect_ret = -EOPNOTSUPP;
	mock_phy_open_present = false;
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == 0 && phy_open_calls == 0 && f.slot == &f.route);

	/* 5b. Absent apple_dpxbar_preselect symbol (-ENOENT) -> non-fatal, continues to open. */
	case_name = "tb-attach-ok-no-preselect-symbol";
	setup(&f, 2, false);
	mock_preselect_ret = -ENOENT;
	mock_phy_open_ret = 0;
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == 0 && preselect_calls == 1 && !preselect_undo_calls &&
	      phy_open_calls == 1 && f.slot == &f.route && f.route.selected);

	/* 5c. Absent preselect symbol (-ENOENT) + PHY open fail -> no preselect undo. */
	case_name = "tb-attach-no-preselect-symbol-open-fail";
	setup(&f, 2, false);
	mock_preselect_ret = -ENOENT;
	mock_phy_open_ret = -EINVAL;
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == -EINVAL && preselect_calls == 1 && preselect_undo_calls == 0 &&
	      phy_open_calls == 1 && !f.slot && !f.route.selected);

	/* 6. Crossbar preselect fails -> rollback, fixed_xbar restored, no claim/connect. */
	case_name = "tb-attach-preselect-fail";
	setup(&f, 2, true);
	mock_source_select_ret = 0;
	mock_preselect_ret = -EBUSY;
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == -EBUSY);
	CHECK(source_select_calls == 1 && source_reset_calls == 1);
	CHECK(preselect_calls == 1 && preselect_undo_calls == 0 && phy_open_calls == 0);
	CHECK(fixed_deselect_calls == 1 && fixed_select_calls == 1 && f.dcp.fixed_route_selected);
	CHECK(!f.slot && !f.dcp.active_typec_route && !f.route.selected && !f.dcp.dptx_tunnel);
	CHECK(!f.dcp.tb_generation && !f.route.tunnel_generation && !f.dcp.tb_dpin_set_active);
	CHECK(f.dcp.phy == &f.old_phy && !f.port.hpd && !connect_oob_calls);

	/* 7. Preselect fails when fixed_xbar was NOT selected -> fixed_xbar stays unselected. */
	case_name = "tb-attach-preselect-fail-unselected-fixed";
	setup(&f, 2, false);
	mock_preselect_ret = -EIO;
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == -EIO);
	CHECK(!fixed_deselect_calls && !fixed_select_calls && !f.dcp.fixed_route_selected &&
	      !f.fixed_xbar.selected);

	/* 8. Partial failure: preselect succeeds, PHY open fails -> preselect unwound & rollback. */
	case_name = "tb-attach-phy-open-fail";
	setup(&f, 2, true);
	mock_preselect_ret = 0;
	mock_phy_open_ret = -EBUSY;
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == -EBUSY);
	CHECK(preselect_calls == 1 && preselect_undo_calls == 1 && phy_open_calls == 1);
	CHECK(phy_open_arg == &f.route_phy);
	CHECK(fixed_select_calls == 1 && f.dcp.fixed_route_selected);
	CHECK(!f.slot && !f.dcp.active_typec_route && !f.route.selected && !f.dcp.dptx_tunnel);
	CHECK(!f.dcp.tb_generation && !f.route.tunnel_generation && !f.dcp.tb_dpin_set_active);
	CHECK(!connect_oob_calls);

	/* 9. T602X source select + PHY open failure -> source reset, no preselect undo. */
	case_name = "tb-attach-source-and-open-fail";
	setup(&f, 2, false);
	mock_source_select_ret = 0;
	mock_preselect_ret = -EOPNOTSUPP;
	mock_phy_open_ret = -EINVAL;
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == -EINVAL);
	CHECK(source_select_calls == 1 && source_reset_calls == 1);
	CHECK(preselect_calls == 1 && preselect_undo_calls == 0);
	CHECK(!f.slot && !f.route.selected && !f.dcp.tb_generation);

	/*
	 * 10. Follow-activate with tunnel + tiled split: PHY open failure rolls
	 * back everything without stopping a clock on stale dcp->phy (or route->phy).
	 */
	for (unsigned int clocks = 0; clocks < 2; clocks++) {
		case_name = clocks ? "follow-activate-open-fail-phy-clock" :
				     "follow-activate-open-fail";
		setup(&f, 2, true);
		f.old_phy.clocks_tunnel = !!clocks;
		mock_preselect_ret = 0;
		mock_phy_open_ret = -EBUSY;
		{
			struct dcp_typec_follow_slot slot = {
				.port = &f.port,
				.tunnel = true,
				.dpin = 0,
				.tunnel_generation = 77,
				.set_active = dummy_set_active,
				.binding = &f.binding_token,
				.split = true,
				.split_generation = 78,
				.split_set_active = dummy_set_active,
				.split_binding = &f.binding_token,
			};

			ret = dcp_follow_activate(&f.route, &slot);
			CHECK(ret == -EBUSY);
			CHECK(preselect_calls == 1 && preselect_undo_calls == 1);
			CHECK(split_preselect_calls == 1 && split_preselect_undo_calls == 1 &&
			      split_source_reset_calls == 1);
			CHECK(!f.port.split_dcp && !f.dcp.split.active && !f.dcp.split.ready &&
			      !f.dcp.split.phy && !f.dcp.split.xbar);
			CHECK(tunnel_rate_calls == 0 && old_phy_rate_calls == 0);
			CHECK(!f.dcp.tb_generation && !f.route.tunnel_generation &&
			      !f.dcp.tb_dpin_set_active);
			CHECK(!f.port.owner && !f.port.preferred_route && !f.dcp.active_typec_route);
			CHECK(f.dcp.phy == &f.old_phy && f.dcp.fixed_route_selected);
		}
	}

	/* 10b. Follow-activate preselect failure also never touches stale dcp->phy. */
	for (unsigned int clocks = 0; clocks < 2; clocks++) {
		case_name = clocks ? "follow-activate-preselect-fail-phy-clock" :
				     "follow-activate-preselect-fail";
		setup(&f, 2, true);
		f.old_phy.clocks_tunnel = !!clocks;
		mock_preselect_ret = -EBUSY;
		{
			struct dcp_typec_follow_slot slot = {
				.port = &f.port,
				.tunnel = true,
				.dpin = 0,
				.tunnel_generation = 79,
				.set_active = dummy_set_active,
				.binding = &f.binding_token,
				.split = true,
				.split_generation = 80,
				.split_set_active = dummy_set_active,
				.split_binding = &f.binding_token,
			};

			ret = dcp_follow_activate(&f.route, &slot);
			CHECK(ret == -EBUSY);
			CHECK(preselect_calls == 1 && preselect_undo_calls == 0 &&
			      phy_open_calls == 0);
			CHECK(split_preselect_calls == 1 && split_preselect_undo_calls == 1 &&
			      split_source_reset_calls == 1);
			CHECK(!f.port.split_dcp && !f.dcp.split.active && !f.dcp.split.ready &&
			      !f.dcp.split.phy && !f.dcp.split.xbar);
			CHECK(tunnel_rate_calls == 0 && old_phy_rate_calls == 0);
			CHECK(f.dcp.phy == &f.old_phy && f.dcp.fixed_route_selected);
		}
	}

	/* 11. Follow-activate succeeds; later live teardown stops dpin1 clock on route->phy. */
	case_name = "follow-activate-ok-and-teardown";
	setup(&f, 2, false);
	{
		struct dcp_typec_follow_slot slot = {
			.port = &f.port,
			.tunnel = true,
			.dpin = 0,
			.tunnel_generation = 88,
			.set_active = dummy_set_active,
			.binding = &f.binding_token,
			.split = true,
			.split_generation = 89,
			.split_set_active = dummy_set_active,
			.split_binding = &f.binding_token,
		};

		ret = dcp_follow_activate(&f.route, &slot);
		CHECK(ret == 0);
		CHECK(preselect_calls == 1 && phy_open_calls == 1 && !split_preselect_undo_calls);
		CHECK(tunnel_rate_calls == 0 && f.port.split_dcp == &f.dcp &&
		      f.dcp.split.phy == &f.route_phy);
		CHECK(f.port.owner == &f.route && f.port.preferred_route == &f.route);
		CHECK(f.dcp.active_typec_route == &f.route && f.route.selected && f.dcp.split.ready);

		/* Live teardown stops dpin1's clock on split.phy, never stale old_phy. */
		f.dcp.phy = &f.old_phy;
		ret = dcp_tb_split_teardown_locked(&f.port, false);
		CHECK(ret == 0 && tunnel_rate_calls == 1 && old_phy_rate_calls == 0);
		CHECK(last_rate_phy == &f.route_phy && last_rate_dpin == 1 && last_rate_val == 0);
		CHECK(!f.port.split_dcp && !f.dcp.split.phy && !f.dcp.split.active &&
		      !f.dcp.split.ready);
	}

	/* 11b. Split teardown with clock_ok && !ready (connect failed after SetLinkRate). */
	case_name = "split-teardown-clock-ok-not-ready";
	setup(&f, 2, false);
	ret = dcp_tb_split_join_locked(&f.port, &f.route, f.route.dpin[1], 91,
				       dummy_set_active, &f.binding_token);
	CHECK(ret == 0 && f.dcp.split.phy == &f.route_phy && !f.dcp.split.ready &&
	      !f.dcp.split.clock_held);
	f.dcp.split.clock_ok = true;
	ret = dcp_tb_split_teardown_locked(&f.port, false);
	CHECK(ret == 0 && tunnel_rate_calls == 1 && old_phy_rate_calls == 0);
	CHECK(last_rate_phy == &f.route_phy && last_rate_dpin == 1 && last_rate_val == 0);
	CHECK(!f.port.split_dcp && !f.dcp.split.phy && !f.dcp.split.clock_ok &&
	      !f.dcp.split.clock_held);

	/*
	 * 11c. Proof Chain A: retired split (failed detach -> dcp_tb_finish_request()
	 * clears generation and ready) -> SetLinkRate(1, 0) returns -ENODEV without
	 * stopping the clock -> retry teardown must still stop dpin1 clock on split.phy.
	 */
	for (unsigned int clocks = 0; clocks < 2; clocks++) {
		case_name = clocks ? "split-retired-enodev-stops-clock-phy" :
				     "split-retired-enodev-stops-clock";
		setup(&f, 2, false);
		f.route_phy.clocks_tunnel = !!clocks;
		ctx = make_tb_ctx(&f);
		ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
		CHECK(ret == 0 && f.dcp.dptx_tunnel && f.dcp.active_typec_route == &f.route);
		ret = dcp_tb_split_join_locked(&f.port, &f.route, f.route.dpin[1], 92,
					       dummy_set_active, &f.binding_token);
		CHECK(ret == 0 && f.dcp.split.phy == &f.route_phy &&
		      !f.dcp.split.clock_ok && !f.dcp.split.clock_held);
		ret = dcp_tunnel_set_rate(&f.dcp, &f.route_phy, 1, 0x1e);
		CHECK(ret == 0 && f.dcp.split.clock_ok && f.dcp.split.clock_held);
		CHECK(tunnel_rate_calls == 1 && last_rate_phy == &f.route_phy &&
		      last_rate_dpin == 1 && last_rate_val == 0x1e);
		f.dcp.split.ready = true;

		/* First detach fails before teardown clears state; finish_request retires it. */
		mock_split_disconnect_ret = -ETIMEDOUT;
		ret = dcp_tb_split_teardown_locked(&f.port, false);
		CHECK(ret == -ETIMEDOUT && f.port.split_dcp == &f.dcp &&
		      f.dcp.split.clock_held && tunnel_rate_calls == 1);
		{
			struct dcp_tb_attach_context detach_req = {
				.generation = 92,
				.dpin = 1,
				.active = false,
				.set_active = dummy_set_active,
				.binding = &f.binding_token,
			};

			dcp_tb_finish_request(&f.port, &detach_req);
		}
		CHECK(!f.dcp.split.generation && !f.dcp.split.ready &&
		      !f.dcp.split.set_active && !f.dcp.split.binding &&
		      f.dcp.split.clock_held);

		/* Subsequent SetLinkRate(1, 0) hits -ENODEV without calling clock_rate. */
		ret = dcp_tunnel_set_rate(&f.dcp, &f.route_phy, 1, 0);
		CHECK(ret == -ENODEV && tunnel_rate_calls == 1);
		CHECK(!f.dcp.split.clock_ok && f.dcp.split.clock_held);

		/* Retry teardown stops dpin1 clock on split.phy, never stale dcp->phy. */
		f.dcp.phy = &f.old_phy;
		mock_split_disconnect_ret = 0;
		ret = dcp_tb_split_teardown_locked(&f.port, false);
		CHECK(ret == 0 && tunnel_rate_calls == 2 && old_phy_rate_calls == 0);
		CHECK(last_rate_phy == &f.route_phy && last_rate_dpin == 1 &&
		      last_rate_val == 0);
		CHECK(!f.port.split_dcp && !f.dcp.split.phy && !f.dcp.split.active &&
		      !f.dcp.split.ready && !f.dcp.split.clock_ok && !f.dcp.split.clock_held);
	}

	/*
	 * 11d. Proof Chain B: pre-ready SetLinkRate(1, rate1) succeeds, then
	 * SetLinkRate(1, rate2) fails without stopping clock -> teardown must still
	 * stop dpin1 clock on split.phy.
	 */
	for (unsigned int clocks = 0; clocks < 2; clocks++) {
		case_name = clocks ? "split-preready-rate-fail-stops-clock-phy" :
				     "split-preready-rate-fail-stops-clock";
		setup(&f, 2, false);
		f.route_phy.clocks_tunnel = !!clocks;
		ctx = make_tb_ctx(&f);
		ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
		CHECK(ret == 0);
		ret = dcp_tb_split_join_locked(&f.port, &f.route, f.route.dpin[1], 93,
					       dummy_set_active, &f.binding_token);
		CHECK(ret == 0 && !f.dcp.split.ready && !f.dcp.split.clock_held);

		ret = dcp_tunnel_set_rate(&f.dcp, &f.route_phy, 1, 0x14);
		CHECK(ret == 0 && f.dcp.split.clock_ok && f.dcp.split.clock_held &&
		      !f.dcp.split.ready && tunnel_rate_calls == 1);

		mock_clock_rate_ret = -EBUSY;
		ret = dcp_tunnel_set_rate(&f.dcp, &f.route_phy, 1, 0x1e);
		CHECK(ret == -EBUSY && !f.dcp.split.clock_ok && f.dcp.split.clock_held &&
		      !f.dcp.split.ready && tunnel_rate_calls == 2);

		mock_clock_rate_ret = 0;
		f.dcp.phy = &f.old_phy;
		ret = dcp_tb_split_teardown_locked(&f.port, false);
		CHECK(ret == 0 && tunnel_rate_calls == 3 && old_phy_rate_calls == 0);
		CHECK(last_rate_phy == &f.route_phy && last_rate_dpin == 1 &&
		      last_rate_val == 0);
		CHECK(!f.port.split_dcp && !f.dcp.split.phy && !f.dcp.split.active &&
		      !f.dcp.split.ready && !f.dcp.split.clock_ok && !f.dcp.split.clock_held);
	}

	/*
	 * 11e. Pre-ready clock_held edge cases: clean SetLinkRate(1, 0) clears clock_held,
	 * failed SetLinkRate(1, 0) keeps it held, and failed initial start keeps it clear.
	 */
	case_name = "split-preready-rate-zero-clears-clock-held";
	setup(&f, 2, false);
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == 0);
	ret = dcp_tb_split_join_locked(&f.port, &f.route, f.route.dpin[1], 94,
				       dummy_set_active, &f.binding_token);
	CHECK(ret == 0);
	ret = dcp_tunnel_set_rate(&f.dcp, &f.route_phy, 1, 0x14);
	CHECK(ret == 0 && f.dcp.split.clock_held && tunnel_rate_calls == 1);
	ret = dcp_tunnel_set_rate(&f.dcp, &f.route_phy, 1, 0);
	CHECK(ret == 0 && !f.dcp.split.clock_ok && !f.dcp.split.clock_held &&
	      tunnel_rate_calls == 2);
	ret = dcp_tb_split_teardown_locked(&f.port, false);
	CHECK(ret == 0 && tunnel_rate_calls == 2 && old_phy_rate_calls == 0);

	case_name = "split-preready-rate-zero-fail-keeps-clock-held";
	setup(&f, 2, false);
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == 0);
	ret = dcp_tb_split_join_locked(&f.port, &f.route, f.route.dpin[1], 95,
				       dummy_set_active, &f.binding_token);
	CHECK(ret == 0);
	ret = dcp_tunnel_set_rate(&f.dcp, &f.route_phy, 1, 0x14);
	CHECK(ret == 0 && f.dcp.split.clock_held && tunnel_rate_calls == 1);
	mock_clock_rate_ret = -EIO;
	ret = dcp_tunnel_set_rate(&f.dcp, &f.route_phy, 1, 0);
	CHECK(ret == -EIO && !f.dcp.split.clock_ok && f.dcp.split.clock_held &&
	      tunnel_rate_calls == 2);
	mock_clock_rate_ret = 0;
	ret = dcp_tb_split_teardown_locked(&f.port, false);
	CHECK(ret == 0 && tunnel_rate_calls == 3 && last_rate_phy == &f.route_phy &&
	      last_rate_dpin == 1 && last_rate_val == 0 && !f.dcp.split.clock_held);

	case_name = "split-preready-initial-start-fail-keeps-clock-unheld";
	setup(&f, 2, false);
	ctx = make_tb_ctx(&f);
	ret = dcp_fabric_run_attach(&test_attach_ops, &ctx);
	CHECK(ret == 0);
	ret = dcp_tb_split_join_locked(&f.port, &f.route, f.route.dpin[1], 96,
				       dummy_set_active, &f.binding_token);
	CHECK(ret == 0);
	mock_clock_rate_ret = -EIO;
	ret = dcp_tunnel_set_rate(&f.dcp, &f.route_phy, 1, 0x14);
	CHECK(ret == -EIO && !f.dcp.split.clock_ok && !f.dcp.split.clock_held &&
	      tunnel_rate_calls == 1);
	mock_clock_rate_ret = 0;
	ret = dcp_tb_split_teardown_locked(&f.port, false);
	CHECK(ret == 0 && tunnel_rate_calls == 1 && old_phy_rate_calls == 0);

	/* 11f. dcp_tb_split_join_locked() clears stale clock_held before pre-commit teardown. */
	case_name = "split-join-resets-stale-clock-held";
	setup(&f, 2, false);
	f.dcp.split.clock_held = true;
	ret = dcp_tb_split_join_locked(&f.port, &f.route, f.route.dpin[1], 97,
				       dummy_set_active, &f.binding_token);
	CHECK(ret == 0 && !f.dcp.split.clock_held && !f.dcp.split.ready &&
	      !f.dcp.split.clock_ok);
	ret = dcp_tb_split_teardown_locked(&f.port, false);
	CHECK(ret == 0 && tunnel_rate_calls == 0 && old_phy_rate_calls == 0);

	/* 12. Direct DP-alt route never invokes tunnel prepare. */
	case_name = "direct-route-unaffected";
	setup(&f, 2, false);
	ret = dcp_typec_route_activate(&f.route, f.route.xbar);
	CHECK(ret == 0 && !preselect_calls && !phy_open_calls && !source_select_calls);
	CHECK(f.route.selected && !f.route.tunnel && f.route.xbar_up);

	if (failures)
		return 1;
	puts("PASS DP tunnel prepare: pre-commit prepare, partial unwind, TB attach & follow/split rollback");
	return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = parser.parse_args()

    fabric = (ROOT / FABRIC).read_text()
    effects = (ROOT / EFFECTS).read_text()
    session = (ROOT / SESSION).read_text()

    if args.mutation:
        old, new = MUTATIONS[args.mutation]
        assert fabric.count(old) == 1, args.mutation
        fabric = fabric.replace(old, new)

    parts = [
        function(session, 'dcp_fabric_callback_valid'),
        function(session, 'dcp_fabric_clock_refused'),
        function(effects, 'dcp_fabric_run_attach'),
        function(fabric, 'dcp_typec_route_activate'),
        function(fabric, 'dcp_tunnel_prepare'),
        function(fabric, 'dcp_tunnel_set_rate'),
        function(fabric, 'dcp_tb_activate'),
        function(fabric, 'dcp_tb_rollback'),
        function(fabric, 'dcp_tb_claim'),
        function(fabric, 'dcp_tb_split_teardown_locked'),
        function(fabric, 'dcp_tb_split_join_locked'),
        function(fabric, 'dcp_tb_finish_request'),
        function(fabric, 'dcp_follow_activate'),
    ]

    code = HARNESS + '\n'.join(parts) + CHECKS
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'check.c'
        path.write_text(code)
        subprocess.run(
            ['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-unused-parameter',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             str(path), '-o', str(path.with_suffix(''))],
            check=True,
        )
        sys.exit(subprocess.run([str(path.with_suffix(''))]).returncode)


if __name__ == '__main__':
    main()
