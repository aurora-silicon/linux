/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* J514S 14.6 iBoot DMA commands: m1n1 src/dcp_iboot.c and fw/dcp/iboot.py.
 * Bounded mode enumeration; opt-in iboot_image adds a retained static surface.
 */
static bool iboot_modes;
module_param(iboot_modes,bool,0400);
static bool ib_announced,ib_pending,ib_done;
static u32 ib_channel,ib_stage,ib_operation,ib_tag,ib_timings,ib_colors;
static u64 ib_hold_until;
static u8 ib_mode[48];
static bool ib_timing_found,ib_color_found;
static u64 ib_next_query;
static unsigned int ib_attempts;
static u8 *ib_rx,*ib_tx;
static u64 ib_tx_dva;
static u64 ib_rx_dva;
static int ib_send(u32 op)
{
 u8 *tx,wire[70]={};u64 tx_dva;u32 size=op==2?17:op==6?64:op==1?188:16;int ret;
 if(ib_pending || !ib_announced)return -EBUSY;
 if(!ib_rx){
  ret=afk_allocate(NULL,4096,&ib_rx,&ib_rx_dva);if(ret)return ret;
  ret=afk_allocate(NULL,256,&ib_tx,&ib_tx_dva);if(ret)return ret;
 }
 tx=ib_tx;tx_dva=ib_tx_dva;memset(tx,0,256);memset(ib_rx,0,4096);
 put_unaligned_le32(op,tx);put_unaligned_le32(size,tx+4);
 if(op==2)tx[16]=ib_stage==1;
 if(op==6){
  if(!ib_timing_found || !ib_color_found)return -ENODATA;
  memcpy(tx+16,ib_mode,48);
 }
 if(op==1){
  /* m1n1 13.3+ layer: three 44-byte planes plus metadata and padding. */
  put_unaligned_le64(BIT_ULL(40)|FRAME_DVA,tx+16+4);
  put_unaligned_le32(1920*4,tx+16+16);
  put_unaligned_le32(1,tx+16+36);
  put_unaligned_le32(1,tx+16+136);
  put_unaligned_le32(1920,tx+16+140);put_unaligned_le32(1080,tx+16+144);
  put_unaligned_le32(1,tx+16+148);put_unaligned_le32(1,tx+16+152);put_unaligned_le32(1,tx+16+156);
 }
 wire[0]=2;put_unaligned_le16(epic_seq++,wire+1);
 put_unaligned_le32(30,wire+16);wire[20]=4;wire[21]=0x30;
 put_unaligned_le16(0xc0,wire+22);put_unaligned_le16(ib_tag,wire+32);
 put_unaligned_le64(ib_rx_dva,wire+44);put_unaligned_le64(tx_dva,wire+52);
 put_unaligned_le32(4096,wire+60);put_unaligned_le32(size,wire+64);
 ib_operation=op;ib_pending=true;
 pr_info("m3_dcpext_iboot: send op=%u stage=%u tag=%u\n",op,ib_stage,ib_tag);
 return old_epic_write_channel(&aux_afk,ib_channel,3,wire,sizeof(wire));
}
static int ib_record(u32 channel,const u8 *p,u32 size)
{
 u32 bytes,count;
 if(size>=108 && p[0]==2 && p[20]==4 && p[21]==0 &&
    get_unaligned_le16(p+22)==0x30 && !memcmp(p+40,"disp0-service",14)){
  if(ib_announced)return -EPROTO;
  ib_announced=true;ib_channel=channel;return 0;
 }
 if(size<70 || p[0]!=2 || p[20]!=4 || p[21]!=0x20 ||
    get_unaligned_le16(p+22)!=0xc0)return 0;
 if(!ib_pending || channel!=ib_channel || get_unaligned_le16(p+32)!=ib_tag ||
    get_unaligned_le64(p+44)!=ib_rx_dva)return -EPROTO;
 bytes=get_unaligned_le32(p+60);
 pr_info("m3_dcpext_iboot: reply op=%u status=%#x bytes=%u\n",ib_operation,get_unaligned_le32(p+40),bytes);
 if(get_unaligned_le32(p+40))return -EREMOTEIO;
 if(bytes>4096)return -EOVERFLOW;
 dma_rmb();
 if(ib_operation!=2 && ib_operation!=6 && ib_operation!=1){
  if(bytes<8 || get_unaligned_le32(ib_rx)!=ib_operation ||
     get_unaligned_le32(ib_rx+4)>bytes || get_unaligned_le32(ib_rx+4)<8)return -EPROTO;
  if(ib_operation==3){
   if(bytes<20 || ib_rx[8]>1)return -EPROTO;
   if(!ib_rx[8] || !get_unaligned_le32(ib_rx+12) || !get_unaligned_le32(ib_rx+16)){
    if(++ib_attempts>=16)return -ENODEV;
    ib_next_query=session_now(NULL)+250;ib_pending=false;ib_tag++;return 0;
   }
   ib_timings=get_unaligned_le32(ib_rx+12);ib_colors=get_unaligned_le32(ib_rx+16);
   if(!ib_timings || !ib_colors || ib_timings>128 || ib_colors>128)return -ERANGE;
   pr_info("m3_dcpext_iboot: HPD=1 timing_count=%u color_count=%u\n",ib_timings,ib_colors);
  }else{
   if(bytes<12)return -EPROTO;
   count=get_unaligned_le32(ib_rx+8);
   if(count!=(ib_operation==4?ib_timings:ib_colors) || bytes<12+count*24)return -EPROTO;
   for(unsigned int i=0;i<count;i++){
    u8 *m=ib_rx+12+i*24;
    if(ib_operation==4 && !ib_timing_found && get_unaligned_le32(m)==1 &&
       get_unaligned_le32(m+4)==1920 && get_unaligned_le32(m+8)==1080 && get_unaligned_le32(m+12)==(60<<16)){
     memcpy(ib_mode,m,24);ib_timing_found=true;
    }
    if(ib_operation==5 && !ib_color_found && get_unaligned_le32(m)==1 &&
       get_unaligned_le32(m+4)==1 && get_unaligned_le32(m+8)==1 &&
       get_unaligned_le32(m+12)==1 && get_unaligned_le32(m+16)==32){
     memcpy(ib_mode+24,m,24);ib_color_found=true;
    }
    pr_info("m3_dcpext_iboot: op=%u mode=%u words=%u,%u,%u,%u,%u,%u\n",ib_operation,i,
     get_unaligned_le32(m),get_unaligned_le32(m+4),get_unaligned_le32(m+8),
     get_unaligned_le32(m+12),get_unaligned_le32(m+16),get_unaligned_le32(m+20));
   }
  }
 }
 if(ib_operation==2 && ib_stage==1)ib_next_query=session_now(NULL)+100; /* m1n1 Sonoma workaround */
 ib_pending=false;ib_tag++;ib_stage++;
 if(iboot_image && ib_stage==6){ib_hold_until=session_now(NULL)+5000;pr_info("m3_dcpext_iboot: test surface accepted; holding five seconds\n");}
 if(ib_stage==(iboot_image?7:5))ib_done=true;
 return 0;
}
static int ib_poll(void)
{
 static const u32 ops[]={3,2,4,5,6,1,2};
 if(!iboot_modes || ib_done || ib_pending || !ib_announced || rpc_stage!=3 ||
    session_now(NULL)+1700<hpd_until || session_now(NULL)<ib_next_query)return 0;
 if(ib_stage>=ARRAY_SIZE(ops))return -EPROTO;
 if(iboot_image && ib_stage==6 && session_now(NULL)<ib_hold_until)return 0;
 return ib_send(!iboot_image && ib_stage==4?2:ops[ib_stage]);
}
