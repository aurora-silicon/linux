#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real DP retirement/repair bodies with modeled paths, PM and a queue.

This checks ordering and reference balance, not kernel structure layout, hardware,
real runtime-PM locking or a complete Thunderbolt workqueue implementation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def function(text, name):
    match = re.search(r'^(?:static\s+)?(?:inline\s+)?(?:int|void|bool)\s+' +
                      name + r'\([^;]*?\)\n\{', text, re.M | re.S)
    if not match:
        raise ValueError('Missing actual function: ' + name)
    pos = text.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[match.start():pos] + '\n'


HEADER = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"CHECK line %d: %s\n",__LINE__,#x); exit(77); } } while (0)
struct list_head {int unused;};
struct device {int unused;};
struct tb_switch {struct device dev;};
struct tb_port {struct tb_switch *sw;bool is_dpin;struct list_head list;};
struct work_struct {void(*fn)(struct work_struct*);bool pending;};
struct tb_nhi {unsigned long quirks;};
#define BIT(n) (1UL<<(n))
#define QUIRK_NO_SYSTEM_SLEEP BIT(5)
struct tb {int lock;struct device dev;void*wq;struct tb_switch*root_switch;struct tb_nhi*nhi;};
struct tb_cm {bool hotplug_active;struct work_struct dp_tunnel_work;bool dp_tunnel_pending;};
struct domain {struct tb tb;struct tb_cm cm;};
_Static_assert(offsetof(struct domain,cm)==sizeof(struct tb),"mock private storage adjacency");
#define INIT_WORK(w,handler) ((w)->fn=(handler))
struct kref {int count;};
struct tb_path {bool reserved;};
enum {TB_TUNNEL_DP,TB_TUNNEL_USB3};
struct tb_tunnel {struct kref kref;struct tb*tb;struct tb_port*src_port,*dst_port;int npaths;void(*destroy)(struct tb_tunnel*);struct list_head list;int type;bool dprx_started,dprx_canceled;void(*callback)(struct tb_tunnel*,void*);void*callback_data;struct tb_path*paths[3];};
static int tb_tunnel_lock,occupied,alloc_calls,alloc_failures,alloc_successes,freed;
static int domain_refs,pm_refs,pm_calls,pm_error,stop_during_pm,requeue_once,lock_depth;
static int restore_calls,scan_calls,unplugged_xdomains;
static struct tb_tunnel *current;
static struct domain domain;
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define READ_ONCE(x) (x)
#define fallthrough __attribute__((fallthrough))
#define tb_port_dbg(...) ((void)0)
#define tb_tunnel_dbg(...) ((void)0)
#define tb_tunnel_warn(...) ((void)0)
static struct tb_cm*tb_priv(struct tb*t){return (struct tb_cm*)((char*)t+sizeof(*t));}
static struct tb*tcm_to_tb(struct tb_cm*c){return (struct tb*)((char*)c-sizeof(struct tb));}
static void mutex_lock(int*x){if(x==&domain.tb.lock){CHECK(!lock_depth);lock_depth++;}}
static void mutex_unlock(int*x){if(x==&domain.tb.lock){CHECK(lock_depth==1);lock_depth--;}}
#define lockdep_assert_held(x) CHECK((x)==&domain.tb.lock && lock_depth==1)
/* guard()/scoped_guard() as in <linux/cleanup.h>: the lock is dropped when the scope ends. */
static int*guard_mutex_lock(int*x){mutex_lock(x);return x;}
static void guard_mutex_unlock(int**x){if(*x)mutex_unlock(*x);}
#define GUARD_CAT2(a,b) a##b
#define GUARD_CAT(a,b) GUARD_CAT2(a,b)
#define guard(type) int*GUARD_CAT(guard_,__COUNTER__) __attribute__((unused,cleanup(guard_##type##_unlock)))=guard_##type##_lock
#define scoped_guard(type,x) \
 for(int*scope __attribute__((cleanup(guard_##type##_unlock)))=guard_##type##_lock(x),*scope_done=NULL; \
     !scope_done;scope_done=scope)
static int tb_domain_unregister_unplugged_xdomains(struct tb*t){return unplugged_xdomains;}
static void tb_scan_switch(struct tb_switch*s){CHECK(lock_depth==1 && s);scan_calls++;}
static void tb_restore_dp_resources(struct tb_switch*s){CHECK(lock_depth==1 && s);restore_calls++;}
static void kref_get(struct kref*k){CHECK(k->count>0);k->count++;}
static void kref_put(struct kref*k,void(*destroy)(struct kref*)){CHECK(k->count>0);if(!--k->count)destroy(k);}
static void tb_path_free(struct tb_path*p){if(p->reserved)occupied--;freed++;free(p);}
#define kfree(x) free(x)
static void list_del(struct list_head*l){current=NULL;}
static void list_del_init(struct list_head*l){}
static bool tb_port_is_dpin(struct tb_port*p){return p->is_dpin;}
static void tb_dp_tunnel_notify(struct tb_tunnel*t,bool active){}
static void tb_tunnel_deactivate(struct tb_tunnel*t){}
static void tb_detach_bandwidth_group(struct tb_port*p){}
static void tb_switch_dealloc_dp_resource(struct tb_switch*s,struct tb_port*p){}
static void tb_configure_sym(struct tb*t,struct tb_port*i,struct tb_port*o,bool active){}
static void pm_runtime_mark_last_busy(struct device*d){}
static void pm_runtime_put_autosuspend(struct device*d){if(d==&domain.tb.dev){CHECK(pm_refs==1);pm_refs--;}}
static int pm_runtime_resume_and_get(struct device*d){
 CHECK(!lock_depth);pm_calls++;
 if(pm_error)return pm_error;
 pm_refs++;
 if(stop_during_pm)domain.cm.hotplug_active=false;
 return 0;
}
static void tb_reclaim_usb3_bandwidth(struct tb*t,struct tb_port*i,struct tb_port*o){}
static bool tb_tunnel_is_active(struct tb_tunnel*t){return false;}
static int tb_tunnel_consumed_bandwidth(struct tb_tunnel*t,int*up,int*down){return 0;}
static void tb_configure_asym(struct tb*t,struct tb_port*i,struct tb_port*o,int up,int down){}
static void tb_increase_tmu_accuracy(struct tb_tunnel*t){}
static struct tb_tunnel*tb_find_tunnel(struct tb*t,int type,struct tb_port*in,struct tb_port*out){return current;}
static void tb_enter_redrive(struct tb_port*p){}
static void tb_recalc_estimated_bandwidth(struct tb*t){}
static struct tb*tb_domain_get(struct tb*t){CHECK(domain_refs>=1);domain_refs++;return t;}
static void tb_domain_put(struct tb*t){CHECK(domain_refs>1);domain_refs--;}
static bool queue_work(void*wq,struct work_struct*w){if(w->pending)return false;w->pending=true;return true;}
static void tb_queue_dp_tunnels(struct tb*tb);
static void tb_tunnel_dp(struct tb*t){
 CHECK(lock_depth==1 && domain.cm.hotplug_active && t->root_switch);
 alloc_calls++;if(occupied)alloc_failures++;else alloc_successes++;
 if(requeue_once){requeue_once=0;tb_queue_dp_tunnels(t);}
}
'''

FOOTER = r'''
static struct tb_tunnel*new_tunnel(struct tb*tb,struct tb_port*in,struct tb_port*out){
 struct tb_tunnel*t=calloc(1,sizeof(*t));CHECK(t);t->tb=tb;t->src_port=in;t->dst_port=out;t->type=TB_TUNNEL_DP;t->kref.count=1;t->npaths=1;
 t->paths[0]=calloc(1,sizeof(struct tb_path));CHECK(t->paths[0]);t->paths[0]->reserved=true;occupied++;t->callback=tb_dp_tunnel_active;t->callback_data=tb;current=t;return t;
}
static void flush_queue(void){
 unsigned n=0;
 while(domain.cm.dp_tunnel_work.pending){
  CHECK(++n<8);CHECK(domain.cm.dp_tunnel_work.fn);domain.cm.dp_tunnel_work.pending=false;
  domain.cm.dp_tunnel_work.fn(&domain.cm.dp_tunnel_work);
 }
 CHECK(!lock_depth && !pm_refs && domain_refs==1);
 if(alloc_successes)CHECK(!domain.cm.dp_tunnel_pending);
}
static void queue_repair(unsigned count){
 mutex_lock(&domain.tb.lock);
 for(unsigned i=0;i<count;i++)tb_queue_dp_tunnels(&domain.tb);
 mutex_unlock(&domain.tb.lock);
}
static struct tb_nhi nhi;
/*
 * Freeze drops the queued pass and keeps the request; thaw replays it while
 * the domain's runtime PM is still disabled (-EACCES), so it is kept again;
 * early resume then enables runtime PM before completion.
 */
static void freeze_then_thaw_with_pm_disabled(void){
 queue_repair(1);
 mutex_lock(&domain.tb.lock);CHECK(!tb_freeze_noirq(&domain.tb));mutex_unlock(&domain.tb.lock);
 flush_queue();CHECK(!alloc_calls && !pm_calls && domain.cm.dp_tunnel_pending);
 pm_error=-13;
 mutex_lock(&domain.tb.lock);CHECK(!tb_thaw_noirq(&domain.tb));mutex_unlock(&domain.tb.lock);
 CHECK(domain.cm.dp_tunnel_work.pending);
 flush_queue();CHECK(!alloc_calls && pm_calls==1 && domain.cm.dp_tunnel_pending);
 pm_error=0;
}
int main(int argc,char**argv){
 CHECK(argc==2);
 domain.tb.nhi=&nhi;
 struct tb_switch sw={0};struct tb_port in={.sw=&sw,.is_dpin=true},out={.sw=&sw};
 domain_refs=1;domain.tb.root_switch=&sw;domain.tb.wq=&domain;domain.cm.hotplug_active=true;
 init_fixture(&domain.cm);CHECK(domain.cm.dp_tunnel_work.fn);
 const char*c=argv[1];
 if(!strcmp(c,"callback-release-before-repair")){
  struct tb_tunnel*t=new_tunnel(&domain.tb,&in,&out);
  tb_tunnel_get(t);tb_domain_get(&domain.tb);
  worker_tail(&domain.tb,t);
  CHECK(domain.cm.dp_tunnel_work.pending && !alloc_calls && !occupied && freed==1 && domain_refs==2);
  flush_queue();CHECK(alloc_calls==1 && !alloc_failures && alloc_successes==1);
 }else if(!strcmp(c,"canceled-callback-does-not-repair")){
  struct tb_tunnel*t=new_tunnel(&domain.tb,&in,&out);
  t->dprx_canceled=true;tb_tunnel_get(t);tb_domain_get(&domain.tb);
  worker_tail(&domain.tb,t);
  CHECK(!domain.cm.dp_tunnel_work.pending && !alloc_calls && occupied==1 && !freed && domain_refs==1);
  mutex_lock(&domain.tb.lock);tb_deactivate_and_free_tunnel(t);mutex_unlock(&domain.tb.lock);
  CHECK(!occupied && freed==1 && !lock_depth);
 }else if(!strcmp(c,"coalesce")){
  queue_repair(20);CHECK(domain_refs==2);flush_queue();CHECK(alloc_calls==1);
 }else if(!strcmp(c,"stop-before-queue")){
  domain.cm.hotplug_active=false;queue_repair(1);CHECK(!domain.cm.dp_tunnel_work.pending && domain_refs==1);flush_queue();CHECK(!alloc_calls && !pm_calls);
 }else if(!strcmp(c,"stop-after-queue")){
  queue_repair(1);domain.cm.hotplug_active=false;domain.tb.root_switch=NULL;flush_queue();CHECK(!alloc_calls && !pm_calls);
 }else if(!strcmp(c,"stop-during-pm")){
  queue_repair(1);stop_during_pm=1;flush_queue();CHECK(!alloc_calls && pm_calls==1);
 }else if(!strcmp(c,"no-root")){
  queue_repair(1);domain.tb.root_switch=NULL;flush_queue();CHECK(!alloc_calls && pm_calls==1);
 }else if(!strcmp(c,"pm-failure")){
  queue_repair(1);pm_error=-5;flush_queue();CHECK(!alloc_calls && pm_calls==1);
 }else if(!strcmp(c,"requeue-running")){
  queue_repair(1);requeue_once=1;flush_queue();CHECK(alloc_calls==2 && alloc_successes==2);
 }else if(!strcmp(c,"freeze-thaw-repair")){
  queue_repair(1);mutex_lock(&domain.tb.lock);CHECK(!tb_freeze_noirq(&domain.tb));mutex_unlock(&domain.tb.lock);
  flush_queue();CHECK(!alloc_calls && !pm_calls && domain.cm.dp_tunnel_pending);
  mutex_lock(&domain.tb.lock);CHECK(!tb_thaw_noirq(&domain.tb));mutex_unlock(&domain.tb.lock);
  CHECK(domain.cm.dp_tunnel_work.pending && domain_refs==2);flush_queue();CHECK(alloc_successes==1);
 }else if(!strcmp(c,"pm-failure-retry")){
  queue_repair(1);pm_error=-5;flush_queue();CHECK(!alloc_calls && domain.cm.dp_tunnel_pending);
  pm_error=0;mutex_lock(&domain.tb.lock);CHECK(!tb_thaw_noirq(&domain.tb));mutex_unlock(&domain.tb.lock);
  flush_queue();CHECK(alloc_successes==1 && pm_calls==2);
 }else if(!strcmp(c,"ordinary-retirement")){
  new_tunnel(&domain.tb,&in,&out);mutex_lock(&domain.tb.lock);
  tb_dp_resource_unavailable(&domain.tb,&in,"ordinary detach");mutex_unlock(&domain.tb.lock);
  CHECK(!occupied && freed==1 && !alloc_calls);flush_queue();CHECK(alloc_successes==1);
 }else if(!strcmp(c,"noirq-pm-error-complete-replay")){
  /* Generic host: completion replays the request the noirq pass could not serve. */
  freeze_then_thaw_with_pm_disabled();
  tb_complete(&domain.tb);
  CHECK(!lock_depth && !restore_calls && !scan_calls && domain.cm.dp_tunnel_work.pending && domain_refs==2);
  flush_queue();CHECK(alloc_calls==1 && alloc_successes==1 && pm_calls==2 && !domain.cm.dp_tunnel_pending);
 }else if(!strcmp(c,"apple-complete-acknowledges")){
  /* QUIRK_NO_SYSTEM_SLEEP pairs directly; that serves the request, so nothing is queued again. */
  nhi.quirks=QUIRK_NO_SYSTEM_SLEEP;
  freeze_then_thaw_with_pm_disabled();
  tb_complete(&domain.tb);
  CHECK(!lock_depth && restore_calls==1 && alloc_calls==1 && !domain.cm.dp_tunnel_pending);
  CHECK(!domain.cm.dp_tunnel_work.pending && domain_refs==1);
  flush_queue();CHECK(alloc_calls==1 && pm_calls==1);
 }else if(!strcmp(c,"complete-no-request")){
  tb_complete(&domain.tb);
  CHECK(!lock_depth && !domain.cm.dp_tunnel_work.pending && !alloc_calls && !restore_calls && domain_refs==1);
  flush_queue();CHECK(!alloc_calls && !pm_calls);
 }else if(!strcmp(c,"complete-xdomain-rescan-replay")){
  /* The rescan's scoped lock is dropped before the pairing lock is taken. */
  unplugged_xdomains=1;
  freeze_then_thaw_with_pm_disabled();
  tb_complete(&domain.tb);
  CHECK(!lock_depth && scan_calls==1 && domain.cm.dp_tunnel_work.pending);
  flush_queue();CHECK(alloc_successes==1 && !domain.cm.dp_tunnel_pending);
 }else return 78;
 printf("PASS %s\n",c);return 0;
}
'''

CASES = ['callback-release-before-repair', 'canceled-callback-does-not-repair', 'coalesce', 'stop-before-queue',
         'stop-after-queue', 'stop-during-pm', 'no-root', 'pm-failure',
         'requeue-running', 'ordinary-retirement', 'freeze-thaw-repair',
         'pm-failure-retry', 'noirq-pm-error-complete-replay',
         'apple-complete-acknowledges', 'complete-no-request',
         'complete-xdomain-rescan-replay']
# The cases a tb_complete() without the replay and the acknowledgement must fail.
COMPLETE_CASES = {'noirq-pm-error-complete-replay', 'apple-complete-acknowledges',
                  'complete-xdomain-rescan-replay'}
MUTANTS = {
    'missing-canceled-callback-guard': ('if (tunnel->dprx_canceled)', 'if (false)'),
    'inline-repair': ('\ttb_recalc_estimated_bandwidth(tb);\n\ttb_queue_dp_tunnels(tb);',
                      '\ttb_recalc_estimated_bandwidth(tb);\n\ttb_tunnel_dp(tb);'),
    'missing-queue-reference': ('\ttb_domain_get(tb);', '\t/* removed get */'),
    'coalescing-leaks-reference': ('\tif (!queue_work(tb->wq, &tcm->dp_tunnel_work))\n\t\ttb_domain_put(tb);',
                                 '\tqueue_work(tb->wq, &tcm->dp_tunnel_work);'),
    'missing-stop-guard': ('\tif (!READ_ONCE(tcm->hotplug_active))', '\tif (false)'),
    'missing-post-resume-guard': ('\tif (tcm->hotplug_active && tb->root_switch)', '\tif (tb->root_switch)'),
    'missing-root-guard': ('\tif (tcm->hotplug_active && tb->root_switch)', '\tif (tcm->hotplug_active)'),
    'missing-pm-error-guard': ('\tif (ret < 0)', '\tif (false)'),
    'missing-pm-put': ('\tpm_runtime_put_autosuspend(&tb->dev);', '\t/* removed PM put */'),
    'missing-probe-init': ('INIT_WORK(&tcm->dp_tunnel_work, tb_dp_tunnel_work);',
                           '/* removed probe initialization */'),
    'missing-pending-request': ('\ttcm->dp_tunnel_pending = true;', '\t/* lost request */'),
    'pending-never-consumed': ('if (tcm->hotplug_active && tb->root_switch) {\n\t\ttcm->dp_tunnel_pending = false;',
                               'if (tcm->hotplug_active && tb->root_switch) {\n\t\t/* pending forever */'),
    'missing-thaw-requeue': ('\ttcm->hotplug_active = true;\n\tif (tcm->dp_tunnel_pending)\n\t\ttb_queue_dp_tunnels(tb);',
                             '\ttcm->hotplug_active = true;\n\t/* lost thaw request */'),
    'missing-complete-replay': ('was disabled. */\n\tif (tcm->dp_tunnel_pending)\n\t\ttb_queue_dp_tunnels(tb);',
                                'was disabled. */\n\t/* lost completion replay */'),
    'missing-apple-ack': ('\t\ttb_restore_dp_resources(tb->root_switch);\n\t\ttcm->dp_tunnel_pending = false;',
                          '\t\ttb_restore_dp_resources(tb->root_switch);\n\t\t/* request not acknowledged */'),
    'unconditional-complete-replay': ('was disabled. */\n\tif (tcm->dp_tunnel_pending)\n\t\ttb_queue_dp_tunnels(tb);',
                                      'was disabled. */\n\ttb_queue_dp_tunnels(tb);'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutations', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--complete-ref', help='negative control: take only tb_complete() from '
                        'this commit; exactly the completion cases must fail')
    args = parser.parse_args()
    tb = (ROOT / 'drivers/thunderbolt/tb.c').read_text()
    tunnel = (ROOT / 'drivers/thunderbolt/tunnel.c').read_text()
    names = ['tb_tunnel_get', 'tb_tunnel_destroy', 'tb_tunnel_put']
    parts = {n: function(tunnel, n) for n in names}
    for name in ['tb_deactivate_and_free_tunnel', 'tb_dp_tunnel_work',
                 'tb_queue_dp_tunnels', 'tb_dp_resource_unavailable', 'tb_dp_tunnel_active',
                 'tb_freeze_noirq', 'tb_thaw_noirq', 'tb_complete']:
        parts[name] = function(tb, name)
    if args.complete_ref:
        old = subprocess.check_output(['git', 'show', args.complete_ref + ':drivers/thunderbolt/tb.c'],
                                      cwd=ROOT, text=True)
        parts['tb_complete'] = function(old, 'tb_complete')
    worker = function(tunnel, 'tb_dp_dprx_work')
    marker = '\ttunnel->callback(tunnel, tunnel->callback_data);'
    if worker.count(marker) != 1:
        raise ValueError('DPRX completion tail changed; review extraction')
    tail = worker[worker.index(marker):].rstrip()[:-1]
    if tail.strip() != 'tunnel->callback(tunnel, tunnel->callback_data);\n\ttb_tunnel_put(tunnel);\n\ttb_domain_put(tb);':
        raise ValueError('DPRX reference-release order changed; review extraction')
    production = '\n'.join(parts.values())
    probe = (ROOT / 'drivers/thunderbolt/tb.c').read_text()
    probe = probe[probe.index('struct tb *tb_probe(struct tb_nhi *nhi)'):]
    initializers = re.findall(r'INIT_WORK\(&tcm->\w+,\s*\w+\);', probe)
    production += '\nstatic void init_fixture(struct tb_cm*tcm){\n' + '\n'.join(initializers) + '\n}\n'
    results = {}
    with tempfile.TemporaryDirectory(prefix='tb-dp-repair-') as tmp:
        out = args.output or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        variants = {'real': production}
        if args.mutations and not args.complete_ref:
            for name, (old, new) in MUTANTS.items():
                if production.count(old) != 1:
                    raise ValueError('Mutation site changed: ' + name)
                variants[name] = production.replace(old, new, 1)
        for name, body in variants.items():
            unit = out / (name + '.c')
            unit.write_text(HEADER + '#include <string.h>\n' + body +
                            '\nstatic void worker_tail(struct tb*tb,struct tb_tunnel*tunnel){\n' +
                            tail + '\n}\n' + FOOTER)
            executable = out / name
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', '-Wno-unused-function',
                            '-Wno-unused-but-set-variable', '-g',
                            '-fsanitize=address,undefined', str(unit), '-o', str(executable)],
                           check=True)
            runs = []
            for case in CASES:
                run = subprocess.run([str(executable), case], capture_output=True, text=True)
                runs.append({'case': case, 'rc': run.returncode,
                             'stdout': run.stdout, 'stderr': run.stderr})
            failures = [r for r in runs if r['rc'] != 0]
            if args.complete_ref:
                failed = {r['case'] for r in failures}
                if failed != COMPLETE_CASES or any(r['rc'] != 77 for r in failures):
                    raise RuntimeError('Control did not fail exactly the completion cases: ' +
                                       json.dumps(sorted(failed)))
            elif name == 'real' and failures:
                raise RuntimeError(json.dumps(failures))
            if name != 'real' and not any(r['rc'] == 77 for r in failures):
                raise RuntimeError('Mutant not caught by a valid runtime assertion: ' + name)
            results[name] = runs
        receipt = {'scope': __doc__, 'production_bodies': {
            n: hashlib.sha256(body.encode()).hexdigest() for n, body in parts.items()},
            'worker_tail_sha256': hashlib.sha256(tail.encode()).hexdigest(),
            'probe_initializers': initializers,
            'complete_ref': args.complete_ref,
            'results': results}
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps({'cases': len(CASES), 'passed': True,
                          'complete_ref': args.complete_ref,
                          'control_failed': sorted(COMPLETE_CASES) if args.complete_ref else [],
                          'mutants_caught': len(variants) - 1, 'output': str(out)}))


if __name__ == '__main__':
    main()
