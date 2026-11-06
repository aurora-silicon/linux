# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""Actual cpuidle driver/registration/selector and tick suspension source replay."""
import re
def function(text, name):
    match=re.search(r'^((?:static\s+)?(?:inline\s+)?(?:__cpuidle\s+|noinstr\s+)?(?:int|void)\s+'+re.escape(name)+r'\([^;{}]*?\))\s*\{',text,re.M)
    if not match: raise ValueError(name)
    start=match.start(); pos=match.end(); depth=1
    while depth:
        if text[pos]=='{': depth+=1
        elif text[pos]=='}': depth-=1
        pos+=1
    return text[start:pos]+'\n'

MOCKS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
typedef uint64_t u64; typedef int64_t s64; typedef int64_t ktime_t;
#define CONFIG_SUSPEND 1
#define CPUIDLE_NAME_LEN 16
#define CPUIDLE_DESC_LEN 32
#define CPUIDLE_STATE_MAX 10
#define BIT(n) (1U<<(n))
#define NSEC_PER_USEC 1000
#define __cpuidle
#define noinstr
#define THIS_MODULE NULL
#define SYSTEM_SUSPEND 1
#define SYSTEM_RUNNING 0
#define TPS(x) x
#define instrumentation_begin() ((void)0)
#define instrumentation_end() ((void)0)
#define raw_spin_lock(x) ((void)0)
#define raw_spin_unlock(x) ((void)0)
#define lock_map_acquire_try(x) ((void)0)
#define lock_map_release(x) ((void)0)
#define trace_suspend_resume(...) ((void)0)
#define WARN_ON(x) ((x)?(warnings++,1):0)
#define WARN_ON_ONCE(x) WARN_ON(x)
#define pr_err(...) ((void)0)
#define for_each_cpu(cpu,mask) for(cpu=0;cpu<(mask)->count;cpu++)
#define per_cpu(v,cpu) v[cpu]
struct device_node;
struct platform_device {struct {struct device_node *of_node;} dev;};
static struct platform_device pdev;
struct module; struct cpumask {int count;};
struct cpuidle_device; struct cpuidle_driver;
struct cpuidle_device {int cpu; struct cpuidle_state_usage states_usage[10];};
static struct cpumask possible={6}; static const struct cpumask *cpu_possible_mask=&possible;
static struct cpuidle_device cpuidle_dev[6];
static int current, irq[6], warnings, wfi, deep, pm, pm_fail, reg_fail=-1, drv_fail;
static int registered[6], unregistered, rcu[6], frozen, stopped[6], online=6;
static int tick_freeze_depth, tick_freeze_lock, tick_freeze_map, system_state;
static int tk_suspend, tk_resume, local_suspend, local_resume, sched_suspend, sched_resume;
static int fanout, faulty_irq, checks;
static uint64_t clock_ns;
static void check(bool b) {checks++; assert(b);}
static int num_online_cpus(void){return online;}
static void sched_clock_suspend(void){sched_suspend++;}
static void sched_clock_resume(void){sched_resume++;}
static void timekeeping_suspend(void){check(tick_freeze_depth==online);tk_suspend++;frozen=1;}
static void timekeeping_resume(void){check(frozen);tk_resume++;frozen=0;}
static void tick_suspend_local(void){local_suspend++;}
static void tick_resume_local(void){local_resume++;}
static void touch_softlockup_watchdog(void){}
static void ct_cpuidle_enter(void){check(irq[current]);rcu[current]++;}
static void ct_cpuidle_exit(void){check(irq[current]);rcu[current]--;}
static bool irqs_disabled(void){return irq[current];}
static void raw_local_irq_disable(void){irq[current]=1;}
static void local_irq_enable(void){check(!frozen);irq[current]=0;}
static void stop_critical_timings(void){stopped[current]++;}
static void start_critical_timings(void){stopped[current]--;}
static ktime_t local_clock_noinstr(void){return clock_ns+=1000;}
static ktime_t ns_to_ktime(ktime_t t){return t;}
static ktime_t ktime_us_delta(ktime_t a,ktime_t b){return (a-b)/1000;}
static u64 div_u64(u64 a,u64 b){return a/b;}
static u64 mul_u32_u32(unsigned a,unsigned b){return (u64)a*b;}
static const char *machine;
static bool of_machine_compatible_match(const char *const *c){for(;machine&&*c;c++)if(!strcmp(*c,machine))return true;return false;}
static int cpu_pm_enter(void){pm++;return pm_fail;}
static void cpu_pm_exit(void){pm--;}
static void apple_cpu_deep_wfi(void){deep++;}
static void cpu_do_idle(void);
static int cpuidle_register_driver(struct cpuidle_driver *drv);
static int cpuidle_register_device(struct cpuidle_device *dev){if(dev->cpu==reg_fail)return -5; registered[dev->cpu]=1;return 0;}
static void cpuidle_unregister(struct cpuidle_driver *drv){(void)drv;memset(registered,0,sizeof(registered));unregistered++;}
'''
MAIN = r'''
static int cpuidle_register_driver(struct cpuidle_driver *drv){if(drv_fail)return -5;__cpuidle_driver_init(drv);return 0;}
static void cpu_do_idle(void){
    check(irq[current]);wfi++;
    if(fanout && current<5){int previous=current;current++;irq[current]=1;
        cpuidle_enter_s2idle(&apple_idle_driver,&cpuidle_dev[current],100000);current=previous;}
    if(faulty_irq)irq[current]=0;
}
static struct cpuidle_driver original;
static void reset(const char *m){
    apple_idle_driver=original;machine=m; current=0;
    memset(cpuidle_dev,0,sizeof(cpuidle_dev)); memset(registered,0,sizeof(registered));
    for(int i=0;i<6;i++){irq[i]=1;rcu[i]=stopped[i]=0;}
    wfi=deep=pm=pm_fail=warnings=unregistered=0;reg_fail=-1;drv_fail=0;
    tick_freeze_depth=frozen=tk_suspend=tk_resume=local_suspend=local_resume=sched_suspend=sched_resume=0;
    fanout=faulty_irq=0;
}
int main(void){
    original=apple_idle_driver;
    reset("apple,t8140");check(!apple_cpuidle_probe(&pdev));
    check(apple_idle_driver.cpumask==cpu_possible_mask);check(!apple_idle_driver.bctimer);
    for(int i=0;i<6;i++)check(registered[i]&&cpuidle_dev[i].cpu==i);
    int expected=EXPECT_FIXED?1:0;
    check(apple_idle_driver.state_count==(EXPECT_FIXED?2:1));
    check(find_deepest_state(&apple_idle_driver,&cpuidle_dev[0],100000,0,true)==expected);
    check(cpuidle_enter_s2idle(&apple_idle_driver,&cpuidle_dev[0],100000)==expected);
    check(tick_freeze_depth==0&&!frozen);check(deep==0&&pm==0);
    check(local_suspend==expected&&local_resume==expected);
    if(EXPECT_FIXED){
        check(apple_idle_driver.states[1].enter==apple_enter_wfi);
        check(apple_idle_driver.states[1].enter_s2idle==apple_enter_wfi);
        check(apple_idle_driver.states[1].flags==0);
        check(apple_idle_driver.states[1].exit_latency_ns==1000);
        check(apple_idle_driver.states[1].target_residency_ns==1000);
        check(apple_idle_driver.states[1].power_usage==apple_idle_driver.states[0].power_usage);
        reset("apple,t8140");apple_cpuidle_probe(&pdev);fanout=1;
        check(cpuidle_enter_s2idle(&apple_idle_driver,&cpuidle_dev[0],100000)==1);
        check(wfi==6&&deep==0&&pm==0);check(tk_suspend==1&&tk_resume==1);
        check(sched_suspend==1&&sched_resume==1&&local_suspend==5&&local_resume==5);
        check(tick_freeze_depth==0&&!frozen);
        for(int i=0;i<6;i++)check(cpuidle_dev[i].states_usage[1].s2idle_usage==1&&rcu[i]==0&&stopped[i]==0);
        reset("apple,t8140");apple_cpuidle_probe(&pdev);fanout=1;cpuidle_dev[5].states_usage[1].disable=1;
        cpuidle_enter_s2idle(&apple_idle_driver,&cpuidle_dev[0],100000);
        check(wfi==5&&tk_suspend==0&&tk_resume==0&&tick_freeze_depth==0);
        reset("apple,t8140");apple_cpuidle_probe(&pdev);cpuidle_dev[0].states_usage[1].disable=1;
        check(cpuidle_enter_s2idle(&apple_idle_driver,&cpuidle_dev[0],100000)==0&&wfi==0);
        cpuidle_dev[0].states_usage[1].disable=0;apple_idle_driver.states[1].enter_s2idle=NULL;
        check(cpuidle_enter_s2idle(&apple_idle_driver,&cpuidle_dev[0],100000)==0&&wfi==0);
        reset("apple,t8140");apple_cpuidle_probe(&pdev);
        check(cpuidle_enter_s2idle(&apple_idle_driver,&cpuidle_dev[0],999)==0);
        check(cpuidle_enter_s2idle(&apple_idle_driver,&cpuidle_dev[0],1000)==1);
        reset("apple,t8140");apple_cpuidle_probe(&pdev);apple_idle_driver.states[1].exit_latency_ns=0;
        check(find_deepest_state(&apple_idle_driver,&cpuidle_dev[0],100000,0,true)==0);
        reset("apple,t8140");apple_cpuidle_probe(&pdev);apple_idle_driver.states[1].flags=CPUIDLE_FLAG_TIMER_STOP;
        check(find_deepest_state(&apple_idle_driver,&cpuidle_dev[0],100000,CPUIDLE_FLAG_TIMER_STOP,false)==0);
        reset("apple,t8140");apple_cpuidle_probe(&pdev);faulty_irq=1;
        check(cpuidle_enter_s2idle(&apple_idle_driver,&cpuidle_dev[0],100000)==1);
        check(warnings==1&&tick_freeze_depth==0&&rcu[0]==0&&stopped[0]==0);
    }
    reset("apple,t8140");apple_cpuidle_probe(&pdev);
    for(int i=0;i<apple_idle_driver.state_count;i++)check(apple_idle_driver.states[i].enter(&cpuidle_dev[0],&apple_idle_driver,i)==i);
    check(deep==0&&pm==0);
    /* M1 to M3 keep CPU PD. */
    reset("apple,t8103");apple_cpuidle_probe(&pdev);
    check(apple_idle_driver.state_count==2&&apple_idle_driver.states[1].enter==apple_enter_idle);
    check(apple_idle_driver.states[1].flags==CPUIDLE_FLAG_RCU_IDLE);
    check(find_deepest_state(&apple_idle_driver,&cpuidle_dev[0],100000,0,true)==1);
    check(apple_enter_idle(&cpuidle_dev[0],&apple_idle_driver,1)==1&&deep==1&&pm==0);
    pm_fail=1;check(apple_enter_idle(&cpuidle_dev[0],&apple_idle_driver,1)==-1&&deep==1);pm=0;
    check(cpuidle_enter_s2idle(&apple_idle_driver,&cpuidle_dev[0],100000)==1);
    check(tick_freeze_depth==0&&cpuidle_dev[0].states_usage[1].s2idle_usage==1&&deep==1);pm=0;
    /* Anything but M1 to M3, T6041 and T8142 included: the same WFI-only states as T8140. */
    static const char *const wfi_only[]={"apple,t6041","apple,t8142",NULL};
    for(const char *const *m=wfi_only;*m;m++){
        reset(*m);check(!apple_cpuidle_probe(&pdev));
        check(apple_idle_driver.state_count==2&&apple_idle_driver.states[1].enter==apple_enter_wfi);
        check(apple_idle_driver.states[1].enter_s2idle==apple_enter_wfi&&apple_idle_driver.states[1].flags==0);
        check(find_deepest_state(&apple_idle_driver,&cpuidle_dev[0],100000,0,true)==1);
        check(cpuidle_enter_s2idle(&apple_idle_driver,&cpuidle_dev[0],100000)==1);
        check(local_suspend==1&&local_resume==1&&deep==0&&pm==0&&tick_freeze_depth==0&&!frozen);
    }
    reset("apple,t8140");reg_fail=3;check(apple_cpuidle_probe(&pdev)==-5&&unregistered==1);
    for(int i=0;i<6;i++)check(!registered[i]);
    reset("apple,t8140");drv_fail=1;check(apple_cpuidle_probe(&pdev)==-5&&unregistered==0);
    printf("%s: %d assertions passed; actual selector/wrapper/driver/tick bodies, mocked hardware/locks/timekeeping\n",EXPECT_FIXED?"patched":"parent",checks);
    return 0;
}
'''


def s2idle(root):
    apple = (root / 'drivers/cpuidle/cpuidle-apple.c').read_text()
    core = (root / 'drivers/cpuidle/cpuidle.c').read_text()
    driver = (root / 'drivers/cpuidle/driver.c').read_text()
    tick = (root / 'kernel/time/tick-common.c').read_text()
    header = (root / 'include/linux/cpuidle.h').read_text()
    structs = header[header.index('struct cpuidle_state_usage {'):header.index('struct cpuidle_device_kobj;')]
    drvstruct = header[header.index('struct cpuidle_driver {'):header.index('#ifdef CONFIG_CPU_IDLE\n')]
    text = MOCKS.replace('struct cpuidle_device {int cpu;', structs + drvstruct + '\nstruct cpuidle_device {int cpu;')
    text += '\n' + function(driver, '__cpuidle_driver_init') + function(core, 'cpuidle_register')
    text += apple[apple.index('enum idle_state {'):apple.index('asm(')]
    text += function(apple, 'apple_enter_wfi') + function(apple, 'apple_enter_idle')
    text += apple[apple.index('static struct cpuidle_driver apple_idle_driver ='):apple.index('static int apple_cpuidle_probe')]
    text += function(apple, 'apple_cpuidle_probe')
    text += function(tick, 'tick_freeze') + function(tick, 'tick_unfreeze')
    text += function(core, 'find_deepest_state') + function(core, 'enter_s2idle_proper') + function(core, 'cpuidle_enter_s2idle')
    return text + '#define EXPECT_FIXED 1\n' + MAIN
