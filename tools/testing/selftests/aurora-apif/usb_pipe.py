#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual PIPE backend with stopped/active-consumer ACK models."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[4]
include=root/'drivers/phy/apple/usb-t6050'
source=r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "apple_t6050_usb3_pipe.h"
static unsigned regs[45][0x120/4], polls, quiesces;
static bool no_lock, bad_override, bad_unlock, bad_mux, bad_quiesce;
static unsigned rd(void *p,unsigned b,unsigned o){return regs[b][o/4];}
static void wr(void *p,unsigned b,unsigned o,unsigned v){
 if(b==44&&o==0xc&&bad_mux&&v==0x42)return;
 regs[b][o/4]=v;
}
static int poll(void*p,unsigned b,unsigned o,unsigned m,unsigned v,unsigned us){
 polls++;
 if(b==44&&o==0x14){
  if(v==1)return no_lock?-1:0;
  return bad_unlock?-1:0;
 }
 if(b==44&&o==0x20)return bad_override?-1:0;
 return 0;
}
static void delay(void*p,unsigned us){}
static int quiesce(void*p){quiesces++;return bad_quiesce?-1:0;}
static struct apple_t6050_usb_io io={0,rd,wr,poll,delay};
static void reset(void){memset(regs,0,sizeof(regs));regs[44][0xc/4]=0x10;regs[44][0x1c/4]=1;polls=quiesces=0;no_lock=bad_override=bad_unlock=bad_mux=bad_quiesce=false;}
int main(void){
 reset();assert(!apple_t6050_usb3_pipe_stop(&io,quiesce,0));assert(quiesces==1&&regs[44][0xc/4]==0x42);
 reset();no_lock=true;assert(!apple_t6050_usb3_pipe_stop(&io,quiesce,0));assert(quiesces==1&&regs[44][0xc/4]==0x42);
 reset();no_lock=bad_quiesce=true;assert(apple_t6050_usb3_pipe_stop(&io,quiesce,0));assert(quiesces==1&&regs[44][0xc/4]==0x10);
 reset();no_lock=bad_override=true;assert(apple_t6050_usb3_pipe_stop(&io,quiesce,0));assert(regs[44][0xc/4]==0x10);
 reset();no_lock=bad_mux=true;assert(apple_t6050_usb3_pipe_stop(&io,quiesce,0));
 reset();no_lock=bad_unlock=true;assert(apple_t6050_usb3_pipe_stop(&io,quiesce,0));
 reset();no_lock=true;assert(apple_t6050_usb3_pipe_switch(&io));assert(regs[44][0xc/4]==0x10);
 reset();regs[44][0xc/4]=0x21;assert(apple_t6050_usb3_pipe_stop(&io,quiesce,0));assert(!polls&&!quiesces);
}
'''
with tempfile.TemporaryDirectory(prefix='apple-pipe-') as temporary:
 src=Path(temporary)/'test.c';exe=Path(temporary)/'test';src.write_text(source)
 subprocess.run(['cc','-fsanitize=address,undefined','-I'+str(include),str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
 # Negative control: the old mandatory stop-lock branch must fail the
 # stopped-consumer/no-ACK case, rather than passing a vacuous test.
 header=(include/'apple_t6050_usb3_pipe.h').read_text()
 control=header.replace('    io->poll(io->context, 44, 0x14, 1, 1, 150000);',
                        '    if (io->poll(io->context, 44, 0x14, 1, 1, 150000)) goto out;')
 assert control!=header
 (Path(temporary)/'apple_t6050_usb3_pipe.h').write_text(control)
 subprocess.run(['cc','-fsanitize=address,undefined','-I'+temporary,'-I'+str(include),str(src),'-o',str(exe)],check=True)
 assert subprocess.run([str(exe)],capture_output=True).returncode!=0
print('PIPE teardown: ACK/no-ACK pass and old-branch control fails; quiesce, override, mux, unlock and active-producer lock failures stay fatal')
