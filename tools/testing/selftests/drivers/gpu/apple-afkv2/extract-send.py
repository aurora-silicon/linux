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
#define U32_MAX 0xffffffffU
#define MSEC_PER_SEC 1000U
#define GFP_KERNEL 0
#define MAX_PENDING_CMDS 16
#define AFKV2_INTERFACES (1U << 16)
#define lockdep_assert_held(l) ((void)(l))
#define cpu_to_le32(x) (x)
#define le32_to_cpu(x) (x)
#define cpu_to_le64(x) (x)
#define kmalloc(n,f) malloc(n)
#define kfree(p) free(p)
#define READ_ONCE(x) (x)
#define spin_lock_irqsave(l,f) ((void)(l),(f)=0)
#define spin_unlock_irqrestore(l,f) ((void)(l),(void)(f))
#define dma_rmb() do {} while(0)
#define dev_warn(...) do {} while(0)
#define dev_err(...) do {} while(0)
#define complete(p) (*(p)=1)
#define EPIC_TYPE_COMMAND 3
#define EPIC_CAT_COMMAND 0x30
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
struct dcp { void *dev; bool quiescing,external; void *rtk; };
struct afk_ringbuffer { bool ready; };
struct apple_dcp_afkep { struct dcp *dcp; bool wire_v2,inline_v2,compact; int endpoint,lock;u8 *command_tags;
 struct afk_ringbuffer txbfr,rxbfr; };
struct ops { const char *name; };
struct apple_epic_service {
 struct apple_dcp_afkep *ep; struct ops *ops; unsigned int channel; unsigned long cmd_map[1];
 int lock; unsigned int users; bool enabled,torndown; u8 cmd_tag; struct cmd_info cmds[16];
};
static struct apple_epic_service *active;
static unsigned int frees, sends;
static int wire_fail, timeout, reply_status;
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
 void *output,size_t output_len,u32 request_status,u32 *retcode,size_t *copied,unsigned long timeout)
{
 (void)timeout;
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
{ void *p=calloc(1,n);(void)dev;(void)flags;*address=(uintptr_t)p;return p; }
static void dma_free_coherent(void *dev,size_t n,void *p,dma_addr_t a)
{ (void)dev;(void)n;assert((uintptr_t)p==a);free(p);frees++; }
static int bitmap_find_free_region(unsigned long *map,unsigned int n,int order)
{ (void)n;(void)order;assert(!*map);*map=1;return 0; }
static void bitmap_release_region(unsigned long *map,int i,int order)
{ (void)i;(void)order;*map=0; }
static int afk_send_epic(struct apple_dcp_afkep *ep,u32 channel,u16 tag,int type,int category,u8 subtype,const void *payload,size_t length)
{
 struct cmd_info *c=&active->cmds[0];struct afkv2_command parsed;
 assert(channel==4 && type==3 && category==0x30 && subtype==0xc0);
 sends++;
 /* The payload-envelope mock bounds the command separately from its response. */
 assert(length<=128);
 if(ep->inline_v2) {
  assert(length==120);
  assert(!afkv2_decode_command(payload,length,&parsed));
  assert(!parsed.oob && parsed.id==tag && parsed.value==32888 && parsed.inline_size==112);
  assert(parsed.inline_data[0]==0x5a);
 } else {
  const struct epic_cmd *legacy=payload;
  assert(length==sizeof(*legacy) && legacy->txlen==112 && legacy->rxlen==32888);
 }
 if(wire_fail)return -ENOSPC;
 if(timeout)return 0;
 if(real_reply) {
  struct epic_cmd reply={.retcode=reply_status,.rxlen=legacy_rxlen};
  assert(!ep->wire_v2);
  memset(c->rxbuf,0x77,legacy_rxlen<c->rxlen?legacy_rxlen:c->rxlen);
  afk_recv_handle_reply(ep,channel,tag,&reply,sizeof(reply));
  assert(c->done && c->reply_len_valid && c->reply_len==legacy_rxlen && *c->completion==1);
  return 0;
 }
 c->reply_length=ep->inline_v2?4:owned_reply_size;c->retcode=reply_status;
 memset(c->rxbuf,0x77,c->reply_length);c->done=true;*c->completion=1;
 return 0;
}
'''
suffix = r'''
int main(void)
{
 u8 tags[AFKV2_INTERFACES]={0};
 struct dcp dcp={0};struct apple_dcp_afkep ep={.dcp=&dcp,.wire_v2=true,.inline_v2=true,.command_tags=tags};
 struct ops ops={.name="test"};struct apple_epic_service s={.ep=&ep,.ops=&ops,.channel=4,.enabled=true,.users=1};
 u8 request[112]={0x5a},*output=calloc(1,32888);size_t received;u32 status=99;
 active=&s;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(received==4 && status==0 && output[0]==0x77 && frees==2 && sends==1 && !s.cmd_map[0]);
 /* Existing strict callers still reject a short inline result. */
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,NULL)==-EMSGSIZE);
 reply_status=5;received=99;
 assert(!afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received));
 assert(status==5 && received==0);reply_status=0;
 wire_fail=1;
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-ENOSPC && !s.cmd_map[0]);
 wire_fail=0;ep.inline_v2=false;
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
 dcp.external=true;ep.txbfr.ready=ep.rxbfr.ready=true;
 ep.wire_v2=true;owned_reply_size=32888;timeout=1;
 assert(afk_send_command_timeout_len(&s,0xc0,request,112,output,32888,&status,1000,&received)==-ETIMEDOUT);
 assert(s.cmd_map[0] && s.cmds[0].free_on_ack && !s.cmds[0].completion);
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
 assert(!s.cmd_map[0] && s.users==1);free(output);
 puts("PASS: extracted production sender, 128-byte command envelope / 32888-byte reply, short OOB, strict/legacy callers, legacy reply lengths, compact status and actual late-ACK ownership");
}
'''
bodies = [function(r"static u32 afk_compact_next_status", "compact status allocator"),
          function(r"static u16 afk_command_tag", "interface tag allocator"),
          function(r"static int afk_send_command_common", "command sender"),
          function(r"static int afk_send_command_timeout_len", "length-aware sender"),
          function(r"int afk_send_command_with_reply_len", "legacy reply-length sender"),
          function(r"static void afk_recv_handle_reply", "reply completer")]
pathlib.Path(sys.argv[2]).write_text(prefix + "".join(bodies) + suffix)
