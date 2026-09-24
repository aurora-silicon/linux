#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host regression tests of the actual polled PMP callbacks, not hardware."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "drivers/soc/apple/pmp-v2-thermal.h").read_text()
smc = (root / "drivers/thermal/apple-smc-cpu-thermal.c").read_text()
assert "hrtimer" not in source
assert "pt_lease" not in source
assert "250000" not in source
assert "count, NULL, ops, &smc_params, 25, 25)" in smc
assert "static unsigned int max_pstate = 17, max_estate = 6;" in source
globals_ = source[source.index("#define PT_TRIP_MC"):source.index("static void pt_start(")]
callbacks = source[source.index("static unsigned int pt_policy_frequency("):
                   source.index("static bool pt_bind(")]
shutdown = source[source.index("static int pt_shutdown(void)"):
                  source.index("static int pt_status_show(")]
ready = smc[smc.index("bool apple_smc_thermal_aux_cooling_ready(void)"):
            smc.index("void apple_smc_thermal_aux_update(void)")]
ready = ready[:ready.rindex("#endif")]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
#define DEFINE_MUTEX(n) int n
#define guard(type) mock_guard
static void mock_guard(int *p) { (void)p; }
#define module_param(a,b,c)
#define MODULE_PARM_DESC(a,b)
#define max(a,b) ((a)>(b)?(a):(b))
struct pmp_v2 { int unused; };
struct pmp_sample_diagnostic { int unused; };
struct thermal_zone_device { int unused; };
enum thermal_device_mode { THERMAL_DEVICE_DISABLED, THERMAL_DEVICE_ENABLED };
struct cpufreq_frequency_table { unsigned frequency, driver_data; };
struct cpufreq_policy { struct cpufreq_frequency_table *freq_table; unsigned cpu; };
struct freq_qos_request { unsigned index; };
#define cpufreq_for_each_valid_entry(e,t) for ((e)=(t); (e)->frequency; (e)++)
static unsigned constraints[2], calls[2], injected_cluster=99;
static unsigned fail_count;
static int freq_qos_update_request(struct freq_qos_request *q, unsigned frequency)
{
    calls[q->index]++;
    if (injected_cluster==q->index && fail_count) {
        fail_count--; return -EIO;
    }
    constraints[q->index]=frequency; return 1;
}
static bool smc_healthy=true, cooling_ready=false;
static bool apple_smc_thermal_healthy(void) { return smc_healthy; }
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
struct thermal_cooling_device;
struct thermal_cooling_device_ops {
    int (*get_cur_state)(struct thermal_cooling_device *, unsigned long *);
};
struct thermal_cooling_device {
    struct thermal_cooling_device_ops *ops;
    int error;
};
static int mock_cooling_state(struct thermal_cooling_device *c, unsigned long *state)
{ *state=cooling_ready ? 1 : 0; return c->error; }
static struct thermal_cooling_device_ops cooling_ops={mock_cooling_state};
static struct thermal_cooling_device devices[2]={{&cooling_ops,0},{&cooling_ops,0}};
struct apple_smc_thermal_cpu { struct thermal_cooling_device *cdev; };
static struct apple_smc_thermal_cpu cpus[2]={{&devices[0]},{&devices[1]}};
static struct apple_smc_thermal_cpu *smc_cpus[2]={&cpus[0],&cpus[1]};
static int smc_lock;
static int sample_ret, sample_value=60000;
static u64 sample_stamp[2]={1,1};
static int pmp_v2_sample(struct pmp_v2 *p, u64 t[2], u64 age[2], int *v,
                         int cluster[2], struct pmp_sample_diagnostic *d)
{
    (void)p; (void)age; (void)cluster; (void)d;
    t[0]=sample_stamp[0]; t[1]=sample_stamp[1]; *v=sample_value;
    return sample_ret;
}
static void mutex_lock(int *p) { (void)p; }
static void mutex_unlock(int *p) { (void)p; }
static int pt_start_work;
static bool cancelled, stuck;
static u64 clock_ns;
static unsigned readbacks;
static void cancel_delayed_work_sync(int *w) { assert(w==&pt_start_work); cancelled=true; }
static u64 ktime_get_boottime_ns(void) { return clock_ns; }
static void usleep_range(unsigned low,unsigned high)
{ assert(low==1000 && high==2000); clock_ns+=1000000; }
static int apple_pmp_cpufreq_readback(unsigned cpu)
{ assert(cpu<2); readbacks++; return stuck ? 6 : 1; }
'''
test = r'''
static void low(void) { assert(constraints[0]==100 && constraints[1]==100); }
static void high(void) { assert(constraints[0]==600 && constraints[1]==1700); }
static int observe(void)
{
    int temperature=-1;
    sample_stamp[0]++; sample_stamp[1]++;
    int ret=pt_temp(NULL,&temperature);
    if (!ret) assert(temperature==sample_value);
    return ret;
}
static void recover(void)
{
    assert(!observe()); low(); assert(!pt_admitted);
    assert(!observe()); low(); assert(!pt_admitted);
    assert(!observe()); high(); assert(pt_admitted);
}
int main(void)
{
    assert(!apple_smc_thermal_aux_cooling_ready());
    cooling_ready=true;
    assert(apple_smc_thermal_aux_cooling_ready());
    devices[1].error=-EIO; assert(!apple_smc_thermal_aux_cooling_ready());
    devices[1].error=0;
    cpus[1].cdev=NULL; assert(!apple_smc_thermal_aux_cooling_ready());
    cpus[1].cdev=&devices[1];
    smc_cpus[0]=NULL; assert(!apple_smc_thermal_aux_cooling_ready());
    smc_cpus[0]=&cpus[0]; cooling_ready=false;
    struct cpufreq_frequency_table table[]={{100,1},{600,6},{1700,17},{0,0}};
    struct cpufreq_policy policy[2]={{table,1},{table,0}};
    pt_policy[0]=&policy[0]; pt_policy[1]=&policy[1];
    pt_qos[0].index=0; pt_qos[1].index=1;
    pt_attached=true; pt_configured=true; pt_zone_enabled=true;
    assert(!pt_guard(false)); low();
    recover();
    /* A failed read clamps both clusters; recovery is automatic. */
    sample_ret=-ENODATA;
    assert(observe()==-ENODATA); low(); assert(!pt_good && !pt_admitted);
    sample_ret=0; recover();
    /* Pending, contention and dropped observations never grant credit. */
    for (unsigned i=0; i<2; i++) {
        sample_ret=i ? -EBUSY : -EAGAIN;
        assert(observe()==sample_ret); low();
        sample_ret=0; recover();
    }
    /* Repeated or backwards delivery timestamps are not fresh readings. */
    int temp;
    assert(pt_temp(NULL,&temp)==-EAGAIN); low();
    sample_stamp[0]--;
    assert(pt_temp(NULL,&temp)==-EAGAIN); low();
    sample_stamp[0]++; recover();
    /* SMC backup loss also clamps, without requiring a reboot to recover. */
    smc_healthy=false; assert(observe()==-ENODATA); low();
    smc_healthy=true; recover();
    assert(!pt_mode(NULL,THERMAL_DEVICE_DISABLED)); low();
    for (unsigned i=0;i<5;i++) assert(!observe());
    low(); assert(!pt_admitted);
    assert(!pt_mode(NULL,THERMAL_DEVICE_ENABLED)); recover();
    /* At the trip, valid readings alone cannot bypass cooling startup. */
    assert(!pt_mode(NULL,THERMAL_DEVICE_ENABLED));
    sample_value=PT_TRIP_MC;
    for (unsigned i=0;i<5;i++) { assert(!observe()); low(); }
    assert(!pt_admitted);
    cooling_ready=true;
    assert(!observe()); high(); assert(pt_admitted);
    /* Here 'high' is only the independent guard's ceiling; thermal-core
     * cooling has its own lower QoS constraint, not emulated by this test. */
    sample_ret=-EIO; assert(observe()==-EIO); low();
    sample_ret=0; cooling_ready=false; sample_value=60000;
    assert(!observe()); low(); assert(!observe()); low();
    injected_cluster=1; fail_count=1;
    assert(observe()==-EIO); low();
    assert(pt_failed && !pt_admitted);
    assert(observe()==-ENODATA); low();
    /* If an actuator rejects even minimum, retain and report that error. */
    pt_failed=false; injected_cluster=0; fail_count=2;
    assert(pt_guard(true)==-EIO);
    assert(pt_failed && !pt_applied[0] && pt_applied[1]==1);
    assert(calls[0] && calls[1]);
    pt_failed=false; injected_cluster=99; pt_stopping=true;
    assert(!pt_guard(true)); low();
    assert(observe()==-ENODATA); low();
    /* Use debug counters so host -Werror catches unintended dead globals. */
    assert(pt_peak==PT_TRIP_MC && pt_temperature==60000);
    assert(pt_failures>=6 && pt_recoveries>=5 && pt_error==-EIO);
    (void)pt_waits;
    pt_stopping=false;
    assert(!pt_shutdown()); low();
    assert(cancelled && pt_stopping && !pt_admitted && readbacks==2);
    stuck=true; readbacks=0; clock_ns=0;
    assert(pt_shutdown()==-ETIMEDOUT); low();
    assert(clock_ns==100000000 && readbacks==200);
    pt_attached=false;
    assert(pt_shutdown()==-ENODEV);
    puts("PMP passive callbacks: PASS (startup, recovery, hot binding, disable, faults)");
}
'''
with tempfile.TemporaryDirectory(prefix="apple-pmp-passive-test-") as tmp:
    c = Path(tmp) / "test.c"
    binary = Path(tmp) / "test"
    c.write_text(prefix + ready + globals_ + callbacks + shutdown + test)
    subprocess.run([os.environ.get("CC", "cc"), "-O1", "-g", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-parameter", "-fsanitize=address,undefined",
                     str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
