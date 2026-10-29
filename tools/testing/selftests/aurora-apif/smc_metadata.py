#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Model a competing shared-buffer reply at the SMC metadata copy boundary."""
from pathlib import Path
import os, subprocess, tempfile
from common import block

ROOT=Path(__file__).resolve().parents[4]
function=block((ROOT/'drivers/mfd/macsmc.c').read_text(), r'^int apple_smc_get_key_info\(')
prefix=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t smc_key;
struct mutex { int held; };
struct apple_smc { struct mutex mutex; struct { void *iomem; } shmem; };
struct apple_smc_key_info { u8 size; uint32_t type_code; u8 flags; };
struct lock_guard { struct mutex *m; };
static struct lock_guard acquire_guard(struct mutex *m) { assert(!m->held);m->held=1;return (struct lock_guard){m}; }
static void release_guard(struct lock_guard *g) { assert(g->m->held);g->m->held=0; }
#define guard(type) struct lock_guard __attribute__((cleanup(release_guard))) scope_guard=acquire_guard
#define SMC_MSG_GET_KEY_INFO 0x13
static int response;
static struct apple_smc *current;
static int apple_smc_cmd_locked(struct apple_smc *s,int command,smc_key key,int a,int b,void *out) {
 (void)key;(void)a;(void)b;(void)out;assert(command==SMC_MSG_GET_KEY_INFO && s->mutex.held);
 const u8 reply[]={2,'u','i','1','6',0x80};memcpy(s->shmem.iomem,reply,6);current=s;return response;
}
static __attribute__((unused)) int apple_smc_cmd(struct apple_smc *s,int c,smc_key k,int a,int b,void *out) {
 guard(mutex)(&s->mutex);return apple_smc_cmd_locked(s,c,k,a,b,out);
}
static void apple_smc_read_sram(struct apple_smc *smc,void *out,size_t n) {
 void *in=smc->shmem.iomem;
 /* The contender can replace the shared reply as soon as the mutex is free. */
 if(!current->mutex.held) memset(in,0xee,n);
 memcpy(out,in,n);
}
static uint32_t get_unaligned_be32(const u8 *p) {return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|(p[2]<<8)|p[3];}
'''
suffix=r'''
int main(void) {
 u8 data[6];struct apple_smc s={.shmem.iomem=data};struct apple_smc_key_info i={0};
 response=6;if(apple_smc_get_key_info(&s,1,&i)!=6 || i.size!=2 || i.type_code!=0x75693136 || i.flags!=0x80) return 42;
 assert(!s.mutex.held);
 response=-EIO;i=(struct apple_smc_key_info){.size=9,.flags=7};
 assert(apple_smc_get_key_info(&s,1,&i)==-EIO && i.size==9 && i.flags==7 && !s.mutex.held);
 response=0;assert(!apple_smc_get_key_info(&s,1,NULL) && !s.mutex.held);
}
'''
old=function.replace('\t/* Keep the shared reply buffer stable until its metadata is copied. */\n\tguard(mutex)(&smc->mutex);\n\tret = apple_smc_cmd_locked', '\tret = apple_smc_cmd')
assert old!=function
with tempfile.TemporaryDirectory(prefix='smc-metadata-') as directory:
    d=Path(directory)
    for name,body in [('fixed',function),('old',old)]:
        (d/(name+'.c')).write_text(prefix+body+suffix)
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(d/(name+'.c')),'-o',str(d/name)],check=True)
        result=subprocess.run([str(d/name)],env={**os.environ,'LD_PRELOAD':''})
        assert result.returncode==(0 if name=='fixed' else 42)
print('SMC metadata: concurrent reply protected; errors and NULL metadata unlock correctly; old path fails the race control')
