#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile and exercise the actual ibootep.c wire helpers, no hardware access."""
from pathlib import Path
import os
import subprocess
import tempfile

base = Path(__file__).resolve().parents[5] / 'drivers/gpu/drm/apple'
src = (base / 'ibootep.c').read_text()
helpers = src[src.index('static int iboot_build_query('):src.index('static int iboot_read_query(')]
layouts = src[src.index('struct iboot_plane {'):src.index('struct iboot_query {')]
prefix = r'''
#include <assert.h>
#include "dcpext_mode.h"
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint32_t __le32;
typedef uint64_t __le64;
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)
#define __packed __attribute__((packed))
#define static_assert _Static_assert
#define IBOOT_QUERY_RX_SIZE 0x4000
#define IBOOT_MODE_SIZE 24
#define IBOOT_PATTERN_WIDTH 3840
#define IBOOT_PATTERN_HEIGHT 2160
#define IBOOT_PATTERN_STRIDE 15360
enum { IBOOT_GET_HPD=3, IBOOT_GET_TIMING_MODES=4, IBOOT_GET_COLOR_MODES=5 };
static u32 get_unaligned_le32(const void *p_) {
    const u8 *p = p_;
    return p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24);
}
static void put32(u8 *p, u32 x) {
    for (unsigned i=0; i<4; i++) p[i] = x >> (i*8);
}
#define put_unaligned_le32(v,p) put32(p,v)
'''
tests = r'''
int main(void) {
    __le32 req[4];
    for (u32 op=0; op<20; op++) {
        memset(req, 0xaa, sizeof(req));
        int ret=iboot_build_query(op, req);
        if (op<3 || op>5) { assert(ret==-EINVAL); continue; }
        assert(ret==0 && req[0]==op && req[1]==16 && req[2]==0 && req[3]==0);
    }
    u8 reply[IBOOT_QUERY_RX_SIZE] = {0};
    const u8 *p;
    size_t n;
    put32(reply, 3); put32(reply+4, 20);
    assert(!iboot_reply_payload(reply, sizeof(reply), 3, &p, &n));
    assert(p==reply+8 && n==12);
    assert(iboot_reply_payload(reply, 7, 3, &p, &n)==-EPROTO);
    assert(iboot_reply_payload(reply, sizeof(reply), 4, &p, &n)==-EPROTO);
    put32(reply+4, 7);
    assert(iboot_reply_payload(reply, sizeof(reply), 3, &p, &n)==-EPROTO);
    put32(reply+4, sizeof(reply)+1);
    assert(iboot_reply_payload(reply, sizeof(reply), 3, &p, &n)==-EPROTO);
    put32(reply+4, 20);
    assert(!iboot_reply_payload(reply, sizeof(reply), 3, &p, &n));
    bool hpd; u32 nt, nc;
    assert(!iboot_parse_hpd(p,n,&hpd,&nt,&nc) && !hpd && !nt && !nc);
    reply[8]=1; put32(reply+12, 4); put32(reply+16, 7);
    assert(!iboot_parse_hpd(p,n,&hpd,&nt,&nc) && hpd && nt==4 && nc==7);
    assert(iboot_parse_hpd(p,11,&hpd,&nt,&nc)==-EPROTO);
    reply[8]=2;
    assert(iboot_parse_hpd(p,n,&hpd,&nt,&nc)==-EPROTO);
    reply[8]=1; put32(reply+12, UINT32_MAX);
    assert(iboot_parse_hpd(p,n,&hpd,&nt,&nc)==-EOVERFLOW);
    memset(reply, 0, sizeof(reply));
    put32(reply, 4); put32(reply+4, 8+4+2*24); put32(reply+8, 2);
    put32(reply+12, 1); put32(reply+16, 3840); put32(reply+20, 2160);
    put32(reply+24, 60U<<16);
    assert(!iboot_reply_payload(reply, sizeof(reply), 4, &p, &n));
    u32 count;
    assert(!iboot_parse_modes(p,n,&count) && count==2);
    assert(iboot_parse_modes(p,3,&count)==-EPROTO);
    assert(iboot_parse_modes(p,n-1,&count)==-EPROTO);
    put32(reply+12, 2);
    assert(iboot_parse_modes(p,n,&count)==-EPROTO);
    put32(reply+12, 1); put32(reply+8, UINT32_MAX);
    assert(iboot_parse_modes(p,n,&count)==-EPROTO);
    put32(reply+8, 0);
    assert(!iboot_parse_modes(p,4,&count) && count==0);
    /* Every bounded length/count pair: no helper may read past a declared list. */
    for (size_t size=0; size<200; size++) {
        for (u32 c=0; c<16; c++) {
            memset(reply,0,sizeof(reply)); put32(reply,c);
            int r=iboot_parse_modes(reply,size,&count);
            assert((r==0)==(size>=4 && c<=(size-4)/24));
        }
    }
    u8 mode[24]={0};
    put32(mode,1); put32(mode+4,3840); put32(mode+8,2160); put32(mode+12,60U<<16);
    assert(iboot_pattern_mode(4,mode));
    put32(mode+4,1920);put32(mode+8,1080);assert(!iboot_pattern_mode(4,mode));
    put32(mode+4,3840);put32(mode+8,2160);
    put32(mode+12,(60U<<16)-1);
    assert(!iboot_pattern_mode(4,mode));
    put32(mode+4,1); put32(mode+8,1); put32(mode+12,1); put32(mode+16,32);
    assert(iboot_pattern_mode(5,mode));
    put32(mode+16,30); assert(!iboot_pattern_mode(5,mode));
    put32(mode+16,32); put32(mode,0); assert(!iboot_pattern_mode(5,mode));
    assert(!iboot_pattern_mode(6,mode));
    u64 iova=0x10100100000ULL;
    size_t fb_size=15360*2160;
    assert(!iboot_pattern_params(iova,fb_size,15360));
    assert(iboot_pattern_params(0,fb_size,15360)==-EINVAL);
    assert(iboot_pattern_params(iova+1,fb_size,15360)==-EINVAL);
    assert(iboot_pattern_params(iova,fb_size-1,15360)==-EINVAL);
    assert(iboot_pattern_params(iova,fb_size,15364)==-EINVAL);
    assert(iboot_pattern_params(iova,65U*1024*1024,15360)==-EINVAL);
    assert(iboot_pattern_params((1ULL<<42)-0x4000,fb_size,15360)==-EINVAL);
    assert(iboot_pattern_params(UINT64_MAX,fb_size,15360)==-EINVAL);
    struct iboot_swap_layer_v13_3 layer;
    iboot_build_pattern_layer(&layer,iova,15360);
    assert(sizeof(layer)==216);
    assert(offsetof(struct iboot_swap_layer_v13_3,src)==180);
    assert(offsetof(struct iboot_swap_layer_v13_3,dst)==196);
    u8 expected[216]={0};
    /* Independent byte fixture: upstream m1n1 >=13.3, opaque linear BGRA. */
    put32(expected+12,(u32)iova); put32(expected+16,(u32)(iova>>32));
    put32(expected+24,15360); put32(expected+44,1);
    put32(expected+144,1); put32(expected+148,3840); put32(expected+152,2160);
    put32(expected+156,1); put32(expected+160,1); put32(expected+164,1);
    put32(expected+180,3840); put32(expected+184,2160);
    put32(expected+196,3840); put32(expected+200,2160);
    assert(!memcmp(&layer,expected,sizeof(expected)));
    u8 request[232], power=1, modes[48]={0}, end[12]={0};
    size_t request_size;
    assert(!iboot_build_pattern_request(2,&power,1,request,sizeof(request),&request_size));
    assert(request_size==17 && request[0]==2 && request[4]==17 && request[16]==1);
    assert(!iboot_build_pattern_request(6,modes,48,request,sizeof(request),&request_size));
    assert(request_size==64);
    assert(!iboot_build_pattern_request(15,NULL,0,request,sizeof(request),&request_size));
    assert(request_size==16);
    assert(!iboot_build_pattern_request(16,&layer,sizeof(layer),request,sizeof(request),&request_size));
    assert(request_size==232 && request[0]==16 && request[4]==232);
    assert(!memcmp(request+16,expected,216));
    for (size_t i=8;i<16;i++) assert(request[i]==0);
    assert(!iboot_build_pattern_request(18,end,12,request,sizeof(request),&request_size));
    assert(request_size==28);
    __le32 wait[4] = {0, 123, 1, 0};
    assert(!iboot_build_pattern_request(19,wait,sizeof(wait),request,sizeof(request),&request_size));
    assert(request_size==32 && request[0]==19 && request[4]==32);
    assert(get_unaligned_le32(request+16)==0 && get_unaligned_le32(request+20)==123);
    assert(get_unaligned_le32(request+24)==1 && get_unaligned_le32(request+28)==0);
    assert(iboot_build_pattern_request(19,wait,12,request,sizeof(request),&request_size)==-EINVAL);
    assert(iboot_build_pattern_request(1,&layer,sizeof(layer),request,sizeof(request),&request_size)==-EINVAL);
    assert(iboot_build_pattern_request(16,&layer,sizeof(layer)-8,request,sizeof(request),&request_size)==-EINVAL);
    assert(iboot_build_pattern_request(16,&layer,sizeof(layer),request,sizeof(request)-1,&request_size)==-EINVAL);
    assert(iboot_build_pattern_request(2,NULL,1,request,sizeof(request),&request_size)==-EINVAL);
    puts("PASS: actual iBoot query/pattern helpers, >=13.3 layer bytes, bounds and malformed replies");
}
'''
with tempfile.TemporaryDirectory(prefix='iboot-query-test-') as path:
    path=Path(path)
    (path/'test.c').write_text(prefix+layouts+helpers+tests)
    subprocess.run([os.environ.get('CC','cc'), '-Wall', '-Wextra', '-Werror', '-I', str(base),
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-o', str(path/'test'), str(path/'test.c')],check=True)
    subprocess.run([str(path/'test')],check=True)
