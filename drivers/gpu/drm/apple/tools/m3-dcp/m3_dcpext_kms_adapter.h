/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* M4 native KMS ownership pattern adapted to J514S old RPC protocol. */
#include <linux/iosys-map.h>
#include <drm/drm_framebuffer.h>
#include "m3_dcpext_kms.h"
static DEFINE_MUTEX(native_route_lock);
static struct m3_dcpext_kms *native_kms;
static bool native_kms_ready,native_kms_finished,native_kms_powered=true;
static bool native_kms_connected;
static unsigned int native_kms_frames,native_kms_slot;
static u32 native_kms_timing,native_kms_color;
static struct drm_framebuffer *native_slot_fb[2], *native_failed_fb;
static unsigned int native_direct_frames;
module_param(native_direct_frames,uint,0400);
/* Completed-frame aggregate costs; read deltas around a bounded animation. */
static unsigned long long native_copy_ns,native_sync_ns,native_swap_ns;
module_param(native_copy_ns,ullong,0400);
module_param(native_sync_ns,ullong,0400);
module_param(native_swap_ns,ullong,0400);
static u64 native_kms_until,native_kms_generation;
module_param(native_kms_ready,bool,0400);
module_param(native_kms_frames,uint,0400);
static int native_kms_present(void *cookie,const struct m3_dcpext_native_mode *mode,
 u64 generation,const struct iosys_map *pixels,u32 source_pitch,bool opaque,
 struct drm_framebuffer *fb,struct sg_table *sgt)
{
 u8 surface[0x22c];void *destination;u64 dva,t0,t1,t2,t3;int ret;
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
 u32 width=mode->geometry.width,height=mode->geometry.height,pitch=sgt?source_pitch:ALIGN(width*4,64);
 ret=m3_dcpext_surface_validate(width,height,pitch,FRAME_DVA,FRAME_BYTES);
 if(ret || (pixels && source_pitch<width*4))return ret?:-EINVAL;
 if(mode->timing_id!=native_kms_timing || mode->color_id!=native_kms_color){
  if(native_kms_powered){ret=m3_dcpext_native_power(native_client,false);if(ret)return ret;native_kms_powered=false;}
  ret=m3_dcpext_native_mode(native_client,mode->color_id,mode->timing_id);if(ret)return ret;
  native_kms_timing=mode->timing_id;native_kms_color=mode->color_id;
 }
 if(!native_kms_powered){ret=m3_dcpext_native_power(native_client,true);if(ret)return ret;native_kms_powered=true;}
 if(!fb)return m3_dcpext_native_background(native_client,0xff000000);
 /* Initial color bars occupy A. Use B first; alternate only on D589. */
 unsigned int slot=native_kms_slot?0:1;
 destination=frame_pixels+slot*FRAME_BYTES;
 dva=(FRAME_DVA+slot*FRAME_BYTES)|BIT_ULL(40);
 t0=ktime_get_ns();
 if(sgt){
  if(native_slot_fb[slot]!=fb){
   struct drm_framebuffer *old=native_slot_fb[slot];
   drm_framebuffer_get(fb);native_slot_fb[slot]=fb;
   ret=frame_map_pages(slot,sgt,fb->offsets[0],(u64)pitch*height);
   if(ret){native_failed_fb=old;return ret;}
   if(old)drm_framebuffer_put(old);
  }
  t3=ktime_get_ns();native_sync_ns+=t3-t0;
 }else{
  if(native_slot_fb[slot]){
   ret=frame_map_pages(slot,NULL,0,0);if(ret)return ret;
   drm_framebuffer_put(native_slot_fb[slot]);native_slot_fb[slot]=NULL;
  }
  frame_sync(slot,true);t1=ktime_get_ns();
  for(u32 y=0;y<height;y++)iosys_map_memcpy_from(destination+y*pitch,pixels,(size_t)y*source_pitch,width*4);
  t2=ktime_get_ns();frame_sync(slot,false);dma_wmb();t3=ktime_get_ns();
  native_sync_ns+=(t1-t0)+(t3-t2);native_copy_ns+=t2-t1;
 }
 native_surface(surface,opaque,width,height,pitch);
 ret=m3_dcpext_native_swap(native_client,surface,dva,width,height);
 native_swap_ns+=ktime_get_ns()-t3;
 if(!ret){native_kms_slot^=1;native_kms_frames++;if(sgt)native_direct_frames++;}
 return ret;
}
static const struct m3_dcpext_kms_ops native_kms_ops={.present=native_kms_present};
/* A rejected catalog is not a failed firmware transport. Keep servicing RPCs
 * and expose an unavailable connector until a new sink publication arrives. */
static int native_kms_unavailable(int error)
{
 int ret;
 if(!native_desktop)return error;
 if(native_modes_error==error && native_kms_ready)return 0;
 native_modes_error=error;
 native_modes_error_generation=m3_dcpext_native_generation(native_client);
 pr_warn("m3_dcpext: external mode metadata rejected (%d); internal desktop retained\n",error);
 if(!native_kms){
  native_kms=m3_dcpext_kms_create(dma_dev,&native_kms_ops,NULL,&native_route_lock,
    usb_c?DRM_MODE_CONNECTOR_DisplayPort:DRM_MODE_CONNECTOR_HDMIA);
  if(IS_ERR(native_kms)){ret=PTR_ERR(native_kms);native_kms=NULL;return ret;}
 }
 native_kms_connected=false;
 mutex_unlock(&native_route_lock);
 m3_dcpext_connector_invalidate(m3_dcpext_kms_connector(native_kms),false);
 if(native_kms_ready)m3_dcpext_connector_hotplug(m3_dcpext_kms_connector(native_kms));
 mutex_lock(&native_route_lock);
 if(!native_kms_ready){
  ret=dcpext_session_enter_runtime(&session);if(ret)return ret;
  afk.streaming=true;aux_afk.streaming=true;
  ret=m3_dcpext_kms_register(native_kms);if(ret)return ret;
  native_kms_ready=true;native_frame_submitted=true;
  desktop_notify(0);
 }
 return 0;
}
static bool native_kms_reconnecting(void)
{return native_kms_ready;}
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
 if(IS_ERR(blob))return native_kms_unavailable(PTR_ERR(blob));
 if(!blob)return 0;
 ret=m3_dcpext_native_dimensions(blob,bytes,&w,&h);kvfree(blob);if(ret)return native_kms_unavailable(ret);
 blob=m3_dcpext_native_property(native_client,"TimingElements",&bytes);
 if(IS_ERR(blob))return native_kms_unavailable(PTR_ERR(blob));
 if(!blob)return 0;
 modes=kcalloc(M3_DCPEXT_NATIVE_MAX_MODES,sizeof(*modes),GFP_KERNEL);
 if(!modes){kvfree(blob);return native_kms_unavailable(-ENOMEM);}
 ret=m3_dcpext_native_modes_parse_link(blob,bytes,modes,M3_DCPEXT_NATIVE_MAX_MODES,&count,link_payload_kbps());kvfree(blob);
 if(!ret)ret=m3_dcpext_native_mode_select(&wanted,modes,count,&index);
 if(ret){kfree(modes);return native_kms_unavailable(ret);}
 native_kms_generation=m3_dcpext_native_generation(native_client);
 if(!native_kms){
  native_kms=m3_dcpext_kms_create(dma_dev,&native_kms_ops,NULL,&native_route_lock,
    usb_c?DRM_MODE_CONNECTOR_DisplayPort:DRM_MODE_CONNECTOR_HDMIA);
  if(IS_ERR(native_kms)){ret=PTR_ERR(native_kms);native_kms=NULL;goto out;}
 }
 connector=m3_dcpext_kms_connector(native_kms);
 mutex_unlock(&native_route_lock);
 generation=m3_dcpext_connector_invalidate(connector,true);
 ret=m3_dcpext_connector_publish_native(connector,generation,av_edid,av_edid_bytes,modes,count,native_kms_generation);
 mutex_lock(&native_route_lock);
 if(ret)goto out;
 native_kms_connected=true;native_kms_powered=true;native_frame_submitted=true;
 if(!native_kms_ready){
  native_kms_slot=0;
  native_kms_timing=modes[index].timing_id;native_kms_color=modes[index].color_id;
 }else{
  /* Retain both pinned slots and the retired/active ordering across hotplug.
   * Force a fresh mode request before the first new desktop framebuffer. */
  native_kms_timing=0;native_kms_color=0;
 }
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
  pr_info("m3_dcpext_native: HDMI DRM registered: native modes, %ux%u mm, desktop=%u\n",w,h,native_desktop);
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
