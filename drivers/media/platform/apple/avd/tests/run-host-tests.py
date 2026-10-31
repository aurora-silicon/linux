#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile actual instruction builders against synthetic V4L2 controls.

The frozen baseline is Linux's licensed pre-frame_ops H.264 builder; no Apple
images or captured parameters are used. The shim replaces only kernel plumbing.
Run on a Linux host: python3 tests/run-host-tests.py --baseline-file FILE
"""
from pathlib import Path
import argparse, os, subprocess, tempfile
ap=argparse.ArgumentParser(); ap.add_argument('--baseline-file', type=Path, default=Path(__file__).parent/"h264-builder-baseline.c")
args=ap.parse_args()
root=Path(__file__).resolve().parent.parent
tests=root/'tests'
def strip_includes(s):
 return '\n'.join(l for l in s.splitlines() if not l.startswith('#include'))+'\n'
def builder(s):
 return s[s.index('#define H264_SCL_DIMS'):s.index('static int avd_h264_alloc_bufs')]
with tempfile.TemporaryDirectory(prefix='avd-h264-host-') as td:
 td=Path(td)
 # Keep the inline instruction encoding functions used by the original builder.
 inst=strip_includes((root/'avd-inst.h').read_text())
 fp_h=strip_includes((root/'avd-h264-fp.h').read_text())
 fp_c=strip_includes((root/'avd-h264-fp.c').read_text())
 common=(tests/'host-compat.h').read_text()+'\n'+inst+'\n'+fp_h+'\n'+fp_c
 outputs=[]
 for name, src in [('baseline',args.baseline_file.read_text()),('frame',(root/'avd-h264.c').read_text())]:
  path=td/(name+'.c'); exe=td/name
  path.write_text(common+'\n'+builder(src)+'\n'+(tests/'builder-main.c').read_text())
  subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-sign-compare',*(['-DTEST_FRAME'] if name=='frame' else []),str(path),'-o',str(exe)],check=True)
  outputs.append(subprocess.check_output([str(exe)]))
 assert outputs[0]==outputs[1], 'frame program differs from baseline'
 print('PASS: 96 synthetic I/P/B, 1/4-slice, scaling/weights, address-format cases match baseline byte for byte')
 path=td/'fp.c'; exe=td/'fp'
 path.write_text(common+'\n'+(tests/'fp-main.c').read_text())
 subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter',str(path),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

# Exercise actual prepare/retire and picture collection across separate requests.
src=(root/'avd-h264.c').read_text()
def section(first, last):
 return src[src.index(first):src.index(last, src.index(first))]
lifecycle=(section('static int avd_h264_validate_pps(', 'static void avd_h264_free_picture(')+
 section('static bool avd_h264_refs_valid(', 'static int avd_h264_run(struct avd_ctx *ctx)')+
 section('static void avd_h264_done(', 'static enum avd_image_fmt avd_h264_get_image_fmt')+
 section('static void avd_h264_frame_controls(', 'static int avd_h264_pack_cmd(')+
 section('static void avd_h264_retire(', 'const struct avd_codec_frame_ops avd_h264_frame_ops'))
plumbing = r"""
static unsigned int scratch_serial;
static int avd_h264_alloc_bufs(struct avd_ctx *ctx, struct avd_h264_ctx *picture)
{
 (void)ctx; picture->bufs.inst.addr=0x40000000ULL + ++scratch_serial * 0x100000;
 return 0;
}
static void avd_h264_free_picture(struct avd_ctx *ctx, struct avd_h264_ctx *picture)
{ (void)ctx; free(picture->slices); free(picture); }
"""
with tempfile.TemporaryDirectory(prefix='avd-h264-lifecycle-') as td:
 td=Path(td)
 path=td/'lifecycle.c'; exe=td/'lifecycle'
 path.write_text(common+'\n'+builder(src)+'\n'+plumbing+'\n'+lifecycle+'\n'+(tests/'lifecycle-main.c').read_text())
 flags=[os.environ.get('HOSTCC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-sign-compare']
 subprocess.run(flags+[str(path),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
 queue=(root/'avd-queue.c').read_text()
 helpers=queue[queue.index('static void avd_sources_done('):queue.index('/* queue_mutex excludes')]
 path=td/'sources.c'; exe=td/'sources'
 path.write_text((tests/'sources-main.c').read_text().replace('/* PRODUCTION_HELPERS */',helpers))
 subprocess.run(flags+[str(path),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
