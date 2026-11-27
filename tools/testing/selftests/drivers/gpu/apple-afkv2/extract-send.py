#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile the real command sender against synthetic ring and DMA operations.

Covers AFK-v2, legacy reply-length and compact EPIC requests.
"""
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()


def function(pattern, what):
    match = re.search(r"^" + pattern + r"\([^;]*?\)\n\{", source, re.M)
    if not match:
        raise RuntimeError("production " + what + " not found")
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


prefix = r'''
#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "afkv2-codec.h"
#include "afk_reply.h"
#define SZ_1M 1048576U
#define SZ_16K 16384UL
#define SZ_16M (16UL<<20)
#ifndef ALIGN
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#endif
#define U16_MAX 0xffffU
#define U8_MAX 0xffU
#define U32_MAX 0xffffffffU
#define MSEC_PER_SEC 1000U
#define GFP_KERNEL 0
#define MAX_PENDING_CMDS 16
#define AFKV2_INTERFACES (1U << 16)
#define lockdep_assert_held(l) assert(*(l))
#define IS_ALIGNED(x,a) (((x) & ((a) - 1)) == 0)
#define roundup(x,y) ((((x) + ((y) - 1)) / (y)) * (y))
#define cpu_to_le16(x) (x)
#define le16_to_cpu(x) (x)
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define cpu_to_le64(x) (x)
#define le64_to_cpu(x) (x)
#define kmalloc(n,f) malloc(n)
#define kfree(p) free(p)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define ESHUTDOWN 108
#ifndef EBUSY
#define EBUSY 16
#endif
#define spin_lock(l) do { assert(!*(l)); *(l)=1; } while(0)
#define spin_unlock(l) do { assert(*(l)); *(l)=0; } while(0)
#define spin_lock_irqsave(l,f) do { assert(!*(l)); *(l)=1; (f)=0; } while(0)
#define spin_unlock_irqrestore(l,f) do { assert(*(l)); *(l)=0; (void)(f); } while(0)
#define mutex_lock(l) do { assert(!*(l)); *(l)=1; } while(0)
#define mutex_unlock(l) do { assert(*(l)); *(l)=0; } while(0)
#define dma_rmb() do {} while(0)
#define dma_wmb() do {} while(0)
#define dev_warn(...) do {} while(0)
#define dev_err(...) do {} while(0)
#define dev_dbg_once(...) do {} while(0)
#define trace_afk_send_rwptr_pre(...) do {} while(0)
#define trace_afk_send_rwptr_post(...) do {} while(0)
#define complete(p) (*(p)=1)
#define QE_MAGIC 0x20504f49U
#define RBEP_TYPE (0xffffULL << 48)
#define RBEP_SEND 0xa2ULL
#define SEND_WPTR 0xffffffffULL
#define FIELD_PREP(mask,val) (((u64)(val) << __builtin_ctzll(mask)) & (mask))
#define DCP_FIRMWARE_H17P 1
#define DCP_FIRMWARE_V_14_7 2
#define DCP_FIRMWARE_M4 3
#define EPIC_SUBTYPE_STD_SERVICE 0xc0
enum epic_type { EPIC_TYPE_NOTIFY = 0, EPIC_TYPE_COMMAND = 3, EPIC_TYPE_REPLY = 4 };
enum epic_category {
 EPIC_CAT_REPORT = 0x00, EPIC_CAT_NOTIFY = 0x10,
 EPIC_CAT_REPLY = 0x20, EPIC_CAT_COMMAND = 0x30
};
struct afk_qe { u32 magic, size, channel, type; u8 data[]; };
struct epic_hdr {
 u8 version; u16 seq; u8 _pad; u32 unk; u64 timestamp;
} __attribute__((packed));
struct epic_sub_hdr {
 u32 length; u8 version, category; u16 type; u64 timestamp; u16 tag, unk; u32 inline_len;
} __attribute__((packed));
struct epic_sub_hdr_h17p {
 u8 type, category; u16 flags; u32 tag;
} __attribute__((packed));
static inline u32 afk_ring_advance(u32 ptr, u32 bytes, u32 bufsz, u32 stride)
{
 u32 next = roundup(ptr + bytes, stride);
 return next == bufsz ? 0 : next;
}
#define DECLARE_COMPLETION_ONSTACK(n) int n=0
#define init_completion(p) (*(p)=0)
#define msecs_to_jiffies(x) (x)
#define wait_for_completion_timeout(p,t) ((void)(t),*(p))
typedef u64 dma_addr_t;
struct epic_cmd { u32 retcode; u64 rxbuf,txbuf; u32 rxlen,txlen; u8 rxcookie,txcookie; };
struct cmd_info {
 u16 tag; u8 type; size_t reply_length; void *rxbuf,*txbuf;
 dma_addr_t rxbuf_dma,txbuf_dma; size_t rxlen,txlen; u32 retcode;
 size_t reply_len; bool reply_len_valid;
 bool done,free_on_ack; int *completion;
};
struct apple_dcp {
 void *dev; int fw_compat;
 bool quiescing,external,crashed,ext26,afk_tx_closed,afk_rx_failed,sleep_v27_closed;
 int afk_tx_lock; void *rtk;
};
static bool dcp_is_ext26(const struct apple_dcp *dcp) { return dcp->ext26; }
static bool dcp_is_v14_7_external(const struct apple_dcp *dcp)
{ return dcp->fw_compat == DCP_FIRMWARE_V_14_7 && dcp->external; }
struct afk_ringbuffer {
 bool ready; u32 *rptr,*wptr; void *buf; size_t bufsz; u32 stride;
};
struct apple_dcp_afkep {
 struct apple_dcp *dcp;
 bool wire_v2,inline_v2,compact,stopping,tx_paused,rx_paused,rx_failed;
 int endpoint,lock,tx_lock; unsigned int tx_active; u16 qe_seq; u8 *command_tags;
 struct afk_ringbuffer txbfr,rxbfr; size_t ext26_command_bytes; unsigned int ext26_command_count;
};
struct ops { const char *name; };
struct apple_epic_service {
 struct apple_dcp_afkep *ep; struct ops *ops; unsigned int channel; unsigned long cmd_map[1];
 int lock; unsigned int users; bool enabled,torndown; u8 cmd_tag; struct cmd_info cmds[16];
};
static struct apple_epic_service *active;
static struct apple_dcp_afkep *active_ep;
static unsigned int frees, sends;
static int wire_fail, timeout, reply_status;
static bool raw_doorbell_mode, inject_overflow_on_alloc, complete_before_doorbell_fail;
static u32 tx_consumed_wptr;
static size_t owned_reply_size=32888;
/* Legacy replies can go through the real completer, as from the ring. */
static bool real_reply;
static u32 legacy_rxlen;
static void afk_recv_handle_reply(struct apple_dcp_afkep *ep,u32 channel,u16 tag,void *payload,size_t payload_size);
/* Compact EPIC goes inline through the ring; this mock stands for that transport. */
static bool compact_echo;
static u32 compact_reply,compact_request;
static size_t compact_copied;
static unsigned int compact_sends;
static int afk_send_compact_command(struct apple_epic_service *s,u8 type,const void *payload,size_t payload_len,
 void *output,size_t output_len,u32 request_status,u32 *retcode,size_t *copied,unsigned long to)
{
 (void)to;
 assert(s==active && type==0xc0 && payload_len==112 && ((const u8 *)payload)[0]==0x5a && output_len==32888);
 compact_sends++;compact_request=request_status;
 *retcode=compact_echo?request_status:compact_reply;
 memset(output,0x33,compact_copied);*copied=compact_copied;
 return 0;
}
static struct apple_epic_service *afk_service_get(struct apple_epic_service *s)
{ if(!s->enabled) return NULL; s->users++; return s; }
static void afk_service_put(struct apple_epic_service *s) { assert(s->users); s->users--; }
static struct apple_epic_service *afk_epic_find_service(struct apple_dcp_afkep *ep,u32 channel)
{ return active->ep==ep && active->channel==channel && (active->enabled || active->cmd_map[0]) ? active : NULL; }
static int test_bit(unsigned int bit,const unsigned long *map) { return !!(*map & (1UL<<bit)); }
static void *dma_alloc_coherent(void *dev,size_t n,dma_addr_t *address,int flags)
{
 void *p=calloc(1,n);(void)dev;(void)flags;*address=(uintptr_t)p;
 if(inject_overflow_on_alloc && n==112) {
  inject_overflow_on_alloc=false;
  active_ep->rx_failed=true;active_ep->tx_paused=true;
  active_ep->dcp->afk_rx_failed=true;active_ep->dcp->afk_tx_closed=true;
 }
 return p;
}
static void dma_free_coherent(void *dev,size_t n,void *p,dma_addr_t a)
{ (void)dev;(void)n;assert((uintptr_t)p==a);free(p);frees++; }
static int bitmap_find_free_region(unsigned long *map,unsigned int n,int order)
{ (void)n;(void)order;assert(!*map);*map=1;return 0; }
static void bitmap_release_region(unsigned long *map,int i,int order)
{ (void)i;(void)order;*map=0; }
static void complete_active_command(struct apple_dcp_afkep *ep)
{
 struct cmd_info *c=&active->cmds[0];
 if(real_reply) {
  struct epic_cmd reply={.retcode=reply_status,.rxlen=legacy_rxlen};
  assert(!ep->wire_v2);
  memset(c->rxbuf,0x77,legacy_rxlen<c->rxlen?legacy_rxlen:c->rxlen);
  afk_recv_handle_reply(ep,active->channel,c->tag,&reply,sizeof(reply));
  assert(c->done && c->reply_len_valid && c->reply_len==legacy_rxlen && *c->completion==1);
  return;
 }
 c->reply_length=ep->inline_v2?4:owned_reply_size;c->retcode=reply_status;
 memset(c->rxbuf,0x77,c->reply_length);c->done=true;*c->completion=1;
}
static int dcp_send_message_ret(struct apple_dcp *dcp,u8 endpoint,u64 message)
{
 bool sleepable=(dcp->fw_compat==DCP_FIRMWARE_V_14_7 || dcp->ext26);
 assert(dcp==active_ep->dcp && endpoint==active_ep->endpoint);
 assert(!active->lock);
 if(sleepable) {
  assert(!dcp->afk_tx_lock && !active_ep->lock);
  if(!raw_doorbell_mode) assert(active_ep->tx_lock);
 } else {
  assert(dcp->afk_tx_lock);
  if(!raw_doorbell_mode) assert(active_ep->tx_lock && active_ep->lock);
 }
 sends++;
 if(raw_doorbell_mode) return wire_fail;

 assert((message >> 48)==RBEP_SEND);
 u32 wptr=(u32)(message & SEND_WPTR);
 assert(wptr==*active_ep->txbfr.wptr);
 u32 pos=tx_consumed_wptr;
 struct afk_qe *qe=(struct afk_qe *)((u8 *)active_ep->txbfr.buf+pos);
 assert(qe->magic==QE_MAGIC);
 if(pos+sizeof(*qe)+qe->size>active_ep->txbfr.bufsz) {
  pos=0;qe=(struct afk_qe *)((u8 *)active_ep->txbfr.buf+pos);
  assert(qe->magic==QE_MAGIC);
 }
 if(active_ep->wire_v2) {
  struct afkv2_frame frame;struct afkv2_command parsed;
  assert(!qe->channel && !qe->type);
  assert(!afkv2_decode_frame(qe->data,qe->size,false,&frame));
  assert(frame.interface==4 && frame.category==AFKV2_COMMAND && frame.type==0xc0);
  assert(!afkv2_decode_command(frame.payload,frame.payload_size,&parsed));
  assert(parsed.id==active->cmds[0].tag);
  if(active_ep->inline_v2) {
   assert(!parsed.oob && parsed.value==32888 && parsed.inline_size==112);
   assert(parsed.inline_data[0]==0x5a);
  } else {
   assert(parsed.oob && parsed.tx_size==112 && parsed.rx_size==32888);
  }
 } else {
  assert(qe->channel==4 && qe->type==EPIC_TYPE_COMMAND);
  assert(qe->size==sizeof(struct epic_hdr)+sizeof(struct epic_sub_hdr)+sizeof(struct epic_cmd));
  const struct epic_cmd *legacy=(const struct epic_cmd *)(qe->data+sizeof(struct epic_hdr)+sizeof(struct epic_sub_hdr));
  assert(legacy->txlen==112 && legacy->rxlen==32888);
 }
 tx_consumed_wptr=wptr;*active_ep->txbfr.rptr=wptr;
 if(wire_fail) {
  if(dcp->ext26) dcp->crashed=true;
  if(complete_before_doorbell_fail) complete_active_command(active_ep);
  return wire_fail;
 }
 if(timeout) return 0;
 complete_active_command(active_ep);
 return 0;
}
'''
suffix = r'''
int main(void)
{
 u8 tags[AFKV2_INTERFACES]={0};
 u8 tx_ring_buf[4096]={0};u32 tx_rptr=0,tx_wptr=0;
 struct apple_dcp dcp={.fw_compat=DCP_FIRMWARE_M4};
 struct apple_dcp_afkep ep={
  .dcp=&dcp,.endpoint=0x2a,.wire_v2=true,.inline_v2=true,.command_tags=tags,
  .txbfr={.ready=true,.rptr=&tx_rptr,.wptr=&tx_wptr,.buf=tx_ring_buf,.bufsz=sizeof(tx_ring_buf),.stride=64}
 };
 struct ops ops={.name="test"};
 struct apple_epic_service s={.ep=&ep,.ops=&ops,.channel=4,.enabled=true,.users=1};
 u8 request[112]={0x5a},*output=calloc(1,32888);size_t received;u32 status=99;
 active=&s;active_ep=&ep;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(received==4 && status==0 && output[0]==0x77 && frees==2 && sends==1 && !s.cmd_map[0] && !ep.tx_active);
 /* Existing strict callers still reject a short inline result. */
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,NULL)==-EMSGSIZE);
 reply_status=5;received=99;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(status==5 && received==0);reply_status=0;
 /* S5: early-RX overflow injected between admission and send must abort before publication. */
 unsigned int before_ovf_frees=frees,before_ovf_sends=sends;u32 before_ovf_wptr=tx_wptr;
 inject_overflow_on_alloc=true;
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-ENOSPC);
 assert(tx_wptr==before_ovf_wptr && sends==before_ovf_sends && frees==before_ovf_frees+2 && !s.cmd_map[0] && !ep.tx_active);
 ep.rx_failed=false;ep.tx_paused=false;dcp.afk_rx_failed=false;dcp.afk_tx_closed=false;
 /* S5: raw retained-session doorbell checks overflow under afk_tx_lock; 14.7 sleeps unlocked. */
 raw_doorbell_mode=true;
 assert(!afk_send_ret(&ep,0x1234ULL));
 assert(sends==before_ovf_sends+1);
 dcp.afk_rx_failed=true;
 assert(afk_send_ret(&ep,0x1234ULL)==-ENOSPC && sends==before_ovf_sends+1);
 dcp.afk_rx_failed=false;dcp.fw_compat=DCP_FIRMWARE_V_14_7;
 assert(!afk_send_ret(&ep,0x1234ULL) && sends==before_ovf_sends+2);
 dcp.fw_compat=DCP_FIRMWARE_M4;raw_doorbell_mode=false;
 /* S6: failed doorbell after publication retains slot and DMA until late ACK or quarantine. */
 ep.inline_v2=false;
 unsigned int before_fail_frees=frees;u32 before_fail_wptr=tx_wptr;
 wire_fail=-ETIMEDOUT;
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-ETIMEDOUT);
 assert(tx_wptr!=before_fail_wptr && sends==before_ovf_sends+3);
 assert(frees==before_fail_frees && s.cmd_map[0] && s.cmds[0].free_on_ack && !s.cmds[0].completion && !ep.tx_active && s.users==1);
 struct epic_cmd late_fail_reply={.retcode=0};
 afk_recv_handle_reply(&ep,4,s.cmds[0].tag,&late_fail_reply,sizeof(late_fail_reply));
 assert(frees==before_fail_frees+2 && !s.cmd_map[0] && s.cmds[0].done && s.users==1);
 /* S6: if firmware already completed the published command before doorbell error returns, free cleanly. */
 dcp.fw_compat=DCP_FIRMWARE_V_14_7;complete_before_doorbell_fail=true;
 unsigned int before_race_frees=frees;
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-ETIMEDOUT);
 assert(frees==before_race_frees+2 && !s.cmd_map[0] && s.cmds[0].done && !ep.tx_active && s.users==1);
 complete_before_doorbell_fail=false;wire_fail=0;dcp.fw_compat=DCP_FIRMWARE_M4;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(received==32888 && status==0);
 /* Length-aware owned OOB callers copy only the bounded actual reply. */
 owned_reply_size=4;memset(output,0x19,32888);
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(received==4 && output[0]==0x77 && output[3]==0x77 && output[4]==0x19 && s.users==1);
 memset(output,0x19,32888);
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,NULL)==-EMSGSIZE);
 assert(output[0]==0x19 && !s.cmd_map[0]);
 owned_reply_size=0;received=99;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(received==0 && output[0]==0x19);
 /* Legacy framing retains its full-buffer copy contract. */
 ep.wire_v2=false;owned_reply_size=4;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(received==32888 && s.users==1);
 /* Legacy EPIC reply lengths: bounded by the buffer, rest of the output zeroed. */
 real_reply=true;legacy_rxlen=4;size_t rlen=99;memset(output,0x19,32888);
 assert(!afk_send_command_with_reply_len(&s,0xc0,request,112,output,32888,&status,&rlen));
 assert(rlen==4 && status==0 && output[0]==0x77 && output[3]==0x77 && !output[4] && !output[32887]);
 assert(!s.cmd_map[0] && s.users==1);
 legacy_rxlen=32889;rlen=99;
 assert(afk_send_command_with_reply_len(&s,0xc0,request,112,output,32888,&status,&rlen)==-EMSGSIZE);
 assert(rlen==0 && !s.cmd_map[0] && s.users==1);
 assert(afk_send_command_with_reply_len(&s,0xc0,request,112,output,32888,&status,NULL)==-EINVAL);
 unsigned int before_sends=sends;
 assert(afk_send_command_with_reply_len(&s,0xc0,request,(size_t)U32_MAX+1,output,32888,&status,&rlen)==-EOVERFLOW);
 /* AFK-v2 reports reply lengths its own way and refuses this interface. */
 ep.wire_v2=true;
 assert(afk_send_command_with_reply_len(&s,0xc0,request,112,output,32888,&status,&rlen)==-EOPNOTSUPP);
 assert(sends==before_sends && s.users==1 && !s.cmd_map[0]);
 real_reply=false;
 /* Compact EPIC: success is the reply echoing the request's own status. */
 ep.compact=true;unsigned int before_frees=frees;status=99;received=99;
 compact_echo=true;compact_copied=20;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(status==0 && received==20 && output[0]==0x33 && compact_sends==1);
 u32 first=compact_request;
 compact_echo=false;compact_reply=0xe00002c2;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(status==0xe00002c2 && compact_request==((((first>>8)+1)&0xff)<<8));
 compact_reply=0;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(compact_request && status==~0U);
 compact_echo=true;compact_copied=8;rlen=99;
 assert(!afk_send_command_with_reply_len(&s,0xc0,request,112,output,32888,&status,&rlen) && rlen==8 && !status);
 assert(compact_sends==4 && sends==before_sends && frees==before_frees && s.users==1 && !s.cmd_map[0]);
 ep.compact=false;
 dcp.external=true;ep.rxbfr.ready=true;
 ep.wire_v2=true;owned_reply_size=32888;timeout=1;
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-ETIMEDOUT);
 assert(s.cmd_map[0] && s.cmds[0].free_on_ack && !s.cmds[0].completion && !ep.tx_active);
 /* Execute the real late-ACK completer: mismatch/duplicate cannot free DMA. */
 unsigned int before_late=frees;u16 retained_tag=s.cmds[0].tag;
 struct epic_cmd reply={.retcode=0};
 s.enabled=false;s.torndown=true; /* Retired service still owns the pending slot. */
 afk_recv_handle_reply(&ep,4,retained_tag^0x10,&reply,sizeof(reply));
 assert(frees==before_late && s.cmd_map[0] && !s.cmds[0].done && s.users==1);
 afk_recv_handle_reply(&ep,4,retained_tag,&reply,sizeof(reply));
 assert(frees==before_late+2 && !s.cmd_map[0] && s.cmds[0].done && s.users==1);
 afk_recv_handle_reply(&ep,4,retained_tag,&reply,sizeof(reply));
 assert(frees==before_late+2);
 s.enabled=true;s.torndown=false;timeout=0;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(!s.cmd_map[0] && s.users==1);
 /* J613 ext26 keeps every command's DMA: a budget bounds it, a timeout latches crashed. */
 dcp.ext26=true;unsigned int ext_frees=frees,ext_sends=sends;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 size_t charge=ep.ext26_command_bytes;
 assert(received==32888 && frees==ext_frees && sends==ext_sends+1 && !s.cmd_map[0]);
 assert(ep.ext26_command_count==1 && charge==SZ_16K+ALIGN(32888,SZ_16K));
 free(s.cmds[0].rxbuf);free(s.cmds[0].txbuf);
 /* S6 on ext26: failed doorbell after publication latches crashed and keeps slot/DMA. */
 wire_fail=-ETIMEDOUT;
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-ETIMEDOUT);
 assert(dcp.crashed && s.cmd_map[0] && !s.cmds[0].free_on_ack && !s.cmds[0].completion && !ep.tx_active);
 assert(frees==ext_frees && ep.ext26_command_count==2);
 free(s.cmds[0].rxbuf);free(s.cmds[0].txbuf);s.cmd_map[0]=0;
 ep.ext26_command_count=1;ep.ext26_command_bytes=charge;dcp.crashed=false;wire_fail=0;
 ep.ext26_command_bytes=SZ_16M-charge+1;
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-ENOSPC);
 assert(ep.ext26_command_count==1 && ep.ext26_command_bytes==SZ_16M-charge+1);
 ep.ext26_command_bytes=SZ_16M-charge;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(ep.ext26_command_bytes==SZ_16M && ep.ext26_command_count==2 && frees==ext_frees);
 free(s.cmds[0].rxbuf);free(s.cmds[0].txbuf);
 ep.ext26_command_bytes=0;ep.ext26_command_count=255;
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-ENOSPC);
 assert(ep.ext26_command_count==255);ep.ext26_command_count=0;dcp.crashed=true;
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-EIO);
 assert(!ep.ext26_command_count && sends==ext_sends+3 && s.users==1 && !s.cmd_map[0]);
 dcp.crashed=false;timeout=1;
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-ETIMEDOUT);
 assert(dcp.crashed && s.cmd_map[0] && !s.cmds[0].free_on_ack && !s.cmds[0].completion);
 assert(frees==ext_frees && s.users==1 && ep.ext26_command_count==1);
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-EIO);
 assert(sends==ext_sends+4 && s.users==1);
 free(s.cmds[0].rxbuf);free(s.cmds[0].txbuf);s.cmd_map[0]=0;free(output);
 puts("PASS: extracted production sender, transport boundary lock assertions, overflow injection, failed doorbell after publication, and ext26 budget");
}
'''
bodies = [function(r"static bool afk_send_may_sleep", "sleepability check"),
          function(r"static int afk_tx_failed_under_tx_lock", "TX failure check"),
          function(r"static int afk_tx_admit_locked", "TX admission check"),
          function(r"static int afk_send_raw_locked", "locked doorbell helper"),
          function(r"static int afk_send_ret", "doorbell helper"),
          function(r"static int afk_send_epic_token", "EPIC ring sender"),
          function(r"static int afk_send_epic_published", "published EPIC sender"),
          function(r"int afk_send_epic", "EPIC sender"),
          function(r"static u32 afk_compact_next_status", "compact status allocator"),
          function(r"static u16 afk_command_tag", "interface tag allocator"),
          function(r"static int afk_send_command_common", "command sender"),
          function(r"static int afk_send_command_timeout_len", "length-aware sender"),
          function(r"int afk_send_command_with_reply_len", "legacy reply-length sender"),
          function(r"static void afk_recv_handle_reply", "reply completer")]
pathlib.Path(sys.argv[2]).write_text(prefix + "".join(bodies) + suffix)
