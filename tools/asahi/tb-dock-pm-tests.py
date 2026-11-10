#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual dock PM callbacks with ordered client/topology lifetimes.

Hardware and the control channel are modeled; this does not qualify ACIO power
loss. Assertions detect a changed stock resume, routes a native CIO wake lost
that are not retired at complete, repeated cleanup, skipped NHI preparation and
a PCIe resume expectation from a sleep that keeps the router powered.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r'^static (?:int|void) ' + name + r'\([^;]*?\)\n\{', source,
                      re.M | re.S)
    if not match:
        raise ValueError(name)
    pos, depth = source.index('{', match.start()) + 1, 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos] + '\n'


HEADER = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define BIT(n) (1U << (n))
#define READ_ONCE(x) (x)
#define EBUSY 16
#define PM_SUSPEND_TO_IDLE 1
#define PM_SUSPEND_MEM 3
#define QUIRK_KEEP_TUNNELS BIT(0)
#define QUIRK_NO_SYSTEM_SLEEP BIT(1)
#define TB_HOST_DP_NOTIFY BIT(0)
struct list_head { int unused; };
#define LIST_HEAD(name) struct list_head name = {}
#define list_empty(x) true
#define list_for_each_entry_safe(p,n,h,m) for ((p)=NULL,(n)=NULL;(p);(p)=NULL)
#define list_for_each_entry_safe_reverse(p,n,h,m) list_for_each_entry_safe(p,n,h,m)
struct tb_switch;
struct tb_port { struct tb_switch *sw; struct tb_port *remote; struct tb_switch *xdomain; };
struct tb_switch { bool is_unplugged; struct tb_port *ports; };
struct tb_nhi { unsigned quirks, host_dp_policy; };
struct tb { int lock; struct tb_switch *root_switch; struct tb_nhi *nhi; };
struct tb_cm { bool hotplug_active, resume_cleanup_pending, dp_tunnel_pending;
 struct list_head tunnel_list; };
struct tb_tunnel { int unused; };
struct device { void *data; };
struct apple_cio { bool resume_had_peer, pcie_resume_expected, pcie_tunnel_requested,
 pcie_pm_prepared; int pcie_tunnel_lock; };
struct apple_nhi { struct tb *tb; struct apple_cio *acio; void *cio_dart; };
static struct tb_cm cm;
static struct tb tb;
static struct tb_nhi nhi;
static struct tb_switch root, child;
static struct tb_port root_port, child_port;
static struct apple_cio acio;
static struct apple_nhi anhi;
static struct device dev;
static bool resume_recovery, client_awake, tunnel_alive, child_alive, lost;
static int pm_suspend_target_state, cio_prepare_ret;
static unsigned stage, cleanup_calls, scans, queues, pauses, unregistered, probes;
static unsigned locks, tb_locks, pc_locks, discovery, activations;
static void lock(int *p)
{
 assert(!*p); *p=1; locks++;
 if (p == &tb.lock) tb_locks++; else { assert(p == &acio.pcie_tunnel_lock); pc_locks++; }
}
static void unlock(int *p) { assert(*p); *p=0; assert(locks); locks--; }
static int *guard_mutex_lock(int *p) { lock(p); return p; }
static void guard_mutex_unlock(int **p) { if (*p) unlock(*p); }
#define CAT_(a,b) a##b
#define CAT(a,b) CAT_(a,b)
#define guard(type) int *CAT(guard_,__COUNTER__) __attribute__((unused,cleanup(guard_##type##_unlock)))=guard_##type##_lock
#define scoped_guard(type,x) \
 for(int *scope __attribute__((cleanup(guard_##type##_unlock)))=guard_##type##_lock(x), *done=NULL; !done; done=scope)
#define tb_switch_for_each_port(s,p) for ((p)=(s)->ports;(p);(p)=NULL)
#define tb_dbg(...) ((void)0)
static void *dev_get_drvdata(struct device *d) { return d->data; }
static bool tb_port_is_null(struct tb_port *p) { return p != NULL; }
static void apple_dpin_pause_retries(struct apple_cio *a)
{ assert(a==&acio && !locks); pauses++; }
static int apple_nhi_cio_prepare(struct apple_nhi *a) { return cio_prepare_ret; }
static void apple_dpin_resume_retries(struct apple_cio *a) { assert(!locks); }
static struct tb_cm *tb_priv(struct tb *t) { assert(t==&tb); return &cm; }
static bool tb_switch_is_usb4(struct tb_switch *s) { return true; }
static void tb_switch_reset(struct tb_switch *s) { assert(false); }
static int tb_switch_resume(struct tb_switch *s, bool runtime)
{ assert(tb.lock && s==&root && !runtime); child.is_unplugged=lost; probes++; return 0; }
static void tb_free_invalid_tunnels(struct tb *t)
{
 assert(tb.lock && client_awake && stage==0);
 if (lost) { assert(tunnel_alive && child_alive); tunnel_alive=false; }
 stage=1; cleanup_calls++;
}
static void tb_free_unplugged_children(struct tb_switch *s)
{
 assert(tb.lock && client_awake && stage==1);
 if(lost) { assert(!tunnel_alive && child_alive); child_alive=false; }
 stage=2;
}
static void tb_free_unplugged_xdomains(struct tb_switch *s)
{ assert(tb.lock && stage==2); stage=3; }
static void tb_restore_children(struct tb_switch *s) { assert(tb.lock); }
static void tb_switch_discover_tunnels(struct tb_switch *s, struct list_head *l, bool alloc)
{ assert(tb.lock); discovery++; }
static bool tb_tunnel_is_usb3(struct tb_tunnel *t) { return false; }
static void tb_tunnel_deactivate(struct tb_tunnel *t) { assert(client_awake); }
static void tb_tunnel_put(struct tb_tunnel *t) { assert(false); }
static int tb_tunnel_activate(struct tb_tunnel *t) { activations++; return 0; }
static void msleep(unsigned ms) {}
static void tb_switch_enter_redrive(struct tb_switch *s) { assert(tb.lock); }
static void tb_queue_dp_tunnels(struct tb *t) { assert(tb.lock && cm.hotplug_active); queues++; }
static bool tb_domain_unregister_unplugged_xdomains(struct tb *t)
{ assert(!locks); unregistered++; return false; }
static void tb_scan_switch(struct tb_switch *s)
{ assert(tb.lock && client_awake && cm.hotplug_active && stage==3); scans++; }
static void tb_restore_dp_resources(struct tb_switch *s) { assert(tb.lock && client_awake); }
static void tb_tunnel_dp(struct tb *t) { assert(tb.lock && client_awake); }
static void tb_dp_relink_replay(struct tb *t) { assert(tb.lock && client_awake); }
'''

FOOTER = r'''
static void setup(void)
{
 memset(&cm,0,sizeof(cm)); memset(&acio,0,sizeof(acio));
 tb=(struct tb){.root_switch=&root,.nhi=&nhi};
 root=(struct tb_switch){.ports=&root_port}; child=(struct tb_switch){};
 root_port=(struct tb_port){.sw=&root,.remote=&child_port};
 child_port=(struct tb_port){.sw=&child};
 anhi=(struct apple_nhi){.tb=&tb,.acio=&acio}; dev.data=&anhi;
 nhi=(struct tb_nhi){};
 resume_recovery=false; pm_suspend_target_state=PM_SUSPEND_TO_IDLE; cio_prepare_ret=0;
 client_awake=true; tunnel_alive=child_alive=true; lost=false;
 stage=cleanup_calls=scans=queues=pauses=unregistered=probes=0;
 locks=tb_locks=pc_locks=discovery=activations=0;
}
static void resume(void) { lock(&tb.lock); assert(!tb_resume_noirq(&tb)); unlock(&tb.lock); }
int main(void)
{
 unsigned cases=0;
 for (unsigned optional=0;optional<2;optional++) {
  for (unsigned requested=0;requested<2;requested++) {
   setup(); resume_recovery=optional; acio.pcie_tunnel_requested=requested;
   assert(!apple_nhi_prepare(&dev));
   assert(pauses==1 && acio.pcie_pm_prepared && !locks);
   assert(acio.pcie_resume_expected==(optional && requested));
   assert(acio.resume_had_peer==!!optional);
   assert(tb_locks==optional && pc_locks==1); cases++;
  }
 }
 /* The J713's native deep sleep power-cycles the router: it expects the tunnel back. */
 for (unsigned deep=0;deep<2;deep++) {
  setup(); anhi.cio_dart=&anhi; acio.pcie_tunnel_requested=true;
  pm_suspend_target_state=deep ? PM_SUSPEND_MEM : PM_SUSPEND_TO_IDLE;
  assert(!apple_nhi_prepare(&dev) && acio.pcie_pm_prepared);
  assert(acio.pcie_resume_expected==!!deep); cases++;
 }
 /* No sleep drops an expectation an earlier one left unresolved. */
 for (unsigned optional=0;optional<2;optional++) {
  setup(); resume_recovery=optional; acio.pcie_resume_expected=true;
  assert(!apple_nhi_prepare(&dev) && acio.pcie_resume_expected); cases++;
 }
 /* A failed CIO prepare undoes the bookkeeping, the expectation included. */
 setup(); resume_recovery=true; acio.pcie_tunnel_requested=true; cio_prepare_ret=-EBUSY;
 assert(apple_nhi_prepare(&dev)==-EBUSY && !acio.pcie_pm_prepared);
 assert(!acio.pcie_resume_expected && !locks); cases++;
 /* The stock resume is unchanged: it retires lost routes at once. */
 for (unsigned quirks=0;quirks<4;quirks++) {
  setup(); lost=quirks&1;
  nhi.quirks=quirks&2 ? QUIRK_KEEP_TUNNELS|QUIRK_NO_SYSTEM_SLEEP : 0;
  nhi.host_dp_policy=TB_HOST_DP_NOTIFY; cm.dp_tunnel_pending=true;
  resume(); assert(cleanup_calls==1 && stage==3 && cm.hotplug_active && queues==1);
  assert(tunnel_alive==!lost && child_alive==!lost && !cm.resume_cleanup_pending);
  tb_complete(&tb); assert(cleanup_calls==1 && !scans && !locks); cases++;
 }
 /* A native CIO wake leaves lost routes to complete, after their clients resume. */
 for (unsigned gone=0;gone<2;gone++) {
  setup(); lost=gone; child.is_unplugged=gone;
  cm.resume_cleanup_pending=true; cm.hotplug_active=true;
  tb_complete(&tb);
  assert(cleanup_calls==1 && scans==1 && stage==3 && !locks);
  assert(tunnel_alive==!gone && child_alive==!gone);
  assert(!cm.resume_cleanup_pending && cm.hotplug_active);
  /* A second complete must not retire anything twice. */
  tb_complete(&tb); assert(cleanup_calls==1 && scans==1 && !locks); cases++;
 }
 setup(); cm.hotplug_active=true;
 assert(!apple_nhi_prepare(&dev)); tb_complete(&tb);
 assert(!cleanup_calls && !scans && !locks); cases++;
 setup();
 for(unsigned cycle=0;cycle<3;cycle++) {
  stage=0; cm.resume_cleanup_pending=true; cm.hotplug_active=true;
  tb_complete(&tb);
  assert(cleanup_calls==cycle+1 && scans==cycle+1 && !locks);
  assert(tunnel_alive && child_alive && !cm.resume_cleanup_pending); cases++;
 }
 printf("PASS %u dock PM lifetime cases\n",cases);
}
'''

MUTATIONS = {
    'prepare-optional-again': ('acio->resume_had_peer = false;',
                             'acio->resume_had_peer = false;\n if (!resume_recovery) return 0;'),
    'skip-pause': ('apple_dpin_pause_retries(acio);', '(void)apple_dpin_pause_retries;'),
    'skip-pcie-prepare': ('acio->pcie_pm_prepared = true;', 'acio->pcie_pm_prepared = false;'),
    'lose-prior-pcie-expectation': ('acio->pcie_resume_expected |=', 'acio->pcie_resume_expected ='),
    'expect-from-every-sleep': ('if (cycles)', 'if (cycles || true)'),
    'native-sleep-unexpected': ('(anhi->cio_dart && pm_suspend_target_state == PM_SUSPEND_MEM)',
                                'false'),
    'keep-expectation-on-failure': ('acio->pcie_resume_expected = expected;', '(void)expected;'),
    'stock-resume-defers': ('tb_resume_cleanup(tb);\n\ttb_restore_children(tb->root_switch);',
                            'tcm->resume_cleanup_pending = true;\n\ttb_restore_children(tb->root_switch);'),
    'repeat-cleanup': ('tcm->resume_cleanup_pending = false;', '/* leave pending */'),
    'skip-rescan': ('rescan = true;', 'rescan = false;'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    sources = {p: (ROOT / p).read_text() for p in
               ['drivers/thunderbolt/tb.c', 'drivers/thunderbolt/apple.c']}
    bodies = {name: function(sources[path], name) for path, names in {
        'drivers/thunderbolt/tb.c': ['tb_resume_cleanup', 'tb_resume_noirq', 'tb_complete'],
        'drivers/thunderbolt/apple.c': ['apple_nhi_prepare'],
    }.items() for name in names}
    production = '\n'.join(bodies.values())
    variants = {'real': production}
    for name, (old, new) in MUTATIONS.items():
        assert production.count(old) == 1, name
        variants[name] = production.replace(old, new)
    with tempfile.TemporaryDirectory(prefix='tb-dock-pm-') as tmp:
        out = args.output or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        results = {}
        for name, source in variants.items():
            unit, binary = out / (name + '.c'), out / name
            unit.write_text(HEADER + source + FOOTER)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', '-Wno-unused-but-set-variable',
                            '-g', '-fsanitize=address,undefined', str(unit), '-o', str(binary)],
                           check=True)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            results[name] = {'exit': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr}
            if name == 'real':
                assert run.returncode == 0, results[name]
                print(run.stdout, end='')
            else:
                assert run.returncode != 0 and 'Assertion' in run.stderr, results[name]
        (out / 'receipt.json').write_text(json.dumps({
            'scope': __doc__, 'bodies_sha256': {
                n: hashlib.sha256(s.encode()).hexdigest() for n, s in bodies.items()},
            'results': results}, indent=2) + '\n')
        print(f'PASS {len(MUTATIONS)} negative controls')


if __name__ == '__main__':
    main()
