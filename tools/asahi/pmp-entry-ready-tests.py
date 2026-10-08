#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host controls for the production entry-based PMP startup consumer."""
from pathlib import Path
import resource
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'drivers/pmdomain/apple/pmp-report.c'

PLATFORM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define EINVAL 22
#define EIO 5
#define ENODEV 19
#define ETIMEDOUT 110
#define EPROBE_DEFER 517
#define BIT_ULL(id) (1ULL << (id))
#define PMP_REPORT_READY 0x1
struct device_node { const char *compatible; struct device_node *parent; u32 id; bool bad_reg; int refs; };
struct device { int refs; };
struct platform_device { struct device dev; };
struct completion { int count; };
struct atomic { int value; };
struct apple_pmp_report_offsets { bool starts_pmp; unsigned status, actual; };
struct apple_pmp_report {
 bool stopping; unsigned waiters; int start_result; u64 ack;
 struct atomic start_requested; struct completion started, waiters_done;
 unsigned char *base; struct apple_pmp_report_offsets *offsets;
};
static struct device_node parent, entry;
static struct platform_device device;
static struct apple_pmp_report_offsets offsets;
static struct apple_pmp_report report;
static u64 mmio[3];
static int apple_pmp_report_mutex, lock_depth;
static int lookup_calls, request_calls, wait_calls, atomic_calls, reads;
static bool missing_device, missing_report, timed_out, stop_during_wait;
struct mutex_guard { int active; };
static struct mutex_guard enter(int *mutex) { assert(lock_depth == 0); lock_depth++; return (struct mutex_guard){1}; }
static void leave(struct mutex_guard *g) { assert(lock_depth == 1); lock_depth--; }
#define scoped_guard(kind,mutex) for(bool once=true;once;) for(struct mutex_guard guard __attribute__((cleanup(leave)))=enter(mutex);once;once=false)
static struct device_node *of_get_parent(struct device_node *node) { if(node->parent)node->parent->refs++;return node->parent; }
static void of_node_put(struct device_node *node) { if(node){assert(node->refs>0);node->refs--;} }
static int of_property_read_u32(struct device_node *node,const char *name,u32 *id) { assert(!strcmp(name,"reg"));*id=node->id;return node->bad_reg?-EINVAL:0; }
static bool of_device_is_compatible(struct device_node *node,const char *compatible) { return !strcmp(node->compatible,compatible); }
static struct platform_device *of_find_device_by_node(struct device_node *node) { lookup_calls++;if(missing_device)return NULL;device.dev.refs++;return &device; }
static struct apple_pmp_report *platform_get_drvdata(struct platform_device *pdev) { assert(lock_depth==1);return missing_report?NULL:&report; }
static void put_device(struct device *dev) { assert(dev->refs==1);dev->refs--; }
static void atomic_set(struct atomic *atomic,int value) { atomic_calls++;atomic->value=value; }
static void smp_mb__after_atomic(void) {}
static void apple_pmp_t6030_request_start(struct apple_pmp_report *rep) { request_calls++;assert(rep->start_requested.value==1);assert(rep->waiters==1); }
static unsigned long wait_for_completion_timeout(struct completion *completion,unsigned long timeout) { wait_calls++;assert(timeout==1234);if(stop_during_wait)report.stopping=true;return timed_out?0:1; }
static u64 readq(const void *address) { u64 result;reads++;memcpy(&result,address,sizeof(result));return result; }
static void complete(struct completion *completion) { completion->count++; }
static void reset(const char *compatible) {
 parent=(struct device_node){.compatible=compatible};entry=(struct device_node){.parent=&parent,.id=7};
 device=(struct platform_device){};offsets=(struct apple_pmp_report_offsets){true,8,16};
 mmio[0]=0;mmio[1]=PMP_REPORT_READY;mmio[2]=BIT_ULL(7);
 report=(struct apple_pmp_report){.ack=BIT_ULL(7),.base=(unsigned char*)mmio,.offsets=&offsets};
 lookup_calls=request_calls=wait_calls=atomic_calls=reads=0;
 missing_device=missing_report=timed_out=stop_during_wait=false;
 assert(lock_depth==0);
}
static void balanced(void) { assert(parent.refs==0 && device.dev.refs==0 && report.waiters==0 && lock_depth==0); }
'''

CONTROLS = r'''
int main(void) {
 const char *known[]={"apple,t6030-pmp-v2-report","apple,t8122-pmp-v2-report"};
 for(unsigned i=0;i<2;i++) {
  reset(known[i]);assert(apple_pmp_report_wait_ready(&entry,1234)==0);
  assert(atomic_calls==1 && report.start_requested.value==1 && request_calls==1 && wait_calls==1 && reads==2);balanced();
 }
 const char *passive[]={"apple,t6000-pmp-v2-report","apple,t6020-pmp-v2-report","apple,t8112-pmp-v2-report","apple,t8132-pmp-v2-report","apple,j613-25g83-pmp-report","apple,unknown-pmp-report"};
 for(unsigned i=0;i<sizeof(passive)/sizeof(*passive);i++) {
  reset(passive[i]);assert(apple_pmp_report_wait_ready(&entry,1234)==-EINVAL);
  assert(!lookup_calls && !atomic_calls && !request_calls && !wait_calls);balanced();
 }
 reset(known[1]);entry.parent=NULL;assert(apple_pmp_report_wait_ready(&entry,1234)==-EINVAL);balanced();
 reset(known[1]);entry.bad_reg=true;assert(apple_pmp_report_wait_ready(&entry,1234)==-EINVAL);balanced();
 reset(known[1]);entry.id=64;assert(apple_pmp_report_wait_ready(&entry,1234)==-EINVAL);balanced();
 reset(known[1]);missing_device=true;assert(apple_pmp_report_wait_ready(&entry,1234)==-EPROBE_DEFER);assert(!request_calls);balanced();
 reset(known[1]);missing_report=true;assert(apple_pmp_report_wait_ready(&entry,1234)==-EPROBE_DEFER);assert(!request_calls);balanced();
 reset(known[1]);report.stopping=true;assert(apple_pmp_report_wait_ready(&entry,1234)==-ENODEV);assert(!request_calls);balanced();
 reset(known[1]);offsets.starts_pmp=false;assert(apple_pmp_report_wait_ready(&entry,1234)==-EINVAL);assert(!atomic_calls && !request_calls && !wait_calls);balanced();
 reset(known[1]);timed_out=true;assert(apple_pmp_report_wait_ready(&entry,1234)==-ETIMEDOUT);assert(request_calls==1 && wait_calls==1 && !reads);balanced();
 reset(known[1]);report.start_result=-EIO;assert(apple_pmp_report_wait_ready(&entry,1234)==-EIO);assert(!reads);balanced();
 reset(known[1]);report.ack=0;assert(apple_pmp_report_wait_ready(&entry,1234)==-EINVAL);assert(!reads);balanced();
 reset(known[1]);mmio[1]=0;assert(apple_pmp_report_wait_ready(&entry,1234)==-EIO);assert(reads==2);balanced();
 reset(known[1]);mmio[2]=0;assert(apple_pmp_report_wait_ready(&entry,1234)==-EIO);assert(reads==2);balanced();
 reset(known[1]);stop_during_wait=true;assert(apple_pmp_report_wait_ready(&entry,1234)==-ENODEV);assert(report.waiters_done.count==1);balanced();
 puts("production PMP entry consumer startup/ready/ACK/lifetime controls: PASS");
}
'''


def main():
    source = SOURCE.read_text()
    begin = source.index('int apple_pmp_report_wait_ready(')
    end = source.index('EXPORT_SYMBOL_GPL(apple_pmp_report_wait_ready);', begin)
    body = source[begin:end]
    current = ('!of_device_is_compatible(parent, "apple,t6030-pmp-v2-report") &&\n'
               '\t    !of_device_is_compatible(parent, "apple,t8122-pmp-v2-report")')
    old = '!of_device_is_compatible(parent, "apple,t6030-pmp-v2-report")'
    assert body.count(current) == 1
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='pmp-entry-controls-') as directory:
        root = Path(directory)
        for name, implementation in [('production', body), ('old-guard', body.replace(current, old))]:
            path = root / (name + '.c')
            path.write_text(PLATFORM + implementation + CONTROLS)
            binary = root / name
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror', str(path), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if name == 'production':
                assert result.returncode == 0, result.stderr
                print(result.stdout, end='')
            else:
                assert result.returncode < 0 and 'apple_pmp_report_wait_ready(&entry,1234)==0' in result.stderr
                print('old T6030-only guard rejected by T8122 positive control: PASS')


if __name__ == '__main__':
    main()
