#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual polled-policy attach: dependencies, cleanup and publication."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[4] /
          'drivers/soc/apple/pmp-v2-thermal.h').read_text()
start = source.index('static int pt_attach(void)')
body = source[start:source.index('\n}', start) + 2]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#define EPROBE_DEFER 517
#define CPUHP_AP_ONLINE_DYN 1
#define FREQ_QOS_MAX 1
#define guard(x) cpu_guard
#define for_each_online_cpu(cpu) for(cpu=0;cpu<6;cpu++)
#define WRITE_ONCE(v,x) publish(&(v),x)
struct cpufreq_policy { unsigned cpu; int related_cpus,cpus; };
static struct cpufreq_policy fixtures[2]={{1,4,4},{0,2,2}};
static struct cpufreq_policy *pt_policy[2];
static int refs[2],pt_lock,depth,fault,callbacks,added;
static unsigned max_pstate=6,max_estate=1,pt_applied[2];
static int pt_qos[2];
static bool pt_attached;
static void complete(void)
{
    callbacks++;
    if(pt_attached) {
        assert(added==2 && pt_policy[0] && pt_policy[1]);
        for(int i=0;i<2;i++) assert(refs[i]>0 && pt_applied[i]==1);
    } else assert(!pt_policy[0] && !pt_policy[1]);
}
static void cpu_guard(void) { assert(!depth); }
static int num_online_cpus(void) { return 6; }
static struct cpufreq_policy *cpufreq_cpu_get(unsigned cpu)
{
    complete();
    if(fault==1 && cpu==3) return 0;
    int i=(cpu==0 || cpu==5); refs[i]++; return &fixtures[i];
}
static void cpufreq_cpu_put(struct cpufreq_policy *p)
{ int i=(p==fixtures+1); assert(refs[i]>0); refs[i]--; complete(); }
static int cpumask_weight(int m) { return m; }
static bool cpumask_equal(int a,int b) { return a==b; }
static bool apple_smc_thermal_healthy(void) { complete(); return fault!=2; }
static unsigned pt_policy_frequency(struct cpufreq_policy *p,unsigned state)
{ (void)p; return state*1000; }
static int apple_pmp_cpufreq_readback(unsigned cpu) { (void)cpu; complete(); return 1; }
static int freq_qos_add_request(void *a,int *q,int type,unsigned value);
'''
# The policy mock needs only the constraints address, not a kernel QoS object.
code = code.replace('int related_cpus,cpus;', 'int related_cpus,cpus,constraints;')
code = code.replace('{{1,4,4},{0,2,2}}', '{{1,4,4,0},{0,2,2,0}}')
code += r'''
static int freq_qos_add_request(void *a,int *q,int type,unsigned value)
{
    (void)a; assert(type==1 && value==1000); complete();
    if((fault==3 && added==0) || (fault==4 && added==1)) return -EIO;
    *q=1; added++; return 0;
}
static void freq_qos_remove_request(int *q) { assert(*q); *q=0; added--; complete(); }
static int pt_offline(unsigned cpu) { (void)cpu; return 0; }
static int cpuhp_setup_state_nocalls_cpuslocked(int state,const char *name,
                                              void *up,int (*down)(unsigned))
{ (void)name;(void)up; assert(state==1 && down==pt_offline); complete(); return fault==5?-EIO:10; }
static void mutex_lock(int *lock) { assert(lock==&pt_lock && !depth); depth++; }
static void mutex_unlock(int *lock) { assert(lock==&pt_lock && depth==1); depth--; complete(); }
static void publish(bool *flag,bool value) { assert(depth==1); *flag=value; complete(); }
'''
# Explicit stand-ins above are not a live concurrency/scheduler test.
code += body + r'''
int main(void)
{
    for(fault=1;fault<=5;fault++) {
        assert(pt_attach()<0);
        assert(!pt_attached && !refs[0] && !refs[1] && !added && !depth);
    }
    fault=0; assert(pt_attach()==0 && pt_attached);
    assert(refs[0]==1 && refs[1]==1 && added==2 && callbacks>50);
}
'''
with tempfile.TemporaryDirectory(prefix='neo-thermal-attach-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run(['cc', '-x', 'c', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-o', str(binary), '-'], input=code, text=True, check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: dependency failures and complete attach publication preserve policy references')
