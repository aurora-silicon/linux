#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the real early-HPD helper with GPIO/RemotePort leaves modelled.

No kernel or hardware executes. Tests that an inherited HDMI link can receive
its training trigger before the waiter, plus detach/read/RPC failure behavior
and the existing Type-C, tunnel and late-HPD paths.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[5]
source = Path(os.environ.get('DCP_SOURCE', root / 'drivers/gpu/drm/apple/dcp.c')).read_text()
a = source.index('static int dcp_assert_hpd_before_link_wait(')
b = source.index('\n}\n', a) + 3
body = source[a:b]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
#define DRM_MODE_CONNECTOR_HDMIA 11
struct apple_dcp {
 int fixed_connector_type; void *hdmi_hpd;
 bool typec, tunnel; struct {void *service;} dptxport[2];
};
static int level=1, rpc_error, samples, calls, timeout_ms;
static bool link_configured;
static bool dcp_is_typec_output(struct apple_dcp*d){return d->typec;}
static bool dcp_uses_t6020_tunnel_flow(struct apple_dcp*d){return d->tunnel;}
static int gpiod_get_value_cansleep(void*p){assert(p);samples++;return level;}
static int dptxport_set_hpd(void*p,bool high)
{assert(p==(void*)123&&high);calls++;if(!rpc_error)link_configured=true;return rpc_error;}
static int dptxport_set_hpd_timeout(void*p,bool high,unsigned ms)
{timeout_ms=ms;return dptxport_set_hpd(p,high);}
'''
cases = r'''
int main(void)
{
 struct apple_dcp d={.fixed_connector_type=DRM_MODE_CONNECTOR_HDMIA,.hdmi_hpd=(void*)1,
                    .dptxport={{(void*)123},{0}}};
 /* Model the observed inherited-display dependency: without AP HPD the
  * request is accepted but the link waiter receives no training event. */
 assert(!link_configured);assert(!dcp_assert_hpd_before_link_wait(&d,0,false,false));
 assert(link_configured&&samples==1&&calls==1&&!timeout_ms);
 calls=samples=0;link_configured=false;level=0;
 assert(dcp_assert_hpd_before_link_wait(&d,0,false,false)==-ENOLINK);
 assert(samples==1&&!calls&&!link_configured);
 level=-EIO;samples=0;
 assert(dcp_assert_hpd_before_link_wait(&d,0,false,false)==-EIO);
 assert(samples==1&&!calls);
 level=1;rpc_error=-ETIMEDOUT;
 assert(dcp_assert_hpd_before_link_wait(&d,0,false,false)==-ETIMEDOUT);
 assert(!link_configured);rpc_error=0;
 calls=samples=0;
 assert(!dcp_assert_hpd_before_link_wait(&d,0,true,false));assert(!calls&&!samples);
 /* 13.5 DP2HDMI and adopted sessions keep their own HPD owner. */
 assert(!dcp_assert_hpd_before_link_wait(&d,0,false,true));assert(!calls&&!samples);
 d.fixed_connector_type=0;
 assert(!dcp_assert_hpd_before_link_wait(&d,0,false,false));assert(!calls&&!samples);
 d.fixed_connector_type=DRM_MODE_CONNECTOR_HDMIA;
 /* A borrowed Type-C route on an HDMI-capable pipe must not sample HDMI. */
 d.typec=true;level=0;calls=samples=0;
 assert(!dcp_assert_hpd_before_link_wait(&d,0,false,true));assert(calls==1&&!samples);
 d.tunnel=true;calls=timeout_ms=0;
 assert(!dcp_assert_hpd_before_link_wait(&d,0,false,false));assert(calls==1&&timeout_ms==8000&&!samples);
 d.typec=d.tunnel=false;d.hdmi_hpd=0;calls=0;
 assert(!dcp_assert_hpd_before_link_wait(&d,0,false,false));assert(!calls&&!samples);
 puts("early AP HPD: inherited HDMI trigger, errors, late-HPD, Type-C and tunnel regressions pass");
}
'''
# Check real call ordering/error unwind in the production caller as well.
start = source.index('static int dcp_dptx_connect_raw(')
caller = source[start:source.index('\n}\n', start)]
request = caller.index('dptxport_request_display(')
hpd = caller.index('dcp_assert_hpd_before_link_wait(')
wait = caller.index('dcp_dptx_wait_linkcfg(')
assert request < hpd < wait
assert 'goto out_release;' in caller[hpd:wait]
with tempfile.TemporaryDirectory(prefix='dcp-hpd-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(prefix+body+cases)
 for mode,flags in [('normal',[]),('sanitizer',['-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
  exe=p/mode
  subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',*flags,str(p/'test.c'),'-o',str(exe)],check=True)
  subprocess.run([exe],check=True)
