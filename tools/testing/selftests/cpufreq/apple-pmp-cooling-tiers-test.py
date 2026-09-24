#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual Neo bindings and step_wise target selection, with host fixtures."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
pmp = (root / 'drivers/soc/apple/pmp-v2-thermal.h').read_text()
smc = (root / 'drivers/thermal/apple-smc-cpu-thermal.c').read_text()
gov = (root / 'drivers/thermal/gov_step_wise.c').read_text()

def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#define THERMAL_TRIP_PRIV_TO_INT(v) ((uintptr_t)(v))
#define THERMAL_INT_TO_TRIP_PRIV(v) ((void *)(uintptr_t)(v))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min(a,b) ((a)<(b)?(a):(b))
#define clamp(v,l,h) ((v)<(l)?(l):((v)>(h)?(h):(v)))
#define guard(x) mock_guard
static void mock_guard(int *p) { (void)p; }
#define WRITE_ONCE(x,v) ((x)=(v))
#define dev_dbg(...) ((void)0)
#define CPUFREQ_TABLE_UNSORTED 0
#define CPUFREQ_TABLE_SORTED_ASCENDING 1
#define CPUFREQ_TABLE_SORTED_DESCENDING 2
#define THERMAL_TRIP_PASSIVE 1
#define THERMAL_NO_TARGET ULONG_MAX
#define cpufreq_for_each_valid_entry(e,t) for((e)=(t);(e)->frequency;(e)++)
struct cpufreq_frequency_table { unsigned frequency; };
struct cpufreq_policy { struct cpufreq_frequency_table *freq_table; int freq_table_sorted; };
struct thermal_cooling_device;
struct thermal_cooling_device_ops {
    int (*get_cur_state)(struct thermal_cooling_device *, unsigned long *);
};
struct thermal_cooling_device {
    struct thermal_cooling_device_ops *ops;
    unsigned long max_state, state;
};
static int get_state(struct thermal_cooling_device *c, unsigned long *s)
{ *s=c->state; return 0; }
static struct thermal_cooling_device_ops cooling_ops={get_state};
struct apple_smc_thermal_cpu {
    struct thermal_cooling_device *cdev;
    struct cpufreq_policy *policy;
};
static struct apple_smc_thermal_cpu *smc_cpus[2];
static int smc_lock;
struct thermal_trip { int type, temperature, hysteresis; void *priv; };
struct thermal_zone_device { struct thermal_trip *trips; };
static void thermal_zone_set_trip_temp(struct thermal_zone_device *z, struct thermal_trip *t, int v)
{ (void)z; t->temperature=v; }
struct smc_aux_trip_update { struct thermal_zone_device *zone; int temperature,hysteresis; };
struct cooling_spec { unsigned long lower,upper; };
enum thermal_trend { THERMAL_TREND_RAISING, THERMAL_TREND_DROPPING, THERMAL_TREND_STABLE };
struct thermal_instance {
    struct thermal_cooling_device *cdev;
    unsigned long lower,upper,target;
    bool initialized;
};
'''
code += '\n'.join(line for line in pmp.splitlines() if line.startswith('#define PT_')) + '\n'
code += function(smc, 'bool apple_smc_thermal_aux_matches(')
code += function(smc, 'int apple_smc_thermal_aux_cooling_floor(')
code += function(smc, 'static int smc_aux_set_trip(')
code += function(pmp, 'static bool pt_bind(')
start = pmp.index('static const struct thermal_trip pt_trips[]')
code += pmp[start:pmp.index('\n};', start) + 3] + '\n'
code += function(gov, 'static unsigned long get_target_state(')
code += r'''
int main(void)
{
    struct cpufreq_frequency_table p_table[] = {
        {744000},{1260000},{1620000},{1944000},{2196000},{2472000},
        {2688000},{2916000},{3144000},{3336000},{3504000},{3648000},
        {3756000},{3864000},{3924000},{3972000},{4044000},{0}};
    struct cpufreq_frequency_table e_table[] = {
        {816000},{1140000},{1464000},{1788000},{2112000},{2424000},{0}};
    struct cpufreq_policy policies[] = {{p_table,1},{e_table,1}};
    struct thermal_cooling_device devices[] = {{&cooling_ops,16,0},{&cooling_ops,5,0}};
    struct apple_smc_thermal_cpu cpus[] = {{devices,policies},{devices+1,policies+1}};
    smc_cpus[0]=cpus; smc_cpus[1]=cpus+1;
    assert(ARRAY_SIZE(pt_trips)==2);
    struct thermal_trip trips[]={pt_trips[0],pt_trips[1],{.priv=THERMAL_INT_TO_TRIP_PRIV(2)}};
    struct thermal_zone_device zone={trips};
    assert(trips[0].temperature==95000 && trips[0].hysteresis==10000);
    assert(trips[1].temperature==100000 && trips[1].hysteresis==10000);
    unsigned long floor;
    assert(!apple_smc_thermal_aux_cooling_floor(devices,2472000,&floor) && floor==11);
    assert(!apple_smc_thermal_aux_cooling_floor(devices+1,2472000,&floor) && floor==1);
    assert(!apple_smc_thermal_aux_cooling_floor(devices,1,&floor) && floor==16);
    /* Mapping is frequency-based and works with descending tables too. */
    for(int i=0;i<8;i++) {
        struct cpufreq_frequency_table tmp=p_table[i];
        p_table[i]=p_table[16-i]; p_table[16-i]=tmp;
    }
    policies[0].freq_table_sorted=CPUFREQ_TABLE_SORTED_DESCENDING;
    assert(!apple_smc_thermal_aux_cooling_floor(devices,2472000,&floor) && floor==11);
    for(unsigned c=0;c<2;c++) {
        for(unsigned t=0;t<2;t++) {
            struct cooling_spec spec;
            assert(pt_bind(&zone,trips+t,devices+c,&spec));
            assert(spec.upper==devices[c].max_state);
            unsigned long expected=t ? devices[c].max_state : (c ? 1 : 11);
            assert(spec.lower==expected);
            struct thermal_instance instance={devices+c,spec.lower,spec.upper,0,false};
            devices[c].state=0;
            assert(get_target_state(&instance,THERMAL_TREND_RAISING,true)==expected);
            instance.initialized=true;
            assert(get_target_state(&instance,THERMAL_TREND_RAISING,true)==expected);
            devices[c].state=expected;
            assert(get_target_state(&instance,THERMAL_TREND_DROPPING,true)>=expected);
            assert(get_target_state(&instance,THERMAL_TREND_DROPPING,false)==THERMAL_NO_TARGET);
        }
    }
    /* Policy initialization may retune the primary, but not collapse tiers. */
    struct smc_aux_trip_update update={&zone,81000,9000};
    assert(!smc_aux_set_trip(trips,&update) && !smc_aux_set_trip(trips+1,&update));
    assert(trips[0].temperature==81000 && trips[0].hysteresis==9000);
    assert(trips[1].temperature==100000 && trips[1].hysteresis==10000);
    struct cooling_spec spec;
    assert(!pt_bind(&zone,trips+2,devices,&spec));
    policies[0].freq_table_sorted=CPUFREQ_TABLE_UNSORTED;
    assert(!pt_bind(&zone,trips,devices,&spec));
    policies[0].freq_table_sorted=CPUFREQ_TABLE_SORTED_DESCENDING;
    devices[0].max_state=15;
    assert(!pt_bind(&zone,trips,devices,&spec));
    smc_cpus[0]=NULL;
    assert(!pt_bind(&zone,trips,devices,&spec));
    puts("PASS: actual cooling-floor mapping, both trip bindings, step_wise jumps, tier preservation and invalid bindings");
}
'''
with tempfile.TemporaryDirectory(prefix='neo-cooling-tiers-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run(['cc', '-x', 'c', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter',
                    '-fsanitize=address,undefined',
                    '-o', str(binary), '-'], input=code, text=True, check=True)
    subprocess.run([str(binary)], check=True)
