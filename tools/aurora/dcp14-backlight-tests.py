#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host checks for the 14.x DCP backlight-correction callback dispatch."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'drivers/gpu/drm/apple/iomfb_v14_7.c'


def dispatch(source):
    callback = source[source.index('static int dcp_v14_callback(void *cookie, u32 tag',
                                   source.index('/* Runs on the thread')):]
    return callback[callback.index('#define SHAPE(i, o)'):callback.index('\t/* get_time */')]


def check(source, directory, old=False):
    prefix = '''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
struct apple_dcp_v14 { u32 backlight_factor; u64 backlight_factor_updates; };
#define D(n) (((u32)'D' <<24)|((u32)('0'+(n)/100)<<16)|((u32)('0'+(n)/10%10)<<8)|(u32)('0'+(n)%10))
static u32 get_unaligned_le32(const void *p) { const u8 *b=p;return b[0]|(u32)b[1]<<8|(u32)b[2]<<16|(u32)b[3]<<24; }
static int callback(struct apple_dcp_v14 *v14, u32 tag, const u8 *in, u32 in_size, u32 out_size) {
'''
    tests = '''
int main(void) {
 struct apple_dcp_v14 v={0x55,0};u8 bytes[]={0,0xff,0x00,0x80,0xff};u8 *in=bytes+1;
 int got=callback(&v,D(208),in,4,0);
#ifdef OLD
 assert(got==-EOPNOTSUPP);assert(v.backlight_factor==0x55 && v.backlight_factor_updates==0);
#else
 assert(got==0);assert(v.backlight_factor==0xff8000ff && v.backlight_factor_updates==1);
 in[0]=0;in[1]=0;in[2]=0;in[3]=0;
 assert(callback(&v,D(208),in,4,0)==0 && v.backlight_factor==0 && v.backlight_factor_updates==2);
 for(unsigned i=0;i<10;i++)for(unsigned o=0;o<10;o++) {
  if(i==4&&o==0)continue;
  v.backlight_factor=0x55;v.backlight_factor_updates=2;
  assert(callback(&v,D(208),NULL,i,o)==-EOPNOTSUPP);
  assert(v.backlight_factor==0x55 && v.backlight_factor_updates==2);
 }
 for(unsigned t=0;t<1000;t++)if(t!=208) {
  v.backlight_factor=0x55;v.backlight_factor_updates=2;
  assert(callback(&v,D(t),NULL,4,0)==-EOPNOTSUPP);
  assert(v.backlight_factor==0x55 && v.backlight_factor_updates==2);
 }
#endif
}
'''
    path = directory / ('old.c' if old else 'current.c')
    path.write_text(prefix + dispatch(source) + '\nreturn -EOPNOTSUPP;\n}\n' + tests)
    binary = path.with_suffix('')
    command = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-unused-parameter']
    if old:
        command.append('-DOLD')
    subprocess.run(command + [str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--old-source', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='dcp14-backlight-') as directory:
        check(SOURCE.read_text(), Path(directory))
        print('D208 dispatch: signed/zero/unaligned factor, 99 bad shapes, 999 other tags PASS')
        if args.old_source:
            check(args.old_source.read_text(), Path(directory), old=True)
            print('Old D208 dispatch rejects the firmware correction notification: PASS')
    print('Controls cover the early notification dispatch, not unrelated callback handlers or hardware.')


if __name__ == '__main__':
    main()
