#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run production MTP PM and magicmouse resume callbacks without a device.

Checks notification order, suspend rollback, resume-error handling and the
actuator mode cache. The old transport must compile and fail at runtime.
"""
import argparse
import os
from pathlib import Path
import resource
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TRANSPORT = 'drivers/hid/dockchannel-hid/dockchannel-hid.c'


def function(source, name):
    for kind in ('int', 'void'):
        prefix = 'static ' + kind + ' ' + name + '('
        if prefix in source:
            start = source.index(prefix)
            return source[start:source.index('\n}', start)+2]
    raise ValueError(name)


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#define MAX_INTERFACES 16
#define PMSG_SUSPEND 1
#define HID_HAPTIC_MODE_DEVICE 0
#define HID_HAPTIC_MODE_HOST 1
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x) = (v))
struct haptics { int mode; };
struct magicmouse_sc { struct haptics *haptics; bool haptic_button_down, haptic_deep_down; };
struct hid_device { struct magicmouse_sc *data; int id; };
struct dchid_iface { struct hid_device *hid; };
struct dockchannel_hid { struct dchid_iface *ifaces[MAX_INTERFACES]; bool suspended; };
struct device { struct dockchannel_hid *data; };
static struct dockchannel_hid *transport;
static unsigned suspends[16], resumes[16], key_releases;
static int suspend_error_at = -1, resume_error_at = -1;
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void *hid_get_drvdata(struct hid_device *d) { return d->data; }
static void magicmouse_force_key(struct magicmouse_sc *m, bool down) {
    assert(!down); key_releases++;
}
static int hid_driver_suspend(struct hid_device *h, int state) {
    assert(state == PMSG_SUSPEND && !transport->suspended);
    suspends[h->id]++;
    return h->id == suspend_error_at ? -EBUSY : 0;
}
'''
MIDDLE = r'''
static int hid_driver_resume(struct hid_device *h) {
    assert(!transport->suspended);
    resumes[h->id]++;
    magicmouse_resume(h);
    return h->id == resume_error_at ? -EIO : 0;
}
'''
CASES = r'''
int main(void) {
    struct haptics hap = {.mode = HID_HAPTIC_MODE_HOST};
    struct magicmouse_sc m = {.haptics = &hap, .haptic_button_down = true, .haptic_deep_down = true};
    struct hid_device h0 = {.data = &m, .id = 0}, h4 = {.id = 4}, h8 = {.id = 8};
    struct dchid_iface i0 = {.hid = &h0}, i4 = {.hid = &h4}, i8 = {.hid = &h8}, empty = {0};
    struct dockchannel_hid t = {.ifaces = {[0] = &i0, [3] = &empty, [4] = &i4, [8] = &i8}};
    struct device d = {.data = &t}; transport = &t;
    assert(!dockchannel_hid_suspend(&d) && t.suspended);
    assert(!dockchannel_hid_resume(&d) && !t.suspended);
    assert(hap.mode == HID_HAPTIC_MODE_DEVICE);
    assert(!m.haptic_button_down && !m.haptic_deep_down && key_releases == 1);
    assert(suspends[0] == 1 && resumes[0] == 1 && resumes[8] == 1);
    /* A later HID failure rolls back only the drivers that suspended. */
    suspend_error_at = 4;
    assert(dockchannel_hid_suspend(&d) == -EBUSY && !t.suspended);
    assert(resumes[0] == 2 && resumes[4] == 1 && suspends[8] == 1);
    suspend_error_at = 0;
    assert(dockchannel_hid_suspend(&d) == -EBUSY && !t.suspended);
    assert(resumes[0] == 2 && suspends[4] == 2);
    suspend_error_at = -1;
    assert(!dockchannel_hid_suspend(&d) && t.suspended);
    /* One failed resume cannot keep the other interfaces suspended. */
    resume_error_at = 4;
    assert(dockchannel_hid_resume(&d) == -EIO && !t.suspended);
    assert(resumes[0] == 3 && resumes[4] == 2 && resumes[8] == 2);
    resume_error_at = -1;
    for (unsigned n = 0; n < 3; n++) {
        hap.mode = HID_HAPTIC_MODE_HOST;
        assert(!dockchannel_hid_suspend(&d));
        assert(!dockchannel_hid_resume(&d));
        assert(hap.mode == HID_HAPTIC_MODE_DEVICE);
    }
    puts("MTP PM: HID notifications, rollback, all-interface resume and haptic cache passed");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out')
    p.add_argument('--old-transport')
    args = p.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    source = ((ROOT / TRANSPORT).read_text() if not args.old_transport else subprocess.check_output(
        ['git', 'show', args.old_transport + ':' + TRANSPORT], cwd=ROOT, text=True))
    mouse = (ROOT / 'drivers/hid/hid-magicmouse.c').read_text()
    with tempfile.TemporaryDirectory(prefix='mtp-pm-') as tmp:
        out = Path(args.out or tmp)
        out.mkdir(parents=True, exist_ok=True)
        c, exe = out/'mtp-pm.c', out/'mtp-pm'
        c.write_text(PREFIX + function(mouse, 'magicmouse_haptic_forget_mode') + '\n' +
                     function(mouse, 'magicmouse_resume') + MIDDLE +
                     function(source, 'dockchannel_hid_suspend') + '\n' +
                     function(source, 'dockchannel_hid_resume') + CASES)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                        '-Wno-unused-parameter', '-Wno-unused-function',
                        str(c), '-o', str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        (out/'run.log').write_text(r.stdout+r.stderr)
        if args.old_transport:
            if r.returncode != -6 or 'Assertion' not in r.stderr:
                raise SystemExit('old transport did not fail its runtime assertion')
            print('MTP PM: old transport compiled and failed the haptic-cache assertion')
        elif r.returncode:
            raise SystemExit(r.stdout+r.stderr)
        else:
            print(r.stdout, end='')


if __name__ == '__main__':
    main()
