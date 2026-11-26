#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Replay the T8132 dcpext Thunderbolt DP tunnel flow on both firmware backends.

A T8132 dcpext is driven by the M4 26.6.2 backend (dcp_is_t8132_ext()) or, on
firmware 27.2, as an ordinary firmware-27 pipe of the DRM device. Its display
hardware is the same either way (dcp_t8132_ext_hw()): the tunnel connects
from the Type-C reconnect work, never inline under the fabric call the
tunnel's provider is waiting in, and DCP's Activate points the crossbar's
DP IN output at the dcpext before the DP IN handshake. The production bodies
are compiled against host mocks; with each mutation (the hardware decisions
taken by the M4 backend predicate again, as before the split) the replay must
fail.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FABRIC = (ROOT / 'drivers/gpu/drm/apple/dcp-fabric.c').read_text()
SESSION = (ROOT / 'drivers/gpu/drm/apple/dcp-fabric-session.h').read_text()
NAMES = ['dcp_t8132_ext_m4_node', 'dcp_is_t8132_ext', 'dcp_t8132_ext_hw',
         'dcp_tunnel_dpin_locked', 'dcp_t8132_tunnel_route_xbar',
         'dcp_tunnel_dpin_activate', 'dcp_tb_external', 'dcp_tb_queue_reconnect']
MUTATIONS = {
    'activate-m4-only': ('bool t8132 = dcp_t8132_ext_hw(dcp);\n\tint ret;\n\n\tguard(mutex)(&dcp->tb_lock);\n\tif (!dcp->dptx_tunnel)',
                         'bool t8132 = dcp_is_t8132_ext(dcp);\n\tint ret;\n\n\tguard(mutex)(&dcp->tb_lock);\n\tif (!dcp->dptx_tunnel)'),
    'inline-connect': ('return ctx->best->dcp->external || dcp_t8132_ext_hw(ctx->best->dcp);',
                       'return ctx->best->dcp->external || dcp_is_t8132_ext(ctx->best->dcp);'),
    'no-attachment': ('\t\tif (dcp->typec_connector)\n\t\t\tdcp_typec_attach_deferred(dcp);\n\t\treturn;',
                      '\t\treturn;'),
}


def function(text, name):
    match = re.search(r'^(?:static\s+)?(?:inline\s+)?(?:bool|int|void)\s+' + re.escape(name) +
                      r'\([^;{]*?\)\n\{', text, re.M | re.S)
    assert match, name
    pos = text.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[match.start():pos] + '\n'


def struct(text, name):
    match = re.search(r'^struct ' + re.escape(name) + r' \{.*?^\};\n', text, re.M | re.S)
    assert match, name
    return match.group(0)


PRE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
enum { DCP_FIRMWARE_V_26_6 = 26, DCP_FIRMWARE_V_27_0 = 27 };
struct device_node { const char *compatible; bool v27_2; };
struct device { struct device_node *of_node; };
struct mux_control { int id; };
struct apple_dcp;
struct apple_dcp_typec_port;
struct apple_dcp_typec_route {
    struct apple_dcp *dcp;
    struct mux_control *active_xbar;
    bool tunnel, xbar_up, deactivate_xbar_done;
    unsigned int tunnel_dpin, mux_index;
    u64 tunnel_generation;
};
struct apple_dcp {
    struct device *dev;
    int fw_compat, tb_lock;
    bool external, dptx_tunnel;
    struct apple_dcp_typec_route *active_typec_route;
    u64 tb_generation;
    int (*tb_dpin_set_active)(void *binding, bool active);
    void *tb_dpin_ctx;
    unsigned int typec_reconnect_tries;
    void *typec_connector;
    struct {
        bool active, xbar_up;
        u64 generation;
        struct mux_control *xbar;
        int mux_state;
        int (*set_active)(void *binding, bool active);
        void *binding;
    } split;
};
static bool of_device_is_compatible(const struct device_node *n, const char *name)
{
    return n && !strcmp(n->compatible, name);
}
/* The 27.2 (26B123) admission, include/linux/soc/apple/t8132.h. */
static bool apple_t8132_dcpext_v27_2(const struct device_node *n)
{
    return of_device_is_compatible(n, "apple,t8132-dcpext") && n->v27_2;
}
#define guard(kind) mock_guard
#define lockdep_assert_held(lock) ((void)(lock))
#define dev_warn(dev, ...) ((void)(dev), warns++)
#define dev_dbg(dev, ...) ((void)(dev))
static unsigned int warns, locks, selects, set_actives, attaches, queues;
static int select_ret;
static bool last_active;
static struct mux_control *selected;
static int selected_state;
static void mock_guard(int *lock) { (void)lock; locks++; }
static int mux_control_try_select(struct mux_control *mux, int state)
{
    selects++; selected = mux; selected_state = state; return select_ret;
}
static bool dcp_t8132_late_release(struct apple_dcp *dcp, const char *what)
{
    (void)dcp; (void)what; return false;
}
/* A T8132 crossbar is no T602X-flow tunnel route of firmware 14.7 or 27. */
/* A T8132 route: never the T6041-layout crossbar. */
static bool dcp_xbar_t6041(struct mux_control *mux) { (void)mux; return false; }
static bool dcp_t602x_tunnel_route(struct apple_dcp_typec_route *route)
{
    (void)route; return false;
}
static int set_active(void *binding, bool active)
{
    (void)binding; set_actives++; last_active = active; return 0;
}
static void dcp_typec_attach_deferred(struct apple_dcp *dcp) { (void)dcp; attaches++; }
static void dcp_queue_typec_reconnect(struct apple_dcp *dcp, unsigned long delay)
{
    (void)dcp; (void)delay; queues++;
}
static void reset(void) { warns = locks = selects = set_actives = attaches = queues = 0; select_ret = 0; selected = NULL; }
'''

CASES = r'''
enum profile { M4_26, J713_27_2, J773G_27_2, OTHER_V27, T6030_EXT };
static const char *const names[] = { "M4 26.6.2", "J713 27.2", "J773g 27.2", "other fw27 dcpext", "T6030 external" };

int main(void)
{
    struct mux_control dpin0 = { 1 };
    int connector;

    for (int p = M4_26; p <= T6030_EXT; p++) {
        struct device_node node = {
            .compatible = p == OTHER_V27 ? "apple,t6041-dcpext" :
                          p == T6030_EXT ? "apple,t6030-dcpext" : "apple,t8132-dcpext",
            .v27_2 = p == J713_27_2 || p == J773G_27_2,
        };
        struct device dev = { &node };
        struct apple_dcp dcp = {
            .dev = &dev,
            .fw_compat = p == M4_26 ? DCP_FIRMWARE_V_26_6 : DCP_FIRMWARE_V_27_0,
            .external = p == T6030_EXT,
            .typec_connector = p == M4_26 ? NULL : &connector,
        };
        struct apple_dcp_typec_route route = {
            .dcp = &dcp, .active_xbar = &dpin0, .tunnel = true, .mux_index = 2,
        };
        struct dcp_tb_attach_context ctx = { .best = &route, .generation = 4 };
        bool t8132 = p <= J773G_27_2;

        /* The processor's backend and its hardware are separate facts. */
        assert(dcp_is_t8132_ext(&dcp) == (p == M4_26));
        assert(dcp_t8132_ext_hw(&dcp) == t8132);

        /*
         * Attach: every T8132 dcpext and the T6030 external processor connect
         * from the work. A pipeline of the DRM device starts its attachment
         * there (a 27.2 dcpext); the M4 backend's and the T6030 one queue the
         * connect as before.
         */
        assert(dcp_tb_external(&ctx) == (t8132 || p == T6030_EXT));
        reset(); dcp.typec_reconnect_tries = 3;
        dcp_tb_queue_reconnect(&ctx);
        if (p == J713_27_2 || p == J773G_27_2) {
            assert(attaches == 1 && queues == 0);
        } else if (p == OTHER_V27) {
            /* Not reached in production (connects inline); with a connector, the work too. */
            assert(attaches == 1 && queues == 0);
        } else {
            assert(attaches == 0 && queues == 1 && dcp.typec_reconnect_tries == 0);
        }
        /* Without a connector, a pipeline of the DRM device connects nothing. */
        if (p == J713_27_2) {
            dcp.typec_connector = NULL;
            reset(); dcp_tb_queue_reconnect(&ctx);
            assert(attaches == 0 && queues == 0);
            dcp.typec_connector = &connector;
        }

        /* DCP's Activate on the admitted binding. */
        dcp.dptx_tunnel = true;
        dcp.active_typec_route = &route;
        dcp.tb_generation = route.tunnel_generation = 4;
        dcp.tb_dpin_set_active = set_active;
        reset();
        assert(dcp_tunnel_dpin_activate(&dcp, 0, true) == 0);
        assert(set_actives == 1 && last_active);
        if (t8132) {
            /* The DP IN output points at this dcpext (source 2) before the handshake. */
            assert(selects == 1 && selected == &dpin0 && selected_state == 2 && route.xbar_up);
            reset();
            assert(dcp_tunnel_dpin_activate(&dcp, 0, true) == 0 && selects == 0);
        } else {
            assert(selects == 0 && !route.xbar_up);
        }
        /* A refused selection fails the Activate before the DP IN is woken. */
        if (t8132) {
            route.xbar_up = false; reset(); select_ret = -EBUSY;
            assert(dcp_tunnel_dpin_activate(&dcp, 0, true) == -EBUSY);
            assert(set_actives == 0 && !route.xbar_up);
        }
        /* A replaced binding: -ESTALE, nothing selected or woken. */
        route.xbar_up = false; dcp.tb_generation = 5; reset();
        assert(dcp_tunnel_dpin_activate(&dcp, 0, true) == -ESTALE);
        assert(selects == 0 && set_actives == 0);
        dcp.tb_generation = 4;
        /* Deactivate selects nothing. */
        reset();
        assert(dcp_tunnel_dpin_activate(&dcp, 0, false) == 0);
        assert(selects == 0 && set_actives == 1 && !last_active);
        printf("ok %s\n", names[p]);
    }
    puts("PASS T8132 dcpext tunnel attach (work, attachment) and Activate (crossbar, stale binding) "
         "on the M4 backend and on firmware 27.2");
    return 0;
}
'''


def build(tmp, name, fabric):
    unit = PRE + function(SESSION, 'dcp_fabric_callback_valid')
    unit += struct(fabric, 'dcp_tb_attach_context')
    for n in NAMES:
        unit += function(fabric, n)
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
    with tempfile.TemporaryDirectory(prefix='t8132-v27-tunnel-') as tmp:
        for name, (old, new) in MUTATIONS.items():
            assert FABRIC.count(old) == 1, 'stale mutation: ' + name
            run = build(tmp, name, FABRIC.replace(old, new))
            assert run.returncode, 'mutation survived: ' + name
            print('PASS negative control:', name)
        run = build(tmp, 'production', FABRIC)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        raise SystemExit(run.returncode)


if __name__ == '__main__':
    main()
