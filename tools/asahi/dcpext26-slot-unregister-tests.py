#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Replay production four-slot removal and retained teardown refusal.

Transport, locks and lists are mocks; this does not prove DMA retirement.
"""
from pathlib import Path
import argparse
import json
import os
import re
import resource
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path)
args = parser.parse_args()
temporary = tempfile.TemporaryDirectory(prefix='dcp-slot-unregister-') if args.out is None else None
out = args.out if args.out is not None else Path(temporary.name)
out.mkdir(parents=True, exist_ok=True)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def function(text, name):
    match = re.search(r'(?m)^(?:(?:static|inline) )*(?:bool|void) ' + name + r'\([^;{}]*\)\n\{', text)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end] + '\n'


apple = ROOT / 'drivers/gpu/drm/apple'
source = (apple / 'dcp-fabric.c').read_text()
helpers = function((apple / 'dcp-internal.h').read_text(), 'dcp_is_ext26')
helpers += function((apple / 'dcp.c').read_text(), 'dcp_is_external_h17p')
helpers += function((apple / 'dcp.c').read_text(), 'dcp_is_retained_external')
helpers += function(source, 'dcp_typec_port_owned')
body = function(source, 'dcp_typec_route_unregister')
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
enum { DCP_IOMFB_METHODS_H17P_EXTERNAL=1, DCP_IOMFB_METHODS_J613_EXT26=2 };
struct device { int unused; };
struct apple_dcp { struct { int iomfb_method_profile; } hw; struct device *dev; bool typec_cable_connected; };
struct list_head { bool empty; };
struct apple_dcp_typec_port;
struct apple_dcp_typec_route {
 struct apple_dcp_typec_port *port; struct apple_dcp *dcp;
 int typec_mux; bool tunnel, dual_stream; struct list_head port_link;
};
struct apple_dcp_typec_port {
 struct apple_dcp_typec_route *preferred_route, *slots[4];
 struct apple_dcp *split_dcp; bool hpd;
 struct list_head link, routes; void *connector_np;
};
static int dcp_typec_fabric_lock, dcp_dual_stream_routes;
static int failure, muxes, retired, disconnected, deactivated, deleted, freed, node_puts, split, order, primary;
static struct apple_dcp_typec_port *current_port;
static void mock_guard(int *lock) { (void)lock; }
#define guard(kind) mock_guard
static void typec_mux_unregister(int mux) { (void)mux; muxes++; }
static void dcp_tb_split_teardown_locked(struct apple_dcp_typec_port *port, bool wait) {
 assert(wait); split++; port->split_dcp=NULL;
}
static void dcp_tb_binding_retire(struct apple_dcp *dcp, struct apple_dcp_typec_route *route) {
 assert(dcp==route->dcp && order==0); order=1; retired++;
}
static struct device *to_platform_device(struct device *dev) { return dev; }
static int dcp_dptx_disconnect_oob(struct device *dev, int mode) {
 (void)dev; assert(mode==0 && order==1); order=2; disconnected++; return 0;
}
static int dcp_typec_route_deactivate(struct apple_dcp_typec_route *route) {
 (void)route; assert(order==2); order=3; deactivated++;
 /* Removing a secondary DP IN must not drop the primary's HPD first. */
 assert(current_port->hpd==!primary); return failure;
}
static void list_del(struct list_head *head) { (void)head; deleted++; }
static bool list_empty(struct list_head *head) { return head->empty; }
static void atomic_dec(int *value) { (*value)--; }
static void of_node_put(void *node) { (void)node; node_puts++; }
static void kfree(void *port) { (void)port; freed++; }
'''
tests = r'''
int main(void) {
 unsigned cases=0;
 for(int profile=0;profile<3;profile++) for(unsigned slot=0;slot<4;slot++)
 for(int fail=0;fail<2;fail++) for(int other=0;other<2;other++) {
  struct device dev={0}; struct apple_dcp d={.hw.iomfb_method_profile=profile,.dev=&dev,.typec_cable_connected=true};
  struct apple_dcp_typec_port port={.hpd=true};
  struct apple_dcp_typec_route route={.port=&port,.dcp=&d,.tunnel=true,.dual_stream=true}, neighbor={0};
  unsigned next=(slot+1)%4; port.slots[slot]=&route;
  if(other) port.slots[next]=&neighbor;
  port.routes.empty=!other; port.preferred_route=&route; port.split_dcp=&d;
  current_port=&port; primary=slot==0; failure=fail?-EBUSY:0; dcp_dual_stream_routes=1;
  muxes=retired=disconnected=deactivated=deleted=freed=node_puts=split=order=0;
  dcp_typec_route_unregister(&route);
  assert(muxes==1 && retired==1 && disconnected==1 && deactivated==1 && split==1);
  assert(port.preferred_route==NULL && port.split_dcp==NULL);
  if(fail && profile) {
   assert(port.slots[slot]==&route && deleted==0 && freed==0 && node_puts==0);
   assert(dcp_dual_stream_routes==1 && port.hpd==(slot!=0));
  } else {
   assert(port.slots[slot]==NULL && port.hpd==!!other && dcp_dual_stream_routes==0);
   assert(deleted==(other?1:2) && freed==!other && node_puts==!other);
  }
  if(other) assert(port.slots[next]==&neighbor);
  cases++;
 }
 assert(cases==48); puts("PASS48 production unregister cases:4slots,3profiles,success/refusal,neighbor retention");
}
'''
mutants = [
    ('retained-slots', body, False),
    ('drop-native26-retention', body.replace('dcp_is_retained_external(dcp)', 'dcp_is_external_h17p(dcp)'), True),
    ('drop-extra-slots', body.replace('ARRAY_SIZE(port->slots)', '2'), True),
    ('drop-primary-only-hpd', body.replace('if (!i)\n', 'if (true)\n'), True),
]
results = []
for name, code, negative in mutants:
    assert code != body or not negative
    path = out / (name + '.c')
    path.write_text(pre + helpers + code + tests)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path), '-o', str(path.with_suffix(''))], check=True)
    process = subprocess.run([str(path.with_suffix(''))], capture_output=True, text=True)
    log = process.stdout + process.stderr
    (out / (name + '.log')).write_text(log)
    if negative:
        assert process.returncode != 0 and re.search(r'assertion.*failed|assertion failed', log, re.I), log
    else:
        assert process.returncode == 0, log
    results.append({'name': name, 'compiled': True, 'exit': process.returncode, 'negative': negative})
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
print('PASS:48 production cases and3 compiled runtime-negative controls')
