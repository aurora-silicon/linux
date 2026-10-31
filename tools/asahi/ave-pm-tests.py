#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the AVE sleep callbacks with a firmware halt failure.

Compile the production callbacks against a simulated m2m scheduler and firmware.
No encoder registers, firmware or media device are needed. The old callback is a
negative control: it must fail the stuck-firmware assertion after compiling.
"""
import argparse
import os
from pathlib import Path
import resource
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = 'drivers/media/platform/apple/ave/ave-drv.c'


def function(source, name):
    start = source.index('static int ' + name + '(')
    end = source.index('\n}', start) + 2
    return source[start:end]


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
struct scheduler { bool stopped; unsigned pauses, restarts; };
struct ave_transport { bool scratch_halt; };
struct ave_abi { const struct ave_transport *transport; };
struct ave_layout { const struct ave_abi *abi; };
struct ave_dev { struct scheduler *m2m_dev; int hw_lock; const struct ave_layout *layout; };
struct device { struct ave_dev *data; };
static int shutdown_error;
static unsigned closes, halts, releases, wakes;
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void mutex_lock(int *l) { assert(!*l); *l = 1; }
static void mutex_unlock(int *l) { assert(*l == 1); *l = 0; }
static void v4l2_m2m_suspend(struct scheduler *s) {
    assert(!s->stopped); s->stopped = true; s->pauses++;
}
static void v4l2_m2m_resume(struct scheduler *s) {
    assert(s->stopped); s->stopped = false; s->restarts++;
}
static void ave_v4l2_suspend(struct ave_dev *a) {
    assert(a->hw_lock && a->m2m_dev->stopped); closes++;
}
static int ave_fw_shutdown(struct ave_dev *a) {
    assert(a->hw_lock && a->m2m_dev->stopped && closes > halts);
    halts++; return shutdown_error;
}
/* A scratch-halt firmware (6070) gives its boot buffers back after a halt. */
static __attribute__((unused)) void ave_fw_release_boot(struct ave_dev *a) {
    assert(a->hw_lock && halts > releases); releases++;
}
static __attribute__((unused)) int ave_runtime_resume(struct device *d) { (void)d; wakes++; return 0; }
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
'''
CASES = r'''
int main(void) {
    struct scheduler s = {0};
    struct ave_dev a = {.m2m_dev = &s};
    struct device d = {.data = &a};
    for (unsigned n = 0; n < 3; n++) {
        shutdown_error = 0;
        assert(ave_suspend(&d) == 0);
        assert(s.stopped && !a.hw_lock);
        assert(ave_resume(&d) == 0);
        assert(!s.stopped && !a.hw_lock);
    }
    /* PM does not call resume for a device whose suspend returned an error. */
    shutdown_error = -EBUSY;
    assert(ave_suspend(&d) == -EBUSY);
    assert(!s.stopped && !a.hw_lock);
    /* An aborted sleep must not leave the scheduler stopped for the next one. */
    shutdown_error = -EIO;
    assert(ave_suspend(&d) == -EIO);
    assert(!s.stopped && !a.hw_lock);
    shutdown_error = 0;
    assert(ave_suspend(&d) == 0);
    assert(s.stopped);
    assert(ave_resume(&d) == 0);
    assert(!s.stopped && !a.hw_lock);
    assert(s.pauses == 6 && s.restarts == 6 && closes == 6 && halts == 6);
    assert(releases == 0 && wakes == 4);
    /* A scratch-halt firmware releases its boot buffers only after a clean halt. */
    static const struct ave_transport scratch = { .scratch_halt = true };
    static const struct ave_abi abi = { .transport = &scratch };
    static const struct ave_layout layout = { .abi = &abi };
    a.layout = &layout;
    shutdown_error = -EBUSY;
    assert(ave_suspend(&d) == -EBUSY && releases == 0);
    shutdown_error = 0;
    assert(ave_suspend(&d) == 0 && releases == 1);
    assert(ave_resume(&d) == 0 && wakes == 5);
    puts("AVE PM: normal/repeated sleep, halt refusal, error propagation, rollback, retry and boot-buffer release passed");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out')
    p.add_argument('--old-ref', help='expect this revision to fail the runtime control')
    args = p.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    source = ((ROOT / SOURCE).read_text() if not args.old_ref else subprocess.check_output(
        ['git', 'show', args.old_ref + ':' + SOURCE], cwd=ROOT, text=True))
    with tempfile.TemporaryDirectory(prefix='ave-pm-') as tmp:
        out = Path(args.out or tmp)
        out.mkdir(parents=True, exist_ok=True)
        c = out / 'ave-pm.c'
        c.write_text(PREFIX + function(source, 'ave_suspend') + '\n' +
                     function(source, 'ave_resume') + CASES)
        exe = out / 'ave-pm'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                        '-Werror', str(c), '-o', str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        (out / 'run.log').write_text(r.stdout + r.stderr)
        if args.old_ref:
            if r.returncode != -6 or 'Assertion' not in r.stderr:
                raise SystemExit('old callback did not fail its runtime assertion')
            print('AVE PM: old callback compiled and failed the halt-refusal assertion')
        elif r.returncode:
            raise SystemExit(r.stdout + r.stderr)
        else:
            print(r.stdout, end='')


if __name__ == '__main__':
    main()
