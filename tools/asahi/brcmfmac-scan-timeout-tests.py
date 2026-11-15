#!/usr/bin/env python3
"""Execute scan entrypoints and timeout cases with mocked firmware and timers."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r'\b' + name + r'\([^;]*?\)\s*\{', source)
    if not match:
        raise ValueError('Missing function ' + name)
    opening = source.index('{', match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    start = source.rfind('\nstatic ', 0, match.start()) + 1
    return source[start:end]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t s32;
#define NUM_NL80211_BANDS 3
#define NL80211_BAND_2GHZ 0
#define NL80211_BAND_5GHZ 1
#define NL80211_BAND_6GHZ 2
#define U32_MAX UINT32_MAX
#define clamp_t(t,v,lo,hi) ((t)(v)<(lo)?(lo):((t)(v)>(hi)?(hi):(t)(v)))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define BRCMF_SCAN_STATUS_BUSY 0
#define BRCMF_SCAN_STATUS_ABORT 1
#define BRCMF_SCAN_STATUS_SUPPRESS 2
#define BRCMF_VIF_STATUS_CONNECTING 0
#define BRCMF_VNDR_IE_PRBREQ_FLAG 1
#define P2PAPI_BSSCFG_DEVICE 0
#define P2PAPI_BSSCFG_PRIMARY 1
#define brcmf_dbg(...) ((void)0)
#define bphy_err(...) ((void)drvr)
#define GFP_KERNEL 0
struct brcmf_cfg80211_info;
struct wireless_dev { int unused; };
struct cfg80211_scan_request { struct wireless_dev *wdev; u32 n_channels; int n_ssids; const void *ie; int ie_len; };
struct ieee80211_supported_band { int n_channels; };
struct wiphy { struct ieee80211_supported_band *bands[3]; struct brcmf_cfg80211_info *cfg; };
struct brcmf_pub { struct brcmf_cfg80211_info *config; };
struct brcmf_if { struct brcmf_pub *drvr; int id; };
struct brcmf_cfg80211_vif { struct wireless_dev wdev; unsigned long sme_state; struct brcmf_if *ifp; bool up; };
struct timer_list { unsigned long expires; unsigned calls; };
typedef int (*run_fn)(struct brcmf_cfg80211_info *,struct brcmf_if *,struct cfg80211_scan_request *);
struct brcmf_cfg80211_info { struct brcmf_pub *pub; unsigned long scan_status; struct cfg80211_scan_request *scan_request; struct {run_fn run;} escan_info; struct {struct {struct brcmf_cfg80211_vif *vif;} bss_idx[2];} p2p; struct timer_list escan_timeout; u32 int_escan_map; };
static struct brcmf_cfg80211_info *wiphy_to_cfg(struct wiphy *w) {return w->cfg;}
static bool check_vif_up(struct brcmf_cfg80211_vif *v) {return v->up;}
static bool test_bit(int b,unsigned long *p) {return (*p>>b)&1;}
static void set_bit(int b,unsigned long *p) {*p|=1UL<<b;}
static void clear_bit(int b,unsigned long *p) {*p&=~(1UL<<b);}
static unsigned long jiffies=100;
static unsigned long msecs_to_jiffies(unsigned long ms) {return ms;}
static void mod_timer(struct timer_list *timer,unsigned long expires) {timer->expires=expires;timer->calls++;}
static int brcmf_run_escan(struct brcmf_cfg80211_info *c,struct brcmf_if *i,struct cfg80211_scan_request *r) {(void)c;(void)i;(void)r;return 0;}
static int p2p_run(struct brcmf_cfg80211_info *c,struct brcmf_if *i,struct cfg80211_scan_request *r) {(void)c;(void)i;(void)r;return 0;}
static int prep_error,ie_error,firmware_error,firmware_calls,abort_calls,sent_id;
static bool use_p2p,change_count;
static int brcmf_p2p_scan_prep(struct wiphy *w,struct cfg80211_scan_request *r,struct brcmf_cfg80211_vif *v) {(void)r;(void)v;if(use_p2p)w->cfg->escan_info.run=p2p_run;return prep_error;}
static int brcmf_vif_set_mgmt_ie(struct brcmf_cfg80211_vif *v,int flag,const void *ie,int len) {(void)v;(void)flag;(void)ie;(void)len;return ie_error;}
static int brcmf_do_escan(struct brcmf_if *i,struct cfg80211_scan_request *r) {firmware_calls++;sent_id=i->id;if(change_count)r->n_channels=1;return firmware_error;}
static void brcmf_abort_scanning(struct brcmf_cfg80211_info *c) {abort_calls++;c->scan_status=0;}
struct kunit {int unused;};
static void *kunit_kzalloc(struct kunit *test,size_t size,int flags) {(void)test;(void)flags;return calloc(1,size);}
#define KUNIT_ASSERT_NOT_NULL(t,p) do {(void)(t);assert(p);}while(0)
#define KUNIT_EXPECT_EQ(t,a,b) do {(void)(t);assert((a)==(b));}while(0)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
'''

MAIN = r'''
static struct brcmf_cfg80211_info cfg;
static struct brcmf_pub pub;
static struct brcmf_if ifp,primary;
static struct brcmf_cfg80211_vif vif,primary_vif;
static struct wiphy wiphy;
static struct cfg80211_scan_request request;
static void reset(void) {
 memset(&cfg,0,sizeof(cfg));memset(&pub,0,sizeof(pub));memset(&vif,0,sizeof(vif));memset(&wiphy,0,sizeof(wiphy));memset(&request,0,sizeof(request));
 pub.config=&cfg;cfg.pub=&pub;ifp=(struct brcmf_if){.drvr=&pub,.id=1};primary=(struct brcmf_if){.drvr=&pub,.id=2};
 vif.up=true;vif.ifp=&ifp;primary_vif.up=true;primary_vif.ifp=&primary;request.wdev=&vif.wdev;wiphy.cfg=&cfg;
 prep_error=ie_error=firmware_error=firmware_calls=abort_calls=0;use_p2p=change_count=false;
}
int main(void) {
 struct kunit test={0};
 brcmf_scan_timeout_channels(&test);brcmf_scan_timeout_all_bands(&test);
 struct {u32 count;unsigned deadline;} cases[]={{1,10000},{37,10000},{38,10120},{97,24280},{233,56920},{246,60000},{17895698,60000},{UINT32_MAX,60000}};
 unsigned controls=2;
 for(unsigned k=0;k<ARRAY_SIZE(cases);k++)for(int active=0;active<2;active++) {
  reset();request.n_channels=cases[k].count;request.n_ssids=active;
  assert(brcmf_cfg80211_scan(&wiphy,&request)==0);assert(cfg.escan_timeout.calls==1);assert(cfg.escan_timeout.expires==jiffies+cases[k].deadline);assert(firmware_calls==1);controls++;
 }
 reset();struct ieee80211_supported_band b[]={ {.n_channels=14},{.n_channels=24},{.n_channels=59} };
 for(int k=0;k<3;k++)wiphy.bands[k]=&b[k];
 assert(brcmf_cfg80211_scan(&wiphy,&request)==0);assert(cfg.escan_timeout.expires==jiffies+24280);controls++;
 reset();request.n_channels=97;use_p2p=true;
 assert(brcmf_cfg80211_scan(&wiphy,&request)==0);assert(cfg.escan_timeout.expires==jiffies+10000);controls++;
 reset();request.n_channels=97;use_p2p=true;cfg.p2p.bss_idx[0].vif=&vif;cfg.p2p.bss_idx[1].vif=&primary_vif;
 assert(brcmf_cfg80211_scan(&wiphy,&request)==0);assert(sent_id==2);assert(cfg.escan_timeout.expires==jiffies+10000);controls++;
 reset();request.n_channels=97;change_count=true;
 assert(brcmf_cfg80211_scan(&wiphy,&request)==0);assert(cfg.escan_timeout.expires==jiffies+24280);controls++;
 for(int k=0;k<5;k++) {
  reset();request.n_channels=97;if(k==0)vif.up=false;else if(k==4)vif.sme_state=1;else cfg.scan_status=1UL<<(k-1);
  assert(brcmf_cfg80211_scan(&wiphy,&request)<0);assert(firmware_calls==0);assert(cfg.escan_timeout.calls==0);controls++;
 }
 for(int k=0;k<3;k++) {
  reset();request.n_channels=97;if(k==0)prep_error=-EIO;else if(k==1)ie_error=-EIO;else firmware_error=-EIO;
  assert(brcmf_cfg80211_scan(&wiphy,&request)==-EIO);assert(!test_bit(0,&cfg.scan_status));assert(cfg.scan_request==NULL);assert(cfg.escan_timeout.calls==0);controls++;
 }
 reset();request.n_channels=97;
 assert(brcmf_start_internal_escan(&ifp,7,&request)==0);assert(cfg.int_escan_map==7);assert(cfg.escan_timeout.calls==0);controls++;
 reset();request.n_channels=97;cfg.scan_status=1;cfg.int_escan_map=3;
 assert(brcmf_start_internal_escan(&ifp,7,&request)==0);assert(abort_calls==1);assert(cfg.int_escan_map==7);assert(cfg.escan_timeout.calls==0);controls++;
 reset();request.n_channels=97;firmware_error=-EIO;
 assert(brcmf_start_internal_escan(&ifp,7,&request)==-EIO);assert(!test_bit(0,&cfg.scan_status));assert(cfg.escan_timeout.calls==0);controls++;
 printf("PASS %u production scan/KUnit-body controls; firmware, clock and timer services mocked\n",controls);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--old-source', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = args.source
    path = root / 'drivers/net/wireless/broadcom/brcm80211/brcmfmac'
    cfg = (path / 'cfg80211.c').read_text()
    helper = re.sub(r'^#include.*\n', '', (path / 'scan_timeout.h').read_text(), flags=re.M)
    tests = (path / 'scan_timeout_test.c').read_text()
    foreground = function(cfg, 'brcmf_cfg80211_scan')
    internal = function(cfg, 'brcmf_start_internal_escan')
    cases = '\n'.join(function(tests, n) for n in ('brcmf_scan_timeout_channels', 'brcmf_scan_timeout_all_bands'))
    out = args.output or Path(tempfile.mkdtemp(prefix='brcmf-scan-controls-'))
    out.mkdir(parents=True, exist_ok=True)
    variants = {'positive': foreground + '\n' + internal}
    if args.old_source:
        old = args.old_source.read_text()
        old_internal = function(old, 'brcmf_start_internal_escan')
        assert old_internal == internal, 'Internal scan changed'
        variants['old-negative'] = function(old, 'brcmf_cfg80211_scan') + '\n' + old_internal
    variants['p2p-mutant'] = foreground.replace('cfg->escan_info.run == brcmf_run_escan ?', 'true ?') + '\n' + internal
    variants['timer-mutant'] = foreground.replace('msecs_to_jiffies(timeout)', 'msecs_to_jiffies(timeout ? BRCMF_ESCAN_TIMER_INTERVAL_MS : timeout)') + '\n' + internal
    variants['overflow-mutant'] = foreground + '\n' + internal
    for name, body in variants.items():
        source = out / (name + '.c')
        selected_helper = helper.replace('u64 n_channels', 'u32 n_channels') if name == 'overflow-mutant' else helper
        source.write_text(PRELUDE + '\n' + selected_helper + '\n' + cases + '\n' + body + '\n' + MAIN)
        binary = out / name
        subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-unused-variable', '-Wno-sign-compare',
                        '-o', str(binary), str(source)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if name == 'positive':
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0, name + ' unexpectedly passed'
            print(name + ': rejected (' + str(result.returncode) + ')')
    print('foreground.sha256=' + hashlib.sha256(foreground.encode()).hexdigest())
    print('internal.sha256=' + hashlib.sha256(internal.encode()).hexdigest())
    print('Scope: real foreground/internal functions and timeout/KUnit case bodies; firmware completion, timer execution and hardware untested.')


if __name__ == '__main__':
    main()
