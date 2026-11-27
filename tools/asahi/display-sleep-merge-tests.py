#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute merged production boundaries with mocked firmware/workqueues."""
from pathlib import Path
import argparse
import tempfile
import json
import re
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path)
args = parser.parse_args()
temporary = tempfile.TemporaryDirectory(prefix='display-sleep-merge-') if args.out is None else None
R = args.out if args.out is not None else Path(temporary.name)
R.mkdir(parents=True, exist_ok=True)
A = ROOT / 'drivers/gpu/drm/apple'
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
results = []


def function(text, name):
    match = re.search(r'(?m)^static [^\n]+\b' + name + r'\([^;{}]*\)\n\{', text)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end] + '\n'


def run(name, code, negative=False, extra=()):
    path = R / (name + '.c')
    path.write_text(code)
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unused-variable',
                    '-fsanitize=address,undefined', *extra, str(path), '-o', str(path.with_suffix(''))], check=True)
    process = subprocess.run([str(path.with_suffix(''))], capture_output=True, text=True)
    (R / (name + '.log')).write_text(process.stdout + process.stderr)
    assert (process.returncode != 0 and 'Assertion' in process.stderr) if negative else process.returncode == 0
    results.append({'name': name, 'compiled': True, 'exit': process.returncode,
                    'expected': 'runtime assertion' if negative else 'PASS'})
    (R / 'boundary-test-results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results[-1]), flush=True)


common = '#include <assert.h>\n#include <stdbool.h>\n#include <stdint.h>\n#include <stddef.h>\n#include <stdlib.h>\n#include <string.h>\n#include <errno.h>\n'
dcp = (A / 'dcp.c').read_text()
prefix = dcp[dcp.index('static int dcp_platform_prepare('):]
prefix = prefix[:prefix.index('\t/*\n\t * J713:')]
v27 = re.search(r'\tif \(pm_suspend_via_firmware\(\) && dcp_v27_sleep_selected\(dcp\)\) \{.*?\n\t\}', prefix, re.S)[0]
native = re.search(r'\tif \(dcp_is_ext26\(dcp\)\) \{.*?\n\t\}', prefix, re.S)[0]
assert prefix.index(v27) < prefix.index(native)
pre = common + r'''
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
struct apple_dcp { bool v27, v27_known, native26, crashed, ext26_blocked, ext26_pm_preparing, ext26_pm_work_off; int ext26_blank_work; };
struct device { struct apple_dcp *d; };
static int invalidations, cancellations, disables; static bool firmware;
static void *dev_get_drvdata(struct device *v) { return v->d; }
static bool pm_suspend_via_firmware(void) { return firmware; }
static bool dcp_v27_sleep_selected(struct apple_dcp *d) { return d->v27; }
static void dcp_v27_sleep_invalidate(struct apple_dcp *d) { invalidations++; }
static int dcp_v27_sleep_known(struct apple_dcp *d) { return d->v27_known ? 0 : -EOPNOTSUPP; }
static bool dcp_is_ext26(struct apple_dcp *d) { return d->native26; }
static void cancel_work_sync(int *w) { cancellations++; }
static void dcp_disable_typec_work(struct apple_dcp *d, bool sync) { disables++; }
'''
tests = r'''
int main(void) {
 struct apple_dcp d={.v27=true,.v27_known=true,.native26=true}; struct device dev={.d=&d}; firmware=true;
 assert(dcp_platform_prepare(&dev)==0 && invalidations==1 && cancellations==0 && !d.ext26_pm_preparing);
 /* A deep sleep whose display sleep the build's table does not carry: refused, nothing else runs. */
 d.v27_known=false; assert(dcp_platform_prepare(&dev)==-EOPNOTSUPP && invalidations==2 && cancellations==0);
 d.v27=false; d.crashed=true; assert(dcp_platform_prepare(&dev)==-EIO && cancellations==0);
 d.crashed=false; assert(dcp_platform_prepare(&dev)==0 && cancellations==1 && disables==1 && d.ext26_pm_work_off);
 d.native26=false; d.v27=true; firmware=false; assert(dcp_platform_prepare(&dev)==42 && invalidations==2);
}
'''
for label, body, negative in [('prepare', prefix, False),
                              ('prepare-drop-v27', prefix.replace(v27, ''), True),
                              ('prepare-ignore-known', prefix.replace('return dcp_v27_sleep_known(dcp);', 'return 0;'), True),
                              ('prepare-drop-native', prefix.replace(native, ''), True),
                              ('prepare-reverse-order', prefix.replace(v27, '').replace(native, native + '\n' + v27), True)]:
    run(label, pre + body + '\nreturn 42;\n}\n' + tests, negative)

sleep = (A / 'dcp-v27-sleep.inc').read_text()
endpoints = re.search(r'static const u8 v27_sleep_endpoints\[\] = \{.*?\};\n', sleep, re.S)[0]
check = function(sleep, 'dcp_v27_sleep_endpoints_check')
opener = function(sleep, 'dcp_v27_sleep_endpoints_open')
preflight = '\tret = dcp_v27_sleep_endpoints_check(dcp);\n\tif (ret)\n\t\treturn ret;\n'
tracked = '\t\tep = READ_ONCE(dcp->sleep_v27_ep[id - 0x20]);\n\t\tif (ep)\n\t\t\tcontinue;\n'
assert opener.count(preflight) == 1 and opener.count(tracked) == 1
# The order reviewed at 7987e8f8: each endpoint checked in the loop that starts the ones before it.
in_loop = opener.replace(preflight, '').replace(tracked, '\t\tif (!apple_rtkit_has_endpoint(dcp->rtk, id))\n\t\t\treturn -ENODEV;\n' + tracked +
                                                '\t\tif (!IS_ERR_OR_NULL(dcp_afkep(dcp, id)))\n\t\t\treturn -EPROTO;\n')
pre = common + r'''
typedef uint8_t u8;
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define ARRAY_SIZE(a) ((int)(sizeof(a)/sizeof((a)[0])))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define WQ_UNBOUND 0
#define AV_AUDIO_ENDPOINT_DESKTOP 0x2b
#define read_poll_timeout(op,val,cond,sleep_us,timeout_us,before,...) ({ (val) = op(__VA_ARGS__); (cond) ? 0 : -ETIMEDOUT; })
struct apple_epic_service_ops { const char *name; };
struct apple_dcp_afkep { u8 endpoint; bool match_epic_name, inline_v2, v27_closed_teardown, sleep_poisoned, sleep_closed; };
struct apple_rtkit { bool present[0x20]; };
struct dcp_v27_sleep { void *ext_wq; bool ext_wq_owned; };
struct apple_dcp { struct apple_rtkit *rtk; struct dcp_v27_sleep *sleep_v27; void *dev;
                   struct apple_dcp_afkep *sleep_v27_ep[0x20], *running[0x20]; };
static const struct apple_epic_service_ops dcp_v27_ext_services[1], dcp_v27_ext_av_services[1];
static bool t6050_ext_services;
static struct apple_dcp_afkep pool[64]; static int inits, starts, controls; static u8 started[64];
static bool dcp_v27_external(struct apple_dcp *d) { return false; }
static bool apple_rtkit_has_endpoint(struct apple_rtkit *r, u8 id) { return r->present[id - 0x20]; }
/* As the driver's: the sleep's own endpoint if tracked, else a ring something else runs. */
static struct apple_dcp_afkep *dcp_afkep(struct apple_dcp *d, u8 id) { return d->sleep_v27_ep[id - 0x20] ?: d->running[id - 0x20]; }
static void *alloc_workqueue(const char *n, int f, int m) { return NULL; }
static int devm_add_action(void *dev, void (*fn)(void *), void *data) { return 0; }
static void dcp_v27_ext_wq_destroy(void *data) {}
static struct apple_dcp_afkep *afk_init(struct apple_dcp *d, u8 id, const struct apple_epic_service_ops *ops) {
 struct apple_dcp_afkep *ep=&pool[inits]; assert(inits < 64); started[inits++]=id; ep->endpoint=id; d->sleep_v27_ep[id - 0x20]=ep; return ep;
}
static int afk_start(struct apple_dcp_afkep *ep) { starts++; return 0; }
static int afk_v27_sleep_check(struct apple_dcp_afkep *ep) { return 0; }
static int afk_v27_sleep_control(struct apple_dcp_afkep *ep, bool open) { assert(open); controls++; return 0; }
''' + endpoints
tests = r'''
static void reset(struct apple_dcp *d) {
 inits=starts=controls=0; memset(pool,0,sizeof(pool)); memset(d->sleep_v27_ep,0,sizeof(d->sleep_v27_ep)); memset(d->running,0,sizeof(d->running));
 for (int i=0;i<ARRAY_SIZE(v27_sleep_endpoints);i++) d->rtk->present[v27_sleep_endpoints[i]-0x20]=true;
}
static void untouched(struct apple_dcp *d) {
 assert(inits==0 && starts==0 && controls==0);
 for (int i=0;i<0x20;i++) assert(!d->sleep_v27_ep[i]);
}
int main(void) {
 struct apple_rtkit rtk={0}; struct dcp_v27_sleep s={0}; struct apple_dcp d={.rtk=&rtk,.sleep_v27=&s};
 struct apple_dcp_afkep foreign={.endpoint=0x2b}, kept={.endpoint=0x28};
 int n=ARRAY_SIZE(v27_sleep_endpoints);
 /* 0x20 would start first; 0x2b, later in the set, runs a ring the sleep does not track. */
 reset(&d); d.running[0x2b-0x20]=&foreign;
 assert(dcp_v27_sleep_endpoints_open(&d)==-EPROTO); untouched(&d); assert(!foreign.sleep_poisoned);
 /* The same with the conflict on the set's last endpoint. */
 reset(&d); d.running[v27_sleep_endpoints[n-1]-0x20]=&foreign;
 assert(dcp_v27_sleep_endpoints_open(&d)==-EPROTO); untouched(&d);
 /* The set's last endpoint missing: refused before the first start too. */
 reset(&d); rtk.present[v27_sleep_endpoints[n-1]-0x20]=false;
 assert(dcp_v27_sleep_endpoints_open(&d)==-ENODEV); untouched(&d);
 /* An endpoint the sleep already tracks is kept, not refused; the rest start in order. */
 reset(&d); d.sleep_v27_ep[0x28-0x20]=&kept;
 assert(dcp_v27_sleep_endpoints_open(&d)==0 && inits==n-1 && starts==n-1 && controls==n-1 && !kept.sleep_poisoned);
 for (int i=0,j=0;i<n;i++) if (v27_sleep_endpoints[i]!=0x28) assert(started[j++]==v27_sleep_endpoints[i]);
 /* Nothing running: every endpoint starts once; a second open finds them all tracked. */
 reset(&d); assert(dcp_v27_sleep_endpoints_open(&d)==0 && inits==n && starts==n && controls==n);
 assert(dcp_v27_sleep_endpoints_open(&d)==0 && inits==n && starts==n);
}
'''
for label, c, o, negative in [('endpoints', check, opener, False),
                              ('endpoints-check-in-loop', check, in_loop, True),
                              ('endpoints-drop-preflight', check, opener.replace(preflight, ''), True),
                              ('endpoints-drop-running', check.replace('\t\tif (!IS_ERR_OR_NULL(dcp_afkep(dcp, id)))\n\t\t\treturn -EPROTO;\n', ''), opener, True),
                              ('endpoints-refuse-tracked', check.replace('\t\tif (READ_ONCE(dcp->sleep_v27_ep[id - 0x20]))\n\t\t\tcontinue;\n', ''), opener, True)]:
    assert negative == (c + o != check + opener)
    run(label, pre + c + o + tests, negative)

iomfb = (A / 'iomfb.c').read_text()
start = iomfb.index('\tif (out_len && dcp->push_out_prefill)')
padding = iomfb[start:iomfb.index('\n\ttrace_iomfb_push(', start)]
pre = common + r'''
struct apple_dcp { bool native26; const void *push_out_prefill; };
static bool dcp_is_ext26(struct apple_dcp *d) { return d->native26; }
'''
tests = r'''
int main(void) {
 unsigned char data[8], preset[4]={1,2,3,4}; struct apple_dcp d={.native26=true};
 memset(data,0x55,sizeof(data)); fill(&d,data,2,4); for(int i=2;i<6;i++) assert(data[i]==0);
 d.native26=false; fill(&d,data,2,4); for(int i=2;i<6;i++) assert(data[i]==0xff);
 d.push_out_prefill=preset; fill(&d,data,2,4); assert(!memcmp(data+2,preset,4));
 d.native26=true; fill(&d,data,2,4); assert(!memcmp(data+2,preset,4));
 fill(&d,NULL,0,0);
}
'''
for label, body, negative in [('padding', padding, False),
                              ('padding-drop-native', padding.replace('dcp_is_ext26(dcp) ? 0 : 0xff', '0xff'), True),
                              ('padding-drop-prefill', padding.replace('if (out_len && dcp->push_out_prefill)', 'if (false && out_len && dcp->push_out_prefill)'), True)]:
    run(label, pre + 'static void fill(struct apple_dcp *dcp,void *out_data,size_t in_len,size_t out_len) {\n' + body + '\n}\n' + tests, negative)

rtkit = (ROOT / 'drivers/soc/apple/rtkit.c').read_text()
dispatch = function(rtkit, 'apple_rtkit_rx_dispatch')
done = function(rtkit, 'apple_rtkit_saved_job_done')
worker = function(rtkit, 'apple_rtkit_rx_work')
pre = common + r'''
typedef uint8_t u8; typedef uint64_t u64;
#define READ_ONCE(x) (x)
#define IS_ENABLED(x) 1
#define dev_warn(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define dev_err(...) ((void)0)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define APPLE_RTKIT_EP_MGMT 0
#define APPLE_RTKIT_EP_CRASHLOG 1
#define APPLE_RTKIT_EP_SYSLOG 2
#define APPLE_RTKIT_EP_IOREPORT 3
#define APPLE_RTKIT_EP_DEBUG 4
#define APPLE_RTKIT_EP_COREANALYTICS 5
#define APPLE_RTKIT_EP_OSLOG 6
#define APPLE_RTKIT_APP_ENDPOINT_START 0x20
struct work_struct { int unused; };
struct apple_rtkit_ops { void (*recv_message)(void *,u8,u64); void (*recv_message_timed)(void *,u8,u64,u64); };
struct apple_rtkit { bool shutting_down,sync_syslog_ack,pmp_registry_v2; void *cookie,*dev; const struct apple_rtkit_ops *ops; struct { bool tracked; int error,jobs; } m5_saved; int v27_jobs; };
struct apple_rtkit_rx_work { struct apple_rtkit *rtk; u8 ep; u64 msg,received_ns; struct work_struct work; };
static int frees,timed,legacy,clock_calls; static u64 observed;
#define kfree(p) do { frees++; free(p); } while(0)
static int atomic_dec_return_release(int *p) { return --*p; }
static u64 ktime_get_ns(void) { clock_calls++; return 37; }
#define STUB(n) static void n(struct apple_rtkit *r,u64 m) {}
STUB(apple_rtkit_management_rx) STUB(apple_rtkit_crashlog_rx) STUB(apple_rtkit_syslog_rx)
STUB(apple_rtkit_ioreport_rx) STUB(apple_rtkit_kdebug_rx) STUB(apple_rtkit_pmp_aux_rx)
STUB(apple_rtkit_coreanalytics_rx) STUB(apple_rtkit_oslog_rx)
static bool apple_rtkit_ep7_served(struct apple_rtkit *r) { return false; }
static void apple_rtkit_unserved_ep_msg(struct apple_rtkit *r,u8 e,u64 m) {}
static void timed_cb(void *c,u8 e,u64 m,u64 ns) { assert(e==0x2a && m==0x1234); timed++; observed=ns; }
static void legacy_cb(void *c,u8 e,u64 m) { assert(e==0x2a && m==0x1234); legacy++; }
'''
tests = r'''
static void queue(struct apple_rtkit *r) {
 struct apple_rtkit_rx_work *w=calloc(1,sizeof(*w)); assert(w);
 r->m5_saved.jobs=r->v27_jobs=1; r->m5_saved.tracked=true;
 w->rtk=r; w->ep=0x2a; w->msg=0x1234; w->received_ns=9; apple_rtkit_rx_work(&w->work);
 assert(r->m5_saved.jobs==0 && r->v27_jobs==0);
}
int main(void) {
 const struct apple_rtkit_ops both={.recv_message=legacy_cb,.recv_message_timed=timed_cb};
 const struct apple_rtkit_ops old={.recv_message=legacy_cb};
 const struct apple_rtkit_ops timed_only={.recv_message_timed=timed_cb};
 struct apple_rtkit r={.ops=&both}; queue(&r);
 assert(timed==1 && legacy==0 && observed==9 && clock_calls==0 && frees==1);
 apple_rtkit_rx_dispatch(&r,0x2a,0x1234,true,0); assert(timed==1 && legacy==1 && clock_calls==0);
 r.ops=&timed_only; apple_rtkit_rx_dispatch(&r,0x2a,0x1234,true,0); assert(timed==2 && observed==0 && clock_calls==0);
 r.ops=&old; queue(&r); assert(legacy==2 && frees==2);
 r.ops=&both; r.shutting_down=true; queue(&r); assert(timed==2 && frees==3);
 apple_rtkit_rx_dispatch(&r,0x2a,0x1234,true,0); assert(r.m5_saved.error==-ESHUTDOWN && clock_calls==0);
}
'''
start = dispatch.index('\t\tif (received_ns && rtk->ops->recv_message_timed)')
end = dispatch.index('\t\telse\n\t\t\tdev_warn', start)
fabricated_dispatch = dispatch[:start] + '''\t\tif (rtk->ops->recv_message_timed)
\t\t\trtk->ops->recv_message_timed(rtk->cookie, ep, msg,
\t\t\t\t\t\t     received_ns ?: ktime_get_ns());
\t\telse if (rtk->ops->recv_message)
\t\t\trtk->ops->recv_message(rtk->cookie, ep, msg);
''' + dispatch[end:]
for label, d, w, negative in [('rtkit', dispatch, worker, False),
                             ('rtkit-fabricated-time-control', fabricated_dispatch, worker, True),
                             ('rtkit-drop-arrival', dispatch, worker.replace('false, w->received_ns', 'false, 0'), True),
                             ('rtkit-drop-timed', dispatch.replace('if (received_ns && rtk->ops->recv_message_timed)', 'if (false && received_ns && rtk->ops->recv_message_timed)'), worker, True),
                             ('rtkit-drop-unknown-delivery', dispatch.replace('else if (rtk->ops->recv_message_timed)', 'else if (false && rtk->ops->recv_message_timed)'), worker, True)]:
    run(label, pre + d + done + w + tests, negative)

template = (A / 'iomfb_template.c').read_text()
at = template.rindex('\tdcp->swap_submit_timestamp = 0;')
at = template.index('#if DCP_FW_H17(DCP_FW_VER)', at)
end = template.index('#if (DCP_FW_H17(DCP_FW_VER)', at)
mode = template[at:end]
assert 'ext26_open_client' in mode and 'if (keep_mode)' in mode and '!dcp_is_ext26(dcp)' in mode
pre = common + r'''
#define DCP_FW_H17(v) 1
#define DCP_FW_VERSION(a,b,c) a
#define DCP_FW_NAME(n) n
#define DCP_IOMFB_METHODS_H17P_EXTERNAL 2
#define dev_info(...) ((void)0)
static bool DCP_INHERIT_BOOT_MODE;
struct apple_dcp { bool native26; struct { int iomfb_method_profile; } hw; };
struct cookie { int refcount; };
static int opens,completions,modesets,drops,open_error;
static bool dcp_is_ext26(struct apple_dcp *d) { return d->native26; }
static bool dcp_fw27_keep_panel_mode(struct apple_dcp *d) { return false; }
static int ext26_open_client(struct apple_dcp *d) { opens++; return open_error; }
static void release_wait_cookie(void *p) {}
static void kref_put(int *ref,void (*fn)(void *)) { --*ref; drops++; }
static void complete_set_digital_out_mode(struct apple_dcp *d,void *p,struct cookie *c) { completions++; }
'''
tests = r'''
static void reset(void) { opens=completions=modesets=drops=open_error=0; }
int main(void) {
 struct apple_dcp d={.native26=true}; struct cookie c={.refcount=2}; DCP_INHERIT_BOOT_MODE=true;
 assert(evaluate(&d,false,&c)==0 && completions==0 && modesets==1);
#if DCP_FW_VER < 27
 assert(opens==1); reset(); open_error=-EPROTO;
 assert(evaluate(&d,false,&c)==-EPROTO && drops==2 && c.refcount==0 && completions==0);
#endif
 reset(); d.native26=false; d.hw.iomfb_method_profile=2;
 assert(evaluate(&d,false,&c)==0 && modesets==1 && completions==0);
 reset(); d.hw.iomfb_method_profile=0;
 assert(evaluate(&d,false,&c)==0 && completions==1 && modesets==0);
#if DCP_FW_VER >= 27
 reset(); DCP_INHERIT_BOOT_MODE=false;
 assert(evaluate(&d,true,&c)==0 && completions==1 && modesets==0 && opens==0);
#endif
}
'''
for label, body, version, negative in [('mode26', mode, 26, False), ('mode27', mode, 27, False),
                                     ('mode-drop-native', mode.replace('\t    !dcp_is_ext26(dcp) &&\n', ''), 26, True),
                                     ('mode-drop-keep', mode.replace('if (keep_mode)', 'if (false && keep_mode)'), 27, True),
                                     ('mode-drop-open', mode.replace('ret = ext26_open_client(dcp);', 'ret = 0;'), 26, True)]:
    code = pre + 'static int evaluate(struct apple_dcp *dcp,bool keep_mode,struct cookie *cookie) { int ret;\n' + body + '{ modesets++; }\nreturn 0;\n}\n' + tests
    run(label, code, negative, extra=(f'-DDCP_FW_VER={version}',))

assert len(results) == 23
print('PASS: 23 compiled merge-boundary runs, including runtime controls', flush=True)
