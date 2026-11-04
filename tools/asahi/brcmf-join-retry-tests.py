#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise real join encoders and bounded BSS-hint retry/cancellation code."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'drivers/net/wireless/broadcom/brcm80211/brcmfmac'


def function(source, name):
    match = re.search(r'static\s+(?:void\s*\*|void|int|bool|s32)\s*' + name + r'\(', source)
    if not match:
        raise ValueError('Missing function ' + name)
    end = source.index('{', match.end()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


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
#include <pthread.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;
typedef uint16_t __le16; typedef uint32_t __le32;
#define ETH_ALEN 6
#define IEEE80211_MAX_SSID_LEN 32
#define GFP_KERNEL 0
#define cpu_to_le16(x) ((uint16_t)(x))
#define cpu_to_le32(x) ((uint32_t)(x))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define struct_size(p,m,n) (sizeof(*(p))+sizeof((p)->m[0])*(n))
#define bphy_err(...) ((void)0)
#define brcmf_dbg(...) ((void)0)
#define BRCMF_BUS_UP 1
#define BRCMF_VIF_STATUS_CONNECTING 0
#define BRCMF_VIF_STATUS_CONNECTED 1
#define BRCMF_VIF_STATUS_EAP_SUCCESS 2
#define BRCMF_VIF_STATUS_ASSOC_SUCCESS 3
#define BIT(n) (1UL<<(n))
#define test_bit(n,p) ((*(p)&BIT(n))!=0)
#define clear_bit(n,p) (*(p)&=~BIT(n))
#define set_bit(n,p) (*(p)|=BIT(n))
static bool test_and_clear_bit(unsigned n,unsigned long *p){bool v=test_bit(n,p);clear_bit(n,p);return v;}
#define BRCMF_C_DISASSOC 52
#define BRCMF_PROFILE_FWSUP_PSK 1
#define BRCMF_PROFILE_FWSUP_SAE 2
#define WLAN_STATUS_SUCCESS 0
#define WLAN_STATUS_AUTH_TIMEOUT 16
static unsigned allocations, commands, reports;
static int allocation_failure, command_failure;
static void *kzalloc(size_t n, int flags) {
 (void)flags;if(allocation_failure)return NULL;
 void *p=calloc(1,n);assert(p);allocations++;return p;
}
static void kfree(void *p) {if(p){assert(allocations);allocations--;free(p);}}
static void eth_broadcast_addr(u8 *p) {memset(p,255,6);}
struct ieee80211_channel {u16 chanspec;};
struct cfg80211_chan_def {struct ieee80211_channel *chan;};
struct cfg80211_ibss_params {const u8 *ssid,*bssid;u32 ssid_len;struct cfg80211_chan_def chandef;};
struct cfg80211_connect_params {struct ieee80211_channel *channel,*channel_hint;
 const u8 *bssid,*bssid_hint,*ssid;size_t ssid_len;};
struct brcmf_cfg80211_info;
struct brcmf_cfg80211_profile {u8 bssid[6];int use_fwsup;};
struct brcmf_cfg80211_vif {pthread_mutex_t connect_sync;void *join_retry;
 u32 join_retry_size;unsigned long sme_state;struct brcmf_cfg80211_profile profile;};
struct brcmf_bus {int state;};
struct brcmf_pub {struct brcmf_cfg80211_info *config;struct brcmf_bus *bus_if;
 struct {void *(*get_struct_for_connect)(struct brcmf_cfg80211_info *,u32 *,struct cfg80211_connect_params *);}join_param_handler;};
struct brcmf_if {struct brcmf_pub *drvr;struct brcmf_cfg80211_vif *vif;};
struct brcmf_cfg80211_connect_info {u8 *req_ie,*resp_ie;int req_ie_len,resp_ie_len;};
struct brcmf_cfg80211_info {struct brcmf_pub *pub;int d11inf,channel;bool suspended;struct brcmf_cfg80211_connect_info conn;};
struct brcmf_event_msg {u32 event_code,status;};
struct wiphy {struct brcmf_cfg80211_info *cfg;};
struct net_device {struct brcmf_if *ifp;};
static void mutex_lock(pthread_mutex_t *m) {assert(!pthread_mutex_lock(m));}
static void mutex_unlock(pthread_mutex_t *m) {assert(!pthread_mutex_unlock(m));}
static u16 channel_to_chanspec(int *d,struct ieee80211_channel *c) {(void)d;return c->chanspec;}
static u16 chandef_to_chanspec(int *d,struct cfg80211_chan_def *c) {return channel_to_chanspec(d,c->chan);}
static struct brcmf_cfg80211_info *wiphy_to_cfg(struct wiphy *w){return w->cfg;}
static struct brcmf_if *netdev_priv(struct net_device *n){return n->ifp;}
static bool check_vif_up(struct brcmf_cfg80211_vif *v){(void)v;return true;}
static int brcmf_fil_bsscfg_data_set(struct brcmf_if *i,const char *s,void *p,u32 n){
 assert(!strcmp(s,"join")&&p&&n&&i->drvr->bus_if->state==BRCMF_BUS_UP);
 assert(!i->vif->join_retry&&!i->vif->join_retry_size);
 assert(!test_bit(BRCMF_VIF_STATUS_EAP_SUCCESS,&i->vif->sme_state));
 assert(!test_bit(BRCMF_VIF_STATUS_ASSOC_SUCCESS,&i->vif->sme_state));
 assert(pthread_mutex_trylock(&i->vif->connect_sync)==EBUSY);
 commands++;return command_failure;
}
static void cfg80211_disconnected(struct net_device *n,u16 r,void *p,size_t z,bool local,int flags){
 (void)r;(void)p;(void)z;(void)flags;assert(local&&!n->ifp->vif->join_retry);reports++;
}
static int brcmf_fil_cmd_data_set(struct brcmf_if *i,int cmd,void *p,size_t n){
 (void)i;(void)p;assert(cmd==BRCMF_C_DISASSOC&&n);return 0;
}
struct cfg80211_connect_resp_params {int status;struct {const u8 *bssid;}links[1];
 u8 *req_ie,*resp_ie;int req_ie_len,resp_ie_len;};
static struct brcmf_cfg80211_connect_info *cfg_to_conn(struct brcmf_cfg80211_info *c){return &c->conn;}
static void brcmf_get_assoc_ies(struct brcmf_cfg80211_info *c,struct brcmf_if *i){(void)c;(void)i;}
static void brcmf_update_bss_info(struct brcmf_cfg80211_info *c,struct brcmf_if *i){(void)c;(void)i;}
static void cfg80211_connect_done(struct net_device *n,struct cfg80211_connect_resp_params *p,int f){
 (void)p;(void)f;assert(!n->ifp->vif->join_retry);reports++;
}
static void cfg80211_port_authorized(struct net_device *n,const u8 *b,void *p,int z,int f){(void)n;(void)b;(void)p;(void)z;(void)f;}
'''

MAIN = r'''
static void check_payload(void *p,unsigned version,const u8 *bssid,struct ieee80211_channel *chan){
 struct brcmf_ssid_le *ssid;struct brcmf_join_scan_params_le *scan;u8 *b;u32 count;u16 *ch;
 if(version){struct brcmf_ext_join_params_v1_le *j=p;assert(j->version==1&&j->assoc_le.version==1);
  ssid=&j->ssid_le;scan=&j->scan_le;b=j->assoc_le.bssid;count=j->assoc_le.chanspec_num;ch=j->assoc_le.chanspec_list;
 }else{struct brcmf_ext_join_params_le *j=p;
  ssid=&j->ssid_le;scan=&j->scan_le;b=j->assoc_le.bssid;count=j->assoc_le.chanspec_num;ch=j->assoc_le.chanspec_list;
 }
 const u8 broadcast[6]={255,255,255,255,255,255};
 assert(ssid->SSID_len==4&&!memcmp(ssid->SSID,"test",4));
 assert(!memcmp(b,bssid?bssid:broadcast,6));assert(count==!!chan);
 if(chan){assert(ch[0]==chan->chanspec);assert(scan->active_time==320&&scan->passive_time==400&&scan->nprobes==16);}
 else assert(scan->active_time==UINT32_MAX&&scan->passive_time==UINT32_MAX&&scan->nprobes==UINT32_MAX);
}
int main(void){
 struct brcmf_cfg80211_vif vif={.connect_sync=PTHREAD_MUTEX_INITIALIZER};
 struct brcmf_bus bus={BRCMF_BUS_UP};struct brcmf_pub pub={.bus_if=&bus};
 struct brcmf_cfg80211_info cfg={.pub=&pub};pub.config=&cfg;
 struct brcmf_if ifp={&pub,&vif};struct wiphy wiphy={&cfg};struct net_device ndev={&ifp};
 const u8 required[6]={2,0,0,0,0,1},hint[6]={2,0,0,0,0,2};
 struct ieee80211_channel hard_channel={0xd024},hint_channel={0xd095};
 struct brcmf_event_msg missing={BRCMF_E_SET_SSID,BRCMF_E_STATUS_NO_NETWORKS};
 unsigned cases=0;
 for(unsigned version=0;version<2;version++)for(unsigned channel=0;channel<2;channel++){
  struct cfg80211_ibss_params p={.ssid=(const u8 *)"test",.ssid_len=4,.bssid=required,
   .chandef={channel?&hard_channel:NULL}};u32 size;void *payload;u8 *b;u32 count;u16 *ch;
  cfg.channel=channel;
  if(version){struct brcmf_join_params_v1 *j=brcmf_get_prepped_struct_for_ibss_v1(&cfg,&size,&p);
   assert(j&&j->params_le.version==1);payload=j;b=j->params_le.bssid;count=j->params_le.chanspec_num;ch=j->params_le.chanspec_list;
  }else{struct brcmf_join_params *j=brcmf_get_struct_for_ibss_v0(&cfg,&size,&p);
   assert(j);payload=j;b=j->params_le.bssid;count=j->params_le.chanspec_num;ch=j->params_le.chanspec_list;
  }
  assert(!memcmp(b,required,6)&&count==channel);
  if(channel)assert(ch[0]==hard_channel.chanspec);
  kfree(payload);assert(!allocations);cases++;
 }
 for(unsigned version=0;version<2;version++)for(unsigned mask=0;mask<16;mask++){
  pub.join_param_handler.get_struct_for_connect=version?brcmf_get_struct_for_connect_v1:brcmf_get_struct_for_connect_v0;
  struct cfg80211_connect_params p={.ssid=(const u8 *)"test",.ssid_len=4,
   .bssid=mask&1?required:NULL,.bssid_hint=mask&2?hint:NULL,
   .channel=mask&4?&hard_channel:NULL,.channel_hint=mask&8?&hint_channel:NULL};
  struct cfg80211_connect_params before=p;u32 size;
  void *initial=pub.join_param_handler.get_struct_for_connect(&cfg,&size,&p);assert(initial);
  check_payload(initial,version,p.bssid?:p.bssid_hint,p.channel?:p.channel_hint);kfree(initial);
  mutex_lock(&vif.connect_sync);assert(!brcmf_prepare_join_retry(&ifp,&p));mutex_unlock(&vif.connect_sync);
  bool retry=(!p.bssid&&p.bssid_hint)||(!p.channel&&p.channel_hint);
  assert(!!vif.join_retry==retry);assert(!memcmp(&p,&before,sizeof(p)));
  if(retry)check_payload(vif.join_retry,version,p.bssid,p.channel);
  vif.sme_state=BIT(BRCMF_VIF_STATUS_CONNECTING)|BIT(BRCMF_VIF_STATUS_EAP_SUCCESS)|BIT(BRCMF_VIF_STATUS_ASSOC_SUCCESS);commands=0;
  assert(brcmf_retry_join(&ifp,&missing)==retry);assert(commands==retry);
  assert(!brcmf_retry_join(&ifp,&missing)&&commands==retry);
  assert(!vif.join_retry&&!vif.join_retry_size&&allocations==0);cases++;
 }
 // Rejections, timeouts, PSK failures and unrelated events cannot consume a retry.
 pub.join_param_handler.get_struct_for_connect=brcmf_get_struct_for_connect_v1;
 struct cfg80211_connect_params p={.ssid=(const u8 *)"test",.ssid_len=4,.bssid_hint=hint};
 for(unsigned event=0;event<128;event++)for(unsigned status=0;status<17;status++){
  if((event==BRCMF_E_SET_SSID||event==BRCMF_E_LINK)&&status==BRCMF_E_STATUS_NO_NETWORKS)continue;
  assert(!brcmf_prepare_join_retry(&ifp,&p));commands=0;
  struct brcmf_event_msg e={event,status};assert(!brcmf_retry_join(&ifp,&e)&&!commands&&vif.join_retry);
  brcmf_clear_join_retry(&vif);assert(!allocations);cases++;
 }
 for(unsigned blocked=0;blocked<3;blocked++){
  assert(!brcmf_prepare_join_retry(&ifp,&p));commands=0;
  cfg.suspended=blocked==0;bus.state=blocked==1?0:BRCMF_BUS_UP;
  vif.sme_state=blocked==2?0:BIT(BRCMF_VIF_STATUS_CONNECTING);
  assert(!brcmf_retry_join(&ifp,&missing)&&!commands);brcmf_clear_join_retry(&vif);cases++;
 }
 cfg.suspended=false;bus.state=BRCMF_BUS_UP;vif.sme_state=BIT(BRCMF_VIF_STATUS_CONNECTING);
 assert(!brcmf_prepare_join_retry(&ifp,&p));command_failure=-EIO;commands=0;
 assert(!brcmf_retry_join(&ifp,&missing)&&commands==1&&!allocations);
 assert(!brcmf_retry_join(&ifp,&missing)&&commands==1);command_failure=0;cases++;
 assert(!brcmf_prepare_join_retry(&ifp,&p));commands=reports=0;
 assert(!brcmf_cfg80211_disconnect(&wiphy,&ndev,3));
 assert(!vif.join_retry&&!allocations&&!vif.sme_state&&reports==1);
 assert(!brcmf_retry_join(&ifp,&missing)&&!commands);cases++;
 for(unsigned success=0;success<2;success++){
  assert(!brcmf_prepare_join_retry(&ifp,&p));reports=0;
  vif.sme_state=BIT(BRCMF_VIF_STATUS_CONNECTING)|BIT(BRCMF_VIF_STATUS_EAP_SUCCESS)|BIT(BRCMF_VIF_STATUS_ASSOC_SUCCESS);
  assert(!brcmf_bss_connect_done(&cfg,&ndev,&missing,success));
  assert(!vif.join_retry&&!allocations&&reports==1);
  assert(!test_bit(BRCMF_VIF_STATUS_CONNECTING,&vif.sme_state));
  assert(test_bit(BRCMF_VIF_STATUS_CONNECTED,&vif.sme_state)==success);
  assert(!brcmf_bss_connect_done(&cfg,&ndev,&missing,success)&&reports==1);
  cases++;
 }
 allocation_failure=1;assert(brcmf_prepare_join_retry(&ifp,&p)==-ENOMEM&&!allocations);cases++;
 pthread_mutex_destroy(&vif.connect_sync);
 printf("%u join payload, bounded retry, rejection and cancellation controls passed\n",cases);
 return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mutation', choices=['repeat', 'rejection', 'constraints', 'stale-auth'])
    args = parser.parse_args()
    cfg = (DRIVER / 'cfg80211.c').read_text()
    join = (DRIVER / 'join_param.c').read_text()
    wire = (DRIVER / 'fwil_types.h').read_text()
    events = (DRIVER / 'fweh.h').read_text()
    constants = '\n'.join(line for source in (join, events) for line in source.splitlines()
                          if re.match(r'#define\s+(BRCMF_SCAN_JOIN_\w+|BRCMF_E_STATUS_NO_NETWORKS)\s', line))
    # Event numbers are part of fweh.h's enumeration macro.
    for name in ('SET_SSID', 'LINK'):
        number = re.search(r'BRCMF_ENUM_DEF\(' + name + r',\s*(\d+)\)', events)[1]
        constants += f'\n#define BRCMF_E_{name} {number}'
    structs = ''.join(structure(wire, name) for name in
                      ('brcmf_ssid_le', 'brcmf_assoc_params_le', 'brcmf_assoc_params_v1_le',
                       'brcmf_join_params', 'brcmf_join_params_v1',
                       'brcmf_join_scan_params_le', 'brcmf_ext_join_params_le',
                       'brcmf_ext_join_params_v1_le', 'brcmf_scb_val_le'))
    body = ''.join(function(join, name) for name in
                   ('brcmf_joinscan_set_ssid', 'brcmf_joinscan_set_bssid',
                    'brcmf_joinscan_set_single_chanspec_from_channel',
                    'brcmf_joinscan_set_single_chanspec_from_chandef',
                    'brcmf_get_struct_for_ibss_v0', 'brcmf_get_prepped_struct_for_ibss_v1',
                    'brcmf_joinscan_set_common_v0v1_params',
                    'brcmf_get_struct_for_connect_v0', 'brcmf_get_struct_for_connect_v1'))
    body += ''.join(function(cfg, name) for name in
                    ('brcmf_clear_join_retry', 'brcmf_prepare_join_retry',
                     'brcmf_retry_join', 'brcmf_cfg80211_disconnect', 'brcmf_bss_connect_done'))
    if args.mutation == 'repeat':
        body = body.replace('vif->join_retry = NULL;', '/* retry retained */')
    elif args.mutation == 'rejection':
        body = body.replace('e->status != BRCMF_E_STATUS_NO_NETWORKS', 'false')
    elif args.mutation == 'constraints':
        body = body.replace('retry.bssid_hint = NULL;', 'retry.bssid = NULL; retry.bssid_hint = NULL;')
        body = body.replace('retry.channel_hint = NULL;', 'retry.channel = NULL; retry.channel_hint = NULL;')
    elif args.mutation == 'stale-auth':
        start = body.index('static bool brcmf_retry_join(')
        end = body.index('static s32', start)
        retry = body[start:end]
        retry = retry.replace('clear_bit(BRCMF_VIF_STATUS_EAP_SUCCESS, &vif->sme_state);', '')
        retry = retry.replace('clear_bit(BRCMF_VIF_STATUS_ASSOC_SUCCESS, &vif->sme_state);', '')
        body = body[:start] + retry + body[end:]
    code = PREFIX + constants + '\n' + structs + body + MAIN

    def run(out):
        out.mkdir(parents=True, exist_ok=True)
        source = out / 'controls.c'
        source.write_text(code)
        exe = out / 'controls'
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-pthread',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(source), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        receipt = {'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr,
                   'limits': 'Host firmware boundaries; no radio, kernel locking or resume timing proof.'}
        (out / 'results.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps(receipt))
        return result.returncode

    if args.out:
        return run(args.out)
    with tempfile.TemporaryDirectory(prefix='brcmf-join-retry-') as out:
        return run(Path(out))


if __name__ == '__main__':
    raise SystemExit(main())
