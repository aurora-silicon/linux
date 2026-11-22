/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ISP_ANE_H__
#define __ISP_ANE_H__

#include "isp-drv.h"

/* At probe, before the presets: whether MLVNR is on (sets isp->mlvnr). */
bool apple_isp_ane_probe(struct apple_isp *isp, bool has_config);
int apple_isp_ane_init(struct apple_isp *isp);
void apple_isp_ane_exit(struct apple_isp *isp);
int apple_isp_ane_start(struct apple_isp *isp);
void apple_isp_ane_stop(struct apple_isp *isp);
int apple_isp_ane_stream_start(struct apple_isp *isp, u32 ch);
/* A sensor frame of this size is an input the MLVNR network takes. */
bool isp_mlvnr_input_ok(u32 width, u32 height);

#endif
