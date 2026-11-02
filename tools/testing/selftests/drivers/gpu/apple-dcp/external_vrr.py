# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright 2026 Ryan Murray
"""H17P Variable-mode callbacks against a delayed, fallible RPC transport."""
from common import block
import rate


def publication(root):
    unit = rate.publication(root)['unit']
    old = block(unit, 'static void drm_connector_set_vrr_capable_property(')
    unit = unit.replace(old, '''static bool vrr_reported;
static void drm_connector_set_vrr_capable_property(struct drm_connector *c,bool capable)
{ (void)c;vrr_reported=capable; }''')
    checks = r'''
static void external_ranges(void) {
    reset();callbacks(false);callbacks(true);timings();admit(0);
    d.modes[0].vrr=true;d.modes[0].min_vrr=48U<<16;d.modes[0].max_vrr=60U<<16;
    con.base.display_info.monitor_range.min_vfreq=48;
    con.base.display_info.monitor_range.max_vfreq=280;
    probe(true);
    assert(vrr_reported && con.base.display_info.monitor_range.min_vfreq==48);
    assert(con.base.display_info.monitor_range.max_vfreq==60);
    /* Reprobe after a link/catalog change must also allow a higher ceiling. */
    d.modes[0].max_vrr=144U<<16;probe(true);
    assert(vrr_reported && con.base.display_info.monitor_range.max_vfreq==144);
    d.modes[0].vrr=false;probe(true);assert(!vrr_reported);
    cleanup();
}
'''
    unit = unit.replace('int main(int argc,char **argv) {',
                        checks + 'int main(int argc,char **argv) {')
    return unit.replace('    transport_edid=raw;',
                        '    transport_edid=raw;external_ranges();', 1)


def callbacks(root):
    source = (root / 'drivers/gpu/drm/apple/iomfb_template.c').read_text()
    functions = '\n'.join(block(source, marker) for marker in (
        'static void dcp_external_vrr_done(',
        'static bool dcp_external_vrr_update(',
        'static void complete_set_digital_out_mode(',
        'static void dcp_external_modeset_complete(',
        'static void dcp_on_external_modeset(',
    ))
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef uint32_t u32;
#define WRITE_ONCE(x,v) ((x)=(v))
#define DCP_IOMFB_METHODS_H17P_EXTERNAL 2
struct apple_dcp;
typedef void (*dcp_callback_t)(struct apple_dcp *, void *, void *);
struct apple_dcp {
    struct { int iomfb_method_profile; } hw;
    bool vrr_enabled;
    struct { bool requested,savings_valid;u32 savings;dcp_callback_t next;void *cookie; } external_vrr;
};
struct dcp_wait_cookie { u32 status;bool replied;int done,refcount; };
static void complete(int *done) { (*done)++; }
static void release_wait_cookie(int *ref) { assert(*ref==0); }
static void kref_put(int *ref,void (*release)(int *)) { if (!--*ref) release(ref); }
static dcp_callback_t reply;
static void *reply_cookie;
static unsigned calls,continued;
static u32 argument;
static void dcp_enable_disable_video_power_savings(struct apple_dcp *d,bool oob,
    u32 *savings,dcp_callback_t next,void *cookie)
{
    assert(!oob && !reply);
    calls++;argument=*savings;reply=next;reply_cookie=cookie;
}
static void after(struct apple_dcp *d,void *data,void *cookie)
{
    assert(!data && cookie==&continued);
    assert(!d->external_vrr.next && !d->external_vrr.cookie);
    continued++;
}
static void respond(struct apple_dcp *d,int *status)
{
    dcp_callback_t next=reply;void *cookie=reply_cookie;
    assert(next);reply=NULL;reply_cookie=NULL;next(d,status,cookie);
}
''' + functions + r'''
int main(void)
{
    struct apple_dcp d={.hw={DCP_IOMFB_METHODS_H17P_EXTERNAL},.vrr_enabled=true};
    int ok=0,error=-1;
    /* Idle and fullscreen transitions: RPC completes before the swap continuation. */
    assert(dcp_external_vrr_update(&d,false,after,&continued));
    assert(argument==1 && calls==1 && !continued && !d.external_vrr.savings_valid);
    respond(&d,&ok);assert(continued==1 && d.external_vrr.savings_valid);
    assert(!dcp_external_vrr_update(&d,false,after,&continued) && calls==1);
    assert(dcp_external_vrr_update(&d,true,after,&continued) && argument==0);
    respond(&d,&error);assert(continued==2 && !d.external_vrr.savings_valid);
    assert(dcp_external_vrr_update(&d,true,after,&continued));
    respond(&d,NULL);assert(continued==3 && !d.external_vrr.savings_valid);
    assert(dcp_external_vrr_update(&d,true,after,&continued));
    respond(&d,&ok);assert(continued==4 && d.external_vrr.savings_valid);
    assert(!dcp_external_vrr_update(&d,true,after,&continued));
    assert(dcp_external_vrr_update(&d,false,after,&continued) && argument==1);
    respond(&d,&ok);assert(continued==5);
    unsigned before=calls;
    d.vrr_enabled=false;assert(!dcp_external_vrr_update(&d,true,after,&continued));
    d.vrr_enabled=true;
    for(int profile=0;profile<2;profile++) {
        d.hw.iomfb_method_profile=profile;
        assert(!dcp_external_vrr_update(&d,true,after,&continued));
    }
    assert(calls==before);d.hw.iomfb_method_profile=DCP_IOMFB_METHODS_H17P_EXTERNAL;
    /* A modeset receipt stays pinned through the following video-savings RPC. */
    struct dcp_wait_cookie wait={.status=~0U,.refcount=2};
    u32 result=0x1234;
    d.external_vrr.requested=true;d.external_vrr.savings_valid=false;
    dcp_on_external_modeset(&d,&result,&wait);
    assert(!wait.done && wait.refcount==2 && argument==0);
    respond(&d,&ok);
    assert(wait.done==1 && wait.replied && wait.status==result && wait.refcount==1);
    /* A timed-out waiter may drop its own ref before the delayed RPC reply. */
    wait=(struct dcp_wait_cookie){.status=~0U,.refcount=2};
    d.external_vrr.savings_valid=false;d.external_vrr.requested=false;
    dcp_on_external_modeset(&d,&result,&wait);assert(argument==1);
    kref_put(&wait.refcount,release_wait_cookie);
    respond(&d,&error);assert(wait.done==1 && wait.refcount==0 && wait.status==result);
    return 0;
}
'''
