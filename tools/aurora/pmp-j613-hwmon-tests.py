#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the PMP telemetry driver with controlled MMIO and device lifetime."""
import argparse
from pathlib import Path
import re
import resource
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = 'drivers/hwmon/apple-pmp-j613-hwmon.c'

PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef uint8_t u8; typedef uint32_t u32; typedef int16_t s16; typedef int64_t s64;
typedef unsigned int umode_t;
#define __iomem
#define __init
#define __exit
#define max(a,b) ((a) > (b) ? (a) : (b))
#define DIV_ROUND_CLOSEST(a,b) (((a) < 0 ? (a)-(b)/2 : (a)+(b)/2)/(b))
enum hwmon_sensor_types { hwmon_temp };
enum { hwmon_temp_input, hwmon_temp_label };
struct device { void *driver; bool bound; pthread_mutex_t lock; };
struct platform_device { struct device dev; };
struct device_node { int unused; };
struct resource { uint64_t start, end; };
struct work_struct { int unused; };
struct delayed_work { int unused; };
struct of_device_id { const char *compatible; };
struct hwmon_ops {
 umode_t (*is_visible)(const void *, enum hwmon_sensor_types, u32, int);
 int (*read)(struct device *, enum hwmon_sensor_types, u32, int, long *);
 int (*read_string)(struct device *, enum hwmon_sensor_types, u32, int, const char **);
};
struct hwmon_channel_info { int unused; };
struct hwmon_chip_info { const struct hwmon_ops *ops; const struct hwmon_channel_info * const *info; };
#define HWMON_T_INPUT 1
#define HWMON_T_LABEL 2
#define HWMON_CHANNEL_INFO(...) (&(const struct hwmon_channel_info){0})
#define MODULE_DEVICE_TABLE(...)
#define MODULE_LICENSE(...)
#define MODULE_DESCRIPTION(...)
#define module_init(...)
#define module_exit(...)
#define pr_warn(...) ((void)0)
#define pr_info(...) ((void)0)
#define IS_ERR(p) ((intptr_t)(p) < 0 && (intptr_t)(p) > -4096)
#define PTR_ERR(p) ((long)(p))
static struct platform_device supplier;
static struct device_node node;
static struct device hwdev;
static u8 memory[0x80000];
static bool j613 = true, t8122 = true, compatible = true, uuid_ok = true;
static bool no_node, no_parent, no_map, no_alloc, bad_resource, no_resource;
static bool strict_reads = true, unstable, pause_read;
static unsigned long reads;
static unsigned int schedules, registrations, unregistrations, cancelled, refs;
static _Thread_local bool locked;
static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cond = PTHREAD_COND_INITIALIZER;
static bool entered, release_read, removal_started, removed;
static uint64_t window_size = 0x80000;
static void device_lock(struct device *dev) { pthread_mutex_lock(&dev->lock); locked = true; }
static void device_unlock(struct device *dev) { locked = false; pthread_mutex_unlock(&dev->lock); }
static bool device_is_bound(struct device *dev) { assert(locked); return dev->bound; }
static void put_unaligned_le32(u32 v, void *p) { memcpy(p, &v, 4); }
static u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v,p,4); return v; }
static uint16_t get_unaligned_le16(const void *p) { uint16_t v; memcpy(&v,p,2); return v; }
static u32 readl(const void *p) {
 uintptr_t off = (const u8 *)p - memory;
 assert(off % 4 == 0 && off + 4 <= sizeof(memory));
 if (strict_reads) assert(locked && supplier.dev.bound);
 reads++;
 if (pause_read) {
  pthread_mutex_lock(&pause_lock); entered = true; pthread_cond_broadcast(&pause_cond);
  while (!release_read) pthread_cond_wait(&pause_cond, &pause_lock);
  pthread_mutex_unlock(&pause_lock);
 }
 return get_unaligned_le32(p) ^ (unstable ? (u32)reads : 0);
}
static void *vmalloc(size_t n) { return no_alloc ? NULL : malloc(n); }
static void vfree(void *p) { free(p); }
static bool of_machine_is_compatible(const char *s) { return strcmp(s,"apple,j613") ? t8122 : j613; }
static struct device_node *of_find_node_by_path(const char *s) { return no_node ? NULL : &node; }
static int of_property_read_string(struct device_node *n, const char *s, const char **out) {
 *out = uuid_ok ? "EFD60284-7C58-33A9-8522-CB3366AF6040" : "wrong"; return 0;
}
static bool of_device_is_compatible(struct device_node *n, const char *s) { return compatible; }
static void of_node_put(struct device_node *n) { }
static int of_property_match_string(struct device_node *n, const char *a, const char *b) { return no_resource ? -1 : 0; }
static int of_address_to_resource(struct device_node *n, int i, struct resource *r) {
 r->start = 0x2d0500000ULL + (bad_resource ? 4 : 0); r->end = r->start + window_size - 1; return 0;
}
static uint64_t resource_size(struct resource *r) { return r->end - r->start + 1; }
static struct platform_device *of_find_device_by_node(struct device_node *n) {
 if (no_parent) return NULL;
 refs++; return &supplier;
}
static void put_device(struct device *d) { assert(refs); refs--; }
static void *ioremap(uint64_t base, size_t size) { assert(base == 0x2d0500000ULL && size == sizeof(memory)); return no_map ? NULL : memory; }
static void iounmap(void *p) { }
#define INIT_DELAYED_WORK(w,fn) ((void)(w), (void)(fn))
static void schedule_delayed_work(struct delayed_work *w, unsigned long t) { schedules++; }
static unsigned long msecs_to_jiffies(unsigned int n) { return n; }
static void cancel_delayed_work_sync(struct delayed_work *w) { cancelled++; }
static struct device *hwmon_device_register_with_info(struct device *d, const char *s, void *data, const struct hwmon_chip_info *c, void *groups) {
 registrations++; return &hwdev;
}
static void hwmon_device_unregister(struct device *d) { unregistrations++; }
'''

CONTROLS = r'''
static void records(void) {
 memset(memory, 0, sizeof(memory));
 for (unsigned int j = 0; j < CHANNELS; j++) {
  unsigned int o = 0x1000 + j * RECORD_SIZE;
  put_unaligned_le32(j + 1, memory + o); put_unaligned_le32(6, memory + o + 4);
  memcpy(memory + o + NAME_OFFSET, names[j], 7);
  s16 raw = (j + 40) * 64; memcpy(memory + o + 0x24, &raw, 2); memory[o + 0x30] = 1;
 }
}
static void *reader(void *unused) {
 long v; assert(read_temp(&hwdev, hwmon_temp, hwmon_temp_input, 0, &v) == 0 && v == 40000); return NULL;
}
static void *remove_supplier(void *unused) {
 pthread_mutex_lock(&pause_lock); removal_started = true; pthread_cond_broadcast(&pause_cond); pthread_mutex_unlock(&pause_lock);
 device_lock(&supplier.dev); supplier.dev.bound = false; supplier.dev.driver = NULL;
 pthread_mutex_lock(&pause_lock); removed = true; pthread_mutex_unlock(&pause_lock);
 device_unlock(&supplier.dev); return NULL;
}
int main(void) {
 pthread_mutex_init(&supplier.dev.lock, NULL);
 supplier.dev.driver = &supplier; supplier.dev.bound = true; records();
#ifdef OLD_CONTROL
 strict_reads = false;
 assert(pmp_j613_init() == 0 && scan() == 0);
 supplier.dev.bound = false;
 unsigned long before = reads; assert(scan() == 0 && reads > before);
 long v; assert(read_temp(&hwdev, hwmon_temp, hwmon_temp_input, 0, &v) == 0);
 put_unaligned_le32(99, memory + offsets[0] + 4);
 assert(read_temp(&hwdev, hwmon_temp, hwmon_temp_input, 0, &v) == 0);
 bad_resource = true; assert(pmp_j613_init() == 0);
 puts("original controls: unfinished/unbound supplier and changed type accepted"); return 0;
#else
 j613 = false; assert(pmp_j613_init() == -ENODEV); j613 = true;
 t8122 = false; assert(pmp_j613_init() == -ENODEV); t8122 = true;
 uuid_ok = false; assert(pmp_j613_init() == -ENODEV); uuid_ok = true;
 compatible = false; assert(pmp_j613_init() == -ENODEV); compatible = true;
 no_node = true; assert(pmp_j613_init() == -ENODEV); no_node = false;
 no_parent = true; assert(pmp_j613_init() == -ENODEV); no_parent = false;
 bad_resource = true; assert(pmp_j613_init() == -ENODEV); bad_resource = false;
 window_size -= 4; assert(pmp_j613_init() == -ENODEV); window_size += 8;
 assert(pmp_j613_init() == -ENODEV); window_size -= 4;
 no_resource = true; assert(pmp_j613_init() == -ENODEV); no_resource = false;
 no_map = true; assert(pmp_j613_init() == -ENOMEM && refs == 0); no_map = false;
 assert(pmp_j613_init() == 0 && refs == 1 && schedules == 1);
 supplier.dev.bound = false; unsigned long before = reads;
 assert(scan() == -EAGAIN && reads == before); scan_fn(NULL);
 assert(schedules == 2 && registrations == 0); supplier.dev.bound = true;
 no_alloc = true; assert(scan() == -ENOMEM); no_alloc = false;
 memset(memory,0,sizeof(memory)); assert(scan() == -EAGAIN); records();
 memcpy(memory + 0x8000, memory + 0x1000, RECORD_SIZE); assert(scan() == -EIO); records();
 memcpy(memory + 0x8000, memory + 0x1000 + RECORD_SIZE, RECORD_SIZE);
 memset(memory + 0x1000 + RECORD_SIZE, 0, RECORD_SIZE); assert(scan() == -EIO); records();
 scan_fn(NULL); assert(registrations == 1 && hwmon == &hwdev);
 long v; const char *label;
 assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,0,&v)==0 && v==40000);
 assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,CHANNELS,&v)==0 && v==45000);
 assert(read_label(&hwdev,hwmon_temp,hwmon_temp_label,0,&label)==0 && !strcmp(label,"ta000m"));
 assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,-1,&v)==-EINVAL);
 assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,CHANNELS+1,&v)==-EINVAL);
 assert(read_label(&hwdev,hwmon_temp,hwmon_temp_label,-1,&label)==-EINVAL);
 assert(visible(NULL,hwmon_temp,hwmon_temp_input,-1)==0);
 memory[offsets[0]+0x30]=0;
 assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,0,&v)==-ENODATA);
 assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,CHANNELS,&v)==0 && v==45000);
 memory[offsets[0]+0x30]=2; assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,0,&v)==-EIO); records();
 put_unaligned_le32(99,memory+offsets[0]+4); assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,0,&v)==-EIO); records();
 put_unaligned_le32(99,memory+offsets[0]); assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,0,&v)==-EIO); records();
 s16 raw=-1; memcpy(memory+offsets[0]+0x24,&raw,2);
 assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,0,&v)==0 && v==-16);
 raw=-2560; memcpy(memory+offsets[0]+0x24,&raw,2);
 assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,0,&v)==0 && v==-40000);
 raw=9600; memcpy(memory+offsets[0]+0x24,&raw,2);
 assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,0,&v)==0 && v==150000);
 raw=9601; memcpy(memory+offsets[0]+0x24,&raw,2); assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,0,&v)==-ERANGE); records();
 for (unsigned int j=0;j<CHANNELS;j++) memory[offsets[j]+0x30]=0;
 assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,CHANNELS,&v)==-ENODATA); records();
 memory[offsets[0]+NAME_OFFSET]='x'; assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,0,&v)==-EIO); records();
 unstable=true; assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,0,&v)==-EAGAIN); unstable=false;
 pthread_t r,t; pause_read=true; pthread_create(&r,NULL,reader,NULL);
 pthread_mutex_lock(&pause_lock); while(!entered) pthread_cond_wait(&pause_cond,&pause_lock); pthread_mutex_unlock(&pause_lock);
 pthread_create(&t,NULL,remove_supplier,NULL);
 pthread_mutex_lock(&pause_lock); while(!removal_started) pthread_cond_wait(&pause_cond,&pause_lock);
 assert(!removed); release_read=true; pthread_cond_broadcast(&pause_cond); pthread_mutex_unlock(&pause_lock);
 pthread_join(r,NULL); pthread_join(t,NULL); before=reads;
 assert(read_temp(&hwdev,hwmon_temp,hwmon_temp_input,0,&v)==-ENODEV && reads==before);
 assert(scan()==-EAGAIN && reads==before);
 attempts=SCAN_ATTEMPTS-1; before=schedules; scan_fn(NULL); assert(schedules==before);
 pmp_j613_exit(); assert(cancelled==1 && unregistrations==1 && refs==0);
 puts("PMP hwmon identity, bounds, parser, poll and supplier-removal controls: PASS"); return 0;
#endif
}
'''


def compile_run(source, directory, name, old=False, expect_failure=False):
    path = directory / (name + '.c')
    # Compile all production functions; only kernel API boundaries are replaced.
    path.write_text(PREFIX + re.sub(r'^#include .*$', '', source, flags=re.M) + CONTROLS)
    binary = directory / name
    command = ['cc', '-std=gnu11', '-pthread', '-O1', '-g',
               '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-function',
               '-Wno-unused-const-variable', '-Wno-sign-compare', '-Werror',
               str(path), '-o', str(binary)]
    if old:
        command.insert(1, '-DOLD_CONTROL')
    subprocess.run(command, check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    if expect_failure:
        assert result.returncode == -signal.SIGABRT, name + ' did not reject the mutant by assertion'
        print(name + ': rejected')
    else:
        assert result.returncode == 0, result.stderr
        print(result.stdout.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--old-ref', help='Git revision containing the original driver')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    source = (ROOT / SOURCE).read_text()
    with tempfile.TemporaryDirectory(prefix='pmp-j613-hwmon-') as tmp:
        directory = Path(tmp)
        compile_run(source, directory, 'candidate')
        mutants = {
            'unfinished-probe': source.replace('device_is_bound(&parent->dev)', 'parent->dev.driver'),
            'changed-record-type': source.replace('get_unaligned_le32(a + 4) != 6 ||', 'false ||'),
            'oversized-resource': source.replace('resource_size(&res) != SRAM_SIZE', 'resource_size(&res) < SRAM_SIZE'),
        }
        for name, mutant in mutants.items():
            assert mutant != source
            compile_run(mutant, directory, name, expect_failure=True)
        if args.old_ref:
            old = subprocess.check_output(['git', '-C', str(ROOT), 'show', args.old_ref + ':' + SOURCE], text=True)
            compile_run(old, directory, 'original', old=True)


if __name__ == '__main__':
    main()
