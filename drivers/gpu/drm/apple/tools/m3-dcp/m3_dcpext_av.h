/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Old EPIC AV service lifecycle: m1n1 EPICStandardService group4 open/close;
 * Asahi dpavservep copyEDID group1/command7. Buffers retained until reboot. */
static bool av_read_ready;
static bool av_announced,av_pending,av_opened,av_closed,av_edid_done;
static u32 av_channel,av_command,av_group,av_size,av_tag;
static u8 *av_rx,*av_tx;static u64 av_rx_dva,av_tx_dva;static u8 av_edid[1024];static u32 av_edid_bytes;
static int av_send(u16 group,u32 command,u32 bytes)
{
 u8 *tx,wire[70]={};u64 txd;int ret;
 if(av_pending || !av_announced)return -EINVAL;
 if(bytes>1144)return -E2BIG;
 if(!av_rx){ret=afk_allocate(NULL,1144,&av_rx,&av_rx_dva);if(ret)return ret;}
 if(!av_tx){ret=afk_allocate(NULL,1144,&av_tx,&av_tx_dva);if(ret)return ret;}
 tx=av_tx;txd=av_tx_dva;memset(tx,0,1144);memset(av_rx,0,1144);
 put_unaligned_le16(group,tx+2);put_unaligned_le32(command,tx+4);
 put_unaligned_le32(bytes-64,tx+8);put_unaligned_le32(0x69706378,tx+12);
 if(group==1)put_unaligned_le64(1032,tx+64);
 wire[0]=2;put_unaligned_le16(epic_seq++,wire+1);put_unaligned_le32(30,wire+16);
 wire[20]=4;wire[21]=0x30;put_unaligned_le16(0xc0,wire+22);put_unaligned_le16(av_tag,wire+32);
 put_unaligned_le64(av_rx_dva,wire+44);put_unaligned_le64(txd,wire+52);
 put_unaligned_le32(bytes,wire+60);put_unaligned_le32(bytes,wire+64);
 av_pending=true;av_group=group;av_command=command;av_size=bytes;
 pr_info("m3_dcpext_av: sending group=%u command=%u bytes=%u\n",group,command,bytes);
 return old_epic_write_channel(&aux_afk,av_channel,3,wire,sizeof(wire));
}
static int av_record(u32 channel,u32 kind,const u8 *p,u32 size)
{
 if(size>=76 && p[0]==2 && p[20]==4 && !p[21] && get_unaligned_le16(p+22)==0x30 &&
    !memcmp(p+40,port?"dispext1:dcpav-service-epic:0":"dispext0:dcpav-service-epic:0",sizeof("dispext0:dcpav-service-epic:0")) && get_unaligned_le32(p+72)==0xd3){
  if(av_announced)return -EALREADY;
  av_announced=true;av_closed=false;av_channel=channel;pr_info("m3_dcpext_av: service announced channel=%u\n",channel);return 0;
 }
 if(size>=40 && av_announced && channel==av_channel && p[0]==2 && p[20]==4 && !p[21] && get_unaligned_le16(p+22)==0x32){
  pr_info("m3_dcpext_av: service teardown report; no DMA released\n");
  /* Teardown is not proof that a pending DMA has stopped. */
  if(av_pending)return -EBUSY;
  av_announced=av_opened=false;av_closed=true;return 0;
 }
 if(!av_pending || channel!=av_channel || (kind!=0 && kind!=4))return 0;
 if(size<70 || p[21]!=0x20 || get_unaligned_le16(p+22)!=0xc0 || get_unaligned_le16(p+32)!=av_tag ||
    get_unaligned_le32(p+40) || get_unaligned_le64(p+44)!=av_rx_dva ||
    get_unaligned_le32(p+60)>av_size)return -EPROTO;
 dma_rmb();
 if(get_unaligned_le16(av_rx+2)!=av_group || get_unaligned_le32(av_rx+4)!=av_command ||
    get_unaligned_le32(av_rx+12)!=0x69706378)return -EPROTO;
 av_pending=false;av_tag++;
 if(av_group==4){av_opened=av_command==6;av_closed=av_command==7;}
 else {
  u64 used=get_unaligned_le64(av_rx+96);const u8 *edid=av_rx+120;u32 bytes;
  if(get_unaligned_le64(av_rx+64)!=1032 || used<136 || used>1032 ||
     get_unaligned_le32(av_rx+112)!=0xd3)return -EPROTO;
  bytes=used-8;
  if(bytes%128 || bytes!=128*((u32)edid[126]+1) || memcmp(edid,"\0\xff\xff\xff\xff\xff\xff\0",8) ||
     get_unaligned_le32(av_rx+116)!=(0x8a000000|bytes))return -EBADMSG;
  for(u32 at=0;at<bytes;at+=128){u8 sum=0;for(u32 i=0;i<128;i++)sum+=edid[at+i];if(sum)return -EBADMSG;}
  memcpy(av_edid,edid,bytes);av_edid_bytes=bytes;av_edid_done=!desktop_disconnected;
  pr_info("m3_dcpext_av: checked live EDID %u bytes\n",bytes);
 }
 pr_info("m3_dcpext_av: group=%u command=%u reply accepted\n",av_group,av_command);return 0;
}
static int av_progress(void)
{
 if(aux_endpoint!=0x28 || !av_announced || av_pending || av_closed)return 0;
 if(!av_opened)return av_send(4,6,96);
 if(rpc_stage==5)return av_send(4,7,80);
 if(av_read_ready && !av_edid_done)return av_send(1,7,64+48+1032);
 return 0;
}
