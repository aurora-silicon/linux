#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Replay the production direct-link gate across merged display profiles."""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'drivers/gpu/drm/apple/dcp-fabric.c').read_text()
header = (ROOT / 'drivers/gpu/drm/apple/dcp-internal.h').read_text()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--drop-native26', action='store_true')
parser.add_argument('--drop-t8132', action='store_true')
parser.add_argument('--drop-on-link', action='store_true')
args = parser.parse_args()

def function(text, name):
    match = re.search(r'^(?:static inline |static )?(?:bool|int) ' + re.escape(name) + r'\(', text, re.M)
    assert match, name
    start = text.index('{', match.end())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]

link = function(source, 'dcp_direct_crossbar_link')
clocks_on_link = function(source, 'dcp_typec_xbar_clocks_on_link')
if args.drop_native26:
    old = ' && !dcp_is_ext26(dcp)'
    assert link.count(old) == 1
    link = link.replace(old, '', 1)
if args.drop_t8132:
    old = '!dcp_t8132_ext_hw(dcp) && '
    assert link.count(old) == 1
    link = link.replace(old, '', 1)
if args.drop_on_link:
    old = '!on_link && '
    assert link.count(old) == 1
    link = link.replace(old, '', 1)

pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
enum { DCP_FIRMWARE_V_27_0 = 27, DCP_IOMFB_METHODS_J613_EXT26 = 26 };
struct device_node { const char *compatible; bool v27_2; };
struct device { struct device_node *of_node; };
struct mux_control { int id; };
struct apple_dcp_typec_route {
    struct mux_control *active_xbar, *xbar;
    bool tunnel, selected;
};
struct apple_dcp {
    struct { int iomfb_method_profile; } hw;
    int fw_compat, tb_lock;
    struct device *dev;
    struct apple_dcp_typec_route *active_typec_route;
    struct mux_control *fixed_xbar, *direct_xbar_up;
    bool fixed_route_selected, desktop_external;
};
static const char *machine;
static unsigned links, v27, locks;
static struct mux_control *last;
static bool last_up;
static int fault;
static bool of_machine_is_compatible(const char *name) { return !strcmp(machine, name); }
static bool of_device_is_compatible(const struct device_node *n, const char *name) {
    return !strcmp(n->compatible, name);
}
/* The J773g's 27.2 (26B123) dcpext admission, include/linux/soc/apple/display.h. */
static bool apple_t8132_dcpext_v27_2(const struct device_node *n) {
    return of_device_is_compatible(n, "apple,t8132-dcpext") && n->v27_2;
}
static void mock_guard(int *lock) { (void)lock; locks++; }
/* The crossbar's export, include/linux/soc/apple/dp-tunnel.h, behind symbol_get(). */
static bool xbar_clocks_on_link, xbar_module = true;
static unsigned queries, symbol_puts;
static struct mux_control *queried;
static bool apple_dpxbar_clocks_on_link(struct mux_control *mux) {
    queries++; queried = mux; return xbar_clocks_on_link;
}
#define symbol_get(sym) (xbar_module ? &(sym) : NULL)
#define symbol_put(sym) do { symbol_puts++; } while (0)
#define guard(kind) mock_guard
#define dev_warn(...) ((void)0)
static int dcp_dpxbar_link(struct mux_control *xbar, bool up) {
    links++; last = xbar; last_up = up; return fault;
}
static int dcp_direct_crossbar_link_t6050(struct apple_dcp *dcp, bool up) {
    (void)dcp; (void)up; v27++; return 37;
}
'''
cases = r'''
static void reset(void) {
    links = v27 = locks = queries = symbol_puts = 0; last = queried = NULL; last_up = false; fault = 0;
}
int main(void) {
    struct device_node node = { .compatible = "apple,other-dcp" };
    struct device dev = { &node };
    struct mux_control a = {1}, b = {2};
    struct apple_dcp_typec_route route = { .xbar = &a, .selected = true };
    for (int profile = 0; profile < 3; profile++) {
        machine = profile == 0 ? "apple,t6030" : "apple,other";
        node.compatible = profile == 1 ? "apple,t8132-dcpext" : "apple,other-dcp";
        struct apple_dcp d = { .dev = &dev, .active_typec_route = &route };
        d.hw.iomfb_method_profile = profile == 2 ? DCP_IOMFB_METHODS_J613_EXT26 : 0;
        reset();
        assert(dcp_direct_crossbar_link(&d, true) == 0);
        assert(links == 1 && locks == 1 && last == &a && last_up && d.direct_xbar_up == &a);
        assert(dcp_direct_crossbar_link(&d, true) == 0 && links == 1);
        assert(dcp_direct_crossbar_link(&d, false) == 0 && links == 2 && !last_up && !d.direct_xbar_up);
        assert(dcp_direct_crossbar_link(&d, false) == 0 && links == (profile == 2 ? 3u : 2u));

        /* A new selected output must be linked, even after a cached up. */
        d.direct_xbar_up = &a; route.active_xbar = &b;
        reset(); assert(dcp_direct_crossbar_link(&d, true) == 0);
        assert(links == 1 && last == &b && d.direct_xbar_up == &b);
        route.active_xbar = NULL;

        d.direct_xbar_up = NULL; reset(); fault = -EIO;
        assert(dcp_direct_crossbar_link(&d, true) == -EIO && !d.direct_xbar_up);
        assert(links == 1);
        d.direct_xbar_up = &a; reset(); fault = -EIO;
        assert(dcp_direct_crossbar_link(&d, false) == -EIO && !d.direct_xbar_up);
        assert(links == 1);

        route.tunnel = true; d.direct_xbar_up = NULL;
        reset(); assert(dcp_direct_crossbar_link(&d, true) == 0 && links == 0);
        route.tunnel = false; route.selected = false;
        reset(); assert(dcp_direct_crossbar_link(&d, true) == 0 && links == 0);
        route.selected = true;
        d.active_typec_route = NULL; d.fixed_xbar = &b; d.fixed_route_selected = true;
        reset(); assert(dcp_direct_crossbar_link(&d, true) == 0 && last == &b && links == 1);
        d.fixed_route_selected = false; d.direct_xbar_up = NULL;
        reset(); assert(dcp_direct_crossbar_link(&d, true) == 0 && links == 0);

        /*
         * The V27 PHY helper precedes every family admission except T6041
         * dcpext: without a Type-C route the crossbar is not asked (the
         * route probe takes the lock once and finds none).
         */
        d.fw_compat = DCP_FIRMWARE_V_27_0;
        reset(); assert(dcp_direct_crossbar_link(&d, true) == 37);
        assert(v27 == 1 && links == 0 && locks == 1 && queries == 0);
        d.desktop_external = true; d.active_typec_route = &route;
        reset(); assert(dcp_direct_crossbar_link(&d, true) == 0);
        assert(v27 == 0 && links == 1 && locks == 1);
    }
    /* A J773g dcpext on firmware 27.2 is no M4 dcpext: it links as a T6041 firmware-27 one. */
    machine = "apple,other"; node.compatible = "apple,t8132-dcpext"; node.v27_2 = true;
    struct apple_dcp j773g = { .dev = &dev, .active_typec_route = &route,
                               .fw_compat = DCP_FIRMWARE_V_27_0, .desktop_external = true };
    assert(!dcp_is_t8132_ext(&j773g) && dcp_t8132_ext_hw(&j773g));
    reset(); assert(dcp_direct_crossbar_link(&j773g, true) == 0);
    assert(v27 == 0 && links == 1 && locks == 1 && j773g.direct_xbar_up == &a);

    /*
     * The Air's (J713) dcpexts on firmware 27.2: T8132 display hardware
     * (dcp_t8132_ext_hw()), no M4 backend. A Type-C route whose crossbar
     * clocks follow the link is linked here; without that, the T6050 path.
     */
    struct apple_dcp j713 = { .dev = &dev, .active_typec_route = &route,
                              .fw_compat = DCP_FIRMWARE_V_27_0 };
    assert(!dcp_is_t8132_ext(&j713) && dcp_t8132_ext_hw(&j713));
    xbar_clocks_on_link = true;
    reset(); assert(dcp_direct_crossbar_link(&j713, true) == 0);
    assert(queries == 1 && v27 == 0 && links == 1 && last_up && j713.direct_xbar_up == &a);
    reset(); assert(dcp_direct_crossbar_link(&j713, false) == 0);
    assert(queries == 0 && links == 1 && !last_up && !j713.direct_xbar_up);
    xbar_clocks_on_link = false;
    reset(); assert(dcp_direct_crossbar_link(&j713, true) == 37);
    assert(v27 == 1 && links == 0 && !j713.direct_xbar_up);
    node.v27_2 = false;
    /* On the M4 26.6.2 path the same processor is both. */
    assert(dcp_is_t8132_ext(&j713) && dcp_t8132_ext_hw(&j713));

    /*
     * Firmware 27 on a Type-C route: the crossbar decides. An output whose
     * clocks follow the link (the M4 Air's 27.2 dcpexts) is linked here on
     * any machine and processor; any other keeps the T6050 PHY path.
     */
    machine = "apple,other"; node.compatible = "apple,other-dcp";
    route.tunnel = false; route.selected = true; route.active_xbar = NULL;
    struct apple_dcp air = { .dev = &dev, .active_typec_route = &route,
                             .fw_compat = DCP_FIRMWARE_V_27_0 };
    xbar_clocks_on_link = true;
    reset(); assert(dcp_direct_crossbar_link(&air, true) == 0);
    assert(queries == 1 && queried == &a && symbol_puts == 1);
    assert(v27 == 0 && links == 1 && last == &a && last_up && air.direct_xbar_up == &a);
    /* Brought up that way, it is taken down that way without asking again. */
    reset(); assert(dcp_direct_crossbar_link(&air, false) == 0);
    assert(queries == 0 && v27 == 0 && links == 1 && !last_up && !air.direct_xbar_up);
    /* The selected output is the one asked about. */
    route.active_xbar = &b;
    reset(); assert(dcp_direct_crossbar_link(&air, true) == 0);
    assert(queried == &b && links == 1 && last == &b && air.direct_xbar_up == &b);
    reset(); assert(dcp_direct_crossbar_link(&air, false) == 0 && !air.direct_xbar_up);
    route.active_xbar = NULL;

    xbar_clocks_on_link = false;
    reset(); assert(dcp_direct_crossbar_link(&air, true) == 37);
    assert(queries == 1 && symbol_puts == 1 && v27 == 1 && links == 0 && !air.direct_xbar_up);
    /* No crossbar module, or a tunnel: not asked, the T6050 path. */
    xbar_clocks_on_link = true; xbar_module = false;
    reset(); assert(dcp_direct_crossbar_link(&air, true) == 37);
    assert(queries == 0 && symbol_puts == 0 && v27 == 1 && links == 0);
    xbar_module = true; route.tunnel = true;
    reset(); assert(dcp_direct_crossbar_link(&air, true) == 37);
    assert(queries == 0 && v27 == 1 && links == 0);
    route.tunnel = false; xbar_clocks_on_link = false;

    machine = "apple,other"; node.compatible = "apple,other-dcp";
    struct apple_dcp unrelated = { .dev = &dev, .active_typec_route = &route };
    reset(); assert(dcp_direct_crossbar_link(&unrelated, true) == 0);
    assert(links == 0 && locks == 0);
    puts("PASS production crossbar union, route/cache/error behavior, V27 precedence, "
         "firmware-27 clocks-on-link crossbar (true/false/no module/tunnel), J773g 27.2, J713 27.2 "
         "(hardware, not M4) and unrelated exclusion");
    return 0;
}
'''
unit = pre + function(header, 'dcp_is_ext26') + '\n'
unit += function(source, 'dcp_t6041_ext_v27') + '\n'
unit += function(source, 'dcp_t8132_ext_m4_node') + '\n'
unit += function(source, 'dcp_is_t8132_ext') + '\n'
unit += function(source, 'dcp_t8132_ext_hw') + '\n' + clocks_on_link + '\n' + link + '\n' + cases
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'crossbar.c'
    binary = Path(tmp) / 'crossbar'
    c.write_text(unit)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(c), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)])
    raise SystemExit(0 if result.returncode == 0 else 1)
