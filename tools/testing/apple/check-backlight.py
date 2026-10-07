#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""Native replay of clean brightness fragments and actual policy/queue functions.

Run with --output on a scratch volume. This mocks kernel locks, DRM registration
and queue transport; it does not claim firmware or KUnit boot acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--cc', default='clang')
args = parser.parse_args()
root = Path(__file__).resolve().parents[3]
src = root / 'drivers/gpu/drm/apple'
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)


def function(file, name):
    text = (src / file).read_text()
    match = re.search(r'^\w[\w\s*]*?\b' + name + r'\(', text, re.M)
    assert match, name
    start = text.index('{', match.end())
    depth = 1
    pos = start + 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[match.start():pos]


include = out / 'include/linux'
include.mkdir(parents=True, exist_ok=True)
(include / 'types.h').write_text('#include <stdint.h>\n#include <stdbool.h>\ntypedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;\n')
(include / 'limits.h').write_text('#include <limits.h>\n#define U32_MAX UINT32_MAX\n')
(include / 'errno.h').write_text('#include <errno.h>\n#ifndef ENODATA\n#define ENODATA 61\n#endif\n')
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dcp_backlight.h"
#define DCP_FIRMWARE_H17P 17
#define DCP_IOMFB_METHODS_H17G 1
#define MAX_BRIGHTNESS_PART2 510U
#define SWAP_SURFACES 4
#define READ_ONCE(x) (x)
#define min(a,b) ((a)<(b)?(a):(b))
#define lockdep_assert_held(x) ((void)(x))
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
#define spin_lock_irqsave(x,f) do { (void)(x);(f)=0; } while(0)
#define spin_unlock_irqrestore(x,f) do { (void)(x);(void)(f); } while(0)
#define GENMASK_ULL(h,l) ((~UINT64_C(0) >> (63-(h))) & (~UINT64_C(0) << (l)))
#define fls(x) (32-__builtin_clz(x))
#define BACKLIGHT_PLATFORM 1
#define BACKLIGHT_SCALE_LINEAR 2
struct device { int unused; };
struct backlight_device { int unused; };
struct backlight_properties { int type;u32 brightness,max_brightness;int scale; };
struct dcp_present_h17p { u8 swap[0x588];u8 guard[32]; };
struct dcp_swap_submit_req_h17p { u8 surf_null[SWAP_SURFACES]; };
struct iomfb_transaction {
    struct dcp_backlight_present backlight;
    bool backlight_reserved,brightness_only,completed;
};
struct apple_dcp {
    int fw_compat;struct { int iomfb_method_profile; } hw;
    bool panel;void *crtc;struct device *dev;
    int bl_register_mutex,bl_register_wq;
    struct { u32 maximum;u32 nits;struct backlight_device *bl_dev;u32 dac; } brightness;
    struct { int lock;struct dcp_backlight_state state;void (*kick)(struct apple_dcp *); } backlight;
    struct { int lock;struct iomfb_transaction *active; } iomfb;
};
static int schedule_count,register_count,retire_count,dac_count;
static struct backlight_properties registered;
static const int dcp_backlight_ops;
static bool dcp_has_panel(struct apple_dcp *dcp) { return dcp->panel; }
static void schedule_work(int *work) { (void)work;schedule_count++; }
static void iomfb_backlight_kick(struct apple_dcp *dcp) { (void)dcp; }
static void iomfb_scanout_complete_h17p(struct apple_dcp *dcp) { (void)dcp;retire_count++; }
static u32 calculate_dac(struct apple_dcp *dcp,u32 nits) { (void)dcp;dac_count++;return nits; }
static struct backlight_device *devm_backlight_device_register(struct device *dev,const char *name,
    struct device *parent,struct apple_dcp *dcp,const int *ops,const struct backlight_properties *props) {
    static struct backlight_device device;
    (void)dev;(void)parent;(void)dcp;(void)ops;
    assert(!strcmp(name,"apple-panel-bl"));registered=*props;register_count++;return &device;
}
#define IS_ERR(p) (!(p))
#define PTR_ERR(p) ((void)(p),-ENOMEM)
static void put_unaligned_le64(u64 v,void *p) { for(int i=0;i<8;i++)((u8 *)p)[i]=v>>(8*i); }
static void put_unaligned_le32(u32 v,void *p) { for(int i=0;i<4;i++)((u8 *)p)[i]=v>>(8*i); }
static void put_unaligned_le16(uint16_t v,void *p) { for(int i=0;i<2;i++)((u8 *)p)[i]=v>>(8*i); }
int iomfb_configure_backlight_h17p(struct apple_dcp *,u32,bool,u32,bool,u32);
'''
functions = [
    ('iomfb_h17p.c', 'iomfb_nits_binary64_h17p'),
    ('iomfb_h17p.c', 'iomfb_encode_backlight_h17p'),
    ('iomfb.c', 'iomfb_uses_queue'),
    ('dcp_backlight.c', 'dcp_backlight_active'),
    ('dcp_backlight.c', 'dcp_backlight_configure'),
    ('iomfb.c', 'iomfb_configure_backlight_h17p'),
    ('dcp_backlight.c', 'dcp_backlight_takeover'),
    ('dcp_backlight.c', 'dcp_backlight_register'),
    ('dcp_backlight.c', 'dcp_backlight_prepare'),
    ('dcp_backlight.c', 'dcp_backlight_complete'),
    ('iomfb.c', 'iomfb_apply_backlight_h17p'),
    ('iomfb.c', 'iomfb_present_failed_h17p'),
    ('iomfb.c', 'iomfb_present_complete_h17p'),
]
main = r'''
static struct apple_dcp fixture(void) {
    return (struct apple_dcp){.fw_compat=DCP_FIRMWARE_H17P,.panel=true,
                             .crtc=(void *)1,.brightness.maximum=525};
}
static u64 level_bits(const struct dcp_present_h17p *wire) {
    u64 value=0;for(int i=0;i<8;i++)value|=(u64)wire->swap[0x35e + i]<<(i*8);return value;
}
static double level(const struct dcp_present_h17p *wire) {
    u64 bits=level_bits(wire);double value;memcpy(&value,&bits,sizeof(value));return value;
}
static void outside_unchanged(const struct dcp_present_h17p *wire) {
    const u8 *data=(const u8 *)wire;
    for(size_t i=0;i<sizeof(*wire);i++) {
        if(i>=0x32f && i<0x33c)assert(!data[i]);
        else if(i<0x354||i>=0x3ee)assert(data[i]==0xa5);
    }
}
int main(int argc,char **argv) {
    struct dcp_present_h17p wire;
    if(argc==3) {
        memset(&wire,0xa5,sizeof(wire));
        iomfb_encode_backlight_h17p(&wire,strtoul(argv[1],NULL,0),525,atoi(argv[2]));
        outside_unchanged(&wire);
        for(int i=0x354;i<0x3ee;i++)printf("%02x",wire.swap[i]);puts("");return 0;
    }
    if(argc==2) {
        for(u32 nits=0;nits<510;nits++)for(int update=0;update<2;update++) {
            memset(&wire,0xa5,sizeof(wire));
            iomfb_encode_backlight_h17p(&wire,nits,525,update);
            outside_unchanged(&wire);
            printf("%u %d ",nits,update);
            for(int i=0x354;i<0x3ee;i++)printf("%02x",wire.swap[i]);puts("");
        }
        return 0;
    }
    struct apple_dcp dcp=fixture();
    assert(dcp_backlight_register(&dcp)==-ENODATA);
    assert(!register_count);
    assert(dcp_backlight_takeover(&dcp,509001)==-ERANGE);
    assert(!dcp_backlight_active(&dcp));
    assert(dcp_backlight_takeover(&dcp,100000)==0);
    assert(dcp.backlight.state.target==100 && dcp.backlight.state.actual==100);
    assert(!dcp.backlight.state.dirty && schedule_count==1);
    assert(dcp_backlight_takeover(&dcp,400000)==0);
    assert(dcp_backlight_takeover(&dcp,0)==0);
    assert(dcp.backlight.state.target==100 && schedule_count==1);
    assert(dcp_backlight_register(&dcp)==0 && register_count==1);
    assert(registered.brightness==100 && registered.max_brightness==509 && !dac_count);
    assert(dcp.backlight.state.controlled);
    struct iomfb_transaction transaction={.brightness_only=true};
    struct dcp_swap_submit_req_h17p request={.surf_null={0,1,1,1}};
    dcp.iomfb.active=&transaction;
    struct apple_dcp unready=fixture();
    memset(&wire,0xa5,sizeof(wire));
    assert(iomfb_apply_backlight_h17p(&unready,&request,&wire));
    assert(!wire.swap[0x354] && !level_bits(&wire));
    assert(wire.swap[0x374] || wire.swap[0x375]);
    unready.brightness.maximum=0;
    memset(&wire,0xa5,sizeof(wire));
    assert(!iomfb_apply_backlight_h17p(&unready,&request,&wire));
    assert(wire.swap[0x354]==0xa5);
    dcp.brightness.maximum=0;
    assert(!iomfb_apply_backlight_h17p(&dcp,&request,&wire));
    dcp.brightness.maximum=525;
    memset(&wire,0xa5,sizeof(wire));
    assert(iomfb_apply_backlight_h17p(&dcp,&request,&wire));
    for(int i=0x354;i<0x366;i++)assert(!wire.swap[i]);
    assert(!transaction.backlight_reserved);
    assert(dcp_bl_request(&dcp.backlight.state,400,false,false)==0);
    assert(iomfb_apply_backlight_h17p(&dcp,&request,&wire));
    assert(level(&wire)==400 && dcp.backlight.state.actual==100);
    assert(transaction.backlight_reserved);
    assert(!dcp_backlight_complete(&dcp,transaction.backlight.sequence+1,true));
    assert(dcp_bl_request(&dcp.backlight.state,140,false,false)==0);
    assert(!iomfb_present_complete_h17p(&dcp));
    assert(dcp.backlight.state.actual==400 && dcp.backlight.state.dirty && !retire_count);
    assert(iomfb_apply_backlight_h17p(&dcp,&request,&wire));
    assert(level(&wire)==140);
    iomfb_present_failed_h17p(&dcp);
    assert(dcp.backlight.state.actual==400 && dcp.backlight.state.dirty);
    assert(iomfb_apply_backlight_h17p(&dcp,&request,&wire));
    assert(!iomfb_present_complete_h17p(&dcp));
    assert(dcp.backlight.state.actual==140 && !retire_count);
    for(int i=0;i<20;i++) {
        assert(dcp_bl_dpms(&dcp.backlight.state,false)==0);
        assert(iomfb_apply_backlight_h17p(&dcp,&request,&wire));
        assert(level(&wire)==0 && wire.swap[0x354]==1);
        assert(!iomfb_present_complete_h17p(&dcp));
        assert(dcp.backlight.state.actual==0 && dcp.backlight.state.target==140);
        assert(dcp_bl_dpms(&dcp.backlight.state,true)==0);
        assert(iomfb_apply_backlight_h17p(&dcp,&request,&wire));
        assert(level(&wire)==140);
        assert(!iomfb_present_complete_h17p(&dcp));
    }
    assert(!retire_count);
    assert(dcp_bl_dpms(&dcp.backlight.state,false)==0);
    assert(dcp_bl_request(&dcp.backlight.state,400,false,false)==0);
    assert(iomfb_apply_backlight_h17p(&dcp,&request,&wire));
    assert(level(&wire)==0 && dcp.backlight.state.target==400);
    assert(!iomfb_present_complete_h17p(&dcp));
    assert(dcp_bl_dpms(&dcp.backlight.state,true)==0);
    assert(iomfb_apply_backlight_h17p(&dcp,&request,&wire));
    assert(level(&wire)==400);
    assert(!iomfb_present_complete_h17p(&dcp));
    assert(dcp_bl_request(&dcp.backlight.state,36,false,false)==0);
    memset(request.surf_null,1,sizeof(request.surf_null));
    memset(&wire,0xa5,sizeof(wire));
    assert(!iomfb_apply_backlight_h17p(&dcp,&request,&wire));
    assert(wire.swap[0x354]==0xa5 && dcp.backlight.state.dirty);
    struct apple_dcp h17g=fixture();h17g.hw.iomfb_method_profile=DCP_IOMFB_METHODS_H17G;
    assert(dcp_backlight_takeover(&h17g,100000)==-EINVAL);
    assert(!h17g.backlight.state.ready);
    struct apple_dcp external=fixture();external.panel=false;
    assert(dcp_backlight_takeover(&external,100000)==-EINVAL);
    struct apple_dcp unlinked=fixture();unlinked.crtc=NULL;
    assert(dcp_backlight_takeover(&unlinked,100000)==-EINVAL);
    struct apple_dcp zero=fixture();
    assert(dcp_backlight_takeover(&zero,0)==0);
    assert(zero.backlight.state.target==0 && !zero.backlight.state.dirty);
    puts("Actual takeover/registration/reservation/accepted completion/rejection/20 DPMS cycles passed");
    return 0;
}
'''
source = prelude + '\n'.join(function(f, n) for f, n in functions) + main
(out / 'fixture.c').write_text(source)
argv = [args.cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
        '-I' + str(out / 'include'), '-I' + str(src), str(out / 'fixture.c'), '-o', str(out / 'fixture')]
build = subprocess.run(argv, text=True, capture_output=True)
(out / 'compile.log').write_text(build.stdout + build.stderr)
assert not build.returncode, build.stderr
run = subprocess.run([str(out / 'fixture')], text=True, capture_output=True)
(out / 'run.log').write_text(run.stdout + run.stderr)
assert not run.returncode, run.stdout + run.stderr
# All measured brightness fragments, including DPMS zero and ordinary startup.
golden = json.loads(Path(__file__).with_name('backlight-golden.json').read_text())
for case in golden:
    fragment = subprocess.check_output([str(out / 'fixture'), str(case['nits']), str(int(case['enabled']))], text=True).strip()
    assert fragment == case['fragment'], (case, fragment)
# Exact integer binary64 throughout the exposed range, plus disabled updates.
import struct
range_output = subprocess.check_output([str(out / 'fixture'), 'range'], text=True)
for line in range_output.splitlines():
    n, enabled, encoded = line.split()
    nits, enabled = int(n), bool(int(enabled))
    expected = bytearray(0x9a)
    expected[0x1a:0x22] = expected[0x22:0x2a] = struct.pack('<d', 525)
    if enabled:
        expected[:10] = bytes.fromhex('01000000010101000100')
        expected[10:18] = struct.pack('<d', nits)
        expected[0x2a:0x32] = expected[0x92:0x9a] = struct.pack('<d', 1)
    assert bytes.fromhex(encoded) == expected
assert len(range_output.splitlines()) == 1020
result = {'rc': 0, 'compile': argv, 'golden_fragments': len(golden), 'range_cases': 1020,
          'actual_functions': [n for f, n in functions], 'stdout': run.stdout,
          'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
          'limits': 'Native ASan/UBSan fixtures; locks, DRM registration, scheduling and transport mocked; no firmware acceptance.'}
(out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(run.stdout, end='')
print(f"{len(golden)} captured fragments and 1020 full-range enabled/disabled cases passed")
