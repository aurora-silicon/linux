# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""Actual wrapper/driver-core/DWC3/xHCI teardown order with modeled scheduling.

Device mutex APIs use pthread mutexes. Clock, unregister, PM and hardware leaves
are mocks; this is not lockdep, system-PM or electrical atomicity qualification.
"""
import pm
from common import block

def unit_for(source):
 unit=pm.ownership(source)
 unit=unit.replace('int main(void)','int __attribute__((unused)) ownership_main(void)',1)
 unit=unit.replace('struct device_driver { const char *name; };', 'struct device;\nstruct device_driver { const char *name; void (*remove)(struct device *); void *dev_groups; };')
 unit=unit.replace('struct device { int refs;', 'struct bus_type { bool need_parent_lock; void (*remove)(struct device *); void (*dma_cleanup)(struct device *); };\nstruct device_private { int knode_driver; };\nstruct device { struct bus_type *bus; struct device_private *p; int kobj; int refs;')
 unit=unit.replace('static struct device host_device;', 'static struct platform_device host_platform_device;\n#define host_device host_platform_device.dev')
 unit=unit.replace('static bool controller_locked, host_locked;', 'static _Atomic bool controller_locked, host_locked;')
 pre=r'''
#include <pthread.h>
#include <stdatomic.h>
static pthread_mutex_t controller_mutex=PTHREAD_MUTEX_INITIALIZER, host_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t schedule_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t schedule_cond=PTHREAD_COND_INITIALIZER;
static _Thread_local bool remover;
static atomic_bool handoff_active, early_write;
static bool removal_started, blocked_attempt, release_remove, inject_remove;
static int remove_target, susphy_calls, host_remove_calls;
static pthread_t remove_thread;
static void start_remove(void);
static void removal_lock_attempt(bool held) {
 if(!remover||!held)return;
 pthread_mutex_lock(&schedule_mutex);blocked_attempt=true;
 pthread_cond_broadcast(&schedule_cond);pthread_mutex_unlock(&schedule_mutex);
}
static void observed_controller_write(void) {
 if(atomic_load(&handoff_active))atomic_store(&early_write,true);
 pthread_mutex_lock(&schedule_mutex);
 removal_started=true;pthread_cond_broadcast(&schedule_cond);
 while(atomic_load(&handoff_active)&&!release_remove)pthread_cond_wait(&schedule_cond,&schedule_mutex);
 pthread_mutex_unlock(&schedule_mutex);
}
'''
 unit=unit.replace('#define CONFIG_USB 1',pre+'\n#define CONFIG_USB 1',1)
 old=block(unit,'static void device_lock(')
 new=r'''static void device_lock(struct device *d) {
 if(d==&controller.dev){
  removal_lock_attempt(atomic_load(&controller_locked));
  pthread_mutex_lock(&controller_mutex);controller_locked=true;
 }else{
  assert(d==&host_device);removal_lock_attempt(atomic_load(&host_locked));
  pthread_mutex_lock(&host_mutex);host_locked=true;
 }
}'''
 unit=unit.replace(old,new)
 old=block(unit,'static void device_unlock(')
 new=r'''static void device_unlock(struct device *d) {
 if(d==&host_device){
  assert(host_locked);
  if(!remover&&remove_target){
   /* Direct xHCI removal is fenced by this lock, not the controller's. */
   assert(!root_locked&&!link_locked&&!pm_phy->lock&&!pm_refs&&!link_pm_refs);
   atomic_store(&handoff_active,false);
  }
  host_locked=false;pthread_mutex_unlock(&host_mutex);
  if(!remover&&remove_target){
   /* Force teardown into the gap before the controller lock is released. */
   pthread_mutex_lock(&schedule_mutex);
   while(!removal_started)pthread_cond_wait(&schedule_cond,&schedule_mutex);
   pthread_mutex_unlock(&schedule_mutex);
  }
 }else{
  assert(d==&controller.dev&&controller_locked);
  if(!remover&&!remove_target){
   assert(!root_locked&&!link_locked&&!pm_phy->lock&&!pm_refs&&!link_pm_refs);
   atomic_store(&handoff_active,false);
  }
  controller_locked=false;pthread_mutex_unlock(&controller_mutex);
 }
}'''
 unit=unit.replace(old,new)
 # The scheduler injection is a child-lookup interleaving, as in the review, or
 # the root resume for a source that takes the SuperSpeed port down instead.
 unit=unit.replace('if(detach_at_lookup)usb_set_device_state(u,USB_STATE_NOTATTACHED);','if(inject_remove){inject_remove=false;start_remove();}\n if(detach_at_lookup)usb_set_device_state(u,USB_STATE_NOTATTACHED);',1)
 unit=unit.replace('static void pm_runtime_put_noidle(struct device *d) {','static void pm_runtime_put_noidle(struct device *d) {\n if(remover&&d==&host_device)return;',1)
 # Old source lacks platform-lock helpers. Keep fixture-only API definitions
 # available without claiming they are invoked by the parent.
 if 'device_lock(&pm->controller->dev)' not in (source/'drivers/phy/apple/atc.c').read_text():
  unit=unit.replace('static struct device *get_device(', 'static __attribute__((unused)) struct device *get_device(')
 prefix=r'''
struct clk { int unused; }; struct reset_control { int unused; };
struct xhci_hcd { atomic_int xhc_state; struct clk *clk,*reg_clk; struct reset_control *reset; struct usb_hcd *shared_hcd; };
static struct xhci_hcd xhci_context;
#define XHCI_STATE_REMOVING 1
#define hcd_to_xhci(h) (&xhci_context)
struct dwc3 { bool sys_wakeup, dis_u3_susphy_quirk; struct platform_device *xhci; };
enum { DWC3_APPLE_PROBE_PENDING, DWC3_APPLE_NO_CABLE, DWC3_APPLE_HOST, DWC3_APPLE_DEVICE };
/* pre_core_rpm is T8132 only: runtime PM before the first core probe; never set here. */
struct dwc3_apple { struct dwc3 dwc; struct device *dev; int lock,state; void *role_sw; bool pre_core_rpm, dt_dis_u3_susphy; };
static struct dwc3_apple apple_context;
#define to_dwc3_apple(d) container_of(d,struct dwc3_apple,dwc)
static bool inside_host_remove;
static void *platform_get_drvdata(struct platform_device *d) { return d==&controller?(void *)&apple_context.dwc:(void *)&hcd; }
static void dwc3_enable_susphy(struct dwc3 *d,bool v) { assert(v&&d->dis_u3_susphy_quirk==apple_context.dt_dis_u3_susphy);susphy_calls++;observed_controller_write(); }
static void device_init_wakeup(struct device *d,bool b) { (void)d;(void)b; }
static void platform_device_unregister(struct platform_device *d);
static void dwc3_gadget_exit(struct dwc3 *d) { (void)d;assert(0); }
static int dwc3_apple_core_stop(struct dwc3_apple *d) { (void)d;return 0; }
static void usb_role_switch_unregister(void *s) { (void)s; }
static void dwc3_core_remove_no_shutdown(struct dwc3 *d) { (void)d; }
static void usb_remove_hcd(struct usb_hcd *h) { host_remove_calls++;h->state=HC_STATE_QUIESCING;usb_set_device_state(h->self.root_hub,USB_STATE_NOTATTACHED); }
static void usb_phy_shutdown(void *p) { (void)p; }
static void usb_put_hcd(struct usb_hcd *h) { (void)h; }
static void clk_disable_unprepare(struct clk *c) { (void)c; }
static void reset_control_assert(struct reset_control *r) { (void)r; }
static void pm_runtime_disable(struct device *d) { (void)d; }
static void pm_runtime_set_suspended(struct device *d) { (void)d; }
static int pm_runtime_get_sync(struct device *d) { if(inside_host_remove&&d==&host_device){assert(xhci_context.xhc_state&XHCI_STATE_REMOVING);observed_controller_write();}return 0; }
static void pm_runtime_put_sync(struct device *d) { (void)d; }
static void pm_runtime_put(struct device *d) { (void)d; }
#define dev_attr_state_synced 0
#define BUS_NOTIFY_UNBIND_DRIVER 0
#define BUS_NOTIFY_UNBOUND_DRIVER 1
#define KOBJ_UNBIND 0
static bool device_links_busy(struct device *d) { (void)d;return false; }
static void device_links_unbind_consumers(struct device *d) { (void)d;assert(0); }
static void driver_sysfs_remove(struct device *d) { (void)d; }
static void bus_notify(struct device *d,int n) { (void)d;(void)n; }
static void device_remove_file(struct device *d,void *a) { (void)d;(void)a; }
static void device_remove_groups(struct device *d,void *a) { (void)d;(void)a; }
static void device_unbind_cleanup(struct device *d) { d->driver=NULL; }
static void device_links_driver_cleanup(struct device *d) { (void)d; }
static void klist_remove(void *k) { (void)k; }
static void device_pm_check_callbacks(struct device *d) { (void)d; }
static void kobject_uevent(void *k,int n) { (void)k;(void)n; }
'''
 prefix=prefix.replace('#define dev_attr_state_synced 0','static int dev_attr_state_synced;')
 unit=unit.replace('struct usb_hcd { int state;', 'struct usb_hcd { void *usb_phy; int state;',1)
 # Extract actual callback order; clock/USB/reset leaves remain mocks.
 host=(source/'drivers/usb/dwc3/host.c').read_text()
 glue=(source/'drivers/usb/dwc3/dwc3-apple.c').read_text()
 plat=(source/'drivers/usb/host/xhci-plat.c').read_text()
 dd=(source/'drivers/base/dd.c').read_text()
 functions=block(host,'void dwc3_host_exit(')+'\n'+block(glue,'static int dwc3_apple_exit(')+'\n'+block(glue,'static void dwc3_apple_remove(')+'\n'+block(plat,'void xhci_plat_remove(')+'\n'
 functions+='\n'.join(block(dd,x) for x in ['static void __device_driver_lock(', 'static void __device_driver_unlock(', 'static void device_remove(', 'static void __device_release_driver(', 'void device_release_driver_internal('])
 controls=r'''
static void controller_remove(struct device *d) { assert(d==&controller.dev);dwc3_apple_remove(&controller); }
static void host_remove(struct device *d) { assert(d==&host_device);inside_host_remove=true;xhci_plat_remove(&host_platform_device);inside_host_remove=false; }
static void platform_device_unregister(struct platform_device *d) { assert(d==&host_platform_device);device_release_driver_internal(&d->dev,NULL,NULL); }
static void *run_remove(void *unused) { (void)unused;remover=true;device_release_driver_internal(remove_target?&host_device:&controller.dev,NULL,NULL);return NULL; }
static void start_remove(void) {
 pthread_mutex_lock(&schedule_mutex);
 assert(pthread_create(&remove_thread,NULL,run_remove,NULL)==0);
 while(!blocked_attempt&&!removal_started)pthread_cond_wait(&schedule_cond,&schedule_mutex);
 pthread_mutex_unlock(&schedule_mutex);
}
int main(void) {
 for(remove_target=0;remove_target<2;remove_target++) {
  struct apple_atcphy p;prepare(&p,true,true,false);
  struct device_private private={0};controller.dev.p=&private;host_device.p=&private;
  controller_driver.remove=controller_remove;host_driver.remove=host_remove;
  apple_context=(struct dwc3_apple){.dwc={.xhci=&host_platform_device,.dis_u3_susphy_quirk=true},.dev=&controller.dev,.state=DWC3_APPLE_HOST};
  xhci_context=(struct xhci_hcd){0};
  inject_remove=true;removal_started=blocked_attempt=release_remove=false;
  atomic_store(&early_write,false);atomic_store(&handoff_active,true);
  susphy_calls=host_remove_calls=0;
  int ret=safe(&p);
  atomic_store(&handoff_active,false);
  pthread_mutex_lock(&schedule_mutex);release_remove=true;pthread_cond_broadcast(&schedule_cond);pthread_mutex_unlock(&schedule_mutex);
  assert(pthread_join(remove_thread,NULL)==0);
  fprintf(stderr,"connected teardown target=%d ret=%d early_write=%d susphy=%d host_remove=%d\n",remove_target,ret,atomic_load(&early_write),susphy_calls,host_remove_calls);
  assert(ret==0&&!atomic_load(&early_write)&&host_remove_calls==1);
  assert(susphy_calls==(remove_target?0:1));balanced(&p);
 }
 puts("PASS actual PM wrapper connected to driver-core/DWC3/xHCI removal; synchronous teardown waits until owned transaction ends");
 return 0;
}
'''
 return unit+prefix+functions+controls
