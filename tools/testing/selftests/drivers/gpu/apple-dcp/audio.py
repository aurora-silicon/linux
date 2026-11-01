# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright 2026 Ryan Murray
"""Compact audio reply bounds through production AV methods, synthetic transport."""
from common import block


def reply_bounds(root):
    source = (root / 'drivers/gpu/drm/apple/av.c').read_text()
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t __le64;
#define KUNIT_STATIC_STUB_REDIRECT(...) ((void)0)
#define READ_ONCE(x) (x)
#undef static_assert
#define static_assert(expr, ...) _Static_assert(expr, #expr)
#define GFP_KERNEL 0
static void free_buffer(void *p) {free(*(void **)p);}
#define __free(x) __attribute__((cleanup(free_buffer)))
static void *kvzalloc(size_t n, int flags) {(void)flags;return calloc(1,n);}
#define cpu_to_le64(x) (x)
#define le64_to_cpu(x) (x)
static u32 get_unaligned_le32(const void *p) {u32 n;memcpy(&n,p,4);return n;}
#define SZ_32K (32U * 1024U)
#define SZ_256K (256U * 1024U)
#define SZ_8M (8U * 1024U * 1024U)
#define DCP_AUDIO_OSOBJECT_MAX SZ_8M
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
struct apple_dcp_afkep {bool compact;};
struct apple_epic_service {struct apple_dcp_afkep *ep;};
static int transport_error;
static size_t wire_received, used;
static unsigned compact_calls, legacy_calls;
static u32 method_status;
static bool growing;
static size_t required, capacities[16];
static unsigned nrequests;
static int transfer(struct apple_epic_service *srv, u16 group, u32 cmd,
                    const void *data, size_t len, size_t pad, void *out,
                    size_t room, size_t out_pad)
{
    assert(srv && cmd && !out_pad);
    (void)pad;
    if (transport_error) return transport_error;
    if (group==1) {
        assert(len==48 && data==out);
        __le64 max, count=used;
        memcpy(&max,data,8);assert(room==max+48 && pad==(srv->ep->compact?0:max));
        u32 status=method_status;
        if (growing) {
            assert(nrequests<16);capacities[nrequests++]=max;
            status=required>max?0xe00002db:0;
            count=status?0:required;
        }
        memcpy((u8 *)out+16,&status,4);
        memcpy((u8 *)out+32,&count,8);
        memset((u8 *)out+48,0x5a,max);
    } else {
        assert(group==0 && len==64 && data && room==64);
        memset(out,0,room);
    }
    return 0;
}
static int afk_service_call_with_reply_len(struct apple_epic_service *srv,
        u16 group,u32 cmd,const void *data,size_t len,size_t pad,void *out,
        size_t room,size_t out_pad,size_t *received)
{
    compact_calls++;
    int ret=transfer(srv,group,cmd,data,len,pad,out,room,out_pad);
    *received=growing?(required>room-48?48:48+required):wire_received;
    return ret;
}
static int afk_service_call(struct apple_epic_service *srv,u16 group,u32 cmd,
        const void *data,size_t len,size_t pad,void *out,size_t room,size_t out_pad)
{
    legacy_calls++;
    return transfer(srv,group,cmd,data,len,pad,out,room,out_pad);
}
'''
    fragments = (root / 'drivers/gpu/drm/apple/afk-fragment.h').read_text().replace(
        '#include <linux/errno.h>', '').replace('#include <linux/types.h>', '')
    return prefix + fragments + block(source, 'static int av_audiosrv_call(') + '\n' + block(
        source, 'static int\ndcp_audiosrv_osobject_call(') + '\n' + block(
        source, 'static int dcp_audiosrv_osobject_get(') + r'''
int main(void)
{
    struct apple_dcp_afkep ep={.compact=true};
    struct apple_epic_service srv={.ep=&ep};
    u8 body[64]={0},reply[64],elements[16];
    size_t actual=0;
    /* Maximum catalog plus every actual compact/service/property header. */
    const u32 full_catalog=SZ_8M+8+8+8+64+48;
    struct afk_fragment_state state={0};
    assert(afk_fragment_accept(&state,1,0,full_catalog,full_catalog)==1);
    memset(&state,0,sizeof(state));
    assert(afk_fragment_accept(&state,1,0,full_catalog+1,1)==-EMSGSIZE);
    wire_received=64;
    assert(!av_audiosrv_call(&srv,6,body,64,0,reply,64));
    for (size_t n=0;n<66;n++) {
        wire_received=n;
        assert(av_audiosrv_call(&srv,6,body,64,0,reply,64)==
               (n==64?0:-EMSGSIZE));
    }
    transport_error=-EIO;
    assert(av_audiosrv_call(&srv,6,body,64,0,reply,64)==-EIO);
    transport_error=0;
    used=8;wire_received=56;
    memset(elements,0xff,sizeof(elements));
    assert(!dcp_audiosrv_osobject_call(&srv,1,16,elements,16,&actual));
    assert(actual==8);
    for (size_t i=0;i<16;i++) assert(elements[i]==(i<8?0x5a:0));
    for (size_t n=0;n<56;n++) {
        wire_received=n;
        assert(dcp_audiosrv_osobject_call(&srv,1,16,elements,16,&actual)==-EMSGSIZE);
    }
    wire_received=64;used=17;
    assert(dcp_audiosrv_osobject_call(&srv,1,16,elements,16,&actual)==-EMSGSIZE);
    used=16;
    assert(!dcp_audiosrv_osobject_call(&srv,1,16,elements,16,&actual) && actual==16);
    assert(!dcp_audiosrv_osobject_call(&srv,1,18,NULL,16,NULL));
    transport_error=-ETIMEDOUT;
    assert(dcp_audiosrv_osobject_call(&srv,1,16,elements,16,&actual)==-ETIMEDOUT);
    transport_error=0;
    method_status=0xe00002db;used=0;wire_received=48;
    memset(elements,0xa5,sizeof(elements));
    assert(dcp_audiosrv_osobject_call(&srv,1,16,elements,16,&actual)==-ENOSPC);
    for (size_t i=0;i<16;i++) assert(elements[i]==0xa5);
    method_status=1;
    assert(dcp_audiosrv_osobject_call(&srv,1,16,elements,16,&actual)==-EIO);
    method_status=0;
    growing=true;required=SZ_8M;
    u8 *catalog=malloc(SZ_8M);assert(catalog);
    assert(!dcp_audiosrv_osobject_get(&srv,16,catalog,SZ_8M,&actual));
    assert(actual==SZ_8M && nrequests==6);
    for (size_t i=0;i<nrequests;i++) assert(capacities[i]==((size_t)SZ_256K<<i));
    for (size_t i=0;i<SZ_8M;i++) assert(catalog[i]==0x5a);
    nrequests=0;required=SZ_8M+1;
    assert(dcp_audiosrv_osobject_get(&srv,16,catalog,SZ_8M,&actual)==-ENOSPC);
    assert(nrequests==6);
    /* A smaller replacement clears the tail left by a larger catalog. */
    nrequests=0;required=8;
    assert(!dcp_audiosrv_osobject_get(&srv,16,catalog,SZ_8M,&actual));
    for (size_t i=0;i<SZ_8M;i++) assert(catalog[i]==(i<8?0x5a:0));
    free(catalog);growing=false;wire_received=64;
    ep.compact=false;
    unsigned compact_before=compact_calls;
    assert(!av_audiosrv_call(&srv,6,body,64,0,reply,64));
    used=0;
    assert(!dcp_audiosrv_osobject_call(&srv,1,16,elements,16,&actual) && actual==0);
    for (size_t i=0;i<16;i++) assert(elements[i]==0x5a);
    assert(compact_calls==compact_before && legacy_calls==2);
    return 0;
}
'''
