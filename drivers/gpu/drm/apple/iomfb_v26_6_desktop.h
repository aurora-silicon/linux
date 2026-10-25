/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright The Asahi Linux Contributors */

/*
 * The classic IOMFB (dcpep) protocol of the desktop 26.6 firmware on T6041
 * (M4 Pro/Max), from the shared IOMFB template
 * (DCP_FIRMWARE_V_26_6_DESKTOP).  It continues the 14.7 IOMFB.  Not to be
 * confused with iomfb_v26_6.h, the J613 DCPLink session.
 */

#ifndef __APPLE_IOMFB_V26_6_DESKTOP_H__
#define __APPLE_IOMFB_V26_6_DESKTOP_H__

#include <linux/types.h>

#include "version_utils.h"

/*
 * dcp-internal.h includes this header, also from inside another template
 * instantiation: keep that one's DCP_FW.
 */
#pragma push_macro("DCP_FW")
#pragma push_macro("DCP_FW_VER")
#undef DCP_FW
#undef DCP_FW_VER

#define DCP_FW v26_6_desktop
#define DCP_FW_VER DCP_FW_VERSION_DESKTOP(26, 6, 0)

#include "iomfb_template.h"

#undef DCP_FW_VER
#undef DCP_FW

#pragma pop_macro("DCP_FW_VER")
#pragma pop_macro("DCP_FW")

/*
 * D006 set_frame_sync_props is 0x54 bytes in and 0x50 out.  The firmware
 * expects the first 0x50 request bytes back, with the three flag bytes at
 * 73..75 cleared.
 */
struct dcp_set_frame_sync_props_req_v26_6_desktop {
	u8 props[0x50];
	u8 opaque[0x4];
} __packed;

struct dcp_set_frame_sync_props_resp_v26_6_desktop {
	u8 props[0x50];
} __packed;

#endif /* __APPLE_IOMFB_V26_6_DESKTOP_H__ */
