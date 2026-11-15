#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check brcmfmac roaming command payloads and setup failure behavior."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

from importlib.util import module_from_spec, spec_from_file_location

ROOT = Path(__file__).resolve().parents[2]
DRIVER = 'drivers/net/wireless/broadcom/brcm80211/brcmfmac/'
spec = spec_from_file_location('bss_controls', ROOT / 'tools/asahi/brcmf-bss-tests.py')
bss = module_from_spec(spec)
spec.loader.exec_module(bss)

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <endian.h>
typedef int32_t s32; typedef uint32_t u32; typedef uint32_t __le32;
#define cpu_to_le32(x) htole32(x)
#define bphy_err(...) ((void)0)
#define brcmf_dbg(...) ((void)0)
struct settings { bool roamoff; };
struct brcmf_pub { struct settings *settings; };
struct brcmf_if { struct brcmf_pub *drvr; };
static unsigned calls, stage;
static int fail_stage;
static uint32_t delta[2];
static int brcmf_fil_iovar_int_set(struct brcmf_if *i, const char *name, u32 v)
{
 assert(++stage <= 2);
 assert(!strcmp(name, stage == 1 ? "bcn_timeout" : "roam_off"));
 assert(v == (stage == 2 ? i->drvr->settings->roamoff :
        i->drvr->settings->roamoff ? BRCMF_DEFAULT_BCN_TIMEOUT_ROAM_OFF :
        BRCMF_DEFAULT_BCN_TIMEOUT_ROAM_ON));
 return fail_stage == stage ? -5 : 0;
}
static int brcmf_fil_cmd_data_set(struct brcmf_if *i, int cmd, void *p, unsigned len)
{
 (void)i; assert(len == 8); ++stage;
 assert(cmd == (stage == 3 ? BRCMF_C_SET_ROAM_TRIGGER : BRCMF_C_SET_ROAM_DELTA));
 if (cmd == BRCMF_C_SET_ROAM_DELTA) { calls++; memcpy(delta, p, 8); }
 return fail_stage == stage ? -5 : 0;
}
'''
MAIN = r'''
int main(void)
{
 for (unsigned off = 0; off < 2; off++) {
  for (unsigned failure = 0; failure <= 4; failure++) {
   struct settings settings = { .roamoff = off };
   struct brcmf_pub pub = { &settings }; struct brcmf_if ifp = { &pub };
   calls = stage = 0; memset(delta, 0, sizeof(delta)); fail_stage = failure;
   int rc = brcmf_dongle_roam(&ifp);
   if (failure == 1 || failure == 2) {
    assert(rc == -5 && stage == failure && calls == 0);
   } else {
    assert(rc == 0 && stage == 4 && calls == 1);
    assert(delta[0] == htole32(WL_ROAM_DELTA));
    assert(delta[1] == htole32(BRCM_BAND_ALL));
   }
  }
 }
 puts("10 roaming payload and failure controls passed"); return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ref', help='Git source revision to exercise')
    parser.add_argument('--out', type=Path, help='retain executable and results')
    args = parser.parse_args()

    def read(name):
        if args.ref:
            return subprocess.check_output(['git', 'show', args.ref + ':' + name],
                                           cwd=ROOT, text=True)
        return (ROOT / name).read_text()

    files = ['cfg80211.c', 'cfg80211.h', 'fwil.h']
    sources = [read(DRIVER + name) for name in files]
    sources.append(read('drivers/net/wireless/broadcom/brcm80211/include/defs.h'))
    constants = '\n'.join(line for source in sources for line in source.splitlines()
                          if re.match(r'#define\s+(BRCMF_DEFAULT_BCN_TIMEOUT_ROAM_(OFF|ON)|'
                                      r'WL_ROAM_TRIGGER_LEVEL|WL_ROAM_DELTA|BRCM_BAND_ALL|'
                                      r'BRCMF_C_SET_ROAM_TRIGGER|BRCMF_C_SET_ROAM_DELTA)\s', line))
    body = bss.function(sources[0], 'brcmf_dongle_roam')
    assert body, 'missing roaming setup function'
    code = PREFIX.replace('static unsigned calls', constants + '\nstatic unsigned calls') + body + MAIN

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        source = out / 'controls.c'
        source.write_text(code)
        exe = out / 'controls'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g',
                        '-ftrivial-auto-var-init=pattern', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(source), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        receipt = {'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr,
                   'limits': 'Firmware calls use observing host boundaries; no hardware roaming test.'}
        (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(result.stdout or result.stderr)
        return result.returncode != 0

    if args.out:
        return run(args.out)
    with tempfile.TemporaryDirectory(prefix='brcmf-roam-') as out:
        return run(Path(out))


if __name__ == '__main__':
    raise SystemExit(main())
