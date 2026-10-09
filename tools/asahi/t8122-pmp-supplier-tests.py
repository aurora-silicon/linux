#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Controls for current14 T8122 GPU/PMP links and supplier readiness."""
import argparse
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
def function(s,marker):
    a=s.index(marker);b=s.index('{',a);n=1;e=b+1
    while n:n+=(s[e]=='{')-(s[e]=='}');e+=1
    return s[a:e]
PREFIX=r'''
#include <assert.h>
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
struct device_node{const char *compatible;bool available;unsigned phandle;int refs;struct device_node *target,*report,*child;};
struct device{struct device_node *of_node;int refs;};
struct platform_device{struct device dev;};
struct apple_pmp_report_offsets{unsigned status;};
struct apple_pmp_report{void *base;struct apple_pmp_report_offsets *offsets;};
struct of_changeset{int added;};struct gate_soc{int chip;};
static const struct gate_soc gate_t8122={8122},gate_t6030={6030};static const struct gate_soc *gate_soc;
static struct device_node gpu,pmp,other,report14,entry7,report25;
static struct platform_device report_device;static struct device supplier;
static struct apple_pmp_report rep;static struct apple_pmp_report_offsets offsets;
static struct device *pmp_device=&supplier;static const void *pmp_data=(void*)1;
static bool machine8122,missing_gpu,missing_report,missing_native_device,lockable,bound;
static int ready_result,add_result,power_result,ready_calls,add_calls,poll_calls,power_calls,lock_depth;
static u64 ptd_status;
static void of_node_put(struct device_node*n){if(n){assert(n->refs>0);n->refs--;}}
static struct device_node *ref(struct device_node*n){if(n)n->refs++;return n;}
static bool of_machine_is_compatible(const char*s){assert(!strcmp(s,"apple,t8122"));return machine8122;}
static bool of_device_is_compatible(struct device_node*n,const char*s){return n && !strcmp(n->compatible,s);}
static struct device_node *of_find_compatible_node(void*a,void*b,const char*s){assert(!a&&!b);if(!strcmp(s,"apple,agx-t8122"))return missing_gpu?NULL:ref(&gpu);assert(!strcmp(s,"apple,t8122-pmp-v2-report"));return missing_report?NULL:ref(&report14);}
static bool of_device_is_available(struct device_node*n){return n->available;}
static struct device_node *of_parse_phandle(struct device_node*n,const char*s,int i){assert(!i);if(!strcmp(s,"apple,pmp"))return ref(n->target);assert(!strcmp(s,"apple,pmp-report"));return ref(n->report);}
static struct device_node *of_get_child_by_name(struct device_node*n,const char*s){assert(!strcmp(s,"report@7"));return ref(n->child);}
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
 entry7=(struct device_node){.compatible="apple,t6000-pmp-v2-report-entry",.phandle=4};
 report14=(struct device_node){.compatible="apple,t8122-pmp-v2-report",.target=&pmp,.child=&entry7};
 report25=(struct device_node){.compatible="apple,j613-25g83-pmp-report"};
 supplier=(struct device){.of_node=&pmp};report_device=(struct platform_device){0};
 offsets=(struct apple_pmp_report_offsets){.status=0};rep=(struct apple_pmp_report){.base=&ptd_status,.offsets=&offsets};
 gate_soc=&gate_t8122;machine8122=true;missing_gpu=missing_report=missing_native_device=false;lockable=bound=true;
 ready_result=add_result=power_result=ready_calls=add_calls=poll_calls=power_calls=0;ptd_status=1;assert(!lock_depth);
}
static void balanced(void){assert(!gpu.refs&&!pmp.refs&&!other.refs&&!report14.refs&&!entry7.refs&&!report25.refs&&!report_device.dev.refs&&!lock_depth);}
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
 p.write_text(code);subprocess.run(['clang','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
 return subprocess.run([str(p.with_suffix(''))],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--old-source',type=Path);args=parser.parse_args()
 gate=(ROOT/'drivers/soc/apple/t6030-display-gate.c').read_text();report=(ROOT/'drivers/pmdomain/apple/pmp-report.c').read_text();bridge=(ROOT/'drivers/soc/apple/pmp-export.c').read_text()
 gpu=function(gate,'static int __init gate_t8122_gpu_pmp_link(')
 ready14=function(report,'static int apple_pmp_report_wait_t8122_14(')
 native=function(report,'int apple_pmp_report_wait_supplier_ready(')
 send=function(bridge,'int apple_pmp_set_device_power(')
 apply=function(gate,'static int __init gate_pmp_apply(')
 assert apply.index('gate_t8122_gpu_pmp_link(')<apply.index('of_changeset_apply(')
 with tempfile.TemporaryDirectory(prefix='t8122-supplier-') as td:
  td=Path(td);code=PREFIX+gpu+ready14+native+send+CONTROLS;r=run(code,td/'current.c');print(r.stdout,end='');assert r.returncode==0
  if args.old_source:
   old=function(args.old_source.read_text(),'int apple_pmp_report_wait_supplier_ready(')
   r=run(PREFIX+gpu+ready14+old+send+CONTROLS,td/'old.c');assert r.returncode!=0;print('PASS original native25-only supplier path rejects current14 positive control')
  mutant=code.replace('ret = apple_pmp_report_wait_supplier_ready(pmp_device, 5000);','ret = pmp_device ? 0 : -ENODEV;');assert mutant!=code;r=run(mutant,td/'no-ready.c');assert r.returncode!=0;print('PASS bypassed-readiness mutant fails current14 control')
 print('Compiled production functions use mocked OF, devices, locks and PTD; separate entry-ready controls cover startup/removal ownership.')
if __name__=='__main__':main()
