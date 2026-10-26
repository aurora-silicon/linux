#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real cross-endpoint callback dispatch with explicit lock/work doubles."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from common import block

ROOT = Path(__file__).resolve().parents[4]


def main():
    print("TAP version 13\n1..1", flush=True)
    if not shutil.which("cc"):
        print("ok 1 - DCP callback ownership # SKIP cc unavailable")
        return 4
    source = (ROOT / "drivers/gpu/drm/apple/iomfb.c").read_text()
    program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8; typedef uint32_t u32;
struct list_head { struct list_head *next,*prev; };
#define INIT_LIST_HEAD(h) ((h)->next=(h),(h)->prev=(h))
static int list_empty(struct list_head*h){return h->next==h;}
static void list_add_tail(struct list_head*n,struct list_head*h){n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;}
static void list_del(struct list_head*n){n->prev->next=n->next;n->next->prev=n->prev;}
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define list_first_entry(h,t,m) container_of((h)->next,t,m)
struct lock { bool held; };
static void lock(struct lock*l){assert(!l->held);l->held=true;}
static void unlock(struct lock*l){assert(l->held);l->held=false;}
#define mutex_lock(l) lock(l)
#define mutex_unlock(l) unlock(l)
#define spin_lock_irqsave(l,f) ((f)=0,lock(l))
#define spin_unlock_irqrestore(l,f) unlock(l)
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v,x) ((v)=(x))
#define GFP_KERNEL 0
#define struct_size(p,m,n) (sizeof(*(p))+(n)*sizeof((p)->m[0]))
static int allocations;static bool allocation_fail;
static void*kzalloc(size_t n,int flags){if(allocation_fail)return NULL;void*p=calloc(1,n);if(p)allocations++;return p;}
static void kfree(void*p){if(p){allocations--;free(p);}}
struct work_struct {bool scheduled;};
static void schedule_work(struct work_struct*w){w->scheduled=true;}
static void cancel_work_sync(struct work_struct*w){w->scheduled=false;}
static void cancel_delayed_work_sync(struct work_struct*w){w->scheduled=false;}
struct apple_dcp;
typedef void(*dcp_callback_t)(struct apple_dcp*,void*,void*);
static void*current=(void*)1;
struct apple_dcp{bool crashed,retained_active,retained_scanout; int dev; struct work_struct vblank_wq;struct{struct lock lock,callback_lock;struct list_head callbacks;unsigned callback_count;bool stopped;void*owner;struct work_struct callback_work,work,timeout,backlight_retry;}iomfb;};
#define lockdep_assert_held(l) assert((l)->held)
#define dev_err(...) ((void)0)
static void dcp_peer_swap_failed_v27(struct apple_dcp*d){}
static void dcp_swap_watchdog_complete(struct apple_dcp*d){}
static void dcp_present_failed(struct apple_dcp*d){}
static bool iomfb_uses_queue(struct apple_dcp*d){return true;}
static void iomfb_discard_pending(struct apple_dcp*d){assert(d->iomfb.lock.held);}
static void iomfb_queue_advance(struct apple_dcp*d){assert(d->iomfb.lock.held);assert(!d->iomfb.owner);}
'''
    program += block(source, r"^struct iomfb_callback") + ";\n"
    for name in ["iomfb_queue_callback", "iomfb_pop_callback", "iomfb_callback_work", "iomfb_queue_stop"]:
        program += block(source, r"^(?:static )?(?:int|void|struct iomfb_callback \*)\s*" + name + r"\(")
    peer = (ROOT / "drivers/gpu/drm/apple/iomfb-peer-runtime-v27.inc").read_text()
    program += block(peer, r"^static void dcp_peer_swap_error\(")
    program += r'''
static int received[128],count;
static void receive(struct apple_dcp*d,void*data,void*cookie){assert(d->iomfb.lock.held);assert(d->iomfb.owner==current);assert(!d->iomfb.callback_lock.held);received[count++]=*(int*)data;}
static void init(struct apple_dcp*d){memset(d,0,sizeof(*d));INIT_LIST_HEAD(&d->iomfb.callbacks);}
int main(void){
 struct apple_dcp d;init(&d);int a=7,b=8;
 assert(!iomfb_queue_callback(&d,receive,&a,sizeof(a),NULL));
 assert(!iomfb_queue_callback(&d,receive,&b,sizeof(b),NULL));
 assert(!d.iomfb.lock.held&&count==0&&d.iomfb.callback_work.scheduled);
 a=99;iomfb_callback_work(&d.iomfb.callback_work);
 assert(count==2&&received[0]==7&&received[1]==8&&allocations==0);
 assert(!d.iomfb.lock.held&&!d.iomfb.owner);
 assert(iomfb_queue_callback(&d,NULL,&a,sizeof(a),NULL)==-EINVAL);
 assert(iomfb_queue_callback(&d,receive,NULL,sizeof(a),NULL)==-EINVAL);
 assert(iomfb_queue_callback(&d,receive,&a,65,NULL)==-EINVAL);
 allocation_fail=true;assert(iomfb_queue_callback(&d,receive,&a,sizeof(a),NULL)==-ENOMEM);allocation_fail=false;
 for(int i=0;i<64;i++)assert(!iomfb_queue_callback(&d,receive,&i,sizeof(i),NULL));
 assert(d.iomfb.callback_count==64&&allocations==64);
 assert(iomfb_queue_callback(&d,receive,&a,sizeof(a),NULL)==-EBUSY&&allocations==64);
 iomfb_queue_stop(&d);assert(d.iomfb.stopped&&d.iomfb.callback_count==0&&allocations==0&&count==2);
 assert(iomfb_queue_callback(&d,receive,&a,sizeof(a),NULL)==-ESHUTDOWN&&allocations==0);
 init(&d);assert(!iomfb_queue_callback(&d,receive,&a,sizeof(a),NULL));d.crashed=true;
 iomfb_callback_work(&d.iomfb.callback_work);assert(allocations==0&&count==2);
 init(&d);d.retained_active=d.retained_scanout=true;
 mutex_lock(&d.iomfb.lock);dcp_peer_swap_error(&d,"peer ACK",1);mutex_unlock(&d.iomfb.lock);
 assert(d.crashed&&d.retained_active&&d.retained_scanout&&d.vblank_wq.scheduled);
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="dcp-callbacks-") as tmp:
        src, binary = Path(tmp) / "test.c", Path(tmp) / "test"
        src.write_text(program)
        subprocess.run(["cc", "-std=gnu11", "-g", "-fsanitize=address,undefined",
                        str(src), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("ok 1 - DCP copied callbacks, owner locking, bounds and stop retention")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
