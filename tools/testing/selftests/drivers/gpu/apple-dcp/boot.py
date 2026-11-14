# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright 2026 Ryan Murray
"""RTKit event dispatch and ACK policies with public synthetic messages."""
from common import block

def oslog(root):
    source = (root / 'drivers/soc/apple/rtkit.c').read_text()
    constants = '\n'.join(line for line in source.splitlines()
                          if line.startswith('#define APPLE_RTKIT_OSLOG_'))
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint8_t u8;
#define GENMASK_ULL(h,l) ((~0ULL << (l)) & (~0ULL >> (63-(h))))
#define FIELD_GET(mask,value) (((value) & (mask)) >> __builtin_ctzll(mask))
#define APPLE_RTKIT_EP_OSLOG 8
struct buffer { unsigned sentinel; unsigned size; };
struct apple_rtkit { void *dev; struct buffer oslog_buffer; bool oslog_inherited, sync_syslog_ack, oslog_echo; u64 oslog_owed; };
#define cmpxchg(p,o,n) __sync_val_compare_and_swap((p),(o),(n))
static unsigned warnings, notifications, requests, acks, errors;
static u64 observed;
static bool neo, m5;
#define of_machine_is_compatible(s) ((!strcmp(s,"apple,t8140") && neo) || (!strcmp(s,"apple,t8142") && m5))
#define dev_warn(dev,fmt,msg) do { (void)(dev); (void)(fmt); warnings++; observed=(msg); } while (0)
#define dev_dbg(dev,fmt,msg) do { (void)(dev); (void)(fmt); notifications++; observed=(msg); } while (0)
#define dev_err(dev,fmt,msg) do { (void)(dev); (void)(fmt); (void)(msg); errors++; } while (0)
static int __attribute__((unused)) apple_rtkit_send_message(struct apple_rtkit *rtk, u8 ep, u64 msg, void *completion, bool atomic)
{ (void)rtk; assert(ep==8 && !completion && !atomic); acks++; observed=msg; return 0; }
static void apple_rtkit_common_rx_get_buffer(struct apple_rtkit *rtk, struct buffer *b, u8 ep, u64 msg)
{ assert(b==&rtk->oslog_buffer && ep==8); requests++; observed=msg; }
''' + constants + '\n' + block(source, 'static void apple_rtkit_oslog_rx(') + r'''
static void reset(void) { warnings=notifications=requests=acks=errors=0; observed=0; }
int main(void)
{
    struct apple_rtkit rtk={.oslog_buffer={.sentinel=0x12345678}};
    neo=true;
    for (u8 type=2; type<=5; type++) {
        for (unsigned edge=0; edge<2; edge++) {
            u64 msg=((u64)type<<56) | (edge ? ((1ULL<<56)-1) : 0);
            reset(); apple_rtkit_oslog_rx(&rtk,msg);
            assert(!warnings && notifications==1 && !requests && !acks && !errors);
            assert(observed==msg);
            assert(rtk.oslog_buffer.sentinel==0x12345678);
        }
    }
    puts("PASS recognized opaque types preserve both payload bounds without buffer/ACK operations");
    const u8 unknown[]={0,6,0xff};
    for (unsigned i=0;i<sizeof(unknown);i++) {
        u64 msg=((u64)unknown[i]<<56) | 42;
        reset(); apple_rtkit_oslog_rx(&rtk,msg);
        assert(warnings==1 && !notifications && !requests && observed==msg);
    }
    puts("PASS unknown types remain warnings");
    reset(); apple_rtkit_oslog_rx(&rtk,1ULL<<56);
    assert(requests==1 && !warnings && !notifications && observed==(1ULL<<56));
    puts("PASS buffer requests retain existing dispatch");
#ifdef APPLE_RTKIT_OSLOG_LOG
    /* Other machines acknowledge only admitted channel-zero counters. */
    neo=m5=false;
    reset(); apple_rtkit_oslog_rx(&rtk,2ULL<<56);
    assert(warnings==1 && !acks && !notifications);
    for (unsigned inherited=0; inherited<2; inherited++) {
        rtk.oslog_buffer.size=inherited ? 0 : 4096;
        rtk.oslog_inherited=inherited;
        reset(); apple_rtkit_oslog_rx(&rtk,(2ULL<<56)|UINT32_MAX);
        assert(acks==1 && !warnings && observed==((2ULL<<56)|UINT32_MAX));
        reset(); apple_rtkit_oslog_rx(&rtk,(2ULL<<56)|(1ULL<<32));
        assert(warnings==1 && !acks);
    }
    /* Each tested platform policy overrides channel and buffer admission. */
    rtk.oslog_inherited=false;
    for (unsigned policy=0; policy<3; policy++) {
        neo=policy==0; m5=policy==1; rtk.sync_syslog_ack=policy==2;
        reset(); apple_rtkit_oslog_rx(&rtk,(2ULL<<56)|(1ULL<<32));
        assert(notifications==1 && !warnings && !acks);
    }
    puts("PASS channel-zero ACK admission and Neo/M5/synchronous no-ACK policies");
#else
    (void)neo; (void)m5;
#endif
    return 0;
}
'''


def syslog(root):
    source = (root / 'drivers/soc/apple/rtkit.c').read_text()
    dcp = (root / 'drivers/gpu/drm/apple/dcp.c').read_text()
    header = (root / 'drivers/gpu/drm/apple/dcp-syslog.h').read_text()
    helper = header[header.index('static inline bool'):header.index('\n#endif')]
    # The policy covers both DCP transports, the internal panel's and the
    # external H17P one. It demotes only the known records it matches, so other
    # records, other firmware and other RTKit clients keep info logging.
    assert '.syslog_is_debug = dcp_rtk_syslog_is_debug' in block(dcp, 'static struct apple_rtkit_ops rtkit_ops =')
    assert '.syslog_is_debug = dcp_rtk_syslog_is_debug' in block(dcp, 'static const struct apple_rtkit_ops external_h17p_rtkit_ops =')
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint64_t u64;
#define APPLE_RTKIT_EP_SYSLOG 2
struct buffer { size_t size; unsigned char *buffer; bool iomem; };
struct apple_rtkit_ops {
    bool (*syslog_is_debug)(void *, const char *, const char *);
};
struct apple_rtkit {
    void *dev, *cookie;
    struct apple_rtkit_ops *ops;
    bool syslog_inherited;
    char *syslog_msg_buffer;
    size_t syslog_msg_size, syslog_n_entries;
    struct buffer syslog_buffer;
};
static unsigned infos, debugs, warnings, copies, acks, calls;
#define dev_info(...) (++infos)
#define dev_dbg(...) (++debugs)
#define dev_warn_ratelimited(...) (++warnings)
static void apple_rtkit_memcpy(struct apple_rtkit *rtk, void *out,
                              struct buffer *b, size_t off, size_t len)
{ (void)rtk; assert(off + len <= b->size); copies++; memcpy(out, b->buffer+off, len); }
static int apple_rtkit_send_message(struct apple_rtkit *rtk, u8 ep, u64 msg, void *c, bool atomic)
{ (void)rtk; assert(ep==2 && msg==0 && c==NULL && !atomic); acks++; return 0; }
''' + helper + '\n' + block(dcp, 'static bool dcp_rtk_syslog_is_debug(') + r'''
static bool policy(void *cookie, const char *context, const char *message)
{ calls++; return dcp_rtk_syslog_is_debug(cookie, context, message); }
''' + block(source, 'static bool should_crop_syslog_char(') + '\n' + block(source, 'static void apple_rtkit_syslog_rx_log(') + r'''
static void reset(void) { infos=debugs=warnings=copies=acks=calls=0; }
static void record(struct apple_rtkit *rtk, const char *context, const char *message)
{
    memset(rtk->syslog_buffer.buffer,0,rtk->syslog_buffer.size);
    memcpy(rtk->syslog_buffer.buffer+8,context,strlen(context));
    memcpy(rtk->syslog_buffer.buffer+32,message,strlen(message));
}
int main(void)
{
    unsigned char ring[288]; char decoded[256];
    struct apple_rtkit_ops ops={.syslog_is_debug=policy};
    struct apple_rtkit rtk={.ops=&ops, .syslog_msg_buffer=decoded,
        .syslog_msg_size=256, .syslog_n_entries=1,
        .syslog_buffer={.size=sizeof(ring),.buffer=ring}};
    const char *messages[]={
        "set_run_mode_safe: deferring: 4 -> 1",
        "set_run_mode_safe: deferring: 2 -> 4",
        "ready_for_run_mode_change: initiating deferred run mode change: 4 -> 1",
        "ready_for_run_mode_change: initiating deferred run mode change: 2 -> 4"
    };
    for (unsigned i=0;i<4;i++) {
        const char *ctx=i<2?"UPPipeDCP_H17P.cpp:2096":"UPPipeDCP_H17P.cpp:2014";
        record(&rtk,ctx,messages[i]); reset(); apple_rtkit_syslog_rx_log(&rtk,0);
        assert(debugs==1 && !infos && !warnings && acks==1 && copies==2 && calls==1);
        char fault[192]; snprintf(fault,sizeof(fault),"%s: timeout",messages[i]);
        record(&rtk,ctx,fault); reset(); apple_rtkit_syslog_rx_log(&rtk,0);
        assert(infos==1 && !debugs && acks==1);
        record(&rtk,"other.cpp:2096",messages[i]); reset(); apple_rtkit_syslog_rx_log(&rtk,0);
        assert(infos==1 && !debugs && acks==1);
    }
    puts("PASS complete transition records use debug; faults and foreign contexts use info; all ACK");
    record(&rtk,"UPPipeDCP_H17P.cpp:2096","set_run_mode_safe: deferring: 4 -> 1\r\n ");
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(debugs==1 && acks==1); /* Existing trimming precedes policy. */
    ops.syslog_is_debug=NULL;
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(infos==1 && !debugs && !calls && acks==1);
    ops.syslog_is_debug=policy;
    record(&rtk,"UPPipeDCP_H17P.cpp:2096","set_run_mode_safe: error -110");
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(infos==1 && !debugs && acks==1);
    rtk.syslog_inherited=true;
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(!calls && !copies && !infos && !debugs && acks==1);
    rtk.syslog_inherited=false; rtk.syslog_n_entries=0;
    reset(); apple_rtkit_syslog_rx_log(&rtk,0);
    assert(warnings==1 && !calls && !copies && acks==1);
    puts("PASS default info, trimming, inherited and invalid records retain transport behavior");
    return 0;
}
'''
