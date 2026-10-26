#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual T6050 combo coordination with hardware dependency doubles."""
from pathlib import Path
import subprocess,re,tempfile,shutil
ROOT=Path(__file__).resolve().parents[4]
s=(ROOT/'drivers/phy/apple/atc-t6050-combo.h').read_text()
def fn(name):
 m=re.search(r'(?m)^static (?:int|void) '+name+r'\(',s);assert m,name;i=s.index('{',m.start());depth=1;j=i+1
 while depth:depth+=(s[j]=='{')-(s[j]=='}');j+=1
 return s[m.start():j]
names=['combo_quiesce','combo_stop','combo_start','combo_orientation','combo_mux','combo_usb2_mode','combo_usb2_reset','combo_usb3_mode','combo_usb3_poweroff','combo_reset_assert','combo_reset_deassert']
preamble=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef uint32_t u32;
#define guard(x) fake_guard
#define lockdep_assert_held(x) ((void)0)
#define container_of(ptr,type,member) ((type*)((char*)(ptr)-offsetof(type,member)))
#define USB_TYPEC_DP_SID 0xff01
#define TYPEC_STATE_SAFE 0
#define TYPEC_STATE_USB 1
#define TYPEC_DP_STATE_C 2
#define TYPEC_DP_STATE_E 3
#define TYPEC_DP_STATE_D 4
static void fake_guard(void*p){}
enum typec_orientation{TYPEC_ORIENTATION_NONE,TYPEC_ORIENTATION_NORMAL,TYPEC_ORIENTATION_REVERSE};
enum phy_mode{PHY_MODE_USB_HOST,PHY_MODE_USB_DEVICE};
enum combo_mode{COMBO_SAFE,COMBO_USB,COMBO_DP4,COMBO_DP2};
struct typec_altmode{unsigned svid;};struct typec_mux_state{unsigned mode;struct typec_altmode*alt;};
struct phy{void *data;};static void*phy_get_drvdata(struct phy*p){return p->data;}
struct reset_controller_dev{int dummy;};struct atc_domain{bool active;};
struct apple_t6050_usb_io{void *context;};struct apple_t6050_usb_platform{void*context;};
struct t6050_dp{bool faulted,combo,controller_active,pipe_active,usb_prepared,usb2_active,enabled,reverse,multifunction;enum combo_mode combo_mode;enum typec_orientation orientation;struct atc_domain domain[3];int lock;struct apple_t6050_usb_io usb_io;struct apple_t6050_usb_platform usb_platform;struct reset_controller_dev reset;void *banks[45];void*pipe_default;};
static char logbuf[2048];static int offset,fail_code;static const char*fail_at;
static int logstep(const char*s){offset+=snprintf(logbuf+offset,sizeof(logbuf)-offset,"%s ",s);return fail_at && !strcmp(fail_at,s)?fail_code:0;}
static int atc_domain_set(struct atc_domain*d,bool active){if(d->active==active)return 0;int r=logstep(active?"domain-on":"domain-off");if(!r)d->active=active;return r;}
static int atc_start(struct t6050_dp*d){int r=logstep(d->multifunction?"dp2-start":"dp4-start");if(!r)d->enabled=true;return r;}
static int atc_stop(struct t6050_dp*d){if(!d->enabled)return 0;int r=logstep(d->multifunction?"dp2-stop":"dp4-stop");if(!r)d->enabled=false;return r;}
static const char apple_t6050_usb3_shutdown[]="usb3-stop",apple_t6050_usb2_host[]="usb2-start",apple_t6050_usb3_init_reverse[]="usb3-reverse",apple_t6050_usb3_init_normal[]="usb3-normal";
#define APPLE_T6050_USB_RUN(p,seq) logstep(seq)
static int apple_t6050_usb2_stop(void*p){return logstep("usb2-stop");}
static int apple_t6050_usb3_pipe_stop(void*p,int(*cb)(void*),void*ctx){int r=logstep("pipe-lock");if(!r)r=cb(ctx);if(!r)r=logstep("pipe-dummy");return r;}
static int apple_t6050_usb3_pipe_switch(void*p){return logstep("pipe-usb3");}
static int apple_t6050_usb3_pipe_cold_dummy(void*p){return logstep("reset-dummy");}
static void apple_t6050_usb_mask(void*p,int b,int o,int m,int v){logstep(v?"reset-bit-on":"reset-bit-off");}
static int apple_t6050_usb2_port_reset(void*io,void*ctx,void(*sleep)(void*,unsigned)){return logstep("usb2-reset");}
static void combo_sleep(void*p,unsigned n){}
static void apple_tunable_apply(void*b,void*t){logstep("pipe-tunable");}
static unsigned readl(void*p){return 0x10;}
'''
tests=r'''
static struct t6050_dp setup(void){struct t6050_dp d={.combo=true};return d;}
static void clearlog(void){offset=0;logbuf[0]=0;fail_at=NULL;fail_code=-EIO;}
static struct typec_altmode alt={USB_TYPEC_DP_SID};
static int mode(struct t6050_dp*d,unsigned mode){struct typec_mux_state s={mode,mode>=2?&alt:NULL};return combo_mux(d,&s);}
static void attach_usb(struct t6050_dp*d){assert(combo_orientation(d,TYPEC_ORIENTATION_NORMAL)==0);assert(mode(d,TYPEC_STATE_USB)==0);struct phy p={d};assert(combo_usb2_mode(&p,PHY_MODE_USB_HOST,0)==0);assert(combo_reset_deassert(&d->reset,0)==0);assert(combo_usb3_mode(&p,PHY_MODE_USB_HOST,0)==0);assert(d->pipe_active);}
static void stop_usb(struct t6050_dp*d){struct phy p={d};assert(combo_usb3_poweroff(&p)==0);assert(!d->pipe_active);assert(combo_reset_assert(&d->reset,0)==0);assert(!d->controller_active);}
static void test_usb_dp_usb(void){struct t6050_dp d=setup();clearlog();attach_usb(&d);clearlog();assert(mode(&d,TYPEC_DP_STATE_C)==-EBUSY);assert(d.pipe_active&&!d.enabled);assert(logbuf[0]==0);stop_usb(&d);assert(mode(&d,TYPEC_DP_STATE_C)==0);assert(strstr(logbuf,"pipe-lock usb3-stop pipe-dummy")&&strstr(logbuf,"dp4-start"));assert(d.enabled&&!d.usb_prepared);clearlog();assert(mode(&d,TYPEC_STATE_USB)==0);assert(strstr(logbuf,"dp4-stop")<strstr(logbuf,"usb3-normal"));assert(d.usb_prepared&&!d.enabled);puts("# USB -> DP -> USB full lifecycle PASS");}
static void test_safe_disconnect(void){struct t6050_dp d=setup();clearlog();attach_usb(&d);assert(mode(&d,TYPEC_STATE_SAFE)==-EBUSY);assert(combo_orientation(&d,TYPEC_ORIENTATION_NONE)==-EBUSY);stop_usb(&d);assert(mode(&d,TYPEC_STATE_SAFE)==0);assert(combo_orientation(&d,TYPEC_ORIENTATION_NONE)==0);assert(!d.enabled&&!d.usb_prepared&&!d.usb2_active&&!d.domain[0].active&&!d.domain[1].active);puts("# active-controller SAFE/orientation exclusion PASS");}
static void test_reverse_and_redundant(void){struct t6050_dp d=setup();clearlog();assert(combo_orientation(&d,TYPEC_ORIENTATION_REVERSE)==0);assert(mode(&d,TYPEC_STATE_USB)==0);assert(strstr(logbuf,"usb3-reverse"));clearlog();assert(mode(&d,TYPEC_STATE_USB)==0);assert(combo_orientation(&d,TYPEC_ORIENTATION_REVERSE)==0);assert(logbuf[0]==0);puts("# reverse orientation and redundant callback PASS");}
static void test_dp2_shared(void){struct t6050_dp d=setup();clearlog();assert(combo_orientation(&d,TYPEC_ORIENTATION_NORMAL)==0);assert(mode(&d,TYPEC_DP_STATE_D)==0);assert(strstr(logbuf,"dp2-start")&&!strstr(logbuf,"usb3-normal"));struct phy p={&d};assert(combo_reset_deassert(&d.reset,0)==0);assert(combo_usb3_mode(&p,PHY_MODE_USB_HOST,0)==0);clearlog();stop_usb(&d);assert(strstr(logbuf,"pipe-lock dp2-stop pipe-dummy"));assert(!strstr(logbuf,"usb3-stop"));assert(mode(&d,TYPEC_STATE_USB)==0);puts("# multifunction selects DP analog backend and ordered PIPE teardown PASS");}
static void test_dp4_dummy_usb2(void){struct t6050_dp d=setup();clearlog();assert(combo_orientation(&d,TYPEC_ORIENTATION_NORMAL)==0);assert(mode(&d,TYPEC_DP_STATE_E)==0);struct phy p={&d};assert(combo_usb2_mode(&p,PHY_MODE_USB_HOST,0)==0);assert(combo_reset_deassert(&d.reset,0)==0);clearlog();assert(combo_usb3_mode(&p,PHY_MODE_USB_HOST,0)==0);assert(!d.pipe_active&&logbuf[0]==0);assert(combo_usb2_reset(&p)==0);stop_usb(&d);assert(d.enabled);assert(mode(&d,TYPEC_STATE_SAFE)==0);assert(!d.enabled);puts("# DP4 retains dummy PIPE and independent USB2 PASS");}
static void test_failed_transitions_latch(void){for(unsigned i=0;i<4;i++){struct t6050_dp d=setup();clearlog();assert(combo_orientation(&d,TYPEC_ORIENTATION_NORMAL)==0);fail_at=(const char*[]){"domain-on","usb2-start","dp4-start","usb3-normal"}[i];assert(mode(&d,i==3?TYPEC_STATE_USB:TYPEC_DP_STATE_C)==-EIO);assert(d.faulted);clearlog();assert(mode(&d,TYPEC_STATE_USB)==-EIO);assert(combo_orientation(&d,TYPEC_ORIENTATION_REVERSE)==-EIO);assert(logbuf[0]==0);}puts("# partial hardware failures latch ownership PASS");}
static void test_pipe_failure_retains(void){struct t6050_dp d=setup();clearlog();attach_usb(&d);fail_at="pipe-dummy";struct phy p={&d};assert(combo_usb3_poweroff(&p)==-EIO);assert(d.pipe_active&&d.controller_active&&d.faulted);clearlog();assert(mode(&d,TYPEC_DP_STATE_C)==-EIO);assert(logbuf[0]==0);puts("# failed PIPE handoff cannot grant DP ownership PASS");}
static void test_unconnected_and_unsupported(void){struct t6050_dp d=setup();clearlog();assert(mode(&d,TYPEC_STATE_USB)==-ENOLINK);assert(mode(&d,TYPEC_DP_STATE_C)==-ENOLINK);struct typec_mux_state invalid={33,NULL};assert(combo_mux(&d,&invalid)==-EOPNOTSUPP);struct phy p={&d};assert(combo_usb2_mode(&p,PHY_MODE_USB_DEVICE,0)==-EOPNOTSUPP);assert(logbuf[0]==0);puts("# absent cable and unsupported roles fail without MMIO PASS");}
int main(void){test_usb_dp_usb();test_safe_disconnect();test_reverse_and_redundant();test_dp2_shared();test_dp4_dummy_usb2();test_failed_transitions_latch();test_pipe_failure_retains();test_unconnected_and_unsupported();puts("# 8 lifecycle groups PASS");}
'''

print("TAP version 13\n1..1", flush=True)
if not shutil.which("cc"):
    print("ok 1 - Type-C coordination # SKIP cc unavailable")
    raise SystemExit(4)
try:
    with tempfile.TemporaryDirectory(prefix="t6050-combo-") as temporary:
        source=Path(temporary)/"test.c"
        binary=Path(temporary)/"test"
        source.write_text(preamble+'\n'.join(fn(n) for n in names)+tests)
        subprocess.run(["cc","-std=gnu11","-Wall","-Wno-unused-parameter",str(source),"-o",str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
except subprocess.CalledProcessError:
    print("not ok 1 - Type-C shared ownership and transitions")
    raise SystemExit(1)
print("ok 1 - Type-C shared ownership and transitions")
