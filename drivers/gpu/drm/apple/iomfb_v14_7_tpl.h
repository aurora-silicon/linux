/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright The Asahi Linux Contributors */

/*
 * The classic IOMFB (dcpep) protocol of 14.7 firmware, from the shared
 * IOMFB template, for the generic M1/M2 14.7.0 firmware
 * (DCP_FIRMWARE_V_14_7_TEMPLATE). Not to be confused with iomfb_v14_7.h,
 * the native DCPLink session for T6030 processors handed over by the boot
 * loader.
 */

#ifndef __APPLE_IOMFB_V14_7_TPL_H__
#define __APPLE_IOMFB_V14_7_TPL_H__

#include "version_utils.h"

/*
 * dcp-internal.h includes this header, also from inside another template
 * instantiation (iomfb_h17p.c): keep that one's DCP_FW.
 */
#pragma push_macro("DCP_FW")
#pragma push_macro("DCP_FW_VER")
#undef DCP_FW
#undef DCP_FW_VER

#define DCP_FW v14_7_0
#define DCP_FW_VER DCP_FW_VERSION(14, 7, 0)

#include "iomfb_template.h"

#undef DCP_FW_VER
#undef DCP_FW

#pragma pop_macro("DCP_FW_VER")
#pragma pop_macro("DCP_FW")

#endif /* __APPLE_IOMFB_V14_7_TPL_H__ */
