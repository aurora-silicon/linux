#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run production retained-stop and admission arithmetic across >8 epochs."""
from pathlib import Path
import subprocess
import tempfile
R = Path(__file__).resolve().parents[2]
s = (R / 'drivers/gpu/drm/apple/iomfb.c').read_text()
h = (R / 'drivers/gpu/drm/apple/dcpext26-lifecycle.h').read_text().replace('#include <linux/types.h>', '')
def function(signature):
    a = s.index(signature + '\n{')
    return s[a:s.index('\n}', a) + 2]
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint32_t u32; typedef uint64_t u64;
#define SWAP_SURFACES 4
#define guard(x) (void)
#define dev_dbg(...) ((void)0)
struct list_head { struct list_head *next,*prev; };
static void init(struct list_head *h){h->next=h->prev=h;}
static void list_add_tail(struct list_head *n,struct list_head *h){n->next=h;n->prev=h->prev;h->prev->next=n;h->prev=n;}
static void list_splice_tail_init(struct list_head *n,struct list_head *h){while(n->next!=n){struct list_head *v=n->next;n->next=v->next;v->next->prev=n;list_add_tail(v,h);}}
#define entry(p) ((struct iomfb_scanout_h17p *)((char *)(p)-offsetof(struct iomfb_scanout_h17p,link)))
#define list_for_each_entry(p,h,m) for(struct list_head *it=(h)->next;it!=(h)&&((p)=entry(it),1);it=it->next)
struct obj {u64 size;};
struct fb {struct obj *obj[1];};
struct iomfb_scanout_h17p {struct fb *fb[4];struct list_head link;};
struct apple_dcp {
 struct {int lock;void *active;struct iomfb_scanout_h17p *scanout,*next_scanout;struct list_head dropped;} iomfb;
 struct {bool pending;} present_state_h17p;struct {void *data;} chunks;
 bool ext26_control_pending,ext26_blanked,ext26_normal,ext26_early_complete;
 unsigned ext26_epochs;u64 ext26_retained_bytes;struct list_head ext26_retained;
};
static bool idle=true;
static bool iomfb_channels_idle(struct apple_dcp *d){(void)d;return idle;}
'''
a = s.index('\tif (dcp_is_ext26(dcp)) {', s.index('iomfb_scanout_prepare_h17p('))
b = s.index('\n\treturn scanout;', a)
admit = s[a:b]
code = pre + h + '\n' + function('static u64 ext26_scanout_bytes(struct iomfb_scanout_h17p *scanout)') + '\n' + function('int iomfb_ext26_retain_epoch(struct apple_dcp *dcp)') + r'''
static unsigned released;
static bool dcp_is_ext26(struct apple_dcp *d){return true;}
#define dev_warn_ratelimited(...) ((void)0)
static void iomfb_scanout_release_h17p(struct iomfb_scanout_h17p *s){released++;}
static struct iomfb_scanout_h17p *admission(struct apple_dcp *dcp,struct iomfb_scanout_h17p *scanout){
''' + admit + '\nreturn scanout;}\n' + r'''
int main(void){
 struct apple_dcp d={0};init(&d.iomfb.dropped);init(&d.ext26_retained);
 struct obj o={32ULL<<20};struct fb f={{&o}};struct iomfb_scanout_h17p frames[16]={0};
 for(unsigned i=0;i<12;i++){
   frames[i].fb[0]=&f;init(&frames[i].link);
   assert(admission(&d,&frames[i]));d.iomfb.scanout=&frames[i];
   assert(!iomfb_ext26_retain_epoch(&d));assert(!d.iomfb.scanout);
 }
 assert(d.ext26_epochs==12&&d.ext26_retained_bytes==(384ULL<<20));
 frames[12].fb[0]=&f;init(&frames[12].link);
 d.ext26_retained_bytes=960ULL<<20;
 assert(!admission(&d,&frames[12])&&released==1); /* unexposed: safe put */
 d.iomfb.scanout=&frames[12]; /* simulate previously exposed backing at limit */
 assert(!iomfb_ext26_retain_epoch(&d)); /* stop never strands route on quota */
 assert(d.ext26_retained_bytes==(992ULL<<20));
 assert(!dcpext26_backing_fits(~0ULL,1));
 assert(dcpext26_backing_fits((960ULL<<20)-1,1));
 assert(!dcpext26_backing_fits(960ULL<<20,1));
 idle=false;assert(iomfb_ext26_retain_epoch(&d)==-EBUSY);
 idle=true;d.ext26_control_pending=true;assert(iomfb_ext26_retain_epoch(&d)==-EBUSY);
}
'''
with tempfile.TemporaryDirectory() as t:
    p = Path(t)/'test.c'
    p.write_text(code)
    subprocess.run(['clang','-std=gnu11','-Wall','-Werror','-Wno-unused-function',
                    '-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
print('PASS production budget admission and retention: 12 epochs, byte limit, overflow, busy fences')
