#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""Native regression for component/devres teardown of an unstopped session.

Mocks kernel resource releases and the component framework. It executes the
actual unbind, PIODMA release, RTKit release, domain release and ring allocator.
No firmware calls or hardware acceptance are claimed.
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


# This is the driver-core switch that transfers default-domain lifetime to us.
text = (src / 'dcp.c').read_text()
assert re.search(r'static struct platform_driver apple_platform_driver = \{\s*'
                 r'\.driver_managed_dma = true,', text)
functions = '\n'.join(function(f, n) for f, n in (
    ('dcp.c', 'dcp_release_dma_domain'), ('dcp.c', 'dcp_release_rtkit'),
    ('dcp.c', 'dcp_release_piodma_iommu_dev'), ('afk.c', 'afk_quiesce'),
    ('afk.c', 'afk_release_context'), ('dcp.c', 'dcp_release_context'),
    ('afk.c', 'afk_getbuf'), ('dcp.c', 'dcp_comp_unbind'),
    ('apple_drv.c', 'apple_drm_uninit')))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
#define GFP_KERNEL 0
#define BLOCK_SHIFT 6
#define GETBUF_SIZE 0xffffU
#define GETBUF_TAG 0xffff0000U
#define RBEP_TYPE 0
#define RBEP_GETBUF_ACK 0
#define GETBUF_ACK_DVA 0
#define FIELD_GET(mask,x) ((x)&(mask))
#define FIELD_PREP(mask,x) ((void)(mask),(u64)(x))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define IS_ERR_OR_NULL(x) (!(x)||(uintptr_t)(x)>(uintptr_t)-4096)
#define DRM_MODE_CONNECTOR_eDP 14
struct work_struct {int id;};struct delayed_work {struct work_struct work;};
struct drm_device {int refs;bool unplugged;};
struct apple_drm_private {struct drm_device drm;};
struct device {void *data;int domain_users;};
struct platform_device {struct device dev;};
struct apple_crtc {struct {struct drm_device *dev;} base;};
struct apple_connector {struct work_struct hotplug_wq;};
struct apple_rtkit {bool rx_stopped;};
struct apple_dcp;
struct apple_dcp_afkep {
 struct apple_dcp *dcp;void *wq;void *bfr;void *debugfs_entry;
 u64 bfr_dma;u32 bfr_size;u16 bfr_tag;
};
struct apple_dcp {
 bool retain_dma,quiescing,drm_retained,piodma_created;int connector_type,hdmi_hpd_irq;
 struct device *dev;struct apple_crtc *crtc;
 struct apple_connector *connector,*typec_connector;
 struct platform_device *piodma;void *iommu_dom,*typec_mux,*shmem,*clk,*clk_194;
 struct apple_rtkit *rtk;
 struct apple_dcp_afkep *avep,*dptxep,*ibootep,*systemep,*dcpavservep;
 struct work_struct bl_register_wq,bl_update_wq,vblank_wq;
 struct delayed_work typec_reconnect_wq,typec_fabric_retrain_wq;
};
static int dma_frees,domain_drops,pio_destroys,pio_puts,firmware_stops,queue_stops;
static int raw_rings,managed_rings,work_drains;
static int context_frees;
static void *ring;
static struct device *component;
static void dcp_comp_unbind(struct device *,struct device *,void *);
static void kfree(void *p) {assert(p);context_frees++;}
static void debugfs_remove_recursive(void *p) {(void)p;}
static void dev_warn(struct device *d,const char *fmt) {(void)d;(void)fmt;}
static void dev_err(struct device *d,const char *fmt,...) {(void)d;(void)fmt;}
static void *dev_get_drvdata(struct device *d) {return d->data;}
static void dev_set_drvdata(struct device *d,void *p) {d->data=p;}
static void drm_dev_unplug(struct drm_device *d) {d->unplugged=true;}
static void drm_atomic_helper_shutdown(struct drm_device *d) {assert(d->unplugged);}
static void component_unbind_all(struct device *m,void *p) {dcp_comp_unbind(component,m,p);}
static void iommu_device_unuse_default_domain(struct device *d) {domain_drops++;d->domain_users--;}
static void apple_rtkit_free(struct apple_rtkit *r) {r->rx_stopped=true;dma_frees++;}
static void apple_rtkit_free_retaining_buffers(struct apple_rtkit *r) {r->rx_stopped=true;}
static void of_platform_device_destroy(struct device *d,void *p) {(void)d;(void)p;pio_destroys++;}
static void put_device(struct device *d) {(void)d;pio_puts++;}
static void drm_dev_get(struct drm_device *d) {d->refs++;}
static void iomfb_queue_stop(struct apple_dcp *d) {assert(d->quiescing||!d->retain_dma);queue_stops++;}
static void destroy_workqueue(void *w) {assert(w);work_drains++;}
static void cancel_work_sync(struct work_struct *w) {(void)w;}
static void cancel_delayed_work_sync(struct delayed_work *w) {(void)w;}
static void disable_irq(int i) {(void)i;}
static void typec_mux_put(void *p) {(void)p;}
static void av_service_disconnect(struct apple_dcp *d) {(void)d;firmware_stops++;}
static void afk_shutdown(struct apple_dcp_afkep *e) {(void)e;firmware_stops++;}
static void iomfb_shutdown(struct apple_dcp *d) {(void)d;firmware_stops++;}
static void devm_clk_put(struct device *d,void *c) {(void)d;(void)c;}
static void trace_afk_getbuf(struct apple_dcp_afkep *e,u32 size,u16 tag) {(void)e;(void)size;(void)tag;}
static void afk_send(struct apple_dcp_afkep *e,u64 msg) {(void)e;(void)msg;}
static void *dma_alloc_coherent(struct device *d,u32 size,u64 *addr,int flags) {
 (void)d;(void)flags;raw_rings++;ring=calloc(1,size);assert(ring);*addr=123;return ring;
}
static void *dmam_alloc_coherent(struct device *d,u32 size,u64 *addr,int flags) {
 (void)d;(void)flags;managed_rings++;ring=calloc(1,size);assert(ring);*addr=123;return ring;
}
'''
main = r'''
int main(void) {
 unsigned cases=0;
 for(int live=0;live<2;live++)for(int created=0;created<2;created++)for(int linked=0;linked<2;linked++)for(int master=0;master<2;master++) {
  struct device dev={.domain_users=1};struct platform_device pio={0};
  struct apple_drm_private apple={.drm={.refs=1}};
  struct device main={.data=&apple};struct apple_crtc crtc={.base={&apple.drm}};
  struct apple_rtkit rtk={0};struct apple_connector con={0};
  struct apple_dcp d={.retain_dma=live,.dev=&dev,.crtc=linked?&crtc:NULL,
   .connector=&con,.piodma=&pio,.piodma_created=created,.iommu_dom=&dev,
   .rtk=&rtk,.shmem=&dev,.connector_type=DRM_MODE_CONNECTOR_eDP};
  struct apple_dcp_afkep ep[5]={0};
  for(int i=0;i<5;i++)ep[i]=(struct apple_dcp_afkep){.dcp=&d,.wq=&dev};
  d.avep=&ep[0];d.dptxep=&ep[1];d.ibootep=&ep[2];d.systemep=&ep[3];d.dcpavservep=&ep[4];dev.data=&d;
  dma_frees=domain_drops=pio_destroys=pio_puts=firmware_stops=queue_stops=0;
  raw_rings=managed_rings=work_drains=context_frees=0;
  afk_getbuf(&ep[0],1);assert(ep[0].bfr_size==64);
  afk_getbuf(&ep[0],1);assert(raw_rings+managed_rings==1);
  component=&dev;
  if(master) {apple_drm_uninit(&main);assert(!main.data&&apple.drm.unplugged);}
  else dcp_comp_unbind(&dev,&dev,NULL);
  /* Component devres release runs even after an early unbind return. */
  dcp_release_rtkit(&d);assert(rtk.rx_stopped&&!d.rtk);
  /* Driver removal also releases probe resources and its domain action. */
  dcp_release_dma_domain(&d);
  if(live) {
   assert(d.quiescing&&!dma_frees&&!domain_drops&&dev.domain_users==1);
   assert(d.piodma==&pio&&d.iommu_dom==&dev&&!pio_destroys&&!pio_puts);
   assert(raw_rings==1&&!managed_rings&&!firmware_stops&&work_drains==5);
   assert(apple.drm.refs==(linked?2:1));
   dcp_comp_unbind(&dev,&dev,NULL);assert(apple.drm.refs==(linked?2:1));
   dcp_release_piodma_iommu_dev(&d);assert(!pio_destroys&&!pio_puts);
  } else {
   assert(dma_frees==1&&domain_drops==1&&dev.domain_users==0);
   assert(!d.piodma&&!d.iommu_dom&&managed_rings==1&&!raw_rings);
   assert(pio_destroys==created&&pio_puts==!created&&firmware_stops>0);
   assert(apple.drm.refs==1);
  }
  for(int i=0;i<5;i++)afk_release_context(&ep[i]);
  dcp_release_context(&d);assert(context_frees==(live?0:6));
  afk_quiesce(NULL);afk_quiesce((struct apple_dcp_afkep *)(intptr_t)-12);
  /* Simulated machine reset: test memory can now be reclaimed. */
  free(ring);cases++;
 }
 /* A failed bind has no unbind callback: resource cleanup must close rebind. */
 {
  struct device dev={0};struct platform_device pio={0};struct apple_rtkit rtk={0};
  struct apple_dcp d={.dev=&dev,.retain_dma=true,.piodma=&pio,.rtk=&rtk};
  assert(!d.quiescing);dcp_release_piodma_iommu_dev(&d);
  assert(d.quiescing&&d.piodma==&pio);cases++;
  d.quiescing=false;dcp_release_rtkit(&d);
  assert(d.quiescing&&rtk.rx_stopped&&!d.rtk);cases++;
 }
 puts("PASS: 18 actual-source component, master-unwind, driver-resource-release and failed-bind cases; live DMA/domain/rings retained; legacy releases preserved");
 assert(cases==18);return 0;
}
'''
code = a.output / 'lifetime.c'
code.write_text(prefix + functions + main)
command = [a.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
           '-Wno-unused-parameter', '-fsanitize=address,undefined', str(code),
           '-o', str(a.output / 'lifetime')]
subprocess.run(command, check=True)
result = subprocess.check_output([str(a.output / 'lifetime')], text=True)
(a.output / 'receipt.json').write_text(json.dumps(dict(command=command, result=result,
    functions_sha256=hashlib.sha256(functions.encode()).hexdigest(),
    scope='Native resource-release simulation; not a live firmware unbind test'), indent=2) + '\n')
print(result)
