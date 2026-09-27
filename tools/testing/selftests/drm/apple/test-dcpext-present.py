#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the actual presentation and buffer-ownership code with host stubs."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

base = Path(__file__).resolve().parents[5] / "drivers/gpu/drm/apple"
iboot = (base / "ibootep.c").read_text()
scanout = (base / "dcpext_scanout.c").read_text()


def function(source, name):
    match = re.search(r"^(?:static )?(?:int|bool) " + name + r"\(", source, re.M)
    assert match, name
    start = source.index("{", match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "dcpext_mode.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef u32 __le32;
typedef u64 __le64;
#define __packed __attribute__((packed))
#define static_assert _Static_assert
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)
#define READ_ONCE(x) (x)
#define smp_load_acquire(p) (*(p))
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define IBOOT_QUERY_RX_SIZE 0x4000
#define IBOOT_MODE_SIZE 24
#define IBOOT_PATTERN_WIDTH DCPEXT_WIDTH
#define IBOOT_PATTERN_HEIGHT DCPEXT_HEIGHT
#define IBOOT_PATTERN_STRIDE DCPEXT_STRIDE
#define SCANOUT_STRIDE DCPEXT_STRIDE
#define IBOOT_GET_HPD 3
#define IBOOT_GET_TIMING_MODES 4
#define IBOOT_GET_COLOR_MODES 5
#define EPIC_SUBTYPE_STD_SERVICE 0x20
#define DCP_FIRMWARE_V_14_7 147
#define GFP_KERNEL 0
#define IS_ERR(p) ((intptr_t)(p) < 0)
#define PTR_ERR(p) ((intptr_t)(p))
#define might_sleep() do {} while(0)
#define dev_err(...) do {} while(0)
#define dma_wmb() do {} while(0)
#define max(a,b) ((a)>(b)?(a):(b))
static bool alloc_fail;
static unsigned live;
static void *kzalloc(size_t n, int flags) {
    (void)flags;
    if (alloc_fail) return NULL;
    void *p = calloc(1,n); assert(p); live++; return p;
}
static void kfree(void *p) { if(p) live--; free(p); }
static int atomic_cmpxchg(int *p, int old, int new) {
    int value=*p; if(value==old) *p=new; return value;
}
#define atomic_set_release(p,v) (*(p)=(v))
static u32 get_unaligned_le32(const void *p) { u32 v; memcpy(&v,p,4); return v; }
static void put_unaligned_le32(u32 v, void *p) { memcpy(p,&v,4); }
struct dcpext_scanout;
struct apple_dcp { int fw_compat; struct dcpext_scanout *dcpext_scanout; };
struct apple_epic_service { bool torndown; };
struct iboot_query {
    struct apple_epic_service *service;
    int busy;
    bool stopping, pattern_requested, presentation_failed;
    u32 last_frame_swap;
};
static struct apple_epic_service service;
static struct iboot_query query;
static struct iboot_query *iboot_find_query(struct apple_dcp *dcp) {
    (void)dcp; return &query;
}
static unsigned calls, fail_step, duplicate_id, short_reply, remote_error;
static u32 returned_id;
static u64 submitted_iova;
static int afk_send_command_with_reply_len(struct apple_epic_service *s, u8 type,
    const void *request, size_t size, void *output, size_t capacity,
    u32 *retcode, size_t *received) {
    const unsigned sequence[]={15,16,18,19};
    const u8 *wire=request;
    unsigned op=get_unaligned_le32(wire);
    assert(s==&service && type==EPIC_SUBTYPE_STD_SERVICE);
    assert(calls<4 && op==sequence[calls++]);
    assert(get_unaligned_le32(wire+4)==size && capacity==IBOOT_QUERY_RX_SIZE);
    if(op==fail_step) return -ETIMEDOUT;
    *retcode=remote_error && op==19 ? 1 : 0;
    *received=0;
    memset(output,0,capacity);
    if(op==15) {
        returned_id=duplicate_id ? query.last_frame_swap : query.last_frame_swap+1;
        put_unaligned_le32(15,output);
        put_unaligned_le32(28,(u8 *)output+4);
        put_unaligned_le32(returned_id,(u8 *)output+20);
        *received=short_reply ? 27 : 28;
    } else if(op==16) {
        assert(size==232);
        memcpy(&submitted_iova,wire+28,8);
    } else if(op==18) {
        assert(size==28);
        for(unsigned i=16;i<28;i++) assert(wire[i]==0);
    } else {
        assert(size==32 && get_unaligned_le32(wire+16)==0);
        assert(get_unaligned_le32(wire+20)==returned_id);
        assert(get_unaligned_le32(wire+24)==1 && get_unaligned_le32(wire+28)==0);
    }
    return 0;
}
struct dcpext_frame { void *pixels; size_t size; };
struct dcpext_scanout {
    bool pageflips, stopping, pattern_ready, link;
    int present_lock, terminal_error;
    void *pixels;
    size_t frame_size;
    unsigned front;
    u64 iova, frames_completed, present_ns_max;
};
static void mutex_lock(int *p) { assert(!*p); *p=1; }
static void mutex_unlock(int *p) { assert(*p); *p=0; }
#define lockdep_assert_held(p) assert(*(p))
static bool scanout_link_ready(struct dcpext_scanout *s) { return s->link && !s->terminal_error; }
static void scanout_fail(struct dcpext_scanout *s, int error) { s->terminal_error=error; }
static u64 ktime_get_ns(void) { static u64 clock; return ++clock; }
'''

tests = r'''
static void reset(void) {
    assert(!live);
    query=(struct iboot_query){.service=&service,.pattern_requested=true};
    calls=fail_step=duplicate_id=short_reply=remote_error=0;
    alloc_fail=false;
    service.torndown=false;
}
int main(void) {
    struct apple_dcp dcp={.fw_compat=DCP_FIRMWARE_V_14_7};
    const u64 address=0x10000000000ULL;
    const size_t bytes=(size_t)DCPEXT_STRIDE*DCPEXT_HEIGHT;
    reset();
    assert(!ibootep_present_frame(&dcp,address,bytes,DCPEXT_STRIDE));
    assert(calls==4 && query.last_frame_swap==1 && !query.busy && !live);
    assert(submitted_iova==address);
    calls=0;
    assert(!ibootep_present_frame(&dcp,address+bytes,bytes,DCPEXT_STRIDE));
    assert(calls==4 && query.last_frame_swap==2 && submitted_iova==address+bytes);
    unsigned steps[]={15,16,18,19};
    for(unsigned i=0;i<4;i++) {
        reset(); fail_step=steps[i];
        assert(ibootep_present_frame(&dcp,address,bytes,DCPEXT_STRIDE)==-ETIMEDOUT);
        assert(calls==i+1 && query.presentation_failed && !query.last_frame_swap);
        assert(!query.busy && !live);
        assert(ibootep_present_frame(&dcp,address,bytes,DCPEXT_STRIDE)==-EIO);
        assert(calls==i+1);
    }
    reset(); short_reply=1;
    assert(ibootep_present_frame(&dcp,address,bytes,DCPEXT_STRIDE)==-EPROTO);
    assert(calls==1 && query.presentation_failed);
    reset(); duplicate_id=1; query.last_frame_swap=10;
    assert(ibootep_present_frame(&dcp,address,bytes,DCPEXT_STRIDE)==-EPROTO);
    assert(calls==1 && query.last_frame_swap==10);
    reset(); remote_error=1;
    assert(ibootep_present_frame(&dcp,address,bytes,DCPEXT_STRIDE)==-EIO);
    assert(calls==4 && query.presentation_failed && !query.last_frame_swap);
    reset(); query.busy=1;
    assert(ibootep_present_frame(&dcp,address,bytes,DCPEXT_STRIDE)==-EBUSY);
    assert(!calls && query.busy);
    reset(); alloc_fail=true;
    assert(ibootep_present_frame(&dcp,address,bytes,DCPEXT_STRIDE)==-ENOMEM);
    assert(!calls && !query.busy && !query.presentation_failed);
    reset(); query.stopping=true;
    assert(ibootep_present_frame(&dcp,address,bytes,DCPEXT_STRIDE)==-ENODEV);
    assert(!calls && query.presentation_failed);
    reset();
    struct dcpext_scanout scan={.pageflips=true,.pattern_ready=true,.link=true,
        .pixels=calloc(2,bytes),.frame_size=bytes,.iova=address};
    assert(scan.pixels);
    dcp.dcpext_scanout=&scan;
    struct dcpext_frame frame;
    assert(!dcpext_scanout_begin_frame(&dcp,&frame));
    assert(frame.pixels==(u8 *)scan.pixels+bytes && frame.size==bytes && scan.present_lock);
    assert(!dcpext_scanout_end_frame(&dcp,false));
    assert(!calls && !scan.front && !scan.present_lock);
    for(unsigned i=0;i<6;i++) {
        calls=0;
        assert(!dcpext_scanout_begin_frame(&dcp,&frame));
        assert(frame.pixels==(u8 *)scan.pixels+(i%2 ? 0 : bytes));
        assert(!dcpext_scanout_end_frame(&dcp,true));
        assert(calls==4 && scan.front==(i%2 ? 0 : 1));
        assert(submitted_iova==address+scan.front*bytes && scan.frames_completed==i+1);
    }
    calls=0; fail_step=19;
    assert(!dcpext_scanout_begin_frame(&dcp,&frame));
    assert(dcpext_scanout_end_frame(&dcp,true)==-ETIMEDOUT);
    assert(scan.front==0 && scan.frames_completed==6 && scan.terminal_error==-EIO);
    assert(!scan.present_lock && !live);
    assert(dcpext_scanout_begin_frame(&dcp,&frame)==-ENOLINK);
    assert(calls==4 && !scan.present_lock);
    scan.terminal_error=0; scan.link=false;
    assert(dcpext_scanout_begin_frame(&dcp,&frame)==-ENOLINK);
    scan.link=true; scan.stopping=true;
    assert(dcpext_scanout_begin_frame(&dcp,&frame)==-ENOLINK);
    free(scan.pixels);
    puts("PASS: actual frame sequence, exact swap wait, malformed/failed replies, no retry, buffer reuse only after completion");
}
'''

layouts = iboot[iboot.index("struct iboot_plane {"):iboot.index("struct iboot_query {")]
helpers = iboot[iboot.index("static int iboot_build_query("):iboot.index("static int iboot_read_query(")]
body = preamble + layouts + helpers
body += function(iboot, "iboot_frame_command") + function(iboot, "ibootep_present_frame")
body += function(scanout, "dcpext_scanout_begin_frame") + function(scanout, "dcpext_scanout_end_frame")
with tempfile.TemporaryDirectory(prefix="dcpext-present-test-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(body + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I", str(base),
                    "-o", str(path / "test"), str(path / "test.c")], check=True)
    subprocess.run([str(path / "test")], check=True)
