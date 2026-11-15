#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check full AFKV2 frame size against the production ring-admission block."""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text()
start = source.index("\ttotal_epic_size = sizeof(*ehdr) +", source.index("static int afk_send_epic_token", source.index("int afk_receive_message")))
end = source.index("\n\thdr = hdr2 = NULL;", start)
block = source[start:end]
prefix = r'''
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include "afkv2-codec.h"
#define IS_ALIGNED(x,a) (!((x)&((a)-1)))
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
struct epic_hdr { u8 data[16]; };
struct epic_sub_hdr { u8 data[24]; };
struct epic_sub_hdr_h17p { u8 data[8]; };
struct afk_qe { u8 data[16]; };
struct ring { bool ready; u32 stride,bufsz; };
struct ep { bool wire_v2; struct ring txbfr; };
static int check_ring(struct ep *ep,u32 rptr,u32 wptr,size_t payload_len,size_t *total)
{
 struct epic_hdr *ehdr;struct epic_sub_hdr *eshdr;struct afk_qe *hdr;
 size_t total_epic_size,total_size;int ret=0;
'''
suffix = r'''
 *total=total_size;
out:
 return ret;
}
int main(void)
{
 u8 request[112]={0},command[120],frame[144];size_t written,total;
 struct ep ep={.wire_v2=true,.txbfr={.ready=true,.stride=64,.bufsz=256}};
 assert(!afkv2_encode_inline_command(command,sizeof(command),1,request,sizeof(request),32888));
 assert(!afkv2_encode_frame(frame,sizeof(frame),0,1,0,0xc0,AFKV2_COMMAND,0,command,sizeof(command),&written));
 assert(written==144 && written+sizeof(struct afk_qe)==160);
 assert(!check_ring(&ep,0,0,sizeof(command),&total));
 assert(total==160 && ALIGN(total,64)==192);
 ep.txbfr.bufsz=192;assert(check_ring(&ep,0,0,sizeof(command),&total)==-ENOSPC);
 ep.txbfr.bufsz=128;assert(check_ring(&ep,0,0,sizeof(command),&total)==-EPROTO);
 ep.txbfr.bufsz=256;assert(check_ring(&ep,192,0,sizeof(command),&total)==-ENOSPC);
 assert(check_ring(&ep,0,1,sizeof(command),&total)==-EPROTO);
 puts("PASS: full request 160 bytes / 192-byte aligned ring consumption; 256-byte empty ring accepted, smaller capacity rejected");
}
'''
pathlib.Path(sys.argv[2]).write_text(prefix + block + suffix)
