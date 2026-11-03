#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the production DART stream-state operation builder with retained SIDs."""
from pathlib import Path
import subprocess,tempfile,sys
root=Path(__file__).resolve().parents[4]
p=Path(sys.argv[1]) if len(sys.argv)>1 else root/'drivers/iommu/apple-dart-apif.c'
s=p.read_text();start=s.index('\t\tcase APIF_DART_ENABLE:');end=s.index('\t\tcase 7:',start);fragment=s[start:end]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t u64;
#define APIF_DART_ENABLE 5
#define APIF_DART_DISABLE 6
#define AURORA_APIF_SELECTOR(t,e) (((u64)(t)<<32)|(e))
#define EINVAL 22
struct apple_dart_apif { unsigned dart_id,num_streams;void *retained[256]; };
struct apif_op { unsigned op;u64 arg[6]; };
struct aurora_apif_op { u64 selector,arg[6],ret; };
static int build(struct apple_dart_apif *dart,unsigned opcode,u64 sid,unsigned *count,struct aurora_apif_op *native) {
 struct apif_op request={.op=opcode,.arg={sid}};struct apif_op *op=&request;u64 *a=op->arg;
 struct aurora_apif_op call={.arg={dart->dart_id,sid}};unsigned n=0;int ret=0;
 switch(op->op) {
''' + fragment + r'''
 default:assert(false);
 }
out:*count=n;return ret;
}
int main(void) {
 const unsigned sizes[]={1,16,256};unsigned checked=0;
 for(unsigned k=0;k<3;k++) {
  struct apple_dart_apif d={.dart_id=16,.num_streams=sizes[k]};
  for(unsigned sid=0;sid<d.num_streams;sid++)d.retained[sid]=sid%3==0?(void *)1:0;
  for(unsigned sid=0;sid<d.num_streams;sid++)for(unsigned op=5;op<=6;op++) {
   unsigned n=99;struct aurora_apif_op out[2]={0};assert(build(&d,op,sid,&n,out)==0);
   if(d.retained[sid])assert(n==0);
   else {assert(n==1);assert(out[0].selector==AURORA_APIF_SELECTOR(3,op==5?8:7));assert(out[0].arg[0]==16&&out[0].arg[1]==sid);}
   checked++;
  }
  for(unsigned op=5;op<=6;op++)for(unsigned invalid=0;invalid<2;invalid++) {
   unsigned n=99;struct aurora_apif_op out[2]={0};u64 sid=invalid?UINT64_MAX:d.num_streams;
   assert(build(&d,op,sid,&n,out)==-EINVAL);assert(n==0);checked++;
  }
 }
 printf("DART retained/native stream-state contracts pass (%u requests)\n",checked);
}
'''
with tempfile.TemporaryDirectory(prefix='m5-dart-retained-') as tmp:
 t=Path(tmp);(t/'test.c').write_text(code)
 subprocess.run(['cc','-std=gnu11','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(t/'test.c'),'-o',str(t/'test')],check=True)
 subprocess.run([str(t/'test')],check=True)
