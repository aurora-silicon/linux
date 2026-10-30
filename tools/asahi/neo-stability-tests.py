#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host controls for retained domains, wake IRQs, USB roles and band-qualified country policy."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r'^(?:static\s+)?(?:inline\s+)?(?:int|void|bool|struct ieee80211_channel \*)\s*' +
                      name + r'\([^;]*?\)\n\{', source, re.M | re.S)
    if not match:
        raise ValueError(name)
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <ctype.h>
#define SHA256_DIGEST_SIZE 32
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int8_t s8; typedef int32_t s32;
enum nl80211_band {NL80211_BAND_2GHZ,NL80211_BAND_5GHZ,NL80211_BAND_60GHZ,NL80211_BAND_6GHZ,NL80211_BAND_S1GHZ,NL80211_BAND_LC,NUM_NL80211_BANDS};
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define BIT_ULL(n) (1ULL<<(n))
#define GENMASK_ULL(h,l) (((~0ULL)>>(63-(h)))&(~0ULL<<(l)))
#define ERR_PTR(e) ((void*)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define MHZ_TO_KHZ(f) ((f)*1000)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define lockdep_assert_held(x) ((void)0)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_info_ratelimited(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define WARN_ON_ONCE(x) ((void)(x))
#define spin_lock_irqsave(l,f) ((f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(f))
#define PD_FLAG_DEV_LINK_ON 1
struct device { bool syscore,wakeup; int wake_irq; void *of_node; };
struct dockchannel { struct device *dev; int rx_irq; };
struct dev_pm_domain_list { unsigned num_pds; struct device **pd_devs; };
struct dev_pm_domain_attach_data { unsigned pd_flags; };
static int domain_count,attach_error,action_error,wakeup_error,irq_error;
static unsigned attach_calls,wakeup_calls,irq_calls,core_exits,role_exits,reset_calls;
static struct device pd0,pd1;static struct device *pd_devices[]={&pd0,&pd1};
static struct dev_pm_domain_list domains={2,pd_devices};
static void (*cleanup)(void *);static void *cleanup_data;
static int of_count_phandle_with_args(void*n,const char*a,const char*b){return domain_count;}
static int devm_pm_domain_attach_list(struct device*d,struct dev_pm_domain_attach_data*a,struct dev_pm_domain_list**l){attach_calls++;assert(a->pd_flags==PD_FLAG_DEV_LINK_ON);*l=&domains;return attach_error;}
static void dev_pm_syscore_device(struct device*d,bool v){d->syscore=v;}
static int devm_add_action_or_reset(struct device*d,void(*f)(void*),void*p){if(action_error){f(p);return action_error;}cleanup=f;cleanup_data=p;return 0;}
static int devm_device_init_wakeup(struct device*d){wakeup_calls++;if(!wakeup_error)d->wakeup=true;return wakeup_error;}
static int devm_pm_set_wake_irq(struct device*d,int i){irq_calls++;if(!irq_error)d->wake_irq=i;return irq_error;}
#define IRQF_NO_SUSPEND 0x4000
#define IRQF_NO_AUTOEN 0x80000
static unsigned long rx_flags;static int rx_irq_requested;
static int dockchannel_rx_irq(int i,void*p){return 0;}
static int dockchannel_rx_irq_thread(int i,void*p){return 0;}
static int devm_request_threaded_irq(struct device*d,int irq,int(*h)(int,void*),int(*t)(int,void*),unsigned long f,const char*n,void*p){
 assert(h==dockchannel_rx_irq&&t==dockchannel_rx_irq_thread&&!strcmp(n,"apple-dockchannel-rx"));rx_irq_requested=irq;rx_flags=f;return 0;}
enum dwc3_apple_state {DWC3_APPLE_PROBE_PENDING,DWC3_APPLE_NO_CABLE,DWC3_APPLE_HOST,DWC3_APPLE_DEVICE,DWC3_APPLE_SUSPENDED};
#define DWC3_GCTL_PRTCAP_HOST 1
struct dwc3 {int num_usb3_ports,num_usb2_ports;int usb3_generic_phy[2],usb2_generic_phy[2],usb3_phy,usb2_phy;int current_dr_role;bool dis_u3_susphy_quirk;};
struct dwc3_apple {struct device*dev;struct dwc3 dwc;int lock,reset;enum dwc3_apple_state state;bool fixed_hub,host_only,rpm_held,core_active,core_exit_pending,reset_pending,dt_dis_u3_susphy;unsigned usb3_power_off_pending,usb2_power_off_pending;};
static int core_error,host_error,gadget_error,reset_error,role_error,start_role;
static unsigned rpm_puts;
/* A started core holds the glue runtime PM reference until core_stop drops it. */
static int dwc3_apple_core_start(struct dwc3_apple*a,enum dwc3_apple_state s){start_role=a->dwc.current_dr_role;if(core_error)return core_error;a->core_active=true;a->reset_pending=true;a->rpm_held=true;return 0;}
static int dwc3_apple_set_role(struct dwc3_apple*a,enum dwc3_apple_state s){assert(a->core_active);return role_error;}
static void pm_runtime_mark_last_busy(struct device*d){}
static void pm_runtime_put_autosuspend(struct device*d){rpm_puts++;}
static int dwc3_host_init(struct dwc3*d){return host_error;}
static int dwc3_gadget_init(struct dwc3*d){return gadget_error;}
static void dwc3_host_exit(struct dwc3*d){role_exits++;}
static void dwc3_gadget_exit(struct dwc3*d){role_exits++;}
static void dwc3_enable_susphy(struct dwc3*d,bool v){assert(v);}
static void dwc3_core_exit(struct dwc3*d){core_exits++;}
static int reset_control_assert(int r){reset_calls++;return reset_error;}
#define BIT(x) (1UL << (x))
static void dwc3_event_buffers_cleanup(struct dwc3*d){}
static int phy_power_off(int p){return 0;}
static void usb_phy_set_suspend(int p,int suspend){}
static void dwc3_core_exit_after_phy_power_off(struct dwc3*d){core_exits++;}
static u16 get_unaligned_le16(const u8*p){return p[0]|p[1]<<8;}
static u32 get_unaligned_le32(const u8*p){return p[0]|p[1]<<8|p[2]<<16|p[3]<<24;}
static void put_unaligned_le16(u16 v,u8*p){p[0]=v;p[1]=v>>8;}
static void put_unaligned_le32(u32 v,u8*p){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
#define IEEE80211_CHAN_DISABLED 1
#define IEEE80211_CHAN_RADAR 2
#define IEEE80211_CHAN_NO_OFDM 4
#define IEEE80211_CHAN_NO_20MHZ 8
#define IEEE80211_CHAN_PSD 16
#define IEEE80211_CHAN_IR_CONCURRENT 32
#define IEEE80211_CHAN_NO_IR 64
#define WLAN_REASON_DEAUTH_LEAVING 3
#define __assign_bit(i,b,v) ((b)[(i)/64]=((b)[(i)/64]&~(1ULL<<((i)%64)))|((uint64_t)!!(v)<<((i)%64)))
#define for_each_set_bit(i,b,n) for((i)=0;(i)<(n);(i)++) if((b)[(i)/64]&(1ULL<<((i)%64)))
#define bitmap_zero(b,n) memset((b),0,((n)+63)/64*8)
#define IEEE80211_CHAN_NO_6GHZ_VLP_CLIENT 128
#define NL80211_RRF_NO_IR (1<<7)
struct ieee80211_channel {unsigned flags,hw_value;int max_power;enum nl80211_band band;int center_freq;s8 psd;};
static u64 get_unaligned_le64(const u8*p){return get_unaligned_le32(p)|(u64)get_unaligned_le32(p+4)<<32;}
static const void *memchr_inv(const void*p,int c,size_t n){const u8*b=p;for(size_t i=0;i<n;i++)if(b[i]!=(u8)c)return b+i;return NULL;}
struct wiphy {void*priv;};struct net_device {int unused;};
struct regulatory_request {u8 alpha2[2];unsigned initiator;};
/* cfg80211 rule lookup for 6 GHz channels; 2.4 and 5 GHz never ask. */
struct ieee80211_reg_rule {u32 flags;};
static struct ieee80211_reg_rule rule6;static bool rule6_absent;static unsigned rule_lookups;
static const struct ieee80211_reg_rule *freq_reg_info(struct wiphy*w,u32 khz){rule_lookups++;assert(khz%1000==0&&khz>=5955000&&khz<=7115000&&(khz/1000-5955)%20==0);return rule6_absent?ERR_PTR(-ERANGE):&rule6;}
static unsigned rtnl_depth,wiphy_depth,schedules;
static void rtnl_lock(void){assert(!rtnl_depth++);}
static void rtnl_unlock(void){assert(rtnl_depth--==1);}
static void wiphy_lock(struct wiphy*w){assert(rtnl_depth&& !wiphy_depth++);}
static void wiphy_unlock(struct wiphy*w){assert(wiphy_depth--==1);}
struct mt7932 {struct ieee80211_channel channels[MT7932_CHANNELS_2G],channels5[MT7932_CHANNELS_5G],channels6[MT7932_CHANNELS_6G];struct wiphy*wiphy;struct net_device*netdev;struct {struct device dev;}*pdev;struct mt7932_reg_snapshot reg_desired,reg_deferred;unsigned reg_generation,response_lock,startup_work;bool reg_pending,stopping,interface_registered,connecting,connected,disconnecting,reg_deferral,data_ready;int data_lock;int connect_error,assoc_start,assoc_done,discovery_done;uint64_t policy_disabled[(MT7932_CHANNELS+63)/64];};
#define spin_lock(l) ((void)(l))
#define spin_unlock(l) ((void)(l))
static void mt_data_kick_locked(struct mt7932*m){assert(0 && "unexpected TX boundary");}
static struct mt7932 *mt_from_wiphy(struct wiphy*w){return w->priv;}
static int mt7932_domain_5g_bw(void){return 0;}
static void netif_stop_queue(struct net_device*n){}
static void complete(int*c){}
static void schedule_work(unsigned*w){schedules++;}
'''

MAIN = r'''
int main(int argc,char**argv){
 assert(argc==2);struct device d={};struct dockchannel dc={.dev=&d,.rx_irq=42};
 if(!strcmp(argv[1],"wake")){
  /* The FIFO's device is a wakeup source; the receive IRQ is not a wake IRQ. */
  assert(!dockchannel_init_wakeup(&dc)&&d.wakeup&&!d.wake_irq&&!irq_calls&&wakeup_calls==1);
  wakeup_error=-ENOMEM;d.wakeup=false;assert(dockchannel_init_wakeup(&dc)==-ENOMEM&&!d.wakeup&&!irq_calls);
  /* It keeps receiving through system sleep, so the client filters wake data. */
  assert(!dockchannel_request_rx(&d,&dc)&&rx_irq_requested==42&&rx_flags==(IRQF_NO_AUTOEN|IRQF_NO_SUSPEND));
 }else if(!strcmp(argv[1],"sep")){
  domain_count=1;assert(!sep_pm_keep_domains(&d)&&!attach_calls);
  domain_count=2;attach_error=-EAGAIN;assert(sep_pm_keep_domains(&d)==-EAGAIN&&!pd0.syscore);
  attach_error=0;assert(!sep_pm_keep_domains(&d)&&pd0.syscore&&pd1.syscore&&cleanup);
  cleanup(cleanup_data);assert(!pd0.syscore&&!pd1.syscore);
  action_error=-ENOMEM;assert(sep_pm_keep_domains(&d)==-ENOMEM&&!pd0.syscore&&!pd1.syscore);
 }else if(!strcmp(argv[1],"usb")){
  for(unsigned fixed=0;fixed<2;fixed++)for(unsigned host_only=0;host_only<2;host_only++)for(unsigned role=DWC3_APPLE_HOST;role<=DWC3_APPLE_DEVICE;role++){
   struct dwc3_apple a={.dev=&d,.fixed_hub=fixed,.host_only=host_only,.state=DWC3_APPLE_PROBE_PENDING};
   unsigned puts=rpm_puts;
   assert(!dwc3_apple_init(&a,role)&&d.syscore&&a.state==role&&a.rpm_held);
   /* Only a host-only host start marks the role before the core comes up. */
   assert(start_role==(host_only&&role==DWC3_APPLE_HOST?DWC3_GCTL_PRTCAP_HOST:0));
   assert(!dwc3_apple_exit(&a)&&!d.syscore&&a.state==DWC3_APPLE_NO_CABLE&&!a.rpm_held&&rpm_puts==puts+1);
   unsigned n=core_exits;assert(!dwc3_apple_exit(&a)&&core_exits==n&&rpm_puts==puts+1);
   core_error=-EIO;assert(dwc3_apple_init(&a,role)==-EIO&&!d.syscore&&!a.rpm_held);core_error=0;
   host_error=gadget_error=-ENOMEM;n=core_exits;assert(dwc3_apple_init(&a,role)==-ENOMEM&&!d.syscore&&core_exits==n+1&&!a.rpm_held&&rpm_puts==puts+2);host_error=gadget_error=0;
   /* A refused PHY role stops the core again before any host or gadget start. */
   role_error=-EIO;n=core_exits;unsigned exits=role_exits;
   assert(dwc3_apple_init(&a,role)==-EIO&&!d.syscore&&!a.core_active&&!a.rpm_held&&core_exits==n+1&&role_exits==exits&&a.state==DWC3_APPLE_NO_CABLE&&rpm_puts==puts+3);role_error=0;
   assert(!dwc3_apple_init(&a,role)&&d.syscore);reset_error=-EIO;assert(dwc3_apple_exit(&a)==-EIO&&d.syscore&&a.reset_pending&&a.rpm_held&&rpm_puts==puts+3);
   reset_error=0;n=core_exits;exits=role_exits;
   assert(!dwc3_apple_exit(&a)&&!d.syscore&&!a.reset_pending&&!a.rpm_held&&rpm_puts==puts+4);
   assert(core_exits==n&&role_exits==exits);
  }
 }else if(!strcmp(argv[1],"regulatory")){
  enum {N25=MT7932_CHANNELS_2G+MT7932_CHANNELS_5G,SIX13=N25+3,VLP=N25+5};
  struct mt7932 m={.reg_generation=7};struct wiphy w={.priv=&m};m.wiphy=&w;
  struct mt7932_policy p={};u8 tables[9][1020]={};
  for(unsigned i=0;i<9;i++){p.table[i+1]=tables[i];tables[i][4]=0;tables[i][5]=i<2?1:2;}
  for(unsigned i=0;i<N25;i++){
   unsigned t=i<8?0:i<MT7932_CHANNELS_2G?1:2+(i-MT7932_CHANNELS_2G)/8;
   unsigned j=i<8?i:i<MT7932_CHANNELS_2G?i-8:(i-MT7932_CHANNELS_2G)%8;
   tables[t][4]=j+1;
   struct ieee80211_channel*c=mt_channel(&m,i);
   c->band=i<MT7932_CHANNELS_2G?NL80211_BAND_2GHZ:NL80211_BAND_5GHZ;
   c->hw_value=i<MT7932_CHANNELS_2G?i+1:mt7932_channel_5g(i-MT7932_CHANNELS_2G);c->max_power=20;
   memset(tables[t]+44+j*122,0xc4,122);tables[t][44+j*122]=c->hw_value;
   if(i!=10)tables[t][45+j*122]=1;
  }
  /* 6 GHz: VLP permission bitmap, NO_IR until cfg80211's rule lifts it, PSD limits. */
  p.six_channels=GENMASK_ULL(MT7932_CHANNELS_6G-1,0)&~BIT_ULL(3);
  for(unsigned k=0;k<MT7932_CHANNELS_6G;k++){
   struct ieee80211_channel*c=mt_channel(&m,N25+k);assert(c==&m.channels6[k]);
   c->band=NL80211_BAND_6GHZ;c->hw_value=mt7932_channel_6g(k);c->center_freq=5950+5*c->hw_value;
   c->max_power=14;c->psd=-1;c->flags=IEEE80211_CHAN_NO_IR|IEEE80211_CHAN_PSD|(N25+k==VLP?IEEE80211_CHAN_NO_6GHZ_VLP_CLIENT:0);
  }
  assert(!mt7932_policy_permits(&p,NL80211_BAND_2GHZ,11)&&mt7932_policy_permits(&p,NL80211_BAND_2GHZ,10)&&mt7932_policy_permits(&p,NL80211_BAND_5GHZ,165)&&!mt7932_policy_permits(&p,NL80211_BAND_2GHZ,14));
  /* Channel numbers mean nothing without their band. */
  assert(mt7932_policy_permits(&p,NL80211_BAND_2GHZ,13)&&!mt7932_policy_permits(&p,NL80211_BAND_6GHZ,13)&&mt7932_policy_permits(&p,NL80211_BAND_6GHZ,1));
  assert(!mt7932_policy_permits(&p,NL80211_BAND_5GHZ,1)&&!mt7932_policy_permits(&p,NL80211_BAND_2GHZ,36)&&!mt7932_policy_permits(&p,NL80211_BAND_6GHZ,3)&&!mt7932_policy_permits(&p,NUM_NL80211_BANDS,1));
  struct regulatory_request request={.alpha2={'U','S'}};rule_lookups=0;mt_regulatory_notify(&w,&request);m.reg_pending=false;
  assert(rule_lookups==MT7932_CHANNELS_6G);
  /* The VLP-client channel stays out of the firmware domain; the others lose NO_IR there. */
  assert(m.reg_desired.length==12+(MT7932_CHANNELS-1)*8&&m.reg_desired.domain[8]==13&&m.reg_desired.domain[9]==MT7932_CHANNELS_5G&&m.reg_desired.domain[10]==MT7932_CHANNELS_6G-1);
  for(unsigned i=0;i<MT7932_CHANNELS-1;i++){
   const u8*e=m.reg_desired.domain+12+i*8;u32 f=get_unaligned_le32(e+4);
   if(i<N25)assert(get_unaligned_le16(e)==mt_channel(&m,i)->hw_value&&!f);
   else{unsigned k=i-N25+(i>=VLP);assert(get_unaligned_le16(e)==mt7932_channel_6g(k)&&f==IEEE80211_CHAN_PSD);}
  }
  for(unsigned k=0;k<MT7932_CHANNELS_6G;k++)assert(m.reg_desired.power[N25+k]==14&&m.reg_desired.psd[N25+k]==-1);
  unsigned generation=m.reg_generation;mt_policy_disable(&m,&p,generation-1);assert(!m.channels[10].flags&&!m.policy_disabled[0]&&!m.policy_disabled[1]);
  assert(m.channels6[0].flags&IEEE80211_CHAN_NO_IR);
  rule_lookups=0;mt_policy_disable(&m,&p,generation);
  assert(m.channels[10].flags==1&&m.policy_disabled[0]==((1ULL<<10)|(1ULL<<SIX13))&&!m.policy_disabled[1]);
  assert(rule_lookups==MT7932_CHANNELS_6G-1&&(m.channels6[3].flags&IEEE80211_CHAN_DISABLED)&&(m.channels6[3].flags&IEEE80211_CHAN_NO_IR));
  for(unsigned k=0;k<MT7932_CHANNELS_6G;k++)if(k!=3)assert(!(m.channels6[k].flags&(IEEE80211_CHAN_NO_IR|IEEE80211_CHAN_DISABLED)));
  struct mt7932_reg_snapshot reg=m.reg_desired;mt7932_policy_filter(&reg,&p);
  assert(reg.length==12+(MT7932_CHANNELS-3)*8&&reg.domain[8]==12&&reg.domain[9]==MT7932_CHANNELS_5G&&reg.domain[10]==MT7932_CHANNELS_6G-2);
  for(unsigned i=0;i<MT7932_CHANNELS-3;i++){unsigned c=get_unaligned_le16(reg.domain+12+i*8);assert(i<12?c!=11:i>=N25-1?c!=13:true);}
  /* A rule that keeps NO_IR, or no rule at all, leaves the channel passive. */
  m.channels6[0].flags|=IEEE80211_CHAN_NO_IR;m.channels6[1].flags|=IEEE80211_CHAN_NO_IR;
  rule6.flags=NL80211_RRF_NO_IR;mt_policy_disable(&m,&p,generation);assert(m.channels6[0].flags&IEEE80211_CHAN_NO_IR);
  rule6.flags=0;rule6_absent=true;mt_policy_disable(&m,&p,generation);assert(m.channels6[1].flags&IEEE80211_CHAN_NO_IR);
  rule6_absent=false;mt_policy_disable(&m,&p,generation);assert(!(m.channels6[0].flags&IEEE80211_CHAN_NO_IR)&&!(m.channels6[1].flags&IEEE80211_CHAN_NO_IR));
  m.channels[10].flags=0;m.channels6[3].flags=IEEE80211_CHAN_NO_IR|IEEE80211_CHAN_PSD;mt_regulatory_notify(&w,&request);
  assert(m.reg_generation==generation&&m.channels[10].flags==1&&(m.channels6[3].flags&IEEE80211_CHAN_DISABLED));
  m.channels[10].flags=0;m.channels6[3].flags=IEEE80211_CHAN_NO_IR|IEEE80211_CHAN_PSD;request.alpha2[0]='C';request.alpha2[1]='A';mt_regulatory_notify(&w,&request);
  assert(m.reg_generation==generation+1&&!m.policy_disabled[0]&&!m.policy_disabled[1]);
  m.reg_pending=false;m.connected=true;unsigned applied=m.reg_generation;
  put_unaligned_le32(IEEE80211_CHAN_NO_IR,m.reg_desired.domain+16);
  mt_regulatory_notify(&w,&request);
  assert(m.reg_generation==applied&&!m.reg_pending&&m.reg_deferral);
  assert(get_unaligned_le32(m.reg_desired.domain+16)==IEEE80211_CHAN_NO_IR);
  assert(get_unaligned_le32(m.reg_deferred.domain+16)==0);
  m.stopping=true;unsigned n=m.reg_generation;request.alpha2[0]='U';mt_regulatory_notify(&w,&request);assert(m.reg_generation==n);
 }
 puts("PASS");return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--old-usb', action='store_true')
    parser.add_argument('--old-regulatory', action='store_true')
    parser.add_argument('--mutation', choices=['sep-syscore', 'wake-irq', 'rx-suspend', 'stale-policy', 'usb-rpm', 'vlp-client', 'six-band'])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    files = {
        'drivers/soc/apple/dockchannel.c': ['dockchannel_init_wakeup'],
        'drivers/soc/apple/pm_shim.c': ['sep_pm_release_domains', 'sep_pm_keep_domains'],
        'drivers/usb/dwc3/dwc3-apple.c': ['dwc3_apple_core_stop', 'dwc3_apple_init', 'dwc3_apple_exit'],
        'drivers/net/wireless/mediatek/mt7932/startup.c': ['mt_channel', 'mt_policy_disable', 'mt_regulatory_notify'],
    }
    bodies = {}
    for path, names in files.items():
        source = (ROOT/path).read_text()
        for name in names:
            text = source
            if args.old_usb and path.endswith('dwc3-apple.c') and name != 'dwc3_apple_core_stop' or args.old_regulatory and name == 'mt_regulatory_notify':
                text = subprocess.check_output(['git', 'show', '990a45b609dd:'+path], cwd=ROOT, text=True)
            bodies[name] = function(text, name)
    headers = [ROOT/'drivers/net/wireless/mediatek/mt7932/channels.h',
               ROOT/'drivers/net/wireless/mediatek/mt7932/regulatory.h']
    header = '\n'.join(re.sub(r'^#include .*\n', '', path.read_text(), flags=re.M)
                       for path in headers)
    # The receive IRQ request statement from dockchannel_init(), run against a recording stub.
    dock = (ROOT/'drivers/soc/apple/dockchannel.c').read_text()
    request = re.search(r'\tret = devm_request_threaded_irq\(dev, dockchannel->rx_irq,.*?\);\n', dock, re.S)
    if not request:
        raise ValueError('dockchannel RX IRQ request')
    bodies['dockchannel_request_rx'] = ('static int dockchannel_request_rx(struct device *dev, struct dockchannel *dockchannel)\n'
                                        '{\n\tint ret;\n\n' + request[0] + '\treturn ret;\n}\n')
    at = PREFIX.index('struct mt7932 {')
    code = PREFIX[:at] + header + PREFIX[at:] + '\n'.join(bodies.values()) + MAIN
    if not args.old_usb:
        code = code.replace(',DWC3_APPLE_SUSPENDED', '')
    mutations = {
        'sep-syscore': ('dev_pm_syscore_device(list->pd_devs[i], true);', 'dev_pm_syscore_device(list->pd_devs[i], false);'),
        'wake-irq': ('return devm_device_init_wakeup(dockchannel->dev);',
                     'return devm_device_init_wakeup(dockchannel->dev) ?: devm_pm_set_wake_irq(dockchannel->dev, dockchannel->rx_irq);'),
        'rx-suspend': ('IRQF_NO_AUTOEN | IRQF_NO_SUSPEND', 'IRQF_NO_AUTOEN'),
        'stale-policy': ('generation != READ_ONCE(m->reg_generation)', 'false'),
        'usb-rpm': ('pm_runtime_put_autosuspend(appledwc->dev);', '(void)0;'),
        'vlp-client': ('if (flags & IEEE80211_CHAN_NO_6GHZ_VLP_CLIENT)', 'if (0)'),
        'six-band': ('(policy->six_channels & BIT_ULL((channel - 1) / 4))', 'true'),
    }
    if args.mutation:
        old, new = mutations[args.mutation]
        # A mutation that no longer matches the driver would test nothing.
        if code.count(old) != 1:
            parser.error(args.mutation + ' mutation no longer matches the driver source')
        code = code.replace(old, new)
    source = args.output/'controls.c'; source.write_text(code)
    binary = args.output/'controls'
    subprocess.run(['cc', '-std=gnu11', '-g', '-Wall', '-Werror', '-Wno-unused-function',
                    '-Wno-unused-variable', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                    str(source), '-o', str(binary)], check=True)
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    results = []
    for case in ['wake', 'sep', 'usb', 'regulatory']:
        run = subprocess.run([str(binary), case], capture_output=True, text=True, preexec_fn=no_core)
        results.append({'case': case, 'exit': run.returncode, 'stderr': run.stderr})
    receipt = {'bodies': {n: hashlib.sha256(b.encode()).hexdigest() for n,b in bodies.items()},
               'translation_sha256': hashlib.sha256(code.encode()).hexdigest(),
               'old_usb': args.old_usb, 'old_regulatory': args.old_regulatory,
               'mutation': args.mutation, 'results': results}
    (args.output/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(results, indent=2))
    if any(x['exit'] for x in results):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
