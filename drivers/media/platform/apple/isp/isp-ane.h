/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ISP_ANE_H__
#define __ISP_ANE_H__

#include "isp-drv.h"

int apple_isp_ane_init(struct apple_isp *isp);
void apple_isp_ane_exit(struct apple_isp *isp);
int apple_isp_ane_start(struct apple_isp *isp);
void apple_isp_ane_stop(struct apple_isp *isp);
int apple_isp_ane_stream_start(struct apple_isp *isp, u32 ch);

#endif
