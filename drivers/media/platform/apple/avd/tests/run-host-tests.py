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
