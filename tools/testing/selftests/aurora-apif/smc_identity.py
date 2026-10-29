#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise exact-length SMC identity reads from the production helper."""
from pathlib import Path
import subprocess, tempfile
from common import block
ROOT=Path(__file__).resolve().parents[4]
helper=block((ROOT/'drivers/power/supply/macsmc-power.c').read_text(),r'^static int macsmc_read_identity\(')
program=r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint32_t smc_key;
struct apple_smc {int dummy;};
struct apple_smc_key_info {uint8_t size,flags;};
#define APPLE_SMC_READABLE 0x80
static int calls, metadata_error, reply;
static struct apple_smc_key_info info;
static int apple_smc_get_key_info(struct apple_smc *s,smc_key key,struct apple_smc_key_info *i){(void)s;(void)key;*i=info;return metadata_error;}
static int apple_smc_read(struct apple_smc *s,smc_key key,char *out,size_t n){(void)s;(void)key;calls++;assert(n==info.size);memset(out,'A',n);return reply;}
''' + helper + r'''
int main(void) {
 struct apple_smc s={0};char out[8];memset(out,0xcc,sizeof(out));
 info=(struct apple_smc_key_info){3,0x80};reply=3;
 assert(!macsmc_read_identity(&s,1,out,sizeof(out)) && calls==1 && !memcmp(out,"AAA\0",4));
 calls=0;info.size=8;assert(macsmc_read_identity(&s,1,out,8)==-EINVAL && !calls && !out[0]);
 info.size=0;assert(macsmc_read_identity(&s,1,out,8)==-EINVAL && !calls);
 info.size=3;info.flags=0;assert(macsmc_read_identity(&s,1,out,8)==-EINVAL && !calls);
 info.flags=0x80;metadata_error=-ENOENT;assert(macsmc_read_identity(&s,1,out,8)==-ENOENT && !calls);
 metadata_error=0;reply=2;assert(macsmc_read_identity(&s,1,out,8)==-EIO && calls==1 && !out[0]);
 calls=0;assert(macsmc_read_identity(&s,1,out,0)==-EINVAL && !calls);
}
'''
with tempfile.TemporaryDirectory(prefix='smc-identity-') as directory:
 d=Path(directory);(d/'test.c').write_text(program)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
print('SMC identity: exact length, terminator, missing/unreadable/oversized keys and short reply passed')
