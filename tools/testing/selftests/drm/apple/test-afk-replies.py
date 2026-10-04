#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Compile actual AFK command/reply paths with host DMA/completion stubs."""
from pathlib import Path
import subprocess,tempfile,re
b=Path(__file__).resolve().parents[5]/'drivers/gpu/drm/apple';s=(b/'afk.c').read_text()
def function(name):
 m=re.search(r'^(?:static )?(?:int|void) '+re.escape(name)+r'\(',s,re.M);assert m,name
 start=m.start();body=s.index('{',start);n=1;i=body+1
 while n:
  n+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]
helper=(b/'afk_reply.h').read_text();helper='\n'.join(x for x in helper.splitlines() if not x.startswith('#include'))
preamble=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uintptr_t dma_addr_t;
#define MAX_PENDING_CMDS 16
#define U32_MAX UINT32_MAX
#define GFP_KERNEL 0
#define MSEC_PER_SEC 1000
#define EPIC_TYPE_COMMAND 3
#define EPIC_CAT_COMMAND 2
#define EPIC_SUBTYPE_STD_SERVICE 0x20
#define EPIC_SERVICE_CALL_MAGIC 0x69706378
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)
#define cpu_to_le16(x) (x)
#define le32_to_cpu(x) (x)
#define le16_to_cpu(x) (x)
#define READ_ONCE(x) (x)
#define dma_rmb() do {} while(0)
#define dev_warn(...) do {} while(0)
#define dev_err(...) do {} while(0)
#define spin_lock_irqsave(a,b) do {(void)(a);(b)=0;} while(0)
#define spin_unlock_irqrestore(a,b) do {(void)(a);(void)(b);} while(0)
#define max(a,b) ((a)>(b)?(a):(b))
#define check_add_overflow(a,b,p) __builtin_add_overflow((a),(b),(p))
struct completion {int done;};
#define DECLARE_COMPLETION_ONSTACK(x) struct completion x={0}
static void init_completion(struct completion *c){c->done=0;}
static void complete(struct completion *c){c->done=1;}
static int wait_for_completion_timeout(struct completion *c,int t){(void)t;return c->done;}
#define msecs_to_jiffies(x) (x)
struct epic_cmd {u32 retcode;uint64_t rxbuf,txbuf;u32 rxlen,txlen;};
struct epic_cmd_info {u16 tag;void *rxbuf,*txbuf;dma_addr_t rxbuf_dma,txbuf_dma;size_t rxlen,txlen,reply_len;bool reply_len_valid;u32 retcode;bool done,free_on_ack;struct completion *completion;};
struct dcp {void *dev;bool external;void *rtk;};
struct hdr {u32 rptr,wptr;};
struct ring {struct hdr *hdr;};
struct apple_dcp_afkep {struct dcp *dcp;unsigned endpoint;struct ring txbfr,rxbfr;};
struct apple_epic_service {struct apple_dcp_afkep *ep;unsigned channel;int lock;unsigned long cmd_map[1];unsigned cmd_tag;struct epic_cmd_info cmds[16];};
struct epic_service_call {u16 pad,group;u32 command,data_len,magic;char rest[48];};
static struct apple_epic_service *current;
static unsigned allocations,frees,live;
struct allocation {void *p;size_t size;} allocs[256];
static void *dma_alloc_coherent(void *dev,size_t n,dma_addr_t *dma,int flags){(void)dev;(void)flags;void *p=calloc(1,n?n:1);assert(p);allocs[allocations++]=(struct allocation){p,n};live++;*dma=(uintptr_t)p;return p;}
static void dma_free_coherent(void *dev,size_t n,void *p,dma_addr_t dma){(void)dev;assert((uintptr_t)p==dma);unsigned i;for(i=0;i<allocations;i++)if(allocs[i].p==p)break;assert(i<allocations&&allocs[i].size==n);allocs[i].p=NULL;live--;frees++;free(p);}
static int bitmap_find_free_region(unsigned long *map,int size,int order){assert(order==0);for(int i=0;i<size;i++)if(!(*map&(1UL<<i))){*map|=1UL<<i;return i;}return -1;}
static void bitmap_release_region(unsigned long *map,int i,int order){assert(order==0);assert(*map&(1UL<<i));*map&=~(1UL<<i);}
static struct apple_epic_service *afk_epic_find_service(struct apple_dcp_afkep *ep,u32 channel){assert(ep==current->ep&&channel==current->channel);return current;}
static void *kzalloc(size_t n,int flags){(void)flags;return calloc(1,n);}
#define kfree free
static int afk_send_epic(struct apple_dcp_afkep *,u32,u16,int,int,u8,const void *,size_t);
'''
mock=r'''
enum scenario {FULL,SHORT,SHORT_ACK,OVERSIZE,SEND_ERROR,TIMEOUT,SERVICE,SERVICE_TRUNCATED,SERVICE_OVERSIZE,REMOTE_ERROR};
static enum scenario scenario;
static size_t response_size;
static u16 sent_tag;
static struct epic_cmd sent_cmd;
static int afk_send_epic(struct apple_dcp_afkep *ep,u32 channel,u16 tag,int type,int cat,u8 sub,const void *payload,size_t n){
(void)type;(void)cat;(void)sub;assert(n==sizeof(struct epic_cmd));sent_cmd=*(const struct epic_cmd*)payload;sent_tag=tag;
if(scenario==SEND_ERROR)return -EIO;
if(scenario==TIMEOUT)return 0;
struct epic_cmd_info *c=&current->cmds[tag&255];
if(scenario==SHORT){c->done=true;c->retcode=0;complete(c->completion);return 0;}
struct epic_cmd reply=sent_cmd;reply.rxlen=response_size;
if(scenario==SHORT_ACK){afk_recv_handle_reply(ep,channel,tag,&reply,sizeof(reply)-1);return 0;}
if(scenario==OVERSIZE)reply.rxlen=sent_cmd.rxlen+1;
if(scenario==REMOTE_ERROR)reply.retcode=42;
if(scenario==SERVICE||scenario==SERVICE_TRUNCATED||scenario==SERVICE_OVERSIZE){
struct epic_service_call *r=(void*)(uintptr_t)sent_cmd.rxbuf,*tx=(void*)(uintptr_t)sent_cmd.txbuf;
*r=*tx;r->data_len=scenario==SERVICE_OVERSIZE?20:8;memset(r+1,0x5a,8);reply.rxlen=scenario==SERVICE_TRUNCATED?sizeof(*r)-1:sizeof(*r)+8;
}else memset((void*)(uintptr_t)sent_cmd.rxbuf,0x5a,sent_cmd.rxlen);
afk_recv_handle_reply(ep,channel,tag,&reply,sizeof(reply));return 0;
}
'''
tests=r'''
static void clean(void){assert(!live&&!current->cmd_map[0]);}
int main(void){struct dcp d={0};struct apple_dcp_afkep ep={.dcp=&d};struct apple_epic_service service={.ep=&ep,.channel=1};current=&service;
u8 input[16]={0},output[32];u32 rc=0;size_t received=999;int ret;
assert(afk_reply_size_valid(false,0,32,0)==-EPROTO);
assert(afk_reply_size_valid(true,33,32,0)==-EMSGSIZE);
assert(afk_service_body_size_valid(63,64,0,32)==-EPROTO);
assert(afk_service_body_size_valid(SIZE_MAX,64,SIZE_MAX,32)==-EPROTO);
scenario=FULL;response_size=8;memset(output,0xcc,sizeof(output));
ret=afk_send_command_with_reply_len(current,1,input,sizeof(input),output,sizeof(output),&rc,&received);
assert(!ret&&!rc&&received==8);for(int i=0;i<32;i++)assert(output[i]==(i<8?0x5a:0));clean();
/* Legacy fixed-capacity semantics remain unchanged. */
memset(output,0xcc,sizeof(output));ret=afk_send_command(current,1,input,sizeof(input),output,sizeof(output),&rc);
assert(!ret);for(int i=0;i<32;i++)assert(output[i]==0x5a);clean();
scenario=OVERSIZE;received=999;memset(output,0xcc,sizeof(output));
assert(afk_send_command_with_reply_len(current,1,input,16,output,32,&rc,&received)==-EMSGSIZE);assert(received==0&&output[0]==0xcc);clean();
scenario=SHORT;assert(afk_send_command_with_reply_len(current,1,input,16,output,32,&rc,&received)==-EPROTO);clean();
scenario=SHORT_ACK;
assert(afk_send_command_with_reply_len(current,1,input,16,output,32,&rc,&received)==-ETIMEDOUT);
assert(!received&&live==2&&current->cmd_map[0]);
struct epic_cmd late=sent_cmd;late.rxlen=8;
afk_recv_handle_reply(&ep,1,sent_tag^0x100,&late,sizeof(late));assert(live==2);
afk_recv_handle_reply(&ep,1,sent_tag,&late,sizeof(late));clean();
afk_recv_handle_reply(&ep,1,sent_tag,&late,sizeof(late));clean();
scenario=REMOTE_ERROR;response_size=8;
assert(!afk_send_command_with_reply_len(current,1,input,16,output,32,&rc,&received));assert(rc==42&&received==8);clean();
memset(output,0xcc,sizeof(output));
assert(afk_service_call_with_reply_len(current,1,7,input,16,0,output,32,0,&received)==-EINVAL);assert(!received&&output[0]==0xcc);clean();
scenario=SEND_ERROR;assert(afk_send_command_with_reply_len(current,1,input,16,output,32,&rc,&received)==-EIO);clean();
scenario=TIMEOUT;unsigned oldfree=frees;
assert(afk_send_command_with_reply_len(current,1,input,16,output,32,&rc,&received)==-ETIMEDOUT);
assert(live==2&&frees==oldfree&&current->cmd_map[0]);
struct epic_cmd reply=sent_cmd;reply.rxlen=8;
afk_recv_handle_reply(&ep,1,sent_tag,&reply,sizeof(reply));clean();
assert(frees==oldfree+2); /* Free ORIGINAL capacities, not received length. */
scenario=SERVICE;memset(output,0xcc,sizeof(output));
assert(!afk_service_call_with_reply_len(current,1,7,input,16,0,output,32,0,&received));assert(received==8&&output[0]==0x5a&&output[8]==0);clean();
scenario=SERVICE;memset(output,0xcc,sizeof(output));
assert(afk_service_call_with_reply_len(current,1,7,input,16,0,output,4,0,&received)==-EMSGSIZE);assert(!received&&output[0]==0xcc);clean();
scenario=SERVICE_TRUNCATED;assert(afk_service_call_with_reply_len(current,1,7,input,16,0,output,32,0,&received)==-EPROTO);assert(!received);clean();
scenario=SERVICE_OVERSIZE;assert(afk_service_call_with_reply_len(current,1,7,input,16,0,output,32,0,&received)==-EPROTO);clean();
assert(afk_service_call_with_reply_len(current,1,7,input,SIZE_MAX,1,output,32,0,&received)==-EOVERFLOW);clean();
assert(afk_service_call_with_reply_len(current,1,7,input,16,0,output,SIZE_MAX,1,&received)==-EOVERFLOW);clean();
assert(afk_service_call_with_reply_len(current,1,7,input,16,0,output,32,0,NULL)==-EINVAL);clean();
puts("AFK actual command/reply/service and delayed-DMA cleanup checks passed");}
'''
functions='\n'.join(function(n) for n in ('afk_recv_handle_reply','afk_send_command_common','afk_send_command','afk_send_command_with_reply_len','afk_service_call_common','afk_service_call','afk_service_call_with_reply_len'))
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(preamble+helper+'\n'+functions+mock+tests)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
