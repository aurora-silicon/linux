#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Scan-record bounds and reporting controls for brcmfmac."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = 'drivers/net/wireless/broadcom/brcm80211/brcmfmac/'


def function(source, name):
    pattern = r'static\s+(?:bool|s32|struct brcmf_bss_info_le \*)\s*' + name + r'\('
    match = re.search(pattern, source)
    if not match:
        return ''
    start = match.start()
    opening = source.index('{', match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def structure(source, name):
    start = source.index('struct ' + name + ' {')
    end = source.index(';', source.index('\n}', start)) + 1
    return source[start:end] + '\n'


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8; typedef int8_t s8; typedef uint16_t u16;
typedef int16_t s16; typedef uint32_t u32; typedef int32_t s32;
typedef uint16_t __le16; typedef uint32_t __le32;
#define __packed __attribute__((packed))
#define ETH_ALEN 6
#define IEEE80211_MAX_SSID_LEN 32
#define BRCMF_MCSSET_LEN 16
#define le16_to_cpu(v) (v)
#define le32_to_cpu(v) (v)
#define BRCMF_ESCAN_BUF_SIZE 65000
#define WL_BSS_INFO_MAX 2048
#define GFP_KERNEL 0
#define BRCMF_BSS_RSSI_ON_CHANNEL 4
#define NL80211_IFTYPE_ADHOC 0
#define BIT(n) (1U<<(n))
#define WLAN_CAPABILITY_IBSS 2
#define BRCMF_SCAN_STATUS_BUSY 0
#define BRCMF_E_STATUS_ABORT 1
#define BRCMF_E_STATUS_PARTIAL 2
#define BRCMF_E_STATUS_SUCCESS 0
#define WL_ESCAN_STATE_IDLE 0
#define CFG80211_BSS_FTYPE_UNKNOWN 0
#define bphy_err(...) ((void)0)
#define brcmf_err(...) ((void)0)
#define brcmf_dbg(...) ((void)0)
#define CHSPEC_IS6G(ch) (((ch)&0xf000)==0x4000)
#define CHSPEC_IS5G(ch) (((ch)&0xf000)==0xc000)
#define test_bit(n,p) ((*(p)&BIT(n))!=0)
enum nl80211_band {NL80211_BAND_2GHZ,NL80211_BAND_5GHZ,NL80211_BAND_6GHZ};
struct ieee80211_channel {int value;};
struct wiphy {u32 interface_modes;};
struct cfg80211_bss {int unused;};
struct brcmu_chan {u16 chspec; u8 control_ch_num; int band;};
struct cfg80211_inform_bss {struct ieee80211_channel *chan; long boottime_ns; int signal;};
struct brcmf_cfg80211_info;
struct brcmf_pub {struct brcmf_cfg80211_info *config;};
struct brcmf_if {struct brcmf_pub *drvr;int bsscfgidx;};
struct brcmf_event_msg {u32 status,datalen;};
struct brcmf_cfg80211_info {
 struct brcmf_pub *pub;struct wiphy wiphy;
 struct {void (*decchspec)(struct brcmu_chan *);} d11inf;
 struct {unsigned char escan_buf[BRCMF_ESCAN_BUF_SIZE];int escan_state;struct brcmf_if *ifp;} escan_info;
 unsigned long scan_status;int int_escan_map;void *scan_request;
};
static int reported,p2p_calls,completed;static u8 captured[64];static size_t captured_len;
static struct cfg80211_bss result;static struct ieee80211_channel channel;
static struct wiphy *cfg_to_wiphy(struct brcmf_cfg80211_info *cfg){return &cfg->wiphy;}
static int ieee80211_channel_to_frequency(int ch,enum nl80211_band b){return 2407+ch*5+b*1000;}
static struct ieee80211_channel *ieee80211_get_channel(struct wiphy *w,int f){(void)w;(void)f;return &channel;}
static long ktime_get_boottime(void){return 1;}
static long ktime_to_ns(long t){return t;}
static struct cfg80211_bss *cfg80211_inform_bss_data(struct wiphy *w,struct cfg80211_inform_bss *d,int t,const u8 *b,int ts,u16 cap,u16 period,const u8 *ie,size_t len,int flags){
 (void)w;(void)d;(void)t;(void)b;(void)ts;(void)cap;(void)period;(void)flags;
 assert(len<=sizeof(captured));memcpy(captured,ie,len);captured_len=len;reported++;return &result;
}
static void cfg80211_put_bss(struct wiphy *w,struct cfg80211_bss *b){(void)w;(void)b;}
static bool brcmf_p2p_scan_finding_common_channel(struct brcmf_cfg80211_info *cfg,struct brcmf_bss_info_le *b){(void)cfg;if(b)p2p_calls++;return false;}
static void brcmf_notify_escan_complete(struct brcmf_cfg80211_info *c,struct brcmf_if *i,bool a,bool f){(void)c;(void)i;(void)a;(void)f;completed++;}
static void decode(struct brcmu_chan *ch){ch->control_ch_num=ch->chspec&255;ch->band=(ch->chspec&0xf000);}
'''
CONTROLS = r'''
static struct brcmf_cfg80211_info cfg;
static struct brcmf_pub pub;
static struct brcmf_if ifp;
static u8 event_storage[1024];
static u8 *ev;
static void reset(void){
 memset(&cfg,0,sizeof(cfg));memset(event_storage,0,sizeof(event_storage));
 pub.config=&cfg;cfg.pub=&pub;ifp.drvr=&pub;
 cfg.d11inf.decchspec=decode;cfg.wiphy.interface_modes=BIT(NL80211_IFTYPE_ADHOC);
 cfg.scan_status=BIT(BRCMF_SCAN_STATUS_BUSY);cfg.scan_request=&cfg;
 ((struct brcmf_scan_results *)cfg.escan_info.escan_buf)->buflen=sizeof(struct brcmf_scan_results);
 reported=p2p_calls=completed=0;captured_len=0;ev=event_storage;
}
static void put16(u8 *p,u16 v){p[0]=v;p[1]=v>>8;}
static void put32(u8 *p,u32 v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static void record(u8 *p,u32 version,u32 fixed){
 /* The firmware record is encoded independently of the driver C structure. */
 memset(p,0,fixed+3);put32(p,version);put32(p+4,fixed+3);
 p[8]=2;put16(p+14,100);put16(p+16,1);p[18]=3;memcpy(p+19,"air",3);
 put32(p+52,2);p[56]=2;p[57]=4;put16(p+72,0x1006);put16(p+78,(u16)-47);
 put16(p+116,fixed);put32(p+120,3);p[fixed]=0;p[fixed+1]=1;p[fixed+2]='x';
}
static struct brcmf_scan_results *list_one(u32 version,u32 fixed){
 struct brcmf_scan_results *l=(void *)cfg.escan_info.escan_buf;
 l->version=version;l->count=1;l->buflen=sizeof(*l)+fixed+3;
 record((u8 *)l->bss_info_le,version,fixed);return l;
}
static struct brcmf_escan_result_le *event_one(u32 version,u32 fixed,bool unaligned){
 ev=event_storage+(unaligned?1:0);put32(ev,12+fixed+3);put32(ev+4,1);put16(ev+10,1);
 record(ev+12,version,fixed);return (void *)ev;
}
static void partial(u32 len){struct brcmf_event_msg e={BRCMF_E_STATUS_PARTIAL,len};assert(brcmf_cfg80211_escan_handler(&ifp,&e,ev)==0);}
static void positive(u32 version,u32 fixed,bool unaligned){
 reset();event_one(version,fixed,unaligned);partial(12+fixed+3);
 struct brcmf_scan_results *l=(void *)cfg.escan_info.escan_buf;
 assert(p2p_calls==1&&l->count==1);assert(brcmf_inform_bss(&cfg)==0);
 assert(reported==1&&captured_len==3&&captured[2]=='x');
 /* A duplicate goes through the real SSID/RSSI comparison without appending. */
 put16(ev+12+78,(u16)-40);partial(12+fixed+3);assert(l->count==1);
 assert((s16)le16_to_cpu(l->bss_info_le->RSSI)==-40);
}
int main(int argc,char **argv){
 assert(argc==2);const char *t=argv[1];reset();
 assert(offsetof(struct brcmf_bss_info_le,ie_offset)==116);
 assert(offsetof(struct brcmf_bss_info_le,ie_length)==120);
 assert(sizeof(struct brcmf_bss_info_le)==172);
 if(!strcmp(t,"v116"))positive(116,172,false);
 else if(!strcmp(t,"v116-unaligned"))positive(116,172,true);
 else if(!strcmp(t,"v109"))positive(109,128,false);
 else if(!strcmp(t,"v109-long"))positive(109,144,false);
 else if(!strcmp(t,"v110"))positive(110,172,false);
 else if(!strcmp(t,"v111"))positive(111,172,false);
 else if(!strcmp(t,"v112"))positive(112,172,false);
 else if(!strcmp(t,"unaligned-list")){
  struct brcmf_scan_results *l=list_one(116,172);record((u8 *)l->bss_info_le+175,116,172);
  l->buflen+=175;l->count=2;assert(brcmf_inform_bss(&cfg)==0);assert(reported==2);
 }
 else if(!strcmp(t,"count")){struct brcmf_scan_results *l=list_one(116,172);l->count=UINT32_MAX;assert(brcmf_inform_bss(&cfg)!=0&&reported==0);}
 else if(!strcmp(t,"list-short")){struct brcmf_scan_results *l=list_one(116,172);l->buflen=4;assert(brcmf_inform_bss(&cfg)!=0&&reported==0);}
 else if(!strcmp(t,"list-long")){struct brcmf_scan_results *l=list_one(116,172);l->buflen=BRCMF_ESCAN_BUF_SIZE+1;assert(brcmf_inform_bss(&cfg)!=0&&reported==0);}
 else if(!strcmp(t,"second-truncated")){
  struct brcmf_scan_results *l=list_one(116,172);record((u8 *)l->bss_info_le+175,116,172);
  l->count=2;l->buflen+=124;assert(brcmf_inform_bss(&cfg)!=0&&reported==1);
 }
 else if(!strcmp(t,"list-version")){struct brcmf_scan_results *l=list_one(116,172);l->bss_info_le->version=117;assert(brcmf_inform_bss(&cfg)!=0&&reported==0);}
 else if(!strcmp(t,"event-count")){event_one(116,172,false);put16(ev+10,2);partial(187);assert(p2p_calls==0&&reported==0);}
 else if(!strcmp(t,"mixed-version")){
  event_one(116,172,false);partial(187);event_one(112,172,false);ev[12+8]=3;partial(187);
  struct brcmf_scan_results *l=(void *)cfg.escan_info.escan_buf;assert(l->count==2&&l->version==112);assert(brcmf_inform_bss(&cfg)==0&&reported==2);
 }
 else if(!strcmp(t,"empty")){assert(brcmf_inform_bss(&cfg)==0&&reported==0);}
 else {
  event_one(116,172,false);u8 *p=ev+12;u32 event_len=187;
  if(!strcmp(t,"ssid"))p[18]=33;
  else if(!strcmp(t,"rates"))put32(p+52,17);
  else if(!strcmp(t,"ie-header"))put16(p+116,124);
  else if(!strcmp(t,"ie-offset"))put16(p+116,176);
  else if(!strcmp(t,"ie-length"))put32(p+120,4);
  else if(!strcmp(t,"ie-overflow"))put32(p+120,UINT32_MAX);
  else if(!strcmp(t,"short116")){put32(p+4,171);put16(p+116,168);put32(ev,183);event_len=183;}
  else if(!strcmp(t,"length-zero"))put32(p+4,0);
  else if(!strcmp(t,"length-overflow"))put32(p+4,UINT32_MAX);
  else if(!strcmp(t,"event-truncated"))event_len=186;
  else if(!strcmp(t,"event-small"))event_len=123;
  else if(!strcmp(t,"version113"))put32(p,113);
  else if(!strcmp(t,"version114"))put32(p,114);
  else if(!strcmp(t,"version115"))put32(p,115);
  else if(!strcmp(t,"version117"))put32(p,117);
  else if(!strcmp(t,"big-endian")){p[0]=0;p[3]=116;}
  else if(!strcmp(t,"zero-ie")){put32(p+120,0);partial(event_len);assert(p2p_calls==1);assert(brcmf_inform_bss(&cfg)==0&&reported==1&&captured_len==0);return 0;}
  else abort();
  partial(event_len);assert(p2p_calls==0&&((struct brcmf_scan_results *)cfg.escan_info.escan_buf)->count==0);
 }
 puts(t);return 0;
}
'''
CASES = ['v116', 'v116-unaligned', 'v109', 'v109-long', 'v110', 'v111', 'v112',
         'unaligned-list', 'count', 'list-short', 'list-long', 'second-truncated',
         'list-version', 'event-count', 'mixed-version', 'empty', 'ssid', 'rates',
         'ie-header', 'ie-offset', 'ie-length', 'ie-overflow', 'short116',
         'length-zero', 'length-overflow', 'event-truncated', 'event-small',
         'version113', 'version114', 'version115', 'version117', 'big-endian', 'zero-ie']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ref', help='Git source revision to exercise')
    parser.add_argument('--out', type=Path, help='retain generated control executable and results')
    parser.add_argument('cases', nargs='*')
    args = parser.parse_args()
    def read(name):
        if args.ref:
            return subprocess.check_output(['git', 'show', args.ref + ':' + DRIVER + name], cwd=ROOT, text=True)
        return (ROOT / DRIVER / name).read_text()
    source, header = read('cfg80211.c'), read('fwil_types.h')
    structs = ''.join(structure(header, n) for n in ['brcmf_bss_info_le', 'brcmf_scan_results', 'brcmf_escan_result_le'])
    constants = '\n'.join(line for line in header.splitlines() if re.match(r'#define\s+BRCMF_BSS_INFO_', line))
    code = PREFIX.replace('static bool brcmf_p2p_scan', 'struct brcmf_bss_info_le;\nstatic bool brcmf_p2p_scan')
    code += structs + constants + '\n#define WL_ESCAN_RESULTS_FIXED_SIZE 12\n'
    for name in ['brcmf_bss_info_version_supported', 'brcmf_bss_info_valid',
                 'brcmf_inform_single_bss', 'next_bss_le', 'brcmf_inform_bss',
                 'brcmf_compare_update_same_bss', 'brcmf_cfg80211_escan_handler']:
        code += function(source, name)
    code += CONTROLS
    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        src = out / 'controls.c'; src.write_text(code)
        exe = out / 'controls'
        subprocess.run(['clang', '-std=gnu11', '-g', '-O1', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(src), '-o', str(exe)], check=True)
        results = []
        for case in args.cases or CASES:
            result = subprocess.run([str(exe), case], text=True, capture_output=True)
            results.append({'case': case, 'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr})
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        failures = [r for r in results if r['exit']]
        print(f'{len(results)-len(failures)}/{len(results)} production scan controls passed')
        for r in failures:
            print(r['case'], r['exit'], r['stderr'][:800])
        return bool(failures)
    if args.out:
        raise SystemExit(run(args.out))
    with tempfile.TemporaryDirectory(prefix='brcmf-bss-') as td:
        raise SystemExit(run(Path(td)))


if __name__ == '__main__':
    main()
