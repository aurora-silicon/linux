#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise production queue, preparation and reference release with mock DRM/RPCs."""
from pathlib import Path
import subprocess
import tempfile
r = Path(__file__).resolve().parents[2]
s = (r / 'drivers/gpu/drm/apple/iomfb.c').read_text()
h = (r / 'drivers/gpu/drm/apple/dcpext26-lifecycle.h').read_text().replace('#include <linux/types.h>', '')
def function(name):
    a = s.index(name + '(')
    if name == 'ext26_scanout_bytes':
        a = s.rindex('static u64 ext26_scanout_bytes(')
    else:
        a = s.rfind('\n', 0, a) + 1
    b = s.index('\n}', s.index('{', a)) + 2
    text = s[a:b].replace('VISIBLE_IF_KUNIT ', '')
    if name == 'iomfb_scanout_prepare_h17p':
        text = 'static struct iomfb_scanout_h17p *\n' + text
    return text
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
typedef uint32_t u32; typedef uint64_t u64;
#define SWAP_SURFACES 4
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define guard(x) (void)
#define lockdep_assert_held(x) ((void)0)
#define mutex_lock(x) ((void)0)
#define mutex_unlock(x) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct list_head { struct list_head *next,*prev; };
static void INIT_LIST_HEAD(struct list_head *h){h->next=h->prev=h;}
static bool list_empty(struct list_head *h){return h->next==h;}
static void list_add_tail(struct list_head *n,struct list_head *h){n->next=h;n->prev=h->prev;h->prev->next=n;h->prev=n;}
static void list_del(struct list_head *n){n->prev->next=n->next;n->next->prev=n->prev;INIT_LIST_HEAD(n);}
#define list_first_entry(h,t,m) container_of((h)->next,t,m)
#define list_for_each_entry(p,h,m) for(struct list_head *it=(h)->next;it!=(h)&&((p)=container_of(it,typeof(*p),m),1);it=it->next)
#define list_for_each_entry_safe(p,n,h,m) for(struct list_head *it=(h)->next,*next;it!=(h)&&((next=it->next),(p)=container_of(it,typeof(*p),m),1);it=next)
static void list_splice_tail_init(struct list_head *n,struct list_head *h){while(!list_empty(n)){struct list_head *v=n->next;list_del(v);list_add_tail(v,h);}}
static int fail_alloc;
static void *alloc(size_t n){if(fail_alloc){fail_alloc--;return NULL;}return calloc(1,n);}
#define kzalloc_obj(x) alloc(sizeof(x))
#define kfree(x) free(x)
struct obj {u64 size;};
struct drm_framebuffer {struct obj *obj[1];int refs;};
static void drm_framebuffer_get(struct drm_framebuffer *f){assert(f->refs>0);f->refs++;}
static void drm_framebuffer_put(struct drm_framebuffer *f){assert(f->refs>1);f->refs--;}
struct drm_crtc {int unused;};
struct drm_plane {unsigned iomfb_surf;};
#define to_apple_plane(p) (p)
struct drm_plane_state {struct drm_crtc *crtc;bool visible;struct drm_framebuffer *fb;};
struct drm_atomic_commit {int refs,count;struct drm_plane plane[4];struct drm_plane_state old[4],new[4];};
#define for_each_oldnew_plane_in_state(s,p,o,n,i) for(i=0;i<(s)->count&&((p)=&(s)->plane[i],(o)=&(s)->old[i],(n)=&(s)->new[i],1);i++)
static struct drm_atomic_commit *drm_atomic_commit_get(struct drm_atomic_commit *s){s->refs++;return s;}
static void drm_atomic_commit_put(struct drm_atomic_commit *s){assert(s->refs>1);s->refs--;}
struct work_struct {int scheduled;};
static void schedule_work(struct work_struct *w){w->scheduled++;}
#define cancel_delayed_work(x) ((void)0)
#define mod_delayed_work(...) ((void)0)
#define msecs_to_jiffies(x) (x)
#define current NULL
static unsigned long jiffies;
struct apple_dcp;
struct iomfb_transaction {struct list_head link;void (*start)(struct apple_dcp *,struct iomfb_transaction *);void (*release)(struct iomfb_transaction *);bool brightness_only,presentation,completed,backlight_failed;};
struct iomfb_scanout_h17p {int request;struct drm_framebuffer *fb[4];struct list_head link;};
struct apple_dcp {
 struct {int lock,timeout,backlight_retry,callback_count;void *owner;unsigned long deadline;unsigned queued;bool presentation_paused,backlight_queued,stopped;struct work_struct work;struct list_head pending,dropped;struct iomfb_transaction *active;struct iomfb_scanout_h17p *scanout,*next_scanout;} iomfb;
 struct {bool pending;} present_state_h17p;struct {bool valid;} mode_state;struct {bool connected;} *connector;
 struct {void *data;} chunks;bool native,crashed,ext26_control_pending,ext26_blanked,ext26_normal,ext26_early_complete;
 unsigned ext26_epochs;u64 ext26_retained_bytes;struct list_head ext26_retained;
 int fw_compat;struct work_struct vblank_wq,bl_update_wq;
};
#define DCP_FIRMWARE_V_27_0 27
static bool dcp_is_ext26(struct apple_dcp *d){return d->native;}
static bool iomfb_channels_idle(struct apple_dcp *d){return true;}
static bool dcp_backlight_pending(struct apple_dcp *d){return false;}
static unsigned dcp_backlight_retry_delay(struct apple_dcp *d){return 0;}
static int iomfb_enqueue_opaque_x(struct apple_dcp *d){return 0;}
static unsigned flushed;
static void iomfb_flush_h17p(struct apple_dcp *d,struct drm_crtc *c,struct drm_atomic_commit *s){assert(d->iomfb.next_scanout);flushed++;d->present_state_h17p.pending=true;}
#define iomfb_flush_v27 iomfb_flush_h17p
'''
names = ['ext26_scanout_bytes', 'iomfb_scanout_release_h17p',
         'iomfb_scanout_prepare_h17p', 'iomfb_scanout_put_h17p',
         'iomfb_ext26_retain_epoch', 'iomfb_discard_pending',
         'iomfb_queue_advance', 'iomfb_queue_work', 'iomfb_queue']
code = pre + h + '\n' + '\n'.join(function(n) for n in names)
a = s.index('struct iomfb_atomic_transaction {')
b = s.index('\nvoid dcp_flush(', a)
code += '\n' + s[a:b]
code += r'''
static void setup(struct apple_dcp *d){
 INIT_LIST_HEAD(&d->iomfb.pending);INIT_LIST_HEAD(&d->iomfb.dropped);INIT_LIST_HEAD(&d->ext26_retained);
 d->native=true;d->mode_state.valid=true;
 static typeof(*d->connector) conn={.connected=true};d->connector=&conn;
}
int main(void){
 struct apple_dcp d={0};setup(&d);struct drm_crtc c={0};
 struct obj o={32ULL<<20};struct drm_framebuffer fb={.obj={&o},.refs=1};
 struct drm_atomic_commit s={.refs=1,.count=1,.new={{.crtc=&c,.visible=true,.fb=&fb}}};
 for(unsigned i=0;i<30;i++){
  iomfb_queue_atomic(&d,&c,&s);assert(s.refs==2&&d.iomfb.queued==1);
  iomfb_queue_work(&d.iomfb.work);assert(d.present_state_h17p.pending&&flushed==i+1);
  /* Mock the firmware completion publication; the queue/release bodies are real. */
  d.iomfb.scanout=d.iomfb.next_scanout;d.iomfb.next_scanout=NULL;
  d.present_state_h17p.pending=false;iomfb_queue_advance(&d);
  assert(!d.iomfb.active&&s.refs==1&&fb.refs==(int)i+2);
  assert(!iomfb_ext26_retain_epoch(&d));
 }
 assert(d.ext26_epochs==30&&d.ext26_retained_bytes==(960ULL<<20));
 iomfb_queue_atomic(&d,&c,&s);iomfb_queue_work(&d.iomfb.work);
 assert(!d.crashed&&!d.iomfb.active&&!d.iomfb.next_scanout&&!d.iomfb.queued);
 assert(s.refs==1&&fb.refs==31&&flushed==30&&d.vblank_wq.scheduled);
 /* Partial prepare replaces inherited refs, then rejects without exposing. */
 struct iomfb_scanout_h17p *old=calloc(1,sizeof(*old));INIT_LIST_HEAD(&old->link);
 old->fb[0]=&fb;drm_framebuffer_get(&fb);d.iomfb.scanout=old;
 iomfb_queue_atomic(&d,&c,&s);iomfb_queue_work(&d.iomfb.work);
 assert(!d.crashed&&fb.refs==32&&s.refs==1&&d.iomfb.scanout==old&&flushed==30);
 d.iomfb.scanout=NULL;iomfb_scanout_release_h17p(old);
 /* Allocation failure at preparation: native survives; classic retains fatal policy. */
 for(unsigned native=0;native<2;native++){
  struct apple_dcp x={0};setup(&x);x.native=native;
  iomfb_queue_atomic(&x,&c,&s);fail_alloc=1;iomfb_queue_work(&x.iomfb.work);
  assert(x.crashed==!native&&x.vblank_wq.scheduled&&!x.iomfb.next_scanout);
  if(x.iomfb.active){x.iomfb.active->release(x.iomfb.active);x.iomfb.active=NULL;}
  assert(s.refs==1&&fb.refs==31);
 }
 /* Exposed failed presents remain retained, not put. */
 struct apple_dcp x={0};setup(&x);
 iomfb_queue_atomic(&x,&c,&s);iomfb_queue_work(&x.iomfb.work);
 x.present_state_h17p.pending=false;iomfb_queue_advance(&x);
 assert(x.ext26_retained_bytes==(32ULL<<20)&&fb.refs==32&&!x.iomfb.next_scanout);
 /* Fixture-only teardown; this is not a production DMA-retirement claim. */
 struct iomfb_scanout_h17p *f,*n;
 list_for_each_entry_safe(f,n,&x.ext26_retained,link){list_del(&f->link);iomfb_scanout_release_h17p(f);}
 list_for_each_entry_safe(f,n,&d.ext26_retained,link){list_del(&f->link);iomfb_scanout_release_h17p(f);}
 assert(fb.refs==1&&s.refs==1);
}
'''
with tempfile.TemporaryDirectory() as t:
    p = Path(t) / 'queue.c'
    p.write_text(code)
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-variable',
                    '-fsanitize=address,undefined', str(p), '-o', str(p.with_suffix(''))], check=True)
    subprocess.run([str(p.with_suffix(''))], check=True)
print('PASS production queue/start/prepare/advance/release: 30 epochs, budget refusal, inherited refs, native/classic allocation failure, exposed failed present')
