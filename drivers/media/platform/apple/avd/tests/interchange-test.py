#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host tests of the production layout helper and capture negotiation functions.

Run with Python 3; the format documentation gives the command.
Only the kernel primitives and the linear V4L2 sizing dependency are shimmed.
No target, kernel build, or third-party Python modules are required.
"""
import os
import re
from pathlib import Path
import subprocess
import tempfile

avd = Path(__file__).resolve().parent.parent

def function(source, name):
    start = source.index(name + '(')
    start = source.rfind('\n', 0, start) + 1
    # The functions exercised here all have single-line return declarations.
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "avd-layout.h"
#define FOURCC(a,b,c,d) ((u32)(a) | (u32)(b)<<8 | (u32)(c)<<16 | (u32)(d)<<24)
#define V4L2_PIX_FMT_APPLE_AX12 FOURCC('A','X','1','2')
#define V4L2_PIX_FMT_APPLE_AX16 FOURCC('A','X','1','6')
#define V4L2_PIX_FMT_APPLE_AX03 FOURCC('A','X','0','3')
#define V4L2_PIX_FMT_APPLE_AX23 FOURCC('A','X','2','3')
#define V4L2_PIX_FMT_NV12 FOURCC('N','V','1','2')
#define V4L2_PIX_FMT_NV16 FOURCC('N','V','1','6')
#define V4L2_PIX_FMT_P010 FOURCC('P','0','1','0')
#define AVD_ALIGN 256
#define WARN_ON(x) (x)
enum avd_image_fmt {
 AVD_IMG_FMT_ANY, AVD_IMG_FMT_420_8BIT, AVD_IMG_FMT_420_10BIT,
 AVD_IMG_FMT_422_8BIT, AVD_IMG_FMT_422_10BIT
};
struct v4l2_pix_format_mplane {
 u32 width, height, pixelformat, num_planes;
 struct { u32 sizeimage, bytesperline; } plane_fmt[1];
};
struct avd_decoded_fmt_desc { u32 fourcc; enum avd_image_fmt image_fmt; };
struct avd_coded_fmt_ops {
 void (*adjust_decoded_fmt)(void *, struct v4l2_pix_format_mplane *);
};
struct avd_coded_fmt_desc {
 const struct avd_decoded_fmt_desc *decoded_fmts;
 unsigned int num_decoded_fmts;
 const struct avd_coded_fmt_ops *ops;
};
struct avd_ctx {
 const struct avd_coded_fmt_desc *coded_fmt_desc;
 enum avd_image_fmt image_fmt;
 struct avd_comp comp;
 struct { struct { struct v4l2_pix_format_mplane pix_mp; } fmt; } decoded_fmt;
 struct { void *m2m_ctx; } fh;
 int ctrl_hdl;
 bool decomp;
 bool *dev;
};
static bool avd_backend_is_apple(const bool *dev) { return *dev; }
static void v4l2_fill_pixfmt_mp(struct v4l2_pix_format_mplane *p,
                              u32 fmt, u32 w, u32 h)
{
 /* Linear sizing is a dependency, not the subject of these host tests. */
 u32 bytes = fmt == V4L2_PIX_FMT_P010 ? 2 : 1;
 p->num_planes = 1;
 p->plane_fmt[0].bytesperline = w * bytes;
 p->plane_fmt[0].sizeimage = w * h * bytes * 3 / 2;
}
'''
# Compile the actual UAPI values, independently checking their spelling below.
uapi = (avd.parents[4] / 'include/uapi/linux/videodev2.h').read_text()
for name in ('APPLE_AX12', 'APPLE_AX16', 'APPLE_AX03', 'APPLE_AX23',
             'NV12', 'NV16', 'P010'):
    actual = re.search(r'^#define V4L2_PIX_FMT_' + name + r'\s+(v4l2_fourcc\([^\n]+?\))',
                       uapi, re.M).group(1).replace('v4l2_fourcc', 'FOURCC')
    harness = re.sub(r'^#define V4L2_PIX_FMT_' + name + r' .*$',
                     '#define V4L2_PIX_FMT_' + name + ' ' + actual, harness, flags=re.M)
source = (avd / 'avd-v4l2.c').read_text()
harness += function((avd / 'avd-drv.c').read_text(), 'fill_comp')
for name in ('avd_is_interchange', 'avd_capture_fmt_supported',
             'avd_image_fmt_match', 'avd_enum_decoded_fmt', 'avd_is_valid_fmt',
             'avd_capture_comp', 'avd_fill_decoded_pixfmt'):
    harness += function(source, name)
# Exercise the actual codec lists so adding an unsupported format fails here.
for codec in ('h264', 'hevc', 'vp9', 'av1'):
    start = source.index('static const struct avd_decoded_fmt_desc avd_' + codec)
    end = source.index('\n};', start) + 3
    harness += source[start:end] + '\n'
harness += r'''
struct media_request { int unused; };
struct vb2_buffer { struct { struct media_request *req; } req_obj; };
struct vb2_v4l2_buffer { struct vb2_buffer vb2_buf; };
struct avd_decoded_buffer { struct avd_comp comp; };
static struct vb2_v4l2_buffer src_buf, dst_buf;
static struct avd_decoded_buffer dst_private;
struct avd_run {
 struct { struct vb2_v4l2_buffer *src, *dst; } bufs;
 unsigned long coded_in, y_out, uv_out, comp_out;
};
static struct vb2_v4l2_buffer *v4l2_m2m_next_src_buf(void *p) { return &src_buf; }
static struct vb2_v4l2_buffer *v4l2_m2m_next_dst_buf(void *p) { return &dst_buf; }
static unsigned long vb2_dma_contig_plane_dma_addr(struct vb2_buffer *p, int i)
 { return p==&src_buf.vb2_buf ? 0x40000000UL : 0x80000000UL; }
static struct avd_decoded_buffer *vb2_to_avd_decoded_buf(struct vb2_buffer *p)
 { return &dst_private; }
static void v4l2_ctrl_request_setup(struct media_request *p, int *h) {}
static void v4l2_m2m_buf_copy_metadata(struct vb2_v4l2_buffer *a,
                                    struct vb2_v4l2_buffer *b) {}
'''
harness += function(source, 'avd_run_preamble')
harness += r'''
struct fixture { u32 w,h,depth,hdiv,vdiv,y,uv,uv_meta,size; };
static const struct fixture fixtures[] = {
 {64,16,8,2,2,2048,2176,3200,3328},
 {64,64,8,2,2,4096,4224,6272,6400},
 {64,48,8,2,1,4096,4224,7296,7424},
 {64,64,10,2,2,5120,5248,7808,7936},
 {65,33,8,2,2,6144,6400,9472,9600},
 {65,33,10,2,2,7680,7936,11776,11904},
 {1920,1080,8,2,2,2088960,2220032,3264512,3297280},
 {1920,1080,10,2,2,2611200,2742272,4047872,4080640},
 {1920,1080,8,2,1,2088960,2220032,4308992,4374528},
 {1920,1080,10,2,1,2611200,2742272,5353472,5419008},
 {64,64,8,1,1,4096,4224,12416,12544},
 {64,64,10,1,1,5120,5248,15488,15616},
 {16384,16384,10,1,1,335544320,343932928,1015021568,1023410176},
};
int main(void)
{
 struct avd_comp c = {0};
 unsigned int count = 0;
 for (size_t i=0; i<sizeof(fixtures)/sizeof(fixtures[0]); ++i) {
  const struct fixture *f=&fixtures[i];
  avd_interchange_layout(&c,f->w,f->h,f->depth,f->hdiv,f->vdiv);
  assert(c.offsets[0]==f->y && c.offsets[1]==0);
  assert(c.offsets[3]==f->uv && c.offsets[2]==f->uv_meta);
  assert(c.size==f->size);
 }
 const u32 sizes[]={1,15,16,17,31,32,33,63,64,65,1080,2160,4096,16384};
 for (unsigned int d=8; d<=10; d+=2)
 for (unsigned int sub=0; sub<3; ++sub)
 for (size_t w=0; w<sizeof(sizes)/sizeof(sizes[0]); ++w)
 for (size_t h=0; h<sizeof(sizes)/sizeof(sizes[0]); ++h) {
  avd_interchange_layout(&c,sizes[w],sizes[h],d,sub==2?1:2,sub?1:2);
  assert(!c.offsets[1] && c.offsets[0]<c.offsets[3]);
  assert(c.offsets[3]<c.offsets[2] && c.offsets[2]<c.size);
  for (int j=0;j<4;++j) assert(c.offsets[j]%128==0);
  assert(c.size%128==0 && c.size<2147483648U);
  ++count;
 }
 const struct avd_decoded_fmt_desc *lists[]={avd_h264_decoded_fmts,
  avd_hevc_decoded_fmts,avd_vp9_decoded_fmts,avd_av1_decoded_fmts};
 const size_t lengths[]={sizeof(avd_h264_decoded_fmts)/sizeof(*lists[0]),
  sizeof(avd_hevc_decoded_fmts)/sizeof(*lists[0]),
  sizeof(avd_vp9_decoded_fmts)/sizeof(*lists[0]),
  sizeof(avd_av1_decoded_fmts)/sizeof(*lists[0])};
 const struct avd_coded_fmt_ops ops={0};
 for (size_t codec=0;codec<4;++codec) {
  struct avd_coded_fmt_desc desc={lists[codec],lengths[codec],&ops};
  bool apple=false;
  struct avd_ctx ctx={.coded_fmt_desc=&desc,.dev=&apple};
  memset(&ctx.comp,0xa5,sizeof(ctx.comp));
  struct avd_comp before=ctx.comp;
  struct v4l2_pix_format_mplane p={.width=1920,.height=1088,
   .pixelformat=V4L2_PIX_FMT_APPLE_AX03};
  /* ANY must use the selected fourcc's depth and subsampling. */
  avd_fill_decoded_pixfmt(&ctx,&p);
  assert(!memcmp(&ctx.comp,&before,sizeof(before)));
  assert(p.num_planes==1 && !p.plane_fmt[0].bytesperline);
  assert(p.plane_fmt[0].sizeimage==4080640);
  ctx.image_fmt=AVD_IMG_FMT_420_8BIT;
  assert(avd_enum_decoded_fmt(&ctx,0,ctx.image_fmt)==V4L2_PIX_FMT_NV12);
  assert(avd_is_valid_fmt(&ctx,V4L2_PIX_FMT_APPLE_AX12,ctx.image_fmt));
  assert(!avd_is_valid_fmt(&ctx,V4L2_PIX_FMT_APPLE_AX03,ctx.image_fmt));
  p.pixelformat=V4L2_PIX_FMT_APPLE_AX12;
  avd_capture_comp(&ctx,&p,&c);
  assert(c.start_offset==0 && c.size==3297280);
  ctx.decoded_fmt.fmt.pix_mp=p;
  ctx.comp=c;
  struct avd_run run;
  avd_run_preamble(&ctx,&run);
  assert(!ctx.decomp && run.comp_out==0x80000000UL);
  assert(!memcmp(&dst_private.comp,&c,sizeof(c)));
  p.pixelformat=V4L2_PIX_FMT_NV12;
  p.plane_fmt[0].bytesperline=1920;
  avd_capture_comp(&ctx,&p,&c);
  assert(c.start_offset==1920*1088*3/2 && c.size==3297280);
  ctx.decoded_fmt.fmt.pix_mp=p;
  ctx.comp=c;
  avd_run_preamble(&ctx,&run);
  assert(ctx.decomp && run.comp_out==0x80000000UL+c.start_offset);
  ctx.image_fmt=AVD_IMG_FMT_422_8BIT;
  p.pixelformat=V4L2_PIX_FMT_APPLE_AX16;
  avd_capture_comp(&ctx,&p,&c);
  assert(c.size==4374528);
  /* A short 4:2:2 frame distinguishes full chroma height from 4:4:0. */
  p.width=64;
  p.height=48;
  avd_capture_comp(&ctx,&p,&c);
  assert(c.offsets[3]==4224 && c.offsets[2]==7296 && c.size==7424);
  apple=true;
  assert(!avd_is_valid_fmt(&ctx,V4L2_PIX_FMT_APPLE_AX16,ctx.image_fmt));
  assert(avd_enum_decoded_fmt(&ctx,0,ctx.image_fmt)==V4L2_PIX_FMT_NV16);
  assert(avd_enum_decoded_fmt(&ctx,1,ctx.image_fmt)==0);
 }
 printf("PASS: 13 golden layouts, %u alignment/bounds cases, 4 codec negotiation and output-selection paths\n",count);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='avd-interchange-') as tmp:
    tmp = Path(tmp)
    inc = tmp / 'linux'
    inc.mkdir()
    (inc / 'types.h').write_text('#include <stdint.h>\n#include <stddef.h>\ntypedef uint32_t u32;\n')
    (inc / 'align.h').write_text('#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))\n')
    (inc / 'log2.h').write_text('static inline unsigned long roundup_pow_of_two(unsigned long x) { unsigned long p=1; while(p<x) p<<=1; return p; }\n')
    c = tmp / 'test.c'
    c.write_text(harness)
    binary = tmp / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    '-I'+str(tmp), '-I'+str(avd), str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
