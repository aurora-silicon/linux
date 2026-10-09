#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Controls for current14 T8122 GPU/PMP links and supplier readiness."""
import argparse
from pathlib import Path
import subprocess
import resource
import signal
import tempfile
ROOT=Path(__file__).resolve().parents[2]
def function(s,marker):
    a=s.index(marker);b=s.index('{',a);n=1;e=b+1
    while n:n+=(s[e]=='{')-(s[e]=='}');e+=1
    return s[a:e]
PREFIX=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <arpa/inet.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define __init
#define ENODEV 19
#define EINVAL 22
#define EIO 5
#define EPROBE_DEFER 517
#define ETIMEDOUT 110
#define PMP_REPORT_READY 1
typedef uint64_t u64;typedef uint32_t u32;typedef uint16_t u16;typedef uint8_t u8;
struct device_node{const char *compatible,*full_name,*status;bool available,has_reg;unsigned phandle;u32 reg;unsigned reg_length;int refs;struct device_node *target,*report,*child,*sibling;};
struct device{struct device_node *of_node;int refs;};
struct platform_device{struct device dev;};
struct apple_pmp_report_offsets{unsigned status;};
struct apple_pmp_report{void *base;struct apple_pmp_report_offsets *offsets;};
struct of_changeset{int added;};struct gate_soc{int chip;};
static const struct gate_soc gate_t8122={8122},gate_t6030={6030};static const struct gate_soc *gate_soc;
static struct device_node gpu,pmp,other,report14,entry7,entry17,report25;
static struct platform_device report_device;static struct device supplier;
static struct apple_pmp_report rep;static struct apple_pmp_report_offsets offsets;
static struct device *pmp_device=&supplier;static const void *pmp_data=(void*)1;
static bool machine8122,missing_gpu,missing_report,missing_native_device,lockable,bound;
static int ready_result,add_result,power_result,ready_calls,add_calls,poll_calls,power_calls,lock_depth;
static u64 ptd_status;
static void of_node_put(struct device_node*n){if(n){assert(n->refs>0);n->refs--;}}
static struct device_node *ref(struct device_node*n){if(n)n->refs++;return n;}
static struct device_node *of_node_get(struct device_node*n){return ref(n);}
struct property {void *value;int length;};
static int devtree_lock,dt_lock_depth;
#define raw_spin_lock_irqsave(lock,flags) ((void)(lock),(flags)=0,assert(!dt_lock_depth++))
#define raw_spin_unlock_irqrestore(lock,flags) ((void)(lock),(void)(flags),assert(dt_lock_depth--==1))
#define ERR_PTR(err) ((void*)(intptr_t)(err))
#define IS_ERR(ptr) ((uintptr_t)(ptr) >= (uintptr_t)-4095)
#define PTR_ERR(ptr) ((long)(intptr_t)(ptr))
#define pr_err(...) ((void)0)
typedef u32 __be32;
static u32 be32_to_cpup(const __be32*p){return ntohl(*p);}
static const char *kbasename(const char*s){const char*p=strrchr(s,'/');return p?p+1:s;}
/* Property storage and locking/refcount primitives are controlled boundaries. */
static struct property *of_find_property(const struct device_node*n,const char*s,int*length){
 static struct property p;static u32 wire_reg[2];
 if(!n)return NULL;
 if(!strcmp(s,"reg")){
  if(!n->has_reg)return NULL;
  wire_reg[0]=htonl(n->reg);wire_reg[1]=0;p.value=wire_reg;p.length=n->reg_length;
 }else if(!strcmp(s,"compatible")){if(!n->compatible)return NULL;p.value=(void*)n->compatible;p.length=strlen(p.value)+1;
 }else if(!strcmp(s,"device_type")){return NULL;
 }else{assert(!strcmp(s,"status"));if(!n->status&&n->available)return NULL;p.value=(void*)(n->status?n->status:"disabled");p.length=strlen(p.value)+1;}
 if(length)*length=p.length;return &p;
}
static struct property *__of_find_property(const struct device_node*n,const char*s,int*length){return of_find_property(n,s,length);}
#define of_compat_cmp(s1,s2,len) strcasecmp((s1),(s2))
static const void *__of_get_property(const struct device_node*n,const char*s,int*length){
 struct property*p=of_find_property(n,s,length);return p?p->value:NULL;
}
static bool of_machine_is_compatible(const char*s){assert(!strcmp(s,"apple,t8122"));return machine8122;}
static struct device_node *of_find_compatible_node(void*a,void*b,const char*s){assert(!a&&!b);if(!strcmp(s,"apple,agx-t8122"))return missing_gpu?NULL:ref(&gpu);assert(!strcmp(s,"apple,t8122-pmp-v2-report"));return missing_report?NULL:ref(&report14);}
static struct device_node *of_parse_phandle(struct device_node*n,const char*s,int i){assert(!i);if(!strcmp(s,"apple,pmp"))return ref(n->target);assert(!strcmp(s,"apple,pmp-report"));return ref(n->report);}
/* OF_IMPLEMENTATION */
static int of_changeset_add_prop_u32(struct of_changeset*c,struct device_node*n,const char*s,u32 v){assert(n==&gpu&&!strcmp(s,"apple,pmp")&&v==pmp.phandle);add_calls++;if(!add_result)c->added++;return add_result;}
static unsigned long msecs_to_jiffies(unsigned ms){return ms;}
static int apple_pmp_report_wait_ready(struct device_node*n,unsigned long t){assert(n==&entry7&&t==5000);ready_calls++;return ready_result;}
static struct platform_device *of_find_device_by_node(struct device_node*n){assert(n==&report25);if(missing_native_device)return NULL;report_device.dev.refs++;return &report_device;}
static bool device_trylock(struct device*d){assert(d==&report_device.dev);return lockable;}
static void device_unlock(struct device*d){assert(d==&report_device.dev);}
static bool device_is_bound(struct device*d){assert(d==&report_device.dev);return bound;}
static struct apple_pmp_report *platform_get_drvdata(struct platform_device*d){assert(d==&report_device);return &rep;}
static void put_device(struct device*d){assert(d->refs>0);d->refs--;}
#define readq_poll_timeout(addr,value,cond,interval,timeout) ((void)(addr),(void)(interval),assert((timeout)==5000000),poll_calls++,value=ptd_status,(cond)?0:-ETIMEDOUT)
static int pmp_lock;
struct guard_state{int dummy;};
static struct guard_state enter(int*p){assert(p==&pmp_lock&&!lock_depth);lock_depth++;return(struct guard_state){0};}
static void leave(struct guard_state*g){(void)g;assert(lock_depth==1);lock_depth--;}
#define guard(kind) struct guard_state held __attribute__((cleanup(leave)))=enter
static int apple_pmp_send_power_command(const void*d,u8 op,u16 id,u32 enabled){assert(d==pmp_data&&lock_depth==1&&op==0xf&&id==5&&enabled<=1);power_calls++;return power_result;}
static void reset(void){
 gpu=(struct device_node){.compatible="apple,agx-t8122",.available=true,.phandle=1};
 pmp=(struct device_node){.compatible="apple,t8122-pmp-v2",.phandle=2};other=(struct device_node){.compatible="apple,other",.phandle=3};
 entry7=(struct device_node){.compatible="apple,t6000-pmp-v2-report-entry",.full_name="/soc/pmp-report/report@7",.available=true,.has_reg=true,.reg=7,.reg_length=4,.phandle=4};
 entry17=(struct device_node){.compatible="apple,t6000-pmp-v2-report-entry",.full_name="/soc/pmp-report/report@11",.available=true,.has_reg=true,.reg=17,.reg_length=4,.phandle=5,.sibling=&entry7};
 report14=(struct device_node){.compatible="apple,t8122-pmp-v2-report",.target=&pmp,.child=&entry17};
 report25=(struct device_node){.compatible="apple,j613-25g83-pmp-report"};
 supplier=(struct device){.of_node=&pmp};report_device=(struct platform_device){0};
 offsets=(struct apple_pmp_report_offsets){.status=0};rep=(struct apple_pmp_report){.base=&ptd_status,.offsets=&offsets};
 gate_soc=&gate_t8122;machine8122=true;missing_gpu=missing_report=missing_native_device=false;lockable=bound=true;
 ready_result=add_result=power_result=ready_calls=add_calls=poll_calls=power_calls=0;ptd_status=1;assert(!lock_depth);
}
static void balanced(void){assert(!gpu.refs&&!pmp.refs&&!other.refs&&!report14.refs&&!entry7.refs&&!entry17.refs&&!report25.refs&&!report_device.dev.refs&&!lock_depth&&!dt_lock_depth);}
'''
CONTROLS=r'''
int main(void){
 struct of_changeset cs;
 reset();cs=(struct of_changeset){0};assert(gate_t8122_gpu_pmp_link(&cs,&pmp)==0&&cs.added==1);balanced();
 reset();gpu.target=&pmp;cs=(struct of_changeset){0};assert(gate_t8122_gpu_pmp_link(&cs,&pmp)==0&&!add_calls);balanced();
 reset();gpu.target=&other;assert(gate_t8122_gpu_pmp_link(&cs,&pmp)==-EINVAL&&!add_calls);balanced();
 reset();gpu.available=false;assert(gate_t8122_gpu_pmp_link(&cs,&pmp)==0&&!add_calls);balanced();
 reset();missing_gpu=true;assert(gate_t8122_gpu_pmp_link(&cs,&pmp)==0&&!add_calls);balanced();
 reset();gate_soc=&gate_t6030;assert(gate_t8122_gpu_pmp_link(&cs,&pmp)==0&&!add_calls);balanced();
 reset();pmp.phandle=0;assert(gate_t8122_gpu_pmp_link(&cs,&pmp)==-EINVAL&&!add_calls);balanced();
 reset();add_result=-EIO;assert(gate_t8122_gpu_pmp_link(&cs,&pmp)==-EIO);balanced();
 reset();assert(apple_pmp_set_device_power(0xf,5,1)==0&&ready_calls==1&&power_calls==1&&!poll_calls);balanced();
 reset();ready_result=-EPROBE_DEFER;assert(apple_pmp_set_device_power(0xf,5,1)==-EPROBE_DEFER&&!power_calls);balanced();
 reset();ready_result=-ETIMEDOUT;assert(apple_pmp_set_device_power(0xf,5,1)==-ETIMEDOUT&&!power_calls);balanced();
 reset();ready_result=-EIO;assert(apple_pmp_set_device_power(0xf,5,1)==-EIO&&!power_calls);balanced();
 reset();power_result=-ETIMEDOUT;assert(apple_pmp_set_device_power(0xf,5,1)==-ETIMEDOUT&&power_calls==1);balanced();
 reset();report14.target=&other;assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!power_calls&&!ready_calls);balanced();
 reset();report14.child=NULL;assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!power_calls);balanced();
 reset();report14.child=&entry7;entry7.sibling=&entry17;entry17.sibling=NULL;assert(apple_pmp_set_device_power(0xf,5,1)==0&&ready_calls==1&&power_calls==1);balanced();
 reset();entry17.sibling=NULL;assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!ready_calls&&!power_calls);balanced();
 reset();entry7.status="ok";assert(apple_pmp_set_device_power(0xf,5,1)==0&&ready_calls==1&&power_calls==1);balanced();
 reset();entry7.status="okay";assert(apple_pmp_set_device_power(0xf,5,1)==0&&ready_calls==1&&power_calls==1);balanced();
 reset();entry7.status="reserved";assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!ready_calls&&!power_calls);balanced();
 reset();entry7.available=false;assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!ready_calls&&!power_calls);balanced();
 reset();entry17.reg=7;assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!ready_calls&&!power_calls);balanced();
 reset();entry17.reg=7;entry17.available=false;assert(apple_pmp_set_device_power(0xf,5,1)==0&&ready_calls==1&&power_calls==1);balanced();
 reset();entry7.reg_length=8;assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!ready_calls&&!power_calls);balanced();
 reset();entry7.reg_length=3;assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!ready_calls&&!power_calls);balanced();
 reset();entry7.reg=8;assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!ready_calls&&!power_calls);balanced();
 reset();entry7.has_reg=false;assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!ready_calls&&!power_calls);balanced();
 reset();entry7.compatible="apple,other";assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!ready_calls&&!power_calls);balanced();
 reset();missing_report=true;assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!power_calls);balanced();
 reset();machine8122=false;assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!power_calls);balanced();
 reset();pmp.compatible="apple,t6030-pmp-v2";assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!power_calls);balanced();
 reset();assert(apple_pmp_set_device_power(1,5,1)==-EINVAL&&!power_calls&&!ready_calls);balanced();
 reset();pmp.report=&report25;assert(apple_pmp_set_device_power(0xf,5,1)==0&&poll_calls==1&&!ready_calls&&power_calls==1);balanced();
 reset();pmp.report=&report25;ptd_status=0;assert(apple_pmp_set_device_power(0xf,5,1)==-ETIMEDOUT&&!ready_calls&&!power_calls);balanced();
 reset();pmp.report=&report25;missing_native_device=true;assert(apple_pmp_set_device_power(0xf,5,1)==-EPROBE_DEFER&&!power_calls);balanced();
 reset();pmp.report=&report25;lockable=false;assert(apple_pmp_set_device_power(0xf,5,1)==-EPROBE_DEFER&&!power_calls);balanced();
 reset();pmp.report=&report25;bound=false;assert(apple_pmp_set_device_power(0xf,5,1)==-EPROBE_DEFER&&!power_calls);balanced();
 reset();pmp.report=&report25;report25.compatible="apple,t8122-pmp-v2-report";assert(apple_pmp_set_device_power(0xf,5,1)==-ENODEV&&!ready_calls&&!power_calls);balanced();
 puts("PASS production GPU link, supplier-ready and power bridge: current14 positive/refusal, unchanged native25 dispatch and balanced references");
}
'''
def run(code,p):
 p.write_text(code);subprocess.run(['clang','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-sign-compare','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 return subprocess.run([str(p.with_suffix(''))],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
def of_implementation():
 base=(ROOT/'drivers/of/base.c').read_text()
 prop=(ROOT/'drivers/of/property.c').read_text()
 header=(ROOT/'include/linux/of.h').read_text()
 bodies=[function(base,'bool of_node_name_eq('),function(base,'static bool __of_node_is_type('),
  function(prop,'const char *of_prop_next_string('),function(base,'static int __of_device_is_compatible('),
  function(base,'int of_device_is_compatible(')]
 bodies.extend(function(base,s) for s in [
  'static struct device_node *__of_get_next_child(',
  'struct device_node *of_get_next_child(', 'static bool __of_device_is_status(',
  'static bool __of_device_is_available(', 'bool of_device_is_available(',
  'static struct device_node *of_get_next_status_child(',
  'struct device_node *of_get_next_available_child('])
 macros='#define for_each_child_of_node(n,c) for(c=of_get_next_child(n,NULL);c;c=of_get_next_child(n,c))\n'
 macros+='#define for_each_available_child_of_node(n,c) for(c=of_get_next_available_child(n,NULL);c;c=of_get_next_available_child(n,c))\n'
 bodies.append(function(base,'struct device_node *of_get_child_by_name('))
 bodies.extend(function(prop,s) for s in ['int of_property_count_elems_of_size(',
  'static void *of_find_property_value_of_size(', 'int of_property_read_variable_u32_array('])
 bodies.extend(function(header,s) for s in ['static inline int of_property_count_u32_elems(',
  'static inline int of_property_read_u32_array(', 'static inline int of_property_read_u32('])
 return macros+'\n'.join(bodies)
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--old-source',type=Path);parser.add_argument('--old-lookup-ref',default='0be9f0c1d769bf94b9fbf5b60ce651525fc2922b');args=parser.parse_args();resource.setrlimit(resource.RLIMIT_CORE,(0,0))
 prefix=PREFIX.replace('/* OF_IMPLEMENTATION */',of_implementation())
 gate=(ROOT/'drivers/soc/apple/t6030-display-gate.c').read_text();report=(ROOT/'drivers/pmdomain/apple/pmp-report.c').read_text();bridge=(ROOT/'drivers/soc/apple/pmp-export.c').read_text()
 gpu=function(gate,'static int __init gate_t8122_gpu_pmp_link(')
 entry=function(report,'static struct device_node *apple_pmp_report_entry(')
 ready14=entry+function(report,'static int apple_pmp_report_wait_t8122_14(')
 native=function(report,'int apple_pmp_report_wait_supplier_ready(')
 send=function(bridge,'int apple_pmp_set_device_power(')
 apply=function(gate,'static int __init gate_pmp_apply(')
 assert apply.index('gate_t8122_gpu_pmp_link(')<apply.index('of_changeset_apply(')
 with tempfile.TemporaryDirectory(prefix='t8122-supplier-') as td:
  td=Path(td);code=prefix+gpu+ready14+native+send+CONTROLS;r=run(code,td/'current.c');print(r.stdout,end='');assert r.returncode==0
  if args.old_source:
   old=function(args.old_source.read_text(),'int apple_pmp_report_wait_supplier_ready(')
   r=run(prefix+gpu+ready14+old+send+CONTROLS,td/'old.c');assert r.returncode==-signal.SIGABRT;print('PASS original native25-only supplier path rejects current14 positive control')
  mutant=code.replace('ret = apple_pmp_report_wait_supplier_ready(pmp_device, 5000);','ret = pmp_device ? 0 : -ENODEV;');assert mutant!=code;r=run(mutant,td/'no-ready.c');assert r.returncode==-signal.SIGABRT;print('PASS bypassed-readiness mutant fails current14 control')
  old_report=subprocess.check_output(['git','-C',str(ROOT),'show',args.old_lookup_ref+':drivers/pmdomain/apple/pmp-report.c'],text=True)
  old14=function(old_report,'static int apple_pmp_report_wait_t8122_14(')
  r=run(prefix+gpu+old14+native+send+CONTROLS,td/'old-lookup.c');assert r.returncode==-signal.SIGABRT;print('PASS exact original lookup fails with actual OF implementation')
  for name,old,new in [('disabled-entry','for_each_available_child_of_node(report, child)','for_each_child_of_node(report, child)'),('reg-shape','of_property_count_u32_elems(child, "reg") != 1','false'),('duplicate-entry','if (entry) {','if (false) {')]:
   mutant=code.replace(old,new);assert mutant!=code;r=run(mutant,td/(name+'.c'));assert r.returncode==-signal.SIGABRT;print('PASS '+name+' mutant rejected')
  mutant=code.replace('entry = apple_pmp_report_entry(report, 7);','entry = of_get_child_by_name(report, "report@7");');assert mutant!=code;r=run(mutant,td/'child-by-name.c');assert r.returncode==-signal.SIGABRT;print('PASS of_get_child_by_name("report@7") mutant (2026.10.09.2) fails current14 control')
 print('Compiled production supplier functions use actual OF name, compatible, child-walk, availability and u32-property implementations; devices, locks, property storage and PTD are controlled boundaries; separate entry-ready controls cover startup/removal ownership.')
if __name__=='__main__':main()
