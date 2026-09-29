// SPDX-License-Identifier: GPL-2.0-only
/* J514S USB-C routing for the two independent external DCP engines.
 * TIPD owns negotiation; the generic PHY owns all common/USB/DP registers.
 * This consumer owns DPXBAR and serializes exclusive physical-port claims.
 * Hardware resources are retained until reboot, including uncertain failures.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spmi.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-apple-m3.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_cd321x.h>
#include "../../../../../usb/typec/tipd/tps6598x.h"
#include "m3_hdmi_control.h"
#include "m3_hdmi_xbar.h"

struct m3_usbc_port {
 const char *controller_name, *phy_name;
 resource_size_t address;
 struct spmi_device *controller;
 struct phy *phy;
 struct m3_hdmi_dpxbar xbar;
 int owner;
};
static struct m3_usbc_port ports[] = {
 { .controller_name="1-0c", .phy_name="dp0", .address=0x70304c000ULL, .owner=-1 },
 { .controller_name="1-0a", .phy_name="dp1", .address=0xb0304c000ULL, .owner=-1 },
 { .controller_name="2-08", .phy_name="dp", .address=0xf0304c000ULL, .owner=-1 },
};
struct m3_usbc_route {
 unsigned int active_port, rate;
 bool flipped, powered, prepared;
 int result;
};
static struct m3_usbc_route routes[2] = {
 { .active_port=0 }, { .active_port=2 },
};
static DEFINE_MUTEX(route_lock);
static int result=-EINPROGRESS;
module_param(result,int,0400);
module_param_named(active_port,routes[1].active_port,uint,0400);
module_param_named(rate,routes[1].rate,uint,0400);
module_param_named(powered,routes[1].powered,bool,0400);
module_param_named(primary_active_port,routes[0].active_port,uint,0400);
module_param_named(primary_rate,routes[0].rate,uint,0400);
module_param_named(primary_powered,routes[0].powered,bool,0400);
module_param_named(primary_result,routes[0].result,int,0400);
module_param_named(secondary_result,routes[1].result,int,0400);

static int partner(unsigned int index, bool *orientation, bool require_hpd)
{
 struct cd321x_dp_snapshot snapshot;u32 status,data,hpd;int ret;
 int (*get_snapshot)(struct device *,struct cd321x_dp_snapshot *);
 if(!ports[index].controller)return -ENODEV;
 get_snapshot=symbol_get(cd321x_get_dp_state);
 if(!get_snapshot)return -EOPNOTSUPP;
 ret=get_snapshot(&ports[index].controller->dev,&snapshot);
 symbol_put(cd321x_get_dp_state);if(ret)return ret;
 status=snapshot.status;data=snapshot.data_status;hpd=snapshot.dp_status;
 *orientation=!!(status&TPS_STATUS_PLUG_UPSIDE_DOWN);
 return !!((status&TPS_STATUS_PLUG_PRESENT) && (data&TPS_DATA_STATUS_DP_CONNECTION) &&
   TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT(data)==TPS_DATA_STATUS_DP_SPEC_PIN_ASSIGNMENT_D &&
   (!require_hpd || (hpd&DP_STATUS_HPD_STATE)));
}
/* Read only cached negotiation and software mode while an inactive port's
 * clocks may be off. Never steal a PHY held by the other DCP session. */
static int candidate(unsigned int slot, bool require_hpd)
{
 unsigned int index;bool orientation;int ret;
 for(unsigned int n=0;n<ARRAY_SIZE(ports);n++) {
  union phy_configure_opts opts={};
  index=(routes[slot].active_port+n)%ARRAY_SIZE(ports);
  if(ports[index].owner>=0 && ports[index].owner!=slot)continue;
  ret=partner(index,&orientation,require_hpd);
  if(ret<0)return ret;
  if(!ret)continue;
  ret=phy_validate(ports[index].phy,PHY_MODE_DP,0,&opts);
  if(ret==-ENOLINK || ret==-EBUSY)continue;
  if(ret)return ret;
  if(opts.dp.lanes==2)return index;
 }
 return -ENOLINK;
}
static int aux(struct m3_usbc_port *p, bool enable)
{
 int ret;int (*fn)(struct phy *,bool)=symbol_get(apple_atc_m3_dp_aux);
 if(!fn)return -EOPNOTSUPP;
 ret=fn(p->phy,enable);symbol_put(apple_atc_m3_dp_aux);return ret;
}
static int acquire(unsigned int slot, bool require_hpd)
{
 struct m3_usbc_route *r=&routes[slot];struct m3_usbc_port *p;
 bool orientation;int ret,index;
 if(r->powered)return 0;
 if(r->rate)return -EBUSY;
 index=candidate(slot,require_hpd);if(index<0)return index;
 p=&ports[index];
 ret=partner(index,&orientation,false);if(ret!=1)return ret<0?ret:-ENOLINK;
 ret=phy_power_on(p->phy);if(ret)return ret;
 /* All later failures retain the exclusive claim and power for recovery. */
 p->owner=slot;r->active_port=index;r->flipped=orientation;r->powered=true;
 p->xbar.selector=2*slot;
 if(!p->xbar.regs){
  if(!request_mem_region(p->address,0x4000,"m3-usbc-dpxbar"))return -EBUSY;
  p->xbar.regs=ioremap_np(p->address,0x4000);
  if(!p->xbar.regs)return -ENOMEM;
 }
 ret=m3_hdmi_dpxbar_idle(&p->xbar);
 pr_info("m3_usbc_phy: DCPEXT%u selected ATC%u target=%#x selector=%u flipped=%u result=%d\n",
         slot,r->active_port,0x8000|(r->active_port<<4),p->xbar.selector,r->flipped,ret);
 return ret;
}
static int activate(unsigned int slot)
{
 struct m3_usbc_route *r=&routes[slot];union phy_configure_opts opts={};
 struct m3_usbc_port *p;int ret;bool orientation;
 if(!r->powered)return -EHOSTDOWN;
 p=&ports[r->active_port];
 ret=partner(r->active_port,&orientation,false);
 if(ret!=1 || orientation!=r->flipped)return ret<0?ret:-ENOLINK;
 ret=phy_validate(p->phy,PHY_MODE_DP,0,&opts);
 if(ret || opts.dp.lanes!=2)return ret ?: -ENOLINK;
 return aux(p,true);
}
static int set_rate(unsigned int slot, unsigned int next)
{
 struct m3_usbc_route *r=&routes[slot];struct m3_usbc_port *p=&ports[r->active_port];
 union phy_configure_opts opts={};int ret;
 if(next!=0 && next!=6 && next!=10 && next!=20)return -EOPNOTSUPP;
 if(next==r->rate)return 0;
 if(!r->powered)return -EHOSTDOWN;
 if(p->xbar.selected)return -EBUSY;
 if(next){ret=activate(slot);if(ret)return ret;}
 opts.dp.set_rate=1;opts.dp.link_rate=next*270;opts.dp.lanes=2;
 ret=phy_configure(p->phy,&opts);
 if(ret)r->result=ret;else r->rate=next;
 pr_info("m3_usbc_phy: DCPEXT%u DP rate=%u result=%d\n",slot,next,ret);
 return ret;
}
static int check(unsigned int slot)
{
 if(slot>=ARRAY_SIZE(routes))return -EINVAL;
 return result ?: routes[slot].result;
}
static int route_prepare(unsigned int slot)
{
 int ret;guard(mutex)(&route_lock);
 ret=check(slot);if(ret)return ret;
 if(routes[slot].prepared)return -EBUSY;
 ret=acquire(slot,false);
 if(!ret)ret=m3_hdmi_dpxbar_up(&ports[routes[slot].active_port].xbar);
 routes[slot].result=ret;routes[slot].prepared=!ret;
 return ret;
}
static int route_hpd(unsigned int slot)
{
 bool orientation;int ret;guard(mutex)(&route_lock);
 ret=check(slot);if(ret)return ret;
 if(routes[slot].powered){
  ret=partner(routes[slot].active_port,&orientation,true);
  return ret<=0?ret:orientation==routes[slot].flipped;
 }
 ret=candidate(slot,true);
 return ret==-ENOLINK?0:ret<0?ret:1;
}
static int route_target(unsigned int slot)
{
 int ret;guard(mutex)(&route_lock);
 ret=check(slot);if(ret)return ret;
 /* Keep the released destination until the next explicit acquisition. */
 return 0x8000 | (routes[slot].active_port<<4);
}
static int route_activate(unsigned int slot)
{
 int ret;guard(mutex)(&route_lock);
 ret=check(slot);return ret ?: activate(slot);
}
static int route_xbar_down(unsigned int slot)
{
 int ret;guard(mutex)(&route_lock);
 ret=check(slot);if(ret)return ret;
 /* Once released, the old physical port may belong to the other session. */
 if(!routes[slot].powered)return 0;
 return m3_hdmi_dpxbar_down(&ports[routes[slot].active_port].xbar);
}
static int route_xbar_up(unsigned int slot)
{
 struct m3_hdmi_dpxbar *xbar;int ret;guard(mutex)(&route_lock);
 ret=check(slot);if(ret)return ret;
 ret=acquire(slot,true);
 if(!routes[slot].powered && (ret==-ENOLINK || ret==-EBUSY))return -EAGAIN;
 if(ret)return ret;
 xbar=&ports[routes[slot].active_port].xbar;
 return xbar->selected?0:m3_hdmi_dpxbar_up(xbar);
}
static int route_set_rate(unsigned int slot, unsigned int next)
{
 int ret;guard(mutex)(&route_lock);
 ret=check(slot);return ret ?: set_rate(slot,next);
}
static int route_deactivate(unsigned int slot)
{
 struct m3_usbc_route *r;struct m3_usbc_port *p;int ret;
 guard(mutex)(&route_lock);
 ret=check(slot);if(ret)return ret;
 r=&routes[slot];if(!r->powered)return 0;
 p=&ports[r->active_port];
 ret=m3_hdmi_dpxbar_down(&p->xbar);
 if(!ret)ret=set_rate(slot,0);
 if(!ret)ret=aux(p,false);
 if(!ret)ret=phy_power_off(p->phy);
 if(!ret){r->powered=false;p->owner=-1;}
 return ret;
}
static int route_get_rate(unsigned int slot)
{
 int ret;guard(mutex)(&route_lock);
 ret=check(slot);return ret ?: routes[slot].rate;
}
static int route_set_drive(unsigned int slot, const unsigned int drive[12])
{
 union phy_configure_opts opts={};int ret;guard(mutex)(&route_lock);
 ret=check(slot);if(ret)return ret;
 ret=activate(slot);if(ret)return ret;
 if(!routes[slot].rate)return -EHOSTDOWN;
 for(unsigned int i=0;i<2;i++){
  if(drive[i*3])return -EINVAL;
  opts.dp.voltage[i]=drive[i*3+1];opts.dp.pre[i]=drive[i*3+2];
 }
 opts.dp.set_voltages=1;opts.dp.lanes=2;
 return phy_configure(ports[routes[slot].active_port].phy,&opts);
}
static const struct m3_usbc_route_ops route_ops={
 .prepare=route_prepare,.hpd=route_hpd,.target=route_target,
 .activate=route_activate,.deactivate=route_deactivate,
 .xbar_up=route_xbar_up,.xbar_down=route_xbar_down,
 .set_rate=route_set_rate,.get_rate=route_get_rate,.set_drive=route_set_drive,
};
const struct m3_usbc_route_ops *m3_usbc_get_ops(void)
{return &route_ops;}
EXPORT_SYMBOL_GPL(m3_usbc_get_ops);
static int __init probe(void)
{
 struct device_node *node;int ret=-ENODEV;int (*supported)(struct phy *);
 if(!of_machine_is_compatible("apple,j514s"))return -ENODEV;
 for_each_compatible_node(node,NULL,"apple,sn201202x"){
  struct spmi_device *sdev=spmi_find_device_by_of_node(node);
  if(!sdev)continue;
  for(unsigned int i=0;i<ARRAY_SIZE(ports);i++)
   if(!strcmp(dev_name(&sdev->dev),ports[i].controller_name)){
    get_device(&sdev->dev);ports[i].controller=sdev;break;
   }
  spmi_device_put(sdev);
 }
 node=of_find_node_by_path("dcpext1");if(!node)goto done;
 for(unsigned int i=0;i<ARRAY_SIZE(ports);i++){
  struct phy *phy;
  if(!ports[i].controller){ret=-ENODEV;break;}
  phy=of_phy_get(node,ports[i].phy_name);
  if(IS_ERR(phy)){ret=PTR_ERR(phy);break;}
  ports[i].phy=phy;
  supported=symbol_get(apple_atc_m3_dp_supported);
  if(!supported){ret=-EOPNOTSUPP;break;}
  ret=supported(phy);symbol_put(apple_atc_m3_dp_supported);if(ret)break;
  if(!phy->ops->power_on || !phy->ops->power_off){ret=-EOPNOTSUPP;break;}
  ret=phy_init(phy);if(ret)break;
  ret=phy_set_mode(phy,PHY_MODE_DP);if(ret)break;
 }
 of_node_put(node);
 done:
 result=ret;
 pr_info("m3_usbc_phy: two-session three-port consumer ready result=%d\n",ret);
 return 0;
}
module_init(probe);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("J514S exclusive USB-C DP routing for two external DCP engines");
