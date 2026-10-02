/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* J514S old-EPIC audio transport. Service framing follows Asahi av.c and
 * m1n1 EPICStandardService; serialized lifetime follows the M4 audio port.
 * No PCM/DMA playback is initiated by this transport. */
#define M3_AUDIO_MAX (256 * 1024)
static struct afkc audio_afk;
static u32 audio_channel, audio_command, audio_group, audio_bytes, audio_received;
static u16 audio_tag;
static u32 audio_generation;
static bool audio_owned;
static bool audio_announced, audio_pending;
static int audio_error, audio_result = -ENODATA;
static u8 *audio_tx, *audio_rx;
static u64 audio_tx_dva, audio_rx_dva;
module_param(audio_result, int, 0400);

static int audio_record(void *cookie, const u8 *p, u32 size)
{
 const u8 *h = audio_afk.capture + audio_afk.capture_size - size - 12;
 u32 channel = get_unaligned_le32(h), kind = get_unaligned_le32(h + 4);
 if (size < 40 || p[0] != 2 || p[20] != 4) return -EPROTO;
 if (!p[21] && get_unaligned_le16(p + 22) == 0x30) {
  pr_info("m3_audio: announcement channel=%u name=%.*s bytes=%u\n", channel, min(size-40,32U), p+40, size);
  if (size < 76 || memcmp(p+40,"dispext0:dcpav-audio-interface",sizeof("dispext0:dcpav-audio-interface")-1) || get_unaligned_le32(p+72)!=0xd3)
   return 0;
  if (audio_announced) return -EALREADY;
  audio_channel=channel; audio_generation++; audio_announced=true; return 0;
 }
 if (channel != audio_channel) return 0;
 if (!p[21]) {
  if (get_unaligned_le16(p+22)==0x32) {
   audio_generation++;audio_announced=false; audio_pending=false; audio_error=-ENODEV;
  }
  return 0;
 }
 if (!audio_pending || (kind!=0 && kind!=4) || size<70 || p[21]!=0x20 ||
     get_unaligned_le16(p+22)!=0xc0 || get_unaligned_le16(p+32)!=audio_tag)
  return -EPROTO;
 if (get_unaligned_le32(p+40)) return -EREMOTEIO;
 if (get_unaligned_le64(p+44)!=audio_rx_dva || get_unaligned_le32(p+60)>audio_bytes)
  return -EPROTO;
 dma_rmb();
 audio_received=get_unaligned_le32(p+60);
 audio_pending=false;
 return 0;
}
static const struct afkc_ops audio_ops={.send=session_send,.allocate=afk_allocate,.record=audio_record};
static int audio_receive(u64 message)
{
 int ret=afkc_receive(&audio_afk,message);
 if(ret) {audio_error=ret;audio_pending=false;}
 return 0; /* Audio faults must not tear down video or free in-flight DMA. */
}
static int audio_wait(bool announcement)
{
 u64 until=session_now(NULL)+2000;
 int ret;
 while(announcement ? !audio_announced : audio_pending) {
  if(audio_error)return audio_error;
  ret=native_poll(NULL,msecs_to_jiffies(5));
  if(ret && ret!=-ETIMEDOUT)return ret;
  if(session_now(NULL)>=until)return -ETIMEDOUT;
 }
 return audio_error;
}
static int audio_call_locked(u16 group,u32 command,const void *body,u32 bytes)
{
 u8 wire[70]={};int ret;
 lockdep_assert_held(&native_route_lock);
 if(port || usb_c || !native_kms_ready || native_kms_finished || !native_kms_connected ||
    desktop_disconnected || READ_ONCE(desktop_stop) || !session.runtime)return -ENODEV;
 if(audio_error)return audio_error;
 if(bytes>M3_AUDIO_MAX+48)return -E2BIG;
 if(!audio_afk.started) {
  if(!(session.endpoints[1]&BIT(9)))return -ENODEV;
  ret=afk_allocate(NULL,M3_AUDIO_MAX+112,&audio_tx,&audio_tx_dva);if(ret)goto fail;
  ret=afk_allocate(NULL,M3_AUDIO_MAX+112,&audio_rx,&audio_rx_dva);if(ret)goto fail;
  audio_afk.streaming=true;
  ret=afkc_start(&audio_afk,&audio_ops,NULL,0x29);if(ret)goto fail;
 }
 ret=audio_wait(true);if(ret)goto fail;
 if(audio_pending){ret=-EBUSY;goto fail;}
 audio_bytes=bytes+64;audio_group=group;audio_command=command;audio_tag++;
 memset(audio_tx,0,audio_bytes);memset(audio_rx,0,audio_bytes);
 put_unaligned_le16(group,audio_tx+2);put_unaligned_le32(command,audio_tx+4);
 put_unaligned_le32(bytes,audio_tx+8);put_unaligned_le32(0x69706378,audio_tx+12);
 memcpy(audio_tx+64,body,bytes);
 wire[0]=2;put_unaligned_le16(epic_seq++,wire+1);put_unaligned_le32(30,wire+16);
 wire[20]=4;wire[21]=0x30;put_unaligned_le16(0xc0,wire+22);put_unaligned_le16(audio_tag,wire+32);
 put_unaligned_le64(audio_rx_dva,wire+44);put_unaligned_le64(audio_tx_dva,wire+52);
 put_unaligned_le32(audio_bytes,wire+60);put_unaligned_le32(audio_bytes,wire+64);
 audio_pending=true;audio_received=0;dma_wmb();
 ret=old_epic_write_channel(&audio_afk,audio_channel,3,wire,sizeof(wire));if(ret)goto fail;
 ret=audio_wait(false);if(ret)goto fail;
 if(audio_received<64 || get_unaligned_le16(audio_rx+2)!=group ||
    get_unaligned_le32(audio_rx+4)!=command || get_unaligned_le32(audio_rx+12)!=0x69706378 ||
    get_unaligned_le32(audio_rx+8)>audio_received-64) {ret=-EPROTO;goto fail;}
 audio_result=0;
 pr_info("m3_audio: group=%u command=%u response=%u body=%u\n",group,command,audio_received,get_unaligned_le32(audio_rx+8));
 return 0;
fail:
 audio_result=audio_error=ret;audio_pending=false;
 pr_err("m3_audio: error=%d; all firmware DMA retained until reboot\n",ret);
 return ret;
}
static ssize_t audio_request_write(struct file *file,const char __user *buf,size_t count,loff_t *offset)
{
 u8 *request;u32 group,command,bytes;int ret;
 if(count<8 || count>M3_AUDIO_MAX+56)return -EINVAL;
 request=memdup_user(buf,count);if(IS_ERR(request))return PTR_ERR(request);
 group=get_unaligned_le32(request);command=get_unaligned_le32(request+4);bytes=count-8;
 /* Initial metadata qualification only: no stream/prepare requests. */
 if(!((group==0 && ((command==4 && bytes==32)||(command==5 && bytes==16))) ||
      (group==1 && (command==16 || command==18) && bytes>=48))) {kfree(request);return -EINVAL;}
 mutex_lock(&native_route_lock);
 ret=audio_owned ? -EBUSY : audio_call_locked(group,command,request+8,bytes);
 mutex_unlock(&native_route_lock);kfree(request);
 return ret?ret:count;
}
static ssize_t audio_response_read(struct file *file,char __user *buf,size_t count,loff_t *offset)
{
 ssize_t ret;
 mutex_lock(&native_route_lock);
 ret=simple_read_from_buffer(buf,count,offset,audio_rx,audio_received);
 mutex_unlock(&native_route_lock);return ret;
}
static ssize_t audio_capture_read(struct file *file,char __user *buf,size_t count,loff_t *offset)
{
 ssize_t ret;
 mutex_lock(&native_route_lock);
 ret=simple_read_from_buffer(buf,count,offset,audio_afk.capture,audio_afk.capture_size);
 mutex_unlock(&native_route_lock);return ret;
}
static const struct file_operations audio_request_fops={.owner=THIS_MODULE,.write=audio_request_write};
static const struct file_operations audio_response_fops={.owner=THIS_MODULE,.read=audio_response_read,.llseek=default_llseek};
static const struct file_operations audio_capture_fops={.owner=THIS_MODULE,.read=audio_capture_read,.llseek=default_llseek};
static void audio_debugfs(struct dentry *dir)
{
 if(port)return;
 debugfs_create_file("audio-request",0200,dir,NULL,&audio_request_fops);
 debugfs_create_file("audio-response",0400,dir,NULL,&audio_response_fops);
 debugfs_create_file("audio-capture",0400,dir,NULL,&audio_capture_fops);
}

#if DCPEXT_INSTANCE == 0
#include "m3_dcpext_audio_api.h"
static u64 audio_current_generation(void)
{return ((u64)audio_generation<<32)|(u32)native_kms_generation;}
static bool audio_live(void)
{return !port && !usb_c && native_kms_ready && !native_kms_finished && native_kms_connected &&
 !desktop_disconnected && !READ_ONCE(desktop_stop) && session.runtime;}
static int audio_identify(char *name,size_t size,u64 *generation)
{
 int ret=0;
 mutex_lock(&native_route_lock);
 if(!audio_live()){ret=-ENODEV;goto out;}
 strscpy(name,"M3 HDMI",size);
 if(av_edid_bytes>=128)for(u32 off=54;off<=108;off+=18) {
  const u8 *d=av_edid+off;
  if(d[0] || d[1] || d[2] || d[3]!=0xfc)continue;
  char display[14];memcpy(display,d+5,13);display[13]=0;
  for(u32 i=0;i<13;i++)if(display[i]=='\n'){display[i]=0;break;}
  strscpy(name,display,size);break;
 }
 *generation=audio_current_generation();
out:mutex_unlock(&native_route_lock);return ret;
}
static int audio_pcm_call(u16 group,u32 command,void *data,u32 bytes,u64 *generation)
{
 int ret;
 if(!data || !generation)return -EINVAL;
 mutex_lock(&native_route_lock);
 if(!audio_live() || (*generation && *generation!=audio_current_generation())){ret=-ENODEV;goto out;}
 /* Lazy endpoint publication may establish the first audio generation. */
 bool first=!audio_afk.started;
 ret=audio_call_locked(group,command,data,bytes);
 if(ret)goto out;
 if(!first && *generation && *generation!=audio_current_generation()){ret=-ENODEV;goto out;}
 *generation=audio_current_generation();
 u32 received=get_unaligned_le32(audio_rx+8);
 if(received>bytes){ret=-EPROTO;goto out;}
 memset(data,0,bytes);memcpy(data,audio_rx+64,received);
out:mutex_unlock(&native_route_lock);return ret;
}
static const struct m3_dcpext_audio_ops audio_pcm_ops={.call=audio_pcm_call,.identify=audio_identify};
const struct m3_dcpext_audio_ops *m3_dcpext_audio_get(unsigned int controller)
{
 if(controller)return NULL;
 mutex_lock(&native_route_lock);audio_owned=true;mutex_unlock(&native_route_lock);
 return &audio_pcm_ops;
}
EXPORT_SYMBOL_GPL(m3_dcpext_audio_get);
#endif
