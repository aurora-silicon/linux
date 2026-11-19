#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Association IE length and publication controls for brcmfmac."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = Path('drivers/net/wireless/broadcom/brcm80211/brcmfmac')


def function(source, name):
    match = re.search(r'^(?:static (?:inline )?)?(?:s32|u32|int|void)\s+\n?' + name + r'\(', source, re.M)
    if not match:
        raise ValueError(name)
    start, position = match.start(), match.end()
    opening = source.index('{', position)
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint16_t u16; typedef uint8_t u8; typedef uint32_t u32; typedef uint32_t uint;
typedef int32_t s32; typedef uint32_t __le32;
#define le32_to_cpu(x) (x)
#define max_t(t,a,b) ((t)(a)>(t)(b)?(t)(a):(t)(b))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define BRCMF_DCMD_MAXLEN 8192
#define BRCMF_BUS_UP 1
#define BRCMF_C_GET_VAR 262
#define WL_ASSOC_INFO_MAX 512
#define WL_EXTRA_BUF_MAX 2048
#define EDCF_AC_COUNT 4
#define GFP_KERNEL 0
#define MAX_HEX_DUMP_LEN 64
#define RETRIES 2
#define BCDC_DCMD_ID_MASK 0xffff0000U
#define BCDC_DCMD_ID_SHIFT 16
#define BCDC_DCMD_ERROR 1
#define BCDC_DCMD_ID(v) (((v)&BCDC_DCMD_ID_MASK)>>BCDC_DCMD_ID_SHIFT)
#define BRCMF_FIL_ON() 0
#define brcmf_dbg(...) ((void)0)
#define brcmf_dbg_hex_dump(...) ((void)0)
#define bphy_err(...) ((void)0)
#define brcmf_err(...) ((void)0)
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
struct brcmf_pub;
struct brcmf_if;
struct brcmf_proto {
 int (*query_dcmd)(struct brcmf_pub *,int,uint,void *,uint,int *);
 int (*query_dcmd_len)(struct brcmf_pub *,int,uint,void *,uint,int *,u32 *);
 void *pd;
};
struct brcmf_bus {int state;void *dev;};
struct brcmf_pub {struct brcmf_proto *proto;struct brcmf_bus *bus_if;int proto_block;u8 proto_buf[8192];};
struct brcmf_if {struct brcmf_pub *drvr;int ifidx;bool fwil_fwerr;};
struct brcmf_cfg80211_connect_info {u8 *req_ie,*resp_ie;int req_ie_len,resp_ie_len;};
struct brcmf_cfg80211_info {struct brcmf_pub *pub;u8 extra_buf[2048];u8 ac_priority[8];struct brcmf_cfg80211_connect_info conn;};
static struct brcmf_cfg80211_connect_info *cfg_to_conn(struct brcmf_cfg80211_info *c){return &c->conn;}
struct sk_buff {u8 data[8192];u32 len;};
struct brcmf_msgbuf {struct brcmf_pub *drvr;void *rx_pktids;u32 ioctl_resp_pktid,ioctl_resp_ret_len;int ioctl_resp_status;bool ctl_completed;u32 ctl_stalls;};
#define BUS_HEADER_LEN 80
@BCDC_STRUCTS@
static struct sk_buff packet;
static u32 declared_req,declared_resp,actual_req,actual_resp,info_len;
static const char *error_name;
static int query_error,wme_error,fw_error,no_skb,malformed_skb,timeout_reply;
static int allocations,fail_alloc,live_allocs,query_calls,free_packets;
static u8 expected(u32 n,bool response){return (u8)(n*37+(response?19:7));}
static void *test_alloc(size_t n){allocations++;if(allocations==fail_alloc)return NULL;void *p=malloc(n);assert(p);live_allocs++;return p;}
static void *kzalloc(size_t n,int flags){(void)flags;void *p=test_alloc(n);if(p)memset(p,0,n);return p;}
static void *kmemdup(const void *s,size_t n,int flags){(void)flags;void *p=test_alloc(n);if(p)memcpy(p,s,n);return p;}
static void kfree(void *p){if(p){live_allocs--;free(p);}}
static void brcmf_wifi_prioritize_acparams(void *p,u8 *priority){(void)p;memset(priority,1,8);}
static const char *brcmf_fil_get_errstr(u32 e){(void)e;return "error";}
static int brcmf_proto_set_dcmd(struct brcmf_pub *d,int i,uint c,void *b,uint l,int *e){(void)d;(void)i;(void)c;(void)b;(void)l;*e=0;return 0;}
static u32 make_reply(const char *name,u8 *out){
 query_calls++;
 if(!strcmp(name,"assoc_info")){u32 lengths[2]={declared_req,declared_resp};memcpy(out,lengths,8);return info_len;}
 bool resp=!strcmp(name,"assoc_resp_ies");
 if(resp||!strcmp(name,"assoc_req_ies")){u32 n=resp?actual_resp:actual_req;for(u32 i=0;i<n&&i<8192;i++)out[i]=expected(i,resp);return n;}
 if(!strcmp(name,"wme_ac_sta")){memset(out,0,16);return 16;}
 assert(0);return 0;
}
static int enforce_request_shape=1;
static void check_request(const char *name,u32 len){
 if(!enforce_request_shape)return;
 if(!strcmp(name,"assoc_req_ies"))assert(len==strlen(name)+1+max_t(u32,declared_req,512));
 if(!strcmp(name,"assoc_resp_ies"))assert(len==strlen(name)+1+max_t(u32,declared_resp,512));
}
static int brcmf_msgbuf_tx_ioctl(struct brcmf_pub *d,int idx,uint c,void *b,uint l){
 (void)idx;(void)c;if(l>512)check_request(b,l);struct brcmf_msgbuf *m=d->proto->pd;
 bool wme=!strcmp(b,"wme_ac_sta"), fail=error_name&&!strcmp(b,error_name);m->ioctl_resp_ret_len=make_reply(b,packet.data);
 packet.len=malformed_skb?0:8192;m->ioctl_resp_status=fw_error;
 return fail?-EIO:(wme?wme_error:query_error);
}
static int brcmf_msgbuf_ioctl_resp_wait(struct brcmf_msgbuf *m){(void)m;return !timeout_reply;}
static void brcmf_bus_ctl_stall(struct brcmf_bus *b,u32 n){(void)b;(void)n;}
static struct sk_buff *brcmf_msgbuf_get_pktid(void *d,void *p,u32 n){(void)d;(void)p;(void)n;return no_skb?NULL:&packet;}
static void brcmu_pkt_buf_free_skb(struct sk_buff *s){if(s)free_packets++;}
static int brcmf_proto_bcdc_msg(struct brcmf_pub *d,int idx,uint c,void *b,uint l,bool set){
 (void)idx;(void)c;(void)set;if(l>512)check_request(b,l);struct brcmf_bcdc *m=d->proto->pd;
 bool wme=!strcmp(b,"wme_ac_sta"), fail=error_name&&!strcmp(b,error_name);m->msg.len=make_reply(b,m->buf);
 m->msg.flags=(++m->reqid<<16)|(fw_error?BCDC_DCMD_ERROR:0);m->msg.status=fw_error;
 return fail?-EIO:(wme?wme_error:query_error);
}
static int rx_calls;
static int brcmf_bus_rxctl(struct brcmf_bus *bus,u8 *out,u32 len){
 (void)len;struct brcmf_bcdc *m=((struct brcmf_pub *)bus->dev)->proto->pd;assert(out==(u8 *)&m->msg);rx_calls++;assert(rx_calls<20);
 return malformed_skb?7:sizeof(m->msg)+m->msg.len;
}
static struct brcmf_if *brcmf_get_ifp(struct brcmf_pub *d,int i){(void)d;(void)i;return NULL;}
static const char *brcmf_ifname(struct brcmf_if *i){(void)i;return "test";}
'''

TESTS = r'''
static struct brcmf_proto proto;
static struct brcmf_bus bus={.state=BRCMF_BUS_UP};
static struct brcmf_pub pub={.proto=&proto,.bus_if=&bus};
static struct brcmf_if ifp={.drvr=&pub};
static struct brcmf_cfg80211_info cfg={.pub=&pub};
static struct brcmf_msgbuf msgbuf={.drvr=&pub};
static struct brcmf_bcdc bcdc;
static void reset(int transport){
 bus.dev=&pub;
 brcmf_clear_assoc_ies(&cfg);assert(live_allocs==0);
 memset(cfg.extra_buf,0xa5,sizeof(cfg.extra_buf));memset(pub.proto_buf,0xdd,sizeof(pub.proto_buf));
 declared_req=declared_resp=actual_req=actual_resp=0;info_len=8;
 error_name=NULL;query_error=wme_error=fw_error=no_skb=malformed_skb=timeout_reply=0;
 allocations=fail_alloc=query_calls=free_packets=rx_calls=0;
 if(transport){proto.pd=&bcdc;proto.query_dcmd=brcmf_proto_bcdc_query_dcmd;proto.query_dcmd_len=brcmf_proto_bcdc_query_dcmd_len;}
 else{proto.pd=&msgbuf;proto.query_dcmd=brcmf_msgbuf_query_dcmd;proto.query_dcmd_len=brcmf_msgbuf_query_dcmd_len;}
}
static void verify_bytes(u8 *p,u32 n,bool response){assert(p||!n);for(u32 i=0;i<n;i++)assert(p[i]==expected(i,response));}
static void failed(int e){assert(brcmf_get_assoc_ies(&cfg,&ifp)==e);assert(!cfg.conn.req_ie&&!cfg.conn.resp_ie);assert(!cfg.conn.req_ie_len&&!cfg.conn.resp_ie_len);assert(live_allocs==0);}
int main(void){
 int controls=0;
 for(int transport=0;transport<2;transport++){
  u32 lengths[]={0,512,513,2048};
  for(unsigned i=0;i<4;i++){
   reset(transport);declared_req=actual_req=lengths[i];declared_resp=actual_resp=lengths[3-i];
   assert(brcmf_get_assoc_ies(&cfg,&ifp)==0);
   assert(cfg.conn.req_ie_len==(int)declared_req&&cfg.conn.resp_ie_len==(int)declared_resp);
   verify_bytes(cfg.conn.req_ie,declared_req,false);verify_bytes(cfg.conn.resp_ie,declared_resp,true);controls++;
  }
  reset(transport);declared_req=UINT32_MAX;failed(-EINVAL);controls++;
  reset(transport);declared_resp=UINT32_MAX;failed(-EINVAL);controls++;
  reset(transport);info_len=7;failed(-EBADMSG);controls++;
  reset(transport);info_len=0;failed(-EBADMSG);controls++;
  reset(transport);declared_req=513;actual_req=512;failed(-EBADMSG);controls++;
  reset(transport);declared_req=actual_req=513;declared_resp=2048;actual_resp=512;failed(-EBADMSG);controls++;
  reset(transport);declared_req=actual_req=512;declared_resp=1;actual_resp=0;failed(-EBADMSG);controls++;
  for(int n=1;n<=3;n++){reset(transport);declared_req=actual_req=512;declared_resp=actual_resp=513;fail_alloc=n;failed(-ENOMEM);controls++;}
  reset(transport);declared_req=actual_req=512;declared_resp=actual_resp=513;wme_error=-EIO;assert(brcmf_get_assoc_ies(&cfg,&ifp)==-EIO);
  assert(cfg.conn.req_ie_len==512&&cfg.conn.resp_ie_len==513);
  verify_bytes(cfg.conn.req_ie,512,false);verify_bytes(cfg.conn.resp_ie,513,true);controls++;
  reset(transport);declared_req=actual_req=512;error_name="assoc_req_ies";failed(-EIO);controls++;
  reset(transport);declared_req=actual_req=512;declared_resp=actual_resp=513;error_name="assoc_resp_ies";failed(-EIO);controls++;
  reset(transport);query_error=-EIO;failed(-EIO);controls++;
  reset(transport);fw_error=-7;failed(-EBADE);controls++;
  reset(transport);proto.query_dcmd_len=NULL;failed(-EOPNOTSUPP);controls++;
  reset(transport);malformed_skb=1;failed(-EBADMSG);controls++;
  reset(transport);u8 bytes[512];memset(bytes,0xa5,sizeof(bytes));declared_req=actual_req=512;u32 received=0;
  assert(brcmf_fil_iovar_data_get_len(&ifp,"assoc_req_ies",bytes,512,&received)==0&&received==512);verify_bytes(bytes,512,false);controls++;
  reset(transport);memset(bytes,0xa5,sizeof(bytes));actual_req=1;
  assert(brcmf_fil_iovar_data_get_len(&ifp,"assoc_req_ies",bytes,512,&received)==0&&received==1);
  assert(bytes[0]==expected(0,false));for(unsigned i=1;i<512;i++)assert(bytes[i]==0xa5);controls++;
  reset(transport);assert(brcmf_fil_iovar_data_get_len(&ifp,"assoc_req_ies",bytes,UINT32_MAX,&received)==-EINVAL&&received==0);controls++;
  reset(transport);
 }
 reset(1);malformed_skb=1;bcdc.msg.flags=(bcdc.reqid<<16);
 assert(brcmf_proto_bcdc_cmplt(&pub,bcdc.reqid,512)==-EBADMSG&&rx_calls==1);controls++;
 reset(1);malformed_skb=1;bcdc.msg.flags=((bcdc.reqid+1)<<16);
 assert(brcmf_proto_bcdc_cmplt(&pub,bcdc.reqid,512)==-EBADMSG&&rx_calls==1);controls++;
 reset(1);u8 destination[16];memset(destination,0,sizeof(destination));memcpy(destination,"assoc_req_ies",14);actual_req=512;declared_req=512;
 u32 received;int fwerr;assert(brcmf_proto_bcdc_query_dcmd_len(&pub,0,0,destination,16,&fwerr,&received)==0&&received==16);controls++;
 reset(0);no_skb=1;failed(-EBADF);controls++;
 reset(0);timeout_reply=1;failed(-EIO);controls++;
 reset(0);bus.state=0;failed(-EIO);bus.state=BRCMF_BUS_UP;controls++;
 printf("%d production association/transport controls PASS\n",controls);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--old-association', help='Git revision for association getter negative control')
    parser.add_argument('--mutation', choices=['short-ie', 'header-length', 'skb-capacity', 'cmplt-header'])
    args = parser.parse_args()
    files = {name: (args.source / DRIVER / name).read_text() for name in
             ['cfg80211.c', 'cfg80211.h', 'fwil.c', 'proto.h', 'msgbuf.c', 'bcdc.c']}
    prefix = PREFIX.replace('enforce_request_shape=1', 'enforce_request_shape=0') if args.old_association else PREFIX
    prefix = prefix.replace('@BCDC_STRUCTS@', structure(files['bcdc.c'], 'brcmf_proto_bcdc_dcmd') + structure(files['bcdc.c'], 'brcmf_bcdc'))
    parts = [prefix, structure(files['cfg80211.h'], 'brcmf_cfg80211_assoc_ielen_le'),
             structure(files['cfg80211.h'], 'brcmf_cfg80211_edcf_acparam')]
    names = {'proto.h': ['brcmf_proto_query_dcmd', 'brcmf_proto_query_dcmd_len'],
             'msgbuf.c': ['brcmf_msgbuf_query_dcmd_len', 'brcmf_msgbuf_query_dcmd'],
             'bcdc.c': ['brcmf_proto_bcdc_cmplt', 'brcmf_proto_bcdc_query_dcmd_len', 'brcmf_proto_bcdc_query_dcmd'],
             'fwil.c': ['brcmf_fil_cmd_data', 'brcmf_create_iovar',
                        'brcmf_fil_iovar_data_get', 'brcmf_fil_iovar_data_get_len'],
             'cfg80211.c': ['brcmf_clear_assoc_ies', 'brcmf_get_assoc_ies']}
    bodies = {}
    for file, functions in names.items():
        source = files[file]
        for name in functions:
            if args.old_association and name == 'brcmf_get_assoc_ies':
                source = subprocess.check_output(
                    ['git', '-C', str(args.source), 'show', args.old_association + ':' + str(DRIVER / file)], text=True)
            body = function(source, name)
            if args.mutation == 'short-ie' and name == 'brcmf_get_assoc_ies':
                body = body.replace('if (received < req_len)', 'if (false)').replace('if (received < resp_len)', 'if (false)')
            if args.mutation == 'header-length' and name == 'brcmf_proto_bcdc_query_dcmd_len':
                body = body.replace('min_t(u32, len, ret - sizeof(*msg))', 'min_t(u32, len, ret)')
            if args.mutation == 'skb-capacity' and name == 'brcmf_msgbuf_query_dcmd_len':
                body = body.replace('ret_len && msgbuf->ioctl_resp_ret_len > skb->len', 'false')
            if args.mutation == 'cmplt-header' and name == 'brcmf_proto_bcdc_cmplt':
                body = body.replace('ret < sizeof(bcdc->msg)', 'false')
            bodies[name] = hashlib.sha256(body.encode()).hexdigest()
            parts.append(body)
    parts.append(TESTS)
    directory = args.output or Path(tempfile.mkdtemp(prefix='brcmf-assoc-'))
    directory.mkdir(parents=True, exist_ok=True)
    cfile = directory / 'controls.c'
    cfile.write_text('\n'.join(parts))
    binary = directory / 'controls'
    compiled = subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                               '-fno-omit-frame-pointer', str(cfile), '-o', str(binary)], capture_output=True, text=True)
    (directory / 'compile.log').write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        raise SystemExit(compiled.stderr)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    (directory / 'run.log').write_text(run.stdout + run.stderr)
    result = {'source_sha256': {name: hashlib.sha256(text.encode()).hexdigest() for name, text in files.items()},
              'body_sha256': bodies, 'old_association': args.old_association, 'mutation': args.mutation,
              'returncode': run.returncode, 'stdout': run.stdout,
              'scope': 'Actual association/iovar/query functions; bus IO, allocation and firmware response boundaries controlled.'}
    (directory / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    raise SystemExit(run.returncode != 0)


if __name__ == '__main__':
    main()
