#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Verify legacy EDID tolerance and stricter external-inline parsing separately."""
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()

def function(name):
    match = re.search(r"static const struct drm_edid \*" + name + r"\([^;]*?\)\n\{", source)
    if not match:
        raise RuntimeError("production reader missing: " + name)
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]

assert "service->ep->wire_v2 ? dcpavserv_read_edid_inline(service)" in source
prefix = r'''
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "epic/dpavserv-edid.h"
#define GFP_KERNEL 0
#define kzalloc(n,f) calloc(1,n)
#define cpu_to_le64(x) (x)
#define le64_to_cpu(x) (x)
#define READ_ONCE(x) (x)
#define KUNIT_STATIC_STUB_REDIRECT(...) do {} while (0)
#define SZ_2K 2048U
#define EDID_COMPACT_BUF_SIZE (EDID_LEADING_DATA_SIZE + SZ_2K)
#define EDID_EXT_BLOCK_COUNT_OFFSET 0x7e
#define ERR_PTR(x) ((void*)(intptr_t)(x))
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
static void cleanup_free(void *p) { free(*(void**)p); }
#define __free(x) __attribute__((cleanup(cleanup_free)))
struct ep { bool compact; };
struct apple_epic_service { struct ep *ep; };
struct drm_edid { size_t size; };
struct dpavserv_copy_edid_cmd { u64 max_size;u8 pad1[24];u64 used_size;u8 pad2[8]; };
struct dpavserv_copy_edid_resp { u64 max_size;u8 pad1[24];u64 used_size;u8 pad2[8];u8 data[]; };
static u8 fixture[48+8+256];
static bool truncated;
static const struct drm_edid *drm_edid_alloc(const void *p,size_t n)
{ struct drm_edid *e=malloc(sizeof(*e));(void)p;e->size=n;return e; }
/* The request asks for the buffer the transport can carry: 2 KiB on compact EPIC. */
static size_t request_size(const struct apple_epic_service *s, bool inline_call)
{ return !inline_call && s->ep->compact ? EDID_COMPACT_BUF_SIZE : EDID_BUF_SIZE; }
static int edid_call(struct apple_epic_service *s,bool inline_call,u16 group,u32 command,
 const void *data,size_t data_len,size_t data_pad,void *output,size_t output_len,size_t output_pad)
{
 const struct dpavserv_copy_edid_cmd *cmd=data;
 size_t want=request_size(s,inline_call);
 assert(group==1 && command==7 && data_len==48 && cmd->max_size==want && data_pad==want);
 assert(output_len==48+want && output_pad==0);
 assert(output_len>=sizeof(fixture));memcpy(output,fixture,sizeof(fixture));return 0;
}
static int afk_service_call_len(struct apple_epic_service *s,u16 group,u32 command,
 const void *data,size_t data_len,size_t data_pad,void *output,size_t output_len,
 size_t output_pad,size_t *received)
{ int ret=edid_call(s,true,group,command,data,data_len,data_pad,output,output_len,output_pad);
 *received=sizeof(fixture)-(truncated?1:0);return ret; }
/* Legacy EPIC: the reply length the firmware reported. */
static int afk_service_call_with_reply_len(struct apple_epic_service *s,u16 group,u32 command,
 const void *data,size_t data_len,size_t data_pad,void *output,size_t output_len,
 size_t output_pad,size_t *reply_len)
{ int ret=edid_call(s,false,group,command,data,data_len,data_pad,output,output_len,output_pad);
 *reply_len=sizeof(fixture)-(truncated?1:0);return ret; }
static void fix_checksum(u8 *p)
{ unsigned int i;u8 sum=0;p[127]=0;for(i=0;i<127;i++)sum+=p[i];p[127]=-sum; }
'''
suffix = r'''
int main(void)
{
 struct ep ep={0};struct apple_epic_service service={.ep=&ep};const struct drm_edid *e;
 u8 *base=fixture+56,*extension=base+128;
 afkv2_put64(fixture,EDID_BUF_SIZE);afkv2_put64(fixture+32,8+256);
 memset(base+1,255,6);base[126]=1;fix_checksum(base);
 extension[0]=2; /* deliberately invalid extension checksum */
 e=dcpavserv_read_edid_legacy(&service);assert(!IS_ERR(e) && e->size==256);free((void*)e);
 e=dcpavserv_read_edid_inline(&service);assert(IS_ERR(e));
 fix_checksum(extension);
 e=dcpavserv_read_edid_inline(&service);assert(!IS_ERR(e) && e->size==256);free((void*)e);
 truncated=true;
 e=dcpavserv_read_edid_inline(&service);assert(IS_ERR(e));
 /* The legacy reader now bounds the EDID by the reported reply length too. */
 e=dcpavserv_read_edid_legacy(&service);assert(IS_ERR(e));
 truncated=false;
 /* Its max_size echo must match the request, and the EDID must be whole blocks. */
 afkv2_put64(fixture,EDID_BUF_SIZE-1);
 e=dcpavserv_read_edid_legacy(&service);assert(IS_ERR(e));
 afkv2_put64(fixture,EDID_BUF_SIZE);afkv2_put64(fixture+32,8+200);
 e=dcpavserv_read_edid_legacy(&service);assert(IS_ERR(e));
 afkv2_put64(fixture+32,8+128);
 e=dcpavserv_read_edid_legacy(&service);assert(IS_ERR(e)); /* base block announces one extension */
 afkv2_put64(fixture+32,8+256);
 /* Compact EPIC asks for 2 KiB and expects that size echoed. */
 ep.compact=true;
 e=dcpavserv_read_edid_legacy(&service);assert(IS_ERR(e));
 afkv2_put64(fixture,EDID_COMPACT_BUF_SIZE);
 e=dcpavserv_read_edid_legacy(&service);assert(!IS_ERR(e) && e->size==256);free((void*)e);
 puts("PASS: actual legacy EDID checksum tolerance preserved with reply-length, echo and block bounds (legacy and compact); inline checksum/actual-length validation enforced");
}
'''
pathlib.Path(sys.argv[2]).write_text(prefix + function("dcpavserv_read_edid_legacy") + function("dcpavserv_read_edid_inline") + suffix)
