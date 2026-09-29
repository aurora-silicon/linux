/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* 14.6 DCPDPTXRemotePortEPClient table at DATA 0x603e48: command 9,
 * 16-byte payload, InterruptRequestOccurred -> TEXT 0x37bd0 -> virtual 360.
 * Same group 8 as HPD change. This is a DPCD service interrupt, not a cable
 * disconnect. Give it separate pinned buffers: firmware may be waiting for
 * this IRQ while the ordinary HPD or native modeset RPC is still pending.
 */
static u32 (*hdmi_irq_count)(void);
static u32 hdmi_irq_seen,hdmi_irq_sent,hdmi_irq_acked;
module_param(hdmi_irq_sent,uint,0400);
module_param(hdmi_irq_acked,uint,0400);
static u16 hdmi_irq_tag=0x8000;
static u8 *hdmi_irq_tx,*hdmi_irq_rx;
static u64 hdmi_irq_txd,hdmi_irq_rxd,hdmi_irq_deadline;
static bool hdmi_irq_pending;
/* GPIO47 did not signal the converter's FRL-ready/active service events on
 * J514S. Poll the standard firmware service handler while connected as a
 * fallback. 100 ms is below both the 500 ms ready and 450 ms training waits;
 * only one request may be outstanding, with a bounded response deadline.
 * No reads of converter registers or modifications of training policy. */
static u64 hdmi_service_next;

static int hdmi_irq_progress(void)
{
 u8 wire[70]={};u32 count;int ret;
 if(!hdmi_irq_count)return 0;
 if(hdmi_irq_pending)
  return session_now(NULL)>=hdmi_irq_deadline ? -ETIMEDOUT : 0;
 count=hdmi_irq_count();
 if(!port0_announced || rpc_stage<2){hdmi_irq_seen=count;return 0;}
 if(session.phase!=DCPEXT_RUNNING)return 0;
 if(READ_ONCE(desktop_stop) || desktop_disconnected)return 0;
 if(count==hdmi_irq_seen && session_now(NULL)<hdmi_service_next)return 0;
 hdmi_service_next=session_now(NULL)+100;
 if(!hdmi_irq_tx){
  ret=afk_allocate(NULL,80,&hdmi_irq_tx,&hdmi_irq_txd);if(ret)return ret;
 }
 if(!hdmi_irq_rx){
  ret=afk_allocate(NULL,80,&hdmi_irq_rx,&hdmi_irq_rxd);if(ret)return ret;
 }
 memset(hdmi_irq_tx,0,80);memset(hdmi_irq_rx,0,80);
 put_unaligned_le16(8,hdmi_irq_tx+2);
 put_unaligned_le32(9,hdmi_irq_tx+4);
 put_unaligned_le32(16,hdmi_irq_tx+8);
 put_unaligned_le32(0x69706378,hdmi_irq_tx+12);
 wire[0]=2;put_unaligned_le16(epic_seq++,wire+1);
 put_unaligned_le32(30,wire+16);wire[20]=4;wire[21]=0x30;
 put_unaligned_le16(0xc0,wire+22);put_unaligned_le16(hdmi_irq_tag,wire+32);
 put_unaligned_le64(hdmi_irq_rxd,wire+44);put_unaligned_le64(hdmi_irq_txd,wire+52);
 put_unaligned_le32(80,wire+60);put_unaligned_le32(80,wire+64);
 ret=old_epic_write(port0_channel,3,wire,sizeof(wire));if(ret)return ret;
 hdmi_irq_pending=true;hdmi_irq_seen=count;
 hdmi_irq_deadline=session_now(NULL)+1000;hdmi_irq_sent++;
 return 0;
}

static int hdmi_irq_record(u32 channel,u32 kind,const u8 *p,u32 size)
{
 if(!hdmi_irq_pending || (kind!=0 && kind!=4) || channel!=port0_channel ||
    size<70 || p[0]!=2 || p[20]!=4 || p[21]!=0x20 ||
    get_unaligned_le16(p+22)!=0xc0 || get_unaligned_le16(p+32)!=hdmi_irq_tag)
  return 0;
 if(get_unaligned_le32(p+40) || get_unaligned_le64(p+44)!=hdmi_irq_rxd ||
    get_unaligned_le32(p+60)!=80)return -EPROTO;
 dma_rmb();
 if(get_unaligned_le16(hdmi_irq_rx+2)!=8 || get_unaligned_le32(hdmi_irq_rx+4)!=9 ||
    get_unaligned_le32(hdmi_irq_rx+12)!=0x69706378 ||
    get_unaligned_le32(hdmi_irq_rx+64))return -EPROTO;
 hdmi_irq_pending=false;hdmi_irq_acked++;
 hdmi_irq_tag=0x8000|((hdmi_irq_tag+1)&0x7fff);
 return 1;
}
