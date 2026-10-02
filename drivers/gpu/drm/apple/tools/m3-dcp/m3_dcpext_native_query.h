/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* M4's bounded DCPLink query design with the qualified M3 14.6 READY/A410.
 * No M4 hash or native startup ABI is assumed. Buffers remain owned to reboot. */
static bool native_query;
module_param(native_query,bool,0400);
static unsigned int native_phase;
static u8 *native_memory;
static u64 native_dva;
static int native_query_start(void)
{
 int ret=afk_allocate(NULL,0xc0000,&native_memory,&native_dva);
 if(ret)return ret;
 put_unaligned_le32(64,native_memory+12);
 native_phase=1;
 ret=session_send(NULL,0,(5ULL<<52)|(0x37ULL<<32)|2);
 dma_wmb();
 return ret?:session_send(NULL,0x37,(native_dva<<16)|0x40);
}
static int native_query_receive(u64 message)
{
 pr_info("m3_dcpext_native: phase=%u RX=%#llx\n",native_phase,message);
 if(native_phase==1){
  if(!message)return 0;
  if(message!=((64ULL<<16)|0x101))return -EPROTO;
  put_unaligned_le32(0x41343130,native_memory);
  put_unaligned_le32(0,native_memory+4);put_unaligned_le32(4,native_memory+8);
  put_unaligned_le32(0xa5,native_memory+12);native_phase=2;dma_wmb();
  return session_send(NULL,0x37,(16ULL<<32)|2);
 }
 dma_rmb();
 if(native_phase!=2 || message!=0x42 || get_unaligned_le32(native_memory)!=0x41343130 ||
    get_unaligned_le32(native_memory+4) || get_unaligned_le32(native_memory+8)!=4 ||
    get_unaligned_le32(native_memory+12))return -EPROTO;
 native_phase=3;
 pr_info("m3_dcpext_native: external READY and A410 main=0 qualified\n");
 return 0;
}
