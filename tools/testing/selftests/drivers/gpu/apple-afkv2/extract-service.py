#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the production service-call body with a mocked wire reply."""
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()
match = re.search(r"^int afk_service_call_timeout_len\([^;]*?\)\n\{", source, re.M)
if not match:
    raise RuntimeError("production service-call definition not found")
start = match.start()
end = match.end()
depth = 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
prefix = r'''
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "afkv2-codec.h"
#define GFP_KERNEL 0
#define kzalloc(n,f) calloc(1,n)
#define kfree(p) free(p)
#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))
#define cpu_to_le16(x) (x)
#define cpu_to_le32(x) (x)
#define le16_to_cpu(x) (x)
#define le32_to_cpu(x) (x)
#define EPIC_SERVICE_CALL_MAGIC 0x69706378
#define EPIC_SUBTYPE_STD_SERVICE 0xc0
#define dev_warn(...) do {} while(0)
#define dev_dbg(...) do {} while(0)
struct epic_service_call {
 u8 pad0[2]; u16 group; u32 command, data_len, magic; u8 pad1[48];
};
struct dcp { void *dev; };
struct ops { const char *name; };
struct ep { struct dcp *dcp; int endpoint; bool inline_v2; };
struct apple_epic_service { struct ep *ep; struct ops *ops; u32 channel; };
static int error_status, malformed, short_header, too_long, wire_error;
static int afk_send_command_timeout_len(struct apple_epic_service *s, u8 type,
 const void *payload, size_t payload_len, void *output, size_t output_len,
 u32 *retcode, unsigned int timeout_ms, size_t *received)
{
 struct epic_service_call *call=output;
 (void)payload;
 assert(payload_len==(s->ep->inline_v2?68:output_len));
 assert(call->data_len==(s->ep->inline_v2?4:4100));
 assert(type==0xc0 && timeout_ms==1000 && output_len>=68);
 if(wire_error) return -ETIMEDOUT;
 call->group=1;call->command=7;call->magic=EPIC_SERVICE_CALL_MAGIC;
 call->data_len=too_long?5:4;
 ((u8*)output)[64]=0xde;((u8*)output)[65]=0xad;
 ((u8*)output)[66]=0xbe;((u8*)output)[67]=0xef;
 if(malformed)call->command=8;
 *retcode=error_status;
 if(received)*received=short_header?63:68;
 return 0;
}
'''
suffix = r'''
int main(void)
{
 struct ep ep={.inline_v2=true};struct apple_epic_service service={.ep=&ep};
 u8 input[4]={0}, output[128]={0}; size_t received=999;
 assert(!afk_service_call_timeout_len(&service,1,7,input,4,4096,output,128,0,1000,&received));
 assert(received==4 && output[0]==0xde && output[3]==0xef && output[4]==0);
 error_status=5;received=999;
 assert(afk_service_call_timeout_len(&service,1,7,input,4,4096,output,128,0,1000,&received)==-EINVAL && received==0);
 error_status=0;malformed=1;
 assert(afk_service_call_timeout_len(&service,1,7,input,4,4096,output,128,0,1000,&received));
 malformed=0;short_header=1;
 assert(afk_service_call_timeout_len(&service,1,7,input,4,4096,output,128,0,1000,&received));
 short_header=0;too_long=1;
 assert(afk_service_call_timeout_len(&service,1,7,input,4,4096,output,128,0,1000,&received));
 too_long=0;wire_error=1;
 assert(afk_service_call_timeout_len(&service,1,7,input,4,4096,output,128,0,1000,&received)==-ETIMEDOUT);
 wire_error=0; ep.inline_v2=false;
 received=999;
 assert(!afk_service_call_timeout_len(&service,1,7,input,4,4096,output,128,0,1000,&received));
 assert(received==4 && output[0]==0xde && output[3]==0xef && output[4]==0);
 assert(!afk_service_call_timeout_len(&service,1,7,input,4,4096,output,128,0,1000,NULL));
 puts("PASS: extracted production service-call actual request / OOB padding and short reply / legacy / status / context / length tests");
}
'''
pathlib.Path(sys.argv[2]).write_text(prefix + source[start:end] + suffix)
