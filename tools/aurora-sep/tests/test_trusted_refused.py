#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""diag/trusted reports an enclave refusal, and the loader stops on it.

The loader runs as shipped against a synthetic sysfs and a fake keyctl, so the
test sees the same control flow as aurora-sep.service: how many seals it sends
and how it exits.
"""
import os
import re
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SHIM = ROOT / 'drivers/soc/apple/diag_shim.c'
HEADER = ROOT / 'drivers/soc/apple/shim.h'
LOADER = ROOT / 'tools/aurora-sep/load-driver'


def function(text, name):
    match = re.search(r'^(?:static ssize_t|void) ' + name + r'\(', text, re.M)
    if not match:
        raise AssertionError(name)
    start = text.index('{', match.start())
    depth = 1
    at = start + 1
    while depth:
        depth += (text[at] == '{') - (text[at] == '}')
        at += 1
    return text[match.start():at]


class ShowTests(unittest.TestCase):
    def test_states_and_signed_status(self):
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler, 'C compiler required; do not count a skip as a pass')
        text = SHIM.read_text()
        state = re.search(r'static struct \{.*?\} sep_diag;', text, re.S).group(0)
        defines = '\n'.join(re.findall(r'^#define SEP_DIAG_(?:TRUSTED_\w+|MAX_EP)\s+\S+$',
                                       HEADER.read_text() + text, re.M))
        body = (function(text, 'trusted_show')
                + function(text, 'sep_diag_set_trusted_atomic')
                + function(text, 'sep_diag_set_trusted')
                + function(text, 'sep_diag_clear_trusted_refusal_atomic')
                + function(text, 'sep_diag_clear_trusted_refusal'))
        prefix = '''#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;
typedef int8_t s8;
typedef int atomic_t;
typedef int64_t atomic64_t;
#define DECLARE_BITMAP(name, bits) unsigned long name[((bits) + 63) / 64]
struct device { int unused; };
struct device_attribute { int unused; };
#define atomic_read(p) (*(p))
#define atomic_set(p, v) (*(p) = (v))
static inline bool atomic_try_cmpxchg(atomic_t *p, int *old, int val) {
 if (*p == *old) { *p = val; return true; }
 *old = *p;
 return false;
}
static int sysfs_emit(char *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int sysfs_emit(char *out, const char *fmt, ...) {
 va_list args; va_start(args, fmt); int n = vsnprintf(out, 4096, fmt, args); va_end(args); return n;
}
'''
        main = '''int main(void) {
 char out[4096];
 trusted_show(NULL, NULL, out); fputs(out, stdout);
 sep_diag_clear_trusted_refusal(); trusted_show(NULL, NULL, out); fputs(out, stdout);
 sep_diag_set_trusted(SEP_DIAG_TRUSTED_REFUSED, -8); trusted_show(NULL, NULL, out); fputs(out, stdout);
 sep_diag_clear_trusted_refusal(); trusted_show(NULL, NULL, out); fputs(out, stdout);
 sep_diag_set_trusted(SEP_DIAG_TRUSTED_READY, 0); trusted_show(NULL, NULL, out); fputs(out, stdout);
 sep_diag_clear_trusted_refusal(); trusted_show(NULL, NULL, out); fputs(out, stdout);
 sep_diag_set_trusted(SEP_DIAG_TRUSTED_REFUSED, -128); trusted_show(NULL, NULL, out); fputs(out, stdout);
 sep_diag_set_trusted(SEP_DIAG_TRUSTED_REFUSED, 127); trusted_show(NULL, NULL, out); fputs(out, stdout);
 sep_diag_clear_trusted_refusal(); trusted_show(NULL, NULL, out); fputs(out, stdout);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            source, program = Path(tmp) / 'trusted.c', Path(tmp) / 'trusted'
            source.write_text(prefix + defines + '\n' + state + body + main)
            subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', str(source), '-o', str(program)], check=True)
            lines = subprocess.check_output([str(program)], text=True).splitlines()
        self.assertEqual(lines, [
            'unknown', 'unknown', 'refused -8', 'unknown',
            'ready', 'ready', 'refused -128', 'refused 127', 'unknown',
        ])


# keyctl as the loader uses it. A refused seal writes what the driver would
# record, when FAKE_REFUSE is set, and prints no key id.
KEYCTL = r'''#!/bin/sh
case "$1" in
    padd)
        cat >/dev/null
        echo x >>"$CALLS"
        if [ -n "${FAKE_REFUSE:-}" ]; then
            [ -d "$DIAG" ] && [ -n "${FAKE_TRUSTED:-}" ] && printf 'refused -8\n' >"$DIAG/trusted"
            exit 1
        fi
        if [ -n "${FAKE_WORK:-}" ]; then
            if [ "${3:-}" = "aurora-sep-load-check" ] && [ -d "$DIAG" ] && [ -n "${FAKE_TRUSTED:-}" ]; then
                printf 'ready\n' >"$DIAG/trusted"
            fi
            echo 1234
            exit 0
        fi
        exit 1
        ;;
    pipe) printf 'c0ffee\n' ;;
    revoke) ;;
esac
'''


class LoaderTests(unittest.TestCase):
    def run_loader(self, trusted=None, guard='0', refuse=True, work=False, timeout='120'):
        tmp = Path(tempfile.mkdtemp(prefix='sep-trusted-refused-'))
        self.addCleanup(shutil.rmtree, tmp)
        diag = tmp / 'devices/1234.sep/diag'
        diag.mkdir(parents=True)
        (tmp / 'drivers/apple_sep').mkdir(parents=True)
        os.symlink(tmp / 'devices/1234.sep', tmp / 'drivers/apple_sep/1234.sep')
        (diag / 'keystore').write_text('open\n')
        (diag / 'xart_guard_refusals').write_text(guard + '\n')
        if trusted is not None:
            (diag / 'trusted').write_text(trusted + '\n')
        (tmp / 'run').mkdir()
        for name in ('keybag', 'refkey'):
            (tmp / name).write_text('x')
        bin_dir = tmp / 'bin'
        bin_dir.mkdir()
        for name, body in (('keyctl', KEYCTL), ('modprobe', '#!/bin/sh\n'),
                           ('journalctl', '#!/bin/sh\n')):
            (bin_dir / name).write_text(body)
            (bin_dir / name).chmod(0o755)
        source = (LOADER.read_text()
                  .replace('/sys/module/apple_sep/parameters/keystore_only', str(tmp / 'none'))
                  .replace('/sys/bus/platform/devices', str(tmp / 'devices'))
                  .replace('/sys/bus/platform/drivers', str(tmp / 'drivers'))
                  .replace('/run/aurora-sep-load', str(tmp / 'run/aurora-sep-load'))
                  .replace('/var/lib/aurora-sep-keybag.bin', str(tmp / 'keybag'))
                  .replace('/var/lib/aurora-sep-refkey.bin', str(tmp / 'refkey')))
        script = tmp / 'load-driver'
        script.write_text(source)
        calls = tmp / 'calls'
        env = dict(os.environ, AURORA_SEP_SESSION='1', CALLS=str(calls), DIAG=str(diag),
                   AURORA_SEP_READY_TIMEOUT=timeout,
                   PATH=str(bin_dir) + os.pathsep + os.environ['PATH'])
        env.pop('FAKE_REFUSE', None)
        env.pop('FAKE_WORK', None)
        if trusted is not None:
            env['FAKE_TRUSTED'] = '1'
        if refuse:
            env['FAKE_REFUSE'] = '1'
        if work:
            env['FAKE_WORK'] = '1'
        began = time.monotonic()
        done = subprocess.run(['sh', str(script)], env=env, capture_output=True, text=True,
                              timeout=30)
        seals = len(calls.read_text().splitlines()) if calls.exists() else 0
        after_trusted = (diag / 'trusted').read_text().strip() if (diag / 'trusted').exists() else None
        return done, seals, time.monotonic() - began, after_trusted

    def test_first_refusal_stops_the_probe(self):
        done, seals, took, after = self.run_loader(trusted='unknown')
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertEqual(seals, 1)
        self.assertLess(took, 10)
        self.assertIn('diag/trusted: refused -8', done.stderr)
        self.assertEqual(after, 'refused -8')

    def test_rerun_sends_no_seal(self):
        done, seals, _, after = self.run_loader(trusted='refused -8')
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertEqual(seals, 0)
        self.assertIn('diag/trusted: refused -8', done.stderr)
        self.assertEqual(after, 'refused -8')

    def test_guard_refusal_is_named_first(self):
        for trusted in ('unknown', 'refused -8'):
            with self.subTest(trusted=trusted):
                done, seals, _, _ = self.run_loader(trusted=trusted, guard='1')
                self.assertEqual(done.returncode, 1, done.stderr)
                self.assertLessEqual(seals, 1)
                self.assertIn('xART guard refused', done.stderr)
                self.assertNotIn('diag/trusted', done.stderr)

    def test_without_the_attribute_the_deadline_still_applies(self):
        done, seals, _, _ = self.run_loader(trusted=None, timeout='1')
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertGreater(seals, 1)
        self.assertIn('within 1 s', done.stderr)

    def test_ready_kernel_still_verifies(self):
        done, seals, _, after = self.run_loader(trusted='ready', refuse=False, work=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(seals, 2)
        self.assertIn('trusted keys work', done.stderr)
        self.assertEqual(after, 'ready')

    def test_cleared_refusal_lets_rerun_probe_and_reach_ready(self):
        done, seals, _, after = self.run_loader(trusted='unknown', refuse=False, work=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(seals, 2)
        self.assertIn('trusted keys work', done.stderr)
        self.assertEqual(after, 'ready')

    def test_cleared_refusal_re_refuses_if_unlock_fails_again(self):
        done, seals, _, after = self.run_loader(trusted='unknown', refuse=True, work=False)
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertEqual(seals, 1)
        self.assertIn('diag/trusted: refused -8', done.stderr)
        self.assertEqual(after, 'refused -8')


if __name__ == '__main__':
    unittest.main()
