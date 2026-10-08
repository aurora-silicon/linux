#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""Native regression for rejected brightness transactions and bounded retries.

Executes actual reservation, failure, queue-drain, retry-work and completion
functions with mocked scheduling and transport. Does not submit firmware RPCs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--cc', default='clang')
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
src = root / 'drivers/gpu/drm/apple'
a.output.mkdir(parents=True, exist_ok=True)


def function(file, name):
    text = (src / file).read_text()
    match = re.search(r'^\w[\w\s*]*?\b' + name + r'\(', text, re.M)
    assert match, name
    start = text.index('{', match.end())
    pos, depth = start + 1, 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[match.start():pos]


policy = re.sub(r'^#include <linux/[^>]+>\n', '',
                (src / 'dcp_backlight.h').read_text(), flags=re.M)
functions = '\n'.join(function(f, n) for f, n in (
    ('dcp_backlight.c', 'dcp_backlight_complete'),
    ('dcp_backlight.c', 'dcp_backlight_pending'),
    ('dcp_backlight.c', 'dcp_backlight_retry_delay'),
    ('iomfb.c', 'iomfb_present_failed_h17p'),
    ('iomfb.c', 'iomfb_present_complete_h17p'),
    ('iomfb.c', 'iomfb_queue_advance'), ('iomfb.c', 'iomfb_backlight_retry')))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define lockdep_assert_held(x) ((void)(x))
#define spin_lock_irqsave(x,f) do { (void)(x);(f)=0; } while(0)
#define spin_unlock_irqrestore(x,f) do { (void)(x);(void)(f); } while(0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define to_delayed_work(p) container_of(p,struct delayed_work,work)
#define msecs_to_jiffies(x) (x)
#define IOMFB_OPAQUE_X_WAITING 0
#define IOMFB_OPAQUE_X_NEEDED 1
struct work_struct {int id;};struct delayed_work {struct work_struct work;};
struct list_head {int empty;};
struct iomfb_transaction {
 struct dcp_backlight_present backlight;
 bool backlight_reserved,backlight_failed,brightness_only,completed;
 void (*release)(struct iomfb_transaction *);
};
struct apple_dcp {
 bool crashed;void *dev;struct work_struct bl_update_wq,vblank_wq;
 struct {int lock;struct dcp_backlight_state state;} backlight;
 struct {bool pending;} present_state_h17p;
 struct {int lock;bool stopped,backlight_queued;struct iomfb_transaction *active;
  struct delayed_work timeout,backlight_retry;struct work_struct work;
  struct list_head pending;int opaque_x_state;
 } iomfb;
};
static unsigned delay,scheduled,released,discarded;static bool idle=true;
static void *system_wq;
static bool iomfb_channels_idle(struct apple_dcp *d) {(void)d;return idle;}
static int iomfb_enqueue_opaque_x(struct apple_dcp *d) {(void)d;return 0;}
static void iomfb_discard_pending(struct apple_dcp *d) {(void)d;discarded++;}
static bool list_empty(struct list_head *l) {return l->empty;}
static void cancel_delayed_work(struct delayed_work *w) {(void)w;}
static void schedule_work(struct work_struct *w) {assert(w->id==1);scheduled++;}
static void mod_delayed_work(void *q,struct delayed_work *w,unsigned ms) {
 (void)q;assert(w->work.id==2);delay=ms;
}
static void dev_warn_ratelimited(void *d,const char *msg) {(void)d;(void)msg;}
static void iomfb_scanout_complete_h17p(struct apple_dcp *d) {(void)d;}
static void atomic_cmpxchg(int *p,int old,int value) {if(*p==old)*p=value;}
static void release(struct iomfb_transaction *t) {(void)t;released++;}
'''
# The policy header defines structs before the fixture needs them.
prefix = prefix[:prefix.index('struct work_struct')] + policy + prefix[prefix.index('struct work_struct'):]
main = r'''
static void init(struct apple_dcp *d) {
 u32 inherited;
 memset(d,0,sizeof(*d));d->bl_update_wq.id=1;d->iomfb.backlight_retry.work.id=2;
 assert(!dcp_bl_takeover_nits(509,36000,&inherited)&&inherited==36);
 d->iomfb.pending.empty=1;assert(!dcp_bl_init(&d->backlight.state,509,true,inherited,false,0));
 assert(dcp_bl_seed(&d->backlight.state,36));assert(!dcp_bl_dpms(&d->backlight.state,true));
 assert(!dcp_bl_request(&d->backlight.state,140,false,false));
 delay=scheduled=released=discarded=0;idle=true;
}
static void reserve(struct apple_dcp *d,struct iomfb_transaction *t) {
 memset(t,0,sizeof(*t));t->brightness_only=true;t->release=release;
 assert(!dcp_bl_prepare(&d->backlight.state,true,&t->backlight));
 t->backlight_reserved=true;d->iomfb.active=t;d->iomfb.backlight_queued=true;
}
static void reject(struct apple_dcp *d) {
 iomfb_present_failed_h17p(d);d->present_state_h17p.pending=false;
 iomfb_queue_advance(d);
}
int main(void) {
 struct apple_dcp d;struct iomfb_transaction t;
 init(&d);reserve(&d,&t);reject(&d);
 assert(delay==100&&!d.iomfb.active&&!d.iomfb.backlight_queued&&released==1);
 assert(d.backlight.state.actual==36&&d.backlight.state.dirty&&!t.completed);
 /* The deferred worker retries on retained pixels, with NO new user request. */
 iomfb_backlight_retry(&d.iomfb.backlight_retry.work);assert(scheduled==1);
 reserve(&d,&t);assert(!iomfb_present_complete_h17p(&d));iomfb_queue_advance(&d);
 assert(d.backlight.state.actual==140&&!d.backlight.state.dirty&&!d.backlight.state.retries);
 init(&d);
 for(unsigned i=0;i<4;i++) {
  reserve(&d,&t);delay=0;reject(&d);assert(delay==(i<3?(100U<<i):0));
 }
 assert(d.backlight.state.dirty&&d.backlight.state.retries==4&&!d.iomfb.active);
 /* A new user request re-arms the budget and can complete. */
 assert(!dcp_bl_request(&d.backlight.state,36,false,false));assert(!d.backlight.state.retries);
 reserve(&d,&t);reject(&d);assert(delay==100);
 for(unsigned stopped=0;stopped<2;stopped++) {
  init(&d);reserve(&d,&t);iomfb_present_failed_h17p(&d);
  d.crashed=!stopped;d.iomfb.stopped=stopped;delay=scheduled=0;
  iomfb_queue_advance(&d);iomfb_backlight_retry(&d.iomfb.backlight_retry.work);
  assert(!delay&&!scheduled&&discarded==1);
 }
 /* A channel that has not drained must not release/schedule a retry. */
 init(&d);reserve(&d,&t);iomfb_present_failed_h17p(&d);idle=false;
 iomfb_queue_advance(&d);assert(!delay&&!released&&d.iomfb.active==&t);
 idle=true;iomfb_queue_advance(&d);assert(delay==100&&released==1);
 puts("PASS: rejected A408 retries then completes without a new request; 100/200/400ms budget; new-target rearm; stopped/crashed and undrained-channel guards");
 return 0;
}
'''
code = a.output / 'backlight-retry.c'
code.write_text(prefix + functions + main)
command = [a.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
           '-Wno-unused-parameter', '-fsanitize=address,undefined', str(code),
           '-o', str(a.output / 'backlight-retry')]
subprocess.run(command, check=True)
result = subprocess.check_output([str(a.output / 'backlight-retry')], text=True)
(a.output / 'receipt.json').write_text(json.dumps(dict(command=command, result=result,
    functions_sha256=hashlib.sha256(functions.encode()).hexdigest(),
    policy_sha256=hashlib.sha256(policy.encode()).hexdigest(),
    scope='Native mocked scheduling/transport; not firmware acceptance'), indent=2) + '\n')
print(result)
