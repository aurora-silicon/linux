#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile this repository's T6050 wake-report callbacks with controlled MMIO.

The test reads drivers/soc/apple/sleep-t6050.h from its own checkout, never
from a frozen proposal. Tests are source/host checks, not hardware qualification.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import resource
import signal
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location('wake_report', HERE / 't6050-wake-report.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
parse = module.parse


def block(source, name):
    match = re.search(r'^static (?:void|int|bool) ' + name + r'\([^;]*?\)\n\{', source, re.M)
    if not match:
        raise RuntimeError('production function missing: ' + name)
    brace = source.index('{', match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def program(source):
    constants = '\n'.join(re.findall(r'^#define T6050_WAKE_(?:ENABLE|WORDS|STATUS|GFX_STATUS)\s+[^\n]+', source, re.M))
    fields = '\n'.join(re.findall(r'^\s*(?:u32 wake_pending\[[^\n]+|bool wake_snapshot_valid;)', source, re.M))
    helper = block(source, 't6050_wake_snapshot') + '\n' + block(source, 't6050_report_wake_snapshot')
    begin = block(source, 't6050_sleep_begin').split('{', 1)[1].split('\tt6050_sleep_pins_low(s);', 1)[0]
    wake = block(source, 't6050_sleep_wake_regs').split('{', 1)[1].split('\tif (r->spmi_masked)', 1)[0]
    finish = block(source, 't6050_sleep_finish')
    assert finish.index('t6050_report_wake_snapshot(s);') > finish.index('t6050_sleep_undo_regs(s);')
    suffix = finish[finish.index('\tr->usb_aon_kept = 0;'):finish.index('\tdev_dbg(s->dev, "entry')]
    return PRELUDE.replace('PRODUCTION_FIELDS', fields) + constants + '\n' + helper + '''
static void begin_prefix(struct apple_sleep *s) {''' + begin + '''
    (void)r; (void)i; (void)ret;
}
static void wake_prefix(struct apple_sleep *s) {''' + wake + '''
    (void)r; (void)i; (void)value;
    mock_ack();
}
static void finish_suffix(struct apple_sleep *s) {
    struct apple_sleep_region_state *r = s->region;
''' + suffix + '\n}\n' + TESTS


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef uint32_t u32;
struct apple_sleep_region_state {
    void *wake, *wake_gfx;
    bool wake_armed;
    unsigned usb_aon_kept;
PRODUCTION_FIELDS
};
struct apple_sleep { struct apple_sleep_region_state *region; bool returned; void *dev; };
static u32 ap[1024], gfx[1024], observed_pending[11], observed_masks[11];
static struct apple_sleep_region_state *active;
static int reads, reports, warnings;
static bool acknowledged;
static u32 mock_readl(const void *ptr)
{
    uintptr_t address=(uintptr_t)ptr, ab=(uintptr_t)ap, gb=(uintptr_t)gfx;
    assert(!active->wake_snapshot_valid && !acknowledged);
    if (address>=ab && address<ab+sizeof(ap)) {
        unsigned offset=(unsigned)(address-ab);
        assert((offset>=0xc && offset<=0x30 && !(offset&3)) ||
               (offset>=0x34 && offset<=0x58 && !(offset&3)));
    } else {
        assert(address>=gb && address<gb+sizeof(gfx));
        unsigned offset=(unsigned)(address-gb);
        assert(offset==0xc || offset==0x10);
    }
    reads++;
    return *(const u32 *)ptr;
}
#define readl(p) mock_readl(p)
#define writel(v,p) assert(!"observation unexpectedly writes MMIO")
static void mock_info(const char *format, int length, const void *bytes, unsigned last)
{
    assert(acknowledged && length==40);
    if (!strcmp(format,"wake pending AP %*ph GFX %#x\n")) {
        memcpy(observed_pending,bytes,40); observed_pending[10]=last;
    } else {
        assert(!strcmp(format,"wake masks AP %*ph GFX %#x\n"));
        memcpy(observed_masks,bytes,40); observed_masks[10]=last;
    }
    reports++;
}
#define dev_info(dev,...) mock_info(__VA_ARGS__)
#define dev_warn(dev,format) do { assert(!strcmp(format,"wake register snapshot unavailable\n")); warnings++; } while(0)
static void mock_ack(void)
{
    acknowledged=true;
    memset(ap,0,sizeof(ap)); memset(gfx,0,sizeof(gfx));
}
'''

TESTS = r'''
static const u32 expected_masks[11]={2952462335U,3221225279U,4294961245U,2080374783U,
    4161273855U,4286576639U,4294967295U,4294967295U,4294967295U,27U,33554429U};
static void setup(struct apple_sleep *s, struct apple_sleep_region_state *r)
{
    memset(r,0,sizeof(*r)); memset(ap,0,sizeof(ap)); memset(gfx,0,sizeof(gfx));
    memset(observed_pending,0,sizeof(observed_pending)); memset(observed_masks,0,sizeof(observed_masks));
    r->wake=ap; r->wake_gfx=gfx; r->wake_armed=true;
    *s=(struct apple_sleep){.region=r,.returned=false}; active=r;
    reads=reports=warnings=0; acknowledged=false;
    for(unsigned i=0;i<10;i++) ap[0xc/4+i]=expected_masks[i];
    gfx[0xc/4]=expected_masks[10];
}
static void dump(const char *label, const u32 values[11])
{
    printf("wake %s AP",label);
    for(unsigned i=0;i<40;i++) printf(" %02x",((const unsigned char *)values)[i]);
    printf(" GFX %#x\n",values[10]);
}
int main(void)
{
    struct apple_sleep s; struct apple_sleep_region_state r;
    setup(&s,&r); r.wake_snapshot_valid=true; begin_prefix(&s);
    assert(!r.wake_snapshot_valid);
    for(unsigned id=0;id<352;id++) {
        setup(&s,&r);
        if(id<320) ap[0x34/4+id/32]=1U<<(id%32); else gfx[0x10/4]=1U<<(id-320);
        wake_prefix(&s);
        assert(reads==22 && r.wake_snapshot_valid && acknowledged);
        /* apple_sleep_return sets this only after the SoC wake callback. */
        s.returned=true;
        finish_suffix(&s);
        assert(reads==22 && reports==2 && warnings==0);
        for(unsigned word=0;word<11;word++) {
            assert(observed_pending[word]==(word==id/32 ? 1U<<(id%32) : 0));
            assert(observed_masks[word]==expected_masks[word]);
        }
        dump("pending",observed_pending); dump("masks",observed_masks);
    }
    for(unsigned missing=0;missing<3;missing++) {
        setup(&s,&r); r.wake_snapshot_valid=true;
        if(missing==0) r.wake=NULL;
        if(missing==1) r.wake_gfx=NULL;
        if(missing==2) r.wake_armed=false;
        t6050_wake_snapshot(&s);
        assert(!r.wake_snapshot_valid && reads==0);
        mock_ack(); s.returned=true; finish_suffix(&s); assert(reports==0 && warnings==1);
    }
    setup(&s,&r); ap[0x34/4+76/32]=1U<<(76%32); wake_prefix(&s);
    s.returned=false; finish_suffix(&s); assert(reports==0 && warnings==0);
    return 0;
}
'''


def controls(source):
    return {
        'truncate-ap-to-nine': source.replace('i < T6050_WAKE_WORDS; i++', 'i < T6050_WAKE_WORDS - 1; i++', 1),
        'wrong-ap-status-offset': source.replace('#define T6050_WAKE_STATUS\t0x34', '#define T6050_WAKE_STATUS\t0x28', 1),
        'gfx-reads-mask-as-pending': source.replace('#define T6050_WAKE_GFX_STATUS\t0x10', '#define T6050_WAKE_GFX_STATUS\t0x0c', 1),
        'gfx-overwrites-ap-word-nine': source.replace('r->wake_pending[T6050_WAKE_WORDS] =', 'r->wake_pending[T6050_WAKE_WORDS - 1] =', 1),
        'invert-mask-readback': source.replace('r->wake_masks_at_return[i] = readl(', 'r->wake_masks_at_return[i] = ~readl(', 1),
        'capture-after-ack': source.replace('\tt6050_wake_snapshot(s);\n\tif (r->spmi_masked)', '\tif (r->spmi_masked)', 1).replace('\t\tt6050_spmi_pmu_ack(s);', '\t\tt6050_spmi_pmu_ack(s);\n\t\tt6050_wake_snapshot(s);', 1),
        'omit-begin-invalidation': source.replace('\tr->wake_snapshot_valid = false;\n\tt6050_sleep_pins_low(s);', '\tt6050_sleep_pins_low(s);', 1),
        'report-on-refusal': source.replace('\tif (!s->returned)\n\t\treturn;\n', '', 1),
        'capture-gated-on-later-returned-flag': source.replace(
            '\tr->wake_snapshot_valid = false;\n\tif (!r->wake_armed',
            '\tr->wake_snapshot_valid = false;\n\tif (!s->returned)\n\t\treturn;\n\tif (!r->wake_armed', 1),
    }


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--tree', type=Path, default=ROOT, help='checkout root (default: this repository)')
    parser.add_argument('--output', '--out', dest='out', type=Path)
    parser.add_argument('--negative-controls', action='store_true')
    args = parser.parse_args()
    args.out = args.out or Path(tempfile.mkdtemp(prefix='t6050-wake-reporting-')) / 'out'
    args.out.mkdir(parents=True, exist_ok=False)
    source_path = args.tree / 'drivers/soc/apple/sleep-t6050.h'
    source = source_path.read_text()
    results = []
    variants = {'production-current': source}
    if args.negative_controls:
        variants.update(controls(source))
    for name, source in variants.items():
        assert name == 'production-current' or source != source_path.read_text(), name
        c = args.out / (name + '.c'); binary = args.out / name
        c.write_text(program(source))
        command = shlex.split(args.cc) + ['-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(binary)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=30)
        (args.out / (name + '-compile.log')).write_text(built.stdout + built.stderr)
        assert built.returncode == 0, (name, built.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10, preexec_fn=no_core)
        (args.out / (name + '.log')).write_text(ran.stdout + ran.stderr)
        if name == 'production-current':
            assert ran.returncode == 0, ran.stderr
            lines = ran.stdout.splitlines()
            assert len(lines) == 704
            for bit in range(352):
                decoded = parse('\n'.join(lines[2*bit:2*bit+2]))
                assert decoded['raw_pending_ids'] == [bit], (bit, decoded)
                masked = bool(decoded['mask_words'][bit // 32] & (1 << (bit % 32)))
                assert decoded['enabled_pending_ids'] == ([] if masked else [bit]), (bit, decoded)
                assert decoded['masked_pending_ids'] == ([bit] if masked else []), (bit, decoded)
            # The retained WM2 RTC76 bit must not become AP12 or lose GFX indexing.
            assert parse('\n'.join(lines[152:154]))['raw_pending_ids'] == [76]
        else:
            assert ran.returncode == -signal.SIGABRT and 'Assertion' in ran.stderr, (name, ran.returncode, ran.stderr)
        results.append({'variant': name, 'exit_code': ran.returncode,
                        'source_sha256': hashlib.sha256(c.read_bytes()).hexdigest()})
    for invalid in ('', 'wake register snapshot unavailable',
                    'wake pending AP 00 GFX 0\nwake masks AP 00 GFX 0',
                    '\n'.join(lines[:2] + lines[:2])):
        try:
            parse(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError('missing/malformed/duplicate observation accepted')
    (args.out / 'result.json').write_text(json.dumps({'compiler': args.cc, 'variants': results,
        'all_352_global_bits_checked': True, 'negative_controls': len(results)-1,
        'input_path': str(source_path.resolve()), 'input_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(),
        'scope': 'source-extracted actual repository callbacks only; no kernel build or hardware'}, indent=2) + '\n')
    print(f'{args.cc}: actual repository callbacks, all 352 bit positions, {len(results)-1} compiled assertion controls, parser refusals PASS')


if __name__ == '__main__':
    main()
