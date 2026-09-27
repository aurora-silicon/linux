#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise actual two-buffer copy/history code against complete reference frames."""
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
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
struct drm_rect { int x1,y1,x2,y2; };
#define DRM_RECT_INIT(x,y,w,h) {x,y,(x)+(w),(y)+(h)}
static bool drm_rect_visible(const struct drm_rect *r) { return r->x1<r->x2 && r->y1<r->y2; }
static int drm_rect_width(const struct drm_rect *r) { return r->x2-r->x1; }
static bool drm_rect_intersect(struct drm_rect *a,const struct drm_rect *b) {
    a->x1=max(a->x1,b->x1);a->y1=max(a->y1,b->y1);
    a->x2=min(a->x2,b->x2);a->y2=min(a->y2,b->y2);return drm_rect_visible(a);
}
struct iosys_map { void *vaddr; };
struct drm_framebuffer { u32 pitches[1]; };
struct drm_plane_state { struct drm_framebuffer *fb; struct drm_rect damage; bool full; };
struct drm_shadow_plane_state { struct drm_plane_state base; struct iosys_map data[1]; };
#define to_drm_shadow_plane_state(p) ((struct drm_shadow_plane_state *)(p))
/* The core helper supplies clipped/merged damage, or full damage when absent. */
static bool drm_atomic_helper_damage_merged(const struct drm_plane_state *old,
                                           const struct drm_plane_state *ps,struct drm_rect *out) {
    (void)old;*out=ps->full?(struct drm_rect)DRM_RECT_INIT(0,0,EXT_WIDTH,EXT_HEIGHT):ps->damage;
    return drm_rect_visible(out);
}
struct dcpext_drm { void *dcp; u8 *row; u32 stride; struct drm_rect previous_damage; bool damage_valid; };
struct dcpext_frame { void *pixels; size_t size; };
static struct dcpext_frame back;
static bool begin_fails, end_fails, held;
static unsigned presented, discarded;
static size_t copied;
static int dcpext_scanout_begin_frame(void *dcp, struct dcpext_frame *frame) {
    (void)dcp;
    if(begin_fails) return -ENOLINK;
    assert(!held); held=true; *frame=back; return 0;
}
static int dcpext_scanout_end_frame(void *dcp, bool present) {
    (void)dcp; assert(held); held=false;
    if(end_fails)return -EIO;
    if(present) presented++; else discarded++;
    return 0;
}
static void iosys_map_memcpy_from(void *dst, const struct iosys_map *src,
                                size_t offset, size_t size) {
    memcpy(dst,(const u8 *)src->vaddr+offset,size);copied+=size;
}
'''
tests = r'''
static void paint(u8 *source,u8 *reference,u32 pitch,struct drm_rect r,unsigned seed) {
    struct drm_rect bounds=DRM_RECT_INIT(0,0,EXT_WIDTH,EXT_HEIGHT);
    if(!drm_rect_intersect(&r,&bounds))return;
    for(int y=r.y1;y<r.y2;y++)for(int x=r.x1;x<r.x2;x++)for(int c=0;c<4;c++) {
        u8 value=(x*13+y*7+c*31+seed*19)&255;
        source[(size_t)y*pitch+x*4+c]=value;
        reference[(size_t)y*EXT_STRIDE+x*4+c]=c==3?255:value;
    }
}
int main(void) {
    const size_t size=(size_t)EXT_STRIDE*EXT_HEIGHT;
    const u32 pitch=EXT_STRIDE+64;
    u8 *source=malloc((size_t)pitch*EXT_HEIGHT),*buffers=malloc(size*2);
    u8 *reference=malloc(size),*saved_front=malloc(size);
    assert(source && buffers && reference && saved_front);
    memset(source,0x5c,(size_t)pitch*EXT_HEIGHT);memset(buffers,0xab,size*2);
    struct drm_rect full=DRM_RECT_INIT(0,0,EXT_WIDTH,EXT_HEIGHT);
    paint(source,reference,pitch,full,0);
    struct dcpext_drm ext={.row=malloc(EXT_STRIDE),.stride=EXT_STRIDE};assert(ext.row);
    struct drm_framebuffer fb={.pitches={pitch}};
    struct drm_shadow_plane_state shadow={.base={.fb=&fb},.data={{source}}};
    unsigned front=0;
    for(unsigned step=0;step<48;step++) {
        /* Disjoint, overlapping, offscreen, empty and missing damage. */
        struct drm_rect d=DRM_RECT_INIT(100+(int)(step%3)*64,100+(int)(step%4)*32,32,24);
        if(step==12)d=(struct drm_rect)DRM_RECT_INIT(3800,2130,80,60);
        if(step==13)d=(struct drm_rect)DRM_RECT_INIT(-20,-10,40,30);
        if(step==14 || step==15)d=(struct drm_rect){0};
        if(step==16)d=(struct drm_rect)DRM_RECT_INIT(4000,2300,10,10);
        shadow.base.full=step==20;shadow.base.damage=d;
        paint(source,reference,pitch,shadow.base.full?full:d,step);
        /* A disable or lost history must reseed both idle buffers. */
        if(step==25){memset(buffers,0x19,size*2);ext.damage_valid=false;}
        memcpy(saved_front,buffers+front*size,size);
        back=(struct dcpext_frame){buffers+(front^1)*size,size};copied=0;
        assert(!ext_copy_frame(&ext,&shadow.base,&shadow.base) && !held);
        assert(!memcmp(saved_front,buffers+front*size,size));
        assert(!memcmp(reference,back.pixels,size));
        if(step<2 || step==20 || step==21 || step==25 || step==26)assert(copied==size);
        if(step>=2 && step<12)assert(copied<size/100);
        if(step==15 || step==16)assert(copied==0);
        front^=1;
    }
    assert(presented==48);
    struct drm_rect history=ext.previous_damage;
    memcpy(saved_front,buffers+front*size,size);
    back=(struct dcpext_frame){buffers+(front^1)*size,size};copied=0;begin_fails=true;
    assert(ext_copy_frame(&ext,&shadow.base,&shadow.base)==-ENOLINK && !copied && !held);
    begin_fails=false;back.size--;
    assert(ext_copy_frame(&ext,&shadow.base,&shadow.base)==-EINVAL && discarded==1 && !held);
    back.size++;end_fails=true;
    shadow.base.full=true;
    assert(ext_copy_frame(&ext,&shadow.base,&shadow.base)==-EIO && presented==48 && !held);
    assert(!memcmp(&history,&ext.previous_damage,sizeof(history)));
    assert(!memcmp(saved_front,buffers+front*size,size));
    free(ext.row);free(source);free(buffers);free(reference);free(saved_front);
    puts("PASS: 48 full-reference frames, two-frame damage history, front preservation, opaque alpha, padded pitch, history reset and failures");
}
'''
body = preamble
body += function("static bool ext_rect_valid(", "static int ext_plane_check(")
body += function("static int ext_copy_frame(", "static void ext_plane_update(")
with tempfile.TemporaryDirectory(prefix="dcpext-copy-test-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(body + tests)
    subprocess.run([os.environ.get("CC", "cc"), "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(base), "-o", str(path / "test"), str(path / "test.c")], check=True)
    subprocess.run([str(path / "test")], check=True)
