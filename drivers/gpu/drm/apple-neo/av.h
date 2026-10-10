#ifndef __AV_H__
#define __AV_H__

#include "parser.h"

//int avep_audiosrv_startlink(struct apple_dcp *dcp, struct dcp_sound_cookie *cookie);
//int avep_audiosrv_stoplink(struct apple_dcp *dcp);

#if IS_ENABLED(CONFIG_DRM_APPLE_NEO_AUDIO)
void neo_av_service_connect(struct neo_apple_dcp *neo_dcp);
void neo_av_service_disconnect(struct neo_apple_dcp *neo_dcp);
#else
static inline void neo_av_service_connect(struct neo_apple_dcp *neo_dcp) { }
static inline void neo_av_service_disconnect(struct neo_apple_dcp *neo_dcp) { }
#endif

#endif /* __AV_H__ */
