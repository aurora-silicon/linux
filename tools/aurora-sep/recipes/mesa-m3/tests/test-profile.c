/* Copyright 2026 Chris Kearney
 * SPDX-License-Identifier: MIT */
#include "mesa-m3-profile.h"
#include <assert.h>
int main(void)
{
   struct drm_asahi_params_global p = {
      .gpu_generation=15, .gpu_variant='G', .gpu_revision=0x20, .chip_id=0x8122,
      .num_dies=1, .num_clusters_total=1, .num_cores_per_cluster=10,
      .core_masks={0x3ff}, .usc_generation=3, .gpu_hal_generation=200,
   };
   assert(mesa_m3_profile_matches(&p,"j613-25g83-hal200"));
   assert(!mesa_m3_profile_matches(&p,"legacy"));
   p.core_masks[0]=0xff;
   assert(mesa_m3_profile_matches(&p,"j613-25g83-hal200"));
   p.gpu_hal_generation=0;
   assert(!mesa_m3_profile_matches(&p,"j613-25g83-hal200"));
   assert(mesa_m3_profile_matches(&p,"legacy"));
   p.chip_id=0x6030; p.gpu_variant='S'; p.num_clusters_total=2;
   assert(mesa_m3_profile_matches(&p,"legacy"));
   assert(!mesa_m3_profile_matches(&p,"j613-25g83-hal200"));
   p.gpu_hal_generation=200;
   assert(!mesa_m3_profile_matches(&p,"j613-25g83-hal200"));
   assert(!mesa_m3_profile_matches(&p,"legacy"));
   p.chip_id=0x8122; p.gpu_variant='G'; p.num_clusters_total=1;
   p.usc_generation=2;
   assert(!mesa_m3_profile_matches(&p,"j613-25g83-hal200"));
   p.usc_generation=3; p.gpu_revision=0;
   assert(!mesa_m3_profile_matches(&p,"j613-25g83-hal200"));
   assert(!mesa_m3_profile_matches(&p,"unknown"));
}
