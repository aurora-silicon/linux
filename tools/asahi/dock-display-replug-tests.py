#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Replay a dock's displays going and coming back on a firmware-27.2 T8132 dcpext.

The display side of a two-display dock's unplug and replug is compiled from
the production bodies against host mocks:

* the swap watchdog of a pipeline with Type-C displays only
  (dcp_swap_watched(), dcp_swap_watchdog_arm(), dcp_swap_watchdog_keep(),
  iomfb_present_in_flight()): a present accepted for a display that went,
  before or after its route is released and across the CRTC's power-off, is
  retired instead of holding the IOMFB queue until its timeout declares the
  processor crashed;
* the display release of a T8132 dcpext (dcp_dptx_release_locked()): a
  release the firmware-27.2 dcpext does not answer is taken as done, as on
  the M4 backend, so the replug's tunnel is not refused on the old route.

Pipelines with a fixed output, the internal panel, firmware without the IOMFB
queue and other processors' releases keep what they had. Each negative
control puts one piece back as it was before; the replay must then fail.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DCP = (ROOT / 'drivers/gpu/drm/apple/dcp.c').read_text()
IOMFB = (ROOT / 'drivers/gpu/drm/apple/iomfb.c').read_text()
FABRIC = (ROOT / 'drivers/gpu/drm/apple/dcp-fabric.c').read_text()

SOURCES = {'dcp': DCP, 'iomfb': IOMFB, 'fabric': FABRIC}
FUNCTIONS = [
    ('fabric', 'dcp_is_typec_output'), ('fabric', 'dcp_t8132_ext_m4_node'),
    ('fabric', 'dcp_is_t8132_ext'), ('fabric', 'dcp_t8132_ext_hw'),
    ('iomfb', 'iomfb_uses_queue'), ('iomfb', 'iomfb_present_in_flight'),
    ('dcp', 'dcp_swap_watched'), ('dcp', 'dcp_swap_watchdog_arm'),
    ('dcp', 'dcp_swap_watchdog_keep'), ('dcp', 'dcp_dptx_release_locked'),
]

# name -> (source, old, new): each puts one piece back as it was.
MUTATIONS = {
    'unwatched-routeless-pipe': ('dcp', '\t       (dcp->nr_typec_routes && !dcp->fixed_connector) ||\n', ''),
    'no-keep': ('dcp', 'if (!iomfb_present_in_flight(dcp) || delayed_work_pending(&dcp->swap_watchdog_wq))\n\t\treturn;',
                'return;'),
    'm4-only-release': ('dcp', 'bool t8132 = dcp_t8132_ext_hw(dcp);\n\tint ret;\n\n\tlockdep_assert_held(&dcp->hpd_mutex);\n\tif (dcp->external) {',
                        'bool t8132 = dcp_is_t8132_ext(dcp);\n\tint ret;\n\n\tlockdep_assert_held(&dcp->hpd_mutex);\n\tif (dcp->external) {'),
}


def function(text, name):
    match = re.search(r'^(?:VISIBLE_IF_KUNIT\s+)?(?:static\s+)?(?:inline\s+)?'
                      r'(?:bool|int|void|enum tb_dp_relink)\s+' + re.escape(name) +
                      r'\([^;{]*?\)\n\{', text, re.M | re.S)
    assert match, name
    pos = text.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return re.sub(r'^VISIBLE_IF_KUNIT\s+', '', text[match.start():pos]) + '\n'


PRE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define lockdep_assert_held(l) ((void)(l))
#define dev_info(dev, ...) ((void)(dev), infos++)
#define dev_warn(dev, ...) ((void)(dev), warns++)
static unsigned int infos, warns;
/* jiffies in ms */
static unsigned long jiffies;
static unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }

/* Display */
enum { DCP_FIRMWARE_V_26_6 = 26, DCP_FIRMWARE_V_26_6_DESKTOP = 260, DCP_FIRMWARE_H17P = 17,
       DCP_FIRMWARE_V_27_0 = 27 };
enum { DCP_IOMFB_METHODS_DEFAULT, DCP_IOMFB_METHODS_H17G };
enum { DRM_MODE_CONNECTOR_USB = 32, DRM_MODE_CONNECTOR_HDMIA = 11 };
struct device_node { const char *compatible; bool v27_2; };
struct device { struct device_node *of_node; };
struct delayed_work { bool pending; unsigned long due; unsigned int arms; };
struct apple_epic_service { int unused; };
struct dptx_port { bool enabled, connected; struct apple_epic_service *service; };
struct apple_dcp {
    struct device *dev;
    int fw_compat, hpd_mutex;
    struct { int iomfb_method_profile; } hw;
    void *active_typec_route, *fixed_connector;
    int fixed_connector_type;
    u32 nr_typec_routes;
    bool external, external_native, t8132_release_late, external_link_ready;
    struct { bool pending, accepted; u32 swap_id; } present_state_h17p;
    unsigned long swap_watchdog_deadline;
    struct delayed_work swap_watchdog_wq;
    struct dptx_port dptxport[2];
};
static bool of_device_is_compatible(const struct device_node *n, const char *c)
{
    return n && !strcmp(n->compatible, c);
}
static bool apple_t8132_dcpext_v27_2(const struct device_node *n)
{
    return of_device_is_compatible(n, "apple,t8132-dcpext") && n->v27_2;
}
/* Desktop 26.x watches its own; not these pipelines. */
static bool dcp_desktop_swap_watched(struct apple_dcp *dcp) { (void)dcp; return false; }
static void *system_dfl_wq;
static bool mod_delayed_work(void *wq, struct delayed_work *w, unsigned long delay)
{
    bool was = w->pending;
    (void)wq; w->pending = true; w->due = jiffies + delay; w->arms++; return was;
}
static bool delayed_work_pending(struct delayed_work *w) { return w->pending; }
static void cancel_delayed_work(struct delayed_work *w) { w->pending = false; }

static int release_ret, releases, link_gones, invalidates, crossbar_downs;
static bool dptx_service_available(struct dptx_port *p) { return p->enabled; }
static int dptxport_release_display(struct apple_epic_service *s) { (void)s; releases++; return release_ret; }
static void smp_store_release_bool(bool *p, bool v) { *p = v; }
#define smp_store_release(p, v) smp_store_release_bool(p, v)
static void dcpext_scanout_invalidate(struct apple_dcp *d) { (void)d; invalidates++; }
static void iomfb_m4_external_link_gone(struct apple_dcp *d) { (void)d; link_gones++; }
static void dcp_t8132_dptx_teardown_log(struct apple_dcp *d, u32 port, const char *what, int ret)
{
    (void)d; (void)port; (void)what; if (ret) warns++;
}
static int dcp_direct_crossbar_link(struct apple_dcp *d, bool up) { (void)d; (void)up; crossbar_downs++; return 0; }
#define DCP_SWAP_WATCHDOG_MS 1000
'''

CASES = r'''
/* ---- Display: the swap watchdog and the release of a firmware-27.2 T8132 dcpext ---- */
static int route;
static struct device_node j713_27 = { "apple,t8132-dcpext", true };
static struct device_node j713_26 = { "apple,t8132-dcpext", false };
static struct device_node t6041 = { "apple,t6041-dcpext", false };

static struct apple_dcp pipe(struct device *dev, int fw)
{
    struct apple_dcp dcp = { .dev = dev, .fw_compat = fw, .nr_typec_routes = 2,
                             .fixed_connector_type = 0 };
    static struct apple_epic_service service;

    dcp.dptxport[0].service = &service;
    return dcp;
}

/* The firmware accepts a present (dcp_swapped()). */
static void accepted(struct apple_dcp *dcp, u32 swap_id)
{
    dcp->present_state_h17p.pending = dcp->present_state_h17p.accepted = true;
    dcp->present_state_h17p.swap_id = swap_id;
    dcp_swap_watchdog_arm(dcp);
}

static void watchdog(void)
{
    struct device dev = { &j713_27 };
    struct apple_dcp dcp = pipe(&dev, DCP_FIRMWARE_V_27_0);
    int fixed;

    /* Routed to the dock: watched, as before */
    dcp.active_typec_route = &route;
    accepted(&dcp, 317);
    assert(dcp.swap_watchdog_wq.pending);
    dcp.present_state_h17p.pending = false;
    cancel_delayed_work(&dcp.swap_watchdog_wq);

    /* The dock goes with present 318 in flight: the disconnect keeps its watchdog */
    accepted(&dcp, 318);
    dcp_swap_watchdog_keep(&dcp);
    assert(dcp.swap_watchdog_wq.pending && dcp.swap_watchdog_wq.arms == 2);
    /* The route is released; the desktop turns the CRTC off: in flight, kept */
    dcp.active_typec_route = NULL;
    cancel_delayed_work(&dcp.swap_watchdog_wq);
    if (iomfb_present_in_flight(&dcp))
        dcp_swap_watchdog_keep(&dcp);
    else
        cancel_delayed_work(&dcp.swap_watchdog_wq);
    assert(dcp.swap_watchdog_wq.pending && infos == 1);
    dcp.present_state_h17p.pending = false;
    cancel_delayed_work(&dcp.swap_watchdog_wq);

    /* The desktop's next present, to the pipe without its route: watched */
    accepted(&dcp, 319);
    assert(dcp.swap_watchdog_wq.pending);
    dcp.present_state_h17p.pending = false;
    cancel_delayed_work(&dcp.swap_watchdog_wq);

    /* Nothing in flight: the CRTC's power-off stops the watchdog, as before */
    assert(!iomfb_present_in_flight(&dcp));
    dcp_swap_watchdog_keep(&dcp);
    assert(!dcp.swap_watchdog_wq.pending);

    /* A pipeline with a fixed output, unrouted, keeps what it had: not watched */
    dcp.fixed_connector = &fixed;
    dcp.fixed_connector_type = DRM_MODE_CONNECTOR_HDMIA;
    accepted(&dcp, 320);
    assert(!dcp.swap_watchdog_wq.pending);
    /* Nor the panel: no Type-C routes */
    dcp.fixed_connector = NULL;
    dcp.nr_typec_routes = 0;
    accepted(&dcp, 321);
    assert(!dcp.swap_watchdog_wq.pending);
    /* Firmware without the IOMFB queue: no present state, nothing kept */
    dcp.nr_typec_routes = 2;
    dcp.fw_compat = DCP_FIRMWARE_V_26_6;
    dcp.present_state_h17p.pending = true;
    cancel_delayed_work(&dcp.swap_watchdog_wq);
    dcp_swap_watchdog_keep(&dcp);
    assert(!dcp.swap_watchdog_wq.pending);
    puts("ok display: a present taken for a display that went is watched until retired");
}

static int release(struct device_node *node, int fw, int ret)
{
    struct device dev = { node };
    struct apple_dcp dcp = pipe(&dev, fw);
    int r;

    dcp.dptxport[0].enabled = dcp.dptxport[0].connected = true;
    release_ret = ret; releases = link_gones = 0;
    r = dcp_dptx_release_locked(&dcp, 0);
    assert(releases == 1);
    assert(r || !dcp.dptxport[0].connected);
    if (r)
        assert(dcp.dptxport[0].connected);
    return r;
}

static void teardown(void)
{
    /* Firmware 27.2: the timed-out release is taken as done, the M4 backend left alone */
    assert(release(&j713_27, DCP_FIRMWARE_V_27_0, -ETIMEDOUT) == 0 && link_gones == 0);
    assert(release(&j713_27, DCP_FIRMWARE_V_27_0, -EIO) == 0);
    assert(release(&j713_27, DCP_FIRMWARE_V_27_0, 0) == 0);
    /* The M4 backend, as before */
    assert(release(&j713_26, DCP_FIRMWARE_V_26_6, -ETIMEDOUT) == 0 && link_gones == 2);
    /* Other processors keep a refused release */
    assert(release(&t6041, DCP_FIRMWARE_V_27_0, -ETIMEDOUT) == -ETIMEDOUT);
    puts("ok display: a firmware-27.2 T8132 dcpext's unanswered release frees its route");
}

int main(void)
{
    watchdog();
    teardown();
    puts("PASS dock unplug with a present in flight, and replug");
    return 0;
}
'''


def build(tmp, name, sources):
    unit = PRE
    for src, fn in FUNCTIONS:
        unit += function(sources[src], fn)
    unit += CASES
    c = Path(tmp) / (name + '.c')
    binary = Path(tmp) / name
    c.write_text(unit)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(c), '-o', str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def main():
    with tempfile.TemporaryDirectory(prefix='dock-hotplug-relink-') as tmp:
        for name, (src, old, new) in MUTATIONS.items():
            assert SOURCES[src].count(old) == 1, 'stale mutation: ' + name
            sources = dict(SOURCES)
            sources[src] = SOURCES[src].replace(old, new)
            run = build(tmp, name, sources)
            assert run.returncode and 'Assertion' in run.stderr, 'mutation survived: ' + name
            print('PASS negative control:', name)
        run = build(tmp, 'production', SOURCES)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        raise SystemExit(run.returncode)


if __name__ == '__main__':
    main()
