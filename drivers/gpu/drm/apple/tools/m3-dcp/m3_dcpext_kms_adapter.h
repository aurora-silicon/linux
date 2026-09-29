/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* M4 native KMS ownership pattern adapted to J514S old RPC protocol. */
#include <linux/iosys-map.h>
#include "m3_dcpext_kms.h"
static DEFINE_MUTEX(native_route_lock);
static struct m3_dcpext_kms *native_kms;
static bool native_kms_ready,native_kms_finished,native_kms_powered=true;
static bool native_kms_connected;
static unsigned int native_kms_frames,native_kms_slot;
static u64 native_kms_until,native_kms_generation;
module_param(native_kms_ready,bool,0400);
module_param(native_kms_frames,uint,0400);
static int native_kms_present(void *cookie,const struct m3_dcpext_native_mode *mode,
 u64 generation,const struct iosys_map *pixels,u32 source_pitch,bool opaque)
{
 u8 surface[0x22c];void *destination;u64 dva;int ret;
 lockdep_assert_held(&native_route_lock);
 /* Hotplug is an ordinary stale atomic commit, not a poisoned device. */
 if(desktop_disconnected || !native_kms_connected)return mode?-ESTALE:0;
 if(!mode){
  if(!native_kms_powered)return 0;
  ret=m3_dcpext_native_power(native_client,false);
  if(!ret)native_kms_powered=false;return ret;
 }
 if(native_kms_finished || !native_kms_ready || generation!=native_kms_generation ||
    generation!=m3_dcpext_native_generation(native_client))return -ESTALE;
 if(mode->geometry.width!=1920 || mode->geometry.height!=1080 || (pixels && source_pitch<7680))return -EINVAL;
 if(!native_kms_powered){ret=m3_dcpext_native_power(native_client,true);if(ret)return ret;native_kms_powered=true;}
 if(!pixels)return m3_dcpext_native_background(native_client,0xff000000);
 /* Initial color bars occupy A. Copy to B first; alternate only on D589. */
 unsigned int slot=native_kms_slot?0:1;
 destination=frame_pixels+slot*FRAME_BYTES;
 dva=(FRAME_DVA+slot*FRAME_BYTES)|BIT_ULL(40);
 frame_sync(slot,true);
 for(u32 y=0;y<1080;y++)iosys_map_memcpy_from(destination+y*7680,pixels,(size_t)y*source_pitch,7680);
 frame_sync(slot,false);dma_wmb();native_surface(surface,opaque);
 ret=m3_dcpext_native_swap(native_client,surface,dva,1920,1080);
 if(!ret){native_kms_slot^=1;native_kms_frames++;}
 return ret;
}
static const struct m3_dcpext_kms_ops native_kms_ops={.present=native_kms_present};
static int native_kms_publish(void)
{
 struct m3_dcpext_native_mode *modes;
 const struct m3_dcpext_native_mode wanted={
  .geometry={1920,1080,148500,2200,1125,false,false},
  .hfront=88,.hsync=44,.vfront=4,.vsync=5,.hpositive=true,.vpositive=true};
 struct m3_dcpext_connector *connector;
 void *blob;u32 bytes=0,count,index,w,h;u64 generation;int ret;
 if(native_kms_connected || !av_edid_done)return 0;
 blob=m3_dcpext_native_property(native_client,"DisplayAttributes",&bytes);
 if(!blob)return 0;
 ret=m3_dcpext_native_dimensions(blob,bytes,&w,&h);kfree(blob);if(ret)return ret;
 blob=m3_dcpext_native_property(native_client,"TimingElements",&bytes);
 if(!blob)return 0;
 modes=kcalloc(M3_DCPEXT_NATIVE_MAX_MODES,sizeof(*modes),GFP_KERNEL);
 if(!modes){kfree(blob);return -ENOMEM;}
 ret=m3_dcpext_native_modes_parse(blob,bytes,modes,M3_DCPEXT_NATIVE_MAX_MODES,&count);kfree(blob);
 if(!ret)ret=m3_dcpext_native_mode_select(&wanted,modes,count,&index);
 if(ret)goto out;
 native_kms_generation=m3_dcpext_native_generation(native_client);
 if(!native_kms){
  native_kms=m3_dcpext_kms_create(dma_dev,&native_kms_ops,NULL,&native_route_lock,
    usb_c?DRM_MODE_CONNECTOR_DisplayPort:DRM_MODE_CONNECTOR_HDMIA);
  if(IS_ERR(native_kms)){ret=PTR_ERR(native_kms);native_kms=NULL;goto out;}
 }
 connector=m3_dcpext_kms_connector(native_kms);
 mutex_unlock(&native_route_lock);
 generation=m3_dcpext_connector_invalidate(connector,true);
 ret=m3_dcpext_connector_publish_native(connector,generation,av_edid,av_edid_bytes,&modes[index],1,native_kms_generation);
 mutex_lock(&native_route_lock);
 if(ret)goto out;
 native_kms_connected=true;native_kms_powered=true;native_kms_slot=0;
 if(native_kms_ready){
  mutex_unlock(&native_route_lock);
  m3_dcpext_connector_hotplug(connector);
  mutex_lock(&native_route_lock);
  pr_info("m3_dcpext_native: HDMI reconnected: fresh EDID and timing generation %llu\n",native_kms_generation);
  goto out;
 }
 if(native_desktop){ret=dcpext_session_enter_runtime(&session);if(ret)goto out;afk.streaming=true;aux_afk.streaming=true;}
 native_kms_ready=true;native_kms_until=session_now(NULL)+8000;
 ret=m3_dcpext_kms_register(native_kms);
 if(ret)native_kms_ready=false;
 else {
  pr_info("m3_dcpext_native: HDMI DRM registered: 1080p60 %ux%u mm, desktop=%u\n",w,h,native_desktop);
  if(native_desktop)desktop_notify(0);
 }
out:
 kfree(modes);return ret;
}
/* Keep DRM and the firmware session alive while the cable is absent. */
static void native_kms_disconnect(void)
{
 lockdep_assert_held(&native_route_lock);
 native_kms_connected=false;
 native_frame_submitted=false;av_read_ready=false;av_edid_done=false;
 mutex_unlock(&native_route_lock);
 m3_dcpext_connector_invalidate(m3_dcpext_kms_connector(native_kms),false);
 m3_dcpext_connector_hotplug(m3_dcpext_kms_connector(native_kms));
 mutex_lock(&native_route_lock);
}
/* Never call with route lock held: final disable runs through present. */
static void native_kms_unplug(void)
{
 if(!native_kms_ready || native_kms_finished)return;
 m3_dcpext_connector_invalidate(m3_dcpext_kms_connector(native_kms),false);
 m3_dcpext_connector_hotplug(m3_dcpext_kms_connector(native_kms));
 m3_dcpext_kms_unplug(native_kms);
 mutex_lock(&native_route_lock);
 native_kms_finished=true;native_frame_done=true;
 pr_info("m3_dcpext_native: HDMI KMS trial finished; %u completed frames\n",native_kms_frames);
 mutex_unlock(&native_route_lock);
}
