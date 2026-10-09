#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host controls for devcoredump registration before built-in device probes."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'drivers/base/devcoredump.c'


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef long ssize_t; typedef long loff_t; typedef int gfp_t; typedef int atomic_t;
#define __init
#define ATOMIC_INIT(n) (n)
#define KOBJ_ADD 1
#define subsys_initcall(fn) static const int class_level = 4
#define __initcall(fn) static const int class_level = 6
struct class { bool registered; };
struct kobject { int unused; };
struct device { struct class *class; struct kobject kobj; };
struct module { int unused; };
struct mutex { int unused; };
struct delayed_work { int unused; };
struct devcd_entry {
 struct device devcd_dev; void *data; size_t datalen; struct mutex mutex;
 bool init_completed, deleted; struct module *owner;
 ssize_t (*read)(char *, loff_t, size_t, void *, size_t);
 void (*free)(void *); struct delayed_work del_wk; struct device *failing_dev;
};
static struct class devcd_class;
static bool devcd_disabled, deny_module, deny_alloc;
static struct devcd_entry *published;
static int warnings, freed, announcements, module_refs;
static int class_register(struct class *c) { c->registered = true; return 0; }
static struct device *class_find_device(struct class *c, void *start,
 const void *dev, int (*match)(struct device *, const void *)) {
 if (!c->registered) { warnings++; return NULL; }
 return published ? &published->devcd_dev : NULL;
}
static int devcd_match_failing(struct device *d, const void *p) { return 1; }
static bool try_module_get(struct module *m) { if (deny_module) return false; module_refs++; return true; }
static void module_put(struct module *m) { module_refs--; }
#define kzalloc_obj(obj, gfp) (deny_alloc ? NULL : calloc(1, sizeof(obj)))
static struct device *get_device(struct device *d) { return d; }
static void put_device(struct device *d) { }
static void mutex_init(struct mutex *m) { }
static void mutex_lock(struct mutex *m) { }
static void mutex_unlock(struct mutex *m) { }
static void device_initialize(struct device *d) { }
static void dev_set_name(struct device *d, const char *format, int n) { }
static int atomic_inc_return(atomic_t *a) { return ++*a; }
static void dev_set_uevent_suppress(struct device *d, bool b) { }
static void devcd_del(void *d) { }
#define INIT_DELAYED_WORK(w, fn) ((void)(w), (void)(fn))
static void schedule_delayed_work(struct delayed_work *w, unsigned long t) { }
static void cancel_delayed_work_sync(struct delayed_work *w) { }
static int device_add(struct device *d) {
 if (!d->class->registered) return -1;
 published = (struct devcd_entry *)((char *)d - offsetof(struct devcd_entry, devcd_dev));
 return 0;
}
static int sysfs_create_link(struct kobject *a, struct kobject *b, const char *n) { return 0; }
static void dev_warn(struct device *d, const char *msg) { warnings++; }
static void kobject_uevent(struct kobject *k, int type) { announcements++; }
'''

CONTROLS = r'''
static void release_snapshot(void *data) { freed++; free(data); }
static ssize_t read_snapshot(char *out, loff_t offset, size_t n, void *data, size_t length) {
 if ((size_t)offset >= length) return 0;
 if (n > length - offset) n = length - offset;
 memcpy(out, (char *)data + offset, n); return n;
}
static void probe(struct device *device) {
 size_t length = 478 * 1024;
 char *snapshot = malloc(length); assert(snapshot);
 memset(snapshot, 0x5a, length); memcpy(snapshot, "M3FWD001", 8);
 dev_coredumpm_timeout(device, NULL, snapshot, length, 0,
                      read_snapshot, release_snapshot, 300);
}
int main(void) {
 struct device failing = {0};
 /* Put the built-in driver first when both initcalls have the same level. */
 if (class_level < 6) assert(devcoredump_init() == 0);
 probe(&failing);
 if (class_level >= 6) assert(devcoredump_init() == 0);
#ifdef OLD
 assert(warnings == 1 && freed == 1 && !published && !announcements);
 puts("PASS original same-level early probe loses snapshot before class registration");
#else
 assert(warnings == 0 && freed == 0 && published && announcements == 1);
 assert(published->init_completed && published->datalen == 478 * 1024);
 char first[8], last[16];
 assert(published->read(first, 0, 8, published->data, published->datalen) == 8);
 assert(!memcmp(first, "M3FWD001", 8));
 assert(published->read(last, published->datalen - 16, 16, published->data, published->datalen) == 16);
 for (unsigned i = 0; i < sizeof(last); i++) assert(last[i] == 0x5a);
 /* A later snapshot cannot replace the first unread snapshot. */
 struct devcd_entry *first_dump = published;
 probe(&failing); assert(published == first_dump && freed == 1 && announcements == 1);
 published->free(published->data); module_put(published->owner); free(published); published = NULL;
 assert(module_refs == 0);
 devcd_disabled = true; probe(&failing); assert(!published && freed == 3);
 devcd_disabled = false; deny_module = true; probe(&failing); assert(!published && freed == 4);
 deny_module = false; deny_alloc = true; probe(&failing); assert(!published && freed == 5 && module_refs == 0);
 puts("PASS early snapshot readback/lifetime, duplicate, disabled, module and allocation refusal");
#endif
}
'''


def check(source, directory, old=False):
    declaration = re.search(r'(?:subsys_initcall|__initcall)\(devcoredump_init\);', source)
    if declaration is None:
        raise ValueError('devcoredump initcall registration was not found')
    code = PREFIX + function(source, 'static int __init devcoredump_init(void)')
    code += '\n' + declaration[0] + ';\n'
    code += function(source, 'void dev_coredumpm_timeout(struct device *dev,') + CONTROLS
    path = directory / ('old.c' if old else 'current.c')
    path.write_text(code)
    command = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-Wno-unused-function']
    if old:
        command.append('-DOLD')
    subprocess.run(command + [str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--old-source', type=Path)
    args = parser.parse_args()
    init = (ROOT / 'include/linux/init.h').read_text()
    assert re.search(r'#define subsys_initcall\(fn\)\s+__define_initcall\(fn, 4\)', init)
    assert re.search(r'#define __initcall\(fn\)\s+device_initcall\(fn\)', init)
    assert re.search(r'#define device_initcall\(fn\)\s+__define_initcall\(fn, 6\)', init)
    with tempfile.TemporaryDirectory(prefix='devcoredump-init-') as tmp:
        check(SOURCE.read_text(), Path(tmp))
        if args.old_source:
            check(args.old_source.read_text(), Path(tmp), old=True)
    print('Production init/publication controls use mocked device/class services; no physical boot claim.')


if __name__ == '__main__':
    main()
