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
struct apple_dcp_afkep {bool compact;};
struct apple_epic_service {struct apple_dcp_afkep *ep;};
static int transport_error;
static size_t wire_received, used;
static unsigned compact_calls, legacy_calls;
static int transfer(struct apple_epic_service *srv, u16 group, u32 cmd,
                    const void *data, size_t len, size_t pad, void *out,
                    size_t room, size_t out_pad)
{
    assert(srv && cmd && !out_pad);
    (void)pad;
    if (transport_error) return transport_error;
    if (group==1) {
        assert(len==48 && room==64 && data==out);
        __le64 max, count=used;
        memcpy(&max,data,8);assert(max==16);
        memcpy((u8 *)out+32,&count,8);
        memset((u8 *)out+48,0x5a,16);
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
    compact_calls++;*received=wire_received;
    return transfer(srv,group,cmd,data,len,pad,out,room,out_pad);
}
static int afk_service_call(struct apple_epic_service *srv,u16 group,u32 cmd,
        const void *data,size_t len,size_t pad,void *out,size_t room,size_t out_pad)
{
    legacy_calls++;
    return transfer(srv,group,cmd,data,len,pad,out,room,out_pad);
}
'''
    return prefix + block(source, 'static int av_audiosrv_call(') + '\n' + block(
        source, 'static int\ndcp_audiosrv_osobject_call(') + r'''
int main(void)
{
    struct apple_dcp_afkep ep={.compact=true};
    struct apple_epic_service srv={.ep=&ep};
    u8 body[64]={0},reply[64],elements[16];
    size_t actual=0;
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


def cec_only_endpoint(root):
    """AV service connect/disconnect on an endpoint started for CEC alone.

    dcp_cec_init_avep() sets dcp->avep on an HDMI processor whose audio is
    not started (no audio device linked, or appledrm.hdmi_audio=0), so no
    audio owner exists. The display code calls both entry points whenever
    dcp->avep is set: they must make no audio call then, and must still open
    and close the service, classic and native link, when an owner exists.
    """
    source = (root / 'drivers/gpu/drm/apple/av.c').read_text()
    prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define IS_ENABLED(x) x
#define CONFIG_DRM_APPLE_DCP_T6030_AUDIO 1
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define dev_info(d, ...) ((void)(d))
static unsigned errs, armed;
#define dev_err(d, ...) ((void)(d), errs++)
#define AV_AUDIOSRV_REPLACE_MS 1000
static int system_wq;
static unsigned long msecs_to_jiffies(unsigned m) {return m;}
#define mod_delayed_work(wq, w, j) ((void)(wq), (void)(w), (void)(j), armed++)
struct device {int unused;};
struct platform_device {int unused;};
struct lock {int held;};
static unsigned takes;
static struct lock *lock_take(struct lock *l) {assert(!l->held); l->held = 1; takes++; return l;}
static void lock_drop(struct lock **l) {if (*l) (*l)->held = 0;}
#define guard(type) struct lock *_g __attribute__((cleanup(lock_drop))) = lock_take
#define scoped_guard(type, l) \
    for (struct lock *_guard __attribute__((cleanup(lock_drop))) = lock_take(l), *_once = _guard; \
         _once; _once = NULL)
struct apple_epic_service {bool torndown; unsigned pins;};
struct apple_dcp_afkep {int endpoint;};
struct audiosrv_data {
    struct lock srv_rwsem, plug_lock;
    struct apple_epic_service *srv;
    bool plugged, is_open;
    u64 gen, v27_boot_gen;
    struct platform_device *audio_dev;
    struct {u32 open, close;} cmds;
    int open_check_wq, open_refused;
    struct device *open_check_dev;
};
struct apple_dcp {struct device *dev; struct apple_dcp_afkep *avep; struct audiosrv_data *audiosrv;};
static bool native, v27;
static int call_ret, hook;
static struct audiosrv_data *hook_asrv;
static struct apple_epic_service *hook_next;
static unsigned calls, last_cmd, connects, disconnects, releases, closes, v27_starts;
static bool dcp_audiosrv_native_link(struct device *dev) {(void)dev; return native;}
static bool dcp_is_v14_7_external(const struct apple_dcp *dcp) {(void)dcp; return false;}
static bool iomfb_v14_7_external_powered(struct apple_dcp *dcp) {(void)dcp; return true;}
static bool dcp_is_v27_external(const struct apple_dcp *dcp) {(void)dcp; return v27;}
static void av_v27_start(struct apple_dcp *dcp, struct apple_epic_service *srv) {(void)dcp; assert(srv->pins); v27_starts++;}
static void dcpaud_connect(struct platform_device *pdev, bool plugged) {(void)pdev; (void)plugged; connects++;}
static void dcpaud_disconnect(struct platform_device *pdev) {(void)pdev; disconnects++;}
static void dcpaud_release_link(struct platform_device *pdev, const u64 *gen) {(void)pdev; (void)gen; releases++;}
static void av_audiosrv_close(struct apple_dcp *dcp, struct audiosrv_data *asrv, const u64 *gen)
{(void)dcp; (void)gen; assert(asrv && !asrv->plug_lock.held); closes++;}
static struct apple_epic_service *afk_service_pin(struct apple_epic_service *s) {if (s) s->pins++; return s;}
static void afk_service_put(struct apple_epic_service *s) {assert(s && s->pins); s->pins--;}
static void afk_service_disable(struct apple_epic_service *s) {(void)s;}
static int afk_service_call(struct apple_epic_service *s, int group, u32 cmd, const void *data, size_t len,
                            size_t pad, void *out, size_t room, size_t out_pad)
{
    (void)group; (void)data; (void)len; (void)pad; (void)out; (void)room; (void)out_pad;
    assert(s && s->pins); calls++; last_cmd = cmd;
    /* A successor published while the RPC runs (and, at a Close, opened) */
    if (hook) {
        hook_asrv->srv = hook_next;
        hook_asrv->is_open = hook == 2;
        hook = 0;
    }
    return call_ret;
}
'''
    return prefix + block(source, 'static bool av_audiosrv_published(') + '\n' + block(
        source, 'void av_service_connect(') + '\n' + block(
        source, 'static void __av_service_disconnect(') + '\n' + block(
        source, 'void av_service_disconnect(') + r'''
int main(void)
{
    struct device dev = {0};
    struct apple_dcp_afkep ep = {.endpoint = 0x2b};
    struct apple_epic_service srv = {0};
    struct platform_device audio = {0};
    struct audiosrv_data asrv = {.srv = &srv, .plugged = true, .gen = 1, .audio_dev = &audio,
                                 .cmds = {.open = 4, .close = 5}};
    struct apple_dcp dcp = {.dev = &dev, .avep = &ep, .audiosrv = NULL};

    /* A CEC-only endpoint, both link kinds: no audio owner, no audio call. */
    for (int n = 0; n < 2; n++) {
        native = v27 = n;
        av_service_connect(&dcp);
        av_service_disconnect(&dcp);
        av_service_connect(&dcp);
    }
    assert(!takes && !calls && !connects && !disconnects && !releases && !closes && !v27_starts);

    /* An audio owner, classic link: open once, close once, the card follows. */
    native = v27 = false;
    dcp.audiosrv = &asrv;
    av_service_connect(&dcp);
    assert(asrv.is_open && calls == 1 && last_cmd == 4 && connects == 1);
    av_service_connect(&dcp);
    assert(calls == 1);
    av_service_disconnect(&dcp);
    assert(!asrv.is_open && calls == 2 && last_cmd == 5 && disconnects == 1);

    /* An audio owner, firmware 27 native link: one start per instance. */
    native = v27 = true;
    av_service_connect(&dcp);
    av_service_connect(&dcp);
    assert(v27_starts == 1 && asrv.v27_boot_gen == 1 && connects == 3);
    av_service_disconnect(&dcp);
    assert(disconnects == 2 && releases == 1 && closes == 1 && calls == 2);

    /* No published audio service yet: nothing to call. */
    asrv.srv = NULL;
    av_service_connect(&dcp);
    av_service_disconnect(&dcp);
    assert(connects == 3 && disconnects == 2 && calls == 2);

    /*
     * Classic link, the firmware refuses the Open (an instance being
     * replaced): judged later, not reported now; any other error is.
     */
    native = v27 = false;
    asrv.srv = &srv;
    call_ret = -EINVAL;
    av_service_connect(&dcp);
    assert(calls == 3 && !asrv.is_open && armed == 1 && !errs && asrv.open_refused == -EINVAL &&
           connects == 3);
    call_ret = -ETIMEDOUT;
    av_service_connect(&dcp);
    assert(calls == 4 && !asrv.is_open && armed == 1 && errs == 1);
    call_ret = 0;
    av_service_connect(&dcp);
    assert(calls == 5 && asrv.is_open && connects == 4);

    /* A Close whose instance is replaced meanwhile leaves the successor's Open. */
    struct apple_epic_service next = {0};
    hook_asrv = &asrv;
    hook_next = &next;
    hook = 2;
    av_service_disconnect(&dcp);
    assert(calls == 6 && last_cmd == 5 && asrv.srv == &next && asrv.is_open);

    /* An Open whose instance is replaced meanwhile is not the successor's. */
    asrv.srv = &srv;
    asrv.is_open = false;
    hook_next = &next;
    hook = 1;
    av_service_connect(&dcp);
    assert(calls == 7 && last_cmd == 4 && asrv.srv == &next && !asrv.is_open && connects == 4);
    assert(!next.pins);
    asrv.srv = &srv;
    assert(!srv.pins && !asrv.srv_rwsem.held && !asrv.plug_lock.held);
    return 0;
}
'''
