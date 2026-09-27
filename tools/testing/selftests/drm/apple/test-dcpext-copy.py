#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check full-frame conversion, padded source pitch and retained front pixels."""
from pathlib import Path
import os
import subprocess
import tempfile

base = Path(__file__).resolve().parents[5] / "drivers/gpu/drm/apple"
source = (base / "dcpext_drm.c").read_text()


def function(start, end):
    return source[source.index(start):source.index(end)]


preamble = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dcpext_mode.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define EXT_WIDTH DCPEXT_WIDTH
#define EXT_HEIGHT DCPEXT_HEIGHT
#define EXT_STRIDE DCPEXT_STRIDE
struct iosys_map { void *vaddr; };
struct drm_framebuffer { u32 pitches[1]; };
struct drm_plane_state { struct drm_framebuffer *fb; };
struct drm_shadow_plane_state { struct drm_plane_state base; struct iosys_map data[1]; };
#define to_drm_shadow_plane_state(p) ((struct drm_shadow_plane_state *)(p))
struct dcpext_drm { void *dcp; u8 *row; u32 stride; };
struct dcpext_frame { void *pixels; size_t size; };
static struct dcpext_frame back;
static bool begin_fails, held;
static unsigned presented, discarded;
static int dcpext_scanout_begin_frame(void *dcp, struct dcpext_frame *frame) {
    (void)dcp;
    if(begin_fails) return -ENOLINK;
    assert(!held); held=true; *frame=back; return 0;
}
static int dcpext_scanout_end_frame(void *dcp, bool present) {
    (void)dcp; assert(held); held=false;
    if(present) presented++; else discarded++;
    return 0;
}
static void iosys_map_memcpy_from(void *dst, const struct iosys_map *src,
                                size_t offset, size_t size) {
    memcpy(dst,(const u8 *)src->vaddr+offset,size);
}
'''
tests = r'''
int main(void) {
    const size_t size=(size_t)EXT_STRIDE*EXT_HEIGHT;
    const u32 pitch=EXT_STRIDE+64;
    u8 *source=malloc((size_t)pitch*EXT_HEIGHT);
    u8 *buffers=malloc(size*2);
    assert(source && buffers);
    memset(buffers,0xab,size*2);
    for(unsigned y=0;y<EXT_HEIGHT;y++)
        for(unsigned x=0;x<pitch;x++) source[(size_t)y*pitch+x]=(x*13+y*7)&255;
    back=(struct dcpext_frame){buffers+size,size};
    struct dcpext_drm ext={.row=malloc(EXT_STRIDE),.stride=EXT_STRIDE};
    assert(ext.row);
    struct drm_framebuffer fb={.pitches={pitch}};
    struct drm_shadow_plane_state shadow={.base={.fb=&fb},.data={{source}}};
    assert(!ext_copy_frame(&ext,&shadow.base) && presented==1 && !held);
    for(size_t i=0;i<size;i++) assert(buffers[i]==0xab);
    for(unsigned y=0;y<EXT_HEIGHT;y++)
        for(unsigned x=0;x<EXT_STRIDE;x++)
            assert(buffers[size+(size_t)y*EXT_STRIDE+x]==
                   (x%4==3 ? 255 : source[(size_t)y*pitch+x]));
    begin_fails=true;
    assert(ext_copy_frame(&ext,&shadow.base)==-ENOLINK && presented==1);
    begin_fails=false; back.size--;
    assert(ext_copy_frame(&ext,&shadow.base)==-EINVAL && presented==1 && discarded==1 && !held);
    free(ext.row); free(source); free(buffers);
    puts("PASS: actual full 4K copy preserves front buffer, handles padded pitch, makes alpha opaque and rejects failures");
}
'''
body = preamble
body += function("static bool ext_rect_valid(", "static int ext_plane_check(")
body += function("static int ext_copy_frame(", "static void ext_plane_update(")
with tempfile.TemporaryDirectory(prefix="dcpext-copy-test-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(body + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(base), "-o", str(path / "test"), str(path / "test.c")], check=True)
    subprocess.run([str(path / "test")], check=True)
