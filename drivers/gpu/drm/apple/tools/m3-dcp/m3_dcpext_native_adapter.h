/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#include "m3_dcpext_rpc.h"
#include "m3_dcpext_native.h"
#include "m3_dcpext_modes.h"
static bool native_frame_done, native_frame_submitted;
static u64 native_frame_until;
static struct m3_dcpext_rpc *native_rpc;
static struct m3_dcpext_native *native_client;
static bool native_started;
static unsigned int received;
static int session_dispatch(u64 endpoint,u64 message);
static int native_receive(u64 message)
{
 if(!native_rpc)return native_query_receive(message);
 m3_dcpext_rpc_receive(native_rpc,message);return 0;
}
static int native_poll(void *cookie,unsigned long timeout)
{
 u64 deadline=session_now(NULL)+jiffies_to_msecs(timeout);
 int ret;
 do {
  u64 msg,ep;
  ret=dcpext_session_poll(&session);if(ret)return ret;
  if(!(readl(session_fifo+0x114)&BIT(17))){
   msg=readq(session_fifo+0x830);ep=readq(session_fifo+0x838);dma_rmb();
   return session_dispatch(ep,msg);
  }
  usleep_range(100,200);
 }while(session_now(NULL)<deadline);
 return -ETIMEDOUT;
}
static struct {struct m3_dcpext_buffer info;bool retired;} native_buffers[32];
static u32 native_buffers_used;
static int native_alloc(void *cookie,struct m3_dcpext_buffer *info)
{
 size_t offset=session_used;u64 dva;int ret;
 if(!info->size || info->size>0x100000 || native_buffers_used==ARRAY_SIZE(native_buffers))return -E2BIG;
 ret=session_buffer(NULL,0x37,0,info->size,&dva);if(ret)return ret;
 info->size=ALIGN(info->size,LEASE_PAGE);info->dva=dva;info->physical=pool_dma+offset;
 info->id=++native_buffers_used;native_buffers[info->id-1].info=*info;
 return 0;
}
static int native_map(void *cookie,struct m3_dcpext_buffer *info)
{
 if(!pio_published || !info->id || info->id>native_buffers_used || native_buffers[info->id-1].retired)return -EINVAL;
 *info=native_buffers[info->id-1].info;return 0;
}
static int native_retire(void *cookie,u32 id)
{
 if(!id || id>native_buffers_used || native_buffers[id-1].retired)return -EINVAL;
 native_buffers[id-1].retired=true;return 0;
}
static int native_power(void *cookie,bool on)
{
 if(!pio_published)return -EUCLEAN;
 pr_info("m3_dcpext_native: firmware DART power=%u; session vote retained\n",on);return 0;
}
static u8 native_capture[0x800000];
static u32 native_capture_used;
static int native_record(void *cookie,u32 kind,u64 msg,const void *data,u32 size)
{
 u8 *p=native_capture+native_capture_used;
 if(session.runtime)return 0;
 if(size>sizeof(native_capture)-native_capture_used-16){
  /* Diagnostics must never break the protocol they observe. */
  pr_warn_once("m3_dcpext: native capture full; retaining prefix, RPC continues\n");
  return 0;
 }
 put_unaligned_le32(kind,p);put_unaligned_le32(size,p+4);put_unaligned_le64(msg,p+8);
 memcpy(p+16,data,size);native_capture_used+=size+16;return 0;
}
static const struct m3_dcpext_rpc_ops native_ops={.send=session_send,.poll=native_poll,
 .alloc=native_alloc,.map=native_map,.retire=native_retire,.dart_power=native_power,.record=native_record};
static void native_surface(u8 *s,bool opaque,u32 width,u32 height,u32 pitch)
{
 memset(s,0,0x22c);s[2]=opaque;
 /* Qualified M3 linear BGRA s, plane 0. */
 put_unaligned_le32(1,s+3);put_unaligned_le32(1,s+7);
 put_unaligned_le32(0x42475241,s+0xb);s[0x13]=13;s[0x14]=12;
 put_unaligned_le32(pitch,s+0x15);put_unaligned_le16(1,s+0x19);
 s[0x1b]=s[0x1c]=1;
 put_unaligned_le32(width,s+0x21);put_unaligned_le32(height,s+0x25);
 put_unaligned_le32(pitch*height,s+0x29);put_unaligned_le32(1,s+0x35);
 put_unaligned_le64(1,s+0x51);put_unaligned_le32(width,s+0x59);
 put_unaligned_le32(height,s+0x5d);put_unaligned_le32(pitch,s+0x69);
 put_unaligned_le32(pitch*height,s+0x6d);put_unaligned_le16(4,s+0x71);
 s[0x73]=s[0x74]=1;put_unaligned_le64(1,s+0x149);
}
static int native_kms_publish(void);
static bool native_kms_reconnecting(void);
static int native_kms_unavailable(int error);
static int native_modes_error;
static u64 native_modes_error_generation;
module_param(native_modes_error,int,0400);
static int native_frame_progress(void)
{
 struct m3_dcpext_native_mode *modes;
 const struct m3_dcpext_native_mode wanted = {
  .geometry = { .width=1920, .height=1080, .clock_khz=148500, .htotal=2200, .vtotal=1125 },
  .hfront=88, .hsync=44, .vfront=4, .vsync=5, .hpositive=true, .vpositive=true,
 };
 u32 bytes=0, count=0, selected; void *blob; int ret;
 u8 surface[0x22c] = {};
 if (!native_frame || native_frame_done) return 0;
 if(desktop_disconnected || desktop_hpd_dirty || desktop_hpd_rpc || desktop_link_stage)return 0;
 ret=m3_dcpext_native_metadata_error(native_client);
 if(ret)return native_kms_unavailable(ret);
 if(native_modes_error){
  if(native_modes_error_generation==m3_dcpext_native_generation(native_client))return 0;
  native_modes_error=0;native_frame_submitted=false;
 }
 if (native_frame_submitted) {
  if(native_kms_trial)return native_kms_publish();
  if (session_now(NULL)<native_frame_until) return 0;
  ret=m3_dcpext_native_background(native_client,0xff000000);
  if (!ret) ret=m3_dcpext_native_power(native_client,false);
  if (!ret) {native_frame_done=true;pr_info("m3_dcpext_native: frame hold and power-off completed\n");}
  return ret;
 }
 if(native_kms_reconnecting())return native_kms_publish();
 blob=m3_dcpext_native_property(native_client,"TimingElements",&bytes);
 if (IS_ERR(blob)) return native_kms_unavailable(PTR_ERR(blob));
 if (!blob) return 0;
 modes=kcalloc(M3_DCPEXT_NATIVE_MAX_MODES,sizeof(*modes),GFP_KERNEL);
 if (!modes) {kvfree(blob);return native_kms_unavailable(-ENOMEM);}
 ret=m3_dcpext_native_modes_parse_link(blob,bytes,modes,M3_DCPEXT_NATIVE_MAX_MODES,&count,link_payload_kbps());
 if (!ret) ret=m3_dcpext_native_mode_select(&wanted,modes,count,&selected);
 if(ret){kfree(modes);kvfree(blob);return native_kms_unavailable(ret);}
 if (!ret) {
  pr_info("m3_dcpext_native: selected timing=%u color=%u 1080p60 from %u modes\n",modes[selected].timing_id,modes[selected].color_id,count);
  ret=m3_dcpext_native_mode(native_client,modes[selected].color_id,modes[selected].timing_id);
 }
 kfree(modes);kvfree(blob);if (ret) return ret;
 ret=m3_dcpext_native_power(native_client,true);if (ret) return ret;
 native_surface(surface,false,1920,1080,7680);
 ret=m3_dcpext_native_swap(native_client,surface,FRAME_DVA|BIT_ULL(40),1920,1080);
 if (!ret) {
  native_frame_submitted=true;av_read_ready=true;native_frame_until=session_now(NULL)+5000;
  pr_info("m3_dcpext_native: test frame completed by D589; holding five seconds\n");
  if(native_kms_trial)return native_kms_publish();
 }
 return ret;
}
#include "m3_dcpext_kms_adapter.h"
static int native_progress(void)
{
 int ret;
 if(!native_startup || native_phase!=3 || !afk.running || rpc_stage!=3)return 0;
 if(!native_started){
  native_started=true;
  native_rpc=m3_dcpext_rpc_create(dma_dev,native_memory,&native_ops,NULL);
  if(IS_ERR(native_rpc)){ret=PTR_ERR(native_rpc);native_rpc=NULL;return ret;}
  native_client=m3_dcpext_native_start(dma_dev,native_rpc,false);
  if(IS_ERR(native_client)){ret=PTR_ERR(native_client);native_client=NULL;return ret;}
  pr_info("m3_dcpext_native: A401/A455 startup completed\n");
 }
 if(native_client){
  ret=m3_dcpext_native_pump(native_client,0);
  if(ret && ret!=-ETIMEDOUT && ret!=-EAGAIN)return ret;
  return native_frame_progress();
 }
 return 0;
}
static ssize_t native_capture_read(struct file *file,char __user *buf,size_t count,loff_t *offset)
{return simple_read_from_buffer(buf,count,offset,native_capture,native_capture_used);}
static const struct file_operations native_capture_fops={.owner=THIS_MODULE,.read=native_capture_read,.llseek=default_llseek};
